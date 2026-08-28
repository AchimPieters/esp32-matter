/*
 * Minimal Matter Color Dimmer Switch — forty-eighth device type, and a
 * direct extension of firmware/dimmer-switch/: the exact same On/Off +
 * LevelControl client commands and press/rotary interaction, plus a
 * second rotary encoder sending real ColorControl::StepHue commands —
 * confirmed against the CSA's own spec to be a genuine superset of
 * Dimmer Switch, not a separate design.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: a complete top-level helper, confirmed a genuine
 * superset of Dimmer Switch ------------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * ColorDimmerSwitch.xml (device type 0x0105, revision 3, explicitly
 * classified `superset="Dimmer Switch"`): identical cluster list to
 * firmware/dimmer-switch/'s own DimmerSwitch.xml (Identify server+client,
 * On/Off client, Level Control client, all mandatoryConform; Groups/
 * Scenes Management client both optionalConform, not implemented) PLUS
 * one addition — Color Control (client, mandatoryConform). `endpoint::
 * color_dimmer_switch::create()` confirmed complete/ready-to-use by
 * reading `esp_matter_endpoint.cpp`'s own `color_dimmer_switch::add()`
 * directly: byte-for-byte the same Descriptor + Binding + Identify
 * [server+client] + OnOff[client] + LevelControl[client] wiring firmware/
 * dimmer-switch/'s own header comment already documents, with exactly
 * one extra line — `color_control::create(endpoint, NULL,
 * CLUSTER_FLAG_CLIENT)` — added. Zero manual cluster-creation code needed
 * in this file either, same simplest-client-side-endpoint precedent
 * firmware/dimmer-switch/ already established.
 *
 * --- Four real gestures, one command shape genuinely new to this file --
 * The on/off button (short press = OnOff::Toggle, long press =
 * Identify::Identify) and the brightness rotary encoder (LevelControl::
 * Step per detent) are both reused verbatim from firmware/dimmer-switch/
 * — see that file's own header comment for the full sourcing detail on
 * both. A SECOND rotary encoder sends real `ColorControl::Commands::
 * StepHue` commands (one per detent) — confirmed by reading
 * connectedhomeip's own generated `ColorControl/Commands.h` directly
 * that this command's own field shape genuinely differs from
 * LevelControl::Step's in two ways worth remembering for any future
 * ColorControl client command in this repo: `StepModeEnum` here is
 * `kUp`=0x01/`kDown`=0x03 (NOT LevelControl::StepModeEnum's own 0/1 —
 * confirmed directly in `ColorControl/Enums.h`, a real, easy mistake to
 * make by assuming the two clusters share one step-direction
 * convention), and `TransitionTime` here is a plain (non-nullable)
 * `uint8_t` in tenths of a second (confirmed directly in the generated
 * `Type` struct), unlike LevelControl::Step's own nullable uint16 —
 * meaning this command's JSON payload needs a real numeric value for
 * that field, not the `NULL` type-tag trick firmware/dimmer-switch/'s
 * own LevelControl::Step payload needs. `StepHue::Fields::kStepMode`=0/
 * `kStepSize`=1/`kTransitionTime`=2/`kOptionsMask`=3/`kOptionsOverride`=4
 * confirmed directly against the same generated header, same "field IDs
 * come from Commands.h, not the spec's own prose numbering" discipline
 * firmware/dimmer-switch/'s own header comment already establishes.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no rotary-encoder/pushbutton hardware for this device
 * type physically available when written — and, like firmware/
 * dimmer-switch/, testing this one for real also needs a second, already-
 * commissioned bindable color light on the same fabric, not just this
 * device alone).
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

static const char *TAG = "matter_color_dimmer_switch";

/* Momentary pushbutton — active-LOW, internal pull-up. Short press sends
 * OnOff::Toggle; long press sends Identify::Identify. Same convention
 * firmware/dimmer-switch/'s own button already uses. */
#define COLOR_DIMMER_SWITCH_BUTTON_GPIO GPIO_NUM_4

/* Brightness rotary encoder — same GPIOs/behavior as firmware/
 * dimmer-switch/'s own single encoder (LevelControl::Step per detent). */
#define BRIGHTNESS_ENCODER_A_GPIO GPIO_NUM_16
#define BRIGHTNESS_ENCODER_B_GPIO GPIO_NUM_17
#define BRIGHTNESS_STEP_SIZE 25 /* out of LevelControl's own 0-254 range, ~10% per detent */

/* Hue rotary encoder — a second, independent encoder for ColorControl::
 * StepHue. GPIO 18/19 are free, unreserved pins on classic ESP32
 * (WROOM-32), not used elsewhere in this firmware. */
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
#define COLOR_DIMMER_SWITCH_LONG_PRESS_MS 1000
#define COLOR_DIMMER_SWITCH_DEBOUNCE_SAMPLES 8

/* How long the bound target's own identify effect should run for, in
 * seconds, when this switch's long press sends Identify::Identify. */
#define COLOR_DIMMER_SWITCH_IDENTIFY_TIME_SEC 5

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t color_dimmer_switch_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static QueueHandle_t brightness_evt_queue = NULL;
static QueueHandle_t hue_evt_queue = NULL;

/* Called by the Matter stack once a bound peer has been resolved, for
 * every outstanding request queued via client::cluster_update(). Builds
 * the right JSON command payload per cluster/command — see the header
 * comment above (and firmware/dimmer-switch/'s own) for the exact schema
 * and field-ID sourcing, and for why StepHue's own payload doesn't need
 * the `NULL` type-tag trick LevelControl::Step's own TransitionTime
 * field needs. */
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
        /* StepMode passed through request_data (0 = Up, 1 = Down per
         * LevelControl::StepModeEnum's own values). */
        uint8_t step_mode = (uint8_t)(uintptr_t)req_handle->request_data;
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U8\": %u, \"1:U8\": %u, \"2:NULL\": null, \"3:U8\": 0, \"4:U8\": 0}",
                 step_mode, (unsigned)BRIGHTNESS_STEP_SIZE);
    } else if (req_handle->command_path.mClusterId == ColorControl::Id &&
               req_handle->command_path.mCommandId == ColorControl::Commands::StepHue::Id) {
        /* StepMode passed through request_data — ColorControl::
         * StepModeEnum's own values (0x01 Up / 0x03 Down), NOT
         * LevelControl's 0/1 — see the header comment above for why
         * these two clusters' StepMode values genuinely differ.
         * TransitionTime is a plain non-nullable U8 here (tenths of a
         * second), unlike LevelControl::Step's own nullable field. */
        uint8_t step_mode = (uint8_t)(uintptr_t)req_handle->request_data;
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U8\": %u, \"1:U8\": %u, \"2:U8\": %u, \"3:U8\": 0, \"4:U8\": 0}",
                 step_mode, (unsigned)HUE_STEP_SIZE, (unsigned)HUE_STEP_TRANSITION_TIME);
    } else if (req_handle->command_path.mClusterId == Identify::Id &&
               req_handle->command_path.mCommandId == Identify::Commands::Identify::Id) {
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U16\": %u}", (unsigned)COLOR_DIMMER_SWITCH_IDENTIFY_TIME_SEC);
    } else {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, command_data_json_str,
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

/* Sends a real command to whatever this endpoint's Binding cluster
 * resolves to. `request_data` carries StepMode for a Step-family request
 * (unused/NULL for Toggle/Identify) — see app_client_request_cb() above
 * for where it's read back out. */
static void send_bound_command(uint32_t cluster_id, uint32_t command_id, void *request_data)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = cluster_id;
    req_handle.command_path.mCommandId = command_id;
    req_handle.request_data = request_data;

    /* Stack lock required before calling into esp-matter's client APIs
     * from a plain FreeRTOS task, same as every other client-invoke
     * device in this repo. */
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(color_dimmer_switch_endpoint_id, &req_handle);
}

/* Debounces the on/off button and distinguishes a short press (Toggle)
 * from a long press (Identify) — reused verbatim from firmware/
 * dimmer-switch/'s own button_task(). */
static void button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(COLOR_DIMMER_SWITCH_BUTTON_GPIO) == 0);
    int64_t press_start_ms = 0;
    bool long_press_fired = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int64_t now_ms = esp_timer_get_time() / 1000;

        bool raw_pressed = (gpio_get_level(COLOR_DIMMER_SWITCH_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < COLOR_DIMMER_SWITCH_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == COLOR_DIMMER_SWITCH_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            press_start_ms = now_ms;
            long_press_fired = false;
        }

        if (debounced_pressed && !long_press_fired && (now_ms - press_start_ms) >= COLOR_DIMMER_SWITCH_LONG_PRESS_MS) {
            long_press_fired = true;
            ESP_LOGI(TAG, "Long press — sending Identify to bound device(s)");
            send_bound_command(Identify::Id, Identify::Commands::Identify::Id, NULL);
        }

        if (consistent_samples == COLOR_DIMMER_SWITCH_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
            if (!long_press_fired) {
                ESP_LOGI(TAG, "Short press — sending Toggle to bound device(s)");
                send_bound_command(OnOff::Id, OnOff::Commands::Toggle::Id, NULL);
            }
        }
    }
}

/* Standard quadrature decoding — same technique/sourcing firmware/
 * thermostat/'s own rotary encoder and firmware/dimmer-switch/'s own
 * brightness encoder already establish. Two independent instances below
 * (brightness, hue) share this exact shape, parameterized only by which
 * GPIO/queue each one uses. */
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
        /* ColorControl::StepModeEnum's own values (0x01/0x03) — NOT
         * LevelControl::StepModeEnum's 0/1, see the header comment above. */
        uint8_t step_mode = (direction > 0)
            ? chip::to_underlying(ColorControl::StepModeEnum::kUp)
            : chip::to_underlying(ColorControl::StepModeEnum::kDown);
        ESP_LOGI(TAG, "Hue encoder — sending ColorControl::StepHue (%s) to bound device(s)",
                 direction > 0 ? "Up" : "Down");
        send_bound_command(ColorControl::Id, ColorControl::Commands::StepHue::Id, (void *)(uintptr_t)step_mode);
    }
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

/* No local attributes this device writes to itself — everything flows
 * outward via bound commands. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

/* Called when a controller asks THIS device to identify itself (its own
 * server-side Identify cluster). */
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

    /* 2. Configure the on/off button — active-LOW, internal pull-up. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << COLOR_DIMMER_SWITCH_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);

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
    xTaskCreate(brightness_task, "brightness_task", 4096, NULL, 5, NULL);
    xTaskCreate(hue_task, "hue_task", 4096, NULL, 5, NULL);

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

    /* 3. Build the Matter data model: one node, one Color Dimmer Switch
     * endpoint — Descriptor + Binding + Identify[server+client] +
     * OnOff[client] + LevelControl[client] + ColorControl[client], all
     * via the complete top-level helper — see the header comment above
     * for why no manual cluster-creation code is needed at all. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Color Dimmer Switch",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::color_dimmer_switch::config_t color_dimmer_switch_config;
    endpoint_t *endpoint = endpoint::color_dimmer_switch::create(node, &color_dimmer_switch_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create color dimmer switch endpoint");
        return;
    }
    color_dimmer_switch_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Color dimmer switch endpoint id: %u", color_dimmer_switch_endpoint_id);

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

    /* Endpoint-agnostic by design (same reasoning as firmware/switch/'s
     * single global registration) — this device only has the one
     * endpoint anyway. binding_manager_init() (which resolves bindings
     * set up via a controller's Binding cluster) runs on its own, inside
     * esp_matter::start() above — no explicit call needed here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);

    ESP_LOGI(TAG, "Matter color dimmer switch started. Scan the QR code to commission.");
}
