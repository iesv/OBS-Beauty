// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreGraphics/CGEventSource.h>
#endif
#include <obs-module.h>
#include <util/platform.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <new>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-beauty", "en-US")
OBS_MODULE_AUTHOR("haliluya.net")
static_assert(LIBOBS_API_VER == MAKE_SEMANTIC_VERSION(32, 0, 4), "Use OBS Studio 32.0.4 headers.");

namespace {
enum Numeric {
    Strength, Smoothing, BlemishReduction, Whitening, Rosy, Sharpness,
    SkinRange, Exposure, ShadowFill, Temperature, Saturation, NumericCount
};
struct NumberSpec {
    const char *key;
    const char *label;
    double initial, minimum, maximum, scale;
    double step = 1.0;
};
constexpr NumberSpec numbers[] = {
    {"strength", "Strength", 100, 0, 100, 0.01},
    {"smoothing", "Smoothing", 45, 0, 100, 0.01},
    {"blemish", "BlemishReduction", 0, 0, 100, 0.01},
    {"whitening", "Whitening", 20, 0, 100, 0.01},
    {"rosy", "Rosy", 10, 0, 100, 0.01},
    {"sharpness", "Sharpness", 0, 0, 100, 0.01},
    {"skin_range", "SkinRange", 100, 50, 200, 0.01},
    {"exposure", "Exposure", 0, -3, 3, 1, 0.05},
    {"shadow_fill", "ShadowFill", 0, 0, 100, 0.01},
    {"temperature", "Temperature", 0, -100, 100, 0.01},
    {"color_saturation", "Saturation", 0, -100, 100, 0.01},
};
static_assert(std::size(numbers) == NumericCount);
constexpr const char *saved_keys[] = {"saved_preset", "saved_preset_2", "saved_preset_3"};
constexpr const char *saved_ids[] = {"saved", "saved_2", "saved_3"};
constexpr const char *saved_name_keys[] = {"saved_preset_name", "saved_preset_name_2", "saved_preset_name_3"};
constexpr int split_preview = 3;

struct BeautyFilter {
    obs_source_t *source = nullptr;
    gs_effect_t *effect = nullptr;
    std::array<gs_eparam_t *, NumericCount> params = {};
    std::array<float, NumericCount> values = {};
    gs_eparam_t *pixel_step = nullptr, *preview_param = nullptr;
    int preview = 0;
    bool show_original = false;
};

void refresh_preset_ui(void *data, float);

struct PresetUi {
    std::string selection;
    std::string displayed_selection;
    // The panel must not retain a filter that has been removed.
    obs_weak_source_t *source = nullptr;
    std::atomic<uint64_t> refresh_after{0};

    ~PresetUi()
    {
        if (source) {
            // Stop video-thread callbacks before releasing their state.
            obs_remove_tick_callback(refresh_preset_ui, this);
            obs_weak_source_release(source);
        }
    }
};

bool pointer_button_down()
{
#ifdef _WIN32
    return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
#else
    return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft) ||
           CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonRight);
#endif
}

void refresh_preset_ui(void *data, float)
{
    auto *ui = static_cast<PresetUi *>(data);
    uint64_t deadline = ui->refresh_after.load();
    // Coalesce edits and wait until the pointer is released.
    if (!deadline || os_gettime_ns() < deadline || pointer_button_down() ||
        !ui->refresh_after.compare_exchange_strong(deadline, 0))
        return;
    auto *source = obs_weak_source_get_source(ui->source);
    if (source) {
        obs_source_update_properties(source);
        obs_source_release(source);
    }
}

double number_value(obs_data_t *settings, const NumberSpec &spec)
{
    double value = spec.initial;
    if (obs_data_has_user_value(settings, spec.key) || obs_data_has_default_value(settings, spec.key))
        value = obs_data_get_double(settings, spec.key);
    // Scene data must stay finite and within the shader's supported range.
    return std::isfinite(value) ? std::clamp(value, spec.minimum, spec.maximum) : spec.initial;
}

obs_data_t *snapshot(obs_data_t *settings)
{
    // Presets contain effect values, excluding preview and panel state.
    auto *saved = obs_data_create();
    for (const auto &spec : numbers)
        obs_data_set_double(saved, spec.key, number_value(settings, spec));
    return saved;
}

void update(void *data, obs_data_t *settings)
{
    auto *filter = static_cast<BeautyFilter *>(data);
    if (!filter || !settings)
        return;
    for (size_t i = 0; i < NumericCount; ++i)
        filter->values[i] = static_cast<float>(number_value(settings, numbers[i]) * numbers[i].scale);
    filter->show_original = obs_data_get_bool(settings, "show_original");
    const auto preview = obs_data_get_int(settings, "skin_preview");
    filter->preview = preview >= 0 && preview <= split_preview ? static_cast<int>(preview) : 0;
    if (preview != filter->preview)
        obs_data_set_int(settings, "skin_preview", filter->preview);
}

void destroy(void *data)
{
    auto *filter = static_cast<BeautyFilter *>(data);
    if (!filter)
        return;
    if (filter->effect) {
        obs_enter_graphics();
        gs_effect_destroy(filter->effect);
        obs_leave_graphics();
    }
    delete filter;
}

void *create(obs_data_t *settings, obs_source_t *source)
{
    if (!settings || !source)
        return nullptr;
    auto *filter = new (std::nothrow) BeautyFilter;
    if (!filter)
        return nullptr;
    filter->source = source;
    char *path = obs_module_file("beauty.effect");
    char *errors = nullptr;
    obs_enter_graphics();
    if (path)
        filter->effect = gs_effect_create_from_file(path, &errors);
    // Validate the complete effect before any render callback can use it.
    bool valid = filter->effect != nullptr;
    if (filter->effect) {
        for (size_t i = 0; i < NumericCount; ++i) {
            filter->params[i] = gs_effect_get_param_by_name(filter->effect, numbers[i].key);
            valid = valid && filter->params[i];
        }
        filter->pixel_step = gs_effect_get_param_by_name(filter->effect, "pixel_step");
        filter->preview_param = gs_effect_get_param_by_name(filter->effect, "skin_preview");
        valid = valid && filter->pixel_step && filter->preview_param &&
                gs_effect_get_technique(filter->effect, "DrawSmooth") && gs_effect_get_technique(filter->effect, "DrawColor");
    }
    obs_leave_graphics();
    bfree(path);
    if (!valid)
        blog(LOG_ERROR, "[obs-beauty] Cannot load beauty.effect: %s", errors ? errors : "missing file or parameters");
    bfree(errors);
    if (!valid) {
        destroy(filter);
        return nullptr;
    }
    update(filter, settings);
    return filter;
}

gs_color_space color_space(void *data, size_t, const gs_color_space *)
{
    auto *filter = static_cast<BeautyFilter *>(data);
    if (!filter || !filter->source)
        return GS_CS_SRGB;
    obs_source_t *target = obs_filter_get_target(filter->source);
    if (!target)
        return GS_CS_SRGB;
    const gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
    return obs_source_get_color_space(target, OBS_COUNTOF(spaces), spaces);
}

void render(void *data, gs_effect_t *)
{
    auto *filter = static_cast<BeautyFilter *>(data);
    if (!filter || !filter->source)
        return;
    if (!filter->effect) {
        obs_source_skip_video_filter(filter->source);
        return;
    }
    obs_source_t *target = obs_filter_get_target(filter->source);
    if (!target)
        return;
    const auto &v = filter->values;
    const auto space = color_space(filter, 0, nullptr);
    const bool neutral = v[Smoothing] == 0 && v[BlemishReduction] == 0 && v[Whitening] == 0 && v[Rosy] == 0 &&
                         v[Sharpness] == 0 && v[Temperature] == 0 && v[Exposure] == 0 &&
                         v[ShadowFill] == 0 && v[Saturation] == 0;
    // Preserve HDR output and avoid GPU work for an unchanged normal view.
    if (filter->show_original || (filter->preview == 0 && (v[Strength] == 0 || neutral)) ||
        (space != GS_CS_SRGB && space != GS_CS_SRGB_16F)) {
        obs_source_skip_video_filter(filter->source);
        return;
    }
    const uint32_t width = obs_source_get_base_width(target), height = obs_source_get_base_height(target);
    if (!width || !height ||
        !obs_source_process_filter_begin_with_color_space(filter->source, gs_get_format_from_space(space), space,
                                                          OBS_NO_DIRECT_RENDERING))
        return;
    // Scale by the shorter edge; radius stays between 1.5 and 6 pixels.
    const float radius = 1.5f * std::clamp(static_cast<float>(std::min(width, height)) / 1080.0f, 1.0f, 4.0f);
    vec2 step;
    vec2_set(&step, radius / static_cast<float>(width), radius / static_cast<float>(height));
    gs_effect_set_vec2(filter->pixel_step, &step);
    for (size_t i = 0; i < NumericCount; ++i)
        gs_effect_set_float(filter->params[i], v[i]);
    gs_effect_set_int(filter->preview_param, filter->preview);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    // Color adjustment and mask previews need only the single-sample path.
    obs_source_process_filter_tech_end(filter->source, filter->effect, 0, 0,
                                     (v[Smoothing] > 0 || v[BlemishReduction] > 0 || v[Sharpness] > 0) &&
                                             (filter->preview == 0 || filter->preview == split_preview)
                                         ? "DrawSmooth" : "DrawColor");
    gs_blend_state_pop();
}

void defaults(obs_data_t *settings)
{
    for (const auto &spec : numbers)
        obs_data_set_default_double(settings, spec.key, spec.initial);
    obs_data_set_default_bool(settings, "show_original", false);
    obs_data_set_default_int(settings, "skin_preview", 0);
    obs_data_set_default_string(settings, "preset", "natural");
    // A nonempty info value enables OBS's native tooltip icon beside the heading.
    obs_data_set_default_string(settings, "beauty_heading", " ");
}

int preset_slot(const std::string &id)
{
    for (int i = 0; i < 3; ++i)
        if (id == saved_ids[i])
            return i;
    return -1;
}

std::string clean_preset_name(const char *text)
{
    std::string name;
    if (!text)
        return name;
    unsigned characters = 0;
    // Count complete UTF-8 code points.
    for (const char *p = text; *p; ++p) {
        const auto byte = static_cast<unsigned char>(*p);
        if (byte < 0x20 || byte == 0x7f || (name.empty() && byte == ' '))
            continue;
        if ((byte & 0xc0) != 0x80 && characters++ == 20)
            break;
        name += *p;
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name;
}

std::string preset_base(obs_data_t *settings)
{
    std::string id = settings ? obs_data_get_string(settings, "preset") : "natural";
    if (id == "modified")
        id = obs_data_get_string(settings, "preset_base");
    return preset_slot(id) >= 0 || id == "clear" || id == "warm" ? id : "natural";
}

obs_data_t *preset_values(obs_data_t *settings, const std::string &id)
{
    const int slot = preset_slot(id);
    if (slot >= 0)
        return obs_data_get_obj(settings, saved_keys[slot]);
    auto *values = obs_data_create();
    for (const auto &spec : numbers)
        obs_data_set_double(values, spec.key, spec.initial);
    if (id == "clear") {
        obs_data_set_double(values, "whitening", 25);
        obs_data_set_double(values, "rosy", 4);
        obs_data_set_double(values, "exposure", 0.1);
    } else if (id == "warm") {
        obs_data_set_double(values, "smoothing", 30);
        obs_data_set_double(values, "whitening", 12);
        obs_data_set_double(values, "rosy", 8);
        obs_data_set_double(values, "temperature", 15);
    }
    return values;
}

std::string preset_label(obs_data_t *settings, const std::string &id)
{
    const int slot = preset_slot(id);
    if (slot < 0)
        return obs_module_text(id == "clear" ? "PresetClear" : id == "warm" ? "PresetWarm" : "PresetNatural");
    auto *saved = settings ? obs_data_get_obj(settings, saved_keys[slot]) : nullptr;
    const std::string key = "PresetSaved" + std::to_string(slot + 1) + (saved ? "" : "Empty");
    std::string label = obs_module_text(key.c_str());
    const std::string name = saved ? clean_preset_name(obs_data_get_string(settings, saved_name_keys[slot])) : "";
    if (!name.empty())
        label += ": " + name;
    obs_data_release(saved);
    return label;
}

void sync_preset_ui(obs_properties_t *props, obs_data_t *settings, bool load_name)
{
    auto *preset = obs_properties_get(props, "preset");
    obs_property_list_clear(preset);
    for (const char *id : {"natural", "clear", "warm", "saved", "saved_2", "saved_3"})
        obs_property_list_add_string(preset, preset_label(settings, id).c_str(), id);
    const std::string base = preset_base(settings);
    if (settings && std::string(obs_data_get_string(settings, "preset")) == "modified") {
        const std::string label = preset_label(settings, base) + " " + obs_module_text("PresetModified");
        obs_property_list_add_string(preset, label.c_str(), "modified");
    }
    const int slot = preset_slot(base);
    obs_property_set_enabled(obs_properties_get(props, "save_preset"), slot >= 0);
    obs_property_set_enabled(obs_properties_get(props, "preset_name"), slot >= 0);
    if (settings && load_name) {
        const std::string name = slot >= 0 ? clean_preset_name(obs_data_get_string(settings, saved_name_keys[slot])) : "";
        obs_data_set_string(settings, "preset_name", name.c_str());
    }
}

bool parameters_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    auto *ui = static_cast<PresetUi *>(obs_properties_get_param(props));
    if (!ui || !settings)
        return false;
    const std::string base = preset_base(settings);
    auto *values = preset_values(settings, base);
    bool matches = true;
    if (values) {
        for (const auto &spec : numbers)
            matches = matches && std::abs(number_value(settings, spec) - number_value(values, spec)) < 0.000001;
    }
    obs_data_release(values);
    const std::string selection = matches ? base : "modified";
    const bool changed = ui->selection != selection;
    obs_data_set_string(settings, "preset_base", base.c_str());
    obs_data_set_string(settings, "preset", selection.c_str());
    ui->selection = selection;
    if (changed)
        sync_preset_ui(props, settings, false);
    // Keep active controls intact; coalesce preset-label refreshes after editing.
    if (property && ui->source)
        ui->refresh_after.store(selection == ui->displayed_selection ? 0 : os_gettime_ns() + 500000000ULL);
    return false;
}

bool preset_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
    auto *ui = static_cast<PresetUi *>(obs_properties_get_param(props));
    if (!ui || !settings)
        return false;
    const std::string selection = obs_data_get_string(settings, "preset");
    // Initialization must preserve current values.
    if (selection == ui->selection)
        return false;
    ui->refresh_after.store(0);
    const std::string base = preset_base(settings);
    auto *values = preset_values(settings, base);
    if (values) {
        auto *clean = snapshot(values);
        obs_data_apply(settings, clean);
        obs_data_release(clean);
        obs_data_set_bool(settings, "show_original", false);
        obs_data_set_int(settings, "skin_preview", 0);
    }
    obs_data_release(values);
    obs_data_set_string(settings, "preset_base", base.c_str());
    obs_data_set_string(settings, "preset", base.c_str());
    ui->selection = base;
    ui->displayed_selection = base;
    sync_preset_ui(props, settings, true);
    return true;
}

bool save_preset(obs_properties_t *props, obs_property_t *, void *data)
{
    auto *filter = static_cast<BeautyFilter *>(data);
    if (!filter || !filter->source)
        return false;
    auto *settings = obs_source_get_settings(filter->source);
    const std::string base = preset_base(settings);
    const int slot = preset_slot(base);
    if (slot < 0) {
        obs_data_release(settings);
        return false;
    }
    auto *saved = snapshot(settings);
    obs_data_set_obj(settings, saved_keys[slot], saved);
    obs_data_release(saved);
    const std::string name = clean_preset_name(obs_data_get_string(settings, "preset_name"));
    obs_data_set_string(settings, saved_name_keys[slot], name.c_str());
    obs_data_set_string(settings, "preset_base", base.c_str());
    obs_data_set_string(settings, "preset", base.c_str());
    auto *ui = static_cast<PresetUi *>(obs_properties_get_param(props));
    if (ui) {
        ui->refresh_after.store(0);
        ui->selection = base;
        ui->displayed_selection = base;
    }
    sync_preset_ui(props, settings, true);
    obs_source_update(filter->source, settings);
    obs_data_release(settings);
    return true;
}

void add_slider(obs_properties_t *props, Numeric index)
{
    const auto &spec = numbers[index];
    auto *slider = obs_properties_add_float_slider(props, spec.key, obs_module_text(spec.label), spec.minimum, spec.maximum, spec.step);
    obs_property_set_modified_callback(slider, parameters_modified);
}

obs_properties_t *properties(void *data)
{
    auto *filter = static_cast<BeautyFilter *>(data);
    auto *ui = new (std::nothrow) PresetUi;
    if (!ui)
        return nullptr;
    obs_properties_t *props = obs_properties_create_param(ui, [](void *value) { delete static_cast<PresetUi *>(value); });
    if (filter && filter->source) {
        ui->source = obs_source_get_weak_source(filter->source);
        if (ui->source)
            obs_add_tick_callback(refresh_preset_ui, ui);
    }
    // A flat common section keeps the native heading compact.
    const std::string heading = "<b>" + std::string(obs_module_text("BeautyGroup")) + "</b>";
    auto *sdr = obs_properties_add_text(props, "beauty_heading", heading.c_str(), OBS_TEXT_INFO);
    obs_property_set_long_description(sdr, obs_module_text("SdrHint"));
    obs_property_text_set_info_word_wrap(sdr, false);
    obs_properties_add_bool(props, "show_original", obs_module_text("ShowOriginal"));
    auto *preset = obs_properties_add_list(props, "preset", obs_module_text("Preset"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    auto *settings = filter && filter->source ? obs_source_get_settings(filter->source) : nullptr;
    obs_property_set_modified_callback(preset, preset_modified);
    auto *preset_name = obs_properties_add_text(props, "preset_name", obs_module_text("PresetName"), OBS_TEXT_DEFAULT);
    obs_property_set_modified_callback(preset_name, parameters_modified);
    obs_properties_add_button(props, "save_preset", obs_module_text("SavePreset"), save_preset);
    for (Numeric index : {Strength, Smoothing, BlemishReduction, Whitening, Rosy, Sharpness})
        add_slider(props, index);

    auto *skin = obs_properties_create();
    add_slider(skin, SkinRange);
    auto *preview = obs_properties_add_list(skin, "skin_preview", obs_module_text("SkinPreview"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(preview, obs_module_text("PreviewOff"), 0);
    obs_property_list_add_int(preview, obs_module_text("PreviewOverlay"), 1);
    obs_property_list_add_int(preview, obs_module_text("PreviewMask"), 2);
    obs_property_list_add_int(preview, obs_module_text("PreviewSplit"), split_preview);
    obs_properties_add_group(props, "skin_group", obs_module_text("SkinGroup"), OBS_GROUP_NORMAL, skin);

    auto *color = obs_properties_create();
    for (Numeric index : {Exposure, ShadowFill, Temperature, Saturation})
        add_slider(color, index);
    obs_properties_add_group(props, "color_group", obs_module_text("ColorGroup"), OBS_GROUP_NORMAL, color);
    const std::string footer = "<p align=\"right\">OBS-Beauty: " PLUGIN_VERSION " &nbsp; " +
                               std::string(obs_module_text("Developer")) +
                               ": <a href=\"https://haliluya.net\">haliluya.net</a></p>";
    obs_properties_add_text(props, "version_info", footer.c_str(), OBS_TEXT_INFO);
    ui->selection = settings ? obs_data_get_string(settings, "preset") : "natural";
    parameters_modified(props, nullptr, settings);
    ui->displayed_selection = ui->selection;
    sync_preset_ui(props, settings, settings && !obs_data_has_user_value(settings, "preset_name"));
    obs_data_release(settings);
    return props;
}

const char *name(void *) { return obs_module_text("BeautyFilter"); }
} // namespace

bool obs_module_load(void)
{
    obs_source_info info = {};
    info.id = "obs_beauty_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = name;
    info.create = create;
    info.destroy = destroy;
    info.update = update;
    info.video_render = render;
    info.get_properties = properties;
    info.get_defaults = defaults;
    info.video_get_color_space = color_space;
    obs_register_source(&info);
    blog(LOG_INFO, "[obs-beauty] Loaded version %s (SDR GPU filter).", PLUGIN_VERSION);
    return true;
}

const char *obs_module_description(void)
{
    return "Lightweight SDR skin smoothing, beauty presets and basic lighting for OBS Studio 32.0.4.";
}
