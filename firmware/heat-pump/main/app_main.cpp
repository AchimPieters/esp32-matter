/*
 * Minimal Matter Heat Pump — thirty-third device type, and this repo's
 * second genuinely composed, multi-endpoint device after firmware/
 * refrigerator/ — but composed very differently, and against real,
 * conflicting guidance from three separate sources that had to be weighed
 * against each other before writing any code (see below).
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
 * --- Three sources, three different answers — what this file actually does
 * and why ------------------------------------------------------------------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * HeatPump.xml: the ROOT endpoint itself is nearly empty — just Identify
 * (optionalConform) and a CLIENT-side Thermostat binding (optionalConform,
 * i.e. this endpoint MAY bind to some other thermostat device — not host one
 * itself). All the real substance is in two `composedDeviceTypes` entries:
 * an Electrical Sensor (0x0510, ElectricalPowerMeasurement +
 * ElectricalEnergyMeasurement both mandatoryConform) and a Thermostat
 * (0x0301, with an EXTRA mandatoryConform User Label cluster layered on top
 * of that device type's own normal Identify+Thermostat requirement).
 *
 * esp-matter's own `endpoint::heat_pump::create()` (confirmed by reading
 * `esp_matter_endpoint.cpp`'s own `heat_pump::add()` directly) does NOT
 * follow that structure literally: it composes PowerSource + ElectricalSensor
 * (EPM+EEM) + DeviceEnergyManagement[PowerAdjustment] all onto the SAME
 * root endpoint — confirmed by reading `electrical_sensor::add()` itself,
 * which calls `add_device_type()` on whatever endpoint it's handed rather
 * than creating a child — a genuinely different composition style from
 * firmware/refrigerator/'s own child-endpoint pattern for Temperature
 * Controlled Cabinet. It implements NO Thermostat composition at all —
 * neither a child endpoint nor anything on the root.
 *
 * Cross-checked against a real, working third source before deciding how to
 * resolve that gap: connectedhomeip's own chef reference device
 * (`examples/chef/devices/rootnode_heatpump_87ivjRAECh.matter`, fetched and
 * read directly). It CONFIRMS esp-matter's own same-endpoint composition
 * choice (endpoint 1 there lists `ma_powersource` + `ma_electricalsensor` +
 * `device_energy_management` + `ma_heatpump` device types together, plus a
 * client-side `binding cluster Thermostat` matching the XML's own root-level
 * client Thermostat) — but for temperature sensing it uses two entirely
 * separate `ma_tempsensor` (Temperature Sensor, 0x0302) child endpoints
 * instead of a composed Thermostat device type at all, an older/alternate
 * interpretation that doesn't match the current ratified XML's own explicit
 * composed-Thermostat-with-UserLabel requirement.
 *
 * Given three real sources that don't fully agree, this file follows the
 * CURRENT ratified 1.6 XML's own stated intent (a composed Thermostat child,
 * since that's the actual spec this repo targets) rather than chef's older
 * reference — implemented as a genuine CHILD endpoint via
 * `esp_matter::set_parent_endpoint(child, parent)`, the same API firmware/
 * refrigerator/'s own Fridge/Freezer Temperature Controlled Cabinet children
 * already establish — while keeping esp-matter's own proven, tested
 * same-endpoint composition for the Electrical Sensor part (confirmed
 * correct by both esp-matter's own implementation AND the independent chef
 * reference agreeing on that specific point, unlike the Thermostat part).
 * A UserLabel cluster is added manually onto the Thermostat child endpoint
 * (the composedDeviceTypes entry's own extra mandatoryConform requirement
 * beyond Thermostat's own base clusters) — confirmed `cluster::user_label::
 * create()` uses a trivial empty `common::config_t`, no special setup needed.
 *
 * --- Root endpoint: esp-matter's own complete top-level helper, used as-is
 * `endpoint::heat_pump::create()` handles PowerSource (wired feature),
 * ElectricalSensor (both EPM+EEM enabled via `config->electrical_sensor.
 * optional_clusters_mask` — mandatory per the XML's own composedDeviceTypes
 * entry; the helper itself sets ElectricalPowerMeasurement's own
 * AlternatingCurrent feature bit internally, confirmed by reading `add()`
 * directly, so this file doesn't need to), and DeviceEnergyManagement[Power
 * Adjustment] — all with zero extra app code needed. Deliberately NOT
 * driven by any real sensor here: `ElectricalPowerMeasurement`'s and
 * `ElectricalEnergyMeasurement`'s legacy `cluster::create()` functions both
 * tolerate a null `delegate` (confirmed by reading `esp_matter_cluster.cpp`
 * directly — `if (config->delegate != nullptr)` only wires one in
 * conditionally, no `VerifyOrDie`), so both clusters exist and report their
 * static/zero default values with no crash risk — same "no sensor, no
 * fabricated data" honesty precedent firmware/evse/'s own always-NoError
 * FaultState already establishes. A real product wanting genuine power
 * telemetry here would want firmware/outlet/'s own hand-written
 * ElectricalPowerMeasurement::Instance/Delegate pair (or one of its 6
 * real power-monitor chip drivers) wired in instead — out of scope for this
 * first cut. Identify is added manually onto the root (the top-level helper
 * doesn't auto-add it, confirmed by reading `add()` directly, even though
 * the XML lists it as optionalConform there) — every device type in this
 * repo ships one.
 *
 * --- Thermostat child endpoint: Heat+Cool, reusing firmware/thermostat/'s
 * own control loop closely ---------------------------------------------
 * Unlike firmware/room-air-conditioner/'s own deliberately Cool-only scope,
 * a heat pump's entire point is doing both — ControlSequenceOfOperation is
 * CoolingAndHeating, same as firmware/thermostat/'s own default scope, and
 * the hysteresis control loop (heat/cool demand vs. LocalTemperature) is
 * reused near-verbatim from that file, including its own default setpoints
 * (20.00 degC heat / 26.00 degC cool) and 0.3 degC hysteresis band.
 * `HEAT_PUMP_SENSOR_GPIO` reuses the exact DS18B20 1-Wire driver this
 * repo's other appliance/HVAC device types already establish verbatim.
 *
 * --- Output: compressor + reversing valve, no defrost/aux-heat logic -----
 * `HEAT_PUMP_COMPRESSOR_RELAY_GPIO` (active-LOW) runs whenever either heat
 * or cool demand is active — a real heat pump's compressor runs in both
 * modes, only the refrigerant flow direction differs.
 * `HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO` (active-LOW) tracks SystemMode
 * directly (energized only in Cool), independent of the compressor's own
 * on/off cycling — a real reversing valve is pre-positioned for the
 * commanded mode before the compressor ever starts, not toggled per
 * hysteresis cycle. The energized-in-Cool convention matches common "O"
 * terminal wiring, but real heat pump systems are NOT universal here — some
 * use a "B" terminal convention (energized-in-Heat) instead; always check
 * your specific unit's own wiring diagram before connecting real hardware,
 * same disclaimer this repo's other relay-polarity choices already carry.
 * Explicitly, deliberately NOT implemented: any defrost cycle or auxiliary/
 * backup electric-heat-strip logic — real cold-climate heat pumps need
 * outdoor coil temperature sensing and real timing logic to detect and
 * clear ice buildup, genuinely more engineering than a hobby-scale single-
 * sensor build should attempt, the same "smallest reasonable next step"
 * scope cut firmware/robot-vacuum/'s own skipped real navigation and
 * firmware/dishwasher/'s own skipped Fill phase already establish.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/DS18B20 hardware for this device type physically
 * available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>

static const char *TAG = "matter_heat_pump";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module and your specific heat pump's own reversing-valve
 * wiring convention" — polarity/convention isn't universal, see the header
 * comment above. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define HEAT_PUMP_COMPRESSOR_RELAY_GPIO GPIO_NUM_16        /* active-LOW */
#define HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO GPIO_NUM_17    /* active-LOW, energized = Cool (see header comment) */
#define HEAT_PUMP_SENSOR_GPIO GPIO_NUM_21                    /* DS18B20, return-air/room temperature */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Heating/cooling setpoint defaults — same values firmware/thermostat/'s
 * own defaults use (20.00 degC heat / 26.00 degC cool), Matter's global
 * `temperature` type (int16, hundredths of a degree C). */
#define HEAT_PUMP_HEATING_SETPOINT_DEFAULT_CENTIDEGREES 2000
#define HEAT_PUMP_COOLING_SETPOINT_DEFAULT_CENTIDEGREES 2600

/* Bang-bang (hysteresis) control band — same 0.3 degC default firmware/
 * thermostat/'s own control loop uses. */
#define HEAT_PUMP_HYSTERESIS_CENTIDEGREES 30

/* How often the control task re-reads the sensor and re-evaluates the
 * compressor/valve outputs. */
#define HEAT_PUMP_CONTROL_INTERVAL_MS 5000

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t heat_pump_root_endpoint_id = 0;
static uint16_t heat_pump_thermostat_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- DS18B20 driver ---------------------------------------------------
 * Reused verbatim from firmware/room-air-conditioner/'s (itself firmware/
 * thermostat/'s / firmware/water-heater/'s) DS18B20 driver — see those
 * files' own header comments for the 1-Wire timing/CRC detail and sourcing. */
static bool ow_reset(void)
{
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(480);
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(70);
    bool present = (gpio_get_level(HEAT_PUMP_SENSOR_GPIO) == 0);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int bit)
{
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
        esp_rom_delay_us(10);
    }
}

static int ow_read_bit(void)
{
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 0);
    esp_rom_delay_us(2);
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
    esp_rom_delay_us(8);
    int bit = gpio_get_level(HEAT_PUMP_SENSOR_GPIO);
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

static bool sensor_setup(void)
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << HEAT_PUMP_SENSOR_GPIO);
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(HEAT_PUMP_SENSOR_GPIO, 1);
    return true;
}

static bool sensor_read(float *temperature_c)
{
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

/* --- Cross-cutting state --------------------------------------------------
 * Written by app_attribute_update_cb()'s own PRE_UPDATE handling, read by
 * run_control_loop() — same shape firmware/thermostat/'s own globals
 * already establish, just without the OpenTherm/Binding output variants. */
static uint8_t heat_pump_system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
static int16_t heat_pump_heating_setpoint_centidegrees = HEAT_PUMP_HEATING_SETPOINT_DEFAULT_CENTIDEGREES;
static int16_t heat_pump_cooling_setpoint_centidegrees = HEAT_PUMP_COOLING_SETPOINT_DEFAULT_CENTIDEGREES;
static bool heat_pump_local_temperature_valid = false;
static int16_t heat_pump_local_temperature_centidegrees = 0;
static bool heat_pump_heat_demand = false;
static bool heat_pump_cool_demand = false;

/* LocalTemperature is a plain ember attribute (Thermostat is confirmed NOT
 * a code-driven cluster class in this SDK version — no `thermostat/` folder
 * under `data_model_provider/clusters/`, same check firmware/thermostat/'s
 * own header comment documents in full) — a direct attribute::update() call. */
static void update_local_temperature(nullable<int16_t> value)
{
    esp_matter_attr_val_t val = esp_matter_nullable_int16(value);
    attribute::update(heat_pump_thermostat_endpoint_id, Thermostat::Id, Thermostat::Attributes::LocalTemperature::Id, &val);
}

static void set_compressor(bool on)
{
    gpio_set_level(HEAT_PUMP_COMPRESSOR_RELAY_GPIO, on ? 0 : 1); /* active-LOW */
}

static void set_reversing_valve(bool cool_position)
{
    gpio_set_level(HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO, cool_position ? 0 : 1); /* active-LOW */
}

/* The actual bang-bang (hysteresis) control decision — Heat+Cool, reused
 * near-verbatim from firmware/thermostat/'s own run_control_loop(). Only
 * acts while heat_pump_local_temperature_valid (an unknown room
 * temperature must never be treated as "cold/warm enough"). Keeps the
 * PREVIOUS demand state inside the deadband — a hysteresis band means
 * "don't switch yet", not "switch off". The reversing valve's own position
 * tracks SystemMode directly and unconditionally (see the header comment
 * above for why it's independent of the compressor's own on/off cycling). */
static void run_control_loop(void)
{
    set_reversing_valve(heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool));

    bool new_heat_demand = heat_pump_heat_demand;
    bool new_cool_demand = heat_pump_cool_demand;

    if (!heat_pump_local_temperature_valid || heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kOff)) {
        new_heat_demand = false;
        new_cool_demand = false;
    } else if (heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kHeat)) {
        new_cool_demand = false;
        if (heat_pump_local_temperature_centidegrees <= heat_pump_heating_setpoint_centidegrees - HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_heat_demand = true;
        } else if (heat_pump_local_temperature_centidegrees >= heat_pump_heating_setpoint_centidegrees + HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_heat_demand = false;
        }
    } else if (heat_pump_system_mode == chip::to_underlying(Thermostat::SystemModeEnum::kCool)) {
        new_heat_demand = false;
        if (heat_pump_local_temperature_centidegrees >= heat_pump_cooling_setpoint_centidegrees + HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = true;
        } else if (heat_pump_local_temperature_centidegrees <= heat_pump_cooling_setpoint_centidegrees - HEAT_PUMP_HYSTERESIS_CENTIDEGREES) {
            new_cool_demand = false;
        }
    } else {
        /* Any other SystemMode value (Auto/EmergencyHeat/Precooling/
         * FanOnly/Dry/Sleep) isn't implemented — same scope cut firmware/
         * thermostat/'s own SystemMode handling already uses. */
        new_heat_demand = false;
        new_cool_demand = false;
    }

    bool changed = (new_heat_demand != heat_pump_heat_demand) || (new_cool_demand != heat_pump_cool_demand);
    heat_pump_heat_demand = new_heat_demand;
    heat_pump_cool_demand = new_cool_demand;

    if (changed) {
        ESP_LOGI(TAG, "Demand changed: heat=%s cool=%s (room %.2f degC, heat setpoint %.2f degC, cool setpoint %.2f degC)",
                 heat_pump_heat_demand ? "ON" : "off", heat_pump_cool_demand ? "ON" : "off",
                 heat_pump_local_temperature_centidegrees / 100.0f,
                 heat_pump_heating_setpoint_centidegrees / 100.0f,
                 heat_pump_cooling_setpoint_centidegrees / 100.0f);
        set_compressor(heat_pump_heat_demand || heat_pump_cool_demand);
    }
}

/* Periodically reads the sensor, pushes LocalTemperature, and re-runs the
 * control loop. Runs as its own task rather than inline in app_main() so it
 * can freely block on 1-Wire transactions and vTaskDelay() without holding
 * up Matter's own startup/event handling — same reasoning as firmware/
 * thermostat/'s own sensor_task(). */
static void control_task(void *arg)
{
    for (;;) {
        float temperature_c = 0.0f;

        if (sensor_read(&temperature_c)) {
            int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
            ESP_LOGI(TAG, "Room air: %.2f degC", temperature_c);
            heat_pump_local_temperature_valid = true;
            heat_pump_local_temperature_centidegrees = temp_centidegrees;
            update_local_temperature(nullable<int16_t>(temp_centidegrees));
        } else {
            heat_pump_local_temperature_valid = false;
            update_local_temperature(nullable<int16_t>());
        }

        run_control_loop();
        vTaskDelay(pdMS_TO_TICKS(HEAT_PUMP_CONTROL_INTERVAL_MS));
    }
}

/* Toggles the identify LED each time the timer fires — the actual blink.
 * Shared between the root endpoint's own Identify cluster and the
 * Thermostat child's own (auto-added by endpoint::thermostat::create()) —
 * see the header comment above for why both legitimately exist; this
 * callback deliberately doesn't filter by endpoint_id, so either one
 * blinks the same physical LED. */
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

/* Reacts to a controller writing the Thermostat child's SystemMode/
 * OccupiedHeatingSetpoint/OccupiedCoolingSetpoint — tracks the new value
 * locally and re-runs the control loop immediately, same pattern firmware/
 * thermostat/'s own app_attribute_update_cb() already establishes. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != heat_pump_thermostat_endpoint_id || cluster_id != Thermostat::Id) {
        return ESP_OK;
    }

    if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
        heat_pump_system_mode = val->val.u8;
        ESP_LOGI(TAG, "SystemMode set to %u", heat_pump_system_mode);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
        heat_pump_heating_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Heating setpoint set to %.2f degC", heat_pump_heating_setpoint_centidegrees / 100.0f);
        run_control_loop();
    } else if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id) {
        heat_pump_cooling_setpoint_centidegrees = val->val.i16;
        ESP_LOGI(TAG, "Cooling setpoint set to %.2f degC", heat_pump_cooling_setpoint_centidegrees / 100.0f);
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

    /* 2. Configure the two relay outputs — boot off (de-energized), same
     * "boot to known safe state" convention every other device type here
     * follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << HEAT_PUMP_COMPRESSOR_RELAY_GPIO) |
        (1ULL << HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(HEAT_PUMP_COMPRESSOR_RELAY_GPIO, 1); /* active-LOW: 1 = off */
    gpio_set_level(HEAT_PUMP_REVERSING_VALVE_RELAY_GPIO, 1);

    /* 2b. Configure the DS18B20 sensor pin. */
    sensor_setup();

    /* 2c. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Heat Pump root endpoint
     * (Descriptor + PowerSource + ElectricalSensor[EPM+EEM] +
     * DeviceEnergyManagement via the complete top-level helper, plus
     * Identify added manually), plus a child Thermostat endpoint (Heat+Cool)
     * with an extra UserLabel cluster — see the header comment above for
     * why this composition shape was chosen over the other two candidates. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::heat_pump::config_t heat_pump_config;
    heat_pump_config.electrical_sensor.with_electrical_power_measurement();
    heat_pump_config.electrical_sensor.with_electrical_energy_measurement();

    endpoint_t *root_endpoint = endpoint::heat_pump::create(node, &heat_pump_config, ENDPOINT_FLAG_NONE, NULL);
    if (!root_endpoint) {
        ESP_LOGE(TAG, "Failed to create heat pump root endpoint");
        return;
    }
    heat_pump_root_endpoint_id = endpoint::get_id(root_endpoint);
    ESP_LOGI(TAG, "Heat pump root endpoint id: %u", heat_pump_root_endpoint_id);

    /* 3a. Identify — heat_pump::add() doesn't create it automatically. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(root_endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3b. Thermostat child endpoint — Heat+Cool, same scope firmware/
     * thermostat/'s own default configuration uses. */
    endpoint::thermostat::config_t thermostat_config;
    thermostat_config.thermostat.local_temperature = nullable<int16_t>();
    thermostat_config.thermostat.control_sequence_of_operation =
        chip::to_underlying(Thermostat::ControlSequenceOfOperationEnum::kCoolingAndHeating);
    thermostat_config.thermostat.system_mode = chip::to_underlying(Thermostat::SystemModeEnum::kOff);
    thermostat_config.thermostat.feature_flags =
        (uint32_t)Thermostat::Feature::kHeating | (uint32_t)Thermostat::Feature::kCooling;
    thermostat_config.thermostat.features.heating.occupied_heating_setpoint = heat_pump_heating_setpoint_centidegrees;
    thermostat_config.thermostat.features.cooling.occupied_cooling_setpoint = heat_pump_cooling_setpoint_centidegrees;

    endpoint_t *thermostat_endpoint = thermostat::create(node, &thermostat_config, ENDPOINT_FLAG_NONE, NULL);
    if (!thermostat_endpoint) {
        ESP_LOGE(TAG, "Failed to create thermostat child endpoint");
        return;
    }
    heat_pump_thermostat_endpoint_id = endpoint::get_id(thermostat_endpoint);
    ESP_LOGI(TAG, "Heat pump thermostat child endpoint id: %u", heat_pump_thermostat_endpoint_id);

    /* 3c. UserLabel — the composedDeviceTypes entry's own extra
     * mandatoryConform requirement beyond Thermostat's own base clusters. */
    cluster::user_label::config_t user_label_config;
    cluster::user_label::create(thermostat_endpoint, &user_label_config, CLUSTER_FLAG_SERVER);

    /* 3d. Parent the Thermostat endpoint under the Heat Pump root — same
     * API firmware/refrigerator/'s own Fridge/Freezer children already use. */
    err = set_parent_endpoint(thermostat_endpoint, root_endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set thermostat endpoint's parent: %d", err);
        return;
    }

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

    /* 5. Start the control task — reads the sensor, pushes LocalTemperature,
     * runs the Heat+Cool hysteresis loop. */
    xTaskCreate(control_task, "heat_pump_control_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter heat pump started. Scan the QR code to commission.");
}
