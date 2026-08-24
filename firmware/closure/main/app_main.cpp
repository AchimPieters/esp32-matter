/*
 * Minimal Matter Closure (garage door / roller shutter / awning) —
 * forty-first device type, and this repo's first over the Closure Control
 * cluster — a brand-new (Matter 1.6) cluster family, not yet used anywhere
 * else in this repo.
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
 * Closure.xml (device type 0x0230): Identify + Closure Control are the only
 * two clusters, both mandatoryConform — Window Covering and Closure
 * Dimension are both explicitly `<disallowConform/>` on this device type
 * (they belong to the separate ClosurePanel child-endpoint device type,
 * 0x0231, for closures with more than one independently-controlled panel —
 * not implemented here; this file is a single-panel closure, the common
 * case for a garage door/roller shutter/awning). `endpoint::closure::
 * create()` confirmed complete/ready-to-use by reading esp-matter's own
 * legacy `closure::add()` directly (Identify + ClosureControl, auto-
 * Descriptor via `common::create<T>()`).
 *
 * ClosureControl's own cluster XML (fetched from inside the esp-matter SDK
 * image, not assumed) defines nine optional features. Positioning (PS) and
 * MotionLatching (LT) form a real "at least one of these two" choice group
 * — confirmed both in the XML's own `optionalConform choice="a" more="true"
 * min="1"` markers AND in esp-matter's own `closure_control::create()`,
 * which calls `VALIDATE_FEATURES_AT_LEAST_ONE("Positioning,MotionLatching",
 * ...)` — the same "choice, at least 1" constraint class firmware/
 * occupancy-sensor/'s own OccupancySensing already established in this
 * repo. This file enables Positioning only — same "smallest reasonable
 * next step" scoping firmware/window-covering/'s own Lift-only choice
 * already applies: MotionLatching/Speed/Ventilation/Pedestrian/
 * Calibration/Protection/ManuallyOperable/Instantaneous are all left off.
 * A real, physical garage door/roller shutter with a simple two-relay
 * (open/close) motor and no position sensor maps directly onto Positioning
 * alone — open, closed, or somewhere in between, exactly the three
 * CurrentPositionEnum values this file actually uses (FullyClosed/
 * FullyOpened/PartiallyOpened; OpenedForPedestrian/OpenedForVentilation
 * need PD/VT, not enabled; OpenedAtSignature is a spec-mandatory enum value
 * with no feature gate at all, but there's no real "signature position"
 * concept on this hardware, so it's simply never used).
 *
 * --- A genuinely new "delegate must be registered BEFORE start()" pattern,
 * the opposite ordering of every other Delegate-based cluster in this repo
 * so far ------------------------------------------------------------------
 * Every other Delegate-based cluster this repo has built (FanControl in
 * firmware/fan/ and firmware/air-purifier/, ValveConfigurationAndControl in
 * firmware/valve/) needs its delegate registered AFTER `esp_matter::
 * start()` — their own cluster instance tolerates a null/absent delegate
 * at construction time and the app supplies the real one afterwards via
 * `SetDefaultDelegate()`/`SetDelegate()`. ClosureControl is the opposite,
 * confirmed by reading BOTH esp-matter's own
 * `data_model_provider/clusters/closure_control/integration.cpp` AND
 * connectedhomeip's own `ClosureControlCluster::Config` constructor
 * directly (not assumed from the header alone, the same discipline this
 * repo's other Delegate-cluster entries already apply): `Config`'s own
 * constructor takes `ClosureControlClusterDelegate & delegate` as a
 * mandatory REFERENCE, not an optional pointer supplied later — the
 * cluster object simply cannot be constructed without one. esp-matter's
 * own `ESPMatterClosureControlClusterServerInitCallback` (registered as
 * this cluster's plain `init_callback`, which esp-matter's own
 * `invoke_init_callbacks_internal()` — confirmed by reading it directly —
 * runs for every cluster on every startup endpoint as part of
 * `esp_matter::start()`, BEFORE that same per-cluster pass's own
 * `delegate_init_callback` step that `config->closure_control.delegate`
 * would otherwise wire up automatically) errors out and refuses to
 * construct the cluster instance at all if no delegate has been registered
 * for that endpoint yet — confirmed directly in its own source, which
 * literally logs "delegate not set for ep %u (call
 * MatterClosureControlSetDelegate first)" and returns. Passing a delegate
 * via `config->closure_control.delegate` therefore does NOT work for this
 * cluster specifically — by the time esp-matter's own automatic wiring
 * would run, the cluster construction step that needed it has already
 * failed. This file works around it the way esp-matter's own source
 * suggests it wants to be worked around: `config->closure_control.delegate`
 * is left null, and `chip::app::Clusters::ClosureControl::
 * MatterClosureControlSetDelegate(endpoint_id, delegate)` (declared in
 * `data_model_provider/clusters/closure_control/integration.h`) is called
 * explicitly, BEFORE the `esp_matter::start()` call in `app_main()` — the
 * opposite placement from firmware/valve/'s and firmware/fan/'s own
 * delegate-registration calls, and worth remembering as an eighth,
 * genuinely distinct "how do I wire up a code-driven cluster's real
 * implementation from app code" pattern in this repo now (after: (1) plain
 * registry-lookup setter, (2) a Delegate whose own reporting call happens
 * to be a working generic free-function proxy, (3) the
 * `chip::app::…registry().Get()`-based fallback for `DefaultServerCluster`-
 * derived clusters, (4) a cluster-family-specific convenience free
 * function, (5) a direct FeatureMap `attribute::update()` override, (6)
 * `get_delegate_managed_instance()` for a legacy-ember-style cluster with a
 * delegate-managed live C++ instance, (7) a `SetDefaultDelegate()`/
 * `SetDelegate()`-style free function/method called AFTER `start()`, and
 * now (8) a dedicated `MatterXxxSetDelegate()` free function called BEFORE
 * `start()`, because the cluster's own constructor requires the delegate
 * as a mandatory reference rather than an optional pointer supplied
 * afterward). This is sourced entirely from reading esp-matter's and
 * connectedhomeip's own code directly — there is no real hardware to
 * confirm this ordering against yet (see the build-verification note at
 * the bottom of this comment), so treat this specific finding with the
 * same caution this repo already applies to any runtime-ordering
 * conclusion that hasn't also been hardware-confirmed (compare firmware/
 * fan/'s own `SetDefaultDelegate()`-before-`start()` bug, which WAS
 * eventually root-caused this same way, by reading source rather than
 * guessing, before any hardware was available to confirm it either).
 *
 * The cluster's own C++ object (the live `ClosureControlCluster` instance)
 * doesn't exist until that same `esp_matter::start()` call actually runs
 * the init pass above — so, symmetrically, `SetOverallCurrentState()`'s
 * very first call (seeding a real initial state — see below) has to happen
 * AFTER `esp_matter::start()` returns, reached via the same registry-
 * lookup-and-cast pattern (`chip::app::…registry().Get()` +
 * `static_cast<ClosureControlCluster*>`) firmware/valve/'s own
 * `get_valve_cluster()` already establishes, since ClosureControl has no
 * `GetClusterInstance()`-style convenience free function the way firmware/
 * air-purifier/'s ResourceMonitoring does.
 *
 * --- What the cluster does automatically vs. what this file does --------
 * Confirmed by reading `ClosureControlCluster.cpp`'s own `HandleMoveTo()`/
 * `HandleStop()`/`HandleCalibrate()`/`SetMainState()` directly (not assumed
 * from the header): the cluster validates commands against the current
 * MainState and FeatureMap conformance, calls the delegate's own
 * `HandleXxxCommand()`, and — ONLY if that returns Success — sets MainState
 * (Moving/WaitingForMotion/Stopped/Calibrating as appropriate) and
 * OverallTargetState itself; `SetMainState()` also automatically pulls
 * `GetMovingCountdownTime()`/`GetCalibrationCountdownTime()`/
 * `GetWaitingForMotionCountdownTime()` from the delegate and republishes
 * CountdownTime, and generates the mandatory EngageStateChanged event on
 * any transition to/from Disengaged (never reached here — ManuallyOperable
 * isn't enabled). So `HandleMoveToCommand()` below only needs to start the
 * motor and record the target — MainState/OverallTargetState/CountdownTime
 * bookkeeping all happen automatically, immediately after, inside the
 * cluster itself. What the cluster does NOT do automatically — same "the
 * app should trigger the state change" responsibility firmware/door-lock/'s
 * LockState, firmware/valve/'s CurrentState, and firmware/refrigerator/'s
 * cabinet control loops already establish — is anything about the
 * *ongoing* physical movement: reporting live OverallCurrentState/
 * SecureState as the motor actually travels, deciding when travel is
 * complete, transitioning MainState back to Stopped once it is, and firing
 * the mandatory MovementCompleted/SecureStateChanged events. All of that is
 * this file's own `closure_task`, built on the same time-based position-
 * estimation technique (no position sensor assumed) firmware/
 * window-covering/'s own `movement_task` already established — see that
 * file's header comment for the general technique; the difference here is
 * that ClosureControl only exposes three coarse discrete position states
 * (FullyClosed/FullyOpened/PartiallyOpened), not a continuous percentage
 * attribute the way WindowCovering's own CurrentPositionLiftPercent100ths
 * does, so this file estimates a continuous 0-100 "percent open" value
 * purely internally (for arrival detection and the CountdownTime
 * countdown) and only ever reports the three coarse enum states a
 * controller can actually see.
 *
 * SecureState is this file's own reasonable, documented interpretation —
 * ClosureControl is new enough (Matter 1.6) that no equivalent to e.g.
 * Espressif's own `MatterWaterLeakDetector` Arduino-ESP32 class exists yet
 * to check a "true" direction against (the way firmware/water-leak-
 * detector/'s and firmware/rain-sensor/'s own StateValue directions were
 * confirmed): true (securing against unauthorized entry) exactly when the
 * closure is FullyClosed, false otherwise — the natural reading of the
 * spec's own field description for a garage door/roller shutter, not a
 * confirmed universal convention.
 *
 * Boots assumed FullyClosed (0% open) — the safer default for something
 * whose whole purpose is "closure", matching firmware/valve/'s own boot-
 * closed convention, and, like firmware/window-covering/'s own boot-open
 * assumption, an ASSUMPTION rather than a measurement until the first real
 * movement — see the header comment on time-based position tracking there
 * for the same caveat applied here.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no garage-door/roller-shutter motor+relay hardware for
 * this device type physically available when written) — including the
 * delegate-registration-ordering finding documented above, which is
 * sourced from reading esp-matter's/connectedhomeip's own code directly,
 * not confirmed against real runtime behavior yet.
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
#include <app/clusters/closure-control-server/ClosureControlCluster.h>
#include <app/clusters/closure-control-server/ClosureControlClusterDelegate.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <data_model_provider/clusters/closure_control/integration.h>

static const char *TAG = "matter_closure";

/* Change these to the GPIOs your two relay channels are wired to — one
 * drives the motor "open" direction, the other "close". Never both at once
 * (see set_motor() below). Same GPIO 4/5 defaults firmware/window-covering/
 * already uses for its own two-relay motor output — same hardware shape,
 * different device type. Adjust to match your board. */
#define CLOSURE_OPEN_GPIO GPIO_NUM_4
#define CLOSURE_CLOSE_GPIO GPIO_NUM_5

/* Active-LOW (relay energizes on GPIO LOW) — common for low-cost
 * opto-isolated relay modules, matches firmware/window-covering/'s and
 * firmware/outlet/'s own default. Always double-check your specific relay
 * module's own documentation. */
#define CLOSURE_OUTPUT_ACTIVE_LOW 1

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the closure's own position. GPIO 2 is common on
 * classic ESP32 (WROOM-32) devkits. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

/* --- Calibration: how long a full open-to-closed (or closed-to-open)
 * traverse actually takes on your hardware. There is no position sensor —
 * see the header comment above — so this single number is what all
 * position estimation and the reported CountdownTime are built on. Time it
 * with a stopwatch against your real motor and adjust; the shipped default
 * (15 seconds — a typical sectional garage door opener's own travel time)
 * is a placeholder, not a measurement of anything. */
#define CLOSURE_FULL_TRAVEL_MS 15000

/* Extra run time added past the calibrated duration so the motor reliably
 * reaches its actual physical end stop instead of stopping just short of
 * it — same overshoot-allowance technique and caveats firmware/
 * window-covering/'s own WINDOW_COVERING_ENDSTOP_OVERSHOOT_MS already
 * documents (harmless on motors with their own end-of-travel cutout; don't
 * set this so high on motors without one that it could strain the
 * mechanism). */
#define CLOSURE_ENDSTOP_OVERSHOOT_MS 1500

/* How often the movement task re-checks progress and (while moving)
 * republishes the CountdownTime attribute. ClosureControl's CountdownTime
 * is explicitly `quieterReporting` — a coarse once-a-second tick is the
 * appropriate granularity for a value the spec itself defines in whole
 * seconds (`elapsed-s`), unlike WindowCovering's own continuous
 * Percent100ths position, which firmware/window-covering/ updates every
 * 200ms for a smooth live percentage. */
#define CLOSURE_TASK_INTERVAL_MS 1000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::ClosureControl;

static uint16_t closure_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Motor output -------------------------------------------------------
 * Mutually exclusive by construction: direction is one of three states,
 * never "both relays on" — same convention firmware/window-covering/'s own
 * set_motor() already establishes. */
enum class motor_direction_t { STOPPED, OPENING, CLOSING };
static motor_direction_t current_direction = motor_direction_t::STOPPED;

static void set_motor(motor_direction_t direction)
{
    current_direction = direction;
#if CLOSURE_OUTPUT_ACTIVE_LOW
    gpio_set_level(CLOSURE_OPEN_GPIO, direction == motor_direction_t::OPENING ? 0 : 1);
    gpio_set_level(CLOSURE_CLOSE_GPIO, direction == motor_direction_t::CLOSING ? 0 : 1);
#else
    gpio_set_level(CLOSURE_OPEN_GPIO, direction == motor_direction_t::OPENING ? 1 : 0);
    gpio_set_level(CLOSURE_CLOSE_GPIO, direction == motor_direction_t::CLOSING ? 1 : 0);
#endif
}

/* Registry-lookup-and-cast — same pattern firmware/valve/'s own
 * get_valve_cluster() already establishes, for the same reason
 * (ClosureControl has no GetClusterInstance()-style convenience free
 * function the way firmware/air-purifier/'s ResourceMonitoring does). Used
 * both to seed the initial OverallCurrentState (in app_main(), after
 * start()) and to report live state from closure_task below. */
static ClosureControlCluster *get_closure_cluster(void)
{
    chip::app::ConcreteClusterPath path(closure_endpoint_id, ClosureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "ClosureControl cluster not found on endpoint %u", closure_endpoint_id);
        return nullptr;
    }
    return static_cast<ClosureControlCluster *>(iface);
}

/* --- Movement state, shared between the delegate (which starts/stops
 * movement in response to Matter commands) and closure_task (which
 * actually drives the motor, estimates position over time, and reports it
 * back through the cluster's own public setters). Percent is 0 = fully
 * closed, 100 = fully open — purely an internal estimation unit; only the
 * three coarse CurrentPositionEnum states derived from it are ever
 * actually reported to a controller (see position_to_enum() below). */
static bool movement_active = false;
static int64_t movement_start_time_us = 0;
static int movement_start_percent = 0;
static int movement_target_percent = 0;
static bool movement_is_endstop_target = false; /* true if target is 0 or 100 */

/* Current estimated position, valid at all times (not just while moving) —
 * updated by closure_task and read by GetMovingCountdownTime() below. */
static int current_percent_open = 0; /* boots assumed fully closed — see header comment */
static bool last_reported_secure = true; /* matches the fully-closed boot assumption */

static CurrentPositionEnum position_to_enum(int percent_open)
{
    if (percent_open <= 0) {
        return CurrentPositionEnum::kFullyClosed;
    }
    if (percent_open >= 100) {
        return CurrentPositionEnum::kFullyOpened;
    }
    return CurrentPositionEnum::kPartiallyOpened;
}

/* Reports the current estimated position (as one of the three coarse
 * states) + SecureState through the cluster's own public
 * SetOverallCurrentState() — see the header comment on SecureState's
 * interpretation. Also fires the mandatory SecureStateChanged event
 * whenever the securing state actually changes, matching the same
 * "generate the event exactly when the underlying value transitions"
 * discipline this repo's other event-generating clusters already follow. */
static void report_current_state(void)
{
    ClosureControlCluster *cluster = get_closure_cluster();
    if (!cluster) {
        return;
    }

    bool secure = (current_percent_open <= 0);
    GenericOverallCurrentState state(
        chip::Optional<chip::app::DataModel::Nullable<CurrentPositionEnum>>(
            chip::app::DataModel::MakeNullable(position_to_enum(current_percent_open))),
        chip::NullOptional, /* no Latch — MotionLatching not enabled */
        chip::NullOptional, /* no Speed — Speed feature not enabled */
        chip::app::DataModel::MakeNullable(secure));
    cluster->SetOverallCurrentState(chip::app::DataModel::MakeNullable(state));

    if (secure != last_reported_secure) {
        cluster->GenerateSecureStateChangedEvent(secure);
        last_reported_secure = secure;
    }
}

/* How many whole seconds of travel remain for the movement currently in
 * progress (or about to start — see the header comment on
 * GetMovingCountdownTime() below for why this is called right as a
 * movement begins, with elapsed time still ~0). Shared by
 * GetMovingCountdownTime() and closure_task's own periodic tick. */
static uint32_t estimate_remaining_seconds(void)
{
    int64_t elapsed_ms = (esp_timer_get_time() - movement_start_time_us) / 1000;
    int64_t travel_budget_ms = CLOSURE_FULL_TRAVEL_MS;
    if (movement_is_endstop_target) {
        travel_budget_ms += CLOSURE_ENDSTOP_OVERSHOOT_MS;
    }
    int64_t remaining_ms = travel_budget_ms - elapsed_ms;
    if (remaining_ms < 0) {
        remaining_ms = 0;
    }
    return (uint32_t)(remaining_ms / 1000);
}

/* Stops the motor immediately and reports the closure idle at whatever
 * position it was last estimated at — called both when a target is
 * reached naturally (by closure_task) and when the delegate's own
 * HandleStopCommand() is invoked for an explicit Stop command. Safe to
 * call in both cases: ClosureControlCluster::SetMainState() is itself
 * idempotent (a no-op if the state isn't actually changing — confirmed by
 * reading it directly), so the redundant SetMainState(Stopped) the cluster
 * makes on its own right after HandleStop() returns Success doesn't cause
 * any double-transition. */
static void stop_movement(void)
{
    bool was_active = movement_active;
    set_motor(motor_direction_t::STOPPED);
    movement_active = false;

    ClosureControlCluster *cluster = get_closure_cluster();
    if (!cluster) {
        return;
    }
    cluster->SetMainState(MainStateEnum::kStopped);
    cluster->SetCountdownTimeFromDelegate(chip::app::DataModel::MakeNullable((chip::ElapsedS)0));
    if (was_active) {
        cluster->GenerateMovementCompletedEvent();
    }
}

/* Single shared task (same "one task handles the whole feature" pattern
 * every button/motor task elsewhere in this repo uses) — wakes once a
 * second, and while a movement is active: estimates the new position from
 * elapsed time, reports it once it's crossed into a new coarse state (or
 * on arrival), republishes the CountdownTime tick, and stops the motor
 * once the target (plus the end-stop overshoot allowance) is reached. */
static void closure_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(CLOSURE_TASK_INTERVAL_MS));
        if (!movement_active) {
            continue;
        }

        int64_t elapsed_ms = (esp_timer_get_time() - movement_start_time_us) / 1000;
        int64_t travel_budget_ms = CLOSURE_FULL_TRAVEL_MS;
        if (movement_is_endstop_target) {
            travel_budget_ms += CLOSURE_ENDSTOP_OVERSHOOT_MS;
        }

        bool opening = movement_target_percent > movement_start_percent;
        int total_delta = movement_target_percent - movement_start_percent;
        int estimated_delta = (int)((int64_t)total_delta * elapsed_ms / CLOSURE_FULL_TRAVEL_MS);
        int estimated_percent = movement_start_percent + estimated_delta;

        bool reached_target = opening ? (estimated_percent >= movement_target_percent)
                                      : (estimated_percent <= movement_target_percent);
        bool overshoot_time_elapsed = elapsed_ms >= travel_budget_ms;

        if (reached_target && !movement_is_endstop_target) {
            current_percent_open = movement_target_percent;
            report_current_state();
            stop_movement();
        } else if (overshoot_time_elapsed) {
            /* Either an endstop target's overshoot window elapsed, or (as a
             * safety net) elapsed time ran past the full calibrated
             * traverse without "reaching" the target mathematically —
             * clamp to target and stop rather than let the motor run
             * forever on a miscalibrated CLOSURE_FULL_TRAVEL_MS. */
            current_percent_open = movement_target_percent;
            report_current_state();
            stop_movement();
        } else {
            int clamped = estimated_percent < 0 ? 0 : (estimated_percent > 100 ? 100 : estimated_percent);
            if (position_to_enum(clamped) != position_to_enum(current_percent_open)) {
                current_percent_open = clamped;
                report_current_state();
            } else {
                current_percent_open = clamped;
            }
            ClosureControlCluster *cluster = get_closure_cluster();
            if (cluster) {
                cluster->SetCountdownTimeFromDelegate(
                    chip::app::DataModel::MakeNullable((chip::ElapsedS)estimate_remaining_seconds()));
            }
        }
    }
}

/* Starts a movement toward a fully-open or fully-closed target — called
 * from the delegate's own HandleMoveToCommand() below, before the cluster
 * itself sets MainState=Moving and OverallTargetState (see the header
 * comment on what the cluster does automatically). */
static void start_movement(bool closing)
{
    movement_start_percent = current_percent_open;
    movement_target_percent = closing ? 0 : 100;
    movement_is_endstop_target = true; /* both of this file's targets are hard endstops */
    movement_start_time_us = esp_timer_get_time();
    movement_active = true;
    set_motor(closing ? motor_direction_t::CLOSING : motor_direction_t::OPENING);
    ESP_LOGI(TAG, "Moving %s: %d%% -> %d%% open", closing ? "closed" : "open",
             movement_start_percent, movement_target_percent);
}

/* ClosureControl's real Delegate — see the header comment above for the
 * full detail on registration timing and on how much of the command-
 * validation/state-transition bookkeeping the cluster itself already
 * handles. */
class ClosureDelegate : public ClosureControlClusterDelegate {
public:
    chip::Protocols::InteractionModel::Status HandleStopCommand() override
    {
        ESP_LOGI(TAG, "Stop requested");
        stop_movement();
        return chip::Protocols::InteractionModel::Status::Success;
    }

    chip::Protocols::InteractionModel::Status HandleMoveToCommand(
        const chip::Optional<TargetPositionEnum> &position, const chip::Optional<bool> &latch,
        const chip::Optional<chip::app::Clusters::Globals::ThreeLevelAutoEnum> &speed) override
    {
        /* Latch/Speed are never populated here — neither MotionLatching nor
         * Speed is enabled (see the header comment on feature scope), so a
         * controller has no way to set either field in the first place;
         * both parameters exist only to satisfy the Delegate's own fixed
         * signature. */
        (void)latch;
        (void)speed;

        if (!position.HasValue()) {
            return chip::Protocols::InteractionModel::Status::Success; /* nothing this device can act on */
        }

        switch (position.Value()) {
        case TargetPositionEnum::kMoveToFullyClosed:
            start_movement(true);
            return chip::Protocols::InteractionModel::Status::Success;
        case TargetPositionEnum::kMoveToFullyOpen:
            start_movement(false);
            return chip::Protocols::InteractionModel::Status::Success;
        default:
            /* Pedestrian/Ventilation positions need features this file
             * doesn't enable; Signature position has no real meaning on
             * this simple two-relay hardware. Same "smallest reasonable
             * next step" scope cut as everywhere else in this repo. */
            ESP_LOGW(TAG, "Unsupported target position %d requested", (int)position.Value());
            return chip::Protocols::InteractionModel::Status::ConstraintError;
        }
    }

    /* Calibration (CL) isn't enabled — the cluster itself rejects a
     * Calibrate command before ever calling this (confirmed by reading
     * ClosureControlCluster::HandleCalibrate() directly: it checks
     * mFeatureMap.Has(Feature::kCalibration) BEFORE calling the delegate),
     * so this body is never actually reached; kept only because the
     * Delegate interface declares it pure virtual. Same "trivial stub for
     * an unreachable pure-virtual override" precedent firmware/
     * window-covering/'s own Tilt-less HandleMovement() and firmware/
     * robot-vacuum/'s dummy OperationalState Start/Stop callbacks already
     * establish. */
    chip::Protocols::InteractionModel::Status HandleCalibrateCommand() override
    {
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;
    }

    /* No pre-motion staging on this simple relay-driven hardware — a
     * MoveTo command can always start moving immediately, so the cluster
     * transitions straight to MainState::kMoving rather than
     * kWaitingForMotion. */
    bool IsReadyToMove() override { return true; }

    /* Real, meaningful implementation — read at the moment SetMainState()
     * transitions to kMoving, right after HandleMoveToCommand() above has
     * already recorded the new target, so this reports an accurate total
     * travel estimate for the movement that's about to start. */
    chip::ElapsedS GetMovingCountdownTime() override { return (chip::ElapsedS)estimate_remaining_seconds(); }

    /* Never actually invoked in this file's feature scope (Calibration
     * isn't enabled, so MainState can never become kCalibrating) — trivial
     * stub, same reasoning as HandleCalibrateCommand() above. */
    chip::ElapsedS GetCalibrationCountdownTime() override { return 0; }

    /* Never actually invoked either — IsReadyToMove() always returns true
     * above, so MainState never becomes kWaitingForMotion. */
    chip::ElapsedS GetWaitingForMotionCountdownTime() override { return 0; }
};

static ClosureDelegate closure_delegate;

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

/* ClosureControl's commands are handled entirely through the Delegate
 * above, not through the attribute::PRE_UPDATE pattern this repo uses for
 * plain ember attributes elsewhere — node::create() still requires an
 * attribute-update callback, so this one is a no-op, same as firmware/
 * window-covering/'s own. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
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

    /* 2. Configure the two relay outputs, both starting de-energized. */
    gpio_config_t motor_io_conf = {};
    motor_io_conf.pin_bit_mask = (1ULL << CLOSURE_OPEN_GPIO) | (1ULL << CLOSURE_CLOSE_GPIO);
    motor_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&motor_io_conf);
    set_motor(motor_direction_t::STOPPED);

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

    /* 3. Build the Matter data model: one node, one Closure endpoint
     * (Identify + ClosureControl[Positioning]). config.closure_control.
     * delegate is deliberately left null — see the header comment above on
     * why the real delegate is registered separately, via
     * MatterClosureControlSetDelegate(), BEFORE esp_matter::start(). */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Closure",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    closure::config_t closure_config;
    closure_config.closure_control.feature_flags = cluster::closure_control::feature::positioning::get_id();
    endpoint_t *endpoint = closure::create(node, &closure_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create closure endpoint");
        return;
    }

    closure_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Closure endpoint id: %u", closure_endpoint_id);

    /* Register the real Delegate — MUST happen before esp_matter::start(),
     * not after: see the header comment above for the full explanation of
     * why this cluster's own ordering requirement is the opposite of every
     * other Delegate-based cluster in this repo. */
    MatterClosureControlSetDelegate(closure_endpoint_id, closure_delegate);

    xTaskCreate(closure_task, "closure_movement", 4096, NULL, 5, NULL);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* Seed the initial OverallCurrentState now that the cluster's own live
     * instance actually exists (it's constructed during the start() call
     * above — see the header comment). ClosureControlCluster::HandleMoveTo()
     * requires a non-null OverallCurrentState before it will accept any
     * MoveTo command at all (confirmed by reading it directly), so this
     * has to happen before a controller can meaningfully use the device —
     * not merely a nice-to-have first report. */
    report_current_state();

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started — see
     * check_factory_reset_boot_count()'s comment on why this can't
     * happen any earlier. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter closure started. Scan the QR code to commission.");
}
