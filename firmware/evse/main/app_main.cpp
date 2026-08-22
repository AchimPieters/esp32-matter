/*
 * Minimal Matter EVSE (electric vehicle charger controller) — twenty-fifth
 * device type, and this repo's first over the Energy EVSE cluster family.
 *
 * *** IMPORTANT SAFETY NOTE — READ BEFORE WIRING ANYTHING ***
 * This firmware does NOT implement the real SAE J1772 / IEC 61851 Control
 * Pilot protocol (the PWM signal + voltage-level state machine a real EV
 * charger uses to negotiate available current with the vehicle and detect
 * plug/fault states) — that protocol is a real, safety-relevant piece of
 * automotive-grade hardware/firmware engineering, well outside what a
 * hobbyist ESP32 GPIO project should be trusted to implement for something
 * that switches vehicle charging current. This file's relay output is
 * designed to drive a REAL, already-approved EVSE unit's own low-voltage
 * "enable"/"authorize" dry-contact input — the same simple interlock input
 * many commercial EVSEs already expose specifically for external control
 * (e.g. for a time-of-use tariff controller or a load-management system) —
 * NOT to switch AC mains or the vehicle's charging current directly. Think
 * of this the same way firmware/thermostat/'s RELAY output gates an
 * existing boiler's own low-voltage call-for-heat input rather than
 * switching the boiler's own gas valve. If your EVSE hardware has no such
 * enable input, this firmware is not a safe way to add one.
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
 * --- Endpoint: esp-matter's own complete top-level helper — including a
 * cluster the CSA's own device type XML doesn't actually list -------------
 * `endpoint::energy_evse::create()` (device type 0x050C) confirmed by
 * reading esp_matter_endpoint.cpp's own `energy_evse::add()` directly:
 * EnergyEvse + EnergyEvseMode + DeviceEnergyManagement, auto-Descriptor via
 * `common::create<T>()`. Cross-checked against the CSA's own
 * data_model/1.6/device_types/EVSE.xml: EnergyEvse + EnergyEvseMode are
 * both `<mandatoryConform/>`, matching — but DeviceEnergyManagement is NOT
 * listed anywhere in that XML at all (only Identify and Temperature
 * Measurement are listed as the device type's own `<optionalConform/>`
 * clusters, plus a composed Electrical Sensor device type). This is a
 * real, checked discrepancy between the CSA's device type XML and
 * esp-matter's own top-level helper, not a mistake in this file — kept as
 * esp-matter ships it (DeviceEnergyManagement costs nothing extra to
 * leave at its default: `delegate = nullptr`, `feature_flags = 0`, which
 * — confirmed by reading `esp_matter_cluster.cpp`'s own
 * `device_energy_management::create()` — creates the cluster with only
 * its base ember attributes (ESAType/ESACanGenerate/ESAState/AbsMinPower/
 * AbsMaxPower, all left at 0) and no delegate wiring at all, since a
 * `nullptr` delegate is a no-op the same way it is for every other
 * `config->delegate`-driven cluster in this repo). Identify is added onto
 * the endpoint afterward, same "optionalConform, not auto-wired" pattern
 * firmware/extractor-hood/ and firmware/water-heater/ already hit.
 * Temperature Measurement and the composed Electrical Sensor device type
 * are both left out — no temperature sensor or power/energy metering
 * hardware assumed, same "smallest reasonable next step" scope cut every
 * other device type's own optional extras get; firmware/temperature-
 * sensor/'s and firmware/outlet/'s own driver libraries already exist if
 * either is ever wanted here.
 *
 * --- EnergyEvseMode: a fourth ModeBase-derived cluster this repo has
 * built, identical integration to firmware/water-heater/'s WaterHeaterMode
 * Confirmed via the same `config->delegate` -> `EnergyEvseModeDelegateInitCB`
 * -> `InitModeDelegate()` automatic-construction chain already established
 * for RvcRunMode/RvcCleanMode/WaterHeaterMode. Real mode/tag values
 * (`EnergyEvseMode::ModeTag::kManual/kTimeOfUse/kSolarCharging` — `kV2X`
 * exists too but is skipped, since this file implements no V2X/bidirectional
 * charging at all) confirmed against connectedhomeip's own generated
 * Enums.h. "TimeOfUse" and "SolarCharging" are both real, selectable modes
 * but behave identically to "Manual" — no tariff schedule or solar-surplus
 * tracking is implemented, same honest "the mode value is correctly
 * reported, nothing extra happens" scope cut firmware/water-heater/'s own
 * "Timed" mode and firmware/robot-vacuum/'s own "Mapping" mode already
 * established.
 *
 * --- EnergyEvse: this repo's biggest single Delegate interface so far,
 * and a cluster the Instance itself does almost nothing for automatically
 * Confirmed by reading `EnergyEvseCluster.cpp`'s own `HandleDisable()`/
 * `HandleEnableCharging()`/`HandleEnableDischarging()` directly: beyond a
 * plain min/max-current sanity check, each one forwards straight to this
 * file's own Delegate with no State/SupplyState transition of its own —
 * exactly the same "the cluster validates the command shape, the app
 * decides the actual state" split firmware/water-heater/'s
 * WaterHeaterManagement already established. Unlike that cluster's
 * Delegate, `EnergyEvse::Delegate` (confirmed by reading `Delegate.h`
 * directly) has NO `SetInstance()`/`GetInstance()` back-pointer at all —
 * only `GetEndpointId()` — so reporting State/SupplyState/etc. back uses
 * the registry-lookup-and-cast pattern firmware/valve/'s and
 * firmware/fan/'s own setters already established (`EnergyEvse::EnergyEvseCluster`
 * confirmed to be a real, registry-registered `ServerClusterInterface`
 * the same way), not a delegate-held instance pointer.
 *
 * `ChargingPreferences` (`PREF` — SetTargets/GetTargets/ClearTargets/
 * LoadTargets, a per-day-of-week charging schedule) is confirmed
 * `<mandatoryConform/>` directly in the cluster XML's own `<features>`
 * block — genuinely mandatory, not an optional extra to skip. Implemented
 * as real, working, bounded in-memory storage (`EVSE_MAX_SCHEDULES` day-
 * of-week entries, `EVSE_MAX_TARGETS_PER_SCHEDULE` targets each) — a
 * controller can SetTargets and read the identical data back via
 * GetTargets, satisfying the mandatory command surface — but, same
 * honesty precedent as EnergyEvseMode's own unused Timed-mode-style
 * values above, NO automatic scheduler in this file actually acts on
 * stored targets (this firmware has no real-time-of-day clock and no
 * pilot-signal current negotiation to act on a target with anyway).
 * `EnableDischarging` always rejects (`UnsupportedCommand`) — no V2X
 * hardware. `StartDiagnostics` accepts and logs but runs no real self-test
 * sequence, the same simulated-self-test honesty
 * firmware/smoke-co-alarm/'s own SelfTestRequest handling already
 * established for hardware with no real diagnostic routine to run.
 *
 * State/SupplyState/FaultState are driven directly by this file's own
 * `EnableCharging()`/`Disable()` handlers and by `evse_task`'s own
 * periodic plug-detect poll — see below. `FaultState` is always
 * `NoError`: no ground-fault/over-current/contact-welding detection
 * hardware is assumed, another real reason this firmware only belongs
 * behind an already-safety-certified EVSE's own enable input, never
 * switching charging current directly (see the safety note up top).
 * `State::PluggedInDemand` (EV connected AND actively requesting current,
 * distinct from merely being connected) is never reported — telling those
 * two apart needs real Control-Pilot state-B/state-C detection this
 * firmware doesn't have, so `PluggedInNoDemand` is used for "connected,
 * not (yet) enabled" instead, an honest simplification of the same kind.
 *
 * --- Hardware: single relay + an optional plug-detect input ------------
 * `EVSE_RELAY_GPIO` (active-LOW, matching firmware/valve/'s and
 * firmware/water-heater/'s own relay convention) drives the existing
 * EVSE unit's enable input — see the safety note at the top of this file.
 * `EVSE_PLUG_DETECT_GPIO` (optional, off by default — same opt-in-GPIO
 * convention firmware/door-lock/'s position sensor and
 * firmware/robot-vacuum/'s dock sensor already use) reads a simple
 * digital HIGH-when-connected signal from a real EVSE unit's own "vehicle
 * connected" status output, if it has one. Without it wired, this file
 * optimistically assumes a vehicle is connected whenever charging is
 * enabled — the same "no feedback sensor = optimistic best-effort report"
 * precedent firmware/door-lock/'s LockState and firmware/robot-vacuum/'s
 * GoHome-without-a-dock-sensor already establish. `EVSE_CIRCUIT_CAPACITY_MA`/
 * `EVSE_MIN_CHARGE_CURRENT_MA`/`EVSE_MAX_CHARGE_CURRENT_MA` are plain,
 * adjustable #defines describing the circuit this controller is wired
 * into (default 6000 mA / 32000 mA — 6A is IEC 61851's own documented
 * minimum EV charging current, 32A a common single-phase EU home circuit
 * rating) — informational attributes only, never communicated to a
 * vehicle via any pilot signal, since none is implemented. Standard
 * quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no EVSE hardware with a real external-enable input
 * physically available when written).
 */

#include <algorithm>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/energy-evse-server/energy-evse-server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_evse";

/* Relay driving the EVSE unit's own low-voltage enable input — active-LOW,
 * matching firmware/valve/'s and firmware/water-heater/'s own relay
 * convention. *** See the safety note at the top of this file — this is
 * NOT for switching AC mains or vehicle charging current directly. *** */
#define EVSE_RELAY_GPIO GPIO_NUM_4

/* Optional "vehicle connected" input from the EVSE unit's own status
 * output — off by default (GPIO_NUM_NC). See the header comment above for
 * the optimistic fallback when this isn't wired. */
#define EVSE_PLUG_DETECT_GPIO GPIO_NUM_NC
#define EVSE_POLL_INTERVAL_MS 2000

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Informational circuit-capacity figures — see the header comment above.
 * Never communicated to a vehicle via any pilot signal (none implemented);
 * purely what this cluster's own CircuitCapacity/MinimumChargeCurrent/
 * MaximumChargeCurrent attributes report. Milliamps, matching the
 * cluster's own unit for these attributes. */
#define EVSE_CIRCUIT_CAPACITY_MA 32000
#define EVSE_MIN_CHARGE_CURRENT_MA 6000
#define EVSE_MAX_CHARGE_CURRENT_MA 32000

/* Bounded in-memory ChargingPreferences storage — see the header comment
 * above for why this is real, working, round-trippable storage but never
 * actually acted on by a scheduler. */
#define EVSE_MAX_SCHEDULES 7
#define EVSE_MAX_TARGETS_PER_SCHEDULE 4

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error a blanket `using namespace chip;` caused there.
 * Everything else from `chip::`/`chip::app::` below is spelled out fully
 * qualified instead. */
using namespace chip::literals;

static uint16_t evse_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

static bool g_relay_on = false;
static bool g_charging_enabled = false;

static void set_relay(bool on)
{
    g_relay_on = on;
    gpio_set_level(EVSE_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static bool plug_detect_enabled(void)
{
    return EVSE_PLUG_DETECT_GPIO != GPIO_NUM_NC;
}

/* Optimistic fallback when no plug-detect sensor is wired — see the
 * header comment above. */
static bool plug_detected(void)
{
    if (!plug_detect_enabled()) {
        return g_charging_enabled;
    }
    return gpio_get_level(EVSE_PLUG_DETECT_GPIO) == 1;
}

/* Registry-lookup-and-cast — same pattern firmware/valve/'s and
 * firmware/fan/'s own setters already use, needed here because
 * EnergyEvse::Delegate has no SetInstance()/GetInstance() back-pointer at
 * all (see the header comment above). */
static EnergyEvse::EnergyEvseCluster *get_evse_cluster(void)
{
    chip::app::ConcreteClusterPath path(evse_endpoint_id, EnergyEvse::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "EnergyEvse cluster not found on endpoint %u", evse_endpoint_id);
        return nullptr;
    }
    return static_cast<EnergyEvse::EnergyEvseCluster *>(iface);
}

/* --- EnergyEvseMode delegate ----------------------------------------------
 * Structurally identical to firmware/water-heater/'s WaterHeaterModeDelegate
 * — see that file's own header comment for the shared reasoning. No
 * business-rule restriction on transitions. */
class EnergyEvseModeDelegate : public ModeBase::Delegate
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
        ESP_LOGI(TAG, "EnergyEvseMode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    static constexpr uint8_t kModeManual = 0;
    static constexpr uint8_t kModeTimeOfUse = 1;
    static constexpr uint8_t kModeSolarCharging = 2;

    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsManual[1] = {{.value = chip::to_underlying(EnergyEvseMode::ModeTag::kManual)}};
    ModeTagType tagsTimeOfUse[1] = {{.value = chip::to_underlying(EnergyEvseMode::ModeTag::kTimeOfUse)}};
    ModeTagType tagsSolar[1] = {{.value = chip::to_underlying(EnergyEvseMode::ModeTag::kSolarCharging)}};

    static constexpr size_t kNumModes = 3;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Manual"_span, .mode = kModeManual, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsManual)},
        {.label = "Time of Use"_span, .mode = kModeTimeOfUse, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsTimeOfUse)},
        {.label = "Solar Charging"_span, .mode = kModeSolarCharging, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsSolar)},
    };
};

static EnergyEvseModeDelegate energy_evse_mode_delegate;

/* --- EnergyEvse delegate ---------------------------------------------------
 * See the header comment above for the overall design (no real pilot
 * signal, registry-lookup-and-cast for reporting State back, bounded
 * ChargingPreferences storage that's real but inert). */
class EnergyEvseDelegate : public EnergyEvse::Delegate
{
public:
    chip::Protocols::InteractionModel::Status Disable() override
    {
        set_relay(false);
        g_charging_enabled = false;
        report_state();
        ESP_LOGI(TAG, "Charging disabled");
        return chip::Protocols::InteractionModel::Status::Success;
    }

    chip::Protocols::InteractionModel::Status EnableCharging(const chip::app::DataModel::Nullable<uint32_t> &enableChargeTime,
                                                              const int64_t &minimumChargeCurrent,
                                                              const int64_t &maximumChargeCurrent) override
    {
        (void)minimumChargeCurrent;
        (void)maximumChargeCurrent;
        g_charging_enabled = true;
        set_relay(plug_detected());
        report_state();

        EnergyEvse::EnergyEvseCluster *cluster = get_evse_cluster();
        if (cluster) {
            cluster->SetChargingEnabledUntil(enableChargeTime);
            cluster->SetMinimumChargeCurrent(minimumChargeCurrent);
            cluster->SetMaximumChargeCurrent(maximumChargeCurrent);
        }
        ESP_LOGI(TAG, "Charging enabled (%lld-%lld mA)", (long long)minimumChargeCurrent, (long long)maximumChargeCurrent);
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /* No V2X/bidirectional-charging hardware — always rejected. */
    chip::Protocols::InteractionModel::Status EnableDischarging(const chip::app::DataModel::Nullable<uint32_t> &dischargingEnabledUntil,
                                                                 const int64_t &maximumDischargeCurrent) override
    {
        (void)dischargingEnabledUntil;
        (void)maximumDischargeCurrent;
        ESP_LOGW(TAG, "EnableDischarging requested — no V2X hardware in this design");
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;
    }

    /* No real self-test hardware — accepted and logged, same simulated-
     * self-test precedent firmware/smoke-co-alarm/'s own SelfTestRequest
     * handling already established. */
    chip::Protocols::InteractionModel::Status StartDiagnostics() override
    {
        ESP_LOGI(TAG, "StartDiagnostics requested — no real self-test hardware, treated as an immediate no-op success");
        return chip::Protocols::InteractionModel::Status::Success;
    }

    chip::Protocols::InteractionModel::Status
    SetTargets(const chip::app::DataModel::DecodableList<EnergyEvse::Structs::ChargingTargetScheduleStruct::DecodableType> &chargingTargetSchedules) override
    {
        size_t schedule_count = 0;
        auto sched_iter = chargingTargetSchedules.begin();
        while (sched_iter.Next() && schedule_count < EVSE_MAX_SCHEDULES) {
            const auto &sched = sched_iter.GetValue();
            m_schedule_day[schedule_count] = sched.dayOfWeekForSequence;

            size_t target_count = 0;
            auto target_iter = sched.chargingTargets.begin();
            while (target_iter.Next() && target_count < EVSE_MAX_TARGETS_PER_SCHEDULE) {
                m_targets[schedule_count][target_count] = target_iter.GetValue();
                target_count++;
            }
            if (target_iter.GetStatus() != CHIP_NO_ERROR) {
                return chip::Protocols::InteractionModel::Status::InvalidCommand;
            }
            m_target_count[schedule_count] = target_count;
            schedule_count++;
        }
        if (sched_iter.GetStatus() != CHIP_NO_ERROR) {
            return chip::Protocols::InteractionModel::Status::InvalidCommand;
        }
        m_schedule_count = schedule_count;
        ESP_LOGI(TAG, "SetTargets: stored %u schedule(s) — informational only, no scheduler acts on these (see header comment)",
                 (unsigned)m_schedule_count);
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /* Nothing persisted across reboots in this v1 — the in-memory store
     * above always starts empty, so there's nothing to load. */
    chip::Protocols::InteractionModel::Status LoadTargets() override { return chip::Protocols::InteractionModel::Status::Success; }

    chip::Protocols::InteractionModel::Status
    GetTargets(chip::app::DataModel::List<const EnergyEvse::Structs::ChargingTargetScheduleStruct::Type> &chargingTargetSchedules) override
    {
        for (size_t i = 0; i < m_schedule_count; i++) {
            m_schedule_out[i].dayOfWeekForSequence = m_schedule_day[i];
            m_schedule_out[i].chargingTargets =
                chip::app::DataModel::List<const EnergyEvse::Structs::ChargingTargetStruct::Type>(m_targets[i], m_target_count[i]);
        }
        chargingTargetSchedules =
            chip::app::DataModel::List<const EnergyEvse::Structs::ChargingTargetScheduleStruct::Type>(m_schedule_out, m_schedule_count);
        return chip::Protocols::InteractionModel::Status::Success;
    }

    chip::Protocols::InteractionModel::Status ClearTargets() override
    {
        m_schedule_count = 0;
        ESP_LOGI(TAG, "ClearTargets: schedule cleared");
        return chip::Protocols::InteractionModel::Status::Success;
    }

    /* Reactive callbacks fired by the cluster after each SetX() call this
     * file itself makes below — mostly just logged, nothing further to
     * react to in this design. */
    void OnStateChanged(EnergyEvse::StateEnum newValue) override { ESP_LOGI(TAG, "State -> %u", chip::to_underlying(newValue)); }
    void OnSupplyStateChanged(EnergyEvse::SupplyStateEnum newValue) override
    {
        ESP_LOGI(TAG, "SupplyState -> %u", chip::to_underlying(newValue));
    }
    void OnFaultStateChanged(EnergyEvse::FaultStateEnum newValue) override { (void)newValue; }
    void OnChargingEnabledUntilChanged(chip::app::DataModel::Nullable<uint32_t> newValue) override { (void)newValue; }
    void OnDischargingEnabledUntilChanged(chip::app::DataModel::Nullable<uint32_t> newValue) override { (void)newValue; }
    void OnCircuitCapacityChanged(int64_t newValue) override { (void)newValue; }
    void OnMinimumChargeCurrentChanged(int64_t newValue) override { (void)newValue; }
    void OnMaximumChargeCurrentChanged(int64_t newValue) override { (void)newValue; }
    void OnMaximumDischargeCurrentChanged(int64_t newValue) override { (void)newValue; }
    void OnUserMaximumChargeCurrentChanged(int64_t newValue) override { (void)newValue; }
    void OnRandomizationDelayWindowChanged(uint32_t newValue) override { (void)newValue; }
    void OnNextChargeStartTimeChanged(chip::app::DataModel::Nullable<uint32_t> newValue) override { (void)newValue; }
    void OnNextChargeTargetTimeChanged(chip::app::DataModel::Nullable<uint32_t> newValue) override { (void)newValue; }
    void OnNextChargeRequiredEnergyChanged(chip::app::DataModel::Nullable<int64_t> newValue) override { (void)newValue; }
    void OnNextChargeTargetSoCChanged(chip::app::DataModel::Nullable<chip::Percent> newValue) override { (void)newValue; }
    void OnApproximateEVEfficiencyChanged(chip::app::DataModel::Nullable<uint16_t> newValue) override { (void)newValue; }
    void OnStateOfChargeChanged(chip::app::DataModel::Nullable<chip::Percent> newValue) override { (void)newValue; }
    void OnBatteryCapacityChanged(chip::app::DataModel::Nullable<int64_t> newValue) override { (void)newValue; }
    void OnVehicleIDChanged(chip::app::DataModel::Nullable<chip::CharSpan> newValue) override { (void)newValue; }
    void OnSessionIDChanged(chip::app::DataModel::Nullable<uint32_t> newValue) override { (void)newValue; }
    void OnSessionDurationChanged(chip::app::DataModel::Nullable<uint32_t> newValue) override { (void)newValue; }
    void OnSessionEnergyChargedChanged(chip::app::DataModel::Nullable<int64_t> newValue) override { (void)newValue; }
    void OnSessionEnergyDischargedChanged(chip::app::DataModel::Nullable<int64_t> newValue) override { (void)newValue; }

    /* Called from EnableCharging()/Disable() above and from evse_task()'s
     * own plug-detect poll — computes and reports State/SupplyState from
     * the current g_charging_enabled/plug_detected() combination. See the
     * header comment above for why PluggedInDemand is never reported. */
    void report_state()
    {
        EnergyEvse::EnergyEvseCluster *cluster = get_evse_cluster();
        if (!cluster) {
            return;
        }
        bool connected = plug_detected();
        EnergyEvse::StateEnum state;
        if (!connected) {
            state = EnergyEvse::StateEnum::kNotPluggedIn;
        } else if (g_charging_enabled) {
            state = EnergyEvse::StateEnum::kPluggedInCharging;
        } else {
            state = EnergyEvse::StateEnum::kPluggedInNoDemand;
        }
        cluster->SetState(state);
        cluster->SetSupplyState(g_charging_enabled ? EnergyEvse::SupplyStateEnum::kChargingEnabled
                                                   : EnergyEvse::SupplyStateEnum::kDisabled);
    }

private:
    chip::BitMask<EnergyEvse::TargetDayOfWeekBitmap> m_schedule_day[EVSE_MAX_SCHEDULES];
    EnergyEvse::Structs::ChargingTargetStruct::Type m_targets[EVSE_MAX_SCHEDULES][EVSE_MAX_TARGETS_PER_SCHEDULE];
    size_t m_target_count[EVSE_MAX_SCHEDULES] = {};
    EnergyEvse::Structs::ChargingTargetScheduleStruct::Type m_schedule_out[EVSE_MAX_SCHEDULES];
    size_t m_schedule_count = 0;
};

static EnergyEvseDelegate energy_evse_delegate;

/* Polls the optional plug-detect input and keeps State in sync if the
 * vehicle is unplugged mid-session without a Disable command — same
 * lock-free "call a code-driven cluster's own update methods directly
 * from a plain periodic task" precedent firmware/air-purifier/'s
 * filter_life_task and firmware/water-heater/'s water_heater_task
 * already established. */
static void evse_task(void *arg)
{
    bool last_connected = plug_detected();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(EVSE_POLL_INTERVAL_MS));

        bool connected = plug_detected();
        if (connected != last_connected) {
            last_connected = connected;
            if (!connected && g_relay_on) {
                /* Vehicle unplugged mid-session — nothing left to charge. */
                set_relay(false);
            }
            energy_evse_delegate.report_state();
            ESP_LOGI(TAG, "Plug detect: %s", connected ? "connected" : "not connected");
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

/* Everything is handled through EnergyEvseDelegate/EnergyEvseModeDelegate
 * above (the clusters' own Delegate mechanisms, not the generic
 * attribute::PRE_UPDATE path) — so this is a no-op required by
 * node::create()'s callback signature, same as firmware/fan/'s and
 * firmware/valve/'s own. */
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

    /* 2. Configure the relay output — boots disabled (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << EVSE_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    set_relay(false);

    /* 2b. Configure the optional plug-detect input. */
    if (plug_detect_enabled()) {
        gpio_config_t plug_io_conf = {};
        plug_io_conf.pin_bit_mask = (1ULL << EVSE_PLUG_DETECT_GPIO);
        plug_io_conf.mode = GPIO_MODE_INPUT;
        plug_io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        gpio_config(&plug_io_conf);
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

    /* 3. Build the Matter data model: one node, one Energy EVSE endpoint
     * (EnergyEvse + EnergyEvseMode + DeviceEnergyManagement via the
     * complete top-level helper — see the header comment above for why
     * DeviceEnergyManagement is there at all), plus Identify added onto
     * that same endpoint afterward. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::energy_evse::config_t evse_config;
    evse_config.energy_evse.delegate = &energy_evse_delegate;
    evse_config.energy_evse_mode.delegate = &energy_evse_mode_delegate;
    /* device_energy_management left at its default (delegate = nullptr,
     * feature_flags = 0) — see the header comment above. */

    endpoint_t *endpoint = endpoint::energy_evse::create(node, &evse_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create EVSE endpoint");
        return;
    }

    evse_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "EVSE endpoint id: %u", evse_endpoint_id);

    /* 3b. Identify — optionalConform for this device type, so
     * energy_evse::add() doesn't create it automatically; added here the
     * same way firmware/extractor-hood/'s and firmware/water-heater/'s own
     * Identify clusters are. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(chip::app::Clusters::Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* 4b. Report the initial (disabled/not-plugged-in) state and set the
     * static circuit-capacity attributes — after esp_matter::start(),
     * same discipline this repo now follows for every registry-lookup
     * call after finding the FanControl ordering bug (see
     * firmware/fan/'s and firmware/valve/'s own header comments). */
    EnergyEvse::EnergyEvseCluster *cluster = get_evse_cluster();
    if (cluster) {
        cluster->SetCircuitCapacity(EVSE_CIRCUIT_CAPACITY_MA);
        cluster->SetMinimumChargeCurrent(EVSE_MIN_CHARGE_CURRENT_MA);
        cluster->SetMaximumChargeCurrent(EVSE_MAX_CHARGE_CURRENT_MA);
        cluster->SetFaultState(EnergyEvse::FaultStateEnum::kNoError);
    }
    energy_evse_delegate.report_state();

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    /* 5. Start the plug-detect polling task. */
    xTaskCreate(evse_task, "evse_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter EVSE controller started. Scan the QR code to commission.");
}
