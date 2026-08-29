/*
 * Minimal Matter Battery Storage — fifty-seventh device type: an AC-coupled
 * home battery storage system (the class of hardware built around a real
 * hybrid inverter/battery bank, e.g. Victron/Growatt-class or a simpler DIY
 * off-grid inverter+battery setup) — real AC-side power/energy metering via
 * firmware/solar-power/'s own already-proven 6-chip subsystem, plus real
 * battery state (voltage + a derived percent-remaining) via a simple ADC
 * voltage divider.
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 *
 * --- The CSA's own BatteryStorage.xml is genuinely malformed in this SDK,
 * and esp-matter's own top-level helper already resolves the ambiguity ---
 * The CSA's own data_model/1.6/device_types/BatteryStorage.xml (device type
 * 0x0018, revision 2) is confirmed, by reading the raw file directly with
 * `cat -A` (not an extraction artifact), to be internally inconsistent: its
 * own revision-history text claims "Added mandate of two Power Source and
 * Electrical Sensor composed devices types," but the actual XML body only
 * contains ONE `<composedDeviceTypes><deviceType id="0x0510".../></
 * composedDeviceTypes>` block (with its own `<clusterRequirements>`
 * duplicating the SAME two cluster entries instead of declaring two
 * distinct instances) plus a second, structurally invalid
 * `<composedDeviceTypes>` block containing a bare `<cluster>` element with
 * no `<deviceType>` wrapper at all. Rather than guess at the CSA's
 * unpublished intent, this file instead uses esp-matter's own complete,
 * ready-to-use top-level helper — `endpoint::battery_storage::create()`,
 * confirmed to exist by reading `esp_matter_endpoint.cpp`'s own
 * `battery_storage::add()` directly — which represents Espressif's own SDK
 * engineers' authoritative resolution of the same ambiguity: ONE endpoint
 * composing a real PowerSource (Battery + Rechargeable features, with a
 * genuine battery-metrics attribute set: BatVoltage, BatPercentRemaining,
 * BatTimeRemaining, BatCapacity, BatTimeToFullCharge, BatChargingCurrent,
 * ActiveBatFaults, ActiveBatChargeFaults) alongside ONE composed Electrical
 * Sensor (EPM+EEM, ExportedEnergy+CumulativeEnergy features force-set —
 * same "generates/exports power" framing firmware/solar-power/'s own
 * device type already establishes) and DeviceEnergyManagement
 * [PowerAdjustment] — not two of anything. This is the same same-endpoint,
 * multiple-device-types-on-one-DeviceTypeList composition style firmware/
 * heat-pump/'s and firmware/solar-power/'s own header comments already
 * confirm valid, applied here to resolve a genuinely ambiguous spec
 * document rather than an unambiguous one.
 *
 * --- PowerSource's battery attributes: plain ember attributes, despite a
 * real `data_model_provider/clusters/power_source/` folder existing -------
 * A real `power_source/` folder DOES exist under `data_model_provider/
 * clusters/` in this SDK — but reading its own `integration.cpp` directly
 * shows it only implements a `PowerSourceAttrAccess`/`PowerSourceServer`
 * pair for PowerSource's own cross-endpoint bookkeeping (EndpointList,
 * multi-power-source-per-node Order/Status — the SDK's own comment there
 * literally says this exists "to keep generated-data-model behavior
 * consistent," not to replace the plain ember attribute store for the
 * individual battery metrics). Confirmed by reading `battery_storage::
 * add()` itself: it populates BatVoltage/BatPercentRemaining/etc. via the
 * exact same plain `attribute::create_bat_voltage()`-style calls every
 * other plain-ember cluster in this repo already uses — so this file
 * writes them at runtime via the ordinary `attribute::update()` +
 * `esp_matter_nullable_uint32()`/`esp_matter_nullable_uint8()` helpers, not
 * a registry-lookup-and-cast pattern.
 *
 * --- Battery state: a real ADC voltage-divider reading, not fabricated ----
 * `BATTERY_STORAGE_VOLTAGE_ADC_GPIO` reads the actual battery pack voltage
 * through a plain resistive divider (a real, standard technique for
 * monitoring a DC battery pack's terminal voltage over an MCU's ADC — the
 * same ADC1 + `esp_adc/adc_cali.h` calibration pattern firmware/
 * light-sensor/'s own LDR and firmware/soil-sensor/'s own capacitive probe
 * already establish) — reported directly as `BatVoltage` (millivolts, per
 * the cluster's own documented unit). `BatPercentRemaining` is derived via
 * a plain two-point linear map between `BATTERY_STORAGE_EMPTY_MV`/
 * `BATTERY_STORAGE_FULL_MV` (both adjustable `#define`s, not a calibrated
 * chemistry-specific discharge curve) — same "adjustable, not a calibrated
 * reading" honesty precedent firmware/soil-sensor/'s own two-point
 * moisture calibration already establishes. `BatCapacity` (the pack's own
 * rated capacity in mAh) is a plain adjustable `#define` the owner sets to
 * their own real pack's rated spec — same "adjustable, real per-unit
 * config" precedent firmware/cooktop/'s own description string and
 * firmware/water-heater/'s own tank-probe choice already establish, not a
 * measurement. `BatTimeRemaining`/`BatTimeToFullCharge`/`BatChargingCurrent`
 * are deliberately left at their created null defaults — this file has no
 * dedicated charging-current sensor (a genuinely different hardware
 * category from the AC-side power-monitor chips below, which measure the
 * whole system's net AC power flow, not the DC charging current
 * specifically) — same "no sensor, no fabricated data" honesty precedent
 * this repo applies throughout, rather than a technical limitation of the
 * cluster itself.
 *
 * --- Power/energy metering: firmware/solar-power/'s own 6-chip subsystem,
 * reused verbatim, framed as the system's net AC power flow ---------------
 * `BATTERY_STORAGE_CHIP` is the exact same 6-way choice firmware/
 * solar-power/'s and firmware/electrical-meter/'s own equivalent `#define`s
 * already offer (BL0942, BL0937, HLW8012, CSE7759, CSE7766, ADE7953) —
 * every driver, protocol detail, and sourcing citation reused unchanged.
 * Wiring one of these chips onto the inverter's own AC port (whichever
 * side the installation exposes for metering — grid-tie or load output)
 * measures exactly the same physics as measuring any other AC circuit; the
 * `ExportedEnergy` framing `battery_storage::add()` itself force-sets
 * matches a battery system's own most natural interpretation (energy
 * flowing OUT of storage to loads), the same semantic reasoning firmware/
 * solar-power/'s own header comment already establishes for its own
 * Exported-not-Imported correction.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no module of any of the six power-monitor chips, and no
 * battery pack/voltage-divider hardware, was physically available when
 * written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#include <esp_matter.h>
/* Deliberately no explicit #include for electrical_meter_device.h — see
 * firmware/outlet/'s own identical comment above its own equivalent
 * #include block for why (the transitive-include double-parse issue that
 * comment documents applies here too, confirmed by reusing the same
 * avoidance). */
#include <data_model_provider/clusters/electrical_power_measurement/integration.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <app/reporting/reporting.h> /* MatterReportingAttributeChangeCallback() */

static const char *TAG = "matter_battery_storage";

/* LED for the Matter "Identify" cluster. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* --- Battery voltage: a real ADC + resistive-divider reading — see the
 * header comment above for the full explanation. GPIO 34 is ADC1 channel
 * 6 on classic ESP32 (WROOM-32) — deliberately ADC1, not ADC2 (unreliable
 * once Wi-Fi is active, same reasoning firmware/soil-sensor/'s and
 * firmware/light-sensor/'s own ADC1 choices already establish). ---
 * `BATTERY_STORAGE_DIVIDER_RATIO` is the resistive divider's own scale
 * factor (e.g. a 100k:10k divider gives an 11:1 ratio, suitable for
 * scaling a 12-48V lead-acid/LiFePO4 pack's own terminal voltage down
 * into the ADC's 0-3.3V input range) — adjust to match your own divider's
 * real resistor values, this default is only a common reference ratio,
 * not a measured calibration. */
#define BATTERY_STORAGE_VOLTAGE_ADC_GPIO GPIO_NUM_34
#define BATTERY_STORAGE_ADC_UNIT ADC_UNIT_1
#define BATTERY_STORAGE_ADC_CHANNEL ADC_CHANNEL_6
#define BATTERY_STORAGE_ADC_ATTEN ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define BATTERY_STORAGE_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define BATTERY_STORAGE_SUPPLY_MV 3300.0f
#define BATTERY_STORAGE_SAMPLE_COUNT 16
#define BATTERY_STORAGE_DIVIDER_RATIO 11.0f
#define BATTERY_STORAGE_VOLTAGE_POLL_INTERVAL_MS 30000

/* Two-point percent-remaining calibration — see the header comment above
 * for why these are adjustable placeholders, not a calibrated chemistry-
 * specific discharge curve. Defaults assume a 4S LiFePO4 pack (nominal
 * 12.8V): ~10.0V empty, ~14.6V full — adjust to match your own pack's
 * real chemistry/cell count. */
#define BATTERY_STORAGE_EMPTY_MV 10000
#define BATTERY_STORAGE_FULL_MV 14600

/* The battery pack's own rated capacity, in mAh — a real, adjustable
 * per-unit config value (the owner's own pack's rated spec), not a
 * measurement. */
#define BATTERY_STORAGE_CAPACITY_MAH 100000u

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

/* --- Power-monitoring chip choice — see the header comment above for the
 * full explanation. Six real chips, reused verbatim from firmware/
 * outlet/'s own identical #define scaffold and per-chip protocol
 * constants (see that file's own header comment for the complete
 * per-chip citation detail). --- */
#define BATTERY_STORAGE_CHIP_BL0942 1
#define BATTERY_STORAGE_CHIP_BL0937 2
#define BATTERY_STORAGE_CHIP_HLW8012 3
#define BATTERY_STORAGE_CHIP_CSE7759 4
#define BATTERY_STORAGE_CHIP_CSE7766 5
#define BATTERY_STORAGE_CHIP_ADE7953 6
#define BATTERY_STORAGE_CHIP BATTERY_STORAGE_CHIP_BL0942

#if BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_BL0942
/* UART, request/response — same UART2-default pins firmware/outlet/'s
 * own BL0942 branch uses. */
#define BL0942_UART_PORT UART_NUM_1
#define BL0942_UART_RX_GPIO GPIO_NUM_16
#define BL0942_UART_TX_GPIO GPIO_NUM_17
#define BL0942_UART_BAUD_RATE 4800
#define BL0942_DEVICE_ADDRESS 0
#define BL0942_READ_COMMAND 0x58
#define BL0942_FULL_PACKET 0xAA
#define BL0942_RESPONSE_LEN 23
#define BL0942_UREF 15883.34116
#define BL0942_IREF 251065.6814
#define BL0942_PREF 623.0270705
#define BL0942_EREF 5347.484240
#define BL0942_POLL_INTERVAL_MS 10000

#elif BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_BL0937 || \
      BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_HLW8012 || \
      BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_CSE7759
#define PULSE_METER_SEL_GPIO GPIO_NUM_25
#define PULSE_METER_CF_GPIO GPIO_NUM_26
#define PULSE_METER_CF1_GPIO GPIO_NUM_27
#define PULSE_METER_VOLTAGE_DIVIDER 1981.0f
#define PULSE_METER_CURRENT_RESISTOR 0.001f
#define PULSE_METER_WINDOW_MS 2000

#if BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_BL0937
#define PULSE_METER_REFERENCE_VOLTAGE 1.218f
#else /* HLW8012 / CSE7759 */
#define PULSE_METER_REFERENCE_VOLTAGE 2.43f
#define HLW8012_CLOCK_FREQUENCY 3579000.0f
#endif

#elif BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_CSE7766
#define CSE7766_UART_PORT UART_NUM_1
#define CSE7766_UART_RX_GPIO GPIO_NUM_16
#define CSE7766_UART_TX_GPIO GPIO_NUM_17
#define CSE7766_UART_BAUD_RATE 4800
#define CSE7766_PACKET_LEN 24

#elif BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_ADE7953
#define ADE7953_I2C_ADDR 0x38
#define ADE7953_SDA_GPIO GPIO_NUM_21
#define ADE7953_SCL_GPIO GPIO_NUM_22
#define ADE7953_I2C_FREQ_HZ 100000
#define ADE7953_POLL_INTERVAL_MS 10000
#define ADE7953_REG_UNLOCK_8 0x00FE
#define ADE7953_REG_UNLOCK_16 0x0120
#define ADE7953_REG_CONFIG_16 0x0102
#define ADE7953_REG_VRMS_32 0x031C
#define ADE7953_REG_IRMS_A_32 0x031A
#define ADE7953_REG_AWATT_A_32 0x0312
#define ADE7953_VOLTAGE_DIVISOR 26000.0f
#define ADE7953_CURRENT_DIVISOR 100000.0f
#define ADE7953_POWER_DIVISOR 154.0f
#endif

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t battery_storage_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static int64_t cumulative_energy_mwh = 0; /* accumulated since boot; not persisted across reboots */

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* ==========================================================================
 * Battery voltage — a real ADC + resistive-divider reading. Same portable
 * curve-fitting/line-fitting calibration pattern firmware/soil-sensor/'s
 * and firmware/light-sensor/'s own ADC setup already establish; see this
 * file's own header comment for the full explanation.
 * ========================================================================== */
static adc_oneshot_unit_handle_t battery_adc_handle = NULL;
static adc_cali_handle_t battery_adc_cali_handle = NULL;
static bool battery_adc_cali_available = false;

static bool battery_setup_adc(void)
{
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = BATTERY_STORAGE_ADC_UNIT;

    esp_err_t err = adc_oneshot_new_unit(&init_config, &battery_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }

    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = BATTERY_STORAGE_ADC_ATTEN;
    chan_config.bitwidth = BATTERY_STORAGE_ADC_BITWIDTH;

    err = adc_oneshot_config_channel(battery_adc_handle, BATTERY_STORAGE_ADC_CHANNEL, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s", esp_err_to_name(err));
        return false;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = BATTERY_STORAGE_ADC_UNIT;
    cali_config.chan = BATTERY_STORAGE_ADC_CHANNEL;
    cali_config.atten = BATTERY_STORAGE_ADC_ATTEN;
    cali_config.bitwidth = BATTERY_STORAGE_ADC_BITWIDTH;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &battery_adc_cali_handle);
    battery_adc_cali_available = (err == ESP_OK);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {};
    cali_config.unit_id = BATTERY_STORAGE_ADC_UNIT;
    cali_config.atten = BATTERY_STORAGE_ADC_ATTEN;
    cali_config.bitwidth = BATTERY_STORAGE_ADC_BITWIDTH;
#if CONFIG_IDF_TARGET_ESP32
    cali_config.default_vref = (uint32_t)BATTERY_STORAGE_SUPPLY_MV;
#endif
    err = adc_cali_create_scheme_line_fitting(&cali_config, &battery_adc_cali_handle);
    battery_adc_cali_available = (err == ESP_OK);
#else
    err = ESP_ERR_NOT_SUPPORTED;
    battery_adc_cali_available = false;
#endif

    if (!battery_adc_cali_available) {
        ESP_LOGW(TAG, "ADC calibration unavailable (%s) — using uncalibrated voltage estimate",
                 esp_err_to_name(err));
    }
    return true;
}

/* Reads and averages BATTERY_STORAGE_SAMPLE_COUNT raw ADC samples, converts
 * to millivolts at the ADC pin, then scales by BATTERY_STORAGE_DIVIDER_RATIO
 * to recover the real pack voltage. */
static bool battery_read_pack_mv(int *out_pack_mv)
{
    int64_t sum_raw = 0;
    for (int i = 0; i < BATTERY_STORAGE_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(battery_adc_handle, BATTERY_STORAGE_ADC_CHANNEL, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
            return false;
        }
        sum_raw += raw;
    }
    int avg_raw = (int)(sum_raw / BATTERY_STORAGE_SAMPLE_COUNT);

    int pin_mv;
    if (battery_adc_cali_available && adc_cali_raw_to_voltage(battery_adc_cali_handle, avg_raw, &pin_mv) == ESP_OK) {
        /* calibrated */
    } else {
        pin_mv = (int)((float)avg_raw * BATTERY_STORAGE_SUPPLY_MV / 4095.0f);
    }

    *out_pack_mv = (int)((float)pin_mv * BATTERY_STORAGE_DIVIDER_RATIO);
    return true;
}

/* Two-point percent-remaining calibration — see the header comment above. */
static uint8_t battery_mv_to_percent(int pack_mv)
{
    if (pack_mv <= BATTERY_STORAGE_EMPTY_MV) {
        return 0;
    }
    if (pack_mv >= BATTERY_STORAGE_FULL_MV) {
        return 100;
    }
    float fraction = (float)(pack_mv - BATTERY_STORAGE_EMPTY_MV) / (float)(BATTERY_STORAGE_FULL_MV - BATTERY_STORAGE_EMPTY_MV);
    return (uint8_t)(fraction * 100.0f + 0.5f);
}

/* Polls the battery voltage and writes BatVoltage/BatPercentRemaining —
 * both plain ember attributes (see the header comment above for why),
 * written via the ordinary attribute::update() pattern. BatPercentRemaining
 * is expressed in the cluster's own half-percent units (0-200), per its
 * own documented 0.5% resolution. */
static void battery_voltage_task(void *arg)
{
    if (!battery_setup_adc()) {
        ESP_LOGE(TAG, "Battery voltage ADC setup failed — battery state disabled");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        int pack_mv;
        if (battery_read_pack_mv(&pack_mv)) {
            uint8_t percent = battery_mv_to_percent(pack_mv);

            esp_matter_attr_val_t voltage_val = esp_matter_nullable_uint32(nullable<uint32_t>((uint32_t)pack_mv));
            attribute::update(battery_storage_endpoint_id, PowerSource::Id, PowerSource::Attributes::BatVoltage::Id, &voltage_val);

            esp_matter_attr_val_t percent_val = esp_matter_nullable_uint8(nullable<uint8_t>((uint8_t)(percent * 2)));
            attribute::update(battery_storage_endpoint_id, PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id, &percent_val);

            ESP_LOGI(TAG, "Battery: %d mV (%u%%)", pack_mv, percent);
        }
        vTaskDelay(pdMS_TO_TICKS(BATTERY_STORAGE_VOLTAGE_POLL_INTERVAL_MS));
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

/* No controller-writable attributes on this device — all state flows from
 * the power-monitor task below. */
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

/* ==========================================================================
 * Power monitoring — ElectricalPowerMeasurement + ElectricalEnergyMeasurement.
 * See the file header comment for the full explanation of the Delegate/
 * Instance integration pattern and firmware/outlet/'s own header comment
 * for the complete per-chip protocol/formula detail and sourcing.
 * ========================================================================== */

/* Adapted from esp-matter's own reference implementation
 * (examples/all_device_types_app/main/electrical_measurement/), the exact
 * same trimmed-to-ActivePower/RMSVoltage/RMSCurrent Delegate firmware/
 * outlet/'s own OutletPowerDelegate already establishes. */
namespace chip { namespace app { namespace Clusters { namespace ElectricalPowerMeasurement {

class MeterPowerDelegate : public Delegate {
public:
    PowerModeEnum GetPowerMode() override { return PowerModeEnum::kAc; }
    uint8_t GetNumberOfMeasurementTypes() override { return 3; } /* ActivePower, RMSVoltage, RMSCurrent */

    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        static const Structs::MeasurementAccuracyRangeStruct::Type kPowerRange[] = {
            { .rangeMin = -50000000, .rangeMax = 50000000,
              .percentMax = chip::MakeOptional(static_cast<chip::Percent100ths>(500)) },
        };
        static const Structs::MeasurementAccuracyRangeStruct::Type kCurrentRange[] = {
            { .rangeMin = 0, .rangeMax = 100000,
              .percentMax = chip::MakeOptional(static_cast<chip::Percent100ths>(500)) },
        };
        static const Structs::MeasurementAccuracyRangeStruct::Type kVoltageRange[] = {
            { .rangeMin = 0, .rangeMax = 260000,
              .percentMax = chip::MakeOptional(static_cast<chip::Percent100ths>(500)) },
        };
        switch (index) {
        case 0:
            accuracy = { .measurementType = MeasurementTypeEnum::kActivePower, .measured = true,
                         .minMeasuredValue = -50000000, .maxMeasuredValue = 50000000,
                         .accuracyRanges = chip::app::DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kPowerRange) };
            return CHIP_NO_ERROR;
        case 1:
            accuracy = { .measurementType = MeasurementTypeEnum::kRMSCurrent, .measured = true,
                         .minMeasuredValue = 0, .maxMeasuredValue = 100000,
                         .accuracyRanges = chip::app::DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kCurrentRange) };
            return CHIP_NO_ERROR;
        case 2:
            accuracy = { .measurementType = MeasurementTypeEnum::kRMSVoltage, .measured = true,
                         .minMeasuredValue = 0, .maxMeasuredValue = 260000,
                         .accuracyRanges = chip::app::DataModel::List<const Structs::MeasurementAccuracyRangeStruct::Type>(kVoltageRange) };
            return CHIP_NO_ERROR;
        default:
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
    }
    CHIP_ERROR EndAccuracyRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartRangesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetRangeByIndex(uint8_t, Structs::MeasurementRangeStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndRangesRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicCurrentsByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicCurrentsRead() override { return CHIP_NO_ERROR; }

    CHIP_ERROR StartHarmonicPhasesRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetHarmonicPhasesByIndex(uint8_t, Structs::HarmonicMeasurementStruct::Type &) override { return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED; }
    CHIP_ERROR EndHarmonicPhasesRead() override { return CHIP_NO_ERROR; }

    DataModel::Nullable<int64_t> GetVoltage() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetActiveCurrent() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetReactiveCurrent() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetApparentCurrent() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetActivePower() override { return mActivePower; }
    DataModel::Nullable<int64_t> GetReactivePower() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetApparentPower() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetRMSVoltage() override { return mRMSVoltage; }
    DataModel::Nullable<int64_t> GetRMSCurrent() override { return mRMSCurrent; }
    DataModel::Nullable<int64_t> GetRMSPower() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetFrequency() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetPowerFactor() override { return DataModel::Nullable<int64_t>(); }
    DataModel::Nullable<int64_t> GetNeutralCurrent() override { return DataModel::Nullable<int64_t>(); }

    void SetActivePower(DataModel::Nullable<int64_t> v)
    {
        if (mActivePower != v) {
            mActivePower = v;
            MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Attributes::ActivePower::Id);
        }
    }
    void SetRMSVoltage(DataModel::Nullable<int64_t> v)
    {
        if (mRMSVoltage != v) {
            mRMSVoltage = v;
            MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Attributes::RMSVoltage::Id);
        }
    }
    void SetRMSCurrent(DataModel::Nullable<int64_t> v)
    {
        if (mRMSCurrent != v) {
            mRMSCurrent = v;
            MatterReportingAttributeChangeCallback(mEndpointId, ElectricalPowerMeasurement::Id, Attributes::RMSCurrent::Id);
        }
    }

private:
    DataModel::Nullable<int64_t> mActivePower;
    DataModel::Nullable<int64_t> mRMSVoltage;
    DataModel::Nullable<int64_t> mRMSCurrent;
};

} } } } /* chip::app::Clusters::ElectricalPowerMeasurement */

static chip::app::Clusters::ElectricalPowerMeasurement::MeterPowerDelegate power_delegate;
static chip::app::Clusters::ElectricalPowerMeasurement::Instance *power_instance = NULL;

/* Reports one new set of readings — same shape as firmware/outlet/'s own
 * report_power(). */
static void report_power(double watts, double volts, double amps, int64_t delta_energy_mwh)
{
    power_delegate.SetActivePower(chip::app::DataModel::MakeNullable((int64_t)(watts * 1000.0)));
    power_delegate.SetRMSVoltage(chip::app::DataModel::MakeNullable((int64_t)(volts * 1000.0)));
    power_delegate.SetRMSCurrent(chip::app::DataModel::MakeNullable((int64_t)(amps * 1000.0)));

    if (delta_energy_mwh > 0) {
        cumulative_energy_mwh += delta_energy_mwh;
    }

    /* Exported, not Imported — see the header comment above for why: this
     * device's storage discharges power to loads, and `battery_storage::
     * add()` force-sets the ExportedEnergy feature (not ImportedEnergy) on
     * ElectricalEnergyMeasurement accordingly, same as firmware/
     * solar-power/'s own identical framing. */
    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::EnergyMeasurementStruct::Type exported = {};
    exported.energy = cumulative_energy_mwh;
    exported.endSystime = chip::MakeOptional(static_cast<uint64_t>(
        chip::System::SystemClock().GetMonotonicTimestamp().count()));
    chip::app::DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> exported_nullable(exported);
    chip::app::DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> imported_nullable; /* generation only, no consumption tracked */
    NotifyCumulativeEnergyMeasured(battery_storage_endpoint_id, imported_nullable, exported_nullable);

    ESP_LOGI(TAG, "Power: %.1f W, %.1f V, %.3f A — cumulative %.3f kWh",
             watts, volts, amps, (double)cumulative_energy_mwh / 1000000.0);
}

#if BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_BL0942

static bool bl0942_uart_setup(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = BL0942_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    esp_err_t err = uart_driver_install(BL0942_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(BL0942_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(BL0942_UART_PORT, BL0942_UART_TX_GPIO, BL0942_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* See firmware/outlet/'s own header comment for the exact packet layout
 * (per Shanghai Belling's own BL0942 datasheet) and where the reference
 * constants come from. */
static bool bl0942_read(double *out_watts, double *out_volts, double *out_amps, int64_t *out_delta_energy_mwh)
{
    uint8_t cmd[2] = { (uint8_t)(BL0942_READ_COMMAND | BL0942_DEVICE_ADDRESS), BL0942_FULL_PACKET };
    int written = uart_write_bytes(BL0942_UART_PORT, (const char *)cmd, sizeof(cmd));
    if (written != (int)sizeof(cmd)) {
        ESP_LOGW(TAG, "BL0942 write failed");
        return false;
    }

    uint8_t resp[BL0942_RESPONSE_LEN];
    int read = uart_read_bytes(BL0942_UART_PORT, resp, sizeof(resp), pdMS_TO_TICKS(500));
    if (read != BL0942_RESPONSE_LEN) {
        ESP_LOGW(TAG, "BL0942 read timed out or short (%d/%d bytes)", read, BL0942_RESPONSE_LEN);
        return false;
    }
    if (resp[0] != 0x55) {
        ESP_LOGW(TAG, "BL0942 bad frame header 0x%02x", resp[0]);
        return false;
    }

    uint8_t checksum = (uint8_t)(BL0942_READ_COMMAND | BL0942_DEVICE_ADDRESS);
    for (int i = 0; i < BL0942_RESPONSE_LEN - 1; i++) {
        checksum = (uint8_t)(checksum + resp[i]);
    }
    checksum ^= 0xFF;
    if (checksum != resp[BL0942_RESPONSE_LEN - 1]) {
        ESP_LOGW(TAG, "BL0942 checksum mismatch (got 0x%02x, expected 0x%02x)", resp[BL0942_RESPONSE_LEN - 1], checksum);
        return false;
    }

    uint32_t i_rms_raw = ((uint32_t)resp[3] << 16) | ((uint32_t)resp[2] << 8) | resp[1];
    uint32_t v_rms_raw = ((uint32_t)resp[6] << 16) | ((uint32_t)resp[5] << 8) | resp[4];
    int32_t watt_raw = ((int32_t)resp[12] << 16) | ((int32_t)resp[11] << 8) | resp[10];
    if (watt_raw & 0x800000) {
        watt_raw |= (int32_t)0xFF000000;
    }
    uint32_t cf_cnt_raw = ((uint32_t)resp[15] << 16) | ((uint32_t)resp[14] << 8) | resp[13];

    *out_volts = (double)v_rms_raw / BL0942_UREF;
    *out_amps = (double)i_rms_raw / BL0942_IREF;
    *out_watts = (double)watt_raw / BL0942_PREF;

    static bool have_last_cf_cnt = false;
    static uint32_t last_cf_cnt = 0;
    uint32_t delta_pulses;
    if (!have_last_cf_cnt) {
        delta_pulses = 0;
        have_last_cf_cnt = true;
    } else if (cf_cnt_raw >= last_cf_cnt) {
        delta_pulses = cf_cnt_raw - last_cf_cnt;
    } else {
        delta_pulses = (0x1000000 - last_cf_cnt) + cf_cnt_raw; /* 24-bit wrap */
    }
    last_cf_cnt = cf_cnt_raw;
    *out_delta_energy_mwh = (int64_t)((double)delta_pulses / BL0942_EREF * 1000.0);

    return true;
}

static void power_monitor_task(void *arg)
{
    if (!bl0942_uart_setup()) {
        ESP_LOGE(TAG, "BL0942 UART setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        double watts = 0, volts = 0, amps = 0;
        int64_t delta_energy_mwh = 0;
        if (bl0942_read(&watts, &volts, &amps, &delta_energy_mwh)) {
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        vTaskDelay(pdMS_TO_TICKS(BL0942_POLL_INTERVAL_MS));
    }
}

#elif BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_BL0937 || \
      BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_HLW8012 || \
      BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_CSE7759

static volatile uint32_t pulse_meter_cf_edges = 0;
static volatile uint32_t pulse_meter_cf1_edges = 0;

static void IRAM_ATTR pulse_meter_cf_isr(void *arg)
{
    pulse_meter_cf_edges++;
}

static void IRAM_ATTR pulse_meter_cf1_isr(void *arg)
{
    pulse_meter_cf1_edges++;
}

static bool pulse_meter_gpio_setup(void)
{
    gpio_config_t sel_conf = {};
    sel_conf.pin_bit_mask = (1ULL << PULSE_METER_SEL_GPIO);
    sel_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&sel_conf);
    gpio_set_level(PULSE_METER_SEL_GPIO, 0);

    gpio_config_t pulse_conf = {};
    pulse_conf.pin_bit_mask = (1ULL << PULSE_METER_CF_GPIO) | (1ULL << PULSE_METER_CF1_GPIO);
    pulse_conf.mode = GPIO_MODE_INPUT;
    pulse_conf.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&pulse_conf);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
        return false;
    }
    gpio_isr_handler_add(PULSE_METER_CF_GPIO, pulse_meter_cf_isr, NULL);
    gpio_isr_handler_add(PULSE_METER_CF1_GPIO, pulse_meter_cf1_isr, NULL);
    return true;
}

/* See firmware/outlet/'s own header comment for the full BL0937/HLW8012/
 * CSE7759 multiplier-formula derivation and sourcing. */
static void power_monitor_task(void *arg)
{
    if (!pulse_meter_gpio_setup()) {
        ESP_LOGE(TAG, "Pulse meter GPIO setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

#if BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_BL0937
    const float power_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_REFERENCE_VOLTAGE *
                                    PULSE_METER_VOLTAGE_DIVIDER / PULSE_METER_CURRENT_RESISTOR / 1721506.0f;
    const float current_multiplier = PULSE_METER_REFERENCE_VOLTAGE / PULSE_METER_CURRENT_RESISTOR / 94638.0f;
    const float voltage_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_VOLTAGE_DIVIDER / 15397.0f;
#else /* HLW8012 / CSE7759 */
    const float power_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_REFERENCE_VOLTAGE *
                                    PULSE_METER_VOLTAGE_DIVIDER / PULSE_METER_CURRENT_RESISTOR * 64.0f / 24.0f /
                                    HLW8012_CLOCK_FREQUENCY;
    const float current_multiplier = PULSE_METER_REFERENCE_VOLTAGE / PULSE_METER_CURRENT_RESISTOR * 512.0f / 24.0f /
                                      HLW8012_CLOCK_FREQUENCY;
    const float voltage_multiplier = PULSE_METER_REFERENCE_VOLTAGE * PULSE_METER_VOLTAGE_DIVIDER * 256.0f /
                                      HLW8012_CLOCK_FREQUENCY;
#endif
    const double wh_per_pulse = (double)power_multiplier / 3600.0;

    double latest_watts = 0.0;
    double latest_volts = 0.0;
    double latest_amps = 0.0;
    bool have_current = false;
    bool have_voltage = false;

    for (;;) {
        bool measuring_voltage = (gpio_get_level(PULSE_METER_SEL_GPIO) == 1);

        pulse_meter_cf_edges = 0;
        pulse_meter_cf1_edges = 0;
        vTaskDelay(pdMS_TO_TICKS(PULSE_METER_WINDOW_MS));

        uint32_t cf_count = pulse_meter_cf_edges;
        uint32_t cf1_count = pulse_meter_cf1_edges;
        double window_s = PULSE_METER_WINDOW_MS / 1000.0;

        double cf_freq = (double)cf_count / window_s;
        latest_watts = cf_freq * power_multiplier;

        double cf1_freq = (double)cf1_count / window_s;
        if (measuring_voltage) {
            latest_volts = cf1_freq * voltage_multiplier;
            have_voltage = true;
        } else {
            latest_amps = cf1_freq * current_multiplier;
            have_current = true;
        }

        if (have_current && have_voltage) {
            int64_t delta_energy_mwh = (int64_t)(wh_per_pulse * (double)cf_count * 1000.0);
            report_power(latest_watts, latest_volts, latest_amps, delta_energy_mwh);
        }

        gpio_set_level(PULSE_METER_SEL_GPIO, measuring_voltage ? 0 : 1);
    }
}

#elif BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_CSE7766

static bool cse7766_uart_setup(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = CSE7766_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_EVEN;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;

    esp_err_t err = uart_driver_install(CSE7766_UART_PORT, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(CSE7766_UART_PORT, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(CSE7766_UART_PORT, CSE7766_UART_TX_GPIO, CSE7766_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* See firmware/outlet/'s own header comment for the full packet layout
 * and Adj-byte semantics (per Chipsea's own CSE7766 User Manual). */
static bool cse7766_read(double *out_watts, double *out_volts, double *out_amps, int64_t *out_delta_energy_mwh)
{
    uint8_t pkt[CSE7766_PACKET_LEN];
    int read = uart_read_bytes(CSE7766_UART_PORT, pkt, sizeof(pkt), pdMS_TO_TICKS(2000));
    if (read != CSE7766_PACKET_LEN) {
        ESP_LOGW(TAG, "CSE7766 read timed out or short (%d/%d bytes)", read, CSE7766_PACKET_LEN);
        return false;
    }
    if (pkt[0] == 0xAA) {
        ESP_LOGW(TAG, "CSE7766 reports itself as not calibrated — discarding reading");
        return false;
    }
    if ((pkt[0] & 0xF0) == 0xF0) {
        ESP_LOGW(TAG, "CSE7766 reports an abnormal condition (header1 0x%02x) — discarding reading", pkt[0]);
        return false;
    }
    if (pkt[1] != 0x5A) {
        ESP_LOGW(TAG, "CSE7766 bad frame header2 0x%02x", pkt[1]);
        return false;
    }

    uint8_t checksum = 0;
    for (int i = 2; i <= 22; i++) {
        checksum = (uint8_t)(checksum + pkt[i]);
    }
    if (checksum != pkt[23]) {
        ESP_LOGW(TAG, "CSE7766 checksum mismatch (got 0x%02x, expected 0x%02x)", pkt[23], checksum);
        return false;
    }

    uint8_t adjustment = pkt[20];
    bool voltage_cycle_complete = (adjustment & 0x40) != 0;
    bool current_cycle_complete = (adjustment & 0x20) != 0;
    bool power_cycle_complete = (adjustment & 0x10) != 0;

    uint32_t voltage_coeff = ((uint32_t)pkt[2] << 16) | ((uint32_t)pkt[3] << 8) | pkt[4];
    uint32_t voltage_cycle = ((uint32_t)pkt[5] << 16) | ((uint32_t)pkt[6] << 8) | pkt[7];
    uint32_t current_coeff = ((uint32_t)pkt[8] << 16) | ((uint32_t)pkt[9] << 8) | pkt[10];
    uint32_t current_cycle = ((uint32_t)pkt[11] << 16) | ((uint32_t)pkt[12] << 8) | pkt[13];
    uint32_t power_coeff = ((uint32_t)pkt[14] << 16) | ((uint32_t)pkt[15] << 8) | pkt[16];
    uint32_t power_cycle = ((uint32_t)pkt[17] << 16) | ((uint32_t)pkt[18] << 8) | pkt[19];
    uint16_t cf_pulses = ((uint16_t)pkt[21] << 8) | pkt[22];

    *out_volts = (voltage_cycle_complete && voltage_cycle) ? (double)voltage_coeff / (double)voltage_cycle : 0.0;
    *out_amps = (current_cycle_complete && current_cycle) ? (double)current_coeff / (double)current_cycle : 0.0;
    *out_watts = (power_cycle_complete && power_cycle) ? (double)power_coeff / (double)power_cycle : 0.0;

    double wh_per_pulse = power_cycle_complete ? ((double)power_coeff / 1000000.0 / 3600.0) : 0.0;
    *out_delta_energy_mwh = (int64_t)(wh_per_pulse * (double)cf_pulses * 1000.0);

    return power_cycle_complete || voltage_cycle_complete || current_cycle_complete;
}

static void power_monitor_task(void *arg)
{
    if (!cse7766_uart_setup()) {
        ESP_LOGE(TAG, "CSE7766 UART setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        double watts = 0, volts = 0, amps = 0;
        int64_t delta_energy_mwh = 0;
        if (cse7766_read(&watts, &volts, &amps, &delta_energy_mwh)) {
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        /* No delay — CSE7766 sends multiple packets a second unprompted;
         * cse7766_read() naturally paces this loop. */
    }
}

#elif BATTERY_STORAGE_CHIP == BATTERY_STORAGE_CHIP_ADE7953

static i2c_master_dev_handle_t ade7953_i2c_dev = NULL;

static bool ade7953_write8(uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value };
    return i2c_master_transmit(ade7953_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool ade7953_write16(uint16_t reg, uint16_t value)
{
    uint8_t buf[4] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(ade7953_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool ade7953_read32(uint16_t reg, uint32_t *out_value)
{
    uint8_t reg_bytes[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    uint8_t data[4];
    if (i2c_master_transmit_receive(ade7953_i2c_dev, reg_bytes, sizeof(reg_bytes), data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    *out_value = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    return true;
}

/* See firmware/outlet/'s own header comment for why this driver is
 * flagged as the least-certain of the six. */
static bool ade7953_setup(void)
{
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = ADE7953_SDA_GPIO;
    bus_config.scl_io_num = ADE7953_SCL_GPIO;
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
    dev_config.device_address = ADE7953_I2C_ADDR;
    dev_config.scl_speed_hz = ADE7953_I2C_FREQ_HZ;

    err = i2c_master_bus_add_device(bus, &dev_config, &ade7953_i2c_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!ade7953_write8(ADE7953_REG_UNLOCK_8, 0xAD)) {
        return false;
    }
    if (!ade7953_write16(ADE7953_REG_UNLOCK_16, 0x0030)) {
        return false;
    }
    if (!ade7953_write16(ADE7953_REG_CONFIG_16, 0x0004)) {
        return false;
    }
    return true;
}

static bool ade7953_read(double *out_watts, double *out_volts, double *out_amps)
{
    uint32_t vrms_raw, irms_raw;
    int32_t watt_raw;

    if (!ade7953_read32(ADE7953_REG_VRMS_32, &vrms_raw)) {
        ESP_LOGW(TAG, "ADE7953 VRMS read failed");
        return false;
    }
    if (!ade7953_read32(ADE7953_REG_IRMS_A_32, &irms_raw)) {
        ESP_LOGW(TAG, "ADE7953 IRMS read failed");
        return false;
    }
    if (!ade7953_read32(ADE7953_REG_AWATT_A_32, (uint32_t *)&watt_raw)) {
        ESP_LOGW(TAG, "ADE7953 AWATT read failed");
        return false;
    }

    *out_volts = (double)vrms_raw / ADE7953_VOLTAGE_DIVISOR;
    *out_amps = (double)irms_raw / ADE7953_CURRENT_DIVISOR;
    *out_watts = (double)watt_raw / ADE7953_POWER_DIVISOR;
    return true;
}

static void power_monitor_task(void *arg)
{
    if (!ade7953_setup()) {
        ESP_LOGE(TAG, "ADE7953 setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

    int64_t last_report_ms = 0;
    for (;;) {
        double watts = 0, volts = 0, amps = 0;
        if (ade7953_read(&watts, &volts, &amps)) {
            int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);
            int64_t elapsed_ms = last_report_ms == 0 ? 0 : (now_ms - last_report_ms);
            last_report_ms = now_ms;
            int64_t delta_energy_mwh = (int64_t)(watts * ((double)elapsed_ms / 3600000.0) * 1000.0);
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        vTaskDelay(pdMS_TO_TICKS(ADE7953_POLL_INTERVAL_MS));
    }
}

#endif /* BATTERY_STORAGE_CHIP == ... */

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

    /* 2. Configure the identify LED + its blink timer. */
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

    /* 3. Build the Matter data model: one node, one Battery Storage
     * endpoint — Descriptor (with its own TagList feature) + PowerSource
     * (Battery+Rechargeable, real battery-metrics attribute set) +
     * ElectricalPowerMeasurement + ElectricalEnergyMeasurement +
     * DeviceEnergyManagement[PowerAdjustment], all via the complete
     * top-level helper — see the header comment above for the full
     * detail, including why this resolves the CSA's own malformed XML by
     * using esp-matter's own authoritative interpretation rather than
     * guessing. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Battery Storage",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::battery_storage::config_t battery_storage_config;
    battery_storage_config.bat_capacity = BATTERY_STORAGE_CAPACITY_MAH;
    /* bat_voltage/bat_percent_remaining/bat_time_remaining/
     * bat_time_to_full_charge/bat_charging_current all stay at their
     * config_t default-constructed null values — no reading exists yet
     * at boot; battery_voltage_task() (below) populates BatVoltage/
     * BatPercentRemaining once real ADC samples are available.
     * BatTimeRemaining/BatTimeToFullCharge/BatChargingCurrent stay null
     * permanently — see the header comment above for why. */
    battery_storage_config.electrical_sensor.with_electrical_power_measurement();
    battery_storage_config.electrical_sensor.with_electrical_energy_measurement();
    /* AlternatingCurrent (EPM) and ExportedEnergy+CumulativeEnergy (EEM)
     * are force-set by `battery_storage::add()` itself once the two calls
     * above enable each cluster — no manual feature_flags assignment
     * needed here, same as firmware/solar-power/'s own identical
     * construction. */
    endpoint_t *endpoint = endpoint::battery_storage::create(node, &battery_storage_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create battery storage endpoint");
        return;
    }
    battery_storage_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Battery storage endpoint id: %u", battery_storage_endpoint_id);

    /* 3a. Identify — optionalConform on BatteryStorage.xml, confirmed NOT
     * auto-wired by `battery_storage::add()` (see the header comment
     * above). */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 3b. ElectricalEnergyMeasurement's own accuracy declaration, plus
     * the manually-constructed ElectricalPowerMeasurement Instance — see
     * the header comment above for why the Instance is constructed by
     * hand rather than through the automatic config->delegate path. */
    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::MeasurementAccuracyStruct::Type energy_accuracy = {};
    energy_accuracy.measurementType = MeasurementTypeEnum::kElectricalEnergy;
    energy_accuracy.measured = true;
    energy_accuracy.minMeasuredValue = 0;
    energy_accuracy.maxMeasuredValue = 1000000000000000LL;
    SetMeasurementAccuracy(battery_storage_endpoint_id, energy_accuracy);

    /* Both ElectricalEnergyMeasurement and ElectricalPowerMeasurement
     * declare their own `Feature` enum — the `using namespace` above is
     * still in scope here, so an unqualified `Feature` would be
     * genuinely ambiguous; qualified explicitly instead, same as
     * firmware/outlet/'s own identical code. */
    chip::BitMask<chip::app::Clusters::ElectricalPowerMeasurement::Feature> features(
        chip::app::Clusters::ElectricalPowerMeasurement::Feature::kAlternatingCurrent);
    chip::BitMask<chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes> optional_attrs(
        chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
        chip::app::Clusters::ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent);
    power_instance = new chip::app::Clusters::ElectricalPowerMeasurement::Instance(
        battery_storage_endpoint_id, power_delegate, features, optional_attrs);
    CHIP_ERROR power_err = power_instance->Init();
    if (power_err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ElectricalPowerMeasurement Instance::Init failed: %" CHIP_ERROR_FORMAT, power_err.Format());
        return;
    }

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

    /* 5. Start polling the power monitor + the battery voltage ADC now
     * that the data model + Matter stack both exist. */
    xTaskCreate(power_monitor_task, "power_monitor_task", 4096, NULL, 5, NULL);
    xTaskCreate(battery_voltage_task, "battery_voltage_task", 3072, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter battery storage started. Scan the QR code to commission.");
}
