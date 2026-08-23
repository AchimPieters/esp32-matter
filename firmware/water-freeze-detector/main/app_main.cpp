/*
 * Minimal Matter Water Freeze Detector — thirty-sixth device type, and the
 * closest sibling to firmware/water-leak-detector/ in this repo: same
 * BooleanState cluster, same "true = alarm/problem" StateValue direction,
 * same real esp-matter FeatureMap gap and fix — but a temperature-threshold
 * classifier instead of a debounced digital sensor input, since freezing is
 * a physical condition a plain digital probe can't detect on its own.
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
 * Confirmed directly via esp-matter's own `water_freeze_detector::add()` in
 * esp_matter_endpoint.cpp: identical structure to `water_leak_detector::
 * add()`/`contact_sensor::add()` (Identify + BooleanState + StateChange
 * event, via `common::create<T>()` so the endpoint's Descriptor cluster is
 * created automatically) — matches the CSA's own data_model/1.6/
 * device_types/WaterFreezeDetector.xml exactly (Identify mandatory,
 * BooleanState mandatory with its ChangeEvent feature mandatory as of the
 * device type's own revision 2, an optional BooleanStateConfiguration
 * cluster not implemented here — same "smallest reasonable next step"
 * scope cut firmware/water-leak-detector/'s own header comment already
 * applies to that same optional cluster).
 *
 * --- StateValue semantic: same "true = alarm/problem" direction as
 * firmware/water-leak-detector/ ---------------------------------------
 * Confirmed against real-world Matter tooling, not assumed: Espressif's own
 * `MatterWaterFreezeDetector` Arduino-ESP32 class exposes `setFreeze(bool)`,
 * documented as setting `true` when a freeze condition IS detected — the
 * same "true = alarm/problem" direction firmware/water-leak-detector/'s own
 * header comment already documents for `setLeak(true)`, not the "true =
 * normal physical state" direction firmware/contact-sensor/'s StateValue
 * uses. Boots with StateValue false (no freeze condition assumed until a
 * real reading confirms otherwise) — a deliberate, safe difference from
 * firmware/water-leak-detector/'s own boot-time behavior (which seeds
 * StateValue from an immediate real GPIO read, since a passive sensor's
 * boot-time level already reflects physical reality); a temperature
 * threshold classifier instead needs one real DS18B20 conversion cycle
 * first, so it can't honestly report anything before that completes.
 *
 * --- The same real, documented esp-matter FeatureMap gap firmware/
 * water-leak-detector/ already found — and the identical fix -------------
 * Confirmed by reading `boolean_state::create()` in esp-matter's own
 * esp_matter_cluster.cpp directly (the exact same function firmware/
 * water-leak-detector/'s endpoint is built from too): it hardcodes
 * `global::attribute::create_feature_map(cluster, 0)`, with no config field
 * to override it — so esp-matter's own `water_freeze_detector::add()`
 * reference implementation never actually sets the ChangeEvent (CHGEVENT)
 * feature bit this device type's own spec (revision 2) makes mandatory.
 * Same fix as firmware/water-leak-detector/'s own header comment documents
 * in full: `boolean_state::event::create_state_change()` fires the
 * StateChange event unconditionally, with no feature-flag gate — confirmed
 * by reading its own implementation — so this FeatureMap bit is pure
 * advertised-conformance metadata here, safe to overwrite directly via
 * `attribute::update()` after endpoint creation but before
 * `esp_matter::start()`, exactly like that file already does.
 *
 * --- Sensor: DS18B20 + an adjustable threshold, not specialized hardware --
 * Unlike firmware/water-leak-detector/'s cheap comparator probe module
 * (which reports a real, already-digital wet/dry signal), there is no
 * equally common, cheap, hobby-accessible "freeze switch" module this repo
 * could point to with the same confidence — real commercial pipe-freeze
 * alarms mostly use a factory-preset bimetallic thermostat switch, a
 * specialized part with no single canonical hobbyist source. This file
 * instead reuses the exact DS18B20 1-Wire driver already established
 * across this repo's other appliance/HVAC device types verbatim, paired
 * with a plain adjustable threshold + hysteresis classifier — the same
 * "adjustable threshold, not a calibrated reading" precedent firmware/
 * smoke-co-alarm/'s and firmware/air-quality-sensor/'s own MQ-series/CCS811
 * classifiers already establish. `WATER_FREEZE_DETECTOR_THRESHOLD_
 * CENTIDEGREES` defaults to 3.00 degC — a few degrees above the actual 0
 * degC freezing point, matching how real freeze-warning products commonly
 * trip early to leave response time before ice actually forms — and
 * `WATER_FREEZE_DETECTOR_HYSTERESIS_CENTIDEGREES` (0.3 degC) matches this
 * repo's own established hysteresis-band convention elsewhere. StateValue
 * goes true once the reading drops to/below the threshold, and only clears
 * once it rises to/above threshold + hysteresis — the same asymmetric
 * "don't switch back and forth right at the line" bang-bang logic every
 * other control loop in this repo already uses, applied here to a plain
 * alarm classifier instead of an actuator.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/boolean-state-server/BooleanStateCluster.h>

static const char *TAG = "matter_water_freeze";

/* Change this to the GPIO your DS18B20's DATA pin is wired to. GPIO 4
 * matches firmware/water-leak-detector/'s own default GPIO for its single
 * digital sensor input, for consistency across this repo's simple-sensor
 * device types. Adjust to match your board. */
#define WATER_FREEZE_DETECTOR_SENSOR_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Freeze-alarm threshold (Matter's global `temperature` type — int16,
 * hundredths of a degree C). 3.00 degC — see the header comment above for
 * why this is deliberately above the actual 0 degC freezing point.
 * Adjustable, not a calibrated reading. */
#define WATER_FREEZE_DETECTOR_THRESHOLD_CENTIDEGREES 300

/* Bang-bang (hysteresis) band around the threshold — same 0.3 degC default
 * this repo's other control loops already use. */
#define WATER_FREEZE_DETECTOR_HYSTERESIS_CENTIDEGREES 30

/* How often the sensor is read and the alarm state re-evaluated — a
 * freezing pipe develops over minutes, not seconds, so this doesn't need
 * to poll quickly. */
#define WATER_FREEZE_DETECTOR_POLL_INTERVAL_MS 10000

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t water_freeze_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Current confirmed freeze-alarm state — true = freeze condition detected
 * (see the header comment on why this is the same "true = alarm/problem"
 * direction firmware/water-leak-detector/'s own StateValue already uses).
 * Kept locally too so an attribute update is only pushed on an actual
 * change, not on every poll. */
static bool water_freeze_detected = false;

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/heat-pump/'s (itself firmware/
 * room-air-conditioner/'s / firmware/thermostat/'s) DS18B20 driver — see
 * those files' own header comments for the 1-Wire timing/CRC detail and
 * sourcing. */
static bool ow_reset(void)
{
    gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO);
    esp_rom_delay_us(50);
    return bit;
}

static void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte = (uint8_t)(byte | (ow_read_bit() << i));
    }
    return byte;
}

/* Dallas/Maxim 1-Wire CRC-8 (reflected, polynomial 0x8C, init 0x00). */
static uint8_t onewire_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int b = 0; b < 8; b++) {
            uint8_t mix = (uint8_t)((crc ^ byte) & 0x01);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}

static bool sensor_setup(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << WATER_FREEZE_DETECTOR_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(WATER_FREEZE_DETECTOR_SENSOR_GPIO, 1);
    return true;
}

static bool sensor_read(float *temperature_c)
{
    portDISABLE_INTERRUPTS();
    bool present = ow_reset();
    if (present) {
        ow_write_byte(0xCC); /* Skip ROM */
        ow_write_byte(0x44); /* Convert T */
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset — check wiring/pull-up");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(750)); /* max conversion time at default 12-bit resolution */

    portDISABLE_INTERRUPTS();
    present = ow_reset();
    uint8_t scratchpad[9] = {0};
    if (present) {
        ow_write_byte(0xCC);
        ow_write_byte(0xBE); /* Read Scratchpad */
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = ow_read_byte();
        }
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset (read phase)");
        return false;
    }

    if (onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGW(TAG, "DS18B20 CRC mismatch — discarding reading");
        return false;
    }

    int16_t raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    *temperature_c = raw * 0.0625f; /* 12-bit default resolution: 1 LSB = 1/16 degC */
    return true;
}

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Same registry-lookup-and-cast pattern firmware/water-leak-detector/'s
 * update_water_leak_state() already established — BooleanState is a
 * "code-driven" cluster class in this SDK version, not the generic
 * ember-style attribute store, so the generic attribute::update() path
 * returns ESP_ERR_NOT_SUPPORTED for it. SetStateValue() also generates the
 * StateChange event itself, so this doesn't need to do that separately. */
static void update_water_freeze_state(uint16_t endpoint_id, bool freeze_detected)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, BooleanState::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "BooleanState cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::BooleanStateCluster *>(iface);
    cluster->SetStateValue(freeze_detected);
}

/* Periodically reads the sensor and re-evaluates the freeze-alarm hysteresis
 * — see the header comment above for the full threshold/hysteresis detail.
 * A failed read is treated the same way firmware/thermostat/'s own control
 * loop treats a sensor failure: no confident alarm decision can be made, so
 * this leaves the current alarm state exactly as it was rather than
 * guessing either way. */
static void water_freeze_task(void *arg)
{
    for (;;) {
        float temperature_c = 0.0f;

        if (sensor_read(&temperature_c)) {
            int16_t measured_centidegrees = (int16_t)(temperature_c * 100.0f);
            bool new_freeze_detected = water_freeze_detected;

            if (measured_centidegrees <= WATER_FREEZE_DETECTOR_THRESHOLD_CENTIDEGREES) {
                new_freeze_detected = true;
            } else if (measured_centidegrees >= WATER_FREEZE_DETECTOR_THRESHOLD_CENTIDEGREES + WATER_FREEZE_DETECTOR_HYSTERESIS_CENTIDEGREES) {
                new_freeze_detected = false;
            }

            ESP_LOGI(TAG, "Sensor: %.2f degC", temperature_c);
            if (new_freeze_detected != water_freeze_detected) {
                water_freeze_detected = new_freeze_detected;
                ESP_LOGW(TAG, "Water freeze state now %s", water_freeze_detected ? "FREEZE DETECTED" : "normal");
                update_water_freeze_state(water_freeze_endpoint_id, water_freeze_detected);
            }
        } else {
            ESP_LOGW(TAG, "Sensor read failed — alarm state unchanged");
        }

        vTaskDelay(pdMS_TO_TICKS(WATER_FREEZE_DETECTOR_POLL_INTERVAL_MS));
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

/* Called whenever a controller changes an attribute on this device. A water
 * freeze detector has nothing to react to here — StateValue is read-only
 * and only ever written locally by water_freeze_task() above — so this is
 * a no-op required by node::create()'s callback signature. */
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

    /* 2. Configure the DS18B20 sensor pin. */
    sensor_setup();

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

    /* 3. Build the Matter data model: one node, one Water Freeze Detector
     * endpoint — boots with StateValue false, see the header comment above
     * for why this device type can't honestly seed a real boot-time
     * reading the way firmware/water-leak-detector/'s own GPIO-based sensor
     * can. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    water_freeze_detector::config_t water_freeze_config;
    water_freeze_config.boolean_state.state_value = false;
    endpoint_t *endpoint = water_freeze_detector::create(node, &water_freeze_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create water freeze detector endpoint");
        return;
    }

    water_freeze_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Water freeze detector endpoint id: %u", water_freeze_endpoint_id);

    /* 3b. Set the BooleanState cluster's FeatureMap to advertise the
     * ChangeEvent feature — see the header comment above for the real
     * esp-matter gap this works around and why it's safe to fix here. Must
     * happen before esp_matter::start(), which is what reads FeatureMap
     * into each code-driven cluster's own runtime state. */
    esp_matter_attr_val_t feature_map_val = esp_matter_uint32(chip::to_underlying(BooleanState::Feature::kChangeEvent));
    attribute::update(water_freeze_endpoint_id, BooleanState::Id, Globals::Attributes::FeatureMap::Id, &feature_map_val);

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

    /* 5. Start reading the sensor now that the data model + Matter stack
     * both exist — water_freeze_task() writes into the cluster created
     * above. */
    xTaskCreate(water_freeze_task, "water_freeze_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter water freeze detector started. Scan the QR code to commission.");
}
