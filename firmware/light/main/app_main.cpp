/*
 * Minimal Matter On/Off Light.
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
 * --- Optional RGB Status LED -----------------------------------------------
 * Off by default (all three GPIOs at GPIO_NUM_NC, checked at runtime the
 * same way firmware/outlet/'s own Status LED is — that sentinel is a
 * gpio_num_t enumerator, not a preprocessor macro, so it can't be tested
 * with `#if`). When wired up, shows Matter's own defined commissioning and
 * Identify states with color + a blink/breathe pattern engine (LEDC PWM,
 * same peripheral firmware/dimmable-light/ and firmware/color-light/
 * already use for their own outputs) — every state and its color/timing is
 * sourced from two real, verified places, not invented and not copied from
 * a reference screenshot without checking it against the actual spec/SDK
 * source first:
 *
 * "Setup ..." states — the DeviceLayer's own lifecycle events, confirmed
 * directly in connectedhomeip's own CHIPDeviceEvent.h:
 *   Setup mode      (breathing green,  ~2s cycle) — kCHIPoBLEAdvertisingChange
 *                    with Result == kActivity_Started (device is
 *                    advertising, not yet paired).
 *   Setup started   (breathing yellow, ~1s cycle) — kSecureSessionEstablished
 *                    (a commissioner has opened a PASE session).
 *   Setup complete  (LED returns to off)          — kCommissioningComplete
 *                    (already logged here before this feature existed).
 *   Setup failed    (solid red)                   — kFailSafeTimerExpired
 *                    ("Signals that the fail-safe timer expired before the
 *                    CommissioningComplete command was successfully
 *                    invoked" — connectedhomeip's own doc comment).
 *
 * "Identification ..." states — the Identify cluster's own
 * EffectIdentifierEnum, confirmed directly in the generated
 * Identify/Enums.h (kBlink=0x00, kBreathe=0x01, kOkay=0x02,
 * kChannelChange=0x0B, kFinishEffect=0xFE, kStopEffect=0xFF — the same six
 * values `app_identification_cb`'s EFFECT case already receives as
 * `effect_id`, previously left undifferentiated ("blinking as usual") in
 * every device type in this repo):
 *   start           (blinking red,    ~1s cycle) — a plain Identify command
 *                    with no specific effect (identification::START).
 *   stop                                          — identification::STOP.
 *   blink           (white, 1 on/off cycle, 1s total)   — kBlink.
 *   breathe         (breathing white, ~1s cycle, 15s total) — kBreathe.
 *   okay            (green,  1s-on/1s-off, repeats until Finish/Stop) — kOkay.
 *   channel change  (yellow, 8s-on/8s-off, repeats until Finish/Stop) —
 *                    kChannelChange.
 *   finish/stop effect (LED returns to off) — kFinishEffect/kStopEffect.
 * The BLINK pattern below is a plain 50%-duty square wave over whatever
 * period it's given — that alone reproduces every one of the timing
 * descriptions above (e.g. "blinks for 8 seconds, about 16 seconds per
 * cycle" is exactly a 50%-duty wave at a 16s period) without needing a
 * separate duty-cycle parameter.
 *
 * Reuses 3 LEDC channels on their own timer (LEDC_TIMER_1, channels 5-7)
 * so this never collides with a device type's own LEDC usage for its main
 * output (LEDC_TIMER_0, channels 0-4 at most, e.g.
 * firmware/color-light/'s RGBWW mode). GPIO 25/26/27 are the defaults here
 * — free on every device type's own default pin set in this repo except
 * firmware/outlet/'s power-monitor chips, which use different defaults
 * there (see that file's own header comment).
 *
 * --- Factory reset via quick power cycle ------------------------------------
 * Power the device off and on 3 times in a row (roughly a couple of
 * seconds each way) and it factory-resets and re-enters commissioning
 * setup mode — no button or extra pin needed, the same mechanism real
 * plug-in/hardwired smart-home devices (which often have no accessible
 * reset button once installed) commonly use; Tasmota's own "Quick Power
 * Cycle" detection works the same way. A plain counter in its own
 * "boot_info" NVS namespace (separate from esp_matter's/Matter's own
 * storage) increments on every boot and starts a one-shot
 * FACTORY_RESET_CONFIRM_DELAY_MS timer; if the device stays powered that
 * long without another reboot, the counter clears back to 0 (a
 * "confirmed" normal boot). 3 reboots landing before that timer fires
 * reaches FACTORY_RESET_BOOT_COUNT_THRESHOLD and triggers a real reset.
 *
 * The actual esp_matter::factory_reset() call (declared in
 * esp_matter_core.h: "Perform factory reset and erase the data stored in
 * the non volatile storage. This also restarts the device.") is only made
 * AFTER esp_matter::start() has completed, not during the early boot-count
 * check — confirmed by reading its own implementation in
 * esp_matter_core.cpp, which calls
 * chip::Server::GetInstance().ScheduleFactoryReset() and needs the Matter
 * server already running, and by cross-checking esp-matter's own reference
 * app_reset component (examples/common/app_reset/app_reset.cpp), which
 * only ever calls it from a runtime button callback, never during boot.
 * check_factory_reset_boot_count() therefore only decides whether a reset
 * is due; app_main() acts on that decision once Matter has actually
 * started.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <math.h>

#include <esp_matter.h>

static const char *TAG = "matter_light";

/* Change this to the GPIO your LED is wired to.
 * GPIO 2 is common on classic ESP32 (WROOM-32) devkits; ESP32-C6 devkits often
 * use GPIO 8. Adjust to match your board. */
#define LIGHT_LED_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the light's own on/off state. Any free GPIO
 * works; GPIO 4 is commonly unused on classic ESP32 (WROOM-32) devkits.
 * Adjust to match your board, or wire it to the same LED as LIGHT_LED_GPIO
 * if you only have one and don't mind it blinking during identify. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Optional RGB status LED — see the header comment above for the full
 * state list and sourcing. Off by default; adjust to match your board if
 * you have one wired up. */
#define STATUS_LED_RED_GPIO GPIO_NUM_NC
#define STATUS_LED_GREEN_GPIO GPIO_NUM_NC
#define STATUS_LED_BLUE_GPIO GPIO_NUM_NC
#define STATUS_LED_LEDC_TIMER LEDC_TIMER_1
#define STATUS_LED_LEDC_RED_CHANNEL LEDC_CHANNEL_5
#define STATUS_LED_LEDC_GREEN_CHANNEL LEDC_CHANNEL_6
#define STATUS_LED_LEDC_BLUE_CHANNEL LEDC_CHANNEL_7
#define STATUS_LED_LEDC_MODE LEDC_LOW_SPEED_MODE
#define STATUS_LED_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define STATUS_LED_LEDC_FREQUENCY_HZ 5000
#define STATUS_LED_TICK_MS 20 /* pattern-engine update interval */
#define STATUS_LED_PI 3.14159265f

/* Quick-power-cycle factory reset — see the header comment above for the
 * full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t light_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Status LED pattern engine ------------------------------------------
 * A small timer-driven state machine: status_led_set() records a target
 * color + pattern + period + optional auto-off duration, and the periodic
 * tick callback computes the current brightness multiplier for whichever
 * pattern is active and writes it to the 3 LEDC channels. */
typedef enum {
    STATUS_LED_PATTERN_OFF,
    STATUS_LED_PATTERN_SOLID,
    STATUS_LED_PATTERN_BLINK,
    STATUS_LED_PATTERN_BREATHE,
} status_led_pattern_t;

static bool status_led_enabled = false;
static esp_timer_handle_t status_led_timer = NULL;
static uint8_t status_led_r = 0, status_led_g = 0, status_led_b = 0;
static status_led_pattern_t status_led_pattern = STATUS_LED_PATTERN_OFF;
static uint32_t status_led_period_ms = 1000;
static uint32_t status_led_elapsed_ms = 0;
static uint32_t status_led_duration_ms = 0; /* 0 = indefinite, until the next status_led_set()/status_led_off() */

static void status_led_set(uint8_t r, uint8_t g, uint8_t b, status_led_pattern_t pattern,
                            uint32_t period_ms, uint32_t duration_ms)
{
    if (!status_led_enabled) {
        return;
    }
    status_led_r = r;
    status_led_g = g;
    status_led_b = b;
    status_led_pattern = pattern;
    status_led_period_ms = period_ms > 0 ? period_ms : 1000;
    status_led_duration_ms = duration_ms;
    status_led_elapsed_ms = 0;
}

static void status_led_off(void)
{
    status_led_set(0, 0, 0, STATUS_LED_PATTERN_OFF, 1000, 0);
}

static void status_led_tick_cb(void *arg)
{
    if (!status_led_enabled) {
        return;
    }
    status_led_elapsed_ms += STATUS_LED_TICK_MS;
    if (status_led_duration_ms > 0 && status_led_elapsed_ms >= status_led_duration_ms) {
        status_led_off();
    }

    float phase = fmodf((float)status_led_elapsed_ms, (float)status_led_period_ms) / (float)status_led_period_ms;
    float brightness;
    switch (status_led_pattern) {
    case STATUS_LED_PATTERN_SOLID:
        brightness = 1.0f;
        break;
    case STATUS_LED_PATTERN_BLINK:
        brightness = (phase < 0.5f) ? 1.0f : 0.0f; /* plain 50%-duty square wave — see header comment */
        break;
    case STATUS_LED_PATTERN_BREATHE:
        brightness = (sinf(phase * 2.0f * STATUS_LED_PI - STATUS_LED_PI / 2.0f) + 1.0f) / 2.0f;
        break;
    case STATUS_LED_PATTERN_OFF:
    default:
        brightness = 0.0f;
        break;
    }

    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL, (uint32_t)(status_led_r * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL, (uint32_t)(status_led_g * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL, (uint32_t)(status_led_b * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL);
}

static void set_led(bool on)
{
    gpio_set_level(LIGHT_LED_GPIO, on ? 1 : 0);
}

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Lifecycle events from the Matter stack (commissioning, connectivity, ...).
 * See the header comment on the Status LED for exactly which events map to
 * which "Setup ..." state and why. */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
        if (event->CHIPoBLEAdvertisingChange.Result == chip::DeviceLayer::kActivity_Started) {
            status_led_set(0, 255, 0, STATUS_LED_PATTERN_BREATHE, 2000, 0); /* Setup mode */
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kSecureSessionEstablished:
        status_led_set(255, 255, 0, STATUS_LED_PATTERN_BREATHE, 1000, 0); /* Setup started */
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete — device is now paired");
        status_led_off(); /* Setup complete */
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        status_led_set(255, 0, 0, STATUS_LED_PATTERN_SOLID, 1000, 0); /* Setup failed */
        break;
    default:
        break;
    }
}

/* Called whenever a controller changes an attribute — e.g. toggles the light.
 * We react on PRE_UPDATE so the LED follows the requested state. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && endpoint_id == light_endpoint_id &&
        cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        set_led(val->val.b);
        ESP_LOGI(TAG, "Light turned %s", val->val.b ? "ON" : "OFF");
    }
    return ESP_OK;
}

/* Called when a controller asks the device to "identify" itself — starts
 * or stops the identify LED blinking accordingly, and (when the Status LED
 * is wired up) shows the matching color/pattern for the specific Identify
 * effect — see the header comment for the full state list and sourcing. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    switch (type) {
    case identification::START:
        ESP_LOGI(TAG, "Identify started on endpoint %u", endpoint_id);
        esp_timer_start_periodic(identify_led_timer, IDENTIFY_BLINK_INTERVAL_MS * 1000);
        status_led_set(255, 0, 0, STATUS_LED_PATTERN_BLINK, 1000, 0);
        break;
    case identification::STOP:
        ESP_LOGI(TAG, "Identify stopped on endpoint %u", endpoint_id);
        esp_timer_stop(identify_led_timer);
        gpio_set_level(IDENTIFY_LED_GPIO, 0);
        status_led_off();
        break;
    case identification::EFFECT:
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u",
                 effect_id, effect_variant, endpoint_id);
        if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBlink)) {
            status_led_set(255, 255, 255, STATUS_LED_PATTERN_BLINK, 1000, 1000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBreathe)) {
            status_led_set(255, 255, 255, STATUS_LED_PATTERN_BREATHE, 1000, 15000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kOkay)) {
            status_led_set(0, 255, 0, STATUS_LED_PATTERN_BLINK, 2000, 0);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kChannelChange)) {
            status_led_set(255, 255, 0, STATUS_LED_PATTERN_BLINK, 16000, 0);
        } else {
            /* kFinishEffect, kStopEffect, or anything unrecognized. */
            status_led_off();
        }
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
 * only acts on that later, after Matter has started — see the header
 * comment on why. Must run after nvs_flash_init(). */
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

    /* 1b. Quick-power-cycle factory reset check — see the header comment
     * above check_factory_reset_boot_count(). The actual reset (if due)
     * only happens later, once Matter has started. */
    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the LED output. */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << LIGHT_LED_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    set_led(false);

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

    /* 2c. Configure the optional RGB status LED + its pattern-engine timer
     * — only if at least one of its 3 GPIOs is actually wired up (not the
     * GPIO_NUM_NC default). */
    status_led_enabled = (STATUS_LED_RED_GPIO != GPIO_NUM_NC) ||
                          (STATUS_LED_GREEN_GPIO != GPIO_NUM_NC) ||
                          (STATUS_LED_BLUE_GPIO != GPIO_NUM_NC);
    if (status_led_enabled) {
        ledc_timer_config_t status_led_ledc_timer = {};
        status_led_ledc_timer.speed_mode = STATUS_LED_LEDC_MODE;
        status_led_ledc_timer.duty_resolution = STATUS_LED_LEDC_DUTY_RES;
        status_led_ledc_timer.timer_num = STATUS_LED_LEDC_TIMER;
        status_led_ledc_timer.freq_hz = STATUS_LED_LEDC_FREQUENCY_HZ;
        status_led_ledc_timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&status_led_ledc_timer);

        struct {
            ledc_channel_t channel;
            gpio_num_t gpio;
        } status_led_channels[] = {
            { STATUS_LED_LEDC_RED_CHANNEL, STATUS_LED_RED_GPIO },
            { STATUS_LED_LEDC_GREEN_CHANNEL, STATUS_LED_GREEN_GPIO },
            { STATUS_LED_LEDC_BLUE_CHANNEL, STATUS_LED_BLUE_GPIO },
        };
        for (size_t i = 0; i < sizeof(status_led_channels) / sizeof(status_led_channels[0]); i++) {
            if (status_led_channels[i].gpio == GPIO_NUM_NC) {
                continue; /* e.g. only the green channel wired up */
            }
            ledc_channel_config_t status_led_channel = {};
            status_led_channel.gpio_num = status_led_channels[i].gpio;
            status_led_channel.speed_mode = STATUS_LED_LEDC_MODE;
            status_led_channel.channel = status_led_channels[i].channel;
            status_led_channel.intr_type = LEDC_INTR_DISABLE;
            status_led_channel.timer_sel = STATUS_LED_LEDC_TIMER;
            status_led_channel.duty = 0;
            status_led_channel.hpoint = 0;
            ledc_channel_config(&status_led_channel);
        }

        const esp_timer_create_args_t status_led_timer_args = {
            .callback = &status_led_tick_cb,
            .name = "status_led",
        };
        esp_timer_create(&status_led_timer_args, &status_led_timer);
        esp_timer_start_periodic(status_led_timer, STATUS_LED_TICK_MS * 1000);
    }

    /* 3. Build the Matter data model: one node, one On/Off Light endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    on_off_light::config_t light_config;
    endpoint_t *endpoint = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create light endpoint");
        return;
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Light endpoint id: %u", light_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* 5. If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started — see the header comment on
     * why this can't happen any earlier. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter light started. Scan the QR code to commission.");
}
