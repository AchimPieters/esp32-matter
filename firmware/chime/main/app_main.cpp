/*
 * Minimal Matter Chime — forty-fourth device type, and the receiving half
 * of the Doorbell + Chime pair firmware/doorbell/ starts: that file's
 * pushbutton sends a real Chime::PlayChimeSound command (via a controller-
 * configured Binding-cluster entry) to whatever real Chime device it's
 * bound to — this file is that device, driving an actual buzzer/speaker.
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
 * Chime.xml (device type 0x0146, revision 1): Identify (server) is only
 * `<optionalConform/>`, Chime (SERVER side) is the only `<mandatoryConform/>`
 * cluster — the whole device type is genuinely just those two. Same as
 * firmware/doorbell/'s own Doorbell endpoint, confirmed there is no
 * `endpoint::chime::create()` TOP-LEVEL helper anywhere in esp-matter's
 * legacy data model (only in the NOT-enabled-here "generated" one, under
 * `data_model/generated/device_types/chime_device/` — see firmware/
 * doorbell/'s own header comment for why that one is deliberately not
 * used), so this endpoint is hand-assembled from lower-level free
 * functions too, including the same `cluster::descriptor::create()`-
 * first discipline firmware/color-light/'s and firmware/addressable-light/'s
 * own header comments document the real bug class for skipping.
 *
 * --- The Chime cluster itself: an ordinary config->delegate cluster,
 * NOT the ember-shell-less special case an earlier draft concluded -------
 * An earlier draft of this file concluded, from searching the wrong
 * header directory (top-level `components/esp_matter/` headers instead of
 * `components/esp_matter/data_model/legacy/`), that no `cluster::
 * chime::create()`
 * ember-shell helper existed at all — and, on that wrong premise, hand-
 * built the raw ember shell via `esp_matter::cluster::create(endpoint,
 * Chime::Id, CLUSTER_FLAG_SERVER)` plus a manually-constructed
 * `chip::app::Clusters::Chime::ChimeServer` + `.Init()` call after
 * `esp_matter::start()`. That code WORKED (it produces the exact same
 * `ChimeServer` construction the real mechanism below does internally,
 * so there was no double-registration bug) but wasn't the SDK's actual,
 * intended path — found and corrected once esp-matter's own
 * `doorbell::add()` (in `esp_matter_endpoint.cpp`) was read directly for
 * an unrelated reason and turned out to call
 * `cluster::chime::create(endpoint, NULL, CLUSTER_FLAG_CLIENT)` itself,
 * immediately proving a real helper does exist. Reading
 * `cluster::chime::create()`'s own body directly (in `data_model/legacy/
 * esp_matter_cluster.cpp`) confirms Chime is actually an entirely
 * ORDINARY `config->delegate` cluster, the same well-worn pattern
 * firmware/water-heater/'s WaterHeaterMode and firmware/robot-vacuum/'s
 * RvcRunMode already establish: `chime::config_t` is just `{ void
 * *delegate; }`; when `CLUSTER_FLAG_SERVER` is set, `create()` requires a
 * non-null `config->delegate` and wires it up via
 * `set_delegate_and_init_callback(cluster, ChimeDelegateInitCB,
 * config->delegate)` — `ChimeDelegateInitCB` (in
 * `esp_matter_delegate_callbacks.cpp`) is what lazily does
 * `new Chime::ChimeServer(endpoint_id, *delegate)` + `.Init()`,
 * automatically, during `esp_matter::start()`'s own init-callback pass —
 * no manual post-`start()` construction needed at all, unlike this file's
 * own earlier (working, but non-idiomatic) draft. This file now simply
 * sets `chime_config.delegate = &chime_delegate;` before calling
 * `cluster::chime::create(endpoint, &chime_config, CLUSTER_FLAG_SERVER)`
 * — no raw ember-shell call, no manual `ChimeServer`/`.Init()` code, no
 * ordering awareness needed in `app_main()` at all. Worth remembering
 * for any future less-common cluster search in this repo: a missing
 * helper should be confirmed by grepping the SAME `data_model/legacy/`
 * location every other per-cluster helper in this repo already lives in,
 * not a plausible-looking top-level header path that simply doesn't
 * contain it.
 *
 * `ChimeDelegate` (declared directly under `chip::app::Clusters`, NOT
 * nested inside the `Chime::` sub-namespace the way `ChimeServer` itself
 * is — confirmed by reading `ChimeCluster.h`'s own namespace block
 * directly) has exactly three pure-virtual methods: `GetChimeSoundByIndex()`/
 * `GetChimeIDByIndex()` (together populate the mandatory
 * `InstalledChimeSounds` list attribute — confirmed the cluster itself
 * validates a `PlayChimeSound` command's ChimeID against this list via its
 * own `IsSupportedChimeID()` BEFORE ever calling this delegate's own
 * `PlayChimeSound()`, so by the time this file's own `PlayChimeSound()`
 * runs, the ID is already known-good) and `PlayChimeSound(chimeID)` itself
 * (this file's real command handler — the cluster also generates the
 * mandatory `ChimeStartedPlaying` event on a successful return, confirmed
 * by reading `ChimeCluster.cpp`'s own `HandlePlayChimeSound()`, so no
 * manual event-firing code is needed here either).
 *
 * --- Two real, distinct chime sounds, not one dummy entry ---------------
 * `InstalledChimeSounds` requires at least 1 entry (confirmed in the
 * cluster's own XML: `<countBetween><from value="1"/>`) — this file offers
 * two, genuinely different, so `SelectedChime`/`PlayChimeSound(ChimeID)`
 * both have real, distinguishable behavior to select between rather than
 * a single dummy option: ChimeID 0 "Ding-Dong" (a classic descending
 * two-tone doorbell pattern) and ChimeID 1 "Chirp" (a short double-beep).
 * Both are plain, arbitrary tone patterns picked for this file — NOT
 * sourced from, or tuned to match, any real commercial doorbell chime's
 * actual sound, since there is no datasheet or reference recording to
 * verify a "correct" tone against the way e.g. a sensor's protocol can be
 * checked against its own manufacturer datasheet. Driven as real audio-
 * frequency PWM via ESP-IDF's `driver/ledc.h` — the same LEDC peripheral
 * firmware/dimmable-light/'s and firmware/fan/'s own outputs already use,
 * just with the frequency changed per note (via `ledc_set_freq()`) instead
 * of held fixed, into a passive piezo buzzer (which needs an actual
 * driven waveform at the desired pitch, unlike a simple active buzzer
 * module that only needs a plain on/off GPIO and always sounds at its own
 * fixed internal tone). `PlayChimeSound()` itself only queues the
 * requested ChimeID (via a small FreeRTOS queue) and returns immediately
 * — the actual multi-hundred-millisecond tone sequence runs from a
 * separate `chime_task`, since blocking inside a Matter command handler
 * for that long would be poor practice (this callback very likely runs on
 * the Matter event loop's own thread).
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no passive piezo buzzer/speaker hardware for this
 * device type physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/chime-server/ChimeCluster.h>

static const char *TAG = "matter_chime";

/* Passive piezo buzzer / small speaker — driven as real audio-frequency
 * PWM via LEDC, not a plain digital on/off (see the header comment above
 * for why). */
#define CHIME_BUZZER_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* LEDC (hardware PWM) settings for the buzzer — one timer + one channel,
 * same portability-first choices (LEDC_LOW_SPEED_MODE, LEDC_AUTO_CLK)
 * firmware/dimmable-light/'s and firmware/fan/'s own LEDC outputs already
 * use. 10-bit resolution (vs. those files' 8-bit) gives finer duty control
 * across the wider audio-frequency range a tone sequence sweeps through;
 * the actual frequency is changed per note via ledc_set_freq(), unlike a
 * fixed-frequency brightness/speed PWM. */
#define CHIME_LEDC_TIMER LEDC_TIMER_0
#define CHIME_LEDC_CHANNEL LEDC_CHANNEL_0
#define CHIME_LEDC_MODE LEDC_LOW_SPEED_MODE
#define CHIME_LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define CHIME_LEDC_DUTY_ON ((1 << 10) / 2) /* ~50% duty = an audible square wave */
#define CHIME_LEDC_INITIAL_FREQUENCY_HZ 880

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using namespace chip::literals;
using Status = chip::Protocols::InteractionModel::Status;

static uint16_t chime_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static QueueHandle_t chime_play_queue = NULL;

/* One note: a frequency held for a duration, then silence for a gap
 * before the next note (or the end of the sequence). */
struct chime_note_t {
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint16_t gap_ms;
};

struct chime_sound_t {
    uint8_t id;
    const char *name;
    const chime_note_t *notes;
    size_t note_count;
};

/* ChimeID 0 — a classic descending two-tone doorbell pattern. */
static const chime_note_t kDingDongNotes[] = {
    {880, 300, 50},
    {659, 500, 0},
};

/* ChimeID 1 — a short double-beep. */
static const chime_note_t kChirpNotes[] = {
    {1500, 100, 80},
    {1500, 100, 0},
};

static const chime_sound_t kChimeSounds[] = {
    {0, "Ding-Dong", kDingDongNotes, sizeof(kDingDongNotes) / sizeof(kDingDongNotes[0])},
    {1, "Chirp", kChirpNotes, sizeof(kChirpNotes) / sizeof(kChirpNotes[0])},
};
static constexpr size_t kNumChimeSounds = sizeof(kChimeSounds) / sizeof(kChimeSounds[0]);

static void buzzer_silence(void)
{
    ledc_set_duty(CHIME_LEDC_MODE, CHIME_LEDC_CHANNEL, 0);
    ledc_update_duty(CHIME_LEDC_MODE, CHIME_LEDC_CHANNEL);
}

static void buzzer_tone(uint16_t frequency_hz)
{
    ledc_set_freq(CHIME_LEDC_MODE, CHIME_LEDC_TIMER, frequency_hz);
    ledc_set_duty(CHIME_LEDC_MODE, CHIME_LEDC_CHANNEL, CHIME_LEDC_DUTY_ON);
    ledc_update_duty(CHIME_LEDC_MODE, CHIME_LEDC_CHANNEL);
}

/* Plays one sound's whole note sequence to completion — blocking, which is
 * fine here since this only ever runs from chime_task's own dedicated
 * task, never from the Matter command-handling path itself (see the
 * header comment above for why PlayChimeSound() only queues the request
 * instead of calling this directly). */
static void play_chime_sound(const chime_sound_t *sound)
{
    for (size_t i = 0; i < sound->note_count; i++) {
        buzzer_tone(sound->notes[i].frequency_hz);
        vTaskDelay(pdMS_TO_TICKS(sound->notes[i].duration_ms));
        buzzer_silence();
        if (sound->notes[i].gap_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(sound->notes[i].gap_ms));
        }
    }
}

static void chime_task(void *arg)
{
    uint8_t chime_id;
    while (true) {
        if (xQueueReceive(chime_play_queue, &chime_id, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const chime_sound_t *sound = nullptr;
        for (size_t i = 0; i < kNumChimeSounds; i++) {
            if (kChimeSounds[i].id == chime_id) {
                sound = &kChimeSounds[i];
                break;
            }
        }
        if (!sound) {
            /* Shouldn't happen — the cluster validates ChimeID against
             * our own GetChimeIDByIndex() list before ever calling
             * PlayChimeSound() below, see the header comment above. */
            ESP_LOGW(TAG, "Queued ChimeID %u not found in kChimeSounds — ignoring", chime_id);
            continue;
        }
        ESP_LOGI(TAG, "Playing chime sound %u (%s)", sound->id, sound->name);
        play_chime_sound(sound);
    }
}

/* --- Chime delegate -------------------------------------------------------
 * See the header comment above for the full detail on this cluster's own
 * genuinely new integration shape and why each method below does what it
 * does. */
class DeviceChimeDelegate : public ChimeDelegate
{
public:
    CHIP_ERROR GetChimeSoundByIndex(uint8_t chimeIndex, uint8_t &chimeID, chip::MutableCharSpan &name) override
    {
        if (chimeIndex >= kNumChimeSounds) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = kChimeSounds[chimeIndex].id;
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan::fromCharString(kChimeSounds[chimeIndex].name), name);
    }

    CHIP_ERROR GetChimeIDByIndex(uint8_t chimeIndex, uint8_t &chimeID) override
    {
        if (chimeIndex >= kNumChimeSounds) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        chimeID = kChimeSounds[chimeIndex].id;
        return CHIP_NO_ERROR;
    }

    Status PlayChimeSound(uint8_t chimeID) override
    {
        if (xQueueSend(chime_play_queue, &chimeID, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Chime play queue full — dropping PlayChimeSound(%u)", chimeID);
        }
        return Status::Success;
    }
};

static DeviceChimeDelegate chime_delegate;

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

/* Nothing on this device needs to react to a plain-ember attribute write —
 * the Chime cluster's own SelectedChime/Enabled attributes are handled
 * entirely inside ChimeCluster itself — kept as a trivial stub, same as
 * several other device types in this repo. */
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

    /* 2. Configure the buzzer PWM output via LEDC (hardware PWM) — see the
     * header comment above for why the frequency (not the duty cycle)
     * carries the actual tone. Starts silent (duty 0). */
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = CHIME_LEDC_MODE;
    ledc_timer.duty_resolution = CHIME_LEDC_DUTY_RES;
    ledc_timer.timer_num = CHIME_LEDC_TIMER;
    ledc_timer.freq_hz = CHIME_LEDC_INITIAL_FREQUENCY_HZ;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = CHIME_BUZZER_GPIO;
    ledc_channel.speed_mode = CHIME_LEDC_MODE;
    ledc_channel.channel = CHIME_LEDC_CHANNEL;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.timer_sel = CHIME_LEDC_TIMER;
    ledc_channel.duty = 0;
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);

    chime_play_queue = xQueueCreate(4, sizeof(uint8_t));

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

    /* 3. Build the Matter data model: one node, one hand-assembled Chime
     * endpoint — see the header comment above for why no top-level helper
     * exists and what each manual step below is for. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Chime",
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

    /* Descriptor cluster FIRST — see the header comment above (and
     * firmware/doorbell/'s own copy of the same lesson) for the real bug
     * class this avoids. */
    cluster::descriptor::config_t descriptor_config;
    cluster_t *descriptor_cluster = cluster::descriptor::create(endpoint, &descriptor_config, CLUSTER_FLAG_SERVER);
    if (!descriptor_cluster) {
        ESP_LOGE(TAG, "Failed to create descriptor cluster");
        return;
    }

    add_device_type(endpoint, 0x0146 /* Chime */, 1 /* device type revision */);

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kVisibleIndicator);
    cluster_t *identify_cluster = cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);
    cluster::identify::command::create_trigger_effect(identify_cluster);

    /* Chime — an ordinary config->delegate cluster (see the header comment
     * above for the corrected understanding of this cluster's own real
     * create() helper): esp-matter's own `ChimeDelegateInitCB` lazily
     * constructs the real `Chime::ChimeServer` and calls its `.Init()`
     * automatically, during `esp_matter::start()`'s own init-callback pass
     * below — no manual construction of any kind needed in this file. */
    cluster::chime::config_t chime_config;
    chime_config.delegate = &chime_delegate;
    cluster_t *chime_cluster = cluster::chime::create(endpoint, &chime_config, CLUSTER_FLAG_SERVER);
    if (!chime_cluster) {
        ESP_LOGE(TAG, "Failed to create chime cluster");
        return;
    }

    chime_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Chime endpoint id: %u", chime_endpoint_id);

    xTaskCreate(chime_task, "chime_player", 4096, NULL, 5, NULL);

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

    ESP_LOGI(TAG, "Matter chime started. Scan the QR code to commission.");
}
