// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace uml::emulator::tests::instruction_evidence {

class Digest final {
public:
  void bytes(const void *data, std::size_t size) noexcept {
    const auto *current = static_cast<const std::byte *>(data);
    for (std::size_t index = 0; index < size; ++index) {
      value_ ^= std::to_integer<std::uint8_t>(current[index]);
      value_ *= 1099511628211ULL;
    }
  }

  template <typename Value> void scalar(Value value) noexcept {
    static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>);
    if constexpr (std::is_enum_v<Value>) {
      scalar(static_cast<std::underlying_type_t<Value>>(value));
      return;
    }
    using Unsigned = std::make_unsigned_t<Value>;
    const auto raw = static_cast<Unsigned>(value);
    for (std::size_t byte = 0; byte < sizeof(raw); ++byte) {
      const auto part = static_cast<std::uint8_t>(raw >> (byte * 8U));
      bytes(&part, sizeof(part));
    }
  }

  void boolean(bool value) noexcept { scalar<std::uint8_t>(value ? 1U : 0U); }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
  std::uint64_t value_{1469598103934665603ULL};
};

template <typename Value, std::size_t Size>
void digest_array(Digest &digest,
                  const std::array<Value, Size> &values) noexcept {
  for (const auto &value : values)
    digest.scalar(value);
}

[[nodiscard]] std::uint64_t architecture_state_digest(
    const ::uml::cpu::arm64::CpuState &state) noexcept;

[[nodiscard]] std::uint64_t empty_memory_digest() noexcept;

[[nodiscard]] std::array<std::uint8_t, 32>
sha256(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::string
sha256_hex(std::span<const std::byte> bytes);

[[nodiscard]] inline std::string sha256_hex(std::string_view text) {
  return sha256_hex(
      std::as_bytes(std::span<const char>{text.data(), text.size()}));
}

} // namespace uml::emulator::tests::instruction_evidence
