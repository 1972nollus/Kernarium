#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Rank fixed guest-GPR pairs from diagnostic JIT link profiles."""

from __future__ import annotations

import argparse
import itertools
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


SUMMARY = re.compile(
    r"^darwin-jit-gpr-link-summary executor=(?P<executor>\S+) "
    r"successful=(?P<successful>\d+) unattributed=(?P<unattributed>\d+) "
    r"outer=(?P<outer>\d+)$"
)
EDGE = re.compile(
    r"^darwin-jit-gpr-link-edge executor=(?P<executor>\S+) "
    r"kind=(?P<kind>direct|pic) source=(?P<source>0x[0-9a-fA-F]+) "
    r"target=(?P<target>0x[0-9a-fA-F]+) "
    r"dirty=(?P<dirty>0x[0-9a-fA-F]+|0) "
    r"livein=(?P<livein>0x[0-9a-fA-F]+|0) count=(?P<count>\d+)$"
)
GUEST_GPRS = range(32)
STACK_POINTER = 31


@dataclass(frozen=True)
class Edge:
    executor: str
    kind: str
    source: int
    target: int
    dirty: int
    livein: int
    count: int


@dataclass
class Profile:
    successful_links: int = 0
    unattributed_links: int = 0
    outer_invocations: int = 0
    summaries: int = 0
    edges: list[Edge] | None = None

    def __post_init__(self) -> None:
        if self.edges is None:
            self.edges = []

    @property
    def attributed_links(self) -> int:
        assert self.edges is not None
        return sum(edge.count for edge in self.edges)

    def saved_operations(self) -> list[int]:
        """Return conservative boundary loads/stores removed for each GPR."""
        saved = [0] * len(GUEST_GPRS)
        assert self.edges is not None
        for edge in self.edges:
            for register in GUEST_GPRS:
                bit = 1 << register
                saved[register] += edge.count * (
                    int(bool(edge.dirty & bit)) +
                    int(bool(edge.livein & bit))
                )
        return saved


@dataclass(frozen=True)
class PairScore:
    first: int
    second: int
    raw_saved: int
    boundary_words: int
    boundary_cost: int

    @property
    def net_saved(self) -> int:
        return self.raw_saved - self.boundary_cost


def read_profile(paths: Iterable[Path]) -> Profile:
    result = Profile()
    assert result.edges is not None
    for path in paths:
        with path.open(encoding="utf-8", errors="replace") as source:
            for raw_line in source:
                line = raw_line.strip()
                summary = SUMMARY.match(line)
                if summary is not None:
                    result.successful_links += int(summary["successful"])
                    result.unattributed_links += int(summary["unattributed"])
                    result.outer_invocations += int(summary["outer"])
                    result.summaries += 1
                    continue
                edge = EDGE.match(line)
                if edge is None:
                    continue
                result.edges.append(
                    Edge(
                        executor=edge["executor"],
                        kind=edge["kind"],
                        source=int(edge["source"], 16),
                        target=int(edge["target"], 16),
                        dirty=int(edge["dirty"], 16),
                        livein=int(edge["livein"], 16),
                        count=int(edge["count"]),
                    )
                )
    return result


def default_pair_boundary_words(first: int, second: int) -> int:
    """Return outer-entry/exit words for a pair in CpuState layout."""
    return 4 if second == first + 1 else 6


def rank_pairs(
    profile: Profile, pair_boundary_cost: int | None = None
) -> list[PairScore]:
    if pair_boundary_cost is not None and pair_boundary_cost < 0:
        raise ValueError("pair boundary cost must not be negative")
    saved = profile.saved_operations()
    scores = []
    for first, second in itertools.combinations(GUEST_GPRS, 2):
        boundary_words = (
            pair_boundary_cost
            if pair_boundary_cost is not None
            else default_pair_boundary_words(first, second)
        )
        scores.append(
            PairScore(
                first=first,
                second=second,
                raw_saved=saved[first] + saved[second],
                boundary_words=boundary_words,
                boundary_cost=profile.outer_invocations * boundary_words,
            )
        )
    return sorted(
        scores,
        key=lambda score: (
            -score.net_saved,
            -score.raw_saved,
            score.first,
            score.second,
        ),
    )


def register_name(register: int) -> str:
    return "sp" if register == STACK_POINTER else f"x{register}"


def pair_name(score: PairScore) -> str:
    return f"{register_name(score.first)}+{register_name(score.second)}"


def print_profile(
    name: str, profile: Profile, scores: list[PairScore], top: int
) -> None:
    attributed = profile.attributed_links
    expected_attributed = (
        profile.successful_links - profile.unattributed_links
    )
    coverage = (
        attributed / profile.successful_links
        if profile.successful_links
        else 0.0
    )
    print(
        f"profile\t{name}\texecutors\t{profile.summaries}\t"
        f"successful\t{profile.successful_links}\t"
        f"attributed\t{attributed}\tunattributed\t"
        f"{profile.unattributed_links}\touter\t"
        f"{profile.outer_invocations}\tcoverage\t{coverage:.6f}"
    )
    if attributed != expected_attributed:
        print(
            f"warning\t{name}\tedge-count-mismatch\t"
            f"observed={attributed}\texpected={expected_attributed}"
        )
    print(
        "rank\tpair\traw_saved\tboundary_words\tboundary_cost\t"
        "net_saved\tnet_per_1000_links"
    )
    denominator = profile.successful_links or 1
    for rank, score in enumerate(scores[:top], 1):
        per_thousand = score.net_saved * 1000.0 / denominator
        print(
            f"{rank}\t{pair_name(score)}\t{score.raw_saved}\t"
            f"{score.boundary_words}\t{score.boundary_cost}\t"
            f"{score.net_saved}\t"
            f"{per_thousand:.3f}"
        )


def pair_rank(scores: list[PairScore], pair: tuple[int, int]) -> int:
    for index, score in enumerate(scores, 1):
        if (score.first, score.second) == pair:
            return index
    raise ValueError(f"pair is not rankable: {pair!r}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Aggregate diagnostic successful-link counters and rank all "
            "496 fixed two-register banks, including SP."
        )
    )
    parser.add_argument("profiles", type=Path, nargs="+")
    parser.add_argument(
        "--against",
        type=Path,
        nargs="+",
        help="second capture whose independently ranked winner must be checked",
    )
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument(
        "--pair-boundary-cost",
        type=int,
        default=None,
        help=(
            "override native words charged per outer invocation for every "
            "pair (default: 4 for adjacent CpuState pairs, including "
            "x30+sp, and 6 otherwise)"
        ),
    )
    arguments = parser.parse_args()
    if arguments.top < 1:
        parser.error("--top must be positive")
    if (
        arguments.pair_boundary_cost is not None
        and arguments.pair_boundary_cost < 0
    ):
        parser.error("--pair-boundary-cost must not be negative")

    primary = read_profile(arguments.profiles)
    primary_scores = rank_pairs(primary, arguments.pair_boundary_cost)
    print_profile("primary", primary, primary_scores, arguments.top)

    if arguments.against is None:
        return
    comparison = read_profile(arguments.against)
    comparison_scores = rank_pairs(
        comparison, arguments.pair_boundary_cost
    )
    print()
    print_profile("comparison", comparison, comparison_scores, arguments.top)
    first_winner = (primary_scores[0].first, primary_scores[0].second)
    second_winner = (
        comparison_scores[0].first,
        comparison_scores[0].second,
    )
    print()
    print(
        "stability\t"
        f"primary={pair_name(primary_scores[0])}\t"
        f"comparison={pair_name(comparison_scores[0])}\t"
        f"same={int(first_winner == second_winner)}\t"
        f"primary_winner_rank_in_comparison="
        f"{pair_rank(comparison_scores, first_winner)}\t"
        f"comparison_winner_rank_in_primary="
        f"{pair_rank(primary_scores, second_winner)}"
    )


if __name__ == "__main__":
    main()
