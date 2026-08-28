/*
 * Minimal Matter Color Temperature Light — fortieth device type, and this
 * repo's first tunable-white bulb: cool-white + warm-white channels only,
 * no RGB at all, matching real "tunable white"/"CCT" smart-bulb products.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 * Works on other ESP32 chips too (C3, C6, S3, H2) — see the README for how to
 * switch target. Note: C6/H2 additionally support Thread; classic ESP32 is Wi-Fi.
 *
 * --- Why this is a separate device type from firmware/color-light/ --------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * ColorTemperatureLight.xml (fetched from inside the esp-matter SDK image,
 * not assumed): id 0x010C, classification "superset: Dimmable Light" —
 * Identify + Groups + OnOff[Lighting] + LevelControl[OnOff+Lighting] +
 * ScenesManagement + ColorControl are ALL mandatoryConform, but ColorControl
 * only requires its ColorTemperature (CT) feature — HueSaturation and XY
 * are not part of this device type's cluster requirements at all (unlike
 * ExtendedColorLight, 0x010D, which firmware/color-light/ and
 * firmware/addressable-light/ both implement and which mandates ALL THREE
 * color features). This is the correct, spec-conformant device type for
 * hardware that genuinely only does cool/warm white blending — a "tunable
 * white" bulb — rather than declaring ExtendedColorLight and only
 * half-implementing its mandatory color surface.
 *
 * --- endpoint::color_temperature_light::create() is a COMPLETE top-level
 * helper — unlike firmware/color-light/'s and firmware/addressable-light/'s
 * own hand-assembled ExtendedColorLight endpoints ------------------------
 * Confirmed by reading esp-matter's own legacy
 * data_model/legacy/esp_matter_endpoint.cpp `color_temperature_light::add()`
 * directly: it wires up Identify(+TriggerEffect) + Groups +
 * OnOff(Lighting feature, On+Toggle commands) +
 * LevelControl(OnOff+Lighting features) +
 * ColorControl(ColorTemperature feature + the mandatory RemainingTime
 * attribute + StopMoveStep command) + ScenesManagement(CopyScene+response)
 * — exactly matching the CSA XML above, nothing missing. And because this
 * goes through esp-matter's own `common::create<T>()` template (the same
 * shared internal path every complete top-level helper in this repo uses —
 * confirmed by reading that template directly), the Descriptor cluster is
 * created automatically too. This sidesteps BOTH real bugs that were found
 * and fixed on firmware/color-light/'s and firmware/addressable-light/'s
 * own hand-assembled endpoints during real Apple Home hardware testing (see
 * CLAUDE.md's "Open next steps" for the full debugging story): a missing
 * Descriptor cluster (which made Apple Home refuse to expose ANY control
 * tile at all), and missing mandatory ColorControl features/attributes.
 * Nothing to work around here — the complete helper already gets both
 * right, the same way it already did for firmware/occupancy-sensor/,
 * firmware/fan/, and every top-level-helper-based device type since.
 *
 * --- No color-space interlock needed -------------------------------------
 * firmware/color-light/'s RGBWW mode and firmware/addressable-light/'s
 * WS2805/SM2335EGH chips both need a runtime "which color space did the
 * controller last command" interlock, because their ColorControl cluster
 * exposes multiple mutually-exclusive color features (HueSaturation vs.
 * ColorTemperature) that can't be rendered by the same hardware channels
 * simultaneously. This device type has exactly ONE color feature
 * (ColorTemperature) — there is nothing to interlock against, so this file
 * is simpler: ColorTemperatureMireds is the only color input, always
 * rendered straight to the cool/warm channels.
 *
 * --- OnOff/LevelControl/ColorTemperatureMireds: plain ember attributes ---
 * Confirmed the same way as every prior light device type in this repo:
 * esp-matter's data_model_provider/clusters/ has no color_control/ or
 * level_control/ folder (listed directly, not assumed) — so none of these
 * three are one of the newer "code-driven" cluster classes (BooleanState,
 * TemperatureMeasurement, etc.) needing a registry-lookup setter. All three
 * go through the exact same attribute::PRE_UPDATE + attribute::update()
 * pattern firmware/dimmable-light/ and firmware/color-light/ already use.
 *
 * --- Cool/warm channel math: ESPHome's own formula, reused verbatim ------
 * mireds_to_cw_ww() below is copied unchanged from firmware/color-light/'s
 * own RGBWW-mode function of the same name, itself ported from ESPHome's
 * real light_call.cpp (LightCall::transform_parameters_): clamp the target
 * mireds into [cool_mireds, warm_mireds], linearly interpolate the warm
 * fraction from that position, then normalize both the warm and cool
 * fractions by whichever is larger so at least one channel always reaches
 * full strength at any color temperature — rather than both dimming
 * together at the midpoint, which is what a naive linear split would do.
 * COLOR_TEMPERATURE_LIGHT_COOL_WHITE_KELVIN/_WARM_WHITE_KELVIN (6500K/2700K)
 * are the same two most-common LED bin ratings firmware/color-light/'s own
 * header comment already documents — explicitly adjustable per your actual
 * LEDs' rated color temperature, not a universal constant.
 *
 * --- OnLevel left null — same real, hardware-confirmed bug fix -----------
 * A concrete OnLevel forces CurrentLevel back to that fixed value on every
 * plain OnOff::On (confirmed by reading connectedhomeip's own
 * level-control.cpp directly, and hardware-confirmed on
 * firmware/addressable-light/'s and firmware/dimmable-light/'s identical
 * fix) — leaving it null instead makes a plain On restore whatever
 * brightness was in effect right before the light went off, the "remembers
 * your last brightness" behavior every real dimmable/tunable-white light
 * has. See firmware/dimmable-light/'s own header comment for the full
 * detail on how this was found.
 *
 * --- Boots off, like every other device type in this repo -----------------
 * on_off_lighting.start_up_on_off is left at its config default (0 = Off) —
 * same convention every device type in this repo follows: start in a known,
 * deterministic state after a fresh boot or power cycle, not whatever it
 * happened to be doing before power was lost.
 *
 * Build-verified in Docker; not hardware-tested (no cool-white/warm-white
 * LED/driver board for this device type physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <cstring>
#include <cmath>

#include <esp_matter.h>

static const char *TAG = "matter_color_temperature_light";

/* Cool-white and warm-white LED channels. GPIO 2 and GPIO 4 are plain,
 * unreserved GPIOs on classic ESP32 (WROOM-32) that don't collide with each
 * other or with IDENTIFY_LED_GPIO below — the same "GPIO2 first output,
 * GPIO4 second" pattern firmware/dimmable-light/ already establishes for a
 * single-channel device, extended to two channels here. Both need to be
 * LEDC-capable pins — true for nearly every GPIO on these chips except
 * input-only ones (GPIO 34-39 on classic ESP32). Adjust to match your
 * board/driver hardware. */
#define COLOR_TEMPERATURE_LIGHT_COOL_WHITE_GPIO GPIO_NUM_2
#define COLOR_TEMPERATURE_LIGHT_WARM_WHITE_GPIO GPIO_NUM_4

/* The two ends of the tunable-white range this fixture can actually
 * produce, expressed as LED bin color temperatures — 6500K/2700K are the
 * same two most common "daylight"/"warm white" LED bin ratings
 * firmware/color-light/'s own RGBWW mode already documents. Converted to
 * mireds (the unit Matter's ColorControl cluster actually uses:
 * mireds = 1,000,000 / kelvin) for both the cluster's own
 * ColorTempPhysicalMin/MaxMireds attributes and the cool/warm conversion
 * math below. Explicitly adjustable per your actual LEDs' rated color
 * temperature — no universal default exists here, same disclaimer
 * firmware/color-light/'s header comment already carries. */
#define COLOR_TEMPERATURE_LIGHT_COOL_WHITE_KELVIN 6500
#define COLOR_TEMPERATURE_LIGHT_WARM_WHITE_KELVIN 2700
#define COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS (1000000 / COLOR_TEMPERATURE_LIGHT_COOL_WHITE_KELVIN) /* ~154 */
#define COLOR_TEMPERATURE_LIGHT_WARM_WHITE_MIREDS (1000000 / COLOR_TEMPERATURE_LIGHT_WARM_WHITE_KELVIN) /* ~370 */

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the light's own on/off/color-temperature state.
 * GPIO 15 is a plain, unreserved GPIO on classic ESP32 (WROOM-32) that
 * doesn't collide with either white channel above — same default
 * firmware/color-light/'s own identify LED already uses. Adjust to match
 * your board, or wire it to one of the white channels if you only have the
 * one LED and don't mind it blinking (at whatever brightness/color
 * temperature it's already at, on top of the blink) during identify. */
#define IDENTIFY_LED_GPIO GPIO_NUM_15
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

/* LEDC (LED Control / PWM) peripheral setup — one shared timer, two
 * channels. Same settings firmware/dimmable-light/'s and
 * firmware/color-light/'s own outputs already use: low-speed mode for
 * portability across every module this repo targets, auto clock source,
 * 8-bit (0-255) duty resolution, 5 kHz. */
#define COLOR_TEMPERATURE_LIGHT_LEDC_TIMER LEDC_TIMER_0
#define COLOR_TEMPERATURE_LIGHT_LEDC_COOL_WHITE_CHANNEL LEDC_CHANNEL_0
#define COLOR_TEMPERATURE_LIGHT_LEDC_WARM_WHITE_CHANNEL LEDC_CHANNEL_1
#define COLOR_TEMPERATURE_LIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define COLOR_TEMPERATURE_LIGHT_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define COLOR_TEMPERATURE_LIGHT_LEDC_FREQUENCY_HZ 5000

/* Initial brightness/color-temperature — used both for each attribute's
 * starting value and for what the light shows the first time it's turned
 * on after a fresh flash or factory reset (same reasoning
 * firmware/dimmable-light/'s own header comment documents: a null
 * attribute has no defined value, a worse first-read experience than a
 * concrete, reasonable one). Default color temperature is the cool end of
 * the range — arbitrary but matches firmware/color-light/'s own default. */
#define COLOR_TEMPERATURE_LIGHT_DEFAULT_LEVEL 128
#define COLOR_TEMPERATURE_LIGHT_DEFAULT_MIREDS COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t light_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Mirrors the Matter OnOff/LevelControl/ColorControl attributes' current
 * values — kept in sync solely by app_attribute_update_cb() below, which
 * fires for every change to any of these three attributes regardless of
 * source. set_output() reads all three to compute the actual cool/warm PWM
 * duties, same as a real tunable-white bulb combining on/off + brightness +
 * color temperature into one physical result. */
static bool light_on = false;
static uint8_t light_level = COLOR_TEMPERATURE_LIGHT_DEFAULT_LEVEL;
static uint16_t light_mireds = COLOR_TEMPERATURE_LIGHT_DEFAULT_MIREDS;

/* Target color temperature (mireds, clamped to the cool/warm physical
 * range) + brightness -> cool-white/warm-white channel duties. Reused
 * verbatim from firmware/color-light/'s own mireds_to_cw_ww() — see this
 * file's header comment for the full explanation of where the formula
 * comes from (ESPHome's light_call.cpp) and why it normalizes both
 * fractions rather than splitting linearly. Duties returned as [0,1]
 * fractions of `value_fraction` (brightness). */
static void mireds_to_cw_ww(uint16_t mireds, float value_fraction, float *cool_out, float *warm_out)
{
    float clamped = fminf(fmaxf((float)mireds, (float)COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS),
                           (float)COLOR_TEMPERATURE_LIGHT_WARM_WHITE_MIREDS);
    float range = (float)(COLOR_TEMPERATURE_LIGHT_WARM_WHITE_MIREDS - COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS);
    float warm_fraction = (clamped - (float)COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS) / range;
    float cool_fraction = 1.0f - warm_fraction;
    float max_fraction = fmaxf(warm_fraction, cool_fraction);
    *cool_out = value_fraction * (cool_fraction / max_fraction);
    *warm_out = value_fraction * (warm_fraction / max_fraction);
}

/* Drives the LEDC channels from the current on/off + level + color
 * temperature state together. */
static void set_output(void)
{
    uint32_t cool_white_duty = 0, warm_white_duty = 0;

    if (light_on) {
        float value_fraction = (float)light_level / 254.0f;
        float cool_fraction, warm_fraction;
        mireds_to_cw_ww(light_mireds, value_fraction, &cool_fraction, &warm_fraction);
        cool_white_duty = (uint32_t)(cool_fraction * 255.0f + 0.5f);
        warm_white_duty = (uint32_t)(warm_fraction * 255.0f + 0.5f);
    }

    ledc_set_duty(COLOR_TEMPERATURE_LIGHT_LEDC_MODE, COLOR_TEMPERATURE_LIGHT_LEDC_COOL_WHITE_CHANNEL, cool_white_duty);
    ledc_update_duty(COLOR_TEMPERATURE_LIGHT_LEDC_MODE, COLOR_TEMPERATURE_LIGHT_LEDC_COOL_WHITE_CHANNEL);
    ledc_set_duty(COLOR_TEMPERATURE_LIGHT_LEDC_MODE, COLOR_TEMPERATURE_LIGHT_LEDC_WARM_WHITE_CHANNEL, warm_white_duty);
    ledc_update_duty(COLOR_TEMPERATURE_LIGHT_LEDC_MODE, COLOR_TEMPERATURE_LIGHT_LEDC_WARM_WHITE_CHANNEL);
}

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Lifecycle events from the Matter stack (commissioning, connectivity, ...). */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete — device is now paired");
        break;
    default:
        break;
    }
}

/* Called whenever a controller changes an attribute — e.g. toggles the
 * light, moves its brightness slider, or moves its color-temperature
 * slider. We react on PRE_UPDATE so the PWM output follows the requested
 * state. All three attributes here are plain ember attributes (see the
 * header comment on why no Delegate is needed), so this is the exact same
 * attribute::PRE_UPDATE pattern firmware/dimmable-light/ already uses. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != light_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        light_on = val->val.b;
        set_output();
        ESP_LOGI(TAG, "Light turned %s", light_on ? "ON" : "OFF");
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        if (!val->val.u8) {
            return ESP_OK; /* null CurrentLevel has no defined brightness — ignore, keep last value */
        }
        light_level = val->val.u8;
        set_output();
        ESP_LOGI(TAG, "Light level set to %u/254", light_level);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
        light_mireds = val->val.u16;
        set_output();
        ESP_LOGI(TAG, "Color temperature set to %u mireds", light_mireds);
    }
    return ESP_OK;
}

/* Called when a controller asks the device to "identify" itself — starts
 * or stops the identify LED blinking accordingly. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    switch (type) {
    case identification::START:
        ESP_LOGI(TAG, "Identify started on endpoint %u", endpoint_id);
        esp_timer_start_periodic(identify_led_timer, IDENTIFY_BLINK_INTERVAL_MS * 1000);
        break;
    case identification::STOP:
        ESP_LOGI(TAG, "Identify stopped on endpoint %u", endpoint_id);
        esp_timer_stop(identify_led_timer);
        gpio_set_level(IDENTIFY_LED_GPIO, 0);
        break;
    case identification::EFFECT:
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u",
                 effect_id, effect_variant, endpoint_id);
        break;
    }
    return ESP_OK;
}

static esp_timer_handle_t factory_reset_confirm_timer = NULL;

/* Fires once the device has stayed powered up for
 * FACTORY_RESET_CONFIRM_DELAY_MS without another reboot — treats this as
 * a normal boot and clears the quick-power-cycle counter. */
static void factory_reset_confirm_timer_cb(void *arg)
{
    nvs_handle_t nvs;
    if (nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Increments the quick-power-cycle boot counter and returns true once it
 * has reached FACTORY_RESET_BOOT_COUNT_THRESHOLD. The caller (app_main())
 * only acts on that later, after Matter has started — see
 * firmware/light/main/app_main.cpp's header comment on why. Must run
 * after nvs_flash_init(). */
static bool check_factory_reset_boot_count(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS for boot-count tracking: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t boot_count = 0;
    nvs_get_u8(nvs, FACTORY_RESET_NVS_KEY, &boot_count); /* stays 0 if not set yet */
    boot_count++;
    nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, boot_count);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Quick-power-cycle boot count: %u/%u", boot_count, FACTORY_RESET_BOOT_COUNT_THRESHOLD);

    if (boot_count >= FACTORY_RESET_BOOT_COUNT_THRESHOLD) {
        /* Clear the counter now so a factory-reset reboot can't
         * immediately re-trigger itself. */
        if (nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, 0);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        return true;
    }

    const esp_timer_create_args_t factory_reset_confirm_timer_args = {
        .callback = &factory_reset_confirm_timer_cb,
        .name = "factory_reset_confirm",
    };
    esp_timer_create(&factory_reset_confirm_timer_args, &factory_reset_confirm_timer);
    esp_timer_start_once(factory_reset_confirm_timer, (uint64_t)FACTORY_RESET_CONFIRM_DELAY_MS * 1000);
    return false;
}

extern "C" void app_main(void)
{
    /* 1. Init NVS — stores the Matter fabric keys and factory data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 1b. Quick-power-cycle factory reset check — see
     * check_factory_reset_boot_count()'s comment above. The actual reset
     * (if due) only happens later, once Matter has started. */
    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the cool/warm white channels via LEDC (hardware PWM) —
     * one timer (shared clock/frequency/resolution) plus two channels
     * (the actual GPIOs + duty registers), same driver/ledc.h API
     * firmware/dimmable-light/'s and firmware/color-light/'s own outputs
     * already use. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = COLOR_TEMPERATURE_LIGHT_LEDC_MODE;
    ledc_timer.duty_resolution = COLOR_TEMPERATURE_LIGHT_LEDC_DUTY_RES;
    ledc_timer.timer_num = COLOR_TEMPERATURE_LIGHT_LEDC_TIMER;
    ledc_timer.freq_hz = COLOR_TEMPERATURE_LIGHT_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    struct {
        ledc_channel_t channel;
        gpio_num_t gpio;
    } channels[] = {
        { COLOR_TEMPERATURE_LIGHT_LEDC_COOL_WHITE_CHANNEL, COLOR_TEMPERATURE_LIGHT_COOL_WHITE_GPIO },
        { COLOR_TEMPERATURE_LIGHT_LEDC_WARM_WHITE_CHANNEL, COLOR_TEMPERATURE_LIGHT_WARM_WHITE_GPIO },
    };
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        ledc_channel_config_t ledc_channel = {};
        ledc_channel.gpio_num = channels[i].gpio;
        ledc_channel.speed_mode = COLOR_TEMPERATURE_LIGHT_LEDC_MODE;
        ledc_channel.channel = channels[i].channel;
        ledc_channel.intr_type = LEDC_INTR_DISABLE;
        ledc_channel.timer_sel = COLOR_TEMPERATURE_LIGHT_LEDC_TIMER;
        ledc_channel.duty = 0;
        ledc_channel.hpoint = 0;
        ledc_channel_config(&ledc_channel);
    }
    set_output(); /* light_on starts false, so this drives both duties to 0 (fully off). */

    /* 2b. Configure the identify LED + its blink timer (not started yet —
     * only runs while a controller has an identify request active). */
    gpio_config_t identify_io_conf = {};
    identify_io_conf.pin_bit_mask = (1ULL << IDENTIFY_LED_GPIO);
    identify_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&identify_io_conf);
    gpio_set_level(IDENTIFY_LED_GPIO, 0);

    const esp_timer_create_args_t identify_timer_args = {
        .callback = &identify_led_timer_cb,
        .name = "identify_led",
    };
    esp_timer_create(&identify_timer_args, &identify_led_timer);

    /* 3. Build the Matter data model: one node, one Color Temperature Light
     * endpoint (Identify + Groups + OnOff + LevelControl +
     * ColorControl[ColorTemperature] + ScenesManagement) — see the header
     * comment on why endpoint::color_temperature_light::create() is a
     * complete top-level helper needing no hand-assembly. */
    node::config_t node_config;
    /* NodeLabel (Basic Information cluster) — same fix
     * firmware/color-light/'s and firmware/addressable-light/'s own header
     * comments document: left empty, esp-matter's own default, a
     * controller would otherwise propose a generic "Matter Accessory" name
     * during pairing instead of anything specific. */
    strncpy(node_config.root_node.basic_information.node_label, "Color Temperature Light",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    color_temperature_light::config_t light_config;
    light_config.level_control.current_level = COLOR_TEMPERATURE_LIGHT_DEFAULT_LEVEL;
    /* OnLevel deliberately left null — see the header comment on the
     * real, hardware-confirmed OnLevel bug fix. */
    light_config.level_control.on_level = nullable<uint8_t>();
    light_config.level_control_lighting.start_up_current_level = COLOR_TEMPERATURE_LIGHT_DEFAULT_LEVEL;
    light_config.color_control_color_temperature.color_temperature_mireds = COLOR_TEMPERATURE_LIGHT_DEFAULT_MIREDS;
    light_config.color_control_color_temperature.color_temp_physical_min_mireds = COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS;
    light_config.color_control_color_temperature.color_temp_physical_max_mireds = COLOR_TEMPERATURE_LIGHT_WARM_WHITE_MIREDS;
    light_config.color_control_color_temperature.couple_color_temp_to_level_min_mireds = COLOR_TEMPERATURE_LIGHT_COOL_WHITE_MIREDS;
    light_config.color_control_color_temperature.start_up_color_temperature_mireds =
        nullable<uint16_t>((uint16_t)COLOR_TEMPERATURE_LIGHT_DEFAULT_MIREDS);
    endpoint_t *endpoint = color_temperature_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create color temperature light endpoint");
        return;
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Color temperature light endpoint id: %u", light_endpoint_id);

    /* Occupancy Sensing (client) — optionalConform on
     * ColorTemperatureLight.xml (Matter Device Types Reference audit, see
     * CLAUDE.md's own "Open next steps"). Same NULL-config/CLUSTER_FLAG_
     * CLIENT shape firmware/light/'s own identical addition already
     * establishes — see that file's own comment for the full detail on
     * why no app code reacts to it directly. */
    cluster::occupancy_sensing::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    /* CurrentLevel/ColorTemperatureMireds both change rapidly while a
     * controller is actively dragging a brightness/color-temperature
     * slider — deferring their NVS persistence avoids writing flash on
     * every single step. Same call firmware/dimmable-light/'s and
     * firmware/color-light/'s own outputs already make, for the same
     * reason. */
    attribute::set_deferred_persistence(attribute::get(light_endpoint_id, LevelControl::Id,
                                                        LevelControl::Attributes::CurrentLevel::Id));
    attribute::set_deferred_persistence(attribute::get(light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::ColorTemperatureMireds::Id));

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started — see
     * check_factory_reset_boot_count()'s comment on why this can't
     * happen any earlier. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter color temperature light started. Scan the QR code to commission.");
}
