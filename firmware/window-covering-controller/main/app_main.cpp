/*
 * Minimal Matter Window Covering Controller — sixty-second device type: a
 * physical three-button remote (Open / Close / Stop) that sends real
 * WindowCovering::UpOrOpen/DownOrClose/StopMotion commands to whatever
 * real covering a controller binds it to — the class of hardware sold as
 * a wall-mounted blinds/shutter remote, letting a household drive a real
 * Matter window covering (e.g. firmware/window-covering/ itself) without
 * reaching for a phone app.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: hand-assembled, since only the "generated" data model
 * this repo never enables has a top-level helper --------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * WindowCoveringController.xml (device type 0x0203, revision 4, confirmed
 * by reading the XML's own `id`/`revision` attributes directly): Window
 * Covering (client) is the ONLY `<mandatoryConform/>` cluster — Identify
 * (both server and client) is `<optionalConform/>`, and Groups (client)
 * is only conditionally mandatory under an "Active" condition this file
 * doesn't declare support for (so it stays `<optionalConform/>` in
 * practice here too) — same "smallest reasonable next step" scoping this
 * repo applies elsewhere. Unlike firmware/door-lock-controller/'s and
 * firmware/thermostat-controller/'s own device types (both genuinely
 * carry NO Identify cluster at all), this one DOES optionally allow a
 * server-side Identify — added here, giving this device a real physical
 * LED, the first of this session's own batch of "controller" device
 * types to have one.
 *
 * Confirmed via Docker `grep` that `window_covering_controller` only
 * exists under esp-matter's "generated" data model (`data_model/
 * generated/device_types/window_covering_controller_device/`) — the data
 * model this repo has never enabled, same situation firmware/
 * door-lock-controller/'s own header comment already documents for its
 * own device type. This endpoint is hand-assembled from lower-level free
 * functions instead, reusing that file's own hard-learned discipline:
 * `cluster::descriptor::create()` called explicitly right after
 * `endpoint::create()`, the literal device-type-ID/revision values
 * (0x0203, revision 4) passed directly to `add_device_type()`, and an
 * explicit `cluster::binding::create()` call (confirmed `cluster::
 * window_covering::create()` does NOT auto-create one, the same check
 * already applied to `cluster::door_lock::create()`/`cluster::on_off::
 * create()` elsewhere in this repo).
 *
 * --- Three commands, none timed, none with fields --------------------------
 * Confirmed by reading the WindowCovering cluster's own real spec XML
 * (`data_model/1.6/clusters/WindowCovering.xml`) directly: `UpOrOpen`
 * (0x00), `DownOrClose` (0x01), and `StopMotion` (0x02) are all
 * `<mandatoryConform/>`, carry no fields at all, and carry no `timed`
 * attribute on their own `<access>` tags — so all three are sent with an
 * empty `"{}"` payload and `chip::NullOptional` for the timeout, the same
 * shape firmware/switch/'s own buttons and firmware/on-off-sensor/'s own
 * On/Off commands already establish (unlike firmware/door-lock-
 * controller/'s own LockDoor/UnlockDoor, which genuinely are timed).
 * Three buttons matches how real wall-mounted blinds/shutter remotes are
 * actually built (an explicit Stop button, not just Open/Close — a real,
 * physically useful third state for a covering mid-travel), reusing
 * firmware/switch/'s own multi-button, one-shared-task debounce shape.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pushbutton hardware for this device type physically
 * available when written, and — like every client-invoke device in this
 * repo — verifying this one for real also needs a second, already-
 * commissioned bindable Window Covering device on the same fabric).
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

static const char *TAG = "matter_window_covering_controller";

/* Three momentary pushbuttons — active-LOW, internal pull-up. Reference
 * wiring: GND -> button -> GPIO, same convention this repo's other
 * buttons use. */
#define WCC_OPEN_BUTTON_GPIO GPIO_NUM_4
#define WCC_CLOSE_BUTTON_GPIO GPIO_NUM_16
#define WCC_STOP_BUTTON_GPIO GPIO_NUM_17

#define WCC_DEBOUNCE_SAMPLES 8

/* LED for the Matter "Identify" cluster (this device's OWN server-side
 * Identify — a controller asking THIS device to identify itself). */
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

static uint16_t wcc_endpoint_id = 0;
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

    if (req_handle->command_path.mClusterId != WindowCovering::Id ||
        (req_handle->command_path.mCommandId != WindowCovering::Commands::UpOrOpen::Id &&
         req_handle->command_path.mCommandId != WindowCovering::Commands::DownOrClose::Id &&
         req_handle->command_path.mCommandId != WindowCovering::Commands::StopMotion::Id)) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

static void send_bound_command(uint32_t command_id)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = WindowCovering::Id;
    req_handle.command_path.mCommandId = command_id;
    req_handle.request_data = NULL;

    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(wcc_endpoint_id, &req_handle);
}

/* One shared task debouncing all three buttons — same "one shared task, N
 * configured inputs" shape firmware/switch/'s and firmware/
 * door-lock-controller/'s own multi-button designs already establish. */
struct button_state_t {
    gpio_num_t gpio;
    bool debounced;
    int consistent;
    bool last_raw;
    uint32_t command_id;
    const char *name;
};

static void button_task(void *arg)
{
    button_state_t buttons[3] = {
        {WCC_OPEN_BUTTON_GPIO, false, 0, false, WindowCovering::Commands::UpOrOpen::Id, "Open"},
        {WCC_CLOSE_BUTTON_GPIO, false, 0, false, WindowCovering::Commands::DownOrClose::Id, "Close"},
        {WCC_STOP_BUTTON_GPIO, false, 0, false, WindowCovering::Commands::StopMotion::Id, "Stop"},
    };
    for (int i = 0; i < 3; i++) {
        buttons[i].last_raw = (gpio_get_level(buttons[i].gpio) == 0);
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        for (int i = 0; i < 3; i++) {
            button_state_t *b = &buttons[i];
            bool raw = (gpio_get_level(b->gpio) == 0); /* active-LOW */
            if (raw == b->last_raw) {
                if (b->consistent < WCC_DEBOUNCE_SAMPLES) {
                    b->consistent++;
                }
            } else {
                b->last_raw = raw;
                b->consistent = 0;
            }

            if (b->consistent == WCC_DEBOUNCE_SAMPLES && raw && !b->debounced) {
                b->debounced = true;
                ESP_LOGI(TAG, "%s button pressed — sending command to bound device(s)", b->name);
                send_bound_command(b->command_id);
            } else if (b->consistent == WCC_DEBOUNCE_SAMPLES && !raw && b->debounced) {
                b->debounced = false;
            }
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

    /* 2. Configure the three buttons — active-LOW, internal pull-up. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << WCC_OPEN_BUTTON_GPIO) | (1ULL << WCC_CLOSE_BUTTON_GPIO) |
                                   (1ULL << WCC_STOP_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "wcc_buttons", 4096, NULL, 5, NULL);

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

    /* 3. Build the Matter data model: one node, one Window Covering
     * Controller endpoint, hand-assembled since no legacy top-level
     * helper exists — see the header comment above for the full detail. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Window Covering Controller",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create window covering controller endpoint");
        return;
    }

    cluster::descriptor::config_t descriptor_config;
    cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);

    err = add_device_type(endpoint, 0x0203 /* Window Covering Controller */, 4 /* device type revision */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add window covering controller device type: %d", err);
        return;
    }

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    cluster::binding::config_t binding_config;
    cluster::binding::create(endpoint, &binding_config, CLUSTER_FLAG_SERVER);

    cluster::window_covering::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    wcc_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Window covering controller endpoint id: %u", wcc_endpoint_id);

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

    ESP_LOGI(TAG, "Matter window covering controller started. Scan the QR code to commission.");
}
