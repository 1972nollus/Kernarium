#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the checked JIT profiling configure/build/test workflow."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import subprocess
from pathlib import Path
from typing import Any


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def indexed_presets(document: dict[str, Any], key: str) -> dict[str, Any]:
    presets = document.get(key)
    require(isinstance(presets, list), f"{key} is not a list")
    result: dict[str, Any] = {}
    for preset in presets:
        require(isinstance(preset, dict), f"{key} contains a non-object")
        name = preset.get("name")
        require(isinstance(name, str) and name, f"{key} has an invalid name")
        require(name not in result, f"{key} repeats preset {name}")
        result[name] = preset
    return result


def effective_cache_variables(
    name: str,
    presets: dict[str, Any],
    resolving: frozenset[str] = frozenset(),
) -> dict[str, Any]:
    require(name in presets, f"unknown configure preset {name}")
    require(name not in resolving, f"configure-preset inheritance cycle at {name}")
    preset = presets[name]
    parents = preset.get("inherits", [])
    if isinstance(parents, str):
        parents = [parents]
    require(isinstance(parents, list), f"{name}: inherits is not a string or list")

    result: dict[str, Any] = {}
    for parent in reversed(parents):
        require(isinstance(parent, str), f"{name}: invalid inherited preset")
        result.update(
            effective_cache_variables(
                parent, presets, resolving | frozenset((name,))
            )
        )
    variables = preset.get("cacheVariables", {})
    require(isinstance(variables, dict), f"{name}: cacheVariables is not an object")
    result.update(variables)
    return result


def list_presets(cmake: Path, source_root: Path, kind: str, name: str) -> None:
    completed = subprocess.run(
        [str(cmake), f"--list-presets={kind}"],
        cwd=source_root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    require(
        completed.returncode == 0,
        f"CMake rejected {kind} presets:\n{completed.stdout}",
    )
    require(
        f'"{name}"' in completed.stdout,
        f"CMake did not list {kind} preset {name}:\n{completed.stdout}",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    arguments = parser.parse_args()

    source_root = arguments.source_root.resolve()
    preset_path = source_root / "CMakePresets.json"
    document = json.loads(preset_path.read_text(encoding="utf-8"))
    configure = indexed_presets(document, "configurePresets")
    build = indexed_presets(document, "buildPresets")
    test = indexed_presets(document, "testPresets")
    workflow = indexed_presets(document, "workflowPresets")

    checked_product_selectors = {
        "UML_GADGET_ADD_SUB_IMMEDIATE_SUCCESSOR_HOIST": False,
        "UML_JIT_CARRY_X8_X9_ACROSS_LINKS": False,
        "UML_JIT_SHARE_TRANSLATIONS_BETWEEN_EXECUTORS": True,
    }
    for name in configure:
        variables = effective_cache_variables(name, configure)
        for variable, expected in checked_product_selectors.items():
            require(
                variables.get(variable) == expected,
                f"{name}: effective {variable} is not {expected!r}",
            )

    profile = configure.get("macos-jitprof")
    require(profile is not None, "missing macos-jitprof configure preset")
    require(
        profile.get("inherits") == "macos-integration",
        "macos-jitprof must inherit the production integration configuration",
    )
    require(
        profile.get("binaryDir")
        == "${sourceDir}/.build/presets/${presetName}",
        "macos-jitprof does not own the canonical separate build directory",
    )
    variables = effective_cache_variables("macos-jitprof", configure)
    for variable, expected in {
        "CMAKE_BUILD_TYPE": "Release",
        "UML_BUILD_CLI": True,
        "UML_BUILD_KERNEL_RUNNER": True,
        "UML_BUILD_TOOLING_TESTS": True,
        "UML_BUILD_INTEGRATION_TESTS": True,
        "UML_BUILD_UNIT_TESTS": False,
        "UML_GUEST_16K_PAGES": True,
        "UML_ENABLE_RUNTIME_PROFILING": True,
    }.items():
        require(
            variables.get(variable) == expected,
            f"macos-jitprof effective {variable} is not {expected!r}",
        )

    explicitly_profiled = {
        name
        for name, preset in configure.items()
        if preset.get("cacheVariables", {}).get(
            "UML_ENABLE_RUNTIME_PROFILING"
        )
        is True
    }
    require(
        explicitly_profiled == {"macos-jitprof"},
        "runtime profiling must be explicit and isolated to macos-jitprof",
    )

    runtime_contract = (
        source_root / "darwin/cpu/arm64/jit/jit_runtime_contract.hpp"
    ).read_text(encoding="utf-8")
    root_cmake = (source_root / "CMakeLists.txt").read_text(encoding="utf-8")
    require(
        "#if defined(DARWIN_UML_JIT_GPR_LINK_PROFILE)" in runtime_contract
        and "share_translations_between_executors = false;" in runtime_contract,
        "the manual GPR-link profile no longer keeps executor-owned counters private",
    )
    require(
        "if(UML_JIT_CARRY_X8_X9_ACROSS_LINKS AND" in root_cmake
        and "DARWIN_UML_JIT_GPR_LINK_PROFILE" in root_cmake,
        "the manual GPR-link profile no longer rejects the fixed carry bank",
    )

    build_profile = build.get("macos-jitprof")
    require(build_profile is not None, "missing macos-jitprof build preset")
    require(
        build_profile.get("configurePreset") == "macos-jitprof",
        "macos-jitprof build preset uses the wrong configure preset",
    )
    require(
        build_profile.get("targets")
        == ["tooling-test-binaries", "architecture-audit"],
        "macos-jitprof must build the CLI/tooling closure and architecture audit",
    )

    test_profile = test.get("tooling-jitprof")
    require(test_profile is not None, "missing tooling-jitprof test preset")
    require(
        test_profile.get("configurePreset") == "macos-jitprof",
        "tooling-jitprof uses the wrong configure preset",
    )
    selected = (
        test_profile.get("filter", {}).get("include", {}).get("name")
    )
    require(
        selected
        == "^(jit-profile-final-report-contract|jit-profile-preset-contract)$",
        "tooling-jitprof does not select the focused profiling audits",
    )

    workflow_profile = workflow.get("check-jitprof")
    require(workflow_profile is not None, "missing check-jitprof workflow")
    require(
        workflow_profile.get("steps")
        == [
            {"type": "configure", "name": "macos-jitprof"},
            {"type": "build", "name": "macos-jitprof"},
            {"type": "test", "name": "tooling-jitprof"},
        ],
        "check-jitprof does not configure, build, and audit the profiling graph",
    )

    for kind, name in (
        ("configure", "macos-jitprof"),
        ("build", "macos-jitprof"),
        ("test", "tooling-jitprof"),
        ("workflow", "check-jitprof"),
    ):
        list_presets(arguments.cmake, source_root, kind, name)

    app_source = (source_root / "darwin/app/macos/main.cpp").read_text(
        encoding="utf-8"
    )
    report_call = "uml::emulator::report_jit_profiles();"
    require(
        app_source.count(report_call) == 1,
        "the macOS CLI must call the final JIT runtime-profile reporter once",
    )
    require(
        app_source.index(report_call) > app_source.index("uml_emulator_run("),
        "the macOS CLI final reporter is not after its emulator run invocation",
    )
    require(
        app_source.index(report_call) < app_source.index("std::_Exit(result)"),
        "the macOS CLI reports JIT profiles only after std::_Exit",
    )
    require(
        '#include "profile_reporter.hpp"' in app_source
        and "link_profile_api.hpp" not in app_source,
        "the macOS CLI bypasses its private emulator profiling facade",
    )
    emulator_profile_source = (
        source_root / "darwin/emulator/src/profile_reporter.cpp"
    ).read_text(encoding="utf-8")
    require(
        "std::fflush(stderr)" not in emulator_profile_source
        and "runtime/jit_profile_reporter.hpp" in emulator_profile_source
        and "cpu/arm64/jit/link_profile_api.hpp" not in emulator_profile_source,
        "the emulator profiling facade bypasses runtime or uses shared stdio",
    )
    emulator_source = (
        source_root / "darwin/emulator/src/emulator.cpp"
    ).read_text(encoding="utf-8")
    embedded_run = "const auto result = uml_elf_run("
    embedded_report = "report_jit_profiles();"
    require(
        emulator_source.count(embedded_run) == 1
        and emulator_source.count(embedded_report) == 1
        and emulator_source.index(embedded_run)
        < emulator_source.index(embedded_report)
        < emulator_source.index("return result;"),
        "the embedded ELF path lacks its quiescent pre-teardown report",
    )
    runtime_profile_source = (
        source_root / "darwin/runtime/jit_profile_reporter.cpp"
    ).read_text(encoding="utf-8")
    require(
        '#include "cpu/arm64/jit/link_profile_api.hpp"'
        in runtime_profile_source
        and "report_global_gpr_link_profile();" in runtime_profile_source
        and "report_jit_runtime_profiles();" in runtime_profile_source,
        "the runtime profiling facade does not own both CPU reporting bridges",
    )
    pool_profile_header = (
        source_root
        / "darwin/cpu/arm64/jit/jit_executor_pool_profile.hpp"
    ).read_text(encoding="utf-8")
    pool_profile_source = (
        source_root
        / "darwin/cpu/arm64/jit/jit_executor_pool_profile.cpp"
    ).read_text(encoding="utf-8")
    pool_source = (
        source_root / "darwin/cpu/arm64/jit/jit_executor_pool.cpp"
    ).read_text(encoding="utf-8")
    engine_source = (
        source_root / "darwin/cpu/arm64/jit/engine_adapter.cpp"
    ).read_text(encoding="utf-8")
    geometry_source = (
        source_root / "darwin/cpu/arm64/jit/jit_cache_geometry.cpp"
    ).read_text(encoding="utf-8")
    profile_api_source = (
        source_root / "darwin/cpu/arm64/jit/jit_profile_api.cpp"
    ).read_text(encoding="utf-8")
    cpu_cmake = (source_root / "darwin/cpu/CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    require(
        "if(UML_ENABLE_JIT_PROFILE)\n"
        "    list(APPEND _uml_cpu_jit_executor_sources\n"
        "        arm64/jit/jit_executor_pool_profile.cpp" in cpu_cmake,
        "the pool profiler is not confined to the profiling source graph",
    )
    require(
        "UML_JIT_PROFILE_EXECUTOR_POOL" in pool_profile_source
        and "std::getenv" in pool_profile_source
        and "std::atomic" not in pool_profile_source
        and "JitProfileRecord record" in pool_profile_source
        and "sequence-collisions=" not in pool_profile_source
        and "sequence=" not in pool_profile_source,
        "the pool profiler lacks its private control, local aggregation, "
        "bounded writer, or raw-address exclusion",
    )
    require(
        "#if UML_ENABLE_JIT_PROFILE" in pool_profile_header
        and "JitExecutorPoolProfile pool_profile_;" in engine_source
        and "pool_profile_{session_}" in engine_source
        and "pool_profile_.enabled() ? &pool_profile_ : nullptr;"
        in engine_source
        and "pool_profile->observe_sequence(request.interrupt.sequence);"
        in engine_source
        and "if (profile == nullptr || !profile->enabled())" in pool_source
        and "profiled_standalone_" in geometry_source
        and "profile_jit_standalone_executor_constructed();"
        in geometry_source,
        "wrapper sequence or cold standalone lease profiling is unowned",
    )
    require(
        "struct SequenceAssociation final" in pool_profile_header
        and "std::uint64_t session{};" in pool_profile_header
        and "std::uint64_t slot_mask{};" in pool_profile_header
        and "entry.session == session && entry.sequence == sequence"
        in pool_profile_source
        and "entry.slot_mask |= slot_mask;" in pool_profile_source
        and "associate_sequence(current_sequence_, slot);" in pool_profile_source,
        "successful pool borrows lack bounded session/sequence slot associations",
    )
    real_pool_calls = {
        "profile->record_borrow_invocation(": 1,
        "profile->record_cell_probe(": 1,
        "profile->record_fallback_claim(": 1,
        "profile->record_waiter_enqueue(": 1,
        "profile->record_waiter_wake(": 1,
        "profile->record_fifo_handoff(": 1,
        "profile->record_construction_attempt(": 1,
        "profile->record_created(": 1,
        "profile->record_successful_borrow(": 1,
    }
    for call, expected in real_pool_calls.items():
        require(
            pool_source.count(call) == expected,
            f"executor-pool profile callsite count drifted for {call!r}",
        )
    require(
        pool_source.count("#if UML_ENABLE_JIT_PROFILE") >= 12
        and engine_source.count("#if UML_ENABLE_JIT_PROFILE") >= 3
        and geometry_source.count("#if UML_ENABLE_JIT_PROFILE") >= 4,
        "ordinary engine, pool, or lease builds no longer preprocess every "
        "trace reference out",
    )
    pool_report = "report_final_jit_executor_pool_profile();"
    shared_report = "report_final_shared_translation_statistics();"
    require(
        profile_api_source.count(pool_report) == 1
        and profile_api_source.index(pool_report)
        < profile_api_source.index(shared_report),
        "the executor-pool reporter is not called before the shared reporter",
    )
    profile_output_source = (
        source_root / "darwin/cpu/arm64/jit/jit_profile_output.cpp"
    ).read_text(encoding="utf-8")
    require(
        "::write(STDERR_FILENO" in profile_output_source
        and "::poll(" in profile_output_source
        and "EINTR" in profile_output_source
        and "EAGAIN" in profile_output_source
        and "EWOULDBLOCK" in profile_output_source
        and "storage_[0] = record_leading_separator" in profile_output_source
        and "buffer[0] = record_leading_separator" in profile_output_source
        and "buffer + record_leading_separator_size" in profile_output_source
        and "Never publish a partial logical record" in profile_output_source
        and "fprintf" not in profile_output_source
        and "fflush" not in profile_output_source,
        "runtime JIT profiles do not use bounded direct descriptor output",
    )
    require(
        "arm64/jit/jit_profile_output.cpp" in cpu_cmake,
        "the CPU-owned runtime profile writer is absent from its component",
    )
    for relative in (
        "darwin/cpu/arm64/jit/jit_diagnostics.cpp",
        "darwin/cpu/arm64/jit/jit_shared_translations.cpp",
    ):
        profile_source = (source_root / relative).read_text(encoding="utf-8")
        require(
            "std::fprintf" not in profile_source
            and "std::fputs" not in profile_source
            and "std::fputc" not in profile_source
            and "output_mutex" not in profile_source,
            f"{relative} still acquires a shared stdio FILE lock",
        )
    diagnostics_source = (
        source_root / "darwin/cpu/arm64/jit/jit_diagnostics.cpp"
    ).read_text(encoding="utf-8")
    shared_source = (
        source_root / "darwin/cpu/arm64/jit/jit_shared_translations.cpp"
    ).read_text(encoding="utf-8")
    require(
        "expansion_words_per_record = 24" in diagnostics_source
        and "section=%s" in diagnostics_source
        and "chunk=%zu/%zu" in diagnostics_source
        and "final_reported_profile" in diagnostics_source
        and "final_reported_translations" in diagnostics_source
        and "same_shared_translation_statistics" in shared_source,
        "runtime profiles lack bounded expansion chunks or changed-state finals",
    )
    fixture_source = (
        source_root / "tests/tooling/cmake/jit_profile_final_report_fixture.cpp"
    ).read_text(encoding="utf-8")
    require(
        "::flockfile(stderr)" in fixture_source
        and "::funlockfile(stderr)" in fixture_source
        and "prove_nonblocking_stderr_backpressure_and_framing" in fixture_source
        and "O_NONBLOCK" in fixture_source
        and "prefix_is_unterminated" in fixture_source
        and "expected_suffix" in fixture_source
        and "JitProfileRecord assembled" in fixture_source
        and "expansion_worker_a" in fixture_source
        and "expansion_worker_b" in fixture_source
        and "phase=retired-unchanged" in fixture_source
        and "phase=pool-only-complete" in fixture_source
        and "live_pool_profile.reset();" in fixture_source
        and "0x5000" in fixture_source,
        "the final-report fixture omits FILE-lock, concurrency, or rerun coverage",
    )
    for synthetic_call in (
        "third.record_waiter_enqueue(3);",
        "third.record_waiter_wake();",
        "third.record_fifo_handoff();",
    ):
        require(
            fixture_source.count(synthetic_call) == 1,
            f"deterministic pool fixture drifted at {synthetic_call!r}",
        )

    print("JIT profiling preset contract passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
