/*
 * Minimal Matter Laundry Washer — twenty-ninth device type, and the
 * closest sibling to firmware/dishwasher/ in this repo: the same generic
 * OperationalState cluster (0x0060), the same TemperatureControl (TN-only)
 * pattern, and a fourth ModeBase-derived Mode cluster — plus one genuinely
 * new cluster, LaundryWasherControls, whose SpinSpeed/NumberOfRinses
 * settings this file actually gives real physical meaning to.
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
 * --- Endpoint: same shape as firmware/dishwasher/, and this time
 * esp-matter's own naming actually matches the CSA's ------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * LaundryWasher.xml: only OperationalState is `<mandatoryConform/>` (with
 * its own OperationCompletion event also mandatory, per this device
 * type's own revision 2) — Identify, On/Off (DeadFrontOnOff feature only,
 * skipped for the identical reason firmware/dishwasher/'s own header
 * comment documents), LaundryWasherMode, LaundryWasherControls, and
 * TemperatureControl are all `<optionalConform/>`. Confirmed by reading
 * `esp_matter_endpoint.cpp`'s own `laundry_washer::add()` directly: it's
 * structurally identical to `dish_washer::add()` (both share `config_t =
 * app_with_operational_state_config` and only create OperationalState) —
 * Identify + TemperatureControl + LaundryWasherMode +
 * LaundryWasherControls are all added manually onto this same endpoint
 * afterward, same pattern. Unlike firmware/dishwasher/'s own `dish_washer`/
 * `dish_washer_mode`/`dish_washer_alarm` naming gotcha, esp-matter's own
 * wrapper names here (`laundry_washer`, `laundry_washer_mode`,
 * `laundry_washer_controls`) DO match the CSA's own naming (just standard
 * snake_case), confirmed by reading the legacy header directly before
 * assuming either way.
 *
 * --- OperationalState: identical pattern to firmware/dishwasher/ --------
 * Same automatic `config->delegate` wiring, same
 * `get_delegate_managed_instance()` lookup for the two places this file
 * touches the cluster from outside the delegate's own callbacks (the door
 * sensor's async safety-pause, and the wash cycle finishing on its own) —
 * see that file's own header comment for the full detail on why (the
 * sixth "reach a live cluster instance from app code" pattern in this
 * repo). `HandleStartStateCallback()`/`HandleResumeStateCallback()`
 * reject with `ErrorStateEnum::kUnableToStartOrResume` if the door is
 * open; opening the door mid-cycle PAUSES rather than errors the
 * OperationalState (real front-loader behavior) — but unlike Dishwasher,
 * LaundryWasher's own device type XML lists no Alarm cluster at all, so
 * there's no DoorError-style bit to raise here; the Pause itself is the
 * only signal a controller gets.
 *
 * --- TemperatureControl: identical TN-only pattern to
 * firmware/dishwasher/ and firmware/refrigerator/ --------------------
 * Represents the selected wash-temperature target (min 20.00 degC/cold
 * wash, max 60.00 degC, default 40.00 degC — ordinary real washing-
 * machine temperature range). Same legacy-vs-generated `feature_flags`/
 * `temp_setpoint` handling those two files already document in full —
 * `feature_flags` is set explicitly here for the identical reason.
 *
 * --- LaundryWasherMode: a fourth ModeBase-derived cluster, same
 * business rule as firmware/dishwasher/'s DishwasherMode -----------------
 * Same automatic `config->delegate` wiring as every ModeBase cluster in
 * this repo. Four real mode tags confirmed directly against
 * connectedhomeip's own generated `LaundryWasherMode/Enums.h` — "Normal"
 * (`kNormal`), "Delicate" (`kDelicate`), "Heavy" (`kHeavy`), "Whites"
 * (`kWhites`) — a genuinely different, larger tag set than Dishwasher's
 * own three. Same `kInvalidInMode`-while-Running rejection rule as
 * firmware/dishwasher/'s own DishwasherMode.
 *
 * --- LaundryWasherControls: a genuinely new cluster for this repo, and
 * the one setting here that actually changes physical behavior ----------
 * Confirmed by reading `esp_matter_cluster.cpp`'s own
 * `laundry_washer_controls::create()` directly: unlike RefrigeratorAlarm/
 * AirQuality's own documented FeatureMap-hardcoded-to-0 gap, this
 * cluster's FeatureMap is set TWICE — once hardcoded to 0, then
 * immediately overwritten with `config->feature_flags` — so there's no
 * real gap here, `feature_flags` just needs to be set explicitly (both
 * Spin and Rinse features are enabled here; the cluster's own
 * `VALIDATE_FEATURES_AT_LEAST_ONE("Spin,Rinse", ...)` check confirms at
 * least one is required). `SpinSpeedCurrent`/`NumberOfRinses` are plain
 * ember attributes (confirmed: the cluster registers a
 * `MatterLaundryWasherControlsClusterServerPreAttributeChangedCallback`,
 * the same PRE_ATTRIBUTE_CHANGED-hook shape OnOff/LevelControl already
 * use elsewhere in this repo, not a code-driven `DefaultServerCluster`) —
 * a controller can write either directly, no command needed. This
 * cluster ALSO has a real Delegate (`GetSpinSpeedAtIndex()`/
 * `GetSupportedRinseAtIndex()`, confirmed by reading
 * `laundry-washer-controls-delegate.h` directly) supplying the
 * *supported-value lists* (`SpinSpeeds`/`SupportedRinses`) — the same
 * "feature-flag-gated attributes plus a separate Delegate for the
 * supported-list" shape TemperatureControl's own TL feature uses,
 * confirmed as a real, distinct pattern rather than assumed identical to
 * any single-mechanism cluster elsewhere in this repo. `SpinSpeeds`
 * offers four real options ("Off"/"Low"/"Medium"/"High", the common
 * spin-speed tiers real washing machines expose) — purely informational
 * here (no real variable-speed motor control, same "smallest reasonable
 * next step" scope cut as firmware/robot-vacuum/'s fixed-speed drive
 * motors). `SupportedRinses` offers all four real
 * `NumberOfRinsesEnum` values (`kNone`/`kNormal`/`kExtra`/`kMax`,
 * confirmed against connectedhomeip's own generated Enums.h) — and
 * UNLIKE SpinSpeedCurrent, `NumberOfRinses` genuinely drives this file's
 * own wash-cycle simulation: tracked via the same `attribute::PRE_UPDATE`
 * pattern firmware/water-heater/'s own SystemMode tracking already
 * established, it sets how many real Rinse-then-Drain phases the cycle
 * actually runs (0/1/2/3 for None/Normal/Extra/Max respectively).
 *
 * --- The wash cycle itself: Washing, N x (Rinsing + Draining), Spinning -
 * `laundry_washer_task()` drives three real relay outputs
 * (`LAUNDRY_WASHER_HEATER_RELAY_GPIO`/`_MOTOR_RELAY_GPIO`/
 * `_DRAIN_PUMP_RELAY_GPIO`, all active-LOW, matching firmware/
 * dishwasher/'s own relay convention) through a real, multi-phase timed
 * sequence: Washing (heater, hysteresis-controlled against a DS18B20
 * reading and TemperatureControl's own live setpoint, same 0.5 degC
 * hysteresis convention used throughout this repo's control loops, plus
 * the motor running) for `LAUNDRY_WASHER_WASH_DURATION_MS`; then, for
 * however many rinses `NumberOfRinses` currently calls for, Rinsing
 * (motor only) for `LAUNDRY_WASHER_RINSE_DURATION_MS` followed by
 * Draining (drain pump only) for `LAUNDRY_WASHER_DRAIN_DURATION_MS`;
 * then Spinning (motor only, no heater) for
 * `LAUNDRY_WASHER_SPIN_DURATION_MS` — before calling
 * `OnOperationCompletionDetected()` and returning to Stopped. A single
 * motor relay drives both agitation (Washing/Rinsing) and spin
 * (Spinning) — a deliberate, documented simplification: real washing
 * machines use a variable-speed/reversible motor and a clutch/gearbox to
 * switch between the two, which this hobby-scale single-relay build
 * doesn't model, the same "smallest reasonable next step" reasoning
 * firmware/refrigerator/'s own single-relay-per-compartment design
 * already applies. No Filling phase is modelled at all (a real water-
 * inlet valve + level sensor would be needed), same honest scope cut as
 * firmware/dishwasher/'s own skipped Fill phase.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/DS18B20/reed-switch hardware for this device
 * type physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/laundry-washer-controls-server/laundry-washer-controls-delegate.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>
#include <app/clusters/temperature-control-server/TemperatureControlCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_laundry_washer";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module" — polarity isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define LAUNDRY_WASHER_DOOR_GPIO GPIO_NUM_4                /* reed switch, pulled up: LOW=closed, HIGH=open */
#define LAUNDRY_WASHER_SENSOR_GPIO GPIO_NUM_21               /* DS18B20, wash-water temperature */
#define LAUNDRY_WASHER_HEATER_RELAY_GPIO GPIO_NUM_16          /* active-LOW */
#define LAUNDRY_WASHER_MOTOR_RELAY_GPIO GPIO_NUM_17            /* active-LOW — agitation AND spin, see header comment */
#define LAUNDRY_WASHER_DRAIN_PUMP_RELAY_GPIO GPIO_NUM_18       /* active-LOW */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Wash-temperature target range (Matter's global `temperature` type — int16,
 * hundredths of a degree C). 20.00-60.00 degC, default 40.00 degC. */
#define LAUNDRY_WASHER_TEMP_MIN_CENTIDEGREES 2000
#define LAUNDRY_WASHER_TEMP_MAX_CENTIDEGREES 6000
#define LAUNDRY_WASHER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES 4000

/* Bang-bang (hysteresis) control band for the heater — same reasoning as
 * firmware/dishwasher/'s and firmware/refrigerator/'s own control loops. */
#define LAUNDRY_WASHER_HYSTERESIS_CENTIDEGREES 50

/* Wash-cycle phase durations — real, adjustable wash-phase-scale timing. */
#define LAUNDRY_WASHER_WASH_DURATION_MS (30UL * 60UL * 1000UL)
#define LAUNDRY_WASHER_RINSE_DURATION_MS (5UL * 60UL * 1000UL)
#define LAUNDRY_WASHER_DRAIN_DURATION_MS (3UL * 60UL * 1000UL)
#define LAUNDRY_WASHER_SPIN_DURATION_MS (8UL * 60UL * 1000UL)

/* How often the control task re-evaluates the heater hysteresis / phase
 * deadlines while a cycle is running. */
#define LAUNDRY_WASHER_CONTROL_INTERVAL_MS 5000

/* Door-sensor debounce — same shape firmware/dishwasher/'s own door task
 * already uses. */
#define LAUNDRY_WASHER_DOOR_POLL_INTERVAL_MS 200
#define LAUNDRY_WASHER_DOOR_DEBOUNCE_SAMPLES 3

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error a blanket `using namespace chip;` caused there.
 * Everything else from `chip::`/`chip::app::` below is spelled out fully
 * qualified instead. */
using namespace chip::literals;

static uint16_t laundry_washer_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/dishwasher/'s (itself from firmware/
 * refrigerator/'s / firmware/water-heater/'s / firmware/thermostat/'s)
 * DS18B20 driver — see those files' own header comments for the 1-Wire
 * timing/CRC detail and sourcing. */
static bool ow_reset(void)
{
    gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(LAUNDRY_WASHER_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(LAUNDRY_WASHER_SENSOR_GPIO);
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
    io_conf.pin_bit_mask = (1ULL << LAUNDRY_WASHER_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(LAUNDRY_WASHER_SENSOR_GPIO, 1);
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
 * `g_operational_state` mirrors whatever OperationalStateDelegate last set
 * via SetOperationalState() — read by LaundryWasherModeDelegate (to reject
 * a mode change while Running) and by laundry_washer_task() (to know
 * whether the wash cycle should currently be advancing). `g_door_open` is
 * written by door_task(), read by both. `g_number_of_rinses` is written by
 * app_attribute_update_cb() (see the header comment above for why this is
 * the one LaundryWasherControls attribute this file actually tracks). */
static uint8_t g_operational_state = 0; /* OperationalStateEnum::kStopped */
static bool g_door_open = false;
static uint8_t g_number_of_rinses = chip::to_underlying(LaundryWasherControls::NumberOfRinsesEnum::kNormal);

static bool g_heater_on = false;
static bool g_motor_on = false;
static bool g_drain_pump_on = false;

static void set_heater(bool on)
{
    g_heater_on = on;
    gpio_set_level(LAUNDRY_WASHER_HEATER_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static void set_motor(bool on)
{
    g_motor_on = on;
    gpio_set_level(LAUNDRY_WASHER_MOTOR_RELAY_GPIO, on ? 0 : 1);
}

static void set_drain_pump(bool on)
{
    g_drain_pump_on = on;
    gpio_set_level(LAUNDRY_WASHER_DRAIN_PUMP_RELAY_GPIO, on ? 0 : 1);
}

static void stop_all_actuators(void)
{
    set_heater(false);
    set_motor(false);
    set_drain_pump(false);
}

/* --- Registry-lookup-and-cast helpers --------------------------------------
 * Same two patterns firmware/dishwasher/'s own header comment documents in
 * full: TemperatureControlCluster is a DefaultServerCluster (code-driven);
 * OperationalState::Instance is delegate-managed but not registry-based,
 * hence `get_delegate_managed_instance()` instead. */
static TemperatureControlCluster *get_temperature_control_cluster(void)
{
    chip::app::ConcreteClusterPath path(laundry_washer_endpoint_id, TemperatureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<TemperatureControlCluster *>(iface);
}

static OperationalState::Instance *get_operational_state_instance(void)
{
    cluster_t *cl = cluster::get(laundry_washer_endpoint_id, OperationalState::Id);
    if (!cl) {
        return nullptr;
    }
    return static_cast<OperationalState::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cl));
}

/* --- OperationalState delegate ---------------------------------------------
 * Ported from the same connectedhomeip reference firmware/dishwasher/'s
 * own header comment documents in full (examples/dishwasher-app/
 * dishwasher-common/src/operational-state-delegate-impl.cpp) — the same
 * shape applies verbatim to the generic OperationalState cluster
 * regardless of which device type hosts it. */
class LaundryWasherOperationalStateDelegate : public OperationalState::Delegate
{
public:
    /* No live countdown reported — same documented scope cut as
     * firmware/dishwasher/'s own delegate. */
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::Nullable<uint32_t>();
    }

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, OperationalState::GenericOperationalState &operationalState) override
    {
        if (index >= kNumStates) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState = OperationalState::GenericOperationalState(kStates[index]);
        return CHIP_NO_ERROR;
    }

    /* No PhaseList implemented — same scope cut as firmware/dishwasher/. */
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandleStartStateCallback(OperationalState::GenericOperationalError &err) override
    {
        if (g_door_open) {
            ESP_LOGW(TAG, "Start rejected — door is open");
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToStartOrResume));
            return;
        }
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kRunning);
        begin_wash_cycle();
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleStopStateCallback(OperationalState::GenericOperationalError &err) override
    {
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
        stop_all_actuators();
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandlePauseStateCallback(OperationalState::GenericOperationalError &err) override
    {
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kPaused));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kPaused);
        stop_all_actuators();
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleResumeStateCallback(OperationalState::GenericOperationalError &err) override
    {
        if (g_door_open) {
            ESP_LOGW(TAG, "Resume rejected — door is open");
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToStartOrResume));
            return;
        }
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kRunning);
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

private:
    static constexpr size_t kNumStates = 4;
    static constexpr uint8_t kStates[kNumStates] = {
        chip::to_underlying(OperationalState::OperationalStateEnum::kStopped),
        chip::to_underlying(OperationalState::OperationalStateEnum::kRunning),
        chip::to_underlying(OperationalState::OperationalStateEnum::kPaused),
        chip::to_underlying(OperationalState::OperationalStateEnum::kError),
    };

    /* Forward-declared below (needs the wash-phase globals, defined
     * further down alongside laundry_washer_task()). */
    void begin_wash_cycle();
};
constexpr uint8_t LaundryWasherOperationalStateDelegate::kStates[];

static LaundryWasherOperationalStateDelegate operational_state_delegate;

/* --- LaundryWasherMode delegate -------------------------------------------
 * Same shape as firmware/dishwasher/'s own DishwasherModeDelegate,
 * including the identical kInvalidInMode-while-Running business rule —
 * see that file's own header comment for the shared reasoning. */
class LaundryWasherModeDelegate : public ModeBase::Delegate
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
        if (g_operational_state == chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
            ESP_LOGW(TAG, "LaundryWasherMode change rejected — a wash cycle is running");
            response.status = chip::to_underlying(ModeBase::StatusCode::kInvalidInMode);
            return;
        }
        ESP_LOGI(TAG, "LaundryWasherMode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsNormal[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kNormal)}};
    ModeTagType tagsDelicate[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kDelicate)}};
    ModeTagType tagsHeavy[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kHeavy)}};
    ModeTagType tagsWhites[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kWhites)}};

    static constexpr uint8_t kModeNormal = 0;
    static constexpr uint8_t kModeDelicate = 1;
    static constexpr uint8_t kModeHeavy = 2;
    static constexpr uint8_t kModeWhites = 3;
    static constexpr size_t kNumModes = 4;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Normal"_span, .mode = kModeNormal, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsNormal)},
        {.label = "Delicate"_span, .mode = kModeDelicate, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsDelicate)},
        {.label = "Heavy"_span, .mode = kModeHeavy, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsHeavy)},
        {.label = "Whites"_span, .mode = kModeWhites, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsWhites)},
    };
};

static LaundryWasherModeDelegate laundry_washer_mode_delegate;

/* --- LaundryWasherControls delegate ---------------------------------------
 * See the header comment above for why this cluster needs BOTH a real
 * FeatureMap and a Delegate supplying the SpinSpeeds/SupportedRinses
 * lists — SpinSpeedCurrent/NumberOfRinses themselves are plain ember
 * attributes, not touched by this Delegate at all. */
class LaundryWasherControlsDelegate : public LaundryWasherControls::Delegate
{
public:
    CHIP_ERROR GetSpinSpeedAtIndex(size_t index, chip::MutableCharSpan &spinSpeed) override
    {
        if (index >= kNumSpinSpeeds) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan(kSpinSpeeds[index], strlen(kSpinSpeeds[index])), spinSpeed);
    }

    CHIP_ERROR GetSupportedRinseAtIndex(size_t index, LaundryWasherControls::NumberOfRinsesEnum &supportedRinse) override
    {
        if (index >= kNumSupportedRinses) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        supportedRinse = kSupportedRinses[index];
        return CHIP_NO_ERROR;
    }

private:
    static constexpr size_t kNumSpinSpeeds = 4;
    const char *kSpinSpeeds[kNumSpinSpeeds] = {"Off", "Low", "Medium", "High"};

    static constexpr size_t kNumSupportedRinses = 4;
    const LaundryWasherControls::NumberOfRinsesEnum kSupportedRinses[kNumSupportedRinses] = {
        LaundryWasherControls::NumberOfRinsesEnum::kNone,
        LaundryWasherControls::NumberOfRinsesEnum::kNormal,
        LaundryWasherControls::NumberOfRinsesEnum::kExtra,
        LaundryWasherControls::NumberOfRinsesEnum::kMax,
    };
};

static LaundryWasherControlsDelegate laundry_washer_controls_delegate;

/* --- Wash-cycle state machine -------------------------------------------
 * See the header comment above for the full phase/timing detail —
 * Washing, then `g_number_of_rinses` x (Rinsing + Draining), then
 * Spinning. */
enum class WashPhase { kIdle, kWashing, kRinsing, kDraining, kSpinning };
static WashPhase g_wash_phase = WashPhase::kIdle;
static int64_t g_phase_deadline_ms = 0;
static uint8_t g_rinses_remaining = 0;

void LaundryWasherOperationalStateDelegate::begin_wash_cycle()
{
    g_wash_phase = WashPhase::kWashing;
    g_phase_deadline_ms = esp_timer_get_time() / 1000 + (int64_t)LAUNDRY_WASHER_WASH_DURATION_MS;
    g_rinses_remaining = g_number_of_rinses; /* NumberOfRinsesEnum values are 0-3, used directly as a count */
    set_motor(true);
    ESP_LOGI(TAG, "Wash cycle started (%u rinse(s) planned)", g_rinses_remaining);
}

/* --- Door sensor: debounced poll, drives a safety-pause ------------------
 * Same shape as firmware/dishwasher/'s own door_task() — no alarm cluster
 * to raise a bit on here, see the header comment above for why. */
static void door_task(void *arg)
{
    bool last_sample = false;
    int consistent_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LAUNDRY_WASHER_DOOR_POLL_INTERVAL_MS));

        bool sample_open = (gpio_get_level(LAUNDRY_WASHER_DOOR_GPIO) == 1);
        if (sample_open == last_sample) {
            consistent_count++;
        } else {
            consistent_count = 1;
            last_sample = sample_open;
        }

        if (consistent_count >= LAUNDRY_WASHER_DOOR_DEBOUNCE_SAMPLES && sample_open != g_door_open) {
            g_door_open = sample_open;
            ESP_LOGI(TAG, "Laundry washer door %s", sample_open ? "OPEN" : "closed");

            if (sample_open && g_operational_state == chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
                OperationalState::Instance *instance = get_operational_state_instance();
                if (instance) {
                    instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kPaused));
                    g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kPaused);
                    stop_all_actuators();
                    ESP_LOGW(TAG, "Door opened mid-cycle — paused for safety");
                }
            }
        }
    }
}

/* --- Control task: heater hysteresis + wash-cycle phase advancement -----
 * Runs continuously; only actually drives anything while
 * OperationalState == Running. */
static void laundry_washer_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LAUNDRY_WASHER_CONTROL_INTERVAL_MS));

        if (g_operational_state != chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        if (g_wash_phase == WashPhase::kWashing) {
            float temperature_c;
            if (sensor_read(&temperature_c)) {
                int16_t measured_centidegrees = (int16_t)(temperature_c * 100.0f);
                TemperatureControlCluster *ctrl = get_temperature_control_cluster();
                int16_t target_centidegrees = ctrl ? ctrl->GetTemperatureSetpoint() : LAUNDRY_WASHER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES;

                if (measured_centidegrees <= target_centidegrees - LAUNDRY_WASHER_HYSTERESIS_CENTIDEGREES) {
                    if (!g_heater_on) {
                        set_heater(true);
                    }
                } else if (measured_centidegrees >= target_centidegrees + LAUNDRY_WASHER_HYSTERESIS_CENTIDEGREES) {
                    if (g_heater_on) {
                        set_heater(false);
                    }
                }
                ESP_LOGI(TAG, "Wash water: %.2f degC", temperature_c);
            } else if (g_heater_on) {
                set_heater(false); /* fail-safe: no reading, no confident heating decision */
            }

            if (now_ms >= g_phase_deadline_ms) {
                set_heater(false);
                if (g_rinses_remaining > 0) {
                    g_rinses_remaining--;
                    g_wash_phase = WashPhase::kRinsing;
                    g_phase_deadline_ms = now_ms + (int64_t)LAUNDRY_WASHER_RINSE_DURATION_MS;
                    ESP_LOGI(TAG, "Wash phase complete — rinsing (%u rinse(s) left after this one)", g_rinses_remaining);
                } else {
                    set_motor(false);
                    g_wash_phase = WashPhase::kSpinning;
                    g_phase_deadline_ms = now_ms + (int64_t)LAUNDRY_WASHER_SPIN_DURATION_MS;
                    set_motor(true);
                    ESP_LOGI(TAG, "Wash phase complete — spinning (no rinses planned)");
                }
            }
        } else if (g_wash_phase == WashPhase::kRinsing) {
            if (now_ms >= g_phase_deadline_ms) {
                set_motor(false);
                set_drain_pump(true);
                g_wash_phase = WashPhase::kDraining;
                g_phase_deadline_ms = now_ms + (int64_t)LAUNDRY_WASHER_DRAIN_DURATION_MS;
                ESP_LOGI(TAG, "Rinse complete — draining");
            }
        } else if (g_wash_phase == WashPhase::kDraining) {
            if (now_ms >= g_phase_deadline_ms) {
                set_drain_pump(false);
                if (g_rinses_remaining > 0) {
                    g_rinses_remaining--;
                    g_wash_phase = WashPhase::kRinsing;
                    g_phase_deadline_ms = now_ms + (int64_t)LAUNDRY_WASHER_RINSE_DURATION_MS;
                    set_motor(true);
                    ESP_LOGI(TAG, "Drain complete — rinsing (%u rinse(s) left after this one)", g_rinses_remaining);
                } else {
                    g_wash_phase = WashPhase::kSpinning;
                    g_phase_deadline_ms = now_ms + (int64_t)LAUNDRY_WASHER_SPIN_DURATION_MS;
                    set_motor(true);
                    ESP_LOGI(TAG, "Drain complete — spinning");
                }
            }
        } else if (g_wash_phase == WashPhase::kSpinning) {
            if (now_ms >= g_phase_deadline_ms) {
                set_motor(false);
                g_wash_phase = WashPhase::kIdle;

                OperationalState::Instance *instance = get_operational_state_instance();
                if (instance) {
                    instance->OnOperationCompletionDetected(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
                    instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
                }
                g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
                ESP_LOGI(TAG, "Wash cycle complete");
            }
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

/* Tracks a controller's own writes to LaundryWasherControls' NumberOfRinses
 * attribute — see the header comment above for why this is the one
 * LaundryWasherControls attribute this file actually gives physical
 * meaning to. SpinSpeedCurrent is left purely informational and not
 * tracked here. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != laundry_washer_endpoint_id ||
        cluster_id != LaundryWasherControls::Id) {
        return ESP_OK;
    }

    if (attribute_id == LaundryWasherControls::Attributes::NumberOfRinses::Id) {
        g_number_of_rinses = val->val.u8;
        ESP_LOGI(TAG, "NumberOfRinses set to %u", g_number_of_rinses);
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

    /* 2. Configure the door sensor (pulled up, LOW=closed/HIGH=open). */
    gpio_config_t door_io_conf = {};
    door_io_conf.pin_bit_mask = (1ULL << LAUNDRY_WASHER_DOOR_GPIO);
    door_io_conf.mode = GPIO_MODE_INPUT;
    door_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&door_io_conf);
    g_door_open = (gpio_get_level(LAUNDRY_WASHER_DOOR_GPIO) == 1);

    /* 2b. Configure the three relay outputs — boot off (de-energized). */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << LAUNDRY_WASHER_HEATER_RELAY_GPIO) |
        (1ULL << LAUNDRY_WASHER_MOTOR_RELAY_GPIO) | (1ULL << LAUNDRY_WASHER_DRAIN_PUMP_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(LAUNDRY_WASHER_HEATER_RELAY_GPIO, 1);
    gpio_set_level(LAUNDRY_WASHER_MOTOR_RELAY_GPIO, 1);
    gpio_set_level(LAUNDRY_WASHER_DRAIN_PUMP_RELAY_GPIO, 1);

    /* 2c. Configure the DS18B20 sensor pin. */
    sensor_setup();

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

    /* 3. Build the Matter data model: one node, one Laundry Washer endpoint
     * (Descriptor + OperationalState via the complete top-level helper,
     * plus Identify + TemperatureControl + LaundryWasherMode +
     * LaundryWasherControls added manually) — see the header comment
     * above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::laundry_washer::config_t laundry_washer_config;
    laundry_washer_config.operational_state.delegate = &operational_state_delegate;

    endpoint_t *endpoint = endpoint::laundry_washer::create(node, &laundry_washer_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create laundry washer endpoint");
        return;
    }
    laundry_washer_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Laundry washer endpoint id: %u", laundry_washer_endpoint_id);

    /* 3a. Identify — optionalConform, so laundry_washer::add() doesn't
     * create it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3b. TemperatureControl (TN-only) — see the header comment above for
     * the legacy-vs-generated feature_flags/field-name note. */
    cluster::temperature_control::config_t temp_ctrl_config;
    temp_ctrl_config.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    temp_ctrl_config.features.temperature_number.temp_setpoint = LAUNDRY_WASHER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES;
    temp_ctrl_config.features.temperature_number.min_temperature = LAUNDRY_WASHER_TEMP_MIN_CENTIDEGREES;
    temp_ctrl_config.features.temperature_number.max_temperature = LAUNDRY_WASHER_TEMP_MAX_CENTIDEGREES;
    cluster::temperature_control::create(endpoint, &temp_ctrl_config, CLUSTER_FLAG_SERVER);

    /* 3c. LaundryWasherMode. */
    cluster::laundry_washer_mode::config_t mode_config;
    mode_config.current_mode = 0;
    mode_config.delegate = &laundry_washer_mode_delegate;
    cluster::laundry_washer_mode::create(endpoint, &mode_config, CLUSTER_FLAG_SERVER);

    /* 3d. LaundryWasherControls — both Spin and Rinse features enabled;
     * see the header comment above for why FeatureMap is safe to set
     * explicitly here (no gap, unlike RefrigeratorAlarm/AirQuality). */
    cluster::laundry_washer_controls::config_t controls_config;
    controls_config.feature_flags = cluster::laundry_washer_controls::feature::spin::get_id() |
        cluster::laundry_washer_controls::feature::rinse::get_id();
    controls_config.features.spin.spin_speed_current = 2; /* "Medium" — see kSpinSpeeds above */
    controls_config.features.rinse.number_of_rinses = chip::to_underlying(LaundryWasherControls::NumberOfRinsesEnum::kNormal);
    controls_config.delegate = &laundry_washer_controls_delegate;
    cluster::laundry_washer_controls::create(endpoint, &controls_config, CLUSTER_FLAG_SERVER);

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

    /* 5. Start the control tasks — the door sensor's own debounced poll,
     * and the wash-cycle/heater-hysteresis control loop. */
    xTaskCreate(door_task, "door_task", 3072, NULL, 5, NULL);
    xTaskCreate(laundry_washer_task, "laundry_washer_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter laundry washer started. Scan the QR code to commission.");
}
