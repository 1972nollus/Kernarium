// SPDX-License-Identifier: GPL-2.0-or-later
// Writes the desktop console bootstrap exactly as Kernarium composes it, with
// an optional mode setup (the first argument) between its two halves.
#include "KernariumBootstrap.hpp"

#include <cstdio>
#include <string>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc > 2) {
        std::fputs("usage: print_desktop_bootstrap [mode-setup]\n", stderr);
        return 2;
    }
    std::string modeSetup;
    if (argc == 2) {
        modeSetup = argv[1];
        modeSetup.push_back('\n');
    }
    const std::string text = kernarium::bootstrap::desktop(modeSetup);
    return std::fwrite(text.data(), 1, text.size(), stdout) == text.size() &&
            std::fflush(stdout) == 0
        ? 0
        : 1;
}
