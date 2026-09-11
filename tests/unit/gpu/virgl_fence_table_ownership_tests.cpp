// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Virglrenderer keeps every registered fence fd in one process-global table,
 * and this address space has two owners of it. The VMM side takes it in
 * virgl_renderer_init() and holds it for the life of the renderer. The
 * same-process render server takes it for as long as it owns contexts, and
 * losing its last context is ordinary: a Venus context whose host device is
 * lost ends its worker. That must not take the table away from the VMM side,
 * whose next fence retire would otherwise search a NULL table and fault.
 *
 * The installed renderer header does not expose that ownership, so this
 * regression exercises the dependency's own fence entry points directly.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>

extern "C" {
int virgl_fence_table_init();
void virgl_fence_table_cleanup();
int virgl_fence_set_fd(std::uint64_t fenceId, int fd);
int virgl_fence_get_fd(std::uint64_t fenceId);
void virgl_fence_retire(std::uint64_t fenceId);
}

namespace {

int failureCount = 0;

void check(bool condition, const char* description)
{
    if (condition)
        return;
    std::fprintf(stderr, "Virgl fence table ownership: %s\n", description);
    ++failureCount;
}

/*
 * A pipe whose write end stays open never becomes readable. The table's
 * opportunistic sweep therefore keeps the entry until an owner drops it,
 * which is how a fence behaves while its work is still outstanding.
 */
class PendingFence {
public:
    PendingFence()
    {
        if (::pipe(descriptors_) != 0) {
            std::perror("pipe");
            std::exit(EXIT_FAILURE);
        }
    }

    ~PendingFence()
    {
        ::close(descriptors_[0]);
        ::close(descriptors_[1]);
    }

    PendingFence(const PendingFence&) = delete;
    PendingFence& operator=(const PendingFence&) = delete;

    [[nodiscard]] int descriptor() const noexcept
    {
        return descriptors_[0];
    }

private:
    int descriptors_[2] {};
};

constexpr std::uint64_t retiredFenceId = 66198;
constexpr std::uint64_t outstandingFenceId = 66199;

} // namespace

int main()
{
    PendingFence retiring;
    PendingFence outstanding;

    // The VMM side takes the table and registers its pending fences.
    check(virgl_fence_table_init() == 0,
        "the renderer owner could not take the fence table");
    check(virgl_fence_set_fd(retiredFenceId, retiring.descriptor()) == 0,
        "the renderer owner could not register a pending fence");
    check(virgl_fence_set_fd(outstandingFenceId, outstanding.descriptor()) == 0,
        "the renderer owner could not register a second pending fence");

    // A same-process render-server context takes its own reference, then
    // gives it back when that context ends.
    check(virgl_fence_table_init() == 0,
        "the render server could not take the fence table");
    virgl_fence_table_cleanup();

    // Retiring a fence the renderer never completed is what both context
    // teardown and stall recovery do once the render server is gone. This is
    // the call that faulted on a table the render server had destroyed.
    virgl_fence_retire(retiredFenceId);
    check(virgl_fence_get_fd(retiredFenceId) < 0,
        "a retired fence stayed in the table");

    const int retained = virgl_fence_get_fd(outstandingFenceId);
    check(retained >= 0,
        "the renderer owner's fence did not survive the render server");
    if (retained >= 0)
        ::close(retained);
    virgl_fence_retire(outstandingFenceId);

    // The last owner releases the table.
    virgl_fence_table_cleanup();

    // A later owner gets a usable table again, with nothing left behind.
    check(virgl_fence_table_init() == 0,
        "the fence table could not be taken again");
    check(virgl_fence_get_fd(outstandingFenceId) < 0,
        "a released fence table kept an entry");
    check(virgl_fence_set_fd(outstandingFenceId, outstanding.descriptor()) == 0,
        "a re-created fence table rejected a fence");
    virgl_fence_table_cleanup();

    if (failureCount != 0)
        return EXIT_FAILURE;

    std::puts("Virgl fence table ownership tests passed");
    return 0;
}
