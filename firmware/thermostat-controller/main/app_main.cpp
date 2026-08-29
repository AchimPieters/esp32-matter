/*
 * Minimal Matter Thermostat Controller — sixty-first device type: a
 * physical rotary-knob remote that sends real Thermostat::
 * SetpointRaiseLower commands to whatever real thermostat a controller
 * binds it to — the class of hardware sold as a simple wall-mounted "turn
 * to adjust the temperature" remote, letting a household nudge a real
 * Matter thermostat's own setpoint (e.g. firmware/thermostat/ itself)
 * without knowing its current absolute value or reaching for a phone app.
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
 * ThermostatController.xml (device type 0x0111, confirmed by reading the
 * XML's own `id` attribute directly): Thermostat (client) is the ONLY
 * `<mandatoryConform/>` cluster — Identify (client), Groups (client), and
 * Scenes Management (client) are all `<optionalConform/>` and not
 * implemented, same "smallest reasonable next step" scoping this repo
 * applies elsewhere. A real, worth-noting spec detail: this device type
 * lists NO server-side Identify cluster at all, unlike firmware/dimmer-
 * switch/'s own DimmerSwitch (which mandates Identify server+client
 * together) — confirmed by reading `esp_matter_endpoint.cpp`'s own
 * `thermostat_controller::add()` directly: even though its `config_t` is
 * `app_client_config` (the same shared `{descriptor, identify, binding}`
 * struct `control_bridge`'s and `closure_controller`'s own config_t
 * types alias too, confirmed by reading the header directly), `add()`
 * itself never calls `identify::create()` at all — the `identify` field
 * on the config struct is simply unused for this specific device type,
 * matching the XML's own real cluster list rather than the shared
 * struct's own shape. Per this repo's own device-type-conformance
 * discipline (same reasoning firmware/temperature-controlled-cabinet/'s
 * and firmware/door-lock-controller/'s own header comments already
 * apply), this file has no Identify LED at all — the rotary encoder is
 * the entire physical interface. `endpoint::thermostat_controller::
 * create()` confirmed complete/ready-to-use by reading `esp_matter_
 * endpoint.cpp`'s own `create()`/`add()` pair directly: Descriptor (via
 * `common::create<T>()`) + Binding (added in `create()` itself, same
 * "Binding added separately from add()'s own cluster additions" detail
 * firmware/dimmer-switch/'s own header comment already documents) +
 * Thermostat[client] — zero manual cluster-creation code needed here at
 * all, the simplest client-side endpoint construction in this repo since
 * firmware/dimmer-switch/'s own.
 *
 * --- SetpointRaiseLower: both fields mandatory, no timed-invoke
 * requirement -------------------------------------------------------------
 * Confirmed by reading the Thermostat cluster's own real spec XML
 * (`data_model/1.6/clusters/Thermostat.xml`) directly: `SetpointRaiseLower`
 * (command 0x00) carries TWO fields, both `<mandatoryConform/>` — `Mode`
 * (a `SetpointRaiseLowerModeEnum`: Heat=0/Cool=1/Both=2/... — confirmed
 * against the same XML's own enum definition) and `Amount` (a signed
 * `int8`, in the cluster's own documented 0.1 degC units, the same
 * fractional-degree encoding firmware/thermostat/'s own setpoints
 * already use). Unlike firmware/door-lock-controller/'s own LockDoor/
 * UnlockDoor, this command's `<access>` tag carries no `timed="true"`
 * attribute — confirmed directly, so `chip::NullOptional` is correct
 * here, the same as every client-invoke command in this repo except
 * DoorLock's own. `Mode` is always sent as `kBoth` (2) — confirmed safe
 * against a Heat-only or Cool-only bound thermostat by reading the same
 * enum's own conform terms: `kBoth` only requires the HEAT-or-COOL
 * feature (an `orTerm`), satisfied by any real thermostat regardless of
 * which single mode it currently has active, so this controller never
 * needs to know or guess which mode the bound device is actually in —
 * the target's own cluster server sorts out which setpoint(s) a "Both"
 * request actually touches. `Amount` is +1 or -1 per detent (0.1 degC of
 * travel, a natural "one click, one small step" feel for a rotary knob),
 * reusing firmware/dimmer-switch/'s and firmware/thermostat/'s own
 * quadrature-decoding technique verbatim (channel A's falling edge as
 * trigger, channel B's level at that instant gives direction).
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no rotary encoder hardware for this device type
 * physically available when written, and — like every client-invoke
 * device in this repo — verifying this one for real also needs a second,
 * already-commissioned bindable Thermostat device on the same fabric).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_client.h>
#include <app-common/zap-generated/cluster-objects.h>

static const char *TAG = "matter_thermostat_controller";

/* Rotary encoder (A/B quadrature channels only — no integrated push-button
 * used, same shape firmware/dimmer-switch/'s own brightness encoder
 * already establishes). GPIO 16/17 are free, unreserved pins on classic
 * ESP32 (WROOM-32). */
#define ROTARY_ENCODER_A_GPIO GPIO_NUM_16
#define ROTARY_ENCODER_B_GPIO GPIO_NUM_17

/* 0.1 degC of setpoint travel per detent — see the header comment above
 * for the unit and why "Both" mode makes this safe regardless of which
 * single mode the bound thermostat is actually in. */
#define THERMOSTAT_CONTROLLER_STEP_AMOUNT 1

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t thermostat_controller_endpoint_id = 0;
static QueueHandle_t rotary_evt_queue = NULL;

static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "Command acknowledged by bound device");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Command failed: %" CHIP_ERROR_FORMAT, error.Format());
}

/* SetpointRaiseLower's own field IDs (`Mode`=0, `Amount`=1) confirmed
 * directly against the cluster's real spec XML — see the header comment
 * above. `request_data` carries the signed Amount (+1/-1) as a
 * sign-extended `intptr_t`. */
static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }

    if (req_handle->command_path.mClusterId != Thermostat::Id ||
        req_handle->command_path.mCommandId != Thermostat::Commands::SetpointRaiseLower::Id) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    int8_t amount = (int8_t)(intptr_t)req_handle->request_data;
    char command_data_json_str[64];
    snprintf(command_data_json_str, sizeof(command_data_json_str), "{\"0:U8\": %u, \"1:I8\": %d}",
             (unsigned)chip::to_underlying(Thermostat::SetpointRaiseLowerModeEnum::kBoth), (int)amount);

    /* Not timed — see the header comment above for why NullOptional is
     * correct here, unlike firmware/door-lock-controller/'s own LockDoor/
     * UnlockDoor. */
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, command_data_json_str,
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

/* Sends a real SetpointRaiseLower command to whatever this endpoint's
 * Binding cluster resolves to. */
static void send_bound_command(int8_t amount)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = Thermostat::Id;
    req_handle.command_path.mCommandId = Thermostat::Commands::SetpointRaiseLower::Id;
    req_handle.request_data = (void *)(intptr_t)amount;

    /* Stack lock required before calling into esp-matter's client APIs
     * from a plain FreeRTOS task, same as every other client-invoke
     * device in this repo. */
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(thermostat_controller_endpoint_id, &req_handle);
}

/* Standard quadrature decoding — reused verbatim from firmware/
 * dimmer-switch/'s and firmware/thermostat/'s own rotary encoder. Runs on
 * channel A's falling edge only; channel B's level at that instant gives
 * direction. */
static void IRAM_ATTR encoder_rotation_isr_handler(void *arg)
{
    static int64_t last_step_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_step_us < 2000) {
        return; /* debounce: ignore steps closer together than 2ms */
    }
    last_step_us = now;

    int direction = gpio_get_level(ROTARY_ENCODER_B_GPIO) ? 1 : -1;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(rotary_evt_queue, &direction, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void rotary_task(void *arg)
{
    int direction;
    for (;;) {
        if (xQueueReceive(rotary_evt_queue, &direction, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int8_t amount = (direction > 0) ? THERMOSTAT_CONTROLLER_STEP_AMOUNT : -THERMOSTAT_CONTROLLER_STEP_AMOUNT;
        ESP_LOGI(TAG, "Rotary encoder — sending SetpointRaiseLower (%+d) to bound device(s)", (int)amount);
        send_bound_command(amount);
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
 * its own XML directly and `thermostat_controller::add()`'s own body —
 * see the header comment above) — kept as a trivial stub, never actually
 * invoked. */
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

    /* 1b. Quick-power-cycle factory reset check — the actual reset (if
     * due) only happens later, once Matter has started. */
    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the rotary encoder's A/B channels + their shared
     * interrupt. */
    gpio_config_t encoder_a_conf = {};
    encoder_a_conf.pin_bit_mask = (1ULL << ROTARY_ENCODER_A_GPIO);
    encoder_a_conf.mode = GPIO_MODE_INPUT;
    encoder_a_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    encoder_a_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&encoder_a_conf);

    gpio_config_t encoder_b_conf = {};
    encoder_b_conf.pin_bit_mask = (1ULL << ROTARY_ENCODER_B_GPIO);
    encoder_b_conf.mode = GPIO_MODE_INPUT;
    encoder_b_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&encoder_b_conf);

    rotary_evt_queue = xQueueCreate(8, sizeof(int));
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    gpio_isr_handler_add(ROTARY_ENCODER_A_GPIO, encoder_rotation_isr_handler, NULL);
    xTaskCreate(rotary_task, "thermostat_controller_rotary", 4096, NULL, 5, NULL);

    /* 3. Build the Matter data model: one node, one Thermostat Controller
     * endpoint — Descriptor + Binding + Thermostat[client], all via the
     * complete top-level helper — see the header comment above for why no
     * manual cluster-creation code is needed at all. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Thermostat Controller",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::thermostat_controller::config_t thermostat_controller_config;
    endpoint_t *endpoint = endpoint::thermostat_controller::create(node, &thermostat_controller_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create thermostat controller endpoint");
        return;
    }
    thermostat_controller_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Thermostat controller endpoint id: %u", thermostat_controller_endpoint_id);

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

    /* Endpoint-agnostic by design (same reasoning as firmware/switch/'s
     * own single global registration) — this device only has the one
     * endpoint anyway. binding_manager_init() (which resolves bindings
     * set up via a controller's Binding cluster) runs on its own, inside
     * esp_matter::start() above — no explicit call needed here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);

    ESP_LOGI(TAG, "Matter thermostat controller started. Scan the QR code to commission.");
}
