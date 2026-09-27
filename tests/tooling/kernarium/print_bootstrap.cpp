// SPDX-License-Identifier: GPL-2.0-or-later
// Writes a console bootstrap exactly as Kernarium composes it: the desktop
// bootstrap with an optional mode setup between its two halves, or a measured
// workload over the overlay root with its failure marker and body.
#include "KernariumBootstrap.hpp"

#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char** argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "";
    std::string text;
    if (mode == "desktop" && argc <= 3) {
        std::string modeSetup;
        if (argc == 3) {
            modeSetup = argv[2];
            modeSetup.push_back('\n');
        }
        text = kernarium::bootstrap::desktop(modeSetup);
    } else if (mode == "workload" && argc == 4) {
        std::string body = argv[3];
        body.push_back('\n');
        text = kernarium::bootstrap::overlayWorkload(argv[2], body);
    } else {
        std::fputs("usage: print_bootstrap desktop [mode-setup]\n"
                   "       print_bootstrap workload marker body\n",
            stderr);
        return 2;
    }
    return std::fwrite(text.data(), 1, text.size(), stdout) == text.size() &&
            std::fflush(stdout) == 0
        ? 0
        : 1;
}
