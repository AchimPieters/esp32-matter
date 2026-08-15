/*
 * Minimal Matter On/Off Switch — 1 to 4 independent buttons, each its own
 * endpoint.
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
 * What this device does: it's SWITCH_BUTTON_COUNT independent Matter on/off
 * switch endpoints (client-side OnOff + Binding clusters each, esp-matter's
 * on_off_light_switch device type), all on one node. Pressing button N sends
 * a real OnOff::Toggle command to whatever *that button's own endpoint* is
 * bound to via a controller-driven Binding-cluster setup (e.g. Home
 * Assistant's "Bindings", or Apple/Google Home's equivalent once they
 * support it) — bind endpoint 1 to one light, endpoint 2 to a different
 * light, etc. This is the same modelling a physical multi-gang wall switch
 * uses in Matter: one node, one endpoint per gang, each independently
 * bindable. SWITCH_BUTTON_COUNT defaults to 1 (a single button/endpoint,
 * identical behavior to every version of this file before multi-button
 * support existed) — raise it (max 4) and wire up that many buttons to add
 * more independent switches.
 *
 * An earlier version of this file updated a *local* OnOff attribute
 * instead, which looked plausible but silently failed
 * ("esp_matter_attribute: Failed to get attribute handle") — on_off_light_
 * switch's OnOff cluster is CLIENT-only (see
 * esp_matter_endpoint.cpp:on_off_light_switch::add(), which calls
 * on_off::create(endpoint, NULL, CLUSTER_FLAG_CLIENT) — no server
 * instance, so no local attribute to update). The current implementation
 * (client::cluster_update() + client::interaction::invoke::send_request())
 * matches esp-matter's own examples/light_switch/main/app_driver.cpp,
 * which is what it was checked against instead of guessing at the client
 * invoke API's exact shape. client::set_request_callback() is registered
 * once, globally, not per endpoint — the callback itself is endpoint-
 * agnostic (it only inspects the invoked cluster/command and forwards to
 * whichever peer_device the Matter stack already resolved for that
 * specific client::cluster_update() call), so one registration correctly
 * serves every button's endpoint without any per-button special-casing.
 *
 * The Identify LED (see IDENTIFY_LED_GPIO below) does double duty ONLY
 * when SWITCH_BUTTON_COUNT is 1: besides blinking on a controller's
 * Identify command, it also flips on/off with every confirmed button
 * press, as a physical on/off indicator for that single switch's own
 * toggle state — handy since there's otherwise no local feedback that a
 * press was registered (the actual Matter state lives on whatever device
 * this switch is bound to, not here). With more than one button this
 * stops being meaningful — N independent buttons can each be bound to a
 * different target with its own independent state, so one shared LED
 * can no longer represent "the" switch state unambiguously — so for
 * SWITCH_BUTTON_COUNT > 1 the LED is a pure Identify indicator only, no
 * per-press toggling. If a controller happens to send Identify while
 * you're pressing a button (count == 1 case), the two will fight over
 * the same LED; harmless, just cosmetic.
 *
 * Known limitation: button presses are handled by one shared FreeRTOS
 * task off one shared debounce/dispatch queue (proven single-button
 * logic, reused as-is for every button rather than rewritten per-button).
 * If two different buttons are pressed at almost the same instant, the
 * second one's Toggle is sent only after the first button's press has
 * been fully handled *and released* — a few tens of milliseconds to
 * however long the first button stays held down. For how these are
 * actually used (one physical press at a time), this is unnoticeable;
 * worth knowing if you're deliberately testing simultaneous presses.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_client.h>

static const char *TAG = "matter_switch";

/* How many independent buttons/endpoints this firmware creates (1-4).
 * Raise this (or let the wizard's sed command do it) to match how many
 * physical buttons you've actually wired up — each one becomes its own
 * on_off_light_switch endpoint, independently bindable to a different
 * target device. Values outside 1-4 are rejected at compile time below
 * rather than silently clamped. */
#define SWITCH_BUTTON_COUNT 1
#define SWITCH_MAX_BUTTONS 4

#if SWITCH_BUTTON_COUNT < 1 || SWITCH_BUTTON_COUNT > SWITCH_MAX_BUTTONS
#error "SWITCH_BUTTON_COUNT must be between 1 and 4"
#endif

/* Change these to the GPIOs your buttons are wired to — only the first
 * SWITCH_BUTTON_COUNT of these four are actually configured/used; the
 * rest stay defined but untouched (harmless, same "unused but harmless"
 * pattern as e.g. firmware/temperature-sensor/'s unused second pin for
 * single-wire sensors). Reference wiring per button is a breadboard
 * pushbutton: GND -> button -> GPIO (no external resistor needed — the
 * internal pull-up enabled below keeps each pin HIGH until its button
 * pulls it to GND on press). None of GPIO 4/16/17/18 are classic ESP32
 * (WROOM-32) strapping pins (those are 0, 2, 5, 12, 15 — avoided
 * deliberately) and none are used by anything else in this firmware.
 * GPIO 4 is this repo's original single-button default, deliberately NOT
 * the onboard BOOT/PROG button (GPIO 0): that pin doubles as boot-mode
 * select and turned out unreliable as a switch input on the board this
 * was tested against (see CLAUDE.md's open next steps). Adjust to match
 * your board if you wire any of these elsewhere. */
#define SWITCH_BUTTON_1_GPIO GPIO_NUM_4
#define SWITCH_BUTTON_2_GPIO GPIO_NUM_16
#define SWITCH_BUTTON_3_GPIO GPIO_NUM_17
#define SWITCH_BUTTON_4_GPIO GPIO_NUM_18

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. Also doubles as
 * a local on/off indicator when SWITCH_BUTTON_COUNT == 1 (see the header
 * comment above). GPIO 2 is commonly the onboard/user LED on classic ESP32
 * (WROOM-32) devkits and isn't otherwise used by this firmware. Adjust to
 * match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

/* Index i here always corresponds to button (i+1) / SWITCH_BUTTON_<i+1>_GPIO
 * — only the first SWITCH_BUTTON_COUNT entries of each array are ever
 * configured or read. */
static const gpio_num_t switch_button_gpios[SWITCH_MAX_BUTTONS] = {
    SWITCH_BUTTON_1_GPIO,
    SWITCH_BUTTON_2_GPIO,
    SWITCH_BUTTON_3_GPIO,
    SWITCH_BUTTON_4_GPIO,
};
static uint16_t switch_endpoint_ids[SWITCH_MAX_BUTTONS] = {0};

static QueueHandle_t button_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
#if SWITCH_BUTTON_COUNT == 1
/* Tracks the (single) switch's own on/off indicator state, shown on the
 * Identify LED — see the header comment above. Purely local; not a Matter
 * attribute (this endpoint's OnOff cluster is client-only, see above). Only
 * meaningful with exactly one button/endpoint. */
static bool switch_indicator_state = false;
#endif

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(button_evt_queue, &gpio_num, NULL);
}

/* Maps a GPIO number back to its button index (0-based) / endpoint. Returns
 * -1 if it's not one of the currently-configured buttons (shouldn't happen
 * in practice — only configured GPIOs ever get an ISR attached — but
 * checked explicitly rather than assumed). */
static int switch_button_index_for_gpio(uint32_t gpio_num)
{
    for (int i = 0; i < SWITCH_BUTTON_COUNT; i++) {
        if ((uint32_t)switch_button_gpios[i] == gpio_num) {
            return i;
        }
    }
    return -1;
}

/* Called by the Matter stack once a bound peer has been resolved, for every
 * outstanding request queued via client::cluster_update(). Only OnOff-cluster
 * invoke requests are expected here (that's all this switch ever sends), so
 * anything else is logged and ignored rather than guessed at. Matches the
 * pattern in esp-matter's own examples/light_switch/main/app_driver.cpp.
 * Registered once for the whole device (see app_main() below) — endpoint-
 * agnostic by design, see the header comment above for why that's correct
 * for multiple buttons too. */
static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "Toggle command acknowledged by bound device");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Toggle command failed: %" CHIP_ERROR_FORMAT, error.Format());
}

static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }
    if (req_handle->command_path.mClusterId != OnOff::Id) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster 0x%04lx",
                 (unsigned long)req_handle->command_path.mClusterId);
        return;
    }
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

/* Debounces the button, then sends a real OnOff::Toggle command to whatever
 * *that button's own endpoint* is bound to (see the header comment above
 * for why this isn't a local attribute::update() call, and why one shared
 * task/queue safely serves every configured button). */
static void button_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

        /* Debounce: require the pin to read continuously low for ~40ms
         * (8 x 5ms samples) before treating this as a real press. A single
         * check after one fixed delay is too fragile in practice — cheap
         * tactile switches bounce (brief high blips right after the
         * initial contact), and a lone sample can land on one of those
         * blips and reject an entirely genuine press.
         *
         * Logs every sample's raw level instead of just the pass/fail
         * outcome. Left in deliberately, not just for development: on the
         * dev board this was written against, the physical BOOT/PROG
         * button (GPIO 0) behaves inconsistently — one clean press
         * produced a perfect "all low" read and a correct toggle, but
         * most attempts since (including after a fully clean rebuild and
         * reflash, with interrupt attachment and the debounce logic both
         * confirmed correct) produced no interrupt at all. That points to
         * a physical/mechanical issue with this specific button, not a
         * software bug — these per-sample logs are what you want when
         * investigating that with the board in hand. Safe to trim back to
         * a plain pass/fail log once a button is confirmed reliable. */
        bool confirmed = true;
        char samples[9] = {0};
        for (int i = 0; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            int level = gpio_get_level((gpio_num_t)io_num);
            samples[i] = level ? 'H' : 'L';
            if (level != 0) {
                confirmed = false;
            }
        }
        ESP_LOGI(TAG, "Samples (5ms apart): %s (%s)", samples, confirmed ? "ALL LOW" : "mixed/HIGH");
        if (!confirmed) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously held low");
            /* A single physical press can trigger several negedge
             * interrupts as the contact bounces, queuing multiple edge
             * events for what's really one action. Without this, we'd
             * immediately dequeue and debounce-check the NEXT stale
             * bounce-edge from the same burst — often long enough after
             * the fact that the pin reads released (HIGH) by then, which
             * looked identical to "the button doesn't work" from the
             * logs. Drop the rest of the burst and only look at genuinely
             * new interrupts from here. */
            xQueueReset(button_evt_queue);
            continue;
        }

        int button_index = switch_button_index_for_gpio(io_num);
        if (button_index < 0) {
            ESP_LOGW(TAG, "Edge on GPIO %lu doesn't match any configured button — ignoring",
                     (unsigned long)io_num);
            continue;
        }
        uint16_t target_endpoint = switch_endpoint_ids[button_index];

        ESP_LOGI(TAG, "Button %d (GPIO %lu) pressed — sending Toggle to endpoint %u's bound device(s)",
                 button_index + 1, (unsigned long)io_num, target_endpoint);

#if SWITCH_BUTTON_COUNT == 1
        /* Local on/off indicator — only meaningful with a single button;
         * see the header comment and IDENTIFY_LED_GPIO's comment above
         * for why this shares the Identify LED. */
        switch_indicator_state = !switch_indicator_state;
        gpio_set_level(IDENTIFY_LED_GPIO, switch_indicator_state ? 1 : 0);
        ESP_LOGI(TAG, "Indicator LED (GPIO %d) now %s", IDENTIFY_LED_GPIO,
                 switch_indicator_state ? "ON" : "OFF");
#endif

        client::request_handle_t req_handle;
        req_handle.type = client::INVOKE_CMD;
        req_handle.command_path.mClusterId = OnOff::Id;
        req_handle.command_path.mCommandId = OnOff::Commands::Toggle::Id;

        {
            /* We're running in a plain FreeRTOS task, not the Matter event
             * loop — the stack lock is required before calling into
             * esp-matter's client APIs from here. */
            lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
            client::cluster_update(target_endpoint, &req_handle);
        }

        /* Wait for release before re-arming, so one press = one toggle. */
        while (gpio_get_level((gpio_num_t)io_num) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        /* Release can bounce too, queuing more edges for the same
         * already-handled press — drop them the same way. */
        xQueueReset(button_evt_queue);
    }
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

/* Called whenever a controller changes an attribute on this device. A switch
 * has nothing to react to here — unlike the light, its own OnOff state is
 * driven by the button, not by remote writes — so this is a no-op required
 * by node::create()'s callback signature. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

/* Called when a controller asks the device to "identify" itself — starts
 * or stops the identify LED blinking accordingly. Same callback (and same
 * one shared LED) regardless of which button's endpoint the Identify
 * request targeted — this device has one physical Identify LED, not one
 * per button. */
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
        /* Effect variants (breathe, flash-twice, ...) aren't implemented —
         * treat any of them the same as a plain blink rather than guessing
         * at per-variant timing. */
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u — blinking as usual",
                 effect_id, effect_variant, endpoint_id);
        break;
    }
    return ESP_OK;
}

extern "C" void app_main(void)
{
    /* 1. Init NVS — stores the Matter fabric keys and factory data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Configure each button's input + interrupt. */
    for (int i = 0; i < SWITCH_BUTTON_COUNT; i++) {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << switch_button_gpios[i]);
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.intr_type = GPIO_INTR_NEGEDGE;
        gpio_config(&io_conf);

        ESP_LOGI(TAG, "Button %d GPIO %d idle level: %d (expect 1/HIGH — 0 here means the pull-up isn't winning, check wiring)",
                 i + 1, switch_button_gpios[i], gpio_get_level(switch_button_gpios[i]));
    }

    button_evt_queue = xQueueCreate(SWITCH_MAX_BUTTONS, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

    /* These two silently doing nothing was a real bug here: unchecked, a
     * failure means no interrupt is ever attached and every button press
     * produces zero log output, which looks identical to "nothing is
     * wired up" from the outside. */
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    for (int i = 0; i < SWITCH_BUTTON_COUNT; i++) {
        esp_err_t isr_add_err = gpio_isr_handler_add(switch_button_gpios[i], button_isr_handler,
                                                       (void *)(uintptr_t)switch_button_gpios[i]);
        if (isr_add_err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_isr_handler_add failed for button %d: %s", i + 1, esp_err_to_name(isr_add_err));
        }
    }

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

    /* 3. Build the Matter data model: one node, SWITCH_BUTTON_COUNT On/Off
     * Switch endpoints, one per configured button. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    for (int i = 0; i < SWITCH_BUTTON_COUNT; i++) {
        on_off_light_switch::config_t switch_config;
        endpoint_t *endpoint = on_off_light_switch::create(node, &switch_config, ENDPOINT_FLAG_NONE, NULL);
        if (!endpoint) {
            ESP_LOGE(TAG, "Failed to create switch endpoint for button %d", i + 1);
            return;
        }
        switch_endpoint_ids[i] = endpoint::get_id(endpoint);
        ESP_LOGI(TAG, "Button %d: switch endpoint id %u (GPIO %d)",
                 i + 1, switch_endpoint_ids[i], switch_button_gpios[i]);
    }

    /* 3b. Register the callback that actually sends the Toggle command once
     * a bound peer is resolved. One registration for the whole device — see
     * the header comment above for why that correctly serves every button's
     * endpoint. binding_manager_init() (which resolves bindings set up via
     * a controller's Binding cluster) runs on its own, inside
     * esp_matter::start() below — no explicit call needed here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Matter switch started (%d button%s). Scan the QR code to commission.",
             SWITCH_BUTTON_COUNT, SWITCH_BUTTON_COUNT == 1 ? "" : "s");
}
