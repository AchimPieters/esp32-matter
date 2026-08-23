/*
 * Minimal Matter Pump — thirtieth device type, and this repo's first over
 * the PumpConfigurationAndControl cluster, and its first plain continuous-
 * control-loop-free device type in three sessions (after Dishwasher/
 * Laundry Washer/Refrigerator's own OperationalState- or hysteresis-loop-
 * driven designs) — a pump reacts to a controller's own On/Off, CurrentLevel,
 * and OperationMode writes directly, with no background task of its own at
 * all.
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
 * --- Endpoint: a complete top-level helper, and the first one this repo
 * has hit whose constructor already sets the Identify type for you -------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * Pump.xml: Identify, On/Off, and PumpConfigurationAndControl are all
 * `<mandatoryConform/>` (a real, non-optional trio, unlike almost every
 * other device type this repo has built, where Identify is the one
 * consistently-optional cluster) — Groups, LevelControl, ScenesManagement,
 * and TemperatureMeasurement/PressureMeasurement/FlowMeasurement (both
 * server AND client side) plus a client-side OccupancySensing are all
 * `<optionalConform/>`. `endpoint::pump::create()` confirmed complete and
 * ready-to-use by reading `esp_matter_endpoint.cpp`'s own `pump::add()`
 * directly: Identify + OnOff (with the On and Toggle commands explicitly
 * added — Off is already part of the base OnOff cluster) +
 * PumpConfigurationAndControl, via `common::create<T>()` (auto-Descriptor).
 * A genuinely new detail for this repo: `pump::config_t`'s own constructor
 * already sets `identify.identify_type =
 * Identify::IdentifyTypeEnum::kActuator` itself — every other device type
 * built so far that needed Identify's type set has done so explicitly at
 * the call site; this is the first top-level helper in this repo whose own
 * constructor does it instead, confirmed by reading the constructor body
 * directly rather than assumed from the pattern of every device type
 * before it. LevelControl (pump speed) is added manually onto this same
 * endpoint afterward, the usual "add extra clusters onto an already-
 * correct endpoint" pattern — confirmed by reading its own legacy
 * `config_t` that, unlike firmware/dimmable-light/'s own LevelControl
 * usage, this cluster's config here has no Lighting-feature field at all
 * (`current_level`/`min_level`/`max_level`/`on_level`/`options` only) —
 * the Lighting feature is specific to light-emitting devices, and a pump
 * genuinely doesn't need it, confirmed by reading the plain legacy
 * `level_control::config_t` directly rather than copying dimmable-light's
 * own lighting-feature setup blindly. Groups/ScenesManagement/
 * Temperature-Pressure-Flow-Measurement/OccupancySensing(client) are all
 * skipped — same "smallest reasonable next step" scope cut this repo
 * applies to every other device type's own optional extras, particularly
 * since this repo already has real, working driver code for all three
 * measurement clusters elsewhere (firmware/temperature-sensor/,
 * firmware/pressure-sensor/) if any of them are ever wanted here later.
 *
 * --- PumpConfigurationAndControl: SPD (ConstantSpeed) only, plain ember
 * attributes with no Delegate at all ------------------------------------
 * Confirmed by reading the legacy `pump_configuration_and_control::
 * config_t` directly: no `delegate` field exists anywhere on this cluster
 * — genuinely different from every Mode/OperationalState-family cluster
 * this repo has built recently, back to the plain
 * `attribute::PRE_UPDATE` + `attribute::update()` pattern OnOff/
 * LevelControl themselves already use. The cluster's own five control-
 * mode features (`ConstantPressure`/`CompensatedPressure`/`ConstantFlow`/
 * `ConstantSpeed`/`ConstantTemperature`) are a "choice, at least 1"
 * conformance set (confirmed directly in the cluster XML) — this file
 * enables only `ConstantSpeed` (SPD), the one feature that maps directly
 * onto a pump's LevelControl-driven speed with no pressure/flow/
 * temperature sensor hardware needed, same "smallest reasonable next
 * step" scoping as every other device type's own first-cut feature
 * choice. `FeatureMap` is confirmed set correctly (no gap): `create()`
 * calls `global::attribute::create_feature_map(cluster,
 * config->feature_flags)` directly, once, with the real value — unlike
 * RefrigeratorAlarm/AirQuality's own documented hardcoded-to-0 gap.
 * `MaxPressure`/`MaxFlow` are left null (not applicable — no pressure/
 * flow feature enabled); `MaxSpeed`/`MinConstSpeed`/`MaxConstSpeed` use a
 * plain 0-100 "percent" scale (the spec places no unit requirement on
 * this field, confirmed by reading the attribute's own `type="uint16"`
 * definition with no accompanying unit metadata) rather than a
 * hardware-specific RPM figure, since no specific real pump motor's rated
 * speed was being modelled. `Capacity` (mandatoryConform but nullable) is
 * left null — no real flow-measurement hardware to report it from,
 * confirmed safe by the attribute's own `nullable="true"` quality.
 * `PumpStatus`/`Speed`/`LifetimeRunningHours`/`Power`/
 * `LifetimeEnergyConsumed` are all `<optionalConform/>` and none are
 * implemented — none are reachable through `pump_configuration_and_
 * control::config_t` at all (would each need a separate
 * `attribute::create_*()` call this file doesn't make), same "smallest
 * reasonable next step" scope cut. This cluster's own rich event set
 * (`SupplyVoltageLow`/`High`, `DryRunning`, `PumpBlocked`,
 * `MotorTemperatureHigh`, `Leakage`, `AirDetection`, etc. — confirmed by
 * reading `esp_matter_event_impl.h`'s own `pump_configuration_and_
 * control::event::` namespace, seventeen real events in total) is not
 * fired anywhere in this file either — every one of them needs real fault-
 * detection hardware (current sensing, pressure transducers, thermal
 * cutouts) this hobby-scale build doesn't have, the same "no sensor, no
 * fabricated fault reporting" honesty precedent firmware/evse/'s own
 * always-`NoError` `FaultState` and firmware/smoke-co-alarm/'s simple
 * heuristic already establish elsewhere in this repo.
 *
 * `OperationMode` (Normal/Minimum/Maximum/Local, all real, spec-defined
 * values) is a plain writable ember attribute a controller sets directly
 * — no command involved — and genuinely drives this file's own output:
 * Normal uses LevelControl's own CurrentLevel as the speed target (the
 * expected common case — a controller's speed slider maps straight onto
 * PWM duty); Minimum/Maximum instead use the feature-configured
 * `MinConstSpeed`/`MaxConstSpeed` bounds, ignoring CurrentLevel entirely,
 * matching the spec's own description of those two modes ("run at the
 * minimum/maximum possible speed... without being stopped" — not
 * "run at whatever level is currently set"). `Local` is accepted (this
 * hobby build has no separate physical control panel to defer to) but
 * behaves identically to Normal, logged as such — an honest, documented
 * scope cut rather than silently ignoring the write. `EffectiveOperation
 * Mode` is written back via `attribute::update()` immediately after
 * `OperationMode` changes, mirroring it 1:1 — correct here since neither
 * `Automatic` nor `LocalOperation` (the two features that would let the
 * device's own internal logic diverge from the commanded mode) is
 * enabled. `EffectiveControlMode` is fixed to `ConstantSpeed` at startup
 * and never changes — the plain `ControlMode` attribute itself is left
 * unimplemented, since with only one control-mode feature enabled there's
 * nothing for a controller to meaningfully choose between (confirmed the
 * attribute is only `<optionalConform/>`, not required regardless of
 * feature selection).
 *
 * --- On/Off + LevelControl: enable relay + PWM speed, driven directly
 * from attribute writes, no background task needed --------------------
 * `PUMP_RELAY_GPIO` (active-LOW, matching this repo's established relay
 * convention) gates the pump motor's own power entirely — separate from
 * the speed signal, matching how a real variable-speed pump/circulator
 * commonly exposes both a plain enable line and a separate 0-10V/PWM
 * speed input. `PUMP_SPEED_PWM_GPIO` is real PWM via ESP-IDF's
 * `driver/ledc.h` (the same LEDC peripheral firmware/dimmable-light/'s
 * own brightness output and firmware/fan/'s own FanControl output already
 * use in this repo), driving whatever speed-controller/VFD hardware a
 * real pump module exposes on its own PWM/analog speed input.
 * `apply_pump_output()` is the single, direct call site both OnOff's and
 * LevelControl's and PumpConfigurationAndControl's own `attribute::
 * PRE_UPDATE` handlers all funnel into — recomputing the real duty cycle
 * from the current OnOff/CurrentLevel/OperationMode state every time any
 * one of them changes, with no periodic task needed at all (unlike every
 * hysteresis-loop device type this repo has built recently) since a
 * pump's speed output is a direct, immediate function of those three
 * attributes, not something that needs to be re-evaluated against a live
 * sensor reading on a timer.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pump/relay/PWM-speed-controller hardware for this
 * device type physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>

static const char *TAG = "matter_pump";

/* --- GPIO pin map ---------------------------------------------------------
 * Non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay/speed-controller module" — polarity/signal type isn't
 * universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define PUMP_RELAY_GPIO GPIO_NUM_16       /* active-LOW — gates pump motor power */
#define PUMP_SPEED_PWM_GPIO GPIO_NUM_17    /* PWM speed signal to a speed controller/VFD */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (PWM) setup — same pattern firmware/dimmable-light/'s own
 * brightness output already establishes: one timer, one channel,
 * LEDC_LOW_SPEED_MODE for portability, 8-bit/0-255 duty resolution. */
#define PUMP_LEDC_TIMER LEDC_TIMER_0
#define PUMP_LEDC_CHANNEL LEDC_CHANNEL_0
#define PUMP_LEDC_FREQUENCY_HZ 25000 /* above the audible range */
#define PUMP_LEDC_DUTY_RESOLUTION LEDC_TIMER_8_BIT

/* MaxSpeed/MinConstSpeed/MaxConstSpeed all use a plain 0-100 "percent"
 * scale — see the header comment above for why. */
#define PUMP_MAX_SPEED_PERCENT 100
#define PUMP_MIN_CONST_SPEED_PERCENT 10
#define PUMP_MAX_CONST_SPEED_PERCENT 100

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t pump_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- Cross-cutting state --------------------------------------------------
 * Written by app_attribute_update_cb()'s own PRE_UPDATE handling, read by
 * apply_pump_output() — see the header comment above for the full detail
 * on how these three combine into a real duty cycle. */
static bool g_on_off = false;
static uint8_t g_current_level = 254; /* LevelControl's own default */
static uint8_t g_operation_mode = chip::to_underlying(PumpConfigurationAndControl::OperationModeEnum::kNormal);

/* Recomputes and applies the real PWM duty from the current OnOff/
 * CurrentLevel/OperationMode state — see the header comment above. */
static void apply_pump_output(void)
{
    uint8_t duty_255 = 0;

    if (g_on_off) {
        switch (g_operation_mode) {
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kMinimum:
            duty_255 = (uint8_t)((PUMP_MIN_CONST_SPEED_PERCENT * 255) / 100);
            break;
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kMaximum:
            duty_255 = (uint8_t)((PUMP_MAX_CONST_SPEED_PERCENT * 255) / 100);
            break;
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kLocal:
            ESP_LOGW(TAG, "OperationMode=Local not supported (no local control panel) — behaving as Normal");
            /* fall through to Normal */
        case (uint8_t)PumpConfigurationAndControl::OperationModeEnum::kNormal:
        default:
            duty_255 = g_current_level; /* CurrentLevel is already 0-254, close enough to 0-255 */
            break;
        }
    }

    gpio_set_level(PUMP_RELAY_GPIO, g_on_off ? 0 : 1); /* active-LOW */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, PUMP_LEDC_CHANNEL, duty_255);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, PUMP_LEDC_CHANNEL);
    ESP_LOGI(TAG, "Pump output: %s, duty %u/255", g_on_off ? "ON" : "OFF", duty_255);
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

/* Reacts to a controller writing OnOff/CurrentLevel/OperationMode —
 * updates the tracked state and immediately recomputes the real output.
 * EffectiveOperationMode is written back right after OperationMode
 * changes — see the header comment above for why that's correct here
 * (neither Automatic nor LocalOperation is enabled). */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != pump_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        g_on_off = val->val.b;
        ESP_LOGI(TAG, "OnOff set to %s", g_on_off ? "true" : "false");
        apply_pump_output();
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        g_current_level = val->val.u8;
        ESP_LOGI(TAG, "CurrentLevel set to %u/254", g_current_level);
        apply_pump_output();
    } else if (cluster_id == PumpConfigurationAndControl::Id &&
               attribute_id == PumpConfigurationAndControl::Attributes::OperationMode::Id) {
        g_operation_mode = val->val.u8;
        ESP_LOGI(TAG, "OperationMode set to %u", g_operation_mode);
        apply_pump_output();

        esp_matter_attr_val_t effective_val = esp_matter_enum8(g_operation_mode);
        attribute::update(pump_endpoint_id, PumpConfigurationAndControl::Id,
                          PumpConfigurationAndControl::Attributes::EffectiveOperationMode::Id, &effective_val);
    }
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

    /* 2. Configure the enable relay — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << PUMP_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(PUMP_RELAY_GPIO, 1); /* active-LOW: 1 = off */

    /* 2b. Configure the speed PWM output. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.timer_num = PUMP_LEDC_TIMER;
    ledc_timer.duty_resolution = PUMP_LEDC_DUTY_RESOLUTION;
    ledc_timer.freq_hz = PUMP_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = PUMP_LEDC_CHANNEL;
    ledc_channel.timer_sel = PUMP_LEDC_TIMER;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = PUMP_SPEED_PWM_GPIO;
    ledc_channel.duty = 0;
    ledc_channel_config(&ledc_channel);

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

    /* 3. Build the Matter data model: one node, one Pump endpoint
     * (Identify + OnOff + PumpConfigurationAndControl via the complete
     * top-level helper, plus LevelControl added manually) — see the
     * header comment above for why. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::pump::config_t pump_config(
        nullable<int16_t>(),                                  /* MaxPressure — not applicable, SPD only */
        nullable<uint16_t>((uint16_t)PUMP_MAX_SPEED_PERCENT),  /* MaxSpeed */
        nullable<uint16_t>());                                 /* MaxFlow — not applicable, SPD only */
    pump_config.pump_configuration_and_control.effective_control_mode =
        chip::to_underlying(PumpConfigurationAndControl::ControlModeEnum::kConstantSpeed);
    pump_config.pump_configuration_and_control.feature_flags =
        cluster::pump_configuration_and_control::feature::constant_speed::get_id();
    pump_config.pump_configuration_and_control.features.constant_speed.min_const_speed =
        nullable<uint16_t>((uint16_t)PUMP_MIN_CONST_SPEED_PERCENT);
    pump_config.pump_configuration_and_control.features.constant_speed.max_const_speed =
        nullable<uint16_t>((uint16_t)PUMP_MAX_CONST_SPEED_PERCENT);

    endpoint_t *endpoint = endpoint::pump::create(node, &pump_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create pump endpoint");
        return;
    }
    pump_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Pump endpoint id: %u", pump_endpoint_id);

    /* 3a. LevelControl (pump speed) — optionalConform, so pump::add()
     * doesn't create it automatically. No Lighting feature needed, see
     * the header comment above. */
    cluster::level_control::config_t level_control_config;
    level_control_config.current_level = nullable<uint8_t>(g_current_level);
    level_control_config.min_level = 1;
    level_control_config.max_level = 254;
    cluster::level_control::create(endpoint, &level_control_config, CLUSTER_FLAG_SERVER);

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

    apply_pump_output(); /* boots off, but sets a real, consistent initial duty of 0 */

    ESP_LOGI(TAG, "Matter pump started. Scan the QR code to commission.");
}
