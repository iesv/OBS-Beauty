// SPDX-License-Identifier: GPL-2.0-or-later
// Runs a separate libobs process. Does not open cameras or modify OBS settings.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#endif
#include <obs-module.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
#ifdef BEAUTY_CI_STRESS
constexpr unsigned warmup_frames = 5;
constexpr unsigned update_interval = 1;
constexpr unsigned render_timeout_seconds = 360;
#else
constexpr unsigned warmup_frames = 20;
constexpr unsigned update_interval = 15;
constexpr unsigned render_timeout_seconds = 180;
#endif
#ifdef __APPLE__
constexpr const char *timing_label = "render-wall";
#else
constexpr const char *timing_label = "GPU";
#endif
using Source = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>;
using Settings = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>;
using Properties = std::unique_ptr<obs_properties_t, decltype(&obs_properties_destroy)>;
std::atomic<bool> count_stress_warnings{false};
std::atomic<unsigned> stress_warnings{0};

void test_log(int level, const char *format, va_list args, void *)
{
    if (level > LOG_WARNING)
        return;
    if (count_stress_warnings.load())
        ++stress_warnings;
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
}

void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Fixture {
    gs_texture_t *texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    gs_color_space space = GS_CS_SRGB;
};

const char *fixture_name(void *) { return "Beauty test fixture"; }
uint32_t fixture_width(void *data) { return static_cast<Fixture *>(data)->width; }
uint32_t fixture_height(void *data) { return static_cast<Fixture *>(data)->height; }
gs_color_space fixture_space(void *data, size_t, const gs_color_space *)
{
    return static_cast<Fixture *>(data)->space;
}

void fixture_destroy(void *data)
{
    auto *fixture = static_cast<Fixture *>(data);
    obs_enter_graphics();
    gs_texture_destroy(fixture->texture);
    obs_leave_graphics();
    delete fixture;
}

void *fixture_create(obs_data_t *settings, obs_source_t *)
{
    auto *fixture = new Fixture;
    fixture->width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
    fixture->height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
    const int mode = static_cast<int>(obs_data_get_int(settings, "fixture"));
    fixture->space = obs_data_get_bool(settings, "hdr") ? GS_CS_709_EXTENDED :
                     (mode == 9 ? GS_CS_SRGB_16F : GS_CS_SRGB);
    std::vector<uint8_t> pixels(static_cast<size_t>(fixture->width) * fixture->height * 4);
    for (uint32_t y = 0; y < fixture->height; ++y) {
        for (uint32_t x = 0; x < fixture->width; ++x) {
            std::array<int, 4> color = {178, 130, 107, 255};
            if (mode == 1) {
                if (x < fixture->width / 2) {
                    const int noise = ((x + y) % 2 == 0) ? 12 : -12;
                    color = {178 + noise, 130 + noise, 107 + noise, 255};
                } else {
                    color = {30, 60, 200, 255};
                }
            } else if (mode == 2) {
                color[3] = x < fixture->width / 2 ? 128 : 0;
            } else if (mode == 3) {
                color = {255, 255, 255, 255};
            } else if (mode == 4) {
                color = {200, 150, 80, 255};
            } else if (mode == 5) {
                // Same scene-space texture at 1080p, 4K and portrait resolutions.
                const double scale = std::min(fixture->width, fixture->height) / 1080.0;
                const double u = (x + 0.5) / scale, v = (y + 0.5) / scale;
                const int noise = static_cast<int>(std::round(10 * std::cos(u * 6.283185307 / 8) +
                                                               4 * std::cos(v * 6.283185307 / 10)));
                color = {178 + noise, 130 + noise, 107 + noise, 255};
                if (u >= 100 && u < 104)
                    color = {30, 25, 20, 255};
                if (x >= fixture->width / 2)
                    color = {30, 60, 200, 255};
            } else if (mode == 6) {
                color = {30, 60, 200, 255};
            } else if (mode == 7) {
                const int noise = ((x + y) % 2 == 0) ? 12 : -12;
                color = {178 + noise, 130 + noise, 107 + noise, 255};
            } else if (mode == 8) {
                constexpr std::array<int, 4> swatches[] = {
                    {90, 50, 30, 255}, {178, 130, 107, 255}, {210, 166, 144, 255}, {238, 211, 198, 255},
                    {250, 229, 217, 255}, {255, 242, 234, 255}, {0, 0, 0, 255}, {255, 255, 255, 255},
                };
                color = swatches[std::min(x * 8 / fixture->width, 7u)];
            } else if (mode == 10) {
                if (y < fixture->height / 2) {
                    const int noise = ((x + y) % 2 == 0) ? 20 : -20;
                    color = {178 + noise, 130 + noise, 107 + noise, 255};
                } else if (x >= fixture->width / 2) {
                    // A high-contrast luminance edge with the same warm chroma.
                    color = {90, 42, 19, 255};
                }
            } else if (mode == 11) {
                const int dx = static_cast<int>(x) - 20;
                const int dark_y = static_cast<int>(y) - 16, red_y = static_cast<int>(y) - 48;
                if (dx * dx + dark_y * dark_y <= 4)
                    color = {154, 106, 83, 255};
                if (dx * dx + red_y * red_y <= 4)
                    color = {190, 124, 103, 255};
                if (x >= 40 && x < 42)
                    color = {30, 25, 20, 255};
                if (x >= 48)
                    color = {90, 42, 19, 255};
            } else if (mode == 12) {
                color = x < fixture->width / 2 ? std::array<int, 4>{150, 110, 90, 255} :
                                                std::array<int, 4>{180, 140, 120, 255};
            }
            const size_t offset = (static_cast<size_t>(y) * fixture->width + x) * 4;
            for (size_t component = 0; component < 4; ++component)
                pixels[offset + component] = static_cast<uint8_t>(color[component]);
        }
    }
    std::vector<float> floating;
    if (mode == 9) {
        // SDR float inputs can contain overshoot from preceding filters.
        constexpr float color[] = {1.125f, 0.75f, 0.5f, 1.0f};
        floating.resize(pixels.size());
        for (size_t i = 0; i < floating.size(); ++i)
            floating[i] = color[i % 4];
    }
    const uint8_t *planes[] = {mode == 9 ? reinterpret_cast<const uint8_t *>(floating.data()) : pixels.data()};
    obs_enter_graphics();
    fixture->texture = gs_texture_create(fixture->width, fixture->height, mode == 9 ? GS_RGBA32F : GS_RGBA, 1, planes, 0);
    obs_leave_graphics();
    if (!fixture->texture) {
        delete fixture;
        return nullptr;
    }
    return fixture;
}

void fixture_render(void *data, gs_effect_t *)
{
    auto *fixture = static_cast<Fixture *>(data);
    obs_source_draw(fixture->texture, 0, 0, fixture->width, fixture->height, false);
}

Source make_fixture(int mode = 0, uint32_t width = 64, uint32_t height = 64, bool hdr = false)
{
    Settings settings(obs_data_create(), obs_data_release);
    obs_data_set_int(settings.get(), "fixture", mode);
    obs_data_set_int(settings.get(), "width", width);
    obs_data_set_int(settings.get(), "height", height);
    obs_data_set_bool(settings.get(), "hdr", hdr);
    Source source(obs_source_create_private("beauty_test_fixture", "fixture", settings.get()), obs_source_release);
    require(source && obs_source_get_width(source.get()) == width, "Cannot create test texture source");
    return source;
}

Source attach_settings(obs_source_t *source, obs_data_t *settings)
{
    Source filter(obs_source_create_private("obs_beauty_filter", "beauty", settings), obs_source_release);
    require(static_cast<bool>(filter), "Cannot create beauty filter");
    Properties props(obs_source_properties(filter.get()), obs_properties_destroy);
    require(props != nullptr, "Cannot create filter properties");
    obs_source_filter_add(source, filter.get());
    return filter;
}

Source attach_filter(obs_source_t *source, double smoothing, double whitening, double rosy,
                     bool original = false)
{
    Settings settings(obs_data_create(), obs_data_release);
    obs_data_set_double(settings.get(), "smoothing", smoothing);
    obs_data_set_double(settings.get(), "whitening", whitening);
    obs_data_set_double(settings.get(), "rosy", rosy);
    obs_data_set_bool(settings.get(), "show_original", original);
    return attach_settings(source, settings.get());
}

Settings neutral_settings()
{
    Settings settings(obs_data_create(), obs_data_release);
    for (const char *key : {"smoothing", "blemish", "whitening", "rosy", "sharpness",
                            "temperature", "exposure", "shadow_fill", "color_saturation"})
        obs_data_set_double(settings.get(), key, 0.0);
    return settings;
}

bool draw_source(obs_source_t *source, gs_texrender_t *output)
{
    const uint32_t width = obs_source_get_width(source);
    const uint32_t height = obs_source_get_height(source);
    gs_texrender_reset(output);
    if (!gs_texrender_begin(output, width, height))
        return false;
    // Initialize 2D render state.
    gs_set_cull_mode(GS_NEITHER);
    gs_enable_depth_test(false);
    vec4 clear = {};
    gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
    gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
    gs_blend_state_push();
    gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    obs_source_video_render(source);
    gs_blend_state_pop();
    gs_texrender_end(output);
    return true;
}

std::vector<uint8_t> capture(obs_source_t *source)
{
    const uint32_t width = obs_source_get_width(source);
    const uint32_t height = obs_source_get_height(source);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    obs_enter_graphics();
    gs_texrender_t *output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    gs_stagesurf_t *stage = gs_stagesurface_create(width, height, GS_RGBA);
    gs_begin_scene();
    const bool rendered = output && stage && draw_source(source, output);
    if (rendered)
        gs_stage_texture(stage, gs_texrender_get_texture(output));
    gs_end_scene();
    gs_flush();
    uint8_t *mapped = nullptr;
    uint32_t stride = 0;
    const bool readable = rendered && gs_stagesurface_map(stage, &mapped, &stride);
    if (readable) {
        for (uint32_t y = 0; y < height; ++y)
            std::copy_n(mapped + static_cast<size_t>(y) * stride, width * 4,
                        pixels.data() + static_cast<size_t>(y) * width * 4);
        gs_stagesurface_unmap(stage);
    }
    gs_stagesurface_destroy(stage);
    gs_texrender_destroy(output);
    obs_leave_graphics();
    require(readable, "Cannot read back GPU output");
    return pixels;
}

std::array<int, 4> pixel(const std::vector<uint8_t> &image, uint32_t x, uint32_t y, uint32_t width = 64)
{
    const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
    return {image[offset], image[offset + 1], image[offset + 2], image[offset + 3]};
}

void rendering_tests()
{
    {
        auto source = make_fixture(1);
        const auto baseline = capture(source.get());
        auto filter = attach_filter(source.get(), 0.0, 0.0, 0.0);
        const auto result = capture(source.get());
        if (result != baseline) {
            const auto before = pixel(baseline, 16, 16), after = pixel(result, 16, 16);
            Settings live(obs_source_get_settings(filter.get()), obs_data_release);
            std::fprintf(stderr, "Bypass diagnostic: before=%d,%d,%d,%d after=%d,%d,%d,%d settings=%s\n",
                         before[0], before[1], before[2], before[3], after[0], after[1], after[2], after[3],
                         obs_data_get_json(live.get()));
        }
        require(result == baseline, "Zero settings must bypass exactly");
        std::puts("PASS: zero settings preserve every pixel");
    }
    {
        auto source = make_fixture(1);
        const auto baseline = capture(source.get());
        auto filter = attach_filter(source.get(), 100.0, 100.0, 100.0, true);
        require(capture(source.get()) == baseline, "Show original must bypass exactly");
        std::puts("PASS: original comparison preserves every pixel");
    }
    {
        auto source = make_fixture();
        const auto before = pixel(capture(source.get()), 16, 16);
        auto filter = attach_filter(source.get(), 0.0, 100.0, 0.0);
        const auto after = pixel(capture(source.get()), 16, 16);
        require(after[0] > before[0] && after[1] > before[1] && after[2] > before[2],
                "Whitening must lift skin midtones");
        require(after[3] == before[3], "Whitening changed alpha");
        std::puts("PASS: whitening lifts skin midtones and preserves alpha");
    }
    {
        auto source = make_fixture();
        const auto before = pixel(capture(source.get()), 16, 16);
        auto filter = attach_filter(source.get(), 0.0, 0.0, 100.0);
        const auto after = pixel(capture(source.get()), 16, 16);
        require(after[0] > before[0] && after[1] <= before[1], "Rosy adjustment must increase red bias");
        std::puts("PASS: rosy adjustment increases red bias");
    }
    {
        auto source = make_fixture(1);
        const auto before = capture(source.get());
        auto filter = attach_filter(source.get(), 100.0, 0.0, 0.0);
        const auto after = capture(source.get());
        const int before_noise = std::abs(pixel(before, 16, 16)[0] - pixel(before, 17, 16)[0]);
        const int after_noise = std::abs(pixel(after, 16, 16)[0] - pixel(after, 17, 16)[0]);
        require(after_noise < before_noise, "Smoothing must reduce skin texture contrast");
        require(pixel(before, 48, 16) == pixel(after, 48, 16), "Skin mask changed the blue background");
        require(std::abs(pixel(after, 31, 16)[2] - pixel(before, 31, 16)[2]) < 15,
                "Smoothing must preserve the skin/background edge");
        std::printf("PASS: smoothing reduces test noise %d -> %d, preserving blue background and edge\n",
                    before_noise, after_noise);
    }
    {
        auto source = make_fixture(2);
        const auto before = capture(source.get());
        auto filter = attach_filter(source.get(), 100.0, 100.0, 100.0);
        const auto after = capture(source.get());
        for (size_t i = 3; i < before.size(); i += 4)
            require(before[i] == after[i], "Processing must preserve alpha, including transparent edges");
        require(pixel(after, 48, 16) == std::array<int, 4>{0, 0, 0, 0}, "Transparent output contains RGB garbage");
        std::puts("PASS: semitransparent and transparent pixels preserve alpha");
    }
    {
        auto source = make_fixture(3);
        const auto before = capture(source.get());
        auto filter = attach_filter(source.get(), 0.0, 100.0, 0.0);
        require(capture(source.get()) == before, "Whitening must preserve white endpoint");
        std::puts("PASS: whitening preserves the white endpoint");
    }
    {
        auto source = make_fixture(1, 64, 64, true);
        const auto before = capture(source.get());
        auto filter = attach_filter(source.get(), 100.0, 100.0, 100.0);
        require(capture(source.get()) == before, "HDR source must pass through unchanged");
        std::puts("PASS: HDR-labelled source bypasses all processing");
    }
}

std::vector<uint8_t> render_settings(obs_data_t *settings, int fixture = 0)
{
    auto source = make_fixture(fixture);
    auto filter = attach_settings(source.get(), settings);
    return capture(source.get());
}

void click_button(obs_source_t *filter, const char *key)
{
    Properties props(obs_source_properties(filter), obs_properties_destroy);
    auto *button = obs_properties_get(props.get(), key);
    require(button != nullptr, "Missing property button");
    require(obs_property_enabled(button), "Cannot click a disabled property button");
    obs_property_button_clicked(button, filter);
}

void choose_preset(obs_source_t *filter, const char *id)
{
    Properties props(obs_source_properties(filter), obs_properties_destroy);
    Settings settings(obs_source_get_settings(filter), obs_data_release);
    obs_data_set_string(settings.get(), "preset", id);
    obs_property_modified(obs_properties_get(props.get(), "preset"), settings.get());
    // Match the native property view's update after the modified callback.
    obs_source_update(filter, settings.get());
}

void feature_tests()
{
    auto plain_source = make_fixture();
    const auto original = capture(plain_source.get());
    const auto base = pixel(original, 16, 16);
    {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "strength", 0);
        obs_data_set_double(settings.get(), "smoothing", 100);
        obs_data_set_double(settings.get(), "exposure", 0.5);
        obs_data_set_double(settings.get(), "temperature", 100);
        require(render_settings(settings.get()) == original, "Overall strength zero must bypass all effects");
        obs_data_set_double(settings.get(), "strength", 100);
        const auto full = pixel(render_settings(settings.get()), 16, 16);
        obs_data_set_double(settings.get(), "strength", 50);
        const auto half = pixel(render_settings(settings.get()), 16, 16);
        for (size_t c = 0; c < 3; ++c)
            require(std::abs(half[c] * 2 - full[c] - base[c]) <= 2, "Overall strength must blend with original");
        std::puts("PASS: master zero bypass and 50% blend including lighting");
    }
    {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "strength", 0);
        obs_data_set_int(settings.get(), "skin_preview", 2);
        const auto mask = render_settings(settings.get(), 1);
        require(pixel(mask, 16, 16)[0] > 200 && pixel(mask, 48, 16)[0] < 5, "Preview must distinguish skin from blue even at master zero");
        obs_data_set_double(settings.get(), "skin_range", 50);
        const auto narrow = pixel(render_settings(settings.get(), 4), 16, 16);
        obs_data_set_double(settings.get(), "skin_range", 200);
        const auto wide = pixel(render_settings(settings.get(), 4), 16, 16);
        require(wide[0] > narrow[0] + 40, "Range slider must expand the mask");
        obs_data_set_int(settings.get(), "skin_preview", 1);
        require(render_settings(settings.get()) != original, "Overlay preview must be visible");
        obs_data_set_bool(settings.get(), "show_original", true);
        require(render_settings(settings.get()) == original, "Show original must override preview");
        std::puts("PASS: skin range, mask/overlay previews and original priority");
    }
    {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "exposure", -1);
        const auto dark = pixel(render_settings(settings.get()), 16, 16);
        obs_data_set_double(settings.get(), "exposure", 1);
        const auto bright = pixel(render_settings(settings.get()), 16, 16);
        for (size_t c = 0; c < 3; ++c) {
            require(std::abs(dark[c] * 2 - base[c]) <= 2, "Exposure -1 must halve RGB");
            require(std::abs(bright[c] - std::min(255, base[c] * 2)) <= 1, "Exposure +1 must double RGB");
        }
        obs_data_set_double(settings.get(), "exposure", 0);
        obs_data_set_double(settings.get(), "temperature", 100);
        const auto warm = pixel(render_settings(settings.get()), 16, 16);
        obs_data_set_double(settings.get(), "temperature", -100);
        const auto cool = pixel(render_settings(settings.get()), 16, 16);
        require(warm[0] > base[0] && warm[2] < base[2] && cool[0] < base[0] && cool[2] > base[2],
                "Temperature must change warm/cool direction");
        std::puts("PASS: exposure and temperature operate in both directions");
    }
    {
        auto source = make_fixture();
        auto filter = attach_filter(source.get(), 35, 15, 5);
        Settings live(obs_source_get_settings(filter.get()), obs_data_release);
        choose_preset(filter.get(), "warm");
        require(obs_data_get_double(live.get(), "temperature") == 15 &&
                obs_data_get_double(live.get(), "smoothing") == 30, "Warm preset must apply supported parameters");
        obs_data_set_double(live.get(), "strength", 57);
        obs_data_set_double(live.get(), "skin_range", 180);
        obs_data_set_double(live.get(), "exposure", 0.4);
        obs_data_set_string(live.get(), "preset", "saved");
        click_button(filter.get(), "save_preset");
        const std::string json = obs_data_get_json_with_defaults(live.get());
        Settings restored(obs_data_create_from_json(json.c_str()), obs_data_release);
        auto restored_source = make_fixture();
        auto restored_filter = attach_settings(restored_source.get(), restored.get());
        Settings restored_live(obs_source_get_settings(restored_filter.get()), obs_data_release);
        choose_preset(restored_filter.get(), "clear");
        require(obs_data_get_double(restored_live.get(), "whitening") == 25 &&
                obs_data_get_double(restored_live.get(), "exposure") == 0.1, "Clear preset must apply correctly");
        choose_preset(restored_filter.get(), "saved");
        require(obs_data_get_double(restored_live.get(), "strength") == 57 &&
                obs_data_get_double(restored_live.get(), "skin_range") == 180 &&
                obs_data_get_double(restored_live.get(), "exposure") == 0.4, "Custom preset must survive scene JSON");
        obs_data_set_double(restored_live.get(), "strength", 12);
        click_button(restored_filter.get(), "save_preset");
        choose_preset(restored_filter.get(), "natural");
        require(obs_data_get_double(restored_live.get(), "smoothing") == 45 &&
                obs_data_get_double(restored_live.get(), "whitening") == 20 &&
                obs_data_get_double(restored_live.get(), "rosy") == 10 &&
                obs_data_get_double(restored_live.get(), "temperature") == 0 &&
                obs_data_get_double(restored_live.get(), "exposure") == 0, "Natural preset must restore defaults");
        choose_preset(restored_filter.get(), "saved");
        require(obs_data_get_double(restored_live.get(), "strength") == 12, "Saving again must replace custom slot");
        std::puts("PASS: three presets, one-slot overwrite and scene JSON restore");
    }
    {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "strength", std::numeric_limits<double>::quiet_NaN());
        obs_data_set_double(settings.get(), "temperature", std::numeric_limits<double>::infinity());
        obs_data_set_double(settings.get(), "exposure", std::numeric_limits<double>::quiet_NaN());
        require(render_settings(settings.get()) == original, "Nonfinite inputs must fall back to neutral defaults");
        obs_data_set_int(settings.get(), "skin_preview", 2);
        obs_data_set_double(settings.get(), "skin_range", -100);
        const auto small = render_settings(settings.get(), 4);
        obs_data_set_double(settings.get(), "skin_range", 50);
        require(small == render_settings(settings.get(), 4), "Range must clamp at its lower bound");
        obs_data_set_double(settings.get(), "skin_range", 10000);
        const auto large = render_settings(settings.get(), 4);
        obs_data_set_double(settings.get(), "skin_range", 200);
        require(large == render_settings(settings.get(), 4), "Range must clamp at its upper bound");
        obs_data_set_int(settings.get(), "skin_preview", 99);
        require(render_settings(settings.get()) == original, "Unknown preview modes must turn off");
        std::puts("PASS: nonfinite values, range bounds and unknown preview recovery");
    }
    {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "smoothing", 100);
        obs_data_set_double(settings.get(), "whitening", 100);
        obs_data_set_double(settings.get(), "rosy", 100);
        obs_data_set_double(settings.get(), "exposure", 3);
        obs_data_set_double(settings.get(), "temperature", -100);
        auto alpha_source = make_fixture(2);
        const auto before = capture(alpha_source.get()), after = render_settings(settings.get(), 2);
        for (size_t i = 3; i < after.size(); i += 4)
            require(before[i] == after[i], "Extreme lighting must preserve every alpha value");
        require(pixel(after, 48, 16) == std::array<int, 4>{0, 0, 0, 0}, "Transparent pixels must remain clear");
        auto tiny_source = make_fixture(0, 1, 1);
        auto tiny_filter = attach_settings(tiny_source.get(), settings.get());
        const auto tiny = capture(tiny_source.get());
        require(tiny.size() == 4 && tiny[3] == 255, "A 1x1 input must render safely");
        std::puts("PASS: extreme settings preserve transparency and handle 1x1 inputs");
    }
    for (const auto &locale : {std::pair{"en-US", "Basic Beauty"}, {"zh-CN", u8"基础美颜"}, {"zh-TW", u8"基礎美顏"}}) {
        obs_set_locale(locale.first);
        require(std::string(obs_source_get_display_name("obs_beauty_filter")) == locale.second, "Plugin locale must follow OBS");
        Properties props(obs_get_source_properties("obs_beauty_filter"), obs_properties_destroy);
        require(obs_properties_get(props.get(), "beauty_group") == nullptr &&
                obs_properties_first(props.get()) == obs_properties_get(props.get(), "beauty_heading"),
                "The beauty heading must lead the panel without an empty-title group");
        for (const char *key : {"skin_group", "color_group"}) {
            auto *group = obs_properties_get(props.get(), key);
            require(group && obs_property_visible(group) && obs_property_group_type(group) == OBS_GROUP_NORMAL,
                    "Range/preview and color groups must stay visible and expanded");
        }
        for (const char *key : {"beauty_heading", "show_original", "preset", "preset_name", "save_preset",
                                "strength", "smoothing", "blemish", "whitening", "rosy", "sharpness",
                                "skin_range", "skin_preview", "exposure", "shadow_fill", "temperature", "color_saturation"}) {
            auto *p = obs_properties_get(props.get(), key);
            require(p && obs_property_visible(p), "All sixteen controls and the heading must stay visible");
            const char *description = obs_property_long_description(p);
            const bool heading = std::string(key) == "beauty_heading";
            require(heading ? description && *description : !description || !*description,
                    "Only the beauty heading may show a tooltip");
            if (heading)
                require(std::string(description).find('\n') != std::string::npos &&
                        std::string(description).find("SDR") != std::string::npos,
                        "The SDR-only heading tooltip must use two short lines");
        }
        require(obs_properties_get(props.get(), "info") == nullptr, "The separate SDR-only row must be removed");
        require(obs_properties_get(props.get(), "apply_preset") == nullptr, "Automatic presets must not expose an Apply button");
        auto *preset_name = obs_properties_get(props.get(), "preset_name");
        require(preset_name && obs_property_get_type(preset_name) == OBS_PROPERTY_TEXT &&
                obs_property_text_type(preset_name) == OBS_TEXT_DEFAULT && !obs_property_enabled(preset_name),
                "Preset names must use a visible native single-line field, disabled for built-ins");
        auto *footer = obs_properties_get(props.get(), "version_info");
        require(footer && obs_property_visible(footer) &&
                std::string(obs_property_description(footer)).find("OBS-Beauty: 1.0.0") != std::string::npos &&
                std::string(obs_property_description(footer)).find("https://haliluya.net") != std::string::npos,
                "The panel must show the developer footer in every locale");
        for (const char *key : {"strength", "smoothing", "blemish", "whitening", "rosy", "sharpness", "shadow_fill"}) {
            auto *p = obs_properties_get(props.get(), key);
            require(obs_property_float_min(p) == 0 && obs_property_float_max(p) == 100,
                    "Beauty slider ranges must remain 0-100");
        }
        auto *saturation = obs_properties_get(props.get(), "color_saturation");
        require(obs_property_float_min(saturation) == -100 && obs_property_float_max(saturation) == 100,
                "Saturation must support grayscale through enhanced color");
    }
    obs_set_locale("zh-CN");
    std::puts("PASS: three locales, compact beauty heading, SDR tooltip, product/version footer and expanded sections");
}

void algorithm_tests()
{
    {
        auto source = make_fixture();
        auto filter = attach_settings(source.get(), nullptr);
        Settings live(obs_source_get_settings(filter.get()), obs_data_release);
        require(obs_data_get_double(live.get(), "strength") == 100 &&
                obs_data_get_double(live.get(), "smoothing") == 45 &&
                obs_data_get_double(live.get(), "whitening") == 20 &&
                obs_data_get_double(live.get(), "rosy") == 10, "New filters must receive the new natural defaults");
        std::puts("PASS: natural defaults for new filters");
    }
    {
        auto source = make_fixture(1);
        auto settings = neutral_settings();
        auto filter = attach_settings(source.get(), settings.get());
        std::vector<int> contrast;
        for (double value : {0.0, 10.0, 25.0, 45.0, 70.0, 100.0}) {
            obs_data_set_double(settings.get(), "smoothing", value);
            obs_source_update(filter.get(), settings.get());
            // OBS caches the filtered texture within a video frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
            const auto image = capture(source.get());
            const int noise = std::abs(pixel(image, 16, 16)[0] - pixel(image, 17, 16)[0]);
            require(contrast.empty() || noise <= contrast.back(), "Increasing smoothing must not increase texture contrast");
            contrast.push_back(noise);
        }
        std::printf("SMOOTHING 0/10/25/45/70/100 texture contrast=%d/%d/%d/%d/%d/%d\n",
                    contrast[0], contrast[1], contrast[2], contrast[3], contrast[4], contrast[5]);
        require(contrast[1] >= 20 && contrast[2] > contrast[4] && contrast.back() < contrast.front() / 2,
                "Low smoothing must be subtle and high smoothing visibly stronger");
        std::puts("PASS: gradual low-strength adjustment and stronger high-strength smoothing");
    }
    {
        const auto texture_rms = [](const std::vector<uint8_t> &image, uint32_t width, double scale) {
            double sum = 0, squared = 0;
            for (unsigned y = 20; y < 84; ++y)
                for (unsigned x = 20; x < 84; ++x) {
                    const auto px = static_cast<uint32_t>((x + 0.5) * scale);
                    const auto py = static_cast<uint32_t>((y + 0.5) * scale);
                    const double value = pixel(image, px, py, width)[0];
                    sum += value;
                    squared += value * value;
                }
            return std::sqrt(std::max(0.0, squared / 4096 - (sum / 4096) * (sum / 4096)));
        };
        double reference = 0;
        for (const auto &size : {std::pair{1920u, 1080u}, {3840u, 2160u}, {1080u, 1920u}, {2160u, 3840u}}) {
            auto source = make_fixture(5, size.first, size.second);
            const auto before = capture(source.get());
            auto filter = attach_filter(source.get(), 100, 0, 0);
            const auto after = capture(source.get());
            const double scale = std::min(size.first, size.second) / 1080.0;
            const double ratio = texture_rms(after, size.first, scale) / texture_rms(before, size.first, scale);
            require(ratio < 0.95, "Smoothing must reduce the scene-space texture at every resolution");
            if (reference == 0)
                reference = ratio;
            require(std::abs(ratio - reference) < 0.06, "1080p/4K and portrait must have similar scene-space smoothing");
            const auto line_x = static_cast<uint32_t>(102 * scale), line_y = static_cast<uint32_t>(64 * scale);
            require(pixel(before, line_x, line_y, size.first) == pixel(after, line_x, line_y, size.first),
                    "Resolution adaptation must preserve the dark detail line");
            require(pixel(before, size.first - 20, 20, size.first) == pixel(after, size.first - 20, 20, size.first),
                    "Resolution adaptation must preserve the blue background");
            std::printf("RESOLUTION %ux%u skin texture RMS ratio=%.4f\n", size.first, size.second, ratio);
        }
        std::puts("PASS: consistent 1080p/4K landscape and portrait smoothing, detail and background");
    }
    {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "exposure", -0.25);
        obs_data_set_double(settings.get(), "temperature", 20);
        const auto lighting = render_settings(settings.get(), 6);
        obs_data_set_double(settings.get(), "smoothing", 100);
        obs_data_set_double(settings.get(), "whitening", 100);
        obs_data_set_double(settings.get(), "rosy", 100);
        require(render_settings(settings.get(), 6) == lighting, "Non-skin smoothing shortcut must retain global lighting");
        obs_data_set_int(settings.get(), "skin_preview", 2);
        const auto preview = render_settings(settings.get(), 1);
        obs_data_set_double(settings.get(), "smoothing", 0);
        require(render_settings(settings.get(), 1) == preview, "Mask preview must not depend on smoothing");
        std::puts("PASS: non-skin shortcut retains lighting and preview remains independent of smoothing");
    }
}

void smoothing_quality_tests()
{
    auto source = make_fixture(10);
    const auto before = capture(source.get());
    const int before_noise = std::abs(pixel(before, 16, 16)[0] - pixel(before, 17, 16)[0]);
    int residual[2] = {};
    int edge_drift = 0;
    for (size_t i = 0; i < 2; ++i) {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "smoothing", i == 0 ? 80 : 100);
        const auto after = render_settings(settings.get(), 10);
        residual[i] = std::abs(pixel(after, 16, 16)[0] - pixel(after, 17, 16)[0]);
        for (uint32_t x = 28; x <= 35; ++x) {
            const auto old_pixel = pixel(before, x, 48), new_pixel = pixel(after, x, 48);
            for (size_t c = 0; c < 3; ++c)
                edge_drift = std::max(edge_drift, std::abs(new_pixel[c] - old_pixel[c]));
        }
        for (size_t alpha = 3; alpha < after.size(); alpha += 4)
            require(before[alpha] == after[alpha], "Stronger smoothing must retain alpha");
    }
    std::printf("SMOOTHING QUALITY original=%d residual80=%d residual100=%d max_edge_drift=%d\n",
                before_noise, residual[0], residual[1], edge_drift);
    require(residual[0] <= 19 && residual[1] <= 11,
            "Stronger smoothing must reduce moderate luminance noise more effectively");
    require(edge_drift <= 3, "Stronger smoothing must keep high-contrast skin edges within three code values");
    std::puts("PASS: stronger luminance-noise smoothing at 80/100 while preserving a warm high-contrast edge");

    auto spots = make_fixture(11);
    const auto untreated = capture(spots.get());
    int previous_spot = 24, previous_redness = 18;
    for (double smoothing : {45.0, 80.0, 100.0}) {
        auto settings = neutral_settings();
        obs_data_set_double(settings.get(), "smoothing", smoothing);
        const auto after = render_settings(settings.get(), 11);
        const int spot = 178 - pixel(after, 20, 16)[0];
        const auto red = pixel(after, 20, 48);
        const int redness = red[0] - red[1] - 48;
        require(spot >= 0 && spot <= previous_spot && redness >= 0 && redness <= previous_redness,
                "Increasing smoothing must gently reduce dark and red spots without reversing them");
        if (smoothing == 100)
            require(spot <= 14 && redness <= 13, "Dual scales must soften small dark spots and uneven skin chroma");
        previous_spot = spot;
        previous_redness = redness;
        for (uint32_t x : {40u, 41u})
            require(pixel(after, x, 32) == pixel(untreated, x, 32), "Fine dark lines must stay intact");
        for (uint32_t x : {38u, 39u, 42u, 43u, 47u, 48u, 49u}) {
            const auto before_pixel = pixel(untreated, x, 32), after_pixel = pixel(after, x, 32);
            for (size_t c = 0; c < 3; ++c)
                require(std::abs(before_pixel[c] - after_pixel[c]) <= 3,
                        "Dual scales must not blur fine-line boundaries or strong warm contours");
        }
        std::printf("DUAL SCALE strength=%.0f dark24_residual=%d redness18_residual=%d\n", smoothing, spot, redness);
    }
    std::puts("PASS: dual-scale spot reduction, gentle skin-tone evenness and protected fine contours");
}

void comparison_tests()
{
    {
        for (uint32_t width : {64u, 65u}) {
            for (double smoothing : {0.0, 100.0}) {
                auto settings = neutral_settings();
                obs_data_set_double(settings.get(), "smoothing", smoothing);
                obs_data_set_double(settings.get(), "whitening", 100);
                obs_data_set_double(settings.get(), "rosy", 50);
                obs_data_set_double(settings.get(), "exposure", -0.25);
                obs_data_set_double(settings.get(), "temperature", 20);
                auto plain = make_fixture(7, width, 48);
                const auto original = capture(plain.get());
                auto full_source = make_fixture(7, width, 48);
                auto full_filter = attach_settings(full_source.get(), settings.get());
                const auto full = capture(full_source.get());
                obs_data_set_int(settings.get(), "skin_preview", 3);
                auto split_source = make_fixture(7, width, 48);
                auto split_filter = attach_settings(split_source.get(), settings.get());
                const auto split = capture(split_source.get());
                require(full != original, "Comparison fixture must show a visible effect");
                for (uint32_t y = 0; y < 48; ++y)
                    for (uint32_t x = 0; x < width; ++x) {
                        const auto expected = pixel((2 * x + 1 < width) ? original : full, x, y, width);
                        const auto actual = pixel(split, x, y, width);
                        for (size_t c = 0; c < 4; ++c) {
                            if (std::abs(actual[c] - expected[c]) > 1)
                                std::fprintf(stderr, "SPLIT mismatch width=%u smoothing=%.0f x=%u y=%u channel=%zu actual=%d expected=%d original=%d full=%d\n",
                                             width, smoothing, x, y, c, actual[c], expected[c],
                                             pixel(original, x, y, width)[c], pixel(full, x, y, width)[c]);
                            require(std::abs(actual[c] - expected[c]) <= 1,
                                    "Split must preserve left original and right full beauty at the same coordinates");
                        }
                    }
            }
        }
        std::puts("PASS: split comparison matches original/beauty in color and smoothing paths, even/odd widths");
    }
    {
        auto plain = make_fixture(1);
        const auto original = capture(plain.get());
        auto settings = neutral_settings();
        obs_data_set_int(settings.get(), "skin_preview", 3);
        obs_data_set_double(settings.get(), "smoothing", 100);
        obs_data_set_double(settings.get(), "whitening", 100);
        obs_data_set_double(settings.get(), "strength", 0);
        require(render_settings(settings.get(), 1) == original, "Split must honor zero overall strength");
        obs_data_set_double(settings.get(), "strength", 100);
        obs_data_set_bool(settings.get(), "show_original", true);
        require(render_settings(settings.get(), 1) == original, "Original must override split view");
        obs_data_set_bool(settings.get(), "show_original", false);
        auto hdr = make_fixture(1, 64, 64, true);
        const auto hdr_before = capture(hdr.get());
        auto hdr_filter = attach_settings(hdr.get(), settings.get());
        require(capture(hdr.get()) == hdr_before, "HDR must bypass split view");
        auto alpha = make_fixture(2);
        const auto before = capture(alpha.get()), after = render_settings(settings.get(), 2);
        for (size_t i = 3; i < after.size(); i += 4)
            require(before[i] == after[i], "Split must preserve every alpha value");
        auto tiny = make_fixture(0, 1, 1);
        auto tiny_filter = attach_settings(tiny.get(), settings.get());
        require(capture(tiny.get())[3] == 255, "Split must handle a 1x1 source");
        std::puts("PASS: split honors zero strength, original priority, HDR, transparency and 1x1 input");
    }
}

void custom_slot_tests()
{
    {
        auto source = make_fixture();
        auto filter = attach_settings(source.get(), nullptr);
        Settings live(obs_source_get_settings(filter.get()), obs_data_release);
        Properties props(obs_source_properties(filter.get()), obs_properties_destroy);
        auto *preset = obs_properties_get(props.get(), "preset");
        auto *save = obs_properties_get(props.get(), "save_preset");
        require(obs_property_list_item_count(preset) == 6 && !obs_property_enabled(save),
                "Three built-ins and three custom slots must share one list; saving needs an explicit slot");
        require(!obs_property_button_clicked(save, filter.get()), "A built-in selection must not overwrite a custom slot");
        obs_data_set_double(live.get(), "strength", 63);
        obs_data_set_bool(live.get(), "show_original", true);
        obs_data_set_int(live.get(), "skin_preview", 3);
        obs_data_set_string(live.get(), "preset", "saved_2");
        obs_property_modified(preset, live.get());
        require(obs_property_enabled(save) && !obs_property_list_item_disabled(preset, 4) &&
                obs_data_get_double(live.get(), "strength") == 63 && obs_data_get_bool(live.get(), "show_original") &&
                obs_data_get_int(live.get(), "skin_preview") == 3,
                "An empty slot must keep numeric and preview states while enabling Save");
        const std::string empty_label = obs_property_list_item_name(preset, 4);
        require(obs_property_button_clicked(save, filter.get()), "Saving a selected empty slot must succeed");
        require(empty_label != obs_property_list_item_name(preset, 4), "Saving must refresh the same property list");
        obs_data_set_double(live.get(), "strength", 12);
        obs_property_modified(obs_properties_get(props.get(), "strength"), live.get());
        require(std::string(obs_data_get_string(live.get(), "preset")) == "modified" &&
                std::string(obs_data_get_string(live.get(), "preset_base")) == "saved_2" && obs_property_enabled(save),
                "Manual changes must show Modified and retain the custom save target");
        obs_properties_apply_settings(props.get(), live.get());
        Properties reopened(obs_source_properties(filter.get()), obs_properties_destroy);
        require(obs_data_get_double(live.get(), "strength") == 12 && obs_data_get_bool(live.get(), "show_original") &&
                obs_data_get_int(live.get(), "skin_preview") == 3,
                "Refreshing or reopening must not reapply a preset or reset comparison states");
        obs_data_set_string(live.get(), "preset", "saved_2");
        obs_property_modified(preset, live.get());
        require(obs_data_get_double(live.get(), "strength") == 63 && !obs_data_get_bool(live.get(), "show_original") &&
                obs_data_get_int(live.get(), "skin_preview") == 0,
                "Reselecting a modified preset must apply saved values and close previews");
        std::puts("PASS: automatic presets, empty-slot retention, modified target and refresh/reopen safeguards");
    }
    {
        constexpr const char *ids[] = {"saved", "saved_2", "saved_3"};
        constexpr const char *keys[] = {"strength", "smoothing", "whitening", "rosy", "skin_range", "exposure", "temperature",
                                      "blemish", "sharpness", "shadow_fill", "color_saturation"};
        std::array<double, 11> expected[] = {
            {30, 20, 10, 2, 80, -0.2, -10, 12, 15, 20, -10},
            {60, 40, 20, 4, 120, 0, 0, 35, 25, 40, 0},
            {90, 60, 30, 6, 160, 0.2, 10, 60, 35, 60, 15},
        };
        auto source = make_fixture();
        auto filter = attach_settings(source.get(), nullptr);
        Settings live(obs_source_get_settings(filter.get()), obs_data_release);
        for (size_t slot = 0; slot < 3; ++slot) {
            choose_preset(filter.get(), ids[slot]);
            for (size_t value = 0; value < std::size(keys); ++value)
                obs_data_set_double(live.get(), keys[value], expected[slot][value]);
            obs_data_set_bool(live.get(), "show_original", true);
            obs_data_set_int(live.get(), "skin_preview", 3);
            click_button(filter.get(), "save_preset");
        }
        choose_preset(filter.get(), "saved_2");
        obs_data_set_double(live.get(), "strength", 88);
        click_button(filter.get(), "save_preset");
        expected[1][0] = 88;
        const std::string json = obs_data_get_json_with_defaults(live.get());
        Settings restored(obs_data_create_from_json(json.c_str()), obs_data_release);
        auto restored_source = make_fixture();
        auto restored_filter = attach_settings(restored_source.get(), restored.get());
        Settings restored_live(obs_source_get_settings(restored_filter.get()), obs_data_release);
        choose_preset(restored_filter.get(), "clear");
        for (size_t slot = 0; slot < 3; ++slot) {
            obs_data_set_bool(restored_live.get(), "show_original", true);
            obs_data_set_int(restored_live.get(), "skin_preview", 3);
            choose_preset(restored_filter.get(), ids[slot]);
            for (size_t value = 0; value < std::size(keys); ++value)
                require(std::abs(obs_data_get_double(restored_live.get(), keys[value]) - expected[slot][value]) < 1e-9,
                        "Each custom slot must survive another slot's overwrite, built-in apply and JSON restore");
            require(!obs_data_get_bool(restored_live.get(), "show_original") &&
                    obs_data_get_int(restored_live.get(), "skin_preview") == 0,
                    "Applying any slot must turn off comparison and preview");
        }
        std::puts("PASS: all three independent slots, targeted overwrite, eleven-value scene JSON and preview reset");
    }
}

void slider_ui_tests()
{
    auto source = make_fixture();
    auto filter = attach_settings(source.get(), nullptr);
    Settings live(obs_source_get_settings(filter.get()), obs_data_release);
    struct RefreshObserver {
        signal_handler_t *handler;
        std::atomic<unsigned> count{0};
        static void record(void *data, calldata_t *)
        {
            ++static_cast<RefreshObserver *>(data)->count;
        }
        explicit RefreshObserver(obs_source_t *value) : handler(obs_source_get_signal_handler(value))
        {
            signal_handler_connect(handler, "update_properties", record, this);
        }
        void stop()
        {
            if (handler)
                signal_handler_disconnect(handler, "update_properties", record, this);
            handler = nullptr;
        }
        ~RefreshObserver() { stop(); }
    } observer(filter.get());
    Properties props(obs_source_properties(filter.get()), obs_properties_destroy);
    auto *slider = obs_properties_get(props.get(), "shadow_fill");
    unsigned changes = 0;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        for (unsigned step = 0; step <= 200; ++step) {
            obs_data_set_double(live.get(), "shadow_fill", step <= 100 ? step : 200 - step);
            require(!obs_property_modified(slider, live.get()), "A slider change must never rebuild active controls");
            ++changes;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    require(observer.count.load() == 0, "Returning to the displayed preset must cancel a redundant refresh");
    for (unsigned value = 10; value <= 60; value += 10) {
        obs_data_set_double(live.get(), "shadow_fill", value);
        require(!obs_property_modified(slider, live.get()), "Continuous dragging must keep its slider alive");
        obs_source_update(filter.get(), live.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
        require(observer.count.load() == 0, "Editing must postpone the preset-label refresh");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (observer.count.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(observer.count.load() == 1 && obs_data_get_double(live.get(), "shadow_fill") == 60,
            "Idle editing must produce one refresh without changing the selected amount");
    props.reset();
    props.reset(obs_source_properties(filter.get()));
    auto *preset = obs_properties_get(props.get(), "preset");
    obs_data_set_string(live.get(), "preset", "saved");
    obs_property_modified(preset, live.get());
    obs_data_set_string(live.get(), "preset_name", "Saved name");
    obs_property_button_clicked(obs_properties_get(props.get(), "save_preset"), filter.get());
    obs_data_set_string(live.get(), "preset_name", "Unsaved draft");
    obs_data_set_double(live.get(), "shadow_fill", 42);
    require(!obs_property_modified(obs_properties_get(props.get(), "shadow_fill"), live.get()),
            "Custom-preset editing must not rebuild its slider");
    props.reset();
    props.reset(obs_source_properties(filter.get()));
    obs_properties_apply_settings(props.get(), live.get());
    require(obs_data_get_double(live.get(), "shadow_fill") == 42 &&
            std::string(obs_data_get_string(live.get(), "preset_name")) == "Unsaved draft" &&
            std::string(obs_data_get_string(live.get(), "saved_preset_name")) == "Saved name",
            "Reloading must retain numeric changes and the unsaved name without renaming the saved slot");
    obs_data_set_double(live.get(), "shadow_fill", 60);
    obs_property_modified(obs_properties_get(props.get(), "shadow_fill"), live.get());
    props.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    require(observer.count.load() == 1, "Closing the property panel must cancel its pending refresh");
    props.reset(obs_source_properties(filter.get()));
    obs_data_set_double(live.get(), "shadow_fill", 42);
    obs_property_modified(obs_properties_get(props.get(), "shadow_fill"), live.get());
    std::unique_ptr<obs_weak_source_t, decltype(&obs_weak_source_release)> weak(
        obs_source_get_weak_source(filter.get()), obs_weak_source_release);
    observer.stop();
    obs_source_filter_remove(source.get(), filter.get());
    // Release the source while its property list still owns only a weak reference.
    filter.reset();
    obs_queue_task(OBS_TASK_DESTROY, [](void *) {}, nullptr, true);
    require(obs_weak_source_expired(weak.get()), "The panel must not keep a removed filter alive");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    props.reset();
    std::printf("PASS: %u slider changes without immediate rebuild, coalesced idle refresh, draft retention and panel closure\n",
                changes);
}

void preset_name_tests()
{
    constexpr const char *ids[] = {"saved", "saved_2", "saved_3"};
    constexpr const char *name_keys[] = {"saved_preset_name", "saved_preset_name_2", "saved_preset_name_3"};
    constexpr const char *names[] = {u8"夜间暖光", "Daylight", u8"柔和 \U0001f31e"};
    auto source = make_fixture();
    auto filter = attach_settings(source.get(), nullptr);
    Settings live(obs_source_get_settings(filter.get()), obs_data_release);
    Properties props(obs_source_properties(filter.get()), obs_properties_destroy);
    auto *preset = obs_properties_get(props.get(), "preset");
    auto *name_field = obs_properties_get(props.get(), "preset_name");
    auto *save = obs_properties_get(props.get(), "save_preset");
    for (size_t slot = 0; slot < 3; ++slot) {
        obs_data_set_string(live.get(), "preset", ids[slot]);
        obs_property_modified(preset, live.get());
        require(obs_property_enabled(name_field) && std::string(obs_data_get_string(live.get(), "preset_name")).empty(),
                "Selecting an empty slot must enable a blank name field");
        const std::string before = obs_property_list_item_name(preset, slot + 3);
        obs_data_set_string(live.get(), "preset_name", names[slot]);
        obs_property_modified(name_field, live.get());
        obs_data_set_double(live.get(), "strength", 30.0 + 20.0 * static_cast<double>(slot));
        require(before == obs_property_list_item_name(preset, slot + 3) &&
                std::string(obs_data_get_string(live.get(), name_keys[slot])).empty(),
                "Editing a name must not rename or save a slot until Save is clicked");
        require(obs_property_button_clicked(save, filter.get()), "Saving a named preset must refresh the panel");
        require(std::string(obs_data_get_string(live.get(), name_keys[slot])) == names[slot] &&
                std::string(obs_property_list_item_name(preset, slot + 3)).find(names[slot]) != std::string::npos &&
                std::string(obs_property_list_item_string(preset, slot + 3)) == ids[slot],
                "Saving must update only the display name and keep the stable slot ID");
    }
    obs_data_set_string(live.get(), "preset", "saved");
    obs_property_modified(preset, live.get());
    require(std::string(obs_data_get_string(live.get(), "preset_name")) == names[0],
            "Reselecting a slot must load its own saved name");
    obs_data_set_string(live.get(), "preset_name", "Unsaved draft");
    obs_data_set_string(live.get(), "preset", "saved_2");
    obs_property_modified(preset, live.get());
    require(std::string(obs_data_get_string(live.get(), "preset_name")) == names[1] &&
            std::string(obs_data_get_string(live.get(), name_keys[0])) == names[0] &&
            obs_data_get_double(live.get(), "strength") == 50,
            "Switching saved slots must discard an unsaved name and automatically apply the chosen values");
    obs_data_set_string(live.get(), "preset", "natural");
    obs_property_modified(preset, live.get());
    require(!obs_property_enabled(name_field) && std::string(obs_data_get_string(live.get(), "preset_name")).empty() &&
            !obs_property_button_clicked(save, filter.get()), "Built-ins must disable naming and reject saving");
    const std::string json = obs_data_get_json_with_defaults(live.get());
    Settings restored(obs_data_create_from_json(json.c_str()), obs_data_release);
    auto restored_source = make_fixture();
    auto restored_filter = attach_settings(restored_source.get(), restored.get());
    Settings restored_live(obs_source_get_settings(restored_filter.get()), obs_data_release);
    Properties restored_props(obs_source_properties(restored_filter.get()), obs_properties_destroy);
    auto *restored_preset = obs_properties_get(restored_props.get(), "preset");
    for (size_t slot = 0; slot < 3; ++slot) {
        obs_data_set_string(restored_live.get(), "preset", ids[slot]);
        obs_property_modified(restored_preset, restored_live.get());
        require(std::string(obs_data_get_string(restored_live.get(), "preset_name")) == names[slot] &&
                std::string(obs_property_list_item_name(restored_preset, slot + 3)).find(names[slot]) != std::string::npos,
                "All three Unicode names must survive built-in Apply and scene JSON restore");
        require(obs_data_get_double(restored_live.get(), "strength") == 30.0 + 20.0 * static_cast<double>(slot),
                "Naming must not change the values stored in each slot");
    }
    std::puts("PASS: three independent Unicode preset names, save-only renaming, stable IDs and scene JSON restore");

    // Duplicate display names must not make different slots share parameters.
    obs_data_set_string(live.get(), "preset", "saved_2");
    obs_property_modified(preset, live.get());
    obs_data_set_string(live.get(), "preset_name", names[0]);
    obs_data_set_double(live.get(), "strength", 88);
    obs_property_button_clicked(save, filter.get());
    require(std::string(obs_property_list_item_name(preset, 3)) != obs_property_list_item_name(preset, 4) &&
            std::string(obs_data_get_string(live.get(), name_keys[0])) == names[0] &&
            std::string(obs_data_get_string(live.get(), name_keys[2])) == names[2],
            "Renaming one slot must retain slot prefixes and leave other names intact");
    obs_data_set_string(live.get(), "preset", "saved");
    obs_property_modified(preset, live.get());
    require(obs_data_get_double(live.get(), "strength") == 30, "Duplicate names must not change slot lookup");
    std::puts("PASS: duplicate names retain independent slots and targeted overwrite");

    const std::string chinese20 = u8"一二三四五六七八九十一二三四五六七八九十";
    const std::pair<std::string, std::string> cases[] = {
        {"  Night light  ", "Night light"},
        {" \t\r\n\x01\x7f ", ""},
        {std::string(21, 'a'), std::string(20, 'a')},
        {chinese20 + u8"夜", chinese20},
        {std::string(19, 'a') + u8"\U0001f31ex", std::string(19, 'a') + u8"\U0001f31e"},
    };
    for (const auto &value : cases) {
        obs_data_set_string(live.get(), "preset_name", value.first.c_str());
        obs_property_button_clicked(save, filter.get());
        require(std::string(obs_data_get_string(live.get(), name_keys[0])) == value.second &&
                std::string(obs_data_get_string(live.get(), "preset_name")) == value.second,
                "Saving must trim spaces, remove controls and limit names to 20 complete UTF-8 characters");
        const std::string label = obs_property_list_item_name(preset, 3);
        require(value.second.empty() ? label.find(": ") == std::string::npos : label.find(value.second) != std::string::npos,
                "Blank names must restore the default label; custom names must appear after saving");
    }
    std::puts("PASS: blank-name fallback, whitespace cleanup and 20-character Chinese/emoji limits");
}

void extra_effect_tests()
{
    auto settings = neutral_settings();
    auto edge_source = make_fixture(12);
    const auto edge = capture(edge_source.get());
    obs_data_set_double(settings.get(), "sharpness", 100);
    const auto sharpened = render_settings(settings.get(), 12);
    const int before_contrast = pixel(edge, 32, 32)[0] - pixel(edge, 31, 32)[0];
    const int after_contrast = pixel(sharpened, 32, 32)[0] - pixel(sharpened, 31, 32)[0];
    require(after_contrast > before_contrast && after_contrast <= before_contrast + 14,
            "Sharpening must increase edge contrast with a bounded correction");
    require(pixel(edge, 16, 16) == pixel(sharpened, 16, 16), "Sharpening must leave flat areas unchanged");
    auto noise_source = make_fixture(7);
    const auto noise = capture(noise_source.get()), sharpened_noise = render_settings(settings.get(), 7);
    for (uint32_t y = 4; y < 60; ++y)
        for (uint32_t x = 4; x < 60; ++x)
            require(pixel(noise, x, y) == pixel(sharpened_noise, x, y),
                    "Sharpening must not amplify flat-area checker noise");
    std::printf("PASS: bounded sharpening, flat-area/noise protection; edge contrast=%d/%d\n",
                before_contrast, after_contrast);

    settings = neutral_settings();
    obs_data_set_double(settings.get(), "shadow_fill", 100);
    auto swatches = make_fixture(8);
    const auto original = capture(swatches.get());
    const auto filled = render_settings(settings.get(), 8);
    const int dark_lift = pixel(filled, 4, 16)[0] - pixel(original, 4, 16)[0];
    const int bright_lift = pixel(filled, 28, 16)[0] - pixel(original, 28, 16)[0];
    require(dark_lift > 20 && bright_lift == 0, "Shadow fill must lift dark tones and retain highlights");
    require(pixel(filled, 52, 16) == pixel(original, 52, 16) &&
            pixel(filled, 60, 16) == pixel(original, 60, 16), "Shadow fill must retain black/white endpoints");
    const auto dark = pixel(original, 4, 16), lifted = pixel(filled, 4, 16);
    for (size_t c = 0; c < 3; ++c)
        require(std::abs(static_cast<double>(dark[c]) / dark[0] -
                         static_cast<double>(lifted[c]) / lifted[0]) < 0.015,
                "Shadow fill must preserve RGB proportions");
    std::printf("PASS: shadow fill retains hue, highlights and endpoints; dark/highlight lift=%d/%d\n",
                dark_lift, bright_lift);

    settings = neutral_settings();
    obs_data_set_double(settings.get(), "color_saturation", -100);
    const auto gray = pixel(render_settings(settings.get()), 16, 16);
    require(gray[0] == gray[1] && gray[1] == gray[2], "Minimum saturation must produce grayscale");
    obs_data_set_double(settings.get(), "color_saturation", 100);
    const auto saturated = pixel(render_settings(settings.get()), 16, 16);
    require(saturated[0] - saturated[2] > 100, "Positive saturation must increase chroma");
    obs_data_set_double(settings.get(), "strength", 50);
    const auto half = pixel(render_settings(settings.get()), 16, 16);
    constexpr int skin[] = {178, 130, 107};
    for (size_t c = 0; c < 3; ++c)
        require(std::abs(half[c] * 2 - saturated[c] - skin[c]) <= 2,
                "Overall strength must blend saturation with the original");
    std::puts("PASS: saturation reduction/increase and overall-strength blending");

    settings = neutral_settings();
    obs_data_set_double(settings.get(), "blemish", 100);
    auto spot_source = make_fixture(11);
    const auto spots = capture(spot_source.get());
    const auto repaired = render_settings(settings.get(), 11);
    const int spot = 178 - pixel(repaired, 20, 16)[0];
    const auto red = pixel(repaired, 20, 48);
    const int redness = red[0] - red[1] - 48;
    require(spot >= 0 && spot <= 17 && redness >= 0 && redness < 18,
            "Blemish reduction alone must soften small dark and red spots without reversing them");
    obs_data_set_double(settings.get(), "smoothing", 100);
    const auto combined = render_settings(settings.get(), 11);
    const int combined_spot = 178 - pixel(combined, 20, 16)[0];
    require(combined_spot >= 0 && combined_spot < spot && combined_spot < 12,
            "Combined smoothing and blemish reduction must improve spots without overshoot");
    for (uint32_t x : {40u, 41u})
        require(pixel(combined, x, 32) == pixel(spots, x, 32),
                "Blemish reduction must preserve fine dark lines");
    int contour_drift = 0;
    for (uint32_t x : {38u, 39u, 42u, 43u, 47u, 48u, 49u}) {
        const auto before = pixel(spots, x, 32), after = pixel(combined, x, 32);
        for (size_t c = 0; c < 3; ++c)
            contour_drift = std::max(contour_drift, std::abs(before[c] - after[c]));
    }
    std::printf("BLEMISH dark24_residual=%d combined=%d redness18_residual=%d contour_drift=%d\n",
                spot, combined_spot, redness, contour_drift);
    require(contour_drift <= 3, "Combined spot repair must retain contours within three code values");
    auto blue = make_fixture(6);
    require(render_settings(settings.get(), 6) == capture(blue.get()),
            "Blemish reduction must retain a non-skin background");
    std::printf("PASS: small-spot repair and protected contours; dark24 residual=%d combined=%d redness18=%d\n",
                spot, combined_spot, redness);

    for (const char *key : {"sharpness", "shadow_fill", "color_saturation"})
        obs_data_set_double(settings.get(), key, 100);
    auto alpha = make_fixture(2);
    const auto alpha_before = capture(alpha.get()), alpha_after = render_settings(settings.get(), 2);
    for (size_t i = 3; i < alpha_before.size(); i += 4)
        require(alpha_before[i] == alpha_after[i], "All new effects must preserve alpha");
    auto tiny = make_fixture(0, 1, 1);
    auto tiny_filter = attach_settings(tiny.get(), settings.get());
    require(capture(tiny.get())[3] == 255, "All new effects must handle 1x1 sources");
    auto hdr = make_fixture(1, 64, 64, true);
    const auto hdr_original = capture(hdr.get());
    auto hdr_filter = attach_settings(hdr.get(), settings.get());
    require(capture(hdr.get()) == hdr_original, "All new effects must bypass HDR");
    obs_data_set_int(settings.get(), "skin_preview", 2);
    const auto preview = render_settings(settings.get(), 1);
    auto neutral = neutral_settings();
    obs_data_set_int(neutral.get(), "skin_preview", 2);
    require(preview == render_settings(neutral.get(), 1), "Range preview must be independent of all new effects");
    obs_data_set_bool(settings.get(), "show_original", true);
    require(render_settings(settings.get(), 1) == capture(make_fixture(1).get()),
            "Show original must override all new effects and previews");
    obs_data_set_bool(settings.get(), "show_original", false);
    obs_data_set_int(settings.get(), "skin_preview", 0);
    obs_data_set_double(settings.get(), "strength", 0);
    require(render_settings(settings.get(), 1) == capture(make_fixture(1).get()),
            "Zero overall strength must bypass all new effects");
    std::puts("PASS: new effects preserve alpha, tiny inputs, HDR/original/zero bypass, and mask preview");
}

void color_protection_tests()
{
    auto source = make_fixture(8);
    const auto original = capture(source.get());
    auto settings = neutral_settings();
    obs_data_set_double(settings.get(), "whitening", 100);
    const auto white = render_settings(settings.get(), 8);
    int midtone_lift = 0, highlight_lift = 0;
    for (uint32_t swatch = 0; swatch < 6; ++swatch) {
        const auto before = pixel(original, swatch * 8 + 4, 16);
        const auto after = pixel(white, swatch * 8 + 4, 16);
        const int peak_before = std::max({before[0], before[1], before[2]});
        const int peak_after = std::max({after[0], after[1], after[2]});
        for (size_t c = 0; c < 3; ++c) {
            require(after[c] >= before[c], "Whitening must not darken a skin swatch");
            require(std::abs(static_cast<double>(before[c]) / peak_before -
                             static_cast<double>(after[c]) / peak_after) < 0.01,
                    "Whitening must retain normalized RGB proportions on dark, midtone and bright skin");
        }
        if (peak_before < 255)
            require(peak_after < 255, "Whitening must not clip a previously unclipped skin swatch");
        if (swatch == 1)
            midtone_lift = peak_after - peak_before;
        if (swatch == 3)
            highlight_lift = peak_after - peak_before;
    }
    require(midtone_lift > 10 && highlight_lift < midtone_lift / 3,
            "Highlight lift must taper while skin midtones still brighten");
    require(pixel(white, 52, 16) == pixel(original, 52, 16) &&
            pixel(white, 60, 16) == pixel(original, 60, 16), "Black and white endpoints must remain unchanged");
    std::printf("PASS: whitening retains RGB proportions and unclipped detail; midtone/highlight lift=%d/%d\n",
                midtone_lift, highlight_lift);
    obs_data_set_double(settings.get(), "rosy", 100);
    const auto combined = render_settings(settings.get(), 8);
    require(pixel(combined, 12, 16)[0] > pixel(white, 12, 16)[0],
            "Rosy must remain visible on whitened skin midtones");
    for (uint32_t swatch : {3u, 4u}) {
        const auto before = pixel(original, swatch * 8 + 4, 16);
        const auto after = pixel(combined, swatch * 8 + 4, 16);
        require(after[0] < 255 && after[0] - before[0] <= 3,
                "Combined whitening and rosy must limit highlight reddening and avoid clipping");
    }
    for (size_t i = 3; i < combined.size(); i += 4)
        require(combined[i] == original[i], "Color protection must preserve alpha");
    std::puts("PASS: rosy remains active on midtones and subdued on bright highlights without changing alpha");
    auto floating_source = make_fixture(9, 1, 1);
    const auto floating_before = capture(floating_source.get());
    auto lighting = neutral_settings();
    obs_data_set_double(lighting.get(), "exposure", -1);
    auto floating_filter = attach_settings(floating_source.get(), lighting.get());
    const auto floating_after = capture(floating_source.get());
    for (size_t c = 0; c < 3; ++c)
        require(std::abs(floating_after[c] * 2 - floating_before[c]) <= 2,
                "Highlight protection must not normalize SDR float overshoot when beauty is off");
    require(floating_after[3] == floating_before[3], "Float input must preserve alpha");
    std::puts("PASS: SDR float overshoot retains the requested exposure without extra normalization");
}

struct Benchmark {
    obs_source_t *source = nullptr;
    gs_texrender_t *output = nullptr;
    gs_timer_t *timer = nullptr;
    gs_timer_range_t *range = nullptr;
    unsigned frames = 0;
    bool failed = false;
    std::vector<double> samples;
    std::vector<obs_source_t *> sources;
    std::vector<obs_source_t *> filters;
    unsigned sample_limit = 90;
    bool change_parameters = false;
    std::atomic<bool> done{false};
};

void benchmark_frame(void *data, uint32_t, uint32_t)
{
    auto *benchmark = static_cast<Benchmark *>(data);
    if (benchmark->done.load())
        return;
    if (benchmark->change_parameters && benchmark->frames % update_interval == 0) {
        const unsigned step = benchmark->frames / update_interval;
        Settings settings(obs_data_create(), obs_data_release);
        obs_data_set_double(settings.get(), "strength", 40 + step % 61);
        obs_data_set_double(settings.get(), "smoothing", 20 + step % 81);
        obs_data_set_double(settings.get(), "whitening", step % 101);
        obs_data_set_double(settings.get(), "rosy", step % 31);
        obs_data_set_double(settings.get(), "temperature", static_cast<int>(step % 101) - 50);
        obs_data_set_double(settings.get(), "exposure", (static_cast<int>(step % 41) - 20) * 0.05);
        obs_data_set_double(settings.get(), "skin_range", 50 + step % 151);
        obs_data_set_double(settings.get(), "blemish", step * 7 % 101);
        obs_data_set_double(settings.get(), "sharpness", step * 11 % 101);
        obs_data_set_double(settings.get(), "shadow_fill", step * 13 % 101);
        obs_data_set_double(settings.get(), "color_saturation", static_cast<int>(step * 17 % 201) - 100);
        obs_data_set_int(settings.get(), "skin_preview",
                         step % 7 == 0 ? 4 : (step % 13 == 0 ? 2 : (step % 11 == 0 ? 1 : 0)));
        for (auto *filter : benchmark->filters)
            obs_source_update(filter, settings.get());
    }
#ifdef __APPLE__
    const auto started = std::chrono::steady_clock::now();
#else
    gs_timer_range_begin(benchmark->range);
    gs_timer_begin(benchmark->timer);
#endif
    bool rendered = true;
    if (benchmark->sources.empty())
        rendered = draw_source(benchmark->source, benchmark->output);
    else
        for (auto *source : benchmark->sources)
            rendered = draw_source(source, benchmark->output) && rendered;
#ifdef __APPLE__
    // Include completed rendering in wall time.
    glFinish();
    const bool measured = rendered;
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
#else
    gs_timer_end(benchmark->timer);
    gs_timer_range_end(benchmark->range);
    gs_flush();
    uint64_t ticks = 0;
    uint64_t frequency = 0;
    bool disjoint = true;
    const bool measured = rendered && gs_timer_get_data(benchmark->timer, &ticks) &&
                          gs_timer_range_get_data(benchmark->range, &disjoint, &frequency) && !disjoint && frequency;
    const double milliseconds = frequency ? static_cast<double>(ticks) / static_cast<double>(frequency) * 1000.0 : 0;
#endif
    if (!measured) {
        benchmark->failed = true;
        benchmark->done.store(true);
        return;
    }
    if (++benchmark->frames > warmup_frames)
        benchmark->samples.push_back(milliseconds);
    if (benchmark->samples.size() >= benchmark->sample_limit)
        benchmark->done.store(true);
}

double measure(Benchmark &state)
{
    state.samples.reserve(state.sample_limit);
    obs_enter_graphics();
    state.output = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
#ifndef __APPLE__
    state.timer = gs_timer_create();
    state.range = gs_timer_range_create();
#endif
    obs_leave_graphics();
#ifdef __APPLE__
    const bool allocated = state.output;
#else
    const bool allocated = state.output && state.timer && state.range;
#endif
    const auto started = std::chrono::steady_clock::now();
    if (allocated) {
        obs_add_main_render_callback(benchmark_frame, &state);
        const auto deadline = started + std::chrono::seconds(render_timeout_seconds);
        while (!state.done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        obs_remove_main_render_callback(benchmark_frame, &state);
    }
    obs_enter_graphics();
    gs_timer_range_destroy(state.range);
    gs_timer_destroy(state.timer);
    gs_texrender_destroy(state.output);
    obs_leave_graphics();
    require(allocated && state.done.load() && !state.failed && state.samples.size() == state.sample_limit,
            "Rendering benchmark failed or timed out");
    std::sort(state.samples.begin(), state.samples.end());
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void benchmark(const char *label, double smoothing, double whitening, double rosy, int fixture = 1,
               double exposure = 0, int preview = 0, bool all_features = false)
{
    auto source = make_fixture(fixture, 1920, 1080);
    auto settings = neutral_settings();
    obs_data_set_double(settings.get(), "smoothing", smoothing);
    obs_data_set_double(settings.get(), "whitening", whitening);
    obs_data_set_double(settings.get(), "rosy", rosy);
    obs_data_set_double(settings.get(), "exposure", exposure);
    obs_data_set_int(settings.get(), "skin_preview", preview);
    if (all_features)
        for (const char *key : {"blemish", "sharpness", "shadow_fill", "color_saturation"})
            obs_data_set_double(settings.get(), key, 100);
    auto filter = attach_settings(source.get(), settings.get());
    Benchmark state;
    state.source = source.get();
    measure(state);
    std::printf("BENCH 1920x1080 %s: %s median=%.4f ms p95=%.4f ms (90 frames after %u warmup frames)\n",
                label, timing_label, state.samples[45], state.samples[85], warmup_frames);
    std::fflush(stdout);
}

double private_memory_mib()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters = {};
    counters.cb = sizeof(counters);
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                                 sizeof(counters)) != 0, "Cannot query process private memory");
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
#else
    task_vm_info_data_t counters = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    require(task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&counters), &count) == KERN_SUCCESS,
            "Cannot query process physical footprint");
    return static_cast<double>(counters.phys_footprint) / (1024.0 * 1024.0);
#endif
}

void stress_case(const char *label, uint32_t width, uint32_t height, unsigned instances,
                 unsigned samples, bool dynamic)
{
    std::printf("STRESS START %s\n", label);
    std::fflush(stdout);
    std::vector<Source> sources, filters;
    Benchmark state;
    state.sample_limit = samples;
    state.change_parameters = dynamic;
    for (unsigned i = 0; i < instances; ++i) {
        sources.push_back(make_fixture(dynamic ? 1 : 7, width, height));
        Settings settings(obs_data_create(), obs_data_release);
        obs_data_set_double(settings.get(), "smoothing", 100);
        obs_data_set_double(settings.get(), "whitening", 100);
        obs_data_set_double(settings.get(), "rosy", 100);
        obs_data_set_double(settings.get(), "exposure", 0.5);
        obs_data_set_double(settings.get(), "temperature", 15);
        for (const char *key : {"blemish", "sharpness", "shadow_fill", "color_saturation"})
            obs_data_set_double(settings.get(), key, 100);
        filters.push_back(attach_settings(sources.back().get(), settings.get()));
        state.sources.push_back(sources.back().get());
        state.filters.push_back(filters.back().get());
    }
    const double elapsed = measure(state);
    const auto over_budget = std::count_if(state.samples.begin(), state.samples.end(), [](double ms) { return ms > 16.667; });
    std::printf("STRESS %s: samples=%zu wall=%.2fs rate=%.2f fps %s median=%.4fms p95=%.4fms max=%.4fms over16.67ms=%lld private=%.2fMiB\n",
                label, state.samples.size(), elapsed, state.frames / elapsed, timing_label,
                state.samples[state.samples.size() / 2], state.samples[state.samples.size() * 95 / 100],
                state.samples.back(), static_cast<long long>(over_budget), private_memory_mib());
    std::fflush(stdout);

}

void drain_task(void *) {}

void lifecycle_stress()
{
    double baseline = 0, final_memory = 0;
    for (unsigned i = 0; i < 550; ++i) {
        {
            auto source = make_fixture(static_cast<int>(i % 3));
            Settings settings(obs_data_create(), obs_data_release);
            auto filter = attach_settings(source.get(), settings.get());
            Properties props(obs_source_properties(filter.get()), obs_properties_destroy);
            Settings live(obs_source_get_settings(filter.get()), obs_data_release);
            const char *slots[] = {"saved", "saved_2", "saved_3"};
            for (unsigned slot = 0; slot < 3; ++slot) {
                obs_data_set_string(live.get(), "preset", slots[slot]);
                obs_property_modified(obs_properties_get(props.get(), "preset"), live.get());
                obs_data_set_double(live.get(), "strength", (i + slot) % 101);
                const std::string name = "Stress " + std::to_string(i) + "." + std::to_string(slot);
                obs_data_set_string(live.get(), "preset_name", name.c_str());
                obs_property_button_clicked(obs_properties_get(props.get(), "save_preset"), filter.get());
            }
            obs_data_set_string(live.get(), "preset", i % 2 ? "warm" : slots[i % 3]);
            obs_property_modified(obs_properties_get(props.get(), "preset"), live.get());
            obs_source_update(filter.get(), live.get());
            if (i % 5 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(35));
                const auto output = capture(source.get());
                require(output.size() == 64 * 64 * 4, "Lifecycle render produced an invalid image");
            }
        }
        if ((i + 1) % 50 == 0) {
            obs_queue_task(OBS_TASK_DESTROY, drain_task, nullptr, true);
            obs_queue_task(OBS_TASK_GRAPHICS, drain_task, nullptr, true);
            final_memory = private_memory_mib();
            if (i == 49)
                baseline = final_memory;
            std::printf("LIFECYCLE cycles=%u private=%.2fMiB\n", i + 1, final_memory);
            std::fflush(stdout);
        }
    }
    require(final_memory - baseline < 64, "Lifecycle stress exceeded 64 MiB growth after warmup");
    std::printf("LIFECYCLE PASS measured=500 after50warmup private_growth=%.2fMiB\n", final_memory - baseline);
}

void stress_tests()
{
    stress_warnings.store(0);
    count_stress_warnings.store(true);
#ifdef BEAUTY_CI_STRESS
    // The hosted Mac uses software OpenGL. Retain resolution/load and lifecycle coverage.
    constexpr unsigned frames[] = {30, 15, 15};
    std::puts("STRESS: CI software-rendering load, 60 measured frames plus 5 warmup per case; 550 lifecycle cycles");
#else
    constexpr unsigned frames[] = {3600, 1800, 1800};
#endif
    stress_case("1x1080p dynamic", 1920, 1080, 1, frames[0], true);
    stress_case("1x4K maximum beauty + lighting", 3840, 2160, 1, frames[1], false);
    stress_case("4x1080p dynamic", 1920, 1080, 4, frames[2], true);
    lifecycle_stress();
    count_stress_warnings.store(false);
    require(stress_warnings.load() == 0, "Stress run produced unexpected OBS warnings/errors");
    std::puts("ALL STRESS TESTS PASSED (zero unexpected OBS warnings/errors)");
}
} // namespace

int beauty_test_main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 4 && !(argc == 5 && (std::string(argv[4]) == "--stress" || std::string(argv[4]) == "--functional"))) {
        std::fprintf(stderr, "Usage: beauty-integration <plugin> <data-directory> <OBS-root> [--stress|--functional]\n");
        return 2;
    }
    try {
        base_set_log_handler(test_log, nullptr);
        const auto obs_root = std::filesystem::u8path(argv[3]);
#ifdef _WIN32
        std::filesystem::current_path(obs_root / "bin" / "64bit");
#else
        std::filesystem::current_path(obs_root / "Contents" / "MacOS");
#endif
        require(obs_startup("zh-CN", nullptr, nullptr), "Cannot initialize libobs");
        require(obs_get_version() == MAKE_SEMANTIC_VERSION(32, 0, 4), "Runtime must be OBS Studio 32.0.4");
#ifdef _WIN32
        const std::string graphics = (obs_root / "bin" / "64bit" / "libobs-d3d11.dll").u8string();
#else
        const std::string graphics = (obs_root / "Contents" / "Frameworks" / "libobs-opengl.dylib").u8string();
#endif
        obs_video_info video = {};
        video.graphics_module = graphics.c_str();
        video.fps_num = 60;
        video.fps_den = 1;
        video.base_width = 1920;
        video.base_height = 1080;
        video.output_width = 1920;
        video.output_height = 1080;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_BILINEAR;
        require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "Cannot initialize the OBS graphics renderer");
#ifdef __APPLE__
        obs_enter_graphics();
        std::printf("RENDERER: %s\n", reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
        obs_leave_graphics();
#endif
        obs_module_t *module = nullptr;
        require(obs_open_module(&module, argv[1], argv[2]) == MODULE_SUCCESS, "Cannot open built plugin DLL");
        require(obs_init_module(module), "Cannot initialize beauty plugin");
        obs_source_info fixture = {};
        fixture.id = "beauty_test_fixture";
        fixture.type = OBS_SOURCE_TYPE_INPUT;
        fixture.output_flags = OBS_SOURCE_VIDEO;
        fixture.get_name = fixture_name;
        fixture.create = fixture_create;
        fixture.destroy = fixture_destroy;
        fixture.get_width = fixture_width;
        fixture.get_height = fixture_height;
        fixture.video_render = fixture_render;
        fixture.video_get_color_space = fixture_space;
        obs_register_source(&fixture);
        {
            auto probe = make_fixture();
            require(pixel(capture(probe.get()), 16, 16) == std::array<int, 4>{178, 130, 107, 255},
                    "Graphics fixture must render the known reference color before testing");
        }
        if (argc == 5 && std::string(argv[4]) == "--stress") {
            stress_tests();
        } else {
            stress_warnings.store(0);
            count_stress_warnings.store(true);
            rendering_tests();
            feature_tests();
            algorithm_tests();
            smoothing_quality_tests();
            comparison_tests();
            custom_slot_tests();
            slider_ui_tests();
            preset_name_tests();
            extra_effect_tests();
            color_protection_tests();
            if (argc != 5) {
            benchmark("bypass", 0.0, 0.0, 0.0);
            benchmark("lighting only", 0.0, 0.0, 0.0, 1, 0.25);
            benchmark("tone only", 0.0, 15.0, 5.0);
            benchmark("default beauty", 45.0, 20.0, 10.0);
            benchmark("maximum beauty + all new effects", 100.0, 100.0, 100.0, 1, 0, 0, true);
            benchmark("no skin, maximum smoothing", 100.0, 0.0, 0.0, 6);
            benchmark("split maximum beauty + all new effects", 100.0, 100.0, 100.0, 1, 0, 3, true);
            }
            count_stress_warnings.store(false);
            require(stress_warnings.load() == 0, "Functional tests produced unexpected OBS warnings/errors");
        }
        obs_shutdown();
        std::puts("ALL INTEGRATION TESTS PASSED");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        if (obs_initialized())
            obs_shutdown();
        return 1;
    }
}

#ifndef __APPLE__
int main(int argc, char **argv) { return beauty_test_main(argc, argv); }
#endif
