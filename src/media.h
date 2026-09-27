#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Video in and out, by piping raw RGBA frames through ffmpeg.
//
// ffmpeg is a child process rather than a linked library on purpose: libav*
// would add a large build dependency and a container/codec matrix to maintain,
// for a renderer whose job is the painting. Frames cross the pipe as raw RGBA,
// which is exactly the format setSource() and readCanvas() already use, so
// there is no conversion on this side at all.
//
// Requires ffmpeg and ffprobe on PATH. `haveFfmpeg()` says whether they are
// there, so the caller can fail with something useful instead of a broken pipe.
namespace media {

bool haveFfmpeg(std::string* whichMissing = nullptr);

struct VideoInfo {
    int width = 0, height = 0;
    double fps = 0.0;
    int64_t frames = 0;     // 0 when the container does not say
    bool hasAudio = false;
    std::string codec;
};

// Reads stream metadata with ffprobe. False if the file cannot be opened or
// has no video stream.
bool probe(const std::string& path, VideoInfo* out, std::string* err);

// Decodes to raw RGBA, one frame at a time.
class Reader {
public:
    ~Reader() { close(); }
    bool open(const std::string& path, int w, int h, std::string* err);
    // Fills `rgba` with w*h*4 bytes. False at end of stream.
    bool read(std::vector<unsigned char>& rgba);
    void close();
    int64_t framesRead() const { return m_count; }

private:
    FILE* m_pipe = nullptr;
    size_t m_frameBytes = 0;
    int64_t m_count = 0;
};

struct EncodeOptions {
    double fps = 30.0;
    int crf = 18;                 // x264 quality; lower is better
    std::string codec = "libx264";
    std::string preset = "medium";
    // Copies the audio from this file, when it has any. Empty = no audio.
    std::string audioFrom;
};

// Encodes raw RGBA frames. Writing is synchronous but ffmpeg buffers, so the
// GPU is not usually waiting on it.
class Writer {
public:
    ~Writer() { close(); }
    bool open(const std::string& path, int w, int h, const EncodeOptions& opt,
              std::string* err);
    bool write(const std::vector<unsigned char>& rgba);
    // Closes the pipe and waits for ffmpeg to finish writing the container.
    // Returns ffmpeg's exit status, so a failed encode is not silent.
    bool close();

private:
    FILE* m_pipe = nullptr;
    size_t m_frameBytes = 0;
    bool m_failed = false;
};

// Image files in a directory, sorted, for batch and frame-sequence mode.
std::vector<std::string> listImages(const std::string& dir);

// Path classification, so the drop handler and the GUI agree on what a file is.
bool isImageExtension(const std::string& path);
bool isVideoExtension(const std::string& path);

// One decoded frame at `seconds`, for the preview. Seeks rather than streams,
// so scrubbing does not need the whole clip decoded. Returns false if ffmpeg
// could not produce a full frame (past the end, for instance).
bool readFrameAt(const std::string& path, double seconds, int w, int h,
                 std::vector<unsigned char>& rgba);

} // namespace media
