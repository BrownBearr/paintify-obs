#include "media.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <io.h>
#define SBR_POPEN  _popen
#define SBR_PCLOSE _pclose
#else
#define SBR_POPEN  popen
#define SBR_PCLOSE pclose
#endif

namespace media {
namespace {

// Quotes one argument for the shell the pipe runs under. Windows' _popen hands
// the string to cmd.exe, which strips the *outer* pair of quotes from the whole
// command line -- so a command that starts with a quoted program path loses
// them and breaks. Wrapping the entire command in one more pair, done by
// `shellWrap` below, is the documented way round it.
std::string q(const std::string& s) { return "\"" + s + "\""; }

std::string shellWrap(const std::string& cmd) {
#ifdef _WIN32
    return "\"" + cmd + "\"";
#else
    return cmd;
#endif
}

// Runs a command and returns its stdout. Empty on failure.
std::string capture(const std::string& cmd) {
    FILE* p = SBR_POPEN(shellWrap(cmd).c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    SBR_PCLOSE(p);
    return out;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ffprobe reports frame rates as exact rationals ("30000/1001").
double parseRate(const std::string& s) {
    const size_t slash = s.find('/');
    if (slash == std::string::npos) return atof(s.c_str());
    const double num = atof(s.substr(0, slash).c_str());
    const double den = atof(s.substr(slash + 1).c_str());
    return (den != 0.0) ? num / den : 0.0;
}

} // namespace

bool haveFfmpeg(std::string* whichMissing) {
    if (trim(capture("ffmpeg -version")).empty()) {
        if (whichMissing) *whichMissing = "ffmpeg";
        return false;
    }
    if (trim(capture("ffprobe -version")).empty()) {
        if (whichMissing) *whichMissing = "ffprobe";
        return false;
    }
    return true;
}

bool probe(const std::string& path, VideoInfo* out, std::string* err) {
    // One field per line, in a fixed order, so the parse needs no JSON.
    const std::string cmd =
        "ffprobe -v error -select_streams v:0 -show_entries "
        "stream=width,height,r_frame_rate,nb_frames,codec_name "
        "-of default=noprint_wrappers=1:nokey=1 " + q(path);
    const std::string res = capture(cmd);
    if (trim(res).empty()) {
        if (err) *err = "ffprobe could not read " + path;
        return false;
    }

    std::vector<std::string> f;
    size_t pos = 0;
    while (pos <= res.size()) {
        const size_t nl = res.find('\n', pos);
        const std::string line = trim(res.substr(pos, nl == std::string::npos
                                                      ? std::string::npos
                                                      : nl - pos));
        if (!line.empty()) f.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (f.size() < 3) {
        if (err) *err = "no video stream in " + path;
        return false;
    }

    out->codec  = f[0];
    out->width  = atoi(f[1].c_str());
    out->height = atoi(f[2].c_str());
    out->fps    = (f.size() > 3) ? parseRate(f[3]) : 0.0;
    // nb_frames is "N/A" for many containers; fall back to duration * fps.
    out->frames = (f.size() > 4 && f[4] != "N/A") ? atoll(f[4].c_str()) : 0;

    if (out->width <= 0 || out->height <= 0) {
        if (err) *err = "could not determine the frame size of " + path;
        return false;
    }
    if (out->fps <= 0.0) out->fps = 30.0;

    if (out->frames <= 0) {
        const std::string dur = trim(capture(
            "ffprobe -v error -show_entries format=duration "
            "-of default=noprint_wrappers=1:nokey=1 " + q(path)));
        const double seconds = atof(dur.c_str());
        if (seconds > 0.0) out->frames = int64_t(seconds * out->fps + 0.5);
    }

    const std::string audio = trim(capture(
        "ffprobe -v error -select_streams a:0 -show_entries stream=codec_type "
        "-of default=noprint_wrappers=1:nokey=1 " + q(path)));
    out->hasAudio = (audio == "audio");
    return true;
}

bool Reader::open(const std::string& path, int w, int h, std::string* err) {
    close();
    m_frameBytes = size_t(w) * size_t(h) * 4;

    // -vsync 0 keeps decoded frames one-to-one with what ffprobe counted,
    // instead of ffmpeg duplicating or dropping to fit a timebase.
    const std::string cmd =
        "ffmpeg -v error -nostdin -i " + q(path) +
        " -f rawvideo -pix_fmt rgba -vsync 0 -";
    m_pipe = SBR_POPEN(shellWrap(cmd).c_str(), "rb");
    if (!m_pipe) {
        if (err) *err = "could not start ffmpeg to read " + path;
        return false;
    }
    return true;
}

bool Reader::read(std::vector<unsigned char>& rgba) {
    if (!m_pipe) return false;
    rgba.resize(m_frameBytes);

    // A pipe can hand back a short read without being at the end, so keep
    // going until the frame is whole.
    size_t got = 0;
    while (got < m_frameBytes) {
        const size_t n = fread(rgba.data() + got, 1, m_frameBytes - got, m_pipe);
        if (n == 0) break;
        got += n;
    }
    if (got < m_frameBytes) return false;
    ++m_count;
    return true;
}

void Reader::close() {
    if (m_pipe) { SBR_PCLOSE(m_pipe); m_pipe = nullptr; }
}

bool Writer::open(const std::string& path, int w, int h, const EncodeOptions& opt,
                  std::string* err) {
    close();
    m_frameBytes = size_t(w) * size_t(h) * 4;
    m_failed = false;

    char size[64], rate[64], crf[32];
    snprintf(size, sizeof(size), "%dx%d", w, h);
    snprintf(rate, sizeof(rate), "%g", opt.fps > 0.0 ? opt.fps : 30.0);
    snprintf(crf, sizeof(crf), "%d", opt.crf);

    std::string cmd = "ffmpeg -v error -y -f rawvideo -pix_fmt rgba -s ";
    cmd += size;
    cmd += " -r ";
    cmd += rate;
    cmd += " -i -";

    // Audio comes from the original file as a second input. `0:v` and `1:a?`
    // map our frames plus the source's audio if it has any -- the trailing `?`
    // makes the audio mapping optional, so a silent input still encodes.
    const bool withAudio = !opt.audioFrom.empty();
    if (withAudio) cmd += " -i " + q(opt.audioFrom) + " -map 0:v -map 1:a? -c:a copy -shortest";

    cmd += " -c:v " + opt.codec + " -preset " + opt.preset + " -crf " + crf;
    // yuv420p rather than the pipeline's RGB, because most players will not
    // decode anything else.
    cmd += " -pix_fmt yuv420p -movflags +faststart " + q(path);

    m_pipe = SBR_POPEN(shellWrap(cmd).c_str(), "wb");
    if (!m_pipe) {
        if (err) *err = "could not start ffmpeg to write " + path;
        return false;
    }
    return true;
}

bool Writer::write(const std::vector<unsigned char>& rgba) {
    if (!m_pipe || m_failed) return false;
    if (rgba.size() < m_frameBytes) return false;
    const size_t n = fwrite(rgba.data(), 1, m_frameBytes, m_pipe);
    if (n != m_frameBytes) { m_failed = true; return false; }
    return true;
}

bool Writer::close() {
    if (!m_pipe) return !m_failed;
    const int status = SBR_PCLOSE(m_pipe);
    m_pipe = nullptr;
    return status == 0 && !m_failed;
}

namespace {

std::string lowerExt(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

} // namespace

// Exactly what stb_image can decode. `.webp` used to be listed here and is
// not: stb has no WebP decoder, so those files were collected and then failed
// on load with "unknown image type", which looks like a bug in the batch
// rather than an unsupported format.
bool isImageExtension(const std::string& path) {
    const std::string e = lowerExt(path);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" ||
           e == ".tga" || e == ".gif" || e == ".psd" || e == ".hdr" ||
           e == ".ppm" || e == ".pgm";
}

bool isVideoExtension(const std::string& path) {
    const std::string e = lowerExt(path);
    return e == ".mp4" || e == ".mov" || e == ".mkv" || e == ".avi" ||
           e == ".webm" || e == ".m4v" || e == ".wmv" || e == ".flv" ||
           e == ".mpg" || e == ".mpeg" || e == ".ts" || e == ".gifv";
}

std::vector<std::string> listImages(const std::string& dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (isImageExtension(e.path().string())) out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool readFrameAt(const std::string& path, double seconds, int w, int h,
                 std::vector<unsigned char>& rgba) {
    char ss[64];
    snprintf(ss, sizeof(ss), "%.3f", seconds < 0.0 ? 0.0 : seconds);

    // -ss before -i is the fast seek: ffmpeg jumps to the nearest keyframe
    // instead of decoding from the start, which is what makes scrubbing
    // usable. -frames:v 1 stops after the single frame we want.
    const std::string cmd =
        "ffmpeg -v error -nostdin -ss " + std::string(ss) + " -i " + q(path) +
        " -frames:v 1 -f rawvideo -pix_fmt rgba -";

    FILE* p = SBR_POPEN(shellWrap(cmd).c_str(), "rb");
    if (!p) return false;

    const size_t want = size_t(w) * size_t(h) * 4;
    rgba.resize(want);
    size_t got = 0;
    while (got < want) {
        const size_t n = fread(rgba.data() + got, 1, want - got, p);
        if (n == 0) break;
        got += n;
    }
    SBR_PCLOSE(p);
    return got == want;
}

} // namespace media
