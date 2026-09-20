#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mutation-pin Darwin's serialized host-TLB and fault-routing policy."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable


class PolicyError(AssertionError):
    """A prepared kernel source tree no longer satisfies the policy."""


@dataclass(frozen=True)
class FunctionRegion:
    text: str
    start: int
    end: int
    header: str


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PolicyError(message)


def mask_c_comments_and_literals(source: str) -> str:
    """Replace comments and literals with spaces while preserving offsets."""
    result = list(source)
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            if end < 0:
                end = len(source)
            for cursor in range(index, end):
                result[cursor] = " "
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            require(end >= 0, "unterminated C block comment")
            end += 2
            for cursor in range(index, end):
                if result[cursor] != "\n":
                    result[cursor] = " "
            index = end
            continue
        if source[index] in ('"', "'"):
            quote = source[index]
            result[index] = " "
            index += 1
            while index < len(source):
                character = source[index]
                if character != "\n":
                    result[index] = " "
                index += 1
                if character == "\\" and index < len(source):
                    if source[index] != "\n":
                        result[index] = " "
                    index += 1
                    continue
                if character == quote:
                    break
            continue
        index += 1
    return "".join(result)


def function_region(source: str, name: str) -> FunctionRegion:
    masked = mask_c_comments_and_literals(source)
    pattern = re.compile(
        rf"(?m)^[^\n;{{}}]*\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{"
    )
    matches = list(pattern.finditer(masked))
    require(len(matches) == 1, f"expected one definition of {name}, found {len(matches)}")
    match = matches[0]
    opening = masked.find("{", match.start(), match.end())
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return FunctionRegion(
                    text=source[match.start() : index + 1],
                    start=match.start(),
                    end=index + 1,
                    header=masked[match.start() : opening],
                )
    raise PolicyError(f"unterminated function definition: {name}")


def find(source: str, pattern: str, message: str, *, flags: int = 0) -> re.Match[str]:
    match = re.search(pattern, mask_c_comments_and_literals(source), flags)
    require(match is not None, message)
    return match


def require_order(
    source: str,
    entries: tuple[tuple[str, str], ...],
) -> dict[str, re.Match[str]]:
    masked = mask_c_comments_and_literals(source)
    matches: dict[str, re.Match[str]] = {}
    previous = -1
    for label, pattern in entries:
        match = re.compile(pattern, re.DOTALL).search(masked, previous + 1)
        require(match is not None, f"missing {label}")
        matches[label] = match
        previous = match.start()
    return matches


def preprocessor_context(source: str, offset: int) -> tuple[str, ...]:
    stack: list[str] = []
    for line in source[:offset].splitlines():
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive is None:
            continue
        kind = directive.group(1)
        expression = directive.group(2).strip()
        if kind == "if":
            stack.append(expression)
        elif kind == "ifdef":
            stack.append(f"defined({expression})")
        elif kind == "ifndef":
            stack.append(f"!defined({expression})")
        elif kind == "elif":
            require(bool(stack), "orphan #elif in source")
            stack[-1] = expression
        elif kind == "else":
            require(bool(stack), "orphan #else in source")
            stack[-1] = f"!({stack[-1]})"
        else:
            require(bool(stack), "orphan #endif in source")
            stack.pop()
    return tuple(stack)


def require_darwin_context(
    source: str,
    offset: int,
    *,
    four_k_only: bool,
    subject: str,
) -> None:
    context = tuple(
        re.sub(r"\s+", "", expression)
        for expression in preprocessor_context(source, offset)
    )
    if four_k_only:
        require(
            context
            == (
                "defined(UML_HOST_DARWIN)&&"
                "!defined(CONFIG_PAGE_SIZE_16KB)",
            ),
            f"{subject} is not confined to the 4 KiB guest profile",
        )
    else:
        require(
            context == ("defined(UML_HOST_DARWIN)",),
            f"{subject} is not enabled for every Darwin page profile",
        )


def validate_init_mm_wrapper(tlb: str) -> None:
    wrapper = function_region(tlb, "um_tlb_sync_init_mm")
    require(
        re.search(r"\bstatic\s+noinline\s+int\b", wrapper.header) is not None,
        "init_mm synchronization wrapper must remain noinline",
    )
    require_darwin_context(
        tlb,
        wrapper.start,
        four_k_only=False,
        subject="init_mm synchronization wrapper",
    )
    find(
        wrapper.text,
        r"if\s*\(\s*READ_ONCE\s*\(\s*init_mm_tlb_owner\s*\)\s*"
        r"==\s*current\s*\)\s*return\s+-EDEADLK\s*;",
        "same-task init_mm synchronization reentry must return -EDEADLK",
        flags=re.DOTALL,
    )
    require_order(
        wrapper.text,
        (
            (
                "init_mm turnstile acquisition",
                r"mutex_lock\s*\(\s*&init_mm\.context\.turnstile\s*\)\s*;",
            ),
            (
                "post-lock owner-null invariant",
                r"WARN_ON_ONCE\s*\(\s*READ_ONCE\s*\(\s*init_mm_tlb_owner\s*\)\s*"
                r"!=\s*NULL\s*\)",
            ),
            (
                "init_mm owner publication",
                r"WRITE_ONCE\s*\(\s*init_mm_tlb_owner\s*,\s*current\s*\)\s*;",
            ),
            (
                "serialized init_mm walker",
                r"__um_tlb_sync\s*\(\s*&init_mm\s*,\s*false\s*,\s*0\s*,\s*0\s*\)\s*;",
            ),
            (
                "init_mm owner clearing",
                r"WRITE_ONCE\s*\(\s*init_mm_tlb_owner\s*,\s*NULL\s*\)\s*;",
            ),
            (
                "init_mm turnstile release",
                r"mutex_unlock\s*\(\s*&init_mm\.context\.turnstile\s*\)\s*;",
            ),
        ),
    )

    masked_tlb = mask_c_comments_and_literals(tlb)
    require(
        len(re.findall(r"__um_tlb_sync\s*\(\s*&init_mm\s*,", masked_tlb)) == 1,
        "init_mm must enter the core walker only through its serialized wrapper",
    )

    public_sync = function_region(tlb, "um_tlb_sync")
    require_order(
        public_sync.text,
        (
            (
                "init_mm wrapper dispatch",
                r"if\s*\(\s*unlikely\s*\(\s*mm\s*==\s*&init_mm\s*\)\s*\)\s*"
                r"return\s+um_tlb_sync_init_mm\s*\(\s*\)\s*;",
            ),
            ("ordinary-mm walker dispatch", r"return\s+__um_tlb_sync\s*\("),
        ),
    )

    locked = function_region(tlb, "um_tlb_sync_locked")
    find(
        locked.text,
        r"if\s*\(\s*WARN_ON_ONCE\s*\(\s*mm\s*==\s*&init_mm\s*\)\s*\)\s*"
        r"return\s+-EINVAL\s*;\s*return\s+__um_tlb_sync\s*\(\s*mm\s*,\s*true\s*,"
        r"\s*start\s*,\s*end\s*\)",
        "um_tlb_sync_locked must reject init_mm before entering the core walker",
        flags=re.DOTALL,
    )


def validate_pending_range_transaction(tlb: str) -> None:
    core = function_region(tlb, "__um_tlb_sync")
    require_order(
        core.text,
        (
            ("retry target", r"retry\s*:"),
            (
                "pending-range lock",
                r"spin_lock_irqsave\s*\(\s*&mm->context\.sync_tlb_lock\s*,\s*flags\s*\)",
            ),
            (
                "caller range joins the snapshot under the pending-range lock",
                r"if\s*\(\s*force_end\s*>\s*force_start\s*\)\s*\{\s*"
                r"um_tlb_mark_sync_locked\s*\(\s*mm\s*,\s*force_start\s*,\s*"
                r"force_end\s*\)\s*;",
            ),
            (
                "pending-range count snapshot",
                r"sync_count\s*=\s*mm->context\.sync_tlb_range_count\s*;",
            ),
            (
                "full pending-range snapshot",
                r"memcpy\s*\(\s*sync_ranges\s*,\s*mm->context\.sync_tlb_ranges\s*,\s*"
                r"sync_count\s*\*\s*sizeof\s*\(\s*sync_ranges\s*\[\s*0\s*\]\s*\)\s*\)\s*;",
            ),
            (
                "pending-range clear",
                r"mm->context\.sync_tlb_range_count\s*=\s*0\s*;",
            ),
            (
                "pending-range unlock",
                r"spin_unlock_irqrestore\s*\(\s*&mm->context\.sync_tlb_lock\s*,\s*flags\s*\)",
            ),
            (
                "complete user snapshot application",
                r"if\s*\(\s*mm\s*!=\s*&init_mm\s*\)\s*"
                r"ret\s*=\s*darwin_replay_user_ranges\s*\(\s*mm\s*,\s*&ops\s*,\s*"
                r"sync_ranges\s*,\s*sync_count\s*\)\s*;",
            ),
            (
                "complete init_mm snapshot application",
                r"for\s*\(\s*sync_index\s*=\s*0\s*;\s*mm\s*==\s*&init_mm\s*&&\s*"
                r"sync_index\s*<\s*sync_count\s*&&\s*!ret\s*;\s*\+\+sync_index\s*\)\s*\{\s*"
                r"addr\s*=\s*sync_ranges\s*\[\s*sync_index\s*\]\.from\s*;\s*"
                r"sync_end\s*=\s*sync_ranges\s*\[\s*sync_index\s*\]\.to\s*;\s*"
                r"ret\s*=\s*update_mm_range\s*\(\s*mm\s*,\s*&ops\s*,\s*addr\s*,\s*sync_end\s*\)\s*;",
            ),
            (
                "failed-snapshot requeue",
                r"if\s*\(\s*ret\s*\)\s*\{\s*"
                r"for\s*\(\s*sync_index\s*=\s*0\s*;\s*"
                r"sync_index\s*<\s*sync_count\s*;\s*\+\+sync_index\s*\)\s*"
                r"um_tlb_mark_sync\s*\(\s*mm\s*,\s*"
                r"sync_ranges\s*\[\s*sync_index\s*\]\.from\s*,\s*"
                r"sync_ranges\s*\[\s*sync_index\s*\]\.to\s*\)\s*;\s*\}",
            ),
            (
                "-EAGAIN retry",
                r"if\s*\(\s*ret\s*==\s*-EAGAIN\s*\)\s*\{\s*"
                r"ret\s*=\s*0\s*;\s*goto\s+retry\s*;\s*\}",
            ),
        ),
    )


def validate_bounded_user_replay(tlb: str) -> None:
    """A user replay publishes bounded batches and repeats only a rejected one."""
    find(
        tlb,
        r"#\s*define\s+UML_TLB_SYNC_BATCH_PAGES\s+64U\b",
        "a user replay batch must stay bounded at 64 pages",
    )
    pte_range = function_region(tlb, "update_pte_range")
    find(
        pte_range.text,
        r"if\s*\(\s*!ret\s*&&\s*ops->budget\s*!=\s*0\s*&&\s*--ops->budget\s*==\s*0\s*\)\s*\{\s*"
        r"ops->resume\s*=\s*addr\s*\+\s*PAGE_SIZE\s*;\s*"
        r"ret\s*=\s*UML_TLB_SYNC_BATCH_FULL\s*;\s*\}",
        "the PTE walk must stop a full batch and record where the next one resumes",
    )
    checkpoint = function_region(tlb, "darwin_tlb_sync_checkpoint")
    require_order(
        checkpoint.text,
        (
            ("logical-IRQ gate", r"if\s*\(\s*!um_get_signals\s*\(\s*\)\s*\)\s*return\s*;"),
            ("cooperative timer checkpoint", r"darwin_timer_kernel_checkpoint\s*\(\s*\)\s*;"),
            ("scheduling point", r"cond_resched\s*\(\s*\)\s*;"),
        ),
    )
    find(
        tlb,
        r"#\s*define\s+UML_TLB_SYNC_REJECTS_PER_CHECKPOINT\s+16U\b",
        "a losing walker must reach a checkpoint within sixteen rejections",
    )
    replay = function_region(tlb, "darwin_replay_user_ranges")
    require_darwin_context(
        tlb, replay.start, four_k_only=False, subject="bounded user replay"
    )
    require_order(
        replay.text,
        (
            ("walker binding", r"darwin_uml_vm_walker_lock\s*\(\s*generation\s*\)"),
            ("batch begin", r"ret\s*=\s*darwin_uml_vm_batch_begin\s*\(\s*generation\s*\)\s*;"),
            (
                "per-batch budget",
                r"ops->budget\s*=\s*UML_TLB_SYNC_BATCH_PAGES\s*;",
            ),
            (
                "bounded walk of the current range",
                r"ret\s*=\s*update_mm_range\s*\(\s*mm\s*,\s*ops\s*,\s*batch_addr\s*,\s*"
                r"ranges\s*\[\s*batch_index\s*\]\.to\s*\)\s*;",
            ),
            (
                "one batch spans the snapshot's ranges",
                r"if\s*\(\s*ret\s*\|\|\s*\+\+batch_index\s*==\s*count\s*\)\s*break\s*;\s*"
                r"batch_addr\s*=\s*ranges\s*\[\s*batch_index\s*\]\.from\s*;",
            ),
            ("batch end", r"batch_ret\s*=\s*darwin_uml_vm_batch_end\s*\(\s*\)\s*;"),
            ("walker release", r"darwin_uml_vm_walker_unlock\s*\(\s*generation\s*\)"),
            (
                "rejected batch repeats from its own start with a bounded checkpoint",
                r"if\s*\(\s*ret\s*==\s*-EAGAIN\s*\)\s*\{\s*ret\s*=\s*0\s*;\s*"
                r"if\s*\(\s*\+\+rejected\s*%\s*UML_TLB_SYNC_REJECTS_PER_CHECKPOINT\s*==\s*0\s*\)\s*"
                r"darwin_tlb_sync_checkpoint\s*\(\s*\)\s*;\s*continue\s*;\s*\}",
            ),
            (
                "published batch advances to its resume address",
                r"index\s*=\s*batch_index\s*;\s*addr\s*=\s*ops->resume\s*;",
            ),
            (
                "a range the batch finished hands over to the next",
                r"if\s*\(\s*addr\s*>=\s*ranges\s*\[\s*index\s*\]\.to\s*&&\s*\+\+index\s*<\s*count\s*\)\s*"
                r"addr\s*=\s*ranges\s*\[\s*index\s*\]\.from\s*;\s*"
                r"darwin_tlb_sync_checkpoint\s*\(\s*\)\s*;",
            ),
        ),
    )
    core = function_region(tlb, "__um_tlb_sync")
    find(
        core.text,
        r"ops\.budget\s*=\s*0\s*;",
        "the init_mm walk must run unbounded inside its serialized wrapper",
    )


def validate_four_k_slot_policy(tlb: str) -> None:
    slot = function_region(tlb, "update_init_mm_pte_slot")
    slot_range = function_region(tlb, "update_init_mm_pte_range")
    mapper = function_region(tlb, "darwin_map_init_mm_pte")
    for subject, region in (
        ("init_mm quarter mapper", mapper),
        ("init_mm host-slot updater", slot),
        ("init_mm host-slot range updater", slot_range),
    ):
        require_darwin_context(
            tlb, region.start, four_k_only=True, subject=subject
        )

    masked = mask_c_comments_and_literals(tlb)
    find(
        tlb,
        r"#\s*define\s+DARWIN_HOST_PAGE_SIZE\s+\(\s*16UL\s*\*\s*1024\s*\)",
        "4 KiB init_mm policy must define the 16 KiB Darwin host-slot size",
    )
    for pattern, message in (
        (r"static_assert\s*\(\s*PAGE_SIZE\s*==\s*4096\s*\)\s*;", "missing 4 KiB page-size assertion"),
        (r"static_assert\s*\(\s*DARWIN_HOST_PAGE_SIZE\s*==\s*4\s*\*\s*PAGE_SIZE\s*\)\s*;", "missing four-quarter geometry assertion"),
        (r"static_assert\s*\(\s*PTRS_PER_PTE\s*%\s*4\s*==\s*0\s*\)\s*;", "missing PTE-page alignment assertion"),
        (r"static_assert\s*\(\s*PMD_SIZE\s*%\s*DARWIN_HOST_PAGE_SIZE\s*==\s*0\s*\)\s*;", "missing PMD alignment assertion"),
    ):
        require(re.search(pattern, masked) is not None, message)

    find(
        slot.text,
        r"WARN_ON_ONCE\s*\(\s*ops->mm\s*!=\s*&init_mm\s*\|\|\s*"
        r"\(\s*addr\s*&\s*\(\s*DARWIN_HOST_PAGE_SIZE\s*-\s*1\s*\)\s*\)\s*\)",
        "host-slot updater must reject non-init_mm or unaligned input",
        flags=re.DOTALL,
    )
    find(
        slot_range.text,
        r"WARN_ON_ONCE\s*\(\s*ops->mm\s*!=\s*&init_mm\s*\|\|\s*end\s*<=\s*addr\s*\|\|\s*"
        r"\(\s*\(\s*addr\s*\|\s*end\s*\)\s*&\s*"
        r"\(\s*DARWIN_HOST_PAGE_SIZE\s*-\s*1\s*\)\s*\)\s*\)",
        "host-slot range updater must enforce nonempty 16 KiB-aligned bounds",
        flags=re.DOTALL,
    )
    find(
        slot_range.text,
        r"for\s*\(\s*;\s*addr\s*<\s*end\s*&&\s*!ret\s*;\s*"
        r"addr\s*\+=\s*DARWIN_HOST_PAGE_SIZE\s*\)\s*"
        r"ret\s*=\s*update_init_mm_pte_slot\s*\(",
        "host-slot range updater must propagate the first slot failure",
        flags=re.DOTALL,
    )

    require(
        re.search(r"\bpte_t\s+snapshots\s*\[\s*4\s*\]\s*;", mask_c_comments_and_literals(slot.text))
        is not None,
        "host-slot updater must snapshot exactly four guest PTEs",
    )
    require(
        "not claimed to be one atomic snapshot" in slot.text
        and "publishes" in slot.text
        and "pending range" in slot.text
        and "serialized later walk" in slot.text
        and "exact fault repair" in slot.text,
        "mixed-PTE snapshot rationale must retain later-publication and exact-fault repair",
    )
    order = require_order(
        slot.text,
        (
            ("PTE lock acquisition", r"spin_lock\s*\(\s*ptl\s*\)\s*;"),
            (
                "four-PTE snapshot loop",
                r"for\s*\(\s*index\s*=\s*0\s*;\s*"
                r"index\s*<\s*ARRAY_SIZE\s*\(\s*snapshots\s*\)\s*;\s*"
                r"\+\+index\s*\)\s*"
                r"snapshots\s*\[\s*index\s*\]\s*=\s*"
                r"ptep_get\s*\(\s*pte\s*\+\s*index\s*\)\s*;",
            ),
            ("PTE lock release", r"spin_unlock\s*\(\s*ptl\s*\)\s*;"),
            (
                "whole-slot preflush",
                r"ret\s*=\s*os_drop_memory\s*\(\s*\(\s*void\s*\*\s*\)\s*addr\s*,\s*"
                r"DARWIN_HOST_PAGE_SIZE\s*\)\s*;\s*"
                r"if\s*\(\s*ret\s*\)\s*return\s+ret\s*;",
            ),
            (
                "present-quarter mapping loop",
                r"for\s*\(\s*index\s*=\s*0\s*;\s*"
                r"index\s*<\s*ARRAY_SIZE\s*\(\s*snapshots\s*\)\s*;\s*"
                r"\+\+index\s*\)\s*\{\s*"
                r"if\s*\(\s*!pte_present\s*\(\s*snapshots\s*\[\s*index\s*\]\s*\)\s*\)\s*"
                r"continue\s*;\s*ret\s*=\s*darwin_map_init_mm_pte\s*\([^;]+;\s*"
                r"if\s*\(\s*ret\s*\)\s*return\s+ret\s*;\s*\}",
            ),
            (
                "absent-quarter unmapping loop",
                r"for\s*\(\s*index\s*=\s*0\s*;\s*"
                r"index\s*<\s*ARRAY_SIZE\s*\(\s*snapshots\s*\)\s*;\s*"
                r"\+\+index\s*\)\s*\{\s*"
                r"if\s*\(\s*pte_present\s*\(\s*snapshots\s*\[\s*index\s*\]\s*\)\s*\)\s*"
                r"continue\s*;\s*ret\s*=\s*ops->unmap\s*\([^;]+;\s*"
                r"if\s*\(\s*ret\s*\)\s*return\s+ret\s*;\s*\}",
            ),
        ),
    )
    host_mutations = (
        order["whole-slot preflush"].start(),
        order["present-quarter mapping loop"].start(),
        order["absent-quarter unmapping loop"].start(),
    )
    require(
        host_mutations[0] == min(host_mutations),
        "whole-slot preflush must precede every host mapping mutation",
    )
    before_preflush = mask_c_comments_and_literals(slot.text)[
        : order["whole-slot preflush"].start()
    ]
    require(
        re.search(r"\bdarwin_map_init_mm_pte\s*\(|\bops->(?:mmap|unmap)\s*\(", before_preflush)
        is None,
        "host mapping mutation occurs before the whole-slot preflush",
    )

    dispatch = find(
        tlb,
        r"if\s*\(\s*unlikely\s*\(\s*ops->mm\s*==\s*&init_mm\s*\)\s*\)\s*"
        r"return\s+update_init_mm_pte_range\s*\(",
        "4 KiB update_pte_range must dispatch init_mm through the slot updater",
        flags=re.DOTALL,
    )
    require_darwin_context(
        tlb,
        dispatch.start(),
        four_k_only=True,
        subject="init_mm host-slot dispatch",
    )


def validate_trap_repair(trap: str) -> None:
    segv = function_region(trap, "segv")
    matches = require_order(
        segv.text,
        (
            (
                "exact faulting Linux page",
                r"unsigned\s+long\s+page\s*=\s*address\s*&\s*PAGE_MASK\s*;",
            ),
            (
                "exact fault-page publication",
                r"um_tlb_mark_sync\s*\(\s*&init_mm\s*,\s*page\s*,\s*"
                r"page\s*\+\s*PAGE_SIZE\s*\)\s*;",
            ),
            (
                "kernel TLB synchronization",
                r"err\s*=\s*um_tlb_sync\s*\(\s*&init_mm\s*\)\s*;",
            ),
            (
                "synchronization failure panic",
                r"if\s*\(\s*err\s*\)\s*panic\s*\(",
            ),
            (
                "host mapping/permission validation",
                r"err\s*=\s*darwin_uml_vm_retry_fault\s*\(\s*address\s*,\s*"
                r"FAULT_EXECUTE\s*\(\s*fi\s*\)\s*,\s*is_write\s*\)\s*;",
            ),
            (
                "exact successful-repair requirement",
                r"if\s*\(\s*err\s*!=\s*1\s*\)\s*panic\s*\(",
            ),
            ("fault retry return path", r"goto\s+out\s*;"),
        ),
    )
    for label in (
        "exact faulting Linux page",
        "exact fault-page publication",
        "host mapping/permission validation",
        "exact successful-repair requirement",
    ):
        require_darwin_context(
            trap,
            segv.start + matches[label].start(),
            four_k_only=False,
            subject=label,
        )
    masked = mask_c_comments_and_literals(segv.text)
    retry_end = matches["host mapping/permission validation"].end()
    exact_check = matches["exact successful-repair requirement"].start()
    require(
        re.search(r"\b(?:return|goto)\b", masked[retry_end:exact_check]) is None,
        "kernel fault path can return before validating the host repair result",
    )


def validate_nested_tlb_range_policy(um_tlb_header: str, mmu_gather: str) -> None:
    header_masked = mask_c_comments_and_literals(um_tlb_header)
    hook_pattern = re.compile(
        r"#\s*define\s+arch_tlb_flush_nested_requires_fullmm\s*"
        r"\(\s*tlb\s*\)\s+false\b"
    )
    hook_matches = list(hook_pattern.finditer(header_masked))
    require(
        len(hook_matches) == 1,
        "UML must define exactly one false nested-TLB fullmm hook",
    )
    hook_context = tuple(
        re.sub(r"\s+", "", expression)
        for expression in preprocessor_context(
            um_tlb_header, hook_matches[0].start()
        )
    )
    require(
        hook_context[-1:] == ("defined(UML_HOST_DARWIN)",)
        and not any("CONFIG_PAGE_SIZE" in expression for expression in hook_context),
        "nested-TLB range preservation must cover every Darwin page profile",
    )

    gather_masked = mask_c_comments_and_literals(mmu_gather)
    default_pattern = re.compile(
        r"#\s*ifndef\s+arch_tlb_flush_nested_requires_fullmm\s*\n"
        r"\s*#\s*define\s+arch_tlb_flush_nested_requires_fullmm\s*"
        r"\(\s*tlb\s*\)\s+true\b\s*\n\s*#\s*endif\b"
    )
    defaults = list(default_pattern.finditer(gather_masked))
    require(
        len(defaults) == 1,
        "generic mmu-gather must default nested TLB flushing to fullmm",
    )
    require(
        preprocessor_context(mmu_gather, defaults[0].start()) == (),
        "generic nested-TLB fullmm default must be unconditional",
    )

    finish = function_region(mmu_gather, "tlb_finish_mmu")
    find(
        finish.text,
        r"if\s*\(\s*mm_tlb_flush_nested\s*\(\s*tlb->mm\s*\)\s*&&\s*"
        r"arch_tlb_flush_nested_requires_fullmm\s*\(\s*tlb\s*\)\s*\)\s*\{\s*"
        r"tlb->fullmm\s*=\s*1\s*;\s*__tlb_reset_range\s*\(\s*tlb\s*\)\s*;\s*"
        r"tlb->freed_tables\s*=\s*1\s*;\s*\}",
        "tlb_finish_mmu must gate nested fullmm promotion on the architecture hook",
        flags=re.DOTALL,
    )


def validate_atomic_uaccess_contention(uaccess: str, um_kconfig: str) -> None:
    """A pagefault-disabled uaccess walk may never report contention as a fault."""
    find(
        uaccess,
        r"#\s*include\s+<linux/rcupdate\.h>",
        "uaccess.c must include the RCU interface it walks page tables under",
    )
    find(
        um_kconfig,
        r"(?m)^\s*select\s+MMU_GATHER_RCU_TABLE_FREE\s*$",
        "UML must select MMU_GATHER_RCU_TABLE_FREE for the RCU page-table walk",
    )
    region = function_region(uaccess, "pin_user_mapping")
    require_order(
        region.text,
        (
            (
                "recorded trylock result",
                r"mmap_locked\s*=\s*mmap_read_trylock\s*\(\s*mm\s*\)\s*;",
            ),
            (
                "RCU fallback on contention",
                r"if\s*\(\s*!\s*mmap_locked\s*\)\s*rcu_read_lock\s*\(\s*\)\s*;",
            ),
            (
                "blocking acquisition for the faulting caller",
                r"\}\s*else\s*\{\s*mmap_read_lock\s*\(\s*mm\s*\)\s*;\s*\}",
            ),
            (
                "matched release of whichever lock was taken",
                r"if\s*\(\s*mmap_locked\s*\)\s*mmap_read_unlock\s*\(\s*mm\s*\)\s*;\s*"
                r"else\s*rcu_read_unlock\s*\(\s*\)\s*;",
            ),
        ),
    )
    masked = mask_c_comments_and_literals(region.text)
    guard = re.search(
        r"if\s*\(\s*faulthandler_disabled\s*\(\s*\)\s*\)", masked)
    require(
        guard is not None,
        "pin_user_mapping must branch on faulthandler_disabled()",
    )
    walk = re.search(r"\bpgd\s*=\s*pgd_offset\s*\(", masked)
    require(walk is not None, "pin_user_mapping must walk from pgd_offset()")
    require(
        re.search(r"\breturn\b", masked[guard.start() : walk.start()]) is None,
        "pin_user_mapping must not abandon the walk when the mmap lock is contended",
    )


def validate_darwin_fault_ownership(
    darwin_internal: str,
    darwin_process: str,
    darwin_signal: str,
    darwin_smp: str,
) -> None:
    """A synchronous host fault may only be interpreted by its owner."""
    internal_masked = mask_c_comments_and_literals(darwin_internal)
    find(
        internal_masked,
        r"int\s+darwin_carrier_cpu\s*\(\s*void\s*\)\s*;\s*"
        r"void\s+darwin_adopt_boot_thread\s*\(\s*void\s*\)\s*;\s*"
        r"void\s+darwin_release_boot_thread\s*\(\s*void\s*\)\s*;",
        "Darwin SMP declarations must expose the fault-owner lifecycle",
        flags=re.DOTALL,
    )
    find(
        internal_masked,
        r"static\s+inline\s+int\s+darwin_carrier_cpu\s*\(\s*void\s*\)\s*"
        r"\{\s*return\s+0\s*;\s*\}\s*"
        r"static\s+inline\s+void\s+darwin_adopt_boot_thread\s*"
        r"\(\s*void\s*\)\s*\{\s*\}\s*"
        r"static\s+inline\s+void\s+darwin_release_boot_thread\s*"
        r"\(\s*void\s*\)\s*\{\s*\}",
        "single-CPU Darwin must retain its boot-thread owner stubs",
        flags=re.DOTALL,
    )

    carrier = function_region(darwin_smp, "darwin_carrier_cpu")
    find(
        carrier.text,
        r"if\s*\(\s*__atomic_load_n\s*\(\s*&current_cpu_key_ready\s*,\s*"
        r"__ATOMIC_ACQUIRE\s*\)\s*!=\s*0\s*\)\s*\{\s*"
        r"carrier\s*=\s*pthread_getspecific\s*\(\s*current_cpu_key\s*\)\s*;\s*"
        r"if\s*\(\s*carrier\s*!=\s*NULL\s*\)\s*return\s+carrier->cpu\s*;\s*\}",
        "carrier lookup must report the owning virtual CPU",
        flags=re.DOTALL,
    )
    find(
        carrier.text,
        r"owner\s*=\s*__atomic_load_n\s*\(\s*&boot_thread\s*,\s*"
        r"__ATOMIC_ACQUIRE\s*\)\s*;\s*"
        r"if\s*\(\s*owner\s*!=\s*0\s*&&\s*"
        r"pthread_equal\s*\(\s*pthread_self\s*\(\s*\)\s*,\s*"
        r"\(\s*pthread_t\s*\)\s*owner\s*\)\s*\)\s*return\s+0\s*;\s*"
        r"return\s+-1\s*;",
        "a thread with no carrier is only the kernel's while it is the boot thread",
        flags=re.DOTALL,
    )

    adopt = function_region(darwin_smp, "darwin_adopt_boot_thread")
    find(
        adopt.text,
        r"__atomic_store_n\s*\(\s*&boot_thread\s*,\s*"
        r"\(\s*uintptr_t\s*\)\s*pthread_self\s*\(\s*\)\s*,\s*"
        r"__ATOMIC_RELEASE\s*\)\s*;",
        "boot-thread adoption must publish the real host thread",
        flags=re.DOTALL,
    )
    release = function_region(darwin_smp, "darwin_release_boot_thread")
    find(
        release.text,
        r"__atomic_store_n\s*\(\s*&boot_thread\s*,\s*0\s*,\s*"
        r"__ATOMIC_RELEASE\s*\)\s*;",
        "boot-thread release must clear ownership",
        flags=re.DOTALL,
    )
    primary = function_region(darwin_smp, "darwin_start_primary_cpu_carrier")
    find(
        primary.text,
        r"if\s*\(\s*error\s*==\s*0\s*\)\s*"
        r"darwin_release_boot_thread\s*\(\s*\)\s*;\s*return\s+error\s*;",
        "the boot thread must stop owning the kernel once CPU0's carrier exists",
        flags=re.DOTALL,
    )

    installer = function_region(darwin_process, "init_new_thread_signals")
    require_order(
        installer.text,
        (
            ("boot-thread adoption", r"darwin_adopt_boot_thread\s*\(\s*\)\s*;"),
            ("SIGSEGV handler installation", r"set_handler\s*\(\s*SIGSEGV\s*\)\s*;"),
            ("SIGBUS handler installation", r"set_handler\s*\(\s*SIGBUS\s*\)\s*;"),
        ),
    )

    hard = function_region(darwin_signal, "hard_handler")
    gate = find(
        hard.text,
        r"if\s*\(\s*\(\s*signal\s*==\s*SIGSEGV\s*\|\|\s*"
        r"signal\s*==\s*SIGBUS\s*\)\s*&&\s*"
        r"darwin_carrier_cpu\s*\(\s*\)\s*<\s*0\s*\)\s*\{\s*"
        r"darwin_report_foreign_fault\s*\(\s*signal\s*,\s*info\s*,\s*"
        r"context\s*\)\s*;\s*errno\s*=\s*saved_errno\s*;\s*return\s*;\s*\}",
        "an unowned synchronous fault must leave the guest signal path",
        flags=re.DOTALL,
    )
    local_state = find(
        hard.text,
        r"state\s*=\s*local_signal_state\s*\(\s*\)\s*;",
        "hard handler must obtain its local signal state",
    )
    require(
        gate.end() < local_state.start(),
        "foreign faults must be rejected before touching virtual-CPU signal state",
    )

    report = function_region(darwin_signal, "darwin_report_foreign_fault")
    require(
        "far=%#lx pc=%#lx sp=%#lx lr=%#lx" in report.text
        and "address=%#lx" in report.text
        and "%p" not in report.text,
        "foreign-fault reports must expose literal host addresses",
    )
    find(
        report.text,
        r"restore\.sa_handler\s*=\s*SIG_DFL\s*;\s*"
        r"sigemptyset\s*\(\s*&restore\.sa_mask\s*\)\s*;\s*"
        r"\(\s*void\s*\)\s*sigaction\s*\(\s*signal\s*,\s*&restore\s*,\s*NULL\s*\)\s*;",
        "a foreign fault must restore the terminating host disposition",
        flags=re.DOTALL,
    )
    require(
        "owns no virtual CPU" in report.text
        and "the guest is not the faulting agent" in report.text,
        "foreign-fault reporting must preserve its ownership diagnosis",
    )


def validate_sources(
    tlb: str,
    trap: str,
    um_tlb_header: str,
    mmu_gather: str,
    darwin_internal: str,
    darwin_process: str,
    darwin_signal: str,
    darwin_smp: str,
    uaccess: str,
    um_kconfig: str,
) -> None:
    validate_init_mm_wrapper(tlb)
    validate_pending_range_transaction(tlb)
    validate_bounded_user_replay(tlb)
    validate_four_k_slot_policy(tlb)
    validate_trap_repair(trap)
    validate_nested_tlb_range_policy(um_tlb_header, mmu_gather)
    validate_atomic_uaccess_contention(uaccess, um_kconfig)
    validate_darwin_fault_ownership(
        darwin_internal,
        darwin_process,
        darwin_signal,
        darwin_smp,
    )


def substitute_once(source: str, pattern: str, replacement: str) -> str:
    mutated, count = re.subn(pattern, replacement, source, count=1, flags=re.DOTALL)
    require(count == 1, f"mutation setup did not match exactly once: {pattern}")
    return mutated


def move_literal_after(source: str, moving: str, anchor: str) -> str:
    require(source.count(moving) == 1, f"mutation moving text is not unique: {moving!r}")
    require(source.count(anchor) == 1, f"mutation anchor is not unique: {anchor!r}")
    without = source.replace(moving, "", 1)
    return without.replace(anchor, anchor + moving, 1)


def run_mutation_fixtures(
    tlb: str,
    trap: str,
    um_tlb_header: str,
    mmu_gather: str,
    darwin_internal: str,
    darwin_process: str,
    darwin_signal: str,
    darwin_smp: str,
    uaccess: str,
    um_kconfig: str,
) -> None:
    Mutation = tuple[str, str, Callable[[str], str]]
    mutations: tuple[Mutation, ...] = (
        (
            "wrapper-noinline",
            "tlb",
            lambda value: substitute_once(
                value,
                r"static\s+noinline\s+int\s+um_tlb_sync_init_mm",
                "static int um_tlb_sync_init_mm",
            ),
        ),
        (
            "wrapper-all-darwin-profiles",
            "tlb",
            lambda value: substitute_once(
                value,
                r"(#\s*ifdef\s+UML_HOST_DARWIN\s*\n"
                r"static\s+struct\s+task_struct\s+\*init_mm_tlb_owner)",
                "#if defined(UML_HOST_DARWIN) && "
                "!defined(CONFIG_PAGE_SIZE_16KB)\n"
                "static struct task_struct *init_mm_tlb_owner",
            ),
        ),
        (
            "same-task-reentry",
            "tlb",
            lambda value: substitute_once(value, r"return\s+-EDEADLK\s*;", "return -EIO;"),
        ),
        (
            "lock-after-walk",
            "tlb",
            lambda value: move_literal_after(
                value,
                "\tmutex_lock(&init_mm.context.turnstile);\n",
                "\tret = __um_tlb_sync(&init_mm, false, 0, 0);\n",
            ),
        ),
        (
            "post-lock-owner-invariant",
            "tlb",
            lambda value: substitute_once(
                value,
                r"if\s*\(WARN_ON_ONCE\(READ_ONCE\(init_mm_tlb_owner\)\s*!=\s*NULL\)\)",
                "if (false)",
            ),
        ),
        (
            "owner-clear-before-unlock",
            "tlb",
            lambda value: move_literal_after(
                value,
                "\tWRITE_ONCE(init_mm_tlb_owner, NULL);\n",
                "\tmutex_unlock(&init_mm.context.turnstile);\n",
            ),
        ),
        (
            "locked-init-mm-rejection",
            "tlb",
            lambda value: substitute_once(
                value,
                r"(if\s*\(WARN_ON_ONCE\(mm == &init_mm\)\)\s*)return -EINVAL;",
                r"\1return 0;",
            ),
        ),
        (
            "caller-range-joins-snapshot",
            "tlb",
            lambda value: substitute_once(
                value,
                r"um_tlb_mark_sync_locked\(mm, force_start, force_end\);",
                "um_tlb_mark_sync(mm, force_start, force_end);",
            ),
        ),
        (
            "user-replay-batch-unbounded",
            "tlb",
            lambda value: substitute_once(
                value,
                r"ops->budget = UML_TLB_SYNC_BATCH_PAGES;",
                "ops->budget = 0;",
            ),
        ),
        (
            "rejected-batch-retries-without-checkpoint",
            "tlb",
            lambda value: substitute_once(
                value,
                r"if \(\+\+rejected % UML_TLB_SYNC_REJECTS_PER_CHECKPOINT == 0\)\s*"
                r"darwin_tlb_sync_checkpoint\(\);",
                "++rejected;",
            ),
        ),
        (
            "full-batch-restarts-the-range",
            "tlb",
            lambda value: substitute_once(
                value,
                r"addr = ops->resume;",
                "ops->resume = addr;",
            ),
        ),
        (
            "user-replay-batch-per-range",
            "tlb",
            lambda value: substitute_once(
                value,
                r"if \(ret \|\| \+\+batch_index == count\)\s*break;",
                "++batch_index; break;",
            ),
        ),
        (
            "pending-range-clear",
            "tlb",
            lambda value: substitute_once(
                value,
                r"mm->context\.sync_tlb_range_count\s*=\s*0\s*;",
                "mm->context.sync_tlb_range_count = sync_count;",
            ),
        ),
        (
            "all-ranges-apply",
            "tlb",
            lambda value: substitute_once(
                value,
                r"sync_index\s*<\s*sync_count\s*&&\s*!ret",
                "sync_index + 1 < sync_count && !ret",
            ),
        ),
        (
            "failed-range-requeue",
            "tlb",
            lambda value: substitute_once(
                value,
                r"um_tlb_mark_sync\(mm, sync_ranges\[sync_index\]\.from,\s*"
                r"sync_ranges\[sync_index\]\.to\);",
                "um_tlb_mark_sync(mm, 0, 0);",
            ),
        ),
        (
            "eagain-retry",
            "tlb",
            lambda value: substitute_once(value, r"goto\s+retry\s*;", "return ret;"),
        ),
        (
            "4k-only-compilation",
            "tlb",
            lambda value: substitute_once(
                value,
                r"#if defined\(UML_HOST_DARWIN\) && !defined\(CONFIG_PAGE_SIZE_16KB\)",
                "#ifdef UML_HOST_DARWIN",
            ),
        ),
        (
            "four-quarter-geometry",
            "tlb",
            lambda value: substitute_once(value, r"snapshots\[4\]", "snapshots[3]"),
        ),
        (
            "pmd-alignment-geometry",
            "tlb",
            lambda value: substitute_once(
                value,
                r"static_assert\(PMD_SIZE % DARWIN_HOST_PAGE_SIZE == 0\);",
                "static_assert(PMD_SIZE % PAGE_SIZE == 0);",
            ),
        ),
        (
            "mixed-snapshot-rationale",
            "tlb",
            lambda value: value.replace(
                "not claimed to be one atomic snapshot",
                "claimed to be one atomic snapshot",
                1,
            ),
        ),
        (
            "whole-slot-preflush",
            "tlb",
            lambda value: substitute_once(
                value,
                r"ret = os_drop_memory\(\(void \*\)addr, DARWIN_HOST_PAGE_SIZE\);\s*"
                r"if \(ret\)\s*return ret;",
                "ret = 0;",
            ),
        ),
        (
            "preflush-before-host-mutation",
            "tlb",
            lambda value: substitute_once(
                value,
                r"(ret = os_drop_memory\(\(void \*\)addr, DARWIN_HOST_PAGE_SIZE\);)",
                "ops->unmap(ops->mm_idp, addr, PAGE_SIZE);\n\t\\1",
            ),
        ),
        (
            "present-before-absent",
            "tlb",
            lambda value: substitute_once(
                value,
                r"if \(!pte_present\(snapshots\[index\]\)\)",
                "if (pte_present(snapshots[index]))",
            ),
        ),
        (
            "slot-failure-propagation",
            "tlb",
            lambda value: substitute_once(
                value,
                r"if \(ret\)\s*return ret;",
                "if (ret) ret = 0;",
            ),
        ),
        (
            "present-map-failure-propagation",
            "tlb",
            lambda value: substitute_once(
                value,
                r"(ret = darwin_map_init_mm_pte\([^;]+;\s*)"
                r"if \(ret\)\s*return ret;",
                r"\1if (ret) ret = 0;",
            ),
        ),
        (
            "absent-unmap-failure-propagation",
            "tlb",
            lambda value: substitute_once(
                value,
                r"(ret = ops->unmap\(ops->mm_idp, addr \+ index \* PAGE_SIZE,\s*"
                r"PAGE_SIZE\);\s*)if \(ret\)\s*return ret;",
                r"\1if (ret) ret = 0;",
            ),
        ),
        (
            "slot-range-failure-propagation",
            "tlb",
            lambda value: substitute_once(
                value,
                r"addr < end && !ret; addr \+= DARWIN_HOST_PAGE_SIZE",
                "addr < end; addr += DARWIN_HOST_PAGE_SIZE",
            ),
        ),
        (
            "exact-fault-page-mask",
            "trap",
            lambda value: substitute_once(
                value,
                r"address\s*&\s*PAGE_MASK",
                "address & ~(16384UL - 1)",
            ),
        ),
        (
            "fault-mark-before-sync",
            "trap",
            lambda value: move_literal_after(
                value,
                "\t\tum_tlb_mark_sync(&init_mm, page, page + PAGE_SIZE);\n",
                "\t\terr = um_tlb_sync(&init_mm);\n",
            ),
        ),
        (
            "exact-retry-success",
            "trap",
            lambda value: substitute_once(value, r"if \(err != 1\)", "if (err < 0)"),
        ),
        (
            "no-return-before-retry-validation",
            "trap",
            lambda value: substitute_once(
                value,
                r"(err = darwin_uml_vm_retry_fault\(address, FAULT_EXECUTE\(fi\),\s*"
                r"is_write\);)",
                r"\1\n\t\treturn 0;",
            ),
        ),
        (
            "darwin-nested-flush-fullmm",
            "um_tlb_header",
            lambda value: substitute_once(
                value,
                r"(arch_tlb_flush_nested_requires_fullmm\(tlb\)\s+)false",
                r"\1true",
            ),
        ),
        (
            "darwin-nested-flush-page-profile",
            "um_tlb_header",
            lambda value: substitute_once(
                value,
                r"#ifdef\s+UML_HOST_DARWIN",
                "#ifdef CONFIG_PAGE_SIZE_16KB",
            ),
        ),
        (
            "generic-nested-flush-default",
            "mmu_gather",
            lambda value: substitute_once(
                value,
                r"(arch_tlb_flush_nested_requires_fullmm\(tlb\)\s+)true",
                r"\1false",
            ),
        ),
        (
            "nested-flush-hook-dispatch",
            "mmu_gather",
            lambda value: substitute_once(
                value,
                r"if \(mm_tlb_flush_nested\(tlb->mm\) &&\s*"
                r"arch_tlb_flush_nested_requires_fullmm\(tlb\)\)",
                "if (mm_tlb_flush_nested(tlb->mm))",
            ),
        ),
        (
            "atomic-contention-reported-as-fault",
            "uaccess",
            lambda value: substitute_once(
                value,
                r"mmap_locked = mmap_read_trylock\(mm\);\n"
                r"\t\tif \(!mmap_locked\)\n"
                r"\t\t\trcu_read_lock\(\);",
                "if (!mmap_read_trylock(mm))\n\t\t\treturn NULL;",
            ),
        ),
        (
            "atomic-contended-walk-without-rcu",
            "uaccess",
            lambda value: substitute_once(
                value,
                r"\t\tif \(!mmap_locked\)\n\t\t\trcu_read_lock\(\);\n",
                "",
            ),
        ),
        (
            "atomic-walk-always-drops-mmap-lock",
            "uaccess",
            lambda value: substitute_once(
                value,
                r"\tif \(mmap_locked\)\n"
                r"\t\tmmap_read_unlock\(mm\);\n"
                r"\telse\n"
                r"\t\trcu_read_unlock\(\);",
                "\tmmap_read_unlock(mm);",
            ),
        ),
        (
            "uml-drops-rcu-table-free",
            "um_kconfig",
            lambda value: substitute_once(
                value,
                r"(?m)^\tselect MMU_GATHER_RCU_TABLE_FREE\n",
                "",
            ),
        ),
        (
            "fault-ownership-hard-handler",
            "darwin_signal",
            lambda value: substitute_once(
                value,
                r"if \(\(signal == SIGSEGV \|\| signal == SIGBUS\) &&\s*"
                r"darwin_carrier_cpu\(\) < 0\) \{\s*"
                r"darwin_report_foreign_fault\(signal, info, context\);\s*",
                "if (false) {\n\t\tdarwin_report_foreign_fault(signal, info, context);\n",
            ),
        ),
        (
            "fault-ownership-carrier-default",
            "darwin_smp",
            lambda value: substitute_once(
                value,
                r"(if \(owner != 0 && pthread_equal\(pthread_self\(\),\s*"
                r"\(pthread_t\)owner\)\)\s*return 0;\s*)return -1;",
                r"\1return 0;",
            ),
        ),
        (
            "fault-ownership-boot-release",
            "darwin_smp",
            lambda value: substitute_once(
                value,
                r"(if \()error == 0(\)\s*darwin_release_boot_thread\(\);)",
                r"\1error != 0\2",
            ),
        ),
        (
            "fault-ownership-boot-adoption",
            "darwin_process",
            lambda value: substitute_once(
                value,
                r"darwin_adopt_boot_thread\(\);",
                "darwin_release_boot_thread();",
            ),
        ),
        (
            "fault-ownership-default-disposition",
            "darwin_signal",
            lambda value: substitute_once(
                value,
                r"restore\.sa_handler = SIG_DFL;",
                "restore.sa_handler = SIG_IGN;",
            ),
        ),
        (
            "fault-ownership-literal-addresses",
            "darwin_signal",
            lambda value: substitute_once(
                value,
                r"far=%#lx pc=%#lx sp=%#lx lr=%#lx",
                "far=%p pc=%p sp=%p lr=%p",
            ),
        ),
    )

    sources = {
        "tlb": tlb,
        "trap": trap,
        "um_tlb_header": um_tlb_header,
        "mmu_gather": mmu_gather,
        "darwin_internal": darwin_internal,
        "darwin_process": darwin_process,
        "darwin_signal": darwin_signal,
        "darwin_smp": darwin_smp,
        "uaccess": uaccess,
        "um_kconfig": um_kconfig,
    }
    for name, target, mutate in mutations:
        mutated = dict(sources)
        mutated[target] = mutate(mutated[target])
        try:
            validate_sources(
                mutated["tlb"],
                mutated["trap"],
                mutated["um_tlb_header"],
                mutated["mmu_gather"],
                mutated["darwin_internal"],
                mutated["darwin_process"],
                mutated["darwin_signal"],
                mutated["darwin_smp"],
                mutated["uaccess"],
                mutated["um_kconfig"],
            )
        except PolicyError:
            continue
        raise AssertionError(f"{name}: policy validator accepted the mutation")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--linux-source",
        required=True,
        type=Path,
        help="prepared Linux source tree containing arch/um/kernel",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    kernel_directory = arguments.linux_source / "arch" / "um" / "kernel"
    tlb_path = kernel_directory / "tlb.c"
    trap_path = kernel_directory / "trap.c"
    um_tlb_header_path = (
        arguments.linux_source / "arch" / "um" / "include" / "asm" / "tlb.h"
    )
    mmu_gather_path = arguments.linux_source / "mm" / "mmu_gather.c"
    darwin_directory = arguments.linux_source / "arch" / "um" / "os-Darwin"
    darwin_internal_path = darwin_directory / "internal.h"
    darwin_process_path = darwin_directory / "process.c"
    darwin_signal_path = darwin_directory / "signal.c"
    darwin_smp_path = darwin_directory / "smp.c"
    uaccess_path = kernel_directory / "skas" / "uaccess.c"
    um_kconfig_path = arguments.linux_source / "arch" / "um" / "Kconfig"
    require(tlb_path.is_file(), f"missing prepared kernel source: {tlb_path}")
    require(trap_path.is_file(), f"missing prepared kernel source: {trap_path}")
    require(
        um_tlb_header_path.is_file(),
        f"missing prepared kernel source: {um_tlb_header_path}",
    )
    require(
        mmu_gather_path.is_file(),
        f"missing prepared kernel source: {mmu_gather_path}",
    )
    for path in (
        darwin_internal_path,
        darwin_process_path,
        darwin_signal_path,
        darwin_smp_path,
        uaccess_path,
        um_kconfig_path,
    ):
        require(path.is_file(), f"missing prepared kernel source: {path}")
    tlb = tlb_path.read_text(encoding="utf-8")
    trap = trap_path.read_text(encoding="utf-8")
    um_tlb_header = um_tlb_header_path.read_text(encoding="utf-8")
    mmu_gather = mmu_gather_path.read_text(encoding="utf-8")
    darwin_internal = darwin_internal_path.read_text(encoding="utf-8")
    darwin_process = darwin_process_path.read_text(encoding="utf-8")
    darwin_signal = darwin_signal_path.read_text(encoding="utf-8")
    darwin_smp = darwin_smp_path.read_text(encoding="utf-8")
    uaccess = uaccess_path.read_text(encoding="utf-8")
    um_kconfig = um_kconfig_path.read_text(encoding="utf-8")
    validate_sources(
        tlb,
        trap,
        um_tlb_header,
        mmu_gather,
        darwin_internal,
        darwin_process,
        darwin_signal,
        darwin_smp,
        uaccess,
        um_kconfig,
    )
    run_mutation_fixtures(
        tlb,
        trap,
        um_tlb_header,
        mmu_gather,
        darwin_internal,
        darwin_process,
        darwin_signal,
        darwin_smp,
        uaccess,
        um_kconfig,
    )
    print("Darwin init_mm TLB policy fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
