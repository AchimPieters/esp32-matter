/*
 * Minimal Matter Occupancy Sensor — fifteenth device type, and the second
 * "requires-at-least-one-modality-feature" cluster in this repo after
 * SmokeCoAlarm's own AlarmStateEnum-driven design (see below for the
 * specific conformance rule this one has that firmware/contact-sensor/'s
 * simpler BooleanState cluster doesn't).
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
 * `endpoint::occupancy_sensor::create()` (device type 0x0107, Identify +
 * OccupancySensing — no Groups; confirmed directly against the CSA's own
 * data_model/1.6/device_types/OccupancySensor.xml, which lists only those
 * two clusters, both `<mandatoryConform/>`) is a complete, ready-to-use
 * top-level helper — unlike firmware/color-light/'s and
 * firmware/addressable-light/'s ExtendedColorLight endpoints, which had to
 * be hand-assembled from lower-level free functions (and, as a real,
 * hardware-testing-caught bug this session, initially missed the Descriptor
 * cluster that `common::create<T>()` — the shared template underneath every
 * complete top-level helper, including this one — creates automatically).
 * Using the complete helper here sidesteps that entire bug class from the
 * start.
 *
 * --- OccupancySensing: a "code-driven" cluster, same class as BooleanState -
 * Confirmed by reading esp-matter's own source directly: a real
 * `occupancy_sensing/` folder exists under
 * `components/esp_matter/data_model_provider/clusters/`, backed by
 * connectedhomeip's own `OccupancySensingCluster`
 * (`src/app/clusters/occupancy-sensor-server/OccupancySensingCluster.h`) —
 * the same "code-driven, not generic ember attribute store" situation
 * firmware/contact-sensor/'s BooleanState, firmware/temperature-sensor/'s
 * TemperatureMeasurement, and firmware/smoke-co-alarm/'s SmokeCoAlarm all
 * already hit in this repo. So the Occupancy attribute is written through
 * `OccupancySensingCluster::SetOccupancy()`, looked up via the data model
 * provider's registry — the same pattern `update_contact_state()` in
 * firmware/contact-sensor/main/app_main.cpp already established, reused
 * here nearly verbatim (see update_occupancy() below).
 *
 * --- The "at least one sensing modality" conformance rule -------------------
 * Unlike a device type where every cluster feature is simply optional,
 * OccupancySensing's eight sensing-modality features (Other/PassiveInfrared/
 * Ultrasonic/PhysicalContact/ActiveInfrared/Radar/RFSensing/Vision) form a
 * single CSA "choice" group requiring at least one — confirmed two ways:
 * the cluster XML marks every one of them `optionalConform choice="a"
 * more="true" min="1"` (the CSA's own idiom for "at least 1 of this named
 * choice set is required"), and esp-matter's own generated
 * occupancy_sensing.cpp enforces it directly in code via a
 * `VALIDATE_FEATURES_AT_LEAST_ONE(...)` macro that aborts cluster creation
 * if none of them are set. This firmware always sets PassiveInfrared (the
 * only sensor type implemented so far — see SENSOR_TYPE below), so this is
 * satisfied automatically; worth remembering if a future sensor type is
 * added here that doesn't map to one of the eight modality bits.
 *
 * --- SENSOR_TYPE: PIR only, for now ------------------------------------
 * A cheap PIR (Passive InfraRed) module — e.g. the ubiquitous HC-SR501 or
 * any of its many clones — is the obvious first, cheapest, most common
 * choice, the same "smallest reasonable next step" scoping this repo has
 * applied to every other device type's first cut (dimmable-light's
 * Level-only scope, window-covering's Lift-only scope, thermostat's
 * Heat+Cool-only scope, etc.). A real mmWave/radar presence sensor (e.g. the
 * HLK-LD2410, mapping to OccupancySensing's own Radar feature bit) would be
 * a genuinely different, UART-based driver and a reasonable next addition —
 * deliberately not attempted in this first pass, the same way
 * firmware/temperature-sensor/ and firmware/light-sensor/ each started with
 * one sensor and grew a componentOptions picker later once a second,
 * genuinely different sensor was actually requested.
 *
 * PIR module wiring/behavior, checked against the datasheet-level detail
 * multiple independent HC-SR501-class module documentation sources agree on
 * (no single canonical "HC-SR501 datasheet" exists — it's a widely cloned
 * hobbyist module, not a single manufacturer's own part — so this is the
 * same "best available, cross-checked" sourcing standard this repo already
 * applies to e.g. APA102/SM2335EGH in firmware/addressable-light/): the
 * module's OUT pin is a plain, actively-driven push-pull digital output
 * (HIGH = motion currently detected/held, LOW = clear) — unlike
 * firmware/contact-sensor/'s reed switch, it needs no internal pull-up,
 * since it's never left floating. Critically, these modules already have
 * their own onboard analog "occupied hold time" (typically an adjustable
 * potentiometer, commonly ~5s-300s depending on the specific board) that
 * keeps OUT held HIGH for a while after the last detected motion — this
 * firmware does NOT reimplement that timing in software at all, it just
 * reports whatever the module's OUT pin is currently doing. The short
 * software debounce below exists only to reject brief electrical
 * noise/glitches on the GPIO line, not to implement any occupancy-hold
 * behavior of its own (that's the module's own job).
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
#include <app/clusters/occupancy-sensor-server/OccupancySensingCluster.h>

static const char *TAG = "matter_occupancy";

/* Change this to the GPIO your PIR module's OUT pin is wired to. Reference
 * wiring: PIR module OUT -> GPIO directly (no pull-up needed — see the
 * header comment on why: it's an actively-driven push-pull output, unlike
 * firmware/contact-sensor/'s passive reed switch). GPIO 4 is a plain,
 * unreserved GPIO on classic ESP32 (WROOM-32) — the same default
 * firmware/contact-sensor/ and firmware/switch/ use for their own single
 * digital sensor input, deliberately NOT the onboard BOOT/PROG button
 * (GPIO 0), which shares boot-mode-select traffic and was an unreliable
 * choice for an external input on the board this repo's other devices were
 * tested against (see CLAUDE.md's "Open next steps"). Adjust to match your
 * board if you wire it elsewhere. */
#define OCCUPANCY_GPIO GPIO_NUM_4

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

static uint16_t occupancy_endpoint_id = 0;
static QueueHandle_t occupancy_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Current confirmed occupancy state — true = occupied. Mirrors the
 * OccupancySensing cluster's Occupancy attribute; kept locally too so we
 * only push an attribute update on an actual change, not on every
 * debounced re-read of the same level. */
static bool occupancy_occupied = false;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR occupancy_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(occupancy_evt_queue, &gpio_num, NULL);
}

/* esp-matter's generic attribute::update() can't write OccupancySensing's
 * Occupancy attribute in this SDK version — same "code-driven cluster
 * class" situation as firmware/contact-sensor/'s BooleanState (see the
 * header comment for the full explanation and sourcing). This looks the
 * cluster instance up directly via the data model provider's registry and
 * calls its SetOccupancy(), which is the actually-supported way to update
 * this particular cluster's state from app code on this SDK version. It
 * also takes care of generating the OccupancyChanged event on its own (when
 * the OccupancyEvent feature is enabled — not enabled here, kept simple),
 * so we don't have to. */
static void update_occupancy(uint16_t endpoint_id, bool occupied)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, OccupancySensing::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "OccupancySensing cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::OccupancySensingCluster *>(iface);
    cluster->SetOccupancy(occupied);
}

/* Debounces the PIR output and, on an actual state change, updates the
 * local Occupancy attribute. Same shape as firmware/contact-sensor/'s
 * contact_task() (ANYEDGE, reacts to both directions, only acts on an
 * actually-changed debounced level) — but see the header comment on why
 * this debounce is only about rejecting electrical noise, not implementing
 * any occupancy-hold timing (the PIR module already does that itself). */
static void occupancy_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(occupancy_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

        /* Debounce: require the pin to read a consistent level for ~40ms
         * (8 x 5ms samples) before treating it as real — same reasoning as
         * firmware/contact-sensor/'s own debounce (a lone sample can land
         * mid-glitch). */
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
         * this same burst. */
        xQueueReset(occupancy_evt_queue);

        if (!consistent) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously stable");
            continue;
        }

        bool new_occupied = (first_level != 0); /* active-HIGH */
        if (new_occupied == occupancy_occupied) {
            ESP_LOGI(TAG, "Debounced level matches current state (%s) — no change",
                     occupancy_occupied ? "OCCUPIED" : "UNOCCUPIED");
            continue;
        }

        occupancy_occupied = new_occupied;
        ESP_LOGI(TAG, "Occupancy now %s", occupancy_occupied ? "OCCUPIED" : "UNOCCUPIED");

        update_occupancy(occupancy_endpoint_id, occupancy_occupied);
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

/* Called whenever a controller changes an attribute on this device. An
 * occupancy sensor has nothing to react to here — Occupancy is read-only
 * and only ever written locally by occupancy_task() above — so this is a
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

    /* 2. Configure the PIR input + its interrupt. ANYEDGE, not a single
     * edge — we need to know about both motion-detected and motion-cleared
     * transitions, not just one discrete action (same reasoning as
     * firmware/contact-sensor/'s reed switch). No internal pull-up — see
     * the header comment on why (actively-driven push-pull output). */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << OCCUPANCY_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    /* Settle briefly, then take the pin's boot-time level as the initial
     * state — an occupancy sensor should report reality from the first
     * commissioned read, not an arbitrary hardcoded default. Note: right
     * after power-up, many PIR modules hold OUT in an indeterminate/settling
     * state for a second or two while their own analog front-end
     * stabilizes — this initial read is a best-effort starting point, not a
     * substitute for that warm-up; the first real edge interrupt afterwards
     * is what actually re-confirms the true state either way. */
    vTaskDelay(pdMS_TO_TICKS(50));
    occupancy_occupied = (gpio_get_level(OCCUPANCY_GPIO) != 0);
    ESP_LOGI(TAG, "Occupancy GPIO %d initial level: %d — starting as %s",
             OCCUPANCY_GPIO, gpio_get_level(OCCUPANCY_GPIO), occupancy_occupied ? "OCCUPIED" : "UNOCCUPIED");

    occupancy_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(occupancy_task, "occupancy_task", 4096, NULL, 10, NULL);

    /* These two silently doing nothing was a real bug in firmware/switch:
     * unchecked, a failure means no interrupt is ever attached and every
     * occupancy change produces zero log output, which looks identical to
     * "nothing is wired up" from the outside. */
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(OCCUPANCY_GPIO, occupancy_isr_handler, (void *)(uintptr_t)OCCUPANCY_GPIO);
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

    /* 3. Build the Matter data model: one node, one Occupancy Sensor
     * endpoint, seeded with the boot-time reading above so its first
     * reported state matches physical reality. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::occupancy_sensor::config_t occupancy_config;
    occupancy_config.occupancy_sensing.occupancy = occupancy_occupied ? 1 : 0;
    /* OccupancySensorType/OccupancySensorTypeBitmap: PIR (0 / bit 0) — see
     * the header comment on SENSOR_TYPE. Both attributes exist in parallel
     * because the spec deprecated the single-value enum in favor of the
     * bitmap (which can express multiple simultaneous modalities) but kept
     * the enum mandatory for legacy client compatibility — confirmed
     * directly in the cluster XML's own <otherwiseConform> tag on
     * OccupancySensorType. */
    occupancy_config.occupancy_sensing.occupancy_sensor_type = 0;
    occupancy_config.occupancy_sensing.occupancy_sensor_type_bitmap = 1;
    /* FeatureMap: PassiveInfrared — mandatory to set at least one sensing
     * modality feature (see the header comment's "at least one" section);
     * esp-matter's own occupancy_sensing::create() aborts cluster creation
     * via VALIDATE_FEATURES_AT_LEAST_ONE() if this is left at 0. */
    occupancy_config.occupancy_sensing.feature_flags =
        cluster::occupancy_sensing::feature::passive_infrared::get_id();
    endpoint_t *endpoint = endpoint::occupancy_sensor::create(node, &occupancy_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create occupancy sensor endpoint");
        return;
    }

    occupancy_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Occupancy sensor endpoint id: %u", occupancy_endpoint_id);

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

    ESP_LOGI(TAG, "Matter occupancy sensor started. Scan the QR code to commission.");
}
