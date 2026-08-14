/*
 * Minimal Matter Temperature + Humidity Sensor — multiple sensor drivers,
 * one selected at compile time.
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
 * Why an external sensor at all: classic ESP32 (WROOM-32, this repo's
 * default target) has no internal temperature sensor peripheral exposed by
 * ESP-IDF — that arrived with later chips (S2, S3, C3, C6, ...).
 *
 * Sensor selection: change SENSOR_TYPE below (or let the wizard's sed
 * command do it) to match whichever sensor you actually have wired up.
 * Each option compiles in that sensor's own driver code — they're
 * different protocols and command sets, not just different I2C addresses,
 * so this can't be a runtime setting without carrying every driver's code
 * and GPIO requirements at once. SENSOR_PIN_1/SENSOR_PIN_2 below are
 * deliberately generic (not "SDA"/"SCL"-specific) so the wizard's
 * Configure Device step doesn't need per-sensor field variants: PIN_1 is
 * I2C SDA for the I2C sensors or the single DATA line for the single-wire
 * ones; PIN_2 is I2C SCL and simply unused (compiled out) for the
 * single-wire sensors.
 *
 * This list follows what's actually most common in beginner-through-
 * professional hobbyist/maker projects (checked against current sources
 * rather than assumed — the ESPHome/Home Assistant community in
 * particular converged hard on BME280 as "the" all-rounder, which almost
 * got left off an earlier draft of this list for only including sensors
 * this repo's own testing happened to already have on hand):
 *
 *   SHT3X   — Sensirion, I2C, temp + humidity. Verified on real hardware
 *             (this device type's original sensor).
 *   SHT4X   — Sensirion's current generation, I2C, temp + humidity;
 *             Sensirion's own recommended default for new designs over
 *             SHT3x. Implemented from datasheet; not personally
 *             hardware-verified in this repo.
 *   AHT20   — I2C, temp + humidity, the current/recommended member of the
 *             AHT1x/2x family — a common cheap SHT3x-adjacent
 *             alternative. Implemented from datasheet/reference driver
 *             sources; not personally hardware-verified in this repo.
 *   DHT11   — single-wire (bit-banged), temp + humidity, the single most
 *             common beginner sensor. Verified on real hardware.
 *   DHT22   — single-wire (bit-banged), temp + humidity, same protocol
 *             family as DHT11 with finer resolution. Verified on real
 *             hardware.
 *   DS18B20 — 1-Wire (bit-banged), temperature only — no humidity, so
 *             this device type's humidity endpoint isn't created at all
 *             when this is selected (see SENSOR_HAS_HUMIDITY below).
 *             Implemented from datasheet; not personally hardware-
 *             verified in this repo. Assumes a single sensor on the bus
 *             (uses the Skip ROM command rather than full ROM search).
 *   BME280  — Bosch, I2C, temp + humidity (+ pressure, not exposed here —
 *             this device type only has Temperature/Humidity endpoints).
 *             The most commonly recommended all-rounder in the ESP32/
 *             ESPHome/Home Assistant maker community. Compensation
 *             formulas are Bosch's own official reference algorithm
 *             (fixed-point integer math against per-chip calibration
 *             coefficients read from the sensor's own NVM at startup —
 *             this is meaningfully more involved than the other I2C
 *             sensors here, not a simplification of it). Implemented from
 *             Bosch's reference driver; not personally hardware-verified
 *             in this repo.
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
 * already established.
 */

#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
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

/* --- Sensor selection — change this one line to match your hardware --- */
#define SENSOR_SHT3X 1
#define SENSOR_SHT4X 2
#define SENSOR_AHT20 3
#define SENSOR_DHT11 4
#define SENSOR_DHT22 5
#define SENSOR_DS18B20 6
#define SENSOR_BME280 7

#define SENSOR_TYPE SENSOR_SHT3X

#if SENSOR_TYPE == SENSOR_DS18B20
#define SENSOR_HAS_HUMIDITY 0
#else
#define SENSOR_HAS_HUMIDITY 1
#endif

#if SENSOR_TYPE == SENSOR_SHT3X || SENSOR_TYPE == SENSOR_SHT4X || SENSOR_TYPE == SENSOR_AHT20 || SENSOR_TYPE == SENSOR_BME280
#define SENSOR_IS_I2C 1
#else
#define SENSOR_IS_I2C 0
#endif

/* PIN_1: I2C SDA (SHT3x/SHT4x/AHT20/BME280) or the single DATA line
 * (DHT11/DHT22/DS18B20). PIN_2: I2C SCL — declared but unused/compiled
 * out for the single-wire sensors, which only need one line. */
#define SENSOR_PIN_1 GPIO_NUM_21
#define SENSOR_PIN_2 GPIO_NUM_22
#define SENSOR_I2C_FREQ_HZ 100000
/* How often to read the sensor and push new values into Matter. */
#define SENSOR_MEASURE_INTERVAL_MS 10000

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
#if SENSOR_HAS_HUMIDITY
static uint16_t humidity_endpoint_id = 0;
#endif
static esp_timer_handle_t identify_led_timer = NULL;

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* ======================================================================
 * Sensor drivers. Only the one matching SENSOR_TYPE is compiled in.
 * Each provides sensor_setup() (called once from app_main) and
 * sensor_read() (called periodically from sensor_task) with the same
 * signatures regardless of which sensor is selected.
 * ====================================================================== */

#if SENSOR_IS_I2C
static i2c_master_dev_handle_t i2c_dev = NULL;

static bool i2c_bus_setup(uint16_t device_address)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = SENSOR_PIN_1;
    bus_config.scl_io_num = SENSOR_PIN_2;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = device_address;
    dev_config.scl_speed_hz = SENSOR_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Sensirion CRC-8 (polynomial 0x31, init 0xFF) — shared by SHT3x and
 * SHT4x (Sensirion uses the same checksum across their whole product
 * line). Covers each 2-byte value the sensor returns. */
static uint8_t sensirion_crc8(const uint8_t *data, size_t len)
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
#endif /* SENSOR_IS_I2C */

#if SENSOR_TYPE == SENSOR_SHT3X

#define SHT3X_I2C_ADDR 0x44 /* 0x45 if the ADDR pin is tied to VDD instead of GND/floating */

static bool sensor_setup(void)
{
    return i2c_bus_setup(SHT3X_I2C_ADDR);
}

static bool sensor_read(float *temperature_c, float *humidity_pct)
{
    /* Single shot, high repeatability, clock stretching disabled. */
    const uint8_t cmd[2] = {0x24, 0x00};
    esp_err_t err = i2c_master_transmit(i2c_dev, cmd, sizeof(cmd), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT3x measurement command failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Max measurement duration for high repeatability is ~15ms per the
     * datasheet; wait a bit longer to be safe. */
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    err = i2c_master_receive(i2c_dev, data, sizeof(data), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT3x read failed: %s", esp_err_to_name(err));
        return false;
    }

    if (sensirion_crc8(data, 2) != data[2] || sensirion_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT3x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = 100.0f * ((float)hum_ticks / 65535.0f);
    return true;
}

#elif SENSOR_TYPE == SENSOR_SHT4X

#define SHT4X_I2C_ADDR 0x44 /* common default; some breakouts wire 0x45/0x46 instead */

static bool sensor_setup(void)
{
    return i2c_bus_setup(SHT4X_I2C_ADDR);
}

static bool sensor_read(float *temperature_c, float *humidity_pct)
{
    /* "Measure T & RH with high precision" command. */
    const uint8_t cmd[1] = {0xFD};
    esp_err_t err = i2c_master_transmit(i2c_dev, cmd, sizeof(cmd), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT4x measurement command failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Datasheet: max ~8.3-10ms for high precision. */
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6];
    err = i2c_master_receive(i2c_dev, data, sizeof(data), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT4x read failed: %s", esp_err_to_name(err));
        return false;
    }

    if (sensirion_crc8(data, 2) != data[2] || sensirion_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT4x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    /* SHT4x's own conversion formulas — note the humidity one has a -6
     * offset and a 125 (not 100) scale, unlike SHT3x's simpler one. */
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = -6.0f + 125.0f * ((float)hum_ticks / 65535.0f);
    if (*humidity_pct < 0.0f) {
        *humidity_pct = 0.0f;
    } else if (*humidity_pct > 100.0f) {
        *humidity_pct = 100.0f;
    }
    return true;
}

#elif SENSOR_TYPE == SENSOR_AHT20

#define AHT20_I2C_ADDR 0x38

static bool sensor_setup(void)
{
    if (!i2c_bus_setup(AHT20_I2C_ADDR)) {
        return false;
    }
    /* AHT20's init command (0xBE) differs from the older AHT10's (0xE1) —
     * everything else about the protocol is the same. */
    const uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
    esp_err_t err = i2c_master_transmit(i2c_dev, init_cmd, sizeof(init_cmd), 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 init command failed: %s", esp_err_to_name(err));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(40)); /* power-on/calibration settle */
    return true;
}

static bool sensor_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t trigger_cmd[3] = {0xAC, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(i2c_dev, trigger_cmd, sizeof(trigger_cmd), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 trigger command failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Datasheet: measurement takes >75ms. */
    vTaskDelay(pdMS_TO_TICKS(85));

    uint8_t data[6];
    err = i2c_master_receive(i2c_dev, data, sizeof(data), 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 read failed: %s", esp_err_to_name(err));
        return false;
    }

    if (data[0] & 0x80) {
        ESP_LOGW(TAG, "AHT20 still busy — discarding reading");
        return false;
    }

    /* 20-bit humidity in data[1..2] + upper nibble of data[3]; 20-bit
     * temperature in the lower nibble of data[3] + data[4..5]. */
    uint32_t raw_humidity = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_temperature = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humidity_pct = (float)raw_humidity * 100.0f / 1048576.0f; /* /2^20 */
    *temperature_c = (float)raw_temperature * 200.0f / 1048576.0f - 50.0f;
    return true;
}

#elif SENSOR_TYPE == SENSOR_DHT11 || SENSOR_TYPE == SENSOR_DHT22

/* DHT11/DHT22 share one single-wire, bit-banged protocol; only how the 5
 * returned bytes are interpreted differs (see dht_parse() below). Timing
 * values below are the documented ones (start signal, response pulses,
 * per-bit LOW/HIGH durations) — this is what an oscilloscope on the data
 * line would show, not something this repo invented.
 *
 * Runs with interrupts disabled on this core for the ~5ms a transaction
 * takes, so a FreeRTOS tick/context switch can't stretch a timing window
 * mid-bit. Only happens once per SENSOR_MEASURE_INTERVAL_MS, so the
 * impact on anything else running on this core (Wi-Fi/BLE included) is a
 * brief, infrequent stall rather than a sustained one. */

static bool dht_wait_level(gpio_num_t pin, int level, uint32_t timeout_us)
{
    for (uint32_t waited = 0; gpio_get_level(pin) != level; waited++) {
        if (waited >= timeout_us) {
            return false;
        }
        esp_rom_delay_us(1);
    }
    return true;
}

static bool dht_read_raw(uint8_t out[5])
{
    gpio_set_level(SENSOR_PIN_1, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); /* start signal: >=18ms LOW works for both DHT11 and DHT22 */

    portDISABLE_INTERRUPTS();
    gpio_set_level(SENSOR_PIN_1, 1);
    esp_rom_delay_us(30);

    bool ok = dht_wait_level(SENSOR_PIN_1, 0, 100) && /* sensor response: ~80us LOW */
              dht_wait_level(SENSOR_PIN_1, 1, 100) && /* ~80us HIGH */
              dht_wait_level(SENSOR_PIN_1, 0, 100);   /* first data bit's leading LOW */

    if (ok) {
        memset(out, 0, 5);
        for (int i = 0; i < 40; i++) {
            if (!dht_wait_level(SENSOR_PIN_1, 1, 80)) { /* each bit starts with ~50us LOW */
                ok = false;
                break;
            }
            /* Bit 0's HIGH pulse is ~26-28us, bit 1's is ~70us — 40us is
             * a safe midpoint threshold between the two. */
            uint32_t high_us = 0;
            while (gpio_get_level(SENSOR_PIN_1) == 1 && high_us < 100) {
                esp_rom_delay_us(1);
                high_us++;
            }
            out[i / 8] = (uint8_t)((out[i / 8] << 1) | (high_us > 40 ? 1 : 0));
        }
    }
    portENABLE_INTERRUPTS();
    return ok;
}

static bool dht_parse(const uint8_t data[5], float *temperature_c, float *humidity_pct)
{
    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        ESP_LOGW(TAG, "DHT checksum mismatch — discarding reading");
        return false;
    }

#if SENSOR_TYPE == SENSOR_DHT11
    /* DHT11: integer-only humidity + temperature (the "decimal" bytes are
     * 0 on genuine DHT11 parts); negative temperature signalled by the
     * top bit of the temperature byte on parts that support it at all. */
    *humidity_pct = (float)data[0];
    int8_t temp_int = (int8_t)(data[2] & 0x7F);
    *temperature_c = (data[2] & 0x80) ? -(float)temp_int : (float)temp_int;
#else /* DHT22 */
    uint16_t raw_humidity = ((uint16_t)data[0] << 8) | data[1];
    *humidity_pct = raw_humidity / 10.0f;
    uint16_t raw_temp = ((uint16_t)data[2] << 8) | data[3];
    *temperature_c = (raw_temp & 0x8000) ? -((raw_temp & 0x7FFF) / 10.0f) : (raw_temp / 10.0f);
#endif
    return true;
}

static bool sensor_setup(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << SENSOR_PIN_1);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD; /* open-drain: the bus is shared, idle-HIGH */
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  /* belt-and-suspenders; most breakouts have their own */
    gpio_config(&io_conf);
    gpio_set_level(SENSOR_PIN_1, 1);
    return true;
}

static bool sensor_read(float *temperature_c, float *humidity_pct)
{
    uint8_t raw[5];
    if (!dht_read_raw(raw)) {
        ESP_LOGW(TAG, "DHT read timed out — check wiring/pull-up");
        return false;
    }
    return dht_parse(raw, temperature_c, humidity_pct);
}

#elif SENSOR_TYPE == SENSOR_DS18B20

/* 1-Wire, bit-banged. Assumes a single DS18B20 on the bus (uses Skip ROM
 * rather than a full ROM search), which covers the common case of one
 * sensor per GPIO. Timing values are the documented 1-Wire slot timings
 * (reset/presence, write-0/write-1, read), not invented here. Same
 * interrupts-disabled reasoning as the DHT driver above — a full
 * convert+read cycle holds the line for under 1ms of actual bit-banging
 * (the ~750ms conversion wait in between is a plain vTaskDelay, not
 * interrupt-disabled). */

static bool ow_reset(void)
{
    gpio_set_level(SENSOR_PIN_1, 0);
    esp_rom_delay_us(480);
    gpio_set_level(SENSOR_PIN_1, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(SENSOR_PIN_1) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(SENSOR_PIN_1, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(SENSOR_PIN_1, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(SENSOR_PIN_1, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(SENSOR_PIN_1, 0);
    esp_rom_delay_us(2);
    gpio_set_level(SENSOR_PIN_1, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(SENSOR_PIN_1);
    esp_rom_delay_us(50);
    return bit;
}

static void ow_write_byte(uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(byte & 0x01);
        byte >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte = (uint8_t)(byte | (ow_read_bit() << i));
    }
    return byte;
}

/* Dallas/Maxim 1-Wire CRC-8 (reflected, polynomial 0x8C, init 0x00) — the
 * standard algorithm used by every 1-Wire device family, not specific to
 * this repo. */
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

static bool sensor_setup(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << SENSOR_PIN_1);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(SENSOR_PIN_1, 1);
    return true;
}

static bool sensor_read(float *temperature_c, float *humidity_pct)
{
    (void)humidity_pct; /* DS18B20 is temperature-only */

    portDISABLE_INTERRUPTS();
    bool present = ow_reset();
    if (present) {
        ow_write_byte(0xCC); /* Skip ROM */
        ow_write_byte(0x44); /* Convert T */
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset — check wiring/pull-up");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(750)); /* max conversion time at default 12-bit resolution */

    portDISABLE_INTERRUPTS();
    present = ow_reset();
    uint8_t scratchpad[9] = {0};
    if (present) {
        ow_write_byte(0xCC);
        ow_write_byte(0xBE); /* Read Scratchpad */
        for (int i = 0; i < 9; i++) {
            scratchpad[i] = ow_read_byte();
        }
    }
    portENABLE_INTERRUPTS();
    if (!present) {
        ESP_LOGW(TAG, "DS18B20 not responding to reset (read phase)");
        return false;
    }

    if (onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGW(TAG, "DS18B20 CRC mismatch — discarding reading");
        return false;
    }

    int16_t raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    *temperature_c = raw * 0.0625f; /* 12-bit default resolution: 1 LSB = 1/16 degC */
    return true;
}

#elif SENSOR_TYPE == SENSOR_BME280

/* Bosch BME280. Compensation formulas below are Bosch's own official
 * fixed-point reference algorithm (from their public BME280_driver
 * repository), reproduced as-is rather than approximated — this sensor's
 * raw ADC counts are meaningless without per-chip calibration
 * coefficients read from its own NVM (registers 0x88.. and 0xE1..) at
 * startup, unlike the other sensors here which return already-linear
 * values. */

#define BME280_I2C_ADDR 0x76 /* 0x77 if the SDO pin is tied to VDD instead of GND */
#define BME280_REG_CHIP_ID 0xD0
#define BME280_REG_CALIB_T 0x88
#define BME280_REG_CALIB_H1 0xA1
#define BME280_REG_CALIB_H2 0xE1
#define BME280_REG_CTRL_HUM 0xF2
#define BME280_REG_CTRL_MEAS 0xF4
#define BME280_REG_DATA 0xFA /* temp_msb..hum_lsb, 5 bytes */
#define BME280_CHIP_ID_EXPECTED 0x60

struct bme280_calib_data {
    uint16_t dig_t1;
    int16_t dig_t2;
    int16_t dig_t3;
    uint8_t dig_h1;
    int16_t dig_h2;
    uint8_t dig_h3;
    int16_t dig_h4;
    int16_t dig_h5;
    int8_t dig_h6;
};

static struct bme280_calib_data bme280_calib;

static bool bme280_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool bme280_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(i2c_dev, &reg, 1, out, len, 1000) == ESP_OK;
}

/* Sign-extends a 12-bit packed value (used by dig_H4/dig_H5, which the
 * datasheet stores split across shared nibbles of adjacent registers). */
static int16_t sign_extend_12bit(uint16_t value)
{
    return (int16_t)((value & 0x0800) ? (value | 0xF000) : value);
}

static bool sensor_setup(void)
{
    if (!i2c_bus_setup(BME280_I2C_ADDR)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (!bme280_read_regs(BME280_REG_CHIP_ID, &chip_id, 1) || chip_id != BME280_CHIP_ID_EXPECTED) {
        ESP_LOGE(TAG, "BME280 chip ID mismatch (got 0x%02X, expected 0x%02X) — check wiring/address",
                 chip_id, BME280_CHIP_ID_EXPECTED);
        return false;
    }

    uint8_t calib_t[6];
    if (!bme280_read_regs(BME280_REG_CALIB_T, calib_t, sizeof(calib_t))) {
        ESP_LOGE(TAG, "BME280 failed to read temperature calibration");
        return false;
    }
    bme280_calib.dig_t1 = (uint16_t)(calib_t[0] | (calib_t[1] << 8));
    bme280_calib.dig_t2 = (int16_t)(calib_t[2] | (calib_t[3] << 8));
    bme280_calib.dig_t3 = (int16_t)(calib_t[4] | (calib_t[5] << 8));

    uint8_t dig_h1 = 0;
    if (!bme280_read_regs(BME280_REG_CALIB_H1, &dig_h1, 1)) {
        ESP_LOGE(TAG, "BME280 failed to read dig_H1");
        return false;
    }
    bme280_calib.dig_h1 = dig_h1;

    uint8_t calib_h[7];
    if (!bme280_read_regs(BME280_REG_CALIB_H2, calib_h, sizeof(calib_h))) {
        ESP_LOGE(TAG, "BME280 failed to read humidity calibration");
        return false;
    }
    bme280_calib.dig_h2 = (int16_t)(calib_h[0] | (calib_h[1] << 8));
    bme280_calib.dig_h3 = calib_h[2];
    bme280_calib.dig_h4 = sign_extend_12bit((uint16_t)((calib_h[3] << 4) | (calib_h[4] & 0x0F)));
    bme280_calib.dig_h5 = sign_extend_12bit((uint16_t)((calib_h[5] << 4) | (calib_h[4] >> 4)));
    bme280_calib.dig_h6 = (int8_t)calib_h[6];

    /* Humidity oversampling must be written before ctrl_meas for it to
     * take effect (datasheet section 5.4.3). x1 oversampling for both —
     * plenty for a 10-second reporting interval. */
    if (!bme280_write_reg(BME280_REG_CTRL_HUM, 0x01)) {
        ESP_LOGE(TAG, "BME280 failed to set humidity oversampling");
        return false;
    }

    return true;
}

/* Bosch's official integer compensation formulas, reproduced verbatim
 * from their public BME280_driver reference implementation. temperature
 * comes out in 0.01 degC units already (matching Matter's own
 * MeasuredValue unit); humidity comes out in Q22.10 fixed point
 * (divide by 1024.0 for %RH — confirmed by its documented max value of
 * 102400, i.e. 102400/1024 = 100.0%). */
static int32_t bme280_compensate_temperature(int32_t adc_t, int32_t *t_fine)
{
    int32_t var1 = ((adc_t / 8) - ((int32_t)bme280_calib.dig_t1 * 2)) * ((int32_t)bme280_calib.dig_t2) / 2048;
    int32_t var2_pre = (adc_t / 16) - ((int32_t)bme280_calib.dig_t1);
    int32_t var2 = (((var2_pre * var2_pre) / 4096) * ((int32_t)bme280_calib.dig_t3)) / 16384;
    *t_fine = var1 + var2;
    int32_t temperature = (*t_fine * 5 + 128) / 256;
    if (temperature < -4000) {
        temperature = -4000;
    } else if (temperature > 8500) {
        temperature = 8500;
    }
    return temperature;
}

static uint32_t bme280_compensate_humidity(int32_t adc_h, int32_t t_fine)
{
    int32_t var1 = t_fine - 76800;
    int32_t var2 = adc_h * 16384;
    int32_t var3 = ((int32_t)bme280_calib.dig_h4) * 1048576;
    int32_t var4 = ((int32_t)bme280_calib.dig_h5) * var1;
    int32_t var5 = (((var2 - var3) - var4) + 16384) / 32768;
    var2 = (var1 * ((int32_t)bme280_calib.dig_h6)) / 1024;
    var3 = (var1 * ((int32_t)bme280_calib.dig_h3)) / 2048;
    var4 = ((var2 * (var3 + 32768)) / 1024) + 2097152;
    var2 = ((var4 * ((int32_t)bme280_calib.dig_h2)) + 8192) / 16384;
    var3 = var5 * var2;
    int32_t var4b = ((var3 / 32768) * (var3 / 32768)) / 128;
    int32_t var5b = var3 - ((var4b * ((int32_t)bme280_calib.dig_h1)) / 16);
    if (var5b < 0) {
        var5b = 0;
    } else if (var5b > 419430400) {
        var5b = 419430400;
    }
    uint32_t humidity = (uint32_t)(var5b / 4096);
    if (humidity > 102400) {
        humidity = 102400;
    }
    return humidity;
}

static bool sensor_read(float *temperature_c, float *humidity_pct)
{
    /* Forced mode: takes one reading then goes back to sleep — matches
     * this device's infrequent-polling pattern better than continuous
     * "normal mode" would. osrs_t=001 (x1), osrs_p=001 (x1, unused —
     * pressure isn't exposed by this device type), mode=01 (forced). */
    if (!bme280_write_reg(BME280_REG_CTRL_MEAS, 0x25)) {
        ESP_LOGW(TAG, "BME280 failed to trigger a measurement");
        return false;
    }

    /* Generous fixed wait instead of polling the status register — x1
     * oversampling completes in a few ms per the datasheet's timing
     * formula, this leaves ample margin. */
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[5];
    if (!bme280_read_regs(BME280_REG_DATA, data, sizeof(data))) {
        ESP_LOGW(TAG, "BME280 read failed");
        return false;
    }

    int32_t adc_t = (int32_t)(((uint32_t)data[0] << 12) | ((uint32_t)data[1] << 4) | (data[2] >> 4));
    int32_t adc_h = (int32_t)(((uint32_t)data[3] << 8) | data[4]);

    int32_t t_fine = 0;
    int32_t temp_centidegrees = bme280_compensate_temperature(adc_t, &t_fine);
    uint32_t hum_q22_10 = bme280_compensate_humidity(adc_h, t_fine);

    *temperature_c = temp_centidegrees / 100.0f;
    *humidity_pct = hum_q22_10 / 1024.0f;
    return true;
}

#else
#error "Unknown SENSOR_TYPE"
#endif

/* esp-matter's generic attribute::update() can't write these attributes —
 * see the header comment above for why. Both follow the exact pattern
 * firmware/contact-sensor/main/app_main.cpp's update_contact_state()
 * established: look the cluster instance up directly via the data model
 * provider's registry, then call its cluster-specific setter. A null
 * value (default-constructed Nullable<>) marks "no valid reading", which
 * is exactly the state after a failed sensor_read() — Matter's
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

#if SENSOR_HAS_HUMIDITY
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
#endif

/* Periodically reads the sensor and pushes fresh values into Matter.
 * Runs as its own task rather than inline in app_main() so it can freely
 * block on I2C/bit-banged transactions and vTaskDelay() without holding
 * up Matter's own startup/event handling. */
static void sensor_task(void *arg)
{
    for (;;) {
        float temperature_c = 0.0f;
        float humidity_pct = 0.0f;

        if (sensor_read(&temperature_c, &humidity_pct)) {
            /* Matter's MeasuredValue attributes are in centi-units:
             * temperature in 0.01 degC, humidity in 0.01 %RH. */
            int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
#if SENSOR_HAS_HUMIDITY
            uint16_t hum_centipercent = (uint16_t)(humidity_pct * 100.0f);
            ESP_LOGI(TAG, "Sensor: %.2f degC, %.2f %%RH", temperature_c, humidity_pct);
            update_humidity(humidity_endpoint_id, chip::app::DataModel::Nullable<uint16_t>(hum_centipercent));
#else
            ESP_LOGI(TAG, "Sensor: %.2f degC", temperature_c);
#endif
            update_temperature(temperature_endpoint_id, chip::app::DataModel::Nullable<int16_t>(temp_centidegrees));
        } else {
            update_temperature(temperature_endpoint_id, chip::app::DataModel::Nullable<int16_t>());
#if SENSOR_HAS_HUMIDITY
            update_humidity(humidity_endpoint_id, chip::app::DataModel::Nullable<uint16_t>());
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_MEASURE_INTERVAL_MS));
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

    /* 2. Set up the selected sensor. */
    if (!sensor_setup()) {
        ESP_LOGE(TAG, "Sensor setup failed — check SENSOR_TYPE and wiring");
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

    /* 3. Build the Matter data model: one node, one endpoint for
     * temperature, plus a second for humidity if this sensor provides
     * it — see the header comment above for why two, not a single
     * combined device type. */
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

#if SENSOR_HAS_HUMIDITY
    humidity_sensor::config_t humidity_config;
    endpoint_t *humidity_endpoint = humidity_sensor::create(node, &humidity_config, ENDPOINT_FLAG_NONE, NULL);
    if (!humidity_endpoint) {
        ESP_LOGE(TAG, "Failed to create humidity sensor endpoint");
        return;
    }
    humidity_endpoint_id = endpoint::get_id(humidity_endpoint);
    ESP_LOGI(TAG, "Humidity sensor endpoint id: %u", humidity_endpoint_id);
#endif

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
