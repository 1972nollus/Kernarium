/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cpu/arm64/engine_adapters.hpp"

#include "cpu/arm64/engine_execution.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_executor_pool.hpp"
#if UML_ENABLE_JIT_PROFILE
#include "cpu/arm64/jit/jit_executor_pool_profile.hpp"
#endif

#include <memory>

namespace uml::cpu::arm64::detail {
namespace {

class JitEngine final : public CpuEngine {
public:
	explicit JitEngine(EngineConfiguration configuration)
		: options_{JitExecutorOptions{
			.enable_guarded_control_flow =
				configuration.enable_guarded_control_flow,
			.enable_pointer_authentication =
				configuration.enable_pointer_authentication,
			.host_capabilities = configuration.host_capabilities,
		}}, session_{active_jit_executor_pool_session()}
#if UML_ENABLE_JIT_PROFILE
		, pool_profile_{session_}
#endif
	{
		if (session_ != 0) {
			pool_configuration_admitted_ =
				bind_jit_executor_pool_configuration(session_, options_);
		} else {
			standalone_ = std::make_unique<JitExecutor>(options_);
		}
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
#if UML_ENABLE_JIT_PROFILE
			auto *pool_profile =
				pool_profile_.enabled() ? &pool_profile_ : nullptr;
			if (pool_profile != nullptr)
				pool_profile->observe_sequence(request.interrupt.sequence);
#endif
			if (session_ == 0)
				return standalone_ == nullptr ?
					ExecutionResult{.reason = ExitReason::fatal} :
					run_translated(*standalone_, request);
			if (!pool_configuration_admitted_)
				return {.reason = ExitReason::fatal};
			auto borrowed = borrow_jit_executor(session_, preferred_slot_
#if UML_ENABLE_JIT_PROFILE
				, pool_profile
#endif
				);
			if (!borrowed)
				return {.reason = ExitReason::fatal};
			preferred_slot_ = borrowed.slot();
			return run_translated(borrowed.executor(), request);
		} catch (...) {
			return {.reason = ExitReason::fatal};
		}
	}

private:
	JitExecutorOptions options_;
	JitExecutorPoolSessionToken session_{};
	std::unique_ptr<JitExecutor> standalone_;
	unsigned preferred_slot_{maximum_jit_cache_executors};
	bool pool_configuration_admitted_{};
#if UML_ENABLE_JIT_PROFILE
	JitExecutorPoolProfile pool_profile_;
#endif
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
