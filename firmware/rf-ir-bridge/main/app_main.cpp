/*
 * Minimal Matter RF433/IR Bridge — sixty-sixth device type: ONE Matter
 * Aggregator that can bridge 433MHz fixed-code remotes/sensors AND NEC-
 * protocol infrared remote buttons at the same time, on the same node —
 * the merged replacement for what this repo originally shipped as two
 * separate device types (`firmware/rf433-bridge/`, `firmware/ir-bridge/`).
 * Real, explicit user feedback after those two shipped: a single bridge
 * a user can independently enable/disable each protocol support on (via
 * two checkable fields in `tools/product-wizard/`, each backed by its
 * own real GPIO — `GPIO_NUM_NC` disables that protocol entirely) is a
 * more honest match for what a real "universal RF/IR bridge" product
 * actually is than two single-purpose firmware images, especially since
 * both share the exact same underlying Matter bridge architecture and
 * only differ in their own decode front-end. Both protocols default ON,
 * matching what each one shipped as on its own before this merge; at
 * least one must stay enabled (a bridge with neither protocol wired up
 * would be a real device that does nothing).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Bridge architecture — unchanged from the two device types this
 * merges: one Aggregator, one shared dynamic-endpoint bridge -------------
 * `endpoint::aggregator::create()` + `app_bridge_initialize()`/
 * `app_bridge_create_new_device()`/`app_bridge_remove_device()` (from
 * `examples/common/app_bridge`) are exactly as documented in this
 * repo's own firmware/ble-mesh-bridge/README.md preamble — the same
 * reusable, protocol-agnostic infrastructure every bridge in this repo
 * shares. A single shared `app_rfir_bridged_device_t` (this file's own
 * `app_bridged_device_t` subclass) now stores a small tagged union — a
 * `bridge_source_t` (kRF433 or kIR) plus either a 24-bit RF433 code or a
 * (address, command) IR pair — instead of the two separate, protocol-
 * specific address classes the original two files each had. Every
 * bridged device is still a Generic Switch endpoint (device type 0x000F,
 * MomentarySwitch feature only, firing InitialPress/ShortRelease on a
 * matched code regardless of which protocol it came from) — the same
 * honest, semantics-free bridged-device shape those two files' own
 * header comments already establish in full (a bare learned code/button
 * carries no meaning about what physical thing sent it).
 *
 * A single, shared LEARN button now serves BOTH protocols at once:
 * pressing it opens one 15-second window during which the next
 * validated code from EITHER decoder — whichever one fires first —
 * becomes a newly bridged endpoint. This is a genuine simplification
 * from having two separate learn buttons, and matches how a real
 * "universal" RF/IR bridge hub product would actually work (one pairing
 * button, whatever remote you press next gets learned).
 *
 * --- RF433 decode (EV1527/PT2262-family) and NEC IR decode: both
 * reused byte-for-byte from this repo's own original two device types
 * -----------------------------------------------------------------------
 * See those files' own former header comments (preserved in git history,
 * and in this file's own per-function comments below) for the complete
 * sourcing: RF433's self-calibrating sync-gap-ratio decoder ported from
 * the real, MIT-licensed `rc-switch` library (chosen specifically because
 * two independent primary-source lookups for the EV1527 chip's own
 * literal timing gave meaningfully different microsecond values); NEC's
 * fixed-tolerance decoder cross-checked against two independent primary
 * sources that agreed exactly. Both still use the same plain GPIO-
 * interrupt-edge-timing technique (not ESP-IDF's dedicated RMT RX
 * peripheral) for the same reasons those files' own header comments
 * already documented.
 *
 * Each protocol's own GPIO/ISR/queue/decode-task is now conditionally
 * set up at runtime — `if (RF433_RX_GPIO != GPIO_NUM_NC)` / `if
 * (IR_RX_GPIO != GPIO_NUM_NC)` — rather than a compile-time `#if`, the
 * same "runtime check, not preprocessor comparison" precedent firmware/
 * addressable-light/'s own `identify_via_strip` GPIO-equality lesson,
 * firmware/oven/'s own optional door sensor, and firmware/robot-vacuum/'s
 * own optional dock sensor already establish (GPIO_NUM_* values are
 * plain C enum constants, not preprocessor macros, so `#if` comparisons
 * between them don't reliably work at all). `RF433_RX_GPIO`/`IR_RX_GPIO`
 * both ship at real, enabled default pins (unlike e.g. firmware/oven/'s
 * own door sensor, which ships disabled) — this bridge's whole purpose
 * is bridging at least one protocol, so both-enabled is the honest
 * default, not both-disabled.
 *
 * Standard quick-power-cycle factory reset (also erases every learned
 * bridged-device code of either protocol via `esp_matter_bridge::
 * factory_reset()`, same as both original files).
 *
 * Hardware: a cheap 433MHz superheterodyne OOK/ASK receiver module
 * (e.g. MX-RM-5V/RXB6-class, ~$1) on `RF433_RX_GPIO`, a cheap 38kHz IR
 * receiver/demodulator module (e.g. TSOP38238/VS1838B-class, ~$1,
 * inverted logic — idle HIGH, LOW during a real carrier burst) on
 * `IR_RX_GPIO`, and one shared momentary LEARN pushbutton on
 * `RFIR_LEARN_BUTTON_GPIO`. Wire only the receiver(s) you actually have
 * and disable the other in the wizard (or by hand-editing its own
 * `#define` to `GPIO_NUM_NC`) — nothing breaks by leaving one
 * unconnected while its own `#define` still points at a real pin, since
 * that decoder will just never see a valid sync/AGC pattern on a
 * floating input, but disabling it outright avoids wasting a GPIO/ISR
 * on a receiver that was never actually wired up.
 *
 * Build-verified in Docker for all three meaningful configurations
 * (RF433-only, IR-only, both enabled); not hardware-tested — no 433MHz
 * receiver module, IR receiver module, or real EV1527/PT2262/NEC-
 * protocol remote/sensor was physically available when this was
 * written, the same standing caveat both of this file's own predecessor
 * device types already carried.
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

static const char *TAG = "matter_rf_ir_bridge";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). Both receivers ship
 * enabled by default (GPIO_NUM_NC disables one) — see the header comment
 * above for why. RF433_RX_GPIO is the 433MHz receiver module's DATA pin;
 * IR_RX_GPIO is the IR receiver module's OUT pin (inverted logic — see the
 * header comment above). Deliberately NOT commented inline the way
 * RFIR_LEARN_BUTTON_GPIO below is: the product wizard's own generated sed
 * command for each of these two rewrites the WHOLE line (a broad `.*`
 * match, since either can also be set to GPIO_NUM_NC to disable that
 * protocol) rather than just the GPIO_NUM_<n> token the way driver/
 * secondary-style fields do — an inline trailing comment on either of
 * these two lines would get silently stripped by that sed the first time
 * a product actually changes the pin. */
#define RF433_RX_GPIO GPIO_NUM_4
#define IR_RX_GPIO GPIO_NUM_5
#define RFIR_LEARN_BUTTON_GPIO GPIO_NUM_16  /* momentary, active-LOW, internal pull-up; shared by both protocols */

/* --- RF433 decode algorithm constants — ported from rc-switch's own
 * real, verified algorithm and "Protocol 1" definition, see the header
 * comment above for the full sourcing. */
#define RF433_CODE_BITS 24
#define RF433_SYNC_LENGTH_IN_PULSES 31
#define RF433_RECEIVE_TOLERANCE_PERCENT 60
#define RF433_MIN_CHANGE_COUNT 7
#define RF433_GAP_CONSISTENCY_TOLERANCE_US 200
#define RF433_ZERO_HIGH_RATIO 1
#define RF433_ZERO_LOW_RATIO 3
#define RF433_ONE_HIGH_RATIO 3
#define RF433_ONE_LOW_RATIO 1
#define RF433_MAX_TIMINGS (2 + 2 * RF433_CODE_BITS + 4)

/* --- NEC IR protocol timing (microseconds) — cross-checked against two
 * independent primary sources, see the header comment above. */
#define NEC_AGC_MARK_US 9000
#define NEC_AGC_SPACE_US 4500
#define NEC_REPEAT_SPACE_US 2250
#define NEC_BIT_MARK_US 560
#define NEC_ZERO_SPACE_US 560
#define NEC_ONE_SPACE_US 1690
#define NEC_TOLERANCE_PERCENT 25
#define NEC_TOTAL_BITS 32

#define RFIR_LEARN_WINDOW_MS 15000
#define RFIR_LEARN_BUTTON_DEBOUNCE_SAMPLES 8

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

/* --- Bridged device address context — a shared tagged union covering
 * both protocols, replacing the two separate, protocol-specific address
 * classes firmware/rf433-bridge/'s and firmware/ir-bridge/'s own files
 * each had before this merge. */
enum class bridge_source_t : uint8_t { kRF433 = 0, kIR = 1 };

typedef struct {
    bridge_source_t source;
    union {
        uint32_t rf433_code;
        struct {
            uint8_t address;
            uint8_t command;
        } ir;
    };
} bridge_device_addr_t;

static bool bridge_addr_equal(const bridge_device_addr_t *a, const bridge_device_addr_t *b)
{
    if (a->source != b->source) {
        return false;
    }
    if (a->source == bridge_source_t::kRF433) {
        return a->rf433_code == b->rf433_code;
    }
    return a->ir.address == b->ir.address && a->ir.command == b->ir.command;
}

class app_rfir_bridged_device_t : public app_bridged_device_t {
public:
    esp_err_t set_dev_addr(const void *addr_ctx) override
    {
        if (!addr_ctx) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!m_dev_addr_ctx) {
            m_dev_addr_ctx = chip::Platform::New<bridge_device_addr_t>();
        }
        if (!m_dev_addr_ctx) {
            return ESP_ERR_NO_MEM;
        }
        *(bridge_device_addr_t *)m_dev_addr_ctx = *(const bridge_device_addr_t *)addr_ctx;
        return ESP_OK;
    }

    bool check_dev_addr(const void *addr_ctx) override
    {
        if (!m_dev_addr_ctx || !addr_ctx) {
            return false;
        }
        return bridge_addr_equal((bridge_device_addr_t *)m_dev_addr_ctx, (const bridge_device_addr_t *)addr_ctx);
    }

    esp_err_t delete_dev_addr() override
    {
        if (m_dev_addr_ctx) {
            chip::Platform::Delete((bridge_device_addr_t *)m_dev_addr_ctx);
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
        snprintf(key, sizeof(key), "rfir_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(RFIR_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_set_blob(handle, key, m_dev_addr_ctx, sizeof(bridge_device_addr_t));
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
        snprintf(key, sizeof(key), "rfir_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(RFIR_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            return err;
        }
        if (!m_dev_addr_ctx) {
            m_dev_addr_ctx = chip::Platform::New<bridge_device_addr_t>();
        }
        size_t len = sizeof(bridge_device_addr_t);
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
        snprintf(key, sizeof(key), "rfir_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(RFIR_NVS_NAMESPACE, NVS_READWRITE, &handle);
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
    static constexpr const char *RFIR_NVS_NAMESPACE = "rfir_bridge";
};

/* --- Registry-lookup-and-cast helper — same pattern firmware/
 * generic-switch/'s own get_switch_cluster() already establishes. */
static SwitchCluster *get_switch_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, Switch::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<SwitchCluster *>(iface);
}

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

static app_bridged_device_t *create_rfir_bridged_device(node_t *node, uint16_t endpoint)
{
    return chip::Platform::New<app_rfir_bridged_device_t>();
}

static void free_rfir_bridged_device(app_bridged_device_t *device)
{
    chip::Platform::Delete((app_rfir_bridged_device_t *)device);
}

/* --- Shared dispatch/learn, called by whichever protocol's own decode
 * task just validated a code — the one real behavioral change from
 * having two separate files: both protocols now feed the SAME learn
 * window and the SAME bridged-device list. */
static bool bridge_dispatch_known(const bridge_device_addr_t *addr)
{
    app_bridged_device_t *device = app_bridge_get_device(addr);
    if (!device) {
        return false;
    }
    uint16_t endpoint_id = app_bridge_get_endpoint(addr);
    SwitchCluster *cluster = get_switch_cluster(endpoint_id);
    if (!cluster) {
        ESP_LOGW(TAG, "No SwitchCluster found for endpoint %u", endpoint_id);
        return true;
    }
    cluster->SetCurrentPosition(1);
    cluster->OnInitialPress(1);
    cluster->SetCurrentPosition(0);
    cluster->OnShortRelease(0);
    ESP_LOGI(TAG, "Matched bridged endpoint %u — fired InitialPress/ShortRelease", endpoint_id);
    return true;
}

static void bridge_learn_new(const bridge_device_addr_t *addr)
{
    esp_err_t err = app_bridge_create_new_device(g_node, aggregator_endpoint_id,
                                                 ESP_MATTER_GENERIC_SWITCH_DEVICE_TYPE_ID, (void *)addr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create bridged device: %d", err);
        return;
    }
    ESP_LOGI(TAG, "Learned new %s code as bridged endpoint %u — leaving learn mode",
             addr->source == bridge_source_t::kRF433 ? "RF433" : "IR", app_bridge_get_endpoint(addr));
    learn_mode_active = false;
}

static void bridge_dispatch_or_learn(const bridge_device_addr_t *addr)
{
    if (bridge_dispatch_known(addr)) {
        return;
    }
    if (learn_mode_active && esp_timer_get_time() < learn_mode_deadline_us) {
        bridge_learn_new(addr);
    } else {
        ESP_LOGI(TAG, "Code not bridged and learn mode is off — ignoring");
    }
}

/* ============================================================
 * RF433 decode (EV1527/PT2262 family) — reused verbatim from
 * firmware/rf433-bridge/'s own original file.
 * ============================================================ */
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

        int64_t delay = timings[0] / RF433_SYNC_LENGTH_IN_PULSES;
        if (delay < 50) {
            continue;
        }
        int64_t tolerance = delay * RF433_RECEIVE_TOLERANCE_PERCENT / 100;

        int required = 1 + 2 * RF433_CODE_BITS;
        if (change_count < required) {
            continue;
        }

        uint32_t code = 0;
        bool valid = true;
        for (int bit = 0; bit < RF433_CODE_BITS; bit++) {
            int64_t high_us = timings[required - 1 - 2 * bit];
            int64_t low_us = timings[required - 2 - 2 * bit];
            bool is_zero = (llabs(high_us - delay * RF433_ZERO_HIGH_RATIO) < tolerance) &&
                           (llabs(low_us - delay * RF433_ZERO_LOW_RATIO) < tolerance);
            bool is_one = (llabs(high_us - delay * RF433_ONE_HIGH_RATIO) < tolerance) &&
                          (llabs(low_us - delay * RF433_ONE_LOW_RATIO) < tolerance);
            if (is_zero == is_one) {
                valid = false;
                break;
            }
            code = (code << 1) | (is_one ? 1u : 0u);
        }

        if (!valid) {
            continue;
        }

        if (llabs(timings[0] - last_sync_gap_us) < RF433_GAP_CONSISTENCY_TOLERANCE_US && code == last_confirmed_code) {
            repeat_count++;
        } else {
            repeat_count = 1;
        }
        last_sync_gap_us = timings[0];
        last_confirmed_code = code;
        change_count = 0;

        if (repeat_count < 2) {
            continue;
        }
        repeat_count = 0;

        ESP_LOGI(TAG, "Validated RF433 code: 0x%06lx (base unit ~%lldus)", (unsigned long)code, (long long)delay);

        bridge_device_addr_t addr = {};
        addr.source = bridge_source_t::kRF433;
        addr.rf433_code = code;
        bridge_dispatch_or_learn(&addr);
    }
}

/* ============================================================
 * NEC IR decode — reused verbatim from firmware/ir-bridge/'s own
 * original file.
 * ============================================================ */
static QueueHandle_t ir_edge_queue = nullptr;

static void IRAM_ATTR ir_isr_handler(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    int level = gpio_get_level(IR_RX_GPIO);
    int64_t packed = (now_us << 1) | (level & 1);
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(ir_edge_queue, &packed, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static inline bool ir_within_tolerance(int64_t measured_us, int64_t expected_us)
{
    int64_t tolerance = expected_us * NEC_TOLERANCE_PERCENT / 100;
    return llabs(measured_us - expected_us) <= tolerance;
}

static void ir_decode_task(void *arg)
{
    enum class State { kIdle, kAgcSeen, kReceivingBits };
    State state = State::kIdle;
    int bits_received = 0;
    uint32_t frame = 0;
    int64_t last_edge_us = 0;
    int last_level = 1;

    int64_t packed;
    for (;;) {
        if (xQueueReceive(ir_edge_queue, &packed, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int64_t edge_us = packed >> 1;
        int level_after = (int)(packed & 1);
        int64_t duration_us = (last_edge_us == 0) ? 0 : (edge_us - last_edge_us);
        int level_before = last_level;
        last_edge_us = edge_us;
        last_level = level_after;

        if (duration_us <= 0) {
            continue;
        }

        bool was_mark = (level_before == 0);

        switch (state) {
        case State::kIdle:
            if (was_mark && ir_within_tolerance(duration_us, NEC_AGC_MARK_US)) {
                state = State::kAgcSeen;
            }
            break;
        case State::kAgcSeen:
            if (!was_mark) {
                if (ir_within_tolerance(duration_us, NEC_AGC_SPACE_US)) {
                    state = State::kReceivingBits;
                    bits_received = 0;
                    frame = 0;
                } else if (ir_within_tolerance(duration_us, NEC_REPEAT_SPACE_US)) {
                    /* Repeat code (button held) — deliberately not
                     * re-dispatched, same "one physical press, one
                     * event pair" choice this file's own RF433 repeat-
                     * count de-duplication makes for a held RF button. */
                    state = State::kIdle;
                } else {
                    state = State::kIdle;
                }
            }
            break;
        case State::kReceivingBits:
            if (was_mark) {
                if (!ir_within_tolerance(duration_us, NEC_BIT_MARK_US)) {
                    state = State::kIdle;
                }
            } else {
                bool is_zero = ir_within_tolerance(duration_us, NEC_ZERO_SPACE_US);
                bool is_one = ir_within_tolerance(duration_us, NEC_ONE_SPACE_US);
                if (is_zero == is_one) {
                    state = State::kIdle;
                    break;
                }
                frame |= (is_one ? 1u : 0u) << bits_received;
                bits_received++;
                if (bits_received == NEC_TOTAL_BITS) {
                    uint8_t address = (uint8_t)(frame & 0xFF);
                    uint8_t address_check = (uint8_t)((frame >> 8) & 0xFF);
                    uint8_t command = (uint8_t)((frame >> 16) & 0xFF);
                    uint8_t command_check = (uint8_t)((frame >> 24) & 0xFF);
                    state = State::kIdle;

                    if ((uint8_t)(~address_check) != address || (uint8_t)(~command_check) != command) {
                        ESP_LOGW(TAG, "NEC frame failed inversion check — discarding (addr 0x%02x/~0x%02x, "
                                      "cmd 0x%02x/~0x%02x)",
                                 address, address_check, command, command_check);
                        break;
                    }

                    ESP_LOGI(TAG, "Validated NEC frame: address 0x%02x, command 0x%02x", address, command);

                    bridge_device_addr_t addr = {};
                    addr.source = bridge_source_t::kIR;
                    addr.ir.address = address;
                    addr.ir.command = command;
                    bridge_dispatch_or_learn(&addr);
                }
            }
            break;
        }
    }
}

/* --- Shared learn button: same shape both original files used, now
 * common to both protocols. */
static void rfir_learn_button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(RFIR_LEARN_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        bool raw_pressed = (gpio_get_level(RFIR_LEARN_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < RFIR_LEARN_BUTTON_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == RFIR_LEARN_BUTTON_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            learn_mode_active = true;
            learn_mode_deadline_us = esp_timer_get_time() + (int64_t)RFIR_LEARN_WINDOW_MS * 1000;
            ESP_LOGI(TAG, "Learn mode ON for %d seconds — trigger the remote/sensor you want to bridge",
                     RFIR_LEARN_WINDOW_MS / 1000);
        } else if (consistent_samples == RFIR_LEARN_BUTTON_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
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
     * bridge's own learned RF433/IR codes. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure whichever receiver(s) are actually enabled — see the
     * header comment above for why this is a runtime check, not #if. */
    bool rf433_enabled = (RF433_RX_GPIO != GPIO_NUM_NC);
    bool ir_enabled = (IR_RX_GPIO != GPIO_NUM_NC);
    bool isr_service_installed = false;

    if (rf433_enabled) {
        gpio_config_t rx_io_conf = {};
        rx_io_conf.pin_bit_mask = (1ULL << RF433_RX_GPIO);
        rx_io_conf.mode = GPIO_MODE_INPUT;
        rx_io_conf.intr_type = GPIO_INTR_ANYEDGE;
        gpio_config(&rx_io_conf);

        rf433_edge_queue = xQueueCreate(128, sizeof(int64_t));
        esp_err_t isr_svc_err = gpio_install_isr_service(0);
        if (isr_svc_err == ESP_OK) {
            isr_service_installed = true;
        } else if (isr_svc_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
        }
        gpio_isr_handler_add(RF433_RX_GPIO, rf433_isr_handler, NULL);
        xTaskCreate(rf433_decode_task, "rf433_decode", 4096, NULL, 10, NULL);
        ESP_LOGI(TAG, "RF433 receiver enabled on GPIO %d", RF433_RX_GPIO);
    } else {
        ESP_LOGI(TAG, "RF433 receiver disabled (GPIO_NUM_NC)");
    }

    if (ir_enabled) {
        gpio_config_t rx_io_conf = {};
        rx_io_conf.pin_bit_mask = (1ULL << IR_RX_GPIO);
        rx_io_conf.mode = GPIO_MODE_INPUT;
        rx_io_conf.intr_type = GPIO_INTR_ANYEDGE;
        gpio_config(&rx_io_conf);

        ir_edge_queue = xQueueCreate(256, sizeof(int64_t));
        if (!isr_service_installed) {
            esp_err_t isr_svc_err = gpio_install_isr_service(0);
            if (isr_svc_err == ESP_OK) {
                isr_service_installed = true;
            } else if (isr_svc_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
            }
        }
        gpio_isr_handler_add(IR_RX_GPIO, ir_isr_handler, NULL);
        xTaskCreate(ir_decode_task, "ir_decode", 4096, NULL, 10, NULL);
        ESP_LOGI(TAG, "IR receiver enabled on GPIO %d", IR_RX_GPIO);
    } else {
        ESP_LOGI(TAG, "IR receiver disabled (GPIO_NUM_NC)");
    }

    /* 2b. Configure the shared learn button — active-LOW, internal
     * pull-up. */
    gpio_config_t learn_io_conf = {};
    learn_io_conf.pin_bit_mask = (1ULL << RFIR_LEARN_BUTTON_GPIO);
    learn_io_conf.mode = GPIO_MODE_INPUT;
    learn_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&learn_io_conf);
    xTaskCreate(rfir_learn_button_task, "rfir_learn_button", 3072, NULL, 5, NULL);

    /* 3. Build the Matter data model: one node, one Aggregator (bridge)
     * root endpoint — see the header comment above for the full bridge-
     * architecture detail. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "RF433/IR Bridge",
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
    err = app_bridge_initialize(g_node, create_bridge_devices, create_rfir_bridged_device, free_rfir_bridged_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the bridge: %d", err);
        return;
    }

    /* If step 1's boot-count check detected 3 quick power cycles in a
     * row, factory-reset now that Matter has actually started — also
     * erases every learned code, not just the fabric credentials. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter_bridge::factory_reset();
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter RF433/IR bridge started. Scan the QR code to commission, then press the learn "
                  "button and trigger the remote/sensor you want to bridge.");
}
