/*
 * Minimal Matter Door Lock Controller — sixtieth device type: a physical
 * two-button remote panel (Lock / Unlock) that sends real DoorLock::
 * LockDoor/UnlockDoor commands to whatever real lock a controller binds it
 * to — the class of hardware sold as a wall-mounted or bedside "smart lock
 * remote," letting a household lock/unlock a real Matter door lock (e.g.
 * firmware/door-lock/ itself) without reaching for a phone app.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type: hand-assembled, since only the "generated" data model
 * this repo never enables has a top-level helper --------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * DoorLockController.xml (device type 0x000B, revision 3, confirmed by
 * reading the XML's own `id`/`revision` attributes directly rather than
 * assumed): Door Lock (client) is the ONLY `<mandatoryConform/>` cluster — Groups and
 * Scenes Management (both client) are `<optionalConform/>` and not
 * implemented, same "smallest reasonable next step" scoping this repo
 * applies elsewhere. A real, worth-noting spec detail: this device type
 * lists NO Identify cluster at all, server or client — confirmed by
 * reading the full `<clusters>` block directly, not assumed from every
 * other client-invoke device type in this repo (firmware/dimmer-switch/,
 * firmware/on-off-sensor/) happening to carry at least a server-side one.
 * Per this repo's own device-type-conformance discipline (adding a
 * cluster the device type's own XML doesn't list would make the endpoint
 * non-conformant, the same reasoning firmware/temperature-controlled-
 * cabinet/'s own header comment applies to skipping its Identify LED
 * too), this file has no Identify cluster and no Identify LED at all —
 * the two buttons are the entire physical interface.
 *
 * Confirmed via Docker `grep` that `door_lock_controller` only exists
 * under esp-matter's "generated" data model
 * (`data_model/generated/device_types/door_lock_controller_device/`) —
 * the data model this repo has never enabled, same situation firmware/
 * doorbell/'s, firmware/chime/'s, and firmware/on-off-sensor/'s own
 * header comments already document for their own niche device types. This
 * endpoint is hand-assembled from lower-level free functions instead,
 * reusing those files' own hard-learned discipline: `cluster::
 * descriptor::create()` called explicitly right after `endpoint::
 * create()` (since `add_device_type()` alone never creates one), the
 * literal device-type-ID/revision values (0x000B, revision 3 — confirmed
 * directly against the XML's own `id`/`revision` attributes) passed
 * directly to `add_device_type()`, and an explicit `cluster::binding::
 * create()` call (confirmed `cluster::door_lock::create()` does NOT
 * auto-create one, the same check firmware/on-off-sensor/'s own header
 * comment already applies to `cluster::on_off::create()`).
 *
 * --- A genuinely new wrinkle for this repo's client-invoke pattern:
 * LockDoor/UnlockDoor are TIMED commands --------------------------------
 * Every prior client-invoke command in this repo (OnOff::Toggle/On/Off,
 * LevelControl::Step, Identify::Identify, Chime::PlayChimeSound) has
 * passed `chip::NullOptional` for `client::interaction::invoke::
 * send_request()`'s own `timed_invoke_timeout_ms` parameter — confirmed
 * by reading the DoorLock cluster's own real spec XML
 * (`data_model/1.6/clusters/DoorLock.xml`) directly that BOTH LockDoor
 * and UnlockDoor carry `access ... timed="true"`, a genuine Matter
 * protocol requirement (the client must first send a TimedRequest action
 * declaring a timeout window, then the actual InvokeRequest within that
 * window) neither of this repo's other client-invoke commands has needed
 * before. Confirmed by reading esp-matter's own `esp_matter_client.h`
 * directly that `send_request()`'s 6th parameter exists specifically for
 * this — a real, non-null `Optional<uint16_t>` millisecond value, not
 * `chip::NullOptional`. `DOOR_LOCK_CONTROLLER_TIMED_INVOKE_TIMEOUT_MS`
 * (1000ms) is a plain, reasonable window for a same-LAN command with no
 * canonical spec-mandated value found (the spec only requires the client
 * choose "a reasonable" timeout, not a specific number) — adjustable, not
 * a value sourced from any reference implementation's own hardcoded
 * default (none was found to cite; `esp-matter`'s own single real
 * reference use, in `examples/camera/main/camera-device.cpp`, uses a
 * 30ms value for a completely unrelated command, not a canonical
 * DoorLock-class default). `LockDoor`'s/`UnlockDoor`'s own PINCode field
 * (id 0, `octstr`) is confirmed, by reading that same cluster XML
 * directly, to be present ONLY when both the COTA and PIN features are
 * enabled (`optionalConform` inside an `andTerm` of both) — genuinely
 * absent for a plain lock like firmware/door-lock/'s own (no PIN feature
 * at all), so both commands are sent with an empty `"{}"` JSON payload,
 * the same "omit a field the spec marks optional-and-unsupported" choice
 * firmware/doorbell/'s own `Chime::PlayChimeSound` (ChimeID omitted)
 * already establishes.
 *
 * Two buttons (Lock, Unlock) — matching real hardware panels of this
 * kind, and avoiding the ambiguity a single "toggle" button would create
 * (DoorLock has no Toggle command of its own, unlike OnOff — a controller
 * genuinely has to pick a direction). Debounce reuses the same simple
 * shared-task, N-configured-inputs shape firmware/switch/'s own multi-
 * button design already establishes, simplified to short-press-only (no
 * long-press gesture needed — there's no second command per button to
 * disambiguate the way firmware/dimmer-switch/'s single button needs
 * short-vs-long for Toggle vs. Identify).
 *
 * --- A real, Docker-build-caught linker gap shared with firmware/
 * door-lock/'s own SERVER-side header comment — hit here from the
 * CLIENT side instead ------------------------------------------------------
 * `emberAfDoorLockClusterInitCallback` has a plain, non-weak prototype in
 * zzz_generated/app-common/app-common/zap-generated/callback.h, and
 * esp-matter's own `door_lock::function_list` (data_model/legacy/
 * esp_matter_cluster.cpp) references it unconditionally, regardless of
 * which cluster flags (SERVER, CLIENT, or both) a given `door_lock::
 * create()` call actually uses — confirmed by an actual Docker build
 * failure (`undefined reference to emberAfDoorLockClusterInitCallback`)
 * even though this file only ever creates a CLIENT-side DoorLock cluster,
 * never a SERVER one. Unlike firmware/door-lock/'s own real definition
 * (which calls `DoorLockServer::Instance().InitServer(endpoint)` since
 * that file genuinely has server state to initialize), this file's own
 * definition is an honest no-op — there is no SERVER-side DoorLock
 * cluster on this endpoint at all, so `CLUSTER_FLAG_INIT_FUNCTION` never
 * actually fires this callback at runtime; the symbol only needs to
 * exist to satisfy the linker.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pushbutton hardware for this device type physically
 * available when written, and — like every client-invoke device in this
 * repo — verifying this one for real also needs a second, already-
 * commissioned bindable Door Lock device on the same fabric).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_client.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/door-lock-server/door-lock-server.h>

static const char *TAG = "matter_door_lock_controller";

/* See the header comment above for why this is a required, but
 * intentionally empty, definition — this endpoint has no SERVER-side
 * DoorLock cluster for it to actually initialize anything for. */
void emberAfDoorLockClusterInitCallback(chip::EndpointId endpoint)
{
}

/* Two momentary pushbuttons — active-LOW, internal pull-up. Reference
 * wiring: GND -> button -> GPIO, same convention this repo's other
 * buttons use. */
#define DOOR_LOCK_CONTROLLER_LOCK_BUTTON_GPIO GPIO_NUM_4
#define DOOR_LOCK_CONTROLLER_UNLOCK_BUTTON_GPIO GPIO_NUM_16

/* Debounce: same N-consistent-10ms-samples shape firmware/dimmer-switch/'s
 * own single-button debounce already establishes. */
#define DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES 8

/* See the header comment above for why this needs to be a real, non-null
 * timeout rather than chip::NullOptional. */
#define DOOR_LOCK_CONTROLLER_TIMED_INVOKE_TIMEOUT_MS 1000

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t door_lock_controller_endpoint_id = 0;

/* Called by the Matter stack once a bound peer has been resolved. Both
 * commands take an empty payload — see the header comment above for why. */
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

    if (req_handle->command_path.mClusterId != DoorLock::Id ||
        (req_handle->command_path.mCommandId != DoorLock::Commands::LockDoor::Id &&
         req_handle->command_path.mCommandId != DoorLock::Commands::UnlockDoor::Id)) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster/command 0x%04lx/0x%02lx",
                 (unsigned long)req_handle->command_path.mClusterId, (unsigned long)req_handle->command_path.mCommandId);
        return;
    }

    /* Real, non-null timeout — LockDoor/UnlockDoor are both timed
     * commands, see the header comment above. */
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::Optional<uint16_t>(DOOR_LOCK_CONTROLLER_TIMED_INVOKE_TIMEOUT_MS));
}

/* Sends a real LockDoor or UnlockDoor command to whatever this endpoint's
 * Binding cluster resolves to. */
static void send_bound_command(uint32_t command_id)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = DoorLock::Id;
    req_handle.command_path.mCommandId = command_id;
    req_handle.request_data = NULL;

    /* Stack lock required before calling into esp-matter's client APIs
     * from a plain FreeRTOS task, same as every other client-invoke
     * device in this repo. */
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(door_lock_controller_endpoint_id, &req_handle);
}

/* One shared task debouncing both buttons — same "one shared task, N
 * configured inputs" shape firmware/switch/'s own multi-button design
 * already establishes, simplified to short-press-only (no second gesture
 * needed on either button). */
static void button_task(void *arg)
{
    bool lock_debounced = false;
    int lock_consistent = 0;
    bool lock_last_raw = (gpio_get_level(DOOR_LOCK_CONTROLLER_LOCK_BUTTON_GPIO) == 0);

    bool unlock_debounced = false;
    int unlock_consistent = 0;
    bool unlock_last_raw = (gpio_get_level(DOOR_LOCK_CONTROLLER_UNLOCK_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        bool lock_raw = (gpio_get_level(DOOR_LOCK_CONTROLLER_LOCK_BUTTON_GPIO) == 0); /* active-LOW */
        if (lock_raw == lock_last_raw) {
            if (lock_consistent < DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES) {
                lock_consistent++;
            }
        } else {
            lock_last_raw = lock_raw;
            lock_consistent = 0;
        }
        if (lock_consistent == DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES && lock_raw && !lock_debounced) {
            lock_debounced = true;
            ESP_LOGI(TAG, "Lock button pressed — sending LockDoor to bound device(s)");
            send_bound_command(DoorLock::Commands::LockDoor::Id);
        } else if (lock_consistent == DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES && !lock_raw && lock_debounced) {
            lock_debounced = false;
        }

        bool unlock_raw = (gpio_get_level(DOOR_LOCK_CONTROLLER_UNLOCK_BUTTON_GPIO) == 0); /* active-LOW */
        if (unlock_raw == unlock_last_raw) {
            if (unlock_consistent < DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES) {
                unlock_consistent++;
            }
        } else {
            unlock_last_raw = unlock_raw;
            unlock_consistent = 0;
        }
        if (unlock_consistent == DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES && unlock_raw && !unlock_debounced) {
            unlock_debounced = true;
            ESP_LOGI(TAG, "Unlock button pressed — sending UnlockDoor to bound device(s)");
            send_bound_command(DoorLock::Commands::UnlockDoor::Id);
        } else if (unlock_consistent == DOOR_LOCK_CONTROLLER_DEBOUNCE_SAMPLES && !unlock_raw && unlock_debounced) {
            unlock_debounced = false;
        }
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

/* This device type has no Identify cluster at all (confirmed by reading
 * its own XML directly — see the header comment above) — kept as a
 * trivial stub, never actually invoked. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
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

    /* 2. Configure both buttons — active-LOW, internal pull-up. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << DOOR_LOCK_CONTROLLER_LOCK_BUTTON_GPIO) |
                                   (1ULL << DOOR_LOCK_CONTROLLER_UNLOCK_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(button_task, "door_lock_controller_buttons", 4096, NULL, 5, NULL);

    /* 3. Build the Matter data model: one node, one Door Lock Controller
     * endpoint, hand-assembled since no legacy top-level helper exists —
     * see the header comment above for the full detail. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Door Lock Controller",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create door lock controller endpoint");
        return;
    }

    cluster::descriptor::config_t descriptor_config;
    cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);

    err = add_device_type(endpoint, 0x000B /* Door Lock Controller */, 3 /* device type revision */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add door lock controller device type: %d", err);
        return;
    }

    cluster::binding::config_t binding_config;
    cluster::binding::create(endpoint, &binding_config, CLUSTER_FLAG_SERVER);

    cluster::door_lock::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);

    door_lock_controller_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Door lock controller endpoint id: %u", door_lock_controller_endpoint_id);

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
     * own single global registration) — this device only has the one
     * endpoint anyway. binding_manager_init() (which resolves bindings
     * set up via a controller's Binding cluster) runs on its own, inside
     * esp_matter::start() above — no explicit call needed here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);

    ESP_LOGI(TAG, "Matter door lock controller started. Scan the QR code to commission.");
}
