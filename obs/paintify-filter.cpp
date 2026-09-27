// SPDX-License-Identifier: GPL-2.0-or-later
#include <obs-module.h>

#include <SpoutDX/SpoutDX.h>
#include <d3d11.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("paintify-obs", "en-US")

namespace {

struct Settings {
    std::string executable;
    std::string preset = "impressionist";
    int fps = 12;
    int relax = 0;
    int flow = 0;
    double threshold = 50.0;
    double curvature = 1.0;
    double brush_texture = 0.45;
    double impasto = 0.0;
    double impasto_light = 0.0;
    double temporal_diff = 0.0;
};

struct Filter {
    obs_source_t *source = nullptr;
    std::mutex settings_mutex;
    Settings settings;
    unsigned settings_revision = 0;
    unsigned applied_revision = 0;
    std::string input_name;
    std::string output_name;
    std::wstring stop_file;
    PROCESS_INFORMATION child{};
    spoutDX sender;
    spoutDX receiver;
    bool spout_ready = false;
    gs_texrender_t *capture = nullptr;
    gs_texture_t *painted = nullptr;
    HANDLE painted_handle = nullptr;
    unsigned painted_width = 0;
    unsigned painted_height = 0;
    ULONGLONG next_send_ms = 0;
    ULONGLONG next_launch_ms = 0;
    bool warned_backend = false;
};

std::atomic<unsigned> next_instance{1};

std::wstring wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), -1, out.data(), n);
    out.pop_back();
    return out;
}

std::wstring quote_arg(const std::wstring &s) { return L"\"" + s + L"\""; }

std::wstring number(double n)
{
    std::wstring s = std::to_wstring(n);
    while (s.size() > 2 && s.back() == L'0') s.pop_back();
    if (s.back() == L'.') s.pop_back();
    return s;
}

void stop_renderer(Filter *f)
{
    if (!f->child.hProcess) return;
    HANDLE signal = CreateFileW(f->stop_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (signal != INVALID_HANDLE_VALUE) CloseHandle(signal);
    if (WaitForSingleObject(f->child.hProcess, 300) == WAIT_TIMEOUT) {
        TerminateProcess(f->child.hProcess, 1);
        WaitForSingleObject(f->child.hProcess, 100);
    }
    CloseHandle(f->child.hThread);
    CloseHandle(f->child.hProcess);
    f->child = {};
    DeleteFileW(f->stop_file.c_str());
}

bool start_renderer(Filter *f, const Settings &s)
{
    const std::wstring exe = wide(s.executable);
    std::error_code file_error;
    if (exe.empty() || !std::filesystem::is_regular_file(exe, file_error)) {
        blog(LOG_WARNING, "Paintify OBS: set Renderer executable to a built gpu-sbr.exe");
        return false;
    }
    DeleteFileW(f->stop_file.c_str());
    std::wstring cmd = quote_arg(exe) + L" --live-spout --spout-in " + wide(f->input_name) +
        L" --spout-out " + wide(f->output_name) +
        L" --live-parent-pid " + std::to_wstring(GetCurrentProcessId()) +
        L" --live-stop-file " + quote_arg(f->stop_file) +
        L" --target-fps " + std::to_wstring(s.fps) +
        L" --preset " + wide(s.preset) +
        L" --relax " + std::to_wstring(s.relax) +
        L" --flow " + std::to_wstring(s.flow) +
        L" --threshold " + number(s.threshold) +
        L" --curvature " + number(s.curvature) +
        L" --brush-texture " + number(s.brush_texture) +
        L" --impasto " + number(s.impasto) +
        L" --impasto-light " + number(s.impasto_light) +
        L" --temporal-diff " + number(s.temporal_diff);
    std::wstring cwd = std::filesystem::path(exe).parent_path().wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup, &f->child)) {
        blog(LOG_ERROR, "Paintify OBS: renderer launch failed (Windows error %lu)", GetLastError());
        return false;
    }
    blog(LOG_INFO, "Paintify OBS: renderer started for %s", f->input_name.c_str());
    return true;
}

void release_graphics(Filter *f)
{
    if (f->painted) gs_texture_destroy(f->painted);
    f->painted = nullptr;
    f->painted_handle = nullptr;
    f->painted_width = f->painted_height = 0;
    if (f->capture) gs_texrender_destroy(f->capture);
    f->capture = nullptr;
    f->receiver.ReleaseReceiver();
    f->sender.ReleaseSender();
    if (f->spout_ready) {
        f->receiver.CloseDirectX11();
        f->sender.CloseDirectX11();
        f->spout_ready = false;
    }
}

bool open_spout(Filter *f)
{
    if (f->spout_ready) return true;
    if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
        if (!f->warned_backend) {
            blog(LOG_ERROR, "Paintify OBS: select OBS's Direct3D 11 graphics backend");
            f->warned_backend = true;
        }
        return false;
    }
    auto *device = static_cast<ID3D11Device *>(gs_get_device_obj());
    if (!device || !f->sender.OpenDirectX11(device) || !f->receiver.OpenDirectX11(device)) {
        blog(LOG_ERROR, "Paintify OBS: Spout could not open the OBS Direct3D device");
        f->sender.CloseDirectX11();
        f->receiver.CloseDirectX11();
        return false;
    }
    f->sender.SetSenderName(f->input_name.c_str());
    f->receiver.SetReceiverName(f->output_name.c_str());
    f->spout_ready = true;
    return true;
}

bool capture_and_send(Filter *f, uint32_t width, uint32_t height)
{
    if (!f->capture) f->capture = gs_texrender_create(GS_BGRA_UNORM, GS_ZS_NONE);
    if (!f->capture) return false;
    gs_texrender_reset(f->capture);
    if (!gs_texrender_begin(f->capture, width, height)) return false;
    struct vec4 clear{};
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, float(width), 0.0f, float(height), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    obs_source_t *target = obs_filter_get_target(f->source);
    obs_source_t *parent = obs_filter_get_parent(f->source);
    const uint32_t flags = parent ? obs_source_get_output_flags(parent) : 0;
    if (target == parent && !(flags & (OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_ASYNC)))
        obs_source_default_render(target);
    else
        obs_source_video_render(target);
    gs_blend_state_pop();
    gs_texrender_end(f->capture);
    gs_texture_t *texture = gs_texrender_get_texture(f->capture);
    if (!texture) return false;
    auto *dx_texture = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(texture));
    return dx_texture && f->sender.SendTexture(dx_texture);
}

void update_painted_texture(Filter *f)
{
    if (!f->receiver.ReceiveTexture()) return;
    if (f->receiver.IsUpdated()) {
        if (f->painted) gs_texture_destroy(f->painted);
        f->painted = nullptr;
        f->painted_handle = nullptr;
    }
    HANDLE handle = f->receiver.GetSenderHandle();
    const unsigned width = f->receiver.GetSenderWidth();
    const unsigned height = f->receiver.GetSenderHeight();
    if (!handle || !width || !height) return;
    if (handle != f->painted_handle || width != f->painted_width || height != f->painted_height) {
        if (f->painted) gs_texture_destroy(f->painted);
        f->painted = gs_texture_open_shared(uint32_t(uintptr_t(handle)));
        f->painted_handle = f->painted ? handle : nullptr;
        f->painted_width = width;
        f->painted_height = height;
    }
}

void render(void *data, gs_effect_t *)
{
    auto *f = static_cast<Filter *>(data);
    Settings settings;
    unsigned revision;
    {
        std::lock_guard<std::mutex> lock(f->settings_mutex);
        settings = f->settings;
        revision = f->settings_revision;
    }
    if (revision != f->applied_revision) {
        stop_renderer(f);
        release_graphics(f);
        f->applied_revision = revision;
        f->next_send_ms = 0;
        f->next_launch_ms = 0;
    }
    if (!open_spout(f)) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (f->child.hProcess && WaitForSingleObject(f->child.hProcess, 0) == WAIT_OBJECT_0) {
        CloseHandle(f->child.hThread);
        CloseHandle(f->child.hProcess);
        f->child = {};
        f->next_launch_ms = now + 2000;
        blog(LOG_WARNING, "Paintify OBS: renderer exited; retrying in two seconds");
    }
    if (!f->child.hProcess && now >= f->next_launch_ms) {
        if (!start_renderer(f, settings)) f->next_launch_ms = now + 2000;
    }
    if (!f->child.hProcess) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    obs_source_t *target = obs_filter_get_target(f->source);
    const uint32_t width = target ? obs_source_get_base_width(target) : 0;
    const uint32_t height = target ? obs_source_get_base_height(target) : 0;
    if (!width || !height) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    if (now >= f->next_send_ms) {
        capture_and_send(f, width, height);
        f->next_send_ms = now + 1000 / std::max(1, settings.fps);
    }
    update_painted_texture(f);
    if (!f->painted || f->painted_width != width || f->painted_height != height) {
        obs_source_skip_video_filter(f->source);
        return;
    }
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    gs_effect_set_texture(image, f->painted);
    // SpoutGL publishes OpenGL's lower-left-origin texture to D3D. OBS opens
    // that shared texture directly, so reverse the vertical texture axis.
    while (gs_effect_loop(effect, "Draw")) gs_draw_sprite(f->painted, GS_FLIP_V, width, height);
}

void update(void *data, obs_data_t *values)
{
    auto *f = static_cast<Filter *>(data);
    Settings next;
    next.executable = obs_data_get_string(values, "executable");
    next.preset = obs_data_get_string(values, "preset");
    next.fps = int(std::clamp<int64_t>(obs_data_get_int(values, "fps"), 1, 60));
    next.relax = int(std::clamp<int64_t>(obs_data_get_int(values, "relax"), 0, 12));
    next.flow = int(std::clamp<int64_t>(obs_data_get_int(values, "flow"), 0, 6));
    next.threshold = std::clamp(obs_data_get_double(values, "threshold"), 0.0, 255.0);
    next.curvature = std::clamp(obs_data_get_double(values, "curvature"), 0.0, 1.0);
    next.brush_texture = std::clamp(obs_data_get_double(values, "brush_texture"), 0.0, 1.0);
    next.impasto = std::clamp(obs_data_get_double(values, "impasto"), 0.0, 1.0);
    next.impasto_light = std::clamp(obs_data_get_double(values, "impasto_light"), 0.0, 1.0);
    next.temporal_diff = std::clamp(obs_data_get_double(values, "temporal_diff"), 0.0, 255.0);
    std::lock_guard<std::mutex> lock(f->settings_mutex);
    f->settings = std::move(next);
    ++f->settings_revision;
}

void *create(obs_data_t *values, obs_source_t *source)
{
    auto *f = new Filter;
    f->source = source;
    const std::string prefix = "Paintify_OBS_" + std::to_string(GetCurrentProcessId()) +
                               "_" + std::to_string(next_instance++);
    f->input_name = prefix + "_input";
    f->output_name = prefix + "_output";
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    f->stop_file = std::wstring(temp) + wide(prefix) + L".stop";
    update(f, values);
    return f;
}

void destroy(void *data)
{
    auto *f = static_cast<Filter *>(data);
    stop_renderer(f);
    obs_enter_graphics();
    release_graphics(f);
    obs_leave_graphics();
    delete f;
}

const char *name(void *) { return obs_module_text("FilterName"); }

void defaults(obs_data_t *values)
{
    char *path = obs_module_file("gpu-sbr.exe");
    if (path) {
        obs_data_set_default_string(values, "executable", path);
        bfree(path);
    }
    obs_data_set_default_string(values, "preset", "impressionist");
    obs_data_set_default_int(values, "fps", 12);
    obs_data_set_default_int(values, "relax", 0);
    obs_data_set_default_int(values, "flow", 0);
    obs_data_set_default_double(values, "threshold", 50.0);
    obs_data_set_default_double(values, "curvature", 1.0);
    obs_data_set_default_double(values, "brush_texture", 0.45);
    obs_data_set_default_double(values, "impasto", 0.0);
    obs_data_set_default_double(values, "impasto_light", 0.0);
    obs_data_set_default_double(values, "temporal_diff", 0.0);
}

bool preset_changed(obs_properties_t *, obs_property_t *, obs_data_t *values)
{
    const char *preset = obs_data_get_string(values, "preset");
    double threshold, curvature, brush_texture;
    if (std::strcmp(preset, "impressionist") == 0) {
        threshold = 50.0; curvature = 1.0; brush_texture = 0.45;
    } else if (std::strcmp(preset, "expressionist") == 0) {
        threshold = 40.0; curvature = 1.0; brush_texture = 0.60;
    } else if (std::strcmp(preset, "pointillist") == 0) {
        threshold = 30.0; curvature = 0.0; brush_texture = 0.0;
    } else if (std::strcmp(preset, "wash") == 0) {
        threshold = 80.0; curvature = 0.7; brush_texture = 0.0;
    } else {
        return false;
    }
    obs_data_set_double(values, "threshold", threshold);
    obs_data_set_double(values, "curvature", curvature);
    obs_data_set_double(values, "brush_texture", brush_texture);
    return true;
}

obs_properties_t *properties(void *)
{
    obs_properties_t *p = obs_properties_create();
    obs_properties_add_path(p, "executable", obs_module_text("Executable"),
                            OBS_PATH_FILE, "Executables (*.exe)", nullptr);
    obs_property_t *preset = obs_properties_add_list(p, "preset", obs_module_text("Preset"),
                                                     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    obs_property_set_modified_callback(preset, preset_changed);
    for (const char *style : {"impressionist", "expressionist", "pointillist", "wash", "detail"})
        obs_property_list_add_string(preset, style, style);
    obs_properties_add_int_slider(p, "fps", obs_module_text("FPS"), 1, 60, 1);
    obs_properties_add_float_slider(p, "threshold", obs_module_text("Threshold"), 0, 255, 1);
    obs_properties_add_float_slider(p, "curvature", obs_module_text("Curvature"), 0, 1, 0.01);
    obs_properties_add_float_slider(p, "brush_texture", obs_module_text("BrushTexture"), 0, 1, 0.01);
    obs_properties_add_float_slider(p, "impasto", obs_module_text("Impasto"), 0, 1, 0.01);
    obs_properties_add_float_slider(p, "impasto_light", obs_module_text("ImpastoLight"), 0, 1, 0.01);
    obs_properties_add_int_slider(p, "relax", obs_module_text("Relax"), 0, 12, 1);
    obs_properties_add_float_slider(p, "temporal_diff", obs_module_text("Temporal"), 0, 255, 1);
    obs_properties_add_int_slider(p, "flow", obs_module_text("Flow"), 0, 6, 1);
    return p;
}

} // namespace

bool obs_module_load(void)
{
    obs_source_info info{};
    info.id = "paintify_obs_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.get_defaults = defaults;
    info.get_properties = properties;
    info.video_render = render;
    obs_register_source(&info);
    return true;
}
