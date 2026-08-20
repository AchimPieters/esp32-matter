/*
 * Minimal Matter Air Purifier — nineteenth device type, and a direct
 * extension of firmware/fan/: same FanControl cluster, same PWM output
 * (reused close to verbatim), plus HepaFilterMonitoring and
 * ActivatedCarbonFilterMonitoring on the same endpoint — the two clusters
 * that actually make this an "Air Purifier" rather than a plain Fan.
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
 * --- Endpoint: esp-matter's own complete top-level helper, plus two extra
 * clusters added onto the SAME already-correct endpoint ------------------
 * `endpoint::air_purifier::create()` (device type 0x002D) confirmed
 * complete/ready-to-use — Identify + FanControl, via `common::create<T>()`
 * (auto-Descriptor) — by reading esp_matter_endpoint.cpp's own
 * `air_purifier::add()` directly. Confirmed against the CSA's own
 * data_model/1.6/device_types/AirPurifier.xml: Identify + FanControl are
 * the only `<mandatoryConform/>` clusters; Groups, On/Off, HEPA Filter
 * Monitoring, and Activated Carbon Filter Monitoring are all
 * `<optionalConform/>`. Groups/On-Off deliberately not added — same
 * scope decision firmware/fan/ already made (PercentSetting=0 already
 * means off; see that file's own header comment for why). The two filter-
 * monitoring clusters ARE added — they're the entire point of building
 * this device type separately from Fan — via their own
 * `cluster::hepa_filter_monitoring::create()`/`cluster::
 * activated_carbon_filter_monitoring::create()` free functions, called
 * onto the SAME endpoint after `air_purifier::create()` already built it
 * correctly. Same "add extra clusters onto an already-correct endpoint"
 * pattern firmware/thermostat/'s BINDING output type and firmware/
 * air-quality-sensor/'s concentration-measurement clusters already
 * established — this does NOT reintroduce the missing-Descriptor bug
 * (that bug was about hand-assembling an endpoint from raw
 * `endpoint::create()` instead of a complete helper).
 *
 * --- FanControl: reused near-verbatim from firmware/fan/ -------------------
 * Same Delegate-based integration (`FanControl::Delegate` subclass,
 * `SetDefaultDelegate()`, registry-lookup-and-cast for
 * `SetPercentCurrent()` since esp-matter's own `fan_control/
 * integration.cpp` only implements `SetDefaultDelegate()`, not the
 * `Attributes::X::Set()` free functions connectedhomeip's generic header
 * declares) — see firmware/fan/main/app_main.cpp's own header comment for
 * the full detail on why, including the two real, sequential Docker build
 * failures (a compile error, then a link error) that established this
 * pattern in the first place. Same PercentSetting/PercentCurrent-only
 * scope (no MultiSpeed/Auto/Rocking/Wind/Step/AirflowDirection), same
 * OffLowMedHigh FanModeSequence, same 25kHz LEDC PWM output.
 *
 * --- Filter monitoring: a real public feature API, not a workaround ------
 * Confirmed by reading esp-matter's own source directly: `resource_
 * monitoring::create()` (the shared template behind both
 * `hepa_filter_monitoring::create()` and `activated_carbon_filter_
 * monitoring::create()`) hardcodes `global::attribute::create_feature_map
 * (cluster, 0)` just like firmware/air-quality-sensor/'s AirQuality and
 * firmware/water-leak-detector/'s BooleanState — but UNLIKE those two,
 * esp-matter DOES expose a real, public way to enable the Condition
 * feature afterwards: `cluster::resource_monitoring::feature::condition::
 * add(cluster, &condition_config)`, which calls `update_feature_map()`
 * (a proper read-modify-write, confirmed by reading its implementation)
 * and creates the Condition + DegradationDirection attributes. This is
 * the same `feature::xxx::add()` pattern firmware/color-light/'s
 * ColorControl features and firmware/air-quality-sensor/'s
 * NumericMeasurement feature already use — a documented API, not a raw
 * FeatureMap attribute override. It still has to run before
 * `esp_matter::start()`: `ResourceMonitoringCluster` (confirmed
 * code-driven — a real `resource_monitor/` folder exists under
 * `data_model_provider/clusters/`) reads FeatureMap once, at its own
 * server-init callback, the same "constructor-time BitFlags<Feature>
 * snapshot" pattern firmware/air-quality-sensor/'s `AirQualityCluster`
 * already established. Warning/ReplacementProductList features and the
 * ResetCondition command are NOT implemented — same "smallest reasonable
 * next step" scoping this repo applies to every other device type's
 * first cut.
 *
 * Updating Condition/ChangeIndication at runtime uses a real, ready-made
 * free function esp-matter's own `resource_monitor/integration.cpp`
 * provides — `chip::app::Clusters::ResourceMonitoring::
 * GetClusterInstance(endpointId, clusterId)` — rather than this repo's
 * usual registry-lookup-and-cast pattern; its returned
 * `ResourceMonitoringCluster*` has public `UpdateCondition()`/
 * `UpdateChangeIndication()` methods. Worth remembering as a fifth,
 * genuinely distinct "how do I write a code-driven cluster attribute
 * from app code" pattern in this repo now (after the plain registry-
 * lookup setter, the two Delegate variants, and the direct FeatureMap
 * `attribute::update()` override): a cluster-family-specific convenience
 * free function, when esp-matter's own integration.cpp happens to
 * provide one — always worth checking for before defaulting to the
 * registry-lookup pattern.
 *
 * --- Filter life: a time-based estimate, not a real sensor reading -------
 * No differential-pressure sensor or particulate-accounting hardware is
 * assumed — same "smallest reasonable next step" reasoning firmware/
 * window-covering/ already applies to its own time-based position
 * estimate (no position sensor assumed there either). While the fan is
 * actually running (PercentSetting > 0), elapsed operating seconds
 * accumulate in their own NVS namespace (same lightweight
 * counter-in-NVS technique this repo's own quick-power-cycle factory
 * reset already uses, reused here for a different purpose) — persisted
 * every AIR_PURIFIER_NVS_SAVE_INTERVAL_MS while running (not on every
 * tick, to avoid flash wear, same reasoning firmware/dimmable-light/'s
 * `set_deferred_persistence()` already documents for a different
 * attribute). Condition (percent remaining) is computed independently
 * against each filter's own configurable rated life in operating hours
 * — AIR_PURIFIER_HEPA_LIFE_HOURS (2000h) and
 * AIR_PURIFIER_CARBON_LIFE_HOURS (1000h), commonly-cited commercial
 * air-purifier filter-life figures (activated carbon media saturates
 * faster than HEPA media in real products, hence the shorter figure) —
 * both explicitly adjustable #defines, not calibrated measurements, same
 * "adjustable threshold, not a calibrated reading" precedent firmware/
 * smoke-co-alarm/'s own MQ classifier already established. ChangeIndication
 * is derived from Condition (>20% Ok, 5-20% Warning, <5% Critical) —
 * again a plain adjustable mapping, not a spec-defined one.
 */

#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <app/clusters/fan-control-server/CodegenIntegration.h>
#include <app/clusters/resource-monitoring-server/ResourceMonitoringCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <data_model_provider/clusters/resource_monitor/integration.h>

static const char *TAG = "matter_air_purifier";

/* Change this to the GPIO your fan's PWM/speed-control input is wired to —
 * same reasoning/wiring as firmware/fan/'s own FAN_PWM_GPIO. */
#define FAN_PWM_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (LED Control / PWM) peripheral setup — identical to firmware/fan/'s
 * own settings, see that file's header comment for the full reasoning. */
#define FAN_LEDC_TIMER LEDC_TIMER_0
#define FAN_LEDC_CHANNEL LEDC_CHANNEL_0
#define FAN_LEDC_MODE LEDC_LOW_SPEED_MODE
#define FAN_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define FAN_LEDC_FREQUENCY_HZ 25000

/* Filter life — see the header comment above: a plain time-based estimate,
 * not a real sensor reading. Both adjustable per your actual filters'
 * rated life. */
#define AIR_PURIFIER_HEPA_LIFE_HOURS 2000
#define AIR_PURIFIER_CARBON_LIFE_HOURS 1000
#define AIR_PURIFIER_CHANGE_WARNING_PERCENT 20
#define AIR_PURIFIER_CHANGE_CRITICAL_PERCENT 5

/* How often accumulated operating time is written to NVS while the fan is
 * running — a plain seconds counter, not on every tick, to avoid flash
 * wear (same reasoning firmware/dimmable-light/'s CurrentLevel deferred
 * persistence already documents for a different attribute). Condition is
 * still recomputed and pushed to the cluster every poll regardless — only
 * the NVS write itself is throttled. */
#define AIR_PURIFIER_POLL_INTERVAL_MS 5000
#define AIR_PURIFIER_NVS_SAVE_INTERVAL_MS 60000
#define AIR_PURIFIER_NVS_NAMESPACE "filter_life"
#define AIR_PURIFIER_NVS_KEY "run_seconds"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t air_purifier_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Accumulated fan-running seconds, loaded from NVS at boot and persisted
 * periodically while the fan runs — see the header comment above. */
static uint32_t total_run_seconds = 0;
static uint8_t current_percent_setting = 0;

/* Drives the LEDC duty from a 0-100 percent value — same as firmware/fan/. */
static void set_output(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)percent * 255 / 100;
    ledc_set_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL, duty);
    ledc_update_duty(FAN_LEDC_MODE, FAN_LEDC_CHANNEL);
}

/* FanControl's real Delegate — see firmware/fan/main/app_main.cpp's own
 * header comment for the full detail on why this pattern is needed. */
class FanDelegate : public FanControl::Delegate
{
public:
    chip::Protocols::InteractionModel::Status HandleStep(FanControl::StepDirectionEnum direction, bool wrap,
                                                          bool lowestOff) override
    {
        return chip::Protocols::InteractionModel::Status::UnsupportedCommand;
    }

    void OnFanDriveStateChanged(const FanControl::FanDriveState &newState) override
    {
        uint8_t percent = newState.percentSetting.IsNull() ? 0 : newState.percentSetting.Value();
        set_output(percent);
        current_percent_setting = percent;

        chip::app::ConcreteClusterPath path(air_purifier_endpoint_id, FanControl::Id);
        chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
        if (iface) {
            static_cast<FanControlCluster *>(iface)->SetPercentCurrent(percent);
        } else {
            ESP_LOGE(TAG, "FanControl cluster not found on endpoint %u", air_purifier_endpoint_id);
        }
        ESP_LOGI(TAG, "Fan mode %u, percent set to %u%%", chip::to_underlying(newState.mode), percent);
    }
};

static FanDelegate fan_delegate;

/* Computes Condition (0-100, percent life remaining) from accumulated
 * run time against a filter's own rated life in hours. Clamped at 0 —
 * a filter overdue for replacement stays at 0%, it doesn't go negative. */
static uint8_t compute_condition(uint32_t run_seconds, uint32_t life_hours)
{
    uint32_t life_seconds = life_hours * 3600u;
    if (run_seconds >= life_seconds) {
        return 0;
    }
    uint32_t remaining_percent = 100u - ((uint64_t)run_seconds * 100u) / life_seconds;
    return (uint8_t)remaining_percent;
}

static ResourceMonitoring::ChangeIndicationEnum change_indication_for(uint8_t condition_percent)
{
    if (condition_percent <= AIR_PURIFIER_CHANGE_CRITICAL_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kCritical;
    }
    if (condition_percent <= AIR_PURIFIER_CHANGE_WARNING_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kWarning;
    }
    return ResourceMonitoring::ChangeIndicationEnum::kOk;
}

/* Pushes a freshly computed Condition/ChangeIndication into one filter
 * cluster — via ResourceMonitoring::GetClusterInstance(), esp-matter's own
 * ready-made convenience free function (see the header comment on why
 * this is used instead of this repo's usual registry-lookup pattern). */
static void update_filter_cluster(uint32_t cluster_id, uint32_t life_hours, const char *label)
{
    uint8_t condition = compute_condition(total_run_seconds, life_hours);
    ResourceMonitoring::ChangeIndicationEnum indication = change_indication_for(condition);

    auto *cluster = ResourceMonitoring::GetClusterInstance(air_purifier_endpoint_id, cluster_id);
    if (!cluster) {
        ESP_LOGE(TAG, "%s filter cluster not found on endpoint %u", label, air_purifier_endpoint_id);
        return;
    }
    cluster->UpdateCondition(condition);
    cluster->UpdateChangeIndication(indication);
    ESP_LOGI(TAG, "%s filter: %u%% remaining (%s)", label, condition,
             indication == ResourceMonitoring::ChangeIndicationEnum::kCritical ? "CRITICAL" :
             indication == ResourceMonitoring::ChangeIndicationEnum::kWarning ? "WARNING" : "OK");
}

/* Polls every AIR_PURIFIER_POLL_INTERVAL_MS: accumulates run time while the
 * fan is actually on, periodically persists it to NVS, and refreshes both
 * filter clusters' Condition/ChangeIndication every poll regardless. */
static void filter_life_task(void *arg)
{
    uint32_t ms_since_save = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AIR_PURIFIER_POLL_INTERVAL_MS));

        if (current_percent_setting > 0) {
            total_run_seconds += AIR_PURIFIER_POLL_INTERVAL_MS / 1000;
            ms_since_save += AIR_PURIFIER_POLL_INTERVAL_MS;

            if (ms_since_save >= AIR_PURIFIER_NVS_SAVE_INTERVAL_MS) {
                nvs_handle_t nvs;
                if (nvs_open(AIR_PURIFIER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_set_u32(nvs, AIR_PURIFIER_NVS_KEY, total_run_seconds);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                }
                ms_since_save = 0;
            }
        }

        update_filter_cluster(HepaFilterMonitoring::Id, AIR_PURIFIER_HEPA_LIFE_HOURS, "HEPA");
        update_filter_cluster(ActivatedCarbonFilterMonitoring::Id, AIR_PURIFIER_CARBON_LIFE_HOURS, "Carbon");
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

/* Fan speed/mode changes are entirely handled through FanDelegate above;
 * filter Condition/ChangeIndication are read-only and only ever written
 * locally by filter_life_task() — so this is a no-op required by
 * node::create()'s callback signature, same as firmware/fan/'s own. */
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
 * header comment for the full mechanism and its sourcing. Deliberately a
 * separate NVS namespace from AIR_PURIFIER_NVS_NAMESPACE above — factory
 * reset erases all of NVS anyway (including the filter-life counter, a
 * reasonable side effect: a factory reset is also a sensible moment to
 * treat the filters as freshly reset, e.g. after physically swapping the
 * device to a new install). */
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

    /* 1c. Load accumulated filter run time from its own NVS namespace —
     * stays 0 on a fresh device (fresh filters). */
    nvs_handle_t filter_nvs;
    if (nvs_open(AIR_PURIFIER_NVS_NAMESPACE, NVS_READWRITE, &filter_nvs) == ESP_OK) {
        nvs_get_u32(filter_nvs, AIR_PURIFIER_NVS_KEY, &total_run_seconds);
        nvs_close(filter_nvs);
    }
    ESP_LOGI(TAG, "Loaded %lu accumulated filter run seconds from NVS", (unsigned long)total_run_seconds);

    /* 2. Configure the fan PWM output via LEDC — same as firmware/fan/. */
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
    set_output(0); /* boots off */

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

    /* 3. Build the Matter data model: one node, one Air Purifier endpoint
     * (Identify + FanControl via the complete top-level helper), plus the
     * two filter-monitoring clusters added onto that same endpoint
     * afterwards — see the header comment above for why this ordering is
     * safe. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Air Purifier",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::air_purifier::config_t air_purifier_config;
    air_purifier_config.fan_control.fan_mode_sequence =
        chip::to_underlying(FanControl::FanModeSequenceEnum::kOffLowMedHigh);
    endpoint_t *endpoint = endpoint::air_purifier::create(node, &air_purifier_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create air purifier endpoint");
        return;
    }

    air_purifier_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Air purifier endpoint id: %u", air_purifier_endpoint_id);

    /* 3b. HEPA filter monitoring, with the Condition feature (percent
     * remaining + DegradationDirection=Down) enabled via esp-matter's own
     * public feature::condition::add() API — see the header comment above
     * for why this is a real API, not a workaround, and why it still has
     * to run before esp_matter::start(). */
    cluster::hepa_filter_monitoring::config_t hepa_config;
    cluster_t *hepa_cluster = cluster::hepa_filter_monitoring::create(endpoint, &hepa_config, CLUSTER_FLAG_SERVER);
    if (!hepa_cluster) {
        ESP_LOGE(TAG, "Failed to create HEPA filter monitoring cluster");
        return;
    }
    cluster::resource_monitoring::feature::condition::config_t hepa_condition_config;
    hepa_condition_config.condition = 100; /* fresh filter until NVS says otherwise, corrected on the first poll */
    hepa_condition_config.degradation_direction =
        chip::to_underlying(ResourceMonitoring::DegradationDirectionEnum::kDown);
    cluster::resource_monitoring::feature::condition::add(hepa_cluster, &hepa_condition_config);

    /* 3c. Activated carbon filter monitoring — same shape as HEPA above. */
    cluster::activated_carbon_filter_monitoring::config_t carbon_config;
    cluster_t *carbon_cluster =
        cluster::activated_carbon_filter_monitoring::create(endpoint, &carbon_config, CLUSTER_FLAG_SERVER);
    if (!carbon_cluster) {
        ESP_LOGE(TAG, "Failed to create activated carbon filter monitoring cluster");
        return;
    }
    cluster::resource_monitoring::feature::condition::config_t carbon_condition_config;
    carbon_condition_config.condition = 100;
    carbon_condition_config.degradation_direction =
        chip::to_underlying(ResourceMonitoring::DegradationDirectionEnum::kDown);
    cluster::resource_monitoring::feature::condition::add(carbon_cluster, &carbon_condition_config);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* Register the real FanControl Delegate — MUST happen after
     * esp_matter::start(), not before (a real, previously-wrong ordering
     * found in firmware/fan/ and fixed there at the same time as here —
     * see that file's own header/inline comment for the full explanation:
     * FanControl::SetDefaultDelegate() looks the cluster instance up in a
     * map keyed by endpoint ID and silently no-ops if it isn't
     * constructed yet, which only happens inside esp_matter::start()'s
     * own chip::Server::GetInstance().Init() call, not at endpoint/cluster
     * creation time earlier in this function). */
    FanControl::SetDefaultDelegate(air_purifier_endpoint_id, &fan_delegate);

    /* If step 1b detected 3 quick power cycles in a row, factory-reset
     * now that Matter has actually started. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    /* 5. Start the filter-life polling task — pushes an initial accurate
     * Condition/ChangeIndication reading from the loaded NVS run time,
     * then keeps both updated for as long as the device runs. */
    xTaskCreate(filter_life_task, "filter_life_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter air purifier started. Scan the QR code to commission.");
}
