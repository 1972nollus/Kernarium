# ADR 0002: emulator-handle runtime ownership

- Status: accepted
- Date: 2026-08-02

Each `uml_emulator` handle owns its runtime context, engine, stop state, and
copied network callbacks. The callback provider itself remains caller-owned.

A handle admits one run at a time, and so does the process: a run maps its
embedded kernel at fixed host addresses, so runs on different handles
serialize through the process address-space gate. An unbound callback that
cannot identify one active context fails closed.

A process runs one guest, and that guest may restart itself. A kernel claims
guest RAM early in its boot. After a run has claimed it, every later run in the
process, on any handle, returns `UML_EMULATOR_UNSUPPORTED` before it names a
console, maps a kernel window or enters a kernel, except the next run on the
handle whose last run ended with `UML_EMULATOR_RUN_END_RESTART`
(`uml_emulator_get_run_end`): that run hands the guest back to the process and
boots again. A power-off, a host stop and any other end leave the guest spent,
and another handle never takes it. A run that ends before its kernel claims
guest RAM leaves the process's guest unspent. A host that wants another guest
starts another process.

The hand-back refuses with `UML_EMULATOR_RUNTIME_ERROR`, and changes nothing,
while a host service still borrows guest RAM or a kernel thread still runs; the
caller stops its device services and runs again, and the refusal is repeatable
because the handle stays restartable. The check and the hand-back are not one
step: a service that maps guest RAM in between makes the final step fail after
the kernel's resources were given back, and a retry then succeeds. Otherwise it
gives back what the kernel took and did not return (descriptors, pthread keys,
allocations and mappings through the loader's ledger, guest-file records), the
handle's own suspension and hibernation, and the guest RAM: the native window,
which a process reserves once and keeps, becomes one protectionless reservation
again, the RAM commitment is retired and the token unregistered.

Outcome, 2026-10-03: the caller-supplied native kernel entry, and with it
concurrent native-callback handles, is removed. No product set it, and a
process runs one guest, so every run enters the embedded kernel image. Tests
reach a run through the emulator's test-only kernel image instead.

Outcome, 2026-10-04: the process-lifetime compatibility context that answered
callbacks outside any run is removed. A run routes its context before its
kernel can call back, so only white-box tests reached it; they now route a
context of their own, and a callback outside any run fails closed.

Outcome, 2026-10-28: a guest that restarts itself no longer ends the launch.
The library boots it again in the same process, because iOS can neither
re-exec nor respawn, and a respawn would leave the app without the fix. This
is the contract the launchers follow: after a run that ends with a restart,
stop every service that maps guest RAM, then run again on the same handle and
build the services of the new boot. The one-guest-per-process rule remains for
every other end of a guest.
