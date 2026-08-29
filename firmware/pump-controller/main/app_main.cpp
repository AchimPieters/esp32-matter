/*
 * Minimal Matter Pump Controller — sixty-fourth device type: a physical
 * on/off remote panel that sends a real OnOff::Toggle command to whatever
 * real pump a controller binds it to, plus a real, present — but locally
 * unused — PumpConfigurationAndControl client shell a controller's own
 * generic UI can use once bound. The class of hardware sold as a wall-
 * mounted "pump on/off" remote for a pool/irrigation/circulation pump,
 * letting a household control a real Matter pump (e.g. firmware/pump/
 * itself) without reaching for a phone app.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: a complete top-level helper, confirmed against the
 * CSA's own XML ------------------------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * PumpController.xml (device type 0x0304, revision 4, confirmed by
 * reading the XML's own `id`/`revision` attributes directly): Identify
 * (server), On/Off (client), and PumpConfigurationAndControl (client) are
 * all `<mandatoryConform/>` — Identify (client), Groups (client), Level
 * Control (client), Scenes Management (client), Temperature/Pressure/
 * Flow Measurement (all client) are `<optionalConform/>` and not
 * implemented, same "smallest reasonable next step" scoping this repo
 * applies elsewhere. Unlike firmware/door-lock-controller/'s, firmware/
 * thermostat-controller/'s, and firmware/closure-controller/'s own device
 * types (all genuinely carry NO server Identify at all), this one
 * mandates one — confirmed by reading `esp_matter_endpoint.cpp`'s own
 * `pump_controller::add()` directly: it calls `identify::create(endpoint,
 * &(config->identify), CLUSTER_FLAG_SERVER)`, giving this device a real
 * physical LED. `pump_controller::config_t`'s own constructor is also
 * confirmed to set `identify.identify_type = kVisibleIndicator` itself
 * (not `kActuator`, the type firmware/pump/'s own Identify config sets
 * explicitly at its call site) — a real, meaningful distinction this
 * repo hadn't hit before: this device is a remote CONTROL panel, not a
 * physical actuator, so "visible indicator" is the honest Identify type
 * for what this device's own LED actually represents. `endpoint::
 * pump_controller::create()` confirmed complete/ready-to-use: Descriptor
 * (via `common::create<T>()`) + Identify[server, with `identify_type`
 * already set by the config's own constructor] + OnOff[client] +
 * PumpConfigurationAndControl[client] + Binding[server, added in
 * `add()` itself this time, not `create()` — a real, worth-noting
 * ordering difference from every other client-invoke device type in this
 * repo, confirmed by reading `pump_controller::add()`'s own body
 * directly] — zero manual cluster-creation code needed.
 *
 * --- One button, one real command; PumpConfigurationAndControl stays a
 * declared shell, same "don't invent busy-work" precedent this repo
 * already applies elsewhere ------------------------------------------------
 * PumpConfigurationAndControl is confirmed, by reading the cluster's own
 * real spec XML, to be mostly attribute-based (OperationMode is a plain
 * writable attribute, not a command) rather than command-based the way
 * OnOff/LevelControl are — a real controller would drive it through its
 * own generic attribute-write UI once bound, not through a command this
 * repo's existing `client::interaction::invoke::send_request()` plumbing
 * (built for commands, not remote attribute writes) can reach without a
 * genuinely new mechanism this file doesn't need to invent for a single
 * physical button. Same "declare the shell, don't invent busy-work"
 * precedent firmware/switch/'s own unused Groups/Scenes client shells and
 * firmware/on-off-sensor/'s own unused Identify client shell already
 * establish — this device's single button only ever sends OnOff::Toggle
 * (empty payload, not timed, the same shape firmware/switch/'s own
 * buttons already use), and a real controller can still use the
 * PumpConfigurationAndControl binding surface via its own UI once bound.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pushbutton hardware for this device type physically
 * available when written, and — like every client-invoke device in this
 * repo — verifying this one for real also needs a second, already-
 * commissioned bindable Pump device on the same fabric).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_client.h>
#include <app-common/zap-generated/cluster-objects.h>

static const char *TAG = "matter_pump_controller";

/* Momentary pushbutton — active-LOW, internal pull-up. Reference wiring:
 * GND -> button -> GPIO, same convention this repo's other buttons use. */
#define PUMP_CONTROLLER_BUTTON_GPIO GPIO_NUM_4

#define PUMP_CONTROLLER_DEBOUNCE_SAMPLES 8

/* LED for the Matter "Identify" cluster (this device's OWN mandatory
 * server-side Identify). */
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

static uint16_t pump_controller_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "Command acknowledged by bound device");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Command failed: %" CHIP_ERROR_FORMAT, error.Format());
}

static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }

    if (req_handle->command_path.mClusterId != OnOff::Id ||
        req_handle->command_path.mCommandId != OnOff::Commands::Toggle::Id) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

static void send_bound_toggle(void)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = OnOff::Id;
    req_handle.command_path.mCommandId = OnOff::Commands::Toggle::Id;
    req_handle.request_data = NULL;

    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(pump_controller_endpoint_id, &req_handle);
}

static void button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(PUMP_CONTROLLER_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        bool raw_pressed = (gpio_get_level(PUMP_CONTROLLER_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < PUMP_CONTROLLER_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == PUMP_CONTROLLER_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            ESP_LOGI(TAG, "Button pressed — sending Toggle to bound device(s)");
            send_bound_toggle();
        } else if (consistent_samples == PUMP_CONTROLLER_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
        }
    }
}

static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

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

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

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

    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the button — active-LOW, internal pull-up. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << PUMP_CONTROLLER_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "pump_controller_button", 4096, NULL, 5, NULL);

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

    /* 3. Build the Matter data model: one node, one Pump Controller
     * endpoint — Descriptor + Identify[server] + OnOff[client] +
     * PumpConfigurationAndControl[client] + Binding, all via the complete
     * top-level helper — see the header comment above for why no manual
     * cluster-creation code is needed at all. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Pump Controller",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::pump_controller::config_t pump_controller_config;
    endpoint_t *endpoint = endpoint::pump_controller::create(node, &pump_controller_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create pump controller endpoint");
        return;
    }
    pump_controller_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Pump controller endpoint id: %u", pump_controller_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset();
        return;
    }

    client::set_request_callback(app_client_request_cb, NULL, NULL);

    ESP_LOGI(TAG, "Matter pump controller started. Scan the QR code to commission.");
}
