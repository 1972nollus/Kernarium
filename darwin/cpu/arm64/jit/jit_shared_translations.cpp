// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_shared_translations.hpp"

#include "jit_diagnostics.hpp"

#include <algorithm>
#include <mutex>
#if UML_ENABLE_JIT_PROFILE
#include <cstdio>
#include <cstdlib>
#endif

namespace uml::cpu::arm64 {

namespace {

#if UML_ENABLE_JIT_PROFILE
/* Compiled out of ordinary production objects, which must carry no optional
 * profiling control. See the engine object boundary audit.
 */
[[nodiscard]] bool shared_translation_report_enabled() noexcept {
  static const bool enabled = [] {
    const auto *value = std::getenv("UML_JIT_PROFILE_SHARED_TRANSLATIONS");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}
#endif

[[nodiscard]] bool same_guest_words(
    const std::vector<std::uint32_t> &stored,
    std::span<const std::uint32_t> requested) noexcept {
  return stored.size() == requested.size() &&
         std::equal(stored.begin(), stored.end(), requested.begin());
}

} // namespace

bool SharedTranslationCache::CodegenProfile::operator==(
    const CodegenProfile &other) const noexcept {
  return guarded_control_flow == other.guarded_control_flow &&
         pointer_authentication == other.pointer_authentication &&
         capabilities.supported == other.capabilities.supported &&
         capabilities.floating_point == other.capabilities.floating_point &&
         capabilities.advanced_simd == other.capabilities.advanced_simd &&
         capabilities.fp16 == other.capabilities.fp16 &&
         capabilities.cssc == other.capabilities.cssc &&
         capabilities.aes == other.capabilities.aes &&
         capabilities.pmull == other.capabilities.pmull &&
         capabilities.sha1 == other.capabilities.sha1 &&
         capabilities.sha2 == other.capabilities.sha2 &&
         capabilities.crc32 == other.capabilities.crc32 &&
         capabilities.sha3 == other.capabilities.sha3 &&
         capabilities.sha512 == other.capabilities.sha512 &&
         capabilities.afp == other.capabilities.afp &&
         capabilities.jscvt_subnormal_flags_correct ==
             other.capabilities.jscvt_subnormal_flags_correct;
}

std::shared_ptr<const SharedTranslation> SharedTranslationCache::find(
    const CodegenProfile &profile, std::uint64_t guest_pc,
    std::span<const std::uint32_t> guest_words) const {
  {
    const auto &shard = shards_[shard_of(guest_pc)];
    const std::shared_lock cache_lock{shard.mutex};
    const auto bucket = shard.entries.find(guest_pc);
    if (bucket != shard.entries.end())
      for (const auto &entry : bucket->second)
        if (entry.profile == profile &&
            same_guest_words(entry.translation->guest_words, guest_words)) {
          hits_.fetch_add(1, std::memory_order_relaxed);
          return entry.translation;
        }
  }
  misses_.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void SharedTranslationCache::insert(
    const CodegenProfile &profile, std::uint64_t guest_pc,
    std::span<const std::uint32_t> guest_words,
    std::shared_ptr<const SharedTranslation> translation) {
  if (live_executors_.load(std::memory_order_relaxed) < 2U)
    return;
  auto &shard = shards_[shard_of(guest_pc)];
  const std::unique_lock cache_lock{shard.mutex};
  if (shard.count >= shard_maximum_entries) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto &bucket = shard.entries[guest_pc];
  /* Another executor may have inserted the same translation between this
   * one's failed lookup and here. Keep the first.
   */
  for (const auto &entry : bucket)
    if (entry.profile == profile &&
        same_guest_words(entry.translation->guest_words, guest_words))
      return;
  bucket.push_back({profile, std::move(translation)});
  ++shard.count;
  const auto published =
      insertions_.fetch_add(1, std::memory_order_relaxed) + 1U;
#if UML_ENABLE_JIT_PROFILE
  if (shared_translation_report_enabled() && (published % 65536U) == 0U)
    std::fprintf(stderr,
                 "Darwin JIT shared translations entries=%zu published=%llu "
                 "reused=%llu missed=%llu rejected=%llu\n",
                 shard.count, static_cast<unsigned long long>(published),
                 static_cast<unsigned long long>(
                     hits_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     misses_.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     rejected_.load(std::memory_order_relaxed)));
#else
  (void)published;
#endif
}

void SharedTranslationCache::add_executor() noexcept {
  live_executors_.fetch_add(1, std::memory_order_relaxed);
}

void SharedTranslationCache::remove_executor() noexcept {
  live_executors_.fetch_sub(1, std::memory_order_relaxed);
}

SharedTranslationStatistics
SharedTranslationCache::statistics() const noexcept {
  std::size_t entries = 0;
  for (const auto &shard : shards_) {
    const std::shared_lock cache_lock{shard.mutex};
    entries += shard.count;
  }
  return {.hits = hits_.load(std::memory_order_relaxed),
          .misses = misses_.load(std::memory_order_relaxed),
          .insertions = insertions_.load(std::memory_order_relaxed),
          .rejected = rejected_.load(std::memory_order_relaxed),
          .entries = entries};
}

SharedTranslationCache &SharedTranslationCache::instance() noexcept {
  /* Intentionally leaked. Executors flush into it from their destructors,
   * which can run after a function-local static would have been destroyed.
   */
  static auto *cache = new SharedTranslationCache{};
  return *cache;
}

} // namespace uml::cpu::arm64
