/*
 * Minimal Matter Smoke/CO Alarm — a fourteenth device type, and this
 * repo's first over the SmokeCoAlarm cluster (life-safety alarm class,
 * not a plain sensor readout or actuator).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain,
 * readable C++ — there is no hidden framework layer and no telemetry.
 * Matter is local-first: commissioning happens over Bluetooth + your LAN,
 * and control runs over your local network. Nothing leaves your home
 * unless you choose to add a cloud hub (Google/Apple/Alexa). With Home
 * Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev
 * setup. Works on other ESP32 chips too (C3, C6, S3, H2) — see the README
 * for how to switch target.
 *
 * --- Matter cluster composition ------------------------------------
 * endpoint::smoke_co_alarm::create() (Identify + SmokeCoAlarm cluster) is
 * a complete, directly usable esp-matter helper — confirmed by reading
 * esp_matter_endpoint.cpp's own smoke_co_alarm::add() directly. Its
 * Identify defaults to IdentifyTypeEnum::kAudibleBeep (same reasoning as
 * firmware/door-lock/: a smoke/CO alarm is often ceiling-mounted, out of
 * easy line of sight, so a beep is a more realistic physical Identify
 * signal for this device class) — but this firmware still blinks a
 * visible LED for Identify like every other device type here, since
 * IdentifyType is purely informational, not something that constrains
 * what this app actually does.
 *
 * Unlike firmware/door-lock/'s DoorLock cluster, SmokeCoAlarm IS a
 * "code-driven" cluster class in this SDK version — confirmed by the
 * presence of a smoke_co_alarm/ folder under
 * data_model_provider/clusters/ (the same signal firmware/contact-sensor/'s
 * BooleanState and firmware/light-sensor/'s IlluminanceMeasurement already
 * used). So SmokeState/COState/TestInProgress/HardwareFaultAlert/etc. are
 * all written through SmokeCoAlarmCluster's own setter API, looked up via
 * the data model provider's registry — the exact same pattern
 * update_contact_state()/update_illuminance() already established
 * elsewhere in this repo, confirmed by reading
 * SmokeCoAlarmCluster.h/.cpp directly rather than guessing.
 *
 * SmokeCoAlarmCluster's setters already generate the right Matter events
 * internally (SmokeAlarm/COAlarm events fire from SetSmokeState()/
 * SetCOState() themselves when transitioning to Warning/Critical; AllClear
 * fires from SetExpressedState() when transitioning back to Normal) — no
 * manual event-generation code needed here, unlike a plain ember
 * attribute write.
 *
 * SetExpressedStateByPriority() computes ExpressedState (the cluster's
 * single "what's the headline state right now" attribute) from whichever
 * of the 9 possible sub-states is highest priority and currently active —
 * called after every state change below with a fixed priority order
 * (life-safety alarms first, then self-test, then secondary conditions).
 * Interconnect/battery/end-of-service/inoperative states are left at their
 * defaults (this device has no interconnect wiring to other alarms, no
 * battery to monitor when USB/PSU-powered, and no service-life tracking
 * for a hobby MQ-series sensor) — same "smallest reasonable next step"
 * scoping already used throughout this repo (e.g. firmware/door-lock/'s
 * skipped PIN/credential/schedule features).
 *
 * --- The SelfTestRequest command needs one thing this app DOES have to
 * do itself ---------------------------------------------------------
 * esp-matter's door_lock::config_t has an optional Delegate; SmokeCoAlarm
 * has one too (SmokeCoAlarmDelegate::OnSelfTestRequested()), left null
 * here for the same reason. Confirmed by reading
 * SmokeCoAlarmCluster::HandleRemoteSelfTestRequest() directly: without a
 * Delegate, a real controller's SelfTestRequest command still succeeds —
 * the cluster sets TestInProgress=true and ExpressedState=Testing
 * entirely on its own, no Delegate needed for that part. But nothing
 * *clears* TestInProgress afterwards unless the app does it — a real gap
 * worth remembering for any future *Request-style command cluster in this
 * repo: the SDK can set a flag on command receipt without ever owning the
 * job of clearing it again. sensor_task() below polls
 * GetTestInProgress() each cycle and, once a self-test has been running
 * for SMOKE_CO_ALARM_SELF_TEST_DURATION_MS, calls SetTestInProgress(false)
 * and recomputes ExpressedState — simulating a completed self-test since
 * there's no real self-test routine to run against a plain analog sensor.
 *
 * --- SENSOR_TYPE: which MQ-series gas sensor(s) are actually wired up --
 * SENSOR_MQ2_MQ7 (default) — both an MQ-2 (smoke/combustible-gas) and an
 *   MQ-7 (carbon monoxide) sensor, matching how real combination smoke+CO
 *   alarms are sold as one product. Enables both the SmokeAlarm and
 *   COAlarm cluster features.
 * SENSOR_MQ2 — MQ-2 only. SmokeAlarm feature only.
 * SENSOR_MQ7 — MQ-7 only. COAlarm feature only.
 *
 * Both are the classic cheap analog gas-sensor modules (a heated tin-
 * dioxide element whose resistance drops as gas concentration rises),
 * almost always sold on a breakout board with an onboard voltage divider:
 * Vcc -> sensor -> AOUT tap -> fixed load resistor -> GND, so AOUT voltage
 * *rises* with gas concentration on most common modules — always check
 * your specific module, some comparator-equipped boards invert this.
 * Deliberately NOT converted to a calibrated ppm figure the way
 * firmware/light-sensor/'s LDR is converted to lux: MQ-series datasheets
 * document ppm only as a family of curves that shift with each sensor's
 * own load resistance, heater voltage, and burn-in state, and Matter's
 * SmokeCoAlarm cluster doesn't have a numeric concentration attribute
 * anyway (only the AlarmStateEnum Normal/Warning/Critical tri-state) — so
 * this is a plain adjustable-millivolt-threshold classifier
 * (SMOKE_CO_ALARM_MQ2_WARNING_MV / _CRITICAL_MV, and the MQ7 equivalents),
 * meant to be tuned against your own module and environment, not a
 * calibrated absolute reading.
 *
 * MQ-series sensors need their heater element to stabilize before
 * readings mean anything — datasheets call for a real 24-48h burn-in for
 * full accuracy, far beyond what this firmware can enforce.
 * SMOKE_CO_ALARM_WARMUP_MS (default 60s) only suppresses the initial
 * power-on resistance-settling transient from causing an immediate false
 * alarm — it is NOT a substitute for real sensor burn-in, documented here
 * so nobody mistakes the two.
 *
 * GPIO 34/35 (ADC1 channels 6/7 on classic ESP32/WROOM-32) are the
 * defaults for MQ2/MQ7 respectively — deliberately ADC1, not ADC2, same
 * reasoning as firmware/light-sensor/'s LDR: ADC2 shares hardware with the
 * Wi-Fi radio and becomes unreliable once Wi-Fi is active, which this
 * device needs to be commissioned/reachable at all times.
 *
 * HardwareFaultAlert is set from a simple heuristic: several consecutive
 * readings pinned at the ADC's extreme raw values (near 0 or near
 * full-scale) most likely mean a disconnected or shorted sensor,
 * regardless of a specific module's exact wiring polarity — a
 * deliberately conservative check rather than one tuned to any single
 * module's normal-vs-fault voltage split.
 *
 * --- Temperature + RelativeHumidity ("hobbyist cluster expansion" pilot,
 * see CLAUDE.md's own "Open next steps") ---------------------------------
 * Originally deliberately NOT added: this device type's only sensing
 * hardware was the MQ2/MQ7 gas-sensor pair, with no temperature/humidity
 * chip anywhere in this file to back either cluster honestly. Revisited
 * the same way firmware/air-quality-sensor/'s own identical gap was —
 * `SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY` reuses firmware/temperature-sensor/'s
 * own already-datasheet-verified 4-chip I2C driver library verbatim
 * (SHT3x/SHT4x/AHT20/BME280 — see that file's own header comment for the
 * full per-chip sourcing; not re-verified here). Both clusters land on
 * this SAME SmokeCoAlarm endpoint, code-driven (registry-lookup +
 * SetMeasuredValue(), same pattern as firmware/temperature-sensor/'s own
 * update_temperature()/update_humidity()) — confirmed NOT the same
 * attribute-write mechanism as SmokeState/COState above (those go through
 * SmokeCoAlarmCluster's own setters) or CO's own LevelValue (a plain
 * ember attribute) — a third, independent attribute-write pattern
 * coexisting on this one endpoint. Unlike air-quality-sensor's own I2C
 * bus (already shared with CCS811), this device has NO existing I2C bus
 * at all — the MQ2/MQ7 pair is pure analog ADC — so this adds a genuinely
 * new, dedicated SDA/SCL pin pair (`SMOKE_CO_ALARM_TEMP_HUMIDITY_SDA_GPIO`/
 * `_SCL_GPIO`, defaulting to GPIO 21/22, this repo's usual I2C default,
 * chosen to avoid the existing ADC1 pins 34/35 and the identify LED's
 * GPIO 2) rather than reusing an existing field. A real, currently-sold
 * combination smoke/CO/temperature alarm is a genuinely common product
 * category, not a contrived addition.
 */

#include <array>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/smoke-co-alarm-server/SmokeCoAlarmCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/clusters/relative-humidity-measurement-server/RelativeHumidityMeasurementCluster.h>

static const char *TAG = "matter_smoke_co_alarm";

/* --- Sensor selection — change this one line to match your hardware --- */
#define SENSOR_MQ2_MQ7 1
#define SENSOR_MQ2 2
#define SENSOR_MQ7 3

#define SENSOR_TYPE SENSOR_MQ2_MQ7

/* ADC-capable input pins — see the header comment above for why these
 * must stay ADC1, and why moving off these GPIOs also means updating the
 * ADC unit/channel constants below by hand (ESP32's ADC channel-to-pin
 * mapping is fixed in hardware, not something software can remap). MQ7
 * pin/channel are unused/compiled out when SENSOR_TYPE == SENSOR_MQ2, and
 * vice versa. */
#define SMOKE_CO_ALARM_MQ2_GPIO GPIO_NUM_34
#define SMOKE_CO_ALARM_MQ7_GPIO GPIO_NUM_35

#define SMOKE_CO_ALARM_MQ2_ADC_UNIT ADC_UNIT_1
#define SMOKE_CO_ALARM_MQ2_ADC_CHANNEL ADC_CHANNEL_6 /* GPIO 34 */
#define SMOKE_CO_ALARM_MQ7_ADC_UNIT ADC_UNIT_1
#define SMOKE_CO_ALARM_MQ7_ADC_CHANNEL ADC_CHANNEL_7 /* GPIO 35 */
#define SMOKE_CO_ALARM_ADC_ATTEN ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define SMOKE_CO_ALARM_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define SMOKE_CO_ALARM_ADC_SUPPLY_MV 3300.0f

/* Adjustable raw-voltage alarm thresholds — see the header comment above
 * for why these are NOT calibrated ppm values. Tune against your own
 * module + environment (e.g. hold a smoke source near the MQ-2, or a
 * disconnected butane lighter's unlit gas near the MQ-7, and read the
 * logged millivolt value to pick sane thresholds for your setup). */
#define SMOKE_CO_ALARM_MQ2_WARNING_MV 1800
#define SMOKE_CO_ALARM_MQ2_CRITICAL_MV 2400
#define SMOKE_CO_ALARM_MQ7_WARNING_MV 1800
#define SMOKE_CO_ALARM_MQ7_CRITICAL_MV 2400

/* Averaging + reporting interval. */
#define SMOKE_CO_ALARM_SAMPLE_COUNT 16
#define SMOKE_CO_ALARM_MEASURE_INTERVAL_MS 5000

/* See the header comment above — NOT a substitute for real MQ-sensor
 * burn-in, only suppresses the initial power-on transient. */
#define SMOKE_CO_ALARM_WARMUP_MS 60000

/* How long a simulated self-test "runs" before this app clears
 * TestInProgress and restores ExpressedState — see the header comment
 * above for why the app has to do this at all. */
#define SMOKE_CO_ALARM_SELF_TEST_DURATION_MS 5000

/* Consecutive extreme-raw-reading samples before HardwareFaultAlert is
 * raised — see the header comment above. */
#define SMOKE_CO_ALARM_FAULT_STREAK_THRESHOLD 5
#define SMOKE_CO_ALARM_FAULT_RAW_LOW 20
#define SMOKE_CO_ALARM_FAULT_RAW_HIGH 4075 /* out of a 12-bit 0-4095 range */

/* --- Temperature + RelativeHumidity — see the header comment above --- */
#define SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY 0
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_SHT3X 1
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_SHT4X 2
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_AHT20 3
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_BME280 4
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_SHT3X

/* A dedicated I2C bus — this device has no existing one (MQ2/MQ7 are pure
 * analog), unlike firmware/air-quality-sensor/'s own reuse of an existing
 * bus. Defaults avoid the ADC1 pins above (34/35) and the identify LED
 * (2). */
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_SDA_GPIO GPIO_NUM_21
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_SCL_GPIO GPIO_NUM_22
#define SMOKE_CO_ALARM_TEMP_HUMIDITY_I2C_FREQ_HZ 100000

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

static uint16_t smoke_co_alarm_endpoint_id = 0;
#if SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY
static bool temp_humidity_ok = false;
#endif
static esp_timer_handle_t identify_led_timer = NULL;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Fixed ExpressedState priority order — see the header comment above for
 * why this list is exactly the 9 entries SetExpressedStateByPriority()
 * expects, and why life-safety alarms (Smoke/CO) sit above everything
 * else. Interconnect states are included (as required by the array's
 * fixed length) even though this device never sets them — they'll just
 * never be "active" and fall through to the next entry. */
static const std::array<SmokeCoAlarm::ExpressedStateEnum, 9> k_expressed_state_priority = {
    SmokeCoAlarm::ExpressedStateEnum::kSmokeAlarm,
    SmokeCoAlarm::ExpressedStateEnum::kCOAlarm,
    SmokeCoAlarm::ExpressedStateEnum::kInterconnectSmoke,
    SmokeCoAlarm::ExpressedStateEnum::kInterconnectCO,
    SmokeCoAlarm::ExpressedStateEnum::kTesting,
    SmokeCoAlarm::ExpressedStateEnum::kBatteryAlert,
    SmokeCoAlarm::ExpressedStateEnum::kHardwareFault,
    SmokeCoAlarm::ExpressedStateEnum::kEndOfService,
    SmokeCoAlarm::ExpressedStateEnum::kInoperative,
};

/* esp-matter's generic attribute::update() can't write this cluster's
 * attributes — see the header comment above for why. Follows the exact
 * pattern firmware/contact-sensor/main/app_main.cpp's update_contact_state()
 * established: look the cluster instance up directly via the data model
 * provider's registry, then call its cluster-specific setters. Returns
 * the cluster pointer (or NULL) so callers can chain multiple setter
 * calls without repeating the lookup. */
static chip::app::Clusters::SmokeCoAlarmCluster *get_smoke_co_alarm_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, SmokeCoAlarm::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "SmokeCoAlarm cluster not found on endpoint %u", endpoint_id);
        return NULL;
    }
    return static_cast<chip::app::Clusters::SmokeCoAlarmCluster *>(iface);
}

/* ======================================================================
 * Temperature + RelativeHumidity driver — the same 4 I2C chip options
 * firmware/temperature-sensor/ (and, since, firmware/air-quality-sensor/)
 * already established, ported verbatim. Unlike air-quality-sensor's own
 * multi-device bus, this device only ever has ONE I2C device, so a
 * plain single-device bus setup (matching temperature-sensor's own
 * original i2c_bus_setup() shape) is enough — no shared bus-add helper
 * needed. See the header comment above for the full sourcing.
 * ====================================================================== */
#if SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY
static i2c_master_dev_handle_t temp_humidity_i2c_dev = NULL;

static bool temp_humidity_i2c_bus_setup(uint16_t device_address)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = SMOKE_CO_ALARM_TEMP_HUMIDITY_SDA_GPIO;
    bus_config.scl_io_num = SMOKE_CO_ALARM_TEMP_HUMIDITY_SCL_GPIO;
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
    dev_config.scl_speed_hz = SMOKE_CO_ALARM_TEMP_HUMIDITY_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &temp_humidity_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Sensirion CRC-8 (polynomial 0x31, init 0xFF) — used by SHT3x/SHT4x. */
static uint8_t sensirion_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

#if SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP == SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_SHT3X

#define TEMP_HUMIDITY_SHT3X_I2C_ADDR 0x44 /* 0x45 if ADDR is tied to VDD */

static bool temp_humidity_setup(void)
{
    return temp_humidity_i2c_bus_setup(TEMP_HUMIDITY_SHT3X_I2C_ADDR);
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[2] = {0x24, 0x00}; /* single shot, high repeatability */
    if (i2c_master_transmit(temp_humidity_i2c_dev, cmd, sizeof(cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    if (i2c_master_receive(temp_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (sensirion_crc8(data, 2) != data[2] || sensirion_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT3x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = 100.0f * ((float)hum_ticks / 65535.0f);
    return true;
}

#elif SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP == SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_SHT4X

#define TEMP_HUMIDITY_SHT4X_I2C_ADDR 0x44

static bool temp_humidity_setup(void)
{
    return temp_humidity_i2c_bus_setup(TEMP_HUMIDITY_SHT4X_I2C_ADDR);
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[1] = {0xFD}; /* measure T & RH, high precision */
    if (i2c_master_transmit(temp_humidity_i2c_dev, cmd, sizeof(cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6];
    if (i2c_master_receive(temp_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (sensirion_crc8(data, 2) != data[2] || sensirion_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT4x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = -6.0f + 125.0f * ((float)hum_ticks / 65535.0f);
    if (*humidity_pct < 0.0f) {
        *humidity_pct = 0.0f;
    } else if (*humidity_pct > 100.0f) {
        *humidity_pct = 100.0f;
    }
    return true;
}

#elif SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP == SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_AHT20

#define TEMP_HUMIDITY_AHT20_I2C_ADDR 0x38

static bool temp_humidity_setup(void)
{
    if (!temp_humidity_i2c_bus_setup(TEMP_HUMIDITY_AHT20_I2C_ADDR)) {
        return false;
    }
    const uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
    if (i2c_master_transmit(temp_humidity_i2c_dev, init_cmd, sizeof(init_cmd), 1000) != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 init command failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    return true;
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t trigger_cmd[3] = {0xAC, 0x33, 0x00};
    if (i2c_master_transmit(temp_humidity_i2c_dev, trigger_cmd, sizeof(trigger_cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(85));

    uint8_t data[6];
    if (i2c_master_receive(temp_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (data[0] & 0x80) {
        ESP_LOGW(TAG, "AHT20 still busy — discarding reading");
        return false;
    }

    uint32_t raw_humidity = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_temperature = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humidity_pct = (float)raw_humidity * 100.0f / 1048576.0f;
    *temperature_c = (float)raw_temperature * 200.0f / 1048576.0f - 50.0f;
    return true;
}

#elif SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP == SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP_BME280

#define TEMP_HUMIDITY_BME280_I2C_ADDR 0x76 /* 0x77 if SDO is tied to VDD */
#define TEMP_HUMIDITY_BME280_REG_CHIP_ID 0xD0
#define TEMP_HUMIDITY_BME280_REG_CALIB_T 0x88
#define TEMP_HUMIDITY_BME280_REG_CALIB_H1 0xA1
#define TEMP_HUMIDITY_BME280_REG_CALIB_H2 0xE1
#define TEMP_HUMIDITY_BME280_REG_CTRL_HUM 0xF2
#define TEMP_HUMIDITY_BME280_REG_CTRL_MEAS 0xF4
#define TEMP_HUMIDITY_BME280_REG_DATA 0xFA
#define TEMP_HUMIDITY_BME280_CHIP_ID_EXPECTED 0x60

struct bme280_calib_data {
    uint16_t dig_t1;
    int16_t dig_t2;
    int16_t dig_t3;
    uint8_t dig_h1;
    int16_t dig_h2;
    uint8_t dig_h3;
    int16_t dig_h4;
    int16_t dig_h5;
    int8_t dig_h6;
};

static struct bme280_calib_data bme280_calib;

static bool bme280_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(temp_humidity_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool bme280_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(temp_humidity_i2c_dev, &reg, 1, out, len, 1000) == ESP_OK;
}

static int16_t sign_extend_12bit(uint16_t value)
{
    return (int16_t)((value & 0x0800) ? (value | 0xF000) : value);
}

static bool temp_humidity_setup(void)
{
    if (!temp_humidity_i2c_bus_setup(TEMP_HUMIDITY_BME280_I2C_ADDR)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CHIP_ID, &chip_id, 1) || chip_id != TEMP_HUMIDITY_BME280_CHIP_ID_EXPECTED) {
        ESP_LOGE(TAG, "BME280 chip ID mismatch (got 0x%02X, expected 0x%02X)", chip_id, TEMP_HUMIDITY_BME280_CHIP_ID_EXPECTED);
        return false;
    }

    uint8_t calib_t[6];
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CALIB_T, calib_t, sizeof(calib_t))) {
        return false;
    }
    bme280_calib.dig_t1 = (uint16_t)(calib_t[0] | (calib_t[1] << 8));
    bme280_calib.dig_t2 = (int16_t)(calib_t[2] | (calib_t[3] << 8));
    bme280_calib.dig_t3 = (int16_t)(calib_t[4] | (calib_t[5] << 8));

    uint8_t dig_h1 = 0;
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CALIB_H1, &dig_h1, 1)) {
        return false;
    }
    bme280_calib.dig_h1 = dig_h1;

    uint8_t calib_h[7];
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CALIB_H2, calib_h, sizeof(calib_h))) {
        return false;
    }
    bme280_calib.dig_h2 = (int16_t)(calib_h[0] | (calib_h[1] << 8));
    bme280_calib.dig_h3 = calib_h[2];
    bme280_calib.dig_h4 = sign_extend_12bit((uint16_t)((calib_h[3] << 4) | (calib_h[4] & 0x0F)));
    bme280_calib.dig_h5 = sign_extend_12bit((uint16_t)((calib_h[5] << 4) | (calib_h[4] >> 4)));
    bme280_calib.dig_h6 = (int8_t)calib_h[6];

    if (!bme280_write_reg(TEMP_HUMIDITY_BME280_REG_CTRL_HUM, 0x01)) {
        return false;
    }
    return true;
}

static int32_t bme280_compensate_temperature(int32_t adc_t, int32_t *t_fine)
{
    int32_t var1 = ((adc_t / 8) - ((int32_t)bme280_calib.dig_t1 * 2)) * ((int32_t)bme280_calib.dig_t2) / 2048;
    int32_t var2_pre = (adc_t / 16) - ((int32_t)bme280_calib.dig_t1);
    int32_t var2 = (((var2_pre * var2_pre) / 4096) * ((int32_t)bme280_calib.dig_t3)) / 16384;
    *t_fine = var1 + var2;
    int32_t temperature = (*t_fine * 5 + 128) / 256;
    if (temperature < -4000) {
        temperature = -4000;
    } else if (temperature > 8500) {
        temperature = 8500;
    }
    return temperature;
}

static uint32_t bme280_compensate_humidity(int32_t adc_h, int32_t t_fine)
{
    int32_t var1 = t_fine - 76800;
    int32_t var2 = adc_h * 16384;
    int32_t var3 = ((int32_t)bme280_calib.dig_h4) * 1048576;
    int32_t var4 = ((int32_t)bme280_calib.dig_h5) * var1;
    int32_t var5 = (((var2 - var3) - var4) + 16384) / 32768;
    var2 = (var1 * ((int32_t)bme280_calib.dig_h6)) / 1024;
    var3 = (var1 * ((int32_t)bme280_calib.dig_h3)) / 2048;
    var4 = ((var2 * (var3 + 32768)) / 1024) + 2097152;
    var2 = ((var4 * ((int32_t)bme280_calib.dig_h2)) + 8192) / 16384;
    var3 = var5 * var2;
    int32_t var4b = ((var3 / 32768) * (var3 / 32768)) / 128;
    int32_t var5b = var3 - ((var4b * ((int32_t)bme280_calib.dig_h1)) / 16);
    if (var5b < 0) {
        var5b = 0;
    } else if (var5b > 419430400) {
        var5b = 419430400;
    }
    uint32_t humidity = (uint32_t)(var5b / 4096);
    if (humidity > 102400) {
        humidity = 102400;
    }
    return humidity;
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    if (!bme280_write_reg(TEMP_HUMIDITY_BME280_REG_CTRL_MEAS, 0x25)) { /* osrs_t=x1, osrs_p=x1, forced mode */
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[5];
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_DATA, data, sizeof(data))) {
        return false;
    }

    int32_t adc_t = (int32_t)(((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | (data[2] >> 4));
    int32_t adc_h = (int32_t)(((uint32_t)data[3] << 8) | data[4]);

    int32_t t_fine = 0;
    int32_t temp_centidegrees = bme280_compensate_temperature(adc_t, &t_fine);
    uint32_t hum_q22_10 = bme280_compensate_humidity(adc_h, t_fine);

    *temperature_c = temp_centidegrees / 100.0f;
    *humidity_pct = hum_q22_10 / 1024.0f;
    return true;
}

#else
#error "Unknown SMOKE_CO_ALARM_TEMP_HUMIDITY_CHIP"
#endif

/* Code-driven cluster setters — see the header comment above. Same
 * ScopedChipStackLock convention firmware/temperature-sensor/'s own
 * update_temperature()/update_humidity() already establish. */
static void update_temperature(chip::app::DataModel::Nullable<int16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    chip::app::ConcreteClusterPath path(smoke_co_alarm_endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "TemperatureMeasurement cluster not found on endpoint %u", smoke_co_alarm_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(value);
}

static void update_humidity(chip::app::DataModel::Nullable<uint16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    chip::app::ConcreteClusterPath path(smoke_co_alarm_endpoint_id, RelativeHumidityMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "RelativeHumidityMeasurement cluster not found on endpoint %u", smoke_co_alarm_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::RelativeHumidityMeasurementCluster *>(iface)->SetMeasuredValue(value);
}
#endif /* SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY */

/* ======================================================================
 * ADC driver — shared by both MQ2 and MQ7 (they're the same kind of
 * analog voltage-divider sensor, just on different channels/thresholds).
 * Only the channel(s) SENSOR_TYPE actually selects are configured.
 * ====================================================================== */

static adc_oneshot_unit_handle_t adc_handle = NULL;

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
static adc_cali_handle_t mq2_cali_handle = NULL;
static bool mq2_cali_available = false;
static uint8_t mq2_fault_streak = 0;
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
static adc_cali_handle_t mq7_cali_handle = NULL;
static bool mq7_cali_available = false;
static uint8_t mq7_fault_streak = 0;
#endif

/* Sets up ADC calibration for one channel — same portable curve-fitting/
 * line-fitting #if pattern as firmware/light-sensor/'s LDR driver, since
 * classic ESP32 only supports line fitting. Calibration failing isn't
 * fatal: read_channel_millivolts() below falls back to an uncalibrated
 * estimate. */
static bool setup_channel_calibration(adc_channel_t channel, adc_cali_handle_t *out_handle)
{
    esp_err_t err;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.chan = channel;
    cali_config.atten = SMOKE_CO_ALARM_ADC_ATTEN;
    cali_config.bitwidth = SMOKE_CO_ALARM_ADC_BITWIDTH;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, out_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = SMOKE_CO_ALARM_ADC_ATTEN;
    cali_config.bitwidth = SMOKE_CO_ALARM_ADC_BITWIDTH;
#if CONFIG_IDF_TARGET_ESP32
    cali_config.default_vref = (uint32_t)SMOKE_CO_ALARM_ADC_SUPPLY_MV;
#endif
    err = adc_cali_create_scheme_line_fitting(&cali_config, out_handle);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable for channel %d (%s) — using uncalibrated estimate",
                 channel, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool sensor_setup(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = SMOKE_CO_ALARM_ADC_ATTEN;
    chan_config.bitwidth = SMOKE_CO_ALARM_ADC_BITWIDTH;

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
    err = adc_oneshot_config_channel(adc_handle, SMOKE_CO_ALARM_MQ2_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQ2 adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }
    mq2_cali_available = setup_channel_calibration(SMOKE_CO_ALARM_MQ2_ADC_CHANNEL, &mq2_cali_handle);
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
    err = adc_oneshot_config_channel(adc_handle, SMOKE_CO_ALARM_MQ7_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQ7 adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }
    mq7_cali_available = setup_channel_calibration(SMOKE_CO_ALARM_MQ7_ADC_CHANNEL, &mq7_cali_handle);
#endif
    return true;
}

/* Reads and averages SMOKE_CO_ALARM_SAMPLE_COUNT raw samples from one
 * channel, converts to millivolts, and reports whether the raw reading
 * looked "stuck" at an extreme value (see the header comment above on
 * HardwareFaultAlert). */
static bool read_channel_millivolts(adc_channel_t channel, adc_cali_handle_t cali_handle, bool cali_available,
                                     int *out_mv, bool *out_extreme)
{
    int64_t sum_raw = 0;
    int last_raw = 0;
    for (int i = 0; i < SMOKE_CO_ALARM_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, channel, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read (channel %d) failed: %s", channel, esp_err_to_name(err));
            return false;
        }
        sum_raw += raw;
        last_raw = raw;
    }
    int avg_raw = (int)(sum_raw / SMOKE_CO_ALARM_SAMPLE_COUNT);
    *out_extreme = (last_raw <= SMOKE_CO_ALARM_FAULT_RAW_LOW) || (last_raw >= SMOKE_CO_ALARM_FAULT_RAW_HIGH);

    if (cali_available) {
        esp_err_t err = adc_cali_raw_to_voltage(cali_handle, avg_raw, out_mv);
        if (err == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s — falling back to uncalibrated estimate",
                 esp_err_to_name(err));
    }
    *out_mv = (int)((float)avg_raw * SMOKE_CO_ALARM_ADC_SUPPLY_MV / 4095.0f);
    return true;
}

/* Millivolts -> AlarmStateEnum via the plain adjustable thresholds above. */
static SmokeCoAlarm::AlarmStateEnum classify(int mv, int warning_mv, int critical_mv)
{
    if (mv >= critical_mv) {
        return SmokeCoAlarm::AlarmStateEnum::kCritical;
    }
    if (mv >= warning_mv) {
        return SmokeCoAlarm::AlarmStateEnum::kWarning;
    }
    return SmokeCoAlarm::AlarmStateEnum::kNormal;
}

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
/* Reuses classify()'s own SmokeCoAlarm::AlarmStateEnum result (already
 * computed once per MQ7 reading for COState) as the Carbon Monoxide
 * Concentration Measurement cluster's own LevelValue — see the header
 * comment above and this cluster's own creation site in app_main() for
 * why LevelIndication, not a fabricated numeric ppm, is what's exposed
 * here. */
static CarbonMonoxideConcentrationMeasurement::LevelValueEnum co_alarm_state_to_level_value(SmokeCoAlarm::AlarmStateEnum state)
{
    switch (state) {
    case SmokeCoAlarm::AlarmStateEnum::kCritical:
        return CarbonMonoxideConcentrationMeasurement::LevelValueEnum::kCritical;
    case SmokeCoAlarm::AlarmStateEnum::kWarning:
        return CarbonMonoxideConcentrationMeasurement::LevelValueEnum::kMedium;
    case SmokeCoAlarm::AlarmStateEnum::kNormal:
    default:
        return CarbonMonoxideConcentrationMeasurement::LevelValueEnum::kLow;
    }
}
#endif

static void sensor_task(void *arg)
{
    int64_t start_us = esp_timer_get_time();
    uint32_t self_test_remaining_ms = 0;
    bool hardware_fault_active = false;

    for (;;) {
        bool warmed_up = (esp_timer_get_time() - start_us) >= ((int64_t)SMOKE_CO_ALARM_WARMUP_MS * 1000);
        bool any_fault = false;

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
        {
            int mv = 0;
            bool extreme = false;
            if (read_channel_millivolts(SMOKE_CO_ALARM_MQ2_ADC_CHANNEL, mq2_cali_handle, mq2_cali_available, &mv, &extreme)) {
                mq2_fault_streak = extreme ? (mq2_fault_streak + 1) : 0;
                SmokeCoAlarm::AlarmStateEnum state = warmed_up
                    ? classify(mv, SMOKE_CO_ALARM_MQ2_WARNING_MV, SMOKE_CO_ALARM_MQ2_CRITICAL_MV)
                    : SmokeCoAlarm::AlarmStateEnum::kNormal;
                ESP_LOGI(TAG, "MQ2 (smoke): %d mV -> %s%s", mv,
                         state == SmokeCoAlarm::AlarmStateEnum::kNormal ? "Normal" :
                         state == SmokeCoAlarm::AlarmStateEnum::kWarning ? "Warning" : "Critical",
                         warmed_up ? "" : " (warming up)");
                if (auto *cluster = get_smoke_co_alarm_cluster(smoke_co_alarm_endpoint_id)) {
                    cluster->SetSmokeState(state);
                }
            }
            if (mq2_fault_streak >= SMOKE_CO_ALARM_FAULT_STREAK_THRESHOLD) {
                any_fault = true;
            }
        }
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
        {
            int mv = 0;
            bool extreme = false;
            if (read_channel_millivolts(SMOKE_CO_ALARM_MQ7_ADC_CHANNEL, mq7_cali_handle, mq7_cali_available, &mv, &extreme)) {
                mq7_fault_streak = extreme ? (mq7_fault_streak + 1) : 0;
                SmokeCoAlarm::AlarmStateEnum state = warmed_up
                    ? classify(mv, SMOKE_CO_ALARM_MQ7_WARNING_MV, SMOKE_CO_ALARM_MQ7_CRITICAL_MV)
                    : SmokeCoAlarm::AlarmStateEnum::kNormal;
                ESP_LOGI(TAG, "MQ7 (CO): %d mV -> %s%s", mv,
                         state == SmokeCoAlarm::AlarmStateEnum::kNormal ? "Normal" :
                         state == SmokeCoAlarm::AlarmStateEnum::kWarning ? "Warning" : "Critical",
                         warmed_up ? "" : " (warming up)");
                if (auto *cluster = get_smoke_co_alarm_cluster(smoke_co_alarm_endpoint_id)) {
                    cluster->SetCOState(state);
                }
                /* CarbonMonoxideConcentrationMeasurement's LevelValue is a
                 * plain ember attribute (confirmed NOT code-driven — no
                 * concentration_measurement/ folder under
                 * data_model_provider/clusters/, same check this repo's
                 * other concentration clusters already apply), so a direct
                 * attribute::update() here, same pattern as door-lock's
                 * LockState. */
                esp_matter_attr_val_t level_val = esp_matter_enum8(chip::to_underlying(co_alarm_state_to_level_value(state)));
                attribute::update(smoke_co_alarm_endpoint_id, CarbonMonoxideConcentrationMeasurement::Id,
                                  CarbonMonoxideConcentrationMeasurement::Attributes::LevelValue::Id, &level_val);
            }
            if (mq7_fault_streak >= SMOKE_CO_ALARM_FAULT_STREAK_THRESHOLD) {
                any_fault = true;
            }
        }
#endif

#if SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY
        if (temp_humidity_ok) {
            float temperature_c = 0.0f, humidity_pct = 0.0f;
            if (temp_humidity_read(&temperature_c, &humidity_pct)) {
                int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
                uint16_t hum_centipercent = (uint16_t)(humidity_pct * 100.0f);
                update_temperature(chip::app::DataModel::Nullable<int16_t>(temp_centidegrees));
                update_humidity(chip::app::DataModel::Nullable<uint16_t>(hum_centipercent));
                ESP_LOGI(TAG, "Temp/Humidity: %.2f degC, %.2f %%RH", temperature_c, humidity_pct);
            } else {
                update_temperature(chip::app::DataModel::Nullable<int16_t>());
                update_humidity(chip::app::DataModel::Nullable<uint16_t>());
            }
        }
#endif

        if (auto *cluster = get_smoke_co_alarm_cluster(smoke_co_alarm_endpoint_id)) {
            if (any_fault != hardware_fault_active) {
                hardware_fault_active = any_fault;
                cluster->SetHardwareFaultAlert(hardware_fault_active);
                if (hardware_fault_active) {
                    ESP_LOGW(TAG, "Sensor reading stuck at an extreme value — check wiring (HardwareFaultAlert set)");
                }
            }

            /* See the header comment above for why the app, not the SDK,
             * has to clear TestInProgress once a simulated self-test has
             * run long enough. */
            if (cluster->GetTestInProgress()) {
                if (self_test_remaining_ms == 0) {
                    self_test_remaining_ms = SMOKE_CO_ALARM_SELF_TEST_DURATION_MS;
                    ESP_LOGI(TAG, "Self-test started");
                } else if (self_test_remaining_ms <= SMOKE_CO_ALARM_MEASURE_INTERVAL_MS) {
                    cluster->SetTestInProgress(false);
                    self_test_remaining_ms = 0;
                    ESP_LOGI(TAG, "Self-test complete");
                } else {
                    self_test_remaining_ms -= SMOKE_CO_ALARM_MEASURE_INTERVAL_MS;
                }
            } else {
                self_test_remaining_ms = 0;
            }

            cluster->SetExpressedStateByPriority(k_expressed_state_priority);
        }

        vTaskDelay(pdMS_TO_TICKS(SMOKE_CO_ALARM_MEASURE_INTERVAL_MS));
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

/* Every attribute this device exposes is either read-only (SmokeState/
 * COState/...) or handled entirely inside SmokeCoAlarmCluster itself
 * (SelfTestRequest) — nothing for app code to react to here. Required by
 * node::create()'s callback signature regardless. */
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

    /* 2. Set up the ADC channel(s) SENSOR_TYPE actually selects. */
    if (!sensor_setup()) {
        ESP_LOGE(TAG, "Sensor setup failed");
        return;
    }

#if SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY
    /* 2a. Set up the optional temperature/humidity chip — non-fatal if it
     * fails (unlike the mandatory gas sensor above), same per-chip
     * graceful-degradation precedent firmware/air-quality-sensor/'s own
     * multi-sensor setup already establishes. */
    temp_humidity_ok = temp_humidity_setup();
    if (!temp_humidity_ok) {
        ESP_LOGE(TAG, "Temperature/humidity sensor init failed — no readings will be reported");
    }
#endif

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

    /* 3. Build the Matter data model: one node, one Smoke/CO Alarm
     * endpoint. feature_flags picks which of Smoke Alarm / CO Alarm the
     * cluster actually exposes, matching SENSOR_TYPE above — the cluster
     * itself requires at least one of the two (VALIDATE_FEATURES_AT_LEAST_ONE
     * in esp-matter's own cluster::smoke_co_alarm::create()). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    smoke_co_alarm::config_t smoke_co_alarm_config;
    uint32_t feature_flags = 0;
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
    feature_flags |= chip::to_underlying(SmokeCoAlarm::Feature::kSmokeAlarm);
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
    feature_flags |= chip::to_underlying(SmokeCoAlarm::Feature::kCoAlarm);
#endif
    smoke_co_alarm_config.smoke_co_alarm.feature_flags = feature_flags;

    endpoint_t *endpoint = smoke_co_alarm::create(node, &smoke_co_alarm_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create smoke/CO alarm endpoint");
        return;
    }
    smoke_co_alarm_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Smoke/CO alarm endpoint id: %u", smoke_co_alarm_endpoint_id);

    /* Groups (server) — optionalConform on SmokeCOAlarm.xml (Matter Device
     * Types Reference audit, see CLAUDE.md's own "Open next steps"). A
     * plain, self-contained cluster shell — lets a controller add this
     * alarm to a group for group-addressed commands (e.g. a whole-house
     * "test all alarms" scene), same trivial addition firmware/switch/'s
     * own Groups client cluster already establishes, just server-side
     * here since this device receives group commands rather than sending
     * them. */
    cluster::groups::config_t groups_config;
    cluster::groups::create(endpoint, &groups_config, CLUSTER_FLAG_SERVER);

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
    /* Carbon Monoxide Concentration Measurement (server) — also
     * optionalConform on this XML, only added when an MQ7 is actually
     * wired up. LevelIndication (LEV) only, deliberately NOT
     * NumericMeasurement: the header comment above already explains in
     * full why this firmware doesn't expose a calibrated ppm figure from
     * the MQ7's own raw millivolt reading (MQ-series ppm curves shift per
     * sensor/module/burn-in state) — LevelIndication is the spec's own
     * qualitative alternative, letting this cluster honestly report the
     * SAME Normal/Warning/Critical classification `classify()` already
     * computes for SmokeCoAlarm's own COState, via `LevelValueEnum`
     * (Low/Medium/High/Critical) instead of a fabricated numeric value —
     * see co_alarm_state_to_level_value() below and its call site in
     * sensor_task(). */
    cluster::carbon_monoxide_concentration_measurement::config_t co_concentration_config;
    co_concentration_config.measurement_medium =
        chip::to_underlying(CarbonMonoxideConcentrationMeasurement::MeasurementMediumEnum::kAir);
    co_concentration_config.feature_flags = chip::to_underlying(CarbonMonoxideConcentrationMeasurement::Feature::kLevelIndication);
    co_concentration_config.features.level_indication.level_value =
        chip::to_underlying(CarbonMonoxideConcentrationMeasurement::LevelValueEnum::kUnknown);
    cluster::carbon_monoxide_concentration_measurement::create(endpoint, &co_concentration_config, CLUSTER_FLAG_SERVER);
#endif

#if SMOKE_CO_ALARM_HAS_TEMP_HUMIDITY
    /* Temperature Measurement and Relative Humidity Measurement (both
     * server, also optionalConform on this XML) — see the header comment
     * above ("hobbyist cluster expansion" pilot) for why these are no
     * longer skipped. Code-driven (registry-lookup + SetMeasuredValue()),
     * confirmed via the same check every other code-driven cluster in
     * this repo uses (a real temperature_measurement/
     * relative_humidity_measurement folder exists under
     * data_model_provider/clusters/). */
    cluster::temperature_measurement::config_t temp_config;
    temp_config.measured_value = nullable<int16_t>();
    temp_config.min_measured_value = nullable<int16_t>(-4000);
    temp_config.max_measured_value = nullable<int16_t>(8500);
    cluster::temperature_measurement::create(endpoint, &temp_config, CLUSTER_FLAG_SERVER);

    cluster::relative_humidity_measurement::config_t hum_config;
    hum_config.measured_value = nullable<uint16_t>();
    hum_config.min_measured_value = nullable<uint16_t>(0);
    hum_config.max_measured_value = nullable<uint16_t>(10000);
    cluster::relative_humidity_measurement::create(endpoint, &hum_config, CLUSTER_FLAG_SERVER);
#endif

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

    /* 5. Start reading the sensor(s) now that the data model + Matter
     * stack both exist — sensor_task() writes into the cluster created
     * above. */
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter smoke/CO alarm started. Scan the QR code to commission.");
}
