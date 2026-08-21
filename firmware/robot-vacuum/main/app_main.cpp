/*
 * Minimal Matter Robotic Vacuum Cleaner (RVC) — twenty-second device type,
 * and this repo's biggest cluster-integration surface so far: three
 * separate command-handling clusters on one endpoint (RvcRunMode,
 * RvcCleanMode, RvcOperationalState), two different SDK integration
 * mechanisms for them, and a genuine (if intentionally simple) mobile
 * actuator — two independent drive motors — instead of a single relay/
 * PWM output.
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
 * --- Endpoint: esp-matter's own complete top-level helper, plus one extra
 * optional cluster added on afterwards ---------------------------------
 * `endpoint::robotic_vacuum_cleaner::create()` (device type 0x0074)
 * confirmed complete/ready-to-use by reading esp_matter_endpoint.cpp's own
 * `robotic_vacuum_cleaner::add()` directly: Identify + RvcRunMode +
 * RvcOperationalState (with the OperationCompletion event pre-registered
 * via `operational_state::event::create_operation_completion()`), auto-
 * Descriptor via `common::create<T>()`. Matches the CSA's own
 * data_model/1.6/device_types/RoboticVacuumCleaner.xml exactly: those two
 * clusters (plus Identify) are the only `<mandatoryConform/>` ones —
 * RVC Clean Mode and Service Area are both `<optionalConform/>`.
 * RvcCleanMode is added here anyway, via `cluster::rvc_clean_mode::create()`
 * called directly on the already-correct endpoint afterwards — same "add
 * an extra cluster onto an endpoint the top-level helper already built
 * correctly" pattern firmware/thermostat/'s BINDING output type and
 * firmware/air-quality-sensor/'s concentration-measurement clusters
 * already established — because choosing vacuum vs. mop vs. both is core
 * to what makes a modern robot vacuum useful, and the integration work is
 * identical to RvcRunMode's (see below), so it was worth the very small
 * extra scope. Service Area (per-room/per-zone cleaning, with its own
 * supported-areas list, current-area tracking, and a storage delegate) is
 * NOT implemented — a genuinely large feature needing real room/map data
 * this simple GPIO-level firmware has no way to generate, same "smallest
 * reasonable next step" scoping this repo applies to every other device
 * type's first cut (e.g. firmware/thermostat/'s no-AutoMode/no-schedules
 * scope, firmware/door-lock/'s no-PIN/no-credential scope).
 *
 * --- RvcRunMode / RvcCleanMode: both ModeBase-derived, both wired the
 * same way esp-matter's own config->delegate mechanism already handles --
 * Both are "code-driven" in the sense that they need a real app-supplied
 * `chip::app::Clusters::ModeBase::Delegate` subclass — confirmed by
 * reading `esp_matter_cluster.cpp`'s own `rvc_run_mode::create()`/
 * `rvc_clean_mode::create()`: when `config->delegate != nullptr`, both
 * call `set_delegate_and_init_callback(cluster, RvcRunModeDelegateInitCB /
 * RvcCleanModeDelegateInitCB, config->delegate)`, which only *stores* the
 * callback + pointer — the callback itself
 * (`InitModeDelegate()` in `esp_matter_delegate_callbacks.cpp`) fires
 * later, during `esp_matter::start()`'s own cluster-init pass, and is what
 * actually constructs a real `ModeBase::Instance(delegate, endpoint_id,
 * cluster_id, feature_map)` and calls its `Init()`. Unlike
 * firmware/fan/'s `FanControl::SetDefaultDelegate()` (a *separate*
 * function the app itself must remember to call after `start()` — see
 * that file's own header comment for the bug this repo hit from calling
 * it too early), the delegate pointer here is simply part of `config_t`,
 * so it's supplied once, before `endpoint::robotic_vacuum_cleaner::create()`
 * — there's no equivalent ordering trap: the store-now/attach-later split
 * is entirely internal to esp-matter and requires no timing awareness
 * from this file, confirmed by reading `InitModeDelegate()` end to end
 * (same file/function esp-matter also uses for
 * LaundryWasherMode/DishwasherMode/RefrigeratorAndTCCMode/
 * MicrowaveOvenMode — RVC's two Mode clusters are not a special case).
 * A real, ready-made helper does the heavy lifting once `HandleChangeToMode`
 * returns success — confirmed by reading `ModeBaseCluster.cpp`'s own
 * `Instance::HandleChangeToMode()`: it calls `UpdateCurrentMode(newMode)`
 * itself immediately after a successful delegate callback, so neither
 * delegate below writes `CurrentMode` directly — same "the SDK does more
 * of the work than you'd assume from the header alone" lesson
 * firmware/valve/'s ValveConfigurationAndControl already taught this repo,
 * here for a different cluster family. Each delegate's own
 * `HandleChangeToMode` still needs a way to know the *other* Mode
 * cluster's currently-selected value (RunMode needs CleanMode's choice to
 * decide which output to drive when cleaning starts; the Pause/Resume
 * logic below needs RunMode's own choice) — esp-matter constructs each
 * `ModeBase::Instance` internally and never hands this file a pointer to
 * either one, so rather than reaching into the data model provider's
 * registry for a class (`ModeBase::Instance`) that isn't registered there
 * in the first place (that registry pattern is specifically for esp-matter's
 * `data_model_provider/clusters/` "code-driven" cluster classes — a
 * different, unrelated mechanism from this ember-plus-separately-
 * constructed-Instance combination — confirmed by checking that no
 * `mode_base/` folder exists under `data_model_provider/clusters/`), this
 * file just keeps two small `static` globals
 * (`g_current_run_mode`/`g_current_clean_mode`) as its own single source
 * of truth, written by each delegate's own successful `HandleChangeToMode`
 * and read by whichever side needs the other's current selection — simple,
 * and correct since these two delegates are the only writers.
 *
 * Real mode/tag values (`RvcRunMode::ModeTag::kIdle/kCleaning/kMapping`,
 * `RvcCleanMode::ModeTag::kVacuum/kMop/kVacuumThenMop`) were confirmed
 * directly against connectedhomeip's own generated
 * `zzz_generated/app-common/clusters/RvcRunMode/Enums.h` and
 * `.../RvcCleanMode/Enums.h`, and the mode-option-list construction
 * pattern (label + mode value + mode tags, `GetModeLabelByIndex`/
 * `GetModeValueByIndex`/`GetModeTagsByIndex` all indexing the same static
 * array) is ported from connectedhomeip's own real, working reference —
 * `examples/chef/common/chef-rvc-mode-delegate.cpp` — same "port a real
 * reference rather than guess the integration shape" precedent already
 * used in this repo for SM2335EGH/APA102/OpenTherm. This file's own
 * "only allowed to enter Mapping from Idle" business rule in
 * `RvcRunModeDelegate::HandleChangeToMode` and "reject a clean-mode change
 * while actively cleaning" rule in `RvcCleanModeDelegate::HandleChangeToMode`
 * (returning `RvcCleanMode::StatusCode::kCleaningInProgress`, the one
 * cluster-specific status code `Mode_RVCClean.xml` defines) are the same
 * two rules chef's own reference delegate encodes — reused deliberately,
 * not reinvented, since they reflect real constraints (you can't sensibly
 * re-plan a mopping pass mid-mop) rather than an implementation detail.
 *
 * --- RvcOperationalState: NOT wired through config->delegate at all —
 * esp-matter's own config_t for it is a literally empty struct ------------
 * Confirmed by reading `esp_matter_cluster_impl.h` directly:
 * `rvc_operational_state`'s `config_t` is `using config_t = common::config_t`
 * — `common::config_t` is documented in-source as "Empty config for API
 * consistency" (no `delegate` field exists at all). Confirmed further by
 * reading `rvc_operational_state::create()` itself in
 * `esp_matter_cluster.cpp`: it only creates the base OperationalState
 * ember attributes (FeatureMap, PhaseList, CurrentPhase,
 * OperationalStateList, OperationalState, OperationalError) — no delegate
 * handling, no `Instance` construction, nothing. This is the same class
 * of gap firmware/valve/'s ValveConfigurationAndControl hit (no
 * `SetDefaultDelegate()`-style free function shipped for this cluster at
 * all) — but a level deeper here: valve's cluster class was still
 * registered with the data model provider's registry automatically at
 * cluster-creation time, just missing a convenience free function to
 * attach a Delegate to it; RvcOperationalState is never registered
 * *anywhere* as a working command handler at all unless this file builds
 * one itself. Fixed the same way connectedhomeip's own real reference
 * app does it (`examples/rvc-app/rvc-common/`, confirmed by reading it
 * directly): construct a real, raw connectedhomeip
 * `chip::app::Clusters::RvcOperationalState::Instance` (declared in
 * `app/clusters/operational-state-server/CodegenIntegration.h` —
 * `RvcOperationalState::Instance : public OperationalState::Instance`,
 * pre-wired with `RvcOperationalState::Id` and the two RVC-specific
 * pause/resume-compatibility overrides already implemented by
 * connectedhomeip itself, confirmed by reading `CodegenIntegration.cpp`'s
 * own `IsDerivedClusterStatePauseCompatible()`
 * [only `SeekingCharger` is pause-compatible] and
 * `IsDerivedClusterStateResumeCompatible()`
 * [only `Charging`/`Docked` are resume-compatible]) with an app-supplied
 * `RvcOperationalState::Delegate` (declared in `OperationalStateDelegate.h`
 * — a small, RVC-specific subclass of the generic `OperationalState::Delegate`
 * that already gives `HandleStartStateCallback`/`HandleStopStateCallback`
 * working dummy bodies, since Start/Stop aren't part of RVC's own command
 * set — confirmed by that header's own comment: "the Start/Stop command
 * is not supported by the RvcOperationalState cluster... so the consumer
 * of this class does not need to define it"). Unlike Pause/Resume/GoHome
 * (below), this file never overrides those two. This Instance is
 * constructed as a file-scope `static` and `.Init()`'d in `app_main()`
 * — deliberately AFTER `esp_matter::start()`, same "register real command
 * handling only once the Matter server itself is actually running"
 * discipline this repo has now hit for FanControl (firmware/fan/,
 * firmware/air-purifier/) and ValveConfigurationAndControl
 * (firmware/valve/), even though `Instance::Init()`'s own doc comment
 * ("Returns an error if the given endpoint and cluster ID have not been
 * enabled") suggests the real requirement is narrower (the ember cluster
 * shell from `rvc_operational_state::create()` must already exist, which
 * it does immediately after `endpoint::robotic_vacuum_cleaner::create()`
 * — before `start()`) — placed after `start()` anyway since that's the
 * one ordering every other Delegate-registration bug in this repo has
 * actually come from, and there is no benefit to risking it here.
 *
 * A second, real, previously-undiscovered SDK behavior worth documenting
 * carefully because it directly shapes every callback below: unlike
 * ModeBase's `Instance::HandleChangeToMode()` (which updates `CurrentMode`
 * itself after a successful delegate call — see above), confirmed by
 * reading `CodegenIntegration.cpp`'s own `Instance::HandlePauseState()`/
 * `HandleResumeState()`/`HandleGoHomeCommand()` directly: NONE of them
 * update the `OperationalState` attribute automatically after the
 * delegate's callback returns `NoError` — that is entirely this file's
 * own responsibility, via the Instance's public `SetOperationalState()`
 * method, exactly matching the real reference app's own
 * `rvc-device.cpp` (which calls `mOperationalStateInstance.SetOperationalState(...)`
 * from inside its own business-logic handlers, not from inside the SDK).
 *
 * --- Hardware scope: a real (if deliberately simple) mobile actuator,
 * with no real navigation — a documented, honest limitation --------------
 * Two independent drive motors (left/right wheel) via a dual H-bridge
 * driver module (an L298N/TB6612FNG-class board is the common, cheap
 * choice for this size of DIY robot) — two GPIOs per motor (FWD/REV,
 * mutually exclusive by construction, same pattern
 * firmware/window-covering/'s UP/DOWN relay outputs already established
 * for a single motor, here doubled for differential steering), driven at
 * a single fixed speed (no PWM enable line) — ENA/ENB tied directly to
 * the driver board's own logic supply in the reference wiring, since
 * FanControl-style variable speed has no Matter attribute to drive it
 * from on this cluster set anyway. A separate suction-motor output
 * (single GPIO, MOSFET or relay driving the vacuum motor — "always check
 * your specific module," same caveat firmware/outlet/'s own relay
 * documentation already carries) and a mop-pump output (single GPIO,
 * active-LOW relay, matching firmware/valve/'s own relay-polarity
 * convention, since a mop-water pump/solenoid is functionally the same
 * kind of load as a water valve) are switched together according to
 * whichever `RvcCleanMode` is currently selected. An optional dock-contact
 * sensor input (default `GPIO_NUM_NC`, off — same opt-in-GPIO convention
 * firmware/door-lock/'s position sensor and firmware/outlet/'s status LED
 * already use) reads a simple digital HIGH-when-docked signal, the same
 * kind of bare charging-contact-pair many hobby robot chassis kits
 * already ship with.
 *
 * Explicitly, deliberately NOT implemented: any actual navigation,
 * obstacle avoidance, or return-to-dock path-finding. "Cleaning" and
 * "Mapping" both just drive both wheels forward at a fixed speed — there
 * is no camera, LIDAR, bump sensor, or cliff sensor assumed, so there is
 * nothing for this firmware to steer with. This is the same category of
 * honest scope cut as firmware/window-covering/'s own documented
 * limitation (position is estimated from motor-on time, not measured,
 * and drifts if the motor stalls or is moved by hand) — here the gap is
 * simply larger, because full autonomous navigation is an entire
 * separate engineering discipline (SLAM, sensor fusion, path planning),
 * not a hardware-driver detail this repo's "read the datasheet, drive
 * the GPIO" style can responsibly cover. `GoHome` reflects this
 * honestly: it stops the drive motors (there is nowhere informed to
 * drive them) and reports `SeekingCharger` rather than pretending to
 * navigate; if a dock-contact sensor is wired, reaching the dock (by
 * whatever means — a real product's own homing beacon, or simply someone
 * placing the robot back on its dock by hand) is detected and reported
 * as `Charging`; without one wired, `GoHome` optimistically reports
 * `Docked` directly — the same "no feedback sensor = optimistic
 * best-effort report" precedent firmware/door-lock/'s LockState and
 * firmware/valve/'s CurrentState already establish for hardware with no
 * way to confirm its own physical result.
 *
 * The mandatory `OperationCompletion` event (already registered on the
 * cluster by `endpoint::robotic_vacuum_cleaner::add()`, see above) is
 * fired via `RvcOperationalState::Instance`'s own real
 * `OnOperationCompletionDetected()` method whenever `RvcRunMode`
 * transitions from Cleaning/Mapping back to Idle — confirmed by reading
 * that method directly that it only calls connectedhomeip's own
 * `LogEvent()`, independent of the ember-attribute-store event
 * descriptor `create_operation_completion()` sets up (that call is about
 * spec-conformance metadata, not a prerequisite for actually emitting the
 * event) — so this is a real, working event, not a placeholder.
 *
 * Reference wiring: a dual H-bridge motor driver module (2 motors), a
 * MOSFET or relay module for the vacuum motor, an active-LOW relay module
 * for the mop pump, all GND -> module -> GPIO as elsewhere in this repo.
 * Not hardware-tested (no robot chassis/motor-driver hardware physically
 * available when written) — Docker build-verified only, like several
 * other device types in this repo when written.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>

static const char *TAG = "matter_robot_vacuum";

/* --- Drive motors (differential steering) ---------------------------------
 * Dual H-bridge driver module (e.g. L298N/TB6612FNG-class), FWD/REV pair
 * per motor, mutually exclusive by construction — same pattern
 * firmware/window-covering/'s single UP/DOWN motor uses, doubled here.
 * Fixed speed only (ENA/ENB tied to the driver board's own logic supply
 * in the reference wiring) — see the header comment on hardware scope. */
#define RVC_LEFT_MOTOR_FWD_GPIO GPIO_NUM_13
#define RVC_LEFT_MOTOR_REV_GPIO GPIO_NUM_14
#define RVC_RIGHT_MOTOR_FWD_GPIO GPIO_NUM_27
#define RVC_RIGHT_MOTOR_REV_GPIO GPIO_NUM_26

/* Vacuum (suction) motor output — a MOSFET or relay module, active-HIGH.
 * "Always check your specific module" — same caveat firmware/outlet/'s
 * own relay documentation carries; polarity isn't universal. */
#define RVC_VACUUM_MOTOR_GPIO GPIO_NUM_25

/* Mop water pump/solenoid output — active-LOW relay, matching
 * firmware/valve/'s own relay-polarity convention (functionally the same
 * kind of load as a water valve). */
#define RVC_MOP_PUMP_GPIO GPIO_NUM_33

/* Optional dock-contact sensor — a simple pair of charging contacts that
 * reads HIGH when the robot is physically docked. Off by default
 * (GPIO_NUM_NC), same opt-in-GPIO convention firmware/door-lock/'s
 * position sensor uses — see the header comment on GoHome's behavior
 * with and without this wired. */
#define RVC_DOCK_CONTACT_GPIO GPIO_NUM_NC
#define RVC_DOCK_POLL_INTERVAL_MS 1000

/* LED for the Matter "Identify" cluster — blinks so you can physically
 * find this device when a controller asks it to identify itself. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — bringing
 * in the whole `chip` namespace collides `chip::detail` (from Span.h) with
 * `chip::app::Clusters::detail` (the Mode/OperationalState struct
 * namespace already opened above), an ambiguity an actual Docker build
 * caught. Everything else from `chip::`/`chip::app::` below is spelled out
 * fully qualified instead. */
using namespace chip::literals;

static uint16_t rvc_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static esp_timer_handle_t dock_poll_timer = NULL;

/* This file's own app-level mode constants — deliberately NOT reopening
 * the SDK's `RvcRunMode`/`RvcCleanMode` namespaces the way
 * connectedhomeip's own `examples/chef` reference does (see the header
 * comment on RvcRunMode/RvcCleanMode) to avoid any risk of colliding with
 * a real SDK symbol of the same name. */
static constexpr uint8_t kRunModeIdle = 0;
static constexpr uint8_t kRunModeCleaning = 1;
static constexpr uint8_t kRunModeMapping = 2;

static constexpr uint8_t kCleanModeVacuum = 0;
static constexpr uint8_t kCleanModeMop = 1;
static constexpr uint8_t kCleanModeVacuumAndMop = 2;

/* This file's own single source of truth for each Mode cluster's current
 * selection — see the header comment on RvcRunMode/RvcCleanMode for why
 * this file can't just ask esp-matter for a pointer to either
 * ModeBase::Instance. Written only by each delegate's own successful
 * HandleChangeToMode. */
static uint8_t g_current_run_mode = kRunModeIdle;
static uint8_t g_current_clean_mode = kCleanModeVacuum;

static RvcOperationalState::Instance *g_op_state_instance = NULL;

/* --- Physical outputs ------------------------------------------------- */

static void drive_stop(void)
{
    gpio_set_level(RVC_LEFT_MOTOR_FWD_GPIO, 0);
    gpio_set_level(RVC_LEFT_MOTOR_REV_GPIO, 0);
    gpio_set_level(RVC_RIGHT_MOTOR_FWD_GPIO, 0);
    gpio_set_level(RVC_RIGHT_MOTOR_REV_GPIO, 0);
}

static void drive_forward(void)
{
    gpio_set_level(RVC_LEFT_MOTOR_FWD_GPIO, 1);
    gpio_set_level(RVC_LEFT_MOTOR_REV_GPIO, 0);
    gpio_set_level(RVC_RIGHT_MOTOR_FWD_GPIO, 1);
    gpio_set_level(RVC_RIGHT_MOTOR_REV_GPIO, 0);
}

/* Drives the vacuum/mop outputs for whichever RvcCleanMode is currently
 * selected. `active` is false whenever the robot isn't actually running a
 * cleaning pass (Idle, Mapping, Paused, seeking the dock, ...). */
static void apply_clean_outputs(uint8_t clean_mode, bool active)
{
    bool vacuum_on = active && (clean_mode == kCleanModeVacuum || clean_mode == kCleanModeVacuumAndMop);
    bool mop_on = active && (clean_mode == kCleanModeMop || clean_mode == kCleanModeVacuumAndMop);
    gpio_set_level(RVC_VACUUM_MOTOR_GPIO, vacuum_on ? 1 : 0);
    gpio_set_level(RVC_MOP_PUMP_GPIO, mop_on ? 0 : 1); /* active-LOW, see the #define comment above */
}

static bool dock_contact_enabled(void)
{
    return RVC_DOCK_CONTACT_GPIO != GPIO_NUM_NC;
}

static bool dock_contact_asserted(void)
{
    return dock_contact_enabled() && gpio_get_level(RVC_DOCK_CONTACT_GPIO) == 1;
}

/* --- RvcRunMode delegate ------------------------------------------------
 * See the header comment above for why the mode-option-list shape here is
 * ported from connectedhomeip's own examples/chef reference rather than
 * invented, and for the "the SDK updates CurrentMode itself after a
 * successful HandleChangeToMode" behavior this delegate relies on. */
class RvcRunModeDelegate : public ModeBase::Delegate
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

    /* "Only allowed to enter Mapping from Idle" is chef's own reference
     * business rule, reused deliberately — see the header comment above. */
    void HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        uint8_t oldMode = g_current_run_mode;

        if (newMode == kRunModeMapping && oldMode != kRunModeIdle) {
            response.status = chip::to_underlying(ModeBase::StatusCode::kGenericFailure);
            response.statusText.SetValue("Mapping is only allowed from Idle"_span);
            return;
        }

        g_current_run_mode = newMode;

        switch (newMode) {
        case kRunModeIdle:
            drive_stop();
            apply_clean_outputs(g_current_clean_mode, false);
            if (g_op_state_instance != NULL) {
                uint8_t opState = g_op_state_instance->GetCurrentOperationalState();
                bool wasCleaning = (oldMode == kRunModeCleaning || oldMode == kRunModeMapping);
                /* Leave Charging/Docked alone — going Idle while docked
                 * shouldn't override that with a plain Stopped. */
                if (opState != chip::to_underlying(RvcOperationalState::OperationalStateEnum::kCharging) &&
                    opState != chip::to_underlying(RvcOperationalState::OperationalStateEnum::kDocked)) {
                    (void)g_op_state_instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
                }
                /* Mandatory OperationCompletion event — see the header
                 * comment above on why this is a real, working event. */
                if (wasCleaning) {
                    g_op_state_instance->OnOperationCompletionDetected(
                        chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
                }
            }
            ESP_LOGI(TAG, "RunMode: Idle");
            break;
        case kRunModeCleaning:
            drive_forward();
            apply_clean_outputs(g_current_clean_mode, true);
            if (g_op_state_instance != NULL) {
                (void)g_op_state_instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
            }
            ESP_LOGI(TAG, "RunMode: Cleaning (clean mode %u)", g_current_clean_mode);
            break;
        case kRunModeMapping:
            /* No real mapping capability — see the header comment on
             * hardware scope. Drives forward with cleaning outputs off. */
            drive_forward();
            apply_clean_outputs(g_current_clean_mode, false);
            if (g_op_state_instance != NULL) {
                (void)g_op_state_instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
            }
            ESP_LOGI(TAG, "RunMode: Mapping (no real navigation — see header comment)");
            break;
        }

        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsIdle[1] = {{.value = chip::to_underlying(RvcRunMode::ModeTag::kIdle)}};
    ModeTagType tagsCleaning[1] = {{.value = chip::to_underlying(RvcRunMode::ModeTag::kCleaning)}};
    ModeTagType tagsMapping[1] = {{.value = chip::to_underlying(RvcRunMode::ModeTag::kMapping)}};

    static constexpr size_t kNumModes = 3;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Idle"_span, .mode = kRunModeIdle, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsIdle)},
        {.label = "Cleaning"_span, .mode = kRunModeCleaning, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsCleaning)},
        {.label = "Mapping"_span, .mode = kRunModeMapping, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsMapping)},
    };
};

static RvcRunModeDelegate rvc_run_mode_delegate;

/* --- RvcCleanMode delegate ----------------------------------------------
 * Structurally identical to RvcRunModeDelegate above — see its own
 * comments for the shared reasoning. */
class RvcCleanModeDelegate : public ModeBase::Delegate
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

    /* "Reject a clean-mode change while actively cleaning" is chef's own
     * reference business rule (RvcCleanMode's one cluster-specific status
     * code exists for exactly this) — reused deliberately, see the header
     * comment above. */
    void HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        if (g_current_run_mode == kRunModeCleaning) {
            response.status = chip::to_underlying(RvcCleanMode::StatusCode::kCleaningInProgress);
            response.statusText.SetValue("Cannot change the cleaning mode during a clean"_span);
            return;
        }

        g_current_clean_mode = newMode;
        ESP_LOGI(TAG, "CleanMode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsVacuum[1] = {{.value = chip::to_underlying(RvcCleanMode::ModeTag::kVacuum)}};
    ModeTagType tagsMop[1] = {{.value = chip::to_underlying(RvcCleanMode::ModeTag::kMop)}};
    ModeTagType tagsBoth[1] = {{.value = chip::to_underlying(RvcCleanMode::ModeTag::kVacuumThenMop)}};

    static constexpr size_t kNumModes = 3;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Vacuum"_span, .mode = kCleanModeVacuum, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsVacuum)},
        {.label = "Mop"_span, .mode = kCleanModeMop, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsMop)},
        {.label = "Vacuum & Mop"_span, .mode = kCleanModeVacuumAndMop, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsBoth)},
    };
};

static RvcCleanModeDelegate rvc_clean_mode_delegate;

/* --- RvcOperationalState delegate ----------------------------------------
 * See the header comment above for why this cluster needs a whole
 * separately-constructed Instance rather than esp-matter's usual
 * config->delegate path, and why none of the three callbacks below can
 * rely on the SDK to update OperationalState for them. */
class RvcOperationalStateDelegate : public RvcOperationalState::Delegate
{
public:
    /* Not tracked — matches connectedhomeip's own real reference app,
     * whose identical override carries the comment "This attribute is
     * not supported in our example RVC app." */
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override { return {}; }

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, OperationalState::GenericOperationalState &state) override
    {
        if (index >= kNumStates) {
            return CHIP_ERROR_NOT_FOUND;
        }
        state = kStates[index];
        return CHIP_NO_ERROR;
    }

    /* No phases implemented — returning NOT_FOUND for index 0 tells the
     * cluster the PhaseList attribute is null, a spec-allowed state. */
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &phase) override
    {
        (void)index;
        (void)phase;
        return CHIP_ERROR_NOT_FOUND;
    }

    /* Only reachable from Running (base cluster) or SeekingCharger
     * (RvcOperationalState::Instance's own pause-compatibility override —
     * see the header comment above). */
    void HandlePauseStateCallback(OperationalState::GenericOperationalError &err) override
    {
        drive_stop();
        apply_clean_outputs(g_current_clean_mode, false);
        if (g_op_state_instance != NULL) {
            (void)g_op_state_instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kPaused));
        }
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
        ESP_LOGI(TAG, "OperationalState: Paused");
    }

    /* Only reachable from Paused (base cluster) or Charging/Docked
     * (RvcOperationalState::Instance's own resume-compatibility
     * override) — resumes whatever RunMode was last selected. */
    void HandleResumeStateCallback(OperationalState::GenericOperationalError &err) override
    {
        bool wasCleaning = (g_current_run_mode == kRunModeCleaning || g_current_run_mode == kRunModeMapping);
        if (wasCleaning) {
            drive_forward();
            apply_clean_outputs(g_current_clean_mode, g_current_run_mode == kRunModeCleaning);
        }
        if (g_op_state_instance != NULL) {
            (void)g_op_state_instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
        }
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
        ESP_LOGI(TAG, "OperationalState: Resumed (run mode %u)", g_current_run_mode);
    }

    /* No real navigation — see the header comment on hardware scope for
     * why this stops the motors and reports SeekingCharger rather than
     * pretending to path-find, and how the optional dock-contact sensor
     * (or its absence) decides what happens next. */
    void HandleGoHomeCommandCallback(OperationalState::GenericOperationalError &err) override
    {
        drive_stop();
        apply_clean_outputs(g_current_clean_mode, false);
        g_current_run_mode = kRunModeIdle;

        if (g_op_state_instance != NULL) {
            if (dock_contact_enabled()) {
                if (dock_contact_asserted()) {
                    /* Already sitting on the dock. */
                    (void)g_op_state_instance->SetOperationalState(
                        chip::to_underlying(RvcOperationalState::OperationalStateEnum::kCharging));
                    ESP_LOGI(TAG, "OperationalState: Charging (already docked)");
                } else {
                    (void)g_op_state_instance->SetOperationalState(
                        chip::to_underlying(RvcOperationalState::OperationalStateEnum::kSeekingCharger));
                    ESP_LOGI(TAG, "OperationalState: SeekingCharger (no real navigation — waiting for dock contact)");
                }
            } else {
                /* No dock sensor wired — optimistic best-effort report,
                 * same precedent as firmware/door-lock/'s LockState. */
                (void)g_op_state_instance->SetOperationalState(chip::to_underlying(RvcOperationalState::OperationalStateEnum::kDocked));
                ESP_LOGI(TAG, "OperationalState: Docked (optimistic — no dock sensor wired)");
            }
        }

        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

private:
    static constexpr size_t kNumStates = 7;
    const OperationalState::GenericOperationalState kStates[kNumStates] = {
        OperationalState::GenericOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped)),
        OperationalState::GenericOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)),
        OperationalState::GenericOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kPaused)),
        OperationalState::GenericOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kError)),
        OperationalState::GenericOperationalState(chip::to_underlying(RvcOperationalState::OperationalStateEnum::kSeekingCharger)),
        OperationalState::GenericOperationalState(chip::to_underlying(RvcOperationalState::OperationalStateEnum::kCharging)),
        OperationalState::GenericOperationalState(chip::to_underlying(RvcOperationalState::OperationalStateEnum::kDocked)),
    };
};

static RvcOperationalStateDelegate rvc_op_state_delegate;

/* Polls the optional dock-contact sensor while seeking the charger — see
 * the header comment on GoHome's behavior. Only created/started if
 * RVC_DOCK_CONTACT_GPIO is actually wired. */
static void dock_poll_timer_cb(void *arg)
{
    if (g_op_state_instance == NULL) {
        return;
    }
    uint8_t opState = g_op_state_instance->GetCurrentOperationalState();
    if (opState == chip::to_underlying(RvcOperationalState::OperationalStateEnum::kSeekingCharger) && dock_contact_asserted()) {
        (void)g_op_state_instance->SetOperationalState(chip::to_underlying(RvcOperationalState::OperationalStateEnum::kCharging));
        ESP_LOGI(TAG, "OperationalState: Charging (dock contact made)");
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

/* RunMode/CleanMode/OperationalState commands are entirely handled through
 * the three Delegate classes above (not the generic attribute::PRE_UPDATE
 * path) — so this is a no-op required by node::create()'s callback
 * signature, same as firmware/fan/'s and firmware/valve/'s own. */
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

    /* 2. Configure the drive motor + vacuum + mop outputs — boots stopped
     * (de-energized), same "boot to known safe state" convention every
     * other device type here follows. */
    gpio_config_t output_io_conf = {};
    output_io_conf.pin_bit_mask = (1ULL << RVC_LEFT_MOTOR_FWD_GPIO) | (1ULL << RVC_LEFT_MOTOR_REV_GPIO) |
                                  (1ULL << RVC_RIGHT_MOTOR_FWD_GPIO) | (1ULL << RVC_RIGHT_MOTOR_REV_GPIO) |
                                  (1ULL << RVC_VACUUM_MOTOR_GPIO) | (1ULL << RVC_MOP_PUMP_GPIO);
    output_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&output_io_conf);
    drive_stop();
    apply_clean_outputs(g_current_clean_mode, false);

    /* 2b. Optional dock-contact sensor input. */
    if (dock_contact_enabled()) {
        gpio_config_t dock_io_conf = {};
        dock_io_conf.pin_bit_mask = (1ULL << RVC_DOCK_CONTACT_GPIO);
        dock_io_conf.mode = GPIO_MODE_INPUT;
        dock_io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        gpio_config(&dock_io_conf);

        const esp_timer_create_args_t dock_poll_timer_args = {
            .callback = &dock_poll_timer_cb,
            .name = "dock_poll",
        };
        esp_timer_create(&dock_poll_timer_args, &dock_poll_timer);
        esp_timer_start_periodic(dock_poll_timer, RVC_DOCK_POLL_INTERVAL_MS * 1000);
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

    /* 3. Build the Matter data model: one node, one Robotic Vacuum Cleaner
     * endpoint (Identify + RvcRunMode + RvcOperationalState), plus
     * RvcCleanMode added onto the same endpoint afterwards — see the
     * header comment above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::robotic_vacuum_cleaner::config_t rvc_config;
    rvc_config.rvc_run_mode.delegate = &rvc_run_mode_delegate;
    endpoint_t *endpoint = endpoint::robotic_vacuum_cleaner::create(node, &rvc_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create robotic vacuum cleaner endpoint");
        return;
    }

    rvc_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Robotic vacuum cleaner endpoint id: %u", rvc_endpoint_id);

    cluster::rvc_clean_mode::config_t clean_mode_config;
    clean_mode_config.delegate = &rvc_clean_mode_delegate;
    cluster::rvc_clean_mode::create(endpoint, &clean_mode_config, CLUSTER_FLAG_SERVER);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* Construct and Init() the RvcOperationalState::Instance — MUST happen
     * after esp_matter::start(), same discipline this repo now follows for
     * every Delegate-registration call after finding the FanControl
     * ordering bug (see the header comment above). Deliberately never
     * freed/reset — this device has exactly one endpoint for its whole
     * lifetime. */
    static RvcOperationalState::Instance op_state_instance(&rvc_op_state_delegate, rvc_endpoint_id);
    if (op_state_instance.Init() != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to init RvcOperationalState instance");
        return;
    }
    g_op_state_instance = &op_state_instance;
    (void)g_op_state_instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter robotic vacuum cleaner started. Scan the QR code to commission.");
}
