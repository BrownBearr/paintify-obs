#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include "pipeline.h"
#include "params.h"
#include "brush_atlas.h"
#include "media.h"
#include "jobs.h"
#include "filedialog.h"
#include "paramfile.h"
#include "live_spout.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// ── Presets ────────────────────────────────────────────────────────────────
// The Hertzmann presets from PainterlyImageCreatorWeb's main.js PRESETS, value
// for value, so a look chosen in the web UI can be reproduced here by name.
// Fields the web preset leaves unset fall back to its PRESET_DEFAULTS, which is
// why several have brushTexture 0. `detail` is the one entry not from the web.
struct Preset {
    const char* name;
    const char* radii;
    float maxLen, minLen, curvature, threshold, gridFactor, opacity;
    float hue, sat, val;
    float brushTexture;
    Underpaint underpaint;
};

const Preset kPresets[] = {
    // name           radii       maxL minL curv  thr   grid   op     hue    sat    val    tex    underpaint
    {"impressionist", "8,4,2",    16,  4,   1.0f, 50.f, 1.0f,  0.90f, 0.05f, 0.10f, 0.10f, 0.45f, Underpaint::Blur},
    {"expressionist", "12,6,3",   28,  8,   1.0f, 40.f, 0.9f,  0.95f, 0.15f, 0.20f, 0.15f, 0.60f, Underpaint::Blur},
    // 'average' matters here: with a blur underpaint the radius-4 layer paints
    // the colour that is already on the canvas and the dabs never show.
    {"pointillist",   "4,2",       3,  1,   0.0f, 30.f, 0.75f, 0.85f, 0.10f, 0.15f, 0.10f, 0.00f, Underpaint::Average},
    {"wash",          "20,10",    32, 10,   0.7f, 80.f, 1.5f,  0.40f, 0.20f, 0.30f, 0.20f, 0.00f, Underpaint::Blur},
    // gpu-sbr's own: a tight threshold and a fine finest layer, for detail.
    {"detail",        "6,3,1.5",  10,  4,   1.0f, 20.f, 1.0f,  1.00f, 0.00f, 0.00f, 0.00f, 0.50f, Underpaint::Blur},
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// parseRadii / radiiToString live in paramfile.cpp: the GUI text box and a
// saved file have to agree on what "8, 4, 2" means, so there is one copy.
using paramfile::parseRadii;
using paramfile::radiiToString;

void applyPreset(const Preset& pr, TuningParams& p, RenderConfig& cfg, std::string* radiiText) {
    cfg.radii = parseRadii(pr.radii);
    cfg.underpaint = pr.underpaint;
    if (radiiText) *radiiText = radiiToString(cfg.radii);
    p.maxStrokeLength = pr.maxLen;
    p.minStrokeLength = pr.minLen;
    p.curvature = pr.curvature;
    p.threshold = pr.threshold;
    p.gridFactor = pr.gridFactor;
    p.opacity = pr.opacity;
    p.jitterHue = pr.hue;
    p.jitterSat = pr.sat;
    p.jitterVal = pr.val;
    p.texStrength = pr.brushTexture;
}

struct Options {
    bool headless = false;
    bool liveSpout = false;
    std::string spoutIn = "Paintify Input";
    std::string spoutOut = "Paintify Output";
    std::string liveStopFile;
    uint32_t liveParentPid = 0;
    double targetFps = 12.0;
    std::string in;
    std::string out = "out.png";
    int dump = 0;
    bool debugCells = false;
    int passes = 0;
    std::string framesDir;
    std::string batchDir;
    std::string videoIn;
    std::string outDir = "frames_out";
    double fps = 0.0;          // 0 = take it from the input
    int crf = 18;
    std::string vcodec = "libx264";
    std::string vpreset = "medium";
    bool noAudio = false;
    std::string batchSuffix = "_painted";
    std::string batchFormat = "png";
    std::string preset;
    std::string paramsFile;
    std::string saveParamsFile;
    std::string radii;
    bool haveUnderpaint = false;
    Underpaint underpaint = Underpaint::Blur;
    // Overrides; NaN means "leave the preset's value alone".
    float threshold = NAN, curvature = NAN, opacity = NAN, gridFactor = NAN;
    float maxLen = NAN, minLen = NAN, tensorSigma = NAN;
    float brushTexture = NAN, bristleDensity = NAN, textureTaper = NAN;
    float impasto = NAN, impastoLight = NAN, lightAngle = NAN, dryBrush = NAN;
    float sizeJitter = NAN, angleJitter = NAN, opacityJitter = NAN;
    float temporalDiff = NAN;
    int relax = -1, relaxSubs = 0;
    int etf = -1;
    int flow = -1, flowIters = 0;
    float etfRadius = NAN;
    float relaxArea = NAN, relaxMove = NAN, relaxCands = NAN, relaxRemove = NAN;
    bool relaxLog = false;
    bool etfLog = false;
    bool flowLog = false;
    bool jitterPerFrame = false;
};

void usage() {
    printf(
        "gpu-sbr -- GPU port of PainterlyImageCreatorWeb's Hertzmann renderer\n\n"
        "  --in <path>            source image (omitted: synthetic subject)\n"
        "  --out <path>            output PNG for --headless and the S key\n"
        "  --headless              render and exit, no window\n"
        "  --live-spout            paint live Spout input and publish Spout output\n"
        "  --spout-in <name>       live input sender name (Paintify Input)\n"
        "  --spout-out <name>      live output sender name (Paintify Output)\n"
        "  --target-fps <n>        live painted frames per second (12)\n"
        "  --live-stop-file <path> exit live mode when this file appears\n"
        "  --live-parent-pid <n>  exit if the owning process closes\n"
        "  --preset <name>         impressionist | expressionist | pointillist |\n"
        "                          wash   (the web's, value for value)\n"
        "                          detail (this project's own)\n"
        "  --save-params <file>    write the fully resolved parameters and carry\n"
        "                          on, so a preset plus a few flags becomes an\n"
        "                          editable file to tweak by hand\n"
        "  --params <file>         load a parameter file saved from the GUI.\n"
        "                          Applied after --preset, before the individual\n"
        "                          overrides below, so a flag still wins.\n"
        "  --radii \"8,4,2\"         brush radii, coarse to fine (max 8)\n"
        "  --threshold <f>         Lab error a cell must exceed to earn a stroke\n"
        "  --curvature <0..1>      Hertzmann's fc filter\n"
        "  --opacity <0..1>        per-stroke alpha\n"
        "  --grid-factor <f>       seed grid pitch, in radii\n"
        "  --max-len / --min-len   stroke length bounds, in steps\n"
        "  --tensor-sigma <f>      > 0 swaps in the structure-tensor flow field\n"
        "  --etf <n>               Edge Tangent Flow iterations over the flow\n"
        "                          field (Kang 2007). Edge-aware smoothing: weak\n"
        "                          pixels defer to strong neighbours and nothing\n"
        "                          is smoothed across a perpendicular flow, which\n"
        "                          a Gaussian cannot express. 0 (default) leaves\n"
        "                          the field exactly as the web version has it;\n"
        "                          2-3 converges.\n"
        "  --etf-radius <f>        ETF neighbourhood radius in px (default 5)\n"
        "  --brush-texture <0..1>  bristle texture strength\n"
        "  --bristle-density <f>   bristles across the stroke (default 10)\n"
        "  --texture-taper <0..1>  how much the stroke tips narrow\n"
        "  --impasto <f>           paint height accumulated per stroke\n"
        "  --impasto-light <f>     relief lighting strength\n"
        "  --light-angle <deg>     relief light direction\n"
        "  --dry-brush <0..1>      opacity falloff along the stroke\n"
        "  --size/--angle/--opacity-jitter <f>   per-stroke variation\n"
        "  --underpaint <mode>     blur | none | average\n"
        "\n Video and batch\n"
        "  --video <in> --out <out.mp4>    paint a video; frames stream through\n"
        "                          ffmpeg as raw RGBA, nothing hits the disk.\n"
        "                          Audio is copied from the input when present.\n"
        "  --batch <dir> --outdir <dir>    paint every image in a directory\n"
        "  --frames <dir> --outdir <dir>   image sequence, temporal coherence on\n"
        "  --fps <f>               output frame rate (default: the input's)\n"
        "  --crf <n>               x264 quality, lower is better (default 18)\n"
        "  --vcodec / --vpreset    encoder and speed preset (libx264, medium)\n"
        "  --no-audio              drop the input's audio track\n"
        "  --suffix <s>            batch output suffix (default _painted)\n"
        "  --format <ext>          batch output format: png | jpg | bmp | tga\n"
        "  --temporal-diff <f>     per-cell source change needed to repaint\n"
        "                          (0..255). 0 paints every frame from scratch,\n"
        "                          which makes strokes shimmer; 12 is a good\n"
        "                          start for video.\n"
        "  --flow <n>              pyramidal Lucas-Kanade levels for temporal\n"
        "                          advection. Carries the painting along the\n"
        "                          motion instead of leaving it pinned to the\n"
        "                          pixel grid, which is what --temporal-diff\n"
        "                          alone cannot fix on a moving camera. 0 (the\n"
        "                          default) is the web version's behaviour; 4\n"
        "                          tracks roughly 32 px/frame. Needs\n"
        "                          --temporal-diff > 0 to do anything.\n"
        "  --flow-iters <n>        LK refinements per level (default 3)\n"
        "  --passes <n>            painting passes per layer (default 8). The web\n"
        "                          version paints one stroke at a time and each\n"
        "                          stroke sees the last one's paint, which is what\n"
        "                          sets stroke length; more passes approximate it\n"
        "                          more closely. 8 lands within 1%%.\n"
        "  --debug-cells           print each layer's error distribution\n"
        "  --dump <n>              print the first n traced polylines\n"
        "\n"
        " Relaxation (Hertzmann 2001, \"Paint By Relaxation\"). After the greedy\n"
        " pass places the strokes, iteratively improve them against\n"
        "   E = sum|Lab(canvas)-Lab(source)| + area-weight * sum(stroke area)\n"
        "  --relax <n>             iterations; 0 (default) = plain Hertzmann 1998\n"
        "  --relax-area <f>        w_area: higher = fewer, larger, bolder strokes\n"
        "  --relax-move <f>        perturbation amplitude, in stroke radii\n"
        "  --relax-candidates <n>  trial moves per stroke per iteration\n"
        "  --relax-remove <f>      > 0 deletes strokes that do not pay for their\n"
        "                          area; 1.0 is break-even\n"
        "  --relax-subpasses <n>   hash-scattered subsets per iteration\n"
        "  --relax-log             print the global energy after each iteration\n");
}

Options parseArgs(int argc, char** argv) {
    Options o;
    auto next = [&](int& i) -> const char* { return (i + 1 < argc) ? argv[++i] : ""; };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--headless")        o.headless = true;
        else if (a == "--live-spout")      o.liveSpout = true;
        else if (a == "--spout-in")        o.spoutIn = next(i);
        else if (a == "--spout-out")       o.spoutOut = next(i);
        else if (a == "--target-fps")      o.targetFps = atof(next(i));
        else if (a == "--live-stop-file")   o.liveStopFile = next(i);
        else if (a == "--live-parent-pid")   o.liveParentPid = uint32_t(std::strtoul(next(i), nullptr, 10));
        else if (a == "--in")              o.in = next(i);
        else if (a == "--out")             o.out = next(i);
        else if (a == "--dump")            o.dump = atoi(next(i));
        else if (a == "--debug-cells")     o.debugCells = true;
        else if (a == "--passes")          o.passes = atoi(next(i));
        else if (a == "--relax")           o.relax = atoi(next(i));
        else if (a == "--etf")             o.etf = atoi(next(i));
        else if (a == "--flow")            o.flow = atoi(next(i));
        else if (a == "--flow-iters")      o.flowIters = atoi(next(i));
        else if (a == "--etf-radius")      o.etfRadius = std::strtof(next(i), nullptr);
        else if (a == "--relax-subpasses") o.relaxSubs = atoi(next(i));
        else if (a == "--relax-area")      o.relaxArea = std::strtof(next(i), nullptr);
        else if (a == "--relax-move")      o.relaxMove = std::strtof(next(i), nullptr);
        else if (a == "--relax-candidates") o.relaxCands = std::strtof(next(i), nullptr);
        else if (a == "--relax-remove")    o.relaxRemove = std::strtof(next(i), nullptr);
        else if (a == "--relax-log")       o.relaxLog = true;
        else if (a == "--etf-log")         o.etfLog = true;
        else if (a == "--flow-log")        o.flowLog = true;
        else if (a == "--jitter-per-frame") o.jitterPerFrame = true;
        else if (a == "--frames")        { o.framesDir = next(i); o.headless = true; }
        else if (a == "--video")         { o.videoIn = next(i); o.headless = true; }
        else if (a == "--batch")         { o.batchDir = next(i); o.headless = true; }
        else if (a == "--fps")             o.fps = atof(next(i));
        else if (a == "--crf")             o.crf = atoi(next(i));
        else if (a == "--vcodec")          o.vcodec = next(i);
        else if (a == "--vpreset")         o.vpreset = next(i);
        else if (a == "--no-audio")        o.noAudio = true;
        else if (a == "--suffix")          o.batchSuffix = next(i);
        else if (a == "--format")          o.batchFormat = next(i);
        else if (a == "--outdir")          o.outDir = next(i);
        else if (a == "--preset")          o.preset = next(i);
        else if (a == "--params")          o.paramsFile = next(i);
        else if (a == "--save-params")     o.saveParamsFile = next(i);
        else if (a == "--radii")           o.radii = next(i);
        else if (a == "--threshold")       o.threshold = std::strtof(next(i), nullptr);
        else if (a == "--curvature")       o.curvature = std::strtof(next(i), nullptr);
        else if (a == "--opacity")         o.opacity = std::strtof(next(i), nullptr);
        else if (a == "--grid-factor")     o.gridFactor = std::strtof(next(i), nullptr);
        else if (a == "--max-len")         o.maxLen = std::strtof(next(i), nullptr);
        else if (a == "--min-len")         o.minLen = std::strtof(next(i), nullptr);
        else if (a == "--tensor-sigma")    o.tensorSigma = std::strtof(next(i), nullptr);
        else if (a == "--brush-texture")   o.brushTexture = std::strtof(next(i), nullptr);
        else if (a == "--bristle-density") o.bristleDensity = std::strtof(next(i), nullptr);
        else if (a == "--texture-taper")   o.textureTaper = std::strtof(next(i), nullptr);
        else if (a == "--impasto")         o.impasto = std::strtof(next(i), nullptr);
        else if (a == "--impasto-light")   o.impastoLight = std::strtof(next(i), nullptr);
        else if (a == "--light-angle")     o.lightAngle = std::strtof(next(i), nullptr);
        else if (a == "--dry-brush")       o.dryBrush = std::strtof(next(i), nullptr);
        else if (a == "--size-jitter")     o.sizeJitter = std::strtof(next(i), nullptr);
        else if (a == "--angle-jitter")    o.angleJitter = std::strtof(next(i), nullptr);
        else if (a == "--opacity-jitter")  o.opacityJitter = std::strtof(next(i), nullptr);
        else if (a == "--temporal-diff")   o.temporalDiff = std::strtof(next(i), nullptr);
        else if (a == "--underpaint") {
            const std::string m = next(i);
            o.haveUnderpaint = true;
            o.underpaint = (m == "none") ? Underpaint::None
                         : (m == "average") ? Underpaint::Average : Underpaint::Blur;
        }
        else if (a == "--no-underpaint") { o.haveUnderpaint = true; o.underpaint = Underpaint::None; }
        else if (a == "--help" || a == "-h") { usage(); exit(0); }
        else fprintf(stderr, "unknown option %s (try --help)\n", a.c_str());
    }
    return o;
}

void applyOverrides(const Options& o, TuningParams& p, RenderConfig& cfg,
                    std::string* radiiText) {
    auto set = [](float& dst, float v) { if (!std::isnan(v)) dst = v; };
    if (!o.radii.empty()) {
        cfg.radii = parseRadii(o.radii);
        if (radiiText) *radiiText = radiiToString(cfg.radii);
    }
    if (o.haveUnderpaint) cfg.underpaint = o.underpaint;
    if (o.passes > 0) cfg.passesPerLayer = o.passes;
    set(p.threshold, o.threshold);
    set(p.curvature, o.curvature);
    set(p.opacity, o.opacity);
    set(p.gridFactor, o.gridFactor);
    set(p.maxStrokeLength, o.maxLen);
    set(p.minStrokeLength, o.minLen);
    set(p.tensorSigma, o.tensorSigma);
    set(p.texStrength, o.brushTexture);
    set(cfg.bristleDensity, o.bristleDensity);
    set(p.texTaper, o.textureTaper);
    set(p.impastoStrength, o.impasto);
    set(p.impastoLight, o.impastoLight);
    set(p.lightAngle, o.lightAngle);
    set(p.dryBrush, o.dryBrush);
    set(p.sizeJitter, o.sizeJitter);
    set(p.angleJitter, o.angleJitter);
    set(p.opacityJitter, o.opacityJitter);
    set(p.frameDiffThreshold, o.temporalDiff);
    if (o.etf >= 0) cfg.etfIterations = o.etf;
    if (o.flow >= 0) cfg.flowLevels = o.flow;
    if (o.flowIters > 0) cfg.flowIterations = o.flowIters;
    set(p.etfRadius, o.etfRadius);
    if (o.relax >= 0) cfg.relaxIterations = o.relax;
    if (o.relaxSubs > 0) cfg.relaxSubPasses = o.relaxSubs;
    set(p.relaxAreaWeight, o.relaxArea);
    set(p.relaxMoveScale, o.relaxMove);
    set(p.relaxCandidates, o.relaxCands);
    set(p.relaxRemove, o.relaxRemove);
    p.jitterPerFrame = o.jitterPerFrame ? 1.f : 0.f;
}

// Fallback subject when no image is supplied: smooth colour ramps plus a few
// hard edges, which exercises both the flow field and the stroke termination.
std::vector<unsigned char> syntheticImage(int w, int h) {
    std::vector<unsigned char> px(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = float(x) / w, v = float(y) / h;
            const float swirl = std::sin((u * 6.0f + v * 3.0f) * 3.14159f)
                              * std::cos((v * 5.0f - u * 2.0f) * 3.14159f);
            float r = 0.5f + 0.45f * swirl;
            float g = 0.45f + 0.4f * std::sin(u * 9.0f + swirl * 2.0f);
            float b = 0.55f + 0.4f * std::cos(v * 7.0f - swirl * 2.0f);

            const float dx = u - 0.35f, dy = v - 0.45f;
            if (dx * dx + dy * dy < 0.02f) { r = 0.95f; g = 0.35f; b = 0.2f; }
            if (std::fabs(u - 0.72f) < 0.06f && v > 0.25f && v < 0.8f) {
                r = 0.1f; g = 0.15f; b = 0.3f;
            }

            const size_t i = (size_t(y) * w + x) * 4;
            px[i + 0] = (unsigned char)(r * 255.f);
            px[i + 1] = (unsigned char)(g * 255.f);
            px[i + 2] = (unsigned char)(b * 255.f);
            px[i + 3] = 255;
        }
    }
    return px;
}

bool loadSource(Pipeline& pipe, const std::string& path, std::string* note) {
    if (!path.empty()) {
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (px) {
            const bool ok = pipe.setSource(px, w, h);
            stbi_image_free(px);
            *note = path + "  " + std::to_string(w) + "x" + std::to_string(h);
            return ok;
        }
        fprintf(stderr, "could not read %s (%s); falling back to the synthetic subject\n",
                path.c_str(), stbi_failure_reason());
    }
    const int w = 1920, h = 1080;
    const std::vector<unsigned char> px = syntheticImage(w, h);
    *note = "synthetic 1920x1080";
    return pipe.setSource(px.data(), w, h);
}

void writePng(const std::string& path, const std::vector<unsigned char>& px, int w, int h) {
    // Canvas texel row 0 is image row 0 (stroke.vert does not flip y), and
    // readCanvas hands back texel rows in order, so the buffer is already
    // top-row-first and must not be flipped again.
    if (!stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4))
        fprintf(stderr, "failed to write %s\n", path.c_str());
    else
        printf("wrote %s (%dx%d)\n", path.c_str(), w, h);
}

// ── GUI state ──────────────────────────────────────────────────────────────

enum class InputKind { None, Image, Video, Images };

// Everything the window needs to know about what is loaded and where it is
// going. Kept in one struct so the drop handler, the picker buttons and the
// export button all agree.
struct Gui {
    InputKind kind = InputKind::None;
    std::string inputPath;                 // image or video
    std::vector<std::string> queue;        // Images: the files to paint
    media::VideoInfo video;                // valid when kind == Video
    double previewAt = 0.0;                // preview position, seconds
    double previewRequested = -1.0;        // set on slider release, -1 = idle

    std::string outDir;
    std::string videoOut;
    std::string format, suffix;
    double fpsOverride = 0.0;
    int crf = 18;
    bool keepAudio = true;

    // Export in flight
    bool startExport = false;
    bool exporting = false;
    bool cancel = false;
    jobs::Progress progress;
    std::string status;
    bool statusIsError = false;
};

std::string fileName(const std::string& p) {
    return std::filesystem::path(p).filename().string();
}

// Loads one frame of a video as the preview, so the sliders act on real
// footage rather than on a still the user has to imagine.
bool loadVideoPreview(Pipeline& pipe, Gui& gui, std::string* note) {
    std::vector<unsigned char> frame;
    if (!media::readFrameAt(gui.inputPath, gui.previewAt,
                            gui.video.width, gui.video.height, frame))
        return false;
    pipe.setSource(frame.data(), gui.video.width, gui.video.height);
    pipe.resetTemporal();

    char buf[512];
    snprintf(buf, sizeof(buf), "%s  %dx%d  %.2f fps%s",
             fileName(gui.inputPath).c_str(), gui.video.width, gui.video.height,
             gui.video.fps, gui.video.hasAudio ? "  +audio" : "");
    *note = buf;
    return true;
}

// One dropped or picked path, or several. A folder means batch, a video means
// video, anything else is tried as a still.
void acceptPaths(const std::vector<std::string>& paths, Pipeline& pipe,
                 Gui& gui, std::string& note) {
    if (paths.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;

    // Several files at once, or a folder, is a batch.
    if (paths.size() > 1 || fs::is_directory(paths[0], ec)) {
        std::vector<std::string> files;
        for (const std::string& p : paths) {
            if (fs::is_directory(p, ec)) {
                const std::vector<std::string> inDir = media::listImages(p);
                files.insert(files.end(), inDir.begin(), inDir.end());
                if (gui.outDir.empty() || gui.outDir == "frames_out")
                    gui.outDir = (fs::path(p) / "painted").string();
            } else if (media::isImageExtension(p)) {
                files.push_back(p);
            }
        }
        if (files.empty()) {
            gui.status = "no images there";
            gui.statusIsError = true;
            return;
        }
        gui.kind = InputKind::Images;
        gui.queue = files;
        gui.inputPath.clear();
        // Show the first one so the sliders have something to act on.
        loadSource(pipe, files.front(), &note);
        pipe.resetTemporal();
        gui.status = std::to_string(files.size()) + " images queued";
        gui.statusIsError = false;
        return;
    }

    const std::string& p = paths[0];

    if (media::isVideoExtension(p)) {
        std::string err;
        media::VideoInfo info;
        if (!media::probe(p, &info, &err)) {
            gui.status = err;
            gui.statusIsError = true;
            return;
        }
        gui.kind = InputKind::Video;
        gui.inputPath = p;
        gui.video = info;
        gui.previewAt = 0.0;
        gui.queue.clear();
        if (gui.videoOut.empty() || gui.videoOut == "painted.mp4") {
            gui.videoOut = (fs::path(p).parent_path() /
                            (fs::path(p).stem().string() + "_painted.mp4")).string();
        }
        if (!loadVideoPreview(pipe, gui, &note)) {
            gui.status = "ffmpeg could not decode a frame from that file";
            gui.statusIsError = true;
            gui.kind = InputKind::None;
            return;
        }
        gui.status = "video loaded - tune, then Export";
        gui.statusIsError = false;
        return;
    }

    gui.kind = InputKind::Image;
    gui.inputPath = p;
    gui.queue.clear();
    loadSource(pipe, p, &note);
    pipe.resetTemporal();
    if (gui.outDir.empty() || gui.outDir == "frames_out")
        gui.outDir = fs::path(p).parent_path().string();
    gui.status.clear();
    gui.statusIsError = false;
}

// ── Framing ────────────────────────────────────────────────────────────────

// The interactive part of ViewXform: what is being dragged, and by how much.
struct View {
    ViewXform xf;
    bool panning = false;
    bool dragDivider = false;
};

// Wheel zooms about the cursor, left-drag pans, and in split view a drag near
// the divider moves it instead.
//
// This runs *inside* the ImGui frame, after NewFrame, for two reasons:
// io.WantCaptureMouse is then this frame's answer rather than the previous
// frame's, and io.MouseWheel has been resolved from the event queue. The blit
// therefore has to happen after the panel is built -- see the main loop.
void updateView(View& v, int fbH, int vx, int vy, int vw, int vh,
                int imgW, int imgH, ViewMode mode) {
    ImGuiIO& io = ImGui::GetIO();
    if (imgW <= 0 || imgH <= 0 || vw <= 0 || vh <= 0) return;

    // io.MousePos is in logical window units with a top-left origin; the blit
    // works in framebuffer pixels with a bottom-left one.
    const float sx = io.DisplayFramebufferScale.x;
    const float sy = io.DisplayFramebufferScale.y;
    const float mx = io.MousePos.x * sx;
    const float my = float(fbH) - io.MousePos.y * sy;
    const bool over = !io.WantCaptureMouse && mx >= float(vx) &&
                      mx < float(vx + vw) && my >= float(vy) && my < float(vy + vh);

    const float fit = std::min(float(vw) / float(imgW), float(vh) / float(imgH));
    const float scale = fit * std::max(v.xf.zoom, 0.01f);
    const float fx = v.xf.focusX < 0.f ? float(imgW) * 0.5f : v.xf.focusX;
    const float fy = v.xf.focusY < 0.f ? float(imgH) * 0.5f : v.xf.focusY;
    const float cx = float(vx) + float(vw) * 0.5f;
    const float cy = float(vy) + float(vh) * 0.5f;

    // The divider gets first refusal on a press: once the image fills the
    // viewport a pan would otherwise make it impossible to grab.
    const float cut = float(vx) + float(vw) * v.xf.wipe;
    const bool onDivider = mode == ViewMode::Split && std::fabs(mx - cut) < 8.f * sx;
    if (over && onDivider) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && over) {
        if (onDivider) v.dragDivider = true;
        else           v.panning = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        v.panning = false;
        v.dragDivider = false;
    }

    if (v.dragDivider) {
        v.xf.wipe = std::clamp((mx - float(vx)) / float(vw), 0.f, 1.f);
    } else if (v.panning) {
        // One screen pixel of drag moves the image one screen pixel, whatever
        // the zoom -- hence dividing the delta by the scale to get image px.
        v.xf.focusX = fx - io.MouseDelta.x * sx / scale;
        v.xf.focusY = fy - io.MouseDelta.y * sy / scale;
    }

    if (over && io.MouseWheel != 0.f) {
        // Pin the image point under the cursor, so the wheel magnifies what is
        // being looked at rather than the middle of the window.
        const float ix = fx + (mx - cx) / scale;
        const float iy = fy - (my - cy) / scale;
        const float z = std::clamp(v.xf.zoom * std::pow(1.15f, io.MouseWheel),
                                   0.25f, 64.f);
        const float s2 = fit * z;
        v.xf.zoom = z;
        v.xf.focusX = ix - (mx - cx) / s2;
        v.xf.focusY = iy + (my - cy) / s2;
    }

    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            v.xf.zoom = 1.f;
            v.xf.focusX = v.xf.focusY = -1.f;   // back to the image centre
        }
        // 1:1 means one canvas texel per screen pixel, which is the only
        // framing at which brush texture can be judged at all.
        if (ImGui::IsKeyPressed(ImGuiKey_1, false) && fit > 0.f) v.xf.zoom = 1.f / fit;
    }

    // Never let the image be panned entirely out of sight.
    v.xf.focusX = std::clamp(v.xf.focusX < 0.f ? float(imgW) * 0.5f : v.xf.focusX,
                             0.f, float(imgW));
    v.xf.focusY = std::clamp(v.xf.focusY < 0.f ? float(imgH) * 0.5f : v.xf.focusY,
                             0.f, float(imgH));
}

void printTimings(const Pipeline& pipe, float relaxAreaWeight) {
    // Seeds vs drawn: a stroke that traced to a single point draws nothing, and
    // the gradient-magnitude floor stops the walk at its first step across flat
    // regions. `drawn` is the figure comparable to worker.js's
    // renderStrokeSolid calls -- seeds outnumber it heavily.
    printf("seeds: %u  drawn: %u  mean points/stroke: %.2f\n",
           pipe.lastStrokeCount(), pipe.lastDrawnCount(), pipe.lastMeanPoints());
    printf("reference %.2f | error %.2f | seeds %.2f | trace %.2f | raster %.2f"
           " | relax %.2f | impasto %.2f | total %.2f ms\n",
           pipe.msReference(), pipe.msError(), pipe.msSeeds(), pipe.msTrace(),
           pipe.msRaster(), pipe.msRelax(), pipe.msImpasto(), pipe.msTotal());
    if (pipe.lastPoolCount() > 0) {
        printf("relaxation: pool %u  moves kept %u  removed %u\n",
               pipe.lastPoolCount(), pipe.lastRelaxAccepted(),
               pipe.lastRelaxRemoved());
        const double coverage =
            pipe.relaxTotalArea() / (double(pipe.width()) * pipe.height());
        if (pipe.relaxLiveStrokes() > 0)
            printf("  geometry: %u live strokes, %.2f points, mean radius %.2f px,"
                   " coverage %.2fx\n",
                   pipe.relaxLiveStrokes(), pipe.relaxMeanPoints(),
                   pipe.relaxMeanRadius(), coverage);

        const std::vector<double>& log = pipe.relaxEnergyLog();
        if (!log.empty()) {
            // E_app alone falls monotonically only at w_area = 0. With a
            // positive area weight relaxation is deliberately spending
            // appearance to buy economy, so E_app can rise while the real
            // objective E_app + w_area * sum(area) still falls -- so print
            // both, and the objective's area term alongside.
            printf("  E_app (mean Lab error/px):");
            for (double e : log) printf(" %.3f", e);
            printf("\n  E_area term: w %.2f x coverage %.2f = %.3f"
                   "   ->  objective %.3f\n",
                   relaxAreaWeight, coverage, relaxAreaWeight * coverage,
                   log.back() + relaxAreaWeight * coverage);
        }
    }
    // Coherence, per layer, before and after ETF. Printed separately from the
    // stroke counts because it is a property of the *field*, not of the
    // painting -- and the field is what ETF changes. A picture that merely
    // looks different is not evidence that the filter works.
    const std::vector<std::pair<double, double>>& elog = pipe.etfLog();
    if (!elog.empty()) {
        printf("flow coherence (mean |t.t| over 8 neighbours; 0.64 = random):\n");
        for (size_t i = 0; i < elog.size(); ++i) {
            if (elog[i].second < 0.0)
                printf("  layer %zu: %.4f  (ETF off)\n", i, elog[i].first);
            else
                printf("  layer %zu: %.4f -> %.4f  (%+.1f%%)\n", i, elog[i].first,
                       elog[i].second,
                       100.0 * (elog[i].second - elog[i].first) /
                           std::max(elog[i].first, 1e-9));
        }
    }
    for (const LayerStats& ls : pipe.layerStats())
        printf("  r %.1f px  grid %.0f  cells %u  passes %u  seeds %u  drawn %u\n",
               ls.radius, ls.grid, ls.cells, ls.chunks, ls.strokes, ls.drawn);
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parseArgs(argc, argv);

    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
    if (opt.headless || opt.liveSpout) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow* win = glfwCreateWindow(1600, 900, "gpu-sbr", nullptr, nullptr);
    if (!win) {
        fprintf(stderr, "need an OpenGL 4.6 core context\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        fprintf(stderr, "glad failed\n");
        return 1;
    }
    glfwSwapInterval(0);   // vsync off: the overlay is meant to show real cost
    printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    Pipeline pipe;
    if (!pipe.init()) return 1;
    pipe.setDebugCells(opt.debugCells);
    pipe.setRelaxLogging(opt.relaxLog);
    pipe.setEtfLogging(opt.etfLog);
    pipe.setFlowLogging(opt.flowLog);

    std::string sourceNote;
    if (!opt.liveSpout && !loadSource(pipe, opt.in, &sourceNote)) {
        fprintf(stderr, "no source image\n");
        return 1;
    }

    TuningParams params;
    RenderConfig cfg;
    std::string radiiText = radiiToString(cfg.radii);

    int presetIdx = 0;
    applyPreset(kPresets[0], params, cfg, &radiiText);
    if (!opt.preset.empty()) {
        bool found = false;
        for (int i = 0; i < kPresetCount; ++i) {
            if (opt.preset == kPresets[i].name) {
                presetIdx = i;
                applyPreset(kPresets[i], params, cfg, &radiiText);
                found = true;
                break;
            }
        }
        if (!found) fprintf(stderr, "unknown preset '%s'; using impressionist\n",
                            opt.preset.c_str());
    }
    // A saved file sits between the preset and the individual flags: it is a
    // whole look, so it should replace the preset, but an explicit --threshold
    // on the same command line is clearly meant to win over both.
    if (!opt.paramsFile.empty()) {
        std::string perr;
        if (!paramfile::load(opt.paramsFile, &params, &cfg, &perr))
            fprintf(stderr, "%s\n", perr.c_str());
        else {
            if (!perr.empty()) fprintf(stderr, "%s: %s\n", opt.paramsFile.c_str(),
                                       perr.c_str());
            radiiText = radiiToString(cfg.radii);
        }
    }
    applyOverrides(opt, params, cfg, &radiiText);

    if (opt.liveSpout) {
        LiveSpoutConfig live;
        live.inputName = opt.spoutIn;
        live.outputName = opt.spoutOut;
        live.fps = opt.targetFps;
        live.stopFile = opt.liveStopFile;
        live.parentPid = opt.liveParentPid;
        const int result = runLiveSpout(win, pipe, params, cfg, live);
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return result;
    }

    if (!opt.saveParamsFile.empty()) {
        std::string perr;
        if (!paramfile::save(opt.saveParamsFile, params, cfg, &perr))
            fprintf(stderr, "%s\n", perr.c_str());
        else
            printf("wrote %s\n", opt.saveParamsFile.c_str());
    }

    // ------------------------------------------------------------------
    // Video in, video out. Frames stream through ffmpeg as raw RGBA, so
    // nothing hits the disk in between and the GPU is the only real cost.
    // ------------------------------------------------------------------
    if (!opt.videoIn.empty()) {
        media::VideoInfo info;
        std::string err;
        if (!media::haveFfmpeg(&err)) {
            fprintf(stderr, "%s is not on PATH; --video needs ffmpeg and ffprobe.\n"
                            "Install ffmpeg, or decode to PNGs yourself and use"
                            " --frames.\n", err.c_str());
            return 1;
        }
        if (!media::probe(opt.videoIn, &info, &err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }

        jobs::VideoSpec spec;
        spec.input = opt.videoIn;
        spec.output = opt.out;
        // A default of out.png would silently produce an unplayable file.
        if (spec.output.size() > 4 &&
            spec.output.compare(spec.output.size() - 4, 4, ".png") == 0)
            spec.output = "out.mp4";
        spec.fps = opt.fps;
        spec.crf = opt.crf;
        spec.codec = opt.vcodec;
        spec.preset = opt.vpreset;
        spec.keepAudio = !opt.noAudio;

        printf("input : %s  %dx%d  %.3f fps  %s%s\n", opt.videoIn.c_str(),
               info.width, info.height, info.fps, info.codec.c_str(),
               info.hasAudio ? " +audio" : "");
        if (info.frames > 0) printf("        %lld frames\n", (long long)info.frames);
        printf("output: %s  %s crf %d%s\n", spec.output.c_str(),
               spec.codec.c_str(), spec.crf,
               (spec.keepAudio && info.hasAudio) ? "  (audio copied)" : "");
        if (params.frameDiffThreshold <= 0.f)
            printf("note  : --temporal-diff is 0, so every frame is painted"
                   " independently and strokes will shimmer. Try 12.\n");

        const jobs::Result res = jobs::runVideo(
            pipe, params, cfg, spec, [&](const jobs::Progress& p) {
                if (p.total > 0)
                    printf("\r  %lld/%lld  %.0f%%  %.1f ms/frame  drawn %u   ",
                           (long long)p.done, (long long)p.total,
                           100.0 * double(p.done) / double(p.total),
                           p.msPerFrame, p.drawn);
                else
                    printf("\r  %lld frames  %.1f ms/frame   ",
                           (long long)p.done, p.msPerFrame);
                fflush(stdout);
                return true;
            });

        printf("\n%lld frames in %.1f s wall (%.1f fps), GPU %.2f ms/frame\n",
               (long long)res.done, res.wallSeconds,
               res.done / std::max(res.wallSeconds, 1e-9), res.gpuMsMean);
        if (!res.ok) fprintf(stderr, "%s\n", res.error.c_str());
        else         printf("wrote %s\n", spec.output.c_str());

        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return res.ok ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Batch images. Each file is independent -- no temporal carry-over,
    // and the size is allowed to change between files.
    // ------------------------------------------------------------------
    if (!opt.batchDir.empty()) {
        jobs::BatchSpec spec;
        spec.dir = opt.batchDir;
        spec.outDir = opt.outDir;
        spec.suffix = opt.batchSuffix;
        spec.format = opt.batchFormat;

        const jobs::Result res = jobs::runBatch(
            pipe, params, cfg, spec, [&](const jobs::Progress& p) {
                printf("  %lld/%lld  %dx%d  %.1f ms  drawn %u  -> %s\n",
                       (long long)p.done, (long long)p.total, p.width, p.height,
                       p.msPerFrame, p.drawn, p.item.c_str());
                return true;
            });

        if (!res.ok && res.done == 0) {
            fprintf(stderr, "%s\n", res.error.c_str());
        } else {
            printf("%lld painted, %lld skipped, %.1f s wall, GPU %.2f ms/image\n",
                   (long long)res.done, (long long)res.skipped, res.wallSeconds,
                   res.gpuMsMean);
        }
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return res.ok ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Frame sequence: image directory in, image directory out. Kept for
    // when ffmpeg is unavailable or the frames are already extracted.
    // ------------------------------------------------------------------
    if (!opt.framesDir.empty()) {
        jobs::FramesSpec spec;
        spec.dir = opt.framesDir;
        spec.outDir = opt.outDir;

        const jobs::Result res = jobs::runFrames(
            pipe, params, cfg, spec, [&](const jobs::Progress& p) {
                printf("  %lld/%lld  drawn %u  gpu %.2f ms\n",
                       (long long)p.done, (long long)p.total, p.drawn, p.msPerFrame);
                return true;
            });

        if (!res.ok) fprintf(stderr, "%s\n", res.error.c_str());
        else printf("mean GPU cost %.2f ms/frame over %lld frames\n",
                    res.gpuMsMean, (long long)res.done);
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return res.ok ? 0 : 1;
    }

    // ------------------------------------------------------------------
    // Headless: render, read back, write, report. Three frames so the GPU
    // timers (which read one frame late) have real numbers to report.
    // ------------------------------------------------------------------
    if (opt.headless) {
        for (int i = 0; i < 3; ++i) {
            pipe.render(params, cfg);
            params.frame += 1.f;
        }
        glFinish();
        pipe.refreshStats();
        if (opt.dump > 0) pipe.dumpStrokes(opt.dump);
        writePng(opt.out, pipe.readCanvas(), pipe.width(), pipe.height());
        printf("source: %s\n", sourceNote.c_str());
        printTimings(pipe, params.relaxAreaWeight);
        pipe.shutdown();
        glfwDestroyWindow(win);
        glfwTerminate();
        return 0;
    }

    // ------------------------------------------------------------------
    // Interactive
    // ------------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 460");

    filedialog::init();

    // Drops arrive on the GLFW callback, which cannot touch local state, so
    // they queue here and the loop drains them.
    static std::vector<std::string> droppedPaths;
    glfwSetDropCallback(win, [](GLFWwindow*, int count, const char** paths) {
        for (int i = 0; i < count; ++i) droppedPaths.push_back(paths[i]);
    });

    bool temporal = false;
    bool tensorOn = params.tensorSigma > 0.f;
    ViewMode view = ViewMode::Split;   // start split so the input is visible
    const char* kViewNames[] = {"painted", "source", "split"};
    const char* kUnderpaintNames[] = {"blur", "none", "average"};
    std::string shaderMsg = "shaders ok";
    View viewCtl;
    char radiiBuf[128];
    snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
    double cpuMs = 0.0;

    // --- what is loaded, and where it is going -------------------------
    Gui gui;
    gui.outDir = opt.outDir;
    gui.format = opt.batchFormat;
    gui.suffix = opt.batchSuffix;
    gui.videoOut = "painted.mp4";
    gui.crf = opt.crf;
    gui.keepAudio = !opt.noAudio;
    gui.fpsOverride = opt.fps;
    if (!opt.in.empty()) { gui.kind = InputKind::Image; gui.inputPath = opt.in; }

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        // A drop can be an image, a video, or a folder; each means something
        // different, so classify before deciding what to do with it.
        if (!droppedPaths.empty()) {
            acceptPaths(droppedPaths, pipe, gui, sourceNote);
            droppedPaths.clear();
        }
        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        static bool tabDown = false;
        const bool tab = glfwGetKey(win, GLFW_KEY_TAB) == GLFW_PRESS;
        if (tab && !tabDown) view = ViewMode(((int)view + 1) % 3);
        tabDown = tab;

        static bool f5Down = false;
        const bool f5 = glfwGetKey(win, GLFW_KEY_F5) == GLFW_PRESS;
        if (f5 && !f5Down) {
            std::string err;
            shaderMsg = pipe.reloadShaders(&err) ? "shaders reloaded" : err;
        }
        f5Down = f5;

        const double t0 = glfwGetTime();
        params.tensorSigma = tensorOn ? std::max(0.1f, params.tensorSigma) : 0.f;
        pipe.render(params, cfg, temporal);
        params.frame += 1.f;

        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(win, &fbW, &fbH);
        // Reserve the left column for the panel so the image is never hidden
        // behind it -- the whole point of the split view is seeing the input.
        const int panelW = 400;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(400, float(fbH)), ImGuiCond_Always);
        ImGui::Begin("gpu-sbr", nullptr,
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoCollapse);
        ImGui::PushItemWidth(-165.0f);   // leave room for the longest label

        // ── Input ──────────────────────────────────────────────────────
        static const std::vector<filedialog::Filter> kImageFilter = {
            {"Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif"}};
        static const std::vector<filedialog::Filter> kVideoFilter = {
            {"Video", "*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.wmv"}};

        ImGui::TextDisabled("INPUT");
        if (ImGui::Button("Open image...")) {
            const std::string p = filedialog::openFile("Choose an image", kImageFilter);
            if (!p.empty()) acceptPaths({p}, pipe, gui, sourceNote);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open video...")) {
            const std::string p = filedialog::openFile("Choose a video", kVideoFilter);
            if (!p.empty()) acceptPaths({p}, pipe, gui, sourceNote);
        }
        if (ImGui::Button("Add images...")) {
            const std::vector<std::string> ps =
                filedialog::openFiles("Choose images to paint", kImageFilter);
            if (!ps.empty()) acceptPaths(ps, pipe, gui, sourceNote);
        }
        ImGui::SameLine();
        if (ImGui::Button("Choose folder...")) {
            const std::string p = filedialog::pickFolder("Choose a folder of images");
            if (!p.empty()) acceptPaths({p}, pipe, gui, sourceNote);
        }
        ImGui::TextDisabled("or drag files / a folder onto the window");

        ImGui::TextWrapped("%s", sourceNote.c_str());

        if (gui.kind == InputKind::Images) {
            ImGui::Text("queued: %zu images", gui.queue.size());
            if (ImGui::BeginListBox("##queue", ImVec2(-1, 70))) {
                for (size_t i = 0; i < gui.queue.size() && i < 200; ++i)
                    ImGui::TextUnformatted(fileName(gui.queue[i]).c_str());
                ImGui::EndListBox();
            }
            if (ImGui::Button("Clear queue")) {
                gui.queue.clear();
                gui.kind = InputKind::None;
            }
        } else if (gui.kind == InputKind::Video) {
            const double dur = (gui.video.fps > 0.0 && gui.video.frames > 0)
                             ? double(gui.video.frames) / gui.video.fps : 0.0;
            ImGui::Text("%lld frames, %.1f s%s", (long long)gui.video.frames, dur,
                        gui.video.hasAudio ? ", audio" : ", no audio");
            // Scrub on release rather than on drag: each move would otherwise
            // spawn an ffmpeg seek per frame of UI.
            float at = float(gui.previewAt);
            if (ImGui::SliderFloat("preview at", &at, 0.0f, float(std::max(dur, 0.1)), "%.2f s"))
                gui.previewAt = at;
            if (ImGui::IsItemDeactivatedAfterEdit()) gui.previewRequested = gui.previewAt;
        }

        ImGui::Separator();
        int vi = (int)view;
        if (ImGui::Combo("view", &vi, kViewNames, 3)) view = ViewMode(vi);
        if (view == ViewMode::Split)
            ImGui::TextDisabled("left = input, right = painted; drag the divider");

        // ── Export ─────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::TextDisabled("EXPORT");
        {
            char buf[1024];
            if (gui.kind == InputKind::Video) {
                snprintf(buf, sizeof(buf), "%s", gui.videoOut.c_str());
                if (ImGui::InputText("out file", buf, sizeof(buf))) gui.videoOut = buf;
                if (ImGui::Button("Choose output file...")) {
                    const std::string p = filedialog::saveFile(
                        "Save painted video as", kVideoFilter,
                        fileName(gui.videoOut).c_str(), "mp4");
                    if (!p.empty()) gui.videoOut = p;
                }
                ImGui::SliderInt("quality (crf)", &gui.crf, 12, 30);
                ImGui::TextDisabled("lower = better and bigger");
                if (gui.video.hasAudio) ImGui::Checkbox("keep audio", &gui.keepAudio);
            } else if (gui.kind != InputKind::None) {
                snprintf(buf, sizeof(buf), "%s", gui.outDir.c_str());
                if (ImGui::InputText("out folder", buf, sizeof(buf))) gui.outDir = buf;
                if (ImGui::Button("Choose output folder...")) {
                    const std::string p = filedialog::pickFolder("Choose where to save");
                    if (!p.empty()) gui.outDir = p;
                }
                char sbuf[128];
                snprintf(sbuf, sizeof(sbuf), "%s", gui.suffix.c_str());
                if (ImGui::InputText("suffix", sbuf, sizeof(sbuf))) gui.suffix = sbuf;
                const char* kFormats[] = {"png", "jpg", "bmp", "tga"};
                int fi = 0;
                for (int i = 0; i < 4; ++i) if (gui.format == kFormats[i]) fi = i;
                if (ImGui::Combo("format", &fi, kFormats, 4)) gui.format = kFormats[fi];
            }

            if (gui.kind == InputKind::None) {
                ImGui::TextDisabled("load something first");
            } else if (gui.exporting) {
                const float frac = gui.progress.total > 0
                    ? float(double(gui.progress.done) / double(gui.progress.total)) : 0.f;
                ImGui::ProgressBar(frac, ImVec2(-1, 0));
                ImGui::Text("%lld / %lld   %.1f ms/frame",
                            (long long)gui.progress.done, (long long)gui.progress.total,
                            gui.progress.msPerFrame);
                if (ImGui::Button("Cancel", ImVec2(-1, 0))) gui.cancel = true;
            } else {
                const char* label = (gui.kind == InputKind::Video) ? "Export video"
                                  : (gui.kind == InputKind::Images) ? "Export all images"
                                  : "Export image";
                if (ImGui::Button(label, ImVec2(-1, 30))) gui.startExport = true;
            }

            if (!gui.status.empty()) {
                if (gui.statusIsError)
                    ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", gui.status.c_str());
                else
                    ImGui::TextWrapped("%s", gui.status.c_str());
            }
        }

        ImGui::Separator();
        ImGui::Text("seeds %u   drawn %u   %.1f pts/stroke",
                    pipe.lastStrokeCount(), pipe.lastDrawnCount(), pipe.lastMeanPoints());
        ImGui::Text("GPU  %.2f ms  (%.0f fps)", pipe.msTotal(),
                    pipe.msTotal() > 0.0 ? 1000.0 / pipe.msTotal() : 0.0);
        ImGui::Text("  reference %.2f  error  %.2f", pipe.msReference(), pipe.msError());
        ImGui::Text("  seeds     %.2f  trace  %.2f", pipe.msSeeds(), pipe.msTrace());
        ImGui::Text("  raster    %.2f  impasto %.2f", pipe.msRaster(), pipe.msImpasto());
        if (pipe.msRelax() > 0.0) ImGui::Text("  relax     %.2f", pipe.msRelax());
        ImGui::Text("frame wall  %.2f ms", cpuMs);

        ImGui::Separator();
        if (ImGui::Combo("preset", &presetIdx,
                         [](void*, int i, const char** out) {
                             *out = kPresets[i].name; return true;
                         }, nullptr, kPresetCount)) {
            applyPreset(kPresets[presetIdx], params, cfg, &radiiText);
            snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
            tensorOn = params.tensorSigma > 0.f;
        }

        // Save / load a whole look. The five built-in presets are the web's
        // and are not editable; this is how a look found by dragging sliders
        // survives the session, and how it reaches a --video render.
        static const std::vector<filedialog::Filter> kParamFilter = {
            {"gpu-sbr parameters", "*.sbr"}};
        if (ImGui::Button("Save params...")) {
            const std::string p = filedialog::saveFile(
                "Save parameters", kParamFilter, "look.sbr", "sbr");
            if (!p.empty()) {
                std::string perr;
                gui.statusIsError = !paramfile::save(p, params, cfg, &perr);
                gui.status = gui.statusIsError ? perr : ("saved " + fileName(p));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load params...")) {
            const std::string p = filedialog::openFile("Load parameters", kParamFilter);
            if (!p.empty()) {
                std::string perr;
                if (!paramfile::load(p, &params, &cfg, &perr)) {
                    gui.status = perr;
                    gui.statusIsError = true;
                } else {
                    // Everything the panel mirrors in its own state has to be
                    // resynced, or the widgets would keep showing the old look
                    // while the renderer used the new one.
                    radiiText = radiiToString(cfg.radii);
                    snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
                    tensorOn = params.tensorSigma > 0.f;
                    pipe.buildBrushTiles(cfg.radii, cfg.bristleDensity);
                    gui.status = perr.empty() ? ("loaded " + fileName(p))
                                              : (fileName(p) + ": " + perr);
                    gui.statusIsError = false;
                }
            }
        }
        if (ImGui::InputText("brush radii", radiiBuf, sizeof(radiiBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            cfg.radii = parseRadii(radiiBuf);
            radiiText = radiiToString(cfg.radii);
            snprintf(radiiBuf, sizeof(radiiBuf), "%s", radiiText.c_str());
        }
        ImGui::TextDisabled("coarse to fine, press Enter to apply");

        int upi = int(cfg.underpaint);
        if (ImGui::Combo("underpaint", &upi, kUnderpaintNames, 3))
            cfg.underpaint = Underpaint(upi);

        ImGui::Separator();
        ImGui::TextDisabled("placement");
        ImGui::SliderFloat("threshold", &params.threshold, 1.f, 150.f, "%.0f");
        ImGui::SliderFloat("grid factor", &params.gridFactor, 0.25f, 2.5f);
        ImGui::TextDisabled("stroke path");
        ImGui::SliderFloat("max length", &params.maxStrokeLength, 1.f, 32.f, "%.0f");
        ImGui::SliderFloat("min length", &params.minStrokeLength, 1.f, 32.f, "%.0f");
        ImGui::SliderFloat("curvature", &params.curvature, 0.f, 1.f);
        ImGui::Checkbox("structure tensor", &tensorOn);
        if (tensorOn) ImGui::SliderFloat("tensor sigma", &params.tensorSigma, 0.5f, 8.f);

        // Edge Tangent Flow. Sits with the flow-field controls because that is
        // what it refines -- it is a filter on the field, not a stroke setting.
        ImGui::SliderInt("etf iterations", &cfg.etfIterations, 0, 8);
        if (cfg.etfIterations > 0) {
            ImGui::SliderFloat("etf radius", &params.etfRadius, 1.f, 12.f, "%.0f px");
            // The kernel is a disc, so the cost is quadratic in the radius and
            // linear in both the iterations and the layer count. Worth stating
            // next to the sliders rather than leaving it to be discovered in
            // the frame time.
            ImGui::TextDisabled("edge-aware flow smoothing; cost ~ r^2 x iters");
        }

        ImGui::Separator();
        ImGui::TextDisabled("paint");
        ImGui::SliderFloat("opacity", &params.opacity, 0.05f, 1.f);
        ImGui::SliderFloat("hue jitter", &params.jitterHue, 0.f, 0.3f);
        ImGui::SliderFloat("sat jitter", &params.jitterSat, 0.f, 0.5f);
        ImGui::SliderFloat("val jitter", &params.jitterVal, 0.f, 0.5f);
        ImGui::SliderFloat("size jitter", &params.sizeJitter, 0.f, 0.6f);
        ImGui::SliderFloat("angle jitter", &params.angleJitter, 0.f, 45.f, "%.0f deg");
        ImGui::SliderFloat("opacity jitter", &params.opacityJitter, 0.f, 0.6f);

        ImGui::Separator();
        ImGui::TextDisabled("brush texture");
        ImGui::SliderFloat("texture", &params.texStrength, 0.f, 1.f);
        if (params.texStrength > 0.f) {
            if (ImGui::SliderFloat("bristle density", &cfg.bristleDensity, 2.f, 30.f))
                pipe.buildBrushTiles(cfg.radii, cfg.bristleDensity);
            ImGui::SliderFloat("tip taper", &params.texTaper, 0.f, 1.f);
            ImGui::SliderFloat("dry brush", &params.dryBrush, 0.f, 1.f);
        }

        ImGui::Separator();
        ImGui::TextDisabled("impasto");
        // The web exposes these as Off / Subtle / Medium / Strong = 0, .15,
        // .3, .5. Past ~0.6 the height field saturates in dense areas and the
        // relief lighting clips to its clamps, so the range stops where the
        // web's does.
        ImGui::SliderFloat("height", &params.impastoStrength, 0.f, 0.5f);
        ImGui::SliderFloat("relief light", &params.impastoLight, 0.f, 0.5f);
        if (params.impastoLight > 0.f)
            ImGui::SliderFloat("light angle", &params.lightAngle, 0.f, 360.f, "%.0f deg");

        // ── Relaxation ─────────────────────────────────────────────────
        // Hertzmann 2001 on top of the greedy result. These were CLI-only,
        // which meant the one knob that trades fidelity for economy could not
        // be turned while looking at the picture it changes.
        ImGui::Separator();
        ImGui::TextDisabled("relaxation (Hertzmann 2001)");
        ImGui::SliderInt("iterations", &cfg.relaxIterations, 0, 16);
        if (cfg.relaxIterations > 0) {
            // Each iteration repaints the whole canvas from the stroke pool,
            // so the cost is roughly one extra render per iteration -- worth
            // saying, because the frame time below is the only other clue.
            ImGui::SliderFloat("area weight", &params.relaxAreaWeight, 0.f, 0.4f);
            ImGui::TextDisabled("higher = fewer, larger, bolder strokes");
            ImGui::SliderFloat("move scale", &params.relaxMoveScale, 0.f, 2.f);
            ImGui::SliderFloat("candidates", &params.relaxCandidates, 1.f, 16.f, "%.0f");
            ImGui::SliderFloat("remove", &params.relaxRemove, 0.f, 2.f);
            ImGui::TextDisabled("0 = off, 1 = break even");
            if (pipe.lastPoolCount() > 0) {
                ImGui::Text("pool %u   moved %u   removed %u",
                            pipe.lastPoolCount(), pipe.lastRelaxAccepted(),
                            pipe.lastRelaxRemoved());
                if (pipe.lastPoolCount() >= SBR_POOL_MAX)
                    ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f),
                                       "pool full - strokes past %u are not relaxed",
                                       SBR_POOL_MAX);
            }
        }

        ImGui::Separator();
        if (ImGui::Checkbox("temporal", &temporal)) pipe.resetTemporal();
        if (temporal) {
            ImGui::SliderInt("flow levels", &cfg.flowLevels, 0, 6);
            if (cfg.flowLevels > 0) {
                ImGui::SliderInt("flow iters", &cfg.flowIterations, 1, 6);
                ImGui::TextDisabled("advects the paint with the motion;");
                ImGui::TextDisabled("raise 'repaint if moved' to see the gain");
                if (pipe.msFlow() > 0.0) ImGui::Text("flow %.2f ms", pipe.msFlow());
            } else {
                ImGui::TextDisabled("0 = paint stays pinned to the pixel grid");
            }
        }
        if (temporal)
            ImGui::SliderFloat("repaint if moved", &params.frameDiffThreshold, 0.f, 64.f, "%.0f");

        if (ImGui::CollapsingHeader("layers")) {
            const std::vector<LayerStats>& ls = pipe.layerStats();
            for (size_t i = 0; i < ls.size(); ++i)
                ImGui::Text("%d: r %.1f grid %.0f cells %u x%u seeds %u drawn %u",
                            int(i), ls[i].radius, ls[i].grid, ls[i].cells,
                            ls[i].chunks, ls[i].strokes, ls[i].drawn);
        }

        ImGui::Separator();
        ImGui::TextWrapped("Tab view - B hold for source - wheel zoom - drag pan");
        ImGui::TextWrapped("F fit - 1 for 1:1 - F5 reload shaders - S save png - Esc quit");
        if (viewCtl.xf.zoom != 1.f) {
            ImGui::SameLine();
            if (ImGui::SmallButton("fit")) {
                viewCtl.xf.zoom = 1.f;
                viewCtl.xf.focusX = viewCtl.xf.focusY = -1.f;
            }
        }
        ImGui::TextWrapped("%s", shaderMsg.c_str());
        ImGui::PopItemWidth();
        ImGui::End();

        // Framing is resolved after the panel so WantCaptureMouse already
        // accounts for the widgets, then the canvas is blitted and ImGui's own
        // draw data goes on top of it.
        updateView(viewCtl, fbH, panelW, 0, fbW - panelW, fbH,
                   pipe.width(), pipe.height(), view);
        // Hold B for the untouched source in place of the painting: a flicker
        // comparison catches things a side-by-side never does.
        const ViewMode shown = (ImGui::IsKeyDown(ImGuiKey_B) &&
                                !ImGui::GetIO().WantTextInput)
                             ? ViewMode::Source : view;

        ImGui::Render();
        pipe.blitToScreen(panelW, 0, fbW - panelW, fbH, fbW, fbH, shown, viewCtl.xf);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        static bool sDown = false;
        const bool s = glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS &&
                       !ImGui::GetIO().WantCaptureKeyboard;
        if (s && !sDown) {
            writePng(opt.out, pipe.readCanvas(), pipe.width(), pipe.height());
            shaderMsg = "saved " + opt.out;
        }
        sDown = s;

        glfwSwapBuffers(win);
        cpuMs = (glfwGetTime() - t0) * 1000.0;

        // Re-seek the video preview once the scrub slider is released.
        if (gui.previewRequested >= 0.0) {
            gui.previewAt = gui.previewRequested;
            gui.previewRequested = -1.0;
            if (!loadVideoPreview(pipe, gui, &sourceNote)) {
                gui.status = "could not decode a frame there";
                gui.statusIsError = true;
            }
        }

        // --- export ----------------------------------------------------
        // Runs on this thread: the GL context is single-threaded, so the
        // progress callback pumps the window between frames instead. That
        // keeps the UI drawing and makes Cancel responsive without a second
        // context and cross-thread sharing.
        if (gui.startExport) {
            gui.startExport = false;
            gui.exporting = true;
            gui.cancel = false;
            gui.progress = jobs::Progress{};

            auto pump = [&](const jobs::Progress& p) {
                gui.progress = p;
                glfwPollEvents();
                if (glfwWindowShouldClose(win)) return false;

                int w = 0, h = 0;
                glfwGetFramebufferSize(win, &w, &h);
                pipe.blitToScreen(panelW, 0, w - panelW, h, w, h, view, viewCtl.xf);

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(400, float(h)), ImGuiCond_Always);
                ImGui::Begin("gpu-sbr", nullptr,
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse);
                ImGui::TextDisabled("EXPORTING");
                const float frac = p.total > 0
                    ? float(double(p.done) / double(p.total)) : 0.f;
                ImGui::ProgressBar(frac, ImVec2(-1, 0));
                ImGui::Text("%lld / %lld", (long long)p.done, (long long)p.total);
                ImGui::Text("%.1f ms/frame   drawn %u", p.msPerFrame, p.drawn);
                if (!p.item.empty()) ImGui::TextWrapped("%s", p.item.c_str());
                if (ImGui::Button("Cancel", ImVec2(-1, 0))) gui.cancel = true;
                ImGui::End();
                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glfwSwapBuffers(win);
                return !gui.cancel;
            };

            jobs::Result res;
            if (gui.kind == InputKind::Video) {
                jobs::VideoSpec vs;
                vs.input = gui.inputPath;
                vs.output = gui.videoOut;
                vs.fps = gui.fpsOverride;
                vs.crf = gui.crf;
                vs.codec = opt.vcodec;
                vs.preset = opt.vpreset;
                vs.keepAudio = gui.keepAudio;
                res = jobs::runVideo(pipe, params, cfg, vs, pump);
            } else if (gui.kind == InputKind::Images) {
                jobs::BatchSpec bs;
                bs.files = gui.queue;
                bs.outDir = gui.outDir;
                bs.suffix = gui.suffix;
                bs.format = gui.format;
                res = jobs::runBatch(pipe, params, cfg, bs, pump);
            } else {
                jobs::BatchSpec bs;
                bs.files = {gui.inputPath};
                bs.outDir = gui.outDir;
                bs.suffix = gui.suffix;
                bs.format = gui.format;
                res = jobs::runBatch(pipe, params, cfg, bs, pump);
            }

            gui.exporting = false;
            char msg[512];
            if (res.cancelled)
                snprintf(msg, sizeof(msg), "cancelled after %lld", (long long)res.done);
            else if (!res.ok)
                snprintf(msg, sizeof(msg), "%s", res.error.c_str());
            else if (gui.kind == InputKind::Video)
                snprintf(msg, sizeof(msg), "wrote %s  (%lld frames, %.1f s)",
                         fileName(gui.videoOut).c_str(), (long long)res.done,
                         res.wallSeconds);
            else
                snprintf(msg, sizeof(msg), "wrote %lld to %s%s",
                         (long long)res.done, gui.outDir.c_str(),
                         res.skipped ? "  (some skipped)" : "");
            gui.status = msg;
            gui.statusIsError = !res.ok && !res.cancelled;

            // The export left the canvas holding its last frame; put the
            // preview back so the sliders keep acting on what is on screen.
            if (gui.kind == InputKind::Video) loadVideoPreview(pipe, gui, &sourceNote);
            else if (!gui.inputPath.empty()) loadSource(pipe, gui.inputPath, &sourceNote);
            else if (!gui.queue.empty()) loadSource(pipe, gui.queue.front(), &sourceNote);
            pipe.resetTemporal();
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    pipe.shutdown();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
