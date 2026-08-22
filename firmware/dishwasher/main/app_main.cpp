/*
 * Minimal Matter Dishwasher — twenty-eighth device type, and this repo's
 * first over the *generic* OperationalState cluster (0x0060 — the same
 * base cluster RvcOperationalState derives from, but used here directly,
 * un-derived) plus a genuinely new command-cycle shape: Start/Stop/Pause/
 * Resume driving a real, if simplified, wash cycle, rather than a
 * continuous regulation loop (thermostat/water-heater/refrigerator) or a
 * one-shot actuation (valve/door-lock).
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
 * --- Endpoint: esp-matter's own naming uses an underscore ("dish_washer"),
 * the CSA's own device type name doesn't --------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * Dishwasher.xml: only OperationalState is `<mandatoryConform/>` (with its
 * own OperationCompletion event also mandatory, per this device type's own
 * revision 2) — Identify, On/Off (DeadFrontOnOff feature only, a narrow
 * "is the device's own UI/front-panel powered" semantic, not a real power
 * switch — confirmed by name and left out, same "smallest reasonable next
 * step" scope cut as skipping other narrow optional-richness gaps
 * elsewhere in this repo), TemperatureControl, DishwasherMode, and
 * DishwasherAlarm are all `<optionalConform/>`. A real, worth-remembering
 * naming gotcha found while researching this: esp-matter's own generated
 * files use `dish_washer` (with an underscore) throughout — the endpoint
 * helper is `esp_matter::endpoint::dish_washer::create()`, the mode
 * cluster is `cluster::dish_washer_mode`, the alarm cluster is
 * `cluster::dish_washer_alarm` — none of which match the CSA's own
 * "Dishwasher"/"DishwasherMode"/"DishwasherAlarm" naming (no underscore,
 * no space) at all. A first search for "dishwasher" (no underscore)
 * across esp-matter's own source came back nearly empty as a result —
 * confirmed only by widening the search to "dish.washer" that the
 * cluster/endpoint wrappers exist at all under this different name. The
 * underlying connectedhomeip cluster server code and its own C++
 * namespaces (`chip::app::Clusters::DishwasherMode`,
 * `::DishwasherAlarm`) use the CSA's un-underscored spelling as expected —
 * only esp-matter's own convenience-wrapper file/namespace names differ.
 * `endpoint::dish_washer::create()` confirmed to add only a Descriptor
 * cluster + OperationalState (with its own mandatory OperationCompletion
 * event pre-registered) via `common::create<T>()` (auto-Descriptor) — same
 * "config_t = app_with_operational_state_config" shared shape
 * `endpoint::laundry_washer::create()` also uses, confirmed by reading
 * both side by side. Identify + TemperatureControl + DishwasherMode +
 * DishwasherAlarm are all added manually onto this same endpoint
 * afterward, the same "add extra clusters onto an already-correct
 * endpoint" pattern established repeatedly in this repo.
 *
 * --- OperationalState: a SIXTH distinct pattern for getting a live
 * cluster-instance pointer from app code, and the delegate itself owns
 * its own state transitions ------------------------------------------
 * Unlike firmware/robot-vacuum/'s RvcOperationalState (whose esp-matter
 * `config_t` is a literally empty struct, needing a hand-built raw
 * `Instance`+`Delegate` pair constructed and `.Init()`'d entirely by
 * hand), the GENERIC OperationalState cluster used here DOES support
 * `config->delegate` — confirmed by reading `esp_matter_cluster.cpp`'s own
 * `operational_state::create()` directly: `set_delegate_and_init_callback
 * (cluster, OperationalStateDelegateInitCB, config->delegate)`, the exact
 * same automatic-construction path FanControl/ModeBase/EnergyEvse/
 * DishwasherAlarm/DishwasherMode all already use in this repo — no
 * ordering awareness needed for *registering* the delegate. Ported
 * directly from connectedhomeip's own real, working reference
 * (`examples/dishwasher-app/dishwasher-common/src/
 * operational-state-delegate-impl.cpp`, read end to end rather than
 * guessed): each `HandleXStateCallback` calls `GetInstance()->
 * SetOperationalState(...)` directly and sets `err` accordingly — the
 * Delegate itself owns the state transition, unlike e.g. firmware/
 * door-lock/'s LockState or firmware/valve/'s CurrentState (both of which
 * are reported back through a *separate* call after the framework's own
 * command handling already ran). `GetInstance()` is a protected accessor
 * on `OperationalState::Delegate` itself (set automatically via
 * `SetInstance()` when the framework constructs the `Instance`), so no
 * registry lookup is needed *inside* the delegate's own callbacks.
 *
 * A registry-style lookup IS still needed for the two places this file
 * needs to touch the cluster from OUTSIDE the delegate's own callbacks —
 * the door-sensor task's own safety-pause (an async event, not a
 * controller command) and the wash-cycle-completion path (the cycle
 * finishing on its own, not from a Stop command). Confirmed esp-matter's
 * own public `get_delegate_managed_instance(cluster::get(endpoint_id,
 * OperationalState::Id))` (declared in `esp_matter_data_model.h`) returns
 * exactly the same `OperationalState::Instance*` the framework
 * constructed — a SIXTH genuinely distinct "how do I reach a live
 * cluster instance from app code" pattern in this repo now, after: (1)
 * plain registry-lookup setter (BooleanState/OccupancySensing), (2) a
 * Delegate whose own reporting call happens to be a working generic
 * free-function proxy (WindowCovering), (3) a Delegate needing the
 * `chip::app::…registry().Get()`-based registry-lookup fallback instead
 * (FanControl/Valve/EnergyEvse/Switch/TemperatureControl/
 * TemperatureMeasurement — all `DefaultServerCluster`-derived), (4) a
 * cluster-family-specific convenience free function
 * (`ResourceMonitoring::GetClusterInstance()`), (5) a direct FeatureMap
 * `attribute::update()` override (firmware/water-leak-detector/), and now
 * (6) esp-matter's own `get_delegate_managed_instance()`, for a
 * legacy-ember-style cluster whose live C++ instance is delegate-managed
 * but NOT one of connectedhomeip's newer `DefaultServerCluster`-registry
 * clusters at all — confirmed by checking: no `operational_state/` folder
 * exists under `data_model_provider/clusters/`.
 *
 * --- TemperatureControl: TN-only, same pattern (and same legacy-vs-
 * generated pitfall) as firmware/refrigerator/ ---------------------------
 * Represents the selected wash-temperature target (min 30.00 degC, max
 * 70.00 degC, default 50.00 degC — ordinary real dishwasher wash-
 * temperature range, not researched against one specific real appliance's
 * spec sheet). Confirmed the same legacy-vs-generated discrepancy
 * firmware/refrigerator/ already found and documented applies here too
 * (this build's own sdkconfig.defaults leaves
 * `CONFIG_ESP_MATTER_ENABLE_GENERATED_DATA_MODEL` off, same as every
 * device type in this repo, so the "legacy" cluster set is what actually
 * compiles): `feature_flags` is set explicitly
 * (`cluster::temperature_control::feature::temperature_number::get_id()`,
 * NOT auto-set by `dish_washer::add()`, which doesn't even touch
 * TemperatureControl at all since it's optionalConform here), and the
 * field name is the legacy header's own `temp_setpoint`, not
 * `temperature_setpoint`. `TemperatureControlCluster::SetDelegate()` is
 * skipped for the identical reason firmware/refrigerator/'s own header
 * comment documents in full: it's TL-specific, and TL is never set here.
 * A controller's SetTemperature command is handled entirely inside the
 * cluster; this file's own control loop only ever *reads* the live
 * setpoint back via the registry-lookup-and-cast pattern (`TemperatureControlCluster
 * ::GetTemperatureSetpoint()`), same as refrigerator's own per-cabinet
 * control loop.
 *
 * --- DishwasherMode: a fifth ModeBase-derived cluster this repo has
 * built, with a real business rule borrowed from firmware/robot-vacuum/ -
 * Same automatic `config->delegate` + `InitModeDelegate()` wiring as
 * every other ModeBase cluster here (confirmed via
 * `DishwasherModeDelegateInitCB` in `esp_matter_delegate_callbacks.cpp`).
 * Three real modes — "Normal" (`ModeTag::kNormal`, 0x4000), "Heavy"
 * (`ModeTag::kHeavy`, 0x4001), "Light" (`ModeTag::kLight`, 0x4002) — all
 * three confirmed directly against connectedhomeip's own generated
 * `DishwasherMode/Enums.h`, which happens to define exactly these three
 * dishwasher-specific tags (unlike e.g. RefrigeratorAndTemperature
 * ControlledCabinetMode, which only has the generic `kAuto` plus two
 * device-specific "Rapid" tags — Dishwasher gets three genuinely named
 * wash-intensity tags of its own). A real business rule, the same
 * category firmware/robot-vacuum/'s own "reject a clean-mode change while
 * actively cleaning" rule already established: `HandleChangeToMode()`
 * rejects the request with `ModeBase::StatusCode::kInvalidInMode`
 * whenever `OperationalState == Running` — a real dishwasher's own wash-
 * program selector is normally locked out mid-cycle. Reading the live
 * OperationalState value for this check reuses the same `get_delegate_
 * managed_instance()` pattern described above.
 *
 * --- DishwasherAlarm: a genuinely complete Delegate + Server API, no
 * event-sending gap this time (unlike firmware/refrigerator/'s
 * RefrigeratorAlarm) --------------------------------------------------
 * Confirmed by reading `dishwasher-alarm-server.h` directly:
 * `DishwasherAlarmServer::Instance().SetStateValue(endpoint, newState)`
 * is a real, complete, ready-to-call API that ALSO fires the cluster's
 * own Notify event internally (`SendNotifyEvent()`, private, called from
 * inside `SetStateValue()`) — no manual event-rigging needed here, unlike
 * RefrigeratorAlarm's own documented gap. `AlarmBitmap` (confirmed
 * directly against connectedhomeip's own generated Enums.h) has six real
 * bits: InflowError/DrainError/DoorError/TempTooLow/TempTooHigh/
 * WaterLevelError. `Supported`/`Mask` are both set to all six bits at
 * startup (via `SetSupportedValue()`/`SetMaskValue()`, called after
 * `esp_matter::start()` — same "delegate-dependent calls only after the
 * Matter server is actually running" discipline firmware/valve/'s and
 * firmware/fan/'s own `SetDefaultDelegate()` ordering fix already
 * established in this repo) — but only DoorError is ever actually
 * asserted, driven by the same real reed-switch door sensor
 * OperationalState's own safety-pause logic already reads: InflowError/
 * DrainError/TempTooLow/TempTooHigh/WaterLevelError would each need real
 * flow/level/temperature-fault sensing hardware this hobby-scale build
 * doesn't have, so they're declared supported (spec-honest conformance)
 * but never raised — same "smallest reasonable next step" scope cut as
 * firmware/evse/'s own always-`NoError` `FaultState`. `dish_washer_alarm
 * ::create()`'s own FeatureMap is hardcoded to 0 (confirmed the same way
 * as RefrigeratorAlarm/AirQuality's own documented gaps) — the Reset
 * (`kReset`) feature bit is therefore never set, meaning a real
 * controller's `ResetAlarms` command isn't advertised at all; this file's
 * own `ResetAlarmsCallback()` implementation exists and works regardless
 * (the delegate method itself has no FeatureMap gate), but the only real
 * path DoorError actually clears through here is the door physically
 * closing again, which is arguably the more honest behavior for a latched
 * safety condition anyway.
 *
 * --- The wash cycle itself: a real, if deliberately simplified, timed
 * state machine — Fill is skipped, Wash + Drain are real phases --------
 * `dishwasher_task()` drives three real relay outputs
 * (`DISHWASHER_HEATER_RELAY_GPIO`/`_WASH_PUMP_RELAY_GPIO`/
 * `_DRAIN_PUMP_RELAY_GPIO`, all active-LOW, matching firmware/valve/'s and
 * firmware/refrigerator/'s own relay convention) through two timed
 * phases — Washing (heater, hysteresis-controlled against the DS18B20
 * reading and TemperatureControl's own live setpoint, same 0.5 degC
 * hysteresis convention firmware/refrigerator/'s and firmware/
 * water-heater/'s own control loops already use, plus the wash pump/
 * motor running continuously) for `DISHWASHER_WASH_DURATION_MS` (45
 * minutes, a real-scale wash-phase duration, adjustable), then Draining
 * (drain pump only) for `DISHWASHER_DRAIN_DURATION_MS` (5 minutes) —
 * before calling `OnOperationCompletionDetected()` and returning to
 * Stopped. No water-inlet-fill phase or turbidity/soil-sensing logic is
 * modelled at all (a real water-inlet valve + float switch would be
 * needed for a genuine Fill phase) — same honest, documented scope cut
 * as firmware/robot-vacuum/'s own "no real navigation" limitation, just
 * for a wash cycle instead of movement. `HandleStartStateCallback()`
 * rejects the command (`ErrorStateEnum::kUnableToStartOrResume`) if the
 * door is open, matching real dishwasher interlock behavior; opening the
 * door mid-cycle doesn't error the OperationalState at all — it PAUSES it
 * (a real, physically-correct dishwasher behavior, not an error
 * condition) while separately raising DishwasherAlarm's DoorError bit,
 * cleared again (but not auto-resumed — the same "no feedback sensor =
 * optimistic-only where honest, but no silent auto-resume of a user-
 * paused-for-safety cycle" precedent as this repo's other door/position
 * interlocks) once the door closes.
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
#include <app/clusters/dishwasher-alarm-server/dishwasher-alarm-server.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>
#include <app/clusters/temperature-control-server/TemperatureControlCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_dishwasher";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module" — polarity isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define DISHWASHER_DOOR_GPIO GPIO_NUM_4                 /* reed switch, pulled up: LOW=closed, HIGH=open */
#define DISHWASHER_SENSOR_GPIO GPIO_NUM_21               /* DS18B20, wash-water temperature */
#define DISHWASHER_HEATER_RELAY_GPIO GPIO_NUM_16          /* active-LOW */
#define DISHWASHER_WASH_PUMP_RELAY_GPIO GPIO_NUM_17       /* active-LOW */
#define DISHWASHER_DRAIN_PUMP_RELAY_GPIO GPIO_NUM_18      /* active-LOW */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Wash-temperature target range (Matter's global `temperature` type — int16,
 * hundredths of a degree C). 30.00-70.00 degC, default 50.00 degC. */
#define DISHWASHER_TEMP_MIN_CENTIDEGREES 3000
#define DISHWASHER_TEMP_MAX_CENTIDEGREES 7000
#define DISHWASHER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES 5000

/* Bang-bang (hysteresis) control band for the heater — same reasoning as
 * firmware/refrigerator/'s and firmware/water-heater/'s own control loops. */
#define DISHWASHER_HYSTERESIS_CENTIDEGREES 50

/* Wash-cycle phase durations — a real, adjustable wash-phase-scale timing
 * (not a compressed demo value); see the header comment above for why
 * there's no separate Fill phase. */
#define DISHWASHER_WASH_DURATION_MS (45UL * 60UL * 1000UL)
#define DISHWASHER_DRAIN_DURATION_MS (5UL * 60UL * 1000UL)

/* How often the control task re-evaluates the heater hysteresis / phase
 * deadlines while a cycle is running. */
#define DISHWASHER_CONTROL_INTERVAL_MS 5000

/* Door-sensor debounce — same shape firmware/refrigerator/'s own door task
 * already uses. */
#define DISHWASHER_DOOR_POLL_INTERVAL_MS 200
#define DISHWASHER_DOOR_DEBOUNCE_SAMPLES 3

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error a blanket `using namespace chip;` caused there.
 * Everything else from `chip::`/`chip::app::` below is spelled out fully
 * qualified instead. */
using namespace chip::literals;

static uint16_t dishwasher_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/refrigerator/'s (itself from firmware/
 * water-heater/'s / firmware/thermostat/'s) DS18B20 driver — see those
 * files' own header comments for the 1-Wire timing/CRC detail and
 * sourcing. Only one sensor here, so the pin is a plain #define again
 * (not parameterized the way refrigerator's two-sensor version needed). */
static bool ow_reset(void)
{
    gpio_set_level(DISHWASHER_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(DISHWASHER_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(DISHWASHER_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(DISHWASHER_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(DISHWASHER_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(DISHWASHER_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(DISHWASHER_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(DISHWASHER_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(DISHWASHER_SENSOR_GPIO);
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
    io_conf.pin_bit_mask = (1ULL << DISHWASHER_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(DISHWASHER_SENSOR_GPIO, 1);
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
 * via SetOperationalState() — read by DishwasherModeDelegate (to reject a
 * mode change while Running) and by dishwasher_task() (to know whether the
 * wash cycle should currently be advancing). `g_door_open` is written by
 * door_task(), read by both. */
static uint8_t g_operational_state = 0; /* OperationalStateEnum::kStopped */
static bool g_door_open = false;

static bool g_heater_on = false;
static bool g_wash_pump_on = false;
static bool g_drain_pump_on = false;

static void set_heater(bool on)
{
    g_heater_on = on;
    gpio_set_level(DISHWASHER_HEATER_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static void set_wash_pump(bool on)
{
    g_wash_pump_on = on;
    gpio_set_level(DISHWASHER_WASH_PUMP_RELAY_GPIO, on ? 0 : 1);
}

static void set_drain_pump(bool on)
{
    g_drain_pump_on = on;
    gpio_set_level(DISHWASHER_DRAIN_PUMP_RELAY_GPIO, on ? 0 : 1);
}

static void stop_all_actuators(void)
{
    set_heater(false);
    set_wash_pump(false);
    set_drain_pump(false);
}

/* --- Registry-lookup-and-cast helpers --------------------------------------
 * TemperatureControlCluster is a DefaultServerCluster (code-driven, real
 * `temperature_control/` folder under `data_model_provider/clusters/`) —
 * same pattern firmware/refrigerator/'s own per-cabinet lookup uses.
 * OperationalState::Instance is NOT — see the header comment above for why
 * `get_delegate_managed_instance()` is the correct call here instead. */
static TemperatureControlCluster *get_temperature_control_cluster(void)
{
    chip::app::ConcreteClusterPath path(dishwasher_endpoint_id, TemperatureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<TemperatureControlCluster *>(iface);
}

static OperationalState::Instance *get_operational_state_instance(void)
{
    cluster_t *cl = cluster::get(dishwasher_endpoint_id, OperationalState::Id);
    if (!cl) {
        return nullptr;
    }
    return static_cast<OperationalState::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cl));
}

/* --- OperationalState delegate ---------------------------------------------
 * Ported directly from connectedhomeip's own real reference
 * (examples/dishwasher-app/dishwasher-common/src/
 * operational-state-delegate-impl.cpp) — see the header comment above for
 * the full detail on why each HandleXStateCallback calls GetInstance()->
 * SetOperationalState() directly rather than a separate app-level call. */
class DishwasherOperationalStateDelegate : public OperationalState::Delegate
{
public:
    /* No live countdown reported — a real value would need this class to
     * read g_phase_deadline_ms (defined further down, alongside
     * dishwasher_task()); returning null here is spec-legal (the
     * attribute is nullable) and matches the "smallest reasonable next
     * step" scope cut this repo applies elsewhere. */
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

    /* No PhaseList implemented — see the header comment above. Returning
     * CHIP_ERROR_NOT_FOUND for index 0 tells the SDK PhaseList is null. */
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

    /* Forward-declared below (needs g_wash_phase / g_phase_deadline_ms,
     * defined further down alongside dishwasher_task()). */
    void begin_wash_cycle();
};
constexpr uint8_t DishwasherOperationalStateDelegate::kStates[];

static DishwasherOperationalStateDelegate operational_state_delegate;

/* --- DishwasherMode delegate ------------------------------------------
 * Same shape as every other ModeBase delegate in this repo (see e.g.
 * firmware/water-heater/'s own WaterHeaterModeDelegate) — the one real
 * business rule (reject a mode change while Running) is the header
 * comment's own "borrowed from firmware/robot-vacuum/" note. */
class DishwasherModeDelegate : public ModeBase::Delegate
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
            ESP_LOGW(TAG, "DishwasherMode change rejected — a wash cycle is running");
            response.status = chip::to_underlying(ModeBase::StatusCode::kInvalidInMode);
            return;
        }
        ESP_LOGI(TAG, "DishwasherMode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

private:
    using ModeTagType = detail::Structs::ModeTagStruct::Type;
    ModeTagType tagsNormal[1] = {{.value = chip::to_underlying(DishwasherMode::ModeTag::kNormal)}};
    ModeTagType tagsHeavy[1] = {{.value = chip::to_underlying(DishwasherMode::ModeTag::kHeavy)}};
    ModeTagType tagsLight[1] = {{.value = chip::to_underlying(DishwasherMode::ModeTag::kLight)}};

    static constexpr uint8_t kModeNormal = 0;
    static constexpr uint8_t kModeHeavy = 1;
    static constexpr uint8_t kModeLight = 2;
    static constexpr size_t kNumModes = 3;
    const detail::Structs::ModeOptionStruct::Type kModes[kNumModes] = {
        {.label = "Normal"_span, .mode = kModeNormal, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsNormal)},
        {.label = "Heavy"_span, .mode = kModeHeavy, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsHeavy)},
        {.label = "Light"_span, .mode = kModeLight, .modeTags = chip::app::DataModel::List<const ModeTagType>(tagsLight)},
    };
};

static DishwasherModeDelegate dishwasher_mode_delegate;

/* --- DishwasherAlarm delegate -------------------------------------------
 * See the header comment above for why FeatureMap's Reset bit is off (a
 * documented, consistent gap with RefrigeratorAlarm/AirQuality) and why
 * that's an acceptable scope cut — DoorError still clears correctly via
 * the door physically closing, just not via a controller-issued
 * ResetAlarms command. */
class DishwasherAlarmDelegate : public DishwasherAlarm::Delegate
{
public:
    bool ModifyEnabledAlarmsCallback(const chip::BitMask<DishwasherAlarm::AlarmMap> mask) override
    {
        ESP_LOGI(TAG, "DishwasherAlarm mask changed to 0x%02x", (unsigned)mask.Raw());
        return true; /* accept every mask a controller sets */
    }

    bool ResetAlarmsCallback(const chip::BitMask<DishwasherAlarm::AlarmMap> alarms) override
    {
        ESP_LOGI(TAG, "DishwasherAlarm reset requested for 0x%02x", (unsigned)alarms.Raw());
        return true; /* let the cluster clear whatever bits it was asked to */
    }
};

static DishwasherAlarmDelegate dishwasher_alarm_delegate;

/* --- Wash-cycle state machine -------------------------------------------
 * See the header comment above for the full phase/timing detail. */
enum class WashPhase { kIdle, kWashing, kDraining };
static WashPhase g_wash_phase = WashPhase::kIdle;
static int64_t g_phase_deadline_ms = 0;

void DishwasherOperationalStateDelegate::begin_wash_cycle()
{
    g_wash_phase = WashPhase::kWashing;
    g_phase_deadline_ms = esp_timer_get_time() / 1000 + (int64_t)DISHWASHER_WASH_DURATION_MS;
    set_wash_pump(true);
    ESP_LOGI(TAG, "Wash cycle started");
}

/* --- Door sensor: debounced poll, drives DoorError + a safety-pause -----
 * See the header comment above for the "pause, don't error" reasoning. */
static void door_task(void *arg)
{
    bool last_sample = false;
    int consistent_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DISHWASHER_DOOR_POLL_INTERVAL_MS));

        bool sample_open = (gpio_get_level(DISHWASHER_DOOR_GPIO) == 1);
        if (sample_open == last_sample) {
            consistent_count++;
        } else {
            consistent_count = 1;
            last_sample = sample_open;
        }

        if (consistent_count >= DISHWASHER_DOOR_DEBOUNCE_SAMPLES && sample_open != g_door_open) {
            g_door_open = sample_open;
            ESP_LOGI(TAG, "Dishwasher door %s", sample_open ? "OPEN" : "closed");

            chip::BitMask<DishwasherAlarm::AlarmMap> door_bit(DishwasherAlarm::AlarmMap::kDoorError);
            DishwasherAlarm::DishwasherAlarmServer::Instance().SetStateValue(dishwasher_endpoint_id,
                                                            sample_open ? door_bit : chip::BitMask<DishwasherAlarm::AlarmMap>());

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
static void dishwasher_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DISHWASHER_CONTROL_INTERVAL_MS));

        if (g_operational_state != chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000;

        if (g_wash_phase == WashPhase::kWashing) {
            float temperature_c;
            if (sensor_read(&temperature_c)) {
                int16_t measured_centidegrees = (int16_t)(temperature_c * 100.0f);
                TemperatureControlCluster *ctrl = get_temperature_control_cluster();
                int16_t target_centidegrees = ctrl ? ctrl->GetTemperatureSetpoint() : DISHWASHER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES;

                if (measured_centidegrees <= target_centidegrees - DISHWASHER_HYSTERESIS_CENTIDEGREES) {
                    if (!g_heater_on) {
                        set_heater(true);
                    }
                } else if (measured_centidegrees >= target_centidegrees + DISHWASHER_HYSTERESIS_CENTIDEGREES) {
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
                set_wash_pump(false);
                set_drain_pump(true);
                g_wash_phase = WashPhase::kDraining;
                g_phase_deadline_ms = now_ms + (int64_t)DISHWASHER_DRAIN_DURATION_MS;
                ESP_LOGI(TAG, "Wash phase complete — draining");
            }
        } else if (g_wash_phase == WashPhase::kDraining) {
            if (now_ms >= g_phase_deadline_ms) {
                set_drain_pump(false);
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

/* Nothing on this device needs to react to a plain-ember attribute write —
 * kept as a trivial stub, same as several other device types in this repo. */
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

    /* 2. Configure the door sensor (pulled up, LOW=closed/HIGH=open). */
    gpio_config_t door_io_conf = {};
    door_io_conf.pin_bit_mask = (1ULL << DISHWASHER_DOOR_GPIO);
    door_io_conf.mode = GPIO_MODE_INPUT;
    door_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&door_io_conf);
    g_door_open = (gpio_get_level(DISHWASHER_DOOR_GPIO) == 1);

    /* 2b. Configure the three relay outputs — boot off (de-energized). */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << DISHWASHER_HEATER_RELAY_GPIO) |
        (1ULL << DISHWASHER_WASH_PUMP_RELAY_GPIO) | (1ULL << DISHWASHER_DRAIN_PUMP_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(DISHWASHER_HEATER_RELAY_GPIO, 1);
    gpio_set_level(DISHWASHER_WASH_PUMP_RELAY_GPIO, 1);
    gpio_set_level(DISHWASHER_DRAIN_PUMP_RELAY_GPIO, 1);

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

    /* 3. Build the Matter data model: one node, one Dishwasher endpoint
     * (Descriptor + OperationalState via the complete top-level helper,
     * plus Identify + TemperatureControl + DishwasherMode + DishwasherAlarm
     * added manually) — see the header comment above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::dish_washer::config_t dishwasher_config;
    dishwasher_config.operational_state.delegate = &operational_state_delegate;

    endpoint_t *endpoint = endpoint::dish_washer::create(node, &dishwasher_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create dishwasher endpoint");
        return;
    }
    dishwasher_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Dishwasher endpoint id: %u", dishwasher_endpoint_id);

    /* 3a. Identify — optionalConform, so dish_washer::add() doesn't create
     * it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3b. TemperatureControl (TN-only) — see the header comment above for
     * the legacy-vs-generated feature_flags/field-name note. */
    cluster::temperature_control::config_t temp_ctrl_config;
    temp_ctrl_config.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    temp_ctrl_config.features.temperature_number.temp_setpoint = DISHWASHER_TEMP_DEFAULT_SETPOINT_CENTIDEGREES;
    temp_ctrl_config.features.temperature_number.min_temperature = DISHWASHER_TEMP_MIN_CENTIDEGREES;
    temp_ctrl_config.features.temperature_number.max_temperature = DISHWASHER_TEMP_MAX_CENTIDEGREES;
    cluster::temperature_control::create(endpoint, &temp_ctrl_config, CLUSTER_FLAG_SERVER);

    /* 3c. DishwasherMode. */
    cluster::dish_washer_mode::config_t mode_config;
    mode_config.current_mode = 0;
    mode_config.delegate = &dishwasher_mode_delegate;
    cluster::dish_washer_mode::create(endpoint, &mode_config, CLUSTER_FLAG_SERVER);

    /* 3d. DishwasherAlarm. */
    cluster::dish_washer_alarm::config_t alarm_config;
    alarm_config.delegate = &dishwasher_alarm_delegate;
    cluster::dish_washer_alarm::create(endpoint, &alarm_config, CLUSTER_FLAG_SERVER);

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

    /* 4b. Declare DishwasherAlarm's Supported/Mask (all 6 bits) now that
     * the delegate is actually attached — same "delegate-dependent calls
     * only after esp_matter::start()" ordering discipline firmware/valve/'s
     * and firmware/fan/'s own SetDefaultDelegate() fix already established
     * (see CLAUDE.md's "Open next steps" item 6). */
    chip::BitMask<DishwasherAlarm::AlarmMap> all_alarms(
        DishwasherAlarm::AlarmMap::kInflowError, DishwasherAlarm::AlarmMap::kDrainError,
        DishwasherAlarm::AlarmMap::kDoorError, DishwasherAlarm::AlarmMap::kTempTooLow,
        DishwasherAlarm::AlarmMap::kTempTooHigh, DishwasherAlarm::AlarmMap::kWaterLevelError);
    DishwasherAlarm::DishwasherAlarmServer::Instance().SetSupportedValue(dishwasher_endpoint_id, all_alarms);
    DishwasherAlarm::DishwasherAlarmServer::Instance().SetMaskValue(dishwasher_endpoint_id, all_alarms);

    /* 5. Start the control tasks — the door sensor's own debounced poll,
     * and the wash-cycle/heater-hysteresis control loop. */
    xTaskCreate(door_task, "door_task", 3072, NULL, 5, NULL);
    xTaskCreate(dishwasher_task, "dishwasher_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter dishwasher started. Scan the QR code to commission.");
}
