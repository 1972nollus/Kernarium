// SPDX-License-Identifier: GPL-2.0-or-later

// The host GL provider hands virglrenderer one root context plus a shared
// context per guest context, and one thread drives all of them. A read in one
// context of a texture another context wrote must therefore finish on that
// thread alone: no second thread exists to submit the writing context's work.
// Zink used to block in that case until some other thread flushed, which never
// happens here, and the block took the whole emulator with it -- the vhost-user
// GPU thread parked forever, the guest vCPU that had kicked the virtqueue
// parked with interrupts masked, and the guest reported RCU stalls until it was
// killed.
//
// Model exactly that topology: a writer context records GPU work into a shared
// texture and then parks, still current, with its batch unsubmitted; the reader
// context then reads the texture back. The contract under test is liveness --
// the read returns -- not the pixels, which are undefined precisely because the
// writer's work was never submitted.

#include "host_gl_provider.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

extern "C" {
#include <virglrenderer.h>
}

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <source_location>
#include <thread>
#include <vector>

namespace {

constexpr GLsizei kWidth = 64;
constexpr GLsizei kHeight = 64;

// Zink gives an unflushed foreign batch ten seconds before it continues
// without it. Allow for that plus a slow host; a regression parks forever, so
// any finite bound separates the two outcomes.
constexpr std::chrono::seconds kReadbackDeadline {90};

void require(
    bool condition,
    const std::source_location location = std::source_location::current())
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "require failed at %s:%u\n",
        location.file_name(),
        location.line());
    std::abort();
}

GLuint compile(GLenum stage, const char* source)
{
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        char log[4096] {};
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
        std::abort();
    }
    return shader;
}

class Gate final {
public:
    void signal()
    {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

    void wait()
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool open_ {};
};

// The readback is the operation that used to hang, so it cannot be bounded
// from inside itself. Watch it from another thread and end the process with a
// diagnosis rather than letting the harness kill a silent test.
class Watchdog final {
public:
    explicit Watchdog(std::chrono::seconds deadline)
        : thread_ {[this, deadline] {
            std::unique_lock lock(mutex_);
            if (condition_.wait_for(lock, deadline, [this] { return done_; }))
                return;
            std::fprintf(
                stderr,
                "cross-context readback did not return within %lld s: the "
                "reader is waiting for a context nothing will flush\n",
                static_cast<long long>(deadline.count()));
            std::fflush(stderr);
            std::_Exit(1);
        }}
    {
    }

    ~Watchdog()
    {
        {
            std::lock_guard lock(mutex_);
            done_ = true;
        }
        condition_.notify_all();
        thread_.join();
    }

    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ {};
    std::thread thread_;
};

void readBackWhatAnotherContextHasNotFlushed()
{
    // Run the driver on the calling thread. Gallium's threaded context would
    // otherwise queue the writer's draw and hand it to the driver only when
    // something synchronizes that context, which would decide the outcome
    // here by timing. The contract under test is about who can submit a
    // recorded batch, not about when the record happens.
    ::setenv("GALLIUM_THREAD", "0", 1);

    uml::gpu::HostGlProvider provider;
    if (provider.display() == nullptr) {
        std::fputs("no host GL display; skipping\n", stderr);
        return;
    }

    virgl_renderer_gl_ctx_param parameters {
        .version = 2,
        .shared = false,
        .major_ver = 3,
        .minor_ver = 0,
        .compat_ctx = 0,
    };
    void* writer = provider.createContext(parameters);
    require(writer != nullptr);
    parameters.shared = true;
    void* reader = provider.createContext(parameters);
    require(reader != nullptr);

    std::atomic<GLuint> texture {0};
    Gate recorded;
    Gate release;
    std::thread writerThread([&] {
        require(provider.makeCurrent(writer) == 0);
        GLuint name = 0;
        glGenTextures(1, &name);
        glBindTexture(GL_TEXTURE_2D, name);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, kWidth, kHeight);
        GLuint target = 0;
        glGenFramebuffers(1, &target);
        glBindFramebuffer(GL_FRAMEBUFFER, target);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
        require(
            glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE);

        const GLuint program = glCreateProgram();
        glAttachShader(
            program,
            compile(
                GL_VERTEX_SHADER,
                "#version 460 core\n"
                "void main(void) {\n"
                "   vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
                "   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
                "}\n"));
        glAttachShader(
            program,
            compile(
                GL_FRAGMENT_SHADER,
                "#version 460 core\n"
                "out vec4 colour;\n"
                "void main(void) {\n"
                "   colour = vec4(0.35, 0.70, 0.15, 1.0);\n"
                "}\n"));
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        require(linked != 0);
        glUseProgram(program);

        GLuint array = 0;
        glGenVertexArrays(1, &array);
        glBindVertexArray(array);
        glViewport(0, 0, kWidth, kHeight);
        // A draw is real GPU work: it opens a render pass on this context's
        // batch and records the texture's last write there. Only this thread
        // can submit that batch.
        glDrawArrays(GL_TRIANGLES, 0, 3);
        require(glGetError() == GL_NO_ERROR);

        texture.store(name, std::memory_order_release);
        // Deliberately no glFlush and no unbind: park holding the batch, the
        // way a virglrenderer guest context does between command buffers.
        recorded.signal();
        release.wait();
        glDeleteVertexArrays(1, &array);
        glDeleteProgram(program);
        glDeleteFramebuffers(1, &target);
        require(provider.makeCurrent(nullptr) == 0);
    });

    recorded.wait();
    require(provider.makeCurrent(reader) == 0);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        texture.load(std::memory_order_acquire),
        0);
    require(
        glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
        GL_FRAMEBUFFER_COMPLETE);

    std::vector<std::uint8_t> readback(
        static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
    {
        Watchdog watchdog {kReadbackDeadline};
        glReadPixels(
            0,
            0,
            kWidth,
            kHeight,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            readback.data());
        require(glGetError() == GL_NO_ERROR);
    }

    glDeleteFramebuffers(1, &framebuffer);
    require(provider.makeCurrent(nullptr) == 0);

    release.signal();
    writerThread.join();

    require(provider.makeCurrent(writer) == 0);
    GLuint name = texture.load(std::memory_order_acquire);
    glDeleteTextures(1, &name);
    require(provider.makeCurrent(nullptr) == 0);
    provider.destroyContext(reader);
    provider.destroyContext(writer);
}

} // namespace

int main()
{
    readBackWhatAnotherContextHasNotFlushed();
    return 0;
}
