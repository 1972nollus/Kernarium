#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Focused preflight fixtures for the real ASLRef materialization batch."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import sys
import tempfile
import time
from dataclasses import replace
from pathlib import Path

import probe_a64_aslref_materialization_batch as probe
from a64_bounded_generation import BoundedWorkspace, GenerationLimits
from probe_a64_aslref_materialization_batch import (
    IMPLEMENTATION_DEFINED_ADAPTER_SHA256,
    IMPDEF_POLICY_SHA256,
    IMPDEF_PROJECTION_BY_KEY,
    IMPDEF_PROJECTIONS,
    PAC_ALGORITHM,
    PAC_RELATION,
    ARCH_MAX_MOPS_CPY_SIZE,
    CPY_POST_RESERVE_BYTES,
    COMPACT_MAX_MEMORY_ADDRESS,
    COMPACT_MAX_MEMORY_DESCRIPTOR_SHA256,
    COMPACT_MAX_MEMORY_FORMULA_SEED,
    COMPACT_MAX_MEMORY_LENGTH,
    COMPACT_MAX_MEMORY_TAG_LENGTH,
    FULL_EXTENT_MIN_RSS_KIB,
    FULL_EXTENT_MIN_TIMEOUT_SECONDS,
    FULL_EXTENT_RESOURCE_LOCK,
    FULL_EXTENT_CAPTURE_PROFILES,
    asl_definition_entries,
    asl_definition_signatures,
    compact_max_memory_descriptor_digest,
    generated_compact_max_memory,
    generated_full_extent_transfer_memory,
    impdef_context_digest,
    impdef_policy_digest,
    cpy_post_size_policy,
    implementation_defined_adapter_digest,
    normalize_reachable_tuple_forwards,
    normalize_virtual_fault_projection,
    require_impdef_context_digest,
    require_mops_whole_sequence,
    require_virtual_fault_observation,
    require_reachable_impdef_projections,
    require_supplied_interface_signature_digest,
    require_tuple_forward_inventory,
    resolve_pac_algorithm,
    supplied_interface_signature_digest,
    expected_physical_fault_observation,
    expected_virtual_fault_observation,
    execute_measured,
    prepare_generated_directory,
    require_generated_artifact_limit,
    tuple_forward_inventory,
    tuple_forward_inventory_digest,
    validate_impdef_projection,
    validate_cpy_post_size_policy,
    verify_cpy_post_size_policy,
    verify_compact_max_memory,
    verify_full_extent_transfer_memory,
    verify_impdef_value_policies,
    verify_pac_dependency,
)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-root", type=Path)
    parser.add_argument("--features-json", type=Path)
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def expect_rejected(action, message: str) -> None:
    try:
        action()
    except ValueError:
        return
    raise AssertionError(message)


def generated_directory_preflight_fixture(root: Path) -> None:
    architecture = root / "architecture"
    capture = root / "capture"
    closure = root / "closure"
    tools = root / "tools"
    input_root = root / "inputs"
    safe_parent = root / "generated"
    for directory in (
        architecture,
        capture,
        closure,
        tools,
        input_root,
        safe_parent,
    ):
        directory.mkdir()
    files = {
        name: tools / name
        for name in (
            "aslref",
            "bundler.py",
            "Features.json",
            "variants.tsv",
            "interface.asl",
            "observer.asl",
        )
    }
    for path in files.values():
        path.write_text("fixture\n", encoding="utf-8")
    input_path = input_root / "shard.inputs.jsonl.gz"
    input_path.write_bytes(b"fixture")
    options = argparse.Namespace(
        architecture_root=architecture,
        capture_directory=capture,
        closure_capture_directory=closure,
        aslref=files["aslref"],
        bundler=files["bundler.py"],
        features_json=files["Features.json"],
        required_variants=files["variants.tsv"],
        interface=files["interface.asl"],
        observer=files["observer.asl"],
        input_jsonl=[input_path],
        generated_directory=safe_parent / "safe",
        minimum_generated_free_gib=1.25,
        max_artifact_bytes=4096,
    )
    required = int(1.25 * 1024**3) + 4096
    free = required
    original_disk_usage = probe.shutil.disk_usage

    def disk_usage(_path):
        return type("DiskUsage", (), {"free": free})()

    try:
        probe.shutil.disk_usage = disk_usage
        prepared = prepare_generated_directory(options)
        require(prepared == safe_parent / "safe", "safe generated target differs")
        prepared.rmdir()

        options.generated_directory = safe_parent / "low-space"
        free = required - 1
        expect_rejected(
            lambda: prepare_generated_directory(options),
            "generated directory accepted one byte below reserve/headroom",
        )
        require(
            not options.generated_directory.exists(),
            "low-space rejection mutated the generated target",
        )

        free = required
        for protected in (
            architecture / "generated",
            capture / "generated",
            closure / "generated",
            input_root / "generated",
            tools / "generated",
        ):
            options.generated_directory = protected
            expect_rejected(
                lambda: prepare_generated_directory(options),
                "generated directory was accepted in a source/input/capture tree",
            )
            require(
                not protected.exists(),
                "unsafe generated-directory rejection mutated a source tree",
            )

        missing_parent = root / "missing-parent" / "generated"
        options.generated_directory = missing_parent
        expect_rejected(
            lambda: prepare_generated_directory(options),
            "generated directory recursively created an unchecked parent",
        )
        require(
            not missing_parent.parent.exists(),
            "generated-directory preflight created an unchecked parent",
        )
    finally:
        probe.shutil.disk_usage = original_disk_usage


def main() -> int:
    options = arguments()
    repository = Path(__file__).resolve().parents[3]
    tooling_cmake = (
        repository / "tests/tooling/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    require(
        "probe_a64_aslref_materialization_batch.py" not in tooling_cmake,
        "slow materialization execution probe entered ordinary CTest",
    )
    require(
        FULL_EXTENT_MIN_TIMEOUT_SECONDS >= 300
        and FULL_EXTENT_MIN_RSS_KIB >= 9 * 1024 * 1024
        and FULL_EXTENT_RESOURCE_LOCK == "a64-aslref-full-extent",
        "full-extent standalone resource policy drifted",
    )
    with tempfile.TemporaryDirectory(
        prefix="a64-aslref-log-cap-fixture-"
    ) as temporary:
        limit_root = Path(temporary).resolve(strict=True)
        exact_root = limit_root / "exact"
        exact_root.mkdir()
        with BoundedWorkspace.adopt(
            exact_root,
            source_root=repository,
            publication_targets=(),
            limits=GenerationLimits(1, 4096, 2048, 1024, 16),
        ) as workspace:
            measured = execute_measured(
                [
                    sys.executable,
                    "-B",
                    "-c",
                    "import sys; sys.stdout.write('x' * 1024)",
                ],
                10,
                16 * 1024 * 1024,
                1024,
                exact_root,
                workspace,
            )
        require(
            measured[0] == 0
            and (exact_root / "aslref.stdout").stat().st_size == 1024
            and (exact_root / "aslref.stderr").stat().st_size == 0,
            "ASLRef subprocess rejected the exact combined log boundary",
        )

        overflow_root = limit_root / "overflow"
        overflow_root.mkdir()
        with BoundedWorkspace.adopt(
            overflow_root,
            source_root=repository,
            publication_targets=(),
            limits=GenerationLimits(1, 4096, 2048, 1024, 16),
        ) as workspace:
            expect_rejected(
                lambda: execute_measured(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        "import sys; sys.stdout.write('x' * 1025)",
                    ],
                    10,
                    16 * 1024 * 1024,
                    1024,
                    overflow_root,
                    workspace,
                ),
                "ASLRef subprocess log hard cap was not enforced",
            )
        require_generated_artifact_limit(exact_root, 2048)
        expect_rejected(
            lambda: require_generated_artifact_limit(exact_root, 1023),
            "generated artifact hard cap accepted excess bytes",
        )

        timeout_root = limit_root / "timeout"
        timeout_root.mkdir()
        timeout_escape = limit_root / "timeout-descendant-escaped"
        with BoundedWorkspace.adopt(
            timeout_root,
            source_root=repository,
            publication_targets=(),
            limits=GenerationLimits(1, 4096, 2048, 1024, 16),
        ) as workspace:
            expect_rejected(
                lambda: execute_measured(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        (
                            "import subprocess,sys,time;"
                            "subprocess.Popen([sys.executable,'-B','-c',"
                            "sys.argv[1]]);time.sleep(60)"
                        ),
                        (
                            "import time;from pathlib import Path;time.sleep(2);"
                            f"Path({str(timeout_escape)!r}).write_text('escaped')"
                        ),
                    ],
                    1,
                    16 * 1024 * 1024,
                    1024,
                    timeout_root,
                    workspace,
                ),
                "ASLRef subprocess timeout was not enforced",
            )
        time.sleep(1.25)
        require(
            not timeout_escape.exists(),
            "ASLRef timeout left a descendant able to mutate later",
        )

        early_root = limit_root / "early-leader"
        early_root.mkdir()
        early_escape = limit_root / "early-descendant-escaped"
        with BoundedWorkspace.adopt(
            early_root,
            source_root=repository,
            publication_targets=(),
            limits=GenerationLimits(1, 4096, 2048, 1024, 16),
        ) as workspace:
            expect_rejected(
                lambda: execute_measured(
                    [
                        sys.executable,
                        "-B",
                        "-c",
                        (
                            "import subprocess,sys;"
                            "subprocess.Popen([sys.executable,'-B','-c',"
                            "sys.argv[1]])"
                        ),
                        (
                            "import time;from pathlib import Path;time.sleep(0.5);"
                            f"Path({str(early_escape)!r}).write_text('escaped')"
                        ),
                    ],
                    10,
                    16 * 1024 * 1024,
                    1024,
                    early_root,
                    workspace,
                ),
                "ASLRef accepted an early leader with live descendants",
            )
        time.sleep(0.7)
        require(
            not early_escape.exists(),
            "ASLRef early-leader rejection left a live descendant",
        )
        generated_preflight_root = limit_root / "generated-preflight"
        generated_preflight_root.mkdir()
        generated_directory_preflight_fixture(generated_preflight_root.resolve())
    require(
        len(IMPDEF_PROJECTIONS) == len(IMPDEF_PROJECTION_BY_KEY) == 65,
        "reviewed implementation-defined projection inventory is not unique/complete",
    )
    environment = Path(__file__).with_name("a64_aslref_state_environment.asl")
    require(
        implementation_defined_adapter_digest(
            environment.read_text(encoding="utf-8")
        )
        == IMPLEMENTATION_DEFINED_ADAPTER_SHA256,
        "reviewed implementation-defined ASL adapter drifted",
    )
    for projection in IMPDEF_PROJECTIONS:
        validate_impdef_projection(projection)
    require(
        impdef_policy_digest(IMPDEF_PROJECTIONS) == IMPDEF_POLICY_SHA256,
        "implementation-defined value-domain policy inventory drifted",
    )
    value_policy_report = verify_impdef_value_policies(
        environment.read_text(encoding="utf-8"), IMPDEF_PROJECTION_BY_KEY
    )
    require(
        value_policy_report["classified_returns"] == 65
        and value_policy_report["adapter_values_verified"] == 65
        and value_policy_report["policy_class_counts"]
        == {"fixed": 4, "bounded": 8, "enumerated": 53, "state-derived": 0},
        "implementation-defined value-domain coverage is incomplete",
    )

    pamax = IMPDEF_PROJECTION_BY_KEY[
        ("Int", "Maximum Physical Address Size")
    ]
    validate_impdef_projection(pamax)
    expect_rejected(
        lambda: validate_impdef_projection(replace(pamax, value=0)),
        "zero physical-address size passed its AddressSize range preflight",
    )
    expect_rejected(
        lambda: validate_impdef_projection(replace(pamax, value=65)),
        "out-of-range physical-address size passed its AddressSize preflight",
    )
    brbe = IMPDEF_PROJECTION_BY_KEY[("Int", "Number of BRBE Branch records")]
    expect_rejected(
        lambda: validate_impdef_projection(replace(brbe, value=12)),
        "non-enumerated BRBE record count passed its official domain",
    )
    breakpoints = IMPDEF_PROJECTION_BY_KEY[("Int", "Number of breakpoints")]
    expect_rejected(
        lambda: validate_impdef_projection(
            replace(breakpoints, minimum=0, maximum=16)
        ),
        "fixed debug-count policy passed with an invented bounded domain",
    )
    boolean_projection = IMPDEF_PROJECTION_BY_KEY[
        ("Bool", "CPY* instructions use Option A")
    ]
    expect_rejected(
        lambda: validate_impdef_projection(
            replace(boolean_projection, allowed_values=(0,))
        ),
        "incomplete boolean enumeration passed value-domain validation",
    )
    expect_rejected(
        lambda: validate_impdef_projection(
            replace(boolean_projection, policy_class="unreviewed")
        ),
        "unknown implementation-defined policy class passed validation",
    )
    mutated_adapter = environment.read_text(encoding="utf-8").replace(
        'if description == "Maximum Physical Address Size" then return 48; end;',
        'if description == "Maximum Physical Address Size" then return 52; end;',
    )
    expect_rejected(
        lambda: verify_impdef_value_policies(
            mutated_adapter, IMPDEF_PROJECTION_BY_KEY
        ),
        "adapter literal drift passed the complete value-policy comparison",
    )

    validate_cpy_post_size_policy(cpy_post_size_policy)
    require(
        [
            cpy_post_size_policy(value)
            for value in (
                0, 1, CPY_POST_RESERVE_BYTES - 1,
                CPY_POST_RESERVE_BYTES, CPY_POST_RESERVE_BYTES + 1,
                ARCH_MAX_MOPS_CPY_SIZE,
            )
        ]
        == [0, 1, 63, 64, 64, 64],
        "CPY post-size boundary policy differs",
    )
    expect_rejected(
        lambda: cpy_post_size_policy(-1),
        "negative Option-B CPY remaining size passed the policy",
    )
    expect_rejected(
        lambda: cpy_post_size_policy(ARCH_MAX_MOPS_CPY_SIZE + 1),
        "oversized Option-B CPY remaining size passed the policy",
    )
    expect_rejected(
        lambda: validate_cpy_post_size_policy(lambda _size: 0),
        "non-progressing CPY epilogue policy passed validation",
    )
    expect_rejected(
        lambda: validate_cpy_post_size_policy(
            lambda size: min(size, CPY_POST_RESERVE_BYTES + 1)
        ),
        "unpinned CPY reserve cap passed validation",
    )

    compact_initial, compact_expected = generated_compact_max_memory()
    compact_tags = bytes(COMPACT_MAX_MEMORY_TAG_LENGTH)
    compact_region = {
        "address": f"{COMPACT_MAX_MEMORY_ADDRESS:016x}",
        "size": f"{COMPACT_MAX_MEMORY_LENGTH:016x}",
        "initial_bytes": compact_initial.hex(),
        "expected_bytes": compact_expected.hex(),
        "initial_tags": compact_tags.hex(),
        "expected_tags": compact_tags.hex(),
    }
    require(
        compact_max_memory_descriptor_digest()
        == COMPACT_MAX_MEMORY_DESCRIPTOR_SHA256,
        "compact maximum-memory descriptor digest differs",
    )
    compact_report = verify_compact_max_memory([compact_region])
    require(
        compact_report["initialized_bytes"]
        == compact_report["compared_bytes"]
        == COMPACT_MAX_MEMORY_LENGTH,
        "compact maximum-memory byte coverage differs",
    )
    expect_rejected(
        lambda: verify_compact_max_memory(
            [compact_region], formula_seed=COMPACT_MAX_MEMORY_FORMULA_SEED ^ 1
        ),
        "one-bit compact-memory formula mutation passed validation",
    )
    mutated_initial = bytearray(compact_initial)
    mutated_initial[COMPACT_MAX_MEMORY_LENGTH // 2] ^= 1
    mutated_region = dict(compact_region)
    mutated_region["initial_bytes"] = bytes(mutated_initial).hex()
    expect_rejected(
        lambda: verify_compact_max_memory([mutated_region]),
        "one-byte compact-memory payload mutation passed validation",
    )
    for guest_page_size, profile in FULL_EXTENT_CAPTURE_PROFILES.items():
        full_initial, full_expected = generated_full_extent_transfer_memory(
            guest_page_size
        )
        backing_length = int(profile["backing_length"])
        full_tags = bytes(backing_length // 16)
        full_region = {
            "address": f"{COMPACT_MAX_MEMORY_ADDRESS:016x}",
            "size": f"{backing_length:016x}",
            "initial_bytes": full_initial.hex(),
            "expected_bytes": full_expected.hex(),
            "initial_tags": full_tags.hex(),
            "expected_tags": full_tags.hex(),
        }
        full_report = verify_full_extent_transfer_memory(
            [full_region], guest_page_size
        )
        require(
            full_report["initialized_bytes"]
            == full_report["compared_bytes"]
            == backing_length,
            "full-extent compact backing coverage differs",
        )
        mutated_full_expected = bytearray(full_expected)
        mutated_full_expected[backing_length // 2] ^= 1
        mutated_full_region = dict(full_region)
        mutated_full_region["expected_bytes"] = bytes(
            mutated_full_expected
        ).hex()
        expect_rejected(
            lambda mutated_full_region=mutated_full_region,
            guest_page_size=guest_page_size: verify_full_extent_transfer_memory(
                [mutated_full_region], guest_page_size
            ),
            "one-byte full-extent expected-memory mutation passed validation",
        )

    original_context = """\
func AArch64_PAMax() => AddressSize
begin
    return ImpDefInt(\"Maximum Physical Address Size\") as AddressSize;
end;
"""
    changed_type_context = original_context.replace(
        "as AddressSize", "as integer{0..64}"
    )
    original_digest = impdef_context_digest(original_context)
    require_impdef_context_digest(original_context, original_digest)
    expect_rejected(
        lambda: require_impdef_context_digest(
            changed_type_context, original_digest
        ),
        "same-description ImpDefInt target-type drift passed preflight",
    )

    unclassified_source = """\
func A64Batch_NewHook() => boolean
begin
    return ImpDefBool(\"new reachable implementation choice\");
end;

func main() => integer
begin
    let choice : boolean = A64Batch_NewHook();
    return if choice then 1 else 0;
end;
"""
    expect_rejected(
        lambda: require_reachable_impdef_projections(
            (unclassified_source,), IMPDEF_PROJECTION_BY_KEY
        ),
        "new reachable/unclassified implementation-defined hook passed preflight",
    )

    tuple_interface = """\
func SuppliedPair(value : bits(64)) => (bits(64), boolean)
begin
    return (value, TRUE);
end;
"""
    arity_mutation = tuple_interface.replace(
        "=> (bits(64), boolean)", "=> bits(64)"
    ).replace("return (value, TRUE);", "return value;")
    tuple_signatures = asl_definition_signatures((tuple_interface,))
    signature_digest, _count = supplied_interface_signature_digest(
        tuple_signatures, ("SuppliedPair",)
    )
    require_supplied_interface_signature_digest(
        tuple_signatures, ("SuppliedPair",), signature_digest
    )
    expect_rejected(
        lambda: require_supplied_interface_signature_digest(
            asl_definition_signatures((arity_mutation,)),
            ("SuppliedPair",),
            signature_digest,
        ),
        "supplied tuple-return arity mutation passed signature preflight",
    )

    tuple_forward_source = """\
func Pair(value : bits(64)) => (bits(64), boolean)
begin
    return (value, TRUE);
end;

func Forward(value : bits(64)) => (bits(64), boolean)
begin
    return Pair(value);
end;

func main() => integer
begin
    var value : bits(64);
    var valid : boolean;
    (value, valid) = Forward(Zeros{64});
    return if valid then UInt(value) else 1;
end;
"""
    tuple_sites = tuple_forward_inventory(
        (tuple_forward_source,), (), ""
    )
    tuple_digest = tuple_forward_inventory_digest(tuple_sites)
    require_tuple_forward_inventory(tuple_sites, 1, tuple_digest)
    normalized, normalization = normalize_reachable_tuple_forwards(
        (tuple_forward_source,), (), "", 1, tuple_digest,
        frozenset({("Forward", "Pair")}), {}
    )
    require(
        "var A64TupleForward0First : bits(64);" in normalized[0]
        and "var A64TupleForward0Second : boolean;" in normalized[0]
        and "(A64TupleForward0First, A64TupleForward0Second) = Pair(value);"
        in normalized[0]
        and "return (A64TupleForward0First, A64TupleForward0Second);"
        in normalized[0],
        "tuple forwarding was not typed-destructured and reconstructed",
    )
    require(
        normalization["rewritten_tuple_sites"] == 1
        and normalization["preserved_tuple_sites"] == 0
        and normalization["rewritten_non_tuple_sites"] == 0,
        "tuple normalization rewrote the wrong source class",
    )
    expect_rejected(
        lambda: normalize_reachable_tuple_forwards(
            (tuple_forward_source,), (), "", 1, tuple_digest,
            frozenset(), {},
        ),
        "newly exercised tuple-forward site escaped the rewrite/proof partition",
    )
    expect_rejected(
        lambda: normalize_reachable_tuple_forwards(
            (tuple_forward_source,), (), "", 1, tuple_digest,
            frozenset(), {("Forward", "Pair"): ""},
        ),
        "unnormalized tuple-forward site passed without a dormant-path proof",
    )
    missing_component = tuple_forward_source.replace(
        "=> (bits(64), boolean)", "=> bits(64)"
    ).replace("return (value, TRUE);", "return value;")
    expect_rejected(
        lambda: require_tuple_forward_inventory(
            tuple_forward_inventory((missing_component,), (), ""),
            1,
            tuple_digest,
        ),
        "missing tuple return component passed normalization preflight",
    )
    extra_component = tuple_forward_source.replace(
        "=> (bits(64), boolean)", "=> (bits(64), boolean, bits(1))"
    ).replace("return (value, TRUE);", "return (value, TRUE, '0');")
    extra_sites = tuple_forward_inventory((extra_component,), (), "")
    expect_rejected(
        lambda: normalize_reachable_tuple_forwards(
            (extra_component,), (), "", 1,
            tuple_forward_inventory_digest(extra_sites),
            frozenset({("Forward", "Pair")}), {},
        ),
        "extra tuple return component passed two-component normalization",
    )
    retyped_component = tuple_forward_source.replace(
        "func Forward(value : bits(64)) => (bits(64), boolean)",
        "func Forward(value : bits(64)) => (bits(64), bits(1))",
    )
    expect_rejected(
        lambda: tuple_forward_inventory((retyped_component,), (), ""),
        "retyped tuple return component passed owner/callee equivalence",
    )
    context_drift = tuple_forward_source.replace(
        "begin\n    return Pair(value);",
        "begin\n    // context mutation\n    return Pair(value);",
    )
    expect_rejected(
        lambda: require_tuple_forward_inventory(
            tuple_forward_inventory((context_drift,), (), ""), 1, tuple_digest
        ),
        "tuple-forward owner context drift passed pinned inventory",
    )

    translation_source = """\
func AArch64_TranslateAddress(va : bits(64), accdesc : AccessDescriptor,
                              aligned : boolean, size : integer) => AddressDescriptor
begin
    var result : AddressDescriptor = AArch64_FullTranslate(va, size, accdesc, aligned);
    result.vaddress = ZeroExtend{64}(va);

    return result;
end;
"""
    translation_entry = [
        entry
        for entry in asl_definition_entries(translation_source)
        if entry.name == "AArch64_TranslateAddress"
    ][0]
    translation_digest = hashlib.sha256(
        translation_entry.body.encode("utf-8")
    ).hexdigest()
    projected, projection_report = normalize_virtual_fault_projection(
        (translation_source,), translation_digest
    )
    require(
        "A64Oracle_ProjectVirtualTranslationFault(" in projected[0]
        and "A64Oracle_RecordProjectedVirtualFault(" in projected[0]
        and projection_report["rewritten_functions"] == 1,
        "virtual fault projection was not inserted at translation return",
    )
    expect_rejected(
        lambda: normalize_virtual_fault_projection(
            (translation_source.replace(
                "var result", "// context mutation\n    var result"
            ),),
            translation_digest,
        ),
        "translation-boundary context drift passed projection preflight",
    )
    expected_fault = expected_virtual_fault_observation(
        status="Fault_Permission", address=0x5100_3FFF, write=True
    )
    require_virtual_fault_observation(expected_fault, expected_fault)
    for field_name, changed in (
        ("domain", "physical"),
        ("status", "Fault_SyncExternal"),
        ("write", False),
        ("address", 0x5100_4000),
    ):
        mutated_fault = dict(expected_fault)
        mutated_fault[field_name] = changed
        expect_rejected(
            lambda mutated_fault=mutated_fault: require_virtual_fault_observation(
                mutated_fault, expected_fault
            ),
            f"virtual fault {field_name} swap passed exact observation policy",
        )
    expected_physical = expected_physical_fault_observation(
        address=0x5100_3FFF, write=True
    )
    require_virtual_fault_observation(expected_physical, expected_physical)
    for field_name, changed in (
        ("domain", "virtual"),
        ("status", "Fault_Permission"),
        ("write", False),
        ("address", 0x5100_4000),
    ):
        mutated_physical = dict(expected_physical)
        mutated_physical[field_name] = changed
        expect_rejected(
            lambda mutated_physical=mutated_physical: require_virtual_fault_observation(
                mutated_physical, expected_physical
            ),
            f"physical fault {field_name} swap passed exact observation policy",
        )

    require_mops_whole_sequence((0x1D0404A3, 0x1D4404A3, 0x1D8404A3))
    expect_rejected(
        lambda: require_mops_whole_sequence((0x1D4404A3,)),
        "standalone MOPS main stage was credited as a whole variant",
    )

    # Primary PAuth alone is not executable: pinned ComputePAC reaches an
    # explicit unreachable unless exactly one concrete algorithm is present.
    expect_rejected(
        lambda: resolve_pac_algorithm({"pauth"}, PAC_RELATION, None),
        "primary PAuth without a concrete algorithm was accepted",
    )
    expect_rejected(
        lambda: resolve_pac_algorithm({"pauth"}, None, PAC_ALGORITHM),
        "PAC normalization without its typed callback relation was accepted",
    )
    expect_rejected(
        lambda: resolve_pac_algorithm(
            {"pauth"}, PAC_RELATION, "FEAT_PACQARMA5"
        ),
        "an unreviewed PAC algorithm was accepted",
    )
    require(
        resolve_pac_algorithm({"pauth"}, PAC_RELATION, PAC_ALGORITHM)
        == PAC_ALGORITHM,
        "reviewed permitted-transformation PAC policy was rejected",
    )
    require(
        resolve_pac_algorithm(set(), None, None) is None,
        "non-PAuth input spuriously selected a PAC algorithm",
    )
    if (options.architecture_root is None) != (options.features_json is None):
        raise ValueError("official dependency inputs must be supplied together")
    if options.architecture_root is not None:
        dependency = verify_pac_dependency(
            options.architecture_root, options.features_json
        )
        require(
            dependency["normalization_obligation"]
            == "bit3-engine-private-state-and-scratch",
            "PAC policy escaped the four reviewed normalization obligations",
        )
    print("A64 ASLRef materialization-batch preflight fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
