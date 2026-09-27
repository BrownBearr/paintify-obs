#include "live_spout.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <SpoutGL/Spout.h>

int runLiveSpout(GLFWwindow* window, Pipeline& pipe, TuningParams params,
                 const RenderConfig& render, const LiveSpoutConfig& live) {
    Spout receiver;
    Spout sender;
    HANDLE parentProcess = live.parentPid ?
        OpenProcess(SYNCHRONIZE, FALSE, live.parentPid) : nullptr;
    receiver.SetReceiverName(live.inputName.c_str());
    sender.SetSenderName(live.outputName.c_str());

    GLuint inputTexture = 0;
    int width = 0, height = 0;
    bool havePainting = false;
    bool announced = false;
    bool everConnected = false;
    unsigned long long paintedFrames = 0;
    auto lastConnected = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / std::max(1.0, live.fps)));
    auto nextFrame = std::chrono::steady_clock::now();
    std::printf("Paintify live: %s -> %s, %.1f fps\n",
                live.inputName.c_str(), live.outputName.c_str(), live.fps);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (parentProcess && WaitForSingleObject(parentProcess, 0) == WAIT_OBJECT_0)
            break;
        if (!live.stopFile.empty() && std::filesystem::exists(live.stopFile))
            break;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextFrame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        // Advance from the previous deadline so scheduling jitter does not
        // accumulate and quietly turn a 12 fps request into 11 fps.
        nextFrame += interval;
        if (nextFrame < now) nextFrame = now;

        // A null destination queries the sender first. On first connection or
        // resize, Spout requires us to allocate an RGBA texture and try again.
        if (!inputTexture) {
            if (!receiver.ReceiveTexture()) {
                // The host filter may have been removed. Avoid leaving a
                // hidden renderer process running after its sender vanishes.
                if (everConnected && now - lastConnected > std::chrono::seconds(5))
                    break;
                if (!announced) {
                    std::printf("Waiting for Spout sender '%s'...\n", live.inputName.c_str());
                    announced = true;
                }
                continue;
            }
            announced = false;
            everConnected = true;
            lastConnected = now;
            width = int(receiver.GetSenderWidth());
            height = int(receiver.GetSenderHeight());
            if (width <= 0 || height <= 0) continue;
            glCreateTextures(GL_TEXTURE_2D, 1, &inputTexture);
            glTextureStorage2D(inputTexture, 1, GL_RGBA8, width, height);
            receiver.IsUpdated(); // reset Spout's resize flag
            havePainting = false;
            pipe.resetTemporal();
        }

        if (!receiver.ReceiveTexture(inputTexture, GL_TEXTURE_2D)) {
            glDeleteTextures(1, &inputTexture);
            inputTexture = 0;
            havePainting = false;
            sender.ReleaseSender();
            continue;
        }
        lastConnected = now;
        if (receiver.IsUpdated()) {
            glDeleteTextures(1, &inputTexture);
            inputTexture = 0;
            havePainting = false;
            sender.ReleaseSender();
            continue;
        }
        if (!receiver.IsFrameNew()) continue;

        if (!pipe.setSourceTexture(inputTexture, width, height)) continue;
        pipe.render(params, render, havePainting && params.frameDiffThreshold > 0.f);
        havePainting = true;
        params.frame += 1.f;
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        if (!sender.SendTexture(pipe.canvasTexture(), GL_TEXTURE_2D,
                                unsigned(width), unsigned(height))) {
            std::fprintf(stderr, "Spout failed to publish painted frame\n");
        } else if (++paintedFrames % 12 == 0) {
            std::printf("Paintify live: %llu frames, %dx%d, GPU %.1f ms\n",
                        paintedFrames, width, height, pipe.msTotal());
            std::fflush(stdout);
        }
    }

    if (inputTexture) glDeleteTextures(1, &inputTexture);
    receiver.ReleaseReceiver();
    sender.ReleaseSender();
    if (parentProcess) CloseHandle(parentProcess);
    return 0;
}
#else
int runLiveSpout(GLFWwindow*, Pipeline&, TuningParams,
                 const RenderConfig&, const LiveSpoutConfig&) {
    std::fprintf(stderr, "Live Spout mode is available on Windows only\n");
    return 1;
}
#endif
