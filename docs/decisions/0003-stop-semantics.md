# ADR 0003: make stop requests run-scoped and handle-local

Status: accepted, 2026-08-02

## Context

A process-global stop bit made one emulator handle interrupt another and made
an old request survive into a later run. Callers also need deterministic
behavior when requesting a stop while a handle is idle.

## Decision

The stop token belongs to the handle's runtime context. `uml_emulator_run`
atomically admits at most one run on that handle and clears the token at both
entry and exit. `uml_emulator_request_stop` sets the token only while that
handle is running; requesting a stop while idle is a no-op. CPU engines poll a
pointer to this handle-local token at their normal interrupt boundaries.

Destroying a handle remains synchronous: the caller must not destroy it while
another thread is inside `uml_emulator_run`. Network-session cleanup stops only
the provider instance owned by that run and cannot unwind through the public C
boundary.

## Consequences

A stop request affects the current run of exactly one handle and is never
inherited by a later run. Concurrent runs on different handles are independent.
The C bridge retains its legacy no-handle behavior only when no public run is
active, as described by ADR 0002.
