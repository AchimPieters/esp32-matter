/*
 * Minimal Matter Air Quality Sensor — seventeenth device type. First in
 * this repo to combine the AirQuality cluster's own qualitative headline
 * state (Good/Fair/.../ExtremelyPoor) with real numeric concentration
 * readings (CarbonDioxideConcentrationMeasurement,
 * TotalVolatileOrganicCompoundsConcentrationMeasurement) on the SAME
 * endpoint — confirmed as a legitimate combination directly against the
 * CSA's own data_model/1.6/device_types/AirQualitySensor.xml, which lists
 * Identify + AirQuality as `<mandatoryConform/>` and every one of ten
 * concentration-measurement clusters (CO/CO2/NO2/Ozone/PM1/PM2.5/PM10/
 * Radon/Formaldehyde/TVOC — plus, notably, even Temperature/Humidity) as
 * `<optionalConform/>` on that same endpoint, not a separate one.
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
 * --- Endpoint: esp-matter's own complete top-level helper, plus every
 * extra concentration/measurement cluster added onto the SAME already-
 * correct endpoint -----------------------------------------------------
 * `endpoint::air_quality_sensor::create()` (device type 0x002C) confirmed
 * complete/ready-to-use by reading esp_matter_endpoint.cpp's own
 * air_quality_sensor::add() directly: Identify + AirQuality only (no
 * Groups — correctly matches the CSA XML, which doesn't list one), and,
 * like every complete top-level helper in this repo, built via
 * `common::create<T>()`, which always creates the endpoint's Descriptor
 * cluster automatically — same "use the complete helper, avoid the
 * missing-Descriptor-cluster bug class" precedent
 * firmware/occupancy-sensor/ and firmware/fan/ already established.
 * Every concentration/measurement cluster below is then added onto that
 * SAME endpoint afterwards, via its own free function — this does NOT
 * reintroduce the missing-Descriptor bug (that bug was about hand-
 * assembling an endpoint from raw endpoint::create() instead of a
 * complete helper; here the complete helper builds the endpoint correctly
 * first, and more clusters are simply added onto it afterwards) — same
 * "add extra clusters onto an already-correct endpoint" pattern
 * firmware/thermostat/'s BINDING output type already established.
 *
 * --- AirQuality: a "code-driven" cluster, registry-lookup setter --------
 * Confirmed by reading esp-matter's own source directly: a real
 * `air_quality/` folder exists under
 * `components/esp_matter/data_model_provider/clusters/`, backed by
 * connectedhomeip's own `AirQualityCluster`
 * (`app/clusters/air-quality-server/AirQualityCluster.h`) — same category
 * as firmware/contact-sensor/'s BooleanState, firmware/occupancy-sensor/'s
 * OccupancySensing, and firmware/fan/'s FanControl. `SetAirQuality()` is a
 * plain method on that class (not a Delegate, unlike FanControl) — so
 * this uses the same registry-lookup-and-cast pattern those first two
 * already established: look the live `AirQualityCluster` instance up via
 * the data model provider's registry, call `SetAirQuality()` directly.
 *
 * --- A real, documented esp-matter gap: AirQuality's own config_t has no
 * way to enable its four optional features -------------------------------
 * AirQuality's `AirQualityEnum` has 7 values (Unknown/Good/Fair/Moderate/
 * Poor/VeryPoor/ExtremelyPoor), but per the cluster's own Enums.h, only
 * Unknown/Good/Poor are usable with zero feature bits set — Fair/
 * Moderate/VeryPoor/ExtremelyPoor each need their own `Feature` bit
 * (kFair/kModerate/kVeryPoor/kExtremelyPoor) set in FeatureMap first.
 * Confirmed by reading `air_quality::create()` in esp-matter's own
 * esp_matter_cluster.cpp directly: unlike every comparable "optional
 * feature" cluster in this repo, `air_quality::create()` hardcodes
 * `global::attribute::create_feature_map(cluster, 0)` and
 * `air_quality::config_t` doesn't even declare a `feature_flags` field.
 * So only the base 3-state Good/Poor/Unknown scale is actually reachable
 * through this helper today. Still not worked around (see the original
 * reasoning below) — same "smallest reasonable next step" scoping this
 * repo has applied to every other device type's first cut.
 *
 * --- Concentration/measurement clusters: two different attribute-write
 * patterns on this one endpoint --------------------------------------
 * Confirmed directly inside the real esp-matter SDK (grepped inside the
 * pinned `espressif/esp-matter:release-v1.6_idf_v5.5.4` Docker image
 * before writing any of this file's own cluster-creation code, per this
 * repo's "confirm the real API, don't extrapolate" discipline):
 *   - CO2/CO/NO2/Ozone/PM1/PM2.5(`pm25_concentration_measurement`, no
 *     underscore between "pm" and "25")/PM10/Formaldehyde/Radon/TVOC all
 *     share ONE underlying `concentration_measurement::create()`
 *     implementation (`data_model/legacy/esp_matter_cluster.cpp`) and one
 *     shared `config_t` (`measurement_medium`, `feature_flags`, and a
 *     `features` union of `numeric_measurement`/`level_indication`/
 *     `peak_measurement`/`average_measurement` config structs) — every
 *     one of the nine gas namespaces here is a thin wrapper passing its
 *     own cluster id into that same shared function. None of these are
 *     "code-driven" (no `concentration_measurement/` folder under
 *     `data_model_provider/clusters/`) — MeasuredValue/LevelValue are
 *     plain ember attributes, written via `attribute::update()`, same
 *     pattern as firmware/door-lock/'s LockState.
 *   - TemperatureMeasurement/RelativeHumidityMeasurement ARE code-driven
 *     (confirmed: real `temperature_measurement/`/
 *     `relative_humidity_measurement/` folders exist under
 *     `data_model_provider/clusters/`) — same registry-lookup +
 *     `SetMeasuredValue()` pattern firmware/temperature-sensor/'s own
 *     `update_temperature()`/`update_humidity()` already establish, ported
 *     here near-verbatim since this device adds those clusters onto its
 *     OWN AirQualitySensor endpoint rather than the two separate
 *     Temperature/Humidity Sensor device-type endpoints that file builds.
 *
 * --- Sensor: seven real, independently-toggleable hobbyist chips —
 * every optional AirQualitySensor.xml cluster this device type can
 * honestly back is now wired up, except Radon --------------------------
 * v1 shipped with exactly one chip (CCS811, CO2+TVOC) and left the other
 * 10 optional clusters as a documented "no sensor, no fabricated data"
 * scope cut. On request, every cluster with a real, currently-sold,
 * affordable part a hobbyist can actually source and wire up now gets
 * one — Radon is the sole holdout (see its own section below for why no
 * such part exists at all). Because a real product combining CO2/TVOC +
 * particulates + a couple of gas alarms genuinely needs several chips
 * wired up AT ONCE (not a mutually-exclusive single choice the way
 * firmware/temperature-sensor/'s SENSOR_TYPE works), each new chip is its
 * own independent `#define AIR_QUALITY_HAS_<CHIP> 0/1` toggle — CCS811
 * stays unconditional/always-on, exactly as it originally shipped, so the
 * default build (every new toggle left at 0) is functionally unchanged.
 *
 * CCS811 (ams/ScioSense, I2C, eCO2/eTVOC) — unchanged from v1; see its
 * own section further down for the full, still-accurate sourcing detail.
 *
 * Temperature + RelativeHumidity (`AIR_QUALITY_HAS_TEMP_HUMIDITY`,
 * `AIR_QUALITY_TEMP_HUMIDITY_CHIP` selecting SHT3x/SHT4x/AHT20/BME280) —
 * the exact same 4 I2C chip drivers firmware/temperature-sensor/ already
 * established (register maps, conversion formulas, Sensirion CRC-8,
 * Bosch's own reference compensation algorithm — all previously verified
 * against each chip's real datasheet in that file, ported here verbatim,
 * just renamed to share this file's own I2C bus/dev-handle naming rather
 * than re-verified from scratch). DHT11/DHT22/DS18B20 are deliberately
 * NOT offered here, unlike that file's own 7-way list: DHT11/DHT22 are
 * single-wire (need a dedicated bit-banged GPIO, not this device's shared
 * I2C bus) and DS18B20 has no humidity output at all — this device's own
 * existing I2C bus is a strictly better fit for a device that already
 * shares that same bus with CCS811 (and, if enabled, the NO2 module
 * below) than adding a second, unrelated single-wire pin would be.
 * Both clusters go on the SAME AirQualitySensor endpoint (not the
 * separate Temperature/Humidity Sensor device-type endpoints
 * firmware/temperature-sensor/ creates) since AirQualitySensor.xml lists
 * them as optional clusters of this device type directly.
 *
 * CO (`AIR_QUALITY_HAS_MQ7_CO`) — an MQ-7, near-verbatim ported from
 * firmware/smoke-co-alarm/'s own already-verified driver + its
 * LevelIndication-only CarbonMonoxideConcentrationMeasurement cluster
 * (same "MQ-series ppm curves shift per sensor/module/burn-in state, no
 * fabricated calibrated reading" reasoning that file's own header comment
 * already establishes in full — reused here, not re-derived). GPIO 34
 * (ADC1 channel 6) — same default that file already uses for its own
 * MQ-7, chosen again here for the same "deliberately ADC1, Wi-Fi needs
 * ADC2" reasoning.
 *
 * Ozone (`AIR_QUALITY_HAS_MQ131_OZONE`) — an MQ-131 (Winsen, "Low
 * Concentration" variant), the same class of cheap analog MOX gas sensor
 * as MQ-7/MQ-2, but a genuinely new chip for this repo — protocol/
 * electrical characteristics verified directly against Winsen's own
 * "Ozone Gas Sensor (Model: MQ131 Low Concentration) Manual" (version
 * 1.6, fetched as a PDF and read via `pdftotext`, this repo's established
 * practice for primary-source hardware detail): VC=VH=5.0V±0.1V DC,
 * RL "Adjustable" in the technical-parameters table but the datasheet's
 * own sensitivity-curve test circuit (Fig. 5) is explicitly captioned
 * "The resistance load RL is 1 MΩ" — used here as the reference value;
 * detection range 10-1000ppb ozone; preheat "Over 48 hours" (same
 * "not enforceable, only document it" framing MQ-7/MQ-2's own 24-48h
 * burn-in already gets in smoke-co-alarm). A genuinely different,
 * worth-flagging electrical detail from MQ-7/MQ-2: this sensor's own
 * profile text states "the sensor's conductivity gets lower along with
 * the gas concentration rising" — on the standard Vc→Rs→AOUT→RL→GND
 * divider, falling conductivity (rising Rs) means AOUT VOLTAGE FALLS as
 * ozone rises, the OPPOSITE direction from smoke-co-alarm's own MQ-7/MQ-2
 * (whose own header comment documents AOUT typically RISING with gas
 * concentration on most common breakout modules) — confirmed by the
 * physics of the divider, not assumed identical just because it's "one
 * more MQ-series sensor." This file's own classifier therefore flags
 * Warning/Critical when the reading drops BELOW an adjustable threshold,
 * not above — see `classify_falling()` below, a genuinely different
 * function from smoke-co-alarm's own `classify()` for exactly this
 * reason. Same LevelIndication-only, adjustable-threshold honesty
 * precedent as CO above (`AIR_QUALITY_OZONE_WARNING_MV`/`_CRITICAL_MV`,
 * not a calibrated ppb reading).
 *
 * PM1 + PM2.5 + PM10 (`AIR_QUALITY_HAS_PMS5003_PM`) — a Plantower
 * PMS5003, the most common hobbyist laser-scattering particulate sensor;
 * ONE UART frame reports all three particle-size concentrations
 * together, so one chip genuinely backs three separate Matter clusters at
 * once. Protocol verified directly against Plantower's own "2016 product
 * data manual of PLANTOWER... PMS5003 series data manual" (v2.3, fetched
 * as a PDF and read via `pdftotext`): default Active Mode (the sensor's
 * own power-on default — no command needed to start streaming),
 * 9600bps/8N1, 32-byte frames starting `0x42 0x4D`, frame-length field
 * `2×13+2`, PM1.0/PM2.5/PM10 each reported TWICE per frame — once as
 * "CF=1 standard particle" data (bytes 4-9, the datasheet's own factory/
 * lab-calibration figures) and once as "atmospheric environment" data
 * (bytes 10-15) — this file reports the atmospheric-environment triplet,
 * the same convention every other real open-source PMS5003 integration
 * (ESPHome, Home Assistant) uses for a real-world ambient-air reading,
 * confirmed by reading the datasheet's own field descriptions directly
 * rather than assumed. Checksum = sum of bytes 0-29, compared against the
 * frame's own trailing 16-bit big-endian check value. Genuinely a
 * DC-5V-powered device with an onboard fan (the datasheet's own "Circuit
 * Attentions" section states "DC 5V power supply is needed because the
 * FAN should be driven by 5V... a level conversion unit should be used if
 * the power of host MCU is 5V" — its RX/TX data lines are 3.3V TTL and
 * connect directly to the ESP32, but the sensor's own VCC needs a real,
 * separate 5V supply, not the ESP32's 3.3V rail). UART1, RX-only wiring
 * is enough for the default active-mode stream this file uses (no TX
 * command ever sent) — `AIR_QUALITY_PMS5003_TX_GPIO` is still configured
 * (ESP-IDF's UART driver wants a full pin set) but nothing is ever
 * written to it.
 *
 * Formaldehyde (`AIR_QUALITY_HAS_ZE08CH2O_HCHO`) — a Winsen ZE08-CH2O
 * electrochemical formaldehyde module. Protocol verified directly against
 * Winsen's own "Electrochemical CH2O Detection Module (Model: ZE08-CH2O)
 * User's Manual" (v1.7, fetched as a PDF and read via `pdftotext`) —
 * 9600bps/8N1; default mode is active upload, pushing a 9-byte
 * `0xFF 0x17 ...` frame once a second with no command needed at all
 * (this file's own driver defensively re-sends the documented "switch to
 * active upload" command, `0xFF 0x01 0x78 0x40 0x00 0x00 0x00 0x00 0x47`,
 * once at startup — cheap insurance against a module a previous owner
 * left in Q&A mode, since this repo has no way to know a fresh module's
 * prior configuration); concentration is a real ppb value at bytes 4-5
 * (high/low), the datasheet's own documented conversion to ppm is a
 * straight `/1000`; checksum is `~(sum of bytes 1-7) + 1`, taken directly
 * from the manual's own published `FucCheckSum()` reference C function,
 * not re-derived. Detection range 0-5ppm, resolution ≤0.01ppm — used
 * directly as Min/MaxMeasuredValue. NumericMeasurement (not
 * LevelIndication) is genuinely warranted here — unlike an MQ-series MOX
 * sensor, this is a real electrochemical sensor with a datasheet-
 * documented ppm output, the same category of "real calibrated reading"
 * CCS811's own eCO2/eTVOC already are. The datasheet's own "initial
 * power-up... needs to be preheated for 24-48 hours" caution is
 * documented here, not enforced as a startup delay — same "not a
 * substitute for the real warm-up time" framing this repo applies to
 * every other MOX/electrochemical sensor's own burn-in. UART2, wired
 * RX+TX (the one-time startup command above needs TX).
 *
 * NO2 (`AIR_QUALITY_HAS_MICS4514_NO2`) — the lowest-confidence chip in
 * this pass, flagged as such rather than silently treated as equally
 * solid: a bare MiCS-4514 die has no simple hobbyist-friendly interface
 * of its own, but DFRobot sells a real, currently-available I2C breakout
 * for it (SKU SEN0377, "Gravity: MEMS Gas Sensor (CO, Alcohol, NO2 & NH3)
 * - I2C - MiCS-4514") with a real, open-source Arduino library
 * (github.com/DFRobot/DFRobot_MICS, MIT-licensed) — no register-level
 * datasheet is published for the module itself, so this driver is a port
 * of that library's own real, working I2C implementation, the same "port
 * a real reference rather than guess the integration shape" precedent
 * already used in this repo for SM2335EGH (firmware/addressable-light/)
 * when no clean official datasheet existed for it either. Confirmed by
 * reading the library's own source directly: I2C address 0x78 (the
 * module's dial-switch "ADDRESS_3" default, `[1 1]`); one 6-byte read
 * transaction starting at register 0x04 (`OX_REGISTER_HIGH`) returns OX/
 * RED/POWER as three big-endian uint16 values; a real wake-up sequence
 * (write `0x01` to register `0x0A`, `POWER_MODE_REGISTER`, then wait
 * 100ms) precedes any read. NO2 uses the OX channel:
 * `ratio = (power - ox) / r0_ox`; `if (ratio < 1.1) return 0`; else
 * `no2_ppm = (ratio - 0.045) / 6.13`, clamped to the library's own
 * documented [0.1, 10.0] ppm range — used directly as this file's own
 * Min/MaxMeasuredValue. `r0_ox` (the library's own calibration baseline)
 * is computed the same way the library's own `warmUpTime()` does — one
 * OX/POWER read, `r0_ox = power - ox` — but taken as a single reading
 * shortly after boot rather than genuinely waiting the several minutes
 * the library's own function blocks for, same "don't hang this device's
 * own boot for a real sensor's own long warm-up" precedent CCS811's own
 * skipped 20-minute conditioning delay already establishes in this file
 * — meaning this baseline (and therefore every NO2 reading derived from
 * it) is honestly less trustworthy the sooner after boot it's taken;
 * documented here, not hidden. NumericMeasurement is used since the
 * library's own formula is a real, coefficient-based ppm computation (not
 * a bare voltage threshold) — closer to CCS811's own category of "real
 * calibrated reading" than to the MQ-series' own "no formula at all,
 * adjustable threshold only" category — but this chip's overall
 * confidence is still lower than every other sensor in this file, and it
 * shares the I2C bus (SENSOR_PIN_1/SENSOR_PIN_2) with CCS811 and the
 * optional temperature/humidity chip, all at distinct addresses.
 *
 * Radon — confirmed to have NO realistic hobbyist-accessible sensor at
 * all, and deliberately NOT implemented: real radon detection needs
 * alpha-spectrometry or ionization-chamber hardware (the kind found in
 * dedicated, often Bluetooth-only consumer radon monitors), not a simple
 * I2C/UART/analog chip a hobbyist can wire directly to a GPIO the way
 * every other sensor in this file works — same "no sensor, no fabricated
 * data" honesty precedent this repo applies everywhere else (e.g.
 * firmware/evse/'s always-NoError FaultState), just for an entire gas
 * this device type's own optional cluster list can't honestly back at
 * all. `cluster::radon_concentration_measurement::create()` is confirmed
 * to exist in esp-matter (grepped directly, same as every other
 * concentration cluster here) — the gap is entirely on the sensor-
 * hardware side, not the SDK side.
 *
 * --- Shared I2C bus, now genuinely multi-device --------------------------
 * v1's `i2c_bus_setup()` created a bus and added exactly one device (the
 * CCS811) in one call. With up to three I2C chips potentially enabled at
 * once (CCS811 + the temp/humidity chip + the NO2 module, each at its own
 * address), this is restructured into `i2c_bus_init()` (creates the bus
 * once, idempotent) + `i2c_add_device()` (adds one more device to that
 * same bus) — real ESP-IDF `driver/i2c_master.h` capability, not a
 * workaround.
 *
 * --- AirQuality classification: still a plain, adjustable threshold per
 * gas, now folding in every currently-enabled gas ------------------------
 * Matter's own spec deliberately leaves "what counts as Good vs Poor" up
 * to the device. CO2/TVOC keep their original thresholds unchanged
 * (AIR_QUALITY_CO2_POOR_PPM/_TVOC_POOR_PPB). Every newly-added gas gets
 * its own adjustable threshold in the same spirit — commonly-cited
 * indoor-air-quality guidance figures, not certified regulatory limits or
 * calibrated absolute judgements (PM2.5: 35 µg/m³, in the ballpark of the
 * US EPA's own 24-hour PM2.5 figure; Formaldehyde: ~0.08ppm, in the
 * ballpark of WHO's 30-minute 0.1mg/m³ guideline converted to ppm at
 * standard conditions; NO2: 0.1ppm, a round short-term-exposure guidance
 * figure) — same "adjustable threshold, not a calibrated reading"
 * precedent every other classifier in this repo already uses. Whichever
 * currently-enabled, currently-valid gas is worst decides the overall
 * AirQuality state; a gas that hasn't produced a reading yet (e.g. right
 * after boot, or a UART sensor mid-frame-sync) simply isn't counted that
 * cycle rather than defaulting to either Good or Poor.
 */

#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/clusters/air-quality-server/AirQualityCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <app/clusters/relative-humidity-measurement-server/RelativeHumidityMeasurementCluster.h>

static const char *TAG = "matter_air_quality";

/* --- AIR_QUALITY_SENSOR_TYPE — CCS811 stays unconditional/always-on,
 * exactly as v1 shipped. Every chip below is its own independent
 * AIR_QUALITY_HAS_<CHIP> toggle instead — see the header comment above
 * for why this device type needs several chips enabled AT ONCE rather
 * than one mutually-exclusive choice. */
#define AIR_QUALITY_SENSOR_CCS811 1
#define AIR_QUALITY_SENSOR_TYPE AIR_QUALITY_SENSOR_CCS811

/* I2C pins — deliberately generic names (not "SDA"/"SCL"-specific),
 * matching firmware/temperature-sensor/'s and firmware/light-sensor/'s own
 * SENSOR_PIN_1/SENSOR_PIN_2 convention, so the wizard's existing I2C field
 * mechanism needs no changes for this device type. PIN_1 = SDA, PIN_2 = SCL.
 * Shared by CCS811 and, if enabled, the temp/humidity chip and the NO2
 * module below, each at its own I2C address. */
#define SENSOR_PIN_1 GPIO_NUM_21
#define SENSOR_PIN_2 GPIO_NUM_22
#define SENSOR_I2C_FREQ_HZ 100000

#define IDENTIFY_LED_GPIO GPIO_NUM_4
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* CCS811's own Mode 1 produces a fresh sample every second; polling every
 * 2s comfortably keeps up without hammering the bus, and is the shared
 * cadence every other enabled sensor is also checked on below. */
#define AIR_QUALITY_POLL_INTERVAL_MS 2000

#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
#define CCS811_I2C_ADDR 0x5A /* ADDR pin low — this file's assumed/documented wiring */
#define CCS811_REG_STATUS 0x00
#define CCS811_REG_MEAS_MODE 0x01
#define CCS811_REG_ALG_RESULT_DATA 0x02
#define CCS811_REG_HW_ID 0x20
#define CCS811_REG_ERROR_ID 0xE0
#define CCS811_REG_APP_START 0xF4
#define CCS811_HW_ID_VALUE 0x81
#define CCS811_STATUS_FW_MODE_BIT (1 << 7)
#define CCS811_STATUS_APP_VALID_BIT (1 << 4)
#define CCS811_STATUS_DATA_READY_BIT (1 << 3)
#define CCS811_STATUS_ERROR_BIT (1 << 0)
#define CCS811_MEAS_MODE_1S 0x10 /* DRIVE_MODE=001: constant power, 1 sample/sec */
#endif

/* --- Temperature + RelativeHumidity — see the header comment above --- */
#define AIR_QUALITY_HAS_TEMP_HUMIDITY 0
#define AIR_QUALITY_TEMP_HUMIDITY_CHIP_SHT3X 1
#define AIR_QUALITY_TEMP_HUMIDITY_CHIP_SHT4X 2
#define AIR_QUALITY_TEMP_HUMIDITY_CHIP_AHT20 3
#define AIR_QUALITY_TEMP_HUMIDITY_CHIP_BME280 4
#define AIR_QUALITY_TEMP_HUMIDITY_CHIP AIR_QUALITY_TEMP_HUMIDITY_CHIP_SHT3X

/* --- CO (MQ-7) — see the header comment above --- */
#define AIR_QUALITY_HAS_MQ7_CO 0
#define AIR_QUALITY_MQ7_CO_GPIO GPIO_NUM_34
#define AIR_QUALITY_MQ7_CO_ADC_CHANNEL ADC_CHANNEL_6 /* GPIO 34 */
#define AIR_QUALITY_MQ7_WARNING_MV 1800
#define AIR_QUALITY_MQ7_CRITICAL_MV 2400

/* --- Ozone (MQ-131) — see the header comment above; NOTE the opposite
 * (falling) polarity from MQ-7 --- */
#define AIR_QUALITY_HAS_MQ131_OZONE 0
#define AIR_QUALITY_MQ131_OZONE_GPIO GPIO_NUM_35
#define AIR_QUALITY_MQ131_OZONE_ADC_CHANNEL ADC_CHANNEL_7 /* GPIO 35 */
#define AIR_QUALITY_MQ131_WARNING_MV 2200
#define AIR_QUALITY_MQ131_CRITICAL_MV 1500

/* Shared ADC1 settings for both MQ-7 and MQ-131 — same values
 * firmware/smoke-co-alarm/'s own MQ2/MQ7 driver already uses. */
#define AIR_QUALITY_ADC_ATTEN ADC_ATTEN_DB_12 /* full ~0-3.3V input range */
#define AIR_QUALITY_ADC_BITWIDTH ADC_BITWIDTH_DEFAULT
#define AIR_QUALITY_ADC_SUPPLY_MV 3300.0f
#define AIR_QUALITY_ADC_SAMPLE_COUNT 16

/* --- PM1 + PM2.5 + PM10 (Plantower PMS5003) — see the header comment
 * above --- */
#define AIR_QUALITY_HAS_PMS5003_PM 0
#define AIR_QUALITY_PMS5003_UART_NUM UART_NUM_1
#define AIR_QUALITY_PMS5003_RX_GPIO GPIO_NUM_16
#define AIR_QUALITY_PMS5003_TX_GPIO GPIO_NUM_17 /* configured but never written to — see above */
#define AIR_QUALITY_PMS5003_BAUD 9600
#define AIR_QUALITY_PMS5003_FRAME_LEN 32
#define AIR_QUALITY_PM25_POOR_UGM3 35.0f

/* --- Formaldehyde (Winsen ZE08-CH2O) — see the header comment above --- */
#define AIR_QUALITY_HAS_ZE08CH2O_HCHO 0
#define AIR_QUALITY_ZE08CH2O_UART_NUM UART_NUM_2
#define AIR_QUALITY_ZE08CH2O_RX_GPIO GPIO_NUM_18
#define AIR_QUALITY_ZE08CH2O_TX_GPIO GPIO_NUM_19
#define AIR_QUALITY_ZE08CH2O_BAUD 9600
#define AIR_QUALITY_ZE08CH2O_FRAME_LEN 9
#define AIR_QUALITY_HCHO_POOR_PPM 0.08f

/* --- NO2 (MiCS-4514 via a DFRobot Gravity I2C breakout) — see the
 * header comment above; lowest-confidence chip in this file --- */
#define AIR_QUALITY_HAS_MICS4514_NO2 0
#define AIR_QUALITY_MICS4514_I2C_ADDR 0x78 /* module's own dial-switch "ADDRESS_3" default */
#define AIR_QUALITY_NO2_POOR_PPM 0.1f

/* AirQuality classification thresholds for the original two gases — see
 * the header comment above: adjustable, not a spec-defined or chip-
 * calibrated mapping. */
#define AIR_QUALITY_CO2_POOR_PPM 1000.0f
#define AIR_QUALITY_TVOC_POOR_PPB 660.0f

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t air_quality_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;

/* ======================================================================
 * Shared I2C bus — one bus, one device add per enabled I2C chip (CCS811
 * always; the temp/humidity chip and the NO2 module if enabled). See the
 * header comment above for why this replaced v1's single-device
 * i2c_bus_setup().
 * ====================================================================== */
static i2c_master_bus_handle_t i2c_bus = NULL;

static bool i2c_bus_init(void)
{
    if (i2c_bus) {
        return true; /* already created by an earlier call */
    }
    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = SENSOR_PIN_1;
    bus_config.scl_io_num = SENSOR_PIN_2;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        i2c_bus = NULL;
        return false;
    }
    return true;
}

static bool i2c_add_device(uint16_t device_address, i2c_master_dev_handle_t *out_dev)
{
    if (!i2c_bus_init()) {
        return false;
    }
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = device_address;
    dev_config.scl_speed_hz = SENSOR_I2C_FREQ_HZ;

    esp_err_t err = i2c_master_bus_add_device(i2c_bus, &dev_config, out_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X) failed: %s", device_address, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* ======================================================================
 * CCS811 driver — unchanged from v1 (own I2C device handle, own register
 * map). See the header comment above for the full datasheet-sourced
 * protocol detail (still accurate, not re-verified this pass).
 * ====================================================================== */
#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
static i2c_master_dev_handle_t ccs811_i2c_dev = NULL;

static bool ccs811_write_reg(uint8_t reg)
{
    return i2c_master_transmit(ccs811_i2c_dev, &reg, 1, 1000) == ESP_OK;
}

static bool ccs811_write_reg_u8(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(ccs811_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool ccs811_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(ccs811_i2c_dev, &reg, 1, data, len, 1000) == ESP_OK;
}

static bool ccs811_init(void)
{
    if (!i2c_add_device(CCS811_I2C_ADDR, &ccs811_i2c_dev)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(30)); /* tSTART max 20ms, safety margin */

    uint8_t hw_id = 0;
    if (!ccs811_read_reg(CCS811_REG_HW_ID, &hw_id, 1) || hw_id != CCS811_HW_ID_VALUE) {
        ESP_LOGE(TAG, "CCS811 not found (HW_ID=0x%02X, expected 0x%02X) — check wiring/I2C address", hw_id, CCS811_HW_ID_VALUE);
        return false;
    }

    uint8_t status = 0;
    if (!ccs811_read_reg(CCS811_REG_STATUS, &status, 1) || !(status & CCS811_STATUS_APP_VALID_BIT)) {
        ESP_LOGE(TAG, "CCS811 has no valid application firmware (STATUS=0x%02X)", status);
        return false;
    }

    if (!ccs811_write_reg(CCS811_REG_APP_START)) {
        ESP_LOGE(TAG, "CCS811 APP_START write failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10)); /* tAPP_START max 1ms, safety margin */

    if (!ccs811_write_reg_u8(CCS811_REG_MEAS_MODE, CCS811_MEAS_MODE_1S)) {
        ESP_LOGE(TAG, "CCS811 MEAS_MODE write failed");
        return false;
    }

    ESP_LOGI(TAG, "CCS811 initialized (Mode 1, 1 sample/sec) — readings won't be fully "
                  "accurate until the sensor's own ~20-minute conditioning period completes");
    return true;
}

static bool ccs811_read(float *co2_ppm, float *tvoc_ppb)
{
    uint8_t status = 0;
    if (!ccs811_read_reg(CCS811_REG_STATUS, &status, 1)) {
        return false;
    }
    if (status & CCS811_STATUS_ERROR_BIT) {
        uint8_t error_id = 0;
        ccs811_read_reg(CCS811_REG_ERROR_ID, &error_id, 1);
        ESP_LOGW(TAG, "CCS811 reported an error (ERROR_ID=0x%02X)", error_id);
        return false;
    }
    if (!(status & CCS811_STATUS_DATA_READY_BIT)) {
        return false; /* no new sample since the last read */
    }

    uint8_t data[5] = { 0 };
    if (!ccs811_read_reg(CCS811_REG_ALG_RESULT_DATA, data, sizeof(data))) {
        return false;
    }

    *co2_ppm = (float)(((uint16_t)data[0] << 8) | data[1]);
    *tvoc_ppb = (float)(((uint16_t)data[2] << 8) | data[3]);
    return true;
}
#endif /* AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811 */

/* ======================================================================
 * Temperature + RelativeHumidity driver — the same 4 I2C chip options
 * firmware/temperature-sensor/ already established, ported verbatim
 * (only the shared I2C dev-handle/bus-add calls are renamed to fit this
 * file's own multi-device bus). See that file's own header comment for
 * the full original datasheet sourcing.
 * ====================================================================== */
#if AIR_QUALITY_HAS_TEMP_HUMIDITY
static i2c_master_dev_handle_t temp_humidity_i2c_dev = NULL;

/* Sensirion CRC-8 (polynomial 0x31, init 0xFF) — used by SHT3x/SHT4x. */
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

#if AIR_QUALITY_TEMP_HUMIDITY_CHIP == AIR_QUALITY_TEMP_HUMIDITY_CHIP_SHT3X

#define TEMP_HUMIDITY_SHT3X_I2C_ADDR 0x44 /* 0x45 if ADDR is tied to VDD */

static bool temp_humidity_setup(void)
{
    return i2c_add_device(TEMP_HUMIDITY_SHT3X_I2C_ADDR, &temp_humidity_i2c_dev);
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[2] = {0x24, 0x00}; /* single shot, high repeatability */
    if (i2c_master_transmit(temp_humidity_i2c_dev, cmd, sizeof(cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    if (i2c_master_receive(temp_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
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

#elif AIR_QUALITY_TEMP_HUMIDITY_CHIP == AIR_QUALITY_TEMP_HUMIDITY_CHIP_SHT4X

#define TEMP_HUMIDITY_SHT4X_I2C_ADDR 0x44

static bool temp_humidity_setup(void)
{
    return i2c_add_device(TEMP_HUMIDITY_SHT4X_I2C_ADDR, &temp_humidity_i2c_dev);
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t cmd[1] = {0xFD}; /* measure T & RH, high precision */
    if (i2c_master_transmit(temp_humidity_i2c_dev, cmd, sizeof(cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t data[6];
    if (i2c_master_receive(temp_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (sensirion_crc8(data, 2) != data[2] || sensirion_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHT4x CRC mismatch — discarding reading");
        return false;
    }

    uint16_t temp_ticks = ((uint16_t)data[0] << 8) | data[1];
    uint16_t hum_ticks = ((uint16_t)data[3] << 8) | data[4];
    *temperature_c = -45.0f + 175.0f * ((float)temp_ticks / 65535.0f);
    *humidity_pct = -6.0f + 125.0f * ((float)hum_ticks / 65535.0f);
    if (*humidity_pct < 0.0f) {
        *humidity_pct = 0.0f;
    } else if (*humidity_pct > 100.0f) {
        *humidity_pct = 100.0f;
    }
    return true;
}

#elif AIR_QUALITY_TEMP_HUMIDITY_CHIP == AIR_QUALITY_TEMP_HUMIDITY_CHIP_AHT20

#define TEMP_HUMIDITY_AHT20_I2C_ADDR 0x38

static bool temp_humidity_setup(void)
{
    if (!i2c_add_device(TEMP_HUMIDITY_AHT20_I2C_ADDR, &temp_humidity_i2c_dev)) {
        return false;
    }
    const uint8_t init_cmd[3] = {0xBE, 0x08, 0x00};
    if (i2c_master_transmit(temp_humidity_i2c_dev, init_cmd, sizeof(init_cmd), 1000) != ESP_OK) {
        ESP_LOGE(TAG, "AHT20 init command failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
    return true;
}

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    const uint8_t trigger_cmd[3] = {0xAC, 0x33, 0x00};
    if (i2c_master_transmit(temp_humidity_i2c_dev, trigger_cmd, sizeof(trigger_cmd), 1000) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(85));

    uint8_t data[6];
    if (i2c_master_receive(temp_humidity_i2c_dev, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    if (data[0] & 0x80) {
        ESP_LOGW(TAG, "AHT20 still busy — discarding reading");
        return false;
    }

    uint32_t raw_humidity = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t raw_temperature = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humidity_pct = (float)raw_humidity * 100.0f / 1048576.0f;
    *temperature_c = (float)raw_temperature * 200.0f / 1048576.0f - 50.0f;
    return true;
}

#elif AIR_QUALITY_TEMP_HUMIDITY_CHIP == AIR_QUALITY_TEMP_HUMIDITY_CHIP_BME280

#define TEMP_HUMIDITY_BME280_I2C_ADDR 0x76 /* 0x77 if SDO is tied to VDD */
#define TEMP_HUMIDITY_BME280_REG_CHIP_ID 0xD0
#define TEMP_HUMIDITY_BME280_REG_CALIB_T 0x88
#define TEMP_HUMIDITY_BME280_REG_CALIB_H1 0xA1
#define TEMP_HUMIDITY_BME280_REG_CALIB_H2 0xE1
#define TEMP_HUMIDITY_BME280_REG_CTRL_HUM 0xF2
#define TEMP_HUMIDITY_BME280_REG_CTRL_MEAS 0xF4
#define TEMP_HUMIDITY_BME280_REG_DATA 0xFA
#define TEMP_HUMIDITY_BME280_CHIP_ID_EXPECTED 0x60

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
    return i2c_master_transmit(temp_humidity_i2c_dev, buf, sizeof(buf), 1000) == ESP_OK;
}

static bool bme280_read_regs(uint8_t reg, uint8_t *out, size_t len)
{
    return i2c_master_transmit_receive(temp_humidity_i2c_dev, &reg, 1, out, len, 1000) == ESP_OK;
}

static int16_t sign_extend_12bit(uint16_t value)
{
    return (int16_t)((value & 0x0800) ? (value | 0xF000) : value);
}

static bool temp_humidity_setup(void)
{
    if (!i2c_add_device(TEMP_HUMIDITY_BME280_I2C_ADDR, &temp_humidity_i2c_dev)) {
        return false;
    }

    uint8_t chip_id = 0;
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CHIP_ID, &chip_id, 1) || chip_id != TEMP_HUMIDITY_BME280_CHIP_ID_EXPECTED) {
        ESP_LOGE(TAG, "BME280 chip ID mismatch (got 0x%02X, expected 0x%02X)", chip_id, TEMP_HUMIDITY_BME280_CHIP_ID_EXPECTED);
        return false;
    }

    uint8_t calib_t[6];
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CALIB_T, calib_t, sizeof(calib_t))) {
        return false;
    }
    bme280_calib.dig_t1 = (uint16_t)(calib_t[0] | (calib_t[1] << 8));
    bme280_calib.dig_t2 = (int16_t)(calib_t[2] | (calib_t[3] << 8));
    bme280_calib.dig_t3 = (int16_t)(calib_t[4] | (calib_t[5] << 8));

    uint8_t dig_h1 = 0;
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CALIB_H1, &dig_h1, 1)) {
        return false;
    }
    bme280_calib.dig_h1 = dig_h1;

    uint8_t calib_h[7];
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_CALIB_H2, calib_h, sizeof(calib_h))) {
        return false;
    }
    bme280_calib.dig_h2 = (int16_t)(calib_h[0] | (calib_h[1] << 8));
    bme280_calib.dig_h3 = calib_h[2];
    bme280_calib.dig_h4 = sign_extend_12bit((uint16_t)((calib_h[3] << 4) | (calib_h[4] & 0x0F)));
    bme280_calib.dig_h5 = sign_extend_12bit((uint16_t)((calib_h[5] << 4) | (calib_h[4] >> 4)));
    bme280_calib.dig_h6 = (int8_t)calib_h[6];

    if (!bme280_write_reg(TEMP_HUMIDITY_BME280_REG_CTRL_HUM, 0x01)) {
        return false;
    }
    return true;
}

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

static bool temp_humidity_read(float *temperature_c, float *humidity_pct)
{
    if (!bme280_write_reg(TEMP_HUMIDITY_BME280_REG_CTRL_MEAS, 0x25)) { /* osrs_t=x1, osrs_p=x1, forced mode */
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[5];
    if (!bme280_read_regs(TEMP_HUMIDITY_BME280_REG_DATA, data, sizeof(data))) {
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
#error "Unknown AIR_QUALITY_TEMP_HUMIDITY_CHIP"
#endif

/* Code-driven cluster setters — see the header comment above. Ported
 * from firmware/temperature-sensor/'s own update_temperature()/
 * update_humidity(), including their ScopedChipStackLock (that file's own
 * established, deliberate choice for this specific pair of setters). */
static void update_temperature(chip::app::DataModel::Nullable<int16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    chip::app::ConcreteClusterPath path(air_quality_endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "TemperatureMeasurement cluster not found on endpoint %u", air_quality_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(value);
}

static void update_humidity(chip::app::DataModel::Nullable<uint16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    chip::app::ConcreteClusterPath path(air_quality_endpoint_id, RelativeHumidityMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "RelativeHumidityMeasurement cluster not found on endpoint %u", air_quality_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::RelativeHumidityMeasurementCluster *>(iface)->SetMeasuredValue(value);
}
#endif /* AIR_QUALITY_HAS_TEMP_HUMIDITY */

/* ======================================================================
 * MQ-7 (CO) / MQ-131 (Ozone) — shared ADC1 unit, one channel each. See
 * the header comment above for the opposite (rising vs. falling)
 * polarity between the two.
 * ====================================================================== */
#if AIR_QUALITY_HAS_MQ7_CO || AIR_QUALITY_HAS_MQ131_OZONE
static adc_oneshot_unit_handle_t gas_adc_handle = NULL;

static bool setup_channel_calibration(adc_channel_t channel, adc_cali_handle_t *out_handle)
{
    esp_err_t err;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.chan = channel;
    cali_config.atten = AIR_QUALITY_ADC_ATTEN;
    cali_config.bitwidth = AIR_QUALITY_ADC_BITWIDTH;
    err = adc_cali_create_scheme_curve_fitting(&cali_config, out_handle);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {};
    cali_config.unit_id = ADC_UNIT_1;
    cali_config.atten = AIR_QUALITY_ADC_ATTEN;
    cali_config.bitwidth = AIR_QUALITY_ADC_BITWIDTH;
#if CONFIG_IDF_TARGET_ESP32
    cali_config.default_vref = (uint32_t)AIR_QUALITY_ADC_SUPPLY_MV;
#endif
    err = adc_cali_create_scheme_line_fitting(&cali_config, out_handle);
#else
    err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable for channel %d (%s) — using uncalibrated estimate",
                 channel, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool gas_adc_init(void)
{
    if (gas_adc_handle) {
        return true;
    }
    adc_oneshot_unit_init_cfg_t init_config = {};
    init_config.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&init_config, &gas_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool gas_adc_config_channel(adc_channel_t channel)
{
    adc_oneshot_chan_cfg_t chan_config = {};
    chan_config.atten = AIR_QUALITY_ADC_ATTEN;
    chan_config.bitwidth = AIR_QUALITY_ADC_BITWIDTH;
    esp_err_t err = adc_oneshot_config_channel(gas_adc_handle, channel, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel(%d) failed: %s", channel, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool read_channel_millivolts(adc_channel_t channel, adc_cali_handle_t cali_handle, bool cali_available, int *out_mv)
{
    int64_t sum_raw = 0;
    for (int i = 0; i < AIR_QUALITY_ADC_SAMPLE_COUNT; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(gas_adc_handle, channel, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read (channel %d) failed: %s", channel, esp_err_to_name(err));
            return false;
        }
        sum_raw += raw;
    }
    int avg_raw = (int)(sum_raw / AIR_QUALITY_ADC_SAMPLE_COUNT);

    if (cali_available) {
        esp_err_t err = adc_cali_raw_to_voltage(cali_handle, avg_raw, out_mv);
        if (err == ESP_OK) {
            return true;
        }
    }
    *out_mv = (int)((float)avg_raw * AIR_QUALITY_ADC_SUPPLY_MV / 4095.0f);
    return true;
}

enum class GasAlarmState { kNormal, kWarning, kCritical };

/* Rising polarity (MQ-7's own AOUT rises with CO concentration — same
 * direction firmware/smoke-co-alarm/'s own MQ7 driver already uses). */
static GasAlarmState classify_rising(int mv, int warning_mv, int critical_mv)
{
    if (mv >= critical_mv) {
        return GasAlarmState::kCritical;
    }
    if (mv >= warning_mv) {
        return GasAlarmState::kWarning;
    }
    return GasAlarmState::kNormal;
}

/* Falling polarity (MQ-131's own AOUT FALLS with ozone concentration —
 * see the header comment above for the physics). */
static GasAlarmState classify_falling(int mv, int warning_mv, int critical_mv)
{
    if (mv <= critical_mv) {
        return GasAlarmState::kCritical;
    }
    if (mv <= warning_mv) {
        return GasAlarmState::kWarning;
    }
    return GasAlarmState::kNormal;
}

/* Maps the internal 3-state alarm classification onto a cluster's own
 * LevelValueEnum — same shape firmware/smoke-co-alarm/'s own
 * co_alarm_state_to_level_value() already establishes, templated here so
 * CO and Ozone (two distinct LevelValueEnum types with the same member
 * names) can share one implementation. */
template <typename LevelValueEnumT>
static LevelValueEnumT gas_alarm_state_to_level_value(GasAlarmState state)
{
    switch (state) {
    case GasAlarmState::kCritical:
        return LevelValueEnumT::kCritical;
    case GasAlarmState::kWarning:
        return LevelValueEnumT::kMedium;
    case GasAlarmState::kNormal:
    default:
        return LevelValueEnumT::kLow;
    }
}
#endif /* AIR_QUALITY_HAS_MQ7_CO || AIR_QUALITY_HAS_MQ131_OZONE */

#if AIR_QUALITY_HAS_MQ7_CO
static adc_cali_handle_t mq7_cali_handle = NULL;
static bool mq7_cali_available = false;

static bool mq7_setup(void)
{
    if (!gas_adc_init() || !gas_adc_config_channel(AIR_QUALITY_MQ7_CO_ADC_CHANNEL)) {
        return false;
    }
    mq7_cali_available = setup_channel_calibration(AIR_QUALITY_MQ7_CO_ADC_CHANNEL, &mq7_cali_handle);
    return true;
}
#endif

#if AIR_QUALITY_HAS_MQ131_OZONE
static adc_cali_handle_t mq131_cali_handle = NULL;
static bool mq131_cali_available = false;

static bool mq131_setup(void)
{
    if (!gas_adc_init() || !gas_adc_config_channel(AIR_QUALITY_MQ131_OZONE_ADC_CHANNEL)) {
        return false;
    }
    mq131_cali_available = setup_channel_calibration(AIR_QUALITY_MQ131_OZONE_ADC_CHANNEL, &mq131_cali_handle);
    return true;
}
#endif

/* ======================================================================
 * Plantower PMS5003 (PM1/PM2.5/PM10) — UART, default active-mode stream,
 * RX-only reading (no command ever sent). See the header comment above
 * for the full datasheet-sourced frame format.
 * ====================================================================== */
#if AIR_QUALITY_HAS_PMS5003_PM
static bool pms5003_setup(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate = AIR_QUALITY_PMS5003_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(AIR_QUALITY_PMS5003_UART_NUM, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS5003 uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(AIR_QUALITY_PMS5003_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS5003 uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_set_pin(AIR_QUALITY_PMS5003_UART_NUM, AIR_QUALITY_PMS5003_TX_GPIO, AIR_QUALITY_PMS5003_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMS5003 uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Syncs on 0x42 0x4D, reads the remaining 30 bytes of one 32-byte frame,
 * validates the checksum, and extracts the "atmospheric environment"
 * PM1.0/PM2.5/PM10 triplet (bytes 10-15) — see the header comment above
 * for why that triplet, not the "CF=1 standard particle" one. Returns
 * false (no fresh, valid frame within the timeout) if nothing arrived —
 * same "try again next poll" convention as every other sensor here. */
static bool pms5003_read(float *pm1_ugm3, float *pm25_ugm3, float *pm10_ugm3)
{
    uint8_t byte = 0;
    bool synced = false;
    for (int attempt = 0; attempt < AIR_QUALITY_PMS5003_FRAME_LEN && !synced; attempt++) {
        if (uart_read_bytes(AIR_QUALITY_PMS5003_UART_NUM, &byte, 1, pdMS_TO_TICKS(300)) != 1) {
            return false; /* no data this cycle */
        }
        if (byte != 0x42) {
            continue;
        }
        if (uart_read_bytes(AIR_QUALITY_PMS5003_UART_NUM, &byte, 1, pdMS_TO_TICKS(50)) != 1) {
            return false;
        }
        if (byte == 0x4D) {
            synced = true;
        }
    }
    if (!synced) {
        return false;
    }

    uint8_t frame[AIR_QUALITY_PMS5003_FRAME_LEN];
    frame[0] = 0x42;
    frame[1] = 0x4D;
    int got = uart_read_bytes(AIR_QUALITY_PMS5003_UART_NUM, frame + 2, AIR_QUALITY_PMS5003_FRAME_LEN - 2, pdMS_TO_TICKS(200));
    if (got != AIR_QUALITY_PMS5003_FRAME_LEN - 2) {
        return false;
    }

    uint16_t sum = 0;
    for (int i = 0; i < AIR_QUALITY_PMS5003_FRAME_LEN - 2; i++) {
        sum += frame[i];
    }
    uint16_t check = ((uint16_t)frame[AIR_QUALITY_PMS5003_FRAME_LEN - 2] << 8) | frame[AIR_QUALITY_PMS5003_FRAME_LEN - 1];
    if (sum != check) {
        ESP_LOGW(TAG, "PMS5003 checksum mismatch — discarding frame");
        return false;
    }

    *pm1_ugm3 = (float)(((uint16_t)frame[10] << 8) | frame[11]);
    *pm25_ugm3 = (float)(((uint16_t)frame[12] << 8) | frame[13]);
    *pm10_ugm3 = (float)(((uint16_t)frame[14] << 8) | frame[15]);
    return true;
}
#endif /* AIR_QUALITY_HAS_PMS5003_PM */

/* ======================================================================
 * Winsen ZE08-CH2O (Formaldehyde) — UART, default active-upload mode
 * (re-asserted once at startup for safety). See the header comment above
 * for the full datasheet-sourced frame format.
 * ====================================================================== */
#if AIR_QUALITY_HAS_ZE08CH2O_HCHO
static bool ze08ch2o_setup(void)
{
    uart_config_t cfg = {};
    cfg.baud_rate = AIR_QUALITY_ZE08CH2O_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(AIR_QUALITY_ZE08CH2O_UART_NUM, 256, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ZE08-CH2O uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_param_config(AIR_QUALITY_ZE08CH2O_UART_NUM, &cfg);
    if (err != ESP_OK) {
        return false;
    }
    err = uart_set_pin(AIR_QUALITY_ZE08CH2O_UART_NUM, AIR_QUALITY_ZE08CH2O_TX_GPIO, AIR_QUALITY_ZE08CH2O_RX_GPIO,
                        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return false;
    }

    /* Table 4 of the manual: "switch to active upload" — defensive, in
     * case a previous owner left the module in Q&A mode. */
    const uint8_t switch_to_active[9] = {0xFF, 0x01, 0x78, 0x40, 0x00, 0x00, 0x00, 0x00, 0x47};
    uart_write_bytes(AIR_QUALITY_ZE08CH2O_UART_NUM, switch_to_active, sizeof(switch_to_active));
    return true;
}

/* Checksum per the manual's own published FucCheckSum(): two's complement
 * of the sum of bytes 1..7 (i.e. every byte except the start byte and the
 * checksum byte itself). */
static uint8_t ze08ch2o_checksum(const uint8_t *frame)
{
    uint8_t sum = 0;
    for (int i = 1; i <= 7; i++) {
        sum = (uint8_t)(sum + frame[i]);
    }
    return (uint8_t)((~sum) + 1);
}

static bool ze08ch2o_read(float *hcho_ppm)
{
    uint8_t byte = 0;
    bool synced = false;
    for (int attempt = 0; attempt < AIR_QUALITY_ZE08CH2O_FRAME_LEN && !synced; attempt++) {
        if (uart_read_bytes(AIR_QUALITY_ZE08CH2O_UART_NUM, &byte, 1, pdMS_TO_TICKS(300)) != 1) {
            return false;
        }
        if (byte == 0xFF) {
            synced = true;
        }
    }
    if (!synced) {
        return false;
    }

    uint8_t frame[AIR_QUALITY_ZE08CH2O_FRAME_LEN];
    frame[0] = 0xFF;
    int got = uart_read_bytes(AIR_QUALITY_ZE08CH2O_UART_NUM, frame + 1, AIR_QUALITY_ZE08CH2O_FRAME_LEN - 1, pdMS_TO_TICKS(200));
    if (got != AIR_QUALITY_ZE08CH2O_FRAME_LEN - 1) {
        return false;
    }

    if (frame[1] != 0x17) {
        return false; /* not an active-upload CH2O frame (Table 5's own Byte1 = gas name) */
    }
    if (ze08ch2o_checksum(frame) != frame[8]) {
        ESP_LOGW(TAG, "ZE08-CH2O checksum mismatch — discarding frame");
        return false;
    }

    uint16_t concentration_ppb = ((uint16_t)frame[4] << 8) | frame[5];
    *hcho_ppm = concentration_ppb / 1000.0f; /* datasheet's own documented PPM = PPB/1000 */
    return true;
}
#endif /* AIR_QUALITY_HAS_ZE08CH2O_HCHO */

/* ======================================================================
 * MiCS-4514 (NO2) via a DFRobot Gravity I2C breakout — ported from
 * DFRobot's own real, open-source DFRobot_MICS library. See the header
 * comment above for the full sourcing and this chip's own, lower,
 * confidence level.
 * ====================================================================== */
#if AIR_QUALITY_HAS_MICS4514_NO2
#define MICS4514_REG_OX_HIGH 0x04
#define MICS4514_REG_POWER_MODE 0x0A
#define MICS4514_POWER_MODE_WAKE 0x01

static i2c_master_dev_handle_t mics4514_i2c_dev = NULL;
static float mics4514_r0_ox = 0.0f;
static bool mics4514_calibrated = false;

static bool mics4514_read_ox_red_power(uint16_t *ox, uint16_t *red, uint16_t *power)
{
    uint8_t reg = MICS4514_REG_OX_HIGH;
    uint8_t data[6];
    if (i2c_master_transmit_receive(mics4514_i2c_dev, &reg, 1, data, sizeof(data), 1000) != ESP_OK) {
        return false;
    }
    *ox = ((uint16_t)data[0] << 8) | data[1];
    *red = ((uint16_t)data[2] << 8) | data[3];
    *power = ((uint16_t)data[4] << 8) | data[5];
    return true;
}

static bool mics4514_setup(void)
{
    if (!i2c_add_device(AIR_QUALITY_MICS4514_I2C_ADDR, &mics4514_i2c_dev)) {
        return false;
    }
    uint8_t wake = MICS4514_POWER_MODE_WAKE;
    if (i2c_master_transmit(mics4514_i2c_dev, (uint8_t[]){MICS4514_REG_POWER_MODE, wake}, 2, 1000) != ESP_OK) {
        ESP_LOGE(TAG, "MiCS-4514 wake-up write failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Best-effort R0 (clean-air) baseline — see the header comment above
     * for why this is a single reading shortly after boot rather than a
     * genuine several-minute warm-up wait, and why that makes early NO2
     * readings honestly less trustworthy. */
    uint16_t ox = 0, red = 0, power = 0;
    if (mics4514_read_ox_red_power(&ox, &red, &power)) {
        mics4514_r0_ox = (float)power - (float)ox;
        mics4514_calibrated = (mics4514_r0_ox > 0.0f);
    }
    if (!mics4514_calibrated) {
        ESP_LOGW(TAG, "MiCS-4514 baseline read failed — NO2 readings will be skipped");
    }
    return true;
}

static bool mics4514_read(float *no2_ppm)
{
    if (!mics4514_calibrated) {
        return false;
    }
    uint16_t ox = 0, red = 0, power = 0;
    if (!mics4514_read_ox_red_power(&ox, &red, &power)) {
        return false;
    }
    float ratio = ((float)power - (float)ox) / mics4514_r0_ox;
    if (ratio < 1.1f) {
        return false; /* library's own documented "below detectable range" cutoff */
    }
    float ppm = (ratio - 0.045f) / 6.13f;
    if (ppm < 0.1f) {
        ppm = 0.1f;
    } else if (ppm > 10.0f) {
        ppm = 10.0f;
    }
    *no2_ppm = ppm;
    return true;
}
#endif /* AIR_QUALITY_HAS_MICS4514_NO2 */

/* ======================================================================
 * AirQuality classification + publishing.
 * ====================================================================== */

/* Persists the last-known valid state per gas across poll cycles, so a
 * slower-updating sensor's own reading still counts toward the overall
 * AirQuality state on cycles where it didn't produce a fresh sample. */
struct air_quality_state_t {
    bool co2_tvoc_valid = false;
    bool co2_tvoc_poor = false;
#if AIR_QUALITY_HAS_MQ7_CO
    bool co_valid = false;
    bool co_poor = false;
#endif
#if AIR_QUALITY_HAS_MQ131_OZONE
    bool ozone_valid = false;
    bool ozone_poor = false;
#endif
#if AIR_QUALITY_HAS_PMS5003_PM
    bool pm_valid = false;
    float pm25_ugm3 = 0.0f;
#endif
#if AIR_QUALITY_HAS_ZE08CH2O_HCHO
    bool hcho_valid = false;
    float hcho_ppm = 0.0f;
#endif
#if AIR_QUALITY_HAS_MICS4514_NO2
    bool no2_valid = false;
    float no2_ppm = 0.0f;
#endif
};
static air_quality_state_t g_state;

/* CO2/TVOC's own original classification — unchanged from v1. */
static bool classify_co2_tvoc(float co2_ppm, float tvoc_ppb)
{
    return (co2_ppm >= AIR_QUALITY_CO2_POOR_PPM) || (tvoc_ppb >= AIR_QUALITY_TVOC_POOR_PPB);
}

/* Recomputes the overall AirQuality state from every currently-valid
 * gas's own last-known classification, and publishes both that and every
 * concentration-measurement cluster's own MeasuredValue/LevelValue —
 * called once per poll cycle after every enabled sensor has had a chance
 * to produce a fresh reading. */
static void recompute_and_publish_air_quality(void)
{
    bool poor = false;
    bool any_valid = false;

    if (g_state.co2_tvoc_valid) {
        any_valid = true;
        poor = poor || g_state.co2_tvoc_poor;
    }
#if AIR_QUALITY_HAS_MQ7_CO
    if (g_state.co_valid) {
        any_valid = true;
        poor = poor || g_state.co_poor;
    }
#endif
#if AIR_QUALITY_HAS_MQ131_OZONE
    if (g_state.ozone_valid) {
        any_valid = true;
        poor = poor || g_state.ozone_poor;
    }
#endif
#if AIR_QUALITY_HAS_PMS5003_PM
    if (g_state.pm_valid) {
        any_valid = true;
        poor = poor || (g_state.pm25_ugm3 >= AIR_QUALITY_PM25_POOR_UGM3);
    }
#endif
#if AIR_QUALITY_HAS_ZE08CH2O_HCHO
    if (g_state.hcho_valid) {
        any_valid = true;
        poor = poor || (g_state.hcho_ppm >= AIR_QUALITY_HCHO_POOR_PPM);
    }
#endif
#if AIR_QUALITY_HAS_MICS4514_NO2
    if (g_state.no2_valid) {
        any_valid = true;
        poor = poor || (g_state.no2_ppm >= AIR_QUALITY_NO2_POOR_PPM);
    }
#endif

    if (!any_valid) {
        return; /* nothing to report yet */
    }

    AirQuality::AirQualityEnum overall = poor ? AirQuality::AirQualityEnum::kPoor : AirQuality::AirQualityEnum::kGood;
    chip::app::ConcreteClusterPath path(air_quality_endpoint_id, AirQuality::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (iface) {
        static_cast<AirQualityCluster *>(iface)->SetAirQuality(overall);
    } else {
        ESP_LOGE(TAG, "AirQuality cluster not found on endpoint %u", air_quality_endpoint_id);
    }
}

/* ======================================================================
 * Shared polling task — inits every enabled sensor once (each
 * independently — one chip's init failure doesn't stop the others), then
 * reads whichever ones are enabled on a shared timer for as long as the
 * device runs.
 * ====================================================================== */
static void air_quality_task(void *arg)
{
#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
    bool ccs811_ok = ccs811_init();
    if (!ccs811_ok) {
        ESP_LOGE(TAG, "CCS811 init failed — no CO2/TVOC readings will be reported");
    }
#endif
#if AIR_QUALITY_HAS_TEMP_HUMIDITY
    bool temp_humidity_ok = temp_humidity_setup();
    if (!temp_humidity_ok) {
        ESP_LOGE(TAG, "Temperature/humidity sensor init failed — no readings will be reported");
    }
#endif
#if AIR_QUALITY_HAS_MQ7_CO
    bool mq7_ok = mq7_setup();
    if (!mq7_ok) {
        ESP_LOGE(TAG, "MQ-7 init failed — no CO readings will be reported");
    }
#endif
#if AIR_QUALITY_HAS_MQ131_OZONE
    bool mq131_ok = mq131_setup();
    if (!mq131_ok) {
        ESP_LOGE(TAG, "MQ-131 init failed — no Ozone readings will be reported");
    }
#endif
#if AIR_QUALITY_HAS_PMS5003_PM
    bool pms5003_ok = pms5003_setup();
    if (!pms5003_ok) {
        ESP_LOGE(TAG, "PMS5003 init failed — no PM readings will be reported");
    }
#endif
#if AIR_QUALITY_HAS_ZE08CH2O_HCHO
    bool ze08ch2o_ok = ze08ch2o_setup();
    if (!ze08ch2o_ok) {
        ESP_LOGE(TAG, "ZE08-CH2O init failed — no Formaldehyde readings will be reported");
    }
#endif
#if AIR_QUALITY_HAS_MICS4514_NO2
    bool mics4514_ok = mics4514_setup();
    if (!mics4514_ok) {
        ESP_LOGE(TAG, "MiCS-4514 init failed — no NO2 readings will be reported");
    }
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(AIR_QUALITY_POLL_INTERVAL_MS));

#if AIR_QUALITY_SENSOR_TYPE == AIR_QUALITY_SENSOR_CCS811
        if (ccs811_ok) {
            float co2_ppm = 0.0f, tvoc_ppb = 0.0f;
            if (ccs811_read(&co2_ppm, &tvoc_ppb)) {
                esp_matter_attr_val_t co2_val = esp_matter_nullable_float(nullable<float>(co2_ppm));
                attribute::update(air_quality_endpoint_id, CarbonDioxideConcentrationMeasurement::Id,
                                  CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id, &co2_val);
                esp_matter_attr_val_t tvoc_val = esp_matter_nullable_float(nullable<float>(tvoc_ppb));
                attribute::update(air_quality_endpoint_id, TotalVolatileOrganicCompoundsConcentrationMeasurement::Id,
                                  TotalVolatileOrganicCompoundsConcentrationMeasurement::Attributes::MeasuredValue::Id, &tvoc_val);
                g_state.co2_tvoc_valid = true;
                g_state.co2_tvoc_poor = classify_co2_tvoc(co2_ppm, tvoc_ppb);
                ESP_LOGI(TAG, "eCO2 %.0f ppm, eTVOC %.0f ppb", co2_ppm, tvoc_ppb);
            }
        }
#endif

#if AIR_QUALITY_HAS_TEMP_HUMIDITY
        if (temp_humidity_ok) {
            float temperature_c = 0.0f, humidity_pct = 0.0f;
            if (temp_humidity_read(&temperature_c, &humidity_pct)) {
                int16_t temp_centidegrees = (int16_t)(temperature_c * 100.0f);
                uint16_t hum_centipercent = (uint16_t)(humidity_pct * 100.0f);
                update_temperature(chip::app::DataModel::Nullable<int16_t>(temp_centidegrees));
                update_humidity(chip::app::DataModel::Nullable<uint16_t>(hum_centipercent));
                ESP_LOGI(TAG, "Temp/Humidity: %.2f degC, %.2f %%RH", temperature_c, humidity_pct);
            } else {
                update_temperature(chip::app::DataModel::Nullable<int16_t>());
                update_humidity(chip::app::DataModel::Nullable<uint16_t>());
            }
        }
#endif

#if AIR_QUALITY_HAS_MQ7_CO
        if (mq7_ok) {
            int mv = 0;
            if (read_channel_millivolts(AIR_QUALITY_MQ7_CO_ADC_CHANNEL, mq7_cali_handle, mq7_cali_available, &mv)) {
                GasAlarmState state = classify_rising(mv, AIR_QUALITY_MQ7_WARNING_MV, AIR_QUALITY_MQ7_CRITICAL_MV);
                esp_matter_attr_val_t level_val = esp_matter_enum8(
                    chip::to_underlying(gas_alarm_state_to_level_value<CarbonMonoxideConcentrationMeasurement::LevelValueEnum>(state)));
                attribute::update(air_quality_endpoint_id, CarbonMonoxideConcentrationMeasurement::Id,
                                  CarbonMonoxideConcentrationMeasurement::Attributes::LevelValue::Id, &level_val);
                g_state.co_valid = true;
                g_state.co_poor = (state != GasAlarmState::kNormal);
                ESP_LOGI(TAG, "MQ-7 (CO): %d mV", mv);
            }
        }
#endif

#if AIR_QUALITY_HAS_MQ131_OZONE
        if (mq131_ok) {
            int mv = 0;
            if (read_channel_millivolts(AIR_QUALITY_MQ131_OZONE_ADC_CHANNEL, mq131_cali_handle, mq131_cali_available, &mv)) {
                GasAlarmState state = classify_falling(mv, AIR_QUALITY_MQ131_WARNING_MV, AIR_QUALITY_MQ131_CRITICAL_MV);
                esp_matter_attr_val_t level_val = esp_matter_enum8(
                    chip::to_underlying(gas_alarm_state_to_level_value<OzoneConcentrationMeasurement::LevelValueEnum>(state)));
                attribute::update(air_quality_endpoint_id, OzoneConcentrationMeasurement::Id,
                                  OzoneConcentrationMeasurement::Attributes::LevelValue::Id, &level_val);
                g_state.ozone_valid = true;
                g_state.ozone_poor = (state != GasAlarmState::kNormal);
                ESP_LOGI(TAG, "MQ-131 (Ozone): %d mV", mv);
            }
        }
#endif

#if AIR_QUALITY_HAS_PMS5003_PM
        if (pms5003_ok) {
            float pm1 = 0.0f, pm25 = 0.0f, pm10 = 0.0f;
            if (pms5003_read(&pm1, &pm25, &pm10)) {
                esp_matter_attr_val_t pm1_val = esp_matter_nullable_float(nullable<float>(pm1));
                attribute::update(air_quality_endpoint_id, Pm1ConcentrationMeasurement::Id,
                                  Pm1ConcentrationMeasurement::Attributes::MeasuredValue::Id, &pm1_val);
                esp_matter_attr_val_t pm25_val = esp_matter_nullable_float(nullable<float>(pm25));
                attribute::update(air_quality_endpoint_id, Pm25ConcentrationMeasurement::Id,
                                  Pm25ConcentrationMeasurement::Attributes::MeasuredValue::Id, &pm25_val);
                esp_matter_attr_val_t pm10_val = esp_matter_nullable_float(nullable<float>(pm10));
                attribute::update(air_quality_endpoint_id, Pm10ConcentrationMeasurement::Id,
                                  Pm10ConcentrationMeasurement::Attributes::MeasuredValue::Id, &pm10_val);
                g_state.pm_valid = true;
                g_state.pm25_ugm3 = pm25;
                ESP_LOGI(TAG, "PMS5003: PM1.0 %.0f, PM2.5 %.0f, PM10 %.0f ug/m3", pm1, pm25, pm10);
            }
        }
#endif

#if AIR_QUALITY_HAS_ZE08CH2O_HCHO
        if (ze08ch2o_ok) {
            float hcho_ppm = 0.0f;
            if (ze08ch2o_read(&hcho_ppm)) {
                esp_matter_attr_val_t hcho_val = esp_matter_nullable_float(nullable<float>(hcho_ppm));
                attribute::update(air_quality_endpoint_id, FormaldehydeConcentrationMeasurement::Id,
                                  FormaldehydeConcentrationMeasurement::Attributes::MeasuredValue::Id, &hcho_val);
                g_state.hcho_valid = true;
                g_state.hcho_ppm = hcho_ppm;
                ESP_LOGI(TAG, "ZE08-CH2O (Formaldehyde): %.3f ppm", hcho_ppm);
            }
        }
#endif

#if AIR_QUALITY_HAS_MICS4514_NO2
        if (mics4514_ok) {
            float no2_ppm = 0.0f;
            if (mics4514_read(&no2_ppm)) {
                esp_matter_attr_val_t no2_val = esp_matter_nullable_float(nullable<float>(no2_ppm));
                attribute::update(air_quality_endpoint_id, NitrogenDioxideConcentrationMeasurement::Id,
                                  NitrogenDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id, &no2_val);
                g_state.no2_valid = true;
                g_state.no2_ppm = no2_ppm;
                ESP_LOGI(TAG, "MiCS-4514 (NO2): %.3f ppm", no2_ppm);
            }
        }
#endif

        recompute_and_publish_air_quality();
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

/* No controller-writable attributes on this device — all state flows from
 * the sensor task above, same no-op shape as firmware/contact-sensor/'s
 * and firmware/occupancy-sensor/'s own app_attribute_update_cb(). */
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

    /* 2. Configure the identify LED + its blink timer (not started yet —
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

    /* 3. Build the Matter data model: one node, one Air Quality Sensor
     * endpoint (Identify + AirQuality + Descriptor, all via the complete
     * top-level helper), plus every concentration-measurement cluster
     * this build has enabled, all added onto that same endpoint
     * afterwards — see the header comment above for why this ordering is
     * safe. */
    node::config_t node_config;
    strncpy(node_config.root_node.basic_information.node_label, "Air Quality Sensor",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::air_quality_sensor::config_t air_quality_config;
    endpoint_t *air_quality_endpoint = endpoint::air_quality_sensor::create(node, &air_quality_config, ENDPOINT_FLAG_NONE, NULL);
    if (!air_quality_endpoint) {
        ESP_LOGE(TAG, "Failed to create air quality sensor endpoint");
        return;
    }
    air_quality_endpoint_id = endpoint::get_id(air_quality_endpoint);
    ESP_LOGI(TAG, "Air quality sensor endpoint id: %u", air_quality_endpoint_id);

    cluster::carbon_dioxide_concentration_measurement::config_t co2_config;
    co2_config.measurement_medium = chip::to_underlying(CarbonDioxideConcentrationMeasurement::MeasurementMediumEnum::kAir);
    co2_config.feature_flags = chip::to_underlying(CarbonDioxideConcentrationMeasurement::Feature::kNumericMeasurement);
    co2_config.features.numeric_measurement.measured_value = nullable<float>();
    co2_config.features.numeric_measurement.min_measured_value = nullable<float>(400.0f);
    co2_config.features.numeric_measurement.max_measured_value = nullable<float>(29206.0f);
    co2_config.features.numeric_measurement.measurement_unit =
        chip::to_underlying(CarbonDioxideConcentrationMeasurement::MeasurementUnitEnum::kPpm);
    if (!cluster::carbon_dioxide_concentration_measurement::create(air_quality_endpoint, &co2_config, CLUSTER_FLAG_SERVER)) {
        ESP_LOGE(TAG, "Failed to create CO2 concentration measurement cluster");
        return;
    }

    cluster::total_volatile_organic_compounds_concentration_measurement::config_t tvoc_config;
    tvoc_config.measurement_medium =
        chip::to_underlying(TotalVolatileOrganicCompoundsConcentrationMeasurement::MeasurementMediumEnum::kAir);
    tvoc_config.feature_flags =
        chip::to_underlying(TotalVolatileOrganicCompoundsConcentrationMeasurement::Feature::kNumericMeasurement);
    tvoc_config.features.numeric_measurement.measured_value = nullable<float>();
    tvoc_config.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
    tvoc_config.features.numeric_measurement.max_measured_value = nullable<float>(32768.0f);
    tvoc_config.features.numeric_measurement.measurement_unit =
        chip::to_underlying(TotalVolatileOrganicCompoundsConcentrationMeasurement::MeasurementUnitEnum::kPpb);
    if (!cluster::total_volatile_organic_compounds_concentration_measurement::create(air_quality_endpoint, &tvoc_config,
                                                                                     CLUSTER_FLAG_SERVER)) {
        ESP_LOGE(TAG, "Failed to create TVOC concentration measurement cluster");
        return;
    }

#if AIR_QUALITY_HAS_TEMP_HUMIDITY
    cluster::temperature_measurement::config_t temp_config;
    temp_config.measured_value = nullable<int16_t>();
    temp_config.min_measured_value = nullable<int16_t>(-4000);
    temp_config.max_measured_value = nullable<int16_t>(8500);
    cluster::temperature_measurement::create(air_quality_endpoint, &temp_config, CLUSTER_FLAG_SERVER);

    cluster::relative_humidity_measurement::config_t hum_config;
    hum_config.measured_value = nullable<uint16_t>();
    hum_config.min_measured_value = nullable<uint16_t>(0);
    hum_config.max_measured_value = nullable<uint16_t>(10000);
    cluster::relative_humidity_measurement::create(air_quality_endpoint, &hum_config, CLUSTER_FLAG_SERVER);
#endif

#if AIR_QUALITY_HAS_MQ7_CO
    cluster::carbon_monoxide_concentration_measurement::config_t co_config;
    co_config.measurement_medium = chip::to_underlying(CarbonMonoxideConcentrationMeasurement::MeasurementMediumEnum::kAir);
    co_config.feature_flags = chip::to_underlying(CarbonMonoxideConcentrationMeasurement::Feature::kLevelIndication);
    co_config.features.level_indication.level_value =
        chip::to_underlying(CarbonMonoxideConcentrationMeasurement::LevelValueEnum::kUnknown);
    cluster::carbon_monoxide_concentration_measurement::create(air_quality_endpoint, &co_config, CLUSTER_FLAG_SERVER);
#endif

#if AIR_QUALITY_HAS_MQ131_OZONE
    cluster::ozone_concentration_measurement::config_t ozone_config;
    ozone_config.measurement_medium = chip::to_underlying(OzoneConcentrationMeasurement::MeasurementMediumEnum::kAir);
    ozone_config.feature_flags = chip::to_underlying(OzoneConcentrationMeasurement::Feature::kLevelIndication);
    ozone_config.features.level_indication.level_value =
        chip::to_underlying(OzoneConcentrationMeasurement::LevelValueEnum::kUnknown);
    cluster::ozone_concentration_measurement::create(air_quality_endpoint, &ozone_config, CLUSTER_FLAG_SERVER);
#endif

#if AIR_QUALITY_HAS_PMS5003_PM
    cluster::pm1_concentration_measurement::config_t pm1_config;
    pm1_config.measurement_medium = chip::to_underlying(Pm1ConcentrationMeasurement::MeasurementMediumEnum::kAir);
    pm1_config.feature_flags = chip::to_underlying(Pm1ConcentrationMeasurement::Feature::kNumericMeasurement);
    pm1_config.features.numeric_measurement.measured_value = nullable<float>();
    pm1_config.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
    pm1_config.features.numeric_measurement.max_measured_value = nullable<float>(1000.0f);
    pm1_config.features.numeric_measurement.measurement_unit = chip::to_underlying(Pm1ConcentrationMeasurement::MeasurementUnitEnum::kUgm3);
    cluster::pm1_concentration_measurement::create(air_quality_endpoint, &pm1_config, CLUSTER_FLAG_SERVER);

    cluster::pm25_concentration_measurement::config_t pm25_config;
    pm25_config.measurement_medium = chip::to_underlying(Pm25ConcentrationMeasurement::MeasurementMediumEnum::kAir);
    pm25_config.feature_flags = chip::to_underlying(Pm25ConcentrationMeasurement::Feature::kNumericMeasurement);
    pm25_config.features.numeric_measurement.measured_value = nullable<float>();
    pm25_config.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
    pm25_config.features.numeric_measurement.max_measured_value = nullable<float>(1000.0f);
    pm25_config.features.numeric_measurement.measurement_unit = chip::to_underlying(Pm25ConcentrationMeasurement::MeasurementUnitEnum::kUgm3);
    cluster::pm25_concentration_measurement::create(air_quality_endpoint, &pm25_config, CLUSTER_FLAG_SERVER);

    cluster::pm10_concentration_measurement::config_t pm10_config;
    pm10_config.measurement_medium = chip::to_underlying(Pm10ConcentrationMeasurement::MeasurementMediumEnum::kAir);
    pm10_config.feature_flags = chip::to_underlying(Pm10ConcentrationMeasurement::Feature::kNumericMeasurement);
    pm10_config.features.numeric_measurement.measured_value = nullable<float>();
    pm10_config.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
    pm10_config.features.numeric_measurement.max_measured_value = nullable<float>(1000.0f);
    pm10_config.features.numeric_measurement.measurement_unit = chip::to_underlying(Pm10ConcentrationMeasurement::MeasurementUnitEnum::kUgm3);
    cluster::pm10_concentration_measurement::create(air_quality_endpoint, &pm10_config, CLUSTER_FLAG_SERVER);
#endif

#if AIR_QUALITY_HAS_ZE08CH2O_HCHO
    cluster::formaldehyde_concentration_measurement::config_t hcho_config;
    hcho_config.measurement_medium = chip::to_underlying(FormaldehydeConcentrationMeasurement::MeasurementMediumEnum::kAir);
    hcho_config.feature_flags = chip::to_underlying(FormaldehydeConcentrationMeasurement::Feature::kNumericMeasurement);
    hcho_config.features.numeric_measurement.measured_value = nullable<float>();
    hcho_config.features.numeric_measurement.min_measured_value = nullable<float>(0.0f);
    hcho_config.features.numeric_measurement.max_measured_value = nullable<float>(5.0f);
    hcho_config.features.numeric_measurement.measurement_unit =
        chip::to_underlying(FormaldehydeConcentrationMeasurement::MeasurementUnitEnum::kPpm);
    cluster::formaldehyde_concentration_measurement::create(air_quality_endpoint, &hcho_config, CLUSTER_FLAG_SERVER);
#endif

#if AIR_QUALITY_HAS_MICS4514_NO2
    cluster::nitrogen_dioxide_concentration_measurement::config_t no2_config;
    no2_config.measurement_medium = chip::to_underlying(NitrogenDioxideConcentrationMeasurement::MeasurementMediumEnum::kAir);
    no2_config.feature_flags = chip::to_underlying(NitrogenDioxideConcentrationMeasurement::Feature::kNumericMeasurement);
    no2_config.features.numeric_measurement.measured_value = nullable<float>();
    no2_config.features.numeric_measurement.min_measured_value = nullable<float>(0.1f);
    no2_config.features.numeric_measurement.max_measured_value = nullable<float>(10.0f);
    no2_config.features.numeric_measurement.measurement_unit =
        chip::to_underlying(NitrogenDioxideConcentrationMeasurement::MeasurementUnitEnum::kPpm);
    cluster::nitrogen_dioxide_concentration_measurement::create(air_quality_endpoint, &no2_config, CLUSTER_FLAG_SERVER);
#endif

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

    /* 5. Start the sensor polling task — inits every enabled sensor and
     * reports readings for as long as the device runs. */
    xTaskCreate(air_quality_task, "air_quality_task", 6144, NULL, 5, NULL);

    ESP_LOGI(TAG, "Matter air quality sensor started. Scan the QR code to commission.");
}
