/*
 * Minimal Matter Generic Switch — twenty-sixth device type, and this
 * repo's first "smart button" accessory: a plain momentary pushbutton
 * that fires real InitialPress/LongPress/ShortRelease/LongRelease/
 * MultiPressOngoing/MultiPressComplete events for automations, rather
 * than sending a command to a bound target the way firmware/switch/'s
 * own On/Off Switch does.
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
 * --- Endpoint + cluster: esp-matter's own complete top-level helper,
 * genuinely simpler than the last several device types in this repo -----
 * `endpoint::generic_switch::create()` (device type 0x000F) confirmed
 * complete/ready-to-use by reading esp_matter_endpoint.cpp's own
 * `generic_switch::add()` directly: Identify + Switch, via `common::
 * create<T>()` (auto-Descriptor). Matches the CSA's own
 * data_model/1.6/device_types/GenericSwitch.xml exactly — those two are
 * the ONLY clusters listed, both `<mandatoryConform/>`. Unlike
 * firmware/extractor-hood/'s, firmware/water-heater/'s, and
 * firmware/evse/'s own endpoint helpers, Identify IS wired in
 * automatically here (confirmed directly in `generic_switch::add()`) —
 * no extra manual `cluster::identify::create()` call needed, the first
 * time in several device types this repo hasn't needed one.
 *
 * Confirmed by reading `esp_matter_cluster.cpp`'s own `switch_cluster::
 * create()` directly that Switch is a PLAIN ember-attribute cluster —
 * NumberOfPositions/CurrentPosition are ordinary attributes, no
 * `config->delegate` field exists on `switch_cluster::config_t` at all —
 * but the six press/release/multi-press EVENTS are generated through a
 * real, registry-registered `chip::app::Clusters::SwitchCluster`
 * (confirmed to be a `DefaultServerCluster`, the same
 * `ServerClusterInterface` category as `FanControlCluster`/
 * `ValveConfigurationAndControlCluster`) via its own public
 * `OnInitialPress()`/`OnLongPress()`/`OnShortRelease()`/`OnLongRelease()`/
 * `OnMultiPressOngoing()`/`OnMultiPressComplete()` methods — reached the
 * same registry-lookup-and-cast way this repo's other Delegate-free-but-
 * event-generating clusters already are. None of these methods touch
 * CurrentPosition at all (confirmed by reading `SwitchCluster.cpp`
 * directly — each one only calls `GenerateEvent()`), so this file calls
 * `SetCurrentPosition()` separately, right alongside each event, to keep
 * the attribute honestly in sync with what actually happened.
 *
 * --- Features: the full "smart button" set, no ActionSwitch -------------
 * `momentary_switch` (MS, the base momentary-vs-latching choice) +
 * `momentary_switch_release` (MSR) + `momentary_switch_long_press` (MSL)
 * + `momentary_switch_multi_press` (MSM) are all enabled — a complete
 * single/double/triple/quadruple-click-plus-long-press button, the same
 * capability set real commercial "smart button" accessories (Hue
 * Dimmer Switch, Aqara buttons, etc.) expose, and what a controller like
 * Apple Home/Home Assistant expects to offer as automation triggers.
 * `action_switch` (AS — a scene-selector-style switch with a fixed list
 * of named actions rather than free-form multi-press counting) is NOT
 * implemented — confirmed by reading the cluster XML's own
 * `MultiPressOngoing` conformance (`MSM AND NOT AS`) that AS and MSM are
 * mutually exclusive in practice for that event anyway, and a plain
 * multi-click button is the more common, more directly automatable
 * hobbyist use case — same "smallest reasonable next step" scoping this
 * repo applies to every other device type's own feature choices.
 * `latching_switch` (a toggle/rocker reporting a fixed position, e.g. 0/1
 * for a wall switch) is not implemented either — this file models a
 * momentary pushbutton, not a latching one; the two features are mutually
 * exclusive per the cluster's own `VALIDATE_FEATURES_EXACT_ONE` check in
 * `switch_cluster::create()`, confirmed by reading that function
 * directly, so there's no way to offer both from one build anyway.
 *
 * --- Press-timing state machine: real engineering this repo had to do
 * itself — the SDK provides no reference driver for it ------------------
 * Checked directly: no example in the esp-matter/connectedhomeip SDK
 * (`examples/chef/common/clusters/switch/` included) implements a real
 * GPIO-to-press-timing state machine — chef's own `SwitchManager.cpp` is
 * a test-event-injection harness (simulating presses from a script), not
 * a driver reading a real button. The cluster's own `TestSwitchCluster.cpp`
 * likewise only exercises each `OnXxx()` call in isolation, confirming
 * the actual debounce/long-press/multi-press timing logic is genuinely
 * the application's own responsibility, not something the cluster
 * provides. This file's own state machine (in `switch_task` below) is
 * the same industry-standard technique virtually every DIY multi-click
 * button library (e.g. ESPHome's own button component) uses, not
 * invented here: a periodic poll with a simple N-consistent-samples
 * debounce (same "one sample is too fragile, cheap tactile switches
 * bounce" reasoning firmware/switch/'s own debounce comment already
 * documents, here via continuous polling instead of firmware/switch/'s
 * ISR+queue shape, since this state machine also needs to track ongoing
 * *duration*, not just react to one debounced edge); a `GENERIC_SWITCH_
 * LONG_PRESS_MS` (1000 ms) hold threshold, matching the ~1 second long-
 * press convention most consumer smart-button products use; and a
 * `GENERIC_SWITCH_MULTI_PRESS_WINDOW_MS` (400 ms) post-release window
 * before deciding no further press is coming, matching the common
 * double-click timing window most UI/button libraries use. Both
 * `MultiPressOngoing`'s field constraint (`CurrentNumberOfPressesCounted`
 * between 2 and MultiPressMax — confirmed directly in the cluster XML)
 * and `MultiPressComplete`'s (`TotalNumberOfPressesCounted` only
 * constrained by `max: MultiPressMax`, no `min: 2`) were checked
 * directly before writing this logic — confirming MultiPressOngoing
 * never fires for a lone single click, but MultiPressComplete correctly
 * DOES fire even after a single click (reporting count=1), the real,
 * spec-grounded reason a plain single click still produces a
 * MultiPressComplete event in this file, not just an unpaired
 * ShortRelease. A long press deliberately does not chain into multi-
 * press counting afterward (LongRelease resets the counter directly) —
 * matches how every real multi-click button product behaves; holding
 * the button is a distinct gesture from clicking it.
 *
 * Reference wiring: a breadboard pushbutton, GND -> button -> GPIO,
 * active-LOW — same wiring convention firmware/switch/'s own button
 * already uses, deliberately not the onboard BOOT/PROG button (see that
 * file's own header comment / CLAUDE.md's open next steps for why).
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (though this repo's other momentary-button device
 * types — firmware/switch/, firmware/outlet/ — already confirm the
 * underlying "read a breadboard pushbutton reliably" wiring works on
 * real hardware; this file's own press-timing state machine on top of
 * that has not itself been exercised on a physical board).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/switch-server/switch-server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_generic_switch";

/* Momentary pushbutton — active-LOW. Reference wiring: GND -> button ->
 * GPIO, same convention firmware/switch/'s own button already uses. */
#define GENERIC_SWITCH_BUTTON_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Press-timing state machine constants — see the header comment above for
 * why these specific values and the overall technique. */
#define GENERIC_SWITCH_POLL_INTERVAL_MS 10
#define GENERIC_SWITCH_DEBOUNCE_SAMPLES 3 /* 3 x 10ms = 30ms of consistent reading before accepting an edge */
#define GENERIC_SWITCH_LONG_PRESS_MS 1000
#define GENERIC_SWITCH_MULTI_PRESS_WINDOW_MS 400
#define GENERIC_SWITCH_MULTI_PRESS_MAX 4

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t generic_switch_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Registry-lookup-and-cast — same pattern firmware/valve/'s and
 * firmware/fan/'s own setters already use. `SwitchCluster` resolves
 * directly under `chip::app::Clusters` (confirmed by reading
 * `SwitchCluster.h`'s own namespace block — unlike firmware/evse/'s
 * `EnergyEvse::EnergyEvseCluster`, this one is NOT nested under a
 * `Switch::` sub-namespace, so no extra qualification is needed here). */
static SwitchCluster *get_switch_cluster(void)
{
    chip::app::ConcreteClusterPath path(generic_switch_endpoint_id, Switch::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "Switch cluster not found on endpoint %u", generic_switch_endpoint_id);
        return nullptr;
    }
    return static_cast<SwitchCluster *>(iface);
}

/* --- Press-timing state machine ------------------------------------------
 * See the header comment above for the overall design and sourcing.
 * Polls GENERIC_SWITCH_BUTTON_GPIO every GENERIC_SWITCH_POLL_INTERVAL_MS
 * from a plain FreeRTOS task — calling straight into SwitchCluster's own
 * event-generation methods with no explicit Matter stack lock, the same
 * lock-free "call a code-driven cluster's own update methods directly
 * from a plain periodic task" precedent firmware/air-purifier/'s
 * filter_life_task, firmware/water-heater/'s water_heater_task, and
 * firmware/evse/'s evse_task already established. */
static void switch_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(GENERIC_SWITCH_BUTTON_GPIO) == 0);

    int64_t press_start_ms = 0;
    bool long_press_fired = false;
    uint8_t press_count = 0;
    int64_t multi_press_window_end_ms = 0;
    bool multi_press_window_active = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(GENERIC_SWITCH_POLL_INTERVAL_MS));
        int64_t now_ms = esp_timer_get_time() / 1000;

        bool raw_pressed = (gpio_get_level(GENERIC_SWITCH_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < GENERIC_SWITCH_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        SwitchCluster *cluster = nullptr;

        /* Debounced press edge. */
        if (consistent_samples == GENERIC_SWITCH_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            press_start_ms = now_ms;
            long_press_fired = false;
            multi_press_window_active = false;

            cluster = get_switch_cluster();
            if (cluster) {
                cluster->SetCurrentPosition(1);
                cluster->OnInitialPress(1);
            }
            ESP_LOGI(TAG, "InitialPress");
        }

        /* Long-press threshold, while still held. */
        if (debounced_pressed && !long_press_fired && (now_ms - press_start_ms) >= GENERIC_SWITCH_LONG_PRESS_MS) {
            long_press_fired = true;
            cluster = get_switch_cluster();
            if (cluster) {
                cluster->OnLongPress(1);
            }
            ESP_LOGI(TAG, "LongPress");
        }

        /* Debounced release edge. */
        if (consistent_samples == GENERIC_SWITCH_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
            cluster = get_switch_cluster();

            if (long_press_fired) {
                if (cluster) {
                    cluster->SetCurrentPosition(0);
                    cluster->OnLongRelease(0);
                }
                ESP_LOGI(TAG, "LongRelease");
                /* A long press doesn't chain into multi-press counting —
                 * see the header comment above. */
                press_count = 0;
                multi_press_window_active = false;
            } else {
                if (cluster) {
                    cluster->SetCurrentPosition(0);
                    cluster->OnShortRelease(0);
                }
                ESP_LOGI(TAG, "ShortRelease");

                if (press_count < GENERIC_SWITCH_MULTI_PRESS_MAX) {
                    press_count++;
                }
                multi_press_window_end_ms = now_ms + GENERIC_SWITCH_MULTI_PRESS_WINDOW_MS;
                multi_press_window_active = true;

                if (press_count >= 2 && cluster) {
                    cluster->OnMultiPressOngoing(1, press_count);
                    ESP_LOGI(TAG, "MultiPressOngoing (count=%u)", press_count);
                }
            }
        }

        /* Multi-press window expiry — reports the final tally, even for a
         * lone single click (count=1), see the header comment above for
         * why that's the spec-correct behavior, not a bug. */
        if (multi_press_window_active && !debounced_pressed && now_ms >= multi_press_window_end_ms) {
            multi_press_window_active = false;
            cluster = get_switch_cluster();
            if (cluster) {
                cluster->OnMultiPressComplete(0, press_count);
            }
            ESP_LOGI(TAG, "MultiPressComplete (count=%u)", press_count);
            press_count = 0;
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

/* CurrentPosition is only ever written locally by switch_task() above
 * (via SwitchCluster::SetCurrentPosition(), not the generic
 * attribute::update() path) — so this is a no-op required by
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

    /* 2. Configure the button input — active-LOW, internal pull-up so an
     * unwired pin doesn't float and report spurious presses. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << GENERIC_SWITCH_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);

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

    /* 3. Build the Matter data model: one node, one Generic Switch
     * endpoint (Identify + Switch, both wired in automatically by the
     * complete top-level helper — see the header comment above). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::generic_switch::config_t switch_config;
    switch_config.switch_cluster.number_of_positions = 2;
    switch_config.switch_cluster.current_position = 0;
    switch_config.switch_cluster.features.momentary_switch_multi_press.multi_press_max = GENERIC_SWITCH_MULTI_PRESS_MAX;
    switch_config.switch_cluster.feature_flags =
        (uint32_t)cluster::switch_cluster::feature::momentary_switch::get_id() |
        (uint32_t)cluster::switch_cluster::feature::momentary_switch_release::get_id() |
        (uint32_t)cluster::switch_cluster::feature::momentary_switch_long_press::get_id() |
        (uint32_t)cluster::switch_cluster::feature::momentary_switch_multi_press::get_id();

    endpoint_t *endpoint = endpoint::generic_switch::create(node, &switch_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create generic switch endpoint");
        return;
    }

    generic_switch_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Generic switch endpoint id: %u", generic_switch_endpoint_id);

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

    /* 5. Start the button-polling / press-timing task. */
    xTaskCreate(switch_task, "switch_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter generic switch started. Scan the QR code to commission.");
}
