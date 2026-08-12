/*
 * Minimal Matter On/Off Switch.
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
 * What this device actually does today: it's a Matter on/off switch endpoint
 * (client-side OnOff + Binding clusters, same as esp-matter's own
 * on_off_switch device type) whose local OnOff attribute toggles when you
 * press a physical button. That attribute change is visible to any
 * controller watching this device.
 *
 * What it does NOT do yet: actually send an OnOff command to another Matter
 * device (e.g. the light in firmware/light/) when bound to one. Doing that
 * needs esp-matter's client invoke APIs (see esp_matter_client.h in your SDK
 * checkout) plus a controller-driven Binding-cluster setup, and the exact
 * function signatures vary a bit by esp-matter release — deliberately left
 * as a TODO below rather than guessed at, so nothing here is code that looks
 * right but silently doesn't compile or work.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>

static const char *TAG = "matter_switch";

/* Change this to the GPIO your button is wired to (active-low, i.e. the
 * button pulls it to GND when pressed — internal pull-up keeps it HIGH
 * otherwise). GPIO 0 is the "BOOT" button on classic ESP32 DevKitC boards;
 * ESP32-C3/C6/H2 DevKits typically wire BOOT to GPIO 9. Adjust to match
 * your board. */
#define SWITCH_BUTTON_GPIO GPIO_NUM_0

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t switch_endpoint_id = 0;
static QueueHandle_t button_evt_queue = NULL;

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(button_evt_queue, &gpio_num, NULL);
}

/* Debounces the button and flips the switch's own OnOff attribute. A real
 * remote-control switch would also invoke a command on whatever device it's
 * bound to here — see the header comment above. */
static void button_task(void *arg)
{
    uint32_t io_num;
    bool switch_state = false;

    for (;;) {
        if (xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* Simple debounce: wait a moment, then confirm the pin is still low. */
        vTaskDelay(pdMS_TO_TICKS(30));
        if (gpio_get_level((gpio_num_t)io_num) != 0) {
            continue; // released again before we confirmed — ignore
        }

        switch_state = !switch_state;
        ESP_LOGI(TAG, "Button pressed — switch now %s", switch_state ? "ON" : "OFF");

        esp_matter_attr_val_t val = esp_matter_bool(switch_state);
        attribute::update(switch_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);

        /* TODO: send an OnOff command to the bound device(s) here using
         * esp-matter's client invoke API, once you've picked an SDK version
         * and confirmed its exact call signature. */

        /* Wait for release before re-arming, so one press = one toggle. */
        while (gpio_get_level((gpio_num_t)io_num) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
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

/* Called when a controller asks the device to "identify" itself. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify requested on endpoint %u", endpoint_id);
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

    /* 2. Configure the button input + its interrupt. */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << SWITCH_BUTTON_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    button_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(SWITCH_BUTTON_GPIO, button_isr_handler, (void *)(uintptr_t)SWITCH_BUTTON_GPIO);

    /* 3. Build the Matter data model: one node, one On/Off Switch endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    on_off_switch::config_t switch_config;
    endpoint_t *endpoint = on_off_switch::create(node, &switch_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create switch endpoint");
        return;
    }

    switch_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Switch endpoint id: %u", switch_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Matter switch started. Scan the QR code to commission.");
}
