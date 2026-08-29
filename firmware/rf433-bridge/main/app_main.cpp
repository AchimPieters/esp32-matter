/*
 * Minimal Matter RF433 Bridge — sixty-sixth device type, and this repo's
 * first genuinely NEW bridge built from scratch (not a verbatim port of an
 * existing esp-matter reference the way firmware/ble-mesh-bridge/ is) —
 * a Matter Aggregator that dynamically bridges cheap 433MHz fixed-code
 * remotes/sensors (the EV1527/PT2262 chip family — the same encoder ICs
 * behind countless wireless doorbells, remote-control sockets, PIR
 * sensors, and door/window sensors sold worldwide) onto the Matter fabric
 * as real, dynamically-created Generic Switch endpoints.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Bridge architecture: reuses the SAME generic dynamic-endpoint
 * machinery firmware/ble-mesh-bridge/'s own ported reference uses ---------
 * `endpoint::aggregator::create()` (a real, complete esp-matter top-level
 * helper — confirmed by reading `esp_matter_endpoint.cpp`'s own
 * `aggregator::add()` directly, which only calls `add_device_type()`, no
 * manual cluster work needed) creates the root Aggregator endpoint, and
 * `app_bridge_initialize()`/`app_bridge_create_new_device()`/
 * `app_bridge_remove_device()` (from `examples/common/app_bridge`, the
 * same reusable, protocol-agnostic dynamic-endpoint infrastructure
 * Espressif's own BLE Mesh/Zigbee/ESP-NOW bridge examples all build on —
 * see `firmware/ble-mesh-bridge/README.md`'s own preamble for the fuller
 * architecture writeup) handle creating/removing real Matter endpoints at
 * runtime as RF codes are learned. Deliberately pulled in via a single,
 * narrowly-targeted `EXTRA_COMPONENT_DIRS` entry pointing directly at
 * `examples/common/app_bridge` (not the whole `examples/common`
 * directory the official bridge examples use) — see this file's own
 * `CMakeLists.txt` comment for why: blanket-including `examples/common`
 * risks pulling in `app_reset`, which this repo's own `firmware/light/`
 * CMakeLists.txt already documents as needing a `button` component this
 * project doesn't declare.
 *
 * Each bridged RF code becomes a real Generic Switch endpoint (device
 * type 0x000F, `endpoint::generic_switch::add()` — reused directly,
 * confirmed the same top-level helper firmware/generic-switch/'s own
 * standalone device type uses) with only the MomentarySwitch (MS) feature
 * enabled, the same "smallest reasonable next step" scope that file's own
 * header comment already establishes — this bridge deliberately does NOT
 * try to infer whether a given RF code represents a "door sensor," a
 * "PIR," or a "remote button": a bare 24-bit fixed code carries no
 * semantic meaning about what physical thing sent it, only that it was
 * sent. A bridged endpoint firing `InitialPress`/`ShortRelease` whenever
 * ITS OWN learned code is received again is an honest representation of
 * that — a real controller's own automation (e.g. Home Assistant) can
 * still attach whatever meaning it wants to a specific bridged switch
 * ("this one is the doorbell", "this one is the back door sensor").
 *
 * --- Decode algorithm: ported from a real, verified, widely-deployed
 * open-source reference, not invented -------------------------------------
 * No esp-matter reference exists for this protocol family at all, unlike
 * firmware/ble-mesh-bridge/'s own verbatim port — this repo's own
 * "smallest reasonable next step, verified against a real source"
 * discipline applies here as a from-scratch port instead. The self-
 * calibrating sync-gap-ratio decode technique (rather than hardcoding one
 * specific microsecond timing) is ported from the real, MIT-licensed,
 * extremely widely-deployed `rc-switch` Arduino library
 * (github.com/sui77/rc-switch, `RCSwitch::receiveProtocol()`/
 * `handleInterrupt()`, fetched and read directly) — the same "port a
 * real, working reference rather than guess the integration shape"
 * principle already used elsewhere in this repo (SM2335EGH, APA102,
 * OpenTherm), applied here because two independent primary-source
 * lookups for the EV1527 chip's own literal timing numbers (a
 * `mathertel/rfcodes` protocol writeup and a datasheet-derived summary)
 * gave meaningfully different microsecond values for the same nominal
 * protocol — a real, confirmed ambiguity (different real-world
 * EV1527-*compatible* remotes/sensors, often clone dies rather than the
 * genuine Silvan Chip part, don't all share identical exact timing) that
 * a self-calibrating decoder sidesteps entirely, the same way `rc-
 * switch`'s own real algorithm does: rather than trusting one hardcoded
 * "the" pulse width, the decoder derives its own per-frame `delay` (base
 * unit) from the actual sync gap it just measured (`delay = sync_gap_us /
 * RF433_SYNC_LENGTH_IN_PULSES`), then classifies every subsequent bit's
 * own high/low durations against `delay`-scaled thresholds with a real
 * tolerance band (`RF433_RECEIVE_TOLERANCE_PERCENT`, 60% — the exact
 * default `rc-switch` itself ships). Protocol 1's own ratios (sync
 * 1:`RF433_SYNC_LENGTH_IN_PULSES`=31, bit-0 high:low=1:3, bit-1 high:
 * low=3:1, 24 total bits) are `rc-switch`'s own documented "Protocol 1"
 * definition — the single most common protocol family for cheap 433MHz
 * fixed-code hardware, confirmed as the right scope choice against the
 * same source, not the much larger multi-protocol decode table a project
 * like `rtl_433` covers (a deliberate, documented scope cut, the same
 * "smallest reasonable next step" reasoning this repo applies
 * throughout, not a technical inability to decode more protocols). Also
 * ported directly: requiring the SAME 24-bit code twice in a row before
 * accepting it (`rc-switch`'s own `repeatCount == 2` check) and a
 * `changeCount > 7` minimum-edge-count floor before even attempting sync
 * detection — both real noise-rejection steps this repo's own port keeps
 * rather than drops, since a bridge that mis-registers random RF noise
 * as a "new device" during learn mode would be a genuinely worse product
 * than a bridge that occasionally needs a second remote-button press.
 *
 * --- Learn mode: a real, documented pairing UX, not silent auto-learn --
 * A physical LEARN button (`RF433_LEARN_BUTTON_GPIO`) opens a
 * `RF433_LEARN_WINDOW_MS` (15s) window during which the NEXT validated,
 * repeat-confirmed code becomes a newly bridged Generic Switch endpoint.
 * Outside a learn window, a validated code matching an ALREADY-bridged
 * endpoint's own stored code fires that endpoint's InitialPress/
 * ShortRelease events; a validated code matching nothing bridged yet is
 * logged and otherwise ignored — never silently auto-registered — the
 * same real "pairing mode" UX actual commercial 433MHz/RF bridge
 * products (Fibaro/Broadlink-class hubs) already use, and a deliberate
 * choice: silently learning every code ever heard (including a
 * neighbor's own garage-door remote) would be a real, unwanted behavior.
 *
 * `app_rf433_bridged_device_t` (this file's own `app_bridged_device_t`
 * subclass, matching `app_blemesh_bridged_device_t`'s own shape in
 * firmware/ble-mesh-bridge/'s ported reference) stores each bridged
 * endpoint's own learned 24-bit code as its "device address" — persisted
 * to NVS via `store_dev_addr()`/`restore_dev_addr()` so learned codes
 * survive a reboot, the same real persistence contract every bridged
 * device in esp-matter's own bridge infrastructure already provides.
 *
 * Standard quick-power-cycle factory reset — also calls
 * `esp_matter_bridge::factory_reset()` to erase all learned bridged-
 * device info, not just the Matter fabric credentials every other device
 * type's factory reset already clears.
 *
 * Hardware: a cheap, extremely common 433MHz superheterodyne OOK/ASK
 * receiver module (e.g. the ubiquitous "MX-RM-5V"/RXB6-class boards, ~$1,
 * a single digital DATA-out pin) wired to `RF433_RX_GPIO`, plus one
 * momentary LEARN pushbutton on `RF433_LEARN_BUTTON_GPIO` — the same
 * "GND -> button -> GPIO" reference wiring this repo's other buttons use.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested — no 433MHz receiver module or EV1527/PT2262-class
 * remote/sensor was physically available when this was written, and this
 * decoder's own real-world correctness (unlike this repo's other bit-
 * banged protocol drivers, e.g. DHT11/WS2812B, which WERE hardware-
 * verified before being trusted) rests entirely on the ported `rc-
 * switch` algorithm being faithfully reproduced, not on anything this
 * session could independently confirm against a real signal.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <driver/gpio.h>
#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_bridge.h>
#include <app_bridged_device.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/switch-server/switch-server.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_rf433_bridge";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). */
#define RF433_RX_GPIO GPIO_NUM_4          /* 433MHz receiver module DATA pin */
#define RF433_LEARN_BUTTON_GPIO GPIO_NUM_16 /* momentary, active-LOW, internal pull-up */

/* --- Decode algorithm constants — see the header comment above for the
 * full sourcing (ported from rc-switch's own real, verified algorithm and
 * "Protocol 1" definition, not invented). */
#define RF433_CODE_BITS 24
#define RF433_SYNC_LENGTH_IN_PULSES 31     /* sync gap = ~31x the base unit */
#define RF433_RECEIVE_TOLERANCE_PERCENT 60 /* rc-switch's own default */
#define RF433_MIN_CHANGE_COUNT 7           /* rc-switch's own minimum edge count before attempting sync detection */
#define RF433_GAP_CONSISTENCY_TOLERANCE_US 200 /* rc-switch's own consecutive-sync-gap consistency check */
/* Bit-0 = short HIGH, long LOW (ratio 1:3). Bit-1 = long HIGH, short LOW (ratio 3:1). */
#define RF433_ZERO_HIGH_RATIO 1
#define RF433_ZERO_LOW_RATIO 3
#define RF433_ONE_HIGH_RATIO 3
#define RF433_ONE_LOW_RATIO 1
/* Maximum edges tracked per frame: sync + 2 edges per bit, plus headroom. */
#define RF433_MAX_TIMINGS (2 + 2 * RF433_CODE_BITS + 4)

#define RF433_LEARN_WINDOW_MS 15000
#define RF433_LEARN_BUTTON_DEBOUNCE_SAMPLES 8

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t aggregator_endpoint_id = chip::kInvalidEndpointId;
static node_t *g_node = nullptr;
static volatile bool learn_mode_active = false;
static int64_t learn_mode_deadline_us = 0;

/* --- Bridged device address context ---------------------------------------
 * Stores each bridged endpoint's own learned 24-bit RF code — reused as
 * this device's "address" the same way firmware/ble-mesh-bridge/'s own
 * ported `app_blemesh_bridged_device_t` stores a BLE Mesh unicast
 * address. Persisted to NVS so learned codes survive a reboot. */
typedef struct {
    uint32_t code;
} rf433_device_addr_t;

class app_rf433_bridged_device_t : public app_bridged_device_t {
public:
    esp_err_t set_dev_addr(const void *addr_ctx) override
    {
        if (!addr_ctx) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!m_dev_addr_ctx) {
            m_dev_addr_ctx = chip::Platform::New<rf433_device_addr_t>();
        }
        if (!m_dev_addr_ctx) {
            return ESP_ERR_NO_MEM;
        }
        *(rf433_device_addr_t *)m_dev_addr_ctx = *(const rf433_device_addr_t *)addr_ctx;
        return ESP_OK;
    }

    bool check_dev_addr(const void *addr_ctx) override
    {
        if (!m_dev_addr_ctx || !addr_ctx) {
            return false;
        }
        return ((rf433_device_addr_t *)m_dev_addr_ctx)->code == ((const rf433_device_addr_t *)addr_ctx)->code;
    }

    esp_err_t delete_dev_addr() override
    {
        if (m_dev_addr_ctx) {
            chip::Platform::Delete((rf433_device_addr_t *)m_dev_addr_ctx);
            m_dev_addr_ctx = nullptr;
        }
        return ESP_OK;
    }

    esp_err_t store_dev_addr() override
    {
        if (!m_dev_addr_ctx || !m_dev) {
            return ESP_ERR_INVALID_STATE;
        }
        nvs_handle_t handle;
        char key[16];
        snprintf(key, sizeof(key), "rf433_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(RF433_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_set_blob(handle, key, m_dev_addr_ctx, sizeof(rf433_device_addr_t));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        return err;
    }

    esp_err_t restore_dev_addr() override
    {
        if (!m_dev) {
            return ESP_ERR_INVALID_STATE;
        }
        nvs_handle_t handle;
        char key[16];
        snprintf(key, sizeof(key), "rf433_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(RF433_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            return err;
        }
        if (!m_dev_addr_ctx) {
            m_dev_addr_ctx = chip::Platform::New<rf433_device_addr_t>();
        }
        size_t len = sizeof(rf433_device_addr_t);
        err = nvs_get_blob(handle, key, m_dev_addr_ctx, &len);
        nvs_close(handle);
        return err;
    }

    esp_err_t erase_dev_addr() override
    {
        if (!m_dev) {
            return ESP_ERR_INVALID_STATE;
        }
        nvs_handle_t handle;
        char key[16];
        snprintf(key, sizeof(key), "rf433_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(RF433_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_erase_key(handle, key);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        return err;
    }

private:
    static constexpr const char *RF433_NVS_NAMESPACE = "rf433_bridge";
};

/* --- Registry-lookup-and-cast helper — same pattern firmware/
 * generic-switch/'s own get_switch_cluster() already establishes,
 * parameterized by endpoint id here since a bridge has one live
 * SwitchCluster instance per bridged device, not just one for the whole
 * node. */
static SwitchCluster *get_switch_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, Switch::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<SwitchCluster *>(iface);
}

/* Called once per newly-created (or resumed) bridged endpoint to add the
 * real Matter clusters for its device type — the same
 * `create_bridge_devices` callback shape firmware/ble-mesh-bridge/'s own
 * ported reference already establishes, reused here for a single device
 * type (Generic Switch) rather than five. */
static esp_err_t create_bridge_devices(endpoint_t *ep, uint32_t device_type_id, void *priv_data)
{
    if (device_type_id != ESP_MATTER_GENERIC_SWITCH_DEVICE_TYPE_ID) {
        ESP_LOGE(TAG, "Unsupported bridged matter device type: 0x%08lx", (unsigned long)device_type_id);
        return ESP_ERR_INVALID_ARG;
    }
    generic_switch::config_t config;
    config.switch_cluster.number_of_positions = 2;
    config.switch_cluster.current_position = 0;
    config.switch_cluster.feature_flags = cluster::switch_cluster::feature::momentary_switch::get_id();
    return generic_switch::add(ep, &config);
}

static app_bridged_device_t *create_rf433_bridged_device(node_t *node, uint16_t endpoint)
{
    return chip::Platform::New<app_rf433_bridged_device_t>();
}

static void free_rf433_bridged_device(app_bridged_device_t *device)
{
    chip::Platform::Delete((app_rf433_bridged_device_t *)device);
}

/* --- 433MHz decode: ISR captures raw edge timestamps only, the task does
 * all the real timing math — same "ISR does the minimum, hand off to a
 * queue+task" convention this repo's other GPIO-interrupt drivers already
 * establish (firmware/contact-sensor/, firmware/switch/). */
static QueueHandle_t rf433_edge_queue = nullptr;

static void IRAM_ATTR rf433_isr_handler(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(rf433_edge_queue, &now_us, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

/* Matches a validated 24-bit code against every currently-bridged
 * endpoint's own stored code; fires InitialPress/ShortRelease on a match.
 * Returns true if a match was found and handled. */
static bool rf433_dispatch_known_code(uint32_t code)
{
    rf433_device_addr_t addr = {code};
    app_bridged_device_t *device = app_bridge_get_device(&addr);
    if (!device) {
        return false;
    }
    uint16_t endpoint_id = app_bridge_get_endpoint(&addr);
    SwitchCluster *cluster = get_switch_cluster(endpoint_id);
    if (!cluster) {
        ESP_LOGW(TAG, "No SwitchCluster found for endpoint %u", endpoint_id);
        return true;
    }
    cluster->SetCurrentPosition(1);
    cluster->OnInitialPress(1);
    cluster->SetCurrentPosition(0);
    cluster->OnShortRelease(0);
    ESP_LOGI(TAG, "Code 0x%06lx matched bridged endpoint %u — fired InitialPress/ShortRelease",
             (unsigned long)code, endpoint_id);
    return true;
}

/* Registers a brand-new bridged device for a code seen for the first time
 * while learn mode is active. */
static void rf433_learn_new_code(uint32_t code)
{
    rf433_device_addr_t addr = {code};
    esp_err_t err = app_bridge_create_new_device(g_node, aggregator_endpoint_id,
                                                 ESP_MATTER_GENERIC_SWITCH_DEVICE_TYPE_ID, &addr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create bridged device for code 0x%06lx: %d", (unsigned long)code, err);
        return;
    }
    ESP_LOGI(TAG, "Learned new code 0x%06lx as bridged endpoint %u — leaving learn mode",
             (unsigned long)code, app_bridge_get_endpoint(&addr));
    learn_mode_active = false;
}

/* The actual decode task: accumulates raw edge timestamps into a local
 * ring buffer of durations, and — following rc-switch's own real
 * `receiveProtocol()` algorithm — treats the MOST RECENT duration as a
 * candidate sync gap on every edge, self-calibrating the frame's own
 * base unit from it rather than trusting one hardcoded microsecond value.
 * See the header comment above for the full sourcing/reasoning. */
static void rf433_decode_task(void *arg)
{
    static int64_t timings[RF433_MAX_TIMINGS];
    static int change_count = 0;
    static int64_t last_edge_us = 0;
    static uint32_t last_confirmed_code = 0;
    static int64_t last_sync_gap_us = 0;
    static int repeat_count = 0;

    int64_t edge_us;
    for (;;) {
        if (xQueueReceive(rf433_edge_queue, &edge_us, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int64_t duration_us = (last_edge_us == 0) ? 0 : (edge_us - last_edge_us);
        last_edge_us = edge_us;
        if (duration_us <= 0) {
            continue;
        }

        /* Shift the new duration in; timings[0] is always the most
         * recent (candidate sync) duration, matching rc-switch's own
         * indexing. */
        if (change_count < RF433_MAX_TIMINGS) {
            change_count++;
        }
        for (int i = change_count - 1; i > 0; i--) {
            timings[i] = timings[i - 1];
        }
        timings[0] = duration_us;

        if (change_count < RF433_MIN_CHANGE_COUNT) {
            continue;
        }

        /* Does timings[0] look like a plausible Protocol-1 sync gap? */
        int64_t delay = timings[0] / RF433_SYNC_LENGTH_IN_PULSES;
        if (delay < 50) {
            continue; /* implausibly short base unit — not a real sync gap */
        }
        int64_t tolerance = delay * RF433_RECEIVE_TOLERANCE_PERCENT / 100;

        /* Need exactly RF433_CODE_BITS*2 + 1 more edges after the sync
         * gap itself (2 per bit, plus the sync gap slot already in
         * timings[0]). */
        int required = 1 + 2 * RF433_CODE_BITS;
        if (change_count < required) {
            continue;
        }

        /* Decode each bit from timings[1..required-1] (oldest-first, so
         * iterate from the high end of the shifted array back down). */
        uint32_t code = 0;
        bool valid = true;
        for (int bit = 0; bit < RF433_CODE_BITS; bit++) {
            int64_t high_us = timings[required - 1 - 2 * bit];
            int64_t low_us = timings[required - 2 - 2 * bit];
            bool is_zero = (llabs(high_us - delay * RF433_ZERO_HIGH_RATIO) < tolerance) &&
                           (llabs(low_us - delay * RF433_ZERO_LOW_RATIO) < tolerance);
            bool is_one = (llabs(high_us - delay * RF433_ONE_HIGH_RATIO) < tolerance) &&
                          (llabs(low_us - delay * RF433_ONE_LOW_RATIO) < tolerance);
            if (is_zero == is_one) { /* neither matched, or ambiguous */
                valid = false;
                break;
            }
            code = (code << 1) | (is_one ? 1u : 0u);
        }

        if (!valid) {
            continue;
        }

        /* Consecutive-sync-gap consistency + repeat-count check — both
         * real rc-switch noise-rejection steps, reused as-is. */
        if (llabs(timings[0] - last_sync_gap_us) < RF433_GAP_CONSISTENCY_TOLERANCE_US && code == last_confirmed_code) {
            repeat_count++;
        } else {
            repeat_count = 1;
        }
        last_sync_gap_us = timings[0];
        last_confirmed_code = code;
        change_count = 0; /* start fresh for the next frame */

        if (repeat_count < 2) {
            continue;
        }
        repeat_count = 0; /* avoid re-dispatching on every further repeat of a held button */

        ESP_LOGI(TAG, "Validated code: 0x%06lx (base unit ~%lldus)", (unsigned long)code, (long long)delay);

        if (rf433_dispatch_known_code(code)) {
            continue;
        }

        if (learn_mode_active && esp_timer_get_time() < learn_mode_deadline_us) {
            rf433_learn_new_code(code);
        } else {
            ESP_LOGI(TAG, "Code 0x%06lx not bridged and learn mode is off — ignoring", (unsigned long)code);
        }
    }
}

/* --- Learn button: simple debounced poll, same shape firmware/
 * refrigerator/'s own door-sensor poll already establishes. */
static void rf433_learn_button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(RF433_LEARN_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        bool raw_pressed = (gpio_get_level(RF433_LEARN_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < RF433_LEARN_BUTTON_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == RF433_LEARN_BUTTON_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            learn_mode_active = true;
            learn_mode_deadline_us = esp_timer_get_time() + (int64_t)RF433_LEARN_WINDOW_MS * 1000;
            ESP_LOGI(TAG, "Learn mode ON for %d seconds — trigger the remote/sensor you want to bridge",
                     RF433_LEARN_WINDOW_MS / 1000);
        } else if (consistent_samples == RF433_LEARN_BUTTON_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
            debounced_pressed = false;
        }

        if (learn_mode_active && esp_timer_get_time() >= learn_mode_deadline_us) {
            learn_mode_active = false;
            ESP_LOGI(TAG, "Learn mode window expired");
        }
    }
}

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

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

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
    /* 1. Init NVS — stores the Matter fabric keys, factory data, and this
     * bridge's own learned RF codes. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the 433MHz receiver input + its interrupt (ANYEDGE —
     * both edges are needed to measure every mark and space duration). */
    gpio_config_t rx_io_conf = {};
    rx_io_conf.pin_bit_mask = (1ULL << RF433_RX_GPIO);
    rx_io_conf.mode = GPIO_MODE_INPUT;
    rx_io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&rx_io_conf);

    rf433_edge_queue = xQueueCreate(128, sizeof(int64_t));
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    gpio_isr_handler_add(RF433_RX_GPIO, rf433_isr_handler, NULL);
    xTaskCreate(rf433_decode_task, "rf433_decode", 4096, NULL, 10, NULL);

    /* 2b. Configure the learn button — active-LOW, internal pull-up. */
    gpio_config_t learn_io_conf = {};
    learn_io_conf.pin_bit_mask = (1ULL << RF433_LEARN_BUTTON_GPIO);
    learn_io_conf.mode = GPIO_MODE_INPUT;
    learn_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&learn_io_conf);
    xTaskCreate(rf433_learn_button_task, "rf433_learn_button", 3072, NULL, 5, NULL);

    /* 3. Build the Matter data model: one node, one Aggregator (bridge)
     * root endpoint — see the header comment above for the full bridge-
     * architecture detail. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "RF433 Bridge",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    g_node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!g_node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    aggregator::config_t aggregator_config;
    endpoint_t *aggregator = endpoint::aggregator::create(g_node, &aggregator_config, ENDPOINT_FLAG_NONE, NULL);
    if (!aggregator) {
        ESP_LOGE(TAG, "Failed to create aggregator endpoint");
        return;
    }
    aggregator_endpoint_id = endpoint::get_id(aggregator);
    ESP_LOGI(TAG, "Aggregator endpoint id: %u", aggregator_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* 5. Resume any previously-learned bridged devices, and register the
     * device-type-creation callback for any newly learned ones. */
    err = app_bridge_initialize(g_node, create_bridge_devices, create_rf433_bridged_device, free_rf433_bridged_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the bridge: %d", err);
        return;
    }

    /* If step 1's boot-count check detected 3 quick power cycles in a
     * row, factory-reset now that Matter has actually started — also
     * erases every learned RF code, not just the fabric credentials. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter_bridge::factory_reset();
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter RF433 bridge started. Scan the QR code to commission, then press the learn "
                  "button and trigger a remote/sensor to bridge it.");
}
