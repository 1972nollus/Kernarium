# x86-64 gadget engine

The engine executes x86-64 userspace as data-only streams of pointers to
handwritten arm64 gadgets (`engine.S` and `engine/*.inc`). `native_builder.cpp`
turns decoded instructions into those streams, `executor.cpp` owns the
translation cache, and `native_memory.cpp` is the C++ side of the software
TLB. This file records the contracts that are not obvious from the code.

## Code pages

`code_page_registry.hpp` keeps, per address-space generation, the set of pages
in which any executor of that generation holds a cached translation. A store
publishes a code change only when its page is in that set, which is what
makes publication precise under READ_IMPLIES_EXEC, where every readable page
is executable. The registry guarantees:

- a page enters the set before any translation built from it is handed out
  (`CodePageHold` covers the build), and leaves it when the last cached
  translation of the page in every executor is unlinked or invalidated;
- when a page enters the set, every participating executor's write-side TLB
  entry for it is retired before the registration returns, and an executor
  inside the engine is waited for until it leaves or passes a checkpoint,
  bounded at 200 ms, so no executor keeps a stale "publishes nothing" entry;
- a TLB fill that raced a registration retries: the fill re-reads the
  registry's change counter after publishing its entry.

The Wine ARM64EC CPU service, whose arm64 translations live outside the
registry, publishes on every executable store instead
(`Executor::publish_every_executable_store`).
