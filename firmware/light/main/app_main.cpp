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
#include <esp_timer.h>

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

    /* 3a. Occupancy Sensing (client) — optionalConform on OnOffLight.xml
     * (Matter Device Types Reference audit, see CLAUDE.md's own "Open next
     * steps" for detail), letting a controller bind this endpoint to a
     * real occupancy sensor's own endpoint. NULL config + CLUSTER_FLAG_
     * CLIENT: confirmed by reading occupancy_sensing::create() directly
     * that its CLIENT branch only calls create_default_binding_cluster()
     * — same shape firmware/switch/'s own Groups/Scenes client clusters
     * and firmware/doorbell/'s client Chime cluster already establish. No
     * app code reacts to the bound sensor's Occupancy attribute itself —
     * this only gives a controller (e.g. Home Assistant) the binding
     * surface to wire up its own automation; same "declared but not acted
     * on locally" honesty this repo already applies to e.g. EVSE's
     * ChargingPreferences. */
    cluster::occupancy_sensing::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    /* Level Control (server) is ALSO optionalConform on OnOffLight.xml —
     * confirmed directly in the XML that it carries the exact same
     * mandatory OO+LT features as firmware/dimmable-light/'s own device
     * type (0x0101). Deliberately NOT added here: enabling it would make
     * this endpoint functionally a dimmable light while still declaring
     * itself device type 0x0100 (On/Off Light) rather than 0x0101 — most
     * real controllers key their UI off the cluster list actually present,
     * not the declared device type, so this would just create a second,
     * misleadingly-labelled way to build what firmware/dimmable-light/
     * already builds correctly. Skipped as a deliberate product-scope
     * decision, not a technical limitation — see CLAUDE.md's own "Open
     * next steps" for the full reasoning. */

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
