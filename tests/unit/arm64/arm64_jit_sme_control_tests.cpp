// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's SME control core at run time, from SmeSupport::control on
 * (design 5.1.3). On a host whose profile implements SME, an SME host with
 * DARWIN_UML_GUEST_SME=1, every test runs in every mode. Elsewhere the SM=0
 * tests run on an injected SME2 profile at SVL 64: SM=0 translations hold
 * no SVE or SME instruction (arm64-jit-sme-codegen), so they run on any
 * host, and only SM=1 code needs the hardware.
 */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t smstart = 0xd503'477fU;
constexpr std::uint32_t smstart_sm = 0xd503'437fU;
constexpr std::uint32_t smstart_za = 0xd503'457fU;
constexpr std::uint32_t smstop = 0xd503'467fU;
constexpr std::uint32_t smstop_sm = 0xd503'427fU;
constexpr std::uint32_t smstop_za = 0xd503'447fU;

[[nodiscard]] constexpr std::uint32_t mrs_svcr(unsigned t) noexcept {
  return 0xd53b'4240U | t;
}
[[nodiscard]] constexpr std::uint32_t msr_svcr(unsigned t) noexcept {
  return 0xd51b'4240U | t;
}
[[nodiscard]] constexpr std::uint32_t mrs_tpidr2(unsigned t) noexcept {
  return 0xd53b'd0a0U | t;
}
[[nodiscard]] constexpr std::uint32_t msr_tpidr2(unsigned t) noexcept {
  return 0xd51b'd0a0U | t;
}
[[nodiscard]] constexpr std::uint32_t rdsvl(unsigned d, int imm) noexcept {
  return 0x04bf'5800U | ((static_cast<unsigned>(imm) & 63U) << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t addsvl(unsigned d, unsigned n,
                                             int imm) noexcept {
  return 0x0420'5800U | (n << 16) |
         ((static_cast<unsigned>(imm) & 63U) << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t addspl(unsigned d, unsigned n,
                                             int imm) noexcept {
  return 0x0460'5800U | (n << 16) |
         ((static_cast<unsigned>(imm) & 63U) << 5) | d;
}
/* LDR or STR ZA[W(12 + v), #off], [Xn|SP, #off, MUL VL]. */
[[nodiscard]] constexpr std::uint32_t za_vector(bool load, unsigned v,
                                                unsigned n,
                                                unsigned off) noexcept {
  return (load ? 0xe100'0000U : 0xe120'0000U) | (v << 13) | (n << 5) | off;
}
[[nodiscard]] constexpr std::uint32_t zt0_vector(bool load,
                                                 unsigned n) noexcept {
  return (load ? 0xe11f'8000U : 0xe13f'8000U) | (n << 5);
}
[[nodiscard]] constexpr std::uint32_t zero_za(unsigned mask) noexcept {
  return 0xc008'0000U | (mask & 0xffU);
}
constexpr std::uint32_t zero_zt0 = 0xc048'0001U;
[[nodiscard]] constexpr std::uint32_t load_x(unsigned t, unsigned n) noexcept {
  return 0xf940'0000U | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t branch(int words) noexcept {
  return 0x1400'0000U | (static_cast<std::uint32_t>(words) & 0x03ff'ffffU);
}
/* CPYFP, CPYFM and CPYFE [X0]!, [X1]!, X2!. */
constexpr std::array<std::uint32_t, 3> copy_forward{0x1901'0440U, 0x1941'0440U,
                                                    0x1981'0440U};
constexpr std::uint32_t initial_fpcr = 0x0040'0000U; // RMode +inf
constexpr std::uint32_t initial_fpsr = 0x0800'0000U; // QC
constexpr std::uint32_t initial_nzcv = 0x6000'0000U; // Z, C
constexpr unsigned stack_pointer_index = 31;

[[nodiscard]] std::byte pattern(std::size_t index, unsigned seed) noexcept {
  return static_cast<std::byte>((index * 7U + seed * 31U + 1U) & 0xffU);
}

/*
 * A vCPU with SME task storage, guest code on page 0 and data on the pages
 * after it, run through one executor, on the host profile current at
 * construction. Every GPR, SP, NZCV and FPCR starts with a pattern a test
 * may override before its first run; require_preserved() checks the rest.
 * The register file is the state to check after a run: every translation
 * leaves streaming mode on exit.
 */
class Machine final {
public:
  Machine(unsigned long generation, std::uint64_t base, std::uint8_t svcr,
          bool storage = true, std::size_t pages = 4,
          std::size_t mapped_pages = 4)
      : generation_{generation}, base_{base}, backing_{pages},
        mapping_{generation, base, backing_, mapped_pages},
        file_{arm64::SmeRegisterFile::for_host(
            uml::cpu::host_capabilities())},
        svl_{file_.svl()}, fp_image_(32U * 16U) {
    require(svl_ != 0U);
    if (storage) {
      task_.resize(arm64::sme_storage_size(svl_));
      for (std::size_t index = 0; index < task_.size(); ++index)
        task_[index] = pattern(index, 1);
    }
    for (std::size_t index = 0; index < fp_image_.size(); ++index)
      fp_image_[index] = pattern(index, 2);
    for (unsigned index = 0; index < cpu.registers.size(); ++index)
      cpu.registers[index] =
          0x5a00'0000'0000'0000ULL | (std::uint64_t{index} << 32) | index;
    cpu.sp = 0x7700'1230;
    cpu.nzcv = initial_nzcv;
    cpu.pc = base;
    cpu.bind_sme_registers(file_);
    cpu.defer_fp_load(fp_image_.data(), initial_fpcr, initial_fpsr, ~0U);
    cpu.defer_sme_load(storage ? task_.data() : nullptr, svcr, 0);
    require(executor_.begin_execution(generation, base));
  }

  ~Machine() { executor_.end_execution(); }

  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  [[nodiscard]] std::uint64_t data(std::size_t offset = 0) const noexcept {
    return base_ + page + offset;
  }

  void code(std::size_t offset, std::initializer_list<std::uint32_t> words) {
    for (const auto word : words) {
      backing_.put32(offset, word);
      offset += 4U;
    }
  }

  void put(std::uint64_t address, std::span<const std::byte> bytes) {
    backing_.put(address - base_, bytes);
  }

  [[nodiscard]] std::vector<std::byte> get(std::uint64_t address,
                                           std::size_t size) const {
    std::vector<std::byte> bytes(size);
    backing_.get(address - base_, bytes);
    return bytes;
  }

  /* The register file as the first translation of this mode will see it. */
  [[nodiscard]] std::vector<std::byte> initial_file() {
    cpu.access_sme_state();
    return file();
  }

  [[nodiscard]] std::vector<std::byte> file() const {
    return {file_.data(), file_.data() + arm64::sme_storage_size(svl_)};
  }

  [[nodiscard]] Event run(std::size_t &retired, std::size_t blocks = 64) {
    if (!started_) {
      started_ = true;
      registers_ = cpu.registers;
      sp_ = cpu.sp;
    }
    retired = 0;
    for (std::size_t round = 0; round < blocks; ++round) {
      std::size_t executed = 0;
      const auto event =
          executor_.run_block(cpu, memory_, generation_, executed);
      /* Release builds compile out the engine's own check. */
      require(arm64::host_streaming_state_clear(
          uml::cpu::host_capabilities()));
      retired += executed;
      if (event != Event::continued)
        return event;
    }
    return Event::continued;
  }

  /* Every GPR and SP but `written` (31 for SP) as the first run found them,
   * FPCR and, unless the code sets flags, NZCV as constructed. */
  void require_preserved(std::initializer_list<unsigned> written = {},
                         bool sets_flags = false) {
    const auto is_written = [&](unsigned index) {
      return std::ranges::find(written, index) != written.end();
    };
    for (unsigned index = 0; index < cpu.registers.size(); ++index)
      if (!is_written(index) && cpu.registers[index] != registers_[index]) {
        std::fprintf(stderr, "x%u changed: %#llx\n", index,
                     static_cast<unsigned long long>(cpu.registers[index]));
        require(false);
      }
    require(is_written(stack_pointer_index) || cpu.sp == sp_);
    arm64::restore_native_fp_environment(cpu);
    if ((!sets_flags && cpu.nzcv != initial_nzcv) ||
        cpu.effective_fpcr() != initial_fpcr) {
      std::fprintf(stderr, "pc %#llx svcr %u: nzcv %#x fpcr %#x\n",
                   static_cast<unsigned long long>(cpu.pc), cpu.svcr,
                   cpu.nzcv, cpu.effective_fpcr());
      require(false);
    }
  }

  [[nodiscard]] std::uint32_t guest_fpsr() {
    arm64::restore_native_fp_environment(cpu);
    return cpu.effective_fpsr();
  }

  /* V0-V31 as the FP image held them. */
  [[nodiscard]] bool vectors_unchanged() {
    arm64::restore_native_fp_environment(cpu);
    cpu.access_fp_state();
    return std::memcmp(cpu.vectors.data(), fp_image_.data(),
                       fp_image_.size()) == 0;
  }

  [[nodiscard]] JitExecutor &executor() noexcept { return executor_; }
  [[nodiscard]] unsigned svl() const noexcept { return svl_; }

  arm64::CpuState cpu{};

private:
  unsigned long generation_;
  std::uint64_t base_;
  JitBacking backing_;
  JitMapping mapping_;
  JitExecutor executor_;
  StaticGuestMemory memory_;
  arm64::SmeRegisterFile file_;
  unsigned svl_;
  std::vector<std::byte> task_;
  std::vector<std::byte> fp_image_;
  bool started_{};
  std::array<std::uint64_t, 31> registers_{};
  std::uint64_t sp_{};
};

[[nodiscard]] std::vector<std::byte> bytes_of(std::size_t size,
                                              unsigned seed) {
  std::vector<std::byte> bytes(size);
  for (std::size_t index = 0; index < size; ++index)
    bytes[index] = pattern(index, seed);
  return bytes;
}

[[nodiscard]] bool all_zero(std::span<const std::byte> bytes) {
  return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{}; });
}

/* The modes a run tests: SM=1 needs the hardware. */
[[nodiscard]] std::vector<std::uint8_t> all_modes(bool hardware) {
  if (hardware)
    return {0, arm64::svcr_sm, arm64::svcr_za, arm64::svcr_mask};
  return {0, arm64::svcr_za};
}

[[nodiscard]] std::vector<std::uint8_t> za_modes(bool hardware) {
  if (hardware)
    return {arm64::svcr_za, arm64::svcr_mask};
  return {arm64::svcr_za};
}

/* Each SVCR write ends its block; leaving streaming mode zeroes V and
 * resets FPSR. */
void streaming_mode_switches() {
  {
    Machine machine{9401, 0xe0'0000, 0};
    machine.code(0, {smstart, mrs_svcr(0), smstop_sm, mrs_svcr(1),
                     smstart_sm, smstop_za, mrs_svcr(2), smstop, mrs_svcr(3),
                     system_call});
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(retired == 10U);
    require(machine.cpu.registers[0] == 3U && machine.cpu.registers[1] == 2U &&
            machine.cpu.registers[2] == 1U && machine.cpu.registers[3] == 0U);
    require(machine.cpu.svcr == 0U);
    require(all_zero(std::as_bytes(std::span{machine.cpu.vectors})));
    require(machine.guest_fpsr() == arm64::streaming_mode_change_fpsr);
    machine.require_preserved({0, 1, 2, 3});
  }
  /* MSR SVCR writes both bits from Xt. */
  Machine machine{9402, 0xe1'0000, 0};
  machine.code(0, {msr_svcr(4), mrs_svcr(5), msr_svcr(31), mrs_svcr(6),
                   system_call});
  machine.cpu.registers[4] = 0xffff'fffe'0000'0003ULL;
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(machine.cpu.registers[5] == 3U && machine.cpu.registers[6] == 0U);
  machine.require_preserved({5, 6});
}

/* SMSTART ZA zeroes ZA and ZT0; ZA writes leave V and FPSR alone. */
void za_mode_switches() {
  Machine machine{9403, 0xe2'0000, 0};
  machine.code(0, {smstart_za, mrs_svcr(0), smstop_za, mrs_svcr(1),
                   msr_svcr(4), mrs_svcr(5), msr_svcr(31), mrs_svcr(6),
                   smstart_za, system_call});
  machine.cpu.registers[4] = 0xffff'fffe'0000'0002ULL;
  const auto before = machine.initial_file();
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(retired == 10U);
  require(machine.cpu.registers[0] == 2U && machine.cpu.registers[1] == 0U &&
          machine.cpu.registers[5] == 2U && machine.cpu.registers[6] == 0U);
  require(machine.cpu.svcr == arm64::svcr_za);
  const auto after = machine.file();
  const auto za = arm64::sme_za_offset(machine.svl(), 0);
  require(std::equal(after.begin(), after.begin() + static_cast<std::ptrdiff_t>(za),
                     before.begin()));
  require(all_zero(std::span{after}.subspan(za)));
  require(machine.vectors_unchanged());
  require(machine.guest_fpsr() == initial_fpsr);
  machine.require_preserved({0, 1, 5, 6});
}

void enabling_without_storage_asks_for_it() {
  Machine machine{9404, 0xe3'0000, 0, false};
  machine.code(0, {smstop, smstart_za, system_call});
  std::size_t retired = 0;
  require(machine.run(retired) == Event::sme_access);
  require(retired == 1U);
  require(machine.cpu.pc == 0xe3'0004U);
  require(machine.cpu.svcr == 0U);
  machine.require_preserved();
}

void tpidr2_is_the_guest_register(bool hardware) {
  unsigned long generation = 9410;
  for (const auto mode : all_modes(hardware)) {
    Machine machine{generation, 0xe4'0000U + (generation - 9410U) * 0x1'0000U,
                    mode};
    ++generation;
    machine.code(0, {msr_tpidr2(1), mrs_tpidr2(2), system_call});
    machine.cpu.registers[1] = 0x1234'5678'9abc'def0ULL;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(machine.cpu.tpidr2 == 0x1234'5678'9abc'def0ULL);
    require(machine.cpu.registers[2] == machine.cpu.tpidr2);
    machine.require_preserved({2});
  }
}

void vector_lengths_come_from_the_svl(bool hardware) {
  unsigned long generation = 9420;
  for (const auto mode : all_modes(hardware)) {
    Machine machine{generation, 0xe8'0000U + (generation - 9420U) * 0x1'0000U,
                    mode};
    ++generation;
    machine.code(0, {rdsvl(0, 1), rdsvl(1, -2), addsvl(2, 3, 3),
                     addspl(4, 3, -5), addsvl(31, 31, 1), addspl(5, 5, 31),
                     rdsvl(31, 4), system_call});
    machine.cpu.registers[3] = 0x1000;
    machine.cpu.registers[5] = 0x40;
    machine.cpu.sp = 0x8000;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    const std::uint64_t svl = machine.svl();
    require(machine.cpu.registers[0] == svl);
    require(machine.cpu.registers[1] == ~(2U * svl) + 1U);
    require(machine.cpu.registers[2] == 0x1000U + 3U * svl);
    require(machine.cpu.registers[4] == 0x1000U - 5U * (svl / 8U));
    require(machine.cpu.registers[5] == 0x40U + 31U * (svl / 8U));
    require(machine.cpu.sp == 0x8000U + svl);
    machine.require_preserved({0, 1, 2, 4, 5, stack_pointer_index});
  }
}

/* LDR ZA, then STR ZA of the same vector through another index register,
 * at each placement the fast and slow paths divide. */
void za_vectors_round_trip(bool hardware) {
  unsigned long generation = 9430;
  for (const auto mode : za_modes(hardware))
    for (int placement = 0; placement < 3; ++placement) {
      Machine machine{generation,
                      0xf0'0000U + (generation - 9430U) * 0x10'0000U, mode};
      ++generation;
      const std::size_t svl = machine.svl();
      /* Aligned, crossing into the next page, and through a tagged
       * pointer. */
      const auto source =
          placement == 1 ? machine.data(page - svl / 2U) : machine.data(64);
      const auto destination = machine.data(2U * page + 3U);
      const auto bytes = bytes_of(svl, 5);
      machine.put(source, bytes);
      machine.code(0, {za_vector(true, 0, 2, 3), za_vector(false, 1, 3, 1),
                       system_call});
      machine.cpu.registers[12] = 5;
      machine.cpu.registers[13] = 7;
      machine.cpu.registers[2] = source - 3U * svl;
      machine.cpu.registers[3] = destination - svl;
      if (placement == 2) {
        machine.cpu.tbi_enabled = true;
        machine.cpu.registers[2] |= 0x5a00'0000'0000'0000ULL;
      }
      const auto before = machine.initial_file();
      std::size_t retired = 0;
      require(machine.run(retired) == Event::system_call);
      require(retired == 3U);
      require(machine.get(destination, svl) == bytes);
      const auto after = machine.file();
      const auto row = arm64::sme_za_offset(svl, 8);
      require(std::equal(bytes.begin(), bytes.end(),
                         after.begin() + static_cast<std::ptrdiff_t>(row)));
      /* Nothing else in the file changed: not Z or P across the enter
       * and leave sequences, nor another ZA vector. */
      for (std::size_t index = 0; index < after.size(); ++index)
        if (index < row || index >= row + svl)
          require(after[index] == before[index]);
      machine.require_preserved();
    }
}

void za_faults_match_the_m4(bool hardware) {
  for (const auto mode : za_modes(hardware)) {
    const std::uint64_t base = 0x1f0'0000U + mode * 0x10'0000U;
    /* Only the code page and the first data page are mapped. */
    {
      Machine machine{9440U + mode, base, mode, true, 4, 2};
      const std::size_t svl = machine.svl();
      machine.code(0, {za_vector(false, 0, 2, 0), system_call});
      machine.cpu.registers[2] = machine.data(page - svl / 2U);
      const auto before = machine.get(machine.data(page - svl / 2U), svl / 2U);
      std::size_t retired = 0;
      require(machine.run(retired) == Event::memory_fault);
      require(retired == 0U);
      require(machine.cpu.pc == base);
      require(machine.executor().fault_address() == machine.data(page));
      require(machine.executor().fault_access() == uml::memory::Access::write);
      require(!machine.executor().fault_alignment());
      /* Nothing of the vector reached the mapped page. */
      require(machine.get(machine.data(page - svl / 2U), svl / 2U) == before);
      machine.require_preserved();
    }
    {
      Machine machine{9450U + mode, base + 0x8'0000U, mode};
      machine.code(0, {za_vector(true, 0, 31, 0), system_call});
      machine.cpu.sp = machine.data(8);
      std::size_t retired = 0;
      require(machine.run(retired) == Event::memory_fault);
      require(machine.executor().fault_alignment());
      require(machine.executor().fault_address() == machine.data(8));
      require(machine.executor().fault_access() == uml::memory::Access::read);
      machine.require_preserved();
    }
  }
}

void zero_za_clears_the_named_tiles(bool hardware) {
  for (const auto mode : za_modes(hardware)) {
    Machine machine{9460U + mode, 0x300'0000U + mode * 0x10'0000U, mode};
    machine.code(0, {zero_za(0x09), system_call});
    const auto before = machine.initial_file();
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    const std::size_t svl = machine.svl();
    const auto after = machine.file();
    for (std::size_t vector = 0; vector < svl; ++vector) {
      const auto offset = arm64::sme_za_offset(svl, vector);
      const bool cleared = vector % 8U == 0U || vector % 8U == 3U;
      for (std::size_t index = 0; index < svl; ++index)
        require(after[offset + index] ==
                (cleared ? std::byte{} : before[offset + index]));
    }
    machine.require_preserved();
  }
}

void zt0_moves_and_clears(bool hardware) {
  if (arm64::sme_profile(uml::cpu::host_capabilities()) !=
      arm64::SmeProfile::sme2)
    return;
  for (const auto mode : za_modes(hardware)) {
    Machine machine{9470U + mode, 0x400'0000U + mode * 0x10'0000U, mode};
    const auto bytes = bytes_of(arm64::sme_zt0_bytes, 9);
    machine.put(machine.data(128), bytes);
    machine.code(0, {zt0_vector(true, 2), zt0_vector(false, 3), zero_zt0,
                     system_call});
    machine.cpu.registers[2] = machine.data(128);
    machine.cpu.registers[3] = machine.data(page + 48);
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(machine.get(machine.data(page + 48), bytes.size()) == bytes);
    const auto after = machine.file();
    require(all_zero(
        std::span{after}.subspan(arm64::sme_zt0_offset(machine.svl()))));
    machine.require_preserved();
  }
}

/*
 * A TLB miss in an SM=1 block takes the streaming bridge, a linked SM=1
 * successor branches past the enter, and the guest's state and FPSR come
 * through both, on the first run and on the linked second one.
 */
void streaming_state_survives_misses_and_links() {
  Machine machine{9480, 0x500'0000, arm64::svcr_mask};
  const std::size_t svl = machine.svl();
  machine.code(0, {load_x(5, 6), za_vector(false, 0, 3, 0), branch(1),
                   za_vector(true, 1, 2, 0), za_vector(false, 1, 4, 0),
                   system_call});
  machine.cpu.registers[6] = machine.data(2U * page);
  machine.cpu.registers[3] = machine.data(0);
  machine.cpu.registers[2] = machine.data(0);
  machine.cpu.registers[4] = machine.data(page);
  machine.cpu.registers[12] = 4;
  machine.cpu.registers[13] = 9;
  const auto before = machine.initial_file();
  for (int pass = 0; pass < 2; ++pass) {
    machine.cpu.pc = 0x500'0000U;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(retired == 6U);
    /* ZA vector 4 went out, came back as vector 9, and went out again. */
    const auto row =
        std::span{before}.subspan(arm64::sme_za_offset(svl, 4), svl);
    const auto out = machine.get(machine.data(page), svl);
    require(std::equal(row.begin(), row.end(), out.begin()));
    const auto after = machine.file();
    const auto nine = arm64::sme_za_offset(svl, 9);
    for (std::size_t index = 0; index < after.size(); ++index)
      require(after[index] ==
              (index >= nine && index < nine + svl ? row[index - nine]
                                                   : before[index]));
    require(machine.guest_fpsr() == initial_fpsr);
    machine.require_preserved({5});
  }
}

/* A host helper in an SM=1 block (the FEAT_MOPS copy) is terminal: the
 * leave before it spills the guest's state and the next block refills it. */
void streaming_state_survives_helpers() {
  Machine machine{9490, 0x600'0000, arm64::svcr_mask};
  const std::size_t svl = machine.svl();
  const auto copied = bytes_of(64, 3);
  const auto row = bytes_of(svl, 4);
  machine.put(machine.data(0), copied);
  machine.put(machine.data(2U * page), row);
  machine.code(0, {za_vector(true, 0, 3, 0), copy_forward[0], copy_forward[1],
                   copy_forward[2], za_vector(false, 0, 4, 0), system_call});
  machine.cpu.registers[0] = machine.data(page);
  machine.cpu.registers[1] = machine.data(0);
  machine.cpu.registers[2] = 64;
  machine.cpu.registers[3] = machine.data(2U * page);
  machine.cpu.registers[4] = machine.data(2U * page + 512U);
  machine.cpu.registers[12] = 5;
  const auto before = machine.initial_file();
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(retired == 6U);
  require(machine.cpu.registers[2] == 0U);
  require(machine.get(machine.data(page), copied.size()) == copied);
  require(machine.get(machine.data(2U * page + 512U), svl) == row);
  const auto after = machine.file();
  const auto five = arm64::sme_za_offset(svl, 5);
  for (std::size_t index = 0; index < after.size(); ++index)
    require(after[index] ==
            (index >= five && index < five + svl ? row[index - five]
                                                 : before[index]));
  require(machine.guest_fpsr() == initial_fpsr);
  /* CPYFP reports the copy's algorithm option in NZCV. */
  machine.require_preserved({0, 1, 2}, true);
}

/* The host's own profile with the SME2 profile at SVL 64. It never claims
 * sme_hardware, so nothing reads the host's SVCR. */
[[nodiscard]] uml::cpu::HostCapabilities injected_sme_host() {
  auto host = uml::cpu::host_capabilities();
  for (const auto feature :
       {uml::cpu::ArmFeature::sme, uml::cpu::ArmFeature::sme2,
        uml::cpu::ArmFeature::sme_f64f64, uml::cpu::ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature));
  host.sme_svl_bytes = 64;
  host.sme_hardware = false;
  return host;
}

void run_tests(bool hardware) {
  if (hardware)
    streaming_mode_switches();
  za_mode_switches();
  enabling_without_storage_asks_for_it();
  tpidr2_is_the_guest_register(hardware);
  vector_lengths_come_from_the_svl(hardware);
  za_vectors_round_trip(hardware);
  za_faults_match_the_m4(hardware);
  zero_za_clears_the_named_tiles(hardware);
  zt0_moves_and_clears(hardware);
  if (hardware) {
    streaming_state_survives_misses_and_links();
    streaming_state_survives_helpers();
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  if (arm64::jit::sme_support == arm64::SmeSupport::none ||
      !jit_available()) {
    std::printf("skipped: the JIT does not run here\n");
    return 0;
  }
  if (arm64::implements_sme(uml::cpu::host_capabilities())) {
    run_tests(true);
    return 0;
  }
  const auto host = injected_sme_host();
  const uml::cpu::HostCapabilitiesScope scope{host};
  run_tests(false);
  std::printf("SM=1 skipped: the host profile does not implement SME\n");
  return 0;
}
