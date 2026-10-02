# ADR 0002: emulator-handle runtime ownership

- Status: accepted
- Date: 2026-08-02

Each `uml_emulator` handle owns its runtime context, engine, stop state, and
copied network callbacks. The callback provider itself remains caller-owned.

A handle admits one run at a time, and so does the process: a run maps its
embedded kernel at fixed host addresses, so a run on another handle queues at
the process address-space gate. An unbound callback that cannot identify one
active context fails closed.

A process runs one guest. A kernel claims guest RAM early in its boot, and a
process hands guest RAM out once. After a run has claimed it, every later run
in the process, on any handle, returns `UML_EMULATOR_UNSUPPORTED` before it
names a console, maps a kernel window or enters a kernel. A run that ends
before its kernel claims guest RAM leaves the process's guest unspent. A host
that wants another guest starts another process.

Outcome, 2026-10-03: the caller-supplied native kernel entry, and with it
concurrent native-callback handles, is removed. No product set it, and a
process runs one guest, so every run enters the embedded kernel image. Tests
reach a run through the emulator's test-only kernel image instead.
