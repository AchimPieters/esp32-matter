/*
 * Minimal Matter Mounted Dimmable Load Control — fifty-fourth device type,
 * and the dimmable sibling of firmware/mounted-onoff-control/: an in-wall
 * dimmer module wired into an existing fixture's own switch-leg (the class
 * of hardware sold as e.g. a "smart dimmer switch" retrofit module),
 * replacing the fixture's own dumb dimmer rather than being a smart bulb
 * or a plug-in dimmer.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Why a separate device type from firmware/dimmable-light/ and
 * firmware/dimmable-plug/ ---------------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * MountedDimmableLoadControl.xml (device type 0x0110, revision 2, explicitly
 * classified as `superset="Dimmable Plug-In Unit"`): Identify (with
 * TriggerEffect) + Groups + On/Off[Lighting] + Level Control[OnOff+Lighting,
 * mandatoryConform here — not merely optional the way it is on firmware/
 * light/'s own OnOffLight device type] + Scenes Management (with CopyScene)
 * are ALL mandatoryConform — the real, meaningful difference from firmware/
 * dimmable-light/'s own DimmableLight (0x0101): Groups and Scenes
 * Management are mandatory here too. `endpoint::mounted_dimmable_load_
 * control::create()` confirmed complete/ready-to-use by reading esp-
 * matter's own legacy `mounted_dimmable_load_control::add()` directly —
 * and its own `config_t` is confirmed to be a straight alias
 * (`using config_t = dimmable_light::config_t;`) for firmware/
 * dimmable-light/'s own config type, so this file's own endpoint
 * construction (including the OnLevel-null fix below) is a direct, verbatim
 * reuse of that file's own proven configuration, just passed to a different
 * top-level `create()` function that also wires up Groups + ScenesManagement
 * automatically. Zero manual cluster-creation code needed here at all.
 *
 * --- Output: PWM gating a real dimmer module's control input, same framing
 * as firmware/dimmable-plug/ -------------------------------------------------
 * Same real, deliberate framing difference from firmware/dimmable-light/'s
 * own LED output as firmware/dimmable-plug/'s own header comment already
 * establishes for the plug-in case: a real AC-mains dimmer needs a genuine
 * TRIAC/phase-control circuit, real safety-relevant power electronics
 * outside this repo's "read the datasheet, drive the GPIO" style without
 * real hardware to validate against (same reasoning firmware/evse/'s own
 * safety note already applies). `MOUNTED_DIMMER_PWM_GPIO` drives real PWM
 * via ESP-IDF's `driver/ledc.h` — the exact same LEDC peripheral/settings
 * firmware/dimmable-light/'s own output already uses, appropriate for
 * either a DC load through a MOSFET, or a real commercial AC dimmer
 * module's own PWM/analog dimming-control input — gating an existing
 * dimmer's own control input rather than attempting to switch mains
 * current directly.
 *
 * `OnLevel` is left null (`nullable<uint8_t>()`), reusing the same real,
 * hardware-confirmed bug fix firmware/dimmable-light/'s, firmware/
 * color-light/'s, and firmware/dimmable-plug/'s own LevelControl configs
 * already apply (a concrete OnLevel would force CurrentLevel back to a
 * fixed value on every plain OnOff::On instead of restoring the last
 * brightness — confirmed by reading `emberAfOnOffClusterLevelControl
 * EffectCallback()` directly, per firmware/dimmable-light/'s own header
 * comment).
 *
 * --- Occupancy Sensing (client): optionalConform, same NULL-config shell
 * pattern already established six times over -------------------------------
 * Confirmed directly against MountedDimmableLoadControl.xml: Occupancy
 * Sensing (client) is `<optionalConform/>` — added via the identical
 * `cluster::occupancy_sensing::create(endpoint, NULL, CLUSTER_FLAG_CLIENT)`
 * shape firmware/mounted-onoff-control/'s own identical addition already
 * establishes.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no MOSFET/dimmer-module hardware physically available
 * for this specific device type when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <esp_matter.h>

static const char *TAG = "matter_mounted_dimmer";

/* Change this to the GPIO your MOSFET (or a real dimmer module's own PWM/
 * analog dimming-control input) is wired to. Needs to be an LEDC-capable
 * output pin — true for nearly every GPIO on these chips except input-only
 * ones (GPIO 34-39 on classic ESP32). */
#define MOUNTED_DIMMER_PWM_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (LED Control / PWM) peripheral setup — same settings firmware/
 * dimmable-light/'s own header comment already justifies in full. */
#define MOUNTED_DIMMER_LEDC_TIMER LEDC_TIMER_0
#define MOUNTED_DIMMER_LEDC_CHANNEL LEDC_CHANNEL_0
#define MOUNTED_DIMMER_LEDC_MODE LEDC_LOW_SPEED_MODE
#define MOUNTED_DIMMER_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define MOUNTED_DIMMER_LEDC_FREQUENCY_HZ 5000

/* Initial brightness (CurrentLevel, and therefore LEDC duty) — see
 * firmware/dimmable-light/'s own header comment for the full reasoning.
 * Roughly 50%; valid range is 1-254. */
#define MOUNTED_DIMMER_DEFAULT_LEVEL 128

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t dimmer_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Mirrors the Matter OnOff/LevelControl attributes' current values — see
 * firmware/dimmable-light/'s own header comment for the full explanation
 * of this pattern. */
static bool load_on = false;
static uint8_t load_level = MOUNTED_DIMMER_DEFAULT_LEVEL;

static void set_output(void)
{
    uint32_t duty = load_on ? load_level : 0;
    ledc_set_duty(MOUNTED_DIMMER_LEDC_MODE, MOUNTED_DIMMER_LEDC_CHANNEL, duty);
    ledc_update_duty(MOUNTED_DIMMER_LEDC_MODE, MOUNTED_DIMMER_LEDC_CHANNEL);
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
 * load or moves its brightness slider. Both OnOff and LevelControl are
 * plain ember attributes here, same as firmware/dimmable-light/'s own
 * identical pattern. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != dimmer_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        load_on = val->val.b;
        set_output();
        ESP_LOGI(TAG, "Load turned %s", load_on ? "ON" : "OFF");
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        if (!val->val.u8) {
            return ESP_OK;
        }
        load_level = val->val.u8;
        set_output();
        ESP_LOGI(TAG, "Load level set to %u/254", load_level);
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

static void factory_reset_confirm_timer_cb(void *arg)
{
    nvs_handle_t nvs;
    if (nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool check_factory_reset_boot_count(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS for boot-count tracking: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t boot_count = 0;
    nvs_get_u8(nvs, FACTORY_RESET_NVS_KEY, &boot_count);
    boot_count++;
    nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, boot_count);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Quick-power-cycle boot count: %u/%u", boot_count, FACTORY_RESET_BOOT_COUNT_THRESHOLD);

    if (boot_count >= FACTORY_RESET_BOOT_COUNT_THRESHOLD) {
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

    /* 1b. Quick-power-cycle factory reset check — the actual reset (if
     * due) only happens later, once Matter has started. */
    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the dimmable output via LEDC (hardware PWM). */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = MOUNTED_DIMMER_LEDC_MODE;
    ledc_timer.duty_resolution = MOUNTED_DIMMER_LEDC_DUTY_RES;
    ledc_timer.timer_num = MOUNTED_DIMMER_LEDC_TIMER;
    ledc_timer.freq_hz = MOUNTED_DIMMER_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = MOUNTED_DIMMER_PWM_GPIO;
    ledc_channel.speed_mode = MOUNTED_DIMMER_LEDC_MODE;
    ledc_channel.channel = MOUNTED_DIMMER_LEDC_CHANNEL;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = MOUNTED_DIMMER_LEDC_TIMER;
    ledc_channel.duty = 0;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
    set_output(); /* load_on starts false, so this drives duty 0 (fully off). */

    /* 2b. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Mounted Dimmable Load
     * Control endpoint (Identify + Groups + OnOff[Lighting] +
     * LevelControl[OnOff+Lighting] + ScenesManagement, all via the
     * complete top-level helper — see the header comment above). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::mounted_dimmable_load_control::config_t dimmer_config;
    dimmer_config.level_control.current_level = MOUNTED_DIMMER_DEFAULT_LEVEL;
    dimmer_config.level_control.on_level = nullable<uint8_t>(); /* see header comment on the OnLevel fix */
    dimmer_config.level_control_lighting.start_up_current_level = MOUNTED_DIMMER_DEFAULT_LEVEL;
    endpoint_t *endpoint = endpoint::mounted_dimmable_load_control::create(node, &dimmer_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create mounted dimmable load control endpoint");
        return;
    }
    dimmer_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Mounted dimmable load control endpoint id: %u", dimmer_endpoint_id);

    /* Occupancy Sensing (client) — optionalConform, see the header comment
     * above. */
    cluster::occupancy_sensing::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    /* CurrentLevel changes rapidly while a controller is actively dragging
     * a brightness slider — same deferred-NVS-persistence reasoning
     * firmware/dimmable-light/'s own header comment already documents. */
    attribute_t *current_level_attribute = attribute::get(dimmer_endpoint_id, LevelControl::Id,
                                                           LevelControl::Attributes::CurrentLevel::Id);
    attribute::set_deferred_persistence(current_level_attribute);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter mounted dimmable load control started. Scan the QR code to commission.");
}
