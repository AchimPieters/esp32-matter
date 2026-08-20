/*
 * Minimal Matter Pressure Sensor — twenty-first device type.
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
 * `endpoint::pressure_sensor::create()` (device type 0x0305) confirmed
 * complete/ready-to-use — Identify + PressureMeasurement, auto-Descriptor
 * via `common::create<T>()` — by reading esp_matter_endpoint.cpp's own
 * `pressure_sensor::add()` directly. Confirmed against the CSA's own
 * data_model/1.6/device_types/PressureSensor.xml: those two clusters are
 * the ONLY ones listed at all, both `<mandatoryConform/>` — the simplest
 * device type XML in this repo so far.
 *
 * --- PressureMeasurement: a "code-driven" cluster, registry-lookup setter --
 * Confirmed by reading esp-matter's own source directly: a real
 * `pressure_measurement/` folder exists under
 * `data_model_provider/clusters/`, backed by connectedhomeip's own
 * `PressureMeasurementCluster` — same category as firmware/
 * temperature-sensor/'s TemperatureMeasurement. `SetMeasuredValue()` is a
 * plain method (not a Delegate), so this uses the same registry-lookup-
 * and-cast pattern firmware/contact-sensor/'s and firmware/
 * temperature-sensor/'s own setters already establish. `Extended`
 * (higher-resolution ScaledValue) feature not implemented — same
 * "smallest reasonable next step" scoping as every other device type's
 * first cut.
 *
 * --- MeasuredValue encoding: kPa, resolution 0.1 kPa ------------------------
 * Matter's PressureMeasurement cluster XML doesn't spell out the unit in
 * its machine-readable form (inherited from the Zigbee ZCL Pressure
 * Measurement cluster, whose own spec does) — confirmed instead against a
 * real, independent open-source implementation: Home Assistant's own
 * Matter integration (`homeassistant/components/matter/sensor.py`, its
 * PressureMeasurement discovery schema) divides the raw MeasuredValue by
 * 10 and reports the result in kPa — i.e. `MeasuredValue = pressure_kPa *
 * 10`. Conveniently, since 1 hPa = 0.1 kPa, this means MeasuredValue is
 * numerically identical to the pressure in hPa — the unit BMP280's own
 * datasheet (see below) already reports in.
 *
 * --- Sensor: BMP280 only for v1 (PRESSURE_SENSOR_TYPE scaffold ready to
 * grow, same shape as SENSOR_TYPE elsewhere in this repo) -------------------
 * Bosch BMP280 — a real, extremely common cheap I2C barometric pressure +
 * temperature sensor (often sold as "GY-BMP280"), chosen as this device
 * type's first sensor for the same "most common hobbyist part" reasoning
 * firmware/air-quality-sensor/'s CCS811 and firmware/temperature-sensor/'s
 * SHT3x were. Protocol (register map, calibration data layout, and the
 * fixed-point compensation formula) verified directly against Bosch's own
 * official datasheet (BST-BMP280-DS001, revision 1.26, fetched as a PDF
 * from bosch-sensortec.com and read via `pdftotext`, this repo's
 * established practice for primary-source hardware protocol detail) —
 * not assumed from a community library, same standard firmware/
 * temperature-sensor/'s BME280 driver already set for this repo:
 *   - I2C address 0x76 (SDO tied to GND — this file's assumed/documented
 *     wiring) or 0x77 (SDO tied to VDDIO).
 *   - CHIP_ID register 0xD0, expected value 0x58 — read and checked at
 *     init to confirm the sensor is actually present before trusting
 *     anything else.
 *   - 24 bytes of factory calibration data at registers 0x88-0x9F (12
 *     little-endian words: dig_T1 (unsigned) / dig_T2 / dig_T3 (signed),
 *     dig_P1 (unsigned) / dig_P2-dig_P9 (signed)) — read once at init,
 *     not re-read per sample.
 *   - ctrl_meas register 0xF4: osrs_t[7:5] / osrs_p[4:2] / mode[1:0].
 *     This file uses Bosch's own documented "Standard resolution" preset
 *     (osrs_t=x1, osrs_p=x4 — 18-bit / 0.66 Pa resolution, from the
 *     datasheet's own Table 7 "Recommended filter settings based on use
 *     cases") in Forced mode (0b01 — one-shot conversion, sensor returns
 *     to sleep afterwards; simpler than Normal mode's continuous
 *     standby-time/IIR-filter configuration for a slowly-changing
 *     quantity like barometric pressure, and lower average power).
 *   - Raw pressure/temperature are 20-bit values split across 3 registers
 *     each (0xF7-0xF9 press_msb/lsb/xlsb, 0xFA-0xFC temp_msb/lsb/xlsb) —
 *     read as one 6-byte burst starting at 0xF7.
 *   - Compensation formula: Bosch's own official 64-bit fixed-point
 *     `bmp280_compensate_T_int32()`/`bmp280_compensate_P_int64()`
 *     (datasheet section 3.11.3 — the primary, "best possible calculation
 *     accuracy" formula the datasheet itself recommends; the appendix's
 *     32-bit variant in section 8.2 is only a fallback for platforms
 *     without 64-bit integer support, not needed here), reproduced
 *     verbatim — this sensor's raw ADC counts are meaningless without
 *     applying it against the per-chip calibration coefficients read
 *     from its NVM, same "reproduced verbatim, not approximated"
 *     precedent firmware/temperature-sensor/'s BME280 driver already
 *     established for the closely-related compensation math Bosch shares
 *     across this sensor family. Temperature compensation feeds into
 *     pressure compensation via the shared `t_fine` intermediate value —
 *     both must be computed together, temperature first, even though
 *     only pressure is exposed via Matter here. The datasheet's own
 *     pressure function returns Pa in Q24.8 fixed-point (documented
 *     example: raw output 24674867 means 24674867/256 = 96386.2 Pa); this
 *     file divides by 256 before returning, so its own
 *     `bmp280_compensate_pressure()` returns plain integer Pa directly.
 *   - Output range 300-1100 hPa (operating range, full accuracy, per the
 *     datasheet's own electrical characteristics table) — used directly
 *     as Min/MaxMeasuredValue (see the header comment above on why hPa
 *     and Matter's kPa*10 encoding are numerically identical).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/pressure-measurement-server/PressureMeasurementCluster.h>

static const char *TAG = "matter_pressure";

/* --- PRESSURE_SENSOR_TYPE — see the header comment above. Only one
 * option today; the #define scaffold is ready for more later. */
#define PRESSURE_SENSOR_BMP280 1
#define PRESSURE_SENSOR_TYPE PRESSURE_SENSOR_BMP280

/* I2C pins — deliberately generic names (not "SDA"/"SCL"-specific),
 * matching firmware/temperature-sensor/'s and firmware/
 * air-quality-sensor/'s own SENSOR_PIN_1/SENSOR_PIN_2 convention, so the
 * wizard's existing I2C field mechanism needs no changes for this device
 * type. PIN_1 = SDA, PIN_2 = SCL. */
#define SENSOR_PIN_1 GPIO_NUM_21
#define SENSOR_PIN_2 GPIO_NUM_22
#define SENSOR_I2C_FREQ_HZ 100000

#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Barometric pressure changes slowly — no need to poll faster than this. */
#define PRESSURE_POLL_INTERVAL_MS 10000

#if PRESSURE_SENSOR_TYPE == PRESSURE_SENSOR_BMP280
#define BMP280_I2C_ADDR 0x76 /* SDO pin low — this file's assumed/documented wiring */
#define BMP280_REG_CALIB_START 0x88
#define BMP280_REG_CHIP_ID 0xD0
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_PRESS_MSB 0xF7
#define BMP280_CHIP_ID_VALUE 0x58
/* osrs_t=001 (x1), osrs_p=011 (x4), mode=01 (Forced) — Bosch's own
 * "Standard resolution" preset, see the header comment above. */
#define BMP280_CTRL_MEAS_FORCED_STANDARD 0b00101101
#endif

/* Output range (hPa == MeasuredValue units, see the header comment above). */
#define PRESSURE_MIN_HPA 300
#define PRESSURE_MAX_HPA 1100

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t pressure_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

#if PRESSURE_SENSOR_TYPE == PRESSURE_SENSOR_BMP280
static i2c_master_dev_handle_t i2c_dev = NULL;

/* Factory calibration coefficients — read once at init, reused for every
 * sample. Types/signedness match the datasheet's own Table 17 exactly. */
static uint16_t dig_T1;
static int16_t dig_T2, dig_T3;
static uint16_t dig_P1;
static int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;

/* Same I2C bus setup as firmware/temperature-sensor/'s and firmware/
 * air-quality-sensor/'s I2C sensors — driver/i2c_master.h. */
static bool i2c_bus_setup(uint16_t device_address)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = SENSOR_PIN_1;
    bus_config.scl_io_num = SENSOR_PIN_2;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = device_address;
    dev_config.scl_speed_hz = SENSOR_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool bmp280_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool bmp280_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(i2c_dev, &reg, 1, data, len, 1000) == ESP_OK;
}

static bool bmp280_init(void)
{
    if (!i2c_bus_setup(BMP280_I2C_ADDR)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (!bmp280_read_reg(BMP280_REG_CHIP_ID, &chip_id, 1) || chip_id != BMP280_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "BMP280 not found (CHIP_ID=0x%02X, expected 0x%02X) — check wiring/I2C address", chip_id, BMP280_CHIP_ID_VALUE);
        return false;
    }

    /* 24 bytes of calibration data, one burst read starting at 0x88 — see
     * the header comment above for the exact layout (Table 17). */
    uint8_t calib[24] = { 0 };
    if (!bmp280_read_reg(BMP280_REG_CALIB_START, calib, sizeof(calib))) {
        ESP_LOGE(TAG, "BMP280 calibration data read failed");
        return false;
    }
    dig_T1 = (uint16_t)(calib[0] | (calib[1] << 8));
    dig_T2 = (int16_t)(calib[2] | (calib[3] << 8));
    dig_T3 = (int16_t)(calib[4] | (calib[5] << 8));
    dig_P1 = (uint16_t)(calib[6] | (calib[7] << 8));
    dig_P2 = (int16_t)(calib[8] | (calib[9] << 8));
    dig_P3 = (int16_t)(calib[10] | (calib[11] << 8));
    dig_P4 = (int16_t)(calib[12] | (calib[13] << 8));
    dig_P5 = (int16_t)(calib[14] | (calib[15] << 8));
    dig_P6 = (int16_t)(calib[16] | (calib[17] << 8));
    dig_P7 = (int16_t)(calib[18] | (calib[19] << 8));
    dig_P8 = (int16_t)(calib[20] | (calib[21] << 8));
    dig_P9 = (int16_t)(calib[22] | (calib[23] << 8));

    ESP_LOGI(TAG, "BMP280 initialized");
    return true;
}

/* Bosch's own official 32-bit fixed-point compensation formula
 * (datasheet section 8.2), reproduced verbatim — see the header comment
 * above. t_fine carries the fine-resolution temperature value over to the
 * pressure compensation step, exactly as the reference implementation
 * does (a file-scope variable there; a plain out-parameter here). */
static int32_t bmp280_compensate_temperature(int32_t adc_T, int32_t *t_fine_out)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) * ((int32_t)dig_T3)) >> 14;
    int32_t t_fine = var1 + var2;
    *t_fine_out = t_fine;
    T = (t_fine * 5 + 128) >> 8;
    return T; /* 0.01 degC resolution — not exposed via Matter here, only used for logging */
}

static uint32_t bmp280_compensate_pressure(int32_t adc_P, int32_t t_fine)
{
    int64_t var1, var2, p;
    var1 = (int64_t)t_fine - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) + ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0) {
        return 0; /* avoid divide-by-zero, matching the datasheet's own guard */
    }
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    /* p is Q24.8 fixed-point here per the datasheet's own formula (see the
     * header comment above) — divide by 256 to get plain integer Pa. */
    return (uint32_t)(p / 256);
}

/* Triggers one forced-mode conversion, waits for it to complete, and
 * returns the compensated pressure in hPa (== Matter's MeasuredValue
 * units, see the header comment above). */
static bool bmp280_read_hpa(float *pressure_hpa)
{
    if (!bmp280_write_reg(BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_FORCED_STANDARD)) {
        return false;
    }
    /* Standard-resolution forced-mode conversion takes ~10ms per the
     * datasheet's own measurement time table; wait generously. */
    vTaskDelay(pdMS_TO_TICKS(40));

    uint8_t data[6] = { 0 };
    if (!bmp280_read_reg(BMP280_REG_PRESS_MSB, data, sizeof(data))) {
        return false;
    }
    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);

    int32_t t_fine = 0;
    int32_t temp_centidegrees = bmp280_compensate_temperature(adc_T, &t_fine);
    uint32_t pressure_pa = bmp280_compensate_pressure(adc_P, t_fine);

    *pressure_hpa = pressure_pa / 100.0f;
    ESP_LOGI(TAG, "BMP280: %.2f hPa (die temp %ld.%02ld degC, not reported via Matter)",
             *pressure_hpa, (long)(temp_centidegrees / 100), (long)(temp_centidegrees % 100));
    return true;
}
#endif /* PRESSURE_SENSOR_TYPE == PRESSURE_SENSOR_BMP280 */

/* Same registry-lookup-and-cast pattern firmware/temperature-sensor/'s
 * own setters already establish — PressureMeasurement is a "code-driven"
 * cluster class in this SDK version, not the generic ember-style
 * attribute store. */
static void update_pressure(uint16_t endpoint_id, float pressure_hpa)
{
    chip::app::ConcreteClusterPath path(endpoint_id, PressureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "PressureMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }
    /* MeasuredValue is numerically identical to hPa — see the header
     * comment above on why. Rounded to the nearest whole unit (0.1 kPa). */
    int16_t measured_value = (int16_t)(pressure_hpa + (pressure_hpa >= 0 ? 0.5f : -0.5f));
    static_cast<PressureMeasurementCluster *>(iface)->SetMeasuredValue(chip::app::DataModel::Nullable<int16_t>(measured_value));
}

/* Shared polling task — inits the sensor once, then reads it on a timer for
 * as long as the device runs. */
static void pressure_task(void *arg)
{
#if PRESSURE_SENSOR_TYPE == PRESSURE_SENSOR_BMP280
    if (!bmp280_init()) {
        ESP_LOGE(TAG, "BMP280 init failed — pressure task exiting, no readings will be reported");
        vTaskDelete(NULL);
        return;
    }
#endif

    while (true) {
#if PRESSURE_SENSOR_TYPE == PRESSURE_SENSOR_BMP280
        float pressure_hpa = 0.0f;
        if (bmp280_read_hpa(&pressure_hpa)) {
            update_pressure(pressure_endpoint_id, pressure_hpa);
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(PRESSURE_POLL_INTERVAL_MS));
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

    /* 3. Build the Matter data model: one node, one Pressure Sensor endpoint. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::pressure_sensor::config_t pressure_config;
    pressure_config.pressure_measurement.min_measured_value = nullable<int16_t>(PRESSURE_MIN_HPA);
    pressure_config.pressure_measurement.max_measured_value = nullable<int16_t>(PRESSURE_MAX_HPA);
    endpoint_t *endpoint = endpoint::pressure_sensor::create(node, &pressure_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create pressure sensor endpoint");
        return;
    }

    pressure_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Pressure sensor endpoint id: %u", pressure_endpoint_id);

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

    /* 5. Start the sensor polling task — inits the sensor and reports
     * readings for as long as the device runs. */
    xTaskCreate(pressure_task, "pressure_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter pressure sensor started. Scan the QR code to commission.");
}
