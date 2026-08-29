/*
 * Minimal Matter Control Bridge — sixty-fifth device type, and the last of
 * this session's own "controller" device-type batch: a universal light
 * remote, reusing firmware/color-dimmer-switch/'s own On/Off + LevelControl
 * + ColorControl button/rotary-encoder interaction verbatim, plus a real,
 * previously-undocumented esp-matter gap fixed onto the same endpoint —
 * Scenes Management (client), genuinely mandatoryConform on this device
 * type but never created by esp-matter's own top-level helper.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: a complete top-level helper, with one real, checked
 * gap — Scenes Management is mandatory but never created --------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * ControlBridge.xml (device type 0x0840, revision 3, confirmed by reading
 * the XML's own `id`/`revision` attributes directly): Identify (server
 * AND client), Groups (client), On/Off (client), Level Control (client),
 * Scenes Management (client), and Color Control (client) are ALL
 * `<mandatoryConform/>` — a real, stricter requirement than firmware/
 * dimmer-switch/'s and firmware/color-dimmer-switch/'s own device types
 * (both leave Groups/Scenes Management merely `<optionalConform/>`, and
 * both skip them for that reason). Illuminance Measurement and Occupancy
 * Sensing (both client) are `<optionalConform/>` and not implemented,
 * same "smallest reasonable next step" scoping this repo applies
 * elsewhere — a real controller could bind either as informational
 * context (an ambient-light or occupancy sensor feeding this device's own
 * automation logic), but this file has no local logic that would react to
 * either, so both stay undeclared rather than adding an unused shell with
 * nothing behind it.
 *
 * `endpoint::control_bridge::create()` confirmed by reading
 * `esp_matter_endpoint.cpp`'s own `create()`/`add()` pair directly:
 * Descriptor (via `common::create<T>()`) + Binding (added in `create()`
 * itself) + Identify[server+client] + Groups[client] + OnOff[client] +
 * LevelControl[client] + ColorControl[client] — but `add()`'s own body,
 * read line by line against the XML's own cluster list, is missing
 * `scenes_management::create(endpoint, NULL, CLUSTER_FLAG_CLIENT)`
 * entirely, despite Scenes Management being genuinely `<mandatoryConform/>`
 * here — a real, previously-undocumented gap in this specific top-level
 * helper (distinct from firmware/refrigerator/'s/firmware/water-heater/'s
 * own kind of gap, where the missing cluster was merely optionalConform
 * and skipping it was a deliberate scope cut, not a spec violation this
 * file needs to correct). Fixed the same "add extra clusters onto an
 * already-correct endpoint" way this repo has fixed every comparable gap
 * before — `cluster::scenes_management::create(endpoint, NULL,
 * CLUSTER_FLAG_CLIENT)` called manually right after `endpoint::
 * control_bridge::create()` returns, a shell only (same "declare the
 * shell, don't invent busy-work" precedent firmware/switch/'s own unused
 * Groups/Scenes client shells already establish — no local gesture on
 * this device maps naturally onto "recall a scene," the same reasoning
 * that file's own header comment already gives).
 *
 * --- Four real gestures, reused verbatim from firmware/color-dimmer-
 * switch/ ---------------------------------------------------------------
 * The on/off button (short press = OnOff::Toggle, long press =
 * Identify::Identify), the brightness rotary encoder (LevelControl::Step),
 * and the hue rotary encoder (ColorControl::StepHue, including its own
 * genuinely different StepModeEnum values and non-nullable TransitionTime
 * field) are all reused byte-for-byte from firmware/color-dimmer-switch/'s
 * own file — see that file's own header comment for the complete field-ID
 * sourcing and the real StepMode-convention difference between
 * LevelControl and ColorControl this repo already caught and documented
 * there. Nothing about this device's own broader cluster set (Groups/
 * Scenes Management/Illuminance/Occupancy) changes any of that existing,
 * already-verified command-payload logic.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no rotary-encoder/pushbutton hardware for this device
 * type physically available when written, and — like every client-invoke
 * device in this repo — verifying this one for real also needs a second,
 * already-commissioned bindable color light on the same fabric).
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

static const char *TAG = "matter_control_bridge";

/* Momentary pushbutton — active-LOW, internal pull-up. Short press sends
 * OnOff::Toggle; long press sends Identify::Identify. Same convention
 * firmware/color-dimmer-switch/'s own button already uses. */
#define CONTROL_BRIDGE_BUTTON_GPIO GPIO_NUM_4

/* Brightness rotary encoder — same GPIOs/behavior as firmware/
 * color-dimmer-switch/'s own (LevelControl::Step per detent). */
#define BRIGHTNESS_ENCODER_A_GPIO GPIO_NUM_16
#define BRIGHTNESS_ENCODER_B_GPIO GPIO_NUM_17
#define BRIGHTNESS_STEP_SIZE 25 /* out of LevelControl's own 0-254 range, ~10% per detent */

/* Hue rotary encoder — a second, independent encoder for ColorControl::
 * StepHue, same GPIOs/behavior as firmware/color-dimmer-switch/'s own. */
#define HUE_ENCODER_A_GPIO GPIO_NUM_18
#define HUE_ENCODER_B_GPIO GPIO_NUM_19
#define HUE_STEP_SIZE 8 /* out of Hue's own 0-254 range (~360 degrees), a modest per-detent shift */
#define HUE_STEP_TRANSITION_TIME 2 /* tenths of a second — a quick, not-instant color change */

/* LED for the Matter "Identify" cluster (this device's OWN server-side
 * Identify). */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Press-timing constants — same values/reasoning firmware/generic-
 * switch/'s own header comment already documents in full. */
#define CONTROL_BRIDGE_LONG_PRESS_MS 1000
#define CONTROL_BRIDGE_DEBOUNCE_SAMPLES 8

/* How long the bound target's own identify effect should run for, in
 * seconds, when this device's long press sends Identify::Identify. */
#define CONTROL_BRIDGE_IDENTIFY_TIME_SEC 5

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t control_bridge_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static QueueHandle_t brightness_evt_queue = NULL;
static QueueHandle_t hue_evt_queue = NULL;

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

    char command_data_json_str[128];

    if (req_handle->command_path.mClusterId == OnOff::Id &&
        req_handle->command_path.mCommandId == OnOff::Commands::Toggle::Id) {
        strcpy(command_data_json_str, "{}");
    } else if (req_handle->command_path.mClusterId == LevelControl::Id &&
               req_handle->command_path.mCommandId == LevelControl::Commands::Step::Id) {
        uint8_t step_mode = (uint8_t)(uintptr_t)req_handle->request_data;
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U8\": %u, \"1:U8\": %u, \"2:NULL\": null, \"3:U8\": 0, \"4:U8\": 0}",
                 step_mode, (unsigned)BRIGHTNESS_STEP_SIZE);
    } else if (req_handle->command_path.mClusterId == ColorControl::Id &&
               req_handle->command_path.mCommandId == ColorControl::Commands::StepHue::Id) {
        uint8_t step_mode = (uint8_t)(uintptr_t)req_handle->request_data;
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U8\": %u, \"1:U8\": %u, \"2:U8\": %u, \"3:U8\": 0, \"4:U8\": 0}",
                 step_mode, (unsigned)HUE_STEP_SIZE, (unsigned)HUE_STEP_TRANSITION_TIME);
    } else if (req_handle->command_path.mClusterId == Identify::Id &&
               req_handle->command_path.mCommandId == Identify::Commands::Identify::Id) {
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U16\": %u}", (unsigned)CONTROL_BRIDGE_IDENTIFY_TIME_SEC);
    } else {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, command_data_json_str,
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

static void send_bound_command(uint32_t cluster_id, uint32_t command_id, void *request_data)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = cluster_id;
    req_handle.command_path.mCommandId = command_id;
    req_handle.request_data = request_data;

    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(control_bridge_endpoint_id, &req_handle);
}

static void button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(CONTROL_BRIDGE_BUTTON_GPIO) == 0);
    int64_t press_start_ms = 0;
    bool long_press_fired = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int64_t now_ms = esp_timer_get_time() / 1000;

        bool raw_pressed = (gpio_get_level(CONTROL_BRIDGE_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < CONTROL_BRIDGE_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == CONTROL_BRIDGE_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            press_start_ms = now_ms;
            long_press_fired = false;
        }

        if (debounced_pressed && !long_press_fired && (now_ms - press_start_ms) >= CONTROL_BRIDGE_LONG_PRESS_MS) {
            long_press_fired = true;
            ESP_LOGI(TAG, "Long press — sending Identify to bound device(s)");
            send_bound_command(Identify::Id, Identify::Commands::Identify::Id, NULL);
        }

        if (consistent_samples == CONTROL_BRIDGE_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
            if (!long_press_fired) {
                ESP_LOGI(TAG, "Short press — sending Toggle to bound device(s)");
                send_bound_command(OnOff::Id, OnOff::Commands::Toggle::Id, NULL);
            }
        }
    }
}

static void IRAM_ATTR brightness_rotation_isr_handler(void *arg)
{
    static int64_t last_step_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_step_us < 2000) {
        return;
    }
    last_step_us = now;

    int direction = gpio_get_level(BRIGHTNESS_ENCODER_B_GPIO) ? 1 : -1;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(brightness_evt_queue, &direction, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR hue_rotation_isr_handler(void *arg)
{
    static int64_t last_step_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_step_us < 2000) {
        return;
    }
    last_step_us = now;

    int direction = gpio_get_level(HUE_ENCODER_B_GPIO) ? 1 : -1;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(hue_evt_queue, &direction, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void brightness_task(void *arg)
{
    int direction;
    for (;;) {
        if (xQueueReceive(brightness_evt_queue, &direction, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t step_mode = (direction > 0)
            ? chip::to_underlying(LevelControl::StepModeEnum::kUp)
            : chip::to_underlying(LevelControl::StepModeEnum::kDown);
        ESP_LOGI(TAG, "Brightness encoder — sending LevelControl::Step (%s) to bound device(s)",
                 direction > 0 ? "Up" : "Down");
        send_bound_command(LevelControl::Id, LevelControl::Commands::Step::Id, (void *)(uintptr_t)step_mode);
    }
}

static void hue_task(void *arg)
{
    int direction;
    for (;;) {
        if (xQueueReceive(hue_evt_queue, &direction, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        uint8_t step_mode = (direction > 0)
            ? chip::to_underlying(ColorControl::StepModeEnum::kUp)
            : chip::to_underlying(ColorControl::StepModeEnum::kDown);
        ESP_LOGI(TAG, "Hue encoder — sending ColorControl::StepHue (%s) to bound device(s)",
                 direction > 0 ? "Up" : "Down");
        send_bound_command(ColorControl::Id, ColorControl::Commands::StepHue::Id, (void *)(uintptr_t)step_mode);
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

    /* 2. Configure the on/off button — active-LOW, internal pull-up. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << CONTROL_BRIDGE_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "control_bridge_button", 4096, NULL, 5, NULL);

    /* 2b. Configure both rotary encoders' A/B channels + their shared
     * interrupt service. */
    gpio_config_t brightness_a_conf = {};
    brightness_a_conf.pin_bit_mask = (1ULL << BRIGHTNESS_ENCODER_A_GPIO);
    brightness_a_conf.mode = GPIO_MODE_INPUT;
    brightness_a_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    brightness_a_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&brightness_a_conf);

    gpio_config_t brightness_b_conf = {};
    brightness_b_conf.pin_bit_mask = (1ULL << BRIGHTNESS_ENCODER_B_GPIO);
    brightness_b_conf.mode = GPIO_MODE_INPUT;
    brightness_b_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&brightness_b_conf);

    gpio_config_t hue_a_conf = {};
    hue_a_conf.pin_bit_mask = (1ULL << HUE_ENCODER_A_GPIO);
    hue_a_conf.mode = GPIO_MODE_INPUT;
    hue_a_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    hue_a_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&hue_a_conf);

    gpio_config_t hue_b_conf = {};
    hue_b_conf.pin_bit_mask = (1ULL << HUE_ENCODER_B_GPIO);
    hue_b_conf.mode = GPIO_MODE_INPUT;
    hue_b_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&hue_b_conf);

    brightness_evt_queue = xQueueCreate(8, sizeof(int));
    hue_evt_queue = xQueueCreate(8, sizeof(int));
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    gpio_isr_handler_add(BRIGHTNESS_ENCODER_A_GPIO, brightness_rotation_isr_handler, NULL);
    gpio_isr_handler_add(HUE_ENCODER_A_GPIO, hue_rotation_isr_handler, NULL);
    xTaskCreate(brightness_task, "cb_brightness_task", 4096, NULL, 5, NULL);
    xTaskCreate(hue_task, "cb_hue_task", 4096, NULL, 5, NULL);

    /* 2c. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Control Bridge
     * endpoint — Descriptor + Binding + Identify[server+client] +
     * Groups[client] + OnOff[client] + LevelControl[client] +
     * ColorControl[client] via the complete top-level helper, plus
     * Scenes Management[client] added manually — see the header comment
     * above for why that one cluster is a real, checked gap in this
     * specific helper. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Control Bridge",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::control_bridge::config_t control_bridge_config;
    endpoint_t *endpoint = endpoint::control_bridge::create(node, &control_bridge_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create control bridge endpoint");
        return;
    }

    /* Scenes Management (client) — mandatoryConform on this device type,
     * but not created by control_bridge::add() itself. See the header
     * comment above for the full detail. */
    cluster::scenes_management::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    control_bridge_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Control bridge endpoint id: %u", control_bridge_endpoint_id);

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

    ESP_LOGI(TAG, "Matter control bridge started. Scan the QR code to commission.");
}
