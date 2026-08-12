// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>

namespace uml::cpu::arm64 {

/* Runtime profiling runs on guest task-carrier threads. Those threads must not
 * acquire a shared stdio FILE lock: a carrier may retire while another thread
 * is producing the final process-wide report. Build each bounded record in
 * private memory and publish it directly to the stderr descriptor instead.
 * Every buffer begins with a line delimiter in the same descriptor write, so
 * unrelated unterminated PTY output cannot prefix the record schema.
 */
class JitProfileRecord final {
public:
  JitProfileRecord() noexcept;

  void append(const char *format, ...) noexcept
      __attribute__((format(printf, 2, 3)));
  void emit() noexcept;

private:
  static constexpr std::size_t capacity = 2048;

  std::array<char, capacity> storage_{};
  std::size_t size_{};
  bool valid_{true};
};

void write_jit_profile_record(const char *format, ...) noexcept
    __attribute__((format(printf, 1, 2)));

} // namespace uml::cpu::arm64
