#include "paramfile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace paramfile {
namespace {

// One table drives both directions. Adding a slider means adding a row here and
// nowhere else, which is the only way save and load stay in step -- two
// hand-written lists would have drifted by the second new parameter.
struct Field {
    const char* key;
    float TuningParams::* member;
};

const Field kFields[] = {
    {"threshold",        &TuningParams::threshold},
    {"max-length",       &TuningParams::maxStrokeLength},
    {"min-length",       &TuningParams::minStrokeLength},
    {"curvature",        &TuningParams::curvature},
    {"grid-factor",      &TuningParams::gridFactor},
    {"opacity",          &TuningParams::opacity},
    {"tensor-sigma",     &TuningParams::tensorSigma},
    {"jitter-hue",       &TuningParams::jitterHue},
    {"jitter-sat",       &TuningParams::jitterSat},
    {"jitter-val",       &TuningParams::jitterVal},
    {"size-jitter",      &TuningParams::sizeJitter},
    {"angle-jitter",     &TuningParams::angleJitter},
    {"opacity-jitter",   &TuningParams::opacityJitter},
    {"brush-texture",    &TuningParams::texStrength},
    {"texture-taper",    &TuningParams::texTaper},
    {"dry-brush",        &TuningParams::dryBrush},
    {"impasto",          &TuningParams::impastoStrength},
    {"impasto-light",    &TuningParams::impastoLight},
    {"light-angle",      &TuningParams::lightAngle},
    {"temporal-diff",    &TuningParams::frameDiffThreshold},
    {"jitter-per-frame", &TuningParams::jitterPerFrame},
    {"relax-area",       &TuningParams::relaxAreaWeight},
    {"relax-move",       &TuningParams::relaxMoveScale},
    {"relax-candidates", &TuningParams::relaxCandidates},
    {"relax-remove",     &TuningParams::relaxRemove},
    {"etf-radius",       &TuningParams::etfRadius},
};

const char* underpaintName(Underpaint u) {
    switch (u) {
        case Underpaint::None:    return "none";
        case Underpaint::Average: return "average";
        default:                  return "blur";
    }
}

} // namespace

std::vector<float> parseRadii(const std::string& s) {
    std::vector<float> out;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) { ++p; continue; }
        if (v >= 1.f) out.push_back(v);
        p = end;
        while (*p == ',' || *p == ' ' || *p == ';') ++p;
    }
    if (out.empty()) out.push_back(4.f);
    std::sort(out.begin(), out.end(), std::greater<float>());
    if (out.size() > SBR_MAX_LAYERS) out.resize(SBR_MAX_LAYERS);
    return out;
}

std::string radiiToString(const std::vector<float>& r) {
    std::string s;
    char buf[32];
    for (size_t i = 0; i < r.size(); ++i) {
        snprintf(buf, sizeof(buf), "%g", r[i]);
        if (i) s += ", ";
        s += buf;
    }
    return s;
}

bool save(const std::string& path, const TuningParams& p, const RenderConfig& cfg,
          std::string* err) {
    std::ofstream f(path);
    if (!f) {
        if (err) *err = "could not write " + path;
        return false;
    }

    f << "# gpu-sbr parameters\n";
    f << "version 1\n\n";
    f << "radii " << radiiToString(cfg.radii) << "\n";
    f << "underpaint " << underpaintName(cfg.underpaint) << "\n";
    f << "bristle-density " << cfg.bristleDensity << "\n";
    f << "passes " << cfg.passesPerLayer << "\n";
    f << "relax-iterations " << cfg.relaxIterations << "\n";
    f << "relax-subpasses " << cfg.relaxSubPasses << "\n";
    f << "etf-iterations " << cfg.etfIterations << "\n";
    f << "flow-levels " << cfg.flowLevels << "\n";
    f << "flow-iterations " << cfg.flowIterations << "\n\n";

    for (const Field& fl : kFields)
        f << fl.key << ' ' << (p.*fl.member) << '\n';

    if (!f) {
        if (err) *err = "write failed on " + path;
        return false;
    }
    return true;
}

bool load(const std::string& path, TuningParams* p, RenderConfig* cfg,
          std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "could not read " + path;
        return false;
    }

    int unknown = 0;
    std::string line;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        std::istringstream ls(line);
        std::string key;
        if (!(ls >> key) || key.empty()) continue;

        // `radii` is the one value that is a list, so it takes the rest of the
        // line rather than a single token.
        if (key == "radii") {
            std::string rest;
            std::getline(ls, rest);
            if (cfg) cfg->radii = parseRadii(rest);
            continue;
        }

        std::string value;
        if (!(ls >> value)) continue;

        if (key == "version") continue;
        if (key == "underpaint") {
            if (cfg) cfg->underpaint = (value == "none")    ? Underpaint::None
                                     : (value == "average") ? Underpaint::Average
                                                            : Underpaint::Blur;
            continue;
        }
        if (key == "bristle-density") {
            if (cfg) cfg->bristleDensity = std::strtof(value.c_str(), nullptr);
            continue;
        }
        if (key == "passes") {
            if (cfg) cfg->passesPerLayer = std::max(1, atoi(value.c_str()));
            continue;
        }
        if (key == "relax-iterations") {
            if (cfg) cfg->relaxIterations = std::max(0, atoi(value.c_str()));
            continue;
        }
        if (key == "relax-subpasses") {
            if (cfg) cfg->relaxSubPasses = std::max(1, atoi(value.c_str()));
            continue;
        }
        if (key == "etf-iterations") {
            if (cfg) cfg->etfIterations = std::max(0, atoi(value.c_str()));
            continue;
        }
        if (key == "flow-levels") {
            if (cfg) cfg->flowLevels = std::max(0, atoi(value.c_str()));
            continue;
        }
        if (key == "flow-iterations") {
            if (cfg) cfg->flowIterations = std::max(1, atoi(value.c_str()));
            continue;
        }

        bool matched = false;
        for (const Field& fl : kFields) {
            if (key == fl.key) {
                if (p) p->*fl.member = std::strtof(value.c_str(), nullptr);
                matched = true;
                break;
            }
        }
        if (!matched) ++unknown;
    }

    if (unknown && err) {
        char buf[128];
        snprintf(buf, sizeof(buf), "loaded, but %d %s not recognised",
                 unknown, unknown == 1 ? "key was" : "keys were");
        *err = buf;
    }
    return true;
}

} // namespace paramfile
