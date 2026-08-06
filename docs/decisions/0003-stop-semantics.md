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

Handle admission remains active through execution-state retirement, so a new
run cannot be admitted while the prior run is still removing generation and
thread registrations. Embedded ELF runs and 4 KiB native-callback runs
additionally wait at one process-wide address-space gate after handle
admission. A stop request made during that wait cancels the queued run; it does
not enter the shared host address space and does not affect the run holding the
gate. A waiting exclusive run prevents later shared admissions from starving
it. The gate remains held until memory-domain retirement finishes. In a 16 KiB
build, native-callback runs use shared admission and rely on per-domain mapping
ownership to reject an overlapping host range; embedded runs still use
exclusive admission.

Destroying a handle remains synchronous: the caller must not destroy it while
another thread is inside `uml_emulator_run`. Network-session cleanup only
deactivates the copied callback binding. The callback context is caller-owned,
the installed callback table has no stop callback, and the macOS application
stops and destroys its injected SLiRP provider after emulator use.

## Consequences

A stop request affects the current or queued run of exactly one handle and is
never inherited by a later run. Native-callback handles remain concurrently
independent in 16 KiB builds; 4 KiB native and embedded handles retain
handle-local stop state while their kernel execution is process-serialized. It
does not imply ownership of or a stop request to the caller's network provider.
The C bridge retains its legacy no-handle behavior only when no public run is
active, as described by ADR 0002.
