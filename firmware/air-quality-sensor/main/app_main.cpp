/*
 * Minimal Matter Air Quality Sensor — seventeenth device type. First in
 * this repo to combine the AirQuality cluster's own qualitative headline
 * state (Good/Fair/.../ExtremelyPoor) with real numeric concentration
 * readings (CarbonDioxideConcentrationMeasurement,
 * TotalVolatileOrganicCompoundsConcentrationMeasurement) on the SAME
 * endpoint — confirmed as a legitimate combination directly against the
 * CSA's own data_model/1.6/device_types/AirQualitySensor.xml, which lists
 * Identify + AirQuality as `<mandatoryConform/>` and every one of ten
 * concentration-measurement clusters (CO/CO2/NO2/Ozone/PM1/PM2.5/PM10/
 * Radon/Formaldehyde/TVOC — plus, notably, even Temperature/Humidity) as
 * `<optionalConform/>` on that same endpoint, not a separate one.
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
 * --- Endpoint: esp-matter's own complete top-level helper, plus two extra
 * clusters added onto the SAME already-correct endpoint ------------------
 * `endpoint::air_quality_sensor::create()` (device type 0x002C) confirmed
 * complete/ready-to-use by reading esp_matter_endpoint.cpp's own
 * air_quality_sensor::add() directly: Identify + AirQuality only (no
 * Groups — correctly matches the CSA XML, which doesn't list one), and,
 * like every complete top-level helper in this repo, built via
 * `common::create<T>()`, which always creates the endpoint's Descriptor
 * cluster automatically — same "use the complete helper, avoid the
 * missing-Descriptor-cluster bug class" precedent
 * firmware/occupancy-sensor/ and firmware/fan/ already established.
 * CarbonDioxideConcentrationMeasurement and
 * TotalVolatileOrganicCompoundsConcentrationMeasurement are then added
 * onto that SAME endpoint afterwards, via their own
 * cluster::xxx_concentration_measurement::create() free functions — this
 * does NOT reintroduce the missing-Descriptor bug (that bug was about
 * hand-assembling an endpoint from raw endpoint::create() instead of a
 * complete helper; here the complete helper builds the endpoint correctly
 * first, and more clusters are simply added onto it afterwards) — same
 * "add extra clusters onto an already-correct endpoint" pattern
 * firmware/thermostat/'s BINDING output type already established (its
 * Binding + client OnOff cluster added onto the same thermostat endpoint,
 * not a second one).
 *
 * --- AirQuality: a "code-driven" cluster, registry-lookup setter --------
 * Confirmed by reading esp-matter's own source directly: a real
 * `air_quality/` folder exists under
 * `components/esp_matter/data_model_provider/clusters/`, backed by
 * connectedhomeip's own `AirQualityCluster`
 * (`app/clusters/air-quality-server/AirQualityCluster.h`) — same category
 * as firmware/contact-sensor/'s BooleanState, firmware/occupancy-sensor/'s
 * OccupancySensing, and firmware/fan/'s FanControl. `SetAirQuality()` is a
 * plain method on that class (not a Delegate, unlike FanControl) — so
 * this uses the same registry-lookup-and-cast pattern those first two
 * already established: look the live `AirQualityCluster` instance up via
 * the data model provider's registry, call `SetAirQuality()` directly.
 *
 * --- A real, documented esp-matter gap: AirQuality's own config_t has no
 * way to enable its four optional features -------------------------------
 * AirQuality's `AirQualityEnum` has 7 values (Unknown/Good/Fair/Moderate/
 * Poor/VeryPoor/ExtremelyPoor), but per the cluster's own Enums.h, only
 * Unknown/Good/Poor are usable with zero feature bits set — Fair/
 * Moderate/VeryPoor/ExtremelyPoor each need their own `Feature` bit
 * (kFair/kModerate/kVeryPoor/kExtremelyPoor) set in FeatureMap first.
 * Confirmed by reading `air_quality::create()` in esp-matter's own
 * esp_matter_cluster.cpp directly: unlike every comparable "optional
 * feature" cluster in this repo (occupancy_sensing, smoke_co_alarm,
 * fan_control, concentration_measurement — all of which thread a real
 * `config->feature_flags` field through to `global::attribute::
 * create_feature_map()`), `air_quality::create()` hardcodes
 * `global::attribute::create_feature_map(cluster, 0)` — and
 * `air_quality::config_t` doesn't even declare a `feature_flags` field to
 * pass one in. So only the base 3-state Good/Poor/Unknown scale is
 * actually reachable through this helper today. Overriding the FeatureMap
 * ember attribute by hand after creation was considered and deliberately
 * NOT done — `AirQualityCluster`'s own constructor takes its
 * `BitFlags<Feature>` once, read via `read_feature_map_u32()` inside its
 * server-init callback (which fires during `esp_matter::start()`), so
 * whether a later attribute override actually reaches that snapshot
 * depends on ordering that isn't obviously guaranteed; shipping an
 * unverified workaround for four extra enum values was judged worse than
 * a clean, correct 3-state scale — same "smallest reasonable next step"
 * scoping this repo has applied to every other device type's first cut.
 * Revisit if esp-matter ever adds a `feature_flags` field to
 * `air_quality::config_t`.
 *
 * --- Concentration measurement clusters: plain ember attributes, not
 * code-driven --------------------------------------------------------
 * Confirmed by the same check used throughout this repo: no
 * `concentration_measurement/` folder exists under
 * `data_model_provider/clusters/`, so — unlike AirQuality above —
 * MeasuredValue is a plain ember attribute, written the same way as
 * firmware/door-lock/'s LockState: construct an `esp_matter_attr_val_t`
 * via `esp_matter_nullable_float()` and call `attribute::update()`
 * directly, no registry lookup needed. `NumericMeasurement` is the only
 * feature enabled (0-100% overkill features like PeakMeasurement/
 * AverageMeasurement aren't implemented — same "smallest reasonable next
 * step" scoping); MeasurementMedium is fixed to Air, MeasurementUnit is
 * Ppm for CO2 and Ppb for TVOC (both confirmed against the CCS811's own
 * output units below), and Min/MaxMeasuredValue are set to the sensor's
 * own datasheet-documented output range.
 *
 * --- Sensor: CCS811 only for v1 (AIR_QUALITY_SENSOR_TYPE scaffold ready
 * to grow, same shape as SENSOR_TYPE elsewhere in this repo) -------------
 * ams/ScioSense CCS811 — a real, calibrated I2C eCO2 (ppm) + eTVOC (ppb)
 * digital gas sensor (not a raw-analog MQ-series sensor the way
 * firmware/smoke-co-alarm/ uses) — a genuinely better fit for Matter's
 * numeric concentration-measurement clusters, which need real calibrated
 * units, not just a threshold classifier. Chosen as this device type's
 * first sensor (over e.g. MQ-135, SGP30, PMS5003) as the most common
 * hobbyist I2C air-quality module with real calibrated output; more
 * sensors can be added later the same way firmware/temperature-sensor/
 * and firmware/occupancy-sensor/ grew from one sensor to several.
 *
 * Protocol verified directly against ams's own "CCS811 Datasheet"
 * (v1-06, 2019-Feb-07 — fetched as a PDF and read via `pdftotext`, this
 * repo's established practice for primary-source hardware protocol
 * detail) and its companion "CCS811 Application Notes — Programming
 * Guide", not assumed from a community library:
 *   - I2C address: 0x5A with the ADDR pin low (this file's default — most
 *     breakout boards ground ADDR by default), 0x5B if ADDR is tied high.
 *   - nWAKE (active-low) must be asserted before and held low throughout
 *     every I²C transaction — but the datasheet's own text explicitly
 *     recommends "tying nWAKE to ground is the simplest hardware [option]"
 *     when the host doesn't need the sensor's lowest-power idle state,
 *     which is what this file assumes (documented reference wiring below)
 *     — no extra GPIO needed for it.
 *   - Boot sequence: after power-up (tSTART, max 20ms until ready for
 *     I²C), read HW_ID (register 0x20) and confirm it reads 0x81; read
 *     STATUS (0x00) and confirm bit 4 (APP_VALID) is set; write APP_START
 *     (0xF4) with no data (switches FW_MODE from Boot to Application);
 *     wait tAPP_START (max 1ms); write MEAS_MODE (0x01) = 0x10 (DRIVE_MODE
 *     bits [6:4] = 001, "Mode 1: constant power, IAQ measurement every
 *     second" — confirmed bit position directly from the register's own
 *     bit table, not assumed).
 *   - Poll STATUS (0x00): bit 3 (DATA_READY) means a fresh sample is
 *     available in ALG_RESULT_DATA; bit 0 (ERROR) means ERROR_ID (0xE0)
 *     has the real cause. Reading ALG_RESULT_DATA (0x02) as 5 bytes in
 *     one transaction (eCO2 hi/lo, eTVOC hi/lo, STATUS) is the datasheet's
 *     own documented efficient technique for polling without using the
 *     nINT interrupt pin, which this file doesn't wire up.
 *   - Output ranges (both confirmed in-text, not inferred): eCO2 400ppm
 *     to 29206ppm, eTVOC 0ppb to 32768ppb — used directly as this file's
 *     Min/MaxMeasuredValue.
 *   - The datasheet also documents a 20-minute conditioning/run-in period
 *     after first enabling a measurement mode before readings are
 *     accurate — not implemented as a startup delay here (this firmware
 *     starts reporting immediately), same "not a substitute for the real
 *     hardware warm-up/burn-in time" framing firmware/smoke-co-alarm/
 *     already uses for its own MQ-series sensors' 24-48h burn-in.
 *   - No temperature/humidity compensation (ENV_DATA register, 0x05) is
 *     implemented — out of scope for v1 (would need wiring in a second
 *     sensor, the way firmware/thermostat/ reuses firmware/
 *     temperature-sensor/'s own drivers; a reasonable future addition).
 *
 * --- AirQuality classification: a plain, adjustable threshold on each of
 * eCO2/eTVOC, not a spec-defined mapping ----------------------------------
 * Matter's own spec deliberately leaves "what counts as Good vs Poor" up
 * to the device — there's no canonical formula to verify against, unlike
 * the CCS811's own register protocol above. AIR_QUALITY_CO2_POOR_PPM
 * (1000ppm) and AIR_QUALITY_TVOC_POOR_PPB (660ppb) are common, widely-
 * cited "acceptable indoor air" thresholds from general IAQ guidance —
 * adjustable #defines, explicitly not a calibrated absolute judgement,
 * same "adjustable threshold, not a calibrated reading" precedent
 * firmware/smoke-co-alarm/'s own MQ-series classifier already uses. The
 * overall AirQuality state reported is whichever of the two (CO2 or
 * TVOC) is worse.
 */

#include <string.h>

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
#include <app/clusters/air-quality-server/AirQualityCluster.h>

static const char *TAG = "matter_air_quality";

/* --- AIR_QUALITY_SENSOR_TYPE — see the header comment above. Only one
 * option today; the #define scaffold is ready for more later. */
#define AIR_QUALITY_SENSOR_CCS811 1
#define AIR_QUALITY_SENSOR_TYPE AIR_QUALITY_SENSOR_CCS811

/* I2C pins — deliberately generic names (not "SDA"/"SCL"-specific),
 * matching firmware/temperature-sensor/'s and firmware/light-sensor/'s own
 * SENSOR_PIN_1/SENSOR_PIN_2 convention, so the wizard's existing I2C field
 * mechanism needs no changes for this device type. PIN_1 = SDA, PIN_2 = SCL. */
#define SENSOR_PIN_1 GPIO_NUM_21
#define SENSOR_PIN_2 GPIO_NUM_22
#define SENSOR_I2C_FREQ_HZ 100000

#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* CCS811's own Mode 1 produces a fresh sample every second; polling every
 * 2s comfortably keeps up without hammering the bus. */
#define AIR_QUALITY_POLL_INTERVAL_MS 2000

#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
#define CCS811_I2C_ADDR 0x5A /* ADDR pin low — this file's assumed/documented wiring */
#define CCS811_REG_STATUS 0x00
#define CCS811_REG_MEAS_MODE 0x01
#define CCS811_REG_ALG_RESULT_DATA 0x02
#define CCS811_REG_HW_ID 0x20
#define CCS811_REG_ERROR_ID 0xE0
#define CCS811_REG_APP_START 0xF4
#define CCS811_HW_ID_VALUE 0x81
#define CCS811_STATUS_FW_MODE_BIT (1 << 7)
#define CCS811_STATUS_APP_VALID_BIT (1 << 4)
#define CCS811_STATUS_DATA_READY_BIT (1 << 3)
#define CCS811_STATUS_ERROR_BIT (1 << 0)
#define CCS811_MEAS_MODE_1S 0x10 /* DRIVE_MODE=001: constant power, 1 sample/sec */
#endif

/* AirQuality classification thresholds — see the header comment above:
 * adjustable, not a spec-defined or chip-calibrated mapping. */
#define AIR_QUALITY_CO2_POOR_PPM 1000.0f
#define AIR_QUALITY_TVOC_POOR_PPB 660.0f

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t air_quality_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
static i2c_master_dev_handle_t i2c_dev = NULL;

/* Same I2C bus setup as firmware/temperature-sensor/'s I2C sensors —
 * driver/i2c_master.h, ESP-IDF's newer I2C API. */
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

/* "Select register address only" write — used for APP_START (0xF4),
 * which the datasheet documents as a single-byte write with no data. */
static bool ccs811_write_reg(uint8_t reg)
{
    return i2c_master_transmit(i2c_dev, &reg, 1, 1000) == ESP_OK;
}

static bool ccs811_write_reg_u8(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool ccs811_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(i2c_dev, &reg, 1, data, len, 1000) == ESP_OK;
}

/* Boot sequence — see the header comment above for the exact
 * datasheet-sourced steps and timing. */
static bool ccs811_init(void)
{
    if (!i2c_bus_setup(CCS811_I2C_ADDR)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30)); /* tSTART max 20ms, safety margin */

    uint8_t hw_id = 0;
    if (!ccs811_read_reg(CCS811_REG_HW_ID, &hw_id, 1) || hw_id != CCS811_HW_ID_VALUE) {
        ESP_LOGE(TAG, "CCS811 not found (HW_ID=0x%02X, expected 0x%02X) — check wiring/I2C address", hw_id, CCS811_HW_ID_VALUE);
        return false;
    }

    uint8_t status = 0;
    if (!ccs811_read_reg(CCS811_REG_STATUS, &status, 1) || !(status & CCS811_STATUS_APP_VALID_BIT)) {
        ESP_LOGE(TAG, "CCS811 has no valid application firmware (STATUS=0x%02X)", status);
        return false;
    }

    if (!ccs811_write_reg(CCS811_REG_APP_START)) {
        ESP_LOGE(TAG, "CCS811 APP_START write failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); /* tAPP_START max 1ms, safety margin */

    if (!ccs811_write_reg_u8(CCS811_REG_MEAS_MODE, CCS811_MEAS_MODE_1S)) {
        ESP_LOGE(TAG, "CCS811 MEAS_MODE write failed");
        return false;
    }

    ESP_LOGI(TAG, "CCS811 initialized (Mode 1, 1 sample/sec) — readings won't be fully "
                  "accurate until the sensor's own ~20-minute conditioning period completes");
    return true;
}

/* Reads eCO2/eTVOC if a fresh sample is ready. Returns false (leaving the
 * output parameters untouched) if there's no new sample yet or an error
 * occurred — the caller simply tries again next poll. */
static bool ccs811_read(float *co2_ppm, float *tvoc_ppb)
{
    uint8_t status = 0;
    if (!ccs811_read_reg(CCS811_REG_STATUS, &status, 1)) {
        return false;
    }
    if (status & CCS811_STATUS_ERROR_BIT) {
        uint8_t error_id = 0;
        ccs811_read_reg(CCS811_REG_ERROR_ID, &error_id, 1);
        ESP_LOGW(TAG, "CCS811 reported an error (ERROR_ID=0x%02X)", error_id);
        return false;
    }
    if (!(status & CCS811_STATUS_DATA_READY_BIT)) {
        return false; /* no new sample since the last read */
    }

    /* 5-byte read: eCO2 hi/lo, eTVOC hi/lo, STATUS — the datasheet's own
     * documented efficient technique for polling without nINT. */
    uint8_t data[5] = { 0 };
    if (!ccs811_read_reg(CCS811_REG_ALG_RESULT_DATA, data, sizeof(data))) {
        return false;
    }

    *co2_ppm = (float)(((uint16_t)data[0] << 8) | data[1]);
    *tvoc_ppb = (float)(((uint16_t)data[2] << 8) | data[3]);
    return true;
}
#endif /* AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811 */

/* See the header comment on AirQuality classification: a plain, adjustable
 * threshold per gas, not a spec-defined or chip-calibrated mapping. Only
 * kGood/kPoor are used — see the header comment on the FeatureMap gap for
 * why the finer Fair/Moderate/VeryPoor/ExtremelyPoor states aren't reachable
 * through esp-matter's air_quality::create() helper today. */
static AirQuality::AirQualityEnum classify(float co2_ppm, float tvoc_ppb)
{
    bool poor = (co2_ppm >= AIR_QUALITY_CO2_POOR_PPM) || (tvoc_ppb >= AIR_QUALITY_TVOC_POOR_PPB);
    return poor ? AirQuality::AirQualityEnum::kPoor : AirQuality::AirQualityEnum::kGood;
}

/* Pushes a fresh reading into both concentration-measurement clusters
 * (plain ember attributes, attribute::update()) and the AirQuality
 * cluster's own headline state (a code-driven cluster, registry-lookup
 * setter — see the header comment above for why these two need different
 * update mechanisms on the very same endpoint). */
static void update_air_quality(float co2_ppm, float tvoc_ppb)
{
    esp_matter_attr_val_t co2_val = esp_matter_nullable_float(nullable<float>(co2_ppm));
    attribute::update(air_quality_endpoint_id, CarbonDioxideConcentrationMeasurement::Id,
                      CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id, &co2_val);

    esp_matter_attr_val_t tvoc_val = esp_matter_nullable_float(nullable<float>(tvoc_ppb));
    attribute::update(air_quality_endpoint_id, TotalVolatileOrganicCompoundsConcentrationMeasurement::Id,
                      TotalVolatileOrganicCompoundsConcentrationMeasurement::Attributes::MeasuredValue::Id, &tvoc_val);

    AirQuality::AirQualityEnum overall = classify(co2_ppm, tvoc_ppb);
    chip::app::ConcreteClusterPath path(air_quality_endpoint_id, AirQuality::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (iface) {
        static_cast<AirQualityCluster *>(iface)->SetAirQuality(overall);
    } else {
        ESP_LOGE(TAG, "AirQuality cluster not found on endpoint %u", air_quality_endpoint_id);
    }

    ESP_LOGI(TAG, "eCO2 %.0f ppm, eTVOC %.0f ppb -> AirQuality %s", co2_ppm, tvoc_ppb,
             overall == AirQuality::AirQualityEnum::kPoor ? "Poor" : "Good");
}

/* Shared polling task — inits the sensor once, then reads it on a timer for
 * as long as the device runs. */
static void air_quality_task(void *arg)
{
#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
    if (!ccs811_init()) {
        ESP_LOGE(TAG, "CCS811 init failed — air quality task exiting, no readings will be reported");
        vTaskDelete(NULL);
        return;
    }
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AIR_QUALITY_POLL_INTERVAL_MS));

#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
        float co2_ppm = 0.0f, tvoc_ppb = 0.0f;
        if (ccs811_read(&co2_ppm, &tvoc_ppb)) {
            update_air_quality(co2_ppm, tvoc_ppb);
        }
#endif
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
 * the sensor task above, same no-op shape as firmware/contact-sensor/'s
 * and firmware/occupancy-sensor/'s own app_attribute_update_cb(). */
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

    /* 3. Build the Matter data model: one node, one Air Quality Sensor
     * endpoint (Identify + AirQuality + Descriptor, all via the complete
     * top-level helper), plus the two concentration-measurement clusters
     * added onto that same endpoint afterwards — see the header comment
     * above for why this ordering is safe. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Air Quality Sensor",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::air_quality_sensor::config_t air_quality_config;
    endpoint_t *air_quality_endpoint = endpoint::air_quality_sensor::create(node, &air_quality_config, ENDPOINT_FLAG_NONE, NULL);
    if (!air_quality_endpoint) {
        ESP_LOGE(TAG, "Failed to create air quality sensor endpoint");
        return;
    }
    air_quality_endpoint_id = endpoint::get_id(air_quality_endpoint);
    ESP_LOGI(TAG, "Air quality sensor endpoint id: %u", air_quality_endpoint_id);

    cluster::carbon_dioxide_concentration_measurement::config_t co2_config;
    co2_config.measurement_medium = chip::to_underlying(CarbonDioxideConcentrationMeasurement::MeasurementMediumEnum::kAir);
    co2_config.feature_flags = chip::to_underlying(CarbonDioxideConcentrationMeasurement::Feature::kNumericMeasurement);
    co2_config.features.numeric_measurement.measured_value = nullable<float>(); /* null until the first reading */
    co2_config.features.numeric_measurement.min_measured_value = nullable<float>(400.0f);
    co2_config.features.numeric_measurement.max_measured_value = nullable<float>(29206.0f);
    co2_config.features.numeric_measurement.measurement_unit =
        chip::to_underlying(CarbonDioxideConcentrationMeasurement::MeasurementUnitEnum::kPpm);
    if (!cluster::carbon_dioxide_concentration_measurement::create(air_quality_endpoint, &co2_config, CLUSTER_FLAG_SERVER)) {
        ESP_LOGE(TAG, "Failed to create CO2 concentration measurement cluster");
        return;
    }

    cluster::total_volatile_organic_compounds_concentration_measurement::config_t tvoc_config;
    tvoc_config.measurement_medium =
        chip::to_underlying(TotalVolatileOrganicCompoundsConcentrationMeasurement::MeasurementMediumEnum::kAir);
    tvoc_config.feature_flags =
        chip::to_underlying(TotalVolatileOrganicCompoundsConcentrationMeasurement::Feature::kNumericMeasurement);
    tvoc_config.features.numeric_measurement.measured_value = nullable<float>();
    tvoc_config.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
    tvoc_config.features.numeric_measurement.max_measured_value = nullable<float>(32768.0f);
    tvoc_config.features.numeric_measurement.measurement_unit =
        chip::to_underlying(TotalVolatileOrganicCompoundsConcentrationMeasurement::MeasurementUnitEnum::kPpb);
    if (!cluster::total_volatile_organic_compounds_concentration_measurement::create(air_quality_endpoint, &tvoc_config,
                                                                                     CLUSTER_FLAG_SERVER)) {
        ESP_LOGE(TAG, "Failed to create TVOC concentration measurement cluster");
        return;
    }

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
    xTaskCreate(air_quality_task, "air_quality_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter air quality sensor started. Scan the QR code to commission.");
}
