/*
 * Minimal Matter Room Air Conditioner — thirty-second device type, and this
 * repo's first to combine firmware/thermostat/'s own Thermostat control-loop
 * pattern with firmware/fan/'s own FanControl Delegate pattern on a single
 * endpoint. Recommended twice before (during firmware/evse/'s and firmware/
 * water-heater/'s own AskUserQuestion rounds) but not chosen until now.
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
 * --- Endpoint: a complete top-level helper, and this repo's first with TWO
 * separate, genuinely different "on/off" concepts on one endpoint ----------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * RoomAirConditioner.xml: Identify, On/Off (DeadFrontOnOff feature), and
 * Thermostat are all `<mandatoryConform/>` — Groups, ScenesManagement,
 * FanControl, ThermostatUserInterfaceConfiguration, HEPA/Activated Carbon
 * Filter Monitoring, TemperatureMeasurement, and RelativeHumidityMeasurement
 * are all `<optionalConform/>`. `endpoint::room_air_conditioner::create()`
 * confirmed complete/ready-to-use by reading `esp_matter_endpoint.cpp`'s own
 * `room_air_conditioner::add()` directly: Identify + OnOff (with the
 * DeadFrontOnOff feature added, plus On + Toggle commands explicitly
 * created — Off is already part of the base OnOff cluster, same as
 * firmware/pump/'s own On/Off) + Thermostat (with the Cooling feature
 * force-added via `config->thermostat.feature_flags |= feature::cooling::
 * get_id()` inside `add()` itself — this app never needs to set that bit),
 * auto-Descriptor via `common::create<T>()`.
 *
 * A real, previously-unseen spec detail in this repo: unlike every other
 * appliance device type here (Dishwasher/Laundry Washer/Laundry Dryer),
 * where On/Off's DeadFrontOnOff feature is `<optionalConform/>` and simply
 * left out (a narrow "is the device's own UI/front-panel powered" semantic,
 * not a real power switch), this device type makes BOTH that On/Off cluster
 * AND a separate Thermostat SystemMode `<mandatoryConform/>` at once — two
 * genuinely different concepts of "on" coexist on the same endpoint. This
 * file deliberately ties NO GPIO to the OnOff/DeadFrontOnOff attribute at
 * all: it is left exactly as the top-level helper wires it (a plain ember
 * attribute a controller can read/write with no physical effect in this
 * hobby build, representing whether the unit's own display/front-panel is
 * lit) — the ACTUAL cooling output is driven entirely by Thermostat's own
 * SystemMode (Off/Cool) further down, the same split firmware/thermostat/'s
 * own header comment would recognize (that file has no separate OnOff
 * cluster at all — SystemMode alone represents its on/off state) but made
 * explicit here since this device type genuinely has both.
 *
 * --- Thermostat: Cool-only, same hysteresis control loop as firmware/
 * thermostat/'s own RELAY output mode, just without the Heat branch --------
 * ControlSequenceOfOperation is CoolingOnly (0x00, confirmed against the
 * cluster's own `ControlSequenceOfOperationEnum` — the Cooling feature bit
 * the endpoint helper force-adds is the only feature this file enables).
 * SystemMode is therefore only ever meaningfully Off or Cool — any other
 * value (Auto/Heat/EmergencyHeat/Precooling/FanOnly/Dry/Sleep) is treated
 * as Off rather than guessed at, same "don't implement what wasn't asked
 * for" scope cut firmware/thermostat/'s own SystemMode handling already
 * uses for its own out-of-scope modes. LocalTemperature/SystemMode/
 * OccupiedCoolingSetpoint are all plain ember attributes (confirmed: no
 * `thermostat/` folder under `data_model_provider/clusters/`, the exact
 * check firmware/thermostat/'s own header comment already documents in
 * full) — same `attribute::PRE_UPDATE` + `attribute::update()` pattern used
 * throughout this repo, no code-driven setter needed.
 * `ROOM_AC_SENSOR_GPIO` reuses the exact DS18B20 1-Wire driver firmware/
 * thermostat/'s/firmware/water-heater/'s/firmware/laundry-dryer/'s own DS18B20
 * path already establishes verbatim — deliberately just this one sensor
 * rather than firmware/thermostat/'s full 7-chip `SENSOR_TYPE` library, the
 * same "smallest reasonable next step" scope cut firmware/water-heater/'s
 * own header comment already applies to its own tank-probe choice (that
 * richer library is still there in firmware/thermostat/ if a future device
 * type wants it). `ROOM_AC_HYSTERESIS_CENTIDEGREES` (0.3 degC) matches
 * firmware/thermostat/'s own default — a room's air responds meaningfully
 * faster than a water tank's own thermal mass, so the tighter band is
 * appropriate here too, not just reused for convenience.
 *
 * --- FanControl: reused near-verbatim from firmware/fan/'s own Delegate,
 * added manually onto this already-correct endpoint -----------------------
 * Confirmed by reading `esp_matter_cluster.cpp`'s own cluster-level
 * `fan_control::create()` directly: unlike firmware/fan/'s and firmware/
 * air-purifier/'s and firmware/extractor-hood/'s own device types (where
 * FanControl is `<mandatoryConform/>` and wired in automatically by their
 * own top-level endpoint helpers), this device type's FanControl is
 * `<optionalConform/>` — `room_air_conditioner::config_t` has no
 * `fan_control` field at all, confirmed by reading the header directly, so
 * this file adds it manually via the lower-level `cluster::fan_control::
 * create()` free function instead, the same "add extra clusters onto an
 * already-correct endpoint" pattern used throughout this repo. The
 * cluster-level `config_t` DOES expose its own `delegate` field (wired via
 * `set_delegate_and_init_callback()` at create time, unlike the endpoint-
 * level path) — but this file deliberately does NOT rely on that,
 * following firmware/air-purifier/'s and firmware/extractor-hood/'s own
 * proven-correct convention instead: `config.delegate` stays null at
 * create time, and the real Delegate is attached afterward via
 * connectedhomeip's own `FanControl::SetDefaultDelegate()` free function,
 * called only AFTER `esp_matter::start()` — see firmware/fan/'s own header
 * comment for the full, hard-won story of why calling it any earlier is a
 * silent no-op (the live `FanControlCluster` instance this call needs to
 * find doesn't exist until `esp_matter::start()`'s own init pass runs).
 * `HandleStep()`/`OnFanDriveStateChanged()`/the PercentSetting-only scope/
 * the `OffLowMedHigh` FanModeSequence choice are all reused verbatim from
 * firmware/fan/ — see that file's own header comment for the complete
 * detail on each. The fan's own PercentSetting is deliberately NOT coupled
 * to Thermostat's own cool demand — a controller can run the fan alone
 * (real room ACs commonly support this), same as leaving the two clusters'
 * state machines independent of each other rather than inventing a
 * coupling neither cluster's own spec requires.
 *
 * --- Output: compressor relay (Thermostat-driven) + fan PWM (FanControl-
 * driven), two independent GPIOs --------------------------------------
 * `ROOM_AC_COMPRESSOR_RELAY_GPIO` (active-LOW, matching this repo's
 * established relay convention) is gated purely by the Cool-only hysteresis
 * loop below — a real compressor contactor/relay module, not the mandatory
 * OnOff/DeadFrontOnOff attribute (see above). `ROOM_AC_FAN_PWM_GPIO` is
 * real PWM via ESP-IDF's driver/ledc.h, the same LEDC peripheral firmware/
 * fan/'s own output already uses.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/DS18B20/fan-driver hardware for this device
 * type physically available when written).
 *
 * --- Later extended: TemperatureMeasurement + RelativeHumidityMeasurement,
 * on request (continuing the `clusterOptions` rollout firmware/cooktop/,
 * firmware/pump/, and firmware/soil-sensor/ started) --------------------
 * Both are `<optionalConform/>` on RoomAirConditioner.xml (see above),
 * originally skipped for the usual "smallest reasonable next step"
 * reasoning, now independently checkable — `ROOM_AC_HAS_TEMPERATURE_
 * MEASUREMENT`/`ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT`, both default
 * off, unchanged default build. The two are genuinely different kinds of
 * addition, not a matched pair:
 *
 * TemperatureMeasurement needs NO new sensor at all — this endpoint
 * already reads a real DS18B20 ambient reading every
 * `ROOM_AC_CONTROL_INTERVAL_MS` cycle for Thermostat's own LocalTemperature
 * (see above); enabling this cluster just reports that SAME value a
 * second time, via a standalone, code-driven TemperatureMeasurement
 * cluster (confirmed the usual registry-lookup-and-cast `SetMeasuredValue()`
 * pattern, no special init-order requirement) — genuinely free to add.
 * MinMeasuredValue/MaxMeasuredValue use the DS18B20's own real rated
 * range (-55.00 to 125.00 degC), the physical sensor's own limits.
 *
 * RelativeHumidityMeasurement, by contrast, needs a genuinely NEW sensor
 * — this device's own existing DS18B20 probe has no humidity output at
 * all. `ROOM_AC_HUMIDITY_CHIP` offers the same 4-chip I2C library
 * (SHT3x/SHT4x/AHT20/BME280) firmware/temperature-sensor/'s own driver
 * already established and firmware/air-quality-sensor/'s and firmware/
 * smoke-co-alarm/'s own clusterOptions rollouts already reused, ported
 * byte-for-byte from firmware/smoke-co-alarm/'s own copy. Each chip's own
 * driver reads BOTH temperature and humidity in one I2C transaction (all
 * four are combined temp/humidity sensors) — only the humidity half is
 * exposed via Matter here, since TemperatureMeasurement already comes
 * from the DS18B20 above; the chip's own temperature reading is taken but
 * discarded, the same "read but unused" precedent firmware/humidity-
 * sensor/'s own header comment already documents for its own reused
 * chips. Needs a genuinely new, dedicated I2C bus (`ROOM_AC_HUMIDITY_SDA_
 * GPIO`/`_SCL_GPIO`, default 32/33) since this device has no existing I2C
 * bus at all — GPIOs chosen to avoid the compressor relay (16)/fan PWM
 * (17)/DS18B20 (21)/identify LED (2) already in use.
 *
 * Both clusters are added onto the SAME Room Air Conditioner endpoint via
 * the usual "add extra clusters onto an already-correct endpoint"
 * pattern. The existing `control_task`'s own 5s loop reports
 * TemperatureMeasurement and (if enabled) polls + reports the humidity
 * chip alongside its existing DS18B20 read and control-loop re-evaluation
 * — no second task needed, since a room's humidity changes on a similar
 * timescale to its temperature. Wizard integration needed one small,
 * genuinely new addition to the shared `clusterOptions` mechanism: a
 * clusterOptions entry with neither `chip` nor `group` (TemperatureMeasurement
 * here) simply has no chip-choice/pin UI to render — but `buildSedCommands()`
 * only ever emits a cluster's own enable-define through a backing CHIP's
 * `enableDefineName`, so this needed a trivial "virtual chip" COMPONENT_LIBRARY
 * entry with an empty `pins: []` array purely to carry `enableDefineName` —
 * confirmed by reading the render code directly that an empty `pins` array
 * already renders no pin-input block at all (the existing `!chip.pins.length`
 * guard), so this needed zero new mechanism code, only a new kind of
 * COMPONENT_LIBRARY entry. Build-verified in Docker for the unchanged
 * default (both off) config, each toggle individually, and both together
 * (with all 4 humidity chip choices); not hardware-tested (no DS18B20/
 * SHT3x-class hardware for this specific addition physically available
 * when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/fan-control-server/CodegenIntegration.h>
#include <app/clusters/resource-monitoring-server/ResourceMonitoringCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/clusters/relative-humidity-measurement-server/RelativeHumidityMeasurementCluster.h>
#include <data_model_provider/clusters/resource_monitor/integration.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_room_air_conditioner";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay/fan-driver module" — polarity/signal type isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define ROOM_AC_COMPRESSOR_RELAY_GPIO GPIO_NUM_16   /* active-LOW */
#define ROOM_AC_FAN_PWM_GPIO GPIO_NUM_17              /* PWM to a fan-speed-controller/MOSFET */
#define ROOM_AC_SENSOR_GPIO GPIO_NUM_21                /* DS18B20, room air temperature */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Cooling-setpoint target range (Matter's global `temperature` type — int16,
 * hundredths of a degree C). 16.00-30.00 degC, default 24.00 degC — an
 * ordinary real room-air-conditioner target range. */
#define ROOM_AC_COOLING_SETPOINT_MIN_CENTIDEGREES 1600
#define ROOM_AC_COOLING_SETPOINT_MAX_CENTIDEGREES 3000
#define ROOM_AC_COOLING_SETPOINT_DEFAULT_CENTIDEGREES 2400

/* Bang-bang (hysteresis) control band — same 0.3 degC default firmware/
 * thermostat/'s own control loop uses; see the header comment above. */
#define ROOM_AC_HYSTERESIS_CENTIDEGREES 30

/* LEDC (PWM) setup for the fan output — same pattern firmware/fan/'s own
 * FAN_PWM_GPIO already establishes. */
#define ROOM_AC_FAN_LEDC_TIMER LEDC_TIMER_0
#define ROOM_AC_FAN_LEDC_CHANNEL LEDC_CHANNEL_0
#define ROOM_AC_FAN_LEDC_MODE LEDC_LOW_SPEED_MODE
#define ROOM_AC_FAN_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define ROOM_AC_FAN_LEDC_FREQUENCY_HZ 25000 /* above the audible range */

/* How often the control task re-reads the sensor and re-evaluates the
 * compressor hysteresis. */
#define ROOM_AC_CONTROL_INTERVAL_MS 5000

/* --- Optional TemperatureMeasurement + RelativeHumidityMeasurement — see
 * the header comment above for the full sourcing/reuse detail. Both
 * default off, unchanged default build.
 *
 * ROOM_AC_HAS_TEMPERATURE_MEASUREMENT reuses the existing DS18B20 above —
 * no new sensor needed. Deliberately no inline comment on its own #define
 * line below (unlike most of this file's other constants) — the product
 * wizard's own generated sed command for it rewrites the WHOLE line (a
 * broad `.*` match, since this is a real two-state 0/1 toggle rather than
 * a fixed sentinel), which would silently strip a trailing inline comment
 * the first time a product actually enables it — the same real,
 * previously-caught bug class firmware/soil-sensor/'s own header comment
 * already documents in full for its own two calibration #defines. */
#define ROOM_AC_HAS_TEMPERATURE_MEASUREMENT 0
#define ROOM_AC_TEMP_MIN_CENTIDEGREES (-5500)  /* -55.00 degC — DS18B20's own rated range */
#define ROOM_AC_TEMP_MAX_CENTIDEGREES 12500    /* 125.00 degC — DS18B20's own rated range */

#define ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT 0
#define ROOM_AC_HUMIDITY_CHIP_SHT3X 1
#define ROOM_AC_HUMIDITY_CHIP_SHT4X 2
#define ROOM_AC_HUMIDITY_CHIP_AHT20 3
#define ROOM_AC_HUMIDITY_CHIP_BME280 4
#define ROOM_AC_HUMIDITY_CHIP ROOM_AC_HUMIDITY_CHIP_SHT3X

/* A dedicated I2C bus — this device has no existing one (the DS18B20 is
 * 1-Wire, not I2C). Defaults avoid the compressor relay (16)/fan PWM
 * (17)/DS18B20 (21)/identify LED (2) already in use. */
#define ROOM_AC_HUMIDITY_SDA_GPIO GPIO_NUM_32
#define ROOM_AC_HUMIDITY_SCL_GPIO GPIO_NUM_33
#define ROOM_AC_HUMIDITY_I2C_FREQ_HZ 100000

/* --- Filter monitoring (Matter Device Types Reference audit) ------------
 * HEPA + Activated Carbon Filter Monitoring, both optionalConform on this
 * device type's own XML (confirmed directly against the CSA's own
 * RoomAirConditioner.xml) — a real room air conditioner's own air-intake
 * filters, tracked the same fan-runtime-based way firmware/air-purifier/'s
 * and firmware/extractor-hood/'s own filter clusters already are (see
 * either file's own header comment for the full "why runtime, not wall-
 * clock time" reasoning): accumulated seconds the fan has actually been
 * running, persisted to its own NVS namespace every
 * ROOM_AC_FILTER_NVS_SAVE_INTERVAL_MS while running (not every tick, to
 * avoid flash wear), against each filter's own configurable rated life in
 * hours. HEPA/carbon life figures reused from firmware/air-purifier/'s own
 * defaults (2000h/1000h) — a room air conditioner's intake filters see
 * broadly comparable duty to a dedicated air purifier's, unlike firmware/
 * extractor-hood/'s own much shorter kitchen-grease-exposure figures. */
#define ROOM_AC_FILTER_POLL_INTERVAL_MS 5000
#define ROOM_AC_FILTER_NVS_SAVE_INTERVAL_MS 60000
#define ROOM_AC_FILTER_NVS_NAMESPACE "filter_life"
#define ROOM_AC_FILTER_NVS_KEY "run_seconds"
#define ROOM_AC_HEPA_FILTER_LIFE_HOURS 2000
#define ROOM_AC_CARBON_FILTER_LIFE_HOURS 1000
#define ROOM_AC_FILTER_CHANGE_WARNING_PERCENT 20
#define ROOM_AC_FILTER_CHANGE_CRITICAL_PERCENT 5

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t room_ac_endpoint_id = 0;
static uint32_t filter_total_run_seconds = 0;
static uint8_t g_fan_percent_setting = 0;
static esp_timer_handle_t identify_led_timer = NULL;
#if ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT
static bool room_ac_humidity_ok = false;
#endif

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/laundry-dryer/'s (itself from firmware/
 * laundry-washer/'s / firmware/dishwasher/'s / firmware/refrigerator/'s /
 * firmware/water-heater/'s / firmware/thermostat/'s) DS18B20 driver — see
 * those files' own header comments for the 1-Wire timing/CRC detail and
 * sourcing. */
static bool ow_reset(void)
{
    gpio_set_level(ROOM_AC_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(ROOM_AC_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(ROOM_AC_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(ROOM_AC_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(ROOM_AC_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(ROOM_AC_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(ROOM_AC_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(ROOM_AC_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(ROOM_AC_SENSOR_GPIO);
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
    io_conf.pin_bit_mask = (1ULL << ROOM_AC_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(ROOM_AC_SENSOR_GPIO, 1);
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

/* --- Cross-cutting state --------------------------------------------------
 * `room_ac_system_mode` mirrors Thermostat's own SystemMode attribute;
 * `room_ac_cooling_setpoint_centidegrees` mirrors OccupiedCoolingSetpoint.
 * Both written by app_attribute_update_cb()'s own PRE_UPDATE handling, read
 * by run_control_loop(). */
static uint8_t room_ac_system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
static int16_t room_ac_cooling_setpoint_centidegrees = ROOM_AC_COOLING_SETPOINT_DEFAULT_CENTIDEGREES;
static bool room_ac_local_temperature_valid = false;
static int16_t room_ac_local_temperature_centidegrees = 0;
static bool room_ac_cool_demand = false;

/* LocalTemperature is a plain ember attribute (see the header comment
 * above) — a direct attribute::update() call, same pattern firmware/
 * thermostat/'s own update_local_temperature() already establishes. */
static void update_local_temperature(nullable<int16_t> value)
{
    esp_matter_attr_val_t val = esp_matter_nullable_int16(value);
    attribute::update(room_ac_endpoint_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
}

/* Same registry-lookup-and-cast pattern as every other TemperatureMeasurement
 * instance in this repo — plain code-driven cluster, no special init-order
 * requirement. Reuses the SAME sensor_read() value control_task() already
 * computes each cycle for LocalTemperature above — see the header comment
 * for why this is genuinely free to add. */
static void update_temperature_measurement(chip::app::DataModel::Nullable<int16_t> value)
{
    chip::app::ConcreteClusterPath path(room_ac_endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "TemperatureMeasurement cluster not found on endpoint %u", room_ac_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(value);
}

#if ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT
/* ======================================================================
 * Humidity driver — the same 4 I2C chip options firmware/temperature-
 * sensor/'s own driver established, ported byte-for-byte from firmware/
 * smoke-co-alarm/'s own copy (itself already reused in firmware/
 * air-quality-sensor/) — see the header comment above for the full
 * sourcing. Each chip's own driver reads BOTH temperature and humidity in
 * one transaction; only the humidity half is exposed here (Temperature
 * already comes from the DS18B20 above), the temperature reading is taken
 * but discarded.
 * ====================================================================== */
static i2c_master_dev_handle_t room_ac_humidity_i2c_dev = NULL;

static bool room_ac_humidity_i2c_bus_setup(uint16_t device_address)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = ROOM_AC_HUMIDITY_SDA_GPIO;
    bus_config.scl_io_num = ROOM_AC_HUMIDITY_SCL_GPIO;
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
    dev_config.scl_speed_hz = ROOM_AC_HUMIDITY_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &room_ac_humidity_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Sensirion CRC-8 (polynomial 0x31, init 0xFF) — used by SHT3x/SHT4x. */
static uint8_t room_ac_sensirion_crc8(const uint8_t *data, size_t len)
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

#if ROOM_AC_HUMIDITY_CHIP == ROOM_AC_HUMIDITY_CHIP_SHT3X

#define ROOM_AC_HUMIDITY_SHT3X_I2C_ADDR 0x44 /* 0x45 if ADDR is tied to VDD */

static bool room_ac_humidity_setup(void)
{
    return room_ac_humidity_i2c_bus_setup(ROOM_AC_HUMIDITY_SHT3X_I2C_ADDR);
}

static bool room_ac_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[2] = {0x24, 0x00}; /* single shot, high repeatability */
    if (i2c_master_transmit(room_ac_humidity_i2c_dev, cmd, sizeof(cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    if (i2c_master_receive(room_ac_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (room_ac_sensirion_crc8(data, 2) != data[2] || room_ac_sensirion_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT3x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = 100.0f * ((float)hum_ticks / 65535.0f);
    return true;
}

#elif ROOM_AC_HUMIDITY_CHIP == ROOM_AC_HUMIDITY_CHIP_SHT4X

#define ROOM_AC_HUMIDITY_SHT4X_I2C_ADDR 0x44

static bool room_ac_humidity_setup(void)
{
    return room_ac_humidity_i2c_bus_setup(ROOM_AC_HUMIDITY_SHT4X_I2C_ADDR);
}

static bool room_ac_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[1] = {0xFD}; /* measure T & RH, high precision */
    if (i2c_master_transmit(room_ac_humidity_i2c_dev, cmd, sizeof(cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6];
    if (i2c_master_receive(room_ac_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (room_ac_sensirion_crc8(data, 2) != data[2] || room_ac_sensirion_crc8(data + 3, 2) != data[5]) {
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

#elif ROOM_AC_HUMIDITY_CHIP == ROOM_AC_HUMIDITY_CHIP_AHT20

#define ROOM_AC_HUMIDITY_AHT20_I2C_ADDR 0x38

static bool room_ac_humidity_setup(void)
{
    if (!room_ac_humidity_i2c_bus_setup(ROOM_AC_HUMIDITY_AHT20_I2C_ADDR)) {
        return false;
    }
    const uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
    if (i2c_master_transmit(room_ac_humidity_i2c_dev, init_cmd, sizeof(init_cmd), 1000) != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 init command failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    return true;
}

static bool room_ac_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t trigger_cmd[3] = {0xAC, 0x33, 0x00};
    if (i2c_master_transmit(room_ac_humidity_i2c_dev, trigger_cmd, sizeof(trigger_cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(85));

    uint8_t data[6];
    if (i2c_master_receive(room_ac_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
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

#elif ROOM_AC_HUMIDITY_CHIP == ROOM_AC_HUMIDITY_CHIP_BME280

#define ROOM_AC_HUMIDITY_BME280_I2C_ADDR 0x76 /* 0x77 if SDO is tied to VDD */
#define ROOM_AC_HUMIDITY_BME280_REG_CHIP_ID 0xD0
#define ROOM_AC_HUMIDITY_BME280_REG_CALIB_T 0x88
#define ROOM_AC_HUMIDITY_BME280_REG_CALIB_H1 0xA1
#define ROOM_AC_HUMIDITY_BME280_REG_CALIB_H2 0xE1
#define ROOM_AC_HUMIDITY_BME280_REG_CTRL_HUM 0xF2
#define ROOM_AC_HUMIDITY_BME280_REG_CTRL_MEAS 0xF4
#define ROOM_AC_HUMIDITY_BME280_REG_DATA 0xFA
#define ROOM_AC_HUMIDITY_BME280_CHIP_ID_EXPECTED 0x60

struct room_ac_bme280_calib_data {
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

static struct room_ac_bme280_calib_data room_ac_bme280_calib;

static bool room_ac_bme280_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(room_ac_humidity_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool room_ac_bme280_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(room_ac_humidity_i2c_dev, &reg, 1, out, len, 1000) == ESP_OK;
}

static int16_t room_ac_sign_extend_12bit(uint16_t value)
{
    return (int16_t)((value & 0x0800) ? (value | 0xF000) : value);
}

static bool room_ac_humidity_setup(void)
{
    if (!room_ac_humidity_i2c_bus_setup(ROOM_AC_HUMIDITY_BME280_I2C_ADDR)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (!room_ac_bme280_read_regs(ROOM_AC_HUMIDITY_BME280_REG_CHIP_ID, &chip_id, 1) || chip_id != ROOM_AC_HUMIDITY_BME280_CHIP_ID_EXPECTED) {
        ESP_LOGE(TAG, "BME280 chip ID mismatch (got 0x%02X, expected 0x%02X)", chip_id, ROOM_AC_HUMIDITY_BME280_CHIP_ID_EXPECTED);
        return false;
    }

    uint8_t calib_t[6];
    if (!room_ac_bme280_read_regs(ROOM_AC_HUMIDITY_BME280_REG_CALIB_T, calib_t, sizeof(calib_t))) {
        return false;
    }
    room_ac_bme280_calib.dig_t1 = (uint16_t)(calib_t[0] | (calib_t[1] << 8));
    room_ac_bme280_calib.dig_t2 = (int16_t)(calib_t[2] | (calib_t[3] << 8));
    room_ac_bme280_calib.dig_t3 = (int16_t)(calib_t[4] | (calib_t[5] << 8));

    uint8_t dig_h1 = 0;
    if (!room_ac_bme280_read_regs(ROOM_AC_HUMIDITY_BME280_REG_CALIB_H1, &dig_h1, 1)) {
        return false;
    }
    room_ac_bme280_calib.dig_h1 = dig_h1;

    uint8_t calib_h[7];
    if (!room_ac_bme280_read_regs(ROOM_AC_HUMIDITY_BME280_REG_CALIB_H2, calib_h, sizeof(calib_h))) {
        return false;
    }
    room_ac_bme280_calib.dig_h2 = (int16_t)(calib_h[0] | (calib_h[1] << 8));
    room_ac_bme280_calib.dig_h3 = calib_h[2];
    room_ac_bme280_calib.dig_h4 = room_ac_sign_extend_12bit((uint16_t)((calib_h[3] << 4) | (calib_h[4] & 0x0F)));
    room_ac_bme280_calib.dig_h5 = room_ac_sign_extend_12bit((uint16_t)((calib_h[5] << 4) | (calib_h[4] >> 4)));
    room_ac_bme280_calib.dig_h6 = (int8_t)calib_h[6];

    if (!room_ac_bme280_write_reg(ROOM_AC_HUMIDITY_BME280_REG_CTRL_HUM, 0x01)) {
        return false;
    }
    return true;
}

static int32_t room_ac_bme280_compensate_temperature(int32_t adc_t, int32_t *t_fine)
{
    int32_t var1 = ((adc_t / 8) - ((int32_t)room_ac_bme280_calib.dig_t1 * 2)) * ((int32_t)room_ac_bme280_calib.dig_t2) / 2048;
    int32_t var2_pre = (adc_t / 16) - ((int32_t)room_ac_bme280_calib.dig_t1);
    int32_t var2 = (((var2_pre * var2_pre) / 4096) * ((int32_t)room_ac_bme280_calib.dig_t3)) / 16384;
    *t_fine = var1 + var2;
    int32_t temperature = (*t_fine * 5 + 128) / 256;
    if (temperature < -4000) {
        temperature = -4000;
    } else if (temperature > 8500) {
        temperature = 8500;
    }
    return temperature;
}

static uint32_t room_ac_bme280_compensate_humidity(int32_t adc_h, int32_t t_fine)
{
    int32_t var1 = t_fine - 76800;
    int32_t var2 = adc_h * 16384;
    int32_t var3 = ((int32_t)room_ac_bme280_calib.dig_h4) * 1048576;
    int32_t var4 = ((int32_t)room_ac_bme280_calib.dig_h5) * var1;
    int32_t var5 = (((var2 - var3) - var4) + 16384) / 32768;
    var2 = (var1 * ((int32_t)room_ac_bme280_calib.dig_h6)) / 1024;
    var3 = (var1 * ((int32_t)room_ac_bme280_calib.dig_h3)) / 2048;
    var4 = ((var2 * (var3 + 32768)) / 1024) + 2097152;
    var2 = ((var4 * ((int32_t)room_ac_bme280_calib.dig_h2)) + 8192) / 16384;
    var3 = var5 * var2;
    int32_t var4b = ((var3 / 32768) * (var3 / 32768)) / 128;
    int32_t var5b = var3 - ((var4b * ((int32_t)room_ac_bme280_calib.dig_h1)) / 16);
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

static bool room_ac_humidity_read(float *temperature_c, float *humidity_pct)
{
    if (!room_ac_bme280_write_reg(ROOM_AC_HUMIDITY_BME280_REG_CTRL_MEAS, 0x25)) { /* osrs_t=x1, osrs_p=x1, forced mode */
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[5];
    if (!room_ac_bme280_read_regs(ROOM_AC_HUMIDITY_BME280_REG_DATA, data, sizeof(data))) {
        return false;
    }

    int32_t adc_t = (int32_t)(((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | (data[2] >> 4));
    int32_t adc_h = (int32_t)(((uint32_t)data[3] << 8) | data[4]);

    int32_t t_fine = 0;
    int32_t temp_centidegrees = room_ac_bme280_compensate_temperature(adc_t, &t_fine);
    uint32_t hum_q22_10 = room_ac_bme280_compensate_humidity(adc_h, t_fine);

    *temperature_c = temp_centidegrees / 100.0f;
    *humidity_pct = hum_q22_10 / 1024.0f;
    return true;
}

#else
#error "Unknown ROOM_AC_HUMIDITY_CHIP"
#endif

/* Same registry-lookup-and-cast pattern as update_temperature_measurement()
 * above — RelativeHumidityMeasurement is also a code-driven cluster. */
static void update_room_ac_humidity(chip::app::DataModel::Nullable<uint16_t> value)
{
    chip::app::ConcreteClusterPath path(room_ac_endpoint_id, RelativeHumidityMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "RelativeHumidityMeasurement cluster not found on endpoint %u", room_ac_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::RelativeHumidityMeasurementCluster *>(iface)->SetMeasuredValue(value);
}
#endif /* ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT */

static void set_compressor(bool on)
{
    room_ac_cool_demand = on;
    gpio_set_level(ROOM_AC_COMPRESSOR_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

/* The actual bang-bang (hysteresis) control decision — Cool-only subset of
 * firmware/thermostat/'s own run_control_loop(), see the header comment
 * above for why there's no Heat branch here at all. Only acts while
 * room_ac_local_temperature_valid (an unknown room temperature must never
 * be treated as "warm enough to cool"). Keeps the PREVIOUS demand state
 * inside the deadband — a hysteresis band means "don't switch yet", not
 * "switch off". */
static void run_control_loop(void)
{
    bool new_cool_demand = room_ac_cool_demand;

    if (!room_ac_local_temperature_valid || room_ac_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kOff)) {
        new_cool_demand = false;
    } else if (room_ac_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool)) {
        if (room_ac_local_temperature_centidegrees >= room_ac_cooling_setpoint_centidegrees + ROOM_AC_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = true;
        } else if (room_ac_local_temperature_centidegrees <= room_ac_cooling_setpoint_centidegrees - ROOM_AC_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = false;
        }
    } else {
        /* Any other SystemMode value isn't implemented (ControlSequenceOfOperation
         * is CoolingOnly, so nothing else should normally be commanded) —
         * treat as Off rather than guessing, same scope cut firmware/
         * thermostat/'s own SystemMode handling already uses. */
        new_cool_demand = false;
    }

    if (new_cool_demand != room_ac_cool_demand) {
        ESP_LOGI(TAG, "Cool demand changed: %s (room %.2f degC, setpoint %.2f degC)",
                 new_cool_demand ? "ON" : "off",
                 room_ac_local_temperature_centidegrees / 100.0f,
                 room_ac_cooling_setpoint_centidegrees / 100.0f);
        set_compressor(new_cool_demand);
    }
}

/* Periodically reads the sensor, pushes LocalTemperature, and re-runs the
 * control loop. Runs as its own task rather than inline in app_main() so it
 * can freely block on 1-Wire transactions and vTaskDelay() without holding
 * up Matter's own startup/event handling — same reasoning as firmware/
 * thermostat/'s own sensor_task(). */
static void control_task(void *arg)
{
    for (;;) {
        float temperature_c = 0.0f;

        if (sensor_read(&temperature_c)) {
            int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
            ESP_LOGI(TAG, "Room air: %.2f degC", temperature_c);
            room_ac_local_temperature_valid = true;
            room_ac_local_temperature_centidegrees = temp_centidegrees;
            update_local_temperature(nullable<int16_t>(temp_centidegrees));
#if ROOM_AC_HAS_TEMPERATURE_MEASUREMENT
            update_temperature_measurement(chip::app::DataModel::Nullable<int16_t>(temp_centidegrees));
#endif
        } else {
            room_ac_local_temperature_valid = false;
            update_local_temperature(nullable<int16_t>());
#if ROOM_AC_HAS_TEMPERATURE_MEASUREMENT
            update_temperature_measurement(chip::app::DataModel::Nullable<int16_t>());
#endif
        }

#if ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT
        if (room_ac_humidity_ok) {
            float humidity_temp_c = 0.0f, humidity_pct = 0.0f;
            if (room_ac_humidity_read(&humidity_temp_c, &humidity_pct)) {
                uint16_t hum_centipercent = (uint16_t)(humidity_pct * 100.0f);
                update_room_ac_humidity(chip::app::DataModel::Nullable<uint16_t>(hum_centipercent));
                ESP_LOGI(TAG, "Room humidity: %.2f %%RH", humidity_pct);
            } else {
                update_room_ac_humidity(chip::app::DataModel::Nullable<uint16_t>());
            }
        }
#endif

        run_control_loop();
        vTaskDelay(pdMS_TO_TICKS(ROOM_AC_CONTROL_INTERVAL_MS));
    }
}

/* Drives the LEDC duty from a 0-100 percent value — PercentSetting's own
 * range, used directly with no remapping, same as firmware/fan/'s own
 * set_output(). */
static void set_fan_output(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(ROOM_AC_FAN_LEDC_MODE, ROOM_AC_FAN_LEDC_CHANNEL, duty);
    ledc_update_duty(ROOM_AC_FAN_LEDC_MODE, ROOM_AC_FAN_LEDC_CHANNEL);
}

/* FanControl's real Delegate — reused near-verbatim from firmware/fan/'s
 * own FanDelegate, see the header comment above for the full detail on why
 * this cluster needs one at all and why SetDefaultDelegate() is called
 * after esp_matter::start() rather than relying on config->delegate. */
class FanDelegate : public FanControl::Delegate
{
public:
    chip::Protocols::InteractionModel::Status HandleStep(FanControl::StepDirectionEnum direction, bool wrap,
                                                          bool lowestOff) override
    {
        /* Step feature not implemented — pure virtual in the base Delegate
         * class, so it must exist even though nothing enables the feature
         * that would let a controller send this command. */
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;
    }

    /* Fires after FanMode or PercentSetting settle (FanControlCluster's own
     * FanMode->PercentSetting cascade means reacting to percentSetting
     * alone covers both paths — see firmware/fan/'s own header comment).
     * Drives the real PWM output, then reports back what the fan is now
     * actually doing via PercentCurrent, through the same registry-lookup-
     * and-cast pattern firmware/fan/'s own Delegate already establishes
     * (esp-matter's own fan_control/integration.cpp only implements
     * SetDefaultDelegate(), not a PercentCurrent::Set() free function). No
     * explicit Matter stack lock here — this runs synchronously on the
     * Matter stack's own thread as part of attribute-write processing, not
     * a separate FreeRTOS task. */
    void OnFanDriveStateChanged(const FanControl::FanDriveState &newState) override
    {
        uint8_t percent = newState.percentSetting.IsNull() ? 0 : newState.percentSetting.Value();
        set_fan_output(percent);
        g_fan_percent_setting = percent; /* read by filter_life_task() below */

        chip::app::ConcreteClusterPath path(room_ac_endpoint_id, FanControl::Id);
        chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
        if (iface) {
            static_cast<FanControlCluster *>(iface)->SetPercentCurrent(percent);
        } else {
            ESP_LOGE(TAG, "FanControl cluster not found on endpoint %u", room_ac_endpoint_id);
        }
        ESP_LOGI(TAG, "Fan mode %u, percent set to %u%%", chip::to_underlying(newState.mode), percent);
    }
};

static FanDelegate fan_delegate;

/* HEPA + Activated Carbon Filter Monitoring — both optionalConform on this
 * device type (added in RoomAirConditioner.xml revision 3), confirmed by
 * reading the CSA's own device type XML directly rather than assumed from
 * firmware/air-purifier/'s or firmware/extractor-hood/'s own precedent. A
 * real room air conditioner commonly has both a washable/replaceable air
 * filter and, on some models, an activated-carbon deodorizing filter —
 * this reuses firmware/extractor-hood/'s own Condition-feature integration
 * (`resource_monitoring::feature::condition::add()` +
 * `ResourceMonitoring::GetClusterInstance()`) verbatim, including its
 * plain time-based (not sensor-based) life estimate: accumulated seconds
 * the fan actually ran, persisted to NVS periodically, against each
 * filter's own configurable rated life in operating hours. Life figures
 * reuse firmware/air-purifier/'s own air-purifier-style defaults (not
 * firmware/extractor-hood/'s much shorter grease-filter figures) since a
 * room AC's filter is a room-air filter, not a kitchen grease filter. */
static uint8_t compute_filter_condition(uint32_t run_seconds, uint32_t life_hours)
{
    uint32_t life_seconds = life_hours * 3600u;
    if (run_seconds >= life_seconds) {
        return 0;
    }
    uint32_t remaining_percent = 100u - ((uint64_t)run_seconds * 100u) / life_seconds;
    return (uint8_t)remaining_percent;
}

static ResourceMonitoring::ChangeIndicationEnum filter_change_indication_for(uint8_t condition_percent)
{
    if (condition_percent <= ROOM_AC_FILTER_CHANGE_CRITICAL_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kCritical;
    }
    if (condition_percent <= ROOM_AC_FILTER_CHANGE_WARNING_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kWarning;
    }
    return ResourceMonitoring::ChangeIndicationEnum::kOk;
}

/* Pushes a freshly computed Condition/ChangeIndication into one filter
 * cluster — via ResourceMonitoring::GetClusterInstance(), esp-matter's own
 * ready-made convenience free function (see firmware/air-purifier/'s own
 * header comment for why this is used instead of this repo's usual
 * registry-lookup pattern). */
static void update_filter_cluster(uint32_t cluster_id, uint32_t life_hours, const char *label)
{
    uint8_t condition = compute_filter_condition(filter_total_run_seconds, life_hours);
    ResourceMonitoring::ChangeIndicationEnum indication = filter_change_indication_for(condition);

    auto *cluster = ResourceMonitoring::GetClusterInstance(room_ac_endpoint_id, cluster_id);
    if (!cluster) {
        ESP_LOGE(TAG, "%s filter cluster not found on endpoint %u", label, room_ac_endpoint_id);
        return;
    }
    cluster->UpdateCondition(condition);
    cluster->UpdateChangeIndication(indication);
    ESP_LOGI(TAG, "%s filter: %u%% remaining (%s)", label, condition,
             indication == ResourceMonitoring::ChangeIndicationEnum::kCritical ? "CRITICAL" :
             indication == ResourceMonitoring::ChangeIndicationEnum::kWarning ? "WARNING" : "OK");
}

/* Polls every ROOM_AC_FILTER_POLL_INTERVAL_MS: accumulates run time while
 * the fan is actually on (g_fan_percent_setting > 0, set by FanDelegate's
 * own OnFanDriveStateChanged() above), periodically persists it to NVS, and
 * refreshes both filter clusters' Condition/ChangeIndication every poll
 * regardless — same shape as firmware/extractor-hood/'s own
 * filter_life_task(). */
static void filter_life_task(void *arg)
{
    uint32_t ms_since_save = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ROOM_AC_FILTER_POLL_INTERVAL_MS));

        if (g_fan_percent_setting > 0) {
            filter_total_run_seconds += ROOM_AC_FILTER_POLL_INTERVAL_MS / 1000;
            ms_since_save += ROOM_AC_FILTER_POLL_INTERVAL_MS;

            if (ms_since_save >= ROOM_AC_FILTER_NVS_SAVE_INTERVAL_MS) {
                nvs_handle_t nvs;
                if (nvs_open(ROOM_AC_FILTER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_set_u32(nvs, ROOM_AC_FILTER_NVS_KEY, filter_total_run_seconds);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                }
                ms_since_save = 0;
            }
        }

        update_filter_cluster(HepaFilterMonitoring::Id, ROOM_AC_HEPA_FILTER_LIFE_HOURS, "HEPA");
        update_filter_cluster(ActivatedCarbonFilterMonitoring::Id, ROOM_AC_CARBON_FILTER_LIFE_HOURS, "Carbon");
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

/* Reacts to a controller writing Thermostat's SystemMode/
 * OccupiedCoolingSetpoint — tracks the new value locally and re-runs the
 * control loop immediately rather than waiting for the next control_task
 * cycle, same pattern firmware/thermostat/'s own app_attribute_update_cb()
 * already establishes (Heat branch simply doesn't exist here — see the
 * header comment above). The mandatory OnOff/DeadFrontOnOff attribute is
 * deliberately NOT handled here at all — see the header comment above for
 * why no GPIO is tied to it. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != room_ac_endpoint_id || cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        room_ac_system_mode = val->val.u8;
        ESP_LOGI(TAG, "SystemMode set to %u", room_ac_system_mode);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        room_ac_cooling_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Cooling setpoint set to %.2f degC", room_ac_cooling_setpoint_centidegrees / 100.0f);
        run_control_loop();
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

    /* 1c. Load the filter life counter accumulated so far — see
     * filter_life_task()'s own header comment above. */
    nvs_handle_t filter_nvs;
    if (nvs_open(ROOM_AC_FILTER_NVS_NAMESPACE, NVS_READWRITE, &filter_nvs) == ESP_OK) {
        nvs_get_u32(filter_nvs, ROOM_AC_FILTER_NVS_KEY, &filter_total_run_seconds);
        nvs_close(filter_nvs);
    }

    /* 2. Configure the compressor relay — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << ROOM_AC_COMPRESSOR_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(ROOM_AC_COMPRESSOR_RELAY_GPIO, 1); /* active-LOW: 1 = off */

    /* 2b. Configure the fan PWM output via LEDC. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = ROOM_AC_FAN_LEDC_MODE;
    ledc_timer.duty_resolution = ROOM_AC_FAN_LEDC_DUTY_RES;
    ledc_timer.timer_num = ROOM_AC_FAN_LEDC_TIMER;
    ledc_timer.freq_hz = ROOM_AC_FAN_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = ROOM_AC_FAN_PWM_GPIO;
    ledc_channel.speed_mode = ROOM_AC_FAN_LEDC_MODE;
    ledc_channel.channel = ROOM_AC_FAN_LEDC_CHANNEL;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = ROOM_AC_FAN_LEDC_TIMER;
    ledc_channel.duty = 0;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
    set_fan_output(0); /* boots off, same convention every other device type here follows */

    /* 2c. Configure the DS18B20 sensor pin. */
    sensor_setup();

#if ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT
    /* 2c-2. Set up the optional humidity chip — non-fatal if it fails,
     * same per-chip graceful-degradation precedent firmware/smoke-co-
     * alarm/'s and firmware/air-quality-sensor/'s own multi-sensor setups
     * already establish. */
    room_ac_humidity_ok = room_ac_humidity_setup();
    if (!room_ac_humidity_ok) {
        ESP_LOGE(TAG, "Humidity sensor init failed — no humidity readings will be reported");
    }
#endif

    /* 2d. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Room Air Conditioner
     * endpoint (Descriptor + Identify + OnOff[DeadFrontOnOff] + Thermostat
     * [Cooling only] via the complete top-level helper, plus FanControl
     * added manually) — see the header comment above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::room_air_conditioner::config_t ac_config;
    ac_config.thermostat.local_temperature = nullable<int16_t>();
    ac_config.thermostat.control_sequence_of_operation =
        chip::to_underlying(Thermostat::ControlSequenceOfOperationEnum::kCoolingOnly);
    ac_config.thermostat.system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
    ac_config.thermostat.features.cooling.occupied_cooling_setpoint = room_ac_cooling_setpoint_centidegrees;

    endpoint_t *endpoint = endpoint::room_air_conditioner::create(node, &ac_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create room air conditioner endpoint");
        return;
    }
    room_ac_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Room air conditioner endpoint id: %u", room_ac_endpoint_id);

    /* 3a. FanControl — optionalConform, so room_air_conditioner::add()
     * doesn't create it automatically. Delegate attached later, after
     * esp_matter::start() — see the header comment above for why. */
    cluster::fan_control::config_t fan_config;
    fan_config.fan_mode_sequence = chip::to_underlying(FanControl::FanModeSequenceEnum::kOffLowMedHigh);
    cluster::fan_control::create(endpoint, &fan_config, CLUSTER_FLAG_SERVER);

    /* 3b. HEPA + Activated Carbon Filter Monitoring — both optionalConform,
     * added onto the already-correct endpoint the same way FanControl just
     * was; see filter_life_task()'s own header comment above for the full
     * detail. */
    cluster::hepa_filter_monitoring::config_t hepa_config;
    cluster_t *hepa_cluster = cluster::hepa_filter_monitoring::create(endpoint, &hepa_config, CLUSTER_FLAG_SERVER);
    if (!hepa_cluster) {
        ESP_LOGE(TAG, "Failed to create HEPA filter monitoring cluster");
        return;
    }
    cluster::resource_monitoring::feature::condition::config_t hepa_condition_config;
    hepa_condition_config.condition = 100; /* fresh filter until NVS says otherwise, corrected on the first poll */
    hepa_condition_config.degradation_direction =
        chip::to_underlying(ResourceMonitoring::DegradationDirectionEnum::kDown);
    cluster::resource_monitoring::feature::condition::add(hepa_cluster, &hepa_condition_config);

    cluster::activated_carbon_filter_monitoring::config_t carbon_config;
    cluster_t *carbon_cluster =
        cluster::activated_carbon_filter_monitoring::create(endpoint, &carbon_config, CLUSTER_FLAG_SERVER);
    if (!carbon_cluster) {
        ESP_LOGE(TAG, "Failed to create activated carbon filter monitoring cluster");
        return;
    }
    cluster::resource_monitoring::feature::condition::config_t carbon_condition_config;
    carbon_condition_config.condition = 100;
    carbon_condition_config.degradation_direction =
        chip::to_underlying(ResourceMonitoring::DegradationDirectionEnum::kDown);
    cluster::resource_monitoring::feature::condition::add(carbon_cluster, &carbon_condition_config);

#if ROOM_AC_HAS_TEMPERATURE_MEASUREMENT
    /* 3c. TemperatureMeasurement — optionalConform, reuses the existing
     * DS18B20 reading, see the header comment above for the full detail. */
    cluster::temperature_measurement::config_t temp_meas_config;
    temp_meas_config.min_measured_value = nullable<int16_t>((int16_t)ROOM_AC_TEMP_MIN_CENTIDEGREES);
    temp_meas_config.max_measured_value = nullable<int16_t>((int16_t)ROOM_AC_TEMP_MAX_CENTIDEGREES);
    cluster::temperature_measurement::create(endpoint, &temp_meas_config, CLUSTER_FLAG_SERVER);
#endif

#if ROOM_AC_HAS_RELATIVE_HUMIDITY_MEASUREMENT
    /* 3d. RelativeHumidityMeasurement — optionalConform, see the header
     * comment above for the full sourcing/reuse detail. */
    cluster::relative_humidity_measurement::config_t hum_meas_config;
    hum_meas_config.min_measured_value = nullable<uint16_t>(0);
    hum_meas_config.max_measured_value = nullable<uint16_t>(10000);
    cluster::relative_humidity_measurement::create(endpoint, &hum_meas_config, CLUSTER_FLAG_SERVER);
#endif

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* Register the real FanControl Delegate — MUST happen after
     * esp_matter::start(), not before. See firmware/fan/'s own header
     * comment for the full, hard-won story of why calling this any earlier
     * is a silent no-op. */
    FanControl::SetDefaultDelegate(room_ac_endpoint_id, &fan_delegate);

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    /* 5. Start the control task — reads the sensor, pushes LocalTemperature,
     * runs the Cool-only hysteresis loop. */
    xTaskCreate(control_task, "room_ac_control_task", 4096, NULL, 5, NULL);

    /* 5b. Start the filter life task — see its own header comment above. */
    xTaskCreate(filter_life_task, "room_ac_filter_life_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter room air conditioner started. Scan the QR code to commission.");
}
