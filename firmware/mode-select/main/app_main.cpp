/*
 * Minimal Matter Mode Select — fifty-second device type: a physical scene/
 * activity selector (three buttons — Home/Away/Night, the classic security-
 * panel-style mode set), reporting which mode is active via the Mode
 * Select cluster and reachable equally from a controller's own ChangeToMode
 * command.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Why this device type, and why now ------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * ModeSelectDeviceType.xml: Mode Select (0x0027) is the whole device type —
 * exactly one mandatoryConform cluster, no options at all. This is Matter's
 * older, simpler predecessor to the ModeBase-derived family this repo has
 * already used five times (WaterHeaterMode, RvcRunMode/RvcCleanMode,
 * DishwasherMode, LaundryWasherMode) — genuinely thin as a device type on
 * its own, which is exactly why it was passed over earlier in favor of
 * Cooktop/On-Off-Sensor/Solar Power; picked up now as part of working
 * through every remaining real Matter device type.
 *
 * `endpoint::mode_select::create()` confirmed complete/ready-to-use by
 * reading esp-matter's own legacy `mode_select::add()` directly — auto-
 * Descriptor via `common::create<T>()`, plus the ModeSelect cluster itself
 * via ONE config_t. Confirmed NOT code-driven (no `mode_select/` folder
 * under `data_model_provider/clusters/` — its own function_list is the
 * classic legacy pair, `emberAfModeSelectClusterServerInitCallback` +
 * `MatterModeSelectClusterServerPreAttributeChangedCallback`) — CurrentMode
 * is a plain ember attribute, written via the same `attribute::update()`
 * call every other plain-ember cluster in this repo uses, both for this
 * file's own local button presses and (confirmed by reading
 * `mode-select-server.cpp`'s own `ChangeToMode()` directly) for a remote
 * controller's ChangeToMode command — that command's own handler validates
 * the requested mode against the SupportedModesManager (below) and calls
 * `ModeSelect::Attributes::CurrentMode::Set()` itself; no app code is
 * needed to accept the command, only to react to the resulting attribute
 * change via `attribute::PRE_UPDATE`, same as every other plain-ember
 * cluster's attribute-driven pattern already established in this repo.
 *
 * --- SupportedModes: a genuinely new "how do I supply a cluster's
 * supported-value list" pattern for this repo ------------------------------
 * `cluster::mode_select::config_t`'s own `delegate` field turned out to BE
 * the mechanism for this — confirmed by reading `ModeSelectDelegateInitCB`
 * directly: it casts that same `delegate` pointer straight to a
 * `ModeSelect::SupportedModesManager*` and calls `ModeSelect::
 * setSupportedModesManager()` — a single, GLOBAL, static manager (same
 * "static class method, no per-cluster-instance construction" shape
 * firmware/cooktop/'s own `TemperatureControlCluster::SetDelegate()`
 * already established, not the per-endpoint `config->delegate` +
 * `InitModeDelegate()` auto-construction shape the five ModeBase-derived
 * clusters above use). `ModeSelectManager` here is ported from
 * connectedhomeip's own real reference
 * (`examples/chef/common/clusters/mode-select/chef-supported-modes-
 * manager.h`, read directly), simplified for a single fixed
 * `ModeOptionsProvider` (begin/end pointers into one static
 * `ModeOptionStruct::Type` array) shared by every endpoint, the same
 * "simplify a per-endpoint std::map reference down to one fixed list"
 * step firmware/cooktop/'s own `CookSurfaceLevelsDelegate` already took.
 * Each `ModeOptionStruct` here uses an EMPTY `SemanticTags` list rather
 * than a placeholder manufacturer-specific tag (chef's own reference
 * fills one in, `{mfgCode: 0xFFF1, value: 0}`, purely as an example) —
 * confirmed the field only carries a `<maxCount value="64"/>` constraint
 * with no minimum, so an empty list is spec-legal, and more honest than
 * inventing a tag value with no real meaning behind it for "Home"/"Away"/
 * "Night" (no CSA-standard semantic tag namespace exists for this kind of
 * generic security-panel mode set).
 *
 * --- Hardware: three buttons, no dedicated indicator LEDs ------------------
 * `MODE_SELECT_BUTTON_HOME_GPIO`/`_AWAY_GPIO`/`_NIGHT_GPIO` — a plain
 * momentary pushbutton per mode (GND -> button -> GPIO, same wiring
 * convention as every other button in this repo), read by one shared
 * FreeRTOS task polling all three with a simple per-button debounce (same
 * "one shared task, N configured inputs" shape firmware/switch/'s own
 * multi-button design already establishes) — a confirmed press writes
 * CurrentMode directly. No separate per-mode indicator LED: unlike e.g.
 * firmware/outlet/'s own optional status LED, nothing in this cluster's
 * own spec calls for one, and CurrentMode is already directly visible to
 * any bound controller — a deliberate, documented scope cut rather than
 * an oversight. Boots to mode 0 ("Home"), matching every other device
 * type's own boot-to-a-known-state convention.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no pushbutton hardware for this specific device type
 * physically available when written — though this repo's other momentary-
 * button device types already confirm the underlying breadboard-
 * pushbutton wiring works on real hardware).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <esp_matter.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-select-server/supported-modes-manager.h>

static const char *TAG = "matter_mode_select";

/* --- GPIO pin map -----------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). Reference wiring:
 * GND -> button -> GPIO, same convention every other button in this repo
 * uses. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define MODE_SELECT_BUTTON_HOME_GPIO GPIO_NUM_4
#define MODE_SELECT_BUTTON_AWAY_GPIO GPIO_NUM_16
#define MODE_SELECT_BUTTON_NIGHT_GPIO GPIO_NUM_17

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Button debounce — plain N-consistent-samples poll, same shape firmware/
 * cooktop/'s own power_button_task() already establishes. */
#define MODE_SELECT_BUTTON_POLL_INTERVAL_MS 20
#define MODE_SELECT_BUTTON_DEBOUNCE_SAMPLES 3

#define MODE_SELECT_NUM_MODES 3

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using chip::operator""_span;

static uint16_t mode_select_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- SupportedModesManager --------------------------------------------------
 * See the header comment above for why this is a single, global, static
 * manager (not a per-endpoint auto-constructed instance) and why each
 * option's own SemanticTags list is deliberately empty. */
static const char *const kModeNames[MODE_SELECT_NUM_MODES] = { "Home", "Away", "Night" }; /* log convenience only */

static const ModeSelect::Structs::ModeOptionStruct::Type kModeOptions[MODE_SELECT_NUM_MODES] = {
    { .label = "Home"_span, .mode = 0, .semanticTags = {} },
    { .label = "Away"_span, .mode = 1, .semanticTags = {} },
    { .label = "Night"_span, .mode = 2, .semanticTags = {} },
};

class ModeSelectManager : public ModeSelect::SupportedModesManager
{
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId endpointId) const override
    {
        return ModeOptionsProvider(&kModeOptions[0], &kModeOptions[MODE_SELECT_NUM_MODES]);
    }

    chip::Protocols::InteractionModel::Status getModeOptionByMode(
        chip::EndpointId endpointId, uint8_t mode, const ModeSelect::Structs::ModeOptionStruct::Type **dataPtr) const override
    {
        for (uint8_t i = 0; i < MODE_SELECT_NUM_MODES; i++) {
            if (kModeOptions[i].mode == mode) {
                *dataPtr = &kModeOptions[i];
                return chip::Protocols::InteractionModel::Status::Success;
            }
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
};

static ModeSelectManager mode_select_manager;

/* Writes CurrentMode directly on a confirmed local button press — same
 * `attribute::update()` pattern every other plain-ember cluster in this
 * repo uses (a remote ChangeToMode command reaches the same attribute
 * through the cluster's own generic handler instead — see the header
 * comment above). */
static void set_current_mode(uint8_t mode)
{
    esp_matter_attr_val_t val = esp_matter_uint8(mode);
    attribute::update(mode_select_endpoint_id, ModeSelect::Id, ModeSelect::Attributes::CurrentMode::Id, &val);
    ESP_LOGI(TAG, "Button press — mode set to %u (%s)", mode, kModeNames[mode]);
}

/* One shared task polling all three buttons — same "one shared task, N
 * configured inputs" shape firmware/switch/'s own multi-button design
 * already establishes. */
struct mode_button_t {
    gpio_num_t gpio;
    uint8_t mode;
    bool last_sample_pressed;
    int consistent_count;
    bool was_pressed;
};

static void mode_buttons_task(void *arg)
{
    mode_button_t buttons[MODE_SELECT_NUM_MODES] = {
        { MODE_SELECT_BUTTON_HOME_GPIO, 0, false, 0, false },
        { MODE_SELECT_BUTTON_AWAY_GPIO, 1, false, 0, false },
        { MODE_SELECT_BUTTON_NIGHT_GPIO, 2, false, 0, false },
    };

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(MODE_SELECT_BUTTON_POLL_INTERVAL_MS));

        for (int i = 0; i < MODE_SELECT_NUM_MODES; i++) {
            mode_button_t *btn = &buttons[i];
            bool sample_pressed = (gpio_get_level(btn->gpio) == 0);
            if (sample_pressed == btn->last_sample_pressed) {
                btn->consistent_count++;
            } else {
                btn->consistent_count = 1;
                btn->last_sample_pressed = sample_pressed;
            }

            if (btn->consistent_count >= MODE_SELECT_BUTTON_DEBOUNCE_SAMPLES) {
                if (sample_pressed && !btn->was_pressed) {
                    set_current_mode(btn->mode);
                }
                btn->was_pressed = sample_pressed;
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

/* Reacts to CurrentMode changes — covers both this file's own button
 * presses and a remote ChangeToMode command, since both funnel through
 * the same ember attribute store (see the header comment above). No
 * dedicated indicator LED (see the header comment above for why) — this
 * is purely a log line confirming the change reached the data model. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && cluster_id == ModeSelect::Id &&
        attribute_id == ModeSelect::Attributes::CurrentMode::Id) {
        uint8_t mode = val->val.u8;
        const char *label = (mode < MODE_SELECT_NUM_MODES) ? kModeNames[mode] : "?";
        ESP_LOGI(TAG, "CurrentMode -> %u (%s)", mode, label);
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

    /* 2. Configure the three mode buttons — pulled up, active-LOW. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << MODE_SELECT_BUTTON_HOME_GPIO) |
                                   (1ULL << MODE_SELECT_BUTTON_AWAY_GPIO) |
                                   (1ULL << MODE_SELECT_BUTTON_NIGHT_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);
    xTaskCreate(mode_buttons_task, "mode_buttons_task", 3072, NULL, 5, NULL);

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

    /* 3. Build the Matter data model: one node, one Mode Select endpoint —
     * Descriptor + ModeSelect, both via the complete top-level helper. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Mode Select",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::mode_select::config_t mode_select_config;
    strncpy(mode_select_config.mode_select.description, "House Mode",
            sizeof(mode_select_config.mode_select.description) - 1);
    mode_select_config.mode_select.current_mode = 0; /* boots to "Home" */
    mode_select_config.mode_select.delegate = &mode_select_manager;

    endpoint_t *endpoint = endpoint::mode_select::create(node, &mode_select_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create mode select endpoint");
        return;
    }
    mode_select_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Mode select endpoint id: %u", mode_select_endpoint_id);

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

    ESP_LOGI(TAG, "Matter mode select started. Scan the QR code to commission.");
}
