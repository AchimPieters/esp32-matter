/*
 * Minimal Matter Water Leak Detector — eighteenth device type, and the
 * closest sibling to firmware/contact-sensor/ in this repo: same BooleanState
 * cluster, same debounced-GPIO-input shape, but a different device type
 * (0x0043 vs. Contact Sensor's 0x0015) and — critically — the OPPOSITE
 * semantic meaning of StateValue. Confirmed directly via esp-matter's own
 * `water_leak_detector::add()` in esp_matter_endpoint.cpp: identical
 * structure to `contact_sensor::add()` (Identify + BooleanState +
 * StateChange event, both via `common::create<T>()` so the endpoint's
 * Descriptor cluster is created automatically) — the two device types
 * really are the same cluster composition wearing a different device type
 * ID, matching how the CSA's own data_model/1.6/device_types/
 * WaterLeakDetector.xml lists exactly Identify (mandatory) + BooleanState
 * (mandatory, with its ChangeEvent feature mandatory as of the device
 * type's own revision 2) + an optional BooleanStateConfiguration cluster
 * (not implemented here — see the scope note below).
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
 * --- StateValue semantic: OPPOSITE direction from Contact Sensor -----------
 * For firmware/contact-sensor/, StateValue=true means "closed" — a normal,
 * safe physical state. For a water leak detector, true means "leak
 * detected" — an ALARM condition, the same "true = alarm/problem" direction
 * SmokeCoAlarm and every other hazard-sensor device type in Matter uses,
 * not the "true = normal physical state" direction ContactSensor/
 * OccupancySensing use. Confirmed against real-world Matter tooling, not
 * assumed: Espressif's own `MatterWaterLeakDetector` Arduino-ESP32 class
 * exposes `setLeak(bool)`/`getLeak()`, and a real product's `setLeak(true)`
 * is documented as reporting "leak detected" — the same direction Apple's
 * own HomeKit Leak Sensor characteristic uses (0 = No Leak, 1 = Leak
 * Detected), which is what Matter-to-HomeKit bridges map this cluster's
 * StateValue onto. Getting this backwards would silently invert every
 * alarm this device ever reports, so it's called out explicitly here
 * rather than left to be inferred from firmware/contact-sensor/'s own
 * (opposite) convention.
 *
 * --- Sensor: a cheap LM393-comparator "water sensor" probe module ----------
 * The near-ubiquitous hobbyist water/rain sensor board (sold under many
 * names — "FC-37", "YL-83", generic "water sensor module" — no single
 * canonical datasheet exists, it's a widely cloned design, not one
 * manufacturer's own part, same "best available, cross-checked" sourcing
 * standard this repo already applies to e.g. firmware/contact-sensor/'s
 * reed switch or firmware/occupancy-sensor/'s PIR module): a set of
 * parallel PCB traces (the actual probe — water bridging adjacent traces
 * lowers the measured resistance) feeding an onboard LM393 comparator
 * against a potentiometer-set threshold, with a digital "DO" output pin.
 * Confirmed (via multiple independent sources, not just one) that DO goes
 * LOW when water/moisture is detected and HIGH when dry — the OPPOSITE
 * polarity convention from what "active-LOW" usually means elsewhere in
 * this repo (e.g. firmware/contact-sensor/'s reed switch, where LOW means
 * a passive short-to-ground the ESP32's own internal pull-up defines
 * against) — here the module's own comparator actively drives the DO pin
 * both ways, so unlike a reed switch, no internal GPIO pull-up is needed
 * or used. Same "always check your specific module" caveat this repo
 * already applies to e.g. firmware/outlet/'s relay polarity — cheap
 * cloned modules aren't guaranteed identical.
 *
 * Reference wiring: module VCC -> 3.3V, GND -> GND, DO -> WATER_LEAK_GPIO
 * (a plain digital input, no pull-up/pull-down — the module drives the
 * line itself).
 *
 * --- Scope: BooleanStateConfiguration not implemented -----------------------
 * The CSA XML lists BooleanStateConfiguration (alarm suppression, local/
 * remote audible-alarm enable/disable, sensitivity level) as
 * `<optionalConform/>` — a real, genuinely more complex cluster (its own
 * Delegate, SuppressAlarm/EnableDisableAlarm commands, AlarmsActive/
 * AlarmsSuppressed attributes) left out for v1, same "smallest reasonable
 * next step" scoping this repo applies to every other device type's first
 * cut (dimmable-light's Level-only scope, fan's PercentSetting-only
 * scope, etc.).
 *
 * --- A real, documented esp-matter gap, and why THIS one gets fixed --------
 * Confirmed by reading `boolean_state::create()` in esp-matter's own
 * esp_matter_cluster.cpp directly: it hardcodes `global::attribute::
 * create_feature_map(cluster, 0)`, and `boolean_state::config_t` has no
 * `feature_flags` field at all to override it — the exact same class of
 * gap firmware/air-quality-sensor/'s `air_quality::create()` has. That
 * means esp-matter's OWN `water_leak_detector::add()` reference
 * implementation never actually sets the ChangeEvent (CHGEVENT) feature
 * bit the device type's own spec (revision 2) makes mandatory — a real,
 * previously undocumented spec-conformance gap in the SDK helper this
 * file's device type is built from. Unlike firmware/air-quality-sensor/'s
 * FeatureMap gap (there, a missing feature bit could plausibly make
 * `SetAirQuality()` reject values HasFeature() doesn't recognize — a
 * genuine *functional* risk, so it was left unfixed rather than risk an
 * unverified workaround), this one is lower-risk to fix: `boolean_state::
 * event::create_state_change()` registers and fires the StateChange event
 * unconditionally, with no feature-flag gate at all — confirmed by reading
 * its own implementation. So the FeatureMap bit here is closer to pure
 * advertised-conformance metadata than something gating real behavior.
 * This file therefore does what firmware/air-quality-sensor/ deliberately
 * did NOT: after creating the endpoint (which leaves FeatureMap at its
 * hardcoded 0) but before `esp_matter::start()`, it overwrites the
 * BooleanState cluster's own FeatureMap ember attribute directly via
 * `attribute::update()` to set `BooleanState::Feature::kChangeEvent` —
 * safe specifically because there's no constructor-time snapshot of this
 * bit gating any later behavior the way `AirQualityCluster`'s
 * `BitFlags<Feature>` does.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/boolean-state-server/BooleanStateCluster.h>

static const char *TAG = "matter_water_leak";

/* Change this to the GPIO your water sensor module's DO pin is wired to —
 * see the header comment above for the module type/wiring assumed. GPIO 4
 * matches firmware/contact-sensor/'s own default GPIO for its single
 * digital sensor input, for consistency across this repo's simple-sensor
 * device types. Adjust to match your board. */
#define WATER_LEAK_GPIO GPIO_NUM_4

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

static uint16_t water_leak_endpoint_id = 0;
static QueueHandle_t water_leak_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Current confirmed leak state — true = leak detected (see the header
 * comment on why this is the OPPOSITE direction from firmware/
 * contact-sensor/'s true=closed). Mirrors the Boolean State cluster's
 * StateValue attribute; kept locally too so we only push an attribute
 * update on an actual change, not on every debounced re-read of the same
 * level. */
static bool water_leak_detected = false;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR water_leak_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(water_leak_evt_queue, &gpio_num, NULL);
}

/* Same registry-lookup-and-cast pattern firmware/contact-sensor/'s
 * update_contact_state() already established — BooleanState is a
 * "code-driven" cluster class in this SDK version, not the generic
 * ember-style attribute store, so the generic attribute::update() path
 * returns ESP_ERR_NOT_SUPPORTED for it. SetStateValue() also generates the
 * StateChange event itself, so this doesn't need to do that separately. */
static void update_water_leak_state(uint16_t endpoint_id, bool leak_detected)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, BooleanState::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "BooleanState cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::BooleanStateCluster *>(iface);
    cluster->SetStateValue(leak_detected);
}

/* Debounces the sensor input and, on an actual state change, updates the
 * local BooleanState attribute — same debounce shape as firmware/
 * contact-sensor/'s contact_task(), reacting to both edges since a water
 * sensor's DO pin can bounce going wet or drying out again, and either
 * transition is a real event worth reporting. */
static void water_leak_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(water_leak_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

        /* Debounce: require the pin to read a consistent level for ~40ms
         * (8 x 5ms samples) before treating it as a real, settled state —
         * same reasoning as firmware/contact-sensor/'s own debounce. */
        int first_level = gpio_get_level((gpio_num_t)io_num);
        bool consistent = true;
        char samples[9] = {0};
        samples[0] = first_level ? 'H' : 'L';
        for (int i = 1; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            int level = gpio_get_level((gpio_num_t)io_num);
            samples[i] = level ? 'H' : 'L';
            if (level != first_level) {
                consistent = false;
            }
        }
        ESP_LOGI(TAG, "Samples (5ms apart): %s (%s)", samples, consistent ? "stable" : "mixed/bouncing");

        /* Whether confirmed or not, flush any further queued edges from
         * this same burst — same reasoning as firmware/contact-sensor/. */
        xQueueReset(water_leak_evt_queue);

        if (!consistent) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously stable");
            continue;
        }

        /* Active-LOW = wet — see the header comment on the sensor module's
         * own polarity (opposite reasoning from a passive reed switch, but
         * the same resulting GPIO level convention). */
        bool new_leak_detected = (first_level == 0);
        if (new_leak_detected == water_leak_detected) {
            ESP_LOGI(TAG, "Debounced level matches current state (%s) — no change",
                     water_leak_detected ? "LEAK" : "DRY");
            continue;
        }

        water_leak_detected = new_leak_detected;
        ESP_LOGW(TAG, "Water leak state now %s", water_leak_detected ? "LEAK DETECTED" : "DRY");

        update_water_leak_state(water_leak_endpoint_id, water_leak_detected);
    }
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

/* Called whenever a controller changes an attribute on this device. A water
 * leak detector has nothing to react to here — StateValue is read-only and
 * only ever written locally by water_leak_task() above — so this is a
 * no-op required by node::create()'s callback signature. */
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

    /* 2. Configure the sensor input + its interrupt. ANYEDGE, not NEGEDGE —
     * we need to know about both the water arriving and drying out again.
     * No internal pull-up: the sensor module's own comparator actively
     * drives the DO line both ways (see the header comment on the module's
     * own electrical behavior). */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << WATER_LEAK_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    /* Settle briefly, then take the pin's boot-time level as the initial
     * state — a leak detector should report reality from the first
     * commissioned read, not an arbitrary hardcoded "dry" default. */
    vTaskDelay(pdMS_TO_TICKS(50));
    water_leak_detected = (gpio_get_level(WATER_LEAK_GPIO) == 0);
    ESP_LOGI(TAG, "Water leak GPIO %d initial level: %d — starting as %s",
             WATER_LEAK_GPIO, gpio_get_level(WATER_LEAK_GPIO), water_leak_detected ? "LEAK" : "DRY");

    water_leak_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(water_leak_task, "water_leak_task", 4096, NULL, 10, NULL);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(WATER_LEAK_GPIO, water_leak_isr_handler, (void *)(uintptr_t)WATER_LEAK_GPIO);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
    }

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

    /* 3. Build the Matter data model: one node, one Water Leak Detector
     * endpoint, seeded with the boot-time reading above so its first
     * reported state matches physical reality. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    water_leak_detector::config_t water_leak_config;
    water_leak_config.boolean_state.state_value = water_leak_detected;
    endpoint_t *endpoint = water_leak_detector::create(node, &water_leak_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create water leak detector endpoint");
        return;
    }

    water_leak_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Water leak detector endpoint id: %u", water_leak_endpoint_id);

    /* 3b. Set the BooleanState cluster's FeatureMap to advertise the
     * ChangeEvent feature — see the header comment above for the real
     * esp-matter gap this works around (the endpoint helper always
     * creates FeatureMap as 0, with no config field to override it) and
     * why it's safe to fix here (the StateChange event itself fires
     * unconditionally either way — this only corrects what the device
     * advertises, it doesn't gate any actual behavior). Must happen
     * before esp_matter::start(), which is what reads FeatureMap into
     * each code-driven cluster's own runtime state. */
    esp_matter_attr_val_t feature_map_val = esp_matter_uint32(chip::to_underlying(BooleanState::Feature::kChangeEvent));
    attribute::update(water_leak_endpoint_id, BooleanState::Id, Globals::Attributes::FeatureMap::Id, &feature_map_val);

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

    ESP_LOGI(TAG, "Matter water leak detector started. Scan the QR code to commission.");
}
