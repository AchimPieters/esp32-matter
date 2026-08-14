/*
 * Minimal Matter Ambient Light Sensor (LDR/photoresistor, ADC).
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
 * This repo's sixth device type and first analog/ADC one — light, switch,
 * contact sensor, outlet, and temperature sensor are all digital (GPIO or
 * I2C/single-wire/1-Wire); this one reads a genuinely analog voltage.
 *
 * Not personally hardware-verified in this repo (no LDR on hand when this
 * was written) — implemented carefully from datasheet/reference sources
 * rather than approximated, same standard as firmware/temperature-sensor/'s
 * SHT4x/AHT20/DS18B20/BME280 drivers, but flagging it plainly rather than
 * presenting it as equally proven as the hardware-tested device types.
 *
 * Wiring: a voltage divider — LDR from 3V3 to LIGHT_SENSOR_ADC_GPIO, a fixed
 * resistor (LDR_FIXED_RESISTOR_OHMS below, 10 kOhm to match the reference
 * LDR's own characteristic resistance at 10 lux — see below) from
 * LIGHT_SENSOR_ADC_GPIO to GND. More light -> lower LDR resistance -> higher
 * voltage at the ADC pin. GPIO 34 is ADC1 channel 6 on classic ESP32
 * (WROOM-32) — deliberately an ADC1 pin, not ADC2: ADC2 shares hardware
 * with the Wi-Fi radio and its readings become unreliable once Wi-Fi is
 * active, which this device needs to be commissioned/reachable at all.
 *
 * LDR characterization: the GL5528 is about the most common hobbyist LDR
 * (found in most starter kits) and its own datasheet is used as the
 * reference here — resistance ~10 kOhm at 10 lux (datasheet range is
 * actually 8-20 kOhm; 10 kOhm is a commonly-used nominal midpoint, not a
 * per-unit calibration — expect the absolute lux reading to be a rough
 * estimate, not a calibrated one, same as most hobby light sensors), gamma
 * (the resistance/light slope exponent) 0.7. If you're using a different
 * photoresistor, adjust LDR_R10_OHMS/LDR_GAMMA to match its own datasheet.
 * The resistance-to-lux relationship (a photoresistor's standard
 * characteristic curve) is:
 *
 *   R_LDR = R10 * (10 / lux) ^ gamma        (datasheet form)
 *   lux   = 10 * (R10 / R_LDR) ^ (1 / gamma)  (rearranged, what this uses)
 *
 * Matter's Illuminance Measurement cluster stores MeasuredValue in a
 * logarithmic encoding, not raw lux — this is the same formula the
 * Zigbee ZCL illuminance cluster uses (Matter's Illuminance Measurement
 * cluster has the same lineage): MeasuredValue = 10000 * log10(lux) + 1.
 *
 * Like firmware/contact-sensor/'s Boolean State and firmware/temperature-
 * sensor/'s Temperature/Humidity Measurement clusters, IlluminanceMeasurement
 * is also implemented via the newer "code-driven" cluster class in this SDK
 * version, not the generic ember attribute store — same fix needed
 * (SetMeasuredValue() via the data model provider's registry, not
 * attribute::update()), confirmed the same way: reading
 * IlluminanceMeasurementCluster.h directly rather than guessing.
 */

#include <math.h>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/illuminance-measurement-server/IlluminanceMeasurementCluster.h>

static const char *TAG = "matter_light_sensor";

/* Change this to match your board's wiring. GPIO 34 = ADC1 channel 6 on
 * classic ESP32 (WROOM-32) — see the header comment above for why it has
 * to be an ADC1 pin, not ADC2. */
#define LIGHT_SENSOR_ADC_GPIO GPIO_NUM_34
#define LIGHT_SENSOR_ADC_UNIT ADC_UNIT_1
#define LIGHT_SENSOR_ADC_CHANNEL ADC_CHANNEL_6
#define LIGHT_SENSOR_ADC_ATTEN ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define LIGHT_SENSOR_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT

/* LDR characterization — see the header comment above for the formula and
 * where these numbers come from (GL5528 datasheet nominal values). */
#define LDR_FIXED_RESISTOR_OHMS 10000.0f
#define LDR_R10_OHMS 10000.0f
#define LDR_GAMMA 0.7f
#define LDR_SUPPLY_MV 3300.0f

/* Averaging + reporting interval. Light level changes slowly in practice;
 * averaging several raw samples per reading cuts down on ADC/electrical
 * noise more than reading faster would help. */
#define LIGHT_SENSOR_SAMPLE_COUNT 16
#define LIGHT_SENSOR_MEASURE_INTERVAL_MS 10000

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t light_sensor_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_cali_available = false;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* esp-matter's generic attribute::update() can't write this attribute —
 * see the header comment above for why. Follows the exact pattern
 * firmware/contact-sensor/main/app_main.cpp's update_contact_state()
 * established: look the cluster instance up directly via the data model
 * provider's registry, then call its cluster-specific setter. */
static void update_illuminance(uint16_t endpoint_id, chip::app::DataModel::Nullable<uint16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, IlluminanceMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "IlluminanceMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::IlluminanceMeasurementCluster *>(iface);
    cluster->SetMeasuredValue(value);
}

/* Reads and averages LIGHT_SENSOR_SAMPLE_COUNT raw ADC samples, converts to
 * millivolts via the calibration handle set up in app_main() (falls back to
 * an uncalibrated approximation if calibration failed to initialize, which
 * still works, just less accurately — see adc_setup() below). */
static bool read_adc_millivolts(int *out_mv)
{
    int64_t sum_raw = 0;
    for (int i = 0; i < LIGHT_SENSOR_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, LIGHT_SENSOR_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
            return false;
        }
        sum_raw += raw;
    }
    int avg_raw = (int)(sum_raw / LIGHT_SENSOR_SAMPLE_COUNT);

    if (adc_cali_available) {
        esp_err_t err = adc_cali_raw_to_voltage(adc_cali_handle, avg_raw, out_mv);
        if (err == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s — falling back to uncalibrated estimate",
                 esp_err_to_name(err));
    }

    /* Uncalibrated fallback: assumes a full-scale raw reading corresponds
     * to the nominal supply voltage. Less accurate than the calibrated
     * path, but keeps the sensor working rather than reporting nothing. */
    *out_mv = (int)((float)avg_raw * LDR_SUPPLY_MV / 4095.0f);
    return true;
}

/* Converts a millivolt ADC reading to lux via the voltage divider + LDR
 * characteristic curve described in the header comment above. Returns
 * false if the reading is out of a sane range (e.g. 0mV — a dead short or
 * disconnected sensor — where the resistance math would divide by zero). */
static bool millivolts_to_lux(int mv, float *out_lux)
{
    if (mv <= 0 || mv >= (int)LDR_SUPPLY_MV) {
        return false;
    }

    float v = (float)mv;
    float r_ldr = LDR_FIXED_RESISTOR_OHMS * ((LDR_SUPPLY_MV / v) - 1.0f);
    if (r_ldr <= 0.0f) {
        return false;
    }

    *out_lux = 10.0f * powf(LDR_R10_OHMS / r_ldr, 1.0f / LDR_GAMMA);
    return true;
}

static void sensor_task(void *arg)
{
    for (;;) {
        int mv = 0;
        float lux = 0.0f;
        bool ok = read_adc_millivolts(&mv) && millivolts_to_lux(mv, &lux);

        if (ok) {
            /* Matter's Illuminance Measurement MeasuredValue is a
             * logarithmic encoding, not raw lux — see the header comment
             * above for the formula. Guard against log10(<=0). */
            if (lux < 0.0001f) {
                lux = 0.0001f;
            }
            int32_t encoded = (int32_t)(10000.0 * log10((double)lux) + 1.0);
            if (encoded < 0) {
                encoded = 0;
            } else if (encoded > 0xFFFE) {
                encoded = 0xFFFE;
            }
            ESP_LOGI(TAG, "Light sensor: %d mV -> %.1f lux (MeasuredValue %ld)", mv, (double)lux, (long)encoded);
            update_illuminance(light_sensor_endpoint_id, chip::app::DataModel::Nullable<uint16_t>((uint16_t)encoded));
        } else {
            ESP_LOGW(TAG, "Light sensor reading out of range — check wiring");
            update_illuminance(light_sensor_endpoint_id, chip::app::DataModel::Nullable<uint16_t>());
        }

        vTaskDelay(pdMS_TO_TICKS(LIGHT_SENSOR_MEASURE_INTERVAL_MS));
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

/* Called whenever a controller changes an attribute on this device.
 * MeasuredValue is read-only and only ever written locally by
 * sensor_task() above, so this is a no-op required by node::create()'s
 * callback signature. */
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
        /* Effect variants (breathe, flash-twice, ...) aren't implemented —
         * treat any of them the same as a plain blink rather than guessing
         * at per-variant timing. */
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u — blinking as usual",
                 effect_id, effect_variant, endpoint_id);
        break;
    }
    return ESP_OK;
}

/* Sets up the ADC unit/channel, then tries to set up calibration so raw
 * counts convert to accurate millivolts rather than a rough linear guess.
 * Classic ESP32 only supports the "line fitting" calibration scheme (no
 * curve fitting hardware, unlike later chips) — this follows ESP-IDF's
 * own documented portable pattern (#if .. #elif ..) so the same source
 * still builds correctly if this firmware is ever targeted at a chip that
 * only has curve fitting instead. Calibration failing isn't fatal: falls
 * back to the uncalibrated estimate in read_adc_millivolts() above. */
static bool adc_setup(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = LIGHT_SENSOR_ADC_UNIT;

    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = LIGHT_SENSOR_ADC_ATTEN;
    chan_config.bitwidth = LIGHT_SENSOR_ADC_BITWIDTH;

    err = adc_oneshot_config_channel(adc_handle, LIGHT_SENSOR_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = LIGHT_SENSOR_ADC_UNIT;
    cali_config.chan = LIGHT_SENSOR_ADC_CHANNEL;
    cali_config.atten = LIGHT_SENSOR_ADC_ATTEN;
    cali_config.bitwidth = LIGHT_SENSOR_ADC_BITWIDTH;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    adc_cali_available = (err == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {};
    cali_config.unit_id = LIGHT_SENSOR_ADC_UNIT;
    cali_config.atten = LIGHT_SENSOR_ADC_ATTEN;
    cali_config.bitwidth = LIGHT_SENSOR_ADC_BITWIDTH;
#if CONFIG_IDF_TARGET_ESP32
    cali_config.default_vref = (uint32_t)LDR_SUPPLY_MV;
#endif
    err = adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle);
    adc_cali_available = (err == ESP_OK);
#else
    err = ESP_ERR_NOT_SUPPORTED;
    adc_cali_available = false;
#endif

    if (!adc_cali_available) {
        ESP_LOGW(TAG, "ADC calibration unavailable (%s) — using uncalibrated voltage estimate",
                 esp_err_to_name(err));
    }

    return true;
}

extern "C" void app_main(void)
{
    /* 1. Init NVS — stores the Matter fabric keys and factory data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Set up the ADC. */
    if (!adc_setup()) {
        ESP_LOGE(TAG, "ADC setup failed");
        return;
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

    /* 3. Build the Matter data model: one node, one Light Sensor endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    light_sensor::config_t light_sensor_config;
    endpoint_t *endpoint = light_sensor::create(node, &light_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create light sensor endpoint");
        return;
    }
    light_sensor_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Light sensor endpoint id: %u", light_sensor_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* 5. Start reading the sensor now that the data model + Matter stack
     * both exist — sensor_task() writes into the cluster created above. */
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter light sensor started. Scan the QR code to commission.");
}
