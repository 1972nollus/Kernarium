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

## Decision

Each `uml_emulator` owns a runtime `Context`, its default or caller-provided
network provider, and any default SLiRP instance. The context owns engine
selection, the stop token, and per-thread architectural/translated-engine
state. Entering `uml_emulator_run` binds the calling thread to that context and
network provider for the complete native or ELF-kernel call.

An unbound bridge callback uses the sole active context/provider when there is
exactly one. With two or more active handles the association is ambiguous, so
CPU callbacks return a fatal/unsupported result and network callbacks return
unavailable. They never borrow either handle. With no public run active, the
process-local compatibility context remains available to legacy direct C-ABI
callers.

Construction of active-scope registries is exception-safe: thread-local state
is changed only after registry insertion succeeds. C entry points translate
allocation and callback exceptions to their documented error boundaries.

## Consequences

Two public entry threads can run concurrently with independent engine, stop,
network, and translated-cache state. A future kernel ABI may propagate an
opaque context to every vCPU and device callback; until then, concurrent
unbound callbacks fail closed instead of weakening handle isolation. The
`emulator-native-entry-api` unit test exercises both concurrent isolation and
the ambiguous-thread failure rule.
