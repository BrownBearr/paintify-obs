#pragma once
#include "params.h"

#include <string>
#include <vector>

// A tuned look, as a small text file.
//
// The point is that a look found by dragging sliders can then drive a batch or
// a video render -- which is the CLI's job -- without being retyped as thirty
// --flags with one of them wrong. The format is `key value` lines so it stays
// diffable and hand-editable; `#` starts a comment.
//
// Only user-facing settings are written. The image size, the per-layer state
// and the derived fields in TuningParams are filled in by the pipeline every
// frame, so storing them would at best be noise and at worst a stale override.
namespace paramfile {

// Brush radii round-trip through the same two functions the GUI's text box
// uses, so a file and the box can never disagree about what "8, 4, 2" means.
std::vector<float> parseRadii(const std::string& s);
std::string radiiToString(const std::vector<float>& r);

bool save(const std::string& path, const TuningParams& p, const RenderConfig& cfg,
          std::string* err);

// Values absent from the file keep whatever the caller already had, so a file
// is a patch rather than a full reset. Unknown keys are counted and reported
// in `err` but are not fatal: a file written by a later build should still
// load whatever this build understands.
bool load(const std::string& path, TuningParams* p, RenderConfig* cfg,
          std::string* err);

} // namespace paramfile
