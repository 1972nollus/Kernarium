# ADR 0002: emulator-handle runtime ownership

- Status: accepted
- Date: 2026-08-02

Each `uml_emulator` handle owns its runtime context, engine, stop state, and
copied network callbacks. The callback provider itself remains caller-owned.

A handle admits one run at a time. Independent 16 KiB native-callback handles
may run concurrently when their mappings do not overlap. Embedded and 4 KiB
native runs serialize through the process address-space gate. An unbound
callback that cannot identify one active context fails closed.
