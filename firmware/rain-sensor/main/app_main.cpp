/*
 * Minimal Matter Rain Sensor — thirty-ninth device type, and the third
 * sibling in this repo's BooleanState family after firmware/
 * water-leak-detector/ and firmware/water-freeze-detector/ — same cluster,
 * same real esp-matter FeatureMap gap and fix, and — confirmed, not
 * assumed — literally the SAME physical sensor module as firmware/
 * water-leak-detector/'s own probe, just mounted and marketed for a
 * different purpose.
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
 * --- Endpoint: structurally identical to firmware/water-leak-detector/'s --
 * Confirmed directly via esp-matter's own `rain_sensor::add()` in
 * esp_matter_endpoint.cpp: identical structure to `water_leak_detector::
 * add()`/`water_freeze_detector::add()`/`contact_sensor::add()` (Identify +
 * BooleanState + StateChange event, via `common::create<T>()` so the
 * endpoint's Descriptor cluster is created automatically) — matches the
 * CSA's own data_model/1.6/device_types/RainSensor.xml exactly (Identify
 * mandatory, BooleanState mandatory with its ChangeEvent feature mandatory
 * as of the device type's own revision 2, an optional
 * BooleanStateConfiguration cluster not implemented here — same "smallest
 * reasonable next step" scope cut firmware/water-leak-detector/'s own
 * header comment already applies to that same optional cluster).
 *
 * --- StateValue semantic: true = rain detected ------------------------
 * Confirmed against real-world Matter tooling, not assumed: Espressif's own
 * `MatterRainSensor` Arduino-ESP32 class exposes `setRain(bool)`,
 * documented as setting the state after a real sensor reading — `true`
 * means rain IS detected, the same "true = the sensed condition is
 * present" direction firmware/water-leak-detector/'s own StateValue
 * already uses for a leak (though unlike a leak or a freeze, rain itself
 * isn't inherently a hazard — it's simply what the sensor reports, closer
 * in spirit to OccupancySensing's own true=occupied than to an alarm
 * cluster, even though it shares BooleanState's cluster shape with this
 * repo's two hazard detectors).
 *
 * --- The same real, documented esp-matter FeatureMap gap firmware/
 * water-leak-detector/ and firmware/water-freeze-detector/ already found —
 * and the identical fix ---------------------------------------------------
 * Confirmed by reading `boolean_state::create()` in esp-matter's own
 * esp_matter_cluster.cpp directly (the exact same function every
 * BooleanState-based device type in this repo is built from): it hardcodes
 * `global::attribute::create_feature_map(cluster, 0)`, with no config
 * field to override it — so esp-matter's own `rain_sensor::add()`
 * reference implementation never actually sets the ChangeEvent (CHGEVENT)
 * feature bit this device type's own spec (revision 2) makes mandatory.
 * Same fix as firmware/water-leak-detector/'s and firmware/
 * water-freeze-detector/'s own header comments already document in full:
 * `boolean_state::event::create_state_change()` fires the StateChange
 * event unconditionally, with no feature-flag gate — confirmed by reading
 * its own implementation — so this FeatureMap bit is pure advertised-
 * conformance metadata here, safe to overwrite directly via
 * `attribute::update()` after endpoint creation but before
 * `esp_matter::start()`, exactly like those two files already do.
 *
 * --- Sensor: literally the same module as firmware/water-leak-detector/'s
 * own probe --------------------------------------------------------------
 * The classic cheap "rain sensor module" (sold under many names — "FC-37",
 * "YL-83", generic "raindrop detection module" — no single canonical
 * datasheet exists, a widely cloned design, same "best available,
 * cross-checked" sourcing standard already used elsewhere in this repo):
 * a set of parallel PCB traces (the exposed probe — rain bridging adjacent
 * traces lowers the measured resistance, EXACTLY the same physical
 * principle firmware/water-leak-detector/'s own probe already uses)
 * feeding an onboard LM393 comparator against a potentiometer-set
 * threshold, with a digital "DO" output pin. Multiple independent sources
 * confirm this is genuinely the same board hardware sold interchangeably
 * as either a "water sensor" (small board, meant to sit flat in a puddle
 * or leak-prone spot) or a "rain sensor" (larger board, meant to be
 * mounted outdoors facing up) — not a coincidence or an assumption, the
 * underlying comparator circuit and DO polarity are identical: DO goes
 * LOW when wet/raining and HIGH when dry, the module's own comparator
 * actively drives the DO pin both ways (no internal GPIO pull-up needed
 * or used, unlike a passive reed switch). Same "always check your
 * specific module" caveat this repo already applies elsewhere — cheap
 * cloned modules aren't guaranteed identical. Reference wiring: module
 * VCC -> 3.3V, GND -> GND, DO -> RAIN_SENSOR_GPIO.
 *
 * Debounce/edge-handling logic (GPIO ISR + FreeRTOS queue, ANYEDGE since
 * both the rain starting AND stopping are real events worth reporting,
 * ~40ms consistent-level debounce) is reused verbatim from firmware/
 * water-leak-detector/'s own water_leak_task() — the exact same technique
 * applies unchanged to a rain-vs-dry transition.
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
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/boolean-state-server/BooleanStateCluster.h>

static const char *TAG = "matter_rain_sensor";

/* Change this to the GPIO your rain sensor module's DO pin is wired to —
 * see the header comment above for the module type/wiring assumed. GPIO 4
 * matches firmware/water-leak-detector/'s own default GPIO for its single
 * digital sensor input, for consistency across this repo's simple-sensor
 * device types. Adjust to match your board. */
#define RAIN_SENSOR_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
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

static uint16_t rain_sensor_endpoint_id = 0;
static QueueHandle_t rain_sensor_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Current confirmed rain state — true = rain detected (see the header
 * comment on StateValue direction above). Mirrors the Boolean State
 * cluster's StateValue attribute; kept locally too so we only push an
 * attribute update on an actual change, not on every debounced re-read of
 * the same level. */
static bool rain_detected = false;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR rain_sensor_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(rain_sensor_evt_queue, &gpio_num, NULL);
}

/* Same registry-lookup-and-cast pattern firmware/water-leak-detector/'s
 * update_water_leak_state() already established — BooleanState is a
 * "code-driven" cluster class in this SDK version, not the generic
 * ember-style attribute store, so the generic attribute::update() path
 * returns ESP_ERR_NOT_SUPPORTED for it. SetStateValue() also generates the
 * StateChange event itself, so this doesn't need to do that separately. */
static void update_rain_state(uint16_t endpoint_id, bool detected)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, BooleanState::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "BooleanState cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::BooleanStateCluster *>(iface);
    cluster->SetStateValue(detected);
}

/* Debounces the sensor input and, on an actual state change, updates the
 * local BooleanState attribute — same debounce shape as firmware/
 * water-leak-detector/'s own water_leak_task(), reacting to both edges
 * since a rain sensor's DO pin can bounce going wet or drying out again,
 * and either transition is a real event worth reporting. */
static void rain_sensor_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(rain_sensor_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

        /* Debounce: require the pin to read a consistent level for ~40ms
         * (8 x 5ms samples) before treating it as a real, settled state —
         * same reasoning as firmware/water-leak-detector/'s own debounce. */
        int first_level = gpio_get_level((gpio_num_t)io_num);
        bool consistent = true;
        char samples[9] = {0};
        samples[0] = first_level ? 'H' : 'L';
        for (int i = 1; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            int level = gpio_get_level((gpio_num_t)io_num);
            samples[i] = level ? 'H' : 'L';
            if (level != first_level) {
                consistent = false;
            }
        }
        ESP_LOGI(TAG, "Samples (5ms apart): %s (%s)", samples, consistent ? "stable" : "mixed/bouncing");

        /* Whether confirmed or not, flush any further queued edges from
         * this same burst — same reasoning as firmware/
         * water-leak-detector/. */
        xQueueReset(rain_sensor_evt_queue);

        if (!consistent) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously stable");
            continue;
        }

        /* Active-LOW = raining — see the header comment on the sensor
         * module's own polarity (same convention firmware/
         * water-leak-detector/'s own module already uses). */
        bool new_rain_detected = (first_level == 0);
        if (new_rain_detected == rain_detected) {
            ESP_LOGI(TAG, "Debounced level matches current state (%s) — no change",
                     rain_detected ? "RAIN" : "DRY");
            continue;
        }

        rain_detected = new_rain_detected;
        ESP_LOGW(TAG, "Rain state now %s", rain_detected ? "RAIN DETECTED" : "DRY");

        update_rain_state(rain_sensor_endpoint_id, rain_detected);
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

/* Called whenever a controller changes an attribute on this device. A rain
 * sensor has nothing to react to here — StateValue is read-only and only
 * ever written locally by rain_sensor_task() above — so this is a no-op
 * required by node::create()'s callback signature. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
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

/* Fires once the device has stayed powered up for
 * FACTORY_RESET_CONFIRM_DELAY_MS without another reboot — treats this as
 * a normal boot and clears the quick-power-cycle counter. */
static void factory_reset_confirm_timer_cb(void *arg)
{
    nvs_handle_t nvs;
    if (nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, 0);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* Increments the quick-power-cycle boot counter and returns true once it
 * has reached FACTORY_RESET_BOOT_COUNT_THRESHOLD. The caller (app_main())
 * only acts on that later, after Matter has started — see
 * firmware/light/main/app_main.cpp's header comment on why. Must run
 * after nvs_flash_init(). */
static bool check_factory_reset_boot_count(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FACTORY_RESET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS for boot-count tracking: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t boot_count = 0;
    nvs_get_u8(nvs, FACTORY_RESET_NVS_KEY, &boot_count); /* stays 0 if not set yet */
    boot_count++;
    nvs_set_u8(nvs, FACTORY_RESET_NVS_KEY, boot_count);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Quick-power-cycle boot count: %u/%u", boot_count, FACTORY_RESET_BOOT_COUNT_THRESHOLD);

    if (boot_count >= FACTORY_RESET_BOOT_COUNT_THRESHOLD) {
        /* Clear the counter now so a factory-reset reboot can't
         * immediately re-trigger itself. */
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

    /* 1b. Quick-power-cycle factory reset check — see
     * check_factory_reset_boot_count()'s comment above. The actual reset
     * (if due) only happens later, once Matter has started. */
    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the sensor input + its interrupt. ANYEDGE, not NEGEDGE —
     * we need to know about both the rain starting and stopping. No
     * internal pull-up: the sensor module's own comparator actively drives
     * the DO line both ways (see the header comment on the module's own
     * electrical behavior). */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << RAIN_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    /* Settle briefly, then take the pin's boot-time level as the initial
     * state — a rain sensor should report reality from the first
     * commissioned read, not an arbitrary hardcoded "dry" default. */
    vTaskDelay(pdMS_TO_TICKS(50));
    rain_detected = (gpio_get_level(RAIN_SENSOR_GPIO) == 0);
    ESP_LOGI(TAG, "Rain sensor GPIO %d initial level: %d — starting as %s",
             RAIN_SENSOR_GPIO, gpio_get_level(RAIN_SENSOR_GPIO), rain_detected ? "RAIN" : "DRY");

    rain_sensor_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(rain_sensor_task, "rain_sensor_task", 4096, NULL, 10, NULL);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(RAIN_SENSOR_GPIO, rain_sensor_isr_handler, (void *)(uintptr_t)RAIN_SENSOR_GPIO);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
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

    /* 3. Build the Matter data model: one node, one Rain Sensor endpoint,
     * seeded with the boot-time reading above so its first reported state
     * matches physical reality. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    rain_sensor::config_t rain_sensor_config;
    rain_sensor_config.boolean_state.state_value = rain_detected;
    endpoint_t *endpoint = rain_sensor::create(node, &rain_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create rain sensor endpoint");
        return;
    }

    rain_sensor_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Rain sensor endpoint id: %u", rain_sensor_endpoint_id);

    /* 3b. Set the BooleanState cluster's FeatureMap to advertise the
     * ChangeEvent feature — see the header comment above for the real
     * esp-matter gap this works around (the endpoint helper always
     * creates FeatureMap as 0, with no config field to override it) and
     * why it's safe to fix here (the StateChange event itself fires
     * unconditionally either way — this only corrects what the device
     * advertises, it doesn't gate any actual behavior). Must happen
     * before esp_matter::start(), which is what reads FeatureMap into
     * each code-driven cluster's own runtime state. */
    esp_matter_attr_val_t feature_map_val = esp_matter_uint32(chip::to_underlying(BooleanState::Feature::kChangeEvent));
    attribute::update(rain_sensor_endpoint_id, BooleanState::Id, Globals::Attributes::FeatureMap::Id, &feature_map_val);

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

    ESP_LOGI(TAG, "Matter rain sensor started. Scan the QR code to commission.");
}
