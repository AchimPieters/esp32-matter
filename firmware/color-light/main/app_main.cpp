/*
 * Minimal Matter Color Light (RGB, RGBW, or RGBWW/RGBCCT) — ninth device
 * type, and the
 * last of the three options offered together back when
 * firmware/dimmable-light/ was added (the other two, dimmable light and
 * window covering, are both already built — this is the remaining one).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 * Works on other ESP32 chips too — see the README for how to switch target.
 *
 * --- Why this endpoint is hand-assembled, not endpoint::extended_color_light::create() ---
 * Checked directly in esp-matter's source before writing any of this:
 * `endpoint::extended_color_light::add()` (esp_matter_endpoint.cpp)
 * unconditionally wires up both the ColorTemperature *and* Xy ColorControl
 * features — it does NOT add the HueSaturation feature at all, despite
 * that being what most controllers' color wheels (Apple Home, Google Home,
 * Home Assistant) actually drive first. Supporting all three color modes
 * properly means three separate, real colorimetry conversions (HSV→RGB,
 * CIE xyY→RGB, and correlated-color-temperature→RGB), each with real
 * accuracy tradeoffs — more scope and more room for a subtly-wrong
 * conversion than this file takes on. Same "smallest reasonable next
 * step" scoping already used for firmware/dimmable-light/ (LevelControl
 * only, no ColorControl at all) and firmware/window-covering/ (Lift only,
 * no Tilt): this device supports exactly one color mode — Hue/Saturation
 * — end to end, correctly, rather than three modes with two of them
 * approximate. The endpoint is built by calling esp-matter's own
 * lower-level free functions directly (`endpoint::create()`,
 * `add_device_type()`, and each cluster's own `create()`/
 * `feature::xxx::add()`) instead of a single higher-level endpoint
 * helper — the same public API `endpoint::extended_color_light::add()`
 * itself is built from, just composed differently. The device type is
 * still declared as ExtendedColorLight (0x010D, confirmed in
 * esp_matter_endpoint_impl.h) — that's the correct Matter device type for
 * any color-capable light regardless of which specific ColorControl
 * features it implements; the cluster's own FeatureMap/ColorCapabilities
 * attributes (both set to HueSaturation-only here) are what tell a
 * real controller exactly which color modes this specific device
 * actually supports, so it won't offer XY or color-temperature controls
 * for a device that can't act on them.
 *
 * --- LevelControl and OnOff: same plain-attribute pattern as before -----
 * Confirmed already for firmware/dimmable-light/: neither cluster is one
 * of the "code-driven" cluster classes needing a special setter — both
 * use the ordinary attribute::PRE_UPDATE + attribute::update() pattern.
 * ColorControl's CurrentHue/CurrentSaturation attributes are the same:
 * confirmed by checking esp-matter's own reference example
 * (examples/light/main/app_driver.cpp), which reacts to
 * ColorControl::Attributes::CurrentHue::Id / CurrentSaturation::Id via
 * the identical attribute::PRE_UPDATE callback used for OnOff/
 * LevelControl elsewhere — no Delegate needed for this cluster, unlike
 * firmware/window-covering/'s WindowCovering.
 *
 * --- Color math ------------------------------------------------------------
 * CurrentHue and CurrentSaturation are both uint8 attributes with a valid
 * range of 0-254 (confirmed directly in the Matter 1.6 ColorControl.xml
 * spec file's own <constraint><max value="254"/> on both attributes) —
 * mapped here as hue_degrees = CurrentHue * 360 / 254 and
 * saturation_fraction = CurrentSaturation / 254, the standard interpretation
 * for this exact attribute pair across the Zigbee-ZCL-derived ecosystem
 * Matter's ColorControl cluster itself descends from. Brightness comes
 * from LevelControl's CurrentLevel (1-254, same range/handling
 * firmware/dimmable-light/ already established) as the "V" (value)
 * component of standard HSV. The HSV→RGB conversion itself
 * (hsv_to_rgb() below) is the textbook algorithm (six 60°-wide hue
 * sectors) — deterministic math with a single well-known correct answer,
 * not something that needs SDK-specific verification the way an
 * attribute name or cluster API does.
 *
 * --- Output -----------------------------------------------------------
 * Three or four LEDC PWM channels (R/G/B, optionally +W) sharing one
 * timer — same LEDC pattern firmware/dimmable-light/ already established
 * for a single channel. Active-HIGH by default (plain RGB(W) LED /
 * common driver board wiring, not a relay — no polarity option needed
 * the way firmware/outlet/'s real relay output has one). Output is the
 * product of on/off state and the current HSV→RGB(W) result together,
 * same "off doesn't forget where it was" behavior as
 * firmware/dimmable-light/'s own brightness: turning back on restores
 * the last color, not a reset one.
 *
 * --- COLOR_LIGHT_COLOR_MODE: RGB / RGBW / RGBWW ---------------------------
 * COLOR_LIGHT_COLOR_MODE selects one of three build-time hardware
 * variants — plain 3-channel RGB (default), RGBW (adds one white channel),
 * or RGBWW (adds separate cool-white + warm-white channels, i.e. what LED
 * strip vendors usually sell as "RGBCCT" or "RGB+CCT" — the two names
 * refer to the same 5-channel hardware).
 *
 * RGBW adds a 4th LEDC channel and converts the HSV→RGB result into RGBW
 * via the standard "extract common white" technique: W = min(R, G, B);
 * R' = R - W; G' = G - W; B' = B - W (implemented in rgb_to_rgbw() below).
 * This is the same algorithm Home Assistant's own color utility
 * (`homeassistant.util.color.color_rgb_to_rgbw`) and WLED use — not
 * something invented for this file, a widely-used, well-understood
 * technique precisely because there's no single Matter- or industry-
 * mandated RGBW formula: real RGBW products differ in how much they lean
 * on the (usually more efficient, sometimes more accurate) dedicated
 * white LED vs. the RGB channels for near-white colors, and this simple
 * subtraction is the common baseline every more elaborate scheme builds
 * on. Matter's own ColorControl cluster has no separate "White" attribute
 * or concept at all here — from the protocol's point of view this is
 * still plain Hue/Saturation control; the white channel is purely a local
 * hardware-rendering decision this device makes on its own, invisible to
 * any controller.
 *
 * RGBWW is a genuinely different case, not just "one more channel": real
 * RGBCCT products don't blend RGB and white simultaneously — you're
 * either driving color (RGB) or driving tunable white (cool+warm), never
 * both at once (confirmed in ESPHome's own rgbww light component docs,
 * which call this "color_interlock": "it is not possible to enable the
 * RGB leds at the same time as the white leds" on this class of
 * hardware). This maps cleanly onto Matter's ColorControl cluster, which
 * already has a `ColorMode` attribute distinguishing
 * CurrentHueAndCurrentSaturation from ColorTemperatureMireds as separate,
 * mutually exclusive color spaces — so RGBWW mode here adds the
 * ColorTemperature feature (`cluster::color_control::feature::
 * color_temperature::add()`, confirmed against esp-matter's own
 * color_control.h for the exact config_t field names) alongside the
 * existing HueSaturation feature, and locally latches which color space
 * was most recently commanded (light_color_source below) — same
 * interlock behavior as ESPHome's, just implemented against Matter's
 * cluster instead of ESPHome's.
 *
 * Converting a target ColorTemperatureMireds into cool/warm channel duty
 * cycles uses the same linear-interpolation-with-intensity-normalization
 * formula as ESPHome's own light_call.cpp
 * (LightCall::transform_parameters_): clamp the target mireds into
 * [cool, warm], compute the warm fraction as
 * (mireds - cool_mireds) / (warm_mireds - cool_mireds), the cool fraction
 * as its complement, then divide both by max(warm_fraction, cool_fraction)
 * before scaling by brightness — this keeps at least one channel at full
 * strength at any color temperature within range instead of both
 * channels dimming together at the midpoint, exactly the effect ESPHome
 * gets from the same normalization. COLOR_LIGHT_COOL_WHITE_KELVIN/
 * COLOR_LIGHT_WARM_WHITE_KELVIN (6500K/2700K) are the two most common
 * "daylight"/"warm white" LED bin ratings used across the LED lighting
 * industry — not this specific strip's measured values (no RGBWW
 * hardware was available to measure), so adjust them to your actual
 * LEDs' rated color temperature if you know it; ESPHome's own documented
 * RGBWW example uses 6536K/2000K instead, underlining that this varies
 * per product and there's no universal default.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <math.h>

#include <esp_matter.h>

static const char *TAG = "matter_color_light";

/* Change these to the GPIOs your RGB LED (or 3-channel driver board) is
 * wired to. Each needs to be LEDC-capable — true for nearly every GPIO on
 * these chips except input-only ones (GPIO 34-39 on classic ESP32). GPIO
 * 2/4/5 are plain, unreserved GPIOs on classic ESP32 (WROOM-32). Adjust to
 * match your board. */
#define COLOR_LIGHT_RED_GPIO GPIO_NUM_2
#define COLOR_LIGHT_GREEN_GPIO GPIO_NUM_4
#define COLOR_LIGHT_BLUE_GPIO GPIO_NUM_5

/* COLOR_LIGHT_COLOR_MODE picks the hardware variant — see the header
 * comment above for the full explanation of each. Only one of these is
 * ever active at a time (mutually exclusive #if branches below), so the
 * RGBW and RGBWW variants can safely share the same "channel 3" LEDC
 * slot and default GPIO 18. */
#define COLOR_LIGHT_MODE_RGB 0
#define COLOR_LIGHT_MODE_RGBW 1
#define COLOR_LIGHT_MODE_RGBWW 2
#define COLOR_LIGHT_COLOR_MODE COLOR_LIGHT_MODE_RGB

/* Optional 4th channel for an RGBW LED/strip (COLOR_LIGHT_MODE_RGBW). GPIO
 * 18 is a plain, unreserved GPIO on classic ESP32 (WROOM-32) that doesn't
 * collide with the three color channels above or the identify LED below.
 * Adjust to match your board. */
#define COLOR_LIGHT_WHITE_GPIO GPIO_NUM_18

/* Optional cool-white + warm-white channels for an RGBWW/RGBCCT LED/strip
 * (COLOR_LIGHT_MODE_RGBWW) — see the header comment for the color-space
 * "interlock" this implies and the mireds conversion it uses. GPIO 18/19
 * are plain, unreserved GPIOs on classic ESP32 (WROOM-32). */
#define COLOR_LIGHT_COOL_WHITE_GPIO GPIO_NUM_18
#define COLOR_LIGHT_WARM_WHITE_GPIO GPIO_NUM_19
#define COLOR_LIGHT_COOL_WHITE_KELVIN 6500
#define COLOR_LIGHT_WARM_WHITE_KELVIN 2700
#define COLOR_LIGHT_COOL_WHITE_MIREDS (1000000 / COLOR_LIGHT_COOL_WHITE_KELVIN) /* ~154 */
#define COLOR_LIGHT_WARM_WHITE_MIREDS (1000000 / COLOR_LIGHT_WARM_WHITE_KELVIN) /* ~370 */

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the light's own on/off/color state. GPIO 15 is a
 * plain, unreserved GPIO on classic ESP32 (WROOM-32) that doesn't collide
 * with the three color channels above. Adjust to match your board, or
 * wire it to one of the RGB channels if you only have the one LED and
 * don't mind it blinking (at whatever color/brightness it's already at,
 * on top of the blink) during identify. */
#define IDENTIFY_LED_GPIO GPIO_NUM_15
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (LED Control / PWM) peripheral setup — one shared timer, three
 * channels. Same settings firmware/dimmable-light/ already uses for its
 * single channel (see that file's header comment for why): low-speed
 * mode for portability across every module this repo targets, auto clock
 * source, 8-bit (0-255) duty resolution, 5 kHz. */
#define COLOR_LIGHT_LEDC_TIMER LEDC_TIMER_0
#define COLOR_LIGHT_LEDC_RED_CHANNEL LEDC_CHANNEL_0
#define COLOR_LIGHT_LEDC_GREEN_CHANNEL LEDC_CHANNEL_1
#define COLOR_LIGHT_LEDC_BLUE_CHANNEL LEDC_CHANNEL_2
#define COLOR_LIGHT_LEDC_WHITE_CHANNEL LEDC_CHANNEL_3
#define COLOR_LIGHT_LEDC_COOL_WHITE_CHANNEL LEDC_CHANNEL_3
#define COLOR_LIGHT_LEDC_WARM_WHITE_CHANNEL LEDC_CHANNEL_4
#define COLOR_LIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define COLOR_LIGHT_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define COLOR_LIGHT_LEDC_FREQUENCY_HZ 5000

/* Initial brightness/hue/saturation — used both for each attribute's
 * starting value and for what the light shows the first time it's turned
 * on after a fresh flash or factory reset (same reasoning
 * firmware/dimmable-light/ documents for its own default level: a null
 * attribute has no defined value, which is a worse first-read experience
 * than a concrete, reasonable one). Default is a mid-brightness warm-ish
 * orange (hue ~30°) — arbitrary but visibly "on and colored" without
 * being blinding, just so a first boot is obviously alive. */
#define COLOR_LIGHT_DEFAULT_LEVEL 128
#define COLOR_LIGHT_DEFAULT_HUE 21   /* ~30 degrees: 21 * 360 / 254 */
#define COLOR_LIGHT_DEFAULT_SATURATION 254 /* fully saturated */

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t color_light_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Mirrors the Matter OnOff/LevelControl/ColorControl attributes' current
 * values — kept in sync solely by app_attribute_update_cb() below, which
 * fires for every change to any of these four attributes regardless of
 * source. set_output() reads all four to compute the actual RGB PWM
 * duties, same as a real color bulb combining on/off + brightness + hue +
 * saturation into one physical result. */
static bool light_on = false;
static uint8_t light_level = COLOR_LIGHT_DEFAULT_LEVEL;
static uint8_t light_hue = COLOR_LIGHT_DEFAULT_HUE;
static uint8_t light_saturation = COLOR_LIGHT_DEFAULT_SATURATION;

#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
/* Which color space was most recently commanded — see the header comment
 * on the RGB/CCT "interlock" this hardware variant needs. Starts in HS
 * mode to match COLOR_LIGHT_DEFAULT_HUE/_SATURATION being the light's
 * initial visible color. */
enum color_source_t { COLOR_SOURCE_HS, COLOR_SOURCE_CCT };
static color_source_t light_color_source = COLOR_SOURCE_HS;
static uint16_t light_mireds = COLOR_LIGHT_COOL_WHITE_MIREDS;
#endif

/* Textbook HSV -> RGB conversion (six 60-degree hue sectors). h in
 * degrees [0,360), s and v in [0,1]. Output components in [0,1]; caller
 * scales to whatever duty range it needs (0-255 here, see set_output()). */
static void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b)
{
    float c = v * s;
    float h_prime = fmodf(h / 60.0f, 6.0f);
    float x = c * (1.0f - fabsf(fmodf(h_prime, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;

    if (h_prime < 1.0f) { r1 = c; g1 = x; b1 = 0; }
    else if (h_prime < 2.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (h_prime < 3.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (h_prime < 4.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (h_prime < 5.0f) { r1 = x; g1 = 0; b1 = c; }
    else { r1 = c; g1 = 0; b1 = x; }

    *r = r1 + m;
    *g = g1 + m;
    *b = b1 + m;
}

#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBW
/* RGB -> RGBW via the standard "extract common white" technique — see the
 * header comment on COLOR_LIGHT_COLOR_MODE for the exact algorithm and
 * its sourcing (matches Home Assistant's own color_rgb_to_rgbw() and
 * WLED). Inputs and outputs are all in [0,1]. */
static void rgb_to_rgbw(float r, float g, float b, float *r_out, float *g_out, float *b_out, float *w_out)
{
    float w = fminf(r, fminf(g, b));
    *r_out = r - w;
    *g_out = g - w;
    *b_out = b - w;
    *w_out = w;
}
#endif

#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
/* Target color temperature (mireds, clamped to the cool/warm physical
 * range) + brightness -> cool-white/warm-white channel duties. Same
 * linear-interpolation-with-intensity-normalization formula as ESPHome's
 * own light_call.cpp (LightCall::transform_parameters_) — see the header
 * comment on COLOR_LIGHT_COLOR_MODE for the full explanation. Duties
 * returned as [0,1] fractions of `value_fraction` (brightness). */
static void mireds_to_cw_ww(uint16_t mireds, float value_fraction, float *cool_out, float *warm_out)
{
    float clamped = fminf(fmaxf((float)mireds, (float)COLOR_LIGHT_COOL_WHITE_MIREDS),
                           (float)COLOR_LIGHT_WARM_WHITE_MIREDS);
    float range = (float)(COLOR_LIGHT_WARM_WHITE_MIREDS - COLOR_LIGHT_COOL_WHITE_MIREDS);
    float warm_fraction = (clamped - (float)COLOR_LIGHT_COOL_WHITE_MIREDS) / range;
    float cool_fraction = 1.0f - warm_fraction;
    float max_fraction = fmaxf(warm_fraction, cool_fraction);
    *cool_out = value_fraction * (cool_fraction / max_fraction);
    *warm_out = value_fraction * (warm_fraction / max_fraction);
}
#endif

/* Drives the LEDC channels from the current on/off + level + hue +
 * saturation (or, in RGBWW mode, color temperature) state together — see
 * the header comment on color math for the exact attribute-range
 * interpretation, and on COLOR_LIGHT_COLOR_MODE for the RGB->RGBW / mireds
 * conversions applied for the RGBW/RGBWW variants. */
static void set_output(void)
{
    uint32_t red_duty = 0, green_duty = 0, blue_duty = 0;
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBW
    uint32_t white_duty = 0;
#elif COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
    uint32_t cool_white_duty = 0, warm_white_duty = 0;
#endif

    if (light_on) {
        float value_fraction = (float)light_level / 254.0f;

#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
        if (light_color_source == COLOR_SOURCE_CCT) {
            /* CCT mode: drive cool/warm only, RGB stays off — see the
             * header comment on the RGB/CCT interlock. */
            float cool_fraction, warm_fraction;
            mireds_to_cw_ww(light_mireds, value_fraction, &cool_fraction, &warm_fraction);
            cool_white_duty = (uint32_t)(cool_fraction * 255.0f + 0.5f);
            warm_white_duty = (uint32_t)(warm_fraction * 255.0f + 0.5f);
        } else
#endif
        {
            float hue_degrees = (float)light_hue * 360.0f / 254.0f;
            float saturation_fraction = (float)light_saturation / 254.0f;
            float r, g, b;
            hsv_to_rgb(hue_degrees, saturation_fraction, value_fraction, &r, &g, &b);

#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBW
            float w;
            rgb_to_rgbw(r, g, b, &r, &g, &b, &w);
            white_duty = (uint32_t)(w * 255.0f + 0.5f);
#endif

            red_duty = (uint32_t)(r * 255.0f + 0.5f);
            green_duty = (uint32_t)(g * 255.0f + 0.5f);
            blue_duty = (uint32_t)(b * 255.0f + 0.5f);
        }
    }

    ledc_set_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_RED_CHANNEL, red_duty);
    ledc_update_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_RED_CHANNEL);
    ledc_set_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_GREEN_CHANNEL, green_duty);
    ledc_update_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_GREEN_CHANNEL);
    ledc_set_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_BLUE_CHANNEL, blue_duty);
    ledc_update_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_BLUE_CHANNEL);
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBW
    ledc_set_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_WHITE_CHANNEL, white_duty);
    ledc_update_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_WHITE_CHANNEL);
#elif COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
    ledc_set_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_COOL_WHITE_CHANNEL, cool_white_duty);
    ledc_update_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_COOL_WHITE_CHANNEL);
    ledc_set_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_WARM_WHITE_CHANNEL, warm_white_duty);
    ledc_update_duty(COLOR_LIGHT_LEDC_MODE, COLOR_LIGHT_LEDC_WARM_WHITE_CHANNEL);
#endif
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
 * light, moves its brightness slider, or picks a color. We react on
 * PRE_UPDATE so the PWM output follows the requested state. All four
 * attributes here are plain ember attributes (see the header comment on
 * why no Delegate is needed), so this is the exact same
 * attribute::PRE_UPDATE pattern firmware/dimmable-light/ already uses —
 * just handling a third cluster too. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != color_light_endpoint_id) {
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
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentHue::Id) {
        light_hue = val->val.u8;
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
        light_color_source = COLOR_SOURCE_HS;
#endif
        set_output();
        ESP_LOGI(TAG, "Hue set to %u/254", light_hue);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
        light_saturation = val->val.u8;
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
        light_color_source = COLOR_SOURCE_HS;
#endif
        set_output();
        ESP_LOGI(TAG, "Saturation set to %u/254", light_saturation);
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
        light_mireds = val->val.u16;
        light_color_source = COLOR_SOURCE_CCT;
        set_output();
        ESP_LOGI(TAG, "Color temperature set to %u mireds", light_mireds);
#endif
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
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u — blinking as usual",
                 effect_id, effect_variant, endpoint_id);
        break;
    }
    return ESP_OK;
}

extern "C" void app_main(void)
{
    /* 1. Init NVS — stores the Matter fabric keys and factory data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Configure the RGB(W) channels via LEDC (hardware PWM) — one timer
     * (shared clock/frequency/resolution) plus three or four channels
     * (the actual GPIOs + duty registers), same driver/ledc.h API
     * firmware/dimmable-light/ already uses for its single channel. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = COLOR_LIGHT_LEDC_MODE;
    ledc_timer.duty_resolution = COLOR_LIGHT_LEDC_DUTY_RES;
    ledc_timer.timer_num = COLOR_LIGHT_LEDC_TIMER;
    ledc_timer.freq_hz = COLOR_LIGHT_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    struct {
        ledc_channel_t channel;
        gpio_num_t gpio;
    } channels[] = {
        { COLOR_LIGHT_LEDC_RED_CHANNEL, COLOR_LIGHT_RED_GPIO },
        { COLOR_LIGHT_LEDC_GREEN_CHANNEL, COLOR_LIGHT_GREEN_GPIO },
        { COLOR_LIGHT_LEDC_BLUE_CHANNEL, COLOR_LIGHT_BLUE_GPIO },
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBW
        { COLOR_LIGHT_LEDC_WHITE_CHANNEL, COLOR_LIGHT_WHITE_GPIO },
#elif COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
        { COLOR_LIGHT_LEDC_COOL_WHITE_CHANNEL, COLOR_LIGHT_COOL_WHITE_GPIO },
        { COLOR_LIGHT_LEDC_WARM_WHITE_CHANNEL, COLOR_LIGHT_WARM_WHITE_GPIO },
#endif
    };
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        ledc_channel_config_t ledc_channel = {};
        ledc_channel.gpio_num = channels[i].gpio;
        ledc_channel.speed_mode = COLOR_LIGHT_LEDC_MODE;
        ledc_channel.channel = channels[i].channel;
        ledc_channel.intr_type = LEDC_INTR_DISABLE;
        ledc_channel.timer_sel = COLOR_LIGHT_LEDC_TIMER;
        ledc_channel.duty = 0;
        ledc_channel.hpoint = 0;
        ledc_channel_config(&ledc_channel);
    }
    set_output(); /* light_on starts false, so this drives all duties to 0 (fully off). */

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

    /* 3. Build the Matter data model: one node, one hand-assembled
     * Extended Color Light endpoint (Identify + Groups + OnOff +
     * LevelControl + ColorControl[HueSaturation, +ColorTemperature in
     * RGBWW mode] + ScenesManagement) — see the header comment on why
     * this isn't endpoint::extended_color_light::create(). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint_t *ep = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!ep) {
        ESP_LOGE(TAG, "Failed to create endpoint");
        return;
    }
    add_device_type(ep, ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID, ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_VERSION);

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kVisibleIndicator);
    cluster_t *identify_cluster = cluster::identify::create(ep, &identify_config, CLUSTER_FLAG_SERVER);
    cluster::identify::command::create_trigger_effect(identify_cluster);

    cluster::groups::config_t groups_config;
    cluster::groups::create(ep, &groups_config, CLUSTER_FLAG_SERVER);

    cluster::on_off::config_t on_off_config;
    cluster::on_off::feature::lighting::config_t on_off_lighting_config;
    cluster_t *on_off_cluster = cluster::on_off::create(ep, &on_off_config, CLUSTER_FLAG_SERVER);
    cluster::on_off::feature::lighting::add(on_off_cluster, &on_off_lighting_config);
    cluster::on_off::command::create_on(on_off_cluster);
    cluster::on_off::command::create_toggle(on_off_cluster);

    cluster::level_control::config_t level_control_config;
    level_control_config.current_level = nullable<uint8_t>(COLOR_LIGHT_DEFAULT_LEVEL);
    level_control_config.on_level = nullable<uint8_t>(COLOR_LIGHT_DEFAULT_LEVEL);
    cluster::level_control::feature::lighting::config_t level_control_lighting_config;
    level_control_lighting_config.start_up_current_level = nullable<uint8_t>(COLOR_LIGHT_DEFAULT_LEVEL);
    cluster_t *level_control_cluster = cluster::level_control::create(ep, &level_control_config, CLUSTER_FLAG_SERVER);
    cluster::level_control::feature::on_off::add(level_control_cluster);
    cluster::level_control::feature::lighting::add(level_control_cluster, &level_control_lighting_config);

    cluster::color_control::config_t color_control_config;
    color_control_config.color_mode = chip::to_underlying(ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation);
    color_control_config.enhanced_color_mode = chip::to_underlying(ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation);
    {
        uint16_t color_capabilities = chip::to_underlying(ColorControl::ColorCapabilitiesBitmap::kHueSaturation);
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
        color_capabilities |= chip::to_underlying(ColorControl::ColorCapabilitiesBitmap::kColorTemperature);
#endif
        color_control_config.color_capabilities = color_capabilities;
    }
    cluster_t *color_control_cluster = cluster::color_control::create(ep, &color_control_config, CLUSTER_FLAG_SERVER);
    cluster::color_control::feature::hue_saturation::config_t hue_saturation_config;
    hue_saturation_config.current_hue = COLOR_LIGHT_DEFAULT_HUE;
    hue_saturation_config.current_saturation = COLOR_LIGHT_DEFAULT_SATURATION;
    cluster::color_control::feature::hue_saturation::add(color_control_cluster, &hue_saturation_config);
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
    cluster::color_control::feature::color_temperature::config_t color_temperature_config;
    color_temperature_config.color_temperature_mireds = COLOR_LIGHT_COOL_WHITE_MIREDS;
    color_temperature_config.color_temp_physical_min_mireds = COLOR_LIGHT_COOL_WHITE_MIREDS;
    color_temperature_config.color_temp_physical_max_mireds = COLOR_LIGHT_WARM_WHITE_MIREDS;
    color_temperature_config.couple_color_temp_to_level_min_mireds = COLOR_LIGHT_COOL_WHITE_MIREDS;
    color_temperature_config.start_up_color_temperature_mireds = nullable<uint16_t>((uint16_t)COLOR_LIGHT_COOL_WHITE_MIREDS);
    cluster::color_control::feature::color_temperature::add(color_control_cluster, &color_temperature_config);
#endif

    cluster::scenes_management::config_t scenes_management_config;
    cluster_t *scenes_management_cluster = cluster::scenes_management::create(ep, &scenes_management_config, CLUSTER_FLAG_SERVER);
    cluster::scenes_management::command::create_copy_scene(scenes_management_cluster);
    cluster::scenes_management::command::create_copy_scene_response(scenes_management_cluster);

    color_light_endpoint_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "Color light endpoint id: %u", color_light_endpoint_id);

    /* CurrentLevel/CurrentHue/CurrentSaturation all change rapidly while a
     * controller is actively dragging a brightness/color slider —
     * deferring their NVS persistence avoids writing flash on every
     * single step. Same call firmware/dimmable-light/ makes for
     * CurrentLevel, for the same reason. */
    attribute::set_deferred_persistence(attribute::get(color_light_endpoint_id, LevelControl::Id,
                                                        LevelControl::Attributes::CurrentLevel::Id));
    attribute::set_deferred_persistence(attribute::get(color_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentHue::Id));
    attribute::set_deferred_persistence(attribute::get(color_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentSaturation::Id));
#if COLOR_LIGHT_COLOR_MODE == COLOR_LIGHT_MODE_RGBWW
    attribute::set_deferred_persistence(attribute::get(color_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::ColorTemperatureMireds::Id));
#endif

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Matter color light started. Scan the QR code to commission.");
}
