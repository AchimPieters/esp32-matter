/*
 * Minimal Matter Door Lock — a thirteenth device type, and this repo's
 * first where the main command (Lock/Unlock) is handled through a plain
 * C weak-symbol override rather than either the attribute::PRE_UPDATE
 * pattern (OnOff/LevelControl/ColorControl/Thermostat elsewhere in this
 * repo) or a C++ Delegate class (WindowCovering, WebRTCTransportProvider
 * in firmware/camera/).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain,
 * readable C++ — there is no hidden framework layer and no telemetry.
 * Matter is local-first: commissioning happens over Bluetooth + your LAN,
 * and control runs over your local network. Nothing leaves your home
 * unless you choose to add a cloud hub (Google/Apple/Alexa). With Home
 * Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev
 * setup. Works on other ESP32 chips too (C3, C6, S3, H2) — see the
 * README for how to switch target.
 *
 * --- Matter cluster composition ------------------------------------
 * endpoint::door_lock::create() (Identify + DoorLock cluster) is a
 * complete, directly usable esp-matter helper — confirmed by reading
 * esp_matter_endpoint.cpp's own door_lock::add() directly. Its Identify
 * defaults to IdentifyTypeEnum::kAudibleBeep (not kVisibleIndicator like
 * every other device type here) — a real, spec-accurate detail (a lock
 * is often mounted where an LED isn't easily seen, a beep is a more
 * realistic physical Identify signal for this device class) — but this
 * firmware still blinks a visible LED for Identify like every other
 * device type here, since IdentifyType is purely an informational
 * attribute value telling a controller what KIND of feedback to expect,
 * not something that constrains what this app actually does; adding a
 * real piezo buzzer to genuinely beep is a reasonable future addition,
 * not implemented here.
 *
 * DoorLock is not a "code-driven" cluster class (confirmed: no
 * door_lock/ folder under data_model_provider/clusters/, unlike e.g.
 * humidistat/ or smoke_co_alarm/ which are) — so LockState/LockType/
 * ActuatorEnabled/OperatingMode are all plain ember attributes, same
 * category as every other device type's non-code-driven attributes.
 *
 * --- How LockDoor/UnlockDoor commands actually reach this file ------
 * Unlike a plain attribute write, LockDoor/UnlockDoor are real Matter
 * commands, and esp-matter's own door_lock::config_t has an OPTIONAL
 * `delegate` pointer (left null here, deliberately — a Delegate is only
 * needed for the fuller PIN/credential-management feature set, out of
 * scope here per the same "smallest reasonable next step" precedent
 * already used throughout this repo, e.g. firmware/dimmable-light/'s
 * Level-only scope or firmware/window-covering/'s Lift-only scope).
 * With no delegate, connectedhomeip's own DoorLockServer
 * (src/app/clusters/door-lock-server/door-lock-server.cpp,
 * HandleRemoteLockOperation()) still handles the command — PIN
 * validation is skipped entirely when RequirePINforRemoteOperation
 * isn't set (the default, since the PIN feature isn't enabled here) —
 * and then calls one of two plain C functions:
 * `emberAfPluginDoorLockOnDoorLockCommand()` /
 * `emberAfPluginDoorLockOnDoorUnlockCommand()`. These are declared
 * `__attribute__((weak))` with a default implementation that just
 * returns false (confirmed by reading
 * door-lock-server-callback.cpp directly) — door-lock-server.h's own
 * comment above their declaration literally says "should be implemented
 * by the server app". This file provides the real (strong) definitions,
 * which the linker uses instead of the SDK's weak defaults — the
 * intended, documented extension point for exactly this device type,
 * not a workaround. HandleRemoteLockOperation()'s own comment notes
 * "the app should trigger the lock state change" — the framework does
 * NOT update LockState automatically after a successful callback, so
 * this file calls attribute::update() itself, same pattern as every
 * other plain-ember-attribute write elsewhere in this repo.
 *
 * --- DOOR_LOCK_OUTPUT_TYPE: two ways to actually move the lock ------
 * SERVO (default) — a hobby servo (e.g. SG90-class) turning an existing
 *   thumb-turn deadbolt from the inside, the same retrofit approach
 *   countless DIY/ESPHome smart-lock projects use when you don't want
 *   to replace the whole lock body. Driven via ESP-IDF's driver/ledc.h
 *   (the same LEDC PWM peripheral firmware/dimmable-light/ and
 *   firmware/color-light/ already use for their own outputs), at the
 *   standard 50Hz/1-2ms hobby-servo signal.
 * RELAY — drives an electric strike or solenoid (active-LOW, matching
 *   firmware/outlet/'s own relay convention), the same class of
 *   hardware a real commercial electric-strike lock uses.
 *
 * Optional door-position sensor (DOOR_LOCK_POSITION_GPIO, off by
 * default) — a reed switch reading real bolt/latch position, the same
 * simple digital HIGH/LOW technique firmware/contact-sensor/ already
 * uses. Without it, LockState is set OPTIMISTICALLY right after
 * actuating (assume the commanded position was reached) — a real,
 * common pattern for this class of cheap retrofit hardware (no
 * feedback = no way to know for certain), and explicitly allowed by the
 * spec's own framing ("the app should trigger the lock state change...
 * as it may take a while before the lock actually locks/unlocks"). With
 * it, LockState instead reflects the sensor's real reading — closer to
 * how a genuine commercial smart lock verifies its own state.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <app/clusters/door-lock-server/door-lock-server.h>

static const char *TAG = "matter_door_lock";

/* --- DOOR_LOCK_OUTPUT_TYPE — see the header comment above for the full
 * description of each. */
#define DOOR_LOCK_OUTPUT_SERVO 1
#define DOOR_LOCK_OUTPUT_RELAY 2

#define DOOR_LOCK_OUTPUT_TYPE DOOR_LOCK_OUTPUT_SERVO

/* SERVO mode — standard 50Hz hobby-servo PWM signal, 1-2ms pulse width
 * (0.5-2.5ms is the wider "safe" range many servos accept; 1-2ms is the
 * narrower, universally-supported range this uses to avoid ever
 * commanding a cheap servo past its mechanical stop). Angles are
 * degrees, LOCKED/UNLOCKED are deliberately far apart (90 degrees of
 * travel) to reliably turn a thumb-turn between its two end positions —
 * adjust to match your actual thumb-turn's real range of motion. */
#define DOOR_LOCK_SERVO_GPIO GPIO_NUM_4
#define DOOR_LOCK_SERVO_LEDC_TIMER LEDC_TIMER_0
#define DOOR_LOCK_SERVO_LEDC_CHANNEL LEDC_CHANNEL_0
#define DOOR_LOCK_SERVO_LEDC_MODE LEDC_LOW_SPEED_MODE
#define DOOR_LOCK_SERVO_LEDC_FREQUENCY_HZ 50
#define DOOR_LOCK_SERVO_LEDC_DUTY_RES LEDC_TIMER_14_BIT
#define DOOR_LOCK_SERVO_MIN_PULSE_US 1000
#define DOOR_LOCK_SERVO_MAX_PULSE_US 2000
#define DOOR_LOCK_SERVO_LOCKED_DEGREES 0
#define DOOR_LOCK_SERVO_UNLOCKED_DEGREES 90
/* How long to hold PWM output after commanding a move, so the servo
 * actually has time to get there before anything reads LockState back —
 * matters most for the optimistic (no position sensor) case below. */
#define DOOR_LOCK_SERVO_MOVE_TIME_MS 600

/* RELAY mode — active-LOW, matching firmware/outlet/'s own relay
 * convention (common for low-cost opto-isolated relay modules; always
 * check your specific module). */
#define DOOR_LOCK_RELAY_GPIO GPIO_NUM_4

/* Optional door-position sensor — off by default (GPIO_NUM_NC, checked
 * at runtime the same way every other optional GPIO feature in this
 * repo is). Wire a reed switch the same way firmware/contact-sensor/
 * does: one leg to this GPIO (internal pull-up enabled), the other to
 * GND. LOW (closed loop) is treated as "locked" — adjust
 * DOOR_LOCK_POSITION_LOCKED_LEVEL if your specific sensor/mounting is
 * the other way around. */
#define DOOR_LOCK_POSITION_GPIO GPIO_NUM_NC
#define DOOR_LOCK_POSITION_LOCKED_LEVEL 0

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t door_lock_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static bool door_lock_position_sensor_enabled = false;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

#if DOOR_LOCK_OUTPUT_TYPE == DOOR_LOCK_OUTPUT_SERVO
/* Converts an angle (degrees) to the matching LEDC duty for a 50Hz/
 * 1-2ms-pulse hobby servo, at DOOR_LOCK_SERVO_LEDC_DUTY_RES resolution. */
static void servo_set_angle(int degrees)
{
    if (degrees < 0) {
        degrees = 0;
    } else if (degrees > 180) {
        degrees = 180;
    }
    uint32_t pulse_us = DOOR_LOCK_SERVO_MIN_PULSE_US +
        (uint32_t)((DOOR_LOCK_SERVO_MAX_PULSE_US - DOOR_LOCK_SERVO_MIN_PULSE_US) * (degrees / 180.0f));
    uint32_t period_us = 1000000 / DOOR_LOCK_SERVO_LEDC_FREQUENCY_HZ;
    uint32_t max_duty = (1u << DOOR_LOCK_SERVO_LEDC_DUTY_RES) - 1;
    uint32_t duty = (uint32_t)(((uint64_t)pulse_us * max_duty) / period_us);
    ledc_set_duty(DOOR_LOCK_SERVO_LEDC_MODE, DOOR_LOCK_SERVO_LEDC_CHANNEL, duty);
    ledc_update_duty(DOOR_LOCK_SERVO_LEDC_MODE, DOOR_LOCK_SERVO_LEDC_CHANNEL);
}
#endif

/* Only ever called when door_lock_position_sensor_enabled is true (set
 * once at boot in app_main() via a runtime comparison against
 * GPIO_NUM_NC — not a preprocessor #if, since GPIO_NUM_* values are
 * plain C enum constants whose numeric value differs per target chip;
 * see firmware/addressable-light/'s header comment for the exact class
 * of bug that would cause if done as an #if instead). */
static bool read_door_position_locked(void)
{
    return gpio_get_level(DOOR_LOCK_POSITION_GPIO) == DOOR_LOCK_POSITION_LOCKED_LEVEL;
}

/* Plain ember attribute (DoorLock is not a "code-driven" cluster class —
 * see the header comment above), so a direct attribute::update() call. */
static void update_lock_state(DlLockState state)
{
    esp_matter_attr_val_t val = esp_matter_nullable_uint8(nullable<uint8_t>(chip::to_underlying(state)));
    attribute::update(door_lock_endpoint_id, DoorLock::Id, DoorLock::Attributes::LockState::Id, &val);
}

/* Actually moves the lock hardware, then updates LockState — either from
 * the optional position sensor's real reading, or optimistically (assume
 * the commanded position was reached) if no sensor is wired up. See the
 * header comment above for why optimistic-by-default is a normal,
 * spec-allowed pattern for this class of hardware, not a shortcut. */
static void actuate_lock(bool lock)
{
#if DOOR_LOCK_OUTPUT_TYPE == DOOR_LOCK_OUTPUT_SERVO
    servo_set_angle(lock ? DOOR_LOCK_SERVO_LOCKED_DEGREES : DOOR_LOCK_SERVO_UNLOCKED_DEGREES);
    vTaskDelay(pdMS_TO_TICKS(DOOR_LOCK_SERVO_MOVE_TIME_MS));
#else /* DOOR_LOCK_OUTPUT_RELAY */
    gpio_set_level(DOOR_LOCK_RELAY_GPIO, lock ? 0 : 1); /* active-LOW */
#endif

    if (door_lock_position_sensor_enabled) {
        update_lock_state(read_door_position_locked() ? DlLockState::kLocked : DlLockState::kUnlocked);
    } else {
        update_lock_state(lock ? DlLockState::kLocked : DlLockState::kUnlocked);
    }
    ESP_LOGI(TAG, "Lock %s (%s)", lock ? "LOCKED" : "UNLOCKED",
             door_lock_position_sensor_enabled ? "sensor-confirmed" : "optimistic");
}

/* Required, NOT weak — unlike emberAfPluginDoorLockOnDoorLockCommand/
 * OnDoorUnlockCommand below (which the SDK stubs out with a default
 * `return false` when an app doesn't define them, confirmed via
 * door-lock-server-callback.cpp's own weak defaults), this Init callback
 * has a plain, non-weak prototype in zzz_generated/app-common/
 * app-common/zap-generated/callback.h — omitting it is a hard link error,
 * confirmed by an actual Docker build (`undefined reference to
 * emberAfDoorLockClusterInitCallback`), not assumed. esp-matter's own
 * door_lock::function_list (data_model/legacy/esp_matter_cluster.cpp)
 * wires this in as the cluster's CLUSTER_FLAG_INIT_FUNCTION entry, so it
 * runs once when the DoorLock cluster on this endpoint starts up.
 * DoorLockServer::Instance().InitServer() is documented in
 * door-lock-server.h as "a deprecated alias for InitEndpoint with no
 * delegate" — exactly matching this file's own choice to leave
 * config_t's `delegate` field null (no PIN/credential/schedule features
 * enabled), and required regardless of that: it registers this
 * endpoint's per-endpoint server state (lockout timestamp, wrong-code
 * attempt counter) that the cluster's internal command-handling logic
 * expects to already exist. Matches the SDK's own
 * examples/door_lock/main/lock/door_lock_callbacks.cpp reference
 * one-for-one. LockState itself is left alone here — already set to its
 * boot default via door_lock_config.door_lock.lock_state below, no need
 * to set it a second time through DoorLockServer's own SetLockState(). */
void emberAfDoorLockClusterInitCallback(chip::EndpointId endpoint)
{
    DoorLockServer::Instance().InitServer(endpoint);
}

/* The actual LockDoor/UnlockDoor command handlers — see the header
 * comment above for why these are plain weak-symbol overrides, not a
 * Delegate class or an attribute::PRE_UPDATE reaction. Declared exactly
 * as door-lock-server.h declares them (its own Nullable/Optional/
 * OperationErrorEnum `using` aliases are visible here too, since that
 * header's own top-level `using` directives aren't namespace-scoped —
 * confirmed by reading that header directly rather than guessing at the
 * right qualification). PIN codes are ignored — this device doesn't
 * enable the PIN/credential feature, so pinCode is never populated by a
 * real controller anyway (RequirePINforRemoteOperation defaults off). */
bool emberAfPluginDoorLockOnDoorLockCommand(chip::EndpointId endpointId, const Nullable<chip::FabricIndex> &fabricIdx,
                                            const Nullable<chip::NodeId> &nodeId, const Optional<chip::ByteSpan> &pinCode,
                                            OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    ESP_LOGI(TAG, "LockDoor command received on endpoint %u", endpointId);
    actuate_lock(true);
    return true;
}

bool emberAfPluginDoorLockOnDoorUnlockCommand(chip::EndpointId endpointId, const Nullable<chip::FabricIndex> &fabricIdx,
                                              const Nullable<chip::NodeId> &nodeId, const Optional<chip::ByteSpan> &pinCode,
                                              OperationErrorEnum &err)
{
    (void)fabricIdx;
    (void)nodeId;
    (void)pinCode;
    ESP_LOGI(TAG, "UnlockDoor command received on endpoint %u", endpointId);
    actuate_lock(false);
    return true;
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

/* LockState is never directly writable by a controller (only via the
 * LockDoor/UnlockDoor commands above, or locally) — nothing for this
 * device to react to here, same no-op shape as
 * firmware/temperature-sensor/'s own app_attribute_update_cb. */
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

    /* 2. Configure the lock output. */
#if DOOR_LOCK_OUTPUT_TYPE == DOOR_LOCK_OUTPUT_SERVO
    ledc_timer_config_t servo_ledc_timer = {};
    servo_ledc_timer.speed_mode = DOOR_LOCK_SERVO_LEDC_MODE;
    servo_ledc_timer.duty_resolution = DOOR_LOCK_SERVO_LEDC_DUTY_RES;
    servo_ledc_timer.timer_num = DOOR_LOCK_SERVO_LEDC_TIMER;
    servo_ledc_timer.freq_hz = DOOR_LOCK_SERVO_LEDC_FREQUENCY_HZ;
    servo_ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&servo_ledc_timer);

    ledc_channel_config_t servo_ledc_channel = {};
    servo_ledc_channel.gpio_num = DOOR_LOCK_SERVO_GPIO;
    servo_ledc_channel.speed_mode = DOOR_LOCK_SERVO_LEDC_MODE;
    servo_ledc_channel.channel = DOOR_LOCK_SERVO_LEDC_CHANNEL;
    servo_ledc_channel.intr_type = LEDC_INTR_DISABLE;
    servo_ledc_channel.timer_sel = DOOR_LOCK_SERVO_LEDC_TIMER;
    servo_ledc_channel.duty = 0;
    servo_ledc_channel.hpoint = 0;
    ledc_channel_config(&servo_ledc_channel);
    servo_set_angle(DOOR_LOCK_SERVO_LOCKED_DEGREES); /* boots locked, see step 3's system_mode-equivalent reasoning below */
#else /* DOOR_LOCK_OUTPUT_RELAY */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << DOOR_LOCK_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(DOOR_LOCK_RELAY_GPIO, 1); /* active-LOW: idle HIGH = locked/de-energized */
#endif

    /* 2b. Configure the optional door-position sensor. */
    door_lock_position_sensor_enabled = (DOOR_LOCK_POSITION_GPIO != GPIO_NUM_NC);
    if (door_lock_position_sensor_enabled) {
        gpio_config_t position_io_conf = {};
        position_io_conf.pin_bit_mask = (1ULL << DOOR_LOCK_POSITION_GPIO);
        position_io_conf.mode = GPIO_MODE_INPUT;
        position_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&position_io_conf);
    }

    /* 2c. Configure the identify LED + its blink timer (not started yet —
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

    /* 3. Build the Matter data model: one node, one Door Lock endpoint
     * (Identify + DoorLock cluster) — see the header comment above for
     * its exact composition and sourcing. Boots LOCKED (lock_state =
     * kLocked) rather than an unknown/null state — matching every other
     * device type's boot-to-known-safe-state convention in this repo
     * (e.g. firmware/thermostat/'s boot-to-Off), and the obviously safer
     * default for something securing a door. lock_type = kDeadBolt
     * matches the most common real retrofit target (an existing deadbolt
     * thumb-turn); adjust if yours is genuinely a different mechanism. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    door_lock::config_t door_lock_config;
    door_lock_config.door_lock.lock_state = nullable<uint8_t>(chip::to_underlying(DlLockState::kLocked));
    door_lock_config.door_lock.lock_type = chip::to_underlying(chip::app::Clusters::DoorLock::DlLockType::kDeadBolt);
    door_lock_config.door_lock.actuator_enabled = true;

    endpoint_t *endpoint = door_lock::create(node, &door_lock_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create door lock endpoint");
        return;
    }
    door_lock_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Door lock endpoint id: %u", door_lock_endpoint_id);

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

    ESP_LOGI(TAG, "Matter door lock started. Scan the QR code to commission.");
}
