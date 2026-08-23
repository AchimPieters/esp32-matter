/*
 * Minimal Matter Flow Sensor — thirty-fourth device type.
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
 * --- Endpoint: esp-matter's own complete top-level helper ------------------
 * `endpoint::flow_sensor::create()` (device type 0x0306) confirmed complete/
 * ready-to-use — Identify + FlowMeasurement, auto-Descriptor via `common::
 * create<T>()` — by reading esp_matter_endpoint.cpp's own `flow_sensor::
 * add()` directly. Confirmed against the CSA's own data_model/1.6/
 * device_types/FlowSensor.xml: those two clusters are the ONLY ones listed
 * at all, both `<mandatoryConform/>` — the same minimal shape firmware/
 * pressure-sensor/'s own device type XML already established.
 *
 * --- FlowMeasurement: a "code-driven" cluster, registry-lookup setter ------
 * Confirmed by reading esp-matter's own source directly: a real
 * `flow_measurement/` folder exists under `data_model_provider/clusters/`,
 * backed by connectedhomeip's own `FlowMeasurementCluster` — same category
 * as firmware/pressure-sensor/'s PressureMeasurement and firmware/
 * temperature-sensor/'s TemperatureMeasurement. `SetMeasuredValue()` is a
 * plain method (not a Delegate), so this uses the same registry-lookup-
 * and-cast pattern those files' own setters already establish.
 *
 * --- MeasuredValue encoding: m3/h, resolution 0.1 m3/h ----------------------
 * Matter's FlowMeasurement cluster XML doesn't spell out the unit in its
 * machine-readable form either (same gap firmware/pressure-sensor/'s own
 * header comment already documents for PressureMeasurement — inherited
 * from the Zigbee ZCL Flow Measurement cluster, whose own spec does state
 * it) — confirmed instead against the same real, independent source already
 * used for that file: Home Assistant's own Matter integration
 * (`homeassistant/components/matter/sensor.py`, its FlowMeasurement
 * discovery schema) divides the raw MeasuredValue by 10 and reports the
 * result in m3/h (cubic meters per hour) — i.e. `MeasuredValue = flow_m3h *
 * 10`.
 *
 * --- Sensor: a Hall-effect pulse-output flow sensor (YF-S201-class) --------
 * The classic cheap water-flow sensor almost every hobbyist project uses —
 * a small pinwheel/Hall-effect assembly that outputs one open-collector
 * pulse per partial rotation, frequency proportional to flow rate. No
 * single canonical datasheet exists (a widely cloned design sold under the
 * YF-S201 name by many different manufacturers, same "best available,
 * cross-checked across multiple independent sources" sourcing standard
 * already used in this repo for e.g. the contact sensor's reed switch or
 * the occupancy sensor's PIR module) — the pulse characteristic
 * `F(Hz) = 7.5 * Q(L/min)` and the 1-30 L/min rated flow range are both
 * consistently documented across every independent source checked.
 * `FLOW_SENSOR_PULSE_GPIO` counts rising edges via a GPIO ISR — the same
 * pulse-counting technique firmware/outlet/'s own BL0937/HLW8012/CSE7759
 * power-monitor drivers already establish in this repo (a plain `volatile
 * uint32_t` edge counter incremented from an `IRAM_ATTR` ISR, read and
 * reset once per sampling window with no critical section — a torn read
 * isn't practically possible for a simple increment counter on this
 * architecture, same reasoning that pattern's own existing code already
 * relies on). Every `FLOW_SENSOR_SAMPLE_INTERVAL_MS` window: frequency =
 * pulses / window_seconds; flow (L/min) = frequency / 7.5; MeasuredValue =
 * round(flow_L/min * 0.6) (the L/min-to-m3/h-times-10 conversion folds to
 * a single multiply: L/min * 60 min/h / 1000 L/m3 * 10 = L/min * 0.6).
 * Zero pulses in a window is reported as a real MeasuredValue of 0 (no
 * flow) rather than null — unlike a bus-based sensor (DS18B20, I2C), a
 * passive pulse GPIO has no way to distinguish "sensor absent" from
 * "genuinely zero flow", so this file doesn't attempt to. Min/
 * MaxMeasuredValue (1 / 18, i.e. 0.1/1.8 m3/h) are set directly from the
 * sensor's own rated 1-30 L/min range — same "use real hardware limits for
 * Min/Max" precedent firmware/pressure-sensor/'s own BMP280 operating-range
 * bounds already establish; MeasuredValue itself is reported as computed,
 * not clamped to this range (a genuine zero-flow reading is valid and
 * useful even though it's below the sensor's own rated minimum).
 * Not hardware-tested (no YF-S201-class sensor physically available when
 * written) — flagged accordingly, same as this repo's other unverified
 * sensor drivers.
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
#include <app/clusters/flow-measurement-server/FlowMeasurementCluster.h>

static const char *TAG = "matter_flow_sensor";

/* Change this to the GPIO your flow sensor's pulse output is wired to.
 * GPIO 4 is commonly unused on classic ESP32 (WROOM-32) devkits — adjust to
 * match your board. Most YF-S201-class sensors have an actively-driven
 * (open-collector-with-pullup or push-pull, depending on the specific
 * module) output — this file enables the internal pull-up regardless,
 * which is harmless either way. */
#define FLOW_SENSOR_PULSE_GPIO GPIO_NUM_4

#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* YF-S201-class pulse characteristic — see the header comment above. */
#define FLOW_SENSOR_PULSES_PER_HZ_PER_LPM 7.5f

/* How often the pulse count is turned into a MeasuredValue report. Longer
 * than 1s to keep quantization error reasonable at low flow rates (a
 * 1 L/min flow is only ~7.5 pulses/sec — a short window would round too
 * coarsely). */
#define FLOW_SENSOR_SAMPLE_INTERVAL_MS 2000

/* Output range (0.1 m3/h units == MeasuredValue, see the header comment
 * above) — directly from the sensor's own rated 1-30 L/min flow range. */
#define FLOW_SENSOR_MIN_MEASURED_VALUE 1  /* 1 L/min -> 0.1 m3/h */
#define FLOW_SENSOR_MAX_MEASURED_VALUE 18 /* 30 L/min -> 1.8 m3/h */

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t flow_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Pulse counter — see the header comment above for why no critical section
 * is needed around reading/resetting it. */
static volatile uint32_t flow_pulse_count = 0;

static void IRAM_ATTR flow_pulse_isr(void *arg)
{
    flow_pulse_count++;
}

static bool flow_pulse_gpio_setup(void)
{
    gpio_config_t pulse_conf = {};
    pulse_conf.pin_bit_mask = (1ULL << FLOW_SENSOR_PULSE_GPIO);
    pulse_conf.mode = GPIO_MODE_INPUT;
    pulse_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    pulse_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&pulse_conf);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
        return false;
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(FLOW_SENSOR_PULSE_GPIO, flow_pulse_isr, NULL);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
        return false;
    }
    return true;
}

/* Same registry-lookup-and-cast pattern firmware/pressure-sensor/'s own
 * update_pressure() already establishes — FlowMeasurement is a "code-
 * driven" cluster class in this SDK version, not the generic ember-style
 * attribute store. */
static void update_flow(uint16_t endpoint_id, uint16_t measured_value)
{
    chip::app::ConcreteClusterPath path(endpoint_id, FlowMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "FlowMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }
    static_cast<FlowMeasurementCluster *>(iface)->SetMeasuredValue(chip::app::DataModel::Nullable<uint16_t>(measured_value));
}

/* Periodically turns the pulse count accumulated over the last window into
 * a flow rate and reports it — see the header comment above for the full
 * conversion detail. */
static void flow_task(void *arg)
{
    if (!flow_pulse_gpio_setup()) {
        ESP_LOGE(TAG, "Flow sensor GPIO setup failed — flow task exiting, no readings will be reported");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        flow_pulse_count = 0;
        vTaskDelay(pdMS_TO_TICKS(FLOW_SENSOR_SAMPLE_INTERVAL_MS));

        uint32_t count = flow_pulse_count;
        float window_s = FLOW_SENSOR_SAMPLE_INTERVAL_MS / 1000.0f;
        float frequency_hz = (float)count / window_s;
        float flow_lpm = frequency_hz / FLOW_SENSOR_PULSES_PER_HZ_PER_LPM;
        uint16_t measured_value = (uint16_t)(flow_lpm * 0.6f + 0.5f); /* see header comment: L/min * 0.6 = MeasuredValue */

        update_flow(flow_endpoint_id, measured_value);
        ESP_LOGI(TAG, "Flow: %.2f L/min (%lu pulses, MeasuredValue=%u)", flow_lpm, (unsigned long)count, measured_value);
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

/* No controller-writable attributes on this device — all state flows from
 * the flow task above. */
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

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

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

    /* 2. Configure the identify LED + its blink timer (not started yet —
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

    /* 3. Build the Matter data model: one node, one Flow Sensor endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::flow_sensor::config_t flow_config;
    flow_config.flow_measurement.min_measured_value = nullable<uint16_t>((uint16_t)FLOW_SENSOR_MIN_MEASURED_VALUE);
    flow_config.flow_measurement.max_measured_value = nullable<uint16_t>((uint16_t)FLOW_SENSOR_MAX_MEASURED_VALUE);
    endpoint_t *endpoint = endpoint::flow_sensor::create(node, &flow_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create flow sensor endpoint");
        return;
    }

    flow_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Flow sensor endpoint id: %u", flow_endpoint_id);

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

    /* 5. Start the flow polling task — inits the pulse-counting GPIO and
     * reports readings for as long as the device runs. */
    xTaskCreate(flow_task, "flow_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter flow sensor started. Scan the QR code to commission.");
}
