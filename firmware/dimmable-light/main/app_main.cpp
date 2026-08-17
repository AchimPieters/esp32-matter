/*
 * Minimal Matter Dimmable Light — seventh device type, and this repo's first
 * with a real actuator beyond plain on/off (every prior device type is
 * either a digital GPIO output, a sensor, or a remote-control switch).
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
 * --- Why this is a separate device type from firmware/light/ -------------
 * firmware/light/ uses esp-matter's `on_off_light` endpoint — OnOff cluster
 * only, a plain digital GPIO output (fully on or fully off, no brightness
 * concept at all). This device uses `dimmable_light` instead, which adds the
 * LevelControl cluster (`level_control::create()`, matches the endpoint
 * composition in esp-matter's own `endpoint::dimmable_light::add()` —
 * checked directly in esp_matter_endpoint.cpp rather than assumed) on top of
 * the same OnOff cluster, driving the output as an actual PWM duty cycle via
 * ESP-IDF's `driver/ledc.h` (the LEDC hardware PWM peripheral) instead of a
 * plain gpio_set_level() — a real functional difference, not just a config
 * flag, since it needs its own peripheral setup and its own attribute to
 * react to.
 *
 * --- LevelControl integration: same "ember attribute" pattern as OnOff ---
 * Checked directly against esp-matter's own reference `examples/light/`
 * (app_driver.cpp/app_main.cpp) before writing this: LevelControl's
 * CurrentLevel is NOT one of the newer "code-driven" cluster classes this
 * repo has hit before (BooleanState in firmware/contact-sensor/,
 * TemperatureMeasurement in firmware/temperature-sensor/, etc. — those need
 * a cluster-specific setter looked up through the data model provider's
 * registry, since esp-matter's generic attribute::update() can't write
 * them). LevelControl has no folder under esp-matter's
 * data_model_provider/clusters/ — confirmed by listing that directory —
 * meaning it's handled the older, plain "ember" way, exactly like OnOff:
 * connectedhomeip's own LevelControlServer (src/app/clusters/level-control/)
 * fully implements the MoveToLevel/Move/Step/Stop commands internally and
 * writes CurrentLevel through the ordinary ember attribute store, which
 * fires this file's app_attribute_update_cb() on PRE_UPDATE just like OnOff
 * already does — no special setter needed, same attribute::update() pattern
 * used everywhere else in this repo for plain ember attributes.
 *
 * --- Brightness scaling ---------------------------------------------------
 * CurrentLevel's valid range is 1-254 (0 is reserved/means "off", which the
 * OnOff cluster already covers separately — matches level_control::config_t's
 * own min_level=1/max_level=254 defaults, unmodified here). LEDC's duty
 * range at 8-bit resolution is 0-255 — close enough to CurrentLevel's range
 * that this file uses CurrentLevel directly as the LEDC duty value with no
 * remapping math (unlike esp-matter's own example, which remaps through a
 * generic led_driver component's separate 0-100 percentage scale via
 * REMAP_TO_RANGE() — not needed here since this file talks to the LEDC
 * peripheral directly). The ~0.4% difference at full brightness (254 vs 255)
 * is not perceptible.
 *
 * The output is the product of two independent pieces of state — on/off AND
 * level — same as any real dimmer: turning the light off doesn't forget the
 * brightness it was at, and turning it back on (from either the physical
 * side or a remote controller) restores that same level. set_output() below
 * computes the actual PWM duty from both `light_on` and `light_level`
 * together, mirroring exactly how a physical dimmer switch behaves.
 *
 * --- Boots off, like every other device type in this repo -----------------
 * `on_off_lighting.start_up_on_off` is left at its config default (0 = Off)
 * rather than overridden to null ("restore previous state") the way
 * esp-matter's own example does — matches firmware/light/'s explicit
 * set_led(false) at boot and firmware/outlet/'s/switch's equivalent
 * behavior: this repo's own convention is that every device starts in a
 * known, deterministic Off state after a fresh boot or power cycle, not
 * whatever it happened to be doing before power was lost.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <esp_matter.h>

static const char *TAG = "matter_dimmable_light";

/* Change this to the GPIO your LED (or MOSFET/driver board gate driving a
 * higher-power LED strip) is wired to. Needs to be an LEDC-capable output
 * pin — true for nearly every GPIO on these chips except input-only ones
 * (GPIO 34-39 on classic ESP32). GPIO 2 is common on classic ESP32
 * (WROOM-32) devkits; ESP32-C6 devkits often use GPIO 8. Adjust to match
 * your board. */
#define DIMMABLE_LIGHT_LED_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the light's own on/off/brightness state. Any free
 * GPIO works; GPIO 4 is commonly unused on classic ESP32 (WROOM-32)
 * devkits. Adjust to match your board, or wire it to the same LED as
 * DIMMABLE_LIGHT_LED_GPIO if you only have one and don't mind it blinking
 * (at full brightness, not dimmed) during identify. Deliberately plain
 * gpio_set_level(), not LEDC — the identify blink is just on/off, no need
 * for a second PWM channel. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (LED Control / PWM) peripheral setup for the dimmable output.
 * LEDC_LOW_SPEED_MODE works identically on every module this repo targets
 * (esp32/c3/c6/s3/h2) — classic ESP32 also has a high-speed mode, but using
 * only low-speed keeps this file portable across all five without an #if.
 * LEDC_AUTO_CLK lets the driver pick a valid source clock for the chosen
 * frequency/resolution itself, rather than hardcoding a clock source that
 * might not exist on every target. 5 kHz is a common LED PWM frequency —
 * fast enough to avoid visible flicker, slow enough that 8-bit resolution
 * (0-255 duty steps) is comfortably achievable on every target's LEDC clock. */
#define DIMMABLE_LIGHT_LEDC_TIMER LEDC_TIMER_0
#define DIMMABLE_LIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define DIMMABLE_LIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DIMMABLE_LIGHT_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define DIMMABLE_LIGHT_LEDC_FREQUENCY_HZ 5000

/* Initial brightness (CurrentLevel, and therefore LEDC duty — see the header
 * comment on brightness scaling above) used both for the attribute's
 * starting value and for what the light shows the first time it's turned on
 * after a fresh flash or factory reset. Roughly 50% — bright enough to
 * confirm dimming actually works without immediately going to full
 * brightness. Valid range is 1-254; adjust freely. */
#define DIMMABLE_LIGHT_DEFAULT_LEVEL 128

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t light_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Mirrors the Matter OnOff/LevelControl attributes' current values — kept in
 * sync solely by app_attribute_update_cb() below, which fires for every
 * change to either attribute regardless of source (a remote controller, or
 * this device's own attribute::update() calls). set_output() reads both to
 * compute the actual PWM duty, same as a real dimmer combining its on/off
 * state with its remembered brightness. */
static bool light_on = false;
static uint8_t light_level = DIMMABLE_LIGHT_DEFAULT_LEVEL;

/* Drives the LEDC duty from the current on/off + level state together — see
 * the header comment on brightness scaling for why light_level is used
 * directly as the duty value with no remapping. */
static void set_output(void)
{
    uint32_t duty = light_on ? light_level : 0;
    ledc_set_duty(DIMMABLE_LIGHT_LEDC_MODE, DIMMABLE_LIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(DIMMABLE_LIGHT_LEDC_MODE, DIMMABLE_LIGHT_LEDC_CHANNEL);
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

/* Called whenever a controller changes an attribute — e.g. toggles the light
 * or moves its brightness slider. We react on PRE_UPDATE so the PWM output
 * follows the requested state. Both OnOff and LevelControl are plain ember
 * attributes here (see the header comment on LevelControl integration for
 * why), so this is the exact same attribute::PRE_UPDATE pattern
 * firmware/light/ already uses for OnOff — just handling a second cluster
 * too. */
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
        /* CurrentLevel is nullable per spec; a null value has no defined
         * brightness, so just keep whatever level we already had rather
         * than writing garbage into light_level. */
        if (!val->val.u8) {
            return ESP_OK;
        }
        light_level = val->val.u8;
        set_output();
        ESP_LOGI(TAG, "Light level set to %u/254", light_level);
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

    /* 2. Configure the dimmable LED output via LEDC (hardware PWM) — a
     * timer (shared clock/frequency/resolution) plus a channel (the actual
     * GPIO + duty register) per ESP-IDF's driver/ledc.h API. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = DIMMABLE_LIGHT_LEDC_MODE;
    ledc_timer.duty_resolution = DIMMABLE_LIGHT_LEDC_DUTY_RES;
    ledc_timer.timer_num = DIMMABLE_LIGHT_LEDC_TIMER;
    ledc_timer.freq_hz = DIMMABLE_LIGHT_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = DIMMABLE_LIGHT_LED_GPIO;
    ledc_channel.speed_mode = DIMMABLE_LIGHT_LEDC_MODE;
    ledc_channel.channel = DIMMABLE_LIGHT_LEDC_CHANNEL;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = DIMMABLE_LIGHT_LEDC_TIMER;
    ledc_channel.duty = 0;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
    set_output(); /* light_on starts false, so this drives duty 0 (fully off). */

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

    /* 3. Build the Matter data model: one node, one Dimmable Light endpoint
     * (OnOff + LevelControl clusters). current_level/on_level/
     * start_up_current_level are set explicitly to DIMMABLE_LIGHT_DEFAULT_LEVEL
     * rather than left at the config defaults' null/0 — matches
     * esp-matter's own examples/light/, which does the same for the same
     * reason: a null CurrentLevel has no defined brightness, which is a
     * worse first-boot/first-read experience than a concrete, reasonable
     * value. on_off.on_off and on_off_lighting.start_up_on_off are left at
     * their config defaults (false / 0 = Off) — see the header comment on
     * booting off. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    dimmable_light::config_t light_config;
    light_config.level_control.current_level = DIMMABLE_LIGHT_DEFAULT_LEVEL;
    light_config.level_control.on_level = DIMMABLE_LIGHT_DEFAULT_LEVEL;
    light_config.level_control_lighting.start_up_current_level = DIMMABLE_LIGHT_DEFAULT_LEVEL;
    endpoint_t *endpoint = dimmable_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create dimmable light endpoint");
        return;
    }

    light_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Dimmable light endpoint id: %u", light_endpoint_id);

    /* CurrentLevel changes rapidly while a controller is actively dragging
     * a brightness slider — deferring its NVS persistence (instead of
     * writing flash on every single step) avoids unnecessary flash wear
     * during normal use. Same call esp-matter's own examples/light/ makes
     * for the same attribute, for the same reason. */
    attribute_t *current_level_attribute = attribute::get(light_endpoint_id, LevelControl::Id,
                                                           LevelControl::Attributes::CurrentLevel::Id);
    attribute::set_deferred_persistence(current_level_attribute);

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

    ESP_LOGI(TAG, "Matter dimmable light started. Scan the QR code to commission.");
}
