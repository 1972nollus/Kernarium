// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/engine.hpp"
#include "cpu/host_capabilities.hpp"

namespace uml::emulator::tests {

/*
 * Test-only reference execution engine.
 *
 * The A64 conformance harness compares every instruction case against the
 * arm64 instruction Dispatcher, which remains the semantic reference oracle.
 * This adapter gives that oracle the same request/result shape the production
 * translated engines expose, so one lane loop can drive all three.
 *
 * It is deliberately not a `uml::cpu::CpuEngine`: the product selects only
 * translated engines, and a reference lane must not be reachable from a
 * production engine factory or linked into a production target.
 */
class ReferenceEngine final {
public:
  explicit ReferenceEngine(
      ::uml::cpu::EngineConfiguration configuration) noexcept
      : host_capabilities_{configuration.host_capabilities} {}

  /* Mirrors the production engine boundary: guest exits, including an
   * internal host-resource failure, are returned as an ExecutionResult and
   * never unwind out of this call. */
  [[nodiscard]] ::uml::cpu::ExecutionResult
  run(::uml::cpu::ExecutionRequest &request) noexcept;

private:
  ::uml::cpu::HostCapabilities host_capabilities_;
};

} // namespace uml::emulator::tests
