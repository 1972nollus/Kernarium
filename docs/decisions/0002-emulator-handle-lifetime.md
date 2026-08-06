# ADR 0002: scope mutable runtime state to an emulator handle

Status: accepted, 2026-08-02

## Context

The public API permits more than one `uml_emulator` handle. Engine selection,
translated-code caches, stop state, and network providers were previously
process globals or unrelated thread locals. A callback or stop request from
one handle could therefore affect another handle or a later run.

The legacy Darwin/kernel bridge does not carry a public-handle pointer on every
callback. The public entry thread can be bound explicitly, while a
kernel-created thread is identifiable only when exactly one public run is
active.

Native-callback handles do not occupy the embedded kernel's fixed linked
window. In the 16 KiB build they retain independent concurrent admission. In
the 4 KiB compatibility build, however, each domain's quarter-page metadata
ultimately materializes at a process-global `MAP_FIXED` host address. Two
domains cannot isolate the same virtual address, so 4 KiB native callbacks and
embedded ELF handles share one process address-space gate. Embedded ELF
handles also require that gate because the linked window and process-global
Linux state permit only one embedded run in the process at a time.

## Decision

Each `uml_emulator` owns a runtime `Context`, a copied network callback table,
and the registry binding that makes those callbacks active during a run. The
callback context and the provider resources behind it remain caller-owned; the
installed callback contract deliberately has no stop or destroy callback. The
library has no default SLiRP instance. The macOS application owns SLiRP,
injects its callback table, and stops and destroys it after emulator use. The
context owns engine selection, the stop token, and per-thread
architectural/translated-engine state. Entering `uml_emulator_run` binds the
calling thread to that context and callback table for the complete native or
ELF-kernel call.

Run admission is handle-local and fail-fast: a second call on the same handle
returns `-UML_EMULATOR_RUNTIME_ERROR` instead of waiting. Different 16 KiB
native-callback handles may execute concurrently. Four-KiB native callbacks
and embedded ELF handles pass handle admission independently but serialize at
the process-wide address-space gate; a stop requested while queued cancels
that handle's pending run before it enters the shared host address space.
Handle admission and the process gate remain owned until execution-state and
memory-domain retirement complete, so the next run cannot observe a prior
native mapping that is still being flushed or unmapped.

Each 16 KiB memory domain also owns its directly materialized host mappings.
A process-wide ownership registry rejects overlap with another live domain,
reserves an exact free host range before replacing it, and retires every owned
mapping before releasing the run. Operations that alter a direct mapping or
its protection must cover complete host pages. The 4 KiB compatibility bridge
routes every high native mapping through per-domain shadow metadata, so later
quarter-page remap, protect, and unmap operations remain coherent within the
16 KiB host slot. Allocation or registry failure fails closed, and every C
bridge boundary returns a negative result with a defined `errno`.

An unbound bridge callback uses the sole active context/provider when there is
exactly one. With two or more active handles the association is ambiguous, so
CPU callbacks return a fatal/unsupported result and network callbacks return
unavailable. They never borrow either handle. With no public run active, the
process-local compatibility context remains available to legacy direct C-ABI
callers.

One neutral opaque route carries the context and provider endpoints. Its
allocation-free reservation, commit, and retirement each occur under one
registry lock, so callbacks cannot observe context from one admission state
and network from another. A pending route leaves the prior sole fallback
unchanged. A successfully committed overlap fails closed for unbound callbacks
until the entire overlapping cohort retires. C entry points translate
allocation and callback exceptions to their documented error boundaries.

## Consequences

Two native-callback public entry threads can run concurrently with independent
engine, stop, network routing, and translated-cache state in a 16 KiB build.
Four-KiB native callbacks and embedded ELF entry threads are process-serialized.
Provider lifetime still belongs to each caller (the macOS application in the
default product), not to the emulator handle. A future kernel ABI may propagate an opaque context
to every vCPU and device callback; until then, concurrent unbound callbacks
fail closed instead of weakening handle isolation. The
`emulator-native-entry-api` exercises 16 KiB native concurrency and the
ambiguous-thread failure rule. `emulator-native-16k-ownership` exercises
mapping ownership, collision rejection, complete rollback, retirement, and
successful retry. The 4 KiB same-address admission test exercises
serialization, queued cancellation, domain retirement, and retry; the
embedded serialization test exercises the same gate for the fixed linked
window.
