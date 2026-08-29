/*
 * Minimal Matter Heat Pump — thirty-third device type, and this repo's
 * second genuinely composed, multi-endpoint device after firmware/
 * refrigerator/ — but composed very differently, and against real,
 * conflicting guidance from three separate sources that had to be weighed
 * against each other before writing any code (see below).
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
 * --- Three sources, three different answers — what this file actually does
 * and why ------------------------------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * HeatPump.xml: the ROOT endpoint itself is nearly empty — just Identify
 * (optionalConform) and a CLIENT-side Thermostat binding (optionalConform,
 * i.e. this endpoint MAY bind to some other thermostat device — not host one
 * itself). All the real substance is in two `composedDeviceTypes` entries:
 * an Electrical Sensor (0x0510, ElectricalPowerMeasurement +
 * ElectricalEnergyMeasurement both mandatoryConform) and a Thermostat
 * (0x0301, with an EXTRA mandatoryConform User Label cluster layered on top
 * of that device type's own normal Identify+Thermostat requirement).
 *
 * esp-matter's own `endpoint::heat_pump::create()` (confirmed by reading
 * `esp_matter_endpoint.cpp`'s own `heat_pump::add()` directly) does NOT
 * follow that structure literally: it composes PowerSource + ElectricalSensor
 * (EPM+EEM) + DeviceEnergyManagement[PowerAdjustment] all onto the SAME
 * root endpoint — confirmed by reading `electrical_sensor::add()` itself,
 * which calls `add_device_type()` on whatever endpoint it's handed rather
 * than creating a child — a genuinely different composition style from
 * firmware/refrigerator/'s own child-endpoint pattern for Temperature
 * Controlled Cabinet. It implements NO Thermostat composition at all —
 * neither a child endpoint nor anything on the root.
 *
 * Cross-checked against a real, working third source before deciding how to
 * resolve that gap: connectedhomeip's own chef reference device
 * (`examples/chef/devices/rootnode_heatpump_87ivjRAECh.matter`, fetched and
 * read directly). It CONFIRMS esp-matter's own same-endpoint composition
 * choice (endpoint 1 there lists `ma_powersource` + `ma_electricalsensor` +
 * `device_energy_management` + `ma_heatpump` device types together, plus a
 * client-side `binding cluster Thermostat` matching the XML's own root-level
 * client Thermostat) — but for temperature sensing it uses two entirely
 * separate `ma_tempsensor` (Temperature Sensor, 0x0302) child endpoints
 * instead of a composed Thermostat device type at all, an older/alternate
 * interpretation that doesn't match the current ratified XML's own explicit
 * composed-Thermostat-with-UserLabel requirement.
 *
 * Given three real sources that don't fully agree, this file follows the
 * CURRENT ratified 1.6 XML's own stated intent (a composed Thermostat child,
 * since that's the actual spec this repo targets) rather than chef's older
 * reference — implemented as a genuine CHILD endpoint via
 * `esp_matter::set_parent_endpoint(child, parent)`, the same API firmware/
 * refrigerator/'s own Fridge/Freezer Temperature Controlled Cabinet children
 * already establish — while keeping esp-matter's own proven, tested
 * same-endpoint composition for the Electrical Sensor part (confirmed
 * correct by both esp-matter's own implementation AND the independent chef
 * reference agreeing on that specific point, unlike the Thermostat part).
 * A UserLabel cluster is added manually onto the Thermostat child endpoint
 * (the composedDeviceTypes entry's own extra mandatoryConform requirement
 * beyond Thermostat's own base clusters) — confirmed `cluster::user_label::
 * create()` uses a trivial empty `common::config_t`, no special setup needed.
 *
 * --- Root endpoint: esp-matter's own complete top-level helper, used as-is
 * `endpoint::heat_pump::create()` handles PowerSource (wired feature),
 * ElectricalSensor (both EPM+EEM enabled via `config->electrical_sensor.
 * optional_clusters_mask` — mandatory per the XML's own composedDeviceTypes
 * entry; the helper itself sets ElectricalPowerMeasurement's own
 * AlternatingCurrent feature bit internally, confirmed by reading `add()`
 * directly, so this file doesn't need to), and DeviceEnergyManagement[Power
 * Adjustment] — all with zero extra app code needed. Identify is added
 * manually onto the root (the top-level helper doesn't auto-add it,
 * confirmed by reading `add()` directly, even though the XML lists it as
 * optionalConform there) — every device type in this repo ships one.
 *
 * --- Power monitoring ("hobbyist cluster expansion" pilot, device #3 —
 * see CLAUDE.md's own "Open next steps") ---------------------------------
 * Originally deliberately left undriven: both clusters' legacy
 * `cluster::create()` functions tolerate a null `delegate` with no crash
 * risk, so they existed and reported static/zero defaults — this file's
 * own header comment used to say outright that a real product wanting
 * genuine telemetry here "would want firmware/outlet/'s own hand-written
 * ElectricalPowerMeasurement::Instance/Delegate pair (or one of its 6 real
 * power-monitor chip drivers) wired in instead — out of scope for this
 * first cut." Revisited exactly that way: `HEAT_PUMP_POWER_MONITOR`
 * (defaulting to `_NONE`, unchanged default build/behavior) offers the
 * identical 6-chip choice firmware/outlet/'s and firmware/
 * electrical-meter/'s own power-monitoring subsystem already establishes
 * (BL0942, BL0937, HLW8012, CSE7759, CSE7766, ADE7953) — every driver,
 * protocol detail, and sourcing citation reused verbatim from those files
 * (see firmware/outlet/'s own header comment for the complete per-chip
 * protocol/formula/datasheet detail; not repeated here). Unlike
 * electrical-meter's own "always on, no NONE option" scope (metering is
 * that device's whole purpose), this device already has real substance
 * without power monitoring (the Thermostat child endpoint), so — same as
 * firmware/outlet/'s own convention — it stays a genuinely optional
 * add-on, off by default.
 *
 * The ElectricalPowerMeasurement Delegate/Instance pattern is ported
 * verbatim from firmware/electrical-meter/'s own `MeterPowerDelegate`
 * (itself adapted from esp-matter's own official reference,
 * `examples/all_device_types_app/main/electrical_measurement/`) — a
 * manually-constructed `Instance` against a Delegate subclass, NOT the
 * newer `config->delegate` automatic-init-callback path (confirmed, same
 * as that file's own header comment documents, this is the reference-
 * grounded approach for this specific cluster, not a missed shortcut).
 * The one real difference from electrical-meter's own file: that Instance
 * is constructed against THIS device's own existing root endpoint (already
 * created by `endpoint::heat_pump::create()`, with its own
 * ElectricalPowerMeasurement cluster already in place) rather than a
 * cluster this file creates itself — no new cluster-creation code needed
 * at all, only the Delegate/Instance wiring. ElectricalEnergyMeasurement,
 * by contrast, still uses esp-matter's own complete ready-made free-
 * function API (`SetMeasurementAccuracy()`, `NotifyCumulativeEnergyMeasured()`)
 * — no custom Delegate needed, same as outlet's/electrical-meter's own
 * identical integration. GPIO defaults for every chip are deliberately
 * different from firmware/electrical-meter/'s own (which would collide
 * with this file's own existing compressor-relay/reversing-valve-relay/
 * DS18B20 pins at GPIO 16/17/21) — see the per-chip `#define`s below.
 *
 * --- Thermostat child endpoint: Heat+Cool, reusing firmware/thermostat/'s
 * own control loop closely ---------------------------------------------
 * Unlike firmware/room-air-conditioner/'s own deliberately Cool-only scope,
 * a heat pump's entire point is doing both — ControlSequenceOfOperation is
 * CoolingAndHeating, same as firmware/thermostat/'s own default scope, and
 * the hysteresis control loop (heat/cool demand vs. LocalTemperature) is
 * reused near-verbatim from that file, including its own default setpoints
 * (20.00 degC heat / 26.00 degC cool) and 0.3 degC hysteresis band.
 * `HEAT_PUMP_SENSOR_GPIO` reuses the exact DS18B20 1-Wire driver this
 * repo's other appliance/HVAC device types already establish verbatim.
 *
 * --- Output: compressor + reversing valve, no defrost/aux-heat logic -----
 * `HEAT_PUMP_COMPRESSOR_RELAY_GPIO` (active-LOW) runs whenever either heat
 * or cool demand is active — a real heat pump's compressor runs in both
 * modes, only the refrigerant flow direction differs.
 * `HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO` (active-LOW) tracks SystemMode
 * directly (energized only in Cool), independent of the compressor's own
 * on/off cycling — a real reversing valve is pre-positioned for the
 * commanded mode before the compressor ever starts, not toggled per
 * hysteresis cycle. The energized-in-Cool convention matches common "O"
 * terminal wiring, but real heat pump systems are NOT universal here — some
 * use a "B" terminal convention (energized-in-Heat) instead; always check
 * your specific unit's own wiring diagram before connecting real hardware,
 * same disclaimer this repo's other relay-polarity choices already carry.
 * Explicitly, deliberately NOT implemented: any defrost cycle or auxiliary/
 * backup electric-heat-strip logic — real cold-climate heat pumps need
 * outdoor coil temperature sensing and real timing logic to detect and
 * clear ice buildup, genuinely more engineering than a hobby-scale single-
 * sensor build should attempt, the same "smallest reasonable next step"
 * scope cut firmware/robot-vacuum/'s own skipped real navigation and
 * firmware/dishwasher/'s own skipped Fill phase already establish.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/DS18B20 hardware for this device type physically
 * available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <data_model_provider/clusters/electrical_power_measurement/integration.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <app/reporting/reporting.h> /* MatterReportingAttributeChangeCallback() */

static const char *TAG = "matter_heat_pump";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module and your specific heat pump's own reversing-valve
 * wiring convention" — polarity/convention isn't universal, see the header
 * comment above. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define HEAT_PUMP_COMPRESSOR_RELAY_GPIO GPIO_NUM_16        /* active-LOW */
#define HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO GPIO_NUM_17    /* active-LOW, energized = Cool (see header comment) */
#define HEAT_PUMP_SENSOR_GPIO GPIO_NUM_21                    /* DS18B20, return-air/room temperature */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* --- Power monitoring — see the header comment above. Off by default
 * (unlike firmware/electrical-meter/'s own "always on" scope) — same 6-chip
 * choice firmware/outlet/'s own OUTLET_POWER_MONITOR already offers, GPIO
 * defaults changed to avoid this file's own existing pins (2/16/17/21). --- */
#define HEAT_PUMP_POWER_MONITOR_NONE 0
#define HEAT_PUMP_POWER_MONITOR_BL0942 1
#define HEAT_PUMP_POWER_MONITOR_BL0937 2
#define HEAT_PUMP_POWER_MONITOR_HLW8012 3
#define HEAT_PUMP_POWER_MONITOR_CSE7759 4
#define HEAT_PUMP_POWER_MONITOR_CSE7766 5
#define HEAT_PUMP_POWER_MONITOR_ADE7953 6
#define HEAT_PUMP_POWER_MONITOR HEAT_PUMP_POWER_MONITOR_NONE

#if HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_BL0942
#define BL0942_UART_PORT UART_NUM_1
#define BL0942_UART_RX_GPIO GPIO_NUM_18
#define BL0942_UART_TX_GPIO GPIO_NUM_19
#define BL0942_UART_BAUD_RATE 4800
#define BL0942_DEVICE_ADDRESS 0
#define BL0942_READ_COMMAND 0x58
#define BL0942_FULL_PACKET 0xAA
#define BL0942_RESPONSE_LEN 23
#define BL0942_UREF 15883.34116
#define BL0942_IREF 251065.6814
#define BL0942_PREF 623.0270705
#define BL0942_EREF 5347.484240
#define BL0942_POLL_INTERVAL_MS 10000

#elif HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_BL0937 || \
      HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_HLW8012 || \
      HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_CSE7759
#define PULSE_METER_SEL_GPIO GPIO_NUM_25
#define PULSE_METER_CF_GPIO GPIO_NUM_26
#define PULSE_METER_CF1_GPIO GPIO_NUM_27
#define PULSE_METER_VOLTAGE_DIVIDER 1981.0f
#define PULSE_METER_CURRENT_RESISTOR 0.001f
#define PULSE_METER_WINDOW_MS 2000

#if HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_BL0937
#define PULSE_METER_REFERENCE_VOLTAGE 1.218f
#else /* HLW8012 / CSE7759 */
#define PULSE_METER_REFERENCE_VOLTAGE 2.43f
#define HLW8012_CLOCK_FREQUENCY 3579000.0f
#endif

#elif HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_CSE7766
#define CSE7766_UART_PORT UART_NUM_1
#define CSE7766_UART_RX_GPIO GPIO_NUM_18
#define CSE7766_UART_TX_GPIO GPIO_NUM_19
#define CSE7766_UART_BAUD_RATE 4800
#define CSE7766_PACKET_LEN 24

#elif HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_ADE7953
#define ADE7953_I2C_ADDR 0x38
#define ADE7953_SDA_GPIO GPIO_NUM_32
#define ADE7953_SCL_GPIO GPIO_NUM_33
#define ADE7953_I2C_FREQ_HZ 100000
#define ADE7953_POLL_INTERVAL_MS 10000
#define ADE7953_REG_UNLOCK_8 0x00FE
#define ADE7953_REG_UNLOCK_16 0x0120
#define ADE7953_REG_CONFIG_16 0x0102
#define ADE7953_REG_VRMS_32 0x031C
#define ADE7953_REG_IRMS_A_32 0x031A
#define ADE7953_REG_AWATT_A_32 0x0312
#define ADE7953_VOLTAGE_DIVISOR 26000.0f
#define ADE7953_CURRENT_DIVISOR 100000.0f
#define ADE7953_POWER_DIVISOR 154.0f
#endif

/* Heating/cooling setpoint defaults — same values firmware/thermostat/'s
 * own defaults use (20.00 degC heat / 26.00 degC cool), Matter's global
 * `temperature` type (int16, hundredths of a degree C). */
#define HEAT_PUMP_HEATING_SETPOINT_DEFAULT_CENTIDEGREES 2000
#define HEAT_PUMP_COOLING_SETPOINT_DEFAULT_CENTIDEGREES 2600

/* Bang-bang (hysteresis) control band — same 0.3 degC default firmware/
 * thermostat/'s own control loop uses. */
#define HEAT_PUMP_HYSTERESIS_CENTIDEGREES 30

/* How often the control task re-reads the sensor and re-evaluates the
 * compressor/valve outputs. */
#define HEAT_PUMP_CONTROL_INTERVAL_MS 5000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t heat_pump_root_endpoint_id = 0;
static uint16_t heat_pump_thermostat_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/room-air-conditioner/'s (itself firmware/
 * thermostat/'s / firmware/water-heater/'s) DS18B20 driver — see those
 * files' own header comments for the 1-Wire timing/CRC detail and sourcing. */
static bool ow_reset(void)
{
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(HEAT_PUMP_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(HEAT_PUMP_SENSOR_GPIO);
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
    io_conf.pin_bit_mask = (1ULL << HEAT_PUMP_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
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
 * Written by app_attribute_update_cb()'s own PRE_UPDATE handling, read by
 * run_control_loop() — same shape firmware/thermostat/'s own globals
 * already establish, just without the OpenTherm/Binding output variants. */
static uint8_t heat_pump_system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
static int16_t heat_pump_heating_setpoint_centidegrees = HEAT_PUMP_HEATING_SETPOINT_DEFAULT_CENTIDEGREES;
static int16_t heat_pump_cooling_setpoint_centidegrees = HEAT_PUMP_COOLING_SETPOINT_DEFAULT_CENTIDEGREES;
static bool heat_pump_local_temperature_valid = false;
static int16_t heat_pump_local_temperature_centidegrees = 0;
static bool heat_pump_heat_demand = false;
static bool heat_pump_cool_demand = false;

/* LocalTemperature is a plain ember attribute (Thermostat is confirmed NOT
 * a code-driven cluster class in this SDK version — no `thermostat/` folder
 * under `data_model_provider/clusters/`, same check firmware/thermostat/'s
 * own header comment documents in full) — a direct attribute::update() call. */
static void update_local_temperature(nullable<int16_t> value)
{
    esp_matter_attr_val_t val = esp_matter_nullable_int16(value);
    attribute::update(heat_pump_thermostat_endpoint_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
}

/* ==========================================================================
 * Power monitoring — ElectricalPowerMeasurement + ElectricalEnergyMeasurement.
 * Ported verbatim from firmware/electrical-meter/'s own identical Delegate/
 * driver code — see this file's own header comment above and firmware/
 * outlet/'s own header comment for the full per-chip protocol/formula
 * sourcing detail (not repeated here). Only compiled in at all when
 * HEAT_PUMP_POWER_MONITOR selects a real chip.
 * ========================================================================== */
#if HEAT_PUMP_POWER_MONITOR != HEAT_PUMP_POWER_MONITOR_NONE

namespace chip { namespace app { namespace Clusters { namespace ElectricalPowerMeasurement {

class HeatPumpPowerDelegate : public Delegate {
public:
    PowerModeEnum GetPowerMode() override { return PowerModeEnum::kAc; }
    uint8_t GetNumberOfMeasurementTypes() override { return 3; } /* ActivePower, RMSVoltage, RMSCurrent */

    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        static const Structs::MeasurementAccuracyRangeStruct::Type kPowerRange[] = {
            { .rangeMin = -50000000, .rangeMax = 50000000,
              .percentMax = chip::MakeOptional(static_cast<chip::Percent100ths>(500)) },
        };
        static const Structs::MeasurementAccuracyRangeStruct::Type kCurrentRange[] = {
            { .rangeMin = 0, .rangeMax = 100000,
              .percentMax = chip::MakeOptional(static_cast<chip::Percent100ths>(500)) },
        };
        static const Structs::MeasurementAccuracyRangeStruct::Type kVoltageRange[] = {
            { .rangeMin = 0, .rangeMax = 260000,
              .percentMax = chip::MakeOptional(static_cast<chip::Percent100ths>(500)) },
        };
        switch (index) {
        case 0:
            accuracy = { .measurementType = MeasurementTypeEnum::kActivePower, .measured = true,
                         .minMeasuredValue = -50000000, .maxMeasuredValue = 50000000,
                         .accuracyRanges = chip::app::DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kPowerRange) };
            return CHIP_NO_ERROR;
        case 1:
            accuracy = { .measurementType = MeasurementTypeEnum::kRMSCurrent, .measured = true,
                         .minMeasuredValue = 0, .maxMeasuredValue = 100000,
                         .accuracyRanges = chip::app::DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kCurrentRange) };
            return CHIP_NO_ERROR;
        case 2:
            accuracy = { .measurementType = MeasurementTypeEnum::kRMSVoltage, .measured = true,
                         .minMeasuredValue = 0, .maxMeasuredValue = 260000,
                         .accuracyRanges = chip::app::DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kVoltageRange) };
            return CHIP_NO_ERROR;
        default:
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, Structs::MeasurementRangeStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    DataModel::Nullable<int64_t> GetVoltage() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetActiveCurrent() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetReactiveCurrent() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetApparentCurrent() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetActivePower() override { return mActivePower; }
    DataModel::Nullable<int64_t> GetReactivePower() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetApparentPower() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetRMSVoltage() override { return mRMSVoltage; }
    DataModel::Nullable<int64_t> GetRMSCurrent() override { return mRMSCurrent; }
    DataModel::Nullable<int64_t> GetRMSPower() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetFrequency() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetPowerFactor() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetNeutralCurrent() override { return DataModel::Nullable<int64_t>(); }

    void SetActivePower(DataModel::Nullable<int64_t> v)
    {
        if (mActivePower != v) {
            mActivePower = v;
            MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Attributes::ActivePower::Id);
        }
    }
    void SetRMSVoltage(DataModel::Nullable<int64_t> v)
    {
        if (mRMSVoltage != v) {
            mRMSVoltage = v;
            MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Attributes::RMSVoltage::Id);
        }
    }
    void SetRMSCurrent(DataModel::Nullable<int64_t> v)
    {
        if (mRMSCurrent != v) {
            mRMSCurrent = v;
            MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Attributes::RMSCurrent::Id);
        }
    }

private:
    DataModel::Nullable<int64_t> mActivePower;
    DataModel::Nullable<int64_t> mRMSVoltage;
    DataModel::Nullable<int64_t> mRMSCurrent;
};

} } } } /* chip::app::Clusters::ElectricalPowerMeasurement */

static chip::app::Clusters::ElectricalPowerMeasurement::HeatPumpPowerDelegate power_delegate;
static chip::app::Clusters::ElectricalPowerMeasurement::Instance *power_instance = NULL;
static int64_t cumulative_energy_mwh = 0; /* accumulated since boot; not persisted across reboots */

/* Reports one new set of readings — same shape as firmware/electrical-meter/'s
 * own report_power(). */
static void report_power(double watts, double volts, double amps, int64_t delta_energy_mwh)
{
    power_delegate.SetActivePower(chip::app::DataModel::MakeNullable((int64_t)(watts * 1000.0)));
    power_delegate.SetRMSVoltage(chip::app::DataModel::MakeNullable((int64_t)(volts * 1000.0)));
    power_delegate.SetRMSCurrent(chip::app::DataModel::MakeNullable((int64_t)(amps * 1000.0)));

    if (delta_energy_mwh > 0) {
        cumulative_energy_mwh += delta_energy_mwh;
    }

    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::EnergyMeasurementStruct::Type imported = {};
    imported.energy = cumulative_energy_mwh;
    imported.endSystime = chip::MakeOptional(static_cast<uint64_t>(
        chip::System::SystemClock().GetMonotonicTimestamp().count()));
    chip::app::DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> imported_nullable(imported);
    chip::app::DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> exported_nullable; /* no export/reverse metering */
    NotifyCumulativeEnergyMeasured(heat_pump_root_endpoint_id, imported_nullable, exported_nullable);

    ESP_LOGI(TAG, "Power: %.1f W, %.1f V, %.3f A — cumulative %.3f kWh",
             watts, volts, amps, (double)cumulative_energy_mwh / 1000000.0);
}

#if HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_BL0942

static bool bl0942_uart_setup(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = BL0942_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    esp_err_t err = uart_driver_install(BL0942_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(BL0942_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(BL0942_UART_PORT, BL0942_UART_TX_GPIO, BL0942_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* See firmware/outlet/'s own header comment for the exact packet layout
 * (per Shanghai Belling's own BL0942 datasheet) and where the reference
 * constants come from. */
static bool bl0942_read(double *out_watts, double *out_volts, double *out_amps, int64_t *out_delta_energy_mwh)
{
    uint8_t cmd[2] = { (uint8_t)(BL0942_READ_COMMAND | BL0942_DEVICE_ADDRESS), BL0942_FULL_PACKET };
    int written = uart_write_bytes(BL0942_UART_PORT, (const char *)cmd, sizeof(cmd));
    if (written != (int)sizeof(cmd)) {
        ESP_LOGW(TAG, "BL0942 write failed");
        return false;
    }

    uint8_t resp[BL0942_RESPONSE_LEN];
    int read = uart_read_bytes(BL0942_UART_PORT, resp, sizeof(resp), pdMS_TO_TICKS(500));
    if (read != BL0942_RESPONSE_LEN) {
        ESP_LOGW(TAG, "BL0942 read timed out or short (%d/%d bytes)", read, BL0942_RESPONSE_LEN);
        return false;
    }
    if (resp[0] != 0x55) {
        ESP_LOGW(TAG, "BL0942 bad frame header 0x%02x", resp[0]);
        return false;
    }

    uint8_t checksum = (uint8_t)(BL0942_READ_COMMAND | BL0942_DEVICE_ADDRESS);
    for (int i = 0; i < BL0942_RESPONSE_LEN - 1; i++) {
        checksum = (uint8_t)(checksum + resp[i]);
    }
    checksum ^= 0xFF;
    if (checksum != resp[BL0942_RESPONSE_LEN - 1]) {
        ESP_LOGW(TAG, "BL0942 checksum mismatch (got 0x%02x, expected 0x%02x)", resp[BL0942_RESPONSE_LEN - 1], checksum);
        return false;
    }

    uint32_t i_rms_raw = ((uint32_t)resp[3] << 16) | ((uint32_t)resp[2] << 8) | resp[1];
    uint32_t v_rms_raw = ((uint32_t)resp[6] << 16) | ((uint32_t)resp[5] << 8) | resp[4];
    int32_t watt_raw = ((int32_t)resp[12] << 16) | ((int32_t)resp[11] << 8) | resp[10];
    if (watt_raw & 0x800000) {
        watt_raw |= (int32_t)0xFF000000;
    }
    uint32_t cf_cnt_raw = ((uint32_t)resp[15] << 16) | ((uint32_t)resp[14] << 8) | resp[13];

    *out_volts = (double)v_rms_raw / BL0942_UREF;
    *out_amps = (double)i_rms_raw / BL0942_IREF;
    *out_watts = (double)watt_raw / BL0942_PREF;

    static bool have_last_cf_cnt = false;
    static uint32_t last_cf_cnt = 0;
    uint32_t delta_pulses;
    if (!have_last_cf_cnt) {
        delta_pulses = 0;
        have_last_cf_cnt = true;
    } else if (cf_cnt_raw >= last_cf_cnt) {
        delta_pulses = cf_cnt_raw - last_cf_cnt;
    } else {
        delta_pulses = (0x1000000 - last_cf_cnt) + cf_cnt_raw; /* 24-bit wrap */
    }
    last_cf_cnt = cf_cnt_raw;
    *out_delta_energy_mwh = (int64_t)((double)delta_pulses / BL0942_EREF * 1000.0);

    return true;
}

static void power_monitor_task(void *arg)
{
    if (!bl0942_uart_setup()) {
        ESP_LOGE(TAG, "BL0942 UART setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        double watts = 0, volts = 0, amps = 0;
        int64_t delta_energy_mwh = 0;
        if (bl0942_read(&watts, &volts, &amps, &delta_energy_mwh)) {
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        vTaskDelay(pdMS_TO_TICKS(BL0942_POLL_INTERVAL_MS));
    }
}

#elif HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_BL0937 || \
      HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_HLW8012 || \
      HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_CSE7759

static volatile uint32_t pulse_meter_cf_edges = 0;
static volatile uint32_t pulse_meter_cf1_edges = 0;

static void IRAM_ATTR pulse_meter_cf_isr(void *arg)
{
    pulse_meter_cf_edges++;
}

static void IRAM_ATTR pulse_meter_cf1_isr(void *arg)
{
    pulse_meter_cf1_edges++;
}

static bool pulse_meter_gpio_setup(void)
{
    gpio_config_t sel_conf = {};
    sel_conf.pin_bit_mask = (1ULL << PULSE_METER_SEL_GPIO);
    sel_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&sel_conf);
    gpio_set_level(PULSE_METER_SEL_GPIO, 0);

    gpio_config_t pulse_conf = {};
    pulse_conf.pin_bit_mask = (1ULL << PULSE_METER_CF_GPIO) | (1ULL << PULSE_METER_CF1_GPIO);
    pulse_conf.mode = GPIO_MODE_INPUT;
    pulse_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&pulse_conf);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
        return false;
    }
    gpio_isr_handler_add(PULSE_METER_CF_GPIO, pulse_meter_cf_isr, NULL);
    gpio_isr_handler_add(PULSE_METER_CF1_GPIO, pulse_meter_cf1_isr, NULL);
    return true;
}

/* See firmware/outlet/'s own header comment for the full BL0937/HLW8012/
 * CSE7759 multiplier-formula derivation and sourcing. */
static void power_monitor_task(void *arg)
{
    if (!pulse_meter_gpio_setup()) {
        ESP_LOGE(TAG, "Pulse meter GPIO setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

#if HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_BL0937
    const float power_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_REFERENCE_VOLTAGE *
                                    PULSE_METER_VOLTAGE_DIVIDER / PULSE_METER_CURRENT_RESISTOR / 1721506.0f;
    const float current_multiplier = PULSE_METER_REFERENCE_VOLTAGE / PULSE_METER_CURRENT_RESISTOR / 94638.0f;
    const float voltage_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_VOLTAGE_DIVIDER / 15397.0f;
#else /* HLW8012 / CSE7759 */
    const float power_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_REFERENCE_VOLTAGE *
                                    PULSE_METER_VOLTAGE_DIVIDER / PULSE_METER_CURRENT_RESISTOR * 64.0f / 24.0f /
                                    HLW8012_CLOCK_FREQUENCY;
    const float current_multiplier = PULSE_METER_REFERENCE_VOLTAGE / PULSE_METER_CURRENT_RESISTOR * 512.0f / 24.0f /
                                      HLW8012_CLOCK_FREQUENCY;
    const float voltage_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_VOLTAGE_DIVIDER * 256.0f /
                                      HLW8012_CLOCK_FREQUENCY;
#endif
    const double wh_per_pulse = (double)power_multiplier / 3600.0;

    double latest_watts = 0.0;
    double latest_volts = 0.0;
    double latest_amps = 0.0;
    bool have_current = false;
    bool have_voltage = false;

    for (;;) {
        bool measuring_voltage = (gpio_get_level(PULSE_METER_SEL_GPIO) == 1);

        pulse_meter_cf_edges = 0;
        pulse_meter_cf1_edges = 0;
        vTaskDelay(pdMS_TO_TICKS(PULSE_METER_WINDOW_MS));

        uint32_t cf_count = pulse_meter_cf_edges;
        uint32_t cf1_count = pulse_meter_cf1_edges;
        double window_s = PULSE_METER_WINDOW_MS / 1000.0;

        double cf_freq = (double)cf_count / window_s;
        latest_watts = cf_freq * power_multiplier;

        double cf1_freq = (double)cf1_count / window_s;
        if (measuring_voltage) {
            latest_volts = cf1_freq * voltage_multiplier;
            have_voltage = true;
        } else {
            latest_amps = cf1_freq * current_multiplier;
            have_current = true;
        }

        if (have_current && have_voltage) {
            int64_t delta_energy_mwh = (int64_t)(wh_per_pulse * (double)cf_count * 1000.0);
            report_power(latest_watts, latest_volts, latest_amps, delta_energy_mwh);
        }

        gpio_set_level(PULSE_METER_SEL_GPIO, measuring_voltage ? 0 : 1);
    }
}

#elif HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_CSE7766

static bool cse7766_uart_setup(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = CSE7766_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_EVEN;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    esp_err_t err = uart_driver_install(CSE7766_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(CSE7766_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(CSE7766_UART_PORT, CSE7766_UART_TX_GPIO, CSE7766_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* See firmware/outlet/'s own header comment for the full packet layout
 * and Adj-byte semantics (per Chipsea's own CSE7766 User Manual). */
static bool cse7766_read(double *out_watts, double *out_volts, double *out_amps, int64_t *out_delta_energy_mwh)
{
    uint8_t pkt[CSE7766_PACKET_LEN];
    int read = uart_read_bytes(CSE7766_UART_PORT, pkt, sizeof(pkt), pdMS_TO_TICKS(2000));
    if (read != CSE7766_PACKET_LEN) {
        ESP_LOGW(TAG, "CSE7766 read timed out or short (%d/%d bytes)", read, CSE7766_PACKET_LEN);
        return false;
    }
    if (pkt[0] == 0xAA) {
        ESP_LOGW(TAG, "CSE7766 reports itself as not calibrated — discarding reading");
        return false;
    }
    if ((pkt[0] & 0xF0) == 0xF0) {
        ESP_LOGW(TAG, "CSE7766 reports an abnormal condition (header1 0x%02x) — discarding reading", pkt[0]);
        return false;
    }
    if (pkt[1] != 0x5A) {
        ESP_LOGW(TAG, "CSE7766 bad frame header2 0x%02x", pkt[1]);
        return false;
    }

    uint8_t checksum = 0;
    for (int i = 2; i <= 22; i++) {
        checksum = (uint8_t)(checksum + pkt[i]);
    }
    if (checksum != pkt[23]) {
        ESP_LOGW(TAG, "CSE7766 checksum mismatch (got 0x%02x, expected 0x%02x)", pkt[23], checksum);
        return false;
    }

    uint8_t adjustment = pkt[20];
    bool voltage_cycle_complete = (adjustment & 0x40) != 0;
    bool current_cycle_complete = (adjustment & 0x20) != 0;
    bool power_cycle_complete = (adjustment & 0x10) != 0;

    uint32_t voltage_coeff = ((uint32_t)pkt[2] << 16) | ((uint32_t)pkt[3] << 8) | pkt[4];
    uint32_t voltage_cycle = ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
    uint32_t current_coeff = ((uint32_t)pkt[8] << 16) | ((uint32_t)pkt[9] << 8) | pkt[10];
    uint32_t current_cycle = ((uint32_t)pkt[11] << 16) | ((uint32_t)pkt[12] << 8) | pkt[13];
    uint32_t power_coeff = ((uint32_t)pkt[14] << 16) | ((uint32_t)pkt[15] << 8) | pkt[16];
    uint32_t power_cycle = ((uint32_t)pkt[17] << 16) | ((uint32_t)pkt[18] << 8) | pkt[19];
    uint16_t cf_pulses = ((uint16_t)pkt[21] << 8) | pkt[22];

    *out_volts = (voltage_cycle_complete && voltage_cycle) ? (double)voltage_coeff / (double)voltage_cycle : 0.0;
    *out_amps = (current_cycle_complete && current_cycle) ? (double)current_coeff / (double)current_cycle : 0.0;
    *out_watts = (power_cycle_complete && power_cycle) ? (double)power_coeff / (double)power_cycle : 0.0;

    double wh_per_pulse = power_cycle_complete ? ((double)power_coeff / 1000000.0 / 3600.0) : 0.0;
    *out_delta_energy_mwh = (int64_t)(wh_per_pulse * (double)cf_pulses * 1000.0);

    return power_cycle_complete || voltage_cycle_complete || current_cycle_complete;
}

static void power_monitor_task(void *arg)
{
    if (!cse7766_uart_setup()) {
        ESP_LOGE(TAG, "CSE7766 UART setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        double watts = 0, volts = 0, amps = 0;
        int64_t delta_energy_mwh = 0;
        if (cse7766_read(&watts, &volts, &amps, &delta_energy_mwh)) {
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        /* No delay — CSE7766 sends multiple packets a second unprompted;
         * cse7766_read() naturally paces this loop. */
    }
}

#elif HEAT_PUMP_POWER_MONITOR == HEAT_PUMP_POWER_MONITOR_ADE7953

static i2c_master_dev_handle_t ade7953_i2c_dev = NULL;

static bool ade7953_write8(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value };
    return i2c_master_transmit(ade7953_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool ade7953_write16(uint16_t reg, uint16_t value)
{
    uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(ade7953_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool ade7953_read32(uint16_t reg, uint32_t *out_value)
{
    uint8_t reg_bytes[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t data[4];
    if (i2c_master_transmit_receive(ade7953_i2c_dev, reg_bytes, sizeof(reg_bytes), data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    *out_value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    return true;
}

/* See firmware/outlet/'s own header comment for why this driver is
 * flagged as the least-certain of the six. */
static bool ade7953_setup(void)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = ADE7953_SDA_GPIO;
    bus_config.scl_io_num = ADE7953_SCL_GPIO;
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
    dev_config.device_address = ADE7953_I2C_ADDR;
    dev_config.scl_speed_hz = ADE7953_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &ade7953_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!ade7953_write8(ADE7953_REG_UNLOCK_8, 0xAD)) {
        return false;
    }
    if (!ade7953_write16(ADE7953_REG_UNLOCK_16, 0x0030)) {
        return false;
    }
    if (!ade7953_write16(ADE7953_REG_CONFIG_16, 0x0004)) {
        return false;
    }
    return true;
}

static bool ade7953_read(double *out_watts, double *out_volts, double *out_amps)
{
    uint32_t vrms_raw, irms_raw;
    int32_t watt_raw;

    if (!ade7953_read32(ADE7953_REG_VRMS_32, &vrms_raw)) {
        ESP_LOGW(TAG, "ADE7953 VRMS read failed");
        return false;
    }
    if (!ade7953_read32(ADE7953_REG_IRMS_A_32, &irms_raw)) {
        ESP_LOGW(TAG, "ADE7953 IRMS read failed");
        return false;
    }
    if (!ade7953_read32(ADE7953_REG_AWATT_A_32, (uint32_t *)&watt_raw)) {
        ESP_LOGW(TAG, "ADE7953 AWATT read failed");
        return false;
    }

    *out_volts = (double)vrms_raw / ADE7953_VOLTAGE_DIVISOR;
    *out_amps = (double)irms_raw / ADE7953_CURRENT_DIVISOR;
    *out_watts = (double)watt_raw / ADE7953_POWER_DIVISOR;
    return true;
}

static void power_monitor_task(void *arg)
{
    if (!ade7953_setup()) {
        ESP_LOGE(TAG, "ADE7953 setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

    int64_t last_report_ms = 0;
    for (;;) {
        double watts = 0, volts = 0, amps = 0;
        if (ade7953_read(&watts, &volts, &amps)) {
            int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);
            int64_t elapsed_ms = last_report_ms == 0 ? 0 : (now_ms - last_report_ms);
            last_report_ms = now_ms;
            int64_t delta_energy_mwh = (int64_t)(watts * ((double)elapsed_ms / 3600000.0) * 1000.0);
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        vTaskDelay(pdMS_TO_TICKS(ADE7953_POLL_INTERVAL_MS));
    }
}

#endif /* HEAT_PUMP_POWER_MONITOR == ... */
#endif /* HEAT_PUMP_POWER_MONITOR != HEAT_PUMP_POWER_MONITOR_NONE */

static void set_compressor(bool on)
{
    gpio_set_level(HEAT_PUMP_COMPRESSOR_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static void set_reversing_valve(bool cool_position)
{
    gpio_set_level(HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO, cool_position ? 0 : 1); /* active-LOW */
}

/* The actual bang-bang (hysteresis) control decision — Heat+Cool, reused
 * near-verbatim from firmware/thermostat/'s own run_control_loop(). Only
 * acts while heat_pump_local_temperature_valid (an unknown room
 * temperature must never be treated as "cold/warm enough"). Keeps the
 * PREVIOUS demand state inside the deadband — a hysteresis band means
 * "don't switch yet", not "switch off". The reversing valve's own position
 * tracks SystemMode directly and unconditionally (see the header comment
 * above for why it's independent of the compressor's own on/off cycling). */
static void run_control_loop(void)
{
    set_reversing_valve(heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool));

    bool new_heat_demand = heat_pump_heat_demand;
    bool new_cool_demand = heat_pump_cool_demand;

    if (!heat_pump_local_temperature_valid || heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kOff)) {
        new_heat_demand = false;
        new_cool_demand = false;
    } else if (heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat)) {
        new_cool_demand = false;
        if (heat_pump_local_temperature_centidegrees <= heat_pump_heating_setpoint_centidegrees - HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_heat_demand = true;
        } else if (heat_pump_local_temperature_centidegrees >= heat_pump_heating_setpoint_centidegrees + HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_heat_demand = false;
        }
    } else if (heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool)) {
        new_heat_demand = false;
        if (heat_pump_local_temperature_centidegrees >= heat_pump_cooling_setpoint_centidegrees + HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = true;
        } else if (heat_pump_local_temperature_centidegrees <= heat_pump_cooling_setpoint_centidegrees - HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = false;
        }
    } else {
        /* Any other SystemMode value (Auto/EmergencyHeat/Precooling/
         * FanOnly/Dry/Sleep) isn't implemented — same scope cut firmware/
         * thermostat/'s own SystemMode handling already uses. */
        new_heat_demand = false;
        new_cool_demand = false;
    }

    bool changed = (new_heat_demand != heat_pump_heat_demand) || (new_cool_demand != heat_pump_cool_demand);
    heat_pump_heat_demand = new_heat_demand;
    heat_pump_cool_demand = new_cool_demand;

    if (changed) {
        ESP_LOGI(TAG, "Demand changed: heat=%s cool=%s (room %.2f degC, heat setpoint %.2f degC, cool setpoint %.2f degC)",
                 heat_pump_heat_demand ? "ON" : "off", heat_pump_cool_demand ? "ON" : "off",
                 heat_pump_local_temperature_centidegrees / 100.0f,
                 heat_pump_heating_setpoint_centidegrees / 100.0f,
                 heat_pump_cooling_setpoint_centidegrees / 100.0f);
        set_compressor(heat_pump_heat_demand || heat_pump_cool_demand);
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
            heat_pump_local_temperature_valid = true;
            heat_pump_local_temperature_centidegrees = temp_centidegrees;
            update_local_temperature(nullable<int16_t>(temp_centidegrees));
        } else {
            heat_pump_local_temperature_valid = false;
            update_local_temperature(nullable<int16_t>());
        }

        run_control_loop();
        vTaskDelay(pdMS_TO_TICKS(HEAT_PUMP_CONTROL_INTERVAL_MS));
    }
}

/* Toggles the identify LED each time the timer fires — the actual blink.
 * Shared between the root endpoint's own Identify cluster and the
 * Thermostat child's own (auto-added by endpoint::thermostat::create()) —
 * see the header comment above for why both legitimately exist; this
 * callback deliberately doesn't filter by endpoint_id, so either one
 * blinks the same physical LED. */
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

/* Reacts to a controller writing the Thermostat child's SystemMode/
 * OccupiedHeatingSetpoint/OccupiedCoolingSetpoint — tracks the new value
 * locally and re-runs the control loop immediately, same pattern firmware/
 * thermostat/'s own app_attribute_update_cb() already establishes. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != heat_pump_thermostat_endpoint_id || cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        heat_pump_system_mode = val->val.u8;
        ESP_LOGI(TAG, "SystemMode set to %u", heat_pump_system_mode);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        heat_pump_heating_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Heating setpoint set to %.2f degC", heat_pump_heating_setpoint_centidegrees / 100.0f);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        heat_pump_cooling_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Cooling setpoint set to %.2f degC", heat_pump_cooling_setpoint_centidegrees / 100.0f);
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

    /* 2. Configure the two relay outputs — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << HEAT_PUMP_COMPRESSOR_RELAY_GPIO) |
        (1ULL << HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(HEAT_PUMP_COMPRESSOR_RELAY_GPIO, 1); /* active-LOW: 1 = off */
    gpio_set_level(HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO, 1);

    /* 2b. Configure the DS18B20 sensor pin. */
    sensor_setup();

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

    /* 3. Build the Matter data model: one node, one Heat Pump root endpoint
     * (Descriptor + PowerSource + ElectricalSensor[EPM+EEM] +
     * DeviceEnergyManagement via the complete top-level helper, plus
     * Identify added manually), plus a child Thermostat endpoint (Heat+Cool)
     * with an extra UserLabel cluster — see the header comment above for
     * why this composition shape was chosen over the other two candidates. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::heat_pump::config_t heat_pump_config;
    heat_pump_config.electrical_sensor.with_electrical_power_measurement();
    heat_pump_config.electrical_sensor.with_electrical_energy_measurement();

    endpoint_t *root_endpoint = endpoint::heat_pump::create(node, &heat_pump_config, ENDPOINT_FLAG_NONE, NULL);
    if (!root_endpoint) {
        ESP_LOGE(TAG, "Failed to create heat pump root endpoint");
        return;
    }
    heat_pump_root_endpoint_id = endpoint::get_id(root_endpoint);
    ESP_LOGI(TAG, "Heat pump root endpoint id: %u", heat_pump_root_endpoint_id);

    /* 3a. Identify — heat_pump::add() doesn't create it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(root_endpoint, &identify_config, CLUSTER_FLAG_SERVER);

#if HEAT_PUMP_POWER_MONITOR != HEAT_PUMP_POWER_MONITOR_NONE
    /* 3a2. Power monitoring — see the header comment above and firmware/
     * electrical-meter/'s own identical wiring. Unlike that file, no new
     * cluster is created here: the root endpoint's own
     * ElectricalPowerMeasurement/ElectricalEnergyMeasurement clusters
     * already exist (created by endpoint::heat_pump::create() above) —
     * only the Delegate/Instance construction is new. */
    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::MeasurementAccuracyStruct::Type energy_accuracy = {};
    energy_accuracy.measurementType = MeasurementTypeEnum::kElectricalEnergy;
    energy_accuracy.measured = true;
    energy_accuracy.minMeasuredValue = 0;
    energy_accuracy.maxMeasuredValue = 1000000000000000LL;
    SetMeasurementAccuracy(heat_pump_root_endpoint_id, energy_accuracy);

    /* Both ElectricalEnergyMeasurement and ElectricalPowerMeasurement
     * declare their own `Feature` enum — the `using namespace` above is
     * still in scope here, so an unqualified `Feature` would be genuinely
     * ambiguous; qualified explicitly instead, same as firmware/outlet/'s
     * and firmware/electrical-meter/'s own identical code. */
    chip::BitMask<chip::app::Clusters::ElectricalPowerMeasurement::Feature> power_features(
        chip::app::Clusters::ElectricalPowerMeasurement::Feature::kAlternatingCurrent);
    chip::BitMask<chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes> power_optional_attrs(
        chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
        chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent);
    power_instance = new chip::app::Clusters::ElectricalPowerMeasurement::Instance(
        heat_pump_root_endpoint_id, power_delegate, power_features, power_optional_attrs);
    CHIP_ERROR power_err = power_instance->Init();
    if (power_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ElectricalPowerMeasurement Instance::Init failed: %" CHIP_ERROR_FORMAT, power_err.Format());
        return;
    }
#endif

    /* 3b. Thermostat child endpoint — Heat+Cool, same scope firmware/
     * thermostat/'s own default configuration uses. */
    endpoint::thermostat::config_t thermostat_config;
    thermostat_config.thermostat.local_temperature = nullable<int16_t>();
    thermostat_config.thermostat.control_sequence_of_operation =
        chip::to_underlying(Thermostat::ControlSequenceOfOperationEnum::kCoolingAndHeating);
    thermostat_config.thermostat.system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
    thermostat_config.thermostat.feature_flags =
        (uint32_t)Thermostat::Feature::kHeating | (uint32_t)Thermostat::Feature::kCooling;
    thermostat_config.thermostat.features.heating.occupied_heating_setpoint = heat_pump_heating_setpoint_centidegrees;
    thermostat_config.thermostat.features.cooling.occupied_cooling_setpoint = heat_pump_cooling_setpoint_centidegrees;

    endpoint_t *thermostat_endpoint = thermostat::create(node, &thermostat_config, ENDPOINT_FLAG_NONE, NULL);
    if (!thermostat_endpoint) {
        ESP_LOGE(TAG, "Failed to create thermostat child endpoint");
        return;
    }
    heat_pump_thermostat_endpoint_id = endpoint::get_id(thermostat_endpoint);
    ESP_LOGI(TAG, "Heat pump thermostat child endpoint id: %u", heat_pump_thermostat_endpoint_id);

    /* 3c. UserLabel — the composedDeviceTypes entry's own extra
     * mandatoryConform requirement beyond Thermostat's own base clusters. */
    cluster::user_label::config_t user_label_config;
    cluster::user_label::create(thermostat_endpoint, &user_label_config, CLUSTER_FLAG_SERVER);

    /* 3d. Parent the Thermostat endpoint under the Heat Pump root — same
     * API firmware/refrigerator/'s own Fridge/Freezer children already use. */
    err = set_parent_endpoint(thermostat_endpoint, root_endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set thermostat endpoint's parent: %d", err);
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

    /* 5. Start the control task — reads the sensor, pushes LocalTemperature,
     * runs the Heat+Cool hysteresis loop. */
    xTaskCreate(control_task, "heat_pump_control_task", 4096, NULL, 5, NULL);

#if HEAT_PUMP_POWER_MONITOR != HEAT_PUMP_POWER_MONITOR_NONE
    xTaskCreate(power_monitor_task, "power_monitor_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "Matter heat pump started. Scan the QR code to commission.");
}
