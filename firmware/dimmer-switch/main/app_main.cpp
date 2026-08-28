/*
 * Minimal Matter Dimmer Switch — forty-seventh device type: a wall-mount
 * (or handheld) remote dimmer, sending real On/Off, LevelControl, and
 * Identify commands to whatever device a controller binds it to — this
 * repo's first client-side device combining brightness control with
 * simple on/off, and its first command ever sent with real, non-empty
 * command fields (every prior client-invoke device here — firmware/
 * switch/'s buttons, firmware/thermostat/'s BINDING output, firmware/
 * doorbell/'s Chime client — has only ever sent commands with no fields
 * at all).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: a complete top-level helper -----------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * DimmerSwitch.xml (device type 0x0104, revision 3, classified as a
 * superset of On/Off Light Switch): Identify (BOTH server AND client,
 * both mandatoryConform — the client side is what lets this device ask a
 * bound target to identify itself, see below) + On/Off (client,
 * mandatoryConform) + Level Control (client, mandatoryConform) — Groups
 * and Scenes Management (both client) are optionalConform and not
 * implemented, same "smallest reasonable next step" scoping this repo
 * applies elsewhere. `endpoint::dimmer_switch::create()` confirmed
 * complete/ready-to-use by reading `esp_matter_endpoint.cpp`'s own
 * `dimmer_switch::add()`/`create()` directly: Descriptor (via `common::
 * create<T>()`) + Binding (added in `create()` itself, not `add()` — a
 * detail worth noting since it means Binding exists even before
 * `add_device_type()` runs, though the ordering has no practical effect
 * here) + Identify (`CLUSTER_FLAG_SERVER | CLUSTER_FLAG_CLIENT` in one
 * call — confirmed this dual-flag call shape is valid and exactly what
 * firmware/switch/'s own `on_off_light_switch::add()` already does for
 * its own Identify cluster) + OnOff (client) + LevelControl (client) —
 * every mandatory cluster, zero manual cluster-creation code needed in
 * this file at all, the simplest endpoint construction of any client-
 * side device in this repo so far.
 *
 * --- Three real commands, three real gestures -----------------------------
 * A short press on the single physical button sends `OnOff::Toggle`
 * (empty payload, same `"{}"`  shape firmware/switch/'s own buttons
 * already use) to whatever this endpoint's Binding cluster resolves to.
 * A long press instead sends `Identify::Commands::Identify` (IdentifyTime
 * = 5s) — a genuinely new, real use for the mandatory client-side
 * Identify cluster the device type's own XML requires: a user can hold
 * the button to make the bound light blink/identify itself, confirming
 * which physical light this switch actually controls, without needing a
 * controller app open at all. Turning the rotary encoder sends
 * `LevelControl::Commands::Step` (StepMode Up/Down per the encoder's own
 * rotation direction, a fixed StepSize) once per detent — the natural
 * "turn to dim" interaction a physical dimmer knob provides, reusing
 * firmware/thermostat/'s own quadrature-decoding technique (channel A's
 * falling edge as trigger, channel B's level at that instant gives
 * direction — the standard technique countless KY-040-class encoder
 * drivers use, not a chip-specific protocol) and firmware/generic-
 * switch/'s own short/long-press debounce timing, both reused directly
 * rather than reinvented.
 *
 * --- Real command fields — a genuinely new risk for this repo's own
 * client-invoke pattern, resolved by reading the JSON-to-TLV encoder's
 * own source directly rather than guessing -------------------------------
 * Every prior client-invoke command in this repo (`OnOff::Toggle`,
 * `Chime::PlayChimeSound` with its ChimeID omitted) has sent an empty
 * `"{}"` payload to `client::interaction::invoke::send_request()` — this
 * file is the first to need real field values encoded into that JSON
 * string. Confirmed the exact schema by reading esp-matter's own
 * `utils/jsontlv/element_types.h` and `json_to_tlv.cpp` directly (not
 * guessed from the one field esp-matter's own `examples/light_switch/
 * main/app_driver.cpp` reference happens to encode, `"0:U16": <value>`
 * for Identify's own IdentifyTime): each key is `"<field-id>:<type-tag>"`
 * (U8/U16/U32/U64/I8/I16/I32/I64/BOOL/FP/DFP/BYT/STR/NULL/OBJ/ARR
 * supported), confirmed by reading `json_to_tlv.cpp`'s own type-parsing
 * function directly — including that a nullable field's key can use the
 * `NULL` type tag to encode a genuine TLV null (`writer.PutNull(tag)`),
 * not just a numeric zero, which `LevelControl::Step`'s own
 * `TransitionTime` field needs (nullable per spec; sent as null here so
 * the bound light's own default fade time applies, rather than this
 * switch guessing a transition duration for hardware it knows nothing
 * about). Field IDs for both commands confirmed directly against
 * connectedhomeip's own generated `Commands.h` for each cluster (not
 * assumed from the Matter spec's own prose, which numbers fields
 * separately from these implementation-level indices):
 * `LevelControl::Commands::Step::Fields` — `kStepMode`=0, `kStepSize`=1,
 * `kTransitionTime`=2, `kOptionsMask`=3, `kOptionsOverride`=4 (the latter
 * two are LevelControl's own "enhanced options" mechanism for overriding
 * global scene/execute-if-off behavior per command — both sent as 0, the
 * standard "don't override anything" value); `Identify::Commands::
 * Identify::Fields` — `kIdentifyTime`=0 (uint16 seconds).
 * `StepModeEnum::kUp`=0/`kDown`=1 confirmed against connectedhomeip's own
 * generated `LevelControl/Enums.h`.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no rotary encoder/pushbutton hardware for this device
 * type physically available when written, and — unlike every server-side
 * device type in this repo — verifying this one for real also needs a
 * second, already-commissioned bindable target device on the same
 * fabric, not just this device alone).
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

static const char *TAG = "matter_dimmer_switch";

/* Momentary pushbutton — active-LOW, internal pull-up. Short press sends
 * OnOff::Toggle; long press sends Identify::Identify. Reference wiring:
 * GND -> button -> GPIO, same convention this repo's other buttons use. */
#define DIMMER_SWITCH_BUTTON_GPIO GPIO_NUM_4

/* Rotary encoder (A/B quadrature channels only — no integrated push-button
 * used here, unlike firmware/thermostat/'s own optional encoder, since
 * this device's single dedicated button above already covers on/off +
 * identify). GPIO 16/17 are free, unreserved pins on classic ESP32
 * (WROOM-32), not used elsewhere in this firmware. */
#define ROTARY_ENCODER_A_GPIO GPIO_NUM_16
#define ROTARY_ENCODER_B_GPIO GPIO_NUM_17
#define ROTARY_ENCODER_STEP_SIZE 25 /* out of LevelControl's own 0-254 range, ~10% per detent */

/* LED for the Matter "Identify" cluster (this device's OWN server-side
 * Identify — a controller asking THIS switch to identify itself, distinct
 * from this switch's own long-press sending Identify to its bound
 * target). */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Press-timing constants — same values/reasoning firmware/generic-
 * switch/'s own header comment already documents in full. */
#define DIMMER_SWITCH_LONG_PRESS_MS 1000
#define DIMMER_SWITCH_DEBOUNCE_SAMPLES 8

/* How long the bound target's own identify effect should run for, in
 * seconds, when this switch's long press sends Identify::Identify. */
#define DIMMER_SWITCH_IDENTIFY_TIME_SEC 5

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t dimmer_switch_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static QueueHandle_t rotary_evt_queue = NULL;

/* Called by the Matter stack once a bound peer has been resolved, for
 * every outstanding request queued via client::cluster_update(). Builds
 * the right JSON command payload per cluster/command — see the header
 * comment above for the exact schema and field-ID sourcing. Modeled on
 * esp-matter's own `examples/light_switch/main/app_driver.cpp` reference
 * dispatch shape (per-cluster, then per-command branching), the first
 * time this repo's own client-request callback has needed more than one
 * command shape to dispatch. */
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
        /* StepMode is passed through priv_data (0 = Up, 1 = Down) — see
         * button_task()/rotary_task() below for where it's set. */
        uint8_t step_mode = (uint8_t)(uintptr_t)req_handle->request_data;
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U8\": %u, \"1:U8\": %u, \"2:NULL\": null, \"3:U8\": 0, \"4:U8\": 0}",
                 step_mode, (unsigned)ROTARY_ENCODER_STEP_SIZE);
    } else if (req_handle->command_path.mClusterId == Identify::Id &&
               req_handle->command_path.mCommandId == Identify::Commands::Identify::Id) {
        snprintf(command_data_json_str, sizeof(command_data_json_str),
                 "{\"0:U16\": %u}", (unsigned)DIMMER_SWITCH_IDENTIFY_TIME_SEC);
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
 * resolves to. `request_data` carries StepMode for a LevelControl::Step
 * request (unused/NULL for the other two) — see app_client_request_cb()
 * above for where it's read back out. */
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
    client::cluster_update(dimmer_switch_endpoint_id, &req_handle);
}

/* Debounces the on/off button and distinguishes a short press (Toggle)
 * from a long press (Identify) — same debounce-then-classify shape
 * firmware/generic-switch/'s own state machine already establishes,
 * simplified since this device only needs the two outcomes, not the
 * full multi-press/release event set. */
static void button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(DIMMER_SWITCH_BUTTON_GPIO) == 0);
    int64_t press_start_ms = 0;
    bool long_press_fired = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));
        int64_t now_ms = esp_timer_get_time() / 1000;

        bool raw_pressed = (gpio_get_level(DIMMER_SWITCH_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < DIMMER_SWITCH_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == DIMMER_SWITCH_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            press_start_ms = now_ms;
            long_press_fired = false;
        }

        if (debounced_pressed && !long_press_fired && (now_ms - press_start_ms) >= DIMMER_SWITCH_LONG_PRESS_MS) {
            long_press_fired = true;
            ESP_LOGI(TAG, "Long press — sending Identify to bound device(s)");
            send_bound_command(Identify::Id, Identify::Commands::Identify::Id, NULL);
        }

        if (consistent_samples == DIMMER_SWITCH_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
            if (!long_press_fired) {
                ESP_LOGI(TAG, "Short press — sending Toggle to bound device(s)");
                send_bound_command(OnOff::Id, OnOff::Commands::Toggle::Id, NULL);
            }
        }
    }
}

/* Standard quadrature decoding — see the header comment above for the
 * full explanation and sourcing (reused from firmware/thermostat/'s own
 * rotary encoder). Runs on channel A's falling edge only; channel B's
 * level at that instant gives direction. */
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
        uint8_t step_mode = (direction > 0)
            ? chip::to_underlying(LevelControl::StepModeEnum::kUp)
            : chip::to_underlying(LevelControl::StepModeEnum::kDown);
        ESP_LOGI(TAG, "Rotary encoder — sending Step (%s) to bound device(s)", direction > 0 ? "Up" : "Down");
        send_bound_command(LevelControl::Id, LevelControl::Commands::Step::Id, (void *)(uintptr_t)step_mode);
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
 * server-side Identify cluster) — starts or stops the identify LED
 * blinking accordingly. */
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
    button_io_conf.pin_bit_mask = (1ULL << DIMMER_SWITCH_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "dimmer_button", 4096, NULL, 5, NULL);

    /* 2b. Configure the rotary encoder's A/B channels + their shared
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
    xTaskCreate(rotary_task, "dimmer_rotary", 4096, NULL, 5, NULL);

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

    /* 3. Build the Matter data model: one node, one Dimmer Switch endpoint
     * — Descriptor + Binding + Identify[server+client] + OnOff[client] +
     * LevelControl[client], all via the complete top-level helper — see
     * the header comment above for why no manual cluster-creation code
     * is needed at all. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Dimmer Switch",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::dimmer_switch::config_t dimmer_switch_config;
    endpoint_t *endpoint = endpoint::dimmer_switch::create(node, &dimmer_switch_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create dimmer switch endpoint");
        return;
    }
    dimmer_switch_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Dimmer switch endpoint id: %u", dimmer_switch_endpoint_id);

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

    ESP_LOGI(TAG, "Matter dimmer switch started. Scan the QR code to commission.");
}
