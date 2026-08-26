# ADR 0003: handle-local stop requests

- Status: accepted
- Date: 2026-08-02

The stop token belongs to the emulator handle's runtime context. A request
affects only that handle's current or queued run, is a no-op while idle, and is
cleared at run entry and exit.

Queued address-space-gated runs can be cancelled without entering the shared
mapping domain. Destroying a handle remains synchronous and requires that no
thread is inside its run call.
