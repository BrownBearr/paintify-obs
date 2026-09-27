#pragma once
// OBS generates this header when libobs is built. The installed binary and
// source checkout provide the ABI; these path macros are sufficient for this
// standalone plugin to compile against that source checkout.
#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
