// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

namespace uml::audio {

class InProcessAudioService final {
public:
    InProcessAudioService();
    ~InProcessAudioService();

    InProcessAudioService(const InProcessAudioService&) = delete;
    InProcessAudioService& operator=(const InProcessAudioService&) = delete;

    [[nodiscard]] std::string takeConnectionName();
    void stop() noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::audio
