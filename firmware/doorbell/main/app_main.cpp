/*
 * Minimal Matter Doorbell — forty-third device type, and this repo's first
 * over the Chime cluster family, built as a natural companion pair with
 * firmware/chime/ (the forty-fourth): this file is the pushbutton at the
 * front door, firmware/chime/ is the receiving buzzer/speaker unit a
 * controller binds it to. "Doorbell" had already come up twice as a
 * recommended-but-unchosen option (during firmware/closure/'s own and a
 * later AskUserQuestion round) before finally being picked.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type + scope: no top-level helper, hand-assembled -----------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * Doorbell.xml (device type 0x0148, revision 2): Identify (server) +
 * Switch (server, MS feature only) + Chime (CLIENT side) are all
 * `<mandatoryConform/>` — those three clusters are the entire device type.
 * Confirmed by grepping esp-matter's own `esp_matter_endpoint.cpp` (the
 * legacy data model this repo's sdkconfig actually compiles against — see
 * firmware/refrigerator/'s own header comment for why that specific
 * distinction matters) that NO `endpoint::doorbell::create()` top-level
 * helper exists there at all (esp-matter's newer, NOT-enabled-here
 * "generated" data model does have one, under `data_model/generated/
 * device_types/doorbell_device/` — deliberately not used, same "read the
 * implementation this project's own Kconfig actually compiles against"
 * discipline firmware/refrigerator/'s legacy-vs-generated gap already
 * established). This endpoint is therefore hand-assembled from lower-level
 * free functions, the same approach firmware/color-light/'s and firmware/
 * addressable-light/'s own ExtendedColorLight endpoints already use for the
 * identical reason (no top-level helper for what's needed) — including
 * their own hard-learned lesson: `cluster::descriptor::create()` is called
 * explicitly, right after `endpoint::create()`, before anything else,
 * since `add_device_type()` only appends to the endpoint's internal
 * `device_types[]` array and does NOT create a Descriptor cluster object a
 * controller can actually read — omitting it was the real reason those two
 * files' endpoints showed up as an unrecognized "Not compatible" accessory
 * in Apple Home before the fix (see their own header comments and
 * CLAUDE.md's "Open next steps" for the full debugging story).
 *
 * --- Switch cluster: MS only, not the full firmware/generic-switch/ set -
 * Doorbell's own XML marks only the `MS` (MomentarySwitch) feature
 * mandatory on its Switch cluster — MSR/MSL/MSM (release/long-press/
 * multi-press, all of which firmware/generic-switch/ enables) aren't
 * listed at all. A real doorbell button doesn't need long-press or
 * multi-click semantics — "someone pressed it" is the entire signal — so
 * this file only ever calls `SwitchCluster::OnInitialPress()`, the same
 * "smallest reasonable next step" scoping this repo applies to every
 * other device type's own feature choices. `SwitchCluster` itself is
 * confirmed to be the identical registry-registered `DefaultServerCluster`
 * firmware/generic-switch/'s own header comment already documents in
 * full (plain ember NumberOfPositions/CurrentPosition attributes, no
 * `config->delegate` field, events reached via `OnXxx()` methods looked up
 * through the data model provider's registry) — reused via the same
 * `cluster::switch_cluster::create()` free function, just with only the
 * `momentary_switch` feature bit set instead of all four. Confirmed by
 * reading `SwitchCluster.cpp`'s own `OnInitialPress()`/`OnShortRelease()`
 * directly that each `OnXxx()` method independently checks its own
 * required feature bit and safely no-ops if that feature isn't enabled —
 * so this file could technically call the release/long-press variants
 * too without harm, but doesn't, to keep the code's own intent matching
 * exactly what's advertised.
 *
 * --- Chime CLIENT + Binding: the real "ring the bell" mechanism ---------
 * The mandatory client-side Chime cluster is what actually lets this
 * button ring a real, physical chime/buzzer once bound to one (a
 * controller — e.g. Home Assistant's own "Bindings" UI — sets up a
 * Binding-cluster entry pointing at firmware/chime/'s own endpoint,
 * exactly the same controller-driven binding step firmware/switch/'s own
 * bound OnOff buttons and firmware/thermostat/'s BINDING output mode
 * already require). Confirmed there is no `cluster::chime::create()`
 * helper at ANY level in esp-matter's legacy data model (unlike e.g.
 * `cluster::on_off::create(endpoint, NULL, CLUSTER_FLAG_CLIENT)`, which
 * firmware/switch/'s and firmware/thermostat/'s own client-side OnOff
 * clusters already reuse) — Chime is new enough (Matter 1.5/1.6) that no
 * per-cluster ember-shell wrapper exists for it at all yet, client or
 * server. Worked around with esp-matter's own genuinely generic,
 * cluster-agnostic primitive instead — `esp_matter::cluster::create(
 * endpoint, cluster_id, flags)` (declared in `esp_matter_data_model.h`,
 * the same underlying function every named per-cluster helper is built
 * from) — called directly with `Chime::Id` and `CLUSTER_FLAG_CLIENT`.
 * This raw shell is what makes the cluster show up in this endpoint's
 * `ClientList` at all: confirmed by reading esp-matter's own
 * `data_model_provider::ClientClusters()`/`ServerClusters()` directly that
 * both walk the endpoint's own linked list of ember `cluster_t` entries
 * looking for the right `CLUSTER_FLAG_*` bit — NOT a live query against the
 * registry the way one might assume from how many other "code-driven"
 * clusters in this repo are reached; the registry is only consulted
 * afterward, for per-cluster metadata (flags/data version) on entries
 * that already exist in that ember list. A worthwhile, previously-
 * undocumented distinction for any future client-side or ember-shell-less
 * cluster added to this repo: `ServerList`/`ClientList` visibility comes
 * from the ember cluster_t list, not from the data model provider's own
 * registry, even on a version of esp-matter where individual clusters'
 * actual attribute/command handling has largely moved off ember entirely.
 * `client::interaction::invoke::send_request()` then sends a real
 * `Chime::Commands::PlayChimeSound` with an empty payload (`"{}"` — no
 * ChimeID field) to whatever this endpoint's Binding cluster resolves to,
 * the exact same `client::cluster_update()` + `client::set_request_callback()`
 * pattern firmware/switch/'s own buttons and firmware/thermostat/'s
 * BINDING output mode already establish — deliberately not selecting a
 * specific ChimeID: which sound to actually play is the bound Chime
 * device's own SelectedChime attribute to decide, the separation of
 * concerns Matter's Binding model is designed around. Standard quick-
 * power-cycle factory reset. Build-verified in Docker; not hardware-
 * tested (no separate chime/buzzer device to bind to was physically
 * available when written — see firmware/chime/ for that half).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_client.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/switch-server/switch-server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_doorbell";

/* Momentary pushbutton — active-LOW, internal pull-up. Reference wiring:
 * GND -> button -> GPIO, same convention firmware/switch/'s and firmware/
 * generic-switch/'s own buttons already use. */
#define DOORBELL_BUTTON_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Debounce constants — same "N consistent samples" technique and default
 * timing firmware/generic-switch/'s own header comment already documents
 * in full; no long-press/multi-press timing needed here at all (see the
 * header comment above on scope), so this is a plain debounced-edge
 * detector, not a full press-timing state machine. */
#define DOORBELL_POLL_INTERVAL_MS 10
#define DOORBELL_DEBOUNCE_SAMPLES 3 /* 3 x 10ms = 30ms of consistent reading before accepting an edge */

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t doorbell_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Registry-lookup-and-cast — same pattern firmware/generic-switch/'s own
 * get_switch_cluster() already establishes. */
static SwitchCluster *get_switch_cluster(void)
{
    chip::app::ConcreteClusterPath path(doorbell_endpoint_id, Switch::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "Switch cluster not found on endpoint %u", doorbell_endpoint_id);
        return nullptr;
    }
    return static_cast<SwitchCluster *>(iface);
}

/* Called by the Matter stack once a bound peer has been resolved, for
 * every outstanding request queued via client::cluster_update(). Only
 * Chime-cluster invoke requests are expected here (that's all this
 * doorbell ever sends) — same filter-and-forward shape firmware/switch/'s
 * own app_client_request_cb already establishes for OnOff. */
static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "PlayChimeSound acknowledged by bound chime device");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "PlayChimeSound failed: %" CHIP_ERROR_FORMAT, error.Format());
}

static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }
    if (req_handle->command_path.mClusterId != Chime::Id) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster 0x%04lx",
                 (unsigned long)req_handle->command_path.mClusterId);
        return;
    }
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

/* Debounces the button, then both (a) fires a real Switch InitialPress
 * event for local automations bound directly to this endpoint, and (b)
 * sends Chime::PlayChimeSound to whatever this endpoint's Binding cluster
 * resolves to — see the header comment above for why a real doorbell
 * needs both. Polling, not ISR+queue, matching firmware/generic-switch/'s
 * own technique — simpler here even than that file's, since there's no
 * press-duration state to track at all. */
static void button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(DOORBELL_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DOORBELL_POLL_INTERVAL_MS));

        bool raw_pressed = (gpio_get_level(DOORBELL_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < DOORBELL_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        /* Debounced press edge. */
        if (consistent_samples == DOORBELL_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;

            SwitchCluster *cluster = get_switch_cluster();
            if (cluster) {
                cluster->SetCurrentPosition(1);
                cluster->OnInitialPress(1);
            }
            ESP_LOGI(TAG, "Doorbell pressed — InitialPress fired, ringing bound chime(s)");

            client::request_handle_t req_handle;
            req_handle.type = client::INVOKE_CMD;
            req_handle.command_path.mClusterId = Chime::Id;
            req_handle.command_path.mCommandId = Chime::Commands::PlayChimeSound::Id;
            {
                /* We're running in a plain FreeRTOS task, not the Matter
                 * event loop — the stack lock is required before calling
                 * into esp-matter's client APIs from here, same as
                 * firmware/switch/'s own button_task. */
                lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
                client::cluster_update(doorbell_endpoint_id, &req_handle);
            }
        }

        /* Debounced release edge — CurrentPosition only; MSR isn't
         * enabled, so no ShortRelease event is fired (see the header
         * comment on scope). */
        if (consistent_samples == DOORBELL_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
            SwitchCluster *cluster = get_switch_cluster();
            if (cluster) {
                cluster->SetCurrentPosition(0);
            }
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

/* CurrentPosition is only ever written locally by button_task() above (via
 * SwitchCluster::SetCurrentPosition(), not the generic attribute::update()
 * path) — so this is a no-op required by node::create()'s callback
 * signature, same as firmware/generic-switch/'s own. */
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
    button_io_conf.pin_bit_mask = (1ULL << DOORBELL_BUTTON_GPIO);
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

    /* 3. Build the Matter data model: one node, one hand-assembled
     * Doorbell endpoint — see the header comment above for why no
     * top-level helper exists and what each manual step below is for. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Doorbell",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint_t *endpoint = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create endpoint");
        return;
    }

    /* Descriptor cluster FIRST — see the header comment above for the
     * real bug class this avoids (add_device_type() alone does not
     * create one). */
    cluster::descriptor::config_t descriptor_config;
    cluster_t *descriptor_cluster = cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);
    if (!descriptor_cluster) {
        ESP_LOGE(TAG, "Failed to create descriptor cluster");
        return;
    }

    add_device_type(endpoint, 0x0148 /* Doorbell */, 2 /* device type revision */);

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kVisibleIndicator);
    cluster_t *identify_cluster = cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);
    cluster::identify::command::create_trigger_effect(identify_cluster);

    /* Switch — MS feature only, see the header comment above on scope. */
    cluster::switch_cluster::config_t switch_config;
    switch_config.number_of_positions = 2;
    switch_config.current_position = 0;
    switch_config.feature_flags = (uint32_t)cluster::switch_cluster::feature::momentary_switch::get_id();
    cluster_t *switch_cluster = cluster::switch_cluster::create(endpoint, &switch_config, CLUSTER_FLAG_SERVER);
    if (!switch_cluster) {
        ESP_LOGE(TAG, "Failed to create switch cluster");
        return;
    }

    /* Binding + client-side Chime — see the header comment above for why
     * Chime needs the raw, cluster-agnostic esp_matter::cluster::create()
     * primitive rather than a named helper. */
    cluster::binding::config_t binding_config;
    cluster_t *binding_cluster = cluster::binding::create(endpoint, &binding_config, CLUSTER_FLAG_SERVER);
    if (!binding_cluster) {
        ESP_LOGE(TAG, "Failed to create binding cluster");
        return;
    }
    cluster::create(endpoint, Chime::Id, CLUSTER_FLAG_CLIENT);

    doorbell_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Doorbell endpoint id: %u", doorbell_endpoint_id);

    xTaskCreate(button_task, "doorbell_button", 4096, NULL, 5, NULL);

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

    /* Endpoint-agnostic by design (same reasoning as firmware/switch/'s
     * single global registration) — this device only has the one
     * endpoint anyway. binding_manager_init() (which resolves bindings
     * set up via a controller's Binding cluster) runs on its own, inside
     * esp_matter::start() above — no explicit call needed here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);

    ESP_LOGI(TAG, "Matter doorbell started. Scan the QR code to commission.");
}
