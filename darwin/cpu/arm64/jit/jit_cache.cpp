// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_cache.hpp"

namespace uml::cpu::arm64 {

#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)
namespace {

void add_gpr_link_profile_totals(
    JitGprLinkProfileDiagnostics &destination,
    const JitGprLinkProfileDiagnostics &source) noexcept {
  destination.successful_links += source.successful_links;
  destination.unattributed_links += source.unattributed_links;
  destination.outer_invocations += source.outer_invocations;
  for (std::size_t reg = 0; reg < destination.live_in_saves.size(); ++reg) {
    destination.live_in_saves[reg] += source.live_in_saves[reg];
    destination.dirty_out_saves[reg] += source.dirty_out_saves[reg];
    destination.full_handoffs[reg] += source.full_handoffs[reg];
  }
}

} // namespace

std::size_t JitGprProfileEdgeHash::operator()(
    const JitGprProfileEdgeKey &key) const noexcept {
  auto hash = static_cast<std::size_t>(key.source_pc ^ (key.source_pc >> 32U));
  const auto mix = [&](std::uint64_t value) {
    hash ^= static_cast<std::size_t>(value ^ (value >> 32U)) +
            0x9e37'79b9U + (hash << 6U) + (hash >> 2U);
  };
  mix(key.target_pc);
  mix((static_cast<std::uint64_t>(key.dirty_live_out) << 32U) |
      key.read_before_write);
  mix(key.indirect);
  return hash;
}

JitGprLinkProfileGlobal &JitGprLinkProfileGlobal::instance() noexcept {
  static auto *aggregate = [] {
    auto *created = new JitGprLinkProfileGlobal;
    if (std::atexit(&JitGprLinkProfileGlobal::report_at_exit) != 0)
      std::fprintf(stderr,
                   "Darwin JIT GPR link profile cannot register report\n");
    return created;
  }();
  return *aggregate;
}

void JitGprLinkProfileGlobal::merge(
    const JitGprLinkProfileDiagnostics &totals,
    const Edges &edges) noexcept {
  std::scoped_lock lock{mutex_};
  add_gpr_link_profile_totals(totals_, totals);
  for (const auto &[edge, count] : edges)
    edges_[edge] += count;
}

void JitGprLinkProfileGlobal::register_live(void *context,
                                            LiveCollector collect) {
  std::scoped_lock lock{mutex_};
  live_.push_back({context, collect});
}

void JitGprLinkProfileGlobal::unregister_live(void *context) noexcept {
  std::scoped_lock lock{mutex_};
  live_.erase(
      std::remove_if(live_.begin(), live_.end(),
                     [&](const auto &profile) {
                       return profile.context == context;
                     }),
      live_.end());
}

JitGprLinkProfileDiagnostics
JitGprLinkProfileGlobal::diagnostics() noexcept {
  std::scoped_lock lock{mutex_};
  return totals_;
}

void JitGprLinkProfileGlobal::report() noexcept {
  std::scoped_lock lock{mutex_};
  if (reported_)
    return;
  reported_ = true;
  for (const auto &profile : live_)
    profile.collect(profile.context, totals_, edges_);
  const auto *path = std::getenv("UML_JIT_GPR_LINK_PROFILE_PATH");
  auto *output =
      path == nullptr || path[0] == '\0' ? stderr : std::fopen(path, "a");
  if (output == nullptr) {
    std::fprintf(stderr,
                 "Darwin JIT GPR link profile cannot open path=%s\n", path);
    output = stderr;
  }
  std::fprintf(
      output,
      "darwin-jit-gpr-link-summary executor=global successful=%llu "
      "unattributed=%llu outer=%llu\n",
      static_cast<unsigned long long>(totals_.successful_links),
      static_cast<unsigned long long>(totals_.unattributed_links),
      static_cast<unsigned long long>(totals_.outer_invocations));
  for (const auto &[edge, count] : edges_) {
    std::fprintf(
        output,
        "darwin-jit-gpr-link-edge executor=global kind=%s source=%#llx "
        "target=%#llx dirty=%#x livein=%#x count=%llu\n",
        edge.indirect ? "pic" : "direct",
        static_cast<unsigned long long>(edge.source_pc),
        static_cast<unsigned long long>(edge.target_pc), edge.dirty_live_out,
        edge.read_before_write, static_cast<unsigned long long>(count));
  }
  for (unsigned reg = 0; reg < totals_.live_in_saves.size(); ++reg) {
    const auto saved =
        totals_.live_in_saves[reg] + totals_.dirty_out_saves[reg];
    if (reg == 31U)
      std::fprintf(
          output,
          "darwin-jit-gpr-link-reg executor=global reg=sp livein=%llu "
          "dirty=%llu overlap=%llu saved=%llu\n",
          static_cast<unsigned long long>(totals_.live_in_saves[reg]),
          static_cast<unsigned long long>(totals_.dirty_out_saves[reg]),
          static_cast<unsigned long long>(totals_.full_handoffs[reg]),
          static_cast<unsigned long long>(saved));
    else
      std::fprintf(
          output,
          "darwin-jit-gpr-link-reg executor=global reg=x%u livein=%llu "
          "dirty=%llu overlap=%llu saved=%llu\n",
          reg, static_cast<unsigned long long>(totals_.live_in_saves[reg]),
          static_cast<unsigned long long>(totals_.dirty_out_saves[reg]),
          static_cast<unsigned long long>(totals_.full_handoffs[reg]),
          static_cast<unsigned long long>(saved));
  }
  std::fflush(output);
  if (output != stderr)
    std::fclose(output);
}

void JitGprLinkProfileGlobal::report_at_exit() noexcept {
  instance().report();
}

JitGprLinkProfiler::JitGprLinkProfiler() noexcept {
  JitGprLinkProfileGlobal::instance().register_live(
      this,
      [](void *context, JitGprLinkProfileDiagnostics &totals,
         JitGprLinkProfileGlobal::Edges &edges) noexcept {
        static_cast<JitGprLinkProfiler *>(context)->collect(totals, edges);
      });
}

JitGprLinkProfiler::~JitGprLinkProfiler() {
  JitGprLinkProfileGlobal::instance().unregister_live(this);
}

std::uint64_t *JitGprLinkProfiler::add_direct(
    const JitBlockLink *target, std::uint64_t source_pc,
    std::uint32_t dirty_live_out) {
  auto sample = std::make_unique<JitGprDirectLinkSample>();
  sample->target = target;
  sample->source_pc = source_pc;
  sample->dirty_live_out = dirty_live_out;
  auto *counter = &sample->successes;
  direct_.push_back(std::move(sample));
  return counter;
}

jit::JitIndirectLinkProfileCounters JitGprLinkProfiler::add_indirect(
    const JitIndirectLink *site, std::uint64_t source_pc,
    std::uint32_t dirty_live_out) {
  auto sample = std::make_unique<JitGprIndirectLinkSample>();
  sample->site = site;
  sample->source_pc = source_pc;
  sample->dirty_live_out = dirty_live_out;
  auto *first = &sample->successes[0];
  auto *second = &sample->successes[1];
  indirect_by_site_[site].push_back(sample.get());
  indirect_.push_back(std::move(sample));
  return {first, second};
}

void JitGprLinkProfiler::record_outer_invocation() noexcept {
  ++lifetime_totals_.outer_invocations;
  ++pending_totals_.outer_invocations;
}

void JitGprLinkProfiler::drain_direct() noexcept {
  for (const auto &sample : direct_)
    drain(sample->successes, sample->target, sample->source_pc,
          sample->dirty_live_out, false);
}

void JitGprLinkProfiler::drain_indirect_slot(
    const JitIndirectLink &site, std::size_t slot) noexcept {
  const auto found = indirect_by_site_.find(&site);
  if (found == indirect_by_site_.end())
    return;
  const auto *target = site.profile_targets[slot];
  for (auto *sample : found->second)
    drain(sample->successes[slot], target, sample->source_pc,
          sample->dirty_live_out, true);
}

void JitGprLinkProfiler::drain_indirect_site(
    const JitIndirectLink &site) noexcept {
  drain_indirect_slot(site, 0);
  drain_indirect_slot(site, 1);
}

void JitGprLinkProfiler::drain_all() noexcept {
  drain_direct();
  for (const auto &sample : indirect_) {
    for (std::size_t slot = 0; slot < sample->successes.size(); ++slot)
      drain(sample->successes[slot], sample->site->profile_targets[slot],
            sample->source_pc, sample->dirty_live_out, true);
  }
}

void JitGprLinkProfiler::clear_code() noexcept {
  drain_all();
  direct_.clear();
  indirect_.clear();
  indirect_by_site_.clear();
}

JitGprLinkProfileDiagnostics JitGprLinkProfiler::diagnostics() noexcept {
  drain_all();
  return lifetime_totals_;
}

void JitGprLinkProfiler::publish() noexcept {
  drain_all();
  if (pending_totals_.successful_links == 0 &&
      pending_totals_.unattributed_links == 0 &&
      pending_totals_.outer_invocations == 0 && pending_edges_.empty())
    return;
  JitGprLinkProfileGlobal::instance().merge(pending_totals_, pending_edges_);
  pending_totals_ = {};
  pending_edges_.clear();
}

void JitGprLinkProfiler::collect(
    JitGprLinkProfileDiagnostics &totals,
    JitGprLinkProfileGlobal::Edges &edges) noexcept {
  drain_all();
  add_gpr_link_profile_totals(totals, pending_totals_);
  for (const auto &[edge, count] : pending_edges_)
    edges[edge] += count;
  pending_totals_ = {};
  pending_edges_.clear();
}

void JitGprLinkProfiler::drain(std::uint64_t &counter,
                               const JitBlockLink *target,
                               std::uint64_t source_pc,
                               std::uint32_t dirty_live_out,
                               bool indirect) noexcept {
  const auto count = std::exchange(counter, std::uint64_t{});
  if (count == 0)
    return;
  lifetime_totals_.successful_links += count;
  pending_totals_.successful_links += count;
  if (target == nullptr || !target->profile_mask_valid) {
    lifetime_totals_.unattributed_links += count;
    pending_totals_.unattributed_links += count;
    return;
  }
  const auto read_before_write = target->profile_read_before_write;
  const JitGprProfileEdgeKey key{
      .source_pc = source_pc,
      .target_pc = target->guest_pc,
      .dirty_live_out = dirty_live_out,
      .read_before_write = read_before_write,
      .indirect = indirect,
  };
  pending_edges_[key] += count;
  for (unsigned reg = 0; reg < lifetime_totals_.live_in_saves.size(); ++reg) {
    const auto bit = std::uint32_t{1} << reg;
    if ((read_before_write & bit) != 0) {
      lifetime_totals_.live_in_saves[reg] += count;
      pending_totals_.live_in_saves[reg] += count;
    }
    if ((dirty_live_out & bit) != 0) {
      lifetime_totals_.dirty_out_saves[reg] += count;
      pending_totals_.dirty_out_saves[reg] += count;
    }
    if (((read_before_write & dirty_live_out) & bit) != 0) {
      lifetime_totals_.full_handoffs[reg] += count;
      pending_totals_.full_handoffs[reg] += count;
    }
  }
}
#endif

} // namespace uml::cpu::arm64
