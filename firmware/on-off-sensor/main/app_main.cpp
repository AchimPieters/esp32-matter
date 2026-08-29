/*
 * Minimal Matter On/Off Sensor — fiftieth device type: a wireless motion
 * trigger that sends real On/Off commands to whatever device a controller
 * binds it to — no light of its own, no server-side sensing cluster at
 * all. The real-world product this maps onto: a battery/mains-powered PIR
 * (or radar) module you stick on a wall, bound to an existing smart bulb
 * or plug, that turns it on when it sees motion and off again once it
 * doesn't — without needing that bulb/plug to have any motion-sensing
 * hardware of its own.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: every cluster except Identify's server half is
 * client-side, and there's no top-level helper -----------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * OnOffSensor.xml (device type 0x0850, revision 3): Identify server AND
 * client both `<mandatoryConform/>`, On/Off CLIENT `<mandatoryConform/>`,
 * Groups/Level Control/Scenes Management/Color Control (all client) are
 * `<optionalConform/>` and not implemented here — same "smallest reasonable
 * next step" scoping firmware/dimmer-switch/'s own skipped Groups/Scenes
 * already establishes. Note this device type has NO server-side sensing
 * cluster at all — unlike firmware/occupancy-sensor/'s own OccupancySensing
 * cluster (which reports a real, controller-visible Occupancy attribute),
 * this device's own motion detection is purely a local, internal trigger
 * for its client-side On/Off commands — a genuinely different shape from
 * every other sensor device type in this repo, and the reason this file
 * needs no code-driven-cluster registry lookup at all despite being
 * "sensor"-named.
 *
 * Confirmed via Docker `grep` against `esp_matter_endpoint.cpp` that no
 * `endpoint::on_off_sensor::create()` top-level helper exists in esp-
 * matter's legacy data model — same situation firmware/doorbell/'s and
 * firmware/chime/'s own header comments already document for their own
 * device types (both niche enough that esp-matter hasn't shipped a
 * top-level helper for either). This endpoint is hand-assembled from
 * lower-level free functions instead, reusing those two files' own hard-
 * learned discipline from the start: `cluster::descriptor::create()` is
 * called explicitly, right after `endpoint::create()`, since
 * `add_device_type()` alone never creates one — the real reason firmware/
 * color-light/'s and firmware/addressable-light/'s own original hand-
 * assembled endpoints were once rejected by Apple Home (see CLAUDE.md's
 * own "Open next steps" for the full debugging story). Confirmed by
 * reading `cluster::on_off::create()` directly that, unlike
 * `dimmer_switch::create()` (whose own top-level wrapper adds Binding
 * itself, separately from `add()`'s own cluster additions — confirmed by
 * reading firmware/dimmer-switch/'s own header comment), the plain
 * `on_off::create()` free function does NOT auto-create a Binding cluster
 * — so this file adds one explicitly too (`cluster::binding::create()`,
 * `common::config_t` — confirmed trivial by reading its own header).
 *
 * --- Motion -> On/Off: reuses firmware/occupancy-sensor/'s own 3-chip
 * driver, firmware/dimmer-switch/'s own client-invoke plumbing -----------
 * `ON_OFF_SENSOR_TYPE` offers the identical PIR/RCWL-0516/HLK-LD2410
 * choice firmware/occupancy-sensor/'s own header comment documents in
 * full (all three share the same actively-driven, active-HIGH digital OUT
 * pin, no internal pull-up needed) — reused here purely as a local
 * trigger, never exposed as a Matter attribute at all, since this device
 * type has no server-side sensing cluster to expose it through. The same
 * debounce shape (ISR + queue, ANYEDGE, 8 x 5ms consistent-sample check)
 * is reused verbatim — only the action taken on a confirmed state change
 * differs: instead of `OccupancySensingCluster::SetOccupancy()`, a
 * confirmed motion-detected edge sends a real `OnOff::Commands::On` to
 * whatever this endpoint's Binding cluster resolves to, and a confirmed
 * motion-cleared edge sends `OnOff::Commands::Off` — both empty-payload
 * commands (same `"{}"` shape firmware/switch/'s own buttons already use;
 * unlike firmware/dimmer-switch/'s own LevelControl::Step/Identify
 * commands, On/Off has no fields to encode at all). The
 * `app_client_request_cb`/`send_bound_command()` plumbing (stack-locked
 * `client::cluster_update()`, dispatch on cluster/command ID) is reused
 * near-verbatim from firmware/dimmer-switch/'s own file. Same "don't
 * reimplement occupancy-hold timing in software" precedent firmware/
 * occupancy-sensor/'s own header comment already establishes — each
 * sensor module's own onboard timing (PIR's adjustable hold potentiometer,
 * RCWL-0516's fixed ~2s pulse, HLK-LD2410's internal presence algorithm)
 * decides how long OUT stays HIGH; this firmware just relays whatever it's
 * currently doing as On/Off commands, with no debounce-driven hold logic
 * of its own beyond noise rejection.
 *
 * The mandatory client-side Identify cluster is a shell only — no local
 * gesture exists on this device to trigger sending
 * `Identify::Commands::Identify` to a bound target (unlike firmware/
 * dimmer-switch/'s own long-press, this device has no button at all), so
 * `app_client_request_cb()` doesn't handle that command shape — same
 * "declare the shell, don't invent busy-work" precedent firmware/switch/'s
 * own unused Groups/Scenes client shells already establish. This device's
 * OWN server-side Identify still works normally (a controller can make
 * THIS device's own LED blink to physically locate it).
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no PIR/RCWL-0516/HLK-LD2410 module physically available
 * when written, and — like every client-invoke device in this repo —
 * verifying this one for real also needs a second, already-commissioned
 * bindable target device on the same fabric).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_client.h>
#include <app-common/zap-generated/cluster-objects.h>

static const char *TAG = "matter_on_off_sensor";

/* ON_OFF_SENSOR_TYPE selects which motion sensor to build for — same
 * choice, same shared GPIO interface, as firmware/occupancy-sensor/'s own
 * OCCUPANCY_SENSOR_TYPE (see that file's own header comment for the full
 * per-chip sourcing detail: PIR's adjustable onboard hold time, RCWL-
 * 0516's fixed ~2s pulse and separate 4-28V supply requirement, HLK-
 * LD2410's mmWave presence algorithm and separate 5V supply requirement —
 * all identical here, since this is the same electrical interface, just
 * driving a client command instead of a server attribute). */
#define ON_OFF_SENSOR_TYPE_PIR 0
#define ON_OFF_SENSOR_TYPE_RCWL0516 1
#define ON_OFF_SENSOR_TYPE_LD2410 2
#define ON_OFF_SENSOR_TYPE ON_OFF_SENSOR_TYPE_PIR

/* Change this to the GPIO your sensor module's OUT pin is wired to.
 * Reference wiring: sensor module OUT -> GPIO directly (no pull-up needed
 * on any of the three supported sensors, same as firmware/
 * occupancy-sensor/'s own identical driver). */
#define ON_OFF_SENSOR_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster (this device's OWN server-side
 * Identify — a controller asking THIS device to identify itself, distinct
 * from this device's own client-side Identify shell, which has no local
 * trigger — see the header comment above). */
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

static uint16_t on_off_sensor_endpoint_id = 0;
static QueueHandle_t motion_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Current confirmed motion state — true = motion present. Kept locally so
 * we only send a command on an actual change, not on every debounced
 * re-read of the same level. */
static bool motion_present = false;

/* Called by the Matter stack once a bound peer has been resolved, for
 * every outstanding request queued via client::cluster_update() — same
 * dispatch shape firmware/dimmer-switch/'s own header comment documents
 * in full, simplified here since On/Off's commands both take empty
 * payloads (no fields to encode at all, unlike LevelControl::Step or
 * Identify::Identify). */
static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "Command acknowledged by bound device");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Command failed: %" CHIP_ERROR_FORMAT, error.Format());
}

static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }

    if (req_handle->command_path.mClusterId != OnOff::Id ||
        (req_handle->command_path.mCommandId != OnOff::Commands::On::Id &&
         req_handle->command_path.mCommandId != OnOff::Commands::Off::Id)) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

/* Sends a real On or Off command to whatever this endpoint's Binding
 * cluster resolves to. */
static void send_bound_command(uint32_t command_id)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = OnOff::Id;
    req_handle.command_path.mCommandId = command_id;
    req_handle.request_data = NULL;

    /* Stack lock required before calling into esp-matter's client APIs
     * from a plain FreeRTOS task, same as every other client-invoke
     * device in this repo. */
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(on_off_sensor_endpoint_id, &req_handle);
}

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR motion_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(motion_evt_queue, &gpio_num, NULL);
}

/* Debounces the motion sensor output and, on an actual state change,
 * sends On or Off to whatever this endpoint is bound to. Same shape as
 * firmware/occupancy-sensor/'s own occupancy_task() (ANYEDGE, reacts to
 * both directions, only acts on an actually-changed debounced level) —
 * see this file's own header comment for why this debounce is only about
 * rejecting electrical noise, not implementing any motion-hold behavior
 * of its own. */
static void motion_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(motion_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

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

        xQueueReset(motion_evt_queue);

        if (!consistent) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously stable");
            continue;
        }

        bool new_present = (first_level != 0); /* active-HIGH */
        if (new_present == motion_present) {
            ESP_LOGI(TAG, "Debounced level matches current state (%s) — no change",
                     motion_present ? "MOTION" : "CLEAR");
            continue;
        }

        motion_present = new_present;
        ESP_LOGI(TAG, "Motion now %s — sending %s to bound device(s)",
                 motion_present ? "PRESENT" : "CLEAR", motion_present ? "On" : "Off");
        send_bound_command(motion_present ? OnOff::Commands::On::Id : OnOff::Commands::Off::Id);
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

/* No local attributes this device writes to itself — everything flows
 * outward via bound commands. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

/* Called when a controller asks THIS device to identify itself (its own
 * server-side Identify cluster) — starts or stops the identify LED
 * blinking accordingly. */
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

    /* 2. Configure the motion sensor input + its interrupt. ANYEDGE, not
     * a single edge — we need to know about both motion-detected and
     * motion-cleared transitions. No internal pull-up — see the header
     * comment on why (actively-driven push-pull output on all three
     * supported sensors). */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << ON_OFF_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    /* Settle briefly, then take the pin's boot-time level as the initial
     * state — same reasoning firmware/occupancy-sensor/'s own header
     * comment gives (a best-effort starting point, not a substitute for
     * each module's own real warm-up time). */
    vTaskDelay(pdMS_TO_TICKS(50));
    motion_present = (gpio_get_level(ON_OFF_SENSOR_GPIO) != 0);
    ESP_LOGI(TAG, "Motion GPIO %d initial level: %d — starting as %s",
             ON_OFF_SENSOR_GPIO, gpio_get_level(ON_OFF_SENSOR_GPIO), motion_present ? "PRESENT" : "CLEAR");

    motion_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(motion_task, "motion_task", 4096, NULL, 10, NULL);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(ON_OFF_SENSOR_GPIO, motion_isr_handler, (void *)(uintptr_t)ON_OFF_SENSOR_GPIO);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
    }

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

    /* 3. Build the Matter data model: one node, one On/Off Sensor
     * endpoint, hand-assembled since no top-level helper exists — see the
     * header comment above for the full detail, including why
     * cluster::descriptor::create() and cluster::binding::create() both
     * need explicit calls here. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "On/Off Sensor",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create on/off sensor endpoint");
        return;
    }

    cluster::descriptor::config_t descriptor_config;
    cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);

    err = add_device_type(endpoint, 0x0850 /* On/Off Sensor */, 3 /* device type revision */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add on/off sensor device type: %d", err);
        return;
    }

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER | CLUSTER_FLAG_CLIENT);

    cluster::binding::config_t binding_config;
    cluster::binding::create(endpoint, &binding_config, CLUSTER_FLAG_SERVER);

    cluster::on_off::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    on_off_sensor_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "On/off sensor endpoint id: %u", on_off_sensor_endpoint_id);

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

    /* Endpoint-agnostic by design (same reasoning as firmware/switch/'s
     * and firmware/dimmer-switch/'s own single global registration) —
     * this device only has the one endpoint anyway.
     * binding_manager_init() (which resolves bindings set up via a
     * controller's Binding cluster) runs on its own, inside
     * esp_matter::start() above — no explicit call needed here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);

    ESP_LOGI(TAG, "Matter on/off sensor started. Scan the QR code to commission.");
}
