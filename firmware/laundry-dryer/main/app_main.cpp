/*
 * Minimal Matter Laundry Dryer — thirty-first device type, and the closest
 * sibling to firmware/laundry-washer/ in this repo: the same generic
 * OperationalState cluster (0x0060), the same TemperatureControl (TN-only)
 * pattern, and its own mode cluster — except the CSA's own device type XML
 * reuses LaundryWasher's Mode cluster verbatim for a dryer rather than
 * defining a dryer-specific one, confirmed directly rather than assumed.
 * This is also this repo's first laundry appliance with no water handling
 * at all — no fill, no rinse, no drain — just heat and tumble.
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
 * --- Endpoint: only OperationalState is mandatory, same shape as
 * firmware/dishwasher/'s and firmware/laundry-washer/'s own endpoints -----
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * LaundryDryer.xml: only OperationalState is `<mandatoryConform/>` (with its
 * own OperationCompletion event also mandatory, per this device type's own
 * revision 2) — Identify, On/Off (DeadFrontOnOff feature only, a narrow "is
 * the device's own UI/front-panel powered" semantic, not a real power
 * switch — confirmed by name and left out, same "smallest reasonable next
 * step" scope cut as firmware/dishwasher/'s and firmware/laundry-washer/'s
 * own identical skip), Laundry Dryer Controls, Laundry Washer Mode, and
 * TemperatureControl are all `<optionalConform/>`. Confirmed by reading
 * `esp_matter_endpoint.cpp`'s own `laundry_dryer::add()` directly: it's
 * structurally identical to `dish_washer::add()`/`laundry_washer::add()`
 * (all three share `config_t = app_with_operational_state_config` and only
 * create OperationalState) — Identify + TemperatureControl +
 * LaundryWasherMode + LaundryDryerControls are all added manually onto this
 * same endpoint afterward, same pattern.
 *
 * A real, spec-level detail worth remembering: the device type XML's own
 * cluster entry is literally `<cluster id="0x0051" name="Laundry Washer
 * Mode" .../>` — NOT a separate "Laundry Dryer Mode" cluster. Confirmed by
 * reading the XML directly rather than assumed from the device type's own
 * name. Two constraints are layered onto that reused cluster for this
 * device type specifically — the DEPONOFF feature and the StartUpMode
 * attribute are both `<disallowConform/>` here — but neither needed any
 * code to enforce: reading `laundry_washer_mode::create()` in
 * `esp_matter_cluster.cpp` directly shows it hardcodes FeatureMap to 0
 * (config_t has no feature_flags field at all — DEPONOFF was never
 * reachable through this helper to begin with) and never calls a
 * StartUpMode attribute-creation function either, so both constraints are
 * already satisfied by the helper's own existing (narrower) scope, the
 * same way firmware/laundry-washer/'s own use of this cluster already
 * relied on. Mode tags offered here are Normal/Delicate/Heavy (reused
 * directly — real settings that apply just as well to drying as washing)
 * plus Quick (`ModeTag::kQuick`, one of `ModeBase`'s own *common* tags, not
 * one of LaundryWasherMode's own Normal/Delicate/Heavy/Whites-specific
 * ones) in place of Whites — Whites is a wash-specific concept (bleach-safe
 * water temperature) that doesn't translate to a drying setting, confirmed
 * by reading the full `LaundryWasherMode::ModeTag` enum directly (which
 * does list both the cluster-specific tags and the base ModeBase common
 * tags in the same header) before picking a fourth option. Same
 * `kInvalidInMode`-while-Running rejection rule as firmware/dishwasher/'s
 * and firmware/laundry-washer/'s own Mode delegates; LaundryDryerMode
 * itself is purely informational here (no distinct heat/duration profile
 * per mode), same "smallest reasonable next step" scope cut as
 * firmware/laundry-washer/'s own SpinSpeedCurrent.
 *
 * --- OperationalState: identical pattern to firmware/dishwasher/'s and
 * firmware/laundry-washer/'s own, but a 2-phase cycle instead of a
 * multi-rinse one ---------------------------------------------------------
 * Same automatic `config->delegate` wiring, same
 * `get_delegate_managed_instance()` lookup for the two places this file
 * touches the cluster from outside the delegate's own callbacks (the door
 * sensor's async safety-pause, and the dry cycle finishing on its own) —
 * see firmware/dishwasher/'s own header comment for the full detail on why
 * (the sixth "reach a live cluster instance from app code" pattern in this
 * repo). `HandleStartStateCallback()`/`HandleResumeStateCallback()` reject
 * with `ErrorStateEnum::kUnableToStartOrResume` if the door is open;
 * opening the door mid-cycle PAUSES rather than errors the
 * OperationalState (real front-loader/tumble-dryer behavior, same as
 * firmware/laundry-washer/'s own door handling) — LaundryDryer's own
 * device type XML lists no Alarm cluster either, so there's no DoorError-
 * style bit to raise here; the Pause itself is the only signal a
 * controller gets.
 *
 * --- TemperatureControl: identical TN-only pattern to
 * firmware/laundry-washer/'s, this time representing the drying-air
 * target temperature (min 40.00 degC/low-heat setting, max 80.00 degC,
 * default 60.00 degC — ordinary real tumble-dryer heater-air temperature
 * range). Same legacy-vs-generated `feature_flags`/`temp_setpoint`
 * handling firmware/refrigerator/'s and firmware/laundry-washer/'s own
 * header comments already document in full.
 *
 * --- LaundryDryerControls: SelectedDrynessLevel is the one setting this
 * file actually gives real physical meaning to ----------------------------
 * Confirmed by reading `esp_matter_cluster.cpp`'s own
 * `laundry_dryer_controls::create()` directly: FeatureMap is hardcoded to
 * 0 (this cluster defines no features at all, confirmed against its own
 * cluster XML — unlike LaundryWasherControls' Spin/Rinse pair, there's
 * nothing to enable here). SupportedDrynessLevels is a real Delegate-
 * served list (`GetSupportedDrynessLevelAtIndex()`, the same
 * "feature-flag-gated attributes plus a separate Delegate for the
 * supported-list" shape firmware/laundry-washer/'s own
 * LaundryWasherControls Delegate already established, minus the feature
 * flags here) — this file offers all four real `DrynessLevelEnum` values
 * (Low/Normal/Extra/Max, confirmed against connectedhomeip's own generated
 * Enums.h). SelectedDrynessLevel itself IS a plain ember attribute
 * (confirmed: the cluster registers a
 * `MatterLaundryDryerControlsClusterServerPreAttributeChangedCallback`,
 * the same PRE_ATTRIBUTE_CHANGED-hook shape LaundryWasherControls already
 * uses) — a controller can write it directly, no command needed. A real,
 * previously-undocumented gotcha was found by reading
 * `laundry-dryer-controls-server.cpp` directly rather than assuming it
 * behaves like LaundryWasherControls: its own
 * `PreAttributeChangedCallback` calls `VerifyOrDie(delegate != nullptr)`
 * before validating a SelectedDrynessLevel write against the supported
 * list — unlike every other optional-delegate cluster in this repo, a
 * controller writing this attribute with NO delegate registered would
 * abort the whole device, not just silently no-op. `config_t.delegate` is
 * therefore always set here, never left null. SelectedDrynessLevel
 * genuinely drives this file's own dry-cycle simulation (tracked via the
 * same `attribute::PRE_UPDATE` pattern firmware/laundry-washer/'s own
 * NumberOfRinses tracking already established): it sets how long the
 * Drying phase actually runs (20/35/50/65 minutes for Low/Normal/Extra/
 * Max respectively) — the one setting in this file given real physical
 * meaning, same "one setting genuinely drives the cycle, the rest stay
 * informational" precedent firmware/laundry-washer/'s own header comment
 * already establishes for NumberOfRinses vs. SpinSpeedCurrent.
 *
 * --- The dry cycle itself: Drying, then Cooldown — no water at all -------
 * `laundry_dryer_task()` drives two real relay outputs
 * (`LAUNDRY_DRYER_HEATER_RELAY_GPIO`/`_MOTOR_RELAY_GPIO`, both active-LOW,
 * matching firmware/laundry-washer/'s own relay convention) through a real,
 * two-phase timed sequence: Drying (heater, hysteresis-controlled against a
 * DS18B20 reading and TemperatureControl's own live setpoint, same 0.5 degC
 * hysteresis convention used throughout this repo's control loops, plus the
 * drum motor tumbling) for however long SelectedDrynessLevel currently
 * calls for; then Cooldown (motor only, heater off) for
 * `LAUNDRY_DRYER_COOLDOWN_DURATION_MS` — a real, standard tumble-dryer
 * behavior (an unheated tumble at the end of a cycle reduces wrinkling and
 * the fire risk of hot lint sitting still), not invented for this file —
 * before calling `OnOperationCompletionDetected()` and returning to
 * Stopped. Only two relays needed here, not firmware/laundry-washer/'s
 * three — this is this repo's first laundry appliance with no water
 * handling at all: no Fill phase, no Rinse phase, no drain pump, matching
 * how a real tumble dryer actually works (a heater + a drum motor, nothing
 * else to actuate).
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
#include <app/clusters/laundry-dryer-controls-server/laundry-dryer-controls-delegate.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>
#include <app/clusters/temperature-control-server/TemperatureControlCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_laundry_dryer";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module" — polarity isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define LAUNDRY_DRYER_DOOR_GPIO GPIO_NUM_4                /* reed switch, pulled up: LOW=closed, HIGH=open */
#define LAUNDRY_DRYER_SENSOR_GPIO GPIO_NUM_21               /* DS18B20, drying-air temperature */
#define LAUNDRY_DRYER_HEATER_RELAY_GPIO GPIO_NUM_16          /* active-LOW */
#define LAUNDRY_DRYER_MOTOR_RELAY_GPIO GPIO_NUM_17            /* active-LOW — drum motor */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Drying-air temperature target range (Matter's global `temperature` type —
 * int16, hundredths of a degree C). 40.00-80.00 degC, default 60.00 degC. */
#define LAUNDRY_DRYER_TEMP_MIN_CENTIDEGREES 4000
#define LAUNDRY_DRYER_TEMP_MAX_CENTIDEGREES 8000
#define LAUNDRY_DRYER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES 6000

/* Bang-bang (hysteresis) control band for the heater — same reasoning as
 * firmware/laundry-washer/'s and firmware/dishwasher/'s own control loops. */
#define LAUNDRY_DRYER_HYSTERESIS_CENTIDEGREES 50

/* Drying-phase duration per SelectedDrynessLevel — see the header comment
 * above for why this is the one LaundryDryerControls attribute this file
 * gives real physical meaning to. */
#define LAUNDRY_DRYER_DRY_DURATION_LOW_MS (20UL * 60UL * 1000UL)
#define LAUNDRY_DRYER_DRY_DURATION_NORMAL_MS (35UL * 60UL * 1000UL)
#define LAUNDRY_DRYER_DRY_DURATION_EXTRA_MS (50UL * 60UL * 1000UL)
#define LAUNDRY_DRYER_DRY_DURATION_MAX_MS (65UL * 60UL * 1000UL)

/* Fixed, unheated cool-down at the end of every cycle — see the header
 * comment above for why this is standard real tumble-dryer behavior. */
#define LAUNDRY_DRYER_COOLDOWN_DURATION_MS (5UL * 60UL * 1000UL)

/* How often the control task re-evaluates the heater hysteresis / phase
 * deadline while a cycle is running. */
#define LAUNDRY_DRYER_CONTROL_INTERVAL_MS 5000

/* Door-sensor debounce — same shape firmware/laundry-washer/'s own door
 * task already uses. */
#define LAUNDRY_DRYER_DOOR_POLL_INTERVAL_MS 200
#define LAUNDRY_DRYER_DOOR_DEBOUNCE_SAMPLES 3

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error a blanket `using namespace chip;` caused there.
 * Everything else from `chip::`/`chip::app::` below is spelled out fully
 * qualified instead. */
using namespace chip::literals;

static uint16_t laundry_dryer_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/laundry-washer/'s (itself from firmware/
 * dishwasher/'s / firmware/refrigerator/'s / firmware/water-heater/'s /
 * firmware/thermostat/'s) DS18B20 driver — see those files' own header
 * comments for the 1-Wire timing/CRC detail and sourcing. */
static bool ow_reset(void)
{
    gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(LAUNDRY_DRYER_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(LAUNDRY_DRYER_SENSOR_GPIO);
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
    io_conf.pin_bit_mask = (1ULL << LAUNDRY_DRYER_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(LAUNDRY_DRYER_SENSOR_GPIO, 1);
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
 * via SetOperationalState() — read by LaundryDryerModeDelegate (to reject a
 * mode change while Running) and by laundry_dryer_task() (to know whether
 * the dry cycle should currently be advancing). `g_door_open` is written by
 * door_task(), read by both. `g_selected_dryness_level` is written by
 * app_attribute_update_cb() (see the header comment above for why this is
 * the one LaundryDryerControls attribute this file actually tracks). */
static uint8_t g_operational_state = 0; /* OperationalStateEnum::kStopped */
static bool g_door_open = false;
static uint8_t g_selected_dryness_level = chip::to_underlying(LaundryDryerControls::DrynessLevelEnum::kNormal);

static bool g_heater_on = false;
static bool g_motor_on = false;

static void set_heater(bool on)
{
    g_heater_on = on;
    gpio_set_level(LAUNDRY_DRYER_HEATER_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static void set_motor(bool on)
{
    g_motor_on = on;
    gpio_set_level(LAUNDRY_DRYER_MOTOR_RELAY_GPIO, on ? 0 : 1);
}

static void stop_all_actuators(void)
{
    set_heater(false);
    set_motor(false);
}

/* Maps SelectedDrynessLevel to a real drying-phase duration — see the
 * header comment above. */
static uint32_t dry_duration_ms_for_level(uint8_t level)
{
    switch ((LaundryDryerControls::DrynessLevelEnum)level) {
    case LaundryDryerControls::DrynessLevelEnum::kLow:
        return LAUNDRY_DRYER_DRY_DURATION_LOW_MS;
    case LaundryDryerControls::DrynessLevelEnum::kExtra:
        return LAUNDRY_DRYER_DRY_DURATION_EXTRA_MS;
    case LaundryDryerControls::DrynessLevelEnum::kMax:
        return LAUNDRY_DRYER_DRY_DURATION_MAX_MS;
    case LaundryDryerControls::DrynessLevelEnum::kNormal:
    default:
        return LAUNDRY_DRYER_DRY_DURATION_NORMAL_MS;
    }
}

/* --- Registry-lookup-and-cast helpers --------------------------------------
 * Same two patterns firmware/dishwasher/'s own header comment documents in
 * full: TemperatureControlCluster is a DefaultServerCluster (code-driven);
 * OperationalState::Instance is delegate-managed but not registry-based,
 * hence `get_delegate_managed_instance()` instead. */
static TemperatureControlCluster *get_temperature_control_cluster(void)
{
    chip::app::ConcreteClusterPath path(laundry_dryer_endpoint_id, TemperatureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<TemperatureControlCluster *>(iface);
}

static OperationalState::Instance *get_operational_state_instance(void)
{
    cluster_t *cl = cluster::get(laundry_dryer_endpoint_id, OperationalState::Id);
    if (!cl) {
        return nullptr;
    }
    return static_cast<OperationalState::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cl));
}

/* --- OperationalState delegate ---------------------------------------------
 * Ported from the same connectedhomeip reference firmware/dishwasher/'s own
 * header comment documents in full (examples/dishwasher-app/
 * dishwasher-common/src/operational-state-delegate-impl.cpp) — the same
 * shape applies verbatim to the generic OperationalState cluster regardless
 * of which device type hosts it. */
class LaundryDryerOperationalStateDelegate : public OperationalState::Delegate
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
        begin_dry_cycle();
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

    /* Forward-declared below (needs the dry-phase globals, defined further
     * down alongside laundry_dryer_task()). */
    void begin_dry_cycle();
};
constexpr uint8_t LaundryDryerOperationalStateDelegate::kStates[];

static LaundryDryerOperationalStateDelegate operational_state_delegate;

/* --- LaundryWasherMode delegate (reused for this dryer — see the header
 * comment above for why there is no separate dryer-specific mode cluster)
 * Same shape as firmware/dishwasher/'s and firmware/laundry-washer/'s own
 * Mode delegates, including the identical kInvalidInMode-while-Running
 * business rule — see those files' own header comments for the shared
 * reasoning. */
class LaundryDryerModeDelegate : public ModeBase::Delegate
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
            ESP_LOGW(TAG, "Mode change rejected — a dry cycle is running");
            response.status = chip::to_underlying(ModeBase::StatusCode::kInvalidInMode);
            return;
        }
        ESP_LOGI(TAG, "Mode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsNormal[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kNormal)}};
    ModeTagType tagsDelicate[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kDelicate)}};
    ModeTagType tagsHeavy[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kHeavy)}};
    /* Quick — one of ModeBase's own *common* tags, not one of
     * LaundryWasherMode's cluster-specific ones. Used in place of Whites
     * (a wash-specific concept, see the header comment above). */
    ModeTagType tagsQuick[1] = {{.value = chip::to_underlying(LaundryWasherMode::ModeTag::kQuick)}};

    static constexpr uint8_t kModeNormal = 0;
    static constexpr uint8_t kModeDelicate = 1;
    static constexpr uint8_t kModeHeavy = 2;
    static constexpr uint8_t kModeQuick = 3;
    static constexpr size_t kNumModes = 4;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Normal"_span, .mode = kModeNormal, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsNormal)},
        {.label = "Delicate"_span, .mode = kModeDelicate, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsDelicate)},
        {.label = "Heavy"_span, .mode = kModeHeavy, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsHeavy)},
        {.label = "Quick"_span, .mode = kModeQuick, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsQuick)},
    };
};

static LaundryDryerModeDelegate laundry_dryer_mode_delegate;

/* --- LaundryDryerControls delegate -----------------------------------------
 * See the header comment above for why this Delegate must ALWAYS be
 * registered (the cluster's own PreAttributeChangedCallback calls
 * VerifyOrDie() on it) — unlike LaundryWasherControls, there is no
 * feature-flag gate here, just this one supported-list method. */
class LaundryDryerControlsDelegate : public LaundryDryerControls::Delegate
{
public:
    CHIP_ERROR GetSupportedDrynessLevelAtIndex(size_t index, LaundryDryerControls::DrynessLevelEnum &supportedDryness) override
    {
        if (index >= kNumSupportedLevels) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        supportedDryness = kSupportedLevels[index];
        return CHIP_NO_ERROR;
    }

private:
    static constexpr size_t kNumSupportedLevels = 4;
    const LaundryDryerControls::DrynessLevelEnum kSupportedLevels[kNumSupportedLevels] = {
        LaundryDryerControls::DrynessLevelEnum::kLow,
        LaundryDryerControls::DrynessLevelEnum::kNormal,
        LaundryDryerControls::DrynessLevelEnum::kExtra,
        LaundryDryerControls::DrynessLevelEnum::kMax,
    };
};

static LaundryDryerControlsDelegate laundry_dryer_controls_delegate;

/* --- Dry-cycle state machine ---------------------------------------------
 * See the header comment above for the full phase/timing detail — Drying
 * (duration set by SelectedDrynessLevel), then a fixed unheated Cooldown. */
enum class DryPhase { kIdle, kDrying, kCooldown };
static DryPhase g_dry_phase = DryPhase::kIdle;
static int64_t g_phase_deadline_ms = 0;

void LaundryDryerOperationalStateDelegate::begin_dry_cycle()
{
    g_dry_phase = DryPhase::kDrying;
    uint32_t duration_ms = dry_duration_ms_for_level(g_selected_dryness_level);
    g_phase_deadline_ms = esp_timer_get_time() / 1000 + (int64_t)duration_ms;
    set_motor(true);
    ESP_LOGI(TAG, "Dry cycle started (%lu minute(s) of drying planned)", (unsigned long)(duration_ms / 60000UL));
}

/* --- Door sensor: debounced poll, drives a safety-pause ------------------
 * Same shape as firmware/laundry-washer/'s own door_task() — no alarm
 * cluster to raise a bit on here, see the header comment above for why. */
static void door_task(void *arg)
{
    bool last_sample = false;
    int consistent_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LAUNDRY_DRYER_DOOR_POLL_INTERVAL_MS));

        bool sample_open = (gpio_get_level(LAUNDRY_DRYER_DOOR_GPIO) == 1);
        if (sample_open == last_sample) {
            consistent_count++;
        } else {
            consistent_count = 1;
            last_sample = sample_open;
        }

        if (consistent_count >= LAUNDRY_DRYER_DOOR_DEBOUNCE_SAMPLES && sample_open != g_door_open) {
            g_door_open = sample_open;
            ESP_LOGI(TAG, "Laundry dryer door %s", sample_open ? "OPEN" : "closed");

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

/* --- Control task: heater hysteresis + dry-cycle phase advancement ------
 * Runs continuously; only actually drives anything while
 * OperationalState == Running. */
static void laundry_dryer_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LAUNDRY_DRYER_CONTROL_INTERVAL_MS));

        if (g_operational_state != chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        if (g_dry_phase == DryPhase::kDrying) {
            float temperature_c;
            if (sensor_read(&temperature_c)) {
                int16_t measured_centidegrees = (int16_t)(temperature_c * 100.0f);
                TemperatureControlCluster *ctrl = get_temperature_control_cluster();
                int16_t target_centidegrees = ctrl ? ctrl->GetTemperatureSetpoint() : LAUNDRY_DRYER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES;

                if (measured_centidegrees <= target_centidegrees - LAUNDRY_DRYER_HYSTERESIS_CENTIDEGREES) {
                    if (!g_heater_on) {
                        set_heater(true);
                    }
                } else if (measured_centidegrees >= target_centidegrees + LAUNDRY_DRYER_HYSTERESIS_CENTIDEGREES) {
                    if (g_heater_on) {
                        set_heater(false);
                    }
                }
                ESP_LOGI(TAG, "Drying air: %.2f degC", temperature_c);
            } else if (g_heater_on) {
                set_heater(false); /* fail-safe: no reading, no confident heating decision */
            }

            if (now_ms >= g_phase_deadline_ms) {
                set_heater(false);
                g_dry_phase = DryPhase::kCooldown;
                g_phase_deadline_ms = now_ms + (int64_t)LAUNDRY_DRYER_COOLDOWN_DURATION_MS;
                ESP_LOGI(TAG, "Drying complete — cooldown (unheated tumble)");
            }
        } else if (g_dry_phase == DryPhase::kCooldown) {
            if (now_ms >= g_phase_deadline_ms) {
                set_motor(false);
                g_dry_phase = DryPhase::kIdle;

                OperationalState::Instance *instance = get_operational_state_instance();
                if (instance) {
                    instance->OnOperationCompletionDetected(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
                    instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
                }
                g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
                ESP_LOGI(TAG, "Dry cycle complete");
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

/* Tracks a controller's own writes to LaundryDryerControls'
 * SelectedDrynessLevel attribute — see the header comment above for why
 * this is the one attribute this file actually gives physical meaning to. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != laundry_dryer_endpoint_id ||
        cluster_id != LaundryDryerControls::Id) {
        return ESP_OK;
    }

    if (attribute_id == LaundryDryerControls::Attributes::SelectedDrynessLevel::Id) {
        g_selected_dryness_level = val->val.u8;
        ESP_LOGI(TAG, "SelectedDrynessLevel set to %u", g_selected_dryness_level);
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
    door_io_conf.pin_bit_mask = (1ULL << LAUNDRY_DRYER_DOOR_GPIO);
    door_io_conf.mode = GPIO_MODE_INPUT;
    door_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&door_io_conf);
    g_door_open = (gpio_get_level(LAUNDRY_DRYER_DOOR_GPIO) == 1);

    /* 2b. Configure the two relay outputs — boot off (de-energized). */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << LAUNDRY_DRYER_HEATER_RELAY_GPIO) |
        (1ULL << LAUNDRY_DRYER_MOTOR_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(LAUNDRY_DRYER_HEATER_RELAY_GPIO, 1);
    gpio_set_level(LAUNDRY_DRYER_MOTOR_RELAY_GPIO, 1);

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

    /* 3. Build the Matter data model: one node, one Laundry Dryer endpoint
     * (Descriptor + OperationalState via the complete top-level helper,
     * plus Identify + TemperatureControl + LaundryWasherMode (reused) +
     * LaundryDryerControls added manually) — see the header comment above
     * for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::laundry_dryer::config_t laundry_dryer_config;
    laundry_dryer_config.operational_state.delegate = &operational_state_delegate;

    endpoint_t *endpoint = endpoint::laundry_dryer::create(node, &laundry_dryer_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create laundry dryer endpoint");
        return;
    }
    laundry_dryer_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Laundry dryer endpoint id: %u", laundry_dryer_endpoint_id);

    /* 3a. Identify — optionalConform, so laundry_dryer::add() doesn't
     * create it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3b. TemperatureControl (TN-only) — see the header comment above for
     * the legacy-vs-generated feature_flags/field-name note. */
    cluster::temperature_control::config_t temp_ctrl_config;
    temp_ctrl_config.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    temp_ctrl_config.features.temperature_number.temp_setpoint = LAUNDRY_DRYER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES;
    temp_ctrl_config.features.temperature_number.min_temperature = LAUNDRY_DRYER_TEMP_MIN_CENTIDEGREES;
    temp_ctrl_config.features.temperature_number.max_temperature = LAUNDRY_DRYER_TEMP_MAX_CENTIDEGREES;
    cluster::temperature_control::create(endpoint, &temp_ctrl_config, CLUSTER_FLAG_SERVER);

    /* 3c. LaundryWasherMode — reused for this dryer, see the header comment
     * above for why there is no separate dryer-specific mode cluster. */
    cluster::laundry_washer_mode::config_t mode_config;
    mode_config.current_mode = 0;
    mode_config.delegate = &laundry_dryer_mode_delegate;
    cluster::laundry_washer_mode::create(endpoint, &mode_config, CLUSTER_FLAG_SERVER);

    /* 3d. LaundryDryerControls — the delegate MUST be set, see the header
     * comment above for the VerifyOrDie() gotcha this cluster's own
     * PreAttributeChangedCallback has. */
    cluster::laundry_dryer_controls::config_t controls_config;
    controls_config.selected_dryness_level = nullable<uint8_t>((uint8_t)g_selected_dryness_level);
    controls_config.delegate = &laundry_dryer_controls_delegate;
    cluster::laundry_dryer_controls::create(endpoint, &controls_config, CLUSTER_FLAG_SERVER);

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
     * and the dry-cycle/heater-hysteresis control loop. */
    xTaskCreate(door_task, "door_task", 3072, NULL, 5, NULL);
    xTaskCreate(laundry_dryer_task, "laundry_dryer_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter laundry dryer started. Scan the QR code to commission.");
}
