/*
 * Minimal Matter Fan — sixteenth device type, and this repo's second
 * genuine Delegate-based cluster after firmware/window-covering/'s
 * WindowCovering (see below for how FanControl's Delegate differs).
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
 * `endpoint::fan::create()` (device type 0x002B, Identify + Groups +
 * FanControl — confirmed directly against the CSA's own
 * data_model/1.6/device_types/Fan.xml: those three are the only
 * `<mandatoryConform/>` clusters; On/Off is listed too but only
 * `<optionalConform/>`, deliberately not added here — see the header comment
 * on scope below) is a complete, ready-to-use top-level helper, confirmed to
 * create the endpoint's Descriptor cluster automatically (via
 * `common::create<T>()`'s shared internal path, read directly in
 * esp_matter_endpoint.cpp) — same "use the complete helper, not a hand-
 * assembled one" lesson firmware/occupancy-sensor/ already applied this
 * session, after firmware/color-light/'s and firmware/addressable-light/'s
 * hand-assembled endpoints were found to be missing that exact cluster
 * during this repo's own Apple Home hardware testing.
 *
 * --- FanControl: a Delegate-based cluster, not a plain setter one ----------
 * Confirmed by reading esp-matter's own source directly: a real
 * `fan_control/` folder exists under
 * `components/esp_matter/data_model_provider/clusters/`, backed by
 * connectedhomeip's own `FanControlCluster`
 * (`src/app/clusters/fan-control-server/FanControlCluster.h`) — so far this
 * matches the same "code-driven, not generic ember attribute store"
 * situation firmware/contact-sensor/'s BooleanState and
 * firmware/occupancy-sensor/'s OccupancySensing already hit. But unlike
 * those two (a plain `SetX()` method looked up via the data model
 * provider's registry is enough), FanControl's own header
 * (`fan-control-delegate.h`) shows it's built around a real
 * `FanControl::Delegate` interface an app is expected to subclass and
 * register via `FanControl::SetDefaultDelegate()` — the same Delegate
 * pattern firmware/window-covering/'s WindowCovering cluster already uses in
 * this repo, just for a different cluster. `HandleStep()` is pure virtual
 * (must be implemented even though this file doesn't enable the Step
 * feature — it simply returns UnsupportedCommand); `OnFanDriveStateChanged()`
 * has an empty default body but is where this file actually hooks in real
 * PWM output — it fires with a `FanDriveState` snapshot (mode,
 * percentSetting, percentCurrent, ...) after FanMode or PercentSetting
 * changes settle, confirmed by reading FanControlCluster.cpp's own
 * `SetFanMode()`/`ApplyFanModeSideEffects()`/`SetPercentSetting()`
 * directly: writing FanMode always cascades into PercentSetting too (Off->
 * 0%, Low->33%, Medium->66%, High->100%), so this file only needs to react
 * to PercentSetting in the Delegate — it never needs to separately handle
 * FanMode's own value. Critically, PercentCurrent is NOT updated
 * automatically by any of that — an app is expected to report back what
 * the fan is actually doing. Two real, sequential gotchas here, each
 * caught by an actual Docker build failure rather than assumed from
 * reading source alone: first, a *compile* error trying just
 * `fan-control-delegate.h` (fixed by switching to
 * `app/clusters/fan-control-server/CodegenIntegration.h`, which declares
 * `SetDefaultDelegate()` and a full `Attributes::X::Get()/Set()` namespace
 * — including `PercentCurrent::Set()`); second, a *link* error even after
 * that, `undefined reference to ...PercentCurrent::Set`. Root cause:
 * connectedhomeip's own generic
 * `src/app/clusters/fan-control-server/CodegenIntegration.cpp` — which is
 * where those declared symbols' actual *definitions* live — is NOT what
 * esp-matter's build compiles. esp-matter substitutes its own
 * `components/esp_matter/data_model_provider/clusters/fan_control/
 * integration.cpp` instead (same "esp-matter has its own integration file
 * per code-driven cluster" pattern firmware/occupancy-sensor/'s
 * OccupancySensing already established) — but unlike that file, this one
 * only implements cluster registration and `SetDefaultDelegate()`, NOT any
 * of the `Attributes::X::Set()` free functions `CodegenIntegration.h`
 * declares. So the header include was fine; the free-function *call* was
 * the mistake. Fixed with the same registry-lookup-and-cast pattern
 * firmware/contact-sensor/'s/firmware/occupancy-sensor/'s own setters
 * already use instead: look the live `FanControlCluster` instance up via
 * the data model provider's registry and call its native
 * `SetPercentCurrent()` C++ method directly. No explicit Matter stack lock
 * around that call, unlike those two files' debounce-task callers —
 * `OnFanDriveStateChanged()` runs synchronously on the Matter stack's own
 * thread as part of attribute-write processing, not a separate FreeRTOS
 * task (matching firmware/window-covering/'s own no-lock precedent for
 * its Delegate-driven `report_position()` call, for the same reason).
 *
 * --- Scope: PercentSetting/PercentCurrent only, no MultiSpeed/Auto/etc. ----
 * Every one of FanControl's six features (MultiSpeed/Auto/Rocking/Wind/
 * Step/AirflowDirection) is independently `<optionalConform/>` per the
 * cluster XML — none required — so this file implements none of them,
 * driving the fan purely through the cluster's own base-mandatory
 * PercentSetting/PercentCurrent (0-100%), the natural fit for a PWM-driven
 * DC fan (no discrete speed taps to model, unlike a real multi-tap AC fan
 * motor, which is exactly what the optional MultiSpeed feature is for) —
 * same "smallest reasonable next step" scoping this repo has applied to
 * every other device type's first cut (dimmable-light's Level-only scope,
 * window-covering's Lift-only scope, thermostat's Heat+Cool-only scope,
 * etc.). FanModeSequence is set to OffLowMedHigh (confirmed against the
 * cluster XML's own FanModeSequenceEnum — this is the only listed value
 * that doesn't require the Auto feature and offers three real speed steps
 * rather than just two) so a controller's own FanMode picker still offers
 * Low/Medium/High shortcuts on top of the continuous percent slider that
 * PercentSetting always provides regardless of FanModeSequence.
 *
 * --- Output: real PWM via LEDC, same peripheral as firmware/dimmable-light/
 * PercentSetting (0-100) is used directly as an LEDC duty percentage — a
 * MOSFET or fan-speed-controller board's PWM input is the common way to
 * speed-control a small 12V/24V DC fan from a microcontroller, the same
 * "PWM to a driver stage" approach firmware/dimmable-light/ already uses
 * for LED brightness. On/Off is folded into percent here (0% = off) rather
 * than a separate OnOff cluster (left out — see the header comment above on
 * why On/Off is only optionalConform for this device type).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <app/clusters/fan-control-server/CodegenIntegration.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_fan";

/* Change this to the GPIO your fan's PWM/speed-control input is wired to
 * (typically a MOSFET gate, or a small fan-speed-controller board's PWM
 * pin). Needs to be an LEDC-capable output pin — true for nearly every GPIO
 * on these chips except input-only ones (GPIO 34-39 on classic ESP32).
 * GPIO 2 is common on classic ESP32 (WROOM-32) devkits. Adjust to match
 * your board. */
#define FAN_PWM_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself. GPIO 4 is commonly unused on classic ESP32 (WROOM-32) devkits.
 * Adjust to match your board. Deliberately plain gpio_set_level(), not
 * LEDC — the identify blink is just on/off, no need for a second PWM
 * channel. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (LED Control / PWM) peripheral setup for the fan output — same
 * settings firmware/dimmable-light/ already uses for its own single
 * channel (see that file's header comment for why): low-speed mode for
 * portability across every module this repo targets, auto clock source,
 * 5 kHz. PercentSetting's own 0-100 range is used directly as the duty
 * resolution here (7-bit = 0-127 would lose precision on a 0-100 scale;
 * 8-bit = 0-255 comfortably covers it with headroom), so no separate
 * remapping constant is needed the way firmware/dimmable-light/ discusses
 * for LevelControl's 1-254 range. */
#define FAN_LEDC_TIMER LEDC_TIMER_0
#define FAN_LEDC_CHANNEL LEDC_CHANNEL_0
#define FAN_LEDC_MODE LEDC_LOW_SPEED_MODE
#define FAN_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define FAN_LEDC_FREQUENCY_HZ 25000 /* 25 kHz — above the audible range, the same PWM frequency most 4-wire PC/DC fan speed inputs expect */

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t fan_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Drives the LEDC duty from a 0-100 percent value — PercentSetting's own
 * range, used directly with no remapping (see the header comment on
 * output). */
static void set_output(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL, duty);
    ledc_update_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL);
}

/* FanControl's real Delegate — see the header comment on why this cluster
 * needs one (unlike firmware/contact-sensor/'s/firmware/occupancy-sensor/'s
 * plain registry-lookup setters). */
class FanDelegate : public FanControl::Delegate
{
public:
    chip::Protocols::InteractionModel::Status HandleStep(FanControl::StepDirectionEnum direction, bool wrap,
                                                          bool lowestOff) override
    {
        /* Step feature not implemented (see the header comment on scope) —
         * this method is pure virtual in the base Delegate class, so it
         * must exist even though nothing ever enables the feature that
         * would let a controller actually send this command. */
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;
    }

    /* Fires after FanMode or PercentSetting settle (see the header comment
     * on FanControl's own FanMode->PercentSetting cascade — reacting to
     * percentSetting alone covers both paths). Drives the real PWM output,
     * then reports back what the fan is now actually doing via
     * PercentCurrent. A real Docker link error (not just a compile error)
     * caught a wrong assumption here: `FanControl::Attributes::
     * PercentCurrent::Set()` is declared in connectedhomeip's own
     * CodegenIntegration.h, but its actual *definition* lives in
     * connectedhomeip's CodegenIntegration.cpp — which esp-matter's build
     * does NOT compile (esp-matter substitutes its own
     * `data_model_provider/clusters/fan_control/integration.cpp` instead,
     * which only implements `SetDefaultDelegate()`, not any
     * `Attributes::X::Set()` free functions) — so the symbol simply isn't
     * anywhere in the link. Fixed with the same registry-lookup pattern
     * firmware/contact-sensor/'s/firmware/occupancy-sensor/'s own setters
     * already use: look the live cluster instance up directly and call its
     * native `SetPercentCurrent()` C++ method. No explicit Matter stack
     * lock here, unlike those two files' debounce-task callers —
     * `OnFanDriveStateChanged()` runs synchronously on the Matter stack's
     * own thread as part of attribute-write processing, not a separate
     * FreeRTOS task. */
    void OnFanDriveStateChanged(const FanControl::FanDriveState &newState) override
    {
        uint8_t percent = newState.percentSetting.IsNull() ? 0 : newState.percentSetting.Value();
        set_output(percent);

        chip::app::ConcreteClusterPath path(fan_endpoint_id, FanControl::Id);
        chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
        if (iface) {
            static_cast<FanControlCluster *>(iface)->SetPercentCurrent(percent);
        } else {
            ESP_LOGE(TAG, "FanControl cluster not found on endpoint %u", fan_endpoint_id);
        }
        ESP_LOGI(TAG, "Fan mode %u, percent set to %u%%", chip::to_underlying(newState.mode), percent);
    }
};

static FanDelegate fan_delegate;

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

/* Called whenever a controller changes an attribute on this device. Fan
 * speed/mode changes are entirely handled through FanDelegate above (the
 * cluster's own Delegate mechanism, not the generic attribute::PRE_UPDATE
 * path) — so this is a no-op required by node::create()'s callback
 * signature, same as firmware/contact-sensor/'s and
 * firmware/occupancy-sensor/'s. */
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

    /* 2. Configure the fan PWM output via LEDC (hardware PWM) — a timer
     * (shared clock/frequency/resolution) plus a channel (the actual GPIO
     * + duty register) per ESP-IDF's driver/ledc.h API. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = FAN_LEDC_MODE;
    ledc_timer.duty_resolution = FAN_LEDC_DUTY_RES;
    ledc_timer.timer_num = FAN_LEDC_TIMER;
    ledc_timer.freq_hz = FAN_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = FAN_PWM_GPIO;
    ledc_channel.speed_mode = FAN_LEDC_MODE;
    ledc_channel.channel = FAN_LEDC_CHANNEL;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = FAN_LEDC_TIMER;
    ledc_channel.duty = 0;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
    set_output(0); /* boots off, same convention every other device type here follows */

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

    /* 3. Build the Matter data model: one node, one Fan endpoint (Identify
     * + Groups + FanControl). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::fan::config_t fan_config;
    /* OffLowMedHigh — see the header comment on scope for why this specific
     * FanModeSequenceEnum value (three real speed steps, no Auto feature
     * required). */
    fan_config.fan_control.fan_mode_sequence =
        chip::to_underlying(FanControl::FanModeSequenceEnum::kOffLowMedHigh);
    endpoint_t *endpoint = endpoint::fan::create(node, &fan_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create fan endpoint");
        return;
    }

    fan_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Fan endpoint id: %u", fan_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* Register the real Delegate — MUST happen after esp_matter::start(),
     * not before. A real, previously-wrong assumption in this file was
     * caught by re-reading esp-matter's own fan_control/integration.cpp
     * closely while researching firmware/valve/'s own, structurally
     * identical SetDefaultDelegate() pattern: esp-matter's own
     * `FanControl::SetDefaultDelegate()` looks up this endpoint's cluster
     * instance in a map keyed by endpoint ID and silently no-ops
     * (`VerifyOrReturn(it != gServers.end() && ...)`) if that instance
     * doesn't exist yet — and it's only actually constructed inside
     * `ESPMatterFanControlClusterServerInitCallback()`, which fires as
     * part of `chip::Server::GetInstance().Init()`, called from
     * `esp_matter::start()` itself, not from `endpoint::fan::create()`
     * earlier in this function. Calling `SetDefaultDelegate()` before
     * `start()` (this file's original ordering) was therefore a silent
     * no-op the whole time: the Delegate was never actually attached, so
     * `OnFanDriveStateChanged()` never fired and the physical PWM output
     * never moved in response to a controller's FanMode/PercentSetting
     * commands — undetected until now because this device type has only
     * ever been Docker build-verified, not hardware-tested (compiling
     * cleanly says nothing about this ordering bug). Without this fix,
     * FanControlCluster still accepts and stores writes internally (via
     * its own wrapper, which tolerates a null wrapped delegate), so a
     * controller would see PercentSetting change with no error — just no
     * physical effect and a PercentCurrent that never updates. */
    FanControl::SetDefaultDelegate(fan_endpoint_id, &fan_delegate);

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started — see
     * check_factory_reset_boot_count()'s comment on why this can't
     * happen any earlier. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter fan started. Scan the QR code to commission.");
}
