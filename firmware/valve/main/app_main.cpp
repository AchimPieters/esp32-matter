/*
 * Minimal Matter Water Valve — twentieth device type, and this repo's
 * third genuine Delegate-based cluster after firmware/window-covering/'s
 * WindowCovering and firmware/fan/'s (and firmware/air-purifier/'s)
 * FanControl — but the SDK does noticeably more of the work here than
 * either of those, see below.
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
 * --- Endpoint: esp-matter's own complete top-level helper ------------------
 * `endpoint::water_valve::create()` (device type 0x0042 — Matter's actual
 * device type name is "Water Valve", not "Valve"; esp-matter's own
 * namespace matches the CSA's own naming) confirmed complete/ready-to-use
 * — Identify + ValveConfigurationAndControl, auto-Descriptor via
 * `common::create<T>()` — by reading esp_matter_endpoint.cpp's own
 * `water_valve::add()` directly. Confirmed against the CSA's own
 * data_model/1.6/device_types/WaterValve.xml: Identify + Valve
 * Configuration and Control are the only `<mandatoryConform/>` clusters;
 * Flow Measurement (both server AND client side — a valve can either have
 * its own flow sensor or read one from a bound external device) is
 * `<optionalConform/>` and not implemented here — same "smallest
 * reasonable next step" scoping this repo applies to every other device
 * type's first cut.
 *
 * --- ValveConfigurationAndControl: a Delegate-based cluster, but the SDK
 * owns the open-duration/auto-close timer entirely --------------------
 * Confirmed to be code-driven (a real `valve_configuration_and_control/`
 * folder exists under `data_model_provider/clusters/`), Delegate-based
 * like FanControl/WindowCovering — but reading
 * `ValveConfigurationAndControlCluster.cpp` directly (not just the header)
 * shows this cluster does substantially more of the work internally than
 * either of those two:
 *   - `Open`/`Close` commands, OpenDuration/DefaultOpenDuration/
 *     RemainingDuration/AutoCloseTime bookkeeping, and the actual
 *     1-second countdown timer (`System::Layer().StartTimer(...)`) that
 *     auto-closes the valve when RemainingDuration reaches 0 — including
 *     calling `HandleCloseValve()` on this file's own Delegate when that
 *     happens — are ALL handled inside the cluster itself. This file
 *     doesn't implement any of its own timing logic at all, unlike
 *     firmware/window-covering/'s Delegate (which owns 100% of its own
 *     movement timing) or even firmware/thermostat/'s hysteresis loop.
 *   - What the Delegate genuinely must do: `HandleOpenValve()`/
 *     `HandleCloseValve()` actuate the physical relay; `HandleRemainingDurationTick()`
 *     is an optional once-a-second countdown notification (logged here,
 *     not otherwise acted on).
 *   - One thing the cluster does NOT do automatically, confirmed by
 *     reading `HandleOpenCommand()`/`OpenValve()`/`CloseValve()` end to
 *     end: it sets CurrentState to `Transitioning` before calling the
 *     Delegate, but never automatically advances it to `Open`/`Closed`
 *     afterwards — that's the app's job, via the cluster's own public
 *     `UpdateCurrentState()` method, the exact same "the app should
 *     trigger the state change" responsibility firmware/door-lock/'s own
 *     header comment already documents for DoorLock's LockState. No
 *     position sensor is assumed here either (a plain solenoid valve has
 *     no way to confirm it physically reached the commanded position),
 *     so — same as door-lock's own default — CurrentState is set
 *     OPTIMISTICALLY, immediately after driving the relay, not from any
 *     real feedback.
 *
 * --- A real, previously-found ordering bug, avoided here from the start --
 * `ValveConfigurationAndControlCluster` is registered with the data model
 * provider's registry the exact same way, and at the exact same time
 * (inside its own server-init callback, fired from `esp_matter::start()`'s
 * `chip::Server::GetInstance().Init()` call, not at endpoint/cluster
 * creation time), as `FanControlCluster` — confirmed by reading both
 * integration.cpp files side by side. firmware/fan/'s and firmware/
 * air-purifier/'s own delegate registration (via esp-matter's
 * `FanControl::SetDefaultDelegate()` free function, which has its own
 * map-lookup guard clause) was originally placed BEFORE
 * `esp_matter::start()`, which silently no-oped the whole time — a real
 * bug only found while researching this device type's own registry-
 * lookup call below, now fixed in both of those files (see CLAUDE.md's
 * "Open next steps" for the full story). This file registers its Delegate
 * (via `get_valve_cluster()`'s registry lookup, below — see that
 * function's own comment for why this cluster uses that instead of an
 * equivalent free function) after `esp_matter::start()` from the start.
 *
 * --- Scope: no Level feature, no FlowMeasurement, boot closed ------------
 * `LVL` (partial-open position, e.g. for a proportional irrigation valve)
 * is `<optionalConform/>` and not implemented — most cheap solenoid valve
 * hardware is simple on/off, not proportional, same "smallest reasonable
 * next step" reasoning as firmware/window-covering/'s Lift-only scope.
 * Boots closed (CurrentState/TargetState = Closed), matching every other
 * device type's boot-to-known-safe-state convention in this repo (a valve
 * silently staying open across a power cycle is a worse default than a
 * light staying on).
 *
 * Reference wiring: a relay module (active-LOW, matching firmware/
 * outlet/'s and firmware/door-lock/'s own relay convention) driving a
 * solenoid valve — GND -> relay board -> GPIO, relay's own output side
 * wired to the valve per its own datasheet/instructions.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <app/clusters/valve-configuration-and-control-server/ValveConfigurationAndControlCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_valve";

/* Change this to the GPIO your relay module's input is wired to. Active-LOW
 * — see the header comment above. Reference wiring: GND -> relay board ->
 * GPIO, relay's own output side wired to the solenoid valve. Always check
 * your specific relay module — polarity isn't universal (same caveat
 * firmware/outlet/ already documents for its own relay). */
#define VALVE_RELAY_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t valve_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Drives the relay — active-LOW, see the header comment above. */
static void set_relay(bool open)
{
    gpio_set_level(VALVE_RELAY_GPIO, open ? 0 : 1);
}

/* Registry-lookup-and-cast — same pattern this repo's other code-driven-
 * cluster setters already use. Used both to register the real Delegate
 * (below, in app_main()) and to report CurrentState back afterwards.
 * ValveConfigurationAndControl doesn't have a public header declaring a
 * SetDefaultDelegate()-style free function the way firmware/fan/'s
 * FanControl or firmware/air-purifier/'s ResourceMonitoring do (confirmed
 * by checking — no `integration.h` exists next to esp-matter's own
 * `valve_configuration_and_control/integration.cpp`, unlike
 * `resource_monitor/`'s) — so this goes straight through the cluster's
 * own public `SetDelegate()` method instead, reached the same way
 * `report_current_state()` below reaches `UpdateCurrentState()`. */
static ValveConfigurationAndControlCluster *get_valve_cluster(void)
{
    chip::app::ConcreteClusterPath path(valve_endpoint_id, ValveConfigurationAndControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "ValveConfigurationAndControl cluster not found on endpoint %u", valve_endpoint_id);
        return nullptr;
    }
    return static_cast<ValveConfigurationAndControlCluster *>(iface);
}

/* ValveConfigurationAndControl's real Delegate — see the header comment
 * above for how much of the auto-close-timer/countdown bookkeeping the
 * cluster itself already handles, and why CurrentState is still this
 * file's own responsibility to report back. */
class ValveDelegate : public ValveConfigurationAndControl::Delegate
{
public:
    /* Called by the cluster after it's already set TargetState=Open and
     * CurrentState=Transitioning. No Level feature here, so always
     * returns null (per the Delegate's own documented contract: "shall
     * return current level if supported, otherwise null"). */
    chip::app::DataModel::Nullable<chip::Percent> HandleOpenValve(chip::app::DataModel::Nullable<chip::Percent> level) override
    {
        set_relay(true);
        ESP_LOGI(TAG, "Valve opened");
        report_current_state(ValveConfigurationAndControl::ValveStateEnum::kOpen);
        return chip::app::DataModel::NullNullable;
    }

    CHIP_ERROR HandleCloseValve() override
    {
        set_relay(false);
        ESP_LOGI(TAG, "Valve closed");
        report_current_state(ValveConfigurationAndControl::ValveStateEnum::kClosed);
        return CHIP_NO_ERROR;
    }

    /* Fires once a second while an OpenDuration countdown is running — the
     * cluster's own timer, not this file's. Optional to act on; logged
     * here for visibility. */
    void HandleRemainingDurationTick(uint32_t duration) override
    {
        ESP_LOGI(TAG, "Valve auto-close in %lu s", (unsigned long)duration);
    }

private:
    /* Reports the (optimistic — no position sensor assumed, see the
     * header comment above) real CurrentState back to the cluster via its
     * own public UpdateCurrentState() method — same registry-lookup-and-
     * cast pattern this repo's other code-driven-cluster setters use,
     * just reached through esp-matter's own SetDefaultDelegate()-paired
     * accessor instead of a raw registry lookup, since
     * ValveConfigurationAndControlCluster doesn't expose an equivalent
     * ready-made free function the way firmware/air-purifier/'s
     * ResourceMonitoring::GetClusterInstance() does. */
    void report_current_state(ValveConfigurationAndControl::ValveStateEnum state)
    {
        ValveConfigurationAndControlCluster *cluster = get_valve_cluster();
        if (cluster) {
            cluster->UpdateCurrentState(state);
        }
    }
};

static ValveDelegate valve_delegate;

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

/* Open/Close commands are entirely handled through ValveDelegate above
 * (the cluster's own Delegate mechanism, not the generic attribute::
 * PRE_UPDATE path) — so this is a no-op required by node::create()'s
 * callback signature, same as firmware/fan/'s and firmware/
 * air-purifier/'s own. */
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

    /* 2. Configure the relay output — boots closed (de-energized), same
     * "boot to known safe state" convention as every other device type
     * here. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << VALVE_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    set_relay(false);

    /* 2b. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Water Valve endpoint,
     * boots closed. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::water_valve::config_t valve_config;
    valve_config.valve_configuration_and_control.current_state =
        nullable<uint8_t>(chip::to_underlying(ValveConfigurationAndControl::ValveStateEnum::kClosed));
    valve_config.valve_configuration_and_control.target_state =
        nullable<uint8_t>(chip::to_underlying(ValveConfigurationAndControl::ValveStateEnum::kClosed));
    endpoint_t *endpoint = endpoint::water_valve::create(node, &valve_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create water valve endpoint");
        return;
    }

    valve_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Water valve endpoint id: %u", valve_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* Register the real Delegate — MUST happen after esp_matter::start(),
     * not before: the cluster instance get_valve_cluster() looks up
     * doesn't exist until esp_matter::start()'s own
     * chip::Server::GetInstance().Init() call constructs it, same timing
     * requirement that was just found silently breaking firmware/fan/'s
     * and firmware/air-purifier/'s own (differently-shaped)
     * SetDefaultDelegate() calls when placed too early — see the header
     * comment above for the full story. */
    ValveConfigurationAndControlCluster *cluster = get_valve_cluster();
    if (cluster) {
        cluster->SetDelegate(&valve_delegate);
    }

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter water valve started. Scan the QR code to commission.");
}
