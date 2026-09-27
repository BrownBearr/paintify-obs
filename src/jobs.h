#pragma once
#include "params.h"
#include "pipeline.h"

#include <functional>
#include <string>
#include <vector>

// The video / batch / frame-sequence pipelines, as callable units.
//
// These used to live inline in main(), which was fine while the only caller
// was the CLI. The GUI has to run exactly the same work, so they moved here
// rather than being written twice -- a second copy would drift.
//
// Progress is reported through a callback that can also cancel, which is what
// lets the GUI keep drawing and offer a Cancel button: the callback pumps the
// window between frames and returns false to stop. The CLI's callback just
// prints a line.
namespace jobs {

struct Progress {
    int64_t done = 0;
    int64_t total = 0;          // 0 when the length is not known in advance
    double msPerFrame = 0.0;    // running mean of Pipeline::msTotal()
    uint32_t drawn = 0;         // strokes in the frame just finished
    int width = 0, height = 0;
    std::string item;           // current file, for batch mode
};

// Return false to cancel. Called once per completed frame or image.
using ProgressFn = std::function<bool(const Progress&)>;

struct Result {
    bool ok = false;
    bool cancelled = false;
    int64_t done = 0;
    int64_t skipped = 0;
    double gpuMsMean = 0.0;
    double wallSeconds = 0.0;
    std::string error;          // set when ok is false
};

struct VideoSpec {
    std::string input;
    std::string output;
    double fps = 0.0;           // 0 = take the input's
    int crf = 18;
    std::string codec = "libx264";
    std::string preset = "medium";
    bool keepAudio = true;
};

struct BatchSpec {
    std::vector<std::string> files;   // empty = scan `dir`
    std::string dir;
    std::string outDir;
    std::string suffix = "_painted";
    std::string format = "png";
};

struct FramesSpec {
    std::string dir;
    std::string outDir;
};

// `params` is taken by value inside: each job advances its own frame counter
// and must not disturb the caller's.
Result runVideo(Pipeline& pipe, TuningParams params, const RenderConfig& cfg,
                const VideoSpec& spec, const ProgressFn& onProgress);

Result runBatch(Pipeline& pipe, TuningParams params, const RenderConfig& cfg,
                const BatchSpec& spec, const ProgressFn& onProgress);

Result runFrames(Pipeline& pipe, TuningParams params, const RenderConfig& cfg,
                 const FramesSpec& spec, const ProgressFn& onProgress);

// Writes the canvas in one of png / jpg / bmp / tga. Canvas texel row 0 is
// image row 0, so nothing is flipped.
void writeImage(const std::string& path, const std::vector<unsigned char>& px,
                int w, int h, const std::string& format);

} // namespace jobs
