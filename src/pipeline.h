#pragma once
#include "gl_util.h"
#include "params.h"

#include <string>
#include <utility>
#include <vector>

// What the window shows. Source and Split exist so you can see what you fed in
// next to what came out -- tuning by memory of the input is guesswork.
enum class ViewMode { Painted, Source, Split, Reference, Error };

// How the canvas is framed in the window.
//
// `zoom` is relative to the *fit* scale rather than to 1:1, so zoom 1 always
// shows the whole image whatever the window size, and `focus` is the image
// pixel held at the centre of the viewport. Keeping the focus in image space
// (not screen space) is what makes a window resize reframe around the same
// feature instead of drifting.
struct ViewXform {
    float zoom = 1.f;
    float focusX = -1.f, focusY = -1.f;   // negative = the image centre
    float wipe = 0.5f;                    // Split: fraction of the width given
};                                        // to the source


struct LayerStats {
    float radius = 0.f;
    float grid = 0.f;
    uint32_t cells = 0;
    uint32_t chunks = 0;
    uint32_t strokes = 0;   // seeds
    uint32_t drawn = 0;     // traced to 2+ points
};

// A GPU port of PainterlyImageCreatorWeb's `paintHertzmann`. Per layer:
//   Gaussian reference blur -> gradient field -> Lab error against the canvas
//   -> per-cell (mean error, argmax) -> seed -> trace -> rasterise.
// Nothing is read back mid-frame; the draw command is written by trace.comp.
//
// The one structural concession to the GPU is chunking: a fine layer's seed
// grid can want more strokes than the stroke buffers hold, so a layer is
// painted in several passes over disjoint, hash-scattered subsets of its
// cells. worker.js shuffles its cell list for the same reason -- so that
// overlap order is not correlated with position.
class Pipeline {
public:
    bool init();
    void shutdown();

    // Uploads an RGBA8 image and (re)sizes every render target to match.
    bool setSource(const unsigned char* rgba, int w, int h);

    // GPU-only input path for live Spout frames. The texture must be RGBA8.
    bool setSourceTexture(GLuint texture, int w, int h);

    // (Re)generates the brush tiles for a set of radii. Called whenever the
    // radii or bristle density change; costs well under a millisecond.
    void buildBrushTiles(const std::vector<float>& radii, float bristleDensity);

    // Paints the canvas. `temporal` reuses the previous frame's canvas and
    // repaints only the cells whose source moved (frameDiffThreshold).
    void render(const TuningParams& base, const RenderConfig& cfg, bool temporal = false);

    // Forget the previous frame -- call when the subject changes.
    void resetTemporal() { m_havePrev = false; }

    // Reads the canvas back as RGBA8, top-row-first.
    std::vector<unsigned char> readCanvas() const;

    void blitToScreen(int x, int y, int w, int h, int fbW, int fbH,
                      ViewMode mode = ViewMode::Painted,
                      const ViewXform& xf = ViewXform{}) const;

    bool reloadShaders(std::string* err);
    void dumpStrokes(int n) const;

    // Diagnostic: print each layer's cell-error distribution and how many
    // cells clear the threshold. Reads the cell buffer back, so it stalls.
    void setDebugCells(bool on) { m_debugCells = on; }

    // Re-reads the stroke counters for the frame just rendered. render() picks
    // them up a frame late on purpose, so the interactive path never stalls;
    // call this after glFinish() when a one-off render's own numbers are
    // wanted, as the headless and frame-sequence paths do.
    void refreshStats();

    // Global E_app for the current canvas (Hertzmann 2001's appearance term).
    // Issues a reduction and reads it back, so it stalls -- it is for
    // verification and the relaxation log, not the interactive path.
    double measureEnergy();

    // Mean tangent alignment of the current flow field with its 8-neighbours.
    // Stalls; for verifying ETF, not for the interactive path.
    double measureFlowCoherence();

    // Per layer, the field's coherence before and after ETF (after is -1 when
    // ETF did not run). Filled only with logging on.
    void setEtfLogging(bool on) { m_etfLog.clear(); m_logEtf = on; }
    // Prints the flow field's mean and max magnitude each frame. Stalls.
    void setFlowLogging(bool on) { m_logFlow = on; }
    const std::vector<std::pair<double, double>>& etfLog() const { return m_etfLog; }

    uint32_t lastPoolCount() const { return m_poolCount; }
    uint32_t lastRelaxAccepted() const { return m_relaxAccepted; }
    uint32_t lastRelaxRemoved() const { return m_relaxRemoved; }
    // Per-iteration global energy from the last render, when relaxation ran
    // with logging on. Empty otherwise.
    const std::vector<double>& relaxEnergyLog() const { return m_relaxLog; }
    // Pool geometry after relaxation. The seeds/drawn/points figures come from
    // the greedy trace counters, which relaxation never touches, so they say
    // nothing about what it did to the strokes -- these do. Filled only when
    // relaxation logging is on, since they need a readback.
    uint32_t relaxLiveStrokes() const { return m_relaxLive; }
    double relaxMeanPoints() const { return m_relaxMeanPts; }
    double relaxMeanRadius() const { return m_relaxMeanRadius; }
    // Total stroke area in pixels, i.e. sum(area) in E_area. Divided by the
    // pixel count it is directly comparable to the reported E_app, so the two
    // can be added into the objective relaxation is actually minimising.
    double relaxTotalArea() const { return m_relaxTotalArea; }
    void setRelaxLogging(bool on) { m_relaxLog.clear(); m_logRelax = on; }

    int width() const { return m_w; }
    int height() const { return m_h; }
    GLuint canvasTexture() const { return m_canvasTex; }

    // --- telemetry ---
    // Stages 2-5 run once per (layer, chunk), so each figure is the whole
    // frame's cost summed over those passes, not the last one's.
    double msReference() const { return m_ms.ref; }
    double msError()     const { return m_ms.error; }
    double msSeeds()     const { return m_ms.seeds; }
    double msTrace()     const { return m_ms.trace; }
    double msRaster()    const { return m_ms.raster; }
    double msImpasto()   const { return m_ms.impasto; }
    double msRelax()     const { return m_ms.relax; }
    double msEtf()       const { return m_ms.etf; }
    double msFlow()      const { return m_ms.flow; }
    double msTotal()     const { return msReference() + msError() + msSeeds()
                                      + msTrace() + msRaster() + msImpasto()
                                      + msRelax() + msEtf() + msFlow(); }
    uint32_t lastStrokeCount() const { return m_lastStrokes; }      // seeds
    uint32_t lastDrawnCount() const { return m_lastDrawn; }         // drew something
    double   lastMeanPoints() const {
        return m_lastDrawn ? double(m_lastPoints) / double(m_lastDrawn) : 0.0;
    }
    const std::vector<LayerStats>& layerStats() const { return m_layers; }

private:
    void resize(int w, int h);
    void uploadParams(const TuningParams& p);
    void ensureCellCapacity(uint32_t cells);
    void readStats();

    // Separable Gaussian, src -> dst via the scratch target.
    void gaussian(GLuint srcTex, GLuint dstTex);
    // Repaints the canvas from the stroke pool alone: underpaint, then every
    // pooled stroke. Relaxation needs this because it moves strokes that were
    // already composited.
    void repaintFromPool(const TuningParams& p, const RenderConfig& cfg,
                         const std::vector<float>& radii);
    // `allowCache` lets a repaint reuse the ground the frame already
    // computed, instead of redoing the Gaussian. Relaxation repaints once per
    // sub-pass, so that blur would otherwise be the dominant cost.
    void layUnderpaint(TuningParams& p, const RenderConfig& cfg,
                       const std::vector<float>& radii, bool temporal,
                       bool allowCache = false);
    void relax(TuningParams& p, const RenderConfig& cfg,
               const std::vector<float>& radii);
    void buildReference(TuningParams& p, float radius);
    void buildGradients(const TuningParams& p);
    // Kang 2007 Edge Tangent Flow over m_gradTex, in place. No-op at
    // cfg.etfIterations == 0, which is what keeps the port verifiable.
    void applyEtf(const TuningParams& p, const RenderConfig& cfg);
    // Dense backward flow from the previous source frame to this one.
    // Leaves m_flowValid false (and the field untouched) when off.
    void computeFlow(const TuningParams& p, const RenderConfig& cfg);

    int m_w = 0, m_h = 0;

    glu::Program m_blur, m_features, m_error, m_seeds, m_trace, m_stroke,
                 m_impasto, m_canvasProg, m_pool, m_relax, m_energy, m_etf, m_flow;

    GLuint m_srcTex = 0, m_refTex = 0, m_tmpTex = 0, m_tensorTex = 0;
    GLuint m_gradTex = 0, m_errTex = 0, m_diffTex = 0;
    // ETF ping-pongs the direction field between m_gradTex and this one.
    GLuint m_etfTex = 0;
    // Optical flow. The luma pyramids are full resolution with mips; the
    // field itself is a quarter of that, and ping-pongs between two.
    GLuint m_lumaPrevTex = 0, m_lumaCurTex = 0;
    GLuint m_flowTex = 0, m_flowTmpTex = 0;
    int m_flowW = 0, m_flowH = 0;
    bool m_flowValid = false;
    GLuint m_canvasTex = 0, m_heightTex = 0, m_canvasFbo = 0, m_srcFbo = 0;
    GLuint m_prevSrcTex = 0, m_prevCanvasTex = 0, m_underTex = 0;
    bool m_underValid = false;
    GLuint m_brushTex = 0;

    GLuint m_paramsUbo = 0;
    GLuint m_cellBuf = 0, m_seedBuf = 0, m_vertexBuf = 0;
    GLuint m_headerBuf = 0, m_counterBuf = 0, m_indirectBuf = 0;
    // Frame-wide stroke pool: every stroke of the frame, for relaxation.
    GLuint m_poolVertexBuf = 0, m_poolHeaderBuf = 0, m_poolCounterBuf = 0;
    GLuint m_energyBuf = 0;
    // One uint: the frame's maximum gradient magnitude, for ETF's w_m.
    GLuint m_etfMaxBuf = 0;
    // Per (layer, chunk) traced-stroke counts, written by a GPU-side copy and
    // read back at the top of the next frame -- by then the data is already
    // resolved, so the read never stalls the pipeline.
    GLuint m_statsBuf = 0;
    GLuint m_vao = 0;
    uint32_t m_cellCapacity = 0;

    std::vector<int> m_brushRows;     // used rows per radius index
    std::vector<float> m_brushRadii;  // radii the tiles were built for
    float m_brushDensity = -1.f;

    bool m_havePrev = false;
    bool m_debugCells = false;
    bool m_logRelax = false;
    bool m_logEtf = false;
    bool m_logFlow = false;
    std::vector<std::pair<double, double>> m_etfLog;
    uint32_t m_poolCount = 0, m_relaxAccepted = 0, m_relaxRemoved = 0;
    uint32_t m_relaxLive = 0;
    double m_relaxMeanPts = 0.0, m_relaxMeanRadius = 0.0, m_relaxTotalArea = 0.0;
    std::vector<double> m_relaxLog;

    glu::GpuTimer m_tRef, m_tError, m_tSeeds, m_tTrace, m_tRaster, m_tImpasto,
                  m_tRelax, m_tEtf, m_tFlow;

    // Each GPU timer reads a frame late, so summing its lastMs after every
    // end() gives the frame total shifted by one measurement -- which settles
    // immediately and never stalls for a result.
    struct StageTimes { double ref, error, seeds, trace, raster, impasto,
                        relax, etf, flow; };
    StageTimes m_ms{};
    void endStage(glu::GpuTimer& t, double& acc) { t.end(); acc += t.lastMs; }
    uint32_t m_lastStrokes = 0, m_lastDrawn = 0, m_lastPoints = 0;
    std::vector<LayerStats> m_layers;
};
