#pragma once
#include <glad/glad.h>
#include <string>
#include <vector>
#include <cstdint>

namespace glu {

// Reads a shader from shaders/, splicing any `#include "x.glsl"` lines and
// emitting #line directives so compiler errors still point at the real file.
std::string loadShaderSource(const std::string& path, std::string* err);

// A shader program that remembers where it came from, so F5 can rebuild it.
struct Program {
    GLuint id = 0;
    std::vector<std::string> paths;   // compute: 1 path; raster: vert, frag
    std::vector<int64_t> mtimes;
    std::string lastError;

    bool loadCompute(const std::string& comp);
    bool loadRaster(const std::string& vert, const std::string& frag);
    bool reloadIfChanged();           // true if it rebuilt successfully
    bool sourcesChanged() const;
    void use() const { glUseProgram(id); }
    GLint uniform(const char* name) const { return glGetUniformLocation(id, name); }

    bool build();
};

// GL_TIME_ELAPSED query, double-buffered so reading it never stalls the CPU.
struct GpuTimer {
    GLuint q[2] = {0, 0};
    int cur = 0;
    bool started = false;
    bool issued[2] = {false, false};
    double lastMs = 0.0;

    void init();
    void begin();
    void end();
    double ms() const { return lastMs; }
};

GLuint createTexture2D(int w, int h, GLenum internalFormat, bool mips = false);
GLuint createBuffer(GLenum target, GLsizeiptr bytes, const void* data, GLenum usage);
void   enableDebugOutput();

} // namespace glu
