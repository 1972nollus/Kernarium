/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/engine_adapters.hpp"

#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <memory>

namespace uml::cpu::arm64::detail {
namespace {

class JitEngine final : public CpuEngine {
public:
	explicit JitEngine(EngineConfiguration configuration)
		: executor_{JitExecutorOptions{
			.enable_guarded_control_flow =
				configuration.enable_guarded_control_flow,
			.enable_pointer_authentication =
				configuration.enable_pointer_authentication,
			.host_capabilities = configuration.host_capabilities,
		}}
	{
	}

	[[nodiscard]] EngineKind kind() const noexcept override
	{
		return EngineKind::jit;
	}

	[[nodiscard]] ExecutionResult run(ExecutionRequest &request) noexcept override
	{
		if (!native_jit_available())
			return {.reason = ExitReason::fatal};
		try {
			return run_translated(executor_, request);
		} catch (...) {
			return {.reason = ExitReason::fatal};
		}
	}

private:
	JitExecutor executor_;
};

} // namespace

std::unique_ptr<CpuEngine> make_jit_engine(EngineConfiguration configuration)
{
	return std::make_unique<JitEngine>(configuration);
}

bool native_jit_engine_available() noexcept
{
	return native_jit_available();
}

} // namespace uml::cpu::arm64::detail
