/*
 * Minimal Matter Temperature + Humidity Sensor (Sensirion SHT3x, I2C).
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
 * Why I2C, not the ESP32's internal temperature sensor: classic ESP32
 * (WROOM-32, this repo's default target) has no internal temperature
 * sensor peripheral exposed by ESP-IDF at all — that arrived with later
 * chips (S2, S3, C3, C6, ...). An external sensor is required either way,
 * so this uses a real one (SHT3x, I2C) rather than a fake/simulated
 * reading.
 *
 * Why two endpoints, not one: Matter has no single device type combining
 * temperature and humidity from one physical sensor chip — `temperature_
 * sensor` (Temperature Measurement cluster) and `humidity_sensor`
 * (Relative Humidity Measurement cluster) are separate device types, each
 * with exactly one cluster. A composed device exposing two endpoints from
 * one sensor is the spec-correct way to report both; this is this repo's
 * first multi-endpoint device.
 *
 * Like firmware/contact-sensor/'s Boolean State cluster, both
 * TemperatureMeasurement and RelativeHumidityMeasurement are implemented
 * via the newer "code-driven" cluster classes in this SDK version, not
 * the generic ember attribute store — esp-matter's generic
 * attribute::update() can't write them (confirmed the same way: reading
 * esp_matter_data_model.cpp's set_val(), which returns
 * ESP_ERR_NOT_SUPPORTED for attributes flagged
 * ATTRIBUTE_FLAG_MANAGED_INTERNALLY). Updating them needs
 * TemperatureMeasurementCluster::SetMeasuredValue() /
 * RelativeHumidityMeasurementCluster::SetMeasuredValue(), looked up via
 * the data model provider's registry — the exact pattern
 * update_contact_state() in firmware/contact-sensor/main/app_main.cpp
 * already established; see that file's header comment for the fuller
 * explanation of why.
 *
 * SHT3x wiring: I2C — SDA/SCL (GPIO 21/22 below, the common ESP32 WROOM-32
 * default; most breakout boards have onboard pull-ups so no external
 * resistors are usually needed), plus VDD/GND. Default I2C address 0x44
 * (ADDR pin low or floating); 0x45 if ADDR is tied to VDD.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/clusters/relative-humidity-measurement-server/RelativeHumidityMeasurementCluster.h>

static const char *TAG = "matter_climate";

/* Change these to match your board's I2C wiring if different. */
#define SHT3X_SDA_GPIO GPIO_NUM_21
#define SHT3X_SCL_GPIO GPIO_NUM_22
#define SHT3X_I2C_ADDR 0x44
#define SHT3X_I2C_FREQ_HZ 100000
/* How often to read the sensor and push new values into Matter. SHT3x
 * itself can sample much faster; this interval just keeps traffic (and
 * self-heating from the sensor's own heater element, if ever enabled)
 * low for a slow-changing quantity like room temperature/humidity. */
#define SHT3X_MEASURE_INTERVAL_MS 10000

/* LED for the Matter "Identify" cluster — blinks so you can physically find
 * this device when a controller asks it to identify itself. GPIO 2 is
 * commonly the onboard/user LED on classic ESP32 (WROOM-32) devkits and
 * isn't otherwise used by this firmware. Adjust to match your board. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t temperature_endpoint_id = 0;
static uint16_t humidity_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static i2c_master_dev_handle_t sht3x_dev = NULL;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* SHT3x CRC-8 check (polynomial 0x31, init 0xFF) — covers each 2-byte
 * value the sensor returns. Per the Sensirion datasheet; rejecting a
 * reading that fails this is cheap insurance against a garbled I2C
 * transaction being reported as a real (wrong) temperature. */
static uint8_t sht3x_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* One single-shot measurement: command 0x2400 = single shot, high
 * repeatability, clock stretching disabled (simpler to handle correctly
 * over the plain i2c_master driver than clock-stretching mode). Returns
 * false — and leaves *temperature_c / *humidity_pct untouched — on any
 * I2C or CRC failure, so the caller can report "no valid reading" rather
 * than a wrong one. */
static bool sht3x_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[2] = {0x24, 0x00};
    esp_err_t err = i2c_master_transmit(sht3x_dev, cmd, sizeof(cmd), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT3x measurement command failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Max measurement duration for high repeatability is ~15ms per the
     * datasheet; wait a bit longer to be safe. */
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    err = i2c_master_receive(sht3x_dev, data, sizeof(data), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT3x read failed: %s", esp_err_to_name(err));
        return false;
    }

    if (sht3x_crc8(data, 2) != data[2] || sht3x_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT3x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = 100.0f * ((float)hum_ticks / 65535.0f);
    return true;
}

/* esp-matter's generic attribute::update() can't write these attributes —
 * see the header comment above for why. Both follow the exact pattern
 * firmware/contact-sensor/main/app_main.cpp's update_contact_state()
 * established: look the cluster instance up directly via the data model
 * provider's registry, then call its cluster-specific setter. A null
 * value (default-constructed Nullable<>) marks "no valid reading", which
 * is exactly the state after a failed sht3x_read() — Matter's
 * MeasuredValue attributes are nullable for precisely this case. */
static void update_temperature(uint16_t endpoint_id, chip::app::DataModel::Nullable<int16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "TemperatureMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::TemperatureMeasurementCluster *>(iface);
    cluster->SetMeasuredValue(value);
}

static void update_humidity(uint16_t endpoint_id, chip::app::DataModel::Nullable<uint16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);

    chip::app::ConcreteClusterPath path(endpoint_id, RelativeHumidityMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "RelativeHumidityMeasurement cluster not found on endpoint %u", endpoint_id);
        return;
    }

    auto *cluster = static_cast<chip::app::Clusters::RelativeHumidityMeasurementCluster *>(iface);
    cluster->SetMeasuredValue(value);
}

/* Periodically reads the sensor and pushes fresh values into both
 * endpoints. Runs as its own task rather than inline in app_main() so it
 * can freely block on I2C transactions and vTaskDelay() without holding
 * up Matter's own startup/event handling. */
static void sensor_task(void *arg)
{
    for (;;) {
        float temperature_c = 0.0f;
        float humidity_pct = 0.0f;

        if (sht3x_read(&temperature_c, &humidity_pct)) {
            /* Matter's MeasuredValue attributes are in centi-units:
             * temperature in 0.01 degC, humidity in 0.01 %RH. */
            int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
            uint16_t hum_centipercent = (uint16_t)(humidity_pct * 100.0f);
            ESP_LOGI(TAG, "SHT3x: %.2f degC, %.2f %%RH", temperature_c, humidity_pct);
            update_temperature(temperature_endpoint_id, chip::app::DataModel::Nullable<int16_t>(temp_centidegrees));
            update_humidity(humidity_endpoint_id, chip::app::DataModel::Nullable<uint16_t>(hum_centipercent));
        } else {
            update_temperature(temperature_endpoint_id, chip::app::DataModel::Nullable<int16_t>());
            update_humidity(humidity_endpoint_id, chip::app::DataModel::Nullable<uint16_t>());
        }

        vTaskDelay(pdMS_TO_TICKS(SHT3X_MEASURE_INTERVAL_MS));
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

/* Called whenever a controller changes an attribute on this device. Both
 * MeasuredValue attributes are read-only and only ever written locally by
 * sensor_task() above, so this is a no-op required by node::create()'s
 * callback signature. */
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
        /* Effect variants (breathe, flash-twice, ...) aren't implemented —
         * treat any of them the same as a plain blink rather than guessing
         * at per-variant timing. */
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u — blinking as usual",
                 effect_id, effect_variant, endpoint_id);
        break;
    }
    return ESP_OK;
}

extern "C" void app_main(void)
{
    /* 1. Init NVS — stores the Matter fabric keys and factory data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 2. Set up the I2C bus + the SHT3x device on it. */
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = SHT3X_SDA_GPIO;
    bus_config.scl_io_num = SHT3X_SCL_GPIO;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t i2c_err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (i2c_err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(i2c_err));
        return;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = SHT3X_I2C_ADDR;
    dev_config.scl_speed_hz = SHT3X_I2C_FREQ_HZ;

    i2c_err = i2c_master_bus_add_device(i2c_bus, &dev_config, &sht3x_dev);
    if (i2c_err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(i2c_err));
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

    /* 3. Build the Matter data model: one node, two endpoints (temperature
     * + humidity) — see the header comment above for why two, not one. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    temperature_sensor::config_t temperature_config;
    endpoint_t *temperature_endpoint = temperature_sensor::create(node, &temperature_config, ENDPOINT_FLAG_NONE, NULL);
    if (!temperature_endpoint) {
        ESP_LOGE(TAG, "Failed to create temperature sensor endpoint");
        return;
    }
    temperature_endpoint_id = endpoint::get_id(temperature_endpoint);
    ESP_LOGI(TAG, "Temperature sensor endpoint id: %u", temperature_endpoint_id);

    humidity_sensor::config_t humidity_config;
    endpoint_t *humidity_endpoint = humidity_sensor::create(node, &humidity_config, ENDPOINT_FLAG_NONE, NULL);
    if (!humidity_endpoint) {
        ESP_LOGE(TAG, "Failed to create humidity sensor endpoint");
        return;
    }
    humidity_endpoint_id = endpoint::get_id(humidity_endpoint);
    ESP_LOGI(TAG, "Humidity sensor endpoint id: %u", humidity_endpoint_id);

    /* 4. Start Matter — begins BLE advertising so a controller can commission it. */
    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }

    /* 5. Start reading the sensor now that the data model + Matter stack
     * both exist — sensor_task() writes into clusters created above. */
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter temperature/humidity sensor started. Scan the QR code to commission.");
}
