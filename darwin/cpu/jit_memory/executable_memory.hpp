// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

/* The only first-party owner of executable memory. It maps JIT memory, opens
 * and closes the calling thread's write window over it, and publishes the
 * instructions written there. JIT engines reach executable memory only
 * through these calls; the gadget engines and shared CPU code never link
 * them. allocate, release and set_writable have the shapes of FEXCore's
 * executable-memory hooks.
 */
namespace uml::cpu::jit_memory {

/* Whether this process can run JIT code: a publication route exists and a JIT
 * mapping succeeds. The answer is probed once per process.
 */
[[nodiscard]] bool available() noexcept;

/* Maps size bytes of JIT memory, readable, writable and executable, or returns
 * nullptr. A thread writes it only inside its write window.
 */
[[nodiscard]] void *allocate(std::size_t size) noexcept;

/* The calls below forward to the C library, which the compiler cannot prove
 * nothrow. They are not noexcept, so each compiles to a tail call rather than
 * a frame with its own terminate handler.
 */
void release(void *mapping, std::size_t size);

/* Opens (true) or closes (false) the calling thread's write window over every
 * JIT mapping. While the window is open, the thread cannot execute JIT code.
 */
void set_writable(bool writable);

/* Makes size bytes of instructions written at code visible to instruction
 * fetch. Call it after closing the write window.
 */
void invalidate_instruction_cache(void *code, std::size_t size);

} // namespace uml::cpu::jit_memory
