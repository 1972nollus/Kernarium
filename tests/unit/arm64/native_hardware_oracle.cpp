// SPDX-License-Identifier: GPL-2.0-or-later
#include "native_hardware_oracle.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)
#include <libkern/OSCacheControl.h>
#endif

namespace uml::emulator::tests::native_oracle {
namespace {

using ::uml::cpu::arm64::CpuState;

constexpr std::uint64_t low_address_mask = 0x00ff'ffff'ffff'ffffULL;
constexpr std::uint64_t top_byte_mask = ~low_address_mask;
constexpr std::uint32_t terminal_breakpoint = 0xd420'0000U;
constexpr std::uint64_t result_magic = 0x4e41'5449'5645'4f52ULL;
constexpr std::size_t maximum_window_bytes = 512U * 1024U * 1024U;
constexpr int child_setup_base = 80;

struct RegionPlan final {
  std::uint64_t address{};
  std::size_t size{};
  std::size_t output_offset{};
  int protection{};
  bool capture{};
};

struct PreparedRequest final {
  std::size_t page_size{};
  std::uint64_t guest_low{};
  std::uint64_t guest_high{};
  std::size_t code_page_index{};
  std::size_t code_page_count{};
  std::vector<int> page_protections;
  std::vector<RegionPlan> regions;
  std::size_t output_bytes{};
};

struct SharedResult final {
  std::uint64_t magic{};
  volatile sig_atomic_t phase{};
  int signal{};
  std::uint64_t fault_address{};
  std::uint64_t raw_pc{};
  std::uint64_t platform_register_before{};
  std::uint64_t platform_register_after{};
  CpuState state;
};

struct ChildContext final {
  const Request *request{};
  const PreparedRequest *prepared{};
  SharedResult *result{};
  std::byte *capture_flags{};
  std::byte *memory_output{};
  std::byte *window{};
  std::uint64_t raw_pc{};
};

#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)
ChildContext *child_context;
#endif

[[nodiscard]] bool add_overflow(std::uint64_t left, std::uint64_t right,
                                std::uint64_t &result) noexcept {
  if (right > std::numeric_limits<std::uint64_t>::max() - left)
    return true;
  result = left + right;
  return false;
}

[[nodiscard]] std::uint64_t align_down(std::uint64_t value,
                                       std::size_t alignment) noexcept {
  return value & ~(static_cast<std::uint64_t>(alignment) - 1U);
}

[[nodiscard]] std::optional<std::uint64_t>
align_up(std::uint64_t value, std::size_t alignment) noexcept {
  std::uint64_t adjusted{};
  if (add_overflow(value, alignment - 1U, adjusted))
    return std::nullopt;
  return align_down(adjusted, alignment);
}

[[nodiscard]] bool safe_system_instruction(std::uint32_t instruction) noexcept {
  switch (instruction) {
  case 0xd503'201fU: // NOP
  case 0xd503'203fU: // YIELD
  case 0xd503'209fU: // SEV
  case 0xd503'20bfU: // SEVL
  case 0xd500'401fU: // CFINV
  case 0xd500'403fU: // XAFLAG
  case 0xd500'405fU: // AXFLAG
    return true;
  default:
    break;
  }
  const auto system_register = instruction & 0xffff'ffe0U;
  if (system_register == 0xd53b'4200U || // MRS NZCV
      system_register == 0xd51b'4200U || // MSR NZCV
      system_register == 0xd53b'4400U || // MRS FPCR
      system_register == 0xd51b'4400U || // MSR FPCR
      system_register == 0xd53b'4420U || // MRS FPSR
      system_register == 0xd51b'4420U)   // MSR FPSR
    return true;
  const auto barrier = instruction & 0xffff'f0ffU;
  return barrier == 0xd503'305fU || barrier == 0xd503'309fU ||
         barrier == 0xd503'30bfU || barrier == 0xd503'30dfU ||
         barrier == 0xd503'30ffU ||
         (instruction & 0xffff'f3ffU) == 0xd503'323fU;
}

[[nodiscard]] bool represented_system_register_uses_platform_register(
    std::uint32_t instruction) noexcept {
  const auto system_register = instruction & 0xffff'ffe0U;
  const bool represented =
      system_register == 0xd53b'4200U || system_register == 0xd51b'4200U ||
      system_register == 0xd53b'4400U || system_register == 0xd51b'4400U ||
      system_register == 0xd53b'4420U || system_register == 0xd51b'4420U;
  return represented && (instruction & 31U) == 18U;
}

[[nodiscard]] bool unsafe_host_effect(std::uint32_t instruction) noexcept {
  const bool breakpoint =
      (instruction & 0xffe0'001fU) == terminal_breakpoint;
  if ((instruction & 0xff00'0000U) == 0xd400'0000U)
    return !breakpoint;
  if ((instruction & 0xff00'0000U) == 0xd500'0000U)
    return !safe_system_instruction(instruction);
  return false;
}

[[nodiscard]] std::int64_t sign_extend(std::uint64_t value,
                                       unsigned bits) noexcept {
  const auto sign = std::uint64_t{1} << (bits - 1U);
  return static_cast<std::int64_t>((value ^ sign) - sign);
}

[[nodiscard]] std::optional<std::int64_t>
direct_branch_offset(std::uint32_t instruction) noexcept {
  if ((instruction & 0x7c00'0000U) == 0x1400'0000U)
    return sign_extend(instruction & 0x03ff'ffffU, 26U) * 4;
  if ((instruction & 0xff00'0010U) == 0x5400'0000U)
    return sign_extend((instruction >> 5U) & 0x7ffffU, 19U) * 4;
  if ((instruction & 0x7e00'0000U) == 0x3400'0000U)
    return sign_extend((instruction >> 5U) & 0x7ffffU, 19U) * 4;
  if ((instruction & 0x7e00'0000U) == 0x3600'0000U)
    return sign_extend((instruction >> 5U) & 0x3fffU, 14U) * 4;
  return std::nullopt;
}

[[nodiscard]] bool uncontained_control_flow(std::uint32_t instruction,
                                            std::uint64_t pc,
                                            const PreparedRequest &prepared) noexcept {
  if ((instruction & 0xfe00'0000U) == 0xd600'0000U)
    return true;
  const auto offset = direct_branch_offset(instruction);
  if (!offset)
    return false;
  const auto code_low = prepared.guest_low +
      prepared.code_page_index * prepared.page_size;
  const auto code_high = code_low +
      prepared.code_page_count * prepared.page_size;
  std::uint64_t target{};
  if (offset.value() < 0) {
    const auto magnitude =
        static_cast<std::uint64_t>(-(offset.value() + 1)) + 1U;
    if (magnitude > pc)
      return true;
    target = pc - magnitude;
  } else if (add_overflow(pc, static_cast<std::uint64_t>(offset.value()),
                          target)) {
    return true;
  }
  return target < code_low || target >= code_high;
}

[[nodiscard]] Observation skipped(const Request &request,
                                  Eligibility eligibility) {
  Observation result;
  result.eligibility = eligibility;
  result.status = ExecutionStatus::skipped;
  result.state = request.initial;
  result.memory.reserve(request.memory.size());
  for (const auto &region : request.memory)
    result.memory.push_back({.address = region.address,
                             .bytes = {region.initial.begin(),
                                       region.initial.end()},
                             .captured = false});
  return result;
}

[[nodiscard]] std::optional<PreparedRequest>
prepare(const Request &request, Eligibility &failure) {
  const auto queried_page_size = ::sysconf(_SC_PAGESIZE);
  if (queried_page_size <= 0 ||
      (queried_page_size & (queried_page_size - 1)) != 0) {
    failure = Eligibility::unsupported_host;
    return std::nullopt;
  }
  PreparedRequest result;
  result.page_size = static_cast<std::size_t>(queried_page_size);
  if ((request.guest_pc % sizeof(std::uint32_t)) != 0) {
    failure = Eligibility::memory_geometry;
    return std::nullopt;
  }
  const auto code_low = align_down(request.guest_pc, result.page_size);
  std::uint64_t code_end{};
  if (add_overflow(request.guest_pc, 2U * sizeof(std::uint32_t), code_end)) {
    failure = Eligibility::memory_geometry;
    return std::nullopt;
  }
  const auto code_high = align_up(code_end, result.page_size);
  if (!code_high) {
    failure = Eligibility::memory_geometry;
    return std::nullopt;
  }
  result.guest_low = code_low;
  result.guest_high = *code_high;

  for (const auto &region : request.memory) {
    std::uint64_t end{};
    if (add_overflow(region.address, region.initial.size(), end)) {
      failure = Eligibility::memory_geometry;
      return std::nullopt;
    }
    if (request.requires_exact_memory_boundaries &&
        ((region.address % result.page_size) != 0 ||
         (region.initial.size() % result.page_size) != 0)) {
      failure = Eligibility::memory_geometry;
      return std::nullopt;
    }
    if (region.capture_after_execution && !region.readable) {
      failure = Eligibility::memory_geometry;
      return std::nullopt;
    }
    if (region.initial.empty())
      continue;
    const auto low = align_down(region.address, result.page_size);
    const auto high = align_up(end, result.page_size);
    if (!high) {
      failure = Eligibility::memory_geometry;
      return std::nullopt;
    }
    result.guest_low = std::min(result.guest_low, low);
    result.guest_high = std::max(result.guest_high, *high);
  }
  for (std::size_t left = 0; left < request.memory.size(); ++left) {
    const auto &first = request.memory[left];
    const auto first_end = first.address + first.initial.size();
    for (std::size_t right = left + 1; right < request.memory.size(); ++right) {
      const auto &second = request.memory[right];
      const auto second_end = second.address + second.initial.size();
      if (first.address < second_end && second.address < first_end) {
        failure = Eligibility::memory_geometry;
        return std::nullopt;
      }
    }
  }
  if (result.guest_high < result.guest_low ||
      result.guest_high - result.guest_low > maximum_window_bytes) {
    failure = Eligibility::memory_geometry;
    return std::nullopt;
  }
  const auto window_bytes =
      static_cast<std::size_t>(result.guest_high - result.guest_low);
  result.page_protections.assign(window_bytes / result.page_size, -1);
  result.code_page_index =
      static_cast<std::size_t>((code_low - result.guest_low) /
                               result.page_size);
  result.code_page_count =
      static_cast<std::size_t>((*code_high - code_low) / result.page_size);
  for (std::size_t index = 0; index < result.code_page_count; ++index)
    result.page_protections[result.code_page_index + index] =
        PROT_READ | PROT_EXEC;

  result.regions.reserve(request.memory.size());
  for (const auto &region : request.memory) {
    const int protection = (region.readable ? PROT_READ : 0) |
                           (region.writable ? PROT_WRITE : 0) |
                           (region.executable ? PROT_EXEC : 0);
    RegionPlan plan{.address = region.address,
                    .size = region.initial.size(),
                    .output_offset = result.output_bytes,
                    .protection = protection,
                    .capture = region.capture_after_execution};
    if (region.initial.size() >
        std::numeric_limits<std::size_t>::max() - result.output_bytes) {
      failure = Eligibility::memory_geometry;
      return std::nullopt;
    }
    result.output_bytes += region.initial.size();
    result.regions.push_back(plan);
    if (region.initial.empty())
      continue;
    const auto low = align_down(region.address, result.page_size);
    const auto end = region.address + region.initial.size();
    const auto high = *align_up(end, result.page_size);
    for (auto page = low; page < high; page += result.page_size) {
      const auto index = static_cast<std::size_t>(
          (page - result.guest_low) / result.page_size);
      if (index >= result.code_page_index &&
          index < result.code_page_index + result.code_page_count) {
        failure = Eligibility::memory_geometry;
        return std::nullopt;
      }
      auto &existing = result.page_protections[index];
      if (existing != -1 && existing != protection) {
        failure = Eligibility::memory_geometry;
        return std::nullopt;
      }
      existing = protection;
    }
  }
  return result;
}

#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__)

[[nodiscard]] std::uint64_t relocate_to_host(std::uint64_t value,
                                             const ChildContext &context) noexcept {
  const auto low = value & low_address_mask;
  if (low < context.prepared->guest_low ||
      low > context.prepared->guest_high)
    return value;
  const auto host = reinterpret_cast<std::uint64_t>(context.window) +
                    (low - context.prepared->guest_low);
  return (value & top_byte_mask) | (host & low_address_mask);
}

[[nodiscard]] std::uint64_t normalize_to_guest(
    std::uint64_t value, const PreparedRequest &prepared,
    const std::byte *window) noexcept {
  const auto low = value & low_address_mask;
  const auto host_low = reinterpret_cast<std::uint64_t>(window);
  const auto bytes = prepared.guest_high - prepared.guest_low;
  if (low < host_low || low - host_low > bytes)
    return value;
  const auto guest = prepared.guest_low + (low - host_low);
  return (value & top_byte_mask) | (guest & low_address_mask);
}

[[nodiscard]] std::uint64_t normalize_pc_relative_to_guest(
    const std::uint64_t value, const Request &request,
    const std::uint64_t raw_pc) noexcept {
  return value - raw_pc + request.guest_pc;
}

[[nodiscard]] std::uint64_t normalize_address_to_guest(
    const std::uint64_t value, const Request &request,
    const std::uint64_t raw_pc) noexcept {
  const auto low = value & low_address_mask;
  const auto raw_low = raw_pc & low_address_mask;
  const auto guest_low = request.guest_pc & low_address_mask;
  const auto bias = (raw_low - guest_low) & low_address_mask;
  const auto guest = (low - bias) & low_address_mask;
  return (value & top_byte_mask) | guest;
}

template <typename Destination, typename Source>
void signal_copy(Destination *destination, const Source *source,
                 std::size_t bytes) noexcept {
  auto *output = reinterpret_cast<volatile std::byte *>(destination);
  const auto *input = reinterpret_cast<const volatile std::byte *>(source);
  for (std::size_t index = 0; index < bytes; ++index)
    output[index] = input[index];
}

void install_initial_context(ucontext_t &context,
                             ChildContext &active) noexcept {
  auto &thread = context.uc_mcontext->__ss;
  const auto &initial = active.request->initial;
  active.result->platform_register_before = thread.__x[18];
  const auto input_register = [&](std::size_t index) {
    const auto value = initial.registers[index];
    return (active.request->input_address_register_mask & (1U << index)) != 0
               ? relocate_to_host(value, active)
               : value;
  };
  for (std::size_t index = 0; index < 18; ++index)
    thread.__x[index] = input_register(index);
  for (std::size_t index = 19; index < 29; ++index)
    thread.__x[index] = input_register(index);
  thread.__fp = input_register(29);
  thread.__lr = input_register(30);
  thread.__sp = active.request->input_stack_pointer_is_address
                    ? relocate_to_host(initial.sp, active)
                    : initial.sp;
  thread.__pc = active.raw_pc;
  thread.__cpsr = (thread.__cpsr & 0x0fff'ffffU) |
                  (initial.nzcv & 0xf000'0000U);
  for (std::size_t index = 0; index < initial.vectors.size(); ++index)
    signal_copy(&context.uc_mcontext->__ns.__v[index],
                initial.vectors[index].bytes.data(), 16U);
  context.uc_mcontext->__ns.__fpcr = initial.fpcr;
  context.uc_mcontext->__ns.__fpsr = initial.fpsr;
}

void capture_terminal_context(int signal, const siginfo_t &info,
                              const ucontext_t &context,
                              ChildContext &active) noexcept {
  auto &output = *active.result;
  const auto &thread = context.uc_mcontext->__ss;
  for (std::size_t index = 0; index < 18; ++index)
    output.state.registers[index] = thread.__x[index];
  for (std::size_t index = 19; index < 29; ++index)
    output.state.registers[index] = thread.__x[index];
  output.state.registers[29] = thread.__fp;
  output.state.registers[30] = thread.__lr;
  output.state.sp = thread.__sp;
  output.state.pc = thread.__pc;
  output.state.nzcv = thread.__cpsr & 0xf000'0000U;
  for (std::size_t index = 0; index < output.state.vectors.size(); ++index)
    signal_copy(output.state.vectors[index].bytes.data(),
                &context.uc_mcontext->__ns.__v[index], 16U);
  output.state.fpcr = context.uc_mcontext->__ns.__fpcr;
  output.state.fpsr = context.uc_mcontext->__ns.__fpsr;
  output.signal = signal;
  output.fault_address = reinterpret_cast<std::uintptr_t>(info.si_addr);
  output.raw_pc = active.raw_pc;
  output.platform_register_after = thread.__x[18];

  for (std::size_t index = 0; index < active.prepared->regions.size();
       ++index) {
    const auto &plan = active.prepared->regions[index];
    if (!plan.capture)
      continue;
    const auto *source = active.window +
        (plan.address - active.prepared->guest_low);
    signal_copy(active.memory_output + plan.output_offset, source, plan.size);
    active.capture_flags[index] = std::byte{1};
  }
  output.magic = result_magic;
  output.phase = 2;
}

void oracle_signal_handler(int signal, siginfo_t *info, void *raw_context) {
  auto *active = child_context;
  if (active == nullptr || raw_context == nullptr || info == nullptr)
    ::_exit(child_setup_base + 9);
  auto &context = *static_cast<ucontext_t *>(raw_context);
  if (signal == SIGUSR1 && active->result->phase == 0) {
    active->result->phase = 1;
    install_initial_context(context, *active);
    return;
  }
  capture_terminal_context(signal, *info, context, *active);
  ::_exit(0);
}

[[noreturn]] void run_child(const Request &request,
                            const PreparedRequest &prepared,
                            SharedResult &result,
                            std::byte *capture_flags,
                            std::byte *memory_output) noexcept {
  const auto window_bytes =
      static_cast<std::size_t>(prepared.guest_high - prepared.guest_low);
  auto *window = static_cast<std::byte *>(::mmap(
      nullptr, window_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (window == MAP_FAILED)
    ::_exit(child_setup_base + 1);

  for (std::size_t index = 0; index < prepared.page_protections.size();
       ++index) {
    if (prepared.page_protections[index] == -1)
      continue;
    if (::mprotect(window + index * prepared.page_size, prepared.page_size,
                   PROT_READ | PROT_WRITE) != 0)
      ::_exit(child_setup_base + 2);
  }
  for (std::size_t index = 0; index < request.memory.size(); ++index) {
    const auto &region = request.memory[index];
    if (!region.initial.empty())
      std::memcpy(window + (region.address - prepared.guest_low),
                  region.initial.data(), region.initial.size());
  }

  auto *code_page = window + prepared.code_page_index * prepared.page_size;
  auto *code_words = reinterpret_cast<std::uint32_t *>(code_page);
  const auto code_bytes = prepared.code_page_count * prepared.page_size;
  std::fill_n(code_words, code_bytes / sizeof(std::uint32_t),
              terminal_breakpoint);
  auto *raw = reinterpret_cast<std::uint32_t *>(
      window + (request.guest_pc - prepared.guest_low));
  *raw = request.instruction;

  for (std::size_t index = 0; index < prepared.page_protections.size();
       ++index) {
    const auto protection = prepared.page_protections[index];
    if (protection == -1)
      continue;
    if (::mprotect(window + index * prepared.page_size, prepared.page_size,
                   protection) != 0)
      ::_exit(child_setup_base + 3);
  }
  ::sys_icache_invalidate(code_page, code_bytes);

  std::array<std::byte, SIGSTKSZ * 2U> alternate_stack{};
  stack_t stack{.ss_sp = alternate_stack.data(),
                .ss_size = alternate_stack.size(),
                .ss_flags = 0};
  if (::sigaltstack(&stack, nullptr) != 0)
    ::_exit(child_setup_base + 4);
  struct sigaction action {};
  action.sa_sigaction = oracle_signal_handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  if (sigemptyset(&action.sa_mask) != 0)
    ::_exit(child_setup_base + 5);
  for (const int installed :
       {SIGUSR1, SIGTRAP, SIGILL, SIGSEGV, SIGBUS, SIGFPE})
    if (::sigaction(installed, &action, nullptr) != 0)
      ::_exit(child_setup_base + 6);

  sigset_t unblocked;
  if (sigemptyset(&unblocked) != 0 ||
      sigaddset(&unblocked, SIGUSR1) != 0 ||
      sigaddset(&unblocked, SIGTRAP) != 0 ||
      sigaddset(&unblocked, SIGILL) != 0 ||
      sigaddset(&unblocked, SIGSEGV) != 0 ||
      sigaddset(&unblocked, SIGBUS) != 0 ||
      sigaddset(&unblocked, SIGFPE) != 0 ||
      ::sigprocmask(SIG_UNBLOCK, &unblocked, nullptr) != 0)
    ::_exit(child_setup_base + 7);

  ChildContext active{.request = &request,
                      .prepared = &prepared,
                      .result = &result,
                      .capture_flags = capture_flags,
                      .memory_output = memory_output,
                      .window = window,
                      .raw_pc = reinterpret_cast<std::uint64_t>(raw)};
  child_context = &active;
  if (::raise(SIGUSR1) != 0)
    ::_exit(child_setup_base + 8);
  ::_exit(child_setup_base + 10);
}

[[nodiscard]] Event classify_event(const SharedResult &result) noexcept {
  if (result.signal == SIGTRAP) {
    if (result.state.pc == result.raw_pc + sizeof(std::uint32_t))
      return Event::completed;
    if (result.state.pc == result.raw_pc)
      return Event::breakpoint;
    return Event::control_target;
  }
  if (result.signal == SIGILL)
    return Event::illegal_instruction;
  if (result.signal == SIGFPE)
    return Event::floating_point_trap;
  if (result.signal == SIGSEGV || result.signal == SIGBUS) {
    if (result.state.pc != result.raw_pc &&
        result.fault_address == result.state.pc)
      return Event::control_target;
    return Event::memory_fault;
  }
  return Event::none;
}

#endif

} // namespace

Observation execute(const Request &request,
                    const ::uml::cpu::HostCapabilities &observed_capabilities)
    noexcept {
  try {
    if (request.program_scope)
      return skipped(request, Eligibility::program_scope);
    if (request.requires_unsupported_event)
      return skipped(request, Eligibility::unsupported_event);
    if (request.requires_unsupported_instruction)
      return skipped(request, Eligibility::unsupported_instruction);
    if (request.requires_unavailable_memory_geometry)
      return skipped(request, Eligibility::memory_geometry);
    if (!observed_capabilities.supports(request.required_features))
      return skipped(request, Eligibility::missing_feature);
    if (request.uses_platform_register ||
        represented_system_register_uses_platform_register(
            request.instruction) ||
        request.requires_unavailable_system_state)
      return skipped(request, Eligibility::unrepresentable_state);
    if (unsafe_host_effect(request.instruction))
      return skipped(request, Eligibility::unsafe_host_effect);
    if (std::ranges::any_of(request.memory, [](const MemoryRegion &region) {
          return region.writable && region.executable;
        }))
      return skipped(request, Eligibility::unsafe_host_effect);
#if !defined(__APPLE__) || !defined(__aarch64__) || defined(__arm64e__)
    return skipped(request, Eligibility::unsupported_host);
#else
    Eligibility preparation_failure = Eligibility::memory_geometry;
    const auto prepared = prepare(request, preparation_failure);
    if (!prepared)
      return skipped(request, preparation_failure);
    if (uncontained_control_flow(request.instruction, request.guest_pc,
                                 *prepared))
      return skipped(request, Eligibility::unsafe_host_effect);

    std::uint64_t shared_bytes = sizeof(SharedResult);
    if (add_overflow(shared_bytes, prepared->regions.size(), shared_bytes) ||
        add_overflow(shared_bytes, prepared->output_bytes, shared_bytes) ||
        shared_bytes > std::numeric_limits<std::size_t>::max())
      return skipped(request, Eligibility::memory_geometry);
    auto *shared = static_cast<SharedResult *>(::mmap(
        nullptr, static_cast<std::size_t>(shared_bytes),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0));
    if (shared == MAP_FAILED) {
      auto failure = skipped(request, Eligibility::eligible);
      failure.status = ExecutionStatus::setup_failed;
      return failure;
    }
    std::construct_at(shared);
    shared->state = request.initial;
    auto *capture_flags = reinterpret_cast<std::byte *>(shared + 1);
    auto *memory_output = capture_flags + prepared->regions.size();
    std::fill_n(capture_flags, prepared->regions.size(), std::byte{});
    for (std::size_t index = 0; index < request.memory.size(); ++index) {
      const auto &region = request.memory[index];
      const auto &plan = prepared->regions[index];
      std::copy(region.initial.begin(), region.initial.end(),
                memory_output + plan.output_offset);
    }

    const auto child = ::fork();
    if (child < 0) {
      std::destroy_at(shared);
      ::munmap(shared, static_cast<std::size_t>(shared_bytes));
      auto failure = skipped(request, Eligibility::eligible);
      failure.status = ExecutionStatus::setup_failed;
      return failure;
    }
    if (child == 0)
      run_child(request, *prepared, *shared, capture_flags, memory_output);

    auto timeout = request.timeout;
    if (timeout <= std::chrono::milliseconds::zero())
      timeout = std::chrono::milliseconds{1};
    timeout = std::min(timeout, std::chrono::milliseconds{10000});
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int child_status{};
    bool timed_out = false;
    pid_t waited{};
    for (;;) {
      waited = ::waitpid(child, &child_status, WNOHANG);
      if (waited == child)
        break;
      if (waited < 0 && errno != EINTR)
        break;
      if (std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        (void)::kill(child, SIGKILL);
        do {
          waited = ::waitpid(child, &child_status, 0);
        } while (waited < 0 && errno == EINTR);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    Observation observation;
    observation.eligibility = Eligibility::eligible;
    observation.state = request.initial;
    if (timed_out) {
      observation.status = ExecutionStatus::timed_out;
    } else if (waited != child || !WIFEXITED(child_status)) {
      observation.status = ExecutionStatus::protocol_failed;
      if (waited == child && WIFSIGNALED(child_status))
        observation.signal = WTERMSIG(child_status);
    } else if (WEXITSTATUS(child_status) != 0) {
      observation.status = ExecutionStatus::setup_failed;
      observation.signal = WEXITSTATUS(child_status);
    } else if (shared->magic != result_magic || shared->phase != 2) {
      observation.status = ExecutionStatus::protocol_failed;
    } else {
      observation.status = ExecutionStatus::executed;
      observation.event = classify_event(*shared);
      observation.signal = shared->signal;
      observation.state = shared->state;
      const auto window = reinterpret_cast<const std::byte *>(
          shared->raw_pc - (request.guest_pc - prepared->guest_low));
      for (std::size_t index = 0;
           index < observation.state.registers.size(); ++index)
        if ((request.output_pc_relative_register_mask & (1U << index)) != 0)
          observation.state.registers[index] =
              normalize_pc_relative_to_guest(
                  observation.state.registers[index], request,
                  shared->raw_pc);
        else if ((request.output_address_register_mask & (1U << index)) != 0)
          observation.state.registers[index] =
              normalize_address_to_guest(
                  observation.state.registers[index], request,
                  shared->raw_pc);
      if (request.output_stack_pointer_is_address)
        observation.state.sp = normalize_address_to_guest(
            observation.state.sp, request, shared->raw_pc);
      observation.state.pc =
          normalize_to_guest(observation.state.pc, *prepared, window);
      if (observation.event == Event::memory_fault ||
          observation.event == Event::floating_point_trap)
        observation.fault_address =
            normalize_to_guest(shared->fault_address, *prepared, window);
      observation.platform_register_changed =
          shared->platform_register_before !=
          shared->platform_register_after;
      if (observation.platform_register_changed)
        observation.eligibility = Eligibility::unrepresentable_state;
      switch (observation.event) {
      case Event::completed:
      case Event::control_target:
      case Event::breakpoint:
        observation.retired_instructions = 1;
        break;
      case Event::none:
      case Event::illegal_instruction:
      case Event::memory_fault:
      case Event::floating_point_trap:
        observation.retired_instructions = 0;
        break;
      }
      if (!request.invalid_encoding &&
          observation.event == Event::illegal_instruction)
        observation.eligibility = Eligibility::unsupported_instruction;
    }
    observation.memory.reserve(request.memory.size());
    for (std::size_t index = 0; index < request.memory.size(); ++index) {
      const auto &region = request.memory[index];
      const auto &plan = prepared->regions[index];
      observation.memory.push_back({
          .address = region.address,
          .bytes = {memory_output + plan.output_offset,
                    memory_output + plan.output_offset + plan.size},
          .captured = capture_flags[index] != std::byte{},
      });
    }
    std::destroy_at(shared);
    ::munmap(shared, static_cast<std::size_t>(shared_bytes));
    return observation;
#endif
  } catch (...) {
    Observation failure;
    failure.eligibility = Eligibility::eligible;
    failure.status = ExecutionStatus::setup_failed;
    return failure;
  }
}

} // namespace uml::emulator::tests::native_oracle
