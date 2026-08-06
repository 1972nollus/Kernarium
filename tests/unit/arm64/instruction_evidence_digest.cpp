// SPDX-License-Identifier: GPL-2.0-or-later
#include "instruction_evidence_digest.hpp"

#include <algorithm>

namespace {

constexpr std::array<std::uint32_t, 64> sha256_round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotate_right(std::uint32_t value,
                                                    unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

void sha256_block(std::array<std::uint32_t, 8> &state,
                  const std::byte *block) noexcept {
  std::array<std::uint32_t, 64> words{};
  for (std::size_t index = 0; index < 16; ++index) {
    const auto offset = index * 4U;
    words[index] =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(block[offset]))
         << 24U) |
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[offset + 1U]))
         << 16U) |
        (static_cast<std::uint32_t>(
             std::to_integer<std::uint8_t>(block[offset + 2U]))
         << 8U) |
        static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(block[offset + 3U]));
  }
  for (std::size_t index = 16; index < words.size(); ++index) {
    const auto left = rotate_right(words[index - 15U], 7U) ^
                      rotate_right(words[index - 15U], 18U) ^
                      (words[index - 15U] >> 3U);
    const auto right = rotate_right(words[index - 2U], 17U) ^
                       rotate_right(words[index - 2U], 19U) ^
                       (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + left + words[index - 7U] + right;
  }

  auto a = state[0];
  auto b = state[1];
  auto c = state[2];
  auto d = state[3];
  auto e = state[4];
  auto f = state[5];
  auto g = state[6];
  auto h = state[7];
  for (std::size_t index = 0; index < words.size(); ++index) {
    const auto sigma1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
                        rotate_right(e, 25U);
    const auto choice = (e & f) ^ (~e & g);
    const auto temporary1 = h + sigma1 + choice +
                            sha256_round_constants[index] + words[index];
    const auto sigma0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
                        rotate_right(a, 22U);
    const auto majority = (a & b) ^ (a & c) ^ (b & c);
    const auto temporary2 = sigma0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

} // namespace

namespace uml::emulator::tests::instruction_evidence {

std::uint64_t architecture_state_digest(
    const ::uml::cpu::arm64::CpuState &state) noexcept {
  Digest digest;
  digest_array(digest, state.registers);
  digest.scalar(state.sp);
  digest.scalar(state.pc);
  digest.scalar(state.nzcv);
  for (const auto &vector : state.vectors)
    digest.bytes(vector.bytes.data(), vector.bytes.size());
  digest.scalar(state.fpcr);
  digest.scalar(state.fpsr);
  digest.scalar(state.fpmr);
  digest.scalar(state.tls);
  digest.boolean(state.mte_enabled);
  digest.scalar(state.mte_exclude);
  digest_array(digest, state.mte_tag_choices);
  digest.boolean(state.mte_random_tags);
  digest.scalar(state.mte_seed);
  digest.scalar(state.mte_tag);
  digest.scalar(state.mte_random_state);
  digest.boolean(state.cpa_enabled);
  digest.boolean(state.cpa_multiply_enabled);
  digest.scalar(state.accdata);
  digest.boolean(state.pstate_ssbs);
  digest.boolean(state.pstate_dit);
  digest.boolean(state.pstate_tco);
  digest.boolean(state.pstate_sm);
  digest.boolean(state.pstate_za);
  digest.boolean(state.pstate_daif_access);
  digest.scalar(state.pstate_daif);
  digest.scalar(state.exclusive_address);
  digest.scalar(static_cast<std::uint64_t>(state.exclusive_key));
  digest.scalar(state.exclusive_value);
  digest.scalar(state.exclusive_value2);
  digest.scalar(state.exclusive_version);
  digest.scalar(state.exclusive_size);
  digest.scalar(state.gcs_pointer);
  digest.boolean(state.gcs_implemented);
  digest.boolean(state.gcs_push_enabled);
  digest.boolean(state.gcs_store_enabled);
  for (const auto &key : state.pointer_auth_keys) {
    digest.scalar(key.low);
    digest.scalar(key.high);
  }
  digest.boolean(state.pointer_authentication_enabled);
  return digest.value();
}

std::uint64_t empty_memory_digest() noexcept {
  Digest digest;
  digest.scalar<std::size_t>(0);
  return digest.value();
}

std::array<std::uint8_t, 32>
sha256(std::span<const std::byte> bytes) noexcept {
  const auto total_size = bytes.size();
  std::array<std::uint32_t, 8> state{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  while (bytes.size() >= 64U) {
    sha256_block(state, bytes.data());
    bytes = bytes.subspan(64U);
  }
  std::array<std::byte, 128> tail{};
  std::copy(bytes.begin(), bytes.end(), tail.begin());
  tail[bytes.size()] = std::byte{0x80U};
  const auto padded_size = bytes.size() < 56U ? 64U : 128U;
  const auto bit_length = static_cast<std::uint64_t>(total_size) * 8U;
  for (std::size_t index = 0; index < 8U; ++index)
    tail[padded_size - 1U - index] =
        static_cast<std::byte>(bit_length >> (index * 8U));
  for (std::size_t offset = 0; offset < padded_size; offset += 64U)
    sha256_block(state, tail.data() + offset);

  std::array<std::uint8_t, 32> result{};
  for (std::size_t word = 0; word < state.size(); ++word)
    for (std::size_t byte = 0; byte < 4U; ++byte)
      result[word * 4U + byte] =
          static_cast<std::uint8_t>(state[word] >> ((3U - byte) * 8U));
  return result;
}

std::string sha256_hex(std::span<const std::byte> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  const auto digest = sha256(bytes);
  std::string result;
  result.reserve(digest.size() * 2U);
  for (const auto byte : digest) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 15U]);
  }
  return result;
}

} // namespace uml::emulator::tests::instruction_evidence
