/*
 * Minimal Matter Extractor Hood — twenty-third device type, and the
 * closest sibling to firmware/air-purifier/ in this repo: the same
 * FanControl Delegate + HepaFilterMonitoring/ActivatedCarbonFilterMonitoring
 * integration, reused almost verbatim, but for a kitchen range hood
 * instead of a room air purifier.
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
 * --- Endpoint: esp-matter's own complete top-level helper, plus Identify
 * and two filter clusters added onto the SAME already-correct endpoint ----
 * `endpoint::extractor_hood::create()` (device type 0x007A) confirmed
 * complete/ready-to-use by reading esp_matter_endpoint.cpp's own
 * `extractor_hood::add()` directly: FanControl only, via `common::
 * create<T>()` (auto-Descriptor). Matches the CSA's own
 * data_model/1.6/device_types/ExtractorHood.xml exactly: FanControl is the
 * ONLY `<mandatoryConform/>` cluster — Identify, HEPA Filter Monitoring,
 * and Activated Carbon Filter Monitoring are all `<optionalConform/>`.
 * This is the first device type in this repo where Identify itself is
 * optional per spec rather than mandatory (confirmed directly in the XML,
 * not assumed) — `extractor_hood::add()` correspondingly does NOT call
 * `identify::create()` at all, unlike every other top-level helper this
 * repo has used so far. Added here anyway, via `cluster::identify::
 * create()` on the endpoint afterwards, same "add an extra cluster onto
 * an already-correct endpoint" pattern firmware/air-purifier/'s own filter
 * clusters already use — every other device type in this repo ships an
 * Identify LED, and nothing in the spec disallows one here, so there's no
 * reason to be the first exception. The XML also explicitly
 * `<disallowConform/>`s three FanControl features on this device type —
 * Rocking (RCK), Wind (WND), and AirflowDirection (DIR) — confirmed by
 * reading the XML directly; zero code impact here, since none of the fan-
 * related device types in this repo (firmware/fan/, firmware/
 * air-purifier/) implement any of those three anyway, but worth recording
 * as a real, checked spec constraint rather than a coincidence.
 *
 * --- FanControl: reused near-verbatim from firmware/air-purifier/ --------
 * Same Delegate-based integration (`FanControl::Delegate` subclass,
 * `SetDefaultDelegate()` called after `esp_matter::start()`, registry-
 * lookup-and-cast for `SetPercentCurrent()`) — see firmware/fan/main/
 * app_main.cpp's own header comment for the full detail on why, including
 * the two real, sequential Docker build failures that established this
 * pattern in the first place, and firmware/fan/'s/firmware/air-purifier/'s
 * own history of the `SetDefaultDelegate()`-before-`start()` ordering bug
 * this file avoids from the start. Same PercentSetting/PercentCurrent-only
 * scope, same OffLowMedHigh FanModeSequence, same 25kHz LEDC PWM output —
 * a range hood's own extraction fan is physically the same kind of PWM-
 * driven DC/EC fan motor as an air purifier's or a plain fan's.
 *
 * --- Filter monitoring: HEPA Filter Monitoring standing in for the hood's
 * real grease filter, since Matter has no dedicated grease-filter cluster
 * Confirmed by reading the device type XML directly: it reuses the exact
 * same two generic filter clusters (0x0071/0x0072) firmware/air-purifier/
 * already uses — Matter's data model has no cluster specific to a range
 * hood's actual removable grease filter (typically a washable metal mesh
 * or baffle filter, not literally HEPA media), so "HEPA Filter Monitoring"
 * is repurposed here to represent that grease filter, same way a real
 * commercial Matter hood implementation would have to. Activated Carbon
 * Filter Monitoring represents the hood's own carbon/charcoal odor filter
 * — only physically present on recirculating ("ductless") hoods, not on
 * hoods ducted straight outside, but both clusters are added
 * unconditionally here anyway (same "smallest reasonable next step,
 * simplest correct default" scoping as firmware/air-purifier/'s own
 * always-both-filters choice) — on a ducted installation, the Carbon
 * filter's reported Condition/ChangeIndication simply won't correspond to
 * a real physical filter; harmless to leave visible, and simpler than
 * adding a build-time toggle for a difference this firmware doesn't
 * otherwise need to know about. Same real, documented esp-matter feature
 * API as firmware/air-purifier/ — `cluster::resource_monitoring::
 * feature::condition::add()` — and same `ResourceMonitoring::
 * GetClusterInstance()` convenience free function for updating Condition/
 * ChangeIndication at runtime; see that file's own header comment for the
 * full detail on both, including why the feature has to be added before
 * `esp_matter::start()` (a constructor-time `BitFlags<Feature>` snapshot,
 * the same pattern this repo has now hit for AirQuality, BooleanState,
 * and ResourceMonitoring specifically).
 *
 * --- Filter life: a time-based estimate, adjusted for hood-specific
 * filters, not a real sensor reading -------------------------------------
 * Same technique as firmware/air-purifier/ (elapsed fan-running seconds
 * accumulated in NVS while the fan is on, persisted periodically to avoid
 * flash wear, Condition computed against each filter's own configurable
 * rated life) — no differential-pressure or grease-buildup sensor
 * assumed, same "smallest reasonable next step" reasoning. Default life
 * figures are adjusted for what a range hood's own filters actually are,
 * not air purifier media: EXTRACTOR_HOOD_GREASE_FILTER_LIFE_HOURS (100h —
 * a metal mesh/baffle grease filter is commonly recommended for cleaning
 * roughly monthly under regular cooking use, which this rough hours-based
 * proxy approximates) and EXTRACTOR_HOOD_CARBON_FILTER_LIFE_HOURS (200h —
 * activated-carbon odor filters on recirculating hoods are commonly
 * recommended for replacement roughly every 3-6 months, longer between
 * changes than a grease filter needs cleaning). Both are plain adjustable
 * #defines, not calibrated measurements or spec-defined values — same
 * "adjustable threshold, not a calibrated reading" precedent firmware/
 * smoke-co-alarm/'s MQ classifier and firmware/air-purifier/'s own filter
 * life figures already establish. ChangeIndication is derived from
 * Condition with the same >20% Ok / 5-20% Warning / <5% Critical mapping
 * firmware/air-purifier/ uses — again a plain adjustable mapping, not a
 * spec-defined one.
 *
 * Reference wiring: a PWM-capable MOSFET or fan-speed-controller board
 * driving the hood's own extraction fan motor, same as firmware/fan/'s
 * and firmware/air-purifier/'s own reference wiring.
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

static const char *TAG = "matter_extractor_hood";

/* Change this to the GPIO your extraction fan's PWM/speed-control input is
 * wired to — same reasoning/wiring as firmware/fan/'s own FAN_PWM_GPIO. */
#define HOOD_FAN_PWM_GPIO GPIO_NUM_2

/* Separate LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (LED Control / PWM) peripheral setup — identical to firmware/fan/'s
 * own settings, see that file's header comment for the full reasoning. */
#define HOOD_LEDC_TIMER LEDC_TIMER_0
#define HOOD_LEDC_CHANNEL LEDC_CHANNEL_0
#define HOOD_LEDC_MODE LEDC_LOW_SPEED_MODE
#define HOOD_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define HOOD_LEDC_FREQUENCY_HZ 25000

/* Filter life — see the header comment above: a plain time-based estimate,
 * not a real sensor reading, using hood-appropriate default life figures.
 * Both adjustable per your actual filters' rated life. */
#define EXTRACTOR_HOOD_GREASE_FILTER_LIFE_HOURS 100
#define EXTRACTOR_HOOD_CARBON_FILTER_LIFE_HOURS 200
#define EXTRACTOR_HOOD_CHANGE_WARNING_PERCENT 20
#define EXTRACTOR_HOOD_CHANGE_CRITICAL_PERCENT 5

/* How often accumulated operating time is written to NVS while the fan is
 * running — a plain seconds counter, not on every tick, to avoid flash
 * wear (same reasoning firmware/dimmable-light/'s CurrentLevel deferred
 * persistence already documents for a different attribute). Condition is
 * still recomputed and pushed to the cluster every poll regardless — only
 * the NVS write itself is throttled. */
#define EXTRACTOR_HOOD_POLL_INTERVAL_MS 5000
#define EXTRACTOR_HOOD_NVS_SAVE_INTERVAL_MS 60000
#define EXTRACTOR_HOOD_NVS_NAMESPACE "filter_life"
#define EXTRACTOR_HOOD_NVS_KEY "run_seconds"

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t extractor_hood_endpoint_id = 0;
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
    ledc_set_duty(HOOD_LEDC_MODE, HOOD_LEDC_CHANNEL, duty);
    ledc_update_duty(HOOD_LEDC_MODE, HOOD_LEDC_CHANNEL);
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

        chip::app::ConcreteClusterPath path(extractor_hood_endpoint_id, FanControl::Id);
        chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
        if (iface) {
            static_cast<FanControlCluster *>(iface)->SetPercentCurrent(percent);
        } else {
            ESP_LOGE(TAG, "FanControl cluster not found on endpoint %u", extractor_hood_endpoint_id);
        }
        ESP_LOGI(TAG, "Fan mode %u, percent set to %u%%", chip::to_underlying(newState.mode), percent);
    }
};

static FanDelegate fan_delegate;

/* Computes Condition (0-100, percent life remaining) from accumulated
 * run time against a filter's own rated life in hours. Clamped at 0 —
 * a filter overdue for cleaning/replacement stays at 0%, it doesn't go
 * negative. */
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
    if (condition_percent <= EXTRACTOR_HOOD_CHANGE_CRITICAL_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kCritical;
    }
    if (condition_percent <= EXTRACTOR_HOOD_CHANGE_WARNING_PERCENT) {
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

    auto *cluster = ResourceMonitoring::GetClusterInstance(extractor_hood_endpoint_id, cluster_id);
    if (!cluster) {
        ESP_LOGE(TAG, "%s filter cluster not found on endpoint %u", label, extractor_hood_endpoint_id);
        return;
    }
    cluster->UpdateCondition(condition);
    cluster->UpdateChangeIndication(indication);
    ESP_LOGI(TAG, "%s filter: %u%% remaining (%s)", label, condition,
             indication == ResourceMonitoring::ChangeIndicationEnum::kCritical ? "CRITICAL" :
             indication == ResourceMonitoring::ChangeIndicationEnum::kWarning ? "WARNING" : "OK");
}

/* Polls every EXTRACTOR_HOOD_POLL_INTERVAL_MS: accumulates run time while
 * the fan is actually on, periodically persists it to NVS, and refreshes
 * both filter clusters' Condition/ChangeIndication every poll regardless. */
static void filter_life_task(void *arg)
{
    uint32_t ms_since_save = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(EXTRACTOR_HOOD_POLL_INTERVAL_MS));

        if (current_percent_setting > 0) {
            total_run_seconds += EXTRACTOR_HOOD_POLL_INTERVAL_MS / 1000;
            ms_since_save += EXTRACTOR_HOOD_POLL_INTERVAL_MS;

            if (ms_since_save >= EXTRACTOR_HOOD_NVS_SAVE_INTERVAL_MS) {
                nvs_handle_t nvs;
                if (nvs_open(EXTRACTOR_HOOD_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_set_u32(nvs, EXTRACTOR_HOOD_NVS_KEY, total_run_seconds);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                }
                ms_since_save = 0;
            }
        }

        update_filter_cluster(HepaFilterMonitoring::Id, EXTRACTOR_HOOD_GREASE_FILTER_LIFE_HOURS, "Grease");
        update_filter_cluster(ActivatedCarbonFilterMonitoring::Id, EXTRACTOR_HOOD_CARBON_FILTER_LIFE_HOURS, "Carbon");
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
 * node::create()'s callback signature, same as firmware/fan/'s and
 * firmware/air-purifier/'s own. */
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
 * separate NVS namespace from EXTRACTOR_HOOD_NVS_NAMESPACE above — factory
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
    if (nvs_open(EXTRACTOR_HOOD_NVS_NAMESPACE, NVS_READWRITE, &filter_nvs) == ESP_OK) {
        nvs_get_u32(filter_nvs, EXTRACTOR_HOOD_NVS_KEY, &total_run_seconds);
        nvs_close(filter_nvs);
    }
    ESP_LOGI(TAG, "Loaded %lu accumulated filter run seconds from NVS", (unsigned long)total_run_seconds);

    /* 2. Configure the extraction fan PWM output via LEDC — same as
     * firmware/fan/. */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = HOOD_LEDC_MODE;
    ledc_timer.duty_resolution = HOOD_LEDC_DUTY_RES;
    ledc_timer.timer_num = HOOD_LEDC_TIMER;
    ledc_timer.freq_hz = HOOD_LEDC_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = HOOD_FAN_PWM_GPIO;
    ledc_channel.speed_mode = HOOD_LEDC_MODE;
    ledc_channel.channel = HOOD_LEDC_CHANNEL;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = HOOD_LEDC_TIMER;
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

    /* 3. Build the Matter data model: one node, one Extractor Hood endpoint
     * (FanControl via the complete top-level helper — Identify is NOT
     * wired in automatically here, see the header comment above), plus
     * Identify and the two filter-monitoring clusters added onto that
     * same endpoint afterwards. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Extractor Hood",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::extractor_hood::config_t hood_config;
    hood_config.fan_control.fan_mode_sequence =
        chip::to_underlying(FanControl::FanModeSequenceEnum::kOffLowMedHigh);
    endpoint_t *endpoint = endpoint::extractor_hood::create(node, &hood_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create extractor hood endpoint");
        return;
    }

    extractor_hood_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Extractor hood endpoint id: %u", extractor_hood_endpoint_id);

    /* 3b. Identify — optionalConform for this device type (unlike every
     * other device type in this repo so far), so extractor_hood::add()
     * doesn't create it automatically; added here the same way the two
     * filter clusters below are, onto the already-correct endpoint. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(chip::app::Clusters::Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3c. "HEPA" filter monitoring, standing in for the hood's real grease
     * filter (see the header comment above for why), with the Condition
     * feature (percent remaining + DegradationDirection=Down) enabled via
     * esp-matter's own public feature::condition::add() API. */
    cluster::hepa_filter_monitoring::config_t grease_config;
    cluster_t *grease_cluster = cluster::hepa_filter_monitoring::create(endpoint, &grease_config, CLUSTER_FLAG_SERVER);
    if (!grease_cluster) {
        ESP_LOGE(TAG, "Failed to create grease filter monitoring cluster");
        return;
    }
    cluster::resource_monitoring::feature::condition::config_t grease_condition_config;
    grease_condition_config.condition = 100; /* fresh filter until NVS says otherwise, corrected on the first poll */
    grease_condition_config.degradation_direction =
        chip::to_underlying(ResourceMonitoring::DegradationDirectionEnum::kDown);
    cluster::resource_monitoring::feature::condition::add(grease_cluster, &grease_condition_config);

    /* 3d. Activated carbon filter monitoring — the hood's own odor filter
     * (only physically meaningful on a recirculating/ductless install —
     * see the header comment above); same shape as the grease filter. */
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
     * esp_matter::start(), not before — see firmware/fan/'s and
     * firmware/air-purifier/'s own header/inline comments for the full
     * explanation of this ordering requirement. */
    FanControl::SetDefaultDelegate(extractor_hood_endpoint_id, &fan_delegate);

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

    ESP_LOGI(TAG, "Matter extractor hood started. Scan the QR code to commission.");
}
