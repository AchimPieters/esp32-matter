/*
 * Minimal Matter Window Covering (roller shade / curtain) — eighth device
 * type, and this repo's first with continuous, timed physical movement
 * (every prior actuator — LED, relay, PWM dimmer — reacts to a command more
 * or less instantly; a real curtain/blind motor takes many seconds to
 * travel, and the device has to track and report that progress live).
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
 * --- Why this needs a Delegate, unlike every other device type here -------
 * Checked directly in esp-matter's own source before writing any of this:
 * `endpoint::window_covering::add()` (esp_matter_endpoint.cpp) creates the
 * WindowCovering cluster via `cluster::window_covering::create()`, and that
 * function (esp_matter_cluster.cpp) only calls the app-supplied
 * `config->delegate`'s `HandleMovement()`/`HandleStopMotion()` methods when
 * a client sends UpOrOpen / DownOrClose / GoToLiftPercentage / StopMotion —
 * it does NOT drive any hardware itself, and (unlike LevelControl, which
 * ramps CurrentLevel on a timer entirely inside connectedhomeip's own
 * LevelControlServer — see firmware/dimmable-light/'s header comment) it
 * does NOT simulate movement over time either. The cluster server's job
 * stops at bookkeeping (validating commands, storing TargetPosition, and
 * calling the delegate); actually moving a motor and reporting real
 * progress is entirely the delegate's job. This is confirmed by reading
 * connectedhomeip's own real reference delegate implementation directly —
 * `examples/chef/common/clusters/window-covering/chef-window-covering.cpp`
 * (referenced from esp-matter's own mock delegate's comment) — which does
 * an instant jump from current to target with no timed movement at all,
 * since chef is a headless/simulated device with no real motor to wait on.
 * This file's OutletWindowCoveringDelegate (below) instead does the timed,
 * physical version chef's doesn't need: HandleMovement() just records the
 * requested direction; a FreeRTOS task actually drives the relay outputs
 * and periodically updates CurrentPositionLiftPercent100ths as real time
 * elapses, so a controller watching the position sees it move live over
 * the calibrated travel time — not jump instantly.
 *
 * --- Position tracking: time-based, not a real sensor ---------------------
 * There's no position encoder or limit switch assumed here — just a
 * calibrated "how long does a full open-to-closed traverse take"
 * (WINDOW_COVERING_FULL_TRAVEL_MS). Position is estimated by linear
 * interpolation against elapsed motor-on time, the same technique
 * ESPHome's/Tasmota's own "time-based cover" components use for cheap
 * curtain motors with no feedback of their own — not a novel approach,
 * just the standard one for this class of hardware. A small time buffer is
 * added on top of the calibrated duration when driving all the way to a
 * hard 0% or 100% target (WINDOW_COVERING_ENDSTOP_OVERSHOOT_MS), so timer
 * jitter/motor-speed variance doesn't leave the covering just short of
 * fully open/closed — common practice for the same reason on that class of
 * hardware. Because there's no real feedback, CurrentPositionLiftPercent100ths
 * is only ever as accurate as the calibration; if the motor stalls, slips,
 * or is moved by hand, the reported position silently drifts from reality
 * until the next full open or full close command re-syncs it to a known
 * endpoint (0% or 100%).
 *
 * --- Matter cluster details (checked against the real spec + SDK) ---------
 * Uses esp-matter's `window_covering` endpoint (Identify + Groups +
 * WindowCovering clusters — confirmed in esp_matter_endpoint.cpp) with the
 * `Lift` + `PositionAwareLift` features only (Feature::kLift = 0x1,
 * Feature::kPositionAwareLift = 0x4 — confirmed in the generated
 * WindowCovering/Enums.h), i.e. up/down travel with percentage position
 * reporting — no Tilt (this repo's reference hardware is a simple roller
 * shade / curtain, not a venetian-blind-style tilt mechanism). Type and
 * EndProductType are both set to RollerShade (0x00), the closest match.
 * CurrentPositionLiftPercent100ths follows the spec's own convention
 * (confirmed in WindowCoveringCluster.h's WC_PERCENT100THS_MIN_OPEN/
 * WC_PERCENT100THS_MAX_CLOSED constants): 0 = fully open, 10000 = fully
 * closed (Matter's Percent100ths unit — hundredths of a percent, so 10000
 * = 100.00%).
 *
 * --- Output polarity ------------------------------------------------------
 * Two relay outputs (UP / DOWN), active-LOW by default — matching
 * firmware/outlet/'s own default (a relay is the realistic, common way to
 * reverse-drive an AC curtain/blind motor; active-LOW is common for
 * low-cost opto-isolated relay modules, but always check your specific
 * module's own documentation). The two outputs are mutually exclusive in
 * software — both energized at once could short or damage a typical
 * reversing-relay motor setup, so set_motor() below never allows that.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <app/clusters/window-covering-server/window-covering-delegate.h>
#include <app/clusters/window-covering-server/WindowCoveringCluster.h>
#include <app-common/zap-generated/attributes/Accessors.h>
#include <app/reporting/reporting.h>

static const char *TAG = "matter_window_covering";

/* Change these to the GPIOs your two relay channels are wired to — one
 * drives the motor "open"/"up" direction, the other "close"/"down". Never
 * both at once (see set_motor() below). GPIO 4/5 are plain, unreserved
 * GPIOs on classic ESP32 (WROOM-32). Adjust to match your board. */
#define WINDOW_COVERING_UP_GPIO GPIO_NUM_4
#define WINDOW_COVERING_DOWN_GPIO GPIO_NUM_5

/* Active-LOW (relay energizes on GPIO LOW) — common for low-cost
 * opto-isolated relay modules, matches firmware/outlet/'s own default.
 * Always double-check your specific relay module's own documentation —
 * "most common low-cost modules" is not a guarantee for yours. */
#define WINDOW_COVERING_OUTPUT_ACTIVE_LOW 1

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of the covering's own position. GPIO 2 is common on
 * classic ESP32 (WROOM-32) devkits. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Optional RGB status LED — off by default (GPIO_NUM_NC). See
 * firmware/light/main/app_main.cpp's header comment for the full state
 * list and its exact sourcing (connectedhomeip's own lifecycle events +
 * the Identify cluster's own EffectIdentifierEnum), not repeated here. */
#define STATUS_LED_RED_GPIO GPIO_NUM_NC
#define STATUS_LED_GREEN_GPIO GPIO_NUM_NC
#define STATUS_LED_BLUE_GPIO GPIO_NUM_NC
#define STATUS_LED_LEDC_TIMER LEDC_TIMER_1
#define STATUS_LED_LEDC_RED_CHANNEL LEDC_CHANNEL_5
#define STATUS_LED_LEDC_GREEN_CHANNEL LEDC_CHANNEL_6
#define STATUS_LED_LEDC_BLUE_CHANNEL LEDC_CHANNEL_7
#define STATUS_LED_LEDC_MODE LEDC_LOW_SPEED_MODE
#define STATUS_LED_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define STATUS_LED_LEDC_FREQUENCY_HZ 5000
#define STATUS_LED_TICK_MS 20
#define STATUS_LED_PI 3.14159265f

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

/* --- Calibration: how long a full open-to-closed (or closed-to-open)
 * traverse actually takes on your hardware. There is no position sensor —
 * see the header comment above on time-based tracking — so this single
 * number is what all position estimation is built on. Time it with a
 * stopwatch against your real motor and adjust; the shipped default
 * (20 seconds) is a placeholder, not a measurement of anything. */
#define WINDOW_COVERING_FULL_TRAVEL_MS 20000

/* Extra run time added only when the target is a hard 0% or 100% (a full
 * open/close command, not an intermediate percentage) — covers timer
 * jitter/motor-speed variance so the covering reliably reaches its actual
 * physical end stop instead of stopping just short of it. Harmless on
 * motors with their own end-of-travel cutout (most curtain/blind motors
 * have one); on motors without one, don't set this so high that running
 * past the end stop could strain the mechanism. */
#define WINDOW_COVERING_ENDSTOP_OVERSHOOT_MS 2000

/* How often the movement task updates CurrentPositionLiftPercent100ths
 * while moving. Shorter = smoother live position updates for a
 * controller's UI, at the cost of more attribute-report traffic. */
#define WINDOW_COVERING_POSITION_UPDATE_INTERVAL_MS 200

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t window_covering_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Motor output -----------------------------------------------------
 * Mutually exclusive by construction: direction is one of three states,
 * never "both relays on". */
enum class motor_direction_t { STOPPED, OPENING, CLOSING };
static motor_direction_t current_direction = motor_direction_t::STOPPED;

static void set_motor(motor_direction_t direction)
{
    current_direction = direction;
#if WINDOW_COVERING_OUTPUT_ACTIVE_LOW
    gpio_set_level(WINDOW_COVERING_UP_GPIO, direction == motor_direction_t::OPENING ? 0 : 1);
    gpio_set_level(WINDOW_COVERING_DOWN_GPIO, direction == motor_direction_t::CLOSING ? 0 : 1);
#else
    gpio_set_level(WINDOW_COVERING_UP_GPIO, direction == motor_direction_t::OPENING ? 1 : 0);
    gpio_set_level(WINDOW_COVERING_DOWN_GPIO, direction == motor_direction_t::CLOSING ? 1 : 0);
#endif
}

/* --- Movement state, shared between the delegate (which starts/stops
 * movement in response to Matter commands) and movement_task (which
 * actually drives the motor + updates position over time). */
static bool movement_active = false;
static int64_t movement_start_time_us = 0;
static chip::Percent100ths movement_start_position = 0;
static chip::Percent100ths movement_target_position = 0;
static bool movement_is_endstop_target = false; /* true if target is 0 or 10000 */

static void report_position(chip::Percent100ths position)
{
    chip::app::DataModel::Nullable<chip::Percent100ths> val(position);
    WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Set(window_covering_endpoint_id, val);
    MatterReportingAttributeChangeCallback(window_covering_endpoint_id, WindowCovering::Id,
                                           WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id);
}

/* Stops the motor immediately and marks the covering idle at whatever
 * position it was last reported at — called both when a target is reached
 * naturally and when a controller sends StopMotion mid-travel. */
static void stop_movement()
{
    set_motor(motor_direction_t::STOPPED);
    movement_active = false;
    WindowCovering::OperationalStateSet(window_covering_endpoint_id, WindowCovering::OperationalStatus::kLift,
                                        WindowCovering::OperationalState::Stall);
}

/* Single shared task (same "one task handles the whole feature" pattern as
 * every button/debounce task elsewhere in this repo) — wakes on a fixed
 * interval, and while a movement is active, computes the new estimated
 * position from elapsed time and reports it, stopping once the target
 * (plus the end-stop overshoot allowance, if applicable) is reached. */
static void movement_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(WINDOW_COVERING_POSITION_UPDATE_INTERVAL_MS));
        if (!movement_active) {
            continue;
        }

        int64_t elapsed_ms = (esp_timer_get_time() - movement_start_time_us) / 1000;
        int64_t travel_budget_ms = WINDOW_COVERING_FULL_TRAVEL_MS;
        if (movement_is_endstop_target) {
            travel_budget_ms += WINDOW_COVERING_ENDSTOP_OVERSHOOT_MS;
        }

        bool opening = movement_target_position < movement_start_position;
        int32_t total_delta = (int32_t)movement_target_position - (int32_t)movement_start_position;
        /* Percent100ths moved per millisecond of the *uncalibrated* full
         * traverse (WINDOW_COVERING_FULL_TRAVEL_MS, not travel_budget_ms —
         * the overshoot allowance only extends how long the motor keeps
         * running after reaching 0/10000, it doesn't change how fast
         * position is estimated to move). */
        int32_t estimated_delta = (int32_t)((int64_t)total_delta * elapsed_ms / WINDOW_COVERING_FULL_TRAVEL_MS);
        int32_t estimated_position = (int32_t)movement_start_position + estimated_delta;

        bool reached_target = opening ? (estimated_position <= movement_target_position)
                                      : (estimated_position >= movement_target_position);
        bool overshoot_time_elapsed = elapsed_ms >= travel_budget_ms;

        if (reached_target && !movement_is_endstop_target) {
            report_position(movement_target_position);
            stop_movement();
        } else if (overshoot_time_elapsed) {
            /* Either an endstop target's overshoot window elapsed, or (as a
             * safety net) elapsed time ran past the full calibrated
             * traverse without "reaching" the target mathematically —
             * clamp to target and stop rather than let the motor run
             * forever on a miscalibrated WINDOW_COVERING_FULL_TRAVEL_MS. */
            report_position(movement_target_position);
            stop_movement();
        } else {
            chip::Percent100ths clamped = (chip::Percent100ths)(estimated_position < 0 ? 0
                                          : (estimated_position > 10000 ? 10000 : estimated_position));
            report_position(clamped);
        }
    }
}

namespace chip {
namespace app {
namespace Clusters {
namespace WindowCovering {

/* Lift-only — HandleMovement() only ever receives WindowCoveringType::Lift
 * in this file's configuration (Tilt feature isn't enabled, see the header
 * comment on Matter cluster details), so Tilt isn't handled here. */
class OutletWindowCoveringDelegate : public WindowCoveringDelegate {
public:
    CHIP_ERROR HandleMovement(WindowCoveringType type) override
    {
        if (type != WindowCoveringType::Lift) {
            return CHIP_NO_ERROR;
        }

        chip::app::DataModel::Nullable<chip::Percent100ths> target;
        chip::app::DataModel::Nullable<chip::Percent100ths> current;
        WindowCovering::Attributes::TargetPositionLiftPercent100ths::Get(mEndpoint, target);
        WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Get(mEndpoint, current);
        if (target.IsNull() || current.IsNull()) {
            ESP_LOGE(TAG, "HandleMovement: target or current position is null, ignoring");
            return CHIP_NO_ERROR;
        }

        movement_start_position = current.Value();
        movement_target_position = target.Value();
        movement_is_endstop_target = (movement_target_position == 0 || movement_target_position == 10000);
        movement_start_time_us = esp_timer_get_time();
        movement_active = true;

        motor_direction_t direction = (movement_target_position < movement_start_position)
                                          ? motor_direction_t::OPENING
                                          : motor_direction_t::CLOSING;
        set_motor(direction);
        WindowCovering::OperationalStateSet(mEndpoint, WindowCovering::OperationalStatus::kLift,
                                            direction == motor_direction_t::OPENING
                                                ? WindowCovering::OperationalState::MovingUpOrOpen
                                                : WindowCovering::OperationalState::MovingDownOrClose);
        ESP_LOGI(TAG, "Moving %s: %u -> %u (Percent100ths)",
                 direction == motor_direction_t::OPENING ? "up/open" : "down/close",
                 (unsigned)movement_start_position, (unsigned)movement_target_position);
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR HandleStopMotion() override
    {
        ESP_LOGI(TAG, "Stop motion requested");
        stop_movement();
        return CHIP_NO_ERROR;
    }
};

} /* namespace WindowCovering */
} /* namespace Clusters */
} /* namespace app */
} /* namespace chip */

static chip::app::Clusters::WindowCovering::OutletWindowCoveringDelegate window_covering_delegate;

/* --- Status LED pattern engine (see the header comment above) --------- */
typedef enum {
    STATUS_LED_PATTERN_OFF,
    STATUS_LED_PATTERN_SOLID,
    STATUS_LED_PATTERN_BLINK,
    STATUS_LED_PATTERN_BREATHE,
} status_led_pattern_t;

static bool status_led_enabled = false;
static esp_timer_handle_t status_led_timer = NULL;
static uint8_t status_led_r = 0, status_led_g = 0, status_led_b = 0;
static status_led_pattern_t status_led_pattern = STATUS_LED_PATTERN_OFF;
static uint32_t status_led_period_ms = 1000;
static uint32_t status_led_elapsed_ms = 0;
static uint32_t status_led_duration_ms = 0;

static void status_led_set(uint8_t r, uint8_t g, uint8_t b, status_led_pattern_t pattern,
                            uint32_t period_ms, uint32_t duration_ms)
{
    if (!status_led_enabled) {
        return;
    }
    status_led_r = r;
    status_led_g = g;
    status_led_b = b;
    status_led_pattern = pattern;
    status_led_period_ms = period_ms > 0 ? period_ms : 1000;
    status_led_duration_ms = duration_ms;
    status_led_elapsed_ms = 0;
}

static void status_led_off(void)
{
    status_led_set(0, 0, 0, STATUS_LED_PATTERN_OFF, 1000, 0);
}

static void status_led_tick_cb(void *arg)
{
    if (!status_led_enabled) {
        return;
    }
    status_led_elapsed_ms += STATUS_LED_TICK_MS;
    if (status_led_duration_ms > 0 && status_led_elapsed_ms >= status_led_duration_ms) {
        status_led_off();
    }

    float phase = fmodf((float)status_led_elapsed_ms, (float)status_led_period_ms) / (float)status_led_period_ms;
    float brightness;
    switch (status_led_pattern) {
    case STATUS_LED_PATTERN_SOLID:
        brightness = 1.0f;
        break;
    case STATUS_LED_PATTERN_BLINK:
        brightness = (phase < 0.5f) ? 1.0f : 0.0f;
        break;
    case STATUS_LED_PATTERN_BREATHE:
        brightness = (sinf(phase * 2.0f * STATUS_LED_PI - STATUS_LED_PI / 2.0f) + 1.0f) / 2.0f;
        break;
    case STATUS_LED_PATTERN_OFF:
    default:
        brightness = 0.0f;
        break;
    }

    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL, (uint32_t)(status_led_r * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL, (uint32_t)(status_led_g * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL, (uint32_t)(status_led_b * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL);
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
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
        if (event->CHIPoBLEAdvertisingChange.Result == chip::DeviceLayer::kActivity_Started) {
            status_led_set(0, 255, 0, STATUS_LED_PATTERN_BREATHE, 2000, 0); /* Setup mode */
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kSecureSessionEstablished:
        status_led_set(255, 255, 0, STATUS_LED_PATTERN_BREATHE, 1000, 0); /* Setup started */
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete — device is now paired");
        status_led_off(); /* Setup complete */
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        status_led_set(255, 0, 0, STATUS_LED_PATTERN_SOLID, 1000, 0); /* Setup failed */
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
        status_led_set(255, 0, 0, STATUS_LED_PATTERN_BLINK, 1000, 0);
        break;
    case identification::STOP:
        ESP_LOGI(TAG, "Identify stopped on endpoint %u", endpoint_id);
        esp_timer_stop(identify_led_timer);
        gpio_set_level(IDENTIFY_LED_GPIO, 0);
        status_led_off();
        break;
    case identification::EFFECT:
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u",
                 effect_id, effect_variant, endpoint_id);
        if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBlink)) {
            status_led_set(255, 255, 255, STATUS_LED_PATTERN_BLINK, 1000, 1000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBreathe)) {
            status_led_set(255, 255, 255, STATUS_LED_PATTERN_BREATHE, 1000, 15000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kOkay)) {
            status_led_set(0, 255, 0, STATUS_LED_PATTERN_BLINK, 2000, 0);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kChannelChange)) {
            status_led_set(255, 255, 0, STATUS_LED_PATTERN_BLINK, 16000, 0);
        } else {
            status_led_off();
        }
        break;
    }
    return ESP_OK;
}

/* Unlike OnOff/LevelControl (plain ember attributes this repo reacts to via
 * attribute::PRE_UPDATE elsewhere), WindowCovering's movement commands are
 * handled entirely through the Delegate above — see the header comment on
 * why this device type needs one. node::create() still requires an
 * attribute-update callback, so this one is a no-op — kept only because
 * the SDK's signature isn't nullable. */
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
    motor_io_conf.pin_bit_mask = (1ULL << WINDOW_COVERING_UP_GPIO) | (1ULL << WINDOW_COVERING_DOWN_GPIO);
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

    /* 2c. Configure the optional RGB status LED + its pattern-engine timer
     * — only if at least one of its 3 GPIOs is actually wired up. */
    status_led_enabled = (STATUS_LED_RED_GPIO != GPIO_NUM_NC) ||
                          (STATUS_LED_GREEN_GPIO != GPIO_NUM_NC) ||
                          (STATUS_LED_BLUE_GPIO != GPIO_NUM_NC);
    if (status_led_enabled) {
        ledc_timer_config_t status_led_ledc_timer = {};
        status_led_ledc_timer.speed_mode = STATUS_LED_LEDC_MODE;
        status_led_ledc_timer.duty_resolution = STATUS_LED_LEDC_DUTY_RES;
        status_led_ledc_timer.timer_num = STATUS_LED_LEDC_TIMER;
        status_led_ledc_timer.freq_hz = STATUS_LED_LEDC_FREQUENCY_HZ;
        status_led_ledc_timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&status_led_ledc_timer);

        struct {
            ledc_channel_t channel;
            gpio_num_t gpio;
        } status_led_channels[] = {
            { STATUS_LED_LEDC_RED_CHANNEL, STATUS_LED_RED_GPIO },
            { STATUS_LED_LEDC_GREEN_CHANNEL, STATUS_LED_GREEN_GPIO },
            { STATUS_LED_LEDC_BLUE_CHANNEL, STATUS_LED_BLUE_GPIO },
        };
        for (size_t i = 0; i < sizeof(status_led_channels) / sizeof(status_led_channels[0]); i++) {
            if (status_led_channels[i].gpio == GPIO_NUM_NC) {
                continue;
            }
            ledc_channel_config_t status_led_channel = {};
            status_led_channel.gpio_num = status_led_channels[i].gpio;
            status_led_channel.speed_mode = STATUS_LED_LEDC_MODE;
            status_led_channel.channel = status_led_channels[i].channel;
            status_led_channel.intr_type = LEDC_INTR_DISABLE;
            status_led_channel.timer_sel = STATUS_LED_LEDC_TIMER;
            status_led_channel.duty = 0;
            status_led_channel.hpoint = 0;
            ledc_channel_config(&status_led_channel);
        }

        const esp_timer_create_args_t status_led_timer_args = {
            .callback = &status_led_tick_cb,
            .name = "status_led",
        };
        esp_timer_create(&status_led_timer_args, &status_led_timer);
        esp_timer_start_periodic(status_led_timer, STATUS_LED_TICK_MS * 1000);
    }

    /* 3. Build the Matter data model: one node, one Window Covering
     * endpoint (Identify + Groups + WindowCovering clusters). Lift +
     * PositionAwareLift features only — see the header comment on Matter
     * cluster details. Starting position is assumed fully open (0) — see
     * the header comment on time-based position tracking for why this is
     * an assumption, not a measurement, until the first real movement. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    window_covering::config_t covering_config((uint8_t)WindowCovering::EndProductType::kRollerShade);
    covering_config.window_covering.type = (uint8_t)WindowCovering::Type::kRollerShade;
    covering_config.window_covering.feature_flags = chip::to_underlying(WindowCovering::Feature::kLift) |
                                                     chip::to_underlying(WindowCovering::Feature::kPositionAwareLift);
    covering_config.window_covering.features.position_aware_lift.current_position_lift_percent_100ths =
        nullable<uint16_t>(0);
    covering_config.window_covering.features.position_aware_lift.target_position_lift_percent_100ths =
        nullable<uint16_t>(0);
    covering_config.window_covering.delegate = &window_covering_delegate;

    endpoint_t *endpoint = window_covering::create(node, &covering_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create window covering endpoint");
        return;
    }

    window_covering_endpoint_id = endpoint::get_id(endpoint);
    window_covering_delegate.SetEndpoint(window_covering_endpoint_id);
    ESP_LOGI(TAG, "Window covering endpoint id: %u", window_covering_endpoint_id);

    /* CurrentPositionLiftPercent100ths changes every
     * WINDOW_COVERING_POSITION_UPDATE_INTERVAL_MS while moving — deferring
     * its NVS persistence avoids writing flash on every single step of a
     * multi-second traverse. Same call firmware/dimmable-light/ makes for
     * CurrentLevel, for the same reason. */
    attribute_t *current_position_attribute = attribute::get(window_covering_endpoint_id, WindowCovering::Id,
                                                              WindowCovering::Attributes::CurrentPositionLiftPercent100ths::Id);
    attribute::set_deferred_persistence(current_position_attribute);

    xTaskCreate(movement_task, "window_covering_movement", 4096, NULL, 5, NULL);

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

    ESP_LOGI(TAG, "Matter window covering started. Scan the QR code to commission.");
}
