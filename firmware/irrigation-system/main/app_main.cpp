/*
 * Minimal Matter Irrigation System — forty-fifth device type, and this
 * repo's first over the Irrigation System device type: a single-zone
 * sprinkler/garden-watering valve controller, combining the same generic
 * OperationalState cluster firmware/dishwasher/, firmware/laundry-washer/,
 * and firmware/laundry-dryer/ already established with firmware/
 * flow-sensor/'s own real Flow Measurement driver — two already-proven
 * patterns in this repo, combined on one endpoint for a genuinely new
 * hobby use case.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Device type + scope: a top-level helper that does almost nothing --
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * IrrigationSystem.xml (device type 0x0040, revision 1): unlike almost
 * every other device type in this repo, NONE of its four clusters are
 * mandatory — Identify, Operational State (server), Flow Measurement
 * (server), and Flow Measurement (client) are ALL individually
 * `<optionalConform/>`. This device implements Identify + OperationalState
 * + FlowMeasurement (server only) — a self-contained single-zone
 * controller with its own local flow sensor — and skips the client-side
 * FlowMeasurement (which would bind to an external, separate flow sensor
 * elsewhere in the plumbing, e.g. a whole-house meter upstream of multiple
 * zones — out of scope for a single hobby device, same "smallest
 * reasonable next step" scoping applied elsewhere in this repo).
 * `endpoint::irrigation_system::create()` DOES exist in esp-matter's
 * legacy data model (confirmed directly in `esp_matter_endpoint.cpp`) —
 * but its own `add()` does essentially nothing beyond `add_device_type()`
 * (matching the XML's own "nothing mandatory" reality): no Identify, no
 * OperationalState, no FlowMeasurement. Still worth using rather than
 * hand-assembling from scratch: its `config_t` carries a
 * `cluster::descriptor::config_t descriptor` field that the shared
 * `common::create<T>()` template (confirmed by reading it directly)
 * always uses to create the Descriptor cluster BEFORE calling `add()` —
 * so this endpoint gets a correct Descriptor cluster for free, avoiding
 * the real bug class firmware/color-light/'s and firmware/
 * addressable-light/'s own header comments document for their own
 * fully-hand-assembled endpoints. Identify + OperationalState +
 * FlowMeasurement are all added manually afterward, the same "add extra
 * clusters onto an already-correct endpoint" pattern firmware/
 * thermostat/'s BINDING output mode and firmware/air-quality-sensor/'s
 * concentration-measurement clusters already establish.
 *
 * --- OperationalState: the exact pattern this repo already knows well --
 * `cluster::operational_state::create()` is the same lower-level free
 * function firmware/dishwasher/'s/firmware/laundry-washer/'s/firmware/
 * laundry-dryer/'s own top-level device helpers call internally — used
 * directly here since `irrigation_system::add()` doesn't call it for us.
 * Confirmed by reading its own source that it does NOT register the
 * mandatory OperationCompletion event on its own (only the always-present
 * OperationalError event, via an unconditional `event::
 * create_operational_error()` call) — `dish_washer::add()`'s own source
 * shows the missing piece: a separate
 * `operational_state::event::create_operation_completion(cluster)` call
 * right after `create()`, reused here identically. The Delegate itself
 * (start/stop/pause/resume handling, `get_delegate_managed_instance()` for
 * reaching the live instance from outside the delegate's own callbacks) is
 * the same shape firmware/laundry-dryer/'s own header comment documents in
 * full, ported from connectedhomeip's own real reference
 * (`examples/dishwasher-app/dishwasher-common/src/
 * operational-state-delegate-impl.cpp`) the same way every other
 * OperationalState-based device type in this repo already does.
 *
 * Unlike firmware/dishwasher/'s and firmware/laundry-dryer/'s own
 * documented null-stub `GetCountdownTime()` (a reasonable choice for
 * their own multi-phase wash/dry cycles, whose real durations depend on
 * a hysteresis-controlled heater reaching temperature, not a plain fixed
 * timer), this file implements a REAL, live countdown — the same choice
 * firmware/microwave-oven/'s own header comment documents (there because
 * that device type's own spec makes CountdownTime mandatory; here because
 * a single watering zone's duration is a plain, fixed, known-in-advance
 * quantity with nothing else to wait on, so a real countdown costs
 * nothing extra and is straightforwardly more useful than a null stub).
 * `IRRIGATION_WATERING_DURATION_SEC` (600s = 10 minutes, a common single-
 * zone watering duration) is a fixed, adjustable `#define` — the generic
 * OperationalState Start command itself takes no parameters (confirmed
 * against the base cluster's own command definition), so there is no way
 * for a controller to request a specific duration through this cluster
 * alone, the same real limitation every other OperationalState-based
 * device type in this repo already has for its own cycle timings.
 *
 * --- FlowMeasurement: firmware/flow-sensor/'s own driver, reused verbatim
 * The same YF-S201-class pulse-output flow sensor, GPIO-ISR pulse-
 * counting technique, and L/min-to-MeasuredValue conversion firmware/
 * flow-sensor/'s own header comment documents and sources in full —
 * reused here unchanged, just parameterized through the lower-level
 * `cluster::flow_measurement::create()` free function instead of that
 * file's own complete `endpoint::flow_sensor::create()` helper (since
 * this endpoint is Irrigation System, not a standalone Flow Sensor).
 * Reports continuously regardless of valve state — for this self-
 * contained single-zone design, real flow will only ever be nonzero
 * while this device's own valve is open, so the reading already reflects
 * this zone's own watering rate with no extra correlation logic needed.
 * No fault detection (e.g. no-flow-while-running suggesting a blocked
 * line, or flow-while-stopped suggesting a stuck/leaking valve) is
 * implemented — a real, sensor-backed possibility, but left as a
 * documented "smallest reasonable next step" scope cut for a first
 * version of this device type, the same category of honest cut as
 * firmware/pump/'s own unfired fault-event set.
 *
 * Single relay (active-LOW, matching this repo's own relay convention
 * elsewhere) — one zone, not a multi-zone manifold; a real multi-zone
 * irrigation controller would need several independent valve outputs,
 * the same "smallest reasonable next step" simplification firmware/
 * refrigerator/'s own single-compressor-per-compartment design already
 * applies to a more complex real appliance. Boots closed (Stopped),
 * matching every other device type's boot-to-known-safe-state
 * convention. Standard quick-power-cycle factory reset. Build-verified
 * in Docker; not hardware-tested (no relay/YF-S201-class sensor hardware
 * for this device type physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/operational-state-server/CodegenIntegration.h>
#include <app/clusters/flow-measurement-server/FlowMeasurementCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_irrigation_system";

/* Single-zone watering valve — active-LOW relay, matching firmware/valve/'s
 * and firmware/water-heater/'s own relay convention. */
#define IRRIGATION_VALVE_RELAY_GPIO GPIO_NUM_4

/* YF-S201-class pulse-output flow sensor — same driver firmware/
 * flow-sensor/'s own header comment documents and sources in full. */
#define IRRIGATION_FLOW_SENSOR_GPIO GPIO_NUM_5
#define IRRIGATION_FLOW_PULSES_PER_HZ_PER_LPM 7.5f
#define IRRIGATION_FLOW_SAMPLE_INTERVAL_MS 2000
#define IRRIGATION_FLOW_MIN_MEASURED_VALUE 1  /* 1 L/min -> 0.1 m3/h */
#define IRRIGATION_FLOW_MAX_MEASURED_VALUE 18 /* 30 L/min -> 1.8 m3/h */

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Fixed watering duration — see the header comment above for why the
 * generic OperationalState Start command has no way to carry one from a
 * controller. Adjustable here. */
#define IRRIGATION_WATERING_DURATION_SEC 600

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t irrigation_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

static uint8_t g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
static uint32_t g_watering_seconds_remaining = IRRIGATION_WATERING_DURATION_SEC;

static void set_valve(bool open)
{
    gpio_set_level(IRRIGATION_VALVE_RELAY_GPIO, open ? 0 : 1); /* active-LOW */
}

/* --- Registry-lookup-and-cast helpers ------------------------------------
 * Same two patterns firmware/laundry-dryer/'s own header comment documents
 * in full: FlowMeasurementCluster is a DefaultServerCluster (code-driven);
 * OperationalState::Instance is delegate-managed but not registry-based,
 * hence get_delegate_managed_instance() instead. */
static FlowMeasurementCluster *get_flow_measurement_cluster(void)
{
    chip::app::ConcreteClusterPath path(irrigation_endpoint_id, FlowMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<FlowMeasurementCluster *>(iface);
}

static OperationalState::Instance *get_operational_state_instance(void)
{
    cluster_t *cl = cluster::get(irrigation_endpoint_id, OperationalState::Id);
    if (!cl) {
        return nullptr;
    }
    return static_cast<OperationalState::Instance *>(esp_matter::cluster::get_delegate_managed_instance(cl));
}

/* --- OperationalState delegate -------------------------------------------
 * Ported from connectedhomeip's own real reference
 * (examples/dishwasher-app/dishwasher-common/src/
 * operational-state-delegate-impl.cpp), same shape firmware/dishwasher/'s
 * and firmware/laundry-dryer/'s own delegates already establish — see the
 * header comment above for why this file's own GetCountdownTime()
 * deliberately reports a real value instead of the null stub those two
 * files use. */
class IrrigationOperationalStateDelegate : public OperationalState::Delegate
{
public:
    chip::app::DataModel::Nullable<uint32_t> GetCountdownTime() override
    {
        return chip::app::DataModel::Nullable<uint32_t>(g_watering_seconds_remaining);
    }

    CHIP_ERROR GetOperationalStateAtIndex(size_t index, OperationalState::GenericOperationalState &operationalState) override
    {
        if (index >= kNumStates) {
            return CHIP_ERROR_NOT_FOUND;
        }
        operationalState = OperationalState::GenericOperationalState(kStates[index]);
        return CHIP_NO_ERROR;
    }

    /* No PhaseList implemented — a single-zone watering cycle has no
     * distinct named phases, same scope cut as firmware/dishwasher/'s and
     * firmware/laundry-dryer/'s own stubs. */
    CHIP_ERROR GetOperationalPhaseAtIndex(size_t index, chip::MutableCharSpan &operationalPhase) override
    {
        (void)index;
        (void)operationalPhase;
        return CHIP_ERROR_NOT_FOUND;
    }

    void HandleStartStateCallback(OperationalState::GenericOperationalError &err) override
    {
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToStartOrResume));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kRunning);
        g_watering_seconds_remaining = IRRIGATION_WATERING_DURATION_SEC;
        set_valve(true);
        ESP_LOGI(TAG, "Watering started (%u seconds planned)", (unsigned)IRRIGATION_WATERING_DURATION_SEC);
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleStopStateCallback(OperationalState::GenericOperationalError &err) override
    {
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
        set_valve(false);
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandlePauseStateCallback(OperationalState::GenericOperationalError &err) override
    {
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kPaused));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToCompleteOperation));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kPaused);
        set_valve(false); /* deliberately does NOT touch g_watering_seconds_remaining — Resume continues from here */
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

    void HandleResumeStateCallback(OperationalState::GenericOperationalError &err) override
    {
        CHIP_ERROR result = GetInstance()->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kRunning));
        if (result != CHIP_NO_ERROR) {
            err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kUnableToStartOrResume));
            return;
        }
        g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kRunning);
        set_valve(true);
        err.Set(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
    }

private:
    static constexpr size_t kNumStates = 4;
    static constexpr uint8_t kStates[kNumStates] = {
        chip::to_underlying(OperationalState::OperationalStateEnum::kStopped),
        chip::to_underlying(OperationalState::OperationalStateEnum::kRunning),
        chip::to_underlying(OperationalState::OperationalStateEnum::kPaused),
        chip::to_underlying(OperationalState::OperationalStateEnum::kError),
    };
};
constexpr uint8_t IrrigationOperationalStateDelegate::kStates[];

static IrrigationOperationalStateDelegate operational_state_delegate;

/* Pulse counter — see firmware/flow-sensor/'s own header comment for why no
 * critical section is needed around reading/resetting it. */
static volatile uint32_t flow_pulse_count = 0;

static void IRAM_ATTR flow_pulse_isr(void *arg)
{
    flow_pulse_count++;
}

static bool flow_pulse_gpio_setup(void)
{
    gpio_config_t pulse_conf = {};
    pulse_conf.pin_bit_mask = (1ULL << IRRIGATION_FLOW_SENSOR_GPIO);
    pulse_conf.mode = GPIO_MODE_INPUT;
    pulse_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    pulse_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&pulse_conf);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
        return false;
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(IRRIGATION_FLOW_SENSOR_GPIO, flow_pulse_isr, NULL);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
        return false;
    }
    return true;
}

/* Reports flow continuously (see the header comment above for why), and —
 * while Running — decrements the watering countdown once per its own
 * sampling window, republishing CountdownTime and ending the cycle when it
 * reaches zero. One task covers both, since both only need to run on the
 * same ~2s cadence. */
static void irrigation_task(void *arg)
{
    if (!flow_pulse_gpio_setup()) {
        ESP_LOGE(TAG, "Flow sensor GPIO setup failed — flow readings will not be reported");
    }

    while (true) {
        flow_pulse_count = 0;
        vTaskDelay(pdMS_TO_TICKS(IRRIGATION_FLOW_SAMPLE_INTERVAL_MS));

        uint32_t count = flow_pulse_count;
        float window_s = IRRIGATION_FLOW_SAMPLE_INTERVAL_MS / 1000.0f;
        float frequency_hz = (float)count / window_s;
        float flow_lpm = frequency_hz / IRRIGATION_FLOW_PULSES_PER_HZ_PER_LPM;
        uint16_t measured_value = (uint16_t)(flow_lpm * 0.6f + 0.5f); /* L/min * 0.6 = MeasuredValue (0.1 m3/h units) */

        FlowMeasurementCluster *flow_cluster = get_flow_measurement_cluster();
        if (flow_cluster) {
            flow_cluster->SetMeasuredValue(chip::app::DataModel::Nullable<uint16_t>(measured_value));
        }
        ESP_LOGI(TAG, "Flow: %.2f L/min (%lu pulses, MeasuredValue=%u)", flow_lpm, (unsigned long)count, measured_value);

        if (g_operational_state != chip::to_underlying(OperationalState::OperationalStateEnum::kRunning)) {
            continue;
        }

        uint32_t elapsed_s = (uint32_t)(IRRIGATION_FLOW_SAMPLE_INTERVAL_MS / 1000);
        if (elapsed_s == 0) {
            elapsed_s = 1; /* sampling window shorter than 1s isn't used here, but guard anyway */
        }
        if (g_watering_seconds_remaining > elapsed_s) {
            g_watering_seconds_remaining -= elapsed_s;
        } else {
            g_watering_seconds_remaining = 0;
        }

        OperationalState::Instance *instance = get_operational_state_instance();
        if (instance) {
            instance->UpdateCountdownTimeFromDelegate();
        }

        if (g_watering_seconds_remaining == 0) {
            ESP_LOGI(TAG, "Watering complete");
            set_valve(false);
            if (instance) {
                instance->OnOperationCompletionDetected(chip::to_underlying(OperationalState::ErrorStateEnum::kNoError));
                instance->SetOperationalState(chip::to_underlying(OperationalState::OperationalStateEnum::kStopped));
            }
            g_operational_state = chip::to_underlying(OperationalState::OperationalStateEnum::kStopped);
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

/* No controller-writable attributes on this device beyond what the
 * OperationalState/FlowMeasurement clusters already handle internally. */
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

    /* 2. Configure the valve relay output — boot closed (de-energized). */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << IRRIGATION_VALVE_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    set_valve(false);

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

    /* 3. Build the Matter data model: one node, one Irrigation System
     * endpoint (Descriptor via the top-level helper's own `common::
     * create<T>()` path, plus Identify + OperationalState +
     * FlowMeasurement all added manually — see the header comment above
     * for why none of the three come from `irrigation_system::add()`
     * itself). */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Irrigation System",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::irrigation_system::config_t irrigation_config;
    endpoint_t *endpoint = endpoint::irrigation_system::create(node, &irrigation_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create irrigation system endpoint");
        return;
    }
    irrigation_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Irrigation system endpoint id: %u", irrigation_endpoint_id);

    /* 3a. Identify — optionalConform, so irrigation_system::add() doesn't
     * create it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3b. OperationalState — see the header comment above for the
     * OperationCompletion-event gap this lower-level create() has (unlike
     * dish_washer::add()'s own equivalent call, which also registers it). */
    cluster::operational_state::config_t operational_state_config;
    operational_state_config.delegate = &operational_state_delegate;
    cluster_t *operational_state_cluster = cluster::operational_state::create(endpoint, &operational_state_config, CLUSTER_FLAG_SERVER);
    if (!operational_state_cluster) {
        ESP_LOGE(TAG, "Failed to create operational state cluster");
        return;
    }
    cluster::operational_state::event::create_operation_completion(operational_state_cluster);

    /* 3c. FlowMeasurement — same driver/config firmware/flow-sensor/'s own
     * header comment documents and sources in full. */
    cluster::flow_measurement::config_t flow_config;
    flow_config.min_measured_value = nullable<uint16_t>((uint16_t)IRRIGATION_FLOW_MIN_MEASURED_VALUE);
    flow_config.max_measured_value = nullable<uint16_t>((uint16_t)IRRIGATION_FLOW_MAX_MEASURED_VALUE);
    cluster::flow_measurement::create(endpoint, &flow_config, CLUSTER_FLAG_SERVER);

    xTaskCreate(irrigation_task, "irrigation_task", 4096, NULL, 5, NULL);

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

    ESP_LOGI(TAG, "Matter irrigation system started. Scan the QR code to commission.");
}
