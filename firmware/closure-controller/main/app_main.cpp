/*
 * Minimal Matter Closure Controller — sixty-third device type: a physical
 * two-button remote (Open / Close) that sends real ClosureControl::MoveTo
 * commands to whatever real closure a controller binds it to — the class
 * of hardware sold as a wall-mounted garage-door/roller-shutter/awning
 * remote, letting a household drive a real Matter Closure device (e.g.
 * firmware/closure/ itself) without reaching for a phone app.
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
 * ClosureController.xml (device type 0x023E, revision 1, confirmed by
 * reading the XML's own `id`/`revision` attributes directly): Closure
 * Control (client) is the ONLY `<mandatoryConform/>` cluster — Identify
 * (client) and Closure Dimension (client) are both `<optionalConform/>`
 * and not implemented, same "smallest reasonable next step" scoping this
 * repo applies elsewhere. This device type lists NO server-side Identify
 * cluster at all — confirmed by reading `esp_matter_endpoint.cpp`'s own
 * `closure_controller::add()` directly: even though its `config_t` is the
 * same shared `app_client_config` struct (`{descriptor, identify,
 * binding}`) firmware/thermostat-controller/'s own config_t already
 * aliases, `add()` never calls `identify::create()` — the `identify`
 * field is simply unused for this device type too, matching the XML's
 * own real cluster list. Per this repo's own device-type-conformance
 * discipline (same reasoning firmware/door-lock-controller/'s and
 * firmware/thermostat-controller/'s own header comments already apply),
 * this file has no Identify LED at all. `endpoint::closure_controller::
 * create()` confirmed complete/ready-to-use: Descriptor (via `common::
 * create<T>()`) + Binding (added in `create()` itself) + ClosureControl
 * [client] — zero manual cluster-creation code needed.
 *
 * --- MoveTo: a genuinely new command shape — three optional fields
 * forming an "at least one" choice, and a timed command like firmware/
 * door-lock-controller/'s own LockDoor/UnlockDoor ---------------------------
 * Confirmed by reading the ClosureControl cluster's own real spec XML
 * (`data_model/1.6/clusters/ClosureControl.xml`) directly: `MoveTo`
 * (command 0x01) carries `access ... timed="true"` — the same real Matter
 * protocol requirement (a TimedRequest action before the InvokeRequest)
 * firmware/door-lock-controller/'s own LockDoor/UnlockDoor already
 * established for this repo, reusing that same `CLOSURE_CONTROLLER_
 * TIMED_INVOKE_TIMEOUT_MS` (1000ms) reasoning. Its three fields
 * (`Position`, `Latch`, `Speed`) are each individually `<optionalConform
 * choice="a" more="true" min="1"/>` — a real "at least one of these three"
 * choice group, confirmed by reading the cluster's own `VALIDATE_
 * FEATURES_AT_LEAST_ONE`-style conform pattern this repo has already
 * catalogued for several other clusters (e.g. firmware/occupancy-sensor/'s
 * own sensing-modality features). This file only ever sends `Position`
 * (field id 0, a `TargetPositionEnum` — `MoveToFullyClosed`=0/
 * `MoveToFullyOpen`=1, confirmed against the same XML's own enum
 * definition), satisfying that "at least one" requirement on its own and
 * leaving `Latch`/`Speed` genuinely absent from the JSON payload — the
 * same "omit a field the choice-group doesn't require this call to
 * include" reasoning firmware/door-lock-controller/'s own optional
 * PINCode field already establishes, just for a choice group instead of
 * a single optional field.
 *
 * Two buttons (Open, Close) — ClosureControl has no single "toggle"
 * command, the same reasoning firmware/door-lock-controller/'s own header
 * comment already gives for its own two-button (not one-toggle-button)
 * design. Debounce reuses the same simple shared-task, N-configured-
 * inputs shape firmware/switch/'s own multi-button design already
 * establishes.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pushbutton hardware for this device type physically
 * available when written, and — like every client-invoke device in this
 * repo — verifying this one for real also needs a second, already-
 * commissioned bindable Closure device on the same fabric).
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

static const char *TAG = "matter_closure_controller";

/* Two momentary pushbuttons — active-LOW, internal pull-up. Reference
 * wiring: GND -> button -> GPIO, same convention this repo's other
 * buttons use. */
#define CLOSURE_CONTROLLER_OPEN_BUTTON_GPIO GPIO_NUM_4
#define CLOSURE_CONTROLLER_CLOSE_BUTTON_GPIO GPIO_NUM_16

#define CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES 8

/* See the header comment above for why this needs to be a real, non-null
 * timeout rather than chip::NullOptional. */
#define CLOSURE_CONTROLLER_TIMED_INVOKE_TIMEOUT_MS 1000

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t closure_controller_endpoint_id = 0;

static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "Command acknowledged by bound device");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Command failed: %" CHIP_ERROR_FORMAT, error.Format());
}

/* `request_data` carries the target TargetPositionEnum value (0 =
 * MoveToFullyClosed, 1 = MoveToFullyOpen). */
static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }

    if (req_handle->command_path.mClusterId != ClosureControl::Id ||
        req_handle->command_path.mCommandId != ClosureControl::Commands::MoveTo::Id) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    uint8_t position = (uint8_t)(uintptr_t)req_handle->request_data;
    char command_data_json_str[48];
    snprintf(command_data_json_str, sizeof(command_data_json_str), "{\"0:U8\": %u}", (unsigned)position);

    /* Timed — see the header comment above for why. */
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, command_data_json_str,
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::Optional<uint16_t>(CLOSURE_CONTROLLER_TIMED_INVOKE_TIMEOUT_MS));
}

static void send_bound_command(uint8_t position)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = ClosureControl::Id;
    req_handle.command_path.mCommandId = ClosureControl::Commands::MoveTo::Id;
    req_handle.request_data = (void *)(uintptr_t)position;

    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(closure_controller_endpoint_id, &req_handle);
}

/* One shared task debouncing both buttons — same "one shared task, N
 * configured inputs" shape firmware/switch/'s and firmware/
 * door-lock-controller/'s own multi-button designs already establish. */
static void button_task(void *arg)
{
    bool open_debounced = false;
    int open_consistent = 0;
    bool open_last_raw = (gpio_get_level(CLOSURE_CONTROLLER_OPEN_BUTTON_GPIO) == 0);

    bool close_debounced = false;
    int close_consistent = 0;
    bool close_last_raw = (gpio_get_level(CLOSURE_CONTROLLER_CLOSE_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        bool open_raw = (gpio_get_level(CLOSURE_CONTROLLER_OPEN_BUTTON_GPIO) == 0);
        if (open_raw == open_last_raw) {
            if (open_consistent < CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES) {
                open_consistent++;
            }
        } else {
            open_last_raw = open_raw;
            open_consistent = 0;
        }
        if (open_consistent == CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES && open_raw && !open_debounced) {
            open_debounced = true;
            ESP_LOGI(TAG, "Open button pressed — sending MoveTo(FullyOpen) to bound device(s)");
            send_bound_command((uint8_t)ClosureControl::TargetPositionEnum::kMoveToFullyOpen);
        } else if (open_consistent == CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES && !open_raw && open_debounced) {
            open_debounced = false;
        }

        bool close_raw = (gpio_get_level(CLOSURE_CONTROLLER_CLOSE_BUTTON_GPIO) == 0);
        if (close_raw == close_last_raw) {
            if (close_consistent < CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES) {
                close_consistent++;
            }
        } else {
            close_last_raw = close_raw;
            close_consistent = 0;
        }
        if (close_consistent == CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES && close_raw && !close_debounced) {
            close_debounced = true;
            ESP_LOGI(TAG, "Close button pressed — sending MoveTo(FullyClosed) to bound device(s)");
            send_bound_command((uint8_t)ClosureControl::TargetPositionEnum::kMoveToFullyClosed);
        } else if (close_consistent == CLOSURE_CONTROLLER_DEBOUNCE_SAMPLES && !close_raw && close_debounced) {
            close_debounced = false;
        }
    }
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

/* This device type has no Identify cluster at all (confirmed by reading
 * its own XML directly and `closure_controller::add()`'s own body — see
 * the header comment above) — kept as a trivial stub, never invoked. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
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

    /* 2. Configure both buttons — active-LOW, internal pull-up. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << CLOSURE_CONTROLLER_OPEN_BUTTON_GPIO) |
                                   (1ULL << CLOSURE_CONTROLLER_CLOSE_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "closure_controller_buttons", 4096, NULL, 5, NULL);

    /* 3. Build the Matter data model: one node, one Closure Controller
     * endpoint — Descriptor + Binding + ClosureControl[client], all via
     * the complete top-level helper — see the header comment above for
     * why no manual cluster-creation code is needed at all. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Closure Controller",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::closure_controller::config_t closure_controller_config;
    endpoint_t *endpoint = endpoint::closure_controller::create(node, &closure_controller_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create closure controller endpoint");
        return;
    }
    closure_controller_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Closure controller endpoint id: %u", closure_controller_endpoint_id);

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

    ESP_LOGI(TAG, "Matter closure controller started. Scan the QR code to commission.");
}
