/*
 * Minimal Matter Water Heater — twenty-fourth device type, and the first
 * to combine three separate clusters this repo has already built distinct
 * integration patterns for (a plain-ember-attribute Thermostat cluster, a
 * ModeBase-derived Mode cluster, and a Delegate-driven command cluster
 * with its own events) onto one endpoint — plus a genuinely new cluster
 * (WaterHeaterManagement) this repo hasn't touched before.
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
 * --- Endpoint: esp-matter's own complete top-level helper, plus Identify
 * added onto the SAME already-correct endpoint --------------------------
 * `endpoint::water_heater::create()` (device type 0x050F) confirmed
 * complete/ready-to-use by reading esp_matter_endpoint.cpp's own
 * `water_heater::add()` directly: Thermostat + WaterHeaterManagement +
 * WaterHeaterMode, via `common::create<T>()` (auto-Descriptor). Matches
 * the CSA's own data_model/1.6/device_types/WaterHeater.xml exactly: all
 * three of those (with Thermostat's own Heat feature mandatory) are
 * `<mandatoryConform/>` — Identify is only `<optionalConform/>`, same
 * spec shape firmware/extractor-hood/ already hit, so it's added onto the
 * endpoint afterward the same way, not wired in automatically.
 *
 * --- Power monitoring: the optional composed Electrical Sensor device
 * type ("hobbyist cluster expansion" pilot, device #4 — see CLAUDE.md's
 * own "Open next steps") ------------------------------------------------
 * Originally not implemented at all — this file's own header comment
 * used to say outright "firmware/outlet/ already has this exact two-
 * cluster pattern built if it's ever wanted here." Revisited exactly that
 * way: `WATER_HEATER_POWER_MONITOR` (defaulting to `_NONE`, unchanged
 * default build) creates a genuinely SECOND Matter endpoint — esp-matter's
 * own `electrical_sensor` device type, via `endpoint::electrical_sensor::
 * create()` — the same two-endpoint composition firmware/outlet/'s own
 * optional power-monitoring feature already establishes, a structurally
 * different situation from firmware/heat-pump/'s own identical-sounding
 * gap (that device's ElectricalSensor clusters already existed,
 * composed onto its root endpoint by `endpoint::heat_pump::create()`
 * itself; here nothing exists until this toggle creates it). Same 6-chip
 * choice (BL0942, BL0937, HLW8012, CSE7759, CSE7766, ADE7953), every
 * driver/protocol/citation reused verbatim from firmware/outlet/'s own
 * header comment (not repeated here). ElectricalPowerMeasurement uses a
 * manually-constructed Delegate/Instance (same pattern as outlet's/
 * electrical-meter's/heat-pump's own identical integration — NOT the
 * newer `config->delegate` automatic-init-callback path, confirmed the
 * reference-grounded approach for this specific cluster);
 * ElectricalEnergyMeasurement uses esp-matter's own complete ready-made
 * free-function API. GPIO defaults for every chip are deliberately
 * different from outlet's/electrical-meter's own (18/19 for UART, 32/33
 * for I2C instead of 16/17/21/22) — those would collide with this file's
 * own existing relay/DS18B20 pins (4/21); the shared pulse-meter pins
 * (BL0937/HLW8012/CSE7759) don't collide, so they're reused unchanged at
 * 25/26/27.
 *
 * --- Thermostat: Heat-only, reused from firmware/thermostat/'s own
 * plain-ember-attribute pattern, scaled down -------------------------
 * `ControlSequenceOfOperation` is `kHeatingOnly` (a water heater has no
 * cooling capability at all — unlike firmware/thermostat/'s own Heat+Cool
 * scope, there is no ambiguity to scope down here, the device physically
 * can't cool). SystemMode/OccupiedHeatingSetpoint/LocalTemperature are
 * plain ember attributes (confirmed the same way firmware/thermostat/
 * already did — no `thermostat/` folder under
 * `data_model_provider/clusters/`), same `attribute::PRE_UPDATE` +
 * `attribute::update()` pattern reused verbatim.
 *
 * --- WaterHeaterMode: a third ModeBase-derived cluster this repo has
 * built, identical integration to firmware/robot-vacuum/'s RvcRunMode/
 * RvcCleanMode ---------------------------------------------------------
 * Confirmed by reading `esp_matter_cluster.cpp`'s own
 * `water_heater_mode::create()`: wired through esp-matter's own
 * `config->delegate` field exactly like RvcRunMode/RvcCleanMode were —
 * `WaterHeaterModeDelegateInitCB` (confirmed present in
 * `esp_matter_delegate_callbacks.cpp`, calling the same `InitModeDelegate()`
 * helper) constructs a real `ModeBase::Instance` automatically during
 * `esp_matter::start()`'s own cluster-init pass — no ordering awareness
 * needed from this file, see firmware/robot-vacuum/'s own header comment
 * for the full detail on why. Real mode/tag values (`WaterHeaterMode::
 * ModeTag::kOff/kManual/kTimed`) confirmed directly against
 * connectedhomeip's own generated
 * `zzz_generated/app-common/clusters/WaterHeaterMode/Enums.h`. No
 * business-rule restriction on transitions (unlike RvcRunMode's own
 * "Mapping only from Idle" rule) — any mode is reachable from any other.
 * "Timed" is accepted as a real selectable mode but behaves identically
 * to "Manual" here — no RTC-driven schedule is implemented, the same
 * "smallest reasonable next step" scope cut firmware/robot-vacuum/'s own
 * "Mapping" mode already applies (a real mode value the cluster reports
 * correctly, honestly not doing anything the spec doesn't require).
 *
 * --- WaterHeaterManagement: a genuinely new cluster for this repo, and a
 * real, previously-undiscovered gap even in connectedhomeip's own
 * reference example ------------------------------------------------------
 * Confirmed by reading `esp_matter_cluster.cpp`'s own
 * `water_heater_management::create()`: wired through `config->delegate`
 * the same automatic way as the ModeBase clusters above
 * (`WaterHeaterManagementDelegateInitCB` constructs a real
 * `WaterHeaterManagement::Instance` — a thin, documented "backwards-
 * compatible wrapper" around the cluster itself, confirmed by reading
 * `CodegenIntegration.h` directly). Confirmed by reading `Delegate.h`
 * directly that EVERY attribute on this cluster (`HeaterTypes`/
 * `HeatDemand`/`TankVolume`/`EstimatedHeatRequired`/`TankPercentage`/
 * `BoostState`) is delegate-driven via a pure-virtual getter, read live on
 * every controller request — despite esp-matter's own `config_t`
 * exposing `heater_types`/`heat_demand`/`boost_state` fields that get
 * seeded into ember attributes at creation time, those become irrelevant
 * once the delegate is attached, same "code-driven cluster shadows the
 * plain ember store" pattern this repo has now hit repeatedly (RvcOperational
 * State, ModeBase, ResourceMonitoring). `GetTankVolume()`/
 * `GetEstimatedHeatRequired()`/`GetTankPercentage()` all return trivial
 * placeholders (0) below — confirmed SAFE to do, not just assumed, by
 * reading `WaterHeaterManagementCluster.cpp`'s own `Attributes()` method
 * directly: it only advertises those three attributes at all when the
 * EnergyManagement (`EM`)/TankPercent (`TP`) feature bits are set, which
 * this file leaves off (feature_map=0, same "smallest reasonable next
 * step" scope cut as every optional feature elsewhere in this repo) — so
 * a real controller never has any attribute path that would reach those
 * getters in the first place.
 *
 * A real, previously-undocumented gap was found while researching this:
 * connectedhomeip's own reference implementation
 * (`examples/water-heater-app/water-heater-common/src/
 * WaterHeaterDelegateImpl.cpp`) contains a literal
 * `// TODO: Implement Thermostat Cluster temperature handling. It's
 * mandatory to be spec conformant.` comment — confirming there is no
 * single prescribed relationship between Thermostat's own SystemMode and
 * WaterHeaterMode's Off/Manual/Timed even from the SDK authors
 * themselves; this is genuinely open design space per device
 * implementation, the same situation firmware/robot-vacuum/'s own
 * RunMode<->OperationalState relationship was in. This file's own
 * deliberate, documented choice: heating is enabled only when BOTH
 * `WaterHeaterMode != Off` AND `Thermostat SystemMode == Heat` — either
 * cluster saying Off turns the heater off, so a controller using either
 * cluster's own UI (a generic Thermostat tile, or a water-heater-specific
 * mode picker) gets a heater that actually listens to it. Both boot to
 * Off, matching every other device type's boot-to-known-safe-state
 * convention in this repo.
 *
 * `Boost`/`CancelBoost` are handled entirely in this file, not by the
 * cluster (confirmed by reading `WaterHeaterManagementCluster.cpp`: it
 * only decodes the command and calls the Delegate's `HandleBoost()`/
 * `HandleCancelBoost()`, no countdown/state-machine logic of its own,
 * unlike e.g. firmware/valve/'s ValveConfigurationAndControl). `HandleBoost()`
 * records the command's parameters and a countdown deadline; the
 * countdown itself, the hysteresis control loop, and the DS18B20 sensor
 * read all run together in one periodic FreeRTOS task
 * (`water_heater_task`) — which calls straight into the same delegate
 * instance's own state/event-generation methods with no explicit Matter
 * stack lock, the same lock-free "call a code-driven cluster's own update
 * methods directly from a plain periodic task" precedent
 * firmware/air-purifier/'s `filter_life_task` already established in this
 * repo (confirmed by re-reading that file — no `lock::ScopedChipStackLock`
 * there either). `OneShot` is honestly implemented (this file DOES have a
 * real local temperature reading to check it against): the task ends the
 * boost itself, generating a real `BoostEnded` event, once the local
 * temperature reaches the boost's target. `EmergencyBoost` is accepted
 * and logged but has no second heat source to actually enable in this
 * single-relay v1 design — a documented, honest scope cut, the same
 * category as firmware/robot-vacuum/'s "Mapping" mode having no real
 * navigation.
 *
 * --- Hardware: single relay + a single waterproof temperature probe ----
 * `WATER_HEATER_RELAY_GPIO` (active-LOW, matching firmware/valve/'s and
 * firmware/door-lock/'s own relay convention) drives a contactor/relay in
 * series with an immersion heating element's own thermostat — the
 * classic DIY smart-electric-water-heater retrofit approach (a relay
 * gating an existing immersion element, not a gas boiler ignition
 * interface, which is a meaningfully different and less hobbyist-safe
 * problem). `GetHeaterTypes()` is correspondingly fixed to
 * `kImmersionElement1` — confirmed against the cluster's own
 * `WaterHeaterHeatSourceBitmap` enum, which also lists ImmersionElement2/
 * HeatPump/Boiler/Other for other real installation types this file
 * doesn't implement. `GetHeatDemand()` mirrors the relay's real,
 * current state.
 *
 * `WATER_HEATER_SENSOR_GPIO` reuses firmware/thermostat/'s own DS18B20
 * driver verbatim (1-Wire, bit-banged, Skip-ROM single-sensor
 * assumption — see that file's own header comment for the timing/CRC
 * detail and sourcing) — deliberately NOT the full 7-chip `SENSOR_TYPE`
 * library firmware/thermostat/ offers for room-air temperature: DS18B20
 * is specifically sold in a waterproof stainless-steel probe variant
 * widely used for exactly this (tank/aquarium/brewing) purpose, and
 * firmware/thermostat/'s other six options (SHT3x/SHT4x/AHT20/DHT11/
 * DHT22/BME280) are all bare room-air sensors with no waterproof form
 * factor at all — offering them here would suggest submerging hardware
 * that was never designed for it. Standard quick-power-cycle factory
 * reset. Build-verified in Docker; not hardware-tested (no relay/DS18B20
 * probe hardware physically available when written).
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
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/water-heater-management-server/water-heater-management-server.h>
#include <data_model_provider/clusters/electrical_power_measurement/integration.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <app/reporting/reporting.h> /* MatterReportingAttributeChangeCallback() */

static const char *TAG = "matter_water_heater";

/* --- Power monitoring — see the header comment above. Off by default. --- */
#define WATER_HEATER_POWER_MONITOR_NONE 0
#define WATER_HEATER_POWER_MONITOR_BL0942 1
#define WATER_HEATER_POWER_MONITOR_BL0937 2
#define WATER_HEATER_POWER_MONITOR_HLW8012 3
#define WATER_HEATER_POWER_MONITOR_CSE7759 4
#define WATER_HEATER_POWER_MONITOR_CSE7766 5
#define WATER_HEATER_POWER_MONITOR_ADE7953 6
#define WATER_HEATER_POWER_MONITOR WATER_HEATER_POWER_MONITOR_NONE

#if WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_BL0942
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

#elif WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_BL0937 || \
      WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_HLW8012 || \
      WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_CSE7759
#define PULSE_METER_SEL_GPIO GPIO_NUM_25
#define PULSE_METER_CF_GPIO GPIO_NUM_26
#define PULSE_METER_CF1_GPIO GPIO_NUM_27
#define PULSE_METER_VOLTAGE_DIVIDER 1981.0f
#define PULSE_METER_CURRENT_RESISTOR 0.001f
#define PULSE_METER_WINDOW_MS 2000

#if WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_BL0937
#define PULSE_METER_REFERENCE_VOLTAGE 1.218f
#else /* HLW8012 / CSE7759 */
#define PULSE_METER_REFERENCE_VOLTAGE 2.43f
#define HLW8012_CLOCK_FREQUENCY 3579000.0f
#endif

#elif WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_CSE7766
#define CSE7766_UART_PORT UART_NUM_1
#define CSE7766_UART_RX_GPIO GPIO_NUM_18
#define CSE7766_UART_TX_GPIO GPIO_NUM_19
#define CSE7766_UART_BAUD_RATE 4800
#define CSE7766_PACKET_LEN 24

#elif WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_ADE7953
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

/* DS18B20 data pin — see the header comment above for why this repo's
 * other six room-air SENSOR_TYPE options aren't offered here. Reference
 * wiring: a waterproof DS18B20 probe, data line pulled up to 3.3V (a
 * 4.7k resistor between DATA and 3.3V, same as firmware/thermostat/'s
 * own DS18B20 wiring), strapped to or inserted into the tank. */
#define WATER_HEATER_SENSOR_GPIO GPIO_NUM_21

/* Relay driving the immersion element's own contactor — active-LOW,
 * matching firmware/valve/'s and firmware/door-lock/'s own relay
 * convention. "Always check your specific module" — polarity isn't
 * universal. */
#define WATER_HEATER_RELAY_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* How often the DS18B20 is read and the hysteresis/boost control logic
 * re-evaluated — same interval firmware/thermostat/'s own sensor_task
 * uses. */
#define WATER_HEATER_MEASURE_INTERVAL_MS 10000

/* Bang-bang (hysteresis) control band, in centidegrees C. Wider than
 * firmware/thermostat/'s own 0.3 degC default — a water tank's thermal
 * mass responds far more slowly than room air, so a tighter band would
 * just cause needless relay chatter with no real benefit. */
#define WATER_HEATER_HYSTERESIS_CENTIDEGREES 50

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error (chip::detail vs. chip::app::Clusters::detail)
 * a blanket `using namespace chip;` caused there. Everything else from
 * `chip::`/`chip::app::` below is spelled out fully qualified instead. */
using namespace chip::literals;

static uint16_t water_heater_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Local temperature (DS18B20) ---------------------------------------
 * Reused verbatim from firmware/thermostat/'s own DS18B20 driver — see
 * that file's header comment for the 1-Wire timing/CRC detail and
 * sourcing. Only the pin macro name differs (WATER_HEATER_SENSOR_GPIO
 * instead of the generic SENSOR_PIN_1, since this file only ever
 * supports one sensor). */
static bool ow_reset(void)
{
    gpio_set_level(WATER_HEATER_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(WATER_HEATER_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(WATER_HEATER_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(WATER_HEATER_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(WATER_HEATER_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(WATER_HEATER_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(WATER_HEATER_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(WATER_HEATER_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(WATER_HEATER_SENSOR_GPIO);
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
    io_conf.pin_bit_mask = (1ULL << WATER_HEATER_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(WATER_HEATER_SENSOR_GPIO, 1);
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

/* --- Cross-cutting state ------------------------------------------------
 * Written by app_attribute_update_cb() (SystemMode/OccupiedHeatingSetpoint)
 * and WaterHeaterModeDelegate::HandleChangeToMode() (g_water_heater_mode);
 * read by water_heater_task()'s own control loop. See the header comment
 * above for the "either cluster saying Off turns off heating" design
 * choice this file makes for the Thermostat/WaterHeaterMode relationship. */
static bool water_heater_local_temperature_valid = false;
static int16_t water_heater_local_temperature_centidegrees = 0;
static int16_t water_heater_setpoint_centidegrees = 5000; /* 50.00 degC, a common default hot-water target */
static uint8_t water_heater_system_mode = 0; /* SystemModeEnum::kOff */

static constexpr uint8_t kWaterHeaterModeOff = 0;
static constexpr uint8_t kWaterHeaterModeManual = 1;
static constexpr uint8_t kWaterHeaterModeTimed = 2;
static uint8_t g_water_heater_mode = kWaterHeaterModeOff;

static bool g_relay_on = false;

static void set_relay(bool on)
{
    g_relay_on = on;
    gpio_set_level(WATER_HEATER_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

/* --- WaterHeaterMode delegate --------------------------------------------
 * Structurally identical to firmware/robot-vacuum/'s RvcRunModeDelegate/
 * RvcCleanModeDelegate — see that file's own header comment for the
 * shared reasoning (mode-option-list shape ported from connectedhomeip's
 * own examples/chef reference, "the SDK updates CurrentMode itself after
 * a successful HandleChangeToMode" behavior). No business-rule
 * restriction on transitions — every mode is reachable from every other. */
class WaterHeaterModeDelegate : public ModeBase::Delegate
{
public:
    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        return chip::CopyCharSpanToMutableCharSpan(kModes[modeIndex].label, label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = kModes[modeIndex].mode;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetModeTagsByIndex(uint8_t modeIndex, chip::app::DataModel::List<detail::Structs::ModeTagStruct::Type> &tags) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        if (tags.size() < kModes[modeIndex].modeTags.size()) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        std::copy(kModes[modeIndex].modeTags.begin(), kModes[modeIndex].modeTags.end(), tags.begin());
        tags.reduce_size(kModes[modeIndex].modeTags.size());
        return CHIP_NO_ERROR;
    }

    void HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        g_water_heater_mode = newMode;
        ESP_LOGI(TAG, "WaterHeaterMode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsOff[1] = {{.value = chip::to_underlying(WaterHeaterMode::ModeTag::kOff)}};
    ModeTagType tagsManual[1] = {{.value = chip::to_underlying(WaterHeaterMode::ModeTag::kManual)}};
    ModeTagType tagsTimed[1] = {{.value = chip::to_underlying(WaterHeaterMode::ModeTag::kTimed)}};

    static constexpr size_t kNumModes = 3;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Off"_span, .mode = kWaterHeaterModeOff, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsOff)},
        {.label = "Manual"_span, .mode = kWaterHeaterModeManual, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsManual)},
        {.label = "Timed"_span, .mode = kWaterHeaterModeTimed, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsTimed)},
    };
};

static WaterHeaterModeDelegate water_heater_mode_delegate;

/* --- WaterHeaterManagement delegate ---------------------------------------
 * See the header comment above for why every attribute here is delegate-
 * driven, why the three Energy-Management/TankPercent getters are safe to
 * stub, and why Boost/CancelBoost's own countdown/hysteresis logic lives
 * in water_heater_task() below rather than in this class. */
class WaterHeaterManagementDelegate : public WaterHeaterManagement::Delegate
{
public:
    chip::Protocols::InteractionModel::Status HandleBoost(uint32_t duration, chip::Optional<bool> oneShot,
                                                           chip::Optional<bool> emergencyBoost,
                                                           chip::Optional<int16_t> temporarySetpoint,
                                                           chip::Optional<chip::Percent> targetPercentage,
                                                           chip::Optional<chip::Percent> targetReheat) override
    {
        (void)targetPercentage;
        (void)targetReheat;

        m_boost_active = true;
        m_boost_one_shot = oneShot.HasValue() && oneShot.Value();
        m_boost_has_temporary_setpoint = temporarySetpoint.HasValue();
        m_boost_temporary_setpoint_centidegrees = temporarySetpoint.HasValue() ? temporarySetpoint.Value() : 0;
        m_boost_end_time_ms = esp_timer_get_time() / 1000 + (int64_t)duration * 1000;

        if (emergencyBoost.HasValue() && emergencyBoost.Value()) {
            ESP_LOGW(TAG, "EmergencyBoost requested — no second heat source to enable in this single-relay design");
        }

        CHIP_ERROR event_err =
            GenerateBoostStartedEvent(duration, oneShot, emergencyBoost, temporarySetpoint, targetPercentage, targetReheat);
        if (event_err != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "Failed to generate BoostStarted event: %" CHIP_ERROR_FORMAT, event_err.Format());
        }
        ESP_LOGI(TAG, "Boost started: %lu s%s%s", (unsigned long)duration,
                 m_boost_one_shot ? ", one-shot" : "",
                 m_boost_has_temporary_setpoint ? ", with temporary setpoint" : "");
        return chip::Protocols::InteractionModel::Status::Success;
    }

    chip::Protocols::InteractionModel::Status HandleCancelBoost() override
    {
        EndBoost();
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /* Called by water_heater_task() when the boost duration expires, or
     * OneShot's own target-reached condition is met — see that function
     * for why this can be called directly with no Matter stack lock. */
    void EndBoost()
    {
        if (!m_boost_active) {
            return;
        }
        m_boost_active = false;
        CHIP_ERROR event_err = GenerateBoostEndedEvent();
        if (event_err != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "Failed to generate BoostEnded event: %" CHIP_ERROR_FORMAT, event_err.Format());
        }
        ESP_LOGI(TAG, "Boost ended");
    }

    bool BoostActive() const { return m_boost_active; }
    bool BoostOneShot() const { return m_boost_one_shot; }
    bool BoostHasTemporarySetpoint() const { return m_boost_has_temporary_setpoint; }
    int16_t BoostTemporarySetpointCentidegrees() const { return m_boost_temporary_setpoint_centidegrees; }
    int64_t BoostEndTimeMs() const { return m_boost_end_time_ms; }

    chip::BitMask<WaterHeaterManagement::WaterHeaterHeatSourceBitmap> GetHeaterTypes() override
    {
        return chip::BitMask<WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(
            WaterHeaterManagement::WaterHeaterHeatSourceBitmap::kImmersionElement1);
    }

    chip::BitMask<WaterHeaterManagement::WaterHeaterHeatSourceBitmap> GetHeatDemand() override
    {
        return g_relay_on ? chip::BitMask<WaterHeaterManagement::WaterHeaterHeatSourceBitmap>(
                                 WaterHeaterManagement::WaterHeaterHeatSourceBitmap::kImmersionElement1)
                          : chip::BitMask<WaterHeaterManagement::WaterHeaterHeatSourceBitmap>();
    }

    /* Energy Management / TankPercent features aren't implemented (see
     * the header comment above) — these three are never actually read by
     * a real controller as a result, confirmed by reading
     * WaterHeaterManagementCluster.cpp's own Attributes() method. */
    uint16_t GetTankVolume() override { return 0; }
    chip::Energy_mWh GetEstimatedHeatRequired() override { return 0; }
    chip::Percent GetTankPercentage() override { return 0; }

    WaterHeaterManagement::BoostStateEnum GetBoostState() override
    {
        return m_boost_active ? WaterHeaterManagement::BoostStateEnum::kActive
                              : WaterHeaterManagement::BoostStateEnum::kInactive;
    }

private:
    bool m_boost_active = false;
    bool m_boost_one_shot = false;
    bool m_boost_has_temporary_setpoint = false;
    int16_t m_boost_temporary_setpoint_centidegrees = 0;
    int64_t m_boost_end_time_ms = 0;
};

static WaterHeaterManagementDelegate water_heater_management_delegate;

/* --- Control loop --------------------------------------------------------
 * Reads the DS18B20 every WATER_HEATER_MEASURE_INTERVAL_MS, applies the
 * hysteresis decision, and drives the relay. Runs as a plain FreeRTOS
 * task (not the Matter stack's own thread) — calling straight into
 * water_heater_management_delegate's own state/event-generation methods
 * with no explicit lock, the same precedent firmware/air-purifier/'s
 * filter_life_task already established (see the header comment above). */
static void water_heater_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WATER_HEATER_MEASURE_INTERVAL_MS));

        float temperature_c;
        if (sensor_read(&temperature_c)) {
            water_heater_local_temperature_valid = true;
            water_heater_local_temperature_centidegrees = (int16_t)(temperature_c * 100.0f);

            esp_matter_attr_val_t val = esp_matter_nullable_int16(water_heater_local_temperature_centidegrees);
            attribute::update(water_heater_endpoint_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
            ESP_LOGI(TAG, "Local temperature: %.2f degC", temperature_c);
        } else {
            water_heater_local_temperature_valid = false;
        }

        bool boost_active = water_heater_management_delegate.BoostActive();
        if (boost_active) {
            /* Duration expiry. */
            if (esp_timer_get_time() / 1000 >= water_heater_management_delegate.BoostEndTimeMs()) {
                water_heater_management_delegate.EndBoost();
                boost_active = false;
            }
        }

        int16_t target_centidegrees = water_heater_setpoint_centidegrees;
        if (boost_active && water_heater_management_delegate.BoostHasTemporarySetpoint()) {
            target_centidegrees = water_heater_management_delegate.BoostTemporarySetpointCentidegrees();
        }

        bool heating_enabled = boost_active ||
            (g_water_heater_mode != kWaterHeaterModeOff &&
             water_heater_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat));

        if (!water_heater_local_temperature_valid || !heating_enabled) {
            if (g_relay_on) {
                set_relay(false);
            }
        } else if (water_heater_local_temperature_centidegrees <= target_centidegrees - WATER_HEATER_HYSTERESIS_CENTIDEGREES) {
            if (!g_relay_on) {
                set_relay(true);
            }
        } else if (water_heater_local_temperature_centidegrees >= target_centidegrees + WATER_HEATER_HYSTERESIS_CENTIDEGREES) {
            if (g_relay_on) {
                set_relay(false);
            }
        }

        /* OneShot: end the boost once the target is actually reached,
         * same behavior the cluster's own spec describes ("automatically
         * canceled once the hot water has first reached the set point"). */
        if (boost_active && water_heater_management_delegate.BoostOneShot() && water_heater_local_temperature_valid &&
            water_heater_local_temperature_centidegrees >= target_centidegrees) {
            water_heater_management_delegate.EndBoost();
        }
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

/* Reacts to a controller writing SystemMode/OccupiedHeatingSetpoint —
 * tracks the new value locally, same firmware/thermostat/ pattern.
 * WaterHeaterMode changes are handled by WaterHeaterModeDelegate above,
 * not here. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != water_heater_endpoint_id || cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        water_heater_system_mode = val->val.u8;
        ESP_LOGI(TAG, "SystemMode set to %u", water_heater_system_mode);
    } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        water_heater_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Heating setpoint set to %.2f degC", water_heater_setpoint_centidegrees / 100.0f);
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

#if WATER_HEATER_POWER_MONITOR != WATER_HEATER_POWER_MONITOR_NONE
/* ==========================================================================
 * Power monitoring — ElectricalPowerMeasurement + ElectricalEnergyMeasurement
 * on a genuinely SECOND Matter endpoint (esp-matter's own `electrical_sensor`
 * device type). Ported verbatim from firmware/outlet/'s and firmware/
 * electrical-meter/'s own identical Delegate/driver code — see this file's
 * own header comment above and firmware/outlet/'s own header comment for
 * the full per-chip protocol/formula sourcing detail (not repeated here).
 * ========================================================================== */

namespace chip { namespace app { namespace Clusters { namespace ElectricalPowerMeasurement {

class WaterHeaterPowerDelegate : public Delegate {
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

static chip::app::Clusters::ElectricalPowerMeasurement::WaterHeaterPowerDelegate power_delegate;
static chip::app::Clusters::ElectricalPowerMeasurement::Instance *power_instance = NULL;
static uint16_t power_endpoint_id = 0;
static int64_t cumulative_energy_mwh = 0; /* accumulated since boot; not persisted across reboots */

/* Reports one new set of readings — same shape as firmware/outlet/'s and
 * firmware/electrical-meter/'s own report_power(). */
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
    NotifyCumulativeEnergyMeasured(power_endpoint_id, imported_nullable, exported_nullable);

    ESP_LOGI(TAG, "Power: %.1f W, %.1f V, %.3f A — cumulative %.3f kWh",
             watts, volts, amps, (double)cumulative_energy_mwh / 1000000.0);
}

#if WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_BL0942

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

#elif WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_BL0937 || \
      WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_HLW8012 || \
      WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_CSE7759

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

#if WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_BL0937
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

#elif WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_CSE7766

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

#elif WATER_HEATER_POWER_MONITOR == WATER_HEATER_POWER_MONITOR_ADE7953

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

#endif /* WATER_HEATER_POWER_MONITOR == ... */

/* Creates the second endpoint (esp-matter's `electrical_sensor` device
 * type) and wires up both measurement clusters — same shape firmware/
 * outlet/'s own power_monitoring_setup() already establishes. */
static bool power_monitoring_setup(node_t *node)
{
    endpoint::electrical_sensor::config_t sensor_config;
    sensor_config.with_electrical_energy_measurement();
    endpoint_t *sensor_endpoint = endpoint::electrical_sensor::create(node, &sensor_config, ENDPOINT_FLAG_NONE, NULL);
    if (!sensor_endpoint) {
        ESP_LOGE(TAG, "Failed to create electrical sensor endpoint");
        return false;
    }
    power_endpoint_id = endpoint::get_id(sensor_endpoint);
    ESP_LOGI(TAG, "Electrical sensor endpoint id: %u", power_endpoint_id);

    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::MeasurementAccuracyStruct::Type energy_accuracy = {};
    energy_accuracy.measurementType = MeasurementTypeEnum::kElectricalEnergy;
    energy_accuracy.measured = true;
    energy_accuracy.minMeasuredValue = 0;
    energy_accuracy.maxMeasuredValue = 1000000000000000LL;
    SetMeasurementAccuracy(power_endpoint_id, energy_accuracy);

    chip::BitMask<chip::app::Clusters::ElectricalPowerMeasurement::Feature> power_features(
        chip::app::Clusters::ElectricalPowerMeasurement::Feature::kAlternatingCurrent);
    chip::BitMask<chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes> power_optional_attrs(
        chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
        chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent);
    power_instance = new chip::app::Clusters::ElectricalPowerMeasurement::Instance(
        power_endpoint_id, power_delegate, power_features, power_optional_attrs);
    CHIP_ERROR power_err = power_instance->Init();
    if (power_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ElectricalPowerMeasurement Instance::Init failed: %" CHIP_ERROR_FORMAT, power_err.Format());
        return false;
    }
    return true;
}
#endif /* WATER_HEATER_POWER_MONITOR != WATER_HEATER_POWER_MONITOR_NONE */

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

    /* 2. Configure the relay output — boots off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << WATER_HEATER_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    set_relay(false);

    /* 2b. Configure the DS18B20 sensor pin. */
    if (!sensor_setup()) {
        ESP_LOGE(TAG, "Sensor setup failed — check WATER_HEATER_SENSOR_GPIO and wiring");
    }

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

    /* 3. Build the Matter data model: one node, one Water Heater endpoint
     * (Thermostat[Heat] + WaterHeaterManagement + WaterHeaterMode via the
     * complete top-level helper), plus Identify added onto that same
     * endpoint afterward — see the header comment above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::water_heater::config_t water_heater_config;
    water_heater_config.thermostat.local_temperature = nullable<int16_t>();
    water_heater_config.thermostat.control_sequence_of_operation =
        chip::to_underlying(Thermostat::ControlSequenceOfOperationEnum::kHeatingOnly);
    water_heater_config.thermostat.system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
    water_heater_config.thermostat.feature_flags = (uint32_t)Thermostat::Feature::kHeating;
    water_heater_config.thermostat.features.heating.occupied_heating_setpoint = water_heater_setpoint_centidegrees;
    water_heater_config.water_heater_mode.delegate = &water_heater_mode_delegate;
    water_heater_config.water_heater_management.delegate = &water_heater_management_delegate;

    endpoint_t *endpoint = endpoint::water_heater::create(node, &water_heater_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create water heater endpoint");
        return;
    }

    water_heater_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Water heater endpoint id: %u", water_heater_endpoint_id);

    /* 3b. Identify — optionalConform for this device type, so
     * water_heater::add() doesn't create it automatically; added here the
     * same way firmware/extractor-hood/'s own Identify cluster is. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(chip::app::Clusters::Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

#if WATER_HEATER_POWER_MONITOR != WATER_HEATER_POWER_MONITOR_NONE
    /* 3c. Power monitoring — a genuinely second Matter endpoint (see the
     * header comment above). */
    if (!power_monitoring_setup(node)) {
        return;
    }
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

    /* 5. Start the control task — reads the DS18B20, runs the hysteresis/
     * boost logic, and drives the relay for as long as the device runs. */
    xTaskCreate(water_heater_task, "water_heater_task", 4096, NULL, 5, NULL);

#if WATER_HEATER_POWER_MONITOR != WATER_HEATER_POWER_MONITOR_NONE
    xTaskCreate(power_monitor_task, "power_monitor_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "Matter water heater started. Scan the QR code to commission.");
}
