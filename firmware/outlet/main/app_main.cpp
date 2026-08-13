/*
 * Minimal Matter On/Off Plug-in Unit.
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
 * What this device does, and why it's a separate device type from
 * firmware/switch/: esp-matter's `on_off_light_switch` (used by
 * firmware/switch/) is a Matter *client* — it's a remote control that sends
 * commands to other devices via the Binding cluster, and has no on/off state
 * of its own for a controller to show or toggle. That's spec-correct for a
 * "remote switch", but it means Apple/Google Home display it as a generic,
 * uncontrollable "Matter Accessory" — they have no UI for setting up
 * Bindings, and there's no local attribute for them to render.
 *
 * This device uses `on_off_plug_in_unit` instead — the same *server-side*
 * OnOff pattern as firmware/light/, just with a "switch/outlet" device type
 * instead of "light" (different icon, same mechanics: same on_off cluster
 * handling in esp_matter_endpoint.cpp; `on_off::create(endpoint, ...,
 * CLUSTER_FLAG_SERVER)`). Apple/Google Home render it as a real, controllable
 * on/off tile: press the physical button and it toggles the Matter OnOff
 * attribute directly (attribute::update(), the exact call firmware/light/
 * uses), which any commissioned controller sees and can also toggle from its
 * own side. Nothing here talks to another device — for that, use
 * firmware/switch/'s Binding-based remote control instead.
 *
 * Apple/Google Home label this an "Outlet"/"Stopcontact", not a "Switch" —
 * that's expected, not a bug: the Matter device type library has no
 * separate device type for "a wall switch with its own on/off state"
 * distinct from a plug-in outlet (checked the spec directly, in
 * connectedhomeip's data_model/<version>/device_types/ folder: every
 * device type with "Switch" in the name — OnOffLightSwitch, DimmerSwitch,
 * ColorDimmerSwitch, GenericSwitch — is a client/input device, none of
 * them a controllable on/off output). This
 * repo follows the Matter device type library as specified rather than
 * picking a device type for its Apple Home icon; on_off_plug_in_unit is
 * the spec-correct type for exactly this device, icon included.
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

static const char *TAG = "matter_outlet";

/* Change this to the GPIO your output (LED / relay) is wired to — this is
 * the actual on/off state, driven by the Matter OnOff attribute regardless
 * of whether the change came from the physical button or a remote
 * controller. GPIO 2 is commonly the onboard/user LED on classic ESP32
 * (WROOM-32) devkits. Adjust to match your board. */
#define OUTLET_GPIO GPIO_NUM_2

/* Change this to the GPIO your button is wired to. Reference wiring is a
 * breadboard pushbutton: GND -> button -> GPIO (no external resistor
 * needed — the internal pull-up below keeps the pin HIGH until the button
 * pulls it to GND on press). GPIO 4 is a plain, unreserved GPIO on classic
 * ESP32 (WROOM-32) — deliberately NOT the onboard BOOT/PROG button
 * (GPIO 0): that pin is also used for boot-mode selection and turned out
 * unreliable as an external input on the board this was tested against
 * (see CLAUDE.md's open next steps). Adjust to match your board if you
 * wire it elsewhere. */
#define OUTLET_BUTTON_GPIO GPIO_NUM_4

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of its own on/off state. Shares OUTLET_GPIO by
 * default (matches this repo's other reference wiring, which only has one
 * LED wired up) — the two will fight over that LED during an identify
 * request, which is harmless and purely cosmetic. Wire a second LED to a
 * free GPIO and change this if you want them independent. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t outlet_endpoint_id = 0;
static QueueHandle_t button_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Mirrors the Matter OnOff attribute's current value — kept in sync solely
 * by app_attribute_update_cb() below, which fires for every change to that
 * attribute regardless of source (our own button, or a remote write from a
 * controller). button_task() reads this to know what to toggle to; nothing
 * else writes it directly, so there's exactly one source of truth. */
static bool outlet_state = false;

static void set_output(bool on)
{
    gpio_set_level(OUTLET_GPIO, on ? 1 : 0);
}

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

/* Debounces the button and, on a confirmed press, toggles the Matter OnOff
 * attribute via attribute::update() — the same server-side call
 * firmware/light/ uses, which is why this shows up as a real controllable
 * tile in Apple/Google Home (unlike firmware/switch/'s client-only
 * approach). Debounce logic itself is identical to firmware/switch/'s
 * button — see the comments there for the reasoning (contact bounce,
 * why continuous-low sampling instead of one fixed delay, why the queue
 * gets reset after each cycle). */
static void button_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

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
            xQueueReset(button_evt_queue);
            continue;
        }

        bool new_state = !outlet_state;
        ESP_LOGI(TAG, "Button pressed — turning outlet %s", new_state ? "ON" : "OFF");

        esp_matter_attr_val_t val = esp_matter_bool(new_state);
        attribute::update(outlet_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);

        /* Wait for release before re-arming, so one press = one toggle. */
        while (gpio_get_level((gpio_num_t)io_num) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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

/* Called whenever the OnOff attribute changes — from our own button
 * (attribute::update() above) or a remote controller's write/command. This
 * is the single place that drives the physical output and the local state
 * mirror, so both stay correct no matter which side triggered the change. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && endpoint_id == outlet_endpoint_id &&
        cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        outlet_state = val->val.b;
        set_output(outlet_state);
        ESP_LOGI(TAG, "Outlet turned %s", outlet_state ? "ON" : "OFF");
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
        set_output(outlet_state);
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

    /* 2. Configure the output (LED/relay). */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << OUTLET_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    set_output(false);

    /* 2b. Configure the button input + its interrupt. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << OUTLET_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    button_io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&button_io_conf);

    button_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Button GPIO %d idle level: %d (expect 1/HIGH — 0 here means the pull-up isn't winning, check wiring)",
             OUTLET_BUTTON_GPIO, gpio_get_level(OUTLET_BUTTON_GPIO));

    /* These two silently doing nothing was a real bug in firmware/switch:
     * unchecked, a failure means no interrupt is ever attached and every
     * button press produces zero log output, which looks identical to
     * "nothing is wired up" from the outside. */
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(OUTLET_BUTTON_GPIO, button_isr_handler, (void *)(uintptr_t)OUTLET_BUTTON_GPIO);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
    }

    /* 2c. Configure the identify LED + its blink timer (not started yet —
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

    /* 3. Build the Matter data model: one node, one On/Off Plug-in Unit
     * endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    on_off_plug_in_unit::config_t outlet_config;
    endpoint_t *endpoint = on_off_plug_in_unit::create(node, &outlet_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create outlet endpoint");
        return;
    }

    outlet_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Outlet endpoint id: %u", outlet_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Matter outlet started. Scan the QR code to commission.");
}
