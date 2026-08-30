/*
 * Minimal Matter Soil Sensor — thirty-seventh device type, and this repo's
 * first over the Soil Measurement cluster — which turned out to hide a
 * genuinely new, more severe class of esp-matter gap than anything found
 * in this repo before: skip the one required app-level call this file
 * makes and the device doesn't misbehave quietly, it hard-crashes at
 * startup (see below).
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
 * `endpoint::soil_sensor::create()` (device type 0x0430) confirmed complete/
 * ready-to-use — Identify + SoilMeasurement, auto-Descriptor via `common::
 * create<T>()` — by reading esp_matter_endpoint.cpp's own `soil_sensor::
 * add()` directly. Confirmed against the CSA's own data_model/1.6/
 * device_types/SoilSensor.xml: Identify and SoilMeasurement are
 * `<mandatoryConform/>`; an optional TemperatureMeasurement cluster is
 * also listed — originally not implemented here (real cheap capacitive
 * soil-moisture probes, see below, don't carry a temperature element at
 * all), later added as an independently checkable option backed by a
 * separately wired DS18B20 probe; see the "Later extended" section
 * further down this comment for the full detail.
 *
 * --- SoilMeasurement: a genuinely new, more severe class of esp-matter gap
 * than anything found in this repo before -----------------------------
 * Confirmed by reading `soil_measurement::create()` in esp-matter's own
 * esp_matter_cluster.cpp directly: `soil_measurement::config_t` is a
 * literally empty `common::config_t` (no fields at all), and `create()`
 * only ever calls `attribute::create_soil_moisture_measurement_limits
 * (cluster, NULL, 0, 0)` — a plain ember-attribute shell with no real data
 * behind it — before registering `ESPMatterSoilMeasurementClusterServer
 * InitCallback` as this cluster's own init hook (fired later, during
 * `esp_matter::start()`, same timing as every other code-driven cluster in
 * this repo). Reading THAT callback's own source directly
 * (`data_model_provider/clusters/soil_measurement/integration.cpp`) is
 * where the real severity shows up: it contains a literal
 * `VerifyOrDieWithMsg(gLimits.find(endpointId) != gLimits.end(), ...)` —
 * if this file doesn't call the free function
 * `chip::app::Clusters::SoilMeasurement::SetSoilMoistureLimits()` for this
 * endpoint BEFORE `esp_matter::start()` runs, the device does not
 * silently misbehave (like every other FeatureMap-class gap this repo has
 * catalogued so far) — it calls `VerifyOrDieWithMsg`, which aborts the
 * whole firmware outright. This is a real, previously undocumented class
 * of "must-call-or-crash" requirement in this SDK version, worth
 * remembering for any future code-driven cluster whose real C++
 * construction is deferred to an init callback like this one: read that
 * callback's own source directly rather than assuming a missing call is
 * merely a silent gap the way it has been every other time in this repo.
 * `SetSoilMoistureLimits()` is therefore called in `app_main()` right
 * after building the endpoint, passing a real `Globals::Structs::
 * MeasurementAccuracyStruct::Type` (measurementType =
 * `MeasurementTypeEnum::kSoilMoisture`, confirmed against connectedhomeip's
 * own generated shared `Enums.h`; measured = true; minMeasuredValue = 0,
 * maxMeasuredValue = 100, matching `SoilMoistureMeasuredValue`'s own
 * `percent` type; an empty `accuracyRanges` list, since no per-range
 * accuracy figures are being claimed). `SetSoilMoistureMeasuredValue()` is
 * the real setter used afterward to report readings — confirmed by
 * reading `SoilMeasurementCluster.h` directly, a plain method (not a
 * Delegate), reached via esp-matter's own ready-made free function rather
 * than this repo's usual registry-lookup-and-cast pattern — the same
 * "esp-matter's own integration.cpp provides a convenience free function"
 * category firmware/air-purifier/'s own `ResourceMonitoring::
 * GetClusterInstance()` already established, just for a cluster this repo
 * hasn't used before.
 *
 * --- Sensor: a cheap capacitive soil-moisture probe (analog output) -------
 * The near-ubiquitous hobbyist "Capacitive Soil Moisture Sensor v1.2"
 * board — a capacitive sense element (immune to the corrosion a cheap
 * resistive two-prong probe suffers from constant DC current through wet
 * soil) feeding an onboard 555-timer-class oscillator/rectifier that
 * outputs an analog voltage inversely proportional to moisture (dry soil
 * reads a HIGHER voltage, saturated/wet soil reads a LOWER one) — no
 * single canonical datasheet exists (a widely cloned design, same "best
 * available, cross-checked" sourcing standard this repo already applies
 * to e.g. firmware/water-leak-detector/'s own probe module or firmware/
 * occupancy-sensor/'s PIR module). Read via the same `esp_adc/
 * adc_oneshot.h` + `esp_adc/adc_cali.h` pattern firmware/light-sensor/'s
 * own LDR driver already establishes (ADC1, not ADC2 — ADC2 is unreliable
 * once Wi-Fi is active, which this device needs to be commissioned at
 * all), including the same portable `#if ADC_CALI_SCHEME_CURVE_FITTING_
 * SUPPORTED / #elif ..._LINE_FITTING_SUPPORTED` calibration-scheme
 * pattern.
 *
 * Unlike firmware/light-sensor/'s own LDR conversion (a real photoresistor
 * characteristic curve, grounded in a manufacturer datasheet), this
 * sensor's raw voltage-to-moisture mapping is NOT chip-datasheet-driven at
 * all — every independent source checked agrees the actual dry/wet
 * voltage range varies meaningfully by specific board batch/capacitance
 * and even by soil type, so real-world use of this exact sensor class
 * universally involves a simple two-point field calibration instead: probe
 * in dry air = 0%, probe fully submerged in water = 100%.
 * `SOIL_SENSOR_DRY_MV`/`SOIL_SENSOR_WET_MV` below are therefore explicitly
 * adjustable placeholder defaults (2800mV dry / 1200mV wet at a 3.3V
 * supply — representative figures commonly cited across multiple
 * independent hobbyist sources for this exact board, not a measured
 * calibration performed against real hardware in this repo), meant to be
 * replaced with your own two-point calibration rather than trusted as-is
 * — same "adjustable, not a calibrated reading" honesty precedent
 * firmware/smoke-co-alarm/'s and firmware/air-quality-sensor/'s own
 * threshold classifiers already establish, applied here to a linear
 * two-point scale instead of a single threshold.
 *
 * --- Later extended: TemperatureMeasurement, on request (continuing the
 * `clusterOptions` rollout firmware/cooktop/ and firmware/pump/ started) --
 * This file's own header comment above already named the gap and the
 * reason: real cheap capacitive soil-moisture probes don't carry a
 * temperature element at all. That's still true of the probe itself —
 * this addition is a SEPARATE sensor, not something extracted from the
 * moisture probe's own signal: an independently wired DS18B20 waterproof
 * 1-Wire probe, the exact same driver already reused across firmware/
 * thermostat/, firmware/water-heater/, and firmware/pump/ — a real, common
 * pairing for soil monitoring (soil temperature meaningfully affects both
 * plant health and how moisture readings should be interpreted, and a
 * waterproof DS18B20 probe is designed to be buried, unlike the fragile
 * capacitive sensor board above). `SOIL_SENSOR_HAS_TEMPERATURE_
 * MEASUREMENT` (default off, unchanged default build) adds
 * TemperatureMeasurement onto this SAME Soil Sensor endpoint via the usual
 * "add extra clusters onto an already-correct endpoint" pattern —
 * confirmed to need none of SoilMeasurement's own "must-call-or-crash"
 * complexity documented above: TemperatureMeasurement is a plain code-
 * driven cluster with the same registry-lookup-and-cast
 * `SetMeasuredValue()` setter every other TemperatureMeasurement instance
 * in this repo already uses, no special init-order requirement. The
 * existing `soil_sensor_task`'s own 30s poll loop reads and reports the
 * DS18B20 alongside the moisture sensor each cycle — no second task
 * needed, since both readings change slowly and there's no reason to
 * poll temperature on a different cadence.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#include <esp_matter.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <data_model_provider/clusters/soil_measurement/integration.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>

static const char *TAG = "matter_soil_sensor";

/* Change this to the GPIO your soil sensor's analog output (AOUT) is wired
 * to. GPIO 34 is ADC1 channel 6 on classic ESP32 (WROOM-32) — deliberately
 * ADC1, not ADC2, since ADC2 is unreliable once Wi-Fi is active (see the
 * header comment above). Adjust to match your board; must stay an
 * ADC1-capable pin (GPIO 32-39 on classic ESP32). */
#define SOIL_SENSOR_PIN GPIO_NUM_34
#define SOIL_SENSOR_ADC_UNIT ADC_UNIT_1
#define SOIL_SENSOR_ADC_CHANNEL ADC_CHANNEL_6
#define SOIL_SENSOR_ADC_ATTEN ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define SOIL_SENSOR_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define SOIL_SENSOR_SUPPLY_MV 3300.0f
#define SOIL_SENSOR_SAMPLE_COUNT 16 /* averaged per reading, same as firmware/light-sensor/'s own LDR path */

/* Two-point calibration — see the header comment above for why this
 * sensor class needs one, and why these particular numbers are only
 * representative placeholders, not a measured calibration.
 * SOIL_SENSOR_DRY_MV is the reading with the probe in dry air;
 * SOIL_SENSOR_WET_MV is the reading with the probe fully submerged in
 * water. Deliberately NOT inline-commented on their own #define lines
 * (unlike most of this file's other constants) — the product wizard's
 * own generated sed command for each rewrites the WHOLE line (a broad
 * `.*` match, since these carry a real calibrated millivolt value rather
 * than a fixed sentinel), which would silently strip a trailing inline
 * comment the first time a product actually changes either value — the
 * same real, previously-caught bug firmware/rf-ir-bridge/'s own header
 * comment documents in full for its own two GPIO defines. */
#define SOIL_SENSOR_DRY_MV 2800
#define SOIL_SENSOR_WET_MV 1200

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Soil moisture changes slowly — no need to poll faster than this. */
#define SOIL_SENSOR_POLL_INTERVAL_MS 30000

/* --- Optional TemperatureMeasurement (DS18B20 probe) — see the header
 * comment above for the full sourcing/reuse detail. Default off,
 * unchanged default build. */
#define SOIL_SENSOR_HAS_TEMPERATURE_MEASUREMENT 0
#define SOIL_SENSOR_TEMP_SENSOR_GPIO GPIO_NUM_4 /* DS18B20 1-Wire data pin */
#define SOIL_SENSOR_TEMP_MIN_CENTIDEGREES (-5500) /* -55.00 degC — DS18B20's own rated range */
#define SOIL_SENSOR_TEMP_MAX_CENTIDEGREES 12500   /* 125.00 degC — DS18B20's own rated range */

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t soil_sensor_endpoint_id = 0;
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

/* Reads and averages SOIL_SENSOR_SAMPLE_COUNT raw ADC samples, converts to
 * millivolts via the calibration handle set up in sensor_setup() below —
 * same pattern firmware/light-sensor/'s own read_adc_millivolts()
 * already establishes. Falls back to an uncalibrated linear estimate if
 * calibration failed to initialize, which still works, just less
 * accurately. */
static bool read_adc_millivolts(int *out_mv)
{
    int64_t sum_raw = 0;
    for (int i = 0; i < SOIL_SENSOR_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, SOIL_SENSOR_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
            return false;
        }
        sum_raw += raw;
    }
    int avg_raw = (int)(sum_raw / SOIL_SENSOR_SAMPLE_COUNT);

    if (adc_cali_available) {
        esp_err_t err = adc_cali_raw_to_voltage(adc_cali_handle, avg_raw, out_mv);
        if (err == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s — falling back to uncalibrated estimate",
                 esp_err_to_name(err));
    }

    *out_mv = (int)((float)avg_raw * SOIL_SENSOR_SUPPLY_MV / 4095.0f);
    return true;
}

/* Converts a millivolt ADC reading to a 0-100% moisture value via the
 * two-point calibration described in the header comment above — a plain
 * linear interpolation (inverted, since drier soil reads a HIGHER
 * voltage), clamped to the valid range. */
static uint8_t millivolts_to_percent(int mv)
{
    if (mv >= SOIL_SENSOR_DRY_MV) {
        return 0;
    }
    if (mv <= SOIL_SENSOR_WET_MV) {
        return 100;
    }
    float fraction = (float)(SOIL_SENSOR_DRY_MV - mv) / (float)(SOIL_SENSOR_DRY_MV - SOIL_SENSOR_WET_MV);
    return (uint8_t)(fraction * 100.0f + 0.5f);
}

/* Sets up the ADC unit/channel, then tries to set up calibration — same
 * portable curve-fitting/line-fitting pattern firmware/light-sensor/'s
 * own sensor_setup() already establishes (see that file's own header
 * comment for why classic ESP32 only supports line fitting). Calibration
 * failing isn't fatal: falls back to the uncalibrated estimate in
 * read_adc_millivolts() above. */
static bool sensor_setup(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = SOIL_SENSOR_ADC_UNIT;

    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = SOIL_SENSOR_ADC_ATTEN;
    chan_config.bitwidth = SOIL_SENSOR_ADC_BITWIDTH;

    err = adc_oneshot_config_channel(adc_handle, SOIL_SENSOR_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = SOIL_SENSOR_ADC_UNIT;
    cali_config.chan = SOIL_SENSOR_ADC_CHANNEL;
    cali_config.atten = SOIL_SENSOR_ADC_ATTEN;
    cali_config.bitwidth = SOIL_SENSOR_ADC_BITWIDTH;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle);
    adc_cali_available = (err == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {};
    cali_config.unit_id = SOIL_SENSOR_ADC_UNIT;
    cali_config.atten = SOIL_SENSOR_ADC_ATTEN;
    cali_config.bitwidth = SOIL_SENSOR_ADC_BITWIDTH;
#if CONFIG_IDF_TARGET_ESP32
    cali_config.default_vref = (uint32_t)SOIL_SENSOR_SUPPLY_MV;
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

#if SOIL_SENSOR_HAS_TEMPERATURE_MEASUREMENT
/* --- DS18B20 1-Wire driver — reused byte-for-byte from firmware/
 * water-heater/'s own driver (itself firmware/thermostat/'s own, also
 * reused verbatim in firmware/pump/), see the header comment above for
 * the full timing/CRC/sourcing detail. Only the pin macro name differs. */
static bool soil_temp_ow_reset(void)
{
    gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(SOIL_SENSOR_TEMP_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void soil_temp_ow_write_bit(int bit)
{
    gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int soil_temp_ow_read_bit(void)
{
    gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(SOIL_SENSOR_TEMP_SENSOR_GPIO);
    esp_rom_delay_us(50);
    return bit;
}

static void soil_temp_ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        soil_temp_ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t soil_temp_ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte = (uint8_t)(byte | (soil_temp_ow_read_bit() << i));
    }
    return byte;
}

/* Dallas/Maxim 1-Wire CRC-8 (reflected, polynomial 0x8C, init 0x00). */
static uint8_t soil_temp_onewire_crc8(const uint8_t *data, size_t len)
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

static bool soil_temp_sensor_setup(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << SOIL_SENSOR_TEMP_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(SOIL_SENSOR_TEMP_SENSOR_GPIO, 1);
    return true;
}

static bool soil_temp_sensor_read(float *temperature_c)
{
    portDISABLE_INTERRUPTS();
    bool present = soil_temp_ow_reset();
    if (present) {
        soil_temp_ow_write_byte(0xCC); /* Skip ROM */
        soil_temp_ow_write_byte(0x44); /* Convert T */
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset — check wiring/pull-up");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(750)); /* max conversion time at default 12-bit resolution */

    portDISABLE_INTERRUPTS();
    present = soil_temp_ow_reset();
    uint8_t scratchpad[9] = {0};
    if (present) {
        soil_temp_ow_write_byte(0xCC);
        soil_temp_ow_write_byte(0xBE); /* Read Scratchpad */
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = soil_temp_ow_read_byte();
        }
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset (read phase)");
        return false;
    }

    if (soil_temp_onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGW(TAG, "DS18B20 CRC mismatch — discarding reading");
        return false;
    }

    int16_t raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    *temperature_c = raw * 0.0625f; /* 12-bit default resolution: 1 LSB = 1/16 degC */
    return true;
}

/* Same registry-lookup-and-cast pattern as the other TemperatureMeasurement
 * instances in this repo — plain code-driven cluster, no "must-call-or-
 * crash" special init requirement like SoilMeasurement above. */
static void soil_update_temperature(uint16_t endpoint_id, float temperature_c)
{
    chip::app::ConcreteClusterPath path(endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "TemperatureMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }
    int16_t measured_value = (int16_t)(temperature_c * 100.0f);
    static_cast<TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(chip::app::DataModel::Nullable<int16_t>(measured_value));
}
#endif /* SOIL_SENSOR_HAS_TEMPERATURE_MEASUREMENT */

/* Periodically reads the sensor(s) and pushes fresh values into Matter —
 * moisture via esp-matter's own ready-made free function (see the header
 * comment above for why this cluster doesn't use this repo's usual
 * registry-lookup-and-cast pattern), and, if enabled, the DS18B20
 * temperature probe alongside it on the same cycle. */
static void soil_sensor_task(void *arg)
{
#if SOIL_SENSOR_HAS_TEMPERATURE_MEASUREMENT
    if (!soil_temp_sensor_setup()) {
        ESP_LOGE(TAG, "Temperature sensor GPIO setup failed — temperature readings will not be reported");
    }
#endif

    for (;;) {
        int mv = 0;
        if (read_adc_millivolts(&mv)) {
            uint8_t percent = millivolts_to_percent(mv);
            ESP_LOGI(TAG, "Soil moisture: %u%% (%d mV)", percent, mv);
            SoilMeasurement::SetSoilMoistureMeasuredValue(soil_sensor_endpoint_id,
                                                           chip::app::DataModel::Nullable<chip::Percent>(percent));
        } else {
            SoilMeasurement::SetSoilMoistureMeasuredValue(soil_sensor_endpoint_id,
                                                           chip::app::DataModel::Nullable<chip::Percent>());
        }

#if SOIL_SENSOR_HAS_TEMPERATURE_MEASUREMENT
        float temperature_c = 0.0f;
        if (soil_temp_sensor_read(&temperature_c)) {
            soil_update_temperature(soil_sensor_endpoint_id, temperature_c);
            ESP_LOGI(TAG, "Soil temperature: %.2f degC", temperature_c);
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(SOIL_SENSOR_POLL_INTERVAL_MS));
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

/* No controller-writable attributes on this device — all state flows from
 * the sensor task above. */
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

    /* 1b. Quick-power-cycle factory reset check — the actual reset (if
     * due) only happens later, once Matter has started. */
    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Set up the ADC. */
    if (!sensor_setup()) {
        ESP_LOGE(TAG, "Sensor setup failed — check wiring");
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

    /* 3. Build the Matter data model: one node, one Soil Sensor endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::soil_sensor::config_t soil_sensor_config;
    endpoint_t *endpoint = endpoint::soil_sensor::create(node, &soil_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create soil sensor endpoint");
        return;
    }
    soil_sensor_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Soil sensor endpoint id: %u", soil_sensor_endpoint_id);

    /* 3a. Set SoilMoistureMeasurementLimits — MUST happen before
     * esp_matter::start(), or the device aborts at startup. See the
     * header comment above for the full detail on this real esp-matter
     * gap. */
    Globals::Structs::MeasurementAccuracyStruct::Type soil_limits;
    soil_limits.measurementType = Globals::MeasurementTypeEnum::kSoilMoisture;
    soil_limits.measured = true;
    soil_limits.minMeasuredValue = 0;
    soil_limits.maxMeasuredValue = 100;
    SoilMeasurement::SetSoilMoistureLimits(soil_sensor_endpoint_id, soil_limits);

#if SOIL_SENSOR_HAS_TEMPERATURE_MEASUREMENT
    /* 3b. TemperatureMeasurement (DS18B20 probe) — optionalConform, see
     * the header comment above for the full sourcing/reuse detail. */
    cluster::temperature_measurement::config_t temp_meas_config;
    temp_meas_config.min_measured_value = nullable<int16_t>((int16_t)SOIL_SENSOR_TEMP_MIN_CENTIDEGREES);
    temp_meas_config.max_measured_value = nullable<int16_t>((int16_t)SOIL_SENSOR_TEMP_MAX_CENTIDEGREES);
    cluster::temperature_measurement::create(endpoint, &temp_meas_config, CLUSTER_FLAG_SERVER);
#endif

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

    /* 5. Start the sensor polling task — reports readings for as long as
     * the device runs. */
    xTaskCreate(soil_sensor_task, "soil_sensor_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter soil sensor started. Scan the QR code to commission.");
}
