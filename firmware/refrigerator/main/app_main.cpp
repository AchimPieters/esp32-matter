/*
 * Minimal Matter Refrigerator — twenty-seventh device type, and this repo's
 * first genuinely composed, multi-endpoint device: a Refrigerator (0x0070)
 * root endpoint with two Temperature Controlled Cabinet (0x0071) *child*
 * endpoints (Fridge + Freezer), linked via esp-matter's real parent-child
 * endpoint API — not just "two endpoints that happen to exist on one
 * node" the way firmware/outlet/'s second (Electrical Sensor) endpoint is.
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
 * --- Why two endpoints, and why esp-matter's own `refrigerator::create()`
 * adds almost nothing on its own ------------------------------------------
 * Confirmed directly against the CSA's own
 * data_model/1.6/device_types/Refrigerator.xml: Refrigerator's
 * `<conditionRequirements>` block mandates at least one child endpoint of
 * device type Temperature Controlled Cabinet (0x0071) with the "Cooler"
 * condition — a real, spec-level structural requirement, not a design
 * choice made here. Confirmed by reading esp-matter's own generated
 * `refrigerator_device.cpp`/`temperature_controlled_cabinet_device.cpp`
 * directly (not assumed): `refrigerator::create()` only creates a
 * Descriptor cluster and the device-type-list entry — Identify,
 * RefrigeratorAndTemperatureControlledCabinetMode, and RefrigeratorAlarm
 * are all `<optionalConform/>` on this device type, so esp-matter doesn't
 * add them automatically (its own generated file's comment says so
 * outright: "... are optional cluster for refrigerator device type so we
 * are not adding them by default"), the same shape firmware/extractor-hood/
 * and firmware/water-heater/ already hit for their own optional Identify.
 * This file adds Identify + RefrigeratorAlarm onto the root endpoint
 * manually, the same "add extra clusters onto an already-correct endpoint"
 * pattern established repeatedly in this repo (firmware/thermostat/'s
 * BINDING output, firmware/air-quality-sensor/'s concentration-measurement
 * clusters). `temperature_controlled_cabinet::create()` DOES add
 * TemperatureControl automatically (it's `<mandatoryConform/>` on that
 * device type) but not RefrigeratorAndTemperatureControlledCabinetMode or
 * TemperatureMeasurement (both `<optionalConform/>` there too) — added
 * manually onto each cabinet endpoint for the same reason.
 *
 * The two cabinet endpoints are linked to the root via
 * `esp_matter::set_parent_endpoint(child, parent)` — confirmed as the
 * correct, real API (not guessed) by reading esp-matter's own official
 * `examples/refrigerator/main/app_main.cpp` reference end to end, which
 * uses the exact same call after creating both endpoints and before
 * `esp_matter::start()`.
 *
 * --- TemperatureControl: TN only, confirmed NOT to need SetDelegate() ---
 * `TemperatureControlledCabinet.xml` mandates the TN (TemperatureNumber)
 * feature and explicitly `<disallowConform/>`s TL (TemperatureLevel) — so
 * this is a plain numeric setpoint, no level list. esp-matter ships TWO
 * parallel cluster/endpoint implementations — a newer "generated" one
 * (only compiled in when `CONFIG_ESP_MATTER_ENABLE_GENERATED_DATA_MODEL`
 * is set, which it isn't here, same as every other device type in this
 * repo) and an older "legacy" one that's the actual default and what this
 * build compiles against (confirmed via `esp_matter_cluster.h`'s own
 * `#if CONFIG_ESP_MATTER_ENABLE_GENERATED_DATA_MODEL` branch). The
 * "generated" version's `temperature_controlled_cabinet_device.cpp`
 * DOES set `config->temperature_control.feature_flags |=
 * cluster::temperature_control::feature::temperature_number::get_id();`
 * internally — but the "legacy" `esp_matter_endpoint.cpp`'s own
 * `temperature_controlled_cabinet::add()` does NOT (confirmed by reading
 * both side by side after a first Docker build attempt, written assuming
 * "generated" behavior, failed to compile) — a real, previously-
 * undocumented discrepancy between the two implementations. This file
 * sets `feature_flags` explicitly itself as a result, and uses the
 * legacy header's own field name (`temp_setpoint`, not
 * `temperature_setpoint` — also different between the two
 * implementations, also only caught by the same failed build).
 *
 * The official reference example also calls
 * `TemperatureControlCluster::SetDelegate(&sAppSupportedTemperatureLevelsDelegate)`
 * — deliberately NOT done here, confirmed safe to skip by reading
 * `TemperatureControlCluster.cpp` directly rather than copying the
 * reference blindly: `mDelegate` (the thing `SetDelegate()` sets) is only
 * ever read in two places — `ReadAttribute()`'s `SupportedTemperatureLevels`
 * branch, and `SetSelectedTemperatureLevel()` — and BOTH are gated behind
 * `mFeatures.Has(Feature::kTemperatureLevel)`, which is never set here (TL
 * is disallowed by the device type XML above). `SetDelegate()`'s own
 * parameter type, `TemperatureControl::SupportedTemperatureLevelsIteratorDelegate*`,
 * is TL-specific by name too. The real, always-relevant API for TN mode is
 * `SetTemperatureSetpoint(int16_t)` / `GetTemperatureSetpoint() const` —
 * confirmed directly in the same header — and `HandleSetTemperature()`
 * (the SetTemperature command's handler) already calls
 * `SetTemperatureSetpoint()` internally whenever `kTemperatureNumber` is
 * set, so a controller's SetTemperature command is handled entirely inside
 * the cluster with zero app code needed. This file's own control loop
 * (below) only ever *reads* the live setpoint back via
 * `GetTemperatureSetpoint()`, through the same registry-lookup-and-cast
 * pattern this repo's other code-driven-cluster access already uses
 * (confirmed: a real `temperature_control/` folder exists under
 * `data_model_provider/clusters/`, i.e. `TemperatureControlCluster` is a
 * `DefaultServerCluster`, not a plain ember attribute — so there is no
 * `attribute::PRE_UPDATE` hook to react to a controller's SetTemperature
 * command even if one were wanted; polling `GetTemperatureSetpoint()` once
 * per control-loop tick is the only option, and is enough for a bang-bang
 * loop like this one).
 *
 * `MinTemperature`/`MaxTemperature`/`TemperatureSetpoint` are all Matter's
 * global `temperature` type — int16, hundredths of a degree C, the same
 * encoding firmware/thermostat/'s setpoints already use. Fridge:
 * 1.00-10.00 degC, default target 4.00 degC. Freezer: -24.00..-14.00 degC,
 * default target -18.00 degC — both ordinary commercial-fridge/freezer
 * target ranges, not measured/researched against any one real appliance's
 * spec sheet (unlike e.g. firmware/pressure-sensor/'s BMP280 datasheet
 * numbers), since there's no single "correct" answer here the way a chip's
 * register map has one.
 *
 * --- RefrigeratorAndTemperatureControlledCabinetMode: one reusable
 * ModeBase delegate class, not two near-duplicates ------------------------
 * Same automatic wiring as firmware/water-heater/'s WaterHeaterMode/
 * firmware/robot-vacuum/'s RvcRunMode — confirmed by reading
 * `esp_matter_delegate_callbacks.cpp`'s own
 * `RefrigeratorAndTemperatureControlledCabinetModeDelegateInitCB`, which
 * calls the same shared `InitModeDelegate()` helper: passing a delegate
 * via `config->delegate` is enough, esp-matter constructs a real
 * `ModeBase::Instance` automatically during `esp_matter::start()`, no
 * ordering awareness needed here. esp-matter's own C++ wrapper namespace
 * for this cluster is shorter than the cluster's real Matter name —
 * `esp_matter::cluster::refrigerator_and_tcc_mode`, not
 * `::refrigerator_and_temperature_controlled_cabinet_mode` (confirmed by
 * reading the legacy header directly, same "legacy vs. generated"
 * discrepancy as TemperatureControl above, caught the same way — this
 * cluster doesn't exist under the longer name in the legacy header at
 * all, a straight compile error rather than a field-name mismatch). The
 * connectedhomeip cluster server itself, and its `ModeTag`/`Id` enum
 * values used below, are unaffected — that split only changes esp-matter's
 * own wrapper naming, not the underlying Matter cluster. Each cabinet
 * gets its OWN instance of
 * this cluster (it's independently `<optionalConform/>`-added per
 * endpoint, not shared) with its own 2-mode list: "Normal" (ModeTag::kAuto)
 * and either "Rapid Cool" (ModeTag::kRapidCool, fridge) or "Rapid Freeze"
 * (ModeTag::kRapidFreeze, freezer) — both tag values confirmed directly
 * against connectedhomeip's own generated
 * `RefrigeratorAndTemperatureControlledCabinetMode/Enums.h`. Rather than
 * writing two near-identical Delegate subclasses (the way firmware/
 * robot-vacuum/'s genuinely different RvcRunMode/RvcCleanMode earned their
 * own separate classes), this file writes ONE parameterized
 * `RefrigeratorCabinetModeDelegate` class taking a `CabinetKind` at
 * construction — the two mode lists differ only in one tag/label, so one
 * class with a runtime branch is more honest about how similar they
 * actually are. "Rapid Cool"/"Rapid Freeze" don't get a genuinely
 * different control algorithm — same "smallest reasonable next step" scope
 * cut as firmware/robot-vacuum/'s "Mapping" mode having no real navigation
 * behind it: while active, the cabinet's own control loop (below) simply
 * targets `REFRIGERATOR_RAPID_MODE_OFFSET_CENTIDEGREES` (3.00 degC) colder
 * than the normal setpoint, honestly simulating "cool down faster" without
 * claiming any real compressor-staging logic this hobby-scale single-relay
 * design doesn't have.
 *
 * --- RefrigeratorAlarm: plain ember attributes, DoorOpen only, and a real,
 * documented gap in esp-matter's own event-sending API --------------------
 * Confirmed NOT code-driven (no `refrigerator_alarm/` folder under
 * `data_model_provider/clusters/`, unlike TemperatureControl/
 * TemperatureMeasurement above) — `Mask`/`State`/`Supported` are plain
 * ember `uint32_t` bitmap attributes, written via the same
 * `attribute::update()` call every other plain-ember cluster in this repo
 * uses. `RefrigeratorAlarm::AlarmBitmap` (confirmed directly against
 * connectedhomeip's own generated Enums.h) has exactly ONE bit,
 * `kDoorOpen` — so `Mask`/`Supported` are both fixed to that single bit at
 * creation time, and `State` is toggled live by a debounced door-sensor
 * poll. A real, previously-undocumented gap was found while wiring this
 * up: esp-matter's `esp_matter_event_impl.h` declares a
 * `refrigerator_alarm::event::create_notify()` (the capability
 * declaration) but, unlike e.g. `switch_cluster`'s own
 * `send_initial_press()`/etc., there is NO matching `send_notify()`-style
 * runtime helper for firing it — confirmed by reading that entire header.
 * This file does NOT hand-rig the low-level connectedhomeip event API to
 * work around that gap; `State`'s own attribute-change report (which DOES
 * fire normally, since it's a plain ember attribute) already tells a
 * controller the alarm changed — the spec's Notify event exists to carry
 * a machine-readable "why" alongside that, and skipping it is the same
 * "smallest reasonable next step" scope cut this repo applies to other
 * optional-richness gaps (e.g. firmware/air-quality-sensor/'s AirQuality
 * FeatureMap gap).
 *
 * --- TemperatureMeasurement: DS18B20, reused verbatim (x2) from
 * firmware/water-heater/ ---------------------------------------------------
 * Same 1-Wire bit-banged driver as firmware/water-heater/'s own DS18B20
 * code (itself reused from firmware/thermostat/'s SENSOR_TYPE library) —
 * only change: every function now takes a `gpio_num_t` parameter instead
 * of reading a single hardcoded `#define`, since this file genuinely needs
 * two independent sensors (one per cabinet) rather than one. Confirmed the
 * same "code-driven cluster" status as firmware/temperature-sensor/'s own
 * TemperatureMeasurement (a real `temperature_measurement/` folder exists
 * under `data_model_provider/clusters/`) — `SetMeasuredValue()` via the
 * registry-lookup-and-cast pattern, parameterized by endpoint id this time
 * since there are two live instances of this cluster on one node.
 *
 * --- Hardware: two independent relay+sensor pairs, one door sensor -------
 * `REFRIGERATOR_FRIDGE_RELAY_GPIO`/`REFRIGERATOR_FREEZER_RELAY_GPIO`
 * (active-LOW, matching firmware/valve/'s and firmware/water-heater/'s own
 * relay convention) each independently gate a compressor/solenoid contactor
 * — a deliberate, documented simplification: a real fridge/freezer
 * combination almost always shares ONE compressor and refrigerant loop
 * with a damper controlling airflow split between compartments, not two
 * fully independent cooling circuits. Modelling the real shared-compressor
 * thermodynamics is out of scope for a hobby retrofit (same "smallest
 * reasonable next step" reasoning firmware/thermostat/'s RELAY output only
 * gates a boiler's own call-for-heat contact rather than modelling the
 * boiler's internals). `REFRIGERATOR_DOOR_GPIO` is a single reed switch
 * covering the whole appliance (real combination fridge/freezers commonly
 * ship one door-open indicator per exterior door, not per compartment) —
 * pulled up, LOW = closed / HIGH = open, same polarity convention
 * firmware/contact-sensor/'s reed switch already uses — read by a simple
 * debounced poll (a plain N-consistent-samples check, not the
 * interrupt+queue machinery firmware/contact-sensor/'s and
 * firmware/switch/'s shared debounce task use, since this is the only
 * digital input in this file and a small dedicated task is simpler than
 * pulling in that shared infrastructure for one GPIO).
 *
 * `REFRIGERATOR_HYSTERESIS_CENTIDEGREES` (0.5 degC) reuses
 * firmware/water-heater/'s own reasoning: a fridge/freezer compartment's
 * air+contents thermal mass responds slowly, so a tight band would just
 * cause needless relay chatter with no real benefit. On a sensor read
 * failure, that cabinet's own cooling is turned off (fail-safe), the same
 * "no reading means no confident heating/cooling decision" convention
 * firmware/water-heater/'s own control loop already established.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/DS18B20/reed-switch hardware for this device
 * type physically available when written).
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
#include <app/clusters/resource-monitoring-server/ResourceMonitoringCluster.h>
#include <data_model_provider/clusters/resource_monitor/integration.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_refrigerator";

/* --- GPIO pin map ---------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay module" — polarity isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define REFRIGERATOR_DOOR_GPIO GPIO_NUM_4               /* reed switch, pulled up: LOW=closed, HIGH=open */
#define REFRIGERATOR_FRIDGE_SENSOR_GPIO GPIO_NUM_21      /* DS18B20 #1 (fridge compartment) */
#define REFRIGERATOR_FREEZER_SENSOR_GPIO GPIO_NUM_22     /* DS18B20 #2 (freezer compartment) */
#define REFRIGERATOR_FRIDGE_RELAY_GPIO GPIO_NUM_16        /* active-LOW */
#define REFRIGERATOR_FREEZER_RELAY_GPIO GPIO_NUM_17       /* active-LOW */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* How often each cabinet's DS18B20 is read and its hysteresis logic
 * re-evaluated. */
#define REFRIGERATOR_MEASURE_INTERVAL_MS 10000

/* Bang-bang (hysteresis) control band, in centidegrees C — see the header
 * comment above for why this reuses firmware/water-heater/'s own value. */
#define REFRIGERATOR_HYSTERESIS_CENTIDEGREES 50

/* How much colder the target gets while a cabinet's "Rapid Cool"/"Rapid
 * Freeze" mode is active — see the header comment above for why this is a
 * documented simplification, not a real staged-compressor algorithm. */
#define REFRIGERATOR_RAPID_MODE_OFFSET_CENTIDEGREES (-300)

/* Fridge compartment: 1.00-10.00 degC, default target 4.00 degC. */
#define REFRIGERATOR_FRIDGE_MIN_CENTIDEGREES 100
#define REFRIGERATOR_FRIDGE_MAX_CENTIDEGREES 1000
#define REFRIGERATOR_FRIDGE_DEFAULT_SETPOINT_CENTIDEGREES 400
#define REFRIGERATOR_FRIDGE_MEASURE_MIN_CENTIDEGREES (-1000)
#define REFRIGERATOR_FRIDGE_MEASURE_MAX_CENTIDEGREES 3000

/* Freezer compartment: -24.00..-14.00 degC, default target -18.00 degC. */
#define REFRIGERATOR_FREEZER_MIN_CENTIDEGREES (-2400)
#define REFRIGERATOR_FREEZER_MAX_CENTIDEGREES (-1400)
#define REFRIGERATOR_FREEZER_DEFAULT_SETPOINT_CENTIDEGREES (-1800)
#define REFRIGERATOR_FREEZER_MEASURE_MIN_CENTIDEGREES (-3000)
#define REFRIGERATOR_FREEZER_MEASURE_MAX_CENTIDEGREES 1000

/* Door-sensor debounce. */
#define REFRIGERATOR_DOOR_POLL_INTERVAL_MS 200
#define REFRIGERATOR_DOOR_DEBOUNCE_SAMPLES 3

/* Activated Carbon Filter Monitoring — optionalConform on this device type
 * as of Refrigerator.xml's own revision 3 ("Added optional Activated
 * Carbon Filter Monitoring cluster"), confirmed by reading that XML
 * directly rather than assumed from firmware/air-purifier/'s or firmware/
 * room-air-conditioner/'s own precedent. Unlike
 * those two device types' own filter-monitoring clusters — both gated
 * behind "is the fan/compressor actually running right now", since a
 * kitchen grease filter or an HVAC air filter only sees load while air is
 * moving through it — a refrigerator's own deodorizing carbon filter sits
 * inside the sealed cabinet and is exposed to (and absorbing odors from)
 * the food storage air continuously, whether or not the compressor happens
 * to be cycling at that instant. So life here is plain wall-clock elapsed
 * time since the counter was last reset (a factory reset, same as every
 * other NVS-backed counter in this repo), not gated by any run-state —
 * the one deliberate difference from firmware/air-purifier/'s/firmware/
 * extractor-hood/'s/firmware/room-air-conditioner/'s own run-time-gated
 * life estimates. REFRIGERATOR_CARBON_FILTER_LIFE_HOURS (4380h ≈ 6 months
 * of continuous operation) is a commonly cited real refrigerator carbon-
 * filter replacement interval — adjustable, not a calibrated reading, same
 * "adjustable threshold" precedent this repo's other filter-life/
 * classifier constants already establish. */
#define REFRIGERATOR_CARBON_FILTER_POLL_INTERVAL_MS 5000
#define REFRIGERATOR_CARBON_FILTER_NVS_SAVE_INTERVAL_MS 60000
#define REFRIGERATOR_CARBON_FILTER_NVS_NAMESPACE "carbon_filter"
#define REFRIGERATOR_CARBON_FILTER_NVS_KEY "run_seconds"
#define REFRIGERATOR_CARBON_FILTER_LIFE_HOURS 4380
#define REFRIGERATOR_CARBON_FILTER_CHANGE_WARNING_PERCENT 20
#define REFRIGERATOR_CARBON_FILTER_CHANGE_CRITICAL_PERCENT 5

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error (chip::detail vs. chip::app::Clusters::detail) a
 * blanket `using namespace chip;` caused there. Everything else from
 * `chip::`/`chip::app::` below is spelled out fully qualified instead. */
using namespace chip::literals;

static uint16_t refrigerator_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* --- DS18B20 driver (parameterized by pin) --------------------------------
 * Reused verbatim from firmware/water-heater/'s own DS18B20 code (itself
 * from firmware/thermostat/'s SENSOR_TYPE library) — see those files' own
 * header comments for the 1-Wire timing/CRC detail and sourcing. Only
 * change: takes a `gpio_num_t` parameter instead of reading a single
 * hardcoded `#define`, since this file needs two independent sensors. */
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
        ESP_LOGW(TAG, "DS18B20 on GPIO %d not responding to reset — check wiring/pull-up", (int)pin);
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
        ESP_LOGW(TAG, "DS18B20 on GPIO %d not responding to reset (read phase)", (int)pin);
        return false;
    }

    if (onewire_crc8(scratchpad, 8) != scratchpad[8]) {
        ESP_LOGW(TAG, "DS18B20 on GPIO %d: CRC mismatch — discarding reading", (int)pin);
        return false;
    }

    int16_t raw = (int16_t)(((uint16_t)scratchpad[1] << 8) | scratchpad[0]);
    *temperature_c = raw * 0.0625f; /* 12-bit default resolution: 1 LSB = 1/16 degC */
    return true;
}

/* --- RefrigeratorAndTemperatureControlledCabinetMode delegate -------------
 * See the header comment above for why this is ONE parameterized class
 * covering both cabinets rather than two near-duplicate classes. */
enum class CabinetKind { kFridge, kFreezer };

class RefrigeratorCabinetModeDelegate : public ModeBase::Delegate
{
public:
    explicit RefrigeratorCabinetModeDelegate(CabinetKind kind) : m_kind(kind) {}

    CHIP_ERROR Init() override { return CHIP_NO_ERROR; }

    CHIP_ERROR GetModeLabelByIndex(uint8_t modeIndex, chip::MutableCharSpan &label) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        const char *text = (modeIndex == 0) ? "Normal" : rapid_mode_label();
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan(text, strlen(text)), label);
    }

    CHIP_ERROR GetModeValueByIndex(uint8_t modeIndex, uint8_t &value) override
    {
        if (modeIndex >= kNumModes) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        value = modeIndex; /* 0 = Normal, 1 = Rapid Cool/Freeze */
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
            : rapid_mode_tag();
        tags[0] = {.value = tag_value};
        tags.reduce_size(1);
        return CHIP_NO_ERROR;
    }

    void HandleChangeToMode(uint8_t newMode, ModeBase::Commands::ChangeToModeResponse::Type &response) override
    {
        m_current_mode = newMode;
        ESP_LOGI(TAG, "%s cabinet mode set to %u", (m_kind == CabinetKind::kFridge) ? "Fridge" : "Freezer", newMode);
        response.status = chip::to_underlying(ModeBase::StatusCode::kSuccess);
    }

    bool RapidActive() const { return m_current_mode == 1; }

private:
    static constexpr uint8_t kNumModes = 2;

    const char *rapid_mode_label() const
    {
        return (m_kind == CabinetKind::kFridge) ? "Rapid Cool" : "Rapid Freeze";
    }

    uint16_t rapid_mode_tag() const
    {
        return chip::to_underlying((m_kind == CabinetKind::kFridge)
                                        ? RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kRapidCool
                                        : RefrigeratorAndTemperatureControlledCabinetMode::ModeTag::kRapidFreeze);
    }

    CabinetKind m_kind;
    uint8_t m_current_mode = 0;
};

static RefrigeratorCabinetModeDelegate fridge_mode_delegate(CabinetKind::kFridge);
static RefrigeratorCabinetModeDelegate freezer_mode_delegate(CabinetKind::kFreezer);

/* --- Registry-lookup-and-cast helpers --------------------------------------
 * Same pattern this repo's other code-driven-cluster access already uses
 * (see e.g. firmware/valve/'s get_valve_cluster(), firmware/pressure-sensor/'s
 * equivalent for PressureMeasurementCluster) — parameterized by endpoint id
 * here since both TemperatureControl and TemperatureMeasurement have two
 * live instances on this node (one per cabinet). */
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

/* --- Per-cabinet control loop ----------------------------------------------
 * One generic task body, spawned twice (fridge + freezer) with a different
 * `cabinet_runtime_t` argument each time — see the header comment above for
 * why the two compartments are modelled as fully independent relays rather
 * than one shared compressor. */
struct cabinet_runtime_t {
    const char *name;
    uint16_t endpoint_id;
    gpio_num_t sensor_gpio;
    gpio_num_t relay_gpio;
    RefrigeratorCabinetModeDelegate *mode_delegate;
    bool relay_on;
};

static cabinet_runtime_t fridge_runtime;
static cabinet_runtime_t freezer_runtime;

static void set_cabinet_relay(cabinet_runtime_t *rt, bool on)
{
    rt->relay_on = on;
    gpio_set_level(rt->relay_gpio, on ? 0 : 1); /* active-LOW */
}

static void cabinet_control_task(void *arg)
{
    cabinet_runtime_t *rt = static_cast<cabinet_runtime_t *>(arg);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REFRIGERATOR_MEASURE_INTERVAL_MS));

        float temperature_c;
        if (!sensor_read(rt->sensor_gpio, &temperature_c)) {
            /* No confident reading — fail safe and stop cooling, same
             * convention firmware/water-heater/'s own control loop uses. */
            if (rt->relay_on) {
                set_cabinet_relay(rt, false);
            }
            continue;
        }

        int16_t measured_centidegrees = (int16_t)(temperature_c * 100.0f);

        TemperatureMeasurementCluster *meas = get_temperature_measurement_cluster(rt->endpoint_id);
        if (meas) {
            meas->SetMeasuredValue(chip::app::DataModel::Nullable<int16_t>(measured_centidegrees));
        }
        ESP_LOGI(TAG, "%s: %.2f degC", rt->name, temperature_c);

        TemperatureControlCluster *ctrl = get_temperature_control_cluster(rt->endpoint_id);
        int16_t target_centidegrees = ctrl ? ctrl->GetTemperatureSetpoint() : 0;
        if (rt->mode_delegate->RapidActive()) {
            target_centidegrees += REFRIGERATOR_RAPID_MODE_OFFSET_CENTIDEGREES;
        }

        if (measured_centidegrees >= target_centidegrees + REFRIGERATOR_HYSTERESIS_CENTIDEGREES) {
            if (!rt->relay_on) {
                set_cabinet_relay(rt, true);
            }
        } else if (measured_centidegrees <= target_centidegrees - REFRIGERATOR_HYSTERESIS_CENTIDEGREES) {
            if (rt->relay_on) {
                set_cabinet_relay(rt, false);
            }
        }
    }
}

/* --- Door sensor: debounced poll, updates RefrigeratorAlarm's State ------
 * See the header comment above for the wiring convention and why a plain
 * dedicated poll (not the shared interrupt+queue debounce infrastructure
 * firmware/contact-sensor/ and firmware/switch/ use) is enough here. */
static void door_task(void *arg)
{
    bool last_reported_open = false;
    bool last_sample = false;
    int consistent_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REFRIGERATOR_DOOR_POLL_INTERVAL_MS));

        bool sample_open = (gpio_get_level(REFRIGERATOR_DOOR_GPIO) == 1);
        if (sample_open == last_sample) {
            consistent_count++;
        } else {
            consistent_count = 1;
            last_sample = sample_open;
        }

        if (consistent_count >= REFRIGERATOR_DOOR_DEBOUNCE_SAMPLES && sample_open != last_reported_open) {
            last_reported_open = sample_open;
            esp_matter_attr_val_t val = esp_matter_bitmap32(
                sample_open ? (uint32_t)RefrigeratorAlarm::AlarmBitmap::kDoorOpen : 0);
            attribute::update(refrigerator_endpoint_id, RefrigeratorAlarm::Id, RefrigeratorAlarm::Attributes::State::Id, &val);
            ESP_LOGI(TAG, "Refrigerator door %s", sample_open ? "OPEN" : "closed");
        }
    }
}

/* --- Activated Carbon Filter Monitoring — see the #define block above for
 * why this is plain wall-clock elapsed time, not gated by any compressor
 * run-state. Reuses firmware/extractor-hood/'s/firmware/room-air-
 * conditioner/'s own Condition-feature integration shape
 * (`resource_monitoring::feature::condition::add()` +
 * `ResourceMonitoring::GetClusterInstance()`), just always-accumulating
 * instead of gated. -------------------------------------------------- */
static uint32_t carbon_filter_total_run_seconds = 0;

static uint8_t compute_carbon_filter_condition(uint32_t run_seconds, uint32_t life_hours)
{
    uint32_t life_seconds = life_hours * 3600u;
    if (run_seconds >= life_seconds) {
        return 0;
    }
    uint32_t remaining_percent = 100u - ((uint64_t)run_seconds * 100u) / life_seconds;
    return (uint8_t)remaining_percent;
}

static ResourceMonitoring::ChangeIndicationEnum carbon_filter_change_indication_for(uint8_t condition_percent)
{
    if (condition_percent <= REFRIGERATOR_CARBON_FILTER_CHANGE_CRITICAL_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kCritical;
    }
    if (condition_percent <= REFRIGERATOR_CARBON_FILTER_CHANGE_WARNING_PERCENT) {
        return ResourceMonitoring::ChangeIndicationEnum::kWarning;
    }
    return ResourceMonitoring::ChangeIndicationEnum::kOk;
}

/* Polls every REFRIGERATOR_CARBON_FILTER_POLL_INTERVAL_MS: unconditionally
 * accumulates elapsed time (unlike firmware/air-purifier/'s/firmware/
 * extractor-hood/'s/firmware/room-air-conditioner/'s own run-gated
 * versions — see the #define block above for why), periodically persists
 * it to NVS, and refreshes the filter cluster's Condition/ChangeIndication
 * every poll regardless. */
static void carbon_filter_life_task(void *arg)
{
    uint32_t ms_since_save = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(REFRIGERATOR_CARBON_FILTER_POLL_INTERVAL_MS));

        carbon_filter_total_run_seconds += REFRIGERATOR_CARBON_FILTER_POLL_INTERVAL_MS / 1000;
        ms_since_save += REFRIGERATOR_CARBON_FILTER_POLL_INTERVAL_MS;

        if (ms_since_save >= REFRIGERATOR_CARBON_FILTER_NVS_SAVE_INTERVAL_MS) {
            nvs_handle_t nvs;
            if (nvs_open(REFRIGERATOR_CARBON_FILTER_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                nvs_set_u32(nvs, REFRIGERATOR_CARBON_FILTER_NVS_KEY, carbon_filter_total_run_seconds);
                nvs_commit(nvs);
                nvs_close(nvs);
            }
            ms_since_save = 0;
        }

        uint8_t condition =
            compute_carbon_filter_condition(carbon_filter_total_run_seconds, REFRIGERATOR_CARBON_FILTER_LIFE_HOURS);
        ResourceMonitoring::ChangeIndicationEnum indication = carbon_filter_change_indication_for(condition);

        auto *cluster = ResourceMonitoring::GetClusterInstance(refrigerator_endpoint_id, ActivatedCarbonFilterMonitoring::Id);
        if (!cluster) {
            ESP_LOGE(TAG, "Activated carbon filter cluster not found on endpoint %u", refrigerator_endpoint_id);
            continue;
        }
        cluster->UpdateCondition(condition);
        cluster->UpdateChangeIndication(indication);
        ESP_LOGI(TAG, "Carbon filter: %u%% remaining (%s)", condition,
                 indication == ResourceMonitoring::ChangeIndicationEnum::kCritical ? "CRITICAL" :
                 indication == ResourceMonitoring::ChangeIndicationEnum::kWarning ? "WARNING" : "OK");
    }
}

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

/* Nothing on this device needs to react to a plain-ember attribute write
 * (RefrigeratorAlarm's Mask is client-writable but nothing here gates
 * behavior on it — DoorOpen is the only bit that exists at all) — kept as
 * a trivial stub, same as several other device types' own minimal
 * callback where there's genuinely nothing to handle. */
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

    /* 1c. Load the carbon filter's elapsed-time counter — see
     * carbon_filter_life_task()'s own header comment above. */
    nvs_handle_t carbon_filter_nvs;
    if (nvs_open(REFRIGERATOR_CARBON_FILTER_NVS_NAMESPACE, NVS_READWRITE, &carbon_filter_nvs) == ESP_OK) {
        nvs_get_u32(carbon_filter_nvs, REFRIGERATOR_CARBON_FILTER_NVS_KEY, &carbon_filter_total_run_seconds);
        nvs_close(carbon_filter_nvs);
    }

    /* 2. Configure the door sensor (pulled up, LOW=closed/HIGH=open). */
    gpio_config_t door_io_conf = {};
    door_io_conf.pin_bit_mask = (1ULL << REFRIGERATOR_DOOR_GPIO);
    door_io_conf.mode = GPIO_MODE_INPUT;
    door_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&door_io_conf);

    /* 2b. Configure both compartments' relay outputs — boot off
     * (de-energized), same "boot to known safe state" convention every
     * other device type here follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << REFRIGERATOR_FRIDGE_RELAY_GPIO) | (1ULL << REFRIGERATOR_FREEZER_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(REFRIGERATOR_FRIDGE_RELAY_GPIO, 1);  /* active-LOW: 1 = off */
    gpio_set_level(REFRIGERATOR_FREEZER_RELAY_GPIO, 1);

    /* 2c. Configure both DS18B20 sensor pins. */
    sensor_setup(REFRIGERATOR_FRIDGE_SENSOR_GPIO);
    sensor_setup(REFRIGERATOR_FREEZER_SENSOR_GPIO);

    /* 2d. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Refrigerator root
     * endpoint (Descriptor via the helper, plus Identify + RefrigeratorAlarm
     * added manually), and two Temperature Controlled Cabinet child
     * endpoints (TemperatureControl via the helper, plus
     * RefrigeratorAndTemperatureControlledCabinetMode + TemperatureMeasurement
     * added manually) — see the header comment above for the full detail. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::refrigerator::config_t refrigerator_config;
    endpoint_t *refrigerator_endpoint = endpoint::refrigerator::create(node, &refrigerator_config, ENDPOINT_FLAG_NONE, NULL);
    if (!refrigerator_endpoint) {
        ESP_LOGE(TAG, "Failed to create refrigerator endpoint");
        return;
    }
    refrigerator_endpoint_id = endpoint::get_id(refrigerator_endpoint);
    ESP_LOGI(TAG, "Refrigerator endpoint id: %u", refrigerator_endpoint_id);

    /* 3a. Identify + RefrigeratorAlarm onto the root endpoint — both
     * optionalConform, so refrigerator::create() doesn't add them. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(refrigerator_endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    cluster::refrigerator_alarm::config_t alarm_config;
    alarm_config.mask = (uint32_t)RefrigeratorAlarm::AlarmBitmap::kDoorOpen;
    alarm_config.supported = (uint32_t)RefrigeratorAlarm::AlarmBitmap::kDoorOpen;
    alarm_config.state = 0;
    cluster::refrigerator_alarm::create(refrigerator_endpoint, &alarm_config, CLUSTER_FLAG_SERVER);

    /* 3a-2. Activated Carbon Filter Monitoring — optionalConform as of
     * Refrigerator.xml revision 3, added onto the same root endpoint the
     * same "extra cluster afterward" way Identify/RefrigeratorAlarm just
     * were. See carbon_filter_life_task()'s own header comment above for
     * why this one's life estimate is plain wall-clock elapsed time rather
     * than gated by any run-state. */
    cluster::activated_carbon_filter_monitoring::config_t carbon_filter_config;
    cluster_t *carbon_filter_cluster = cluster::activated_carbon_filter_monitoring::create(
        refrigerator_endpoint, &carbon_filter_config, CLUSTER_FLAG_SERVER);
    if (!carbon_filter_cluster) {
        ESP_LOGE(TAG, "Failed to create activated carbon filter monitoring cluster");
        return;
    }
    cluster::resource_monitoring::feature::condition::config_t carbon_filter_condition_config;
    carbon_filter_condition_config.condition = 100; /* fresh filter until NVS says otherwise, corrected on the first poll */
    carbon_filter_condition_config.degradation_direction =
        chip::to_underlying(ResourceMonitoring::DegradationDirectionEnum::kDown);
    cluster::resource_monitoring::feature::condition::add(carbon_filter_cluster, &carbon_filter_condition_config);

    /* 3b. Fridge compartment. Note: unlike esp-matter's newer "generated"
     * data model (only enabled via CONFIG_ESP_MATTER_ENABLE_GENERATED_
     * DATA_MODEL, off by default and left off here, same as every other
     * device type in this repo), the "legacy" cluster/endpoint set this
     * build actually compiles against does NOT set the TN feature flag
     * automatically inside temperature_controlled_cabinet::add() — confirmed
     * by reading BOTH implementations side by side after an initial Docker
     * build caught the mismatch: the legacy header's own field name is also
     * `temp_setpoint`, not `temperature_setpoint`. `feature_flags` is set
     * explicitly below as a result. */
    endpoint::temperature_controlled_cabinet::config_t fridge_config;
    fridge_config.temperature_control.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    fridge_config.temperature_control.features.temperature_number.temp_setpoint =
        REFRIGERATOR_FRIDGE_DEFAULT_SETPOINT_CENTIDEGREES;
    fridge_config.temperature_control.features.temperature_number.min_temperature = REFRIGERATOR_FRIDGE_MIN_CENTIDEGREES;
    fridge_config.temperature_control.features.temperature_number.max_temperature = REFRIGERATOR_FRIDGE_MAX_CENTIDEGREES;

    endpoint_t *fridge_endpoint =
        endpoint::temperature_controlled_cabinet::create(node, &fridge_config, ENDPOINT_FLAG_NONE, NULL);
    if (!fridge_endpoint) {
        ESP_LOGE(TAG, "Failed to create fridge cabinet endpoint");
        return;
    }
    uint16_t fridge_endpoint_id = endpoint::get_id(fridge_endpoint);
    ESP_LOGI(TAG, "Fridge cabinet endpoint id: %u", fridge_endpoint_id);

    cluster::refrigerator_and_tcc_mode::config_t fridge_mode_config;
    fridge_mode_config.current_mode = 0;
    fridge_mode_config.delegate = &fridge_mode_delegate;
    cluster::refrigerator_and_tcc_mode::create(fridge_endpoint, &fridge_mode_config,
                                                                          CLUSTER_FLAG_SERVER);

    cluster::temperature_measurement::config_t fridge_meas_config;
    fridge_meas_config.measured_value = nullable<int16_t>();
    fridge_meas_config.min_measured_value = nullable<int16_t>(REFRIGERATOR_FRIDGE_MEASURE_MIN_CENTIDEGREES);
    fridge_meas_config.max_measured_value = nullable<int16_t>(REFRIGERATOR_FRIDGE_MEASURE_MAX_CENTIDEGREES);
    cluster::temperature_measurement::create(fridge_endpoint, &fridge_meas_config, CLUSTER_FLAG_SERVER);

    err = set_parent_endpoint(fridge_endpoint, refrigerator_endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set fridge cabinet's parent endpoint: %d", err);
        return;
    }

    /* 3c. Freezer compartment — same `feature_flags`/`temp_setpoint` note
     * as the fridge compartment above. */
    endpoint::temperature_controlled_cabinet::config_t freezer_config;
    freezer_config.temperature_control.feature_flags = cluster::temperature_control::feature::temperature_number::get_id();
    freezer_config.temperature_control.features.temperature_number.temp_setpoint =
        REFRIGERATOR_FREEZER_DEFAULT_SETPOINT_CENTIDEGREES;
    freezer_config.temperature_control.features.temperature_number.min_temperature = REFRIGERATOR_FREEZER_MIN_CENTIDEGREES;
    freezer_config.temperature_control.features.temperature_number.max_temperature = REFRIGERATOR_FREEZER_MAX_CENTIDEGREES;

    endpoint_t *freezer_endpoint =
        endpoint::temperature_controlled_cabinet::create(node, &freezer_config, ENDPOINT_FLAG_NONE, NULL);
    if (!freezer_endpoint) {
        ESP_LOGE(TAG, "Failed to create freezer cabinet endpoint");
        return;
    }
    uint16_t freezer_endpoint_id = endpoint::get_id(freezer_endpoint);
    ESP_LOGI(TAG, "Freezer cabinet endpoint id: %u", freezer_endpoint_id);

    cluster::refrigerator_and_tcc_mode::config_t freezer_mode_config;
    freezer_mode_config.current_mode = 0;
    freezer_mode_config.delegate = &freezer_mode_delegate;
    cluster::refrigerator_and_tcc_mode::create(freezer_endpoint, &freezer_mode_config,
                                                                          CLUSTER_FLAG_SERVER);

    cluster::temperature_measurement::config_t freezer_meas_config;
    freezer_meas_config.measured_value = nullable<int16_t>();
    freezer_meas_config.min_measured_value = nullable<int16_t>(REFRIGERATOR_FREEZER_MEASURE_MIN_CENTIDEGREES);
    freezer_meas_config.max_measured_value = nullable<int16_t>(REFRIGERATOR_FREEZER_MEASURE_MAX_CENTIDEGREES);
    cluster::temperature_measurement::create(freezer_endpoint, &freezer_meas_config, CLUSTER_FLAG_SERVER);

    err = set_parent_endpoint(freezer_endpoint, refrigerator_endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set freezer cabinet's parent endpoint: %d", err);
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

    /* 5. Start the control tasks — read each DS18B20, run its hysteresis
     * logic, and drive its relay for as long as the device runs; plus the
     * door sensor's own debounced poll. */
    fridge_runtime = {
        .name = "Fridge",
        .endpoint_id = fridge_endpoint_id,
        .sensor_gpio = REFRIGERATOR_FRIDGE_SENSOR_GPIO,
        .relay_gpio = REFRIGERATOR_FRIDGE_RELAY_GPIO,
        .mode_delegate = &fridge_mode_delegate,
        .relay_on = false,
    };
    freezer_runtime = {
        .name = "Freezer",
        .endpoint_id = freezer_endpoint_id,
        .sensor_gpio = REFRIGERATOR_FREEZER_SENSOR_GPIO,
        .relay_gpio = REFRIGERATOR_FREEZER_RELAY_GPIO,
        .mode_delegate = &freezer_mode_delegate,
        .relay_on = false,
    };

    xTaskCreate(cabinet_control_task, "fridge_task", 4096, &fridge_runtime, 5, NULL);
    xTaskCreate(cabinet_control_task, "freezer_task", 4096, &freezer_runtime, 5, NULL);
    xTaskCreate(door_task, "door_task", 3072, NULL, 5, NULL);
    xTaskCreate(carbon_filter_life_task, "carbon_filter_life_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter refrigerator started. Scan the QR code to commission.");
}
