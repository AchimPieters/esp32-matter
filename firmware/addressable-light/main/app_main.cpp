/*
 * Minimal Matter Addressable Color Light (WS2812B or SK6812) — tenth device
 * type, and this repo's first over an addressable single-wire LED protocol
 * (RMT peripheral) rather than plain PWM (firmware/color-light/'s RGB/RGBW/
 * RGBWW modes, firmware/dimmable-light/).
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
 * --- Why "addressable" doesn't mean per-pixel control here -----------------
 * WS2812B/SK6812 strips are called "addressable" because each LED has its
 * own driver chip and can, in principle, show its own color — that's what
 * lets products marketed as "RGBIC" do chases/gradients/rainbow effects
 * across one strip. This firmware does NOT expose that: it fills every
 * pixel with the exact same color, because Matter itself gives a controller
 * no way to ask for anything else. Checked directly in connectedhomeip's own
 * `src/controller/data_model/controller-clusters.matter`: there IS a cluster
 * for this — `provisional cluster DynamicLighting = 773` (0x0305), with
 * `EffectStruct`/`EffectColorStruct` types that look exactly like what a
 * real per-pixel/gradient effect would need — but it's marked `provisional`
 * and doesn't appear in any ratified data_model spec folder (checked 1.0
 * through 1.6, all shipped versions) — not something a real, certified
 * controller (Apple Home, Google Home, Home Assistant) can command today,
 * and esp-matter has no cluster support for it either. So this device is,
 * from Matter's point of view, exactly as capable as firmware/color-light/:
 * one Hue/Saturation/Level color for the whole accessory. The only thing
 * that changes here is the physical layer driving that one color out to N
 * physically addressable LEDs at once, instead of to LEDC PWM channels.
 * Revisit once DynamicLighting (or an equivalent) ships in a ratified
 * Matter spec version and esp-matter grows support for it.
 *
 * --- Same hand-assembled ExtendedColorLight endpoint as firmware/color-light/ ---
 * Identify + Groups + OnOff + LevelControl + ColorControl[HueSaturation
 * only] + ScenesManagement, device type ExtendedColorLight (0x010D) — see
 * firmware/color-light/main/app_main.cpp's header comment for the full
 * rationale (not repeated here); this file's endpoint-assembly code is a
 * direct copy of that one, changed only where the addressable output needs
 * something different from LEDC PWM (see below).
 *
 * --- WS2812B / SK6812 protocol -- verified against each chip's own primary
 * manufacturer (Worldsemi) datasheet, not a secondary source, per this
 * repo's established practice -----------------------------------------------
 * Both are single-wire NRZ protocols: each bit is one HIGH-then-LOW pulse
 * pair whose relative HIGH/LOW duration (not amplitude or frequency) encodes
 * 0 vs 1, sent MSB-first, one pixel's full color data back-to-back, followed
 * by a LOW "reset" pulse that latches the whole frame. ADDRESSABLE_LIGHT_CHIP
 * selects between them:
 *
 * WS2812B (24-bit, 3 bytes/pixel) — timing from Worldsemi's own WS2812B
 * datasheet ("Data transfer time" table, TH+TL=1.25us±600ns):
 *   T0H=0.4us±150ns, T0L=0.85us±150ns, T1H=0.8us±150ns, T1L=0.45us±150ns,
 *   RES(reset)>=50us. Byte order confirmed in the same datasheet's
 *   "Composition of 24bit data" diagram: G7..G0, R7..R0, B7..B0 — i.e. GRB,
 *   not RGB. ADDRESSABLE_LIGHT_RESET_US here is set to 300us, well above
 *   that 50us datasheet minimum: a second, separately-circulated Worldsemi
 *   WS2812B datasheet revision documents up to ">300us" for newer "-V5"
 *   silicon, and since the reset only costs time once per full-strip update
 *   (never per bit), being generous here is free and avoids flicker/latch
 *   failures on newer chips soldered onto strips still sold as "WS2812B".
 *
 * SK6812 (32-bit RGBW, 4 bytes/pixel) — timing and byte order from
 * Worldsemi's own SK6812RGBW datasheet (Document No. SPC/SK6812RGBW Rev.01,
 * section 10 "data transmission time" + section 13 "data structure of
 * 32bit"): T0H=0.3us±0.15us, T0L=0.9us±0.15us, T1H=0.6us±0.15us,
 * T1L=0.6us±0.15us, Trst(reset)=80us. Byte order per that same section 13:
 * R7..R0, G7..G0, B7..B0, W7..W0 — i.e. RGBW, sent high-bit-first. Flagged
 * explicitly because it's a real point of disagreement in the wild: several
 * widely-used Arduino/ESPHome libraries default to treating SK6812 RGBW
 * strips as GRBW order instead — if your strip's colors come out visibly
 * swapped, that library-vs-datasheet discrepancy (not a bug in this file)
 * is the first thing to check, and ADDRESSABLE_LIGHT_CHIP_SK6812's pixel-
 * fill code below is exactly where to adjust the byte order if so.
 *
 * Converting the resulting RGB into RGBW for SK6812 reuses the exact same
 * "extract common white" technique as firmware/color-light/'s RGBW mode
 * (W = min(R,G,B), then subtract W from each of R/G/B) — see that file's
 * header comment for the full sourcing (matches Home Assistant's own
 * color utility and WLED); not re-derived here, just re-applied.
 *
 * --- RMT (Remote Control Transceiver) peripheral -----------------------
 * Both chips' bit timing is implemented via ESP-IDF's `driver/rmt_tx.h` —
 * a real ESP32 hardware peripheral built exactly for generating precise
 * pulse-timed waveforms like this, not a bit-banged GPIO loop (unlike
 * firmware/temperature-sensor/'s DHT11/DHT22/DS18B20 drivers, which predate
 * this repo's first use of RMT and bit-bang instead — RMT is the correct,
 * jitter-free tool for a protocol this timing-sensitive, and this device
 * type is a reasonable place to introduce it). The exact API pattern here —
 * `rmt_new_tx_channel()`, `rmt_new_simple_encoder()` with a byte-by-byte
 * callback, `rmt_transmit()`, `rmt_tx_wait_all_done()` — is checked directly
 * against Espressif's own official reference example
 * (`examples/peripherals/rmt/led_strip_simple_encoder` in this repo's
 * pinned ESP-IDF v5.5.4), which targets classic ESP32 among its supported
 * boards; only the per-chip timing constants and pixel byte order differ
 * from that example, both sourced from the chips' own datasheets as above,
 * not copied from the example (which only demonstrates WS2812 and uses
 * slightly different — but still within-tolerance — round numbers).
 *
 * --- Output -------------------------------------------------------------
 * ADDRESSABLE_LIGHT_PIXEL_COUNT pixels, all set to the same on/off + level +
 * hue + saturation result together — same "off doesn't forget where it was"
 * behavior as firmware/color-light/ and firmware/dimmable-light/: turning
 * back on restores the last color, not a reset one. The full pixel buffer is
 * re-sent on every relevant attribute change, same as firmware/color-light/
 * re-computing LEDC duty on every change — there's no partial/incremental
 * update, which is unnecessary at this pixel-count/update-rate scale.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include <esp_matter.h>

static const char *TAG = "matter_addressable_light";

/* Change this to the GPIO your strip's DIN (data in) pin is wired to. Any
 * GPIO works — this is a software/RMT-timed protocol, not a fixed
 * peripheral pin. GPIO 2 is a plain, unreserved GPIO on classic ESP32
 * (WROOM-32), matching this repo's other single-pin device types'
 * default. Adjust to match your board. */
#define ADDRESSABLE_LIGHT_GPIO GPIO_NUM_2

/* Number of physically wired pixels on your strip — MUST match reality:
 * fewer configured than wired just leaves the extra ones dark (harmless),
 * but more configured than wired sends extra bits that the last real pixel
 * on the strip passes through as if they were meant for a pixel after it,
 * which does nothing useful but also isn't harmful. 8 is a small, common
 * test-strip length; adjust to your actual strip. */
#define ADDRESSABLE_LIGHT_PIXEL_COUNT 8

/* ADDRESSABLE_LIGHT_CHIP selects which addressable protocol/byte order to
 * generate — see the header comment above for the full explanation and
 * exact datasheet sourcing of every constant below. */
#define ADDRESSABLE_LIGHT_CHIP_WS2812B 0
#define ADDRESSABLE_LIGHT_CHIP_SK6812 1
#define ADDRESSABLE_LIGHT_CHIP ADDRESSABLE_LIGHT_CHIP_WS2812B

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2812B
#define ADDRESSABLE_LIGHT_T0H_US 0.4f
#define ADDRESSABLE_LIGHT_T0L_US 0.85f
#define ADDRESSABLE_LIGHT_T1H_US 0.8f
#define ADDRESSABLE_LIGHT_T1L_US 0.45f
#define ADDRESSABLE_LIGHT_RESET_US 300
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 3
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812
#define ADDRESSABLE_LIGHT_T0H_US 0.3f
#define ADDRESSABLE_LIGHT_T0L_US 0.9f
#define ADDRESSABLE_LIGHT_T1H_US 0.6f
#define ADDRESSABLE_LIGHT_T1L_US 0.6f
#define ADDRESSABLE_LIGHT_RESET_US 80
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 4
#endif

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the strip's own on/off/color state. GPIO 15 is a
 * plain, unreserved GPIO on classic ESP32 (WROOM-32) that doesn't collide
 * with the data pin above. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_15
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* RMT resolution — 10MHz (1 tick = 0.1us), same as Espressif's own
 * reference example; fine enough to represent every timing value above to
 * within its documented tolerance. */
#define ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ 10000000

/* Initial brightness/hue/saturation — same defaults and reasoning as
 * firmware/color-light/'s (a visibly "on and colored" first boot). */
#define ADDRESSABLE_LIGHT_DEFAULT_LEVEL 128
#define ADDRESSABLE_LIGHT_DEFAULT_HUE 21          /* ~30 degrees: 21 * 360 / 254 */
#define ADDRESSABLE_LIGHT_DEFAULT_SATURATION 254  /* fully saturated */

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t addressable_light_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static rmt_channel_handle_t addressable_light_rmt_chan = NULL;
static rmt_encoder_handle_t addressable_light_rmt_encoder = NULL;
static uint8_t addressable_light_pixels[ADDRESSABLE_LIGHT_PIXEL_COUNT * ADDRESSABLE_LIGHT_BYTES_PER_PIXEL];

/* Mirrors the Matter OnOff/LevelControl/ColorControl attributes' current
 * values — same pattern as firmware/color-light/'s. */
static bool light_on = false;
static uint8_t light_level = ADDRESSABLE_LIGHT_DEFAULT_LEVEL;
static uint8_t light_hue = ADDRESSABLE_LIGHT_DEFAULT_HUE;
static uint8_t light_saturation = ADDRESSABLE_LIGHT_DEFAULT_SATURATION;

/* Textbook HSV -> RGB conversion — identical to firmware/color-light/'s
 * hsv_to_rgb(); see that file's header comment for why this needs no
 * SDK-specific verification (deterministic math, one well-known answer). */
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

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812
/* RGB -> RGBW via the standard "extract common white" technique — see the
 * header comment above for sourcing (same algorithm as
 * firmware/color-light/'s RGBW mode). Inputs and outputs are all in [0,1]. */
static void rgb_to_rgbw(float r, float g, float b, float *r_out, float *g_out, float *b_out, float *w_out)
{
    float w = fminf(r, fminf(g, b));
    *r_out = r - w;
    *g_out = g - w;
    *b_out = b - w;
    *w_out = w;
}
#endif

/* RMT symbol pair (HIGH pulse + LOW pulse) for a single WS2812B/SK6812 "0"
 * or "1" bit, plus the reset symbol — computed once from the datasheet
 * timing constants above, in the same style as Espressif's own reference
 * example. */
static const rmt_symbol_word_t addressable_light_bit0 = {
    .duration0 = (uint16_t)(ADDRESSABLE_LIGHT_T0H_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level0 = 1,
    .duration1 = (uint16_t)(ADDRESSABLE_LIGHT_T0L_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
};
static const rmt_symbol_word_t addressable_light_bit1 = {
    .duration0 = (uint16_t)(ADDRESSABLE_LIGHT_T1H_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level0 = 1,
    .duration1 = (uint16_t)(ADDRESSABLE_LIGHT_T1L_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
};
static const rmt_symbol_word_t addressable_light_reset = {
    .duration0 = (uint16_t)((uint64_t)ADDRESSABLE_LIGHT_RESET_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000 / 2),
    .level0 = 0,
    .duration1 = (uint16_t)((uint64_t)ADDRESSABLE_LIGHT_RESET_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000 / 2),
    .level1 = 0,
};

/* RMT "simple encoder" callback — serializes addressable_light_pixels[]
 * (whatever byte order the caller filled it with) MSB-first per byte, then
 * emits the reset symbol once all bytes are done. Pattern confirmed
 * against Espressif's own reference example (see header comment); only
 * the bit0/bit1/reset symbol VALUES differ, sourced from the datasheets
 * above rather than that example's round numbers. */
static size_t addressable_light_rmt_encode(const void *data, size_t data_size, size_t symbols_written,
                                           size_t symbols_free, rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    if (symbols_free < 8) {
        return 0; /* need room for a whole byte's worth of symbols before encoding one */
    }
    size_t data_pos = symbols_written / 8;
    const uint8_t *data_bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (int bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (data_bytes[data_pos] & bitmask) ? addressable_light_bit1 : addressable_light_bit0;
        }
        return symbol_pos;
    }
    symbols[0] = addressable_light_reset;
    *done = true;
    return 1;
}

/* Fills every pixel with the same on/off + level + hue + saturation result
 * (see the header comment on why "addressable" doesn't mean per-pixel
 * control here) and transmits the whole buffer via RMT. */
static void set_output(void)
{
    uint8_t r = 0, g = 0, b = 0, w = 0;

    if (light_on) {
        float hue_degrees = (float)light_hue * 360.0f / 254.0f;
        float saturation_fraction = (float)light_saturation / 254.0f;
        float value_fraction = (float)light_level / 254.0f;
        float rf, gf, bf;
        hsv_to_rgb(hue_degrees, saturation_fraction, value_fraction, &rf, &gf, &bf);

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812
        float wf;
        rgb_to_rgbw(rf, gf, bf, &rf, &gf, &bf, &wf);
        w = (uint8_t)(wf * 255.0f + 0.5f);
#endif
        r = (uint8_t)(rf * 255.0f + 0.5f);
        g = (uint8_t)(gf * 255.0f + 0.5f);
        b = (uint8_t)(bf * 255.0f + 0.5f);
    }

    for (int i = 0; i < ADDRESSABLE_LIGHT_PIXEL_COUNT; i++) {
        uint8_t *pixel = &addressable_light_pixels[i * ADDRESSABLE_LIGHT_BYTES_PER_PIXEL];
#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2812B
        /* GRB order — confirmed in WS2812B's own datasheet, see header comment. */
        pixel[0] = g;
        pixel[1] = r;
        pixel[2] = b;
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812
        /* RGBW order — confirmed in SK6812RGBW's own datasheet, see header
         * comment (including the note on library-vs-datasheet disagreement). */
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = w;
#endif
    }

    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0;
    esp_err_t err = rmt_transmit(addressable_light_rmt_chan, addressable_light_rmt_encoder,
                                  addressable_light_pixels, sizeof(addressable_light_pixels), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %d", err);
        return;
    }
    rmt_tx_wait_all_done(addressable_light_rmt_chan, pdMS_TO_TICKS(1000));
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

/* Called whenever a controller changes an attribute — same
 * attribute::PRE_UPDATE pattern as firmware/color-light/'s (all four
 * attributes here are plain ember attributes, no Delegate needed). */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != addressable_light_endpoint_id) {
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
        set_output();
        ESP_LOGI(TAG, "Hue set to %u/254", light_hue);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
        light_saturation = val->val.u8;
        set_output();
        ESP_LOGI(TAG, "Saturation set to %u/254", light_saturation);
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

    /* 2. Configure the RMT TX channel + simple encoder that drives the
     * addressable strip — see the header comment for why RMT (not
     * bit-banging) and the exact API pattern's sourcing. */
    rmt_tx_channel_config_t tx_chan_config = {};
    tx_chan_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_chan_config.gpio_num = ADDRESSABLE_LIGHT_GPIO;
    tx_chan_config.mem_block_symbols = 64;
    tx_chan_config.resolution_hz = ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ;
    tx_chan_config.trans_queue_depth = 4;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &addressable_light_rmt_chan));

    rmt_simple_encoder_config_t simple_encoder_config = {};
    simple_encoder_config.callback = addressable_light_rmt_encode;
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&simple_encoder_config, &addressable_light_rmt_encoder));

    ESP_ERROR_CHECK(rmt_enable(addressable_light_rmt_chan));
    memset(addressable_light_pixels, 0, sizeof(addressable_light_pixels));
    set_output(); /* light_on starts false, so this drives every pixel off. */

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
     * LevelControl + ColorControl[HueSaturation only] + ScenesManagement)
     * — identical composition to firmware/color-light/'s, see that file's
     * header comment for why this isn't endpoint::extended_color_light::create(). */
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
    level_control_config.current_level = nullable<uint8_t>(ADDRESSABLE_LIGHT_DEFAULT_LEVEL);
    level_control_config.on_level = nullable<uint8_t>(ADDRESSABLE_LIGHT_DEFAULT_LEVEL);
    cluster::level_control::feature::lighting::config_t level_control_lighting_config;
    level_control_lighting_config.start_up_current_level = nullable<uint8_t>(ADDRESSABLE_LIGHT_DEFAULT_LEVEL);
    cluster_t *level_control_cluster = cluster::level_control::create(ep, &level_control_config, CLUSTER_FLAG_SERVER);
    cluster::level_control::feature::on_off::add(level_control_cluster);
    cluster::level_control::feature::lighting::add(level_control_cluster, &level_control_lighting_config);

    cluster::color_control::config_t color_control_config;
    color_control_config.color_mode = chip::to_underlying(ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation);
    color_control_config.enhanced_color_mode = chip::to_underlying(ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation);
    color_control_config.color_capabilities = chip::to_underlying(ColorControl::ColorCapabilitiesBitmap::kHueSaturation);
    cluster_t *color_control_cluster = cluster::color_control::create(ep, &color_control_config, CLUSTER_FLAG_SERVER);
    cluster::color_control::feature::hue_saturation::config_t hue_saturation_config;
    hue_saturation_config.current_hue = ADDRESSABLE_LIGHT_DEFAULT_HUE;
    hue_saturation_config.current_saturation = ADDRESSABLE_LIGHT_DEFAULT_SATURATION;
    cluster::color_control::feature::hue_saturation::add(color_control_cluster, &hue_saturation_config);

    cluster::scenes_management::config_t scenes_management_config;
    cluster_t *scenes_management_cluster = cluster::scenes_management::create(ep, &scenes_management_config, CLUSTER_FLAG_SERVER);
    cluster::scenes_management::command::create_copy_scene(scenes_management_cluster);
    cluster::scenes_management::command::create_copy_scene_response(scenes_management_cluster);

    addressable_light_endpoint_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "Addressable light endpoint id: %u", addressable_light_endpoint_id);

    /* CurrentLevel/CurrentHue/CurrentSaturation all change rapidly while a
     * controller is actively dragging a brightness/color slider —
     * deferring their NVS persistence avoids writing flash on every single
     * step. Same call firmware/color-light/ and firmware/dimmable-light/
     * make, for the same reason. */
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, LevelControl::Id,
                                                        LevelControl::Attributes::CurrentLevel::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentHue::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentSaturation::Id));

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Matter addressable light started. Scan the QR code to commission.");
}
