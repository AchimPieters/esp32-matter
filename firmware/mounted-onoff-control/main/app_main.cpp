/*
 * Minimal Matter Mounted On/Off Control — fifty-third device type, and the
 * real-world product this maps onto: an in-wall relay module (the class of
 * hardware sold as e.g. "Shelly 1"/"Sonoff Basic") wired directly into an
 * existing fixture's own switch-leg, replacing the fixture's own dumb wall
 * switch rather than being a smart bulb or a plug-in outlet.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Why a separate device type from firmware/light/ and firmware/outlet/ -
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * MountedOnOffControl.xml (device type 0x010F, revision 2, explicitly
 * classified as `superset="On/Off Plug-in Unit"`): Identify (with
 * TriggerEffect) + Groups + On/Off[Lighting] + Scenes Management (with
 * CopyScene) are ALL mandatoryConform — the real, meaningful difference
 * from firmware/light/'s own OnOffLight (0x0100): Groups and Scenes
 * Management are mandatory here, not simply absent the way firmware/
 * light/'s own device type doesn't even list them. `endpoint::
 * mounted_on_off_control::create()` confirmed complete/ready-to-use by
 * reading esp-matter's own legacy `mounted_on_off_control::add()`
 * directly: it wires up Identify (with TriggerEffect) + Groups + OnOff
 * [Lighting, On+Toggle commands] + ScenesManagement [with CopyScene/
 * CopyScene-response], all from one `config_t` (extending the exact same
 * `on_off_with_lighting_config` base firmware/light/'s own `on_off_light::
 * config_t` uses) — zero manual cluster-creation code needed here at all,
 * the simplest endpoint construction this repo has built in a while.
 *
 * --- Output: a relay, not an LED — a real, deliberate framing difference
 * from firmware/light/ ------------------------------------------------------
 * firmware/light/'s own LED represents a smart bulb's own light-emitting
 * element — the bulb IS the load. This device type instead represents a
 * wall-mounted CONTROL module wired into an existing, separate, non-smart
 * fixture's own switch-leg — the module gates power to a load it doesn't
 * itself emit light from. `MOUNTED_ONOFF_RELAY_GPIO` (active-LOW, matching
 * firmware/outlet/'s own relay convention) reflects that: a real relay
 * module switching an existing ceiling light, fan, or other fixed AC load,
 * not a bulb driven directly off this board. No local physical button —
 * same "purely remote-controlled, Identify LED is the only local hardware
 * beyond the load output" shape firmware/light/'s own device already
 * establishes; a real product in this category commonly also senses an
 * existing physical wall switch's own contact closure to keep local control
 * working, but that's a genuine hardware variant this file doesn't attempt
 * (same "smallest reasonable next step" scope cut this repo applies
 * throughout).
 *
 * --- Occupancy Sensing (client): optionalConform, same NULL-config shell
 * pattern already established five times over -----------------------------
 * Confirmed directly against MountedOnOffControl.xml: Occupancy Sensing
 * (client) is `<optionalConform/>` — added via the identical `cluster::
 * occupancy_sensing::create(endpoint, NULL, CLUSTER_FLAG_CLIENT)` shape
 * firmware/light/'s, firmware/outlet/'s, firmware/dimmable-light/'s,
 * firmware/color-light/'s, firmware/addressable-light/'s, and firmware/
 * color-temperature-light/'s own identical additions already establish —
 * lets a controller bind this endpoint to a real occupancy sensor's own
 * endpoint; no app code reacts to it locally, same "declared but not acted
 * on locally" honesty this repo already applies elsewhere.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay module physically available for this specific
 * device type when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <esp_matter.h>

static const char *TAG = "matter_mounted_onoff_control";

/* Change this to the GPIO your relay module's control input is wired to.
 * Active-LOW, matching firmware/outlet/'s own relay convention — always
 * check your specific relay module, polarity isn't universal. */
#define MOUNTED_ONOFF_RELAY_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t mounted_onoff_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

static void set_relay(bool on)
{
    gpio_set_level(MOUNTED_ONOFF_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
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
 * relay. We react on PRE_UPDATE so the relay follows the requested state. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && endpoint_id == mounted_onoff_endpoint_id &&
        cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        set_relay(val->val.b);
        ESP_LOGI(TAG, "Load turned %s", val->val.b ? "ON" : "OFF");
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

    /* 2. Configure the relay output — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << MOUNTED_ONOFF_RELAY_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    set_relay(false);

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

    /* 3. Build the Matter data model: one node, one Mounted On/Off Control
     * endpoint (Identify + Groups + OnOff[Lighting] + ScenesManagement, all
     * via the complete top-level helper — see the header comment above). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::mounted_on_off_control::config_t control_config;
    endpoint_t *endpoint = endpoint::mounted_on_off_control::create(node, &control_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create mounted on/off control endpoint");
        return;
    }
    mounted_onoff_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Mounted on/off control endpoint id: %u", mounted_onoff_endpoint_id);

    /* Occupancy Sensing (client) — optionalConform, see the header comment
     * above. */
    cluster::occupancy_sensing::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

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

    ESP_LOGI(TAG, "Matter mounted on/off control started. Scan the QR code to commission.");
}
