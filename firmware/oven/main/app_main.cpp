/*
 * Minimal Matter Oven — fifty-eighth device type, and this repo's third
 * genuinely composed, multi-endpoint device after firmware/refrigerator/
 * and firmware/heat-pump/: an Oven (0x007B) root endpoint with one
 * Temperature Controlled Cabinet (0x0071) *child* endpoint, linked via
 * esp-matter's real parent-child endpoint API — the exact same shape
 * firmware/refrigerator/'s own Fridge/Freezer pair already establishes.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Why this was deferred earlier, and what actually unblocked it --------
 * Oven was researched once already this session and deferred: its own
 * mandatory composed Temperature Controlled Cabinet child (under the
 * "Heater" condition) unlocks Oven Cavity Operational State (0x0048) and
 * Oven Mode (0x0049), and confirmed via Docker `grep` that esp-matter's
 * legacy `esp_matter_cluster.cpp` has NO `cluster::oven_cavity_
 * operational_state::create()`/`cluster::oven_mode::create()` namespace
 * for either — only a "generated"-data-model-only pair exists, under the
 * data model this repo has never enabled. Revisited now given an explicit
 * request to keep going past a first pass's "documented skip" — and this
 * genuinely IS unblocked, by reading deeper than the first pass did:
 *
 * 1. The delegate-construction machinery for BOTH clusters already exists
 *    in the shared legacy `esp_matter_delegate_callbacks.cpp` —
 *    `OvenModeDelegateInitCB`/`OvenCavityOperationalStateDelegateInitCB`,
 *    confirmed by reading both directly. Each does exactly what every
 *    other `*DelegateInitCB` in this repo already does (WaterHeaterMode,
 *    RvcRunMode, ...): lazily constructs the real C++ Instance the first
 *    time it runs, during `esp_matter::start()`'s own init-callback pass —
 *    no special ordering awareness needed in `app_main()` at all.
 * 2. `OvenMode` reuses the GENERIC `chip::app::Clusters::ModeBase::
 *    Delegate`/`ModeBase::Instance` classes directly — confirmed by
 *    reading connectedhomeip's own real reference
 *    (`examples/chef/common/clusters/oven-mode/chef-oven-mode.h`, read
 *    directly): there is no OvenMode-specific Delegate class at all, only
 *    a `ModeBase::Delegate` subclass, the exact same base class firmware/
 *    water-heater/'s WaterHeaterMode and firmware/robot-vacuum/'s
 *    RvcRunMode/RvcCleanMode already use in this repo.
 * 3. `OvenCavityOperationalState::Instance` is confirmed, by reading
 *    connectedhomeip's own `CodegenIntegration.h` directly, to be a
 *    TRIVIAL one-line derived class — `Instance(Delegate *d, EndpointId e)
 *    : OperationalState::Instance(d, e, Id) {}` — differing from the base
 *    generic `OperationalState::Instance` only in its own fixed cluster
 *    ID. Its Delegate is the plain generic `OperationalState::Delegate`,
 *    the exact same base class firmware/dishwasher/'s, firmware/
 *    laundry-washer/'s, and firmware/laundry-dryer/'s own delegates
 *    already use. Unlike firmware/robot-vacuum/'s own `RvcOperationalState`
 *    (whose own legacy ember-shell wrapper, `cluster::rvc_operational_
 *    state::create()`, is confirmed to skip delegate wiring entirely,
 *    forcing that file to hand-construct its `Instance` manually in
 *    `app_main()`), THIS file writes its own ember-shell construction code
 *    directly — so it simply includes the `set_delegate_and_init_
 *    callback()` call itself, getting the SAME automatic Instance
 *    construction WaterHeaterMode/RvcRunMode already enjoy, with no manual
 *    `new Instance` needed anywhere in `app_main()`.
 * 4. `operational_state::event::create_operation_completion(cluster_t*)`
 *    (needed for the mandatory OperationCompletion event) is confirmed
 *    generic and safe to call on a derived-cluster-typed `cluster_t*` by
 *    reading its own implementation directly (`esp_matter::event::
 *    create(cluster, OperationalState::Events::OperationCompletion::Id)`
 *    — the event's real cluster association comes from the `cluster`
 *    argument's own registered ID, not from which enum namespace the ID
 *    constant happened to be fetched from) — and, more importantly,
 *    confirmed by reading esp-matter's own ALREADY-SHIPPED, working
 *    `robotic_vacuum_cleaner::add()` directly, which calls this exact
 *    same generic helper on an `RvcOperationalState`-typed cluster
 *    pointer. Not a guess — a pattern already proven working in this
 *    exact SDK.
 *
 * --- Root: Identify (optional) + the child cabinet -------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * Oven.xml: the root only lists Identify as `<optionalConform/>` — all the
 * real substance is a MANDATORY composed Temperature Controlled Cabinet
 * (0x0071) child under the "Heater" condition. `endpoint::oven::create()`
 * confirmed, by reading esp-matter's own legacy `oven::add()` directly, to
 * do only `add_device_type()` — matching the XML's own minimal root
 * exactly, no surprises. Identify added manually, same "optionalConform,
 * not auto-wired" shape this repo hits repeatedly.
 *
 * --- Child: Temperature Controlled Cabinet under "Heater" ------------------
 * `endpoint::temperature_controlled_cabinet::create()` reused verbatim
 * from firmware/refrigerator/'s own proven construction, including that
 * file's own documented legacy-vs-generated gap (the legacy `add()` does
 * NOT auto-set the TN feature flag the way the "generated" data model
 * does, and the legacy field name is `temp_setpoint`, not
 * `temperature_setpoint` — both set explicitly here, same fix). Under the
 * "Heater" condition, TemperatureControlledCabinet.xml's own
 * `<conditionRequirements>` confirm TemperatureControl is TN-mandatory/
 * TL-disallowed (same as the "Cooler" condition firmware/refrigerator/
 * already uses) — plus, uniquely to "Heater," Oven Cavity Operational
 * State and Oven Mode both become real optionalConform options (both
 * implemented here) alongside Temperature Alarm (provisionalConform-first
 * per the XML, same documented-skip precedent firmware/microwave-oven/'s
 * own PowerInWatts feature and firmware/electrical-utility-meter/'s own
 * Commodity Metering skip already establish) and Temperature Measurement
 * (optionalConform — deliberately NOT added, see below).
 *
 * --- No closed-loop temperature control: an honest, documented scope cut -
 * TemperatureControl's own SetTemperature command is handled entirely
 * inside the cluster (same pattern firmware/refrigerator/'s own header
 * comment already documents in full for TN mode) — a controller's real
 * setpoint is genuinely accepted and stored. But this file's own physical
 * heating output does NOT close a real feedback loop against that
 * setpoint: this repo's only verified temperature driver (DS18B20, ~125
 * degC max rating) cannot honestly survive a real oven cavity's actual
 * cooking temperatures (commonly 150-250 degC) — the exact same "no
 * oven-safe sensor available/verified in this repo" reasoning firmware/
 * cooktop/'s own skipped TemperatureMeasurement addition already
 * establishes for the same underlying hardware gap. Rather than fabricate
 * a closed loop against a sensor that couldn't survive the environment it
 * would be measuring, the heating relay here simply follows
 * OvenCavityOperationalState's own Running/Stopped state directly — a
 * real, honest "the operator starts it, the operator stops it" manual
 * oven, giving real Start/Stop/Door-interlock control without a fabricated
 * thermostatic claim. TemperatureMeasurement (optionalConform on this same
 * XML) is correspondingly NOT added, for the identical reason.
 *
 * --- Oven Mode: 9 real modes, purely informational --------------------
 * `ModeTag` values (Bake/Convection/Grill/Roast/Clean/ConvectionBake/
 * ConvectionRoast/Warming/Proofing) confirmed directly against
 * connectedhomeip's own generated `OvenMode/Enums.h`, and the mode list
 * itself ported from the same chef reference cited above. CurrentMode
 * carries no differentiated physical behavior — same "smallest reasonable
 * next step" scope cut firmware/laundry-washer/'s own Delicate/Heavy modes
 * and firmware/microwave-oven/'s own Normal/Defrost modes already
 * establish.
 *
 * --- Hardware: one relay, one optional door sensor -------------------------
 * `OVEN_HEATING_RELAY_GPIO` (active-LOW, matching this repo's established
 * relay convention) energizes whenever OvenCavityOperationalState reports
 * Running AND (if wired) the door is confirmed closed — a real, genuinely
 * useful safety interlock (many real ovens refuse to heat with the door
 * open), reusing the exact same opt-in-GPIO, optimistic-when-absent
 * convention firmware/door-lock/'s position sensor and firmware/
 * robot-vacuum/'s dock sensor already establish (`OVEN_DOOR_GPIO`,
 * default `GPIO_NUM_NC`). Pause/Resume are both `<disallowConform/>` on
 * OvenCavityOperationalState.xml — confirmed structurally enforced by the
 * BASE `OperationalState::Instance` class itself (its own
 * `IsDerivedClusterStatePauseCompatible()`/`...ResumeCompatible()` default
 * to always-false, and `OvenCavityOperationalState::Instance` never
 * overrides either), so this file's own delegate callbacks for Pause/
 * Resume are simple, defensive rejections that in practice are unlikely
 * to ever be reached at all. Boots to Stopped, matching every other
 * device type's own boot-to-known-safe-state convention.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker after
 * two real, sequential compile errors were caught and fixed — not guessed:
 * every hand-assembled attribute/command/event helper (`global::`,
 * `mode_base::`, `operational_state::`) needs the same `cluster::` prefix
 * every other legacy wrapper function in this repo already implicitly
 * carries (easy to miss when writing this construction by hand for the
 * first time, since `mode_select::create()`'s own body — the template this
 * file's OvenMode shell construction is directly modeled on — reads as
 * bare `mode_base::attribute::...` from INSIDE the `esp_matter::cluster`
 * namespace, not from `app_main()`'s own top-level scope); and
 * `OvenModeDelegateInitCB`/`OvenCavityOperationalStateDelegateInitCB`
 * themselves are declared inside a further-nested `esp_matter::cluster::
 * delegate_cb::` namespace, not directly under `esp_matter::cluster::` —
 * confirmed by the compiler's own suggested-fix message, not assumed. Not
 * hardware-tested (no relay/reed-switch hardware for this device type
 * physically available when written).
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
#include <app/clusters/operational-state-server/operational-state-server.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>
#include <app/clusters/temperature-control-server/TemperatureControlCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <esp_matter_delegate_callbacks.h>

static const char *TAG = "matter_oven";

/* --- GPIO pin map -----------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module" — polarity isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define OVEN_HEATING_RELAY_GPIO GPIO_NUM_16   /* active-LOW */
/* Optional door-position reed switch, pulled up: LOW=closed, HIGH=open.
 * Off by default (GPIO_NUM_NC) — see HandleStartStateCallback()/door_task()
 * below for what changes once this is wired to a real GPIO. */
#define OVEN_DOOR_GPIO GPIO_NUM_NC

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Door-sensor debounce (only used if OVEN_DOOR_GPIO is actually wired). */
#define OVEN_DOOR_POLL_INTERVAL_MS 200
#define OVEN_DOOR_DEBOUNCE_SAMPLES 3

/* Oven cavity: 30.00-260.00 degC, default target 180.00 degC — an ordinary
 * real oven's own commercial range, not researched against one specific
 * real appliance's spec sheet, same "there's no single correct answer
 * here" reasoning firmware/refrigerator/'s own fridge/freezer setpoint
 * ranges already establish. */
#define OVEN_MIN_CENTIDEGREES 3000
#define OVEN_MAX_CENTIDEGREES 26000
#define OVEN_DEFAULT_SETPOINT_CENTIDEGREES 18000

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using namespace chip::literals;

static uint16_t oven_endpoint_id = 0;
static uint16_t cabinet_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static bool oven_door_open = false; /* only meaningful if OVEN_DOOR_GPIO is wired */
static bool oven_relay_on = false;

/* --- OvenMode: the generic ModeBase::Delegate, same base class firmware/
 * water-heater/'s WaterHeaterMode and firmware/robot-vacuum/'s RvcRunMode/
 * RvcCleanMode already use — see the header comment above for why no
 * OvenMode-specific Delegate class exists at all. Ported from
 * connectedhomeip's own real chef reference. --------------------------- */
class OvenModeDelegate : public ModeBase::Delegate
{
public:
    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        const char *text = kModeLabels[modeIndex];
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan(text, strlen(text)), label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = modeIndex;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetModeTagsByIndex(uint8_t modeIndex, chip::app::DataModel::List<detail::Structs::ModeTagStruct::Type> &tags) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        if (tags.size() < 1) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        tags[0] = {.value = kModeTags[modeIndex]};
        tags.reduce_size(1);
        return CHIP_NO_ERROR;
    }

    void HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        ESP_LOGI(TAG, "Oven mode set to %u (%s)", newMode, (newMode < kNumModes) ? kModeLabels[newMode] : "?");
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    static constexpr uint8_t kNumModes = 9;
    static constexpr const char *kModeLabels[kNumModes] = {
        "Bake", "Convection", "Grill", "Roast", "Clean",
        "Convection Bake", "Convection Roast", "Warming", "Proofing",
    };
    static constexpr uint16_t kModeTags[kNumModes] = {
        chip::to_underlying(OvenMode::ModeTag::kBake),
        chip::to_underlying(OvenMode::ModeTag::kConvection),
        chip::to_underlying(OvenMode::ModeTag::kGrill),
        chip::to_underlying(OvenMode::ModeTag::kRoast),
        chip::to_underlying(OvenMode::ModeTag::kClean),
        chip::to_underlying(OvenMode::ModeTag::kConvectionBake),
        chip::to_underlying(OvenMode::ModeTag::kConvectionRoast),
        chip::to_underlying(OvenMode::ModeTag::kWarming),
        chip::to_underlying(OvenMode::ModeTag::kProofing),
    };
};

static OvenModeDelegate oven_mode_delegate;

/* --- OvenCavityOperationalState: the generic OperationalState::Delegate,
 * same base class firmware/dishwasher/'s, firmware/laundry-washer/'s, and
 * firmware/laundry-dryer/'s own delegates already use — ported from
 * connectedhomeip's own real dishwasher-app reference. Pause/Resume are
 * both disallowConform on this cluster's own XML; the base Instance class
 * itself already structurally refuses them (see the header comment
 * above), so these two callbacks are simple, defensive rejections. ----- */
class OvenCavityDelegate : public OperationalState::Delegate
{
public:
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override { return {}; }

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, OperationalState::GenericOperationalState &state) override
    {
        if (index >= kNumStates) {
            return CHIP_ERROR_NOT_FOUND;
        }
        state = OperationalState::GenericOperationalState(kStates[index]);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &phase) override
    {
        return CHIP_ERROR_NOT_FOUND; /* no phases defined — PhaseList stays null */
    }

    void HandlePauseStateCallback(OperationalState::GenericOperationalError &err) override
    {
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kCommandInvalidInState));
    }

    void HandleResumeStateCallback(OperationalState::GenericOperationalError &err) override
    {
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kCommandInvalidInState));
    }

    void HandleStartStateCallback(OperationalState::GenericOperationalError &err) override
    {
        if (oven_door_open) {
            ESP_LOGW(TAG, "Refusing to start — door is open");
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kCommandInvalidInState));
            return;
        }
        oven_relay_on = true;
        gpio_set_level(OVEN_HEATING_RELAY_GPIO, 0); /* active-LOW: on */
        ESP_LOGI(TAG, "Oven started — heating relay ON");
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleStopStateCallback(OperationalState::GenericOperationalError &err) override
    {
        oven_relay_on = false;
        gpio_set_level(OVEN_HEATING_RELAY_GPIO, 1); /* active-LOW: off */
        ESP_LOGI(TAG, "Oven stopped — heating relay OFF");
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));

        /* Fire the mandatory OperationCompletion event — reached via the
         * registry-lookup-and-cast pattern, same "get_delegate_managed_
         * instance()" shape firmware/dishwasher/'s and firmware/
         * irrigation-system/'s own OperationalState reach-back already
         * establish. */
        cluster_t *cluster = cluster::get(cabinet_endpoint_id, OvenCavityOperationalState::Id);
        if (cluster) {
            auto *instance = static_cast<OvenCavityOperationalState::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cluster));
            if (instance) {
                instance->OnOperationCompletionDetected(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
            }
        }
    }

private:
    static constexpr size_t kNumStates = 3; /* no Paused — Pause is disallowConform on this cluster */
    static constexpr uint8_t kStates[kNumStates] = {
        chip::to_underlying(OperationalState::OperationalStateEnum::kStopped),
        chip::to_underlying(OperationalState::OperationalStateEnum::kRunning),
        chip::to_underlying(OperationalState::OperationalStateEnum::kError),
    };
};

static OvenCavityDelegate oven_cavity_delegate;

/* --- Registry-lookup-and-cast helper for the code-driven TemperatureControl
 * cluster — same pattern firmware/refrigerator/'s own get_temperature_
 * control_cluster() already establishes (only used here to read the live
 * setpoint back for logging; no closed loop is driven from it — see the
 * header comment above for why). --------------------------------------- */
static chip::app::Clusters::TemperatureControlCluster *get_temperature_control_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, TemperatureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<chip::app::Clusters::TemperatureControlCluster *>(iface);
}

/* --- Door sensor: only meaningful if OVEN_DOOR_GPIO is actually wired ---
 * Same debounce shape firmware/refrigerator/'s own door_task() already
 * establishes. If left unwired (GPIO_NUM_NC), oven_door_open stays false
 * forever — the optimistic-when-absent convention this repo's other
 * opt-in position sensors already use. */
static void door_task(void *arg)
{
    if (OVEN_DOOR_GPIO == GPIO_NUM_NC) {
        vTaskDelete(NULL);
        return;
    }

    bool last_sample = false;
    int consistent_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(OVEN_DOOR_POLL_INTERVAL_MS));

        bool sample_open = (gpio_get_level(OVEN_DOOR_GPIO) == 1);
        if (sample_open == last_sample) {
            consistent_count++;
        } else {
            consistent_count = 1;
            last_sample = sample_open;
        }

        if (consistent_count >= OVEN_DOOR_DEBOUNCE_SAMPLES && sample_open != oven_door_open) {
            oven_door_open = sample_open;
            ESP_LOGI(TAG, "Oven door %s", oven_door_open ? "OPEN" : "closed");
            if (oven_door_open && oven_relay_on) {
                /* Real safety interlock: door opened mid-run — stop
                 * heating immediately regardless of what OperationalState
                 * itself currently says (a real hardware cutoff, not
                 * something that waits for a Stop command). */
                oven_relay_on = false;
                gpio_set_level(OVEN_HEATING_RELAY_GPIO, 1);
                ESP_LOGW(TAG, "Door opened mid-run — heating relay forced OFF");
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

/* Nothing on this device needs to react to a plain-ember attribute write —
 * TemperatureControl's SetTemperature command is handled entirely inside
 * that code-driven cluster (see the header comment above), and OvenMode/
 * OvenCavityOperationalState are both delegate-driven, not plain ember
 * attributes. */
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

    /* 2. Configure the heating relay — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << OVEN_HEATING_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(OVEN_HEATING_RELAY_GPIO, 1); /* active-LOW: 1 = off */

    /* 2b. Configure the optional door sensor, if wired. */
    if (OVEN_DOOR_GPIO != GPIO_NUM_NC) {
        gpio_config_t door_io_conf = {};
        door_io_conf.pin_bit_mask = (1ULL << OVEN_DOOR_GPIO);
        door_io_conf.mode = GPIO_MODE_INPUT;
        door_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&door_io_conf);
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

    /* 3. Build the Matter data model: one node, one Oven root endpoint
     * (Descriptor via the helper, plus Identify added manually), and one
     * Temperature Controlled Cabinet child endpoint (TemperatureControl
     * via the helper, plus OvenCavityOperationalState + OvenMode both
     * hand-assembled — see the header comment above for the full
     * detail). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::oven::config_t oven_config;
    endpoint_t *oven_endpoint = endpoint::oven::create(node, &oven_config, ENDPOINT_FLAG_NONE, NULL);
    if (!oven_endpoint) {
        ESP_LOGE(TAG, "Failed to create oven endpoint");
        return;
    }
    oven_endpoint_id = endpoint::get_id(oven_endpoint);
    ESP_LOGI(TAG, "Oven endpoint id: %u", oven_endpoint_id);

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(oven_endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3a. Temperature Controlled Cabinet child — same legacy-vs-generated
     * TN feature-flag/field-name fix firmware/refrigerator/'s own header
     * comment already documents in full. */
    endpoint::temperature_controlled_cabinet::config_t cabinet_config;
    cabinet_config.temperature_control.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    cabinet_config.temperature_control.features.temperature_number.temp_setpoint = OVEN_DEFAULT_SETPOINT_CENTIDEGREES;
    cabinet_config.temperature_control.features.temperature_number.min_temperature = OVEN_MIN_CENTIDEGREES;
    cabinet_config.temperature_control.features.temperature_number.max_temperature = OVEN_MAX_CENTIDEGREES;

    endpoint_t *cabinet_endpoint =
        endpoint::temperature_controlled_cabinet::create(node, &cabinet_config, ENDPOINT_FLAG_NONE, NULL);
    if (!cabinet_endpoint) {
        ESP_LOGE(TAG, "Failed to create oven cavity endpoint");
        return;
    }
    cabinet_endpoint_id = endpoint::get_id(cabinet_endpoint);
    ESP_LOGI(TAG, "Oven cavity endpoint id: %u", cabinet_endpoint_id);

    /* 3b. Oven Mode — hand-assembled raw ember shell mirroring esp-
     * matter's own `water_heater_mode::create()` body exactly (same
     * shape, just OvenMode::Id + OvenModeDelegateInitCB — both confirmed
     * to already exist in the shared legacy delegate-callbacks file, see
     * the header comment above). */
    {
        cluster_t *mode_cluster = cluster::create(cabinet_endpoint, OvenMode::Id, CLUSTER_FLAG_SERVER);
        if (!mode_cluster) {
            ESP_LOGE(TAG, "Failed to create oven mode cluster");
            return;
        }
        static const auto oven_mode_delegate_init_cb = esp_matter::cluster::delegate_cb::OvenModeDelegateInitCB;
        cluster::set_delegate_and_init_callback(mode_cluster, oven_mode_delegate_init_cb, &oven_mode_delegate);
        cluster::global::attribute::create_feature_map(mode_cluster, 0);
        cluster::mode_base::attribute::create_supported_modes(mode_cluster, NULL, 0, 0);
        cluster::global::attribute::create_cluster_revision(mode_cluster, 2);
        cluster::mode_base::attribute::create_current_mode(mode_cluster, 0); /* boots to "Bake" */
        cluster::mode_base::command::create_change_to_mode(mode_cluster);
        cluster::mode_base::command::create_change_to_mode_response(mode_cluster);
    }

    /* 3c. Oven Cavity Operational State — hand-assembled raw ember shell
     * mirroring esp-matter's own `operational_state::create()` body
     * exactly (same shape, just OvenCavityOperationalState::Id +
     * OvenCavityOperationalStateDelegateInitCB — both confirmed to
     * already exist). */
    {
        cluster_t *ops_cluster = cluster::create(cabinet_endpoint, OvenCavityOperationalState::Id, CLUSTER_FLAG_SERVER);
        if (!ops_cluster) {
            ESP_LOGE(TAG, "Failed to create oven cavity operational state cluster");
            return;
        }
        static const auto oven_cavity_delegate_init_cb = esp_matter::cluster::delegate_cb::OvenCavityOperationalStateDelegateInitCB;
        cluster::set_delegate_and_init_callback(ops_cluster, oven_cavity_delegate_init_cb, &oven_cavity_delegate);
        cluster::global::attribute::create_feature_map(ops_cluster, 0);
        cluster::operational_state::attribute::create_phase_list(ops_cluster, NULL, 0, 0);
        cluster::operational_state::attribute::create_current_phase(ops_cluster, 0);
        cluster::operational_state::attribute::create_operational_state_list(ops_cluster, NULL, 0, 0);
        cluster::operational_state::attribute::create_operational_state(ops_cluster, 0);
        cluster::operational_state::attribute::create_operational_error(ops_cluster, NULL, 0, 0);
        cluster::global::attribute::create_cluster_revision(ops_cluster, 1);
        cluster::operational_state::event::create_operational_error(ops_cluster);
        cluster::operational_state::event::create_operation_completion(ops_cluster);
    }

    err = set_parent_endpoint(cabinet_endpoint, oven_endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set oven cavity's parent endpoint: %d", err);
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

    /* 5. Start the door sensor's own debounced poll (a no-op task exit if
     * OVEN_DOOR_GPIO isn't wired — see door_task() above). */
    xTaskCreate(door_task, "door_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter oven started. Scan the QR code to commission.");
}
