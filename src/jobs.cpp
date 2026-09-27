#include "jobs.h"
#include "media.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace jobs {
namespace {

// Renders one already-uploaded source frame and waits for it, so the readback
// and the stats are this frame's rather than the previous one's.
void renderOne(Pipeline& pipe, TuningParams& params, const RenderConfig& cfg,
               bool temporal) {
    pipe.render(params, cfg, temporal);
    params.frame += 1.f;
    glFinish();
    pipe.refreshStats();
}

bool report(const ProgressFn& fn, const Progress& p) {
    return fn ? fn(p) : true;
}

} // namespace

void writeImage(const std::string& path, const std::vector<unsigned char>& px,
                int w, int h, const std::string& format) {
    int ok = 0;
    if (format == "jpg" || format == "jpeg")
        ok = stbi_write_jpg(path.c_str(), w, h, 4, px.data(), 92);
    else if (format == "bmp")
        ok = stbi_write_bmp(path.c_str(), w, h, 4, px.data());
    else if (format == "tga")
        ok = stbi_write_tga(path.c_str(), w, h, 4, px.data());
    else
        ok = stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4);
    if (!ok) fprintf(stderr, "failed to write %s\n", path.c_str());
}

Result runVideo(Pipeline& pipe, TuningParams params, const RenderConfig& cfg,
                const VideoSpec& spec, const ProgressFn& onProgress) {
    Result r;
    const double t0 = glfwGetTime();

    std::string missing;
    if (!media::haveFfmpeg(&missing)) {
        r.error = missing + " is not on PATH; video needs ffmpeg and ffprobe.";
        return r;
    }

    media::VideoInfo info;
    if (!media::probe(spec.input, &info, &r.error)) return r;

    media::Reader reader;
    if (!reader.open(spec.input, info.width, info.height, &r.error)) return r;

    media::EncodeOptions enc;
    enc.fps = (spec.fps > 0.0) ? spec.fps : info.fps;
    enc.crf = spec.crf;
    enc.codec = spec.codec;
    enc.preset = spec.preset;
    if (spec.keepAudio && info.hasAudio) enc.audioFrom = spec.input;

    media::Writer writer;
    if (!writer.open(spec.output, info.width, info.height, enc, &r.error)) return r;

    // Temporal coherence reuses the previous frame's paint where the source
    // did not move. Only meaningful with a threshold set.
    const bool temporal = params.frameDiffThreshold > 0.f;

    pipe.resetTemporal();
    std::vector<unsigned char> frame;
    double totalMs = 0.0;

    while (reader.read(frame)) {
        pipe.setSource(frame.data(), info.width, info.height);
        renderOne(pipe, params, cfg, temporal);

        if (!writer.write(pipe.readCanvas())) {
            r.error = "ffmpeg stopped accepting frames";
            break;
        }
        totalMs += pipe.msTotal();
        ++r.done;

        Progress p;
        p.done = r.done;
        p.total = info.frames;
        p.msPerFrame = totalMs / double(r.done);
        p.drawn = pipe.lastDrawnCount();
        p.width = info.width;
        p.height = info.height;
        if (!report(onProgress, p)) { r.cancelled = true; break; }
    }

    reader.close();
    // Always close the writer, even on cancel: ffmpeg still has to finalise
    // the container or the file is unplayable.
    const bool encoded = writer.close();

    r.wallSeconds = glfwGetTime() - t0;
    r.gpuMsMean = r.done ? totalMs / double(r.done) : 0.0;
    r.ok = encoded && r.error.empty();
    if (!encoded && r.error.empty())
        r.error = "ffmpeg reported a problem writing " + spec.output;
    return r;
}

Result runBatch(Pipeline& pipe, TuningParams params, const RenderConfig& cfg,
                const BatchSpec& spec, const ProgressFn& onProgress) {
    namespace fs = std::filesystem;
    Result r;
    const double t0 = glfwGetTime();

    std::vector<std::string> files = spec.files;
    if (files.empty() && !spec.dir.empty()) files = media::listImages(spec.dir);
    if (files.empty()) {
        r.error = spec.dir.empty() ? "no images selected"
                                   : "no images found in " + spec.dir;
        return r;
    }

    std::error_code ec;
    fs::create_directories(spec.outDir, ec);

    double totalMs = 0.0;
    for (const std::string& f : files) {
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load(f.c_str(), &w, &h, &comp, 4);
        if (!px) {
            fprintf(stderr, "  skip %s (%s)\n", f.c_str(), stbi_failure_reason());
            ++r.skipped;
            continue;
        }
        pipe.setSource(px, w, h);
        stbi_image_free(px);

        // Every image stands alone, so nothing carries over between them.
        pipe.resetTemporal();
        renderOne(pipe, params, cfg, /*temporal=*/false);

        const fs::path out = fs::path(spec.outDir) /
            (fs::path(f).stem().string() + spec.suffix + "." + spec.format);
        writeImage(out.string(), pipe.readCanvas(), pipe.width(), pipe.height(),
                   spec.format);

        totalMs += pipe.msTotal();
        ++r.done;

        Progress p;
        p.done = r.done;
        p.total = int64_t(files.size());
        p.msPerFrame = totalMs / double(r.done);
        p.drawn = pipe.lastDrawnCount();
        p.width = w;
        p.height = h;
        p.item = out.filename().string();
        if (!report(onProgress, p)) { r.cancelled = true; break; }
    }

    r.wallSeconds = glfwGetTime() - t0;
    r.gpuMsMean = r.done ? totalMs / double(r.done) : 0.0;
    r.ok = r.done > 0;
    if (!r.ok && r.error.empty()) r.error = "every image failed to load";
    return r;
}

Result runFrames(Pipeline& pipe, TuningParams params, const RenderConfig& cfg,
                 const FramesSpec& spec, const ProgressFn& onProgress) {
    namespace fs = std::filesystem;
    Result r;
    const double t0 = glfwGetTime();

    const std::vector<std::string> frames = media::listImages(spec.dir);
    if (frames.empty()) {
        r.error = "no frames found in " + spec.dir;
        return r;
    }
    std::error_code ec;
    fs::create_directories(spec.outDir, ec);

    // Temporal coherence needs a threshold to be worth anything; pick the
    // web UI's mid slider value if the caller did not set one.
    if (params.frameDiffThreshold <= 0.f) params.frameDiffThreshold = 12.f;

    pipe.resetTemporal();
    double totalMs = 0.0;

    for (size_t i = 0; i < frames.size(); ++i) {
        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load(frames[i].c_str(), &w, &h, &comp, 4);
        if (!px) { ++r.skipped; continue; }
        pipe.setSource(px, w, h);
        stbi_image_free(px);

        renderOne(pipe, params, cfg, /*temporal=*/true);

        char name[512];
        snprintf(name, sizeof(name), "%s/frame_%05d.png", spec.outDir.c_str(), int(i));
        writeImage(name, pipe.readCanvas(), pipe.width(), pipe.height(), "png");

        totalMs += pipe.msTotal();
        ++r.done;

        Progress p;
        p.done = r.done;
        p.total = int64_t(frames.size());
        p.msPerFrame = totalMs / double(r.done);
        p.drawn = pipe.lastDrawnCount();
        p.width = w;
        p.height = h;
        if (!report(onProgress, p)) { r.cancelled = true; break; }
    }

    r.wallSeconds = glfwGetTime() - t0;
    r.gpuMsMean = r.done ? totalMs / double(r.done) : 0.0;
    r.ok = r.done > 0;
    if (!r.ok && r.error.empty()) r.error = "no frames could be loaded";
    return r;
}

} // namespace jobs
