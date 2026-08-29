/*
 * Minimal Matter Meter Reference Point — fifty-sixth device type, and the
 * simplest device type in this whole repo: exactly one cluster (Identify),
 * plus this repo's second real root-endpoint addition (TimeSynchronization,
 * reusing firmware/electrical-utility-meter/'s own newly-established
 * pattern). The real-world product this maps onto: a bare marker endpoint
 * tagging a specific, named point within a larger electrical installation's
 * own metering topology (e.g. "this is the point where solar ties into the
 * grid," or "this is the sub-panel feeding the garage") — genuinely no
 * measurement of its own, purely a logical reference point a controller can
 * see and label.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: confirmed via direct source reading, not assumed too
 * thin to bother with -------------------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * MeterReferencePoint.xml (device type 0x0512, revision 1): `<clusters>`
 * lists exactly one entry, Identify (mandatoryConform) — genuinely the
 * device type's entire cluster surface. Its own `<conditions>` block
 * declares a named condition, "ElectricalEnergy," but that condition is
 * never actually referenced by any conditional clause anywhere in this same
 * file (confirmed by reading the whole file directly) — a declared-but-
 * unused condition, likely meant only for downstream composition into a
 * larger topology this repo doesn't model. Its own `<conditionRequirements>`
 * block makes `TimeSyncCond` mandatoryConform on the Root Node, the exact
 * same requirement firmware/electrical-utility-meter/'s own device type
 * already satisfies — confirmed the same fix applies here directly, with no
 * new research needed. No top-level `endpoint::meter_reference_point::
 * create()` helper exists (confirmed via Docker `grep`) — same niche-
 * device-type gap firmware/doorbell/'s, firmware/chime/'s, and firmware/
 * on-off-sensor/'s own device types already hit — hand-assembled here from
 * the same lower-level free functions those files already establish:
 * `endpoint::create()` + `cluster::descriptor::create()` (always first,
 * per the hard-learned Descriptor-cluster lesson those files' own header
 * comments document in full) + `add_device_type(endpoint, 0x0512, 1)`
 * (literal hex values, same pattern firmware/on-off-sensor/'s own header
 * comment already establishes for a device type with no header constant) +
 * a single `cluster::identify::create()` call.
 *
 * --- TimeSyncCond: the exact same root-endpoint pattern firmware/
 * electrical-utility-meter/ already established --------------------------
 * `esp_matter::endpoint::get(node, 0)` fetches the root endpoint (id 0,
 * created internally by `node::create()`) so `cluster::time_
 * synchronization::create()` can be added onto it directly — same call,
 * same null-delegate defaults (`UtcTime` stays nullable-null,
 * `Granularity` stays 0/`NoTimeGranularity`, both real, spec-legal values
 * honestly representing "no trusted time source wired up") as firmware/
 * electrical-utility-meter/'s own identical addition.
 *
 * --- Hardware: an Identify LED, and genuinely nothing else -----------------
 * With no measurement cluster at all, there is no sensor or actuator this
 * firmware could honestly claim — the Identify LED (this repo's standard
 * "find this physical device" convenience) is the only hardware this
 * device type needs or gets. This is the smallest, simplest firmware in
 * this entire repository.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (nothing beyond a plain LED is needed for this device
 * type — this repo's own standard identify-LED wiring is already confirmed
 * working on real hardware many times over).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <esp_matter.h>

static const char *TAG = "matter_meter_reference_point";

/* LED for the Matter "Identify" cluster — the only hardware this device
 * type has. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
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

static esp_timer_handle_t identify_led_timer = NULL;

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

/* No controller-writable attributes on this device at all. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
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

    /* 2. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Meter Reference Point
     * endpoint (hand-assembled — see the header comment above for why no
     * top-level helper exists). */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Meter Reference Point",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    /* 3a. TimeSyncCond — satisfied the same way firmware/electrical-
     * utility-meter/'s own identical addition already establishes: adding
     * TimeSynchronization onto the ROOT endpoint (id 0). */
    endpoint_t *root_endpoint = endpoint::get(node, 0);
    if (!root_endpoint) {
        ESP_LOGE(TAG, "Failed to find root endpoint");
        return;
    }
    cluster::time_synchronization::config_t time_sync_config;
    cluster::time_synchronization::create(root_endpoint, &time_sync_config, CLUSTER_FLAG_SERVER);

    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create meter reference point endpoint");
        return;
    }

    cluster::descriptor::config_t descriptor_config;
    cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);

    err = add_device_type(endpoint, 0x0512 /* Meter Reference Point */, 1 /* device type revision */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add meter reference point device type: %d", err);
        return;
    }

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    ESP_LOGI(TAG, "Meter reference point endpoint id: %u", endpoint::get_id(endpoint));

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

    ESP_LOGI(TAG, "Matter meter reference point started. Scan the QR code to commission.");
}
