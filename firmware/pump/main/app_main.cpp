/*
 * Minimal Matter Pump — thirtieth device type, and this repo's first over
 * the PumpConfigurationAndControl cluster, and its first plain continuous-
 * control-loop-free device type in three sessions (after Dishwasher/
 * Laundry Washer/Refrigerator's own OperationalState- or hysteresis-loop-
 * driven designs) — a pump reacts to a controller's own On/Off, CurrentLevel,
 * and OperationMode writes directly, with no background task of its own at
 * all.
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
 * --- Endpoint: a complete top-level helper, and the first one this repo
 * has hit whose constructor already sets the Identify type for you -------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * Pump.xml: Identify, On/Off, and PumpConfigurationAndControl are all
 * `<mandatoryConform/>` (a real, non-optional trio, unlike almost every
 * other device type this repo has built, where Identify is the one
 * consistently-optional cluster) — Groups, LevelControl, ScenesManagement,
 * and TemperatureMeasurement/PressureMeasurement/FlowMeasurement (both
 * server AND client side) plus a client-side OccupancySensing are all
 * `<optionalConform/>`. `endpoint::pump::create()` confirmed complete and
 * ready-to-use by reading `esp_matter_endpoint.cpp`'s own `pump::add()`
 * directly: Identify + OnOff (with the On and Toggle commands explicitly
 * added — Off is already part of the base OnOff cluster) +
 * PumpConfigurationAndControl, via `common::create<T>()` (auto-Descriptor).
 * A genuinely new detail for this repo: `pump::config_t`'s own constructor
 * already sets `identify.identify_type =
 * Identify::IdentifyTypeEnum::kActuator` itself — every other device type
 * built so far that needed Identify's type set has done so explicitly at
 * the call site; this is the first top-level helper in this repo whose own
 * constructor does it instead, confirmed by reading the constructor body
 * directly rather than assumed from the pattern of every device type
 * before it. LevelControl (pump speed) is added manually onto this same
 * endpoint afterward, the usual "add extra clusters onto an already-
 * correct endpoint" pattern — confirmed by reading its own legacy
 * `config_t` that, unlike firmware/dimmable-light/'s own LevelControl
 * usage, this cluster's config here has no Lighting-feature field at all
 * (`current_level`/`min_level`/`max_level`/`on_level`/`options` only) —
 * the Lighting feature is specific to light-emitting devices, and a pump
 * genuinely doesn't need it, confirmed by reading the plain legacy
 * `level_control::config_t` directly rather than copying dimmable-light's
 * own lighting-feature setup blindly. Groups/ScenesManagement/
 * OccupancySensing(client) remain skipped — same "smallest reasonable
 * next step" scope cut this repo applies to every other device type's own
 * optional extras. TemperatureMeasurement/PressureMeasurement/
 * FlowMeasurement were originally skipped here too, for the identical
 * reason — but later added on request; see the "Later extended" section
 * further down this comment for the full detail.
 *
 * --- PumpConfigurationAndControl: SPD (ConstantSpeed) only, plain ember
 * attributes with no Delegate at all ------------------------------------
 * Confirmed by reading the legacy `pump_configuration_and_control::
 * config_t` directly: no `delegate` field exists anywhere on this cluster
 * — genuinely different from every Mode/OperationalState-family cluster
 * this repo has built recently, back to the plain
 * `attribute::PRE_UPDATE` + `attribute::update()` pattern OnOff/
 * LevelControl themselves already use. The cluster's own five control-
 * mode features (`ConstantPressure`/`CompensatedPressure`/`ConstantFlow`/
 * `ConstantSpeed`/`ConstantTemperature`) are a "choice, at least 1"
 * conformance set (confirmed directly in the cluster XML) — this file
 * enables only `ConstantSpeed` (SPD), the one feature that maps directly
 * onto a pump's LevelControl-driven speed with no pressure/flow/
 * temperature sensor hardware needed, same "smallest reasonable next
 * step" scoping as every other device type's own first-cut feature
 * choice. `FeatureMap` is confirmed set correctly (no gap): `create()`
 * calls `global::attribute::create_feature_map(cluster,
 * config->feature_flags)` directly, once, with the real value — unlike
 * RefrigeratorAlarm/AirQuality's own documented hardcoded-to-0 gap.
 * `MaxPressure`/`MaxFlow` are left null (not applicable — no pressure/
 * flow feature enabled); `MaxSpeed`/`MinConstSpeed`/`MaxConstSpeed` use a
 * plain 0-100 "percent" scale (the spec places no unit requirement on
 * this field, confirmed by reading the attribute's own `type="uint16"`
 * definition with no accompanying unit metadata) rather than a
 * hardware-specific RPM figure, since no specific real pump motor's rated
 * speed was being modelled. `Capacity` (mandatoryConform but nullable) is
 * left null — no real flow-measurement hardware to report it from,
 * confirmed safe by the attribute's own `nullable="true"` quality.
 * `PumpStatus`/`Speed`/`LifetimeRunningHours`/`Power`/
 * `LifetimeEnergyConsumed` are all `<optionalConform/>` and none are
 * implemented — none are reachable through `pump_configuration_and_
 * control::config_t` at all (would each need a separate
 * `attribute::create_*()` call this file doesn't make), same "smallest
 * reasonable next step" scope cut. This cluster's own rich event set
 * (`SupplyVoltageLow`/`High`, `DryRunning`, `PumpBlocked`,
 * `MotorTemperatureHigh`, `Leakage`, `AirDetection`, etc. — confirmed by
 * reading `esp_matter_event_impl.h`'s own `pump_configuration_and_
 * control::event::` namespace, seventeen real events in total) is not
 * fired anywhere in this file either — every one of them needs real fault-
 * detection hardware (current sensing, pressure transducers, thermal
 * cutouts) this hobby-scale build doesn't have, the same "no sensor, no
 * fabricated fault reporting" honesty precedent firmware/evse/'s own
 * always-`NoError` `FaultState` and firmware/smoke-co-alarm/'s simple
 * heuristic already establish elsewhere in this repo.
 *
 * `OperationMode` (Normal/Minimum/Maximum/Local, all real, spec-defined
 * values) is a plain writable ember attribute a controller sets directly
 * — no command involved — and genuinely drives this file's own output:
 * Normal uses LevelControl's own CurrentLevel as the speed target (the
 * expected common case — a controller's speed slider maps straight onto
 * PWM duty); Minimum/Maximum instead use the feature-configured
 * `MinConstSpeed`/`MaxConstSpeed` bounds, ignoring CurrentLevel entirely,
 * matching the spec's own description of those two modes ("run at the
 * minimum/maximum possible speed... without being stopped" — not
 * "run at whatever level is currently set"). `Local` is accepted (this
 * hobby build has no separate physical control panel to defer to) but
 * behaves identically to Normal, logged as such — an honest, documented
 * scope cut rather than silently ignoring the write. `EffectiveOperation
 * Mode` is written back via `attribute::update()` immediately after
 * `OperationMode` changes, mirroring it 1:1 — correct here since neither
 * `Automatic` nor `LocalOperation` (the two features that would let the
 * device's own internal logic diverge from the commanded mode) is
 * enabled. `EffectiveControlMode` is fixed to `ConstantSpeed` at startup
 * and never changes — the plain `ControlMode` attribute itself is left
 * unimplemented, since with only one control-mode feature enabled there's
 * nothing for a controller to meaningfully choose between (confirmed the
 * attribute is only `<optionalConform/>`, not required regardless of
 * feature selection).
 *
 * --- On/Off + LevelControl: enable relay + PWM speed, driven directly
 * from attribute writes, no background task needed --------------------
 * `PUMP_RELAY_GPIO` (active-LOW, matching this repo's established relay
 * convention) gates the pump motor's own power entirely — separate from
 * the speed signal, matching how a real variable-speed pump/circulator
 * commonly exposes both a plain enable line and a separate 0-10V/PWM
 * speed input. `PUMP_SPEED_PWM_GPIO` is real PWM via ESP-IDF's
 * `driver/ledc.h` (the same LEDC peripheral firmware/dimmable-light/'s
 * own brightness output and firmware/fan/'s own FanControl output already
 * use in this repo), driving whatever speed-controller/VFD hardware a
 * real pump module exposes on its own PWM/analog speed input.
 * `apply_pump_output()` is the single, direct call site both OnOff's and
 * LevelControl's and PumpConfigurationAndControl's own `attribute::
 * PRE_UPDATE` handlers all funnel into — recomputing the real duty cycle
 * from the current OnOff/CurrentLevel/OperationMode state every time any
 * one of them changes, with no periodic task needed at all (unlike every
 * hysteresis-loop device type this repo has built recently) since a
 * pump's speed output is a direct, immediate function of those three
 * attributes, not something that needs to be re-evaluated against a live
 * sensor reading on a timer.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pump/relay/PWM-speed-controller hardware for this
 * device type physically available when written).
 *
 * --- Later extended: TemperatureMeasurement/PressureMeasurement/
 * FlowMeasurement, on request (continuing the `clusterOptions` rollout
 * firmware/cooktop/ started) ------------------------------------------
 * This file's own header comment above already named the gap: three
 * optionalConform measurement clusters, deliberately skipped at first
 * "since this repo already has real, working driver code for all three
 * measurement clusters elsewhere... if any of them are ever wanted here
 * later." Closed now, each independently checkable via
 * `PUMP_HAS_TEMPERATURE_MEASUREMENT`/`PUMP_HAS_PRESSURE_MEASUREMENT`/
 * `PUMP_HAS_FLOW_MEASUREMENT` (all default off, unchanged default build).
 * Unlike firmware/cooktop/'s own two-chip choice, each of these three
 * clusters has exactly ONE realistic backing chip already proven in this
 * repo — no `chipChoiceGroups` needed, just three independent `clusterOptions`
 * entries each naming a single fixed `chip`, the same shape firmware/
 * air-quality-sensor/'s own single-chip clusters (CO/Ozone/etc.) already
 * establish.
 *
 * All three drivers are reused byte-for-byte from their own already-
 * datasheet-verified source files, not reimplemented: `PUMP_TEMP_SENSOR_GPIO`
 * drives the exact same DS18B20 1-Wire bit-bang/CRC-8 sequence firmware/
 * water-heater/'s own (itself firmware/thermostat/'s own) driver already
 * establishes — a waterproof probe reading the pumped fluid's own
 * temperature, a real, common feature on circulation/well pumps.
 * `PUMP_PRESSURE_SDA_GPIO`/`PUMP_PRESSURE_SCL_GPIO` drive the exact same
 * BMP280 I2C register map + Bosch's own 64-bit fixed-point compensation
 * formula firmware/pressure-sensor/'s own driver already establishes — a
 * real, common feature on well/booster pumps (line pressure). Note this is
 * a plain barometric sensor, not a pump-rated pressure transducer — real
 * pump line pressures can exceed a BMP280's own 300-1100 hPa range
 * entirely; this is the same "real, working, already-verified driver
 * reused as-is" choice this repo makes throughout, not a claim that a
 * BMP280 is the ideal sensor for this specific job. `PUMP_FLOW_PULSE_GPIO`
 * drives the exact same YF-S201-class pulse-counting driver firmware/
 * flow-sensor/'s own file already establishes — a real, common feature on
 * any pump moving a metered amount of fluid. All three clusters are added
 * onto the SAME Pump endpoint that already carries On/Off/LevelControl/
 * PumpConfigurationAndControl — the usual "add extra clusters onto an
 * already-correct endpoint" pattern. A single shared
 * `pump_sensors_task` polls whichever of the three are enabled once per
 * `PUMP_SENSOR_POLL_INTERVAL_MS` — these are independent readings with no
 * interaction with the pump's own direct-attribute-write speed control
 * above, so one plain shared polling task (rather than three separate
 * ones) is enough. Build-verified in Docker for the unchanged default
 * (all off) config and all three toggles individually and together; not
 * hardware-tested (no DS18B20/BMP280/YF-S201-class hardware for this
 * device type physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/clusters/pressure-measurement-server/PressureMeasurementCluster.h>
#include <app/clusters/flow-measurement-server/FlowMeasurementCluster.h>

static const char *TAG = "matter_pump";

/* --- GPIO pin map ---------------------------------------------------------
 * Non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay/speed-controller module" — polarity/signal type isn't
 * universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define PUMP_RELAY_GPIO GPIO_NUM_16       /* active-LOW — gates pump motor power */
#define PUMP_SPEED_PWM_GPIO GPIO_NUM_17    /* PWM speed signal to a speed controller/VFD */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* --- Optional TemperatureMeasurement/PressureMeasurement/FlowMeasurement
 * — see the header comment above for the full sourcing/reuse detail. Each
 * is its own independent toggle (all default off — unchanged default
 * build); GPIOs chosen to avoid the three pins above. */
#define PUMP_HAS_TEMPERATURE_MEASUREMENT 0
#define PUMP_TEMP_SENSOR_GPIO GPIO_NUM_18 /* DS18B20 1-Wire data pin */

#define PUMP_HAS_PRESSURE_MEASUREMENT 0
#define PUMP_PRESSURE_SDA_GPIO GPIO_NUM_21 /* BMP280 I2C */
#define PUMP_PRESSURE_SCL_GPIO GPIO_NUM_22
#define PUMP_PRESSURE_I2C_FREQ_HZ 100000

#define PUMP_HAS_FLOW_MEASUREMENT 0
#define PUMP_FLOW_PULSE_GPIO GPIO_NUM_19 /* YF-S201-class pulse output */

/* One shared poll interval for all three — see the header comment above
 * for why one combined task is enough. */
#define PUMP_SENSOR_POLL_INTERVAL_MS 5000

#if PUMP_HAS_PRESSURE_MEASUREMENT
#define PUMP_BMP280_I2C_ADDR 0x76 /* SDO pin low — this file's assumed/documented wiring */
#define PUMP_BMP280_REG_CALIB_START 0x88
#define PUMP_BMP280_REG_CHIP_ID 0xD0
#define PUMP_BMP280_REG_CTRL_MEAS 0xF4
#define PUMP_BMP280_REG_PRESS_MSB 0xF7
#define PUMP_BMP280_CHIP_ID_VALUE 0x58
/* osrs_t=001 (x1), osrs_p=011 (x4), mode=01 (Forced) — Bosch's own
 * "Standard resolution" preset, same as firmware/pressure-sensor/'s. */
#define PUMP_BMP280_CTRL_MEAS_FORCED_STANDARD 0b00101101
#endif

/* Output ranges — see the header comment above (BMP280's own real
 * operating range for pressure; a plausible pumped-fluid range for
 * temperature; the YF-S201-class sensor's own rated 1-30 L/min range for
 * flow, same as firmware/flow-sensor/'s own). */
#define PUMP_PRESSURE_MIN_HPA 300
#define PUMP_PRESSURE_MAX_HPA 1100
#define PUMP_TEMP_MIN_CENTIDEGREES (-5000)  /* -50.00 degC */
#define PUMP_TEMP_MAX_CENTIDEGREES 15000    /* 150.00 degC */
#define PUMP_FLOW_MIN_MEASURED_VALUE 1      /* 1 L/min -> 0.1 m3/h */
#define PUMP_FLOW_MAX_MEASURED_VALUE 18     /* 30 L/min -> 1.8 m3/h */

/* LEDC (PWM) setup — same pattern firmware/dimmable-light/'s own
 * brightness output already establishes: one timer, one channel,
 * LEDC_LOW_SPEED_MODE for portability, 8-bit/0-255 duty resolution. */
#define PUMP_LEDC_TIMER LEDC_TIMER_0
#define PUMP_LEDC_CHANNEL LEDC_CHANNEL_0
#define PUMP_LEDC_FREQUENCY_HZ 25000 /* above the audible range */
#define PUMP_LEDC_DUTY_RESOLUTION LEDC_TIMER_8_BIT

/* MaxSpeed/MinConstSpeed/MaxConstSpeed all use a plain 0-100 "percent"
 * scale — see the header comment above for why. */
#define PUMP_MAX_SPEED_PERCENT 100
#define PUMP_MIN_CONST_SPEED_PERCENT 10
#define PUMP_MAX_CONST_SPEED_PERCENT 100

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t pump_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Cross-cutting state --------------------------------------------------
 * Written by app_attribute_update_cb()'s own PRE_UPDATE handling, read by
 * apply_pump_output() — see the header comment above for the full detail
 * on how these three combine into a real duty cycle. */
static bool g_on_off = false;
static uint8_t g_current_level = 254; /* LevelControl's own default */
static uint8_t g_operation_mode = chip::to_underlying(PumpConfigurationAndControl::OperationModeEnum::kNormal);

#if PUMP_HAS_TEMPERATURE_MEASUREMENT
/* --- DS18B20 1-Wire driver — reused byte-for-byte from firmware/
 * water-heater/'s own driver (itself firmware/thermostat/'s own), see the
 * header comment above for the full timing/CRC/sourcing detail. Only the
 * pin macro name differs. */
static bool pump_temp_ow_reset(void)
{
    gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(PUMP_TEMP_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void pump_temp_ow_write_bit(int bit)
{
    gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int pump_temp_ow_read_bit(void)
{
    gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(PUMP_TEMP_SENSOR_GPIO);
    esp_rom_delay_us(50);
    return bit;
}

static void pump_temp_ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        pump_temp_ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t pump_temp_ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte = (uint8_t)(byte | (pump_temp_ow_read_bit() << i));
    }
    return byte;
}

/* Dallas/Maxim 1-Wire CRC-8 (reflected, polynomial 0x8C, init 0x00). */
static uint8_t pump_temp_onewire_crc8(const uint8_t *data, size_t len)
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

static bool pump_temp_sensor_setup(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << PUMP_TEMP_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(PUMP_TEMP_SENSOR_GPIO, 1);
    return true;
}

static bool pump_temp_sensor_read(float *temperature_c)
{
    portDISABLE_INTERRUPTS();
    bool present = pump_temp_ow_reset();
    if (present) {
        pump_temp_ow_write_byte(0xCC); /* Skip ROM */
        pump_temp_ow_write_byte(0x44); /* Convert T */
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset — check wiring/pull-up");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(750)); /* max conversion time at default 12-bit resolution */

    portDISABLE_INTERRUPTS();
    present = pump_temp_ow_reset();
    uint8_t scratchpad[9] = {0};
    if (present) {
        pump_temp_ow_write_byte(0xCC);
        pump_temp_ow_write_byte(0xBE); /* Read Scratchpad */
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = pump_temp_ow_read_byte();
        }
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset (read phase)");
        return false;
    }

    if (pump_temp_onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGW(TAG, "DS18B20 CRC mismatch — discarding reading");
        return false;
    }

    int16_t raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    *temperature_c = raw * 0.0625f; /* 12-bit default resolution: 1 LSB = 1/16 degC */
    return true;
}

/* Same registry-lookup-and-cast pattern firmware/refrigerator/'s own
 * get_temperature_measurement_cluster() already establishes —
 * TemperatureMeasurement is a "code-driven" cluster class in this SDK
 * version, not the generic ember-style attribute store. */
static void pump_update_temperature(uint16_t endpoint_id, float temperature_c)
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
#endif /* PUMP_HAS_TEMPERATURE_MEASUREMENT */

#if PUMP_HAS_PRESSURE_MEASUREMENT
/* --- BMP280 I2C driver — reused byte-for-byte from firmware/
 * pressure-sensor/'s own driver, see the header comment above for the
 * full register-map/compensation-formula/sourcing detail. */
static i2c_master_dev_handle_t pump_pressure_i2c_dev = NULL;

static uint16_t pump_bmp280_dig_T1;
static int16_t pump_bmp280_dig_T2, pump_bmp280_dig_T3;
static uint16_t pump_bmp280_dig_P1;
static int16_t pump_bmp280_dig_P2, pump_bmp280_dig_P3, pump_bmp280_dig_P4, pump_bmp280_dig_P5,
    pump_bmp280_dig_P6, pump_bmp280_dig_P7, pump_bmp280_dig_P8, pump_bmp280_dig_P9;

static bool pump_pressure_i2c_bus_setup(uint16_t device_address)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_1; /* NUM_0 may be in use by another sensor on this device */
    bus_config.sda_io_num = PUMP_PRESSURE_SDA_GPIO;
    bus_config.scl_io_num = PUMP_PRESSURE_SCL_GPIO;
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
    dev_config.scl_speed_hz = PUMP_PRESSURE_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &pump_pressure_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool pump_bmp280_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(pump_pressure_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool pump_bmp280_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(pump_pressure_i2c_dev, &reg, 1, data, len, 1000) == ESP_OK;
}

static bool pump_bmp280_init(void)
{
    if (!pump_pressure_i2c_bus_setup(PUMP_BMP280_I2C_ADDR)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (!pump_bmp280_read_reg(PUMP_BMP280_REG_CHIP_ID, &chip_id, 1) || chip_id != PUMP_BMP280_CHIP_ID_VALUE) {
        ESP_LOGE(TAG, "BMP280 not found (CHIP_ID=0x%02X, expected 0x%02X) — check wiring/I2C address", chip_id, PUMP_BMP280_CHIP_ID_VALUE);
        return false;
    }

    uint8_t calib[24] = { 0 };
    if (!pump_bmp280_read_reg(PUMP_BMP280_REG_CALIB_START, calib, sizeof(calib))) {
        ESP_LOGE(TAG, "BMP280 calibration data read failed");
        return false;
    }
    pump_bmp280_dig_T1 = (uint16_t)(calib[0] | (calib[1] << 8));
    pump_bmp280_dig_T2 = (int16_t)(calib[2] | (calib[3] << 8));
    pump_bmp280_dig_T3 = (int16_t)(calib[4] | (calib[5] << 8));
    pump_bmp280_dig_P1 = (uint16_t)(calib[6] | (calib[7] << 8));
    pump_bmp280_dig_P2 = (int16_t)(calib[8] | (calib[9] << 8));
    pump_bmp280_dig_P3 = (int16_t)(calib[10] | (calib[11] << 8));
    pump_bmp280_dig_P4 = (int16_t)(calib[12] | (calib[13] << 8));
    pump_bmp280_dig_P5 = (int16_t)(calib[14] | (calib[15] << 8));
    pump_bmp280_dig_P6 = (int16_t)(calib[16] | (calib[17] << 8));
    pump_bmp280_dig_P7 = (int16_t)(calib[18] | (calib[19] << 8));
    pump_bmp280_dig_P8 = (int16_t)(calib[20] | (calib[21] << 8));
    pump_bmp280_dig_P9 = (int16_t)(calib[22] | (calib[23] << 8));

    ESP_LOGI(TAG, "BMP280 initialized");
    return true;
}

/* Bosch's own official 32-bit fixed-point compensation formula (datasheet
 * section 8.2), reproduced verbatim — see the header comment above. */
static int32_t pump_bmp280_compensate_temperature(int32_t adc_T, int32_t *t_fine_out)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)pump_bmp280_dig_T1 << 1))) * ((int32_t)pump_bmp280_dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)pump_bmp280_dig_T1)) * ((adc_T >> 4) - ((int32_t)pump_bmp280_dig_T1))) >> 12) * ((int32_t)pump_bmp280_dig_T3)) >> 14;
    int32_t t_fine = var1 + var2;
    *t_fine_out = t_fine;
    T = (t_fine * 5 + 128) >> 8;
    return T;
}

static uint32_t pump_bmp280_compensate_pressure(int32_t adc_P, int32_t t_fine)
{
    int64_t var1, var2, p;
    var1 = (int64_t)t_fine - 128000;
    var2 = var1 * var1 * (int64_t)pump_bmp280_dig_P6;
    var2 = var2 + ((var1 * (int64_t)pump_bmp280_dig_P5) << 17);
    var2 = var2 + (((int64_t)pump_bmp280_dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)pump_bmp280_dig_P3) >> 8) + ((var1 * (int64_t)pump_bmp280_dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)pump_bmp280_dig_P1) >> 33;
    if (var1 == 0) {
        return 0;
    }
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)pump_bmp280_dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)pump_bmp280_dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)pump_bmp280_dig_P7) << 4);
    return (uint32_t)(p / 256);
}

static bool pump_bmp280_read_hpa(float *pressure_hpa)
{
    if (!pump_bmp280_write_reg(PUMP_BMP280_REG_CTRL_MEAS, PUMP_BMP280_CTRL_MEAS_FORCED_STANDARD)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(40));

    uint8_t data[6] = { 0 };
    if (!pump_bmp280_read_reg(PUMP_BMP280_REG_PRESS_MSB, data, sizeof(data))) {
        return false;
    }
    int32_t adc_P = ((int32_t)data[0] << 12) | ((int32_t)data[1] << 4) | (data[2] >> 4);
    int32_t adc_T = ((int32_t)data[3] << 12) | ((int32_t)data[4] << 4) | (data[5] >> 4);

    int32_t t_fine = 0;
    int32_t temp_centidegrees = pump_bmp280_compensate_temperature(adc_T, &t_fine);
    uint32_t pressure_pa = pump_bmp280_compensate_pressure(adc_P, t_fine);

    *pressure_hpa = pressure_pa / 100.0f;
    ESP_LOGI(TAG, "BMP280: %.2f hPa (die temp %ld.%02ld degC, not reported via Matter)",
             *pressure_hpa, (long)(temp_centidegrees / 100), (long)(temp_centidegrees % 100));
    return true;
}

/* Same registry-lookup-and-cast pattern as pump_update_temperature() above
 * — PressureMeasurement is also a "code-driven" cluster class. */
static void pump_update_pressure(uint16_t endpoint_id, float pressure_hpa)
{
    chip::app::ConcreteClusterPath path(endpoint_id, PressureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "PressureMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }
    int16_t measured_value = (int16_t)(pressure_hpa + (pressure_hpa >= 0 ? 0.5f : -0.5f));
    static_cast<PressureMeasurementCluster *>(iface)->SetMeasuredValue(chip::app::DataModel::Nullable<int16_t>(measured_value));
}
#endif /* PUMP_HAS_PRESSURE_MEASUREMENT */

#if PUMP_HAS_FLOW_MEASUREMENT
/* --- YF-S201-class pulse-counting flow driver — reused byte-for-byte from
 * firmware/flow-sensor/'s own driver, see the header comment above for the
 * full sourcing/conversion detail. */
#define PUMP_FLOW_PULSES_PER_HZ_PER_LPM 7.5f

static volatile uint32_t pump_flow_pulse_count = 0;

static void IRAM_ATTR pump_flow_pulse_isr(void *arg)
{
    pump_flow_pulse_count++;
}

static bool pump_flow_pulse_gpio_setup(void)
{
    gpio_config_t pulse_conf = {};
    pulse_conf.pin_bit_mask = (1ULL << PUMP_FLOW_PULSE_GPIO);
    pulse_conf.mode = GPIO_MODE_INPUT;
    pulse_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    pulse_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&pulse_conf);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
        return false;
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(PUMP_FLOW_PULSE_GPIO, pump_flow_pulse_isr, NULL);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
        return false;
    }
    return true;
}

/* Same registry-lookup-and-cast pattern as the other two above —
 * FlowMeasurement is also a "code-driven" cluster class. */
static void pump_update_flow(uint16_t endpoint_id, uint16_t measured_value)
{
    chip::app::ConcreteClusterPath path(endpoint_id, FlowMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "FlowMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }
    static_cast<FlowMeasurementCluster *>(iface)->SetMeasuredValue(chip::app::DataModel::Nullable<uint16_t>(measured_value));
}
#endif /* PUMP_HAS_FLOW_MEASUREMENT */

#if PUMP_HAS_TEMPERATURE_MEASUREMENT || PUMP_HAS_PRESSURE_MEASUREMENT || PUMP_HAS_FLOW_MEASUREMENT
/* One shared task polling whichever of the three optional sensors is
 * enabled, once per PUMP_SENSOR_POLL_INTERVAL_MS — see the header comment
 * above for why one combined task is enough (these are independent
 * readings with no interaction with the pump's own direct-attribute-write
 * speed control). */
static void pump_sensors_task(void *arg)
{
#if PUMP_HAS_TEMPERATURE_MEASUREMENT
    if (!pump_temp_sensor_setup()) {
        ESP_LOGE(TAG, "Temperature sensor GPIO setup failed — temperature readings will not be reported");
    }
#endif
#if PUMP_HAS_PRESSURE_MEASUREMENT
    bool pressure_ready = pump_bmp280_init();
    if (!pressure_ready) {
        ESP_LOGE(TAG, "BMP280 init failed — pressure readings will not be reported");
    }
#endif
#if PUMP_HAS_FLOW_MEASUREMENT
    if (!pump_flow_pulse_gpio_setup()) {
        ESP_LOGE(TAG, "Flow sensor GPIO setup failed — flow readings will not be reported");
    }
    pump_flow_pulse_count = 0;
#endif

    for (;;) {
#if PUMP_HAS_TEMPERATURE_MEASUREMENT
        float temperature_c = 0.0f;
        if (pump_temp_sensor_read(&temperature_c)) {
            pump_update_temperature(pump_endpoint_id, temperature_c);
            ESP_LOGI(TAG, "Pump fluid temperature: %.2f degC", temperature_c);
        }
#endif
#if PUMP_HAS_PRESSURE_MEASUREMENT
        if (pressure_ready) {
            float pressure_hpa = 0.0f;
            if (pump_bmp280_read_hpa(&pressure_hpa)) {
                pump_update_pressure(pump_endpoint_id, pressure_hpa);
            }
        }
#endif
#if PUMP_HAS_FLOW_MEASUREMENT
        uint32_t count = pump_flow_pulse_count;
        pump_flow_pulse_count = 0;
        float window_s = PUMP_SENSOR_POLL_INTERVAL_MS / 1000.0f;
        float frequency_hz = (float)count / window_s;
        float flow_lpm = frequency_hz / PUMP_FLOW_PULSES_PER_HZ_PER_LPM;
        uint16_t flow_measured_value = (uint16_t)(flow_lpm * 0.6f + 0.5f);
        pump_update_flow(pump_endpoint_id, flow_measured_value);
        ESP_LOGI(TAG, "Pump flow: %.2f L/min (%lu pulses)", flow_lpm, (unsigned long)count);
#endif
        vTaskDelay(pdMS_TO_TICKS(PUMP_SENSOR_POLL_INTERVAL_MS));
    }
}
#endif /* any optional sensor enabled */

/* Recomputes and applies the real PWM duty from the current OnOff/
 * CurrentLevel/OperationMode state — see the header comment above. */
static void apply_pump_output(void)
{
    uint8_t duty_255 = 0;

    if (g_on_off) {
        switch (g_operation_mode) {
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kMinimum:
            duty_255 = (uint8_t)((PUMP_MIN_CONST_SPEED_PERCENT * 255) / 100);
            break;
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kMaximum:
            duty_255 = (uint8_t)((PUMP_MAX_CONST_SPEED_PERCENT * 255) / 100);
            break;
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kLocal:
            ESP_LOGW(TAG, "OperationMode=Local not supported (no local control panel) — behaving as Normal");
            /* fall through to Normal */
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kNormal:
        default:
            duty_255 = g_current_level; /* CurrentLevel is already 0-254, close enough to 0-255 */
            break;
        }
    }

    gpio_set_level(PUMP_RELAY_GPIO, g_on_off ? 0 : 1); /* active-LOW */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PUMP_LEDC_CHANNEL, duty_255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PUMP_LEDC_CHANNEL);
    ESP_LOGI(TAG, "Pump output: %s, duty %u/255", g_on_off ? "ON" : "OFF", duty_255);
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

/* Reacts to a controller writing OnOff/CurrentLevel/OperationMode —
 * updates the tracked state and immediately recomputes the real output.
 * EffectiveOperationMode is written back right after OperationMode
 * changes — see the header comment above for why that's correct here
 * (neither Automatic nor LocalOperation is enabled). */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != pump_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        g_on_off = val->val.b;
        ESP_LOGI(TAG, "OnOff set to %s", g_on_off ? "true" : "false");
        apply_pump_output();
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        g_current_level = val->val.u8;
        ESP_LOGI(TAG, "CurrentLevel set to %u/254", g_current_level);
        apply_pump_output();
    } else if (cluster_id == PumpConfigurationAndControl::Id &&
               attribute_id == PumpConfigurationAndControl::Attributes::OperationMode::Id) {
        g_operation_mode = val->val.u8;
        ESP_LOGI(TAG, "OperationMode set to %u", g_operation_mode);
        apply_pump_output();

        esp_matter_attr_val_t effective_val = esp_matter_enum8(g_operation_mode);
        attribute::update(pump_endpoint_id, PumpConfigurationAndControl::Id,
                          PumpConfigurationAndControl::Attributes::EffectiveOperationMode::Id, &effective_val);
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

    /* 2. Configure the enable relay — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << PUMP_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(PUMP_RELAY_GPIO, 1); /* active-LOW: 1 = off */

    /* 2b. Configure the speed PWM output. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.timer_num = PUMP_LEDC_TIMER;
    ledc_timer.duty_resolution = PUMP_LEDC_DUTY_RESOLUTION;
    ledc_timer.freq_hz = PUMP_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = PUMP_LEDC_CHANNEL;
    ledc_channel.timer_sel = PUMP_LEDC_TIMER;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = PUMP_SPEED_PWM_GPIO;
    ledc_channel.duty = 0;
    ledc_channel_config(&ledc_channel);

    /* 2c. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Pump endpoint
     * (Identify + OnOff + PumpConfigurationAndControl via the complete
     * top-level helper, plus LevelControl added manually) — see the
     * header comment above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::pump::config_t pump_config(
        nullable<int16_t>(),                                  /* MaxPressure — not applicable, SPD only */
        nullable<uint16_t>((uint16_t)PUMP_MAX_SPEED_PERCENT),  /* MaxSpeed */
        nullable<uint16_t>());                                 /* MaxFlow — not applicable, SPD only */
    pump_config.pump_configuration_and_control.effective_control_mode =
        chip::to_underlying(PumpConfigurationAndControl::ControlModeEnum::kConstantSpeed);
    pump_config.pump_configuration_and_control.feature_flags =
        cluster::pump_configuration_and_control::feature::constant_speed::get_id();
    pump_config.pump_configuration_and_control.features.constant_speed.min_const_speed =
        nullable<uint16_t>((uint16_t)PUMP_MIN_CONST_SPEED_PERCENT);
    pump_config.pump_configuration_and_control.features.constant_speed.max_const_speed =
        nullable<uint16_t>((uint16_t)PUMP_MAX_CONST_SPEED_PERCENT);

    endpoint_t *endpoint = endpoint::pump::create(node, &pump_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create pump endpoint");
        return;
    }
    pump_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Pump endpoint id: %u", pump_endpoint_id);

    /* 3a. LevelControl (pump speed) — optionalConform, so pump::add()
     * doesn't create it automatically. No Lighting feature needed, see
     * the header comment above. */
    cluster::level_control::config_t level_control_config;
    level_control_config.current_level = nullable<uint8_t>(g_current_level);
    level_control_config.min_level = 1;
    level_control_config.max_level = 254;
    cluster::level_control::create(endpoint, &level_control_config, CLUSTER_FLAG_SERVER);

#if PUMP_HAS_TEMPERATURE_MEASUREMENT
    /* 3b. TemperatureMeasurement — optionalConform, see the header comment
     * above for the full sourcing/reuse detail. */
    cluster::temperature_measurement::config_t temp_meas_config;
    temp_meas_config.min_measured_value = nullable<int16_t>((int16_t)PUMP_TEMP_MIN_CENTIDEGREES);
    temp_meas_config.max_measured_value = nullable<int16_t>((int16_t)PUMP_TEMP_MAX_CENTIDEGREES);
    cluster::temperature_measurement::create(endpoint, &temp_meas_config, CLUSTER_FLAG_SERVER);
#endif

#if PUMP_HAS_PRESSURE_MEASUREMENT
    /* 3c. PressureMeasurement — optionalConform, see the header comment
     * above for the full sourcing/reuse detail. */
    cluster::pressure_measurement::config_t pressure_meas_config;
    pressure_meas_config.min_measured_value = nullable<int16_t>((int16_t)PUMP_PRESSURE_MIN_HPA);
    pressure_meas_config.max_measured_value = nullable<int16_t>((int16_t)PUMP_PRESSURE_MAX_HPA);
    cluster::pressure_measurement::create(endpoint, &pressure_meas_config, CLUSTER_FLAG_SERVER);
#endif

#if PUMP_HAS_FLOW_MEASUREMENT
    /* 3d. FlowMeasurement — optionalConform, see the header comment above
     * for the full sourcing/reuse detail. */
    cluster::flow_measurement::config_t flow_meas_config;
    flow_meas_config.min_measured_value = nullable<uint16_t>((uint16_t)PUMP_FLOW_MIN_MEASURED_VALUE);
    flow_meas_config.max_measured_value = nullable<uint16_t>((uint16_t)PUMP_FLOW_MAX_MEASURED_VALUE);
    cluster::flow_measurement::create(endpoint, &flow_meas_config, CLUSTER_FLAG_SERVER);
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

    apply_pump_output(); /* boots off, but sets a real, consistent initial duty of 0 */

#if PUMP_HAS_TEMPERATURE_MEASUREMENT || PUMP_HAS_PRESSURE_MEASUREMENT || PUMP_HAS_FLOW_MEASUREMENT
    xTaskCreate(pump_sensors_task, "pump_sensors_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "Matter pump started. Scan the QR code to commission.");
}
