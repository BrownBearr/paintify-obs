#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <SpoutGL/Spout.h>

#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* window = glfwCreateWindow(64, 64, "Spout smoke", nullptr, nullptr);
    if (!window) return 2;
    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) return 3;

    const bool full = argc > 1 && std::strcmp(argv[1], "--full") == 0;
    const int W = full ? 1920 : 640;
    const int H = full ? 1080 : 360;
    const int target = full ? 48 : 3;
    std::vector<unsigned char> pixels(size_t(W) * H * 4);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const size_t p = (size_t(y) * W + x) * 4;
        pixels[p] = static_cast<unsigned char>(x * 255 / W);
        pixels[p+1] = static_cast<unsigned char>(y * 255 / H);
        pixels[p+2] = ((x/32 + y/32) % 2) ? 220 : 40;
        pixels[p+3] = 255;
    }
    GLuint source = 0, output = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &source);
    glTextureStorage2D(source, 1, GL_RGBA8, W, H);
    glTextureSubImage2D(source, 0, 0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    Spout sender, receiver;
    sender.SetSenderName("Paintify Smoke Input");
    receiver.SetReceiverName("Paintify Smoke Output");
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    const std::string renderer =
        (std::filesystem::path(modulePath).parent_path() / "gpu-sbr.exe").string();
    std::string cmd = "\"" + renderer + "\" --live-spout --spout-in \"Paintify Smoke Input\" "
        "--spout-out \"Paintify Smoke Output\" --target-fps 12";
    if (full) cmd += " --relax 4";
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
        std::fprintf(stderr, "could not launch renderer: %lu\n", GetLastError());
        return 4;
    }

    int received = 0;
    unsigned char lastSample[4] = {};
    bool haveSample = false;
    int sourceStep = 0;
    std::chrono::steady_clock::time_point firstPainted, lastPainted;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && received < target) {
        glfwPollEvents();
        const unsigned char swatch[4] = {
            static_cast<unsigned char>((sourceStep++ * 5) % 256), 60, 210, 255};
        glClearTexSubImage(source, 0, W/2, 0, 0, W/2, H, 1,
                           GL_RGBA, GL_UNSIGNED_BYTE, swatch);
        sender.SendTexture(source, GL_TEXTURE_2D, W, H);
        if (!output) {
            if (receiver.ReceiveTexture() && receiver.GetSenderWidth() == W &&
                receiver.GetSenderHeight() == H) {
                glCreateTextures(GL_TEXTURE_2D, 1, &output);
                glTextureStorage2D(output, 1, GL_RGBA8, W, H);
                receiver.IsUpdated();
            }
        } else if (receiver.ReceiveTexture(output, GL_TEXTURE_2D)) {
            if (receiver.IsUpdated()) {
                glDeleteTextures(1, &output);
                output = 0;
            } else if (receiver.IsFrameNew()) {
                unsigned char sample[4] = {};
                glGetTextureSubImage(output, 0, 3*W/4, H/2, 0, 1, 1, 1,
                                     GL_RGBA, GL_UNSIGNED_BYTE, 4, sample);
                if (!haveSample || std::memcmp(sample, lastSample, 4) != 0) {
                    std::memcpy(lastSample, sample, 4);
                    haveSample = true;
                    if (received == 0) firstPainted = std::chrono::steady_clock::now();
                    lastPainted = std::chrono::steady_clock::now();
                    ++received;
                }
            }
        }
        // Feed faster than the painter's 12 fps cap, as a 30 fps camera would.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    TerminateProcess(child.hProcess, 0);
    WaitForSingleObject(child.hProcess, 2000);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    sender.ReleaseSender();
    receiver.ReleaseReceiver();
    if (source) glDeleteTextures(1, &source);
    if (output) glDeleteTextures(1, &output);
    glfwDestroyWindow(window);
    glfwTerminate();
    const double elapsed = received > 1 ?
        std::chrono::duration<double>(lastPainted - firstPainted).count() : 0.0;
    std::printf("Received %d painted Spout frames (%.1f fps after startup)\n",
                received, elapsed > 0.0 ? (received - 1) / elapsed : 0.0);
    return received >= target ? 0 : 5;
}
