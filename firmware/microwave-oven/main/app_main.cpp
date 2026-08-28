/*
 * Minimal Matter Microwave Oven — forty-second device type, and this
 * repo's first over the Microwave Oven Mode + Microwave Oven Control
 * clusters — a genuinely new command-and-cluster combination, layered on
 * top of the same generic OperationalState cluster firmware/dishwasher/,
 * firmware/laundry-washer/, and firmware/laundry-dryer/ already established.
 *
 * *** SAFETY NOTE — read this before wiring anything up ****************
 * This firmware does NOT implement, control, or interface with a real
 * consumer microwave oven's own high-voltage magnetron/transformer, or its
 * door-interlock safety-switch system — that machinery stays entirely
 * internal to a certified appliance and is never bypassed, duplicated, or
 * second-guessed by this firmware, the same "well outside what this repo's
 * read-the-datasheet-drive-the-GPIO style should attempt" boundary
 * firmware/evse/'s own header comment already draws around a vehicle
 * charger's Control Pilot protocol. `MICROWAVE_COOK_RELAY_GPIO` is designed
 * to gate an ALREADY-BUILT, already-certified OEM/commercial cooking
 * MODULE's own external remote-start/enable input — the kind of
 * control-board-integration-ready module sold for vending machines,
 * restaurant equipment, and commercial microwave retrofit kits, which
 * expose exactly that kind of simple external control interface separate
 * from their own always-on internal safety circuitry. A typical countertop
 * consumer microwave has NO such external control input at all and is NOT
 * what this firmware is meant to be wired into — same "gate an existing
 * device's own control input, never its unsafe internals directly"
 * framing firmware/thermostat/'s boiler RELAY output and firmware/evse/'s
 * own relay already establish for their own mains-adjacent hardware.
 * ************************************************************************
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 * Works on other ESP32 chips too — see the README for how to switch target.
 *
 * --- Device type + scope -----------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * MicrowaveOven.xml (device type 0x0079): Identify is only optionalConform
 * (added manually below, same as firmware/dishwasher/'s/firmware/
 * water-heater/'s own device types); MicrowaveOvenMode + MicrowaveOvenControl
 * + OperationalState are all mandatoryConform (OperationalState's own
 * CountdownTime attribute AND OperationCompletion event are BOTH explicitly
 * called out as mandatoryConform here too, unlike the base OperationalState
 * cluster where CountdownTime is merely optional — this file implements a
 * real, live countdown rather than the null-stub firmware/dishwasher/'s own
 * header comment documents for that same attribute, specifically because
 * of this device type's own stricter requirement); Fan Control is
 * optionalConform (not implemented — no fan on this hobby build, same
 * "smallest reasonable next step" scope cut as every other optional extra
 * in this repo). `endpoint::microwave_oven::create()` confirmed complete/
 * ready-to-use by reading esp-matter's own legacy `microwave_oven::add()`
 * directly — Descriptor (via `common::create<T>()`) + OperationalState
 * (with CountdownTime attribute + OperationCompletion event pre-registered)
 * + MicrowaveOvenMode + MicrowaveOvenControl, all from ONE config_t.
 *
 * --- Feature scope: PowerAsNumber, not PowerInWatts ----------------------
 * MicrowaveOvenControl's cluster XML defines PowerAsNumber (PWRNUM) and
 * PowerInWatts (WATTS) as a real, exactly-one choice group — confirmed both
 * in the XML's own conform markers and in esp-matter's own
 * `microwave_oven_control::create()`, which calls
 * `VALIDATE_FEATURES_EXACT_ONE("PowerAsNumber,PowerInWatts", ...)`. WATTS
 * is chosen against here for two reasons: its own XML marks it
 * `provisionalConform` in the fallback case (not implementable against a
 * real certified product yet), and enabling it would also need
 * SupportedWatts/SelectedWattIndex — themselves independently marked
 * provisional in the same XML. PowerAsNumber (a plain 1-100 power
 * percentage) is both the non-provisional, spec-stable choice AND the more
 * natural fit for the duty-cycle-driven relay output below. PowerNumberLimits
 * (PWRLMTS, which would expose adjustable Min/Max/StepPower attributes) is
 * left off too — same "smallest reasonable next step" scope cut as
 * elsewhere in this repo; MicrowaveOvenControlCluster's own source falls
 * back to a sensible fixed 10-100 range in 10-step increments when PWRLMTS
 * isn't enabled (confirmed by reading MicrowaveOvenControlCluster.cpp's own
 * anonymous-namespace kDefaultMinPowerNum/kDefaultMaxPowerNum/
 * kDefaultPowerStepNum constants directly), so this file's own
 * GetMinPowerNum()/GetMaxPowerNum()/GetPowerStepNum() overrides are never
 * actually called by the cluster and exist only to satisfy the Delegate's
 * fixed interface.
 *
 * --- Power level -> relay duty cycle -------------------------------------
 * Real consumer microwaves implement "power level" (as opposed to a
 * variable-output magnetron, which doesn't exist in cheap consumer
 * hardware) by cycling a fixed-output magnetron on and off over a repeating
 * multi-second window — well-documented, general appliance-design
 * knowledge, not something needing per-chip datasheet verification.
 * MICROWAVE_DUTY_CYCLE_WINDOW_SEC (10s) is that window; cook_task below
 * computes, once per second, whether the relay should currently be
 * energized by comparing the current position within the window against
 * PowerSetting's own percentage of that window — a real 50% power setting
 * genuinely turns the relay on for half of each 10-second window, off for
 * the other half, matching the real-world technique.
 *
 * --- MicrowaveOvenControl needs BOTH OperationalState AND MicrowaveOvenMode
 * already constructed — esp-matter handles this automatically -----------
 * Confirmed by reading esp-matter's own `MicrowaveOvenControlDelegateInitCB`
 * (in `esp_matter_delegate_callbacks.cpp`) directly: unlike ClosureControl
 * (see firmware/closure/'s own header comment for that cluster's OPPOSITE,
 * before-`start()` ordering requirement), this callback is fully
 * self-contained — it lazily constructs the `OperationalState::Instance`
 * and `ModeBase::Instance` itself (if they don't already exist) using
 * whichever of those two clusters' own `config->delegate` was set at
 * `create()` time, THEN constructs the `MicrowaveOvenControlCluster` with
 * an esp-matter-provided `IntegrationDelegate` bridging to both. This means
 * simply setting all three delegates (`operational_state.delegate`,
 * `microwave_oven_mode.delegate`, `microwave_oven_control.delegate`) on the
 * SAME `microwave_oven::config_t` before calling `microwave_oven::create()`
 * — the plain, ordinary `config->delegate`-before-`start()` pattern
 * firmware/water-heater/'s WaterHeaterMode and firmware/robot-vacuum/'s
 * RvcRunMode already use — is enough; no manual pre/post-`start()` calls of
 * any kind are needed for this cluster family, regardless of which cluster
 * esp-matter's own `invoke_init_callbacks_internal()` happens to process
 * first (confirmed self-healing either way, since the callback checks for
 * an already-constructed instance before building a new one).
 *
 * --- Command flow, ported from connectedhomeip's own real reference ------
 * `MicrowaveOvenControlCluster::HandleSetCookingParameters()`/
 * `HandleAddMoreTime()` (read directly, not assumed) validate the request
 * against MainState/FeatureMap conformance and then call this file's own
 * `AppDelegate::HandleSetCookingParametersCallback()`/
 * `HandleModifyCookTimeSecondsCallback()` — but, confirmed by reading that
 * same source, do NOT themselves call `SetCookTimeSec()` on the cluster or
 * `SetOperationalState()` on OperationalState afterward; that responsibility
 * is entirely the delegate's own, the same "the app should trigger the
 * state change" pattern firmware/door-lock/'s LockState and firmware/
 * valve/'s CurrentState already establish. This file's own business logic
 * (which cook time/power/mode to apply, and whether `startAfterSetting`
 * should immediately transition to Running) is ported from
 * connectedhomeip's own real reference
 * (`examples/microwave-oven-app/microwave-oven-common/src/
 * microwave-oven-device.cpp`, read end to end) — the same "port a real
 * reference rather than guess the integration shape" precedent already
 * used in this repo for SM2335EGH/APA102/OpenTherm/RVC — adapted from that
 * reference's own manually-constructed `Instance` members (that example
 * targets connectedhomeip's generic Linux app framework directly, not
 * esp-matter) to this repo's own registry-lookup-and-cast /
 * `get_delegate_managed_instance()` accessors instead.
 *
 * MicrowaveOvenMode offers Normal/Defrost (`ModeTag::kNormal`/`kDefrost`,
 * confirmed against connectedhomeip's own generated `MicrowaveOvenMode/
 * Enums.h`) — the same two modes the reference example ships, and the only
 * two tags this cluster's own Enums.h actually defines beyond the shared,
 * generic ModeBase tags. Same "reject a mode change while actively
 * running" business rule firmware/dishwasher/'s and firmware/
 * laundry-washer/'s own Mode delegates already establish.
 *
 * A real countdown IS implemented (unlike firmware/dishwasher/'s own
 * documented null-stub GetCountdownTime(), justified there by CountdownTime
 * being merely optional on that device type) — `cook_task` decrements the
 * tracked remaining cook time once a second while Running, calls
 * `OperationalState::Instance::UpdateCountdownTimeFromDelegate()` (the
 * real, documented "please re-read GetCountdownTime() and report if it
 * changed" trigger — confirmed by reading `CodegenIntegration.h` directly)
 * to push the change, and also republishes CookTime via
 * `MicrowaveOvenControlCluster::SetCookTimeSec()` so both attributes track
 * the same underlying countdown consistently.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no OEM microwave cooking module or relay hardware for
 * this device type physically available when written — see the safety note
 * above for why a typical consumer microwave isn't a substitute).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/microwave-oven-control-server/microwave-oven-control-server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_microwave_oven";

/* Gates an OEM/commercial cooking module's own remote-start/enable input —
 * see the prominent safety note at the top of this file for the full
 * explanation of what this is (and is NOT) meant to be wired into.
 * Active-LOW, matching this repo's own relay convention elsewhere. GPIO 4
 * is a plain, unreserved GPIO on classic ESP32 (WROOM-32). Adjust to match
 * your board. */
#define MICROWAVE_COOK_RELAY_GPIO GPIO_NUM_4

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself. GPIO 2 is common on classic ESP32 (WROOM-32) devkits. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

/* See the header comment on power-level-to-duty-cycle for the full
 * explanation of why this window exists and what it represents. */
#define MICROWAVE_DUTY_CYCLE_WINDOW_SEC 10

/* Real, adjustable maximum cook time this device will accept — 30 minutes,
 * a generous real-world ceiling for a microwave, well inside the cluster's
 * own spec-wide 86400s (24h) cap. */
#define MICROWAVE_MAX_COOK_TIME_SEC 1800

/* Default cook time (a plain, common "quick cook" starting point) and
 * default power (spec's own documented default for SetCookingParameters'
 * PowerSetting field when omitted is MaxPower — 100%, matching
 * connectedhomeip's own reference example). */
#define MICROWAVE_DEFAULT_COOK_TIME_SEC 30
#define MICROWAVE_DEFAULT_POWER_NUM 100

using namespace esp_matter;
using namespace chip::app::Clusters;
/* Needed for the "..."_span string-literal operator used by the mode
 * option table below — narrowed to just this operator rather than a
 * blanket `using namespace chip;`, the same fix firmware/robot-vacuum/'s
 * and firmware/water-heater/'s own header comments already document for
 * the identical compile error. */
using namespace chip::literals;
using Status = chip::Protocols::InteractionModel::Status;

static uint16_t microwave_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Cross-delegate shared state ----------------------------------------
 * Mirrors what each Matter attribute/cluster currently holds — mutated
 * only by the delegates below and cook_task, read by all three. */
static uint8_t g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
static uint8_t g_cook_mode = 0; /* Normal */
static uint32_t g_cook_time_remaining_sec = MICROWAVE_DEFAULT_COOK_TIME_SEC;
static uint8_t g_power_setting_num = MICROWAVE_DEFAULT_POWER_NUM;
static uint32_t g_duty_cycle_seconds_elapsed = 0;

/* Drives the cook relay directly — see the header comment on duty cycling
 * for why this is toggled once a second from cook_task, not held solidly
 * on for the whole cook. Active-LOW. */
static void set_cook_relay(bool on)
{
    gpio_set_level(MICROWAVE_COOK_RELAY_GPIO, on ? 0 : 1);
}

/* --- Registry-lookup-and-cast helpers ------------------------------------
 * OperationalState::Instance and ModeBase::Instance are both delegate-
 * managed live C++ objects (constructed automatically by esp-matter's own
 * MicrowaveOvenControlDelegateInitCB — see the header comment above), so
 * `get_delegate_managed_instance()` is the right accessor for both, same
 * pattern firmware/dishwasher/'s own get_operational_state_instance()
 * already establishes. MicrowaveOvenControlCluster is a genuine
 * DefaultServerCluster (code-driven, registered with the data model
 * provider's registry), so it needs the plain registry-lookup-and-cast
 * pattern instead, same as firmware/valve/'s get_valve_cluster() and
 * firmware/closure/'s get_closure_cluster(). */
static OperationalState::Instance *get_operational_state_instance(void)
{
    cluster_t *cl = cluster::get(microwave_endpoint_id, OperationalState::Id);
    if (!cl) {
        return nullptr;
    }
    return static_cast<OperationalState::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cl));
}

static ModeBase::Instance *get_mode_instance(void)
{
    cluster_t *cl = cluster::get(microwave_endpoint_id, MicrowaveOvenMode::Id);
    if (!cl) {
        return nullptr;
    }
    return static_cast<ModeBase::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cl));
}

static MicrowaveOvenControlCluster *get_control_cluster(void)
{
    chip::app::ConcreteClusterPath path(microwave_endpoint_id, MicrowaveOvenControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<MicrowaveOvenControlCluster *>(iface);
}

/* Shared by HandleSetCookingParametersCallback() (when startAfterSetting is
 * true) and HandleStartStateCallback()/HandleResumeStateCallback() below —
 * deliberately does NOT touch g_cook_time_remaining_sec, so Resume
 * continues from wherever Pause left off rather than restarting. */
static bool start_cooking(void)
{
    OperationalState::Instance *instance = get_operational_state_instance();
    if (!instance) {
        return false;
    }
    CHIP_ERROR result = instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
    if (result != CHIP_NO_ERROR) {
        return false;
    }
    g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kRunning);
    g_duty_cycle_seconds_elapsed = 0; /* restart the duty-cycle window cleanly */
    ESP_LOGI(TAG, "Cooking started: %lus remaining at %u%% power", (unsigned long)g_cook_time_remaining_sec, g_power_setting_num);
    return true;
}

static void stop_cooking(uint8_t new_state)
{
    OperationalState::Instance *instance = get_operational_state_instance();
    if (instance) {
        instance->SetOperationalState(new_state);
    }
    g_operational_state = new_state;
    set_cook_relay(false);
}

/* --- OperationalState delegate -------------------------------------------
 * See the header comment above for the full detail on why each
 * HandleXStateCallback below is ported from connectedhomeip's own real
 * reference (examples/microwave-oven-app/), and why GetCountdownTime()
 * returns a real, live value rather than the null stub firmware/
 * dishwasher/'s own delegate uses for the same method. */
class MicrowaveOperationalStateDelegate : public OperationalState::Delegate
{
public:
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::Nullable<uint32_t>(g_cook_time_remaining_sec);
    }

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, OperationalState::GenericOperationalState &operationalState) override
    {
        if (index >= kNumStates) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState = OperationalState::GenericOperationalState(kStates[index]);
        return CHIP_NO_ERROR;
    }

    /* No PhaseList implemented — a plain cook cycle has no distinct named
     * phases the way a wash cycle does. Returning CHIP_ERROR_NOT_FOUND for
     * index 0 tells the SDK PhaseList is null, same as firmware/
     * dishwasher/'s own stub. */
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandleStartStateCallback(OperationalState::GenericOperationalError &err) override
    {
        if (!start_cooking()) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleStopStateCallback(OperationalState::GenericOperationalError &err) override
    {
        stop_cooking(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandlePauseStateCallback(OperationalState::GenericOperationalError &err) override
    {
        stop_cooking(chip::to_underlying(OperationalState::OperationalStateEnum::kPaused));
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleResumeStateCallback(OperationalState::GenericOperationalError &err) override
    {
        if (!start_cooking()) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
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
};
constexpr uint8_t MicrowaveOperationalStateDelegate::kStates[];

static MicrowaveOperationalStateDelegate operational_state_delegate;

/* --- MicrowaveOvenMode delegate ------------------------------------------
 * Normal/Defrost — the only two ModeTag values this cluster's own
 * generated Enums.h actually defines beyond ModeBase's shared generic
 * tags, confirmed by reading MicrowaveOvenMode/Enums.h directly (same
 * check applied to every other ModeBase-derived cluster in this repo).
 * Same "reject a mode change while running" business rule firmware/
 * dishwasher/'s own DishwasherModeDelegate already establishes. */
class MicrowaveModeDelegate : public ModeBase::Delegate
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
            ESP_LOGW(TAG, "MicrowaveOvenMode change rejected — actively cooking");
            response.status = chip::to_underlying(ModeBase::StatusCode::kInvalidInMode);
            return;
        }
        g_cook_mode = newMode;
        ESP_LOGI(TAG, "MicrowaveOvenMode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsNormal[1] = {{.value = chip::to_underlying(MicrowaveOvenMode::ModeTag::kNormal)}};
    ModeTagType tagsDefrost[1] = {{.value = chip::to_underlying(MicrowaveOvenMode::ModeTag::kDefrost)}};

    static constexpr uint8_t kModeNormal = 0;
    static constexpr uint8_t kModeDefrost = 1;
    static constexpr size_t kNumModes = 2;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Normal"_span, .mode = kModeNormal, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsNormal)},
        {.label = "Defrost"_span, .mode = kModeDefrost, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsDefrost)},
    };
};

static MicrowaveModeDelegate mode_delegate;

/* --- MicrowaveOvenControl AppDelegate -------------------------------------
 * See the header comment above for the full command-flow detail (what the
 * cluster itself validates vs. what this delegate has to do), ported from
 * connectedhomeip's own real reference example. */
class MicrowaveControlDelegate : public MicrowaveOvenControl::Delegate
{
public:
    Status HandleSetCookingParametersCallback(uint8_t cookMode, uint32_t cookTimeSec, bool startAfterSetting,
                                              chip::Optional<uint8_t> powerSettingNum,
                                              chip::Optional<uint8_t> wattSettingIndex) override
    {
        (void)wattSettingIndex; /* PowerInWatts feature not enabled — see the header comment on feature scope */

        ModeBase::Instance *mode = get_mode_instance();
        if (mode) {
            Status s = mode->UpdateCurrentMode(cookMode);
            if (s != Status::Success) {
                return s;
            }
        }
        g_cook_mode = cookMode;

        MicrowaveOvenControlCluster *cluster = get_control_cluster();
        if (cluster) {
            cluster->SetCookTimeSec(cookTimeSec);
        }
        g_cook_time_remaining_sec = cookTimeSec;

        if (powerSettingNum.HasValue()) {
            g_power_setting_num = powerSettingNum.Value();
        }

        ESP_LOGI(TAG, "Cooking parameters set: mode=%u time=%lus power=%u%% startAfterSetting=%d",
                 cookMode, (unsigned long)cookTimeSec, g_power_setting_num, (int)startAfterSetting);

        if (startAfterSetting && !start_cooking()) {
            return Status::Failure;
        }
        return Status::Success;
    }

    Status HandleModifyCookTimeSecondsCallback(uint32_t finalCookTimeSec) override
    {
        MicrowaveOvenControlCluster *cluster = get_control_cluster();
        if (cluster) {
            cluster->SetCookTimeSec(finalCookTimeSec);
        }
        g_cook_time_remaining_sec = finalCookTimeSec;
        ESP_LOGI(TAG, "Cook time updated to %lus", (unsigned long)finalCookTimeSec);
        return Status::Success;
    }

    /* PowerInWatts feature isn't enabled — never actually called by the
     * cluster (confirmed by reading MicrowaveOvenControlCluster.cpp
     * directly: GetWattSettingByIndex() is only invoked from code paths
     * gated behind Feature::kPowerInWatts). Trivial stub, same "unreachable
     * pure-virtual override" precedent used elsewhere in this repo. */
    CHIP_ERROR GetWattSettingByIndex(uint8_t index, uint16_t &wattSetting) override
    {
        (void)index;
        (void)wattSetting;
        return CHIP_ERROR_NOT_FOUND;
    }

    uint32_t GetMaxCookTimeSec() const override { return MICROWAVE_MAX_COOK_TIME_SEC; }
    uint8_t GetPowerSettingNum() const override { return g_power_setting_num; }

    /* PowerNumberLimits isn't enabled — never actually called (only read
     * behind Feature::kPowerNumberLimits) — trivial stubs matching the
     * cluster's own internal defaults for documentation purposes only. */
    uint8_t GetMinPowerNum() const override { return 10; }
    uint8_t GetMaxPowerNum() const override { return 100; }
    uint8_t GetPowerStepNum() const override { return 10; }

    /* PowerInWatts isn't enabled — never actually called. */
    uint8_t GetCurrentWattIndex() const override { return 0; }
    uint16_t GetWattRating() const override { return 0; }
};

static MicrowaveControlDelegate control_delegate;

/* Single shared task — wakes once a second, and while Running: decrements
 * the tracked remaining cook time, republishes it through both CookTime
 * (MicrowaveOvenControl) and CountdownTime (OperationalState), drives the
 * cook relay's duty cycle for the current second (see the header comment
 * on power-level-to-duty-cycle), and — once the countdown reaches zero —
 * stops cooking and fires the mandatory OperationCompletion event. */
static void cook_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (g_operational_state != chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
            continue;
        }

        if (g_cook_time_remaining_sec > 0) {
            g_cook_time_remaining_sec--;
        }

        MicrowaveOvenControlCluster *cluster = get_control_cluster();
        if (cluster) {
            cluster->SetCookTimeSec(g_cook_time_remaining_sec);
        }
        OperationalState::Instance *instance = get_operational_state_instance();
        if (instance) {
            instance->UpdateCountdownTimeFromDelegate();
        }

        if (g_cook_time_remaining_sec == 0) {
            ESP_LOGI(TAG, "Cooking complete");
            if (instance) {
                instance->OnOperationCompletionDetected(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
            }
            stop_cooking(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
            continue;
        }

        g_duty_cycle_seconds_elapsed++;
        uint32_t window_pos = g_duty_cycle_seconds_elapsed % MICROWAVE_DUTY_CYCLE_WINDOW_SEC;
        uint32_t on_seconds = (g_power_setting_num * MICROWAVE_DUTY_CYCLE_WINDOW_SEC + 50) / 100;
        set_cook_relay(window_pos < on_seconds);
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

/* Nothing on this device needs to react to a plain-ember attribute write —
 * every attribute that matters here is served by a code-driven cluster
 * instead (OperationalState/MicrowaveOvenMode/MicrowaveOvenControl, all
 * Delegate-based) — kept as a trivial stub, same as several other device
 * types in this repo. */
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

    /* 2. Configure the cook relay output, starting de-energized. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << MICROWAVE_COOK_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    set_cook_relay(false);

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

    /* 3. Build the Matter data model: one node, one Microwave Oven endpoint
     * (Descriptor + OperationalState + MicrowaveOvenMode +
     * MicrowaveOvenControl via the complete top-level helper, plus Identify
     * added manually — see the header comment on device type + scope). All
     * three delegates are set on the same config_t before create() — see
     * the header comment on why no manual pre/post-start() ordering is
     * needed for this cluster family, unlike firmware/closure/'s own
     * ClosureControl. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Microwave Oven",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::microwave_oven::config_t microwave_config;
    microwave_config.operational_state.delegate = &operational_state_delegate;
    microwave_config.microwave_oven_mode.delegate = &mode_delegate;
    microwave_config.microwave_oven_mode.current_mode = 0; /* Normal */
    microwave_config.microwave_oven_control.delegate = &control_delegate;
    microwave_config.microwave_oven_control.feature_flags =
        cluster::microwave_oven_control::feature::power_as_number::get_id();

    endpoint_t *endpoint = endpoint::microwave_oven::create(node, &microwave_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create microwave oven endpoint");
        return;
    }
    microwave_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Microwave oven endpoint id: %u", microwave_endpoint_id);

    /* 3a. Identify — optionalConform, so microwave_oven::add() doesn't
     * create it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    xTaskCreate(cook_task, "microwave_cook", 4096, NULL, 5, NULL);

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

    ESP_LOGI(TAG, "Matter microwave oven started. Scan the QR code to commission.");
}
