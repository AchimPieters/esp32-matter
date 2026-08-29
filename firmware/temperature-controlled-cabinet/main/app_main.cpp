/*
 * Minimal Matter Temperature Controlled Cabinet — fifty-ninth device type,
 * and this repo's first STANDALONE use of a device type every other device
 * that touches it (firmware/refrigerator/'s own Fridge/Freezer, firmware/
 * oven/'s own single cavity) has only ever built as a *child* endpoint
 * under a bigger appliance. A real, honest product category on its own:
 * a single-zone wine cooler / beverage fridge / mini-fridge, the class of
 * appliance that genuinely IS just one temperature-controlled compartment
 * with nothing else attached — no separate freezer, no oven cavity, no
 * ice maker.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- Why this is genuinely standalone-buildable, not just a smaller copy
 * of firmware/refrigerator/'s own child endpoint --------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * TemperatureControlledCabinet.xml (0x0071, revision 6): its own
 * `<classification>` is `class="simple" scope="endpoint"` — NOT
 * `class="utility"`, the classification Matter's own spec uses for device
 * types that only exist as composed pieces of something else (confirmed
 * the same way for ElectricalSensor/PowerSource/DeviceEnergyManagement,
 * all genuinely `class="utility"`, all still composed-only in this repo).
 * A prior pass in this repo's own history called Temperature Controlled
 * Cabinet a "utility"-class type too, by extrapolating from Refrigerator's/
 * Oven's own use of it as a child rather than reading its own
 * classification tag directly — a real, now-corrected documentation
 * mistake, the same kind of "re-check the primary source directly rather
 * than trust an earlier pass's own conclusion" finding that already
 * unblocked firmware/battery-storage/ and firmware/oven/ earlier in this
 * same catalog sweep. Nothing in the XML requires composition under a
 * parent device type either (no `<conditionRequirements>` block at all,
 * unlike Refrigerator's own mandatory-child rule) — `endpoint::
 * temperature_controlled_cabinet::create()` is called here directly on
 * the node's own root/primary endpoint, exactly the same top-level helper
 * firmware/refrigerator/'s own Fridge/Freezer children already use, just
 * with no `set_parent_endpoint()` call afterward at all.
 *
 * This is the "Cooler" condition only — a real wine cooler/mini-fridge has
 * no heating functionality, so Oven Cavity Operational State and Oven Mode
 * (both gated behind the XML's own "Heater" condition) are correctly never
 * created; a "Heater"-only product using this same device type would be
 * functionally a single-cavity oven, which firmware/oven/ already covers.
 * Temperature Alarm is `provisionalConform` on this XML — skipped, same
 * "provisional, not yet real spec text" precedent firmware/thermostat/'s
 * own skipped Ambient Context Sensing already establishes.
 *
 * `temperature_controlled_cabinet::create()` confirmed to add ONLY
 * Descriptor (via `common::create<T>()`) + TemperatureControl[TN] —
 * confirmed by reading `esp_matter_endpoint.cpp`'s own `temperature_
 * controlled_cabinet::add()` directly, the exact same helper body
 * firmware/refrigerator/'s own header comment already documents in full,
 * including the same legacy-vs-generated `feature_flags`/`temp_setpoint`
 * pitfall (the "generated" data model this repo never enables sets the TN
 * feature flag automatically inside its own add(); the actual "legacy"
 * default this build compiles against does not, so it's set explicitly
 * here too). No Identify at all — confirmed the XML lists none, the same
 * "genuinely no Identify cluster on this device type" finding
 * firmware/refrigerator/'s own child endpoints already establish (their
 * own Identify LED lives on the ROOT Refrigerator endpoint instead,
 * something this device type has no parent to borrow one from) — so, per
 * the same device-type-conformance discipline this repo applies
 * throughout (adding a cluster the device type's own XML doesn't list
 * would make the endpoint non-conformant), this file has no Identify LED
 * of its own at all, the first device type in this repo without one.
 * RefrigeratorAndTemperatureControlledCabinetMode + TemperatureMeasurement
 * (both `<optionalConform/>` under the "Cooler" condition) are added
 * manually onto this same endpoint afterward, reusing firmware/
 * refrigerator/'s own `RefrigeratorCabinetModeDelegate` class and DS18B20
 * driver verbatim (a single instance each, parameterized the same way,
 * just never spawned a second time for a freezer compartment that doesn't
 * exist on this product).
 *
 * Hardware: one relay (active-LOW, matching this repo's own established
 * convention) + one DS18B20 probe — literally firmware/refrigerator/'s own
 * Fridge cabinet code, unchanged, just running as the device's only
 * endpoint instead of a child of a bigger Refrigerator root. Default
 * temperature range (0.00-20.00 degC, default target 8.00 degC) widened
 * from firmware/refrigerator/'s own narrower 1.00-10.00 degC fridge range
 * to also cover real wine-cooler use (commonly serving a wider 6-18 degC
 * band across red/white storage), still ordinary commercial-appliance
 * numbers, not researched against one specific product's own spec sheet.
 * `TEMPERATURE_CONTROLLED_CABINET_HYSTERESIS_CENTIDEGREES` (0.5 degC) and
 * the fail-safe-off-on-read-failure behavior both reuse firmware/
 * refrigerator/'s own reasoning verbatim.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/DS18B20 hardware for this device type
 * physically available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/mode-base-server/mode-base-server.h>
#include <app/clusters/temperature-control-server/TemperatureControlCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_tcc";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module" — polarity isn't universal. */
#define TCC_SENSOR_GPIO GPIO_NUM_21   /* DS18B20 */
#define TCC_RELAY_GPIO GPIO_NUM_16    /* active-LOW */

/* How often the DS18B20 is read and the hysteresis logic re-evaluated. */
#define TCC_MEASURE_INTERVAL_MS 10000

/* Bang-bang (hysteresis) control band, in centidegrees C — same value/
 * reasoning firmware/refrigerator/'s own control loop already uses. */
#define TCC_HYSTERESIS_CENTIDEGREES 50

/* How much colder the target gets while "Rapid Cool" mode is active — same
 * documented simplification firmware/refrigerator/'s own header comment
 * explains in full (no real staged-compressor logic behind it). */
#define TCC_RAPID_MODE_OFFSET_CENTIDEGREES (-300)

/* 0.00-20.00 degC, default target 8.00 degC — see the header comment above
 * for why this is wider than firmware/refrigerator/'s own fridge range. */
#define TCC_MIN_CENTIDEGREES 0
#define TCC_MAX_CENTIDEGREES 2000
#define TCC_DEFAULT_SETPOINT_CENTIDEGREES 800
#define TCC_MEASURE_MIN_CENTIDEGREES (-1000)
#define TCC_MEASURE_MAX_CENTIDEGREES 3000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t tcc_endpoint_id = 0;

/* --- DS18B20 driver — reused verbatim from firmware/refrigerator/'s own
 * (itself from firmware/water-heater/'s/firmware/thermostat/'s SENSOR_TYPE
 * library) — see those files' own header comments for the 1-Wire timing/
 * CRC detail and sourcing. */
static bool ow_reset(gpio_num_t pin)
{
    gpio_set_level(pin, 0);
    esp_rom_delay_us(480);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(pin) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(gpio_num_t pin, int bit)
{
    gpio_set_level(pin, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(pin, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(pin, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(gpio_num_t pin)
{
    gpio_set_level(pin, 0);
    esp_rom_delay_us(2);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(pin);
    esp_rom_delay_us(50);
    return bit;
}

static void ow_write_byte(gpio_num_t pin, uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(pin, byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(gpio_num_t pin)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte = (uint8_t)(byte | (ow_read_bit(pin) << i));
    }
    return byte;
}

/* Dallas/Maxim 1-Wire CRC-8 (reflected, polynomial 0x8C, init 0x00). */
static uint8_t onewire_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int b = 0; b < 8; b++) {
            uint8_t mix = (uint8_t)((crc ^ byte) & 0x01);
            crc >>= 1;
            if (mix) {
                crc ^= 0x8C;
            }
            byte >>= 1;
        }
    }
    return crc;
}

static bool sensor_setup(gpio_num_t pin)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(pin, 1);
    return true;
}

static bool sensor_read(gpio_num_t pin, float *temperature_c)
{
    portDISABLE_INTERRUPTS();
    bool present = ow_reset(pin);
    if (present) {
        ow_write_byte(pin, 0xCC); /* Skip ROM */
        ow_write_byte(pin, 0x44); /* Convert T */
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset — check wiring/pull-up");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(750)); /* max conversion time at default 12-bit resolution */

    portDISABLE_INTERRUPTS();
    present = ow_reset(pin);
    uint8_t scratchpad[9] = {0};
    if (present) {
        ow_write_byte(pin, 0xCC);
        ow_write_byte(pin, 0xBE); /* Read Scratchpad */
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = ow_read_byte(pin);
        }
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset (read phase)");
        return false;
    }

    if (onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGW(TAG, "DS18B20: CRC mismatch — discarding reading");
        return false;
    }

    int16_t raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    *temperature_c = raw * 0.0625f; /* 12-bit default resolution: 1 LSB = 1/16 degC */
    return true;
}

/* --- RefrigeratorAndTemperatureControlledCabinetMode delegate -------------
 * Reused verbatim from firmware/refrigerator/'s own `RefrigeratorCabinetModeDelegate`
 * (there parameterized by CabinetKind for Fridge vs. Freezer; this device
 * only ever has one cabinet, so the class is simplified to a single,
 * unparameterized "Rapid Cool" mode list). */
class CabinetModeDelegate : public ModeBase::Delegate
{
public:
    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        const char *text = (modeIndex == 0) ? "Normal" : "Rapid Cool";
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan(text, strlen(text)), label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = modeIndex; /* 0 = Normal, 1 = Rapid Cool */
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetModeTagsByIndex(uint8_t modeIndex, chip::app::DataModel::List<detail::Structs::ModeTagStruct::Type> &tags) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        if (tags.size() < 1) {
            return CHIP_ERROR_INVALID_ARGUMENT;
        }
        uint16_t tag_value = (modeIndex == 0)
            ? chip::to_underlying(RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kAuto)
            : chip::to_underlying(RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kRapidCool);
        tags[0] = {.value = tag_value};
        tags.reduce_size(1);
        return CHIP_NO_ERROR;
    }

    void HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        m_current_mode = newMode;
        ESP_LOGI(TAG, "Cabinet mode set to %u", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

    bool RapidActive() const { return m_current_mode == 1; }

private:
    static constexpr uint8_t kNumModes = 2;
    uint8_t m_current_mode = 0;
};

static CabinetModeDelegate mode_delegate;

/* --- Registry-lookup-and-cast helpers — same pattern this repo's other
 * code-driven-cluster access already uses. */
static TemperatureControlCluster *get_temperature_control_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, TemperatureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<TemperatureControlCluster *>(iface);
}

static TemperatureMeasurementCluster *get_temperature_measurement_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<TemperatureMeasurementCluster *>(iface);
}

static bool relay_on = false;

static void set_relay(bool on)
{
    relay_on = on;
    gpio_set_level(TCC_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static void control_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(TCC_MEASURE_INTERVAL_MS));

        float temperature_c;
        if (!sensor_read(TCC_SENSOR_GPIO, &temperature_c)) {
            /* No confident reading — fail safe and stop cooling, same
             * convention firmware/refrigerator/'s own control loop uses. */
            if (relay_on) {
                set_relay(false);
            }
            continue;
        }

        int16_t measured_centidegrees = (int16_t)(temperature_c * 100.0f);

        TemperatureMeasurementCluster *meas = get_temperature_measurement_cluster(tcc_endpoint_id);
        if (meas) {
            meas->SetMeasuredValue(chip::app::DataModel::Nullable<int16_t>(measured_centidegrees));
        }
        ESP_LOGI(TAG, "Cabinet: %.2f degC", temperature_c);

        TemperatureControlCluster *ctrl = get_temperature_control_cluster(tcc_endpoint_id);
        int16_t target_centidegrees = ctrl ? ctrl->GetTemperatureSetpoint() : 0;
        if (mode_delegate.RapidActive()) {
            target_centidegrees += TCC_RAPID_MODE_OFFSET_CENTIDEGREES;
        }

        if (measured_centidegrees >= target_centidegrees + TCC_HYSTERESIS_CENTIDEGREES) {
            if (!relay_on) {
                set_relay(true);
            }
        } else if (measured_centidegrees <= target_centidegrees - TCC_HYSTERESIS_CENTIDEGREES) {
            if (relay_on) {
                set_relay(false);
            }
        }
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

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

/* This device type has no Identify cluster at all (confirmed by reading
 * its own XML directly — see the header comment above) — kept as a
 * trivial stub, never actually invoked. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
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

    /* 2. Configure the relay output — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << TCC_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(TCC_RELAY_GPIO, 1); /* active-LOW: 1 = off */

    /* 2b. Configure the DS18B20 sensor pin. */
    sensor_setup(TCC_SENSOR_GPIO);

    /* 3. Build the Matter data model: one node, one Temperature Controlled
     * Cabinet endpoint, standing on its own — Descriptor + TemperatureControl
     * via the top-level helper, plus RefrigeratorAndTemperatureControlled
     * CabinetMode + TemperatureMeasurement added manually — see the header
     * comment above for the full detail, including why this endpoint gets
     * no Identify cluster at all. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Temperature Controlled Cabinet",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    /* Same legacy-vs-generated `feature_flags`/`temp_setpoint` note as
     * firmware/refrigerator/'s own header comment already documents in
     * full — the legacy add() this build compiles against doesn't set the
     * TN feature flag automatically, so it's set explicitly here. */
    endpoint::temperature_controlled_cabinet::config_t tcc_config;
    tcc_config.temperature_control.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    tcc_config.temperature_control.features.temperature_number.temp_setpoint = TCC_DEFAULT_SETPOINT_CENTIDEGREES;
    tcc_config.temperature_control.features.temperature_number.min_temperature = TCC_MIN_CENTIDEGREES;
    tcc_config.temperature_control.features.temperature_number.max_temperature = TCC_MAX_CENTIDEGREES;

    endpoint_t *tcc_endpoint = endpoint::temperature_controlled_cabinet::create(node, &tcc_config, ENDPOINT_FLAG_NONE, NULL);
    if (!tcc_endpoint) {
        ESP_LOGE(TAG, "Failed to create temperature controlled cabinet endpoint");
        return;
    }
    tcc_endpoint_id = endpoint::get_id(tcc_endpoint);
    ESP_LOGI(TAG, "Temperature controlled cabinet endpoint id: %u", tcc_endpoint_id);

    cluster::refrigerator_and_tcc_mode::config_t mode_config;
    mode_config.current_mode = 0;
    mode_config.delegate = &mode_delegate;
    cluster::refrigerator_and_tcc_mode::create(tcc_endpoint, &mode_config, CLUSTER_FLAG_SERVER);

    cluster::temperature_measurement::config_t meas_config;
    meas_config.measured_value = nullable<int16_t>();
    meas_config.min_measured_value = nullable<int16_t>(TCC_MEASURE_MIN_CENTIDEGREES);
    meas_config.max_measured_value = nullable<int16_t>(TCC_MEASURE_MAX_CENTIDEGREES);
    cluster::temperature_measurement::create(tcc_endpoint, &meas_config, CLUSTER_FLAG_SERVER);

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

    /* 5. Start the control task — reads the DS18B20, runs its hysteresis
     * logic, and drives the relay for as long as the device runs. */
    xTaskCreate(control_task, "tcc_control_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter temperature controlled cabinet started. Scan the QR code to commission.");
}
