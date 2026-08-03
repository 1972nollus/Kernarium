/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/engine_adapters.hpp"

#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/gadgets/executor.hpp"

#include <memory>

namespace uml::cpu::arm64::detail {
namespace {

class GadgetEngine final : public CpuEngine {
public:
	explicit GadgetEngine(EngineConfiguration configuration)
		: executor_{gadgets::BuildOptions{
			.enable_pointer_authentication =
				configuration.enable_pointer_authentication,
			.enable_guarded_control_flow =
				configuration.enable_guarded_control_flow,
			.host_capabilities = configuration.host_capabilities,
		}}
	{
	}

	[[nodiscard]] EngineKind kind() const noexcept override
	{
		return EngineKind::gadgets;
	}

	[[nodiscard]] ExecutionResult run(ExecutionRequest &request) noexcept override
	{
		try {
			return run_translated(executor_, request);
		} catch (...) {
			return {.reason = ExitReason::fatal};
		}
	}

private:
	gadgets::GadgetExecutor executor_;
};

} // namespace

std::unique_ptr<CpuEngine> make_gadget_engine(EngineConfiguration configuration)
{
	return std::make_unique<GadgetEngine>(configuration);
}

} // namespace uml::cpu::arm64::detail
