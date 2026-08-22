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
 * endpoint afterward the same way, not wired in automatically. An
 * optional composed Electrical Sensor device type (Electrical Power/
 * Energy Measurement) is listed too — not implemented here, same
 * "smallest reasonable next step" scope cut every other device type in
 * this repo applies to its own optional extras; firmware/outlet/ already
 * has this exact two-cluster pattern built if it's ever wanted here.
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
#include <esp_timer.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/water-heater-management-server/water-heater-management-server.h>

static const char *TAG = "matter_water_heater";

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

    ESP_LOGI(TAG, "Matter water heater started. Scan the QR code to commission.");
}
