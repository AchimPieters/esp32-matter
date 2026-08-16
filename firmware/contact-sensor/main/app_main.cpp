/*
 * Minimal Matter Contact Sensor.
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
 * What this device does: it's a Matter contact sensor endpoint (esp-matter's
 * contact_sensor device type), reporting a digital contact's open/closed
 * state — e.g. a magnetic reed switch on a door or window — via the Boolean
 * State cluster's StateValue attribute (server-side, read-only from a
 * controller's point of view; this firmware is the only thing that ever
 * writes it). StateValue = true means the contact is closed (e.g. door/
 * window shut, magnet present); false means open.
 *
 * Wiring assumption: GND -> reed switch -> GPIO, same active-low idiom as
 * firmware/switch's button (internal pull-up keeps the pin HIGH/open until
 * the magnet closes the loop and pulls it LOW/closed). Unlike the switch's
 * button, this device reacts to BOTH edges — a contact sensor has to track
 * live physical state, not just count discrete presses.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/boolean-state-server/BooleanStateCluster.h>

static const char *TAG = "matter_contact";

/* Change this to the GPIO your contact (reed switch) is wired to. Reference
 * wiring: GND -> reed switch -> GPIO (no external resistor needed — the
 * internal pull-up below keeps the pin HIGH/open until the switch pulls it
 * to GND/closed). GPIO 4 is a plain, unreserved GPIO on classic ESP32
 * (WROOM-32) — deliberately NOT the onboard BOOT/PROG button (GPIO 0): that
 * pin is also used for boot-mode selection and shares a lot of traffic
 * during reset, and turned out to be an unreliable choice for an external
 * input on the board this was tested against (see CLAUDE.md's open next
 * steps — same finding as firmware/switch's button). Adjust to match your
 * board if you wire it elsewhere. */
#define CONTACT_GPIO GPIO_NUM_4

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Optional RGB status LED — off by default (GPIO_NUM_NC). See
 * firmware/light/main/app_main.cpp's header comment for the full state
 * list and its exact sourcing (connectedhomeip's own lifecycle events +
 * the Identify cluster's own EffectIdentifierEnum), not repeated here. */
#define STATUS_LED_RED_GPIO GPIO_NUM_NC
#define STATUS_LED_GREEN_GPIO GPIO_NUM_NC
#define STATUS_LED_BLUE_GPIO GPIO_NUM_NC
#define STATUS_LED_LEDC_TIMER LEDC_TIMER_1
#define STATUS_LED_LEDC_RED_CHANNEL LEDC_CHANNEL_5
#define STATUS_LED_LEDC_GREEN_CHANNEL LEDC_CHANNEL_6
#define STATUS_LED_LEDC_BLUE_CHANNEL LEDC_CHANNEL_7
#define STATUS_LED_LEDC_MODE LEDC_LOW_SPEED_MODE
#define STATUS_LED_LEDC_DUTY_RES LEDC_TIMER_8_BIT
#define STATUS_LED_LEDC_FREQUENCY_HZ 5000
#define STATUS_LED_TICK_MS 20
#define STATUS_LED_PI 3.14159265f

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t contact_endpoint_id = 0;
static QueueHandle_t contact_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Current confirmed contact state — true = closed. Mirrors the Boolean
 * State cluster's StateValue attribute; kept locally too so we only push an
 * attribute update on an actual change, not on every debounced re-read of
 * the same level. */
static bool contact_closed = false;

/* --- Status LED pattern engine (see the header comment above) --------- */
typedef enum {
    STATUS_LED_PATTERN_OFF,
    STATUS_LED_PATTERN_SOLID,
    STATUS_LED_PATTERN_BLINK,
    STATUS_LED_PATTERN_BREATHE,
} status_led_pattern_t;

static bool status_led_enabled = false;
static esp_timer_handle_t status_led_timer = NULL;
static uint8_t status_led_r = 0, status_led_g = 0, status_led_b = 0;
static status_led_pattern_t status_led_pattern = STATUS_LED_PATTERN_OFF;
static uint32_t status_led_period_ms = 1000;
static uint32_t status_led_elapsed_ms = 0;
static uint32_t status_led_duration_ms = 0;

static void status_led_set(uint8_t r, uint8_t g, uint8_t b, status_led_pattern_t pattern,
                            uint32_t period_ms, uint32_t duration_ms)
{
    if (!status_led_enabled) {
        return;
    }
    status_led_r = r;
    status_led_g = g;
    status_led_b = b;
    status_led_pattern = pattern;
    status_led_period_ms = period_ms > 0 ? period_ms : 1000;
    status_led_duration_ms = duration_ms;
    status_led_elapsed_ms = 0;
}

static void status_led_off(void)
{
    status_led_set(0, 0, 0, STATUS_LED_PATTERN_OFF, 1000, 0);
}

static void status_led_tick_cb(void *arg)
{
    if (!status_led_enabled) {
        return;
    }
    status_led_elapsed_ms += STATUS_LED_TICK_MS;
    if (status_led_duration_ms > 0 && status_led_elapsed_ms >= status_led_duration_ms) {
        status_led_off();
    }

    float phase = fmodf((float)status_led_elapsed_ms, (float)status_led_period_ms) / (float)status_led_period_ms;
    float brightness;
    switch (status_led_pattern) {
    case STATUS_LED_PATTERN_SOLID:
        brightness = 1.0f;
        break;
    case STATUS_LED_PATTERN_BLINK:
        brightness = (phase < 0.5f) ? 1.0f : 0.0f;
        break;
    case STATUS_LED_PATTERN_BREATHE:
        brightness = (sinf(phase * 2.0f * STATUS_LED_PI - STATUS_LED_PI / 2.0f) + 1.0f) / 2.0f;
        break;
    case STATUS_LED_PATTERN_OFF:
    default:
        brightness = 0.0f;
        break;
    }

    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL, (uint32_t)(status_led_r * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL, (uint32_t)(status_led_g * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL, (uint32_t)(status_led_b * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL);
}

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR contact_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(contact_evt_queue, &gpio_num, NULL);
}

/* esp-matter's generic attribute::update() can't write BooleanState's
 * StateValue in this SDK version: unlike simple clusters (e.g. OnOff),
 * BooleanState is implemented via the newer "code-driven" cluster class
 * (BooleanStateCluster) rather than the generic ember-style attribute
 * store, so the generic update path returns ESP_ERR_NOT_SUPPORTED —
 * confirmed by reading esp_matter_data_model.cpp's set_val(), which has a
 * literal TODO for exactly this case ("we could use the cluster-specific
 * setter API to update the value"). This looks the cluster instance up
 * directly via the data model provider's registry and calls its
 * SetStateValue(), which is the actually-supported way to update this
 * particular cluster's state from app code on this SDK version. It also
 * takes care of generating the StateChange event, so we don't have to. */
static void update_contact_state(uint16_t endpoint_id, bool closed)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, BooleanState::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "BooleanState cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::BooleanStateCluster *>(iface);
    cluster->SetStateValue(closed);
}

/* Debounces the contact and, on an actual state change, updates the local
 * BooleanState attribute. Unlike firmware/switch's button (which only cares
 * about discrete presses and always acts on a confirmed press), this reacts
 * to both directions and only acts when the debounced level differs from
 * what we already reported — a reed switch can bounce on both closing and
 * opening, and either edge is a real event worth debouncing the same way. */
static void contact_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(contact_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

        /* Debounce: require the pin to read a consistent level for ~40ms
         * (8 x 5ms samples) before treating it as a real, settled state —
         * same reasoning as firmware/switch's button debounce (a lone
         * sample can land mid-bounce and misread the transition). Here we
         * don't know in advance which level counts as "confirmed" (unlike
         * the switch, which only cares about the low/pressed level) — any
         * consistent level, high or low, is a valid settled state. */
        int first_level = gpio_get_level((gpio_num_t)io_num);
        bool consistent = true;
        char samples[9] = {0};
        samples[0] = first_level ? 'H' : 'L';
        for (int i = 1; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            int level = gpio_get_level((gpio_num_t)io_num);
            samples[i] = level ? 'H' : 'L';
            if (level != first_level) {
                consistent = false;
            }
        }
        ESP_LOGI(TAG, "Samples (5ms apart): %s (%s)", samples, consistent ? "stable" : "mixed/bouncing");

        /* Whether confirmed or not, flush any further queued edges from
         * this same burst — same reasoning as firmware/switch: a single
         * physical transition can trigger several interrupts as the
         * contact bounces, and processing each one independently just
         * repeats the same debounce cycle for no benefit. */
        xQueueReset(contact_evt_queue);

        if (!consistent) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously stable");
            continue;
        }

        bool new_closed = (first_level == 0);
        if (new_closed == contact_closed) {
            ESP_LOGI(TAG, "Debounced level matches current state (%s) — no change",
                     contact_closed ? "CLOSED" : "OPEN");
            continue;
        }

        contact_closed = new_closed;
        ESP_LOGI(TAG, "Contact now %s", contact_closed ? "CLOSED" : "OPEN");

        update_contact_state(contact_endpoint_id, contact_closed);
    }
}

/* Lifecycle events from the Matter stack (commissioning, connectivity, ...). */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
        if (event->CHIPoBLEAdvertisingChange.Result == chip::DeviceLayer::kActivity_Started) {
            status_led_set(0, 255, 0, STATUS_LED_PATTERN_BREATHE, 2000, 0); /* Setup mode */
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kSecureSessionEstablished:
        status_led_set(255, 255, 0, STATUS_LED_PATTERN_BREATHE, 1000, 0); /* Setup started */
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete — device is now paired");
        status_led_off(); /* Setup complete */
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        status_led_set(255, 0, 0, STATUS_LED_PATTERN_SOLID, 1000, 0); /* Setup failed */
        break;
    default:
        break;
    }
}

/* Called whenever a controller changes an attribute on this device. A
 * contact sensor has nothing to react to here — StateValue is read-only and
 * only ever written locally by contact_task() above — so this is a no-op
 * required by node::create()'s callback signature. */
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
        status_led_set(255, 0, 0, STATUS_LED_PATTERN_BLINK, 1000, 0);
        break;
    case identification::STOP:
        ESP_LOGI(TAG, "Identify stopped on endpoint %u", endpoint_id);
        esp_timer_stop(identify_led_timer);
        gpio_set_level(IDENTIFY_LED_GPIO, 0);
        status_led_off();
        break;
    case identification::EFFECT:
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u",
                 effect_id, effect_variant, endpoint_id);
        if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBlink)) {
            status_led_set(255, 255, 255, STATUS_LED_PATTERN_BLINK, 1000, 1000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBreathe)) {
            status_led_set(255, 255, 255, STATUS_LED_PATTERN_BREATHE, 1000, 15000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kOkay)) {
            status_led_set(0, 255, 0, STATUS_LED_PATTERN_BLINK, 2000, 0);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kChannelChange)) {
            status_led_set(255, 255, 0, STATUS_LED_PATTERN_BLINK, 16000, 0);
        } else {
            status_led_off();
        }
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

    /* 2. Configure the contact input + its interrupt. ANYEDGE, not NEGEDGE
     * like the switch's button — we need to know about both opening and
     * closing, not just one discrete action. */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << CONTACT_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&io_conf);

    /* Settle briefly, then take the pin's boot-time level as the initial
     * state — a contact sensor should report reality from the first
     * commissioned read, not an arbitrary hardcoded default. */
    vTaskDelay(pdMS_TO_TICKS(50));
    contact_closed = (gpio_get_level(CONTACT_GPIO) == 0);
    ESP_LOGI(TAG, "Contact GPIO %d initial level: %d — starting as %s",
             CONTACT_GPIO, gpio_get_level(CONTACT_GPIO), contact_closed ? "CLOSED" : "OPEN");

    contact_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(contact_task, "contact_task", 4096, NULL, 10, NULL);

    /* These two silently doing nothing was a real bug in firmware/switch:
     * unchecked, a failure means no interrupt is ever attached and every
     * contact change produces zero log output, which looks identical to
     * "nothing is wired up" from the outside. */
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(CONTACT_GPIO, contact_isr_handler, (void *)(uintptr_t)CONTACT_GPIO);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
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

    /* 2c. Configure the optional RGB status LED + its pattern-engine timer
     * — only if at least one of its 3 GPIOs is actually wired up. */
    status_led_enabled = (STATUS_LED_RED_GPIO != GPIO_NUM_NC) ||
                          (STATUS_LED_GREEN_GPIO != GPIO_NUM_NC) ||
                          (STATUS_LED_BLUE_GPIO != GPIO_NUM_NC);
    if (status_led_enabled) {
        ledc_timer_config_t status_led_ledc_timer = {};
        status_led_ledc_timer.speed_mode = STATUS_LED_LEDC_MODE;
        status_led_ledc_timer.duty_resolution = STATUS_LED_LEDC_DUTY_RES;
        status_led_ledc_timer.timer_num = STATUS_LED_LEDC_TIMER;
        status_led_ledc_timer.freq_hz = STATUS_LED_LEDC_FREQUENCY_HZ;
        status_led_ledc_timer.clk_cfg = LEDC_AUTO_CLK;
        ledc_timer_config(&status_led_ledc_timer);

        struct {
            ledc_channel_t channel;
            gpio_num_t gpio;
        } status_led_channels[] = {
            { STATUS_LED_LEDC_RED_CHANNEL, STATUS_LED_RED_GPIO },
            { STATUS_LED_LEDC_GREEN_CHANNEL, STATUS_LED_GREEN_GPIO },
            { STATUS_LED_LEDC_BLUE_CHANNEL, STATUS_LED_BLUE_GPIO },
        };
        for (size_t i = 0; i < sizeof(status_led_channels) / sizeof(status_led_channels[0]); i++) {
            if (status_led_channels[i].gpio == GPIO_NUM_NC) {
                continue;
            }
            ledc_channel_config_t status_led_channel = {};
            status_led_channel.gpio_num = status_led_channels[i].gpio;
            status_led_channel.speed_mode = STATUS_LED_LEDC_MODE;
            status_led_channel.channel = status_led_channels[i].channel;
            status_led_channel.intr_type = LEDC_INTR_DISABLE;
            status_led_channel.timer_sel = STATUS_LED_LEDC_TIMER;
            status_led_channel.duty = 0;
            status_led_channel.hpoint = 0;
            ledc_channel_config(&status_led_channel);
        }

        const esp_timer_create_args_t status_led_timer_args = {
            .callback = &status_led_tick_cb,
            .name = "status_led",
        };
        esp_timer_create(&status_led_timer_args, &status_led_timer);
        esp_timer_start_periodic(status_led_timer, STATUS_LED_TICK_MS * 1000);
    }

    /* 3. Build the Matter data model: one node, one Contact Sensor endpoint,
     * seeded with the boot-time reading above so its first reported state
     * matches physical reality. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    contact_sensor::config_t contact_config;
    contact_config.boolean_state.state_value = contact_closed;
    endpoint_t *endpoint = contact_sensor::create(node, &contact_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create contact sensor endpoint");
        return;
    }

    contact_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Contact sensor endpoint id: %u", contact_endpoint_id);

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

    ESP_LOGI(TAG, "Matter contact sensor started. Scan the QR code to commission.");
}
