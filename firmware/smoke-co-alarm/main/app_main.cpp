/*
 * Minimal Matter Smoke/CO Alarm — a fourteenth device type, and this
 * repo's first over the SmokeCoAlarm cluster (life-safety alarm class,
 * not a plain sensor readout or actuator).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain,
 * readable C++ — there is no hidden framework layer and no telemetry.
 * Matter is local-first: commissioning happens over Bluetooth + your LAN,
 * and control runs over your local network. Nothing leaves your home
 * unless you choose to add a cloud hub (Google/Apple/Alexa). With Home
 * Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev
 * setup. Works on other ESP32 chips too (C3, C6, S3, H2) — see the README
 * for how to switch target.
 *
 * --- Matter cluster composition ------------------------------------
 * endpoint::smoke_co_alarm::create() (Identify + SmokeCoAlarm cluster) is
 * a complete, directly usable esp-matter helper — confirmed by reading
 * esp_matter_endpoint.cpp's own smoke_co_alarm::add() directly. Its
 * Identify defaults to IdentifyTypeEnum::kAudibleBeep (same reasoning as
 * firmware/door-lock/: a smoke/CO alarm is often ceiling-mounted, out of
 * easy line of sight, so a beep is a more realistic physical Identify
 * signal for this device class) — but this firmware still blinks a
 * visible LED for Identify like every other device type here, since
 * IdentifyType is purely informational, not something that constrains
 * what this app actually does.
 *
 * Unlike firmware/door-lock/'s DoorLock cluster, SmokeCoAlarm IS a
 * "code-driven" cluster class in this SDK version — confirmed by the
 * presence of a smoke_co_alarm/ folder under
 * data_model_provider/clusters/ (the same signal firmware/contact-sensor/'s
 * BooleanState and firmware/light-sensor/'s IlluminanceMeasurement already
 * used). So SmokeState/COState/TestInProgress/HardwareFaultAlert/etc. are
 * all written through SmokeCoAlarmCluster's own setter API, looked up via
 * the data model provider's registry — the exact same pattern
 * update_contact_state()/update_illuminance() already established
 * elsewhere in this repo, confirmed by reading
 * SmokeCoAlarmCluster.h/.cpp directly rather than guessing.
 *
 * SmokeCoAlarmCluster's setters already generate the right Matter events
 * internally (SmokeAlarm/COAlarm events fire from SetSmokeState()/
 * SetCOState() themselves when transitioning to Warning/Critical; AllClear
 * fires from SetExpressedState() when transitioning back to Normal) — no
 * manual event-generation code needed here, unlike a plain ember
 * attribute write.
 *
 * SetExpressedStateByPriority() computes ExpressedState (the cluster's
 * single "what's the headline state right now" attribute) from whichever
 * of the 9 possible sub-states is highest priority and currently active —
 * called after every state change below with a fixed priority order
 * (life-safety alarms first, then self-test, then secondary conditions).
 * Interconnect/battery/end-of-service/inoperative states are left at their
 * defaults (this device has no interconnect wiring to other alarms, no
 * battery to monitor when USB/PSU-powered, and no service-life tracking
 * for a hobby MQ-series sensor) — same "smallest reasonable next step"
 * scoping already used throughout this repo (e.g. firmware/door-lock/'s
 * skipped PIN/credential/schedule features).
 *
 * --- The SelfTestRequest command needs one thing this app DOES have to
 * do itself ---------------------------------------------------------
 * esp-matter's door_lock::config_t has an optional Delegate; SmokeCoAlarm
 * has one too (SmokeCoAlarmDelegate::OnSelfTestRequested()), left null
 * here for the same reason. Confirmed by reading
 * SmokeCoAlarmCluster::HandleRemoteSelfTestRequest() directly: without a
 * Delegate, a real controller's SelfTestRequest command still succeeds —
 * the cluster sets TestInProgress=true and ExpressedState=Testing
 * entirely on its own, no Delegate needed for that part. But nothing
 * *clears* TestInProgress afterwards unless the app does it — a real gap
 * worth remembering for any future *Request-style command cluster in this
 * repo: the SDK can set a flag on command receipt without ever owning the
 * job of clearing it again. sensor_task() below polls
 * GetTestInProgress() each cycle and, once a self-test has been running
 * for SMOKE_CO_ALARM_SELF_TEST_DURATION_MS, calls SetTestInProgress(false)
 * and recomputes ExpressedState — simulating a completed self-test since
 * there's no real self-test routine to run against a plain analog sensor.
 *
 * --- SENSOR_TYPE: which MQ-series gas sensor(s) are actually wired up --
 * SENSOR_MQ2_MQ7 (default) — both an MQ-2 (smoke/combustible-gas) and an
 *   MQ-7 (carbon monoxide) sensor, matching how real combination smoke+CO
 *   alarms are sold as one product. Enables both the SmokeAlarm and
 *   COAlarm cluster features.
 * SENSOR_MQ2 — MQ-2 only. SmokeAlarm feature only.
 * SENSOR_MQ7 — MQ-7 only. COAlarm feature only.
 *
 * Both are the classic cheap analog gas-sensor modules (a heated tin-
 * dioxide element whose resistance drops as gas concentration rises),
 * almost always sold on a breakout board with an onboard voltage divider:
 * Vcc -> sensor -> AOUT tap -> fixed load resistor -> GND, so AOUT voltage
 * *rises* with gas concentration on most common modules — always check
 * your specific module, some comparator-equipped boards invert this.
 * Deliberately NOT converted to a calibrated ppm figure the way
 * firmware/light-sensor/'s LDR is converted to lux: MQ-series datasheets
 * document ppm only as a family of curves that shift with each sensor's
 * own load resistance, heater voltage, and burn-in state, and Matter's
 * SmokeCoAlarm cluster doesn't have a numeric concentration attribute
 * anyway (only the AlarmStateEnum Normal/Warning/Critical tri-state) — so
 * this is a plain adjustable-millivolt-threshold classifier
 * (SMOKE_CO_ALARM_MQ2_WARNING_MV / _CRITICAL_MV, and the MQ7 equivalents),
 * meant to be tuned against your own module and environment, not a
 * calibrated absolute reading.
 *
 * MQ-series sensors need their heater element to stabilize before
 * readings mean anything — datasheets call for a real 24-48h burn-in for
 * full accuracy, far beyond what this firmware can enforce.
 * SMOKE_CO_ALARM_WARMUP_MS (default 60s) only suppresses the initial
 * power-on resistance-settling transient from causing an immediate false
 * alarm — it is NOT a substitute for real sensor burn-in, documented here
 * so nobody mistakes the two.
 *
 * GPIO 34/35 (ADC1 channels 6/7 on classic ESP32/WROOM-32) are the
 * defaults for MQ2/MQ7 respectively — deliberately ADC1, not ADC2, same
 * reasoning as firmware/light-sensor/'s LDR: ADC2 shares hardware with the
 * Wi-Fi radio and becomes unreliable once Wi-Fi is active, which this
 * device needs to be commissioned/reachable at all times.
 *
 * HardwareFaultAlert is set from a simple heuristic: several consecutive
 * readings pinned at the ADC's extreme raw values (near 0 or near
 * full-scale) most likely mean a disconnected or shorted sensor,
 * regardless of a specific module's exact wiring polarity — a
 * deliberately conservative check rather than one tuned to any single
 * module's normal-vs-fault voltage split.
 */

#include <array>
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/smoke-co-alarm-server/SmokeCoAlarmCluster.h>

static const char *TAG = "matter_smoke_co_alarm";

/* --- Sensor selection — change this one line to match your hardware --- */
#define SENSOR_MQ2_MQ7 1
#define SENSOR_MQ2 2
#define SENSOR_MQ7 3

#define SENSOR_TYPE SENSOR_MQ2_MQ7

/* ADC-capable input pins — see the header comment above for why these
 * must stay ADC1, and why moving off these GPIOs also means updating the
 * ADC unit/channel constants below by hand (ESP32's ADC channel-to-pin
 * mapping is fixed in hardware, not something software can remap). MQ7
 * pin/channel are unused/compiled out when SENSOR_TYPE == SENSOR_MQ2, and
 * vice versa. */
#define SMOKE_CO_ALARM_MQ2_GPIO GPIO_NUM_34
#define SMOKE_CO_ALARM_MQ7_GPIO GPIO_NUM_35

#define SMOKE_CO_ALARM_MQ2_ADC_UNIT ADC_UNIT_1
#define SMOKE_CO_ALARM_MQ2_ADC_CHANNEL ADC_CHANNEL_6 /* GPIO 34 */
#define SMOKE_CO_ALARM_MQ7_ADC_UNIT ADC_UNIT_1
#define SMOKE_CO_ALARM_MQ7_ADC_CHANNEL ADC_CHANNEL_7 /* GPIO 35 */
#define SMOKE_CO_ALARM_ADC_ATTEN ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define SMOKE_CO_ALARM_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define SMOKE_CO_ALARM_ADC_SUPPLY_MV 3300.0f

/* Adjustable raw-voltage alarm thresholds — see the header comment above
 * for why these are NOT calibrated ppm values. Tune against your own
 * module + environment (e.g. hold a smoke source near the MQ-2, or a
 * disconnected butane lighter's unlit gas near the MQ-7, and read the
 * logged millivolt value to pick sane thresholds for your setup). */
#define SMOKE_CO_ALARM_MQ2_WARNING_MV 1800
#define SMOKE_CO_ALARM_MQ2_CRITICAL_MV 2400
#define SMOKE_CO_ALARM_MQ7_WARNING_MV 1800
#define SMOKE_CO_ALARM_MQ7_CRITICAL_MV 2400

/* Averaging + reporting interval. */
#define SMOKE_CO_ALARM_SAMPLE_COUNT 16
#define SMOKE_CO_ALARM_MEASURE_INTERVAL_MS 5000

/* See the header comment above — NOT a substitute for real MQ-sensor
 * burn-in, only suppresses the initial power-on transient. */
#define SMOKE_CO_ALARM_WARMUP_MS 60000

/* How long a simulated self-test "runs" before this app clears
 * TestInProgress and restores ExpressedState — see the header comment
 * above for why the app has to do this at all. */
#define SMOKE_CO_ALARM_SELF_TEST_DURATION_MS 5000

/* Consecutive extreme-raw-reading samples before HardwareFaultAlert is
 * raised — see the header comment above. */
#define SMOKE_CO_ALARM_FAULT_STREAK_THRESHOLD 5
#define SMOKE_CO_ALARM_FAULT_RAW_LOW 20
#define SMOKE_CO_ALARM_FAULT_RAW_HIGH 4075 /* out of a 12-bit 0-4095 range */

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

static uint16_t smoke_co_alarm_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Fixed ExpressedState priority order — see the header comment above for
 * why this list is exactly the 9 entries SetExpressedStateByPriority()
 * expects, and why life-safety alarms (Smoke/CO) sit above everything
 * else. Interconnect states are included (as required by the array's
 * fixed length) even though this device never sets them — they'll just
 * never be "active" and fall through to the next entry. */
static const std::array<SmokeCoAlarm::ExpressedStateEnum, 9> k_expressed_state_priority = {
    SmokeCoAlarm::ExpressedStateEnum::kSmokeAlarm,
    SmokeCoAlarm::ExpressedStateEnum::kCOAlarm,
    SmokeCoAlarm::ExpressedStateEnum::kInterconnectSmoke,
    SmokeCoAlarm::ExpressedStateEnum::kInterconnectCO,
    SmokeCoAlarm::ExpressedStateEnum::kTesting,
    SmokeCoAlarm::ExpressedStateEnum::kBatteryAlert,
    SmokeCoAlarm::ExpressedStateEnum::kHardwareFault,
    SmokeCoAlarm::ExpressedStateEnum::kEndOfService,
    SmokeCoAlarm::ExpressedStateEnum::kInoperative,
};

/* esp-matter's generic attribute::update() can't write this cluster's
 * attributes — see the header comment above for why. Follows the exact
 * pattern firmware/contact-sensor/main/app_main.cpp's update_contact_state()
 * established: look the cluster instance up directly via the data model
 * provider's registry, then call its cluster-specific setters. Returns
 * the cluster pointer (or NULL) so callers can chain multiple setter
 * calls without repeating the lookup. */
static chip::app::Clusters::SmokeCoAlarmCluster *get_smoke_co_alarm_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, SmokeCoAlarm::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "SmokeCoAlarm cluster not found on endpoint %u", endpoint_id);
        return NULL;
    }
    return static_cast<chip::app::Clusters::SmokeCoAlarmCluster *>(iface);
}

/* ======================================================================
 * ADC driver — shared by both MQ2 and MQ7 (they're the same kind of
 * analog voltage-divider sensor, just on different channels/thresholds).
 * Only the channel(s) SENSOR_TYPE actually selects are configured.
 * ====================================================================== */

static adc_oneshot_unit_handle_t adc_handle = NULL;

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
static adc_cali_handle_t mq2_cali_handle = NULL;
static bool mq2_cali_available = false;
static uint8_t mq2_fault_streak = 0;
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
static adc_cali_handle_t mq7_cali_handle = NULL;
static bool mq7_cali_available = false;
static uint8_t mq7_fault_streak = 0;
#endif

/* Sets up ADC calibration for one channel — same portable curve-fitting/
 * line-fitting #if pattern as firmware/light-sensor/'s LDR driver, since
 * classic ESP32 only supports line fitting. Calibration failing isn't
 * fatal: read_channel_millivolts() below falls back to an uncalibrated
 * estimate. */
static bool setup_channel_calibration(adc_channel_t channel, adc_cali_handle_t *out_handle)
{
    esp_err_t err;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.chan = channel;
    cali_config.atten = SMOKE_CO_ALARM_ADC_ATTEN;
    cali_config.bitwidth = SMOKE_CO_ALARM_ADC_BITWIDTH;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, out_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = SMOKE_CO_ALARM_ADC_ATTEN;
    cali_config.bitwidth = SMOKE_CO_ALARM_ADC_BITWIDTH;
#if CONFIG_IDF_TARGET_ESP32
    cali_config.default_vref = (uint32_t)SMOKE_CO_ALARM_ADC_SUPPLY_MV;
#endif
    err = adc_cali_create_scheme_line_fitting(&cali_config, out_handle);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable for channel %d (%s) — using uncalibrated estimate",
                 channel, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool sensor_setup(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = SMOKE_CO_ALARM_ADC_ATTEN;
    chan_config.bitwidth = SMOKE_CO_ALARM_ADC_BITWIDTH;

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
    err = adc_oneshot_config_channel(adc_handle, SMOKE_CO_ALARM_MQ2_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQ2 adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }
    mq2_cali_available = setup_channel_calibration(SMOKE_CO_ALARM_MQ2_ADC_CHANNEL, &mq2_cali_handle);
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
    err = adc_oneshot_config_channel(adc_handle, SMOKE_CO_ALARM_MQ7_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQ7 adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }
    mq7_cali_available = setup_channel_calibration(SMOKE_CO_ALARM_MQ7_ADC_CHANNEL, &mq7_cali_handle);
#endif
    return true;
}

/* Reads and averages SMOKE_CO_ALARM_SAMPLE_COUNT raw samples from one
 * channel, converts to millivolts, and reports whether the raw reading
 * looked "stuck" at an extreme value (see the header comment above on
 * HardwareFaultAlert). */
static bool read_channel_millivolts(adc_channel_t channel, adc_cali_handle_t cali_handle, bool cali_available,
                                     int *out_mv, bool *out_extreme)
{
    int64_t sum_raw = 0;
    int last_raw = 0;
    for (int i = 0; i < SMOKE_CO_ALARM_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(adc_handle, channel, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read (channel %d) failed: %s", channel, esp_err_to_name(err));
            return false;
        }
        sum_raw += raw;
        last_raw = raw;
    }
    int avg_raw = (int)(sum_raw / SMOKE_CO_ALARM_SAMPLE_COUNT);
    *out_extreme = (last_raw <= SMOKE_CO_ALARM_FAULT_RAW_LOW) || (last_raw >= SMOKE_CO_ALARM_FAULT_RAW_HIGH);

    if (cali_available) {
        esp_err_t err = adc_cali_raw_to_voltage(cali_handle, avg_raw, out_mv);
        if (err == ESP_OK) {
            return true;
        }
        ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s — falling back to uncalibrated estimate",
                 esp_err_to_name(err));
    }
    *out_mv = (int)((float)avg_raw * SMOKE_CO_ALARM_ADC_SUPPLY_MV / 4095.0f);
    return true;
}

/* Millivolts -> AlarmStateEnum via the plain adjustable thresholds above. */
static SmokeCoAlarm::AlarmStateEnum classify(int mv, int warning_mv, int critical_mv)
{
    if (mv >= critical_mv) {
        return SmokeCoAlarm::AlarmStateEnum::kCritical;
    }
    if (mv >= warning_mv) {
        return SmokeCoAlarm::AlarmStateEnum::kWarning;
    }
    return SmokeCoAlarm::AlarmStateEnum::kNormal;
}

static void sensor_task(void *arg)
{
    int64_t start_us = esp_timer_get_time();
    uint32_t self_test_remaining_ms = 0;
    bool hardware_fault_active = false;

    for (;;) {
        bool warmed_up = (esp_timer_get_time() - start_us) >= ((int64_t)SMOKE_CO_ALARM_WARMUP_MS * 1000);
        bool any_fault = false;

#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
        {
            int mv = 0;
            bool extreme = false;
            if (read_channel_millivolts(SMOKE_CO_ALARM_MQ2_ADC_CHANNEL, mq2_cali_handle, mq2_cali_available, &mv, &extreme)) {
                mq2_fault_streak = extreme ? (mq2_fault_streak + 1) : 0;
                SmokeCoAlarm::AlarmStateEnum state = warmed_up
                    ? classify(mv, SMOKE_CO_ALARM_MQ2_WARNING_MV, SMOKE_CO_ALARM_MQ2_CRITICAL_MV)
                    : SmokeCoAlarm::AlarmStateEnum::kNormal;
                ESP_LOGI(TAG, "MQ2 (smoke): %d mV -> %s%s", mv,
                         state == SmokeCoAlarm::AlarmStateEnum::kNormal ? "Normal" :
                         state == SmokeCoAlarm::AlarmStateEnum::kWarning ? "Warning" : "Critical",
                         warmed_up ? "" : " (warming up)");
                if (auto *cluster = get_smoke_co_alarm_cluster(smoke_co_alarm_endpoint_id)) {
                    cluster->SetSmokeState(state);
                }
            }
            if (mq2_fault_streak >= SMOKE_CO_ALARM_FAULT_STREAK_THRESHOLD) {
                any_fault = true;
            }
        }
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
        {
            int mv = 0;
            bool extreme = false;
            if (read_channel_millivolts(SMOKE_CO_ALARM_MQ7_ADC_CHANNEL, mq7_cali_handle, mq7_cali_available, &mv, &extreme)) {
                mq7_fault_streak = extreme ? (mq7_fault_streak + 1) : 0;
                SmokeCoAlarm::AlarmStateEnum state = warmed_up
                    ? classify(mv, SMOKE_CO_ALARM_MQ7_WARNING_MV, SMOKE_CO_ALARM_MQ7_CRITICAL_MV)
                    : SmokeCoAlarm::AlarmStateEnum::kNormal;
                ESP_LOGI(TAG, "MQ7 (CO): %d mV -> %s%s", mv,
                         state == SmokeCoAlarm::AlarmStateEnum::kNormal ? "Normal" :
                         state == SmokeCoAlarm::AlarmStateEnum::kWarning ? "Warning" : "Critical",
                         warmed_up ? "" : " (warming up)");
                if (auto *cluster = get_smoke_co_alarm_cluster(smoke_co_alarm_endpoint_id)) {
                    cluster->SetCOState(state);
                }
            }
            if (mq7_fault_streak >= SMOKE_CO_ALARM_FAULT_STREAK_THRESHOLD) {
                any_fault = true;
            }
        }
#endif

        if (auto *cluster = get_smoke_co_alarm_cluster(smoke_co_alarm_endpoint_id)) {
            if (any_fault != hardware_fault_active) {
                hardware_fault_active = any_fault;
                cluster->SetHardwareFaultAlert(hardware_fault_active);
                if (hardware_fault_active) {
                    ESP_LOGW(TAG, "Sensor reading stuck at an extreme value — check wiring (HardwareFaultAlert set)");
                }
            }

            /* See the header comment above for why the app, not the SDK,
             * has to clear TestInProgress once a simulated self-test has
             * run long enough. */
            if (cluster->GetTestInProgress()) {
                if (self_test_remaining_ms == 0) {
                    self_test_remaining_ms = SMOKE_CO_ALARM_SELF_TEST_DURATION_MS;
                    ESP_LOGI(TAG, "Self-test started");
                } else if (self_test_remaining_ms <= SMOKE_CO_ALARM_MEASURE_INTERVAL_MS) {
                    cluster->SetTestInProgress(false);
                    self_test_remaining_ms = 0;
                    ESP_LOGI(TAG, "Self-test complete");
                } else {
                    self_test_remaining_ms -= SMOKE_CO_ALARM_MEASURE_INTERVAL_MS;
                }
            } else {
                self_test_remaining_ms = 0;
            }

            cluster->SetExpressedStateByPriority(k_expressed_state_priority);
        }

        vTaskDelay(pdMS_TO_TICKS(SMOKE_CO_ALARM_MEASURE_INTERVAL_MS));
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

/* Every attribute this device exposes is either read-only (SmokeState/
 * COState/...) or handled entirely inside SmokeCoAlarmCluster itself
 * (SelfTestRequest) — nothing for app code to react to here. Required by
 * node::create()'s callback signature regardless. */
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

    /* 2. Set up the ADC channel(s) SENSOR_TYPE actually selects. */
    if (!sensor_setup()) {
        ESP_LOGE(TAG, "Sensor setup failed");
        return;
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

    /* 3. Build the Matter data model: one node, one Smoke/CO Alarm
     * endpoint. feature_flags picks which of Smoke Alarm / CO Alarm the
     * cluster actually exposes, matching SENSOR_TYPE above — the cluster
     * itself requires at least one of the two (VALIDATE_FEATURES_AT_LEAST_ONE
     * in esp-matter's own cluster::smoke_co_alarm::create()). */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    smoke_co_alarm::config_t smoke_co_alarm_config;
    uint32_t feature_flags = 0;
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ2
    feature_flags |= chip::to_underlying(SmokeCoAlarm::Feature::kSmokeAlarm);
#endif
#if SENSOR_TYPE == SENSOR_MQ2_MQ7 || SENSOR_TYPE == SENSOR_MQ7
    feature_flags |= chip::to_underlying(SmokeCoAlarm::Feature::kCoAlarm);
#endif
    smoke_co_alarm_config.smoke_co_alarm.feature_flags = feature_flags;

    endpoint_t *endpoint = smoke_co_alarm::create(node, &smoke_co_alarm_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create smoke/CO alarm endpoint");
        return;
    }
    smoke_co_alarm_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Smoke/CO alarm endpoint id: %u", smoke_co_alarm_endpoint_id);

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

    /* 5. Start reading the sensor(s) now that the data model + Matter
     * stack both exist — sensor_task() writes into the cluster created
     * above. */
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter smoke/CO alarm started. Scan the QR code to commission.");
}
