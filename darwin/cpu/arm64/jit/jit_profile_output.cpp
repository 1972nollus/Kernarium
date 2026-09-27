// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_profile_output.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <poll.h>
#include <unistd.h>

namespace uml::cpu::arm64 {
namespace {

/* stderr can share a PTY with guest console output. Keep this delimiter in
 * the record buffer so the first descriptor write terminates any unrelated
 * unterminated text before publishing the parseable record body.
 */
constexpr char record_leading_separator = '\n';
constexpr std::size_t record_leading_separator_size = 1U;

[[nodiscard]] bool wait_until_writable(int descriptor) noexcept {
  pollfd writable{
      .fd = descriptor,
      .events = POLLOUT,
      .revents = 0,
  };
  for (;;) {
    writable.revents = 0;
    const auto ready = ::poll(&writable, 1, -1);
    if (ready > 0) {
      if ((writable.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return false;
      return (writable.revents & POLLOUT) != 0;
    }
    if (ready < 0 && errno == EINTR)
      continue;
    return false;
  }
}

void write_record(const char *data, std::size_t size) noexcept {
  const int saved_errno = errno;
  while (size != 0) {
    const auto written = ::write(STDERR_FILENO, data, size);
    if (written > 0) {
      data += written;
      size -= static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
        wait_until_writable(STDERR_FILENO))
      continue;
    break;
  }
  errno = saved_errno;
}

} // namespace

JitProfileRecord::JitProfileRecord() noexcept {
  storage_[0] = record_leading_separator;
  size_ = record_leading_separator_size;
}

void JitProfileRecord::append(const char *format, ...) noexcept {
  std::va_list arguments;
  va_start(arguments, format);
  vappend(format, arguments);
  va_end(arguments);
}

void JitProfileRecord::vappend(const char *format,
                               std::va_list arguments) noexcept {
  if (!valid_ || format == nullptr)
    return;
  const int saved_errno = errno;
  const auto available = storage_.size() - size_;
  const int formatted = std::vsnprintf(storage_.data() + size_, available,
                                       format, arguments);
  if (formatted < 0 || static_cast<std::size_t>(formatted) >= available) {
    /* Never publish a partial logical record. Callers that can exceed this
     * fixed bound split their evidence into independently parseable records.
     */
    valid_ = false;
    size_ = 0;
  } else {
    size_ += static_cast<std::size_t>(formatted);
  }
  errno = saved_errno;
}

void JitProfileRecord::emit() noexcept {
  if (!valid_ || size_ <= record_leading_separator_size)
    return;
  if (storage_[size_ - 1U] != '\n') {
    if (size_ == storage_.size())
      return;
    storage_[size_++] = '\n';
  }
  write_record(storage_.data(), size_);
}

void write_jit_profile_record(const char *format, ...) noexcept {
  JitProfileRecord record;
  std::va_list arguments;
  va_start(arguments, format);
  record.vappend(format, arguments);
  va_end(arguments);
  record.emit();
}

} // namespace uml::cpu::arm64
