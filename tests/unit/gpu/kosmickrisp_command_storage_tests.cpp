// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using uml::testing::require;
using uml::gpu::test::DrawConfig;
using uml::gpu::test::DrawContext;
using uml::gpu::test::SubmitStormConfig;

/* A virtualized GL or Vulkan stack ends every guest command buffer in a
 * submission, so the cost of one submission is the cost of its frames. On a
 * Metal backend that cost is dominated by the command storage an encoder
 * records into: a backend that gives that storage back to the kernel when the
 * GPU finishes with it buys it again on the next submission, and both halves
 * are kernel calls. The bound below is deliberately loose - it is a guard
 * against that regression returning, not a benchmark - but it is far under
 * the 0.5 ms per submission the released-per-submission behaviour costs on an
 * M1 Ultra, and far above the ~0.05 ms the recycled path measures there. */
constexpr double kMaxMicrosecondsPerSubmission = 150.0;
constexpr uint32_t kSubmissions = 240;
/* More live recordings than the driver's cache can hold, so the path where it
 * runs out and creates storage is exercised in the same run. */
constexpr uint32_t kOutstanding = 32;
/* The fastest of several rounds: the first pays for warm-up, and a loaded
 * host inflates any single round. Measured on an M1 Ultra: 86-96 us per
 * submission with the storage recycled, 186-244 us when every submission
 * returns its storage to the kernel and buys it again. */
constexpr unsigned kRounds = 3;

} // namespace

int main()
{
    try {
        double microseconds = 0.0;
        {
            DrawContext context;

            SubmitStormConfig storm;
            storm.submissions = kSubmissions;
            storm.outstanding = kOutstanding;
            for (unsigned round = 0; round < kRounds; ++round) {
                const auto result = context.submit_storm(storm);

                require(result.values.size() == kSubmissions,
                        "submit storm returned the wrong number of slots");
                for (uint32_t index = 0; index < kSubmissions; ++index) {
                    require(result.values[index] == index + 1u,
                            "submission " + std::to_string(index) + " wrote " +
                                std::to_string(result.values[index]) + ", expected " +
                                std::to_string(index + 1u));
                }

                const double each = result.wall_nanoseconds / 1000.0 / double(kSubmissions);
                if (round == 0 || each < microseconds)
                    microseconds = each;
            }
            std::printf("PASS command storage: %u one-time submissions, %u outstanding, "
                        "%u rounds, %.1f us each\n", kSubmissions, kOutstanding,
                        kRounds, microseconds);
            std::fflush(stdout);
            require(microseconds <= kMaxMicrosecondsPerSubmission,
                    "a submission costs " + std::to_string(microseconds) +
                        " us, over the " + std::to_string(kMaxMicrosecondsPerSubmission) +
                        " us bound: command storage is not being reused");
        }

        /* Destroying the device with submissions still in flight: the backend
         * may only complete them through state that outlives the device, and
         * anything it recycles has to survive that. */
        {
            DrawContext context;
            SubmitStormConfig abandoned;
            abandoned.submissions = kOutstanding * 2;
            abandoned.outstanding = kOutstanding;
            abandoned.abandon = true;
            const auto result = context.submit_storm(abandoned);
            require(result.submissions == kOutstanding * 2,
                    "abandoned storm reported the wrong submission count");
        }
        std::puts("PASS command storage: device destroyed under outstanding submissions");

        /* A device created after that teardown must still record and execute,
         * which is what a recycled allocator handed back by a late completion
         * would break. */
        {
            DrawContext context;
            SubmitStormConfig again;
            again.submissions = kOutstanding;
            again.outstanding = kOutstanding;
            const auto result = context.submit_storm(again);
            for (uint32_t index = 0; index < kOutstanding; ++index) {
                require(result.values[index] == index + 1u,
                        "post-teardown submission " + std::to_string(index) +
                            " did not execute");
            }
        }
        std::puts("PASS command storage: recording after a teardown still executes");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL command storage: %s\n", error.what());
        return 1;
    }
}
