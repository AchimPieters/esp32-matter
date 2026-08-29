/*
 * Minimal Matter Infrared Bridge — sixty-seventh device type: a Matter
 * Aggregator that dynamically bridges infrared remote-control button
 * presses (the NEC protocol — by far the most common IR protocol across
 * cheap consumer electronics remotes worldwide) onto the Matter fabric as
 * real, dynamically-created Generic Switch endpoints. The direct sibling
 * of firmware/rf433-bridge/ — same bridge architecture, same learn-mode
 * UX, same bridged-device shape, a different decode front-end.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Bridge architecture: identical to firmware/rf433-bridge/'s own —
 * see that file's own header comment for the full writeup of
 * `endpoint::aggregator::create()`, `app_bridge_initialize()`/
 * `app_bridge_create_new_device()`, the `EXTRA_COMPONENT_DIRS` pointed
 * directly at `examples/common/app_bridge` (not the whole `examples/
 * common` directory — same real `app_reset`/`button` gotcha firmware/
 * light/'s own CMakeLists.txt comment documents), and why each bridged
 * IR remote button becomes a Generic Switch endpoint (device type
 * 0x000F, MomentarySwitch feature only) rather than this bridge guessing
 * what a given button is "for." A learned NEC (Address, Command) pair
 * carries exactly as much semantic meaning as a learned 433MHz code
 * does — none on its own — so the same honest "fire InitialPress/
 * ShortRelease, let a real controller's own automation decide what it
 * means" design is reused verbatim, not reinvented.
 *
 * --- Decode algorithm: NEC protocol, verified against real, independently
 * cross-checked primary-source timing --------------------------------------
 * No esp-matter reference exists for this either — a from-scratch port,
 * same as firmware/rf433-bridge/, but for a protocol with a genuinely
 * complete, unambiguous, widely-corroborated specification (unlike that
 * file's own EV1527/PT2262 timing, where two independent lookups
 * disagreed on the literal microsecond values). NEC's own timing was
 * fetched and cross-checked against sb-projects.net's own widely-cited
 * technical reference (the de facto canonical community source for IR
 * protocol timing, in the same "best available, independently checked"
 * category this repo already applies to e.g. SM2335EGH/APA102) and a
 * second, independent summary drawn from Vishay's own IR receiver
 * datasheet-adjacent documentation and multiple hobbyist technical write-
 * ups — both agreed exactly, with no ambiguity this time: a 9ms AGC
 * (mark) burst + 4.5ms space starts a frame; a logical 0 is a 560us mark
 * + 560us space (1.125ms total); a logical 1 is a 560us mark + 1690us
 * space (2.25ms total); 32 bits follow LSB-first (8-bit address + its
 * own bitwise-inverted 8-bit check byte + 8-bit command + its own
 * bitwise-inverted 8-bit check byte); a final 560us mark closes the
 * frame. `NEC_TOLERANCE_PERCENT` (25%) is a plain, reasonable
 * classification tolerance around those fixed values — unlike firmware/
 * rf433-bridge/'s own self-calibrating decoder (needed there because
 * different real EV1527-*compatible* clone dies don't all share
 * identical exact timing), NEC's own timing is tightly, consistently
 * specified across virtually every real NEC-protocol remote, so a fixed-
 * value classifier is the honest, correct approach here rather than
 * self-calibration solving a problem that doesn't actually exist for
 * this protocol.
 *
 * A real, worth-noting implementation choice: this file decodes IR
 * timing via the same plain GPIO-interrupt-edge-timing technique
 * firmware/rf433-bridge/'s own decoder uses (and firmware/generic-
 * switch/'s own press-timing state machine, at a coarser timescale) —
 * NOT ESP-IDF's dedicated `driver/rmt_rx.h` peripheral, which is
 * Espressif's own typically-recommended approach for IR receive (and
 * would be a genuine repo *first*, since firmware/addressable-light/'s
 * own RMT usage is TX-only, driving WS2812B-class LEDs, never RX). A
 * deliberate choice, not an oversight: NEC's own tightest real timing
 * margin (560us marks/spaces) is still comfortably within what a plain
 * ISR + `esp_timer_get_time()` can reliably resolve (unlike, say,
 * WS2812B's own sub-microsecond NRZ bit timing, which genuinely needs
 * RMT's own hardware-timed precision) — using the same GPIO-ISR
 * technique this repo already has two independent working examples of
 * (firmware/rf433-bridge/'s decoder, firmware/generic-switch/'s press
 * timing) keeps this file consistent with an already-proven pattern
 * rather than introducing a new peripheral API for a protocol that
 * doesn't strictly need it.
 *
 * The receiver's own output polarity is INVERTED relative to the
 * physical IR carrier — the standard behavior of every common IR
 * demodulator receiver module (TSOP38238-class, ~38kHz): output idles
 * HIGH, and goes LOW for the duration of each real 38kHz-modulated
 * "mark" burst. This file's own decode logic accounts for that directly
 * (a "mark" is measured as a LOW-level duration, a "space" as a HIGH-
 * level duration) rather than assuming non-inverted logic.
 *
 * Bridged-device architecture (the `app_ir_bridged_device_t` address-
 * context class, NVS persistence, `create_bridge_devices()` callback,
 * `SwitchCluster` registry-lookup-and-cast dispatch, learn-mode UX and
 * timeout) is reused byte-for-byte from firmware/rf433-bridge/'s own
 * file, just keyed on a 16-bit (Address, Command) pair instead of a
 * 24-bit RF code.
 *
 * Hardware: a cheap, extremely common 38kHz IR receiver/demodulator
 * module (e.g. the ubiquitous TSOP38238/VS1838B-class 3-pin modules,
 * ~$1, a single digital OUT pin, inverted logic as described above)
 * wired to `IR_RX_GPIO`, plus one momentary LEARN pushbutton on
 * `IR_LEARN_BUTTON_GPIO` — same "GND -> button -> GPIO" reference wiring
 * this repo's other buttons use.
 *
 * Standard quick-power-cycle factory reset (also erases every learned
 * bridged-device IR code via `esp_matter_bridge::factory_reset()`, same
 * as firmware/rf433-bridge/). Build-verified in Docker; not hardware-
 * tested — no IR receiver module or real NEC-protocol remote was
 * physically available when this was written.
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

static const char *TAG = "matter_ir_bridge";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). */
#define IR_RX_GPIO GPIO_NUM_4              /* IR receiver module OUT pin (inverted logic — see header comment) */
#define IR_LEARN_BUTTON_GPIO GPIO_NUM_16   /* momentary, active-LOW, internal pull-up */

/* --- NEC protocol timing (microseconds) — see the header comment above
 * for the full sourcing (cross-checked against two independent primary
 * sources, both agreeing exactly). */
#define NEC_AGC_MARK_US 9000
#define NEC_AGC_SPACE_US 4500
#define NEC_REPEAT_SPACE_US 2250
#define NEC_BIT_MARK_US 560
#define NEC_ZERO_SPACE_US 560
#define NEC_ONE_SPACE_US 1690
#define NEC_TOLERANCE_PERCENT 25
#define NEC_TOTAL_BITS 32 /* 8-bit address + ~address + 8-bit command + ~command, LSB first */

#define IR_LEARN_WINDOW_MS 15000
#define IR_LEARN_BUTTON_DEBOUNCE_SAMPLES 8

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

/* --- Bridged device address context ----------------------------------------
 * Stores each bridged endpoint's own learned (address, command) NEC pair
 * — reused as this device's "address" the same way firmware/rf433-
 * bridge/'s own `app_rf433_bridged_device_t` stores an RF code. */
typedef struct {
    uint8_t address;
    uint8_t command;
} ir_device_addr_t;

class app_ir_bridged_device_t : public app_bridged_device_t {
public:
    esp_err_t set_dev_addr(const void *addr_ctx) override
    {
        if (!addr_ctx) {
            return ESP_ERR_INVALID_ARG;
        }
        if (!m_dev_addr_ctx) {
            m_dev_addr_ctx = chip::Platform::New<ir_device_addr_t>();
        }
        if (!m_dev_addr_ctx) {
            return ESP_ERR_NO_MEM;
        }
        *(ir_device_addr_t *)m_dev_addr_ctx = *(const ir_device_addr_t *)addr_ctx;
        return ESP_OK;
    }

    bool check_dev_addr(const void *addr_ctx) override
    {
        if (!m_dev_addr_ctx || !addr_ctx) {
            return false;
        }
        const ir_device_addr_t *a = (ir_device_addr_t *)m_dev_addr_ctx;
        const ir_device_addr_t *b = (const ir_device_addr_t *)addr_ctx;
        return a->address == b->address && a->command == b->command;
    }

    esp_err_t delete_dev_addr() override
    {
        if (m_dev_addr_ctx) {
            chip::Platform::Delete((ir_device_addr_t *)m_dev_addr_ctx);
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
        snprintf(key, sizeof(key), "ir_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(IR_NVS_NAMESPACE, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return err;
        }
        err = nvs_set_blob(handle, key, m_dev_addr_ctx, sizeof(ir_device_addr_t));
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
        snprintf(key, sizeof(key), "ir_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(IR_NVS_NAMESPACE, NVS_READONLY, &handle);
        if (err != ESP_OK) {
            return err;
        }
        if (!m_dev_addr_ctx) {
            m_dev_addr_ctx = chip::Platform::New<ir_device_addr_t>();
        }
        size_t len = sizeof(ir_device_addr_t);
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
        snprintf(key, sizeof(key), "ir_%x", m_dev->persistent_info.device_endpoint_id);
        esp_err_t err = nvs_open(IR_NVS_NAMESPACE, NVS_READWRITE, &handle);
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
    static constexpr const char *IR_NVS_NAMESPACE = "ir_bridge";
};

/* --- Registry-lookup-and-cast helper — same pattern firmware/
 * rf433-bridge/'s own get_switch_cluster() already establishes. */
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

static app_bridged_device_t *create_ir_bridged_device(node_t *node, uint16_t endpoint)
{
    return chip::Platform::New<app_ir_bridged_device_t>();
}

static void free_ir_bridged_device(app_bridged_device_t *device)
{
    chip::Platform::Delete((app_ir_bridged_device_t *)device);
}

static bool ir_dispatch_known_code(uint8_t address, uint8_t command)
{
    ir_device_addr_t addr = {address, command};
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
    ESP_LOGI(TAG, "Address 0x%02x / Command 0x%02x matched bridged endpoint %u — fired InitialPress/ShortRelease",
             address, command, endpoint_id);
    return true;
}

static void ir_learn_new_code(uint8_t address, uint8_t command)
{
    ir_device_addr_t addr = {address, command};
    esp_err_t err = app_bridge_create_new_device(g_node, aggregator_endpoint_id,
                                                 ESP_MATTER_GENERIC_SWITCH_DEVICE_TYPE_ID, &addr, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create bridged device for address 0x%02x / command 0x%02x: %d", address, command,
                 err);
        return;
    }
    ESP_LOGI(TAG, "Learned new IR button (address 0x%02x, command 0x%02x) as bridged endpoint %u — leaving learn mode",
             address, command, app_bridge_get_endpoint(&addr));
    learn_mode_active = false;
}

/* --- IR decode: ISR captures raw edge timestamps only (same "ISR does the
 * minimum" convention firmware/rf433-bridge/'s own decoder already
 * establishes), the task does the real NEC frame-timing math. */
static QueueHandle_t ir_edge_queue = nullptr;

static void IRAM_ATTR ir_isr_handler(void *arg)
{
    int64_t now_us = esp_timer_get_time();
    int level = gpio_get_level(IR_RX_GPIO);
    /* Pack (timestamp, level-after-edge) into one 64-bit word: the low
     * bit carries the level, the rest carries the microsecond timestamp
     * (plenty of headroom — esp_timer wraps after ~292000 years). */
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
    /* Simple state machine: wait for the AGC mark+space, then decode 32
     * bits (each a mark + a space whose length says 0 or 1), LSB first. */
    enum class State { kIdle, kAgcSeen, kReceivingBits };
    State state = State::kIdle;
    int bits_received = 0;
    uint32_t frame = 0;
    int64_t last_edge_us = 0;
    int last_level = 1; /* receiver output idles HIGH (inverted logic) */

    int64_t packed;
    for (;;) {
        if (xQueueReceive(ir_edge_queue, &packed, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int64_t edge_us = packed >> 1;
        int level_after = (int)(packed & 1);
        int64_t duration_us = (last_edge_us == 0) ? 0 : (edge_us - last_edge_us);
        int level_before = last_level; /* the level that just ENDED, i.e. the duration's own level */
        last_edge_us = edge_us;
        last_level = level_after;

        if (duration_us <= 0) {
            continue;
        }

        bool was_mark = (level_before == 0); /* LOW = mark, inverted receiver logic */

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
                     * re-dispatched here; the original press already
                     * fired InitialPress/ShortRelease once. A held
                     * button simply doesn't repeat the Matter event,
                     * the same "one physical press, one event pair"
                     * choice firmware/rf433-bridge/'s own repeat-count
                     * de-duplication makes for a held RF remote button. */
                    state = State::kIdle;
                } else {
                    state = State::kIdle;
                }
            }
            break;
        case State::kReceivingBits:
            if (was_mark) {
                if (!ir_within_tolerance(duration_us, NEC_BIT_MARK_US)) {
                    state = State::kIdle; /* not a valid bit mark — resync */
                }
                /* Marks themselves don't encode data in NEC — only the
                 * following space does. Nothing else to do here. */
            } else {
                bool is_zero = ir_within_tolerance(duration_us, NEC_ZERO_SPACE_US);
                bool is_one = ir_within_tolerance(duration_us, NEC_ONE_SPACE_US);
                if (is_zero == is_one) {
                    state = State::kIdle; /* ambiguous or neither — resync */
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

                    if (ir_dispatch_known_code(address, command)) {
                        break;
                    }

                    if (learn_mode_active && esp_timer_get_time() < learn_mode_deadline_us) {
                        ir_learn_new_code(address, command);
                    } else {
                        ESP_LOGI(TAG, "Address 0x%02x / command 0x%02x not bridged and learn mode is off — ignoring",
                                 address, command);
                    }
                }
            }
            break;
        }
    }
}

/* --- Learn button: same shape firmware/rf433-bridge/'s own establishes. */
static void ir_learn_button_task(void *arg)
{
    bool debounced_pressed = false;
    int consistent_samples = 0;
    bool last_raw = (gpio_get_level(IR_LEARN_BUTTON_GPIO) == 0);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10));

        bool raw_pressed = (gpio_get_level(IR_LEARN_BUTTON_GPIO) == 0); /* active-LOW */
        if (raw_pressed == last_raw) {
            if (consistent_samples < IR_LEARN_BUTTON_DEBOUNCE_SAMPLES) {
                consistent_samples++;
            }
        } else {
            last_raw = raw_pressed;
            consistent_samples = 0;
        }

        if (consistent_samples == IR_LEARN_BUTTON_DEBOUNCE_SAMPLES && raw_pressed && !debounced_pressed) {
            debounced_pressed = true;
            learn_mode_active = true;
            learn_mode_deadline_us = esp_timer_get_time() + (int64_t)IR_LEARN_WINDOW_MS * 1000;
            ESP_LOGI(TAG, "Learn mode ON for %d seconds — press the remote button you want to bridge",
                     IR_LEARN_WINDOW_MS / 1000);
        } else if (consistent_samples == IR_LEARN_BUTTON_DEBOUNCE_SAMPLES && !raw_pressed && debounced_pressed) {
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
     * bridge's own learned IR codes. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    bool should_factory_reset = check_factory_reset_boot_count();

    /* 2. Configure the IR receiver input + its interrupt (ANYEDGE — both
     * edges are needed to measure every mark and space duration). No
     * internal pull-up needed — the receiver module actively drives its
     * own output (idle HIGH, inverted logic — see the header comment
     * above). */
    gpio_config_t rx_io_conf = {};
    rx_io_conf.pin_bit_mask = (1ULL << IR_RX_GPIO);
    rx_io_conf.mode = GPIO_MODE_INPUT;
    rx_io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&rx_io_conf);

    ir_edge_queue = xQueueCreate(256, sizeof(int64_t));
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    gpio_isr_handler_add(IR_RX_GPIO, ir_isr_handler, NULL);
    xTaskCreate(ir_decode_task, "ir_decode", 4096, NULL, 10, NULL);

    /* 2b. Configure the learn button — active-LOW, internal pull-up. */
    gpio_config_t learn_io_conf = {};
    learn_io_conf.pin_bit_mask = (1ULL << IR_LEARN_BUTTON_GPIO);
    learn_io_conf.mode = GPIO_MODE_INPUT;
    learn_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&learn_io_conf);
    xTaskCreate(ir_learn_button_task, "ir_learn_button", 3072, NULL, 5, NULL);

    /* 3. Build the Matter data model: one node, one Aggregator (bridge)
     * root endpoint — see the header comment above for the full bridge-
     * architecture detail. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Infrared Bridge",
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
    err = app_bridge_initialize(g_node, create_bridge_devices, create_ir_bridged_device, free_ir_bridged_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the bridge: %d", err);
        return;
    }

    /* If step 1's boot-count check detected 3 quick power cycles in a
     * row, factory-reset now that Matter has actually started — also
     * erases every learned IR code, not just the fabric credentials. */
    if (should_factory_reset) {
        ESP_LOGW(TAG, "Quick power cycle detected — factory resetting");
        esp_matter_bridge::factory_reset();
        esp_matter::factory_reset(); /* erases NVS + restarts the device */
        return;
    }

    ESP_LOGI(TAG, "Matter infrared bridge started. Scan the QR code to commission, then press the learn "
                  "button and press a remote button to bridge it.");
}
