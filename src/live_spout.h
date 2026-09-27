#pragma once

#include "params.h"
#include "pipeline.h"

#include <string>
#include <cstdint>

struct GLFWwindow;

struct LiveSpoutConfig {
    std::string inputName = "Paintify Input";
    std::string outputName = "Paintify Output";
    std::string stopFile; // optional per-process shutdown signal
    uint32_t parentPid = 0; // exit if the owning host process exits
    double fps = 12.0;
};

// Run the live bridge on the GL thread until the window closes.
int runLiveSpout(GLFWwindow* window, Pipeline& pipe, TuningParams params,
                 const RenderConfig& render, const LiveSpoutConfig& live);
