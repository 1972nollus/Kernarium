// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_diagnostics.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_executor_pool.hpp"
#include "cpu/arm64/jit/jit_executor_pool_profile.hpp"
#include "cpu/arm64/jit/jit_profile_output.hpp"
#include "cpu/arm64/jit/jit_shared_translations.hpp"
#include "emulator/src/profile_reporter.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

namespace arm64 = ::uml::cpu::arm64;

[[nodiscard]] bool exercise_executor_pool_profile(
    std::optional<arm64::JitExecutorPoolProfile> &live_profile,
    unsigned int &interleaved_sequence, unsigned int &separator_sequence,
    unsigned int &cross_wrapper_sequence, unsigned int &third_sequence) {
  const auto budget = arm64::minimum_jit_cache_budget(2);
  if (budget == 0 || !arm64::set_jit_cache_budget(budget, 2))
    return false;

  {
    arm64::JitExecutor first_standalone;
    arm64::JitExecutor second_standalone;
    if (!first_standalone.budget_admitted() ||
        !second_standalone.budget_admitted())
      return false;
  }

  if (!arm64::acquire_jit_cache_run_session())
    return false;
  const auto session = arm64::active_jit_executor_pool_session();
  if (session == 0 || !arm64::bind_jit_executor_pool_configuration(
                          session, arm64::JitExecutorOptions{})) {
    arm64::release_jit_cache_run_session();
    return false;
  }
  live_profile.emplace(session);
  arm64::JitExecutorPoolProfile second{session};
  arm64::JitExecutorPoolProfile third{session};

  live_profile->observe_sequence(&interleaved_sequence);
  {
    auto borrowed = arm64::borrow_jit_executor(
        session, arm64::maximum_jit_cache_executors, &*live_profile);
    if (!borrowed || borrowed.slot() != 0) {
      arm64::release_jit_cache_run_session();
      return false;
    }
  }

  second.observe_sequence(&cross_wrapper_sequence);
  auto holder = std::make_optional(
      arm64::borrow_jit_executor(
          session, arm64::maximum_jit_cache_executors, &second));
  if (!*holder || holder->slot() != 0) {
    holder.reset();
    arm64::release_jit_cache_run_session();
    return false;
  }

  live_profile->observe_sequence(&separator_sequence);
  {
    auto borrowed =
        arm64::borrow_jit_executor(session, 0, &*live_profile);
    if (!borrowed || borrowed.slot() != 1) {
      holder.reset();
      arm64::release_jit_cache_run_session();
      return false;
    }
  }

  /* The same wrapper's successful associations deliberately interleave
   * S@0, T@1, S@1. Slot stability therefore cannot be inferred from only
   * consecutive same-sequence borrows.
   */
  live_profile->observe_sequence(&interleaved_sequence);
  {
    auto borrowed =
        arm64::borrow_jit_executor(session, 0, &*live_profile);
    if (!borrowed || borrowed.slot() != 1) {
      holder.reset();
      arm64::release_jit_cache_run_session();
      return false;
    }
  }

  third.observe_sequence(nullptr);
  /* This association reaches slot 1 while second associated the same
   * sequence only with slot 0, independently pinning cross-wrapper mask OR.
   */
  third.observe_sequence(&cross_wrapper_sequence);
  {
    auto borrowed = arm64::borrow_jit_executor(session, 1, &third);
    if (!borrowed || borrowed.slot() != 1) {
      holder.reset();
      arm64::release_jit_cache_run_session();
      return false;
    }
  }

  holder.reset();
  second.observe_sequence(&third_sequence);
  {
    auto borrowed = arm64::borrow_jit_executor(session, 0, &second);
    if (!borrowed || borrowed.slot() != 0) {
      arm64::release_jit_cache_run_session();
      return false;
    }
  }

  /* The production FIFO tests exercise real blocking/handoff. This fixture
   * adds deterministic reporter aggregation without timing a condition
   * variable; the preset contract pins every matching production call site.
   */
  third.record_waiter_enqueue(3);
  third.record_waiter_wake();
  third.record_fifo_handoff();
  arm64::release_jit_cache_run_session();
  return true;
}

[[nodiscard]] bool exercise_followup_executor_pool_profile(
    unsigned int &reused_sequence) {
  if (!arm64::acquire_jit_cache_run_session())
    return false;
  const auto session = arm64::active_jit_executor_pool_session();
  if (session == 0 || !arm64::bind_jit_executor_pool_configuration(
                          session, arm64::JitExecutorOptions{})) {
    arm64::release_jit_cache_run_session();
    return false;
  }
  arm64::JitExecutorPoolProfile profile{session};
  profile.observe_sequence(&reused_sequence);
  {
    auto borrowed = arm64::borrow_jit_executor(
        session, arm64::maximum_jit_cache_executors, &profile);
    if (!borrowed || borrowed.slot() != 0) {
      arm64::release_jit_cache_run_session();
      return false;
    }
  }
  arm64::release_jit_cache_run_session();
  return true;
}

[[nodiscard]] bool prove_nonblocking_stderr_backpressure_and_framing() {
  int pipe_descriptors[2];
  if (::pipe(pipe_descriptors) != 0)
    return false;
  const auto close_pipe = [&] {
    if (pipe_descriptors[0] >= 0)
      (void)::close(pipe_descriptors[0]);
    if (pipe_descriptors[1] >= 0)
      (void)::close(pipe_descriptors[1]);
  };

  const auto flags = ::fcntl(pipe_descriptors[1], F_GETFL);
  if (flags < 0 ||
      ::fcntl(pipe_descriptors[1], F_SETFL, flags | O_NONBLOCK) < 0) {
    close_pipe();
    return false;
  }

  std::array<char, 4096> filler;
  filler.fill('#');
  std::size_t filled = 0;
  for (;;) {
    const auto written =
        ::write(pipe_descriptors[1], filler.data(), filler.size());
    if (written > 0) {
      filled += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    close_pipe();
    return false;
  }
  /* A nonblocking atomic 4 KiB write may return EAGAIN while a smaller write
   * would still fit. Consume the remaining capacity byte-by-byte so the test
   * record cannot complete before the delayed reader starts.
   */
  constexpr char final_filler = '#';
  for (;;) {
    const auto written = ::write(pipe_descriptors[1], &final_filler, 1);
    if (written == 1) {
      ++filled;
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    close_pipe();
    return false;
  }

  const auto saved_stderr = ::dup(STDERR_FILENO);
  if (saved_stderr < 0 ||
      ::dup2(pipe_descriptors[1], STDERR_FILENO) < 0) {
    if (saved_stderr >= 0)
      (void)::close(saved_stderr);
    close_pipe();
    return false;
  }
  (void)::close(pipe_descriptors[1]);
  pipe_descriptors[1] = -1;

  std::mutex writer_mutex;
  std::condition_variable writer_changed;
  bool writer_entered = false;
  bool writer_done = false;
  constexpr char one_shot_record[] =
      "Darwin JIT nonblocking backpressure path=one-shot token=complete";
  constexpr char assembled_record[] =
      "Darwin JIT nonblocking backpressure path=assembled token=complete";
  constexpr char expected_suffix[] =
      "\nDarwin JIT nonblocking backpressure path=one-shot token=complete\n"
      "\nDarwin JIT nonblocking backpressure path=assembled token=complete\n";
  std::thread writer{[&] {
    {
      const std::lock_guard lock{writer_mutex};
      writer_entered = true;
    }
    writer_changed.notify_one();
    uml::cpu::arm64::write_jit_profile_record("%s", one_shot_record);
    uml::cpu::arm64::JitProfileRecord assembled;
    assembled.append("%s", assembled_record);
    assembled.emit();
    {
      const std::lock_guard lock{writer_mutex};
      writer_done = true;
    }
    writer_changed.notify_one();
  }};
  {
    std::unique_lock lock{writer_mutex};
    writer_changed.wait(lock, [&] { return writer_entered; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  bool blocked_before_drain;
  {
    const std::lock_guard lock{writer_mutex};
    blocked_before_drain = !writer_done;
  }

  bool read_ok = true;
  std::string captured;
  captured.reserve(filled + sizeof(expected_suffix) - 1U);
  std::thread reader{[&] {
    std::array<char, 4096> buffer;
    for (;;) {
      const auto received =
          ::read(pipe_descriptors[0], buffer.data(), buffer.size());
      if (received > 0) {
        captured.append(buffer.data(), static_cast<std::size_t>(received));
        continue;
      }
      if (received == 0)
        return;
      if (errno == EINTR)
        continue;
      read_ok = false;
      return;
    }
  }};
  writer.join();
  const bool restored = ::dup2(saved_stderr, STDERR_FILENO) >= 0;
  (void)::close(saved_stderr);
  reader.join();
  (void)::close(pipe_descriptors[0]);
  pipe_descriptors[0] = -1;

  const auto captured_prefix_size = std::min(filled, captured.size());
  const auto suffix = captured.begin() +
                      static_cast<std::ptrdiff_t>(captured_prefix_size);
  const bool prefix_is_unterminated =
      filled != 0 && captured.size() >= filled &&
      std::none_of(captured.begin(), suffix,
                   [](char value) { return value == '\n' || value == '\r'; });
  return blocked_before_drain && restored && read_ok &&
         prefix_is_unterminated &&
         captured.size() == filled + sizeof(expected_suffix) - 1U &&
         std::all_of(captured.begin(), suffix,
                     [](char value) { return value == '#'; }) &&
         std::equal(expected_suffix,
                    expected_suffix + sizeof(expected_suffix) - 1U, suffix) &&
         captured.back() == '\n';
}

} // namespace

int main() {
  if (!prove_nonblocking_stderr_backpressure_and_framing())
    return 2;
  std::puts("JIT nonblocking backpressure and line framing passed");
  std::optional<arm64::JitExecutorPoolProfile> live_pool_profile;
  unsigned int interleaved_sequence = 1;
  unsigned int separator_sequence = 2;
  unsigned int cross_wrapper_sequence = 3;
  unsigned int third_sequence = 4;
  if (!exercise_executor_pool_profile(
          live_pool_profile, interleaved_sequence, separator_sequence,
          cross_wrapper_sequence, third_sequence))
    return 3;

  constexpr std::array<std::uint32_t, 1> guest_words{0xd503201fU};
  constexpr std::array<std::uint32_t, 1> host_words{0xd503201fU};
  /* Each logical expansion crosses the fixed record bound. Concurrent
   * producers prove that typed, numbered continuation records reconstruct
   * independently instead of interleaving partial lines.
   */
  std::array<std::uint32_t, 400> expansion_a{};
  std::array<std::uint32_t, 400> expansion_b{};
  for (std::size_t index = 0; index < expansion_a.size(); ++index) {
    expansion_a[index] = 0xa0000000U | static_cast<std::uint32_t>(index);
    expansion_b[index] = 0xb0000000U | static_cast<std::uint32_t>(index);
  }
  std::thread retired{[&] {
    uml::cpu::arm64::profile_jit_compilation(
        0x1000, guest_words, host_words, host_words.size(), false, 0, 0);
    uml::cpu::arm64::profile_jit_local_compile_latency(1000);
    uml::cpu::arm64::profile_jit_shared_materialization_latency(100);
  }};
  retired.join();

  std::mutex expansion_mutex;
  std::condition_variable expansion_changed;
  unsigned expansion_ready = 0;
  bool start_expansions = false;
  const auto expand = [&](std::uint64_t pc,
                          const std::array<std::uint32_t, 400> &words) {
    {
      std::unique_lock lock{expansion_mutex};
      ++expansion_ready;
      expansion_changed.notify_all();
      expansion_changed.wait(lock, [&] { return start_expansions; });
    }
    uml::cpu::arm64::profile_jit_compilation(
        pc, guest_words, words, words.size(), false, 0, 0);
  };
  std::thread expansion_worker_a{expand, 0x3000, std::cref(expansion_a)};
  std::thread expansion_worker_b{expand, 0x4000, std::cref(expansion_b)};
  {
    std::unique_lock lock{expansion_mutex};
    expansion_changed.wait(lock, [&] { return expansion_ready == 2; });
    start_expansions = true;
  }
  expansion_changed.notify_all();
  expansion_worker_a.join();
  expansion_worker_b.join();
  uml::cpu::arm64::profile_jit_compilation(
      0x2000, guest_words, host_words, host_words.size(), false, 0, 0);
  uml::cpu::arm64::profile_jit_local_compile_latency(2000);
  uml::cpu::arm64::profile_jit_shared_materialization_latency(200);

  std::mutex stdio_mutex;
  std::condition_variable stdio_changed;
  bool stdio_locked = false;
  bool release_stdio = false;
  std::thread stdio_owner{[&] {
    ::flockfile(stderr);
    {
      std::lock_guard lock{stdio_mutex};
      stdio_locked = true;
    }
    stdio_changed.notify_one();
    {
      std::unique_lock lock{stdio_mutex};
      stdio_changed.wait(lock, [&] { return release_stdio; });
    }
    ::funlockfile(stderr);
  }};
  {
    std::unique_lock lock{stdio_mutex};
    stdio_changed.wait(lock, [&] { return stdio_locked; });
  }

  /* The first call combines retired workers with still-live main-thread
   * compile and executor-pool recorders without acquiring the FILE lock
   * deliberately held by stdio_owner. Moving the pool profile into the
   * retired aggregate cannot change its cumulative snapshot. The next pool
   * session changes only pool state, so its final must not force an unchanged
   * shared-cache final. Later compile/shared activity permits one updated
   * cumulative final of each corresponding profile.
   */
  uml::emulator::report_jit_profiles();
  uml::emulator::report_jit_profiles();
  live_pool_profile.reset();
  uml::emulator::report_jit_profiles();
  uml::cpu::arm64::write_jit_profile_record(
      "%s", "Darwin JIT executor pool fixture phase=retired-unchanged");
  if (!exercise_followup_executor_pool_profile(interleaved_sequence)) {
    {
      std::lock_guard lock{stdio_mutex};
      release_stdio = true;
    }
    stdio_changed.notify_one();
    stdio_owner.join();
    return 4;
  }
  uml::emulator::report_jit_profiles();
  uml::cpu::arm64::write_jit_profile_record(
      "%s", "Darwin JIT executor pool fixture phase=pool-only-complete");
  uml::emulator::report_jit_profiles();
  uml::cpu::arm64::profile_jit_compilation(
      0x5000, guest_words, host_words, host_words.size(), false, 0, 0);
  uml::cpu::arm64::profile_jit_local_compile_latency(500);
  uml::cpu::arm64::profile_jit_shared_materialization_latency(50);
  auto &shared = uml::cpu::arm64::SharedTranslationCache::instance();
  (void)shared.find({}, 0x5000, guest_words);
  uml::emulator::report_jit_profiles();
  uml::emulator::report_jit_profiles();
  {
    std::lock_guard lock{stdio_mutex};
    release_stdio = true;
  }
  stdio_changed.notify_one();
  stdio_owner.join();
  return 0;
}
