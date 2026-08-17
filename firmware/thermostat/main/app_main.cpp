/*
 * Minimal Matter Thermostat (Heat + Cool) — an eleventh device type, and
 * this repo's first with a genuine control loop (compares a measured value
 * against a setpoint and drives an output) rather than a direct pass-
 * through of a command or a plain sensor readout.
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
 * --- Matter cluster composition ----------------------------------------
 * Unlike firmware/color-light/ and firmware/addressable-light/ (which had
 * to hand-assemble their endpoint because esp-matter's own
 * extended_color_light helper has real gaps), esp-matter DOES ship a
 * complete, directly usable endpoint::thermostat::create() helper
 * (Identify + Groups + Thermostat cluster) — confirmed by reading
 * data_model/legacy/esp_matter_endpoint.cpp's own thermostat::add()
 * directly, not assumed. One real gap worth noting: its config_t declares
 * a scenes_management field that add() never actually wires up (a
 * ScenesManagement cluster.:create() call is simply missing) — harmless
 * for this device (thermostats don't typically need Scenes), so this file
 * uses the helper as-is rather than patching around an unused field.
 *
 * Feature scope: Heat + Cool (ControlSequenceOfOperation =
 * CoolingAndHeating, feature_flags = Heating|Cooling). Not AutoMode
 * (simultaneous heat/cool deadband logic), not Occupancy/Setback/
 * schedule-configuration/presets — this device runs in whichever single
 * mode (Off/Heat/Cool) a controller last set via the SystemMode
 * attribute, matching the read PID/weather-compensation logic real
 * commercial thermostats add on top of these same primitives, which is
 * out of scope here.
 *
 * SystemMode/ControlSequenceOfOperation/OccupiedHeatingSetpoint/
 * OccupiedCoolingSetpoint/LocalTemperature are all plain ember attributes,
 * not the newer "code-driven" cluster classes firmware/contact-sensor/'s
 * BooleanState or firmware/temperature-sensor/'s TemperatureMeasurement
 * are — confirmed by checking that data_model_provider/clusters/ has no
 * thermostat/ folder (unlike boolean_state/, temperature_measurement/,
 * humidistat/, smoke_co_alarm/ which ARE in that list). So this uses the
 * exact same attribute::PRE_UPDATE + attribute::update() pattern as
 * OnOff/LevelControl/ColorControl elsewhere in this repo — no
 * SetMeasuredValue()-style setter needed.
 *
 * Boots to SystemMode=Off (matching every other device type's boot-to-
 * known-state convention in this repo), not whatever a controller last
 * set — a heating/cooling system staying silently active across a power
 * cycle is a worse default than a light staying on.
 *
 * --- Local temperature sensor -------------------------------------------
 * Reuses firmware/temperature-sensor/'s exact 7-chip SENSOR_TYPE driver
 * library verbatim (SHT3x/SHT4x/AHT20/DHT11/DHT22/DS18B20/BME280 — see
 * that file's own header comment for the full per-chip verification
 * detail, not repeated here). Only LocalTemperature is pushed into
 * Matter; humidity (where the selected sensor provides it) is read but
 * unused, since Thermostat has no humidity attribute and Matter has no
 * single device type combining Thermostat + a humidity measurement.
 *
 * --- THERMOSTAT_OUTPUT_TYPE: three ways to actually drive heat/cool -----
 * A control loop needs somewhere to send its heat/cool demand. Requested
 * together as one feature (not staged): direct relay wiring, a bound
 * remote relay module, and native OpenTherm — because a real room
 * thermostat in Europe genuinely needs to support any of these depending
 * on the installation:
 *
 * RELAY (default) — two GPIO relay outputs, active-LOW (matching
 *   firmware/outlet/'s own relay convention — common for low-cost
 *   opto-isolated relay modules; always check your specific module).
 *   Wired in series with a boiler's or AC's "room thermostat" volt-free
 *   contact loop (on virtually every European CV/gas-combi boiler this is
 *   a simple 2-wire "T1-T2" input — the boiler supplies its own low
 *   voltage across those terminals and just needs the loop closed to
 *   signal heat demand, the same way a classic mechanical/bimetal
 *   thermostat has always worked). No separate power feed needed on this
 *   side — it's a dry contact closure, not a powered output.
 *
 * BINDING — sends real OnOff::On/Off commands (not Toggle — a specific
 *   target state, not a flip, since demand state must stay correct even
 *   if a command is missed) to whatever this endpoint's Binding cluster
 *   is bound to, via the exact client::cluster_update() +
 *   client::interaction::invoke::send_request() pattern
 *   firmware/switch/'s buttons already use for the same purpose. Covers
 *   the "a small receiver module hangs next to the boiler, wired to it,
 *   and this thermostat tells that module when to call for heat"
 *   installation style — that receiver module doesn't need new firmware
 *   at all, firmware/outlet/'s existing relay output already does the
 *   job. Binding + a client OnOff cluster are added onto this SAME
 *   thermostat endpoint (not a second endpoint) — the exact cluster pair
 *   esp-matter's own on_off_light_switch::add() uses for its equivalent
 *   purpose, confirmed by reading that function directly rather than
 *   guessing at the shape. Heat-only: this mode's real-world motivation
 *   (Europe's "second module at the boiler") is specifically a heating
 *   scenario; Cool demand still requires RELAY or OPENTHERM.
 *
 * OPENTHERM — a full OpenTherm master implementation talking directly to
 *   an OpenTherm-capable boiler over its native 2-wire interface, for
 *   modulating (not just on/off) control. See the large comment block
 *   above the OpenTherm driver section below for the complete protocol
 *   detail and its sourcing — verified against the OpenTherm
 *   Association's own Protocol Specification v2.2 PDF (fetched and read
 *   via pdftotext, this repo's established practice for primary-source
 *   datasheets/specs — a web summary alone materially underspecifies the
 *   bit-timing and frame-format detail this needs), and its GPIO-level
 *   driver logic ported from Ihor Melnyk's opentherm_library
 *   (github.com/ihormelnyk/opentherm_library), the reference
 *   implementation this DIY/ESPHome/Home-Assistant OpenTherm community
 *   has standardized on — same "best available, independently
 *   cross-checked" sourcing standard already used in this repo for
 *   APA102 and SM2335EGH in firmware/addressable-light/.
 */

#include <string.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <esp_matter.h>
#include <esp_matter_client.h>

static const char *TAG = "matter_thermostat";

/* --- Sensor selection — change this one line to match your hardware ---
 * Identical driver library to firmware/temperature-sensor/'s (SENSOR_TYPE
 * chosen the same way, same SENSOR_PIN_1/SENSOR_PIN_2 generic naming for
 * the same wizard-friendliness reason) — see that file's header comment
 * for full per-chip sourcing/verification detail. */
#define SENSOR_SHT3X 1
#define SENSOR_SHT4X 2
#define SENSOR_AHT20 3
#define SENSOR_DHT11 4
#define SENSOR_DHT22 5
#define SENSOR_DS18B20 6
#define SENSOR_BME280 7

#define SENSOR_TYPE SENSOR_SHT3X

#if SENSOR_TYPE == SENSOR_SHT3X || SENSOR_TYPE == SENSOR_SHT4X || SENSOR_TYPE == SENSOR_AHT20 || SENSOR_TYPE == SENSOR_BME280
#define SENSOR_IS_I2C 1
#else
#define SENSOR_IS_I2C 0
#endif

#define SENSOR_PIN_1 GPIO_NUM_21
#define SENSOR_PIN_2 GPIO_NUM_22
#define SENSOR_I2C_FREQ_HZ 100000
/* How often to physically re-measure temperature. Deliberately separate
 * from THERMOSTAT_OPENTHERM_POLL_INTERVAL_MS below — OpenTherm needs a
 * message at least once a second to keep the link alive regardless of
 * how often the sensor itself is actually re-read. */
#define SENSOR_MEASURE_INTERVAL_MS 10000

/* --- THERMOSTAT_OUTPUT_TYPE — how heat/cool demand actually reaches the
 * boiler/AC. See the header comment above for the full description of
 * each. */
#define THERMOSTAT_OUTPUT_RELAY 1
#define THERMOSTAT_OUTPUT_BINDING 2
#define THERMOSTAT_OUTPUT_OPENTHERM 3

#define THERMOSTAT_OUTPUT_TYPE THERMOSTAT_OUTPUT_RELAY

/* RELAY mode GPIOs — active-LOW, see the header comment above. */
#define THERMOSTAT_HEAT_RELAY_GPIO GPIO_NUM_4
#define THERMOSTAT_COOL_RELAY_GPIO GPIO_NUM_16

/* OPENTHERM mode GPIOs — these connect to an OpenTherm adapter board's
 * logic-level in/out pins (e.g. Ihor Melnyk's widely-used design), NOT
 * directly to the OpenTherm 2-wire bus — see the driver section's header
 * comment for exactly why bare GPIO can't do this. */
#define THERMOSTAT_OPENTHERM_IN_GPIO GPIO_NUM_17
#define THERMOSTAT_OPENTHERM_OUT_GPIO GPIO_NUM_18
/* Spec requires the master to communicate at least once a second
 * (+15% tolerance) or a compliant boiler falls back to treating a
 * sustained idle-line as "no thermostat connected". 1000ms with margin
 * inside that tolerance. */
#define THERMOSTAT_OPENTHERM_POLL_INTERVAL_MS 1000
/* CH (central heating) flow-water-temperature setpoint sent as OpenTherm
 * data-id 1 (Control setpoint) whenever CH is enabled — this device does
 * on/off room control (CH-enable bit), not full weather-compensated
 * modulation, so a fixed reasonable flow temperature is sent and the
 * boiler's own internal modulation logic handles the rest (the spec's
 * own data-id 14 description explicitly calls this "on-off control
 * mode", a real, documented, simpler alternative to full weather-comp —
 * not invented here). 60 degC suits typical radiator systems; lower it
 * (e.g. 35-45 degC) for underfloor heating. */
#define THERMOSTAT_OPENTHERM_CH_SETPOINT_C 60.0f

/* Hysteresis (deadband) around each setpoint before the output actually
 * switches — a bare threshold would rapidly chatter the relay/boiler
 * on and off right at the setpoint due to ordinary sensor noise. Every
 * real thermostat (this repo's own bang-bang implementation included)
 * uses some form of this; 0.3 degC is a common, unremarkable choice
 * (tight enough that a room's temperature swing stays barely
 * noticeable, wide enough that relay wear from rapid cycling isn't a
 * concern). In centidegrees C to match Matter's own attribute units. */
#define THERMOSTAT_HYSTERESIS_CENTIDEGREES 30

/* Optional rotary encoder — local setpoint adjustment without needing a
 * controller. Off by default (GPIO_NUM_NC on all three pins, checked at
 * runtime the same way other optional GPIO features in this repo are).
 * Standard two-channel
 * quadrature rotation (A/B) plus the encoder's own integrated push-button
 * — the same class of part sold as a "KY-040" breakout. Rotating adjusts
 * whichever setpoint the current SystemMode actually uses (heating
 * setpoint while in Heat, cooling setpoint while in Cool, no-op while
 * Off); pressing the button cycles SystemMode Off -> Heat -> Cool -> Off.
 * Local changes are written back into the real Matter attributes (see
 * encoder_task() below) so a bound controller sees them too, not just
 * whatever this device shows locally — the same "local input drives the
 * Matter attribute directly" shape firmware/dimmable-light/'s own output
 * already follows in the other direction. */
#define ROTARY_ENCODER_A_GPIO GPIO_NUM_NC
#define ROTARY_ENCODER_B_GPIO GPIO_NUM_NC
#define ROTARY_ENCODER_BUTTON_GPIO GPIO_NUM_NC
#define ROTARY_ENCODER_STEP_CENTIDEGREES 50 /* 0.5 degC per detent */

/* --- Optional local display — DISPLAY_TYPE ------------------------------
 * Off by default (DISPLAY_NONE). Shows the current room temperature as
 * large 7-segment-style digits (drawn as filled rectangles, not a bitmap
 * font — see the rendering section's own header comment for why) plus a
 * color/shape indicator for SystemMode + heat/cool demand. Three real,
 * genuinely different display chips/protocols, picked to match what was
 * actually asked for:
 *
 * GC9A01 (1.28" round, 240x240, RGB565 over SPI) — its own datasheet
 *   documents the standard MIPI-DCS command set (SLPOUT/DISPON/CASET/
 *   RASET/RAMWR/MADCTL/COLMOD, all confirmed directly in the datasheet
 *   PDF via pdftotext) but leaves roughly 70% of its actual power-on init
 *   sequence as unlabelled vendor-specific registers — a real, widely
 *   reported gap (confirmed independently, not just claimed by one
 *   source), the same situation APA102/SM2335EGH in
 *   firmware/addressable-light/ were in. Same resolution: this file's
 *   init sequence is ported verbatim from moononournation/Arduino_GFX
 *   (MIT-licensed, a real, actively-maintained, widely-used Arduino
 *   graphics library), fetched directly rather than assumed from a
 *   secondary description.
 * ST7789 (the "1.25 inch", 76x284 module) — ST7789 itself is fully,
 *   properly documented (Sitronix's own datasheet — SWRESET/SLPOUT/
 *   INVON/MADCTL/COLMOD/CASET/RASET/RAMWR/NORON/DISPON, all standard,
 *   all confirmed). The 76x284 pixel window is this specific module's
 *   own cropped/offset active area within the chip's larger addressable
 *   range (up to 240x320) — DISPLAY_ST7789_COL_OFFSET/_ROW_OFFSET below
 *   are deliberately exposed as adjustable #defines rather than hardcoded,
 *   since that offset is set by the glass panel this specific module
 *   uses, not by the ST7789 chip itself, and isn't something a generic
 *   driver can know in advance — "always check your specific module",
 *   the same honest caveat this repo already gives for relay polarity in
 *   firmware/outlet/.
 * SSD1306 (0.96" 128x64, I2C, monochrome) — this repo's first I2C
 *   display (reuses driver/i2c_master.h, the same API
 *   firmware/temperature-sensor/'s I2C sensors already use, but its own
 *   dedicated bus/pins rather than sharing the temperature sensor's, to
 *   avoid any address/bus-sharing complexity when both are enabled at
 *   once). One of the most standardized init sequences in hobby
 *   electronics — effectively every open-source SSD1306 driver (this
 *   repo's own sequence checked directly against that near-universal
 *   pattern, not assumed from memory) uses the same command bytes:
 *   display off, clock divide, multiplex ratio, display offset, start
 *   line, charge pump, memory addressing mode, segment remap, COM scan
 *   direction, COM pins, contrast, pre-charge, VCOMH deselect, resume to
 *   RAM content, normal (non-inverted) display, display on.
 *
 * None of the three are hardware-tested in this repo (none physically
 * available when written) — flagged here and in the wizard accordingly. */
#define DISPLAY_NONE 0
#define DISPLAY_GC9A01 1
#define DISPLAY_ST7789 2
#define DISPLAY_SSD1306 3

#define DISPLAY_TYPE DISPLAY_NONE

#if DISPLAY_TYPE == DISPLAY_GC9A01 || DISPLAY_TYPE == DISPLAY_ST7789
#define DISPLAY_IS_SPI 1
#else
#define DISPLAY_IS_SPI 0
#endif
#if DISPLAY_TYPE == DISPLAY_SSD1306
#define DISPLAY_IS_I2C 1
#else
#define DISPLAY_IS_I2C 0
#endif

/* SPI displays (GC9A01/ST7789) */
#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_SCLK_GPIO GPIO_NUM_14
#define DISPLAY_MOSI_GPIO GPIO_NUM_13
#define DISPLAY_CS_GPIO GPIO_NUM_15
#define DISPLAY_DC_GPIO GPIO_NUM_27
#define DISPLAY_RST_GPIO GPIO_NUM_26

/* I2C display (SSD1306) — its own bus, see the header comment above. */
#define DISPLAY_I2C_PORT I2C_NUM_1
#define DISPLAY_SDA_GPIO GPIO_NUM_5
#define DISPLAY_SCL_GPIO GPIO_NUM_19
#define DISPLAY_I2C_FREQ_HZ 400000
#define SSD1306_I2C_ADDR 0x3C /* 0x3D on some breakouts */

#define DISPLAY_GC9A01_WIDTH 240
#define DISPLAY_GC9A01_HEIGHT 240
#define DISPLAY_ST7789_WIDTH 76
#define DISPLAY_ST7789_HEIGHT 284
/* This module's own active-area offset within ST7789's larger addressable
 * range — see the header comment above; verify against your specific
 * module if the image appears shifted/cropped. */
#define DISPLAY_ST7789_COL_OFFSET 40
#define DISPLAY_ST7789_ROW_OFFSET 0
#define DISPLAY_SSD1306_WIDTH 128
#define DISPLAY_SSD1306_HEIGHT 64

#define DISPLAY_UPDATE_INTERVAL_MS 2000

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

static uint16_t thermostat_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* Local shadow of controller-writable attributes — same pattern
 * firmware/color-light/ etc. use (track the current value locally,
 * updated from app_attribute_update_cb's PRE_UPDATE, read back by the
 * control loop) since there's no cheap "read the cluster's current
 * attribute value" getter used elsewhere in this repo. Defaults match
 * cluster::thermostat::config_t's own constructor defaults (2000/2600 =
 * 20.00/26.00 degC) and this repo's boot-to-Off convention. */
static uint8_t thermostat_system_mode = 0; /* SystemModeEnum::kOff */
static int16_t thermostat_heating_setpoint_centidegrees = 2000;
static int16_t thermostat_cooling_setpoint_centidegrees = 2600;
static bool thermostat_local_temperature_valid = false;
static int16_t thermostat_local_temperature_centidegrees = 0;
static bool thermostat_heat_demand = false;
static bool thermostat_cool_demand = false;

/* gpio_install_isr_service() may be needed by up to two independent
 * subsystems here (OpenTherm's RX pin, the rotary encoder's A/button
 * pins) — calling it twice returns ESP_ERR_INVALID_STATE, not a crash,
 * but there's no reason to rely on that being harmless. This makes the
 * call idempotent so either/both subsystems can call it freely. */
static bool gpio_isr_service_installed = false;

static void ensure_gpio_isr_service(void)
{
    if (gpio_isr_service_installed) {
        return;
    }
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        gpio_isr_service_installed = true;
    } else {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
    }
}

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

#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_OPENTHERM
/* ======================================================================
 * OpenTherm master driver.
 *
 * Protocol summary — verified directly against the OpenTherm Association's
 * own "OpenTherm Protocol Specification v2.2" (fetched as a PDF and read
 * via pdftotext, not a web summary):
 *
 * Physical layer (section 3): the master (this thermostat) transmits by
 * modulating LINE VOLTAGE — idle <=7V, active 15-18V. The slave (boiler)
 * transmits by modulating LINE CURRENT — idle 5-9mA, active 17-23mA. This
 * is why bare GPIO cannot drive an OpenTherm bus directly (unlike this
 * repo's other bit-banged protocols, e.g. DHT/DS18B20/WS2812B, which are
 * plain single-supply digital logic) — THERMOSTAT_OPENTHERM_IN_GPIO/
 * _OUT_GPIO are meant to connect to an OpenTherm adapter board's
 * logic-level pins (e.g. Ihor Melnyk's widely-used opto-isolated design,
 * the de facto DIY/ESPHome/Home-Assistant reference hardware for this),
 * which does the actual voltage/current-loop driving and sensing.
 *
 * Data-link layer (section 4.2): 34-bit frames — 1 start bit ('1'), 32
 * data bits, 1 stop bit ('1'). The 32 data bits are: 1 parity bit P (set
 * so the total number of '1' bits across all 32 is EVEN) + 3-bit
 * MSG-TYPE + 4 spare bits (always 0) + 8-bit DATA-ID + 16-bit DATA-VALUE.
 * MSG-TYPE values used here: READ-DATA=0b000, WRITE-DATA=0b001 (master to
 * slave); READ-ACK=0b100, WRITE-ACK=0b101, DATA-INVALID=0b110,
 * UNKNOWN-DATAID=0b111 (slave to master).
 *
 * Bit-level signalling (section 3.4): Manchester/Bi-phase-L encoding at
 * 1000 bits/sec nominal (900-1150us per bit, timing reset on every
 * transition). Bit '1' = active-to-idle transition mid-bit; bit '0' =
 * idle-to-active transition mid-bit.
 *
 * Conversation timing (section 4.3.1): the master sends one frame, the
 * slave must reply within 20-800ms, and the master must wait >=100ms
 * before starting the next conversation. The master must communicate at
 * least once a second (+15% tolerance) — see
 * THERMOSTAT_OPENTHERM_POLL_INTERVAL_MS above — or a compliant boiler
 * falls back to a basic "short-circuit" on/off compatibility mode
 * (section 3.5), losing modulating control.
 *
 * Data-IDs used (section 5.3, "mandatory" data items plus the room-unit
 * side of Class 1): id=0 Status (master writes CH-enable/Cooling-enable
 * bits in the high byte, READ-DATA; slave replies with its own status in
 * the low byte), id=1 Control setpoint (WRITE-DATA, f8.8 degC — see
 * THERMOSTAT_OPENTHERM_CH_SETPOINT_C above for why a fixed value), id=16
 * Room Setpoint (WRITE-DATA, f8.8 degC, informational), id=24 Room
 * temperature (WRITE-DATA, f8.8 degC, informational). id=25 Boiler
 * temperature and id=17 Relative modulation level are read back
 * (READ-DATA) purely for diagnostic logging — this device doesn't act on
 * either. f8.8 is a signed fixed-point format: 1 sign bit, 7 integer
 * bits, 8 fractional bits, i.e. the raw 16-bit value divided by 256.0.
 *
 * GPIO-level driver logic (bit-banged TX via esp_rom_delay_us, edge-
 * interrupt-driven RX state machine, response/frame timeouts) is ported
 * from Ihor Melnyk's opentherm_library (github.com/ihormelnyk/
 * opentherm_library, MIT-licensed) — the reference implementation this
 * whole DIY/ESPHome/Home-Assistant OpenTherm community has standardized
 * on, itself built directly against this same spec. Ported to ESP-IDF's
 * gpio_isr_handler_add()+esp_timer_get_time() in place of Arduino's
 * attachInterrupt()+micros(), logic otherwise unchanged.
 *
 * Deliberately does NOT disable interrupts around the ~34ms TX bit-bang
 * sequence the way this repo's DHT/DS18B20 drivers disable them for their
 * own (much shorter, ~1-5ms) transactions — 34ms with interrupts off
 * would risk disrupting Wi-Fi/BLE badly enough to matter, and the
 * reference implementation itself doesn't disable interrupts during
 * sendBit() either (only very briefly, around its own internal state
 * checks) — matching that decision rather than a "more careful sounding"
 * change no other OpenTherm implementation actually makes. The spec's
 * generous +-15% per-bit tolerance is what makes this acceptable — an
 * occasional few-hundred-microsecond scheduling jitter, if it ever
 * happens at all, is expected to still land inside the acceptable window.
 * ====================================================================== */

typedef enum {
    OT_NOT_INITIALIZED,
    OT_READY,
    OT_DELAY,
    OT_RESPONSE_WAITING,
    OT_RESPONSE_START_BIT,
    OT_RESPONSE_RECEIVING,
    OT_RESPONSE_READY,
    OT_RESPONSE_INVALID,
} ot_status_t;

typedef enum {
    OT_MSG_READ_DATA = 0b000,
    OT_MSG_WRITE_DATA = 0b001,
    OT_MSG_READ_ACK = 0b100,
    OT_MSG_WRITE_ACK = 0b101,
    OT_MSG_DATA_INVALID = 0b110,
    OT_MSG_UNKNOWN_DATA_ID = 0b111,
} ot_msg_type_t;

#define OT_ID_STATUS 0
#define OT_ID_CONTROL_SETPOINT 1
#define OT_ID_ROOM_SETPOINT 16
#define OT_ID_ROOM_TEMPERATURE 24
#define OT_ID_RELATIVE_MODULATION_LEVEL 17
#define OT_ID_BOILER_TEMPERATURE 25

static volatile ot_status_t ot_status = OT_NOT_INITIALIZED;
static volatile uint32_t ot_response = 0;
static volatile int ot_response_bit_index = 0;
static volatile int64_t ot_response_timestamp = 0;

static void ot_set_active(void)
{
    /* Matches the reference adapter board's polarity: pulling the output
     * pin LOW switches the line into its "active" high-voltage state. */
    gpio_set_level(THERMOSTAT_OPENTHERM_OUT_GPIO, 0);
}

static void ot_set_idle(void)
{
    gpio_set_level(THERMOSTAT_OPENTHERM_OUT_GPIO, 1);
}

/* Manchester-encodes and sends one bit: a '1' is an active-to-idle
 * transition at the bit's midpoint, a '0' is idle-to-active — see the
 * header comment above. 500us + 500us = 1000us total, the spec's nominal
 * bit period. */
static void ot_send_bit(bool high)
{
    if (high) {
        ot_set_active();
    } else {
        ot_set_idle();
    }
    esp_rom_delay_us(500);
    if (high) {
        ot_set_idle();
    } else {
        ot_set_active();
    }
    esp_rom_delay_us(500);
}

/* Edge-interrupt handler on THERMOSTAT_OPENTHERM_IN_GPIO — reconstructs
 * the slave's Manchester-encoded response one bit at a time. Faithful
 * port of opentherm_library's handleInterrupt(); see the header comment
 * above for the sourcing. Only counts transitions that land more than
 * 750us after the last counted one — since a Manchester bit period is
 * ~1000us with a transition at its midpoint (and sometimes also one at
 * its boundary, if consecutive bits share a value), this reliably picks
 * out just the midpoint transition of each bit regardless of which
 * pattern occurred. */
static void IRAM_ATTR ot_gpio_isr_handler(void *arg)
{
    int64_t now = esp_timer_get_time();
    int level = gpio_get_level(THERMOSTAT_OPENTHERM_IN_GPIO);

    if (ot_status == OT_RESPONSE_WAITING) {
        if (level == 1) {
            ot_status = OT_RESPONSE_START_BIT;
            ot_response_timestamp = now;
        } else {
            ot_status = OT_RESPONSE_INVALID;
            ot_response_timestamp = now;
        }
    } else if (ot_status == OT_RESPONSE_START_BIT) {
        if ((now - ot_response_timestamp) < 750 && level == 0) {
            ot_status = OT_RESPONSE_RECEIVING;
            ot_response_timestamp = now;
            ot_response_bit_index = 0;
            ot_response = 0;
        } else {
            ot_status = OT_RESPONSE_INVALID;
            ot_response_timestamp = now;
        }
    } else if (ot_status == OT_RESPONSE_RECEIVING) {
        if ((now - ot_response_timestamp) > 750) {
            if (ot_response_bit_index < 32) {
                ot_response = (ot_response << 1) | (level ? 0u : 1u);
                ot_response_timestamp = now;
                ot_response_bit_index++;
            } else {
                ot_status = OT_RESPONSE_READY;
                ot_response_timestamp = now;
            }
        }
    }
}

/* Even parity over all 32 bits (P included) — sets bit 31 (the parity
 * bit position) so the total '1'-bit count of the whole frame is even,
 * per section 4.2.1. */
static uint32_t ot_add_parity(uint32_t frame)
{
    uint32_t count = 0;
    for (uint32_t f = frame; f; f >>= 1) {
        count += (f & 1);
    }
    if (count & 1) {
        frame |= (1UL << 31);
    }
    return frame;
}

static uint32_t ot_build_request(ot_msg_type_t type, uint8_t data_id, uint16_t data_value)
{
    uint32_t frame = ((uint32_t)type << 28) | ((uint32_t)data_id << 16) | data_value;
    return ot_add_parity(frame);
}

/* f8.8: 1 sign bit, 7 integer bits, 8 fractional bits — raw value / 256. */
static uint16_t ot_float_to_f88(float value)
{
    return (uint16_t)(int16_t)(value * 256.0f);
}

static float ot_f88_to_float(uint16_t raw)
{
    return (int16_t)raw / 256.0f;
}

/* Sends one request and blocks (this runs in its own FreeRTOS task, not
 * the Matter event loop) for the slave's reply — up to the spec's own
 * worst case (800ms) plus margin. Returns true and fills *response_value
 * on a valid READ-ACK/WRITE-ACK; false on timeout, DATA-INVALID,
 * UNKNOWN-DATA-ID, or a Manchester/parity error. */
static bool ot_send_request(ot_msg_type_t type, uint8_t data_id, uint16_t data_value, uint16_t *response_value)
{
    uint32_t request = ot_build_request(type, data_id, data_value);

    ot_response_bit_index = 0;
    ot_response = 0;
    ot_status = OT_RESPONSE_WAITING; /* armed before the first bit goes out, so no reply can be missed */

    ot_send_bit(true); /* start bit */
    for (int i = 31; i >= 0; i--) {
        ot_send_bit((request >> i) & 1);
    }
    ot_send_bit(true); /* stop bit */
    ot_set_idle();

    ot_response_timestamp = esp_timer_get_time();

    int64_t deadline = esp_timer_get_time() + 900000; /* spec max is 800ms; generous margin */
    bool got_reply = false;
    while (esp_timer_get_time() < deadline) {
        ot_status_t st = ot_status;
        if (st == OT_RESPONSE_READY) {
            got_reply = true;
            break;
        }
        if (st == OT_RESPONSE_INVALID) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    /* Spec: wait >=100ms from the end of this conversation before the
     * next one starts. */
    ot_status = OT_DELAY;
    vTaskDelay(pdMS_TO_TICKS(100));
    ot_status = OT_READY;

    if (!got_reply) {
        ESP_LOGW(TAG, "OpenTherm: no valid reply for data-id %u (timeout or framing error)", data_id);
        return false;
    }

    uint32_t response = ot_response;
    uint32_t parity_check = 0;
    for (uint32_t f = response; f; f >>= 1) {
        parity_check += (f & 1);
    }
    if (parity_check & 1) {
        ESP_LOGW(TAG, "OpenTherm: parity error on reply for data-id %u", data_id);
        return false;
    }

    ot_msg_type_t reply_type = (ot_msg_type_t)((response >> 28) & 0x7);
    if (reply_type != OT_MSG_READ_ACK && reply_type != OT_MSG_WRITE_ACK) {
        ESP_LOGW(TAG, "OpenTherm: data-id %u replied with type %d (not ACK)", data_id, (int)reply_type);
        return false;
    }

    if (response_value) {
        *response_value = (uint16_t)(response & 0xFFFF);
    }
    return true;
}

static void ot_master_setup(void)
{
    gpio_config_t out_conf = {};
    out_conf.pin_bit_mask = (1ULL << THERMOSTAT_OPENTHERM_OUT_GPIO);
    out_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out_conf);
    ot_set_idle();

    gpio_config_t in_conf = {};
    in_conf.pin_bit_mask = (1ULL << THERMOSTAT_OPENTHERM_IN_GPIO);
    in_conf.mode = GPIO_MODE_INPUT;
    in_conf.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&in_conf);

    ensure_gpio_isr_service();
    gpio_isr_handler_add(THERMOSTAT_OPENTHERM_IN_GPIO, ot_gpio_isr_handler, NULL);

    ot_status = OT_READY;
    /* Section 3.5: line held idle for 1s+ before the first real frame
     * gives the boiler's own short-circuit-detection logic time to settle
     * rather than possibly reacting to a mid-boot glitch as a fault. */
    vTaskDelay(pdMS_TO_TICKS(1000));
}

/* Sends one full poll cycle: Status (CH/Cooling-enable), Control setpoint
 * (only meaningful while CH is enabled), Room Setpoint, Room temperature
 * — then, purely for diagnostics, reads back Boiler temperature and
 * Relative modulation level. Called at THERMOSTAT_OPENTHERM_POLL_INTERVAL_MS
 * from the control task below, using whatever thermostat_heat_demand/
 * thermostat_cool_demand the hysteresis logic last decided. */
static void ot_master_poll(void)
{
    uint16_t response = 0;

    uint16_t status_data = (uint16_t)((thermostat_heat_demand ? 1 : 0) | (thermostat_cool_demand ? (1 << 2) : 0)) << 8;
    if (ot_send_request(OT_MSG_READ_DATA, OT_ID_STATUS, status_data, &response)) {
        bool slave_fault = response & 0x01;
        bool slave_ch_active = response & 0x02;
        bool slave_flame_on = response & 0x08;
        ESP_LOGI(TAG, "OpenTherm status: fault=%d CH-active=%d flame=%d",
                 slave_fault, slave_ch_active, slave_flame_on);
    }

    if (thermostat_heat_demand) {
        ot_send_request(OT_MSG_WRITE_DATA, OT_ID_CONTROL_SETPOINT,
                         ot_float_to_f88(THERMOSTAT_OPENTHERM_CH_SETPOINT_C), NULL);
    }

    ot_send_request(OT_MSG_WRITE_DATA, OT_ID_ROOM_SETPOINT,
                     ot_float_to_f88(thermostat_heating_setpoint_centidegrees / 100.0f), NULL);

    if (thermostat_local_temperature_valid) {
        ot_send_request(OT_MSG_WRITE_DATA, OT_ID_ROOM_TEMPERATURE,
                         ot_float_to_f88(thermostat_local_temperature_centidegrees / 100.0f), NULL);
    }

    if (ot_send_request(OT_MSG_READ_DATA, OT_ID_BOILER_TEMPERATURE, 0, &response)) {
        ESP_LOGI(TAG, "OpenTherm boiler water temperature: %.1f degC", ot_f88_to_float(response));
    }
    if (ot_send_request(OT_MSG_READ_DATA, OT_ID_RELATIVE_MODULATION_LEVEL, 0, &response)) {
        ESP_LOGI(TAG, "OpenTherm relative modulation: %.1f%%", ot_f88_to_float(response));
    }
}

static void ot_master_task(void *arg)
{
    ot_master_setup();
    for (;;) {
        ot_master_poll();
        vTaskDelay(pdMS_TO_TICKS(THERMOSTAT_OPENTHERM_POLL_INTERVAL_MS));
    }
}
#endif /* THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_OPENTHERM */

/* LocalTemperature is a plain ember attribute (see the header comment
 * above) — a direct attribute::update() call, no SetMeasuredValue()-style
 * setter needed, unlike firmware/temperature-sensor/'s TemperatureMeasurement.
 * Takes esp-matter's own nullable<T> (matching cluster::thermostat::config_t's
 * own local_temperature field type) rather than chip::app::DataModel::Nullable<T>
 * — see firmware/window-covering/main/app_main.cpp's header comment on why
 * those two nullable wrapper types are NOT interchangeable. */
static void update_local_temperature(nullable<int16_t> value)
{
    esp_matter_attr_val_t val = esp_matter_nullable_int16(value);
    attribute::update(thermostat_endpoint_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
}

#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_RELAY
static void set_heat_relay(bool on)
{
    /* Active-LOW — see the header comment above. */
    gpio_set_level(THERMOSTAT_HEAT_RELAY_GPIO, on ? 0 : 1);
}

static void set_cool_relay(bool on)
{
    gpio_set_level(THERMOSTAT_COOL_RELAY_GPIO, on ? 0 : 1);
}
#endif

#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_BINDING
/* Same client-invoke pattern firmware/switch/'s buttons use — see the
 * header comment above for why On/Off (a specific target state) rather
 * than Toggle. Registered once for the whole device in app_main(), same
 * as firmware/switch/'s single global client::set_request_callback(). */
static void app_client_invoke_success_cb(void *context, const chip::app::ConcreteCommandPath &command_path,
                                         const chip::app::StatusIB &status, chip::TLV::TLVReader *response_data)
{
    ESP_LOGI(TAG, "Bound relay module acknowledged the command");
}

static void app_client_invoke_failure_cb(void *context, CHIP_ERROR error)
{
    ESP_LOGW(TAG, "Command to bound relay module failed: %" CHIP_ERROR_FORMAT, error.Format());
}

static void app_client_request_cb(client::peer_device_t *peer_device, client::request_handle_t *req_handle, void *priv_data)
{
    if (req_handle->type != client::INVOKE_CMD) {
        return;
    }
    if (req_handle->command_path.mClusterId != OnOff::Id) {
        ESP_LOGW(TAG, "Ignoring invoke request for unsupported cluster 0x%04lx",
                 (unsigned long)req_handle->command_path.mClusterId);
        return;
    }
    client::interaction::invoke::send_request(NULL, peer_device, req_handle->command_path, "{}",
                                               app_client_invoke_success_cb, app_client_invoke_failure_cb,
                                               chip::NullOptional);
}

/* Sends a real On or Off command (never Toggle) to whatever this
 * endpoint's Binding cluster is bound to — heat demand only, see the
 * header comment above for why. */
static void send_heat_demand_via_binding(bool on)
{
    client::request_handle_t req_handle;
    req_handle.type = client::INVOKE_CMD;
    req_handle.command_path.mClusterId = OnOff::Id;
    req_handle.command_path.mCommandId = on ? OnOff::Commands::On::Id : OnOff::Commands::Off::Id;

    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    client::cluster_update(thermostat_endpoint_id, &req_handle);
}
#endif /* THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_BINDING */

/* Applies whatever thermostat_heat_demand/thermostat_cool_demand the
 * hysteresis logic below just decided, via whichever
 * THERMOSTAT_OUTPUT_TYPE is compiled in. RELAY/BINDING act immediately on
 * a change; OPENTHERM doesn't need an explicit push here — its own
 * ot_master_task polls thermostat_heat_demand/thermostat_cool_demand on
 * its own schedule (the spec requires a steady heartbeat regardless of
 * whether demand actually changed). */
static void apply_demand_outputs(bool heat_changed, bool cool_changed)
{
#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_RELAY
    if (heat_changed) {
        set_heat_relay(thermostat_heat_demand);
    }
    if (cool_changed) {
        set_cool_relay(thermostat_cool_demand);
    }
#elif THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_BINDING
    if (heat_changed) {
        send_heat_demand_via_binding(thermostat_heat_demand);
    }
    if (cool_changed) {
        ESP_LOGW(TAG, "Cool demand changed but BINDING output mode is heat-only — no target to notify. "
                       "Use RELAY or OPENTHERM output for cooling.");
    }
#else
    (void)heat_changed;
    (void)cool_changed;
#endif
}

/* The actual bang-bang (hysteresis) control decision — see
 * THERMOSTAT_HYSTERESIS_CENTIDEGREES's comment above for why a plain
 * threshold isn't used. Only acts while thermostat_local_temperature_valid
 * (an unknown room temperature must never be treated as "cold enough to
 * heat" or "warm enough to cool"). Keeps the PREVIOUS demand state inside
 * the deadband — a hysteresis band means "don't switch yet", not
 * "switch off". */
static void run_control_loop(void)
{
    bool new_heat_demand = thermostat_heat_demand;
    bool new_cool_demand = thermostat_cool_demand;

    if (!thermostat_local_temperature_valid || thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kOff)) {
        new_heat_demand = false;
        new_cool_demand = false;
    } else if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat)) {
        new_cool_demand = false;
        if (thermostat_local_temperature_centidegrees <= thermostat_heating_setpoint_centidegrees - THERMOSTAT_HYSTERESIS_CENTIDEGREES) {
            new_heat_demand = true;
        } else if (thermostat_local_temperature_centidegrees >= thermostat_heating_setpoint_centidegrees + THERMOSTAT_HYSTERESIS_CENTIDEGREES) {
            new_heat_demand = false;
        }
    } else if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool)) {
        new_heat_demand = false;
        if (thermostat_local_temperature_centidegrees >= thermostat_cooling_setpoint_centidegrees + THERMOSTAT_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = true;
        } else if (thermostat_local_temperature_centidegrees <= thermostat_cooling_setpoint_centidegrees - THERMOSTAT_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = false;
        }
    } else {
        /* Any other SystemMode value (Auto/EmergencyHeat/Precooling/
         * FanOnly/Dry/Sleep) isn't implemented — see the header comment
         * on feature scope. Treat as Off rather than guessing. */
        new_heat_demand = false;
        new_cool_demand = false;
    }

    bool heat_changed = (new_heat_demand != thermostat_heat_demand);
    bool cool_changed = (new_cool_demand != thermostat_cool_demand);
    thermostat_heat_demand = new_heat_demand;
    thermostat_cool_demand = new_cool_demand;

    if (heat_changed || cool_changed) {
        ESP_LOGI(TAG, "Demand changed: heat=%s cool=%s (room %.2f degC, heat setpoint %.2f degC, cool setpoint %.2f degC)",
                 thermostat_heat_demand ? "ON" : "off", thermostat_cool_demand ? "ON" : "off",
                 thermostat_local_temperature_centidegrees / 100.0f,
                 thermostat_heating_setpoint_centidegrees / 100.0f,
                 thermostat_cooling_setpoint_centidegrees / 100.0f);
    }
    apply_demand_outputs(heat_changed, cool_changed);
}

/* Periodically reads the sensor, pushes LocalTemperature, and re-runs the
 * control loop. Runs as its own task rather than inline in app_main() so
 * it can freely block on I2C/bit-banged transactions and vTaskDelay()
 * without holding up Matter's own startup/event handling — same
 * reasoning as firmware/temperature-sensor/'s sensor_task(). */
static void sensor_task(void *arg)
{
    for (;;) {
        float temperature_c = 0.0f;
        float humidity_pct = 0.0f; /* read but unused — see the header comment above */

        if (sensor_read(&temperature_c, &humidity_pct)) {
            int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
            ESP_LOGI(TAG, "Sensor: %.2f degC", temperature_c);
            thermostat_local_temperature_valid = true;
            thermostat_local_temperature_centidegrees = temp_centidegrees;
            update_local_temperature(nullable<int16_t>(temp_centidegrees));
        } else {
            thermostat_local_temperature_valid = false;
            update_local_temperature(nullable<int16_t>());
        }

        run_control_loop();

        vTaskDelay(pdMS_TO_TICKS(SENSOR_MEASURE_INTERVAL_MS));
    }
}

/* ======================================================================
 * Rotary encoder — see ROTARY_ENCODER_A_GPIO's header comment above for
 * the overall behaviour. Standard quadrature decoding technique (not
 * chip/vendor-specific — the same generic two-bit Gray-code sequence
 * every incremental mechanical/optical rotary encoder produces): channel
 * A's falling edge is the trigger, channel B's level at that instant
 * gives direction (B high = one rotation direction, B low = the other) —
 * the common simplified "sample the other channel on one channel's edge"
 * approach used by countless KY-040-class encoder drivers, adequate for
 * a detented mechanical encoder's low rotation speed. A minimum time
 * between accepted steps acts as a debounce against contact bounce,
 * mirroring the spirit (not the exact mechanism) of
 * firmware/switch/'s own button debounce. The push-button reuses that
 * same debounce-by-polling shape directly: an ISR queues the raw edge,
 * a task debounces it before acting.
 * ====================================================================== */
static QueueHandle_t encoder_button_evt_queue = NULL;

static void IRAM_ATTR encoder_rotation_isr_handler(void *arg)
{
    /* Runs on channel A's falling edge only (see ROTARY_ENCODER_A_GPIO's
     * config below) — sample B right now to get direction. */
    static int64_t last_step_us = 0;
    int64_t now = esp_timer_get_time();
    if (now - last_step_us < 2000) {
        return; /* debounce: ignore steps closer together than 2ms */
    }
    last_step_us = now;

    int direction = gpio_get_level(ROTARY_ENCODER_B_GPIO) ? 1 : -1;
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(encoder_button_evt_queue, &direction, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR encoder_button_isr_handler(void *arg)
{
    int zero = 0; /* 0 is not a valid rotation direction — marks "this is a button event" */
    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(encoder_button_evt_queue, &zero, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

/* Writes a new SystemMode/setpoint straight into the real Matter
 * attribute — the same effect a controller's own write would have,
 * confirmed by using the identical esp_matter_uint8()/esp_matter_int16()
 * + attribute::update() call app_attribute_update_cb reacts to, just
 * initiated locally instead of by a PRE_UPDATE callback. Also updates the
 * local shadow variable directly (rather than relying on PRE_UPDATE firing
 * for our own write) and re-runs the control loop immediately. */
static void encoder_apply_system_mode(uint8_t new_mode)
{
    thermostat_system_mode = new_mode;
    esp_matter_attr_val_t val = esp_matter_uint8(new_mode);
    attribute::update(thermostat_endpoint_id, Thermostat::Id, Thermostat::Attributes::SystemMode::Id, &val);
    ESP_LOGI(TAG, "Encoder: SystemMode set to %u", new_mode);
    run_control_loop();
}

static void encoder_adjust_setpoint(int direction)
{
    if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat)) {
        thermostat_heating_setpoint_centidegrees += direction * ROTARY_ENCODER_STEP_CENTIDEGREES;
        esp_matter_attr_val_t val = esp_matter_int16(thermostat_heating_setpoint_centidegrees);
        attribute::update(thermostat_endpoint_id, Thermostat::Id, Thermostat::Attributes::OccupiedHeatingSetpoint::Id, &val);
        ESP_LOGI(TAG, "Encoder: heating setpoint now %.2f degC", thermostat_heating_setpoint_centidegrees / 100.0f);
    } else if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool)) {
        thermostat_cooling_setpoint_centidegrees += direction * ROTARY_ENCODER_STEP_CENTIDEGREES;
        esp_matter_attr_val_t val = esp_matter_int16(thermostat_cooling_setpoint_centidegrees);
        attribute::update(thermostat_endpoint_id, Thermostat::Id, Thermostat::Attributes::OccupiedCoolingSetpoint::Id, &val);
        ESP_LOGI(TAG, "Encoder: cooling setpoint now %.2f degC", thermostat_cooling_setpoint_centidegrees / 100.0f);
    } else {
        return; /* Off — rotation has nothing to adjust */
    }
    run_control_loop();
}

static void encoder_task(void *arg)
{
    int event;
    for (;;) {
        if (xQueueReceive(encoder_button_evt_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (event != 0) {
            encoder_adjust_setpoint(event);
            continue;
        }

        /* Button event — debounce the same way firmware/switch/'s button
         * does (require continuously-low samples), then cycle
         * Off -> Heat -> Cool -> Off. */
        bool confirmed = true;
        for (int i = 0; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            if (gpio_get_level(ROTARY_ENCODER_BUTTON_GPIO) != 0) {
                confirmed = false;
            }
        }
        if (!confirmed) {
            xQueueReset(encoder_button_evt_queue);
            continue;
        }

        uint8_t next_mode;
        if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kOff)) {
            next_mode = chip::to_underlying(Thermostat::SystemModeEnum::kHeat);
        } else if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat)) {
            next_mode = chip::to_underlying(Thermostat::SystemModeEnum::kCool);
        } else {
            next_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
        }
        encoder_apply_system_mode(next_mode);

        while (gpio_get_level(ROTARY_ENCODER_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        xQueueReset(encoder_button_evt_queue);
    }
}

#if DISPLAY_TYPE != DISPLAY_NONE
/* ======================================================================
 * Local display — see DISPLAY_TYPE's header comment above for the full
 * per-chip protocol sourcing. Rendering is deliberately NOT a bitmap-font
 * text renderer: just large 7-segment-style digits (drawn as filled
 * rectangles — a well-understood, simple technique, the same shape real
 * numeric-only thermostat/clock displays actually use) for the
 * temperature reading, plus a colored border/bar for SystemMode +
 * heat/cool demand (red border/fill = heat demand, blue = cool demand,
 * dim gray = off — color instead of text, avoiding the need for any font
 * table at all, and arguably a closer match to how a real thermostat like
 * Nest indicates state than a text label would be). SSD1306 is
 * monochrome — same digit shapes, drawn as filled-vs-not, no color.
 * ====================================================================== */

#if DISPLAY_IS_SPI
static spi_device_handle_t display_spi_dev = NULL;

static void display_write_command(uint8_t cmd)
{
    gpio_set_level(DISPLAY_DC_GPIO, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(display_spi_dev, &t);
}

static void display_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    gpio_set_level(DISPLAY_DC_GPIO, 1);
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(display_spi_dev, &t);
}

static void display_write_data_byte(uint8_t data)
{
    display_write_data(&data, 1);
}

/* CASET/RASET/RAMWR — standard MIPI-DCS, identical on GC9A01 and ST7789
 * (confirmed directly in both chips' own datasheets). */
static void display_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col[4] = { (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF), (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF) };
    display_write_command(0x2A);
    display_write_data(col, sizeof(col));

    uint8_t row[4] = { (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF), (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF) };
    display_write_command(0x2B);
    display_write_data(row, sizeof(row));

    display_write_command(0x2C); /* Memory Write — following data bytes are pixel data */
}

/* Fills a rectangle with a solid RGB565 color — the only drawing
 * primitive the 7-segment digit renderer below actually needs. Sent in
 * chunks (not the whole rectangle in one SPI transaction) to keep each
 * transaction's buffer a fixed, modest size regardless of rectangle
 * size. */
static void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color565)
{
    if (w == 0 || h == 0) {
        return;
    }
#if DISPLAY_TYPE == DISPLAY_GC9A01
    display_set_addr_window(x, y, x + w - 1, y + h - 1);
#else /* DISPLAY_ST7789 */
    display_set_addr_window(x + DISPLAY_ST7789_COL_OFFSET, y + DISPLAY_ST7789_ROW_OFFSET,
                             x + w - 1 + DISPLAY_ST7789_COL_OFFSET, y + h - 1 + DISPLAY_ST7789_ROW_OFFSET);
#endif

    uint8_t hi = (uint8_t)(color565 >> 8);
    uint8_t lo = (uint8_t)(color565 & 0xFF);
    uint8_t chunk[64];
    for (size_t i = 0; i < sizeof(chunk); i += 2) {
        chunk[i] = hi;
        chunk[i + 1] = lo;
    }

    gpio_set_level(DISPLAY_DC_GPIO, 1);
    uint32_t pixels_remaining = (uint32_t)w * h;
    while (pixels_remaining > 0) {
        uint32_t pixels_this_chunk = pixels_remaining < (sizeof(chunk) / 2) ? pixels_remaining : (sizeof(chunk) / 2);
        spi_transaction_t t = {};
        t.length = pixels_this_chunk * 2 * 8;
        t.tx_buffer = chunk;
        spi_device_polling_transmit(display_spi_dev, &t);
        pixels_remaining -= pixels_this_chunk;
    }
}

/* RGB565: 5 bits red, 6 bits green, 5 bits blue. */
static uint16_t display_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void display_hw_reset(void)
{
    gpio_set_level(DISPLAY_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(DISPLAY_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

#if DISPLAY_TYPE == DISPLAY_GC9A01
/* Ported verbatim (register bytes + parameter bytes, same order) from
 * moononournation/Arduino_GFX's Arduino_GC9A01 driver — see the header
 * comment above for why: the chip's own datasheet leaves most of this
 * undocumented. */
static void display_init_panel(void)
{
    display_hw_reset();
    display_write_command(0xEF);
    display_write_command(0xEB);
    display_write_data_byte(0x14);
    display_write_command(0xFE);
    display_write_command(0xEF);
    display_write_command(0xEB);
    display_write_data_byte(0x14);
    display_write_command(0x84);
    display_write_data_byte(0x40);
    display_write_command(0x85);
    display_write_data_byte(0xFF);
    display_write_command(0x86);
    display_write_data_byte(0xFF);
    display_write_command(0x87);
    display_write_data_byte(0xFF);
    display_write_command(0x88);
    display_write_data_byte(0x0A);
    display_write_command(0x89);
    display_write_data_byte(0x21);
    display_write_command(0x8A);
    display_write_data_byte(0x00);
    display_write_command(0x8B);
    display_write_data_byte(0x80);
    display_write_command(0x8C);
    display_write_data_byte(0x01);
    display_write_command(0x8D);
    display_write_data_byte(0x01);
    display_write_command(0x8E);
    display_write_data_byte(0xFF);
    display_write_command(0x8F);
    display_write_data_byte(0xFF);
    display_write_command(0xB6);
    { uint8_t d[2] = { 0x00, 0x20 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x3A);
    display_write_data_byte(0x05); /* COLMOD: 16bpp/RGB565 */
    display_write_command(0x90);
    { uint8_t d[4] = { 0x08, 0x08, 0x08, 0x08 }; display_write_data(d, sizeof(d)); }
    display_write_command(0xBD);
    display_write_data_byte(0x06);
    display_write_command(0xBC);
    display_write_data_byte(0x00);
    display_write_command(0xFF);
    { uint8_t d[3] = { 0x60, 0x01, 0x04 }; display_write_data(d, sizeof(d)); }
    display_write_command(0xC3);
    display_write_data_byte(0x13);
    display_write_command(0xC4);
    display_write_data_byte(0x13);
    display_write_command(0xC9);
    display_write_data_byte(0x22);
    display_write_command(0xBE);
    display_write_data_byte(0x11);
    display_write_command(0xE1);
    { uint8_t d[2] = { 0x10, 0x0E }; display_write_data(d, sizeof(d)); }
    display_write_command(0xDF);
    { uint8_t d[3] = { 0x21, 0x0C, 0x02 }; display_write_data(d, sizeof(d)); }
    display_write_command(0xF0);
    { uint8_t d[6] = { 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A }; display_write_data(d, sizeof(d)); }
    display_write_command(0xF1);
    { uint8_t d[6] = { 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F }; display_write_data(d, sizeof(d)); }
    display_write_command(0xF2);
    { uint8_t d[6] = { 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A }; display_write_data(d, sizeof(d)); }
    display_write_command(0xF3);
    { uint8_t d[6] = { 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F }; display_write_data(d, sizeof(d)); }
    display_write_command(0xED);
    { uint8_t d[2] = { 0x1B, 0x0B }; display_write_data(d, sizeof(d)); }
    display_write_command(0xAE);
    display_write_data_byte(0x77);
    display_write_command(0xCD);
    display_write_data_byte(0x63);
    display_write_command(0x70);
    { uint8_t d[9] = { 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03 }; display_write_data(d, sizeof(d)); }
    display_write_command(0xE8);
    display_write_data_byte(0x34);
    display_write_command(0x62);
    { uint8_t d[12] = { 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x63);
    { uint8_t d[12] = { 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x64);
    { uint8_t d[7] = { 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x66);
    { uint8_t d[10] = { 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x67);
    { uint8_t d[10] = { 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x74);
    { uint8_t d[7] = { 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x98);
    { uint8_t d[2] = { 0x3E, 0x07 }; display_write_data(d, sizeof(d)); }
    display_write_command(0x35); /* Tearing effect line ON */
    display_write_command(0x11); /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));
    display_write_command(0x29); /* Display ON */
    vTaskDelay(pdMS_TO_TICKS(20));
}
#else /* DISPLAY_ST7789 */
/* Standard, fully-documented ST7789 init — no undocumented registers
 * needed (unlike GC9A01 above). */
static void display_init_panel(void)
{
    display_hw_reset();
    display_write_command(0x01); /* Software reset */
    vTaskDelay(pdMS_TO_TICKS(150));
    display_write_command(0x11); /* Sleep Out */
    vTaskDelay(pdMS_TO_TICKS(120));
    display_write_command(0x3A); /* COLMOD */
    display_write_data_byte(0x55); /* 16bpp/RGB565 */
    vTaskDelay(pdMS_TO_TICKS(10));
    display_write_command(0x36); /* MADCTL */
    display_write_data_byte(0x00);
    display_write_command(0x21); /* Display Inversion On — most ST7789 panels need this for correct colors */
    display_write_command(0x13); /* Normal Display Mode On */
    display_write_command(0x29); /* Display ON */
    vTaskDelay(pdMS_TO_TICKS(20));
}
#endif

static void display_setup(void)
{
    gpio_config_t dc_conf = {};
    dc_conf.pin_bit_mask = (1ULL << DISPLAY_DC_GPIO) | (1ULL << DISPLAY_RST_GPIO);
    dc_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&dc_conf);

    spi_bus_config_t bus_config = {};
    bus_config.sclk_io_num = DISPLAY_SCLK_GPIO;
    bus_config.mosi_io_num = DISPLAY_MOSI_GPIO;
    bus_config.miso_io_num = -1;
    bus_config.max_transfer_sz = 4096;
    ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_config = {};
    dev_config.clock_speed_hz = 40 * 1000 * 1000;
    dev_config.mode = 0;
    dev_config.spics_io_num = DISPLAY_CS_GPIO;
    dev_config.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(DISPLAY_SPI_HOST, &dev_config, &display_spi_dev));

    display_init_panel();
}

#elif DISPLAY_IS_I2C /* DISPLAY_SSD1306 */
static i2c_master_dev_handle_t display_i2c_dev = NULL;
/* One bit per pixel, page-addressed (8 rows per page) — the SSD1306's
 * own GDDRAM layout, not an arbitrary choice. */
static uint8_t ssd1306_framebuffer[DISPLAY_SSD1306_WIDTH * DISPLAY_SSD1306_HEIGHT / 8];

static void ssd1306_write_command(uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd }; /* control byte 0x00 = command stream follows */
    i2c_master_transmit(display_i2c_dev, buf, sizeof(buf), 1000);
}

static void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool on)
{
    for (uint16_t yy = y; yy < y + h && yy < DISPLAY_SSD1306_HEIGHT; yy++) {
        for (uint16_t xx = x; xx < x + w && xx < DISPLAY_SSD1306_WIDTH; xx++) {
            size_t index = (size_t)(yy / 8) * DISPLAY_SSD1306_WIDTH + xx;
            uint8_t bit = 1 << (yy % 8);
            if (on) {
                ssd1306_framebuffer[index] |= bit;
            } else {
                ssd1306_framebuffer[index] &= (uint8_t)~bit;
            }
        }
    }
}

static void ssd1306_flush(void)
{
    /* Full-screen page-addressing sweep — set column 0-127 + every page
     * (8px each, height/8 of them), then stream the whole framebuffer.
     * The standard way every open-source SSD1306 driver pushes a full
     * frame. */
    ssd1306_write_command(0x21); /* Column address */
    ssd1306_write_command(0);
    ssd1306_write_command(DISPLAY_SSD1306_WIDTH - 1);
    ssd1306_write_command(0x22); /* Page address */
    ssd1306_write_command(0);
    ssd1306_write_command((DISPLAY_SSD1306_HEIGHT / 8) - 1);

    uint8_t chunk[129];
    chunk[0] = 0x40; /* control byte: data stream follows */
    size_t total = sizeof(ssd1306_framebuffer);
    for (size_t offset = 0; offset < total; offset += 128) {
        size_t n = (total - offset) < 128 ? (total - offset) : 128;
        memcpy(chunk + 1, ssd1306_framebuffer + offset, n);
        i2c_master_transmit(display_i2c_dev, chunk, n + 1, 1000);
    }
}

static void display_setup(void)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = DISPLAY_I2C_PORT;
    bus_config.sda_io_num = DISPLAY_SDA_GPIO;
    bus_config.scl_io_num = DISPLAY_SCL_GPIO;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus));

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = SSD1306_I2C_ADDR;
    dev_config.scl_speed_hz = DISPLAY_I2C_FREQ_HZ;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_config, &display_i2c_dev));

    /* Standard SSD1306 init sequence — see the header comment above. */
    ssd1306_write_command(0xAE); /* Display off */
    ssd1306_write_command(0xD5); ssd1306_write_command(0x80); /* Clock divide */
    ssd1306_write_command(0xA8); ssd1306_write_command(0x3F); /* Multiplex ratio: 64 rows - 1 */
    ssd1306_write_command(0xD3); ssd1306_write_command(0x00); /* Display offset: none */
    ssd1306_write_command(0x40); /* Start line 0 */
    ssd1306_write_command(0x8D); ssd1306_write_command(0x14); /* Charge pump enable */
    ssd1306_write_command(0x20); ssd1306_write_command(0x00); /* Memory addressing mode: horizontal */
    ssd1306_write_command(0xA1); /* Segment remap */
    ssd1306_write_command(0xC8); /* COM scan direction: remapped */
    ssd1306_write_command(0xDA); ssd1306_write_command(0x12); /* COM pins config */
    ssd1306_write_command(0x81); ssd1306_write_command(0xCF); /* Contrast */
    ssd1306_write_command(0xD9); ssd1306_write_command(0xF1); /* Pre-charge period */
    ssd1306_write_command(0xDB); ssd1306_write_command(0x40); /* VCOMH deselect level */
    ssd1306_write_command(0xA4); /* Resume to RAM content */
    ssd1306_write_command(0xA6); /* Normal (non-inverted) display */
    ssd1306_write_command(0xAF); /* Display on */

    memset(ssd1306_framebuffer, 0, sizeof(ssd1306_framebuffer));
}
#endif /* DISPLAY_IS_SPI / DISPLAY_IS_I2C */

/* --- 7-segment digit renderer -------------------------------------------
 * Draws one digit (0-9), a minus sign, or nothing (blank) as filled
 * rectangles forming the classic 7-segment shape — no font table needed.
 * Segment layout (standard 7-segment lettering):
 *      _a_
 *     f   b
 *      _g_
 *     e   c
 *      _d_
 */
typedef struct { uint16_t x, y, w, h; uint16_t color; bool on; } display_seg_t;

static const uint8_t DIGIT_SEGMENTS[11] = {
    /* 0-9 as a,b,c,d,e,f,g bitmask (bit0=a .. bit6=g); index 10 = minus (g only) */
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
    0b1000000,
};

#if DISPLAY_IS_SPI
static void draw_digit(uint16_t x, uint16_t y, uint16_t digit_w, uint16_t digit_h, int value, uint16_t on_color, uint16_t off_color)
{
    if (value < 0 || value > 10) {
        return;
    }
    uint8_t segs = DIGIT_SEGMENTS[value];
    uint16_t thickness = digit_w / 5;
    uint16_t seg_len_h = digit_w - 2 * thickness;
    uint16_t seg_len_v = (digit_h - 3 * thickness) / 2;

    display_seg_t segments[7] = {
        { (uint16_t)(x + thickness), y, seg_len_h, thickness, 0, (bool)(segs & 0x01) },                                             /* a: top */
        { (uint16_t)(x + digit_w - thickness), (uint16_t)(y + thickness), thickness, seg_len_v, 0, (bool)(segs & 0x02) },           /* b: top-right */
        { (uint16_t)(x + digit_w - thickness), (uint16_t)(y + 2 * thickness + seg_len_v), thickness, seg_len_v, 0, (bool)(segs & 0x04) }, /* c: bottom-right */
        { (uint16_t)(x + thickness), (uint16_t)(y + digit_h - thickness), seg_len_h, thickness, 0, (bool)(segs & 0x08) },           /* d: bottom */
        { x, (uint16_t)(y + 2 * thickness + seg_len_v), thickness, seg_len_v, 0, (bool)(segs & 0x10) },                             /* e: bottom-left */
        { x, (uint16_t)(y + thickness), thickness, seg_len_v, 0, (bool)(segs & 0x20) },                                             /* f: top-left */
        { (uint16_t)(x + thickness), (uint16_t)(y + thickness + seg_len_v), seg_len_h, thickness, 0, (bool)(segs & 0x40) },         /* g: middle */
    };
    for (int i = 0; i < 7; i++) {
        display_fill_rect(segments[i].x, segments[i].y, segments[i].w, segments[i].h,
                           segments[i].on ? on_color : off_color);
    }
}
#else /* DISPLAY_SSD1306: same layout, monochrome on/off instead of a color */
static void draw_digit(uint16_t x, uint16_t y, uint16_t digit_w, uint16_t digit_h, int value)
{
    if (value < 0 || value > 10) {
        return;
    }
    uint8_t segs = DIGIT_SEGMENTS[value];
    uint16_t thickness = digit_w / 5 > 0 ? digit_w / 5 : 1;
    uint16_t seg_len_h = digit_w - 2 * thickness;
    uint16_t seg_len_v = (digit_h - 3 * thickness) / 2;

    if (segs & 0x01) display_fill_rect(x + thickness, y, seg_len_h, thickness, true);
    if (segs & 0x02) display_fill_rect(x + digit_w - thickness, y + thickness, thickness, seg_len_v, true);
    if (segs & 0x04) display_fill_rect(x + digit_w - thickness, y + 2 * thickness + seg_len_v, thickness, seg_len_v, true);
    if (segs & 0x08) display_fill_rect(x + thickness, y + digit_h - thickness, seg_len_h, thickness, true);
    if (segs & 0x10) display_fill_rect(x, y + 2 * thickness + seg_len_v, thickness, seg_len_v, true);
    if (segs & 0x20) display_fill_rect(x, y + thickness, thickness, seg_len_v, true);
    if (segs & 0x40) display_fill_rect(x + thickness, y + thickness + seg_len_v, seg_len_h, thickness, true);
}
#endif

/* Renders the current room temperature (2 digits + a decimal indicator
 * dot handled implicitly by digit spacing) and a SystemMode/demand color
 * indicator. Called periodically from display_task() below. */
static void display_render(void)
{
#if DISPLAY_IS_SPI
    uint16_t bg = display_rgb565(0, 0, 0);
    uint16_t mode_color;
    if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat)) {
        mode_color = thermostat_heat_demand ? display_rgb565(255, 60, 0) : display_rgb565(90, 30, 0);
    } else if (thermostat_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool)) {
        mode_color = thermostat_cool_demand ? display_rgb565(0, 120, 255) : display_rgb565(0, 40, 90);
    } else {
        mode_color = display_rgb565(60, 60, 60);
    }

#if DISPLAY_TYPE == DISPLAY_GC9A01
    const uint16_t w = DISPLAY_GC9A01_WIDTH, h = DISPLAY_GC9A01_HEIGHT;
#else
    const uint16_t w = DISPLAY_ST7789_WIDTH, h = DISPLAY_ST7789_HEIGHT;
#endif
    display_fill_rect(0, 0, w, h, bg);
    display_fill_rect(0, 0, w, 6, mode_color); /* top bar = mode/demand indicator */

    if (thermostat_local_temperature_valid) {
        int whole = thermostat_local_temperature_centidegrees / 100;
        bool negative = whole < 0;
        if (negative) {
            whole = -whole;
        }
        int tens = (whole / 10) % 10;
        int ones = whole % 10;
        uint16_t digit_w = w / 4 > 20 ? w / 4 : 20;
        uint16_t digit_h = digit_w * 2;
        uint16_t start_x = (w > digit_w * 2) ? (w - digit_w * 2) / 2 : 0;
        uint16_t start_y = (h > digit_h) ? (h - digit_h) / 2 : 0;
        if (negative) {
            draw_digit(start_x > digit_w / 2 ? start_x - digit_w / 2 : 0, start_y, digit_w / 2, digit_h, 10, mode_color, bg);
        }
        draw_digit(start_x, start_y, digit_w, digit_h, tens, mode_color, bg);
        draw_digit(start_x + digit_w + 4, start_y, digit_w, digit_h, ones, mode_color, bg);
    }
#else /* DISPLAY_SSD1306 */
    memset(ssd1306_framebuffer, 0, sizeof(ssd1306_framebuffer));
    /* Mode/demand indicator: a filled bar at the top when there's active
     * demand, an outline (top+bottom line only) otherwise — no color
     * available on a monochrome display. */
    bool demand_active = thermostat_heat_demand || thermostat_cool_demand;
    display_fill_rect(0, 0, DISPLAY_SSD1306_WIDTH, demand_active ? 4 : 1, true);

    if (thermostat_local_temperature_valid) {
        int whole = thermostat_local_temperature_centidegrees / 100;
        bool negative = whole < 0;
        if (negative) {
            whole = -whole;
        }
        int tens = (whole / 10) % 10;
        int ones = whole % 10;
        uint16_t digit_w = 20, digit_h = 40;
        uint16_t start_x = (DISPLAY_SSD1306_WIDTH - digit_w * 2 - 8) / 2;
        uint16_t start_y = (DISPLAY_SSD1306_HEIGHT - digit_h) / 2;
        if (negative) {
            draw_digit(start_x > 10 ? start_x - 10 : 0, start_y, 8, digit_h, 10);
        }
        draw_digit(start_x, start_y, digit_w, digit_h, tens);
        draw_digit(start_x + digit_w + 8, start_y, digit_w, digit_h, ones);
    }
    ssd1306_flush();
#endif
}

static void display_task(void *arg)
{
    display_setup();
    for (;;) {
        display_render();
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_UPDATE_INTERVAL_MS));
    }
}
#endif /* DISPLAY_TYPE != DISPLAY_NONE */

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

/* Reacts to a controller writing SystemMode/OccupiedHeatingSetpoint/
 * OccupiedCoolingSetpoint — tracks the new value locally (see the header
 * comment above on why) and re-runs the control loop immediately rather
 * than waiting for the next sensor_task cycle, so a setpoint/mode change
 * takes effect right away. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != thermostat_endpoint_id || cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        thermostat_system_mode = val->val.u8;
        ESP_LOGI(TAG, "SystemMode set to %u", thermostat_system_mode);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        thermostat_heating_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Heating setpoint set to %.2f degC", thermostat_heating_setpoint_centidegrees / 100.0f);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        thermostat_cooling_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Cooling setpoint set to %.2f degC", thermostat_cooling_setpoint_centidegrees / 100.0f);
        run_control_loop();
    }
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

    /* 2. Set up the selected local-temperature sensor. */
    if (!sensor_setup()) {
        ESP_LOGE(TAG, "Sensor setup failed — check SENSOR_TYPE and wiring");
        return;
    }

    /* 2b. Configure the heat/cool output for whichever THERMOSTAT_OUTPUT_TYPE
     * is compiled in. RELAY needs its two GPIOs configured up front (idle =
     * relay off); BINDING and OPENTHERM's own setup happens later (BINDING
     * needs no GPIO at all; OPENTHERM's ot_master_task configures its GPIOs
     * itself, once Matter has started, since it also spins up its own
     * FreeRTOS task). */
#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_RELAY
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << THERMOSTAT_HEAT_RELAY_GPIO) | (1ULL << THERMOSTAT_COOL_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    set_heat_relay(false);
    set_cool_relay(false);
#endif

    /* 2c. Configure the identify LED + its blink timer (not started yet —
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

    /* 2e. Configure the optional rotary encoder — only if all three GPIOs
     * are actually wired up (a rotary encoder is useless with only some
     * of its pins connected). */
    bool rotary_encoder_enabled = (ROTARY_ENCODER_A_GPIO != GPIO_NUM_NC) &&
                                   (ROTARY_ENCODER_B_GPIO != GPIO_NUM_NC) &&
                                   (ROTARY_ENCODER_BUTTON_GPIO != GPIO_NUM_NC);
    if (rotary_encoder_enabled) {
        gpio_config_t encoder_a_conf = {};
        encoder_a_conf.pin_bit_mask = (1ULL << ROTARY_ENCODER_A_GPIO);
        encoder_a_conf.mode = GPIO_MODE_INPUT;
        encoder_a_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        encoder_a_conf.intr_type = GPIO_INTR_NEGEDGE;
        gpio_config(&encoder_a_conf);

        gpio_config_t encoder_b_conf = {};
        encoder_b_conf.pin_bit_mask = (1ULL << ROTARY_ENCODER_B_GPIO);
        encoder_b_conf.mode = GPIO_MODE_INPUT;
        encoder_b_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&encoder_b_conf);

        gpio_config_t encoder_button_conf = {};
        encoder_button_conf.pin_bit_mask = (1ULL << ROTARY_ENCODER_BUTTON_GPIO);
        encoder_button_conf.mode = GPIO_MODE_INPUT;
        encoder_button_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        encoder_button_conf.intr_type = GPIO_INTR_NEGEDGE;
        gpio_config(&encoder_button_conf);

        encoder_button_evt_queue = xQueueCreate(10, sizeof(int));
        ensure_gpio_isr_service();
        gpio_isr_handler_add(ROTARY_ENCODER_A_GPIO, encoder_rotation_isr_handler, NULL);
        gpio_isr_handler_add(ROTARY_ENCODER_BUTTON_GPIO, encoder_button_isr_handler, NULL);
    }

    /* 3. Build the Matter data model: one node, one Thermostat endpoint
     * (Identify + Groups + Thermostat cluster, Heat+Cool) — see the
     * header comment above for its exact composition and sourcing. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    thermostat::config_t thermostat_config;
    thermostat_config.thermostat.local_temperature = nullable<int16_t>();
    thermostat_config.thermostat.control_sequence_of_operation =
        chip::to_underlying(Thermostat::ControlSequenceOfOperationEnum::kCoolingAndHeating);
    thermostat_config.thermostat.system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
    thermostat_config.thermostat.feature_flags =
        (uint32_t)Thermostat::Feature::kHeating | (uint32_t)Thermostat::Feature::kCooling;
    thermostat_config.thermostat.features.heating.occupied_heating_setpoint = thermostat_heating_setpoint_centidegrees;
    thermostat_config.thermostat.features.cooling.occupied_cooling_setpoint = thermostat_cooling_setpoint_centidegrees;

    endpoint_t *endpoint = thermostat::create(node, &thermostat_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create thermostat endpoint");
        return;
    }
    thermostat_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Thermostat endpoint id: %u", thermostat_endpoint_id);

#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_BINDING
    /* Add a client-side OnOff cluster + the Binding cluster itself onto
     * this SAME endpoint — the exact pair esp-matter's own
     * on_off_light_switch::add() uses for its equivalent purpose (see
     * the header comment above). */
    cluster::binding::config_t binding_config;
    cluster_t *binding_cluster = cluster::binding::create(endpoint, &binding_config, CLUSTER_FLAG_SERVER);
    if (!binding_cluster) {
        ESP_LOGE(TAG, "Failed to create binding cluster");
        return;
    }
    cluster::on_off::create(endpoint, NULL, CLUSTER_FLAG_CLIENT);
#endif

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

#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_BINDING
    /* Endpoint-agnostic by design (same reasoning as
     * firmware/switch/'s single global registration) — this device only
     * has the one endpoint anyway. binding_manager_init() (which resolves
     * bindings set up via a controller's Binding cluster) runs on its
     * own, inside esp_matter::start() above — no explicit call needed
     * here. */
    client::set_request_callback(app_client_request_cb, NULL, NULL);
#endif

    /* 5. Start reading the sensor + running the control loop now that the
     * data model + Matter stack both exist. */
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);

#if THERMOSTAT_OUTPUT_TYPE == THERMOSTAT_OUTPUT_OPENTHERM
    /* Its own task: OpenTherm's own conversation timing (100ms+ between
     * requests, up to ~900ms per request while waiting for a reply) is
     * unrelated to and slower than sensor_task's polling cadence — keeping
     * them separate means a slow/absent boiler reply can never delay a
     * temperature reading or a SystemMode/setpoint reaction. */
    xTaskCreate(ot_master_task, "ot_master_task", 4096, NULL, 5, NULL);
#endif

    if (rotary_encoder_enabled) {
        xTaskCreate(encoder_task, "encoder_task", 4096, NULL, 5, NULL);
    }

#if DISPLAY_TYPE != DISPLAY_NONE
    /* display_setup() (SPI/I2C bus init + panel init sequence) runs
     * inside display_task() itself, same reasoning as every other
     * peripheral-owning task in this file — keeps app_main() from
     * blocking on it. */
    xTaskCreate(display_task, "display_task", 4096, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "Matter thermostat started. Scan the QR code to commission.");
}
