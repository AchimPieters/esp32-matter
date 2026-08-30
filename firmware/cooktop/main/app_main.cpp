/*
 * Minimal Matter Cooktop — forty-ninth device type, and this repo's second
 * genuinely composed, multi-endpoint device after firmware/refrigerator/: a
 * Cooktop (0x0078) root endpoint with one Cook Surface (0x0077) *child*
 * endpoint (a single heating zone), linked via esp-matter's real parent-child
 * endpoint API, same as firmware/refrigerator/'s own Fridge/Freezer pair.
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
 * --- Why Oven wasn't picked instead, and why Cooktop+CookSurface is a
 * cleaner next step -------------------------------------------------------
 * Oven (0x007B) was researched first as this repo's next candidate — its own
 * root endpoint mandates a composed Temperature Controlled Cabinet child
 * under the "Heater" condition, which unlocks two clusters this repo hasn't
 * used before: Oven Cavity Operational State (0x0048) and Oven Mode (0x0049).
 * Both turned out to be a genuinely deeper SDK gap than anything else built
 * so far: confirmed by reading esp-matter's own source directly, neither has
 * a legacy `esp_matter_cluster.cpp` namespace helper at all (only a
 * `generated/clusters/oven_mode/oven_mode.cpp` /
 * `generated/clusters/oven_cavity_operational_state/` pair exists — under
 * the "generated" data model this repo has never enabled, gated behind
 * `CONFIG_ESP_MATTER_ENABLE_GENERATED_DATA_MODEL`, same split firmware/
 * refrigerator/'s own header comment already documents for
 * TemperatureControl). The delegate-construction machinery
 * (`OvenModeDelegateInitCB`/`OvenCavityOperationalStateDelegateInitCB`) IS
 * present in the legacy `esp_matter_delegate_callbacks.cpp` — but with no
 * legacy `cluster::oven_mode::create()`/`cluster::oven_cavity_operational_
 * state::create()` wrapper calling it, building either cluster here would
 * mean hand-reconstructing that wrapper from the "generated" file's own
 * logic while depending on legacy-only headers — a real, deeper
 * architectural risk than any of this repo's prior hand-built Delegate
 * cases (RvcOperationalState included, which at least had a legacy
 * `common::config_t`-driven cluster shell already in place to attach to).
 * Given that, Oven is deferred rather than forced through; Cooktop +
 * CookSurface cover the same "hob/cooking appliance" ground with zero such
 * gaps — both have complete, ready-to-use legacy top-level helpers — and
 * introduce a genuinely new pattern of their own (TemperatureControl's TL
 * feature, never used elsewhere in this repo, which has always used TN).
 *
 * --- Cooktop root: Identify (optional) + On/Off[OffOnly] (mandatory) ------
 * Confirmed directly against the CSA's own data_model/1.6/device_types/
 * Cooktop.xml: Identify is `<optionalConform/>`; On/Off is
 * `<mandatoryConform/>` with its OffOnly (OFFONLY) feature also
 * `<mandatoryConform/>`. `endpoint::cooktop::create()` confirmed complete/
 * ready-to-use by reading esp-matter's own legacy `cooktop::add()` directly:
 * it calls `add_device_type()` + `on_off::create()` +
 * `on_off::feature::off_only::add(cluster)` internally, auto-Descriptor via
 * `common::create<T>()` — but does NOT add Identify (confirmed: no
 * `identify::create()` call in that function), added manually here, same
 * "optionalConform, not auto-wired" shape firmware/extractor-hood/'s and
 * firmware/water-heater/'s own root endpoints already hit.
 *
 * OffOnly is a real, spec-enforced safety restriction, not just documentation
 * text: confirmed by reading connectedhomeip's own `OnOffCluster.cpp`
 * directly — when the OffOnly feature bit is set, the cluster's own accepted-
 * commands list is narrowed to Off only (a `kOffOnlyCommands` array,
 * enforced by the Interaction Model itself via the cluster's advertised
 * `AcceptedCommands`) — a remote controller's On command is rejected at the
 * protocol level before it ever reaches this file's own code, the same kind
 * of built-in, spec-mandated safety net a real cooktop needs (nobody should
 * be able to remotely switch on a hot appliance they can't see). This means
 * the ONLY way this cooktop turns on is locally, at the device itself — this
 * file wires a real `COOKTOP_POWER_BUTTON_GPIO` pushbutton that writes the
 * On/Off attribute directly via `attribute::update()` (the same "app writes
 * the attribute in response to a local physical event" pattern firmware/
 * outlet/'s own button already uses) — writing the attribute directly is
 * unaffected by OffOnly, since that restriction only narrows what a remote
 * *command* may do, not what local app code may write. A controller can
 * still always turn it Off remotely (both by the Off command and by reading
 * the live attribute), matching the safety feature's whole intent.
 *
 * --- CookSurface child: TemperatureControl[TL] only, no
 * TemperatureMeasurement -----------------------------------------------
 * Confirmed directly against the CSA's own CookSurface.xml (revision 2 —
 * "Made TemperatureLevel (TL) the only valid temperature control mode"):
 * On/Off[OffOnly] optionalConform (not added here — a single-zone design
 * already has the root's own On/Off as its master switch; a second,
 * per-zone On/Off would be redundant complexity for one zone, same
 * "smallest reasonable next step" scope cut as every other device type's
 * own first cut); TemperatureControl and TemperatureMeasurement are both
 * `<optionalConform choice="a" more="true" min="1"/>` — a real "at least
 * one of these two" choice group, the same kind of constraint firmware/
 * closure/'s own Positioning/MotionLatching pair and firmware/
 * occupancy-sensor/'s own sensing-modality features already established in
 * this repo, confirmed by reading `endpoint::cook_surface::add()` directly:
 * it enforces the same choice itself via `VALIDATE_OPTIONAL_CLUSTERS_AT_
 * LEAST_ONE("cook_surface", ...)`. This file enables TemperatureControl
 * only — satisfying that choice on its own — and deliberately skips
 * TemperatureMeasurement: a real cook-surface temperature reading needs a
 * sensor rated for genuinely hot surface temperatures (a hob element can
 * exceed 200 degC), and this repo's only high-temperature-capable driver
 * so far (K-type thermocouple amplifiers like MAX6675/MAX31855) hasn't been
 * sourced or datasheet-verified for this device type — same "no sensor, no
 * fabricated data" honesty precedent firmware/air-quality-sensor/'s own
 * skipped particulate/gas clusters and firmware/smoke-co-alarm/'s skipped
 * Temperature/Humidity Measurement already establish, not a technical
 * limitation of the cluster itself. TemperatureControl[TL] needs no
 * numeric sensor at all — it's a discrete, controller/locally-selected
 * *power level*, not a measured temperature, so this device is honestly
 * buildable with nothing more than a relay/SSR and zero sensors.
 *
 * `cook_surface::add()` confirmed to force-add the TL feature flag itself
 * (`config->temperature_control.feature_flags |= cluster::
 * temperature_control::feature::temperature_level::get_id();`) once
 * `COOK_SURFACE_OPTIONAL_CLUSTER_TEMPERATURE_CONTROL` is requested via the
 * config's own `with_temperature_control()` builder method — no manual
 * feature-flag line needed here, unlike firmware/refrigerator/'s own TN
 * case (which the legacy `temperature_controlled_cabinet::add()` does NOT
 * auto-set, a real, previously-documented discrepancy specific to that
 * helper). `feature::temperature_level::config_t` is a single-field struct
 * (`selected_temp_level`, confirmed by reading `esp_matter_feature_impl.h`
 * directly) — no Min/Max fields exist for TL the way TN has them, since
 * discrete levels have no numeric range to bound.
 *
 * --- SetTemperature(TargetTemperatureLevel): handled entirely inside the
 * cluster, same as TN's TargetTemperature already established -------------
 * Confirmed by reading `TemperatureControlCluster::HandleSetTemperature()`
 * directly: exactly symmetric with firmware/refrigerator/'s own TN path —
 * when the TemperatureLevel feature is set, a controller's SetTemperature
 * command (carrying `TargetTemperatureLevel`) is handled entirely inside
 * the cluster via `SetSelectedTemperatureLevel()`, no delegate or app code
 * needed to *accept* the command. This file's own duty-cycle task (below)
 * only ever *reads* the live level back via `GetSelectedTemperatureLevel()`,
 * through the same registry-lookup-and-cast pattern firmware/
 * refrigerator/'s own `get_temperature_control_cluster()` already
 * establishes (confirmed: `TemperatureControlCluster` is a
 * `DefaultServerCluster`, not a plain ember attribute, so there is no
 * `attribute::PRE_UPDATE` hook to react to a level change — polling once
 * per duty-cycle window is enough here, the same reasoning firmware/
 * refrigerator/'s own bang-bang loop already uses for the same cluster).
 *
 * --- SupportedTemperatureLevels: a real, new Delegate for this repo -------
 * Unlike TN mode (firmware/refrigerator/'s own header comment explains in
 * full why `TemperatureControlCluster::SetDelegate()` is safely skippable
 * there — TL was disallowed, so the delegate-gated code paths were never
 * reached), THIS file genuinely needs one: `SupportedTemperatureLevels` is
 * itself gated behind a static
 * `TemperatureControl::SupportedTemperatureLevelsIteratorDelegate *`
 * (confirmed by reading `TemperatureControlCluster::ReadAttribute()`
 * directly — the `SupportedTemperatureLevels` case returns an empty list if
 * no delegate is set, and otherwise calls `mDelegate->Reset(endpoint)` then
 * iterates via `Size()`/`Next()`). This is a genuinely new "how do I supply
 * a cluster's own supported-value list" shape for this repo, closest in
 * spirit to firmware/laundry-washer/'s own `LaundryWasherControls` Delegate
 * (`GetSupportedRinseAtIndex()`) but for TemperatureControl specifically.
 * `CookSurfaceLevelsDelegate` here is a plain, ported-from-the-real-
 * reference shape (`examples/refrigerator-app/refrigerator-common/include/
 * static-supported-temperature-levels.h`, read directly) simplified for a
 * single endpoint with a single fixed level list — the reference's own
 * version is endpoint-aware (`supportedOptionsByEndpoints[]`, for an app
 * with more than one TemperatureControl instance); this device only ever
 * has one, so `mEndpoint` (inherited, set by the base class's own `Reset()`)
 * is simply unused. `TemperatureControlCluster::SetDelegate()` is a
 * `static` class method (confirmed directly — it's keyed to the class, not
 * any one endpoint's cluster instance), so it has no ordering dependency on
 * endpoint/cluster construction the way e.g. firmware/fan/'s own
 * `FanControl::SetDefaultDelegate()` does — called once, early, in
 * `app_main()`.
 *
 * Three levels are offered — "Low"/"Medium"/"High" — mapped to 33/66/100%
 * duty cycle respectively. A real induction/electric hob typically offers
 * more (commonly 9 numbered levels, or a dial with many more positions) —
 * three is a deliberate "smallest reasonable next step" scope cut, the same
 * kind of reduced-but-honest step count firmware/fan/'s own
 * `FanModeSequence::OffLowMedHigh` already applies to a continuous PWM
 * output for the same underlying reason (a real, useful subset rather than
 * an arbitrarily large invented list).
 *
 * --- Output: time-proportioned relay/SSR duty cycle, reusing firmware/
 * microwave-oven/'s own technique verbatim ---------------------------------
 * `COOKTOP_DUTY_CYCLE_WINDOW_SEC` (10s) is a fixed window; the zone's
 * relay/SSR is energized for whatever fraction of that window matches the
 * live level's duty percentage, then off for the rest — the exact same
 * time-proportioning technique firmware/microwave-oven/'s own
 * `MICROWAVE_DUTY_CYCLE_WINDOW_SEC` already establishes for switching a
 * fixed-output heating element to a variable *apparent* power level, real
 * general appliance-design knowledge (not a per-chip datasheet fact) that
 * applies identically here. The relay only ever energizes while the root
 * Cooktop's own On/Off attribute is true — a plain `static bool`
 * (`g_cooktop_on`) kept in sync by `app_attribute_update_cb()`'s
 * `PRE_UPDATE` hook, the same "app tracks a plain ember attribute via
 * PRE_UPDATE" pattern used throughout this repo (e.g. firmware/
 * water-heater/'s own SystemMode tracking) — covers both a remote Off
 * command and the local power button's own direct `attribute::update()`
 * write, since both funnel through the same attribute-store path.
 *
 * Standard quick-power-cycle factory reset. Build-verified in Docker; not
 * hardware-tested (no relay/SSR hardware for this device type physically
 * available when written).
 *
 * --- Later extended: TemperatureMeasurement, on request (rolling out this
 * repo's own `clusterOptions` wizard mechanism beyond air-quality-sensor/
 * smoke-co-alarm) ----------------------------------------------------------
 * `COOKTOP_HAS_TEMPERATURE_MEASUREMENT` (default off, unchanged default
 * build) closes the exact gap this file's own header comment above already
 * named: a real cook-surface temperature reading needs a sensor rated for
 * genuinely hot surface temperatures, and this repo's only high-temperature-
 * capable driver category — K-type thermocouple amplifiers — had never
 * actually been implemented/verified for any device type. Two real chip
 * choices (`COOKTOP_TEMP_SENSOR_CHIP`): MAX6675 (cheaper, 12-bit, 0-1024
 * degC, no fault detection) and MAX31855 (more capable — wider range
 * including negative temperatures, plus real SCV/SCG/OC fault-bit
 * detection) — added onto the SAME Cook Surface child endpoint that already
 * carries TemperatureControl[TL], satisfying CookSurface.xml's own "at
 * least one of TemperatureControl/TemperatureMeasurement" choice group
 * with BOTH now, not just one.
 *
 * A genuinely different sourcing situation from every other chip verified
 * in this repo, worth documenting honestly rather than glossing over: this
 * session had no direct network access to download either chip's real PDF
 * datasheet and run it through `pdftotext` (this repo's own established
 * practice for primary-source hardware protocol detail, used for every
 * other chip in this repo) — the sandboxed environment this session ran in
 * had no outbound network access from the shell at all, only through the
 * WebFetch/WebSearch tools. Verified instead via WebSearch cross-
 * referencing multiple independent sources: Analog Devices' own official
 * datasheet content (indexed by the search engine, since the raw PDF fetch
 * itself timed out repeatedly), Adafruit's own real, widely-used, open-
 * source Arduino libraries for both chips (fetched directly from GitHub —
 * `adafruit/MAX6675-library` and `adafruit/Adafruit-MAX31855-library`,
 * both real, working, hardware-tested reference implementations), and
 * public technical documentation. Flagged here, in the wizard, and in
 * CLAUDE.md as sourced this way rather than the usual direct-pdftotext
 * method — the same "best available, cross-checked" sourcing standard
 * already used in this repo for CSE7759/ADE7953/SM2335EGH/MiCS-4514, just
 * for a genuinely different reason (a tooling constraint this session,
 * not an unavailable/undocumented chip).
 *
 * Both chips share the exact same 3-wire, read-only interface (CS + SCK +
 * SO — no MOSI/data-in at all, confirmed by both real reference libraries
 * never driving a data-out pin), so both chip choices reuse the same three
 * GPIO defines. Deliberately bit-banged via plain GPIO (matching this
 * repo's own DHT/DS18B20/WS2812B precedent) rather than driven through
 * ESP-IDF's real SPI peripheral (the way firmware/addressable-light/'s own
 * APA102 driver is) — secondary sources genuinely disagreed on whether
 * these chips need SPI Mode 0 or Mode 1 (CPOL/CPHA), a real, unresolved
 * ambiguity without the primary datasheet in hand to settle it; bit-
 * banging the exact edge sequence Adafruit's own real MAX6675 library
 * uses (CS low, then for each bit: SCK low + a settle delay, sample SO,
 * SCK high + a settle delay) sidesteps that ambiguity entirely, the same
 * "port a real reference rather than guess" precedent this repo already
 * applies elsewhere. MAX6675's own 16-bit word layout (ported from that
 * same reference, cross-checked against the indexed datasheet content):
 * D15 dummy sign bit (always 0), D14-D3 the 12-bit temperature (0.25 degC/
 * LSB, 0-1024 degC only — no negative-temperature support at all, a real,
 * documented chip limitation, not a driver bug), D2 open-thermocouple
 * fault flag, D1/D0 device-ID/state bits (ignored here). MAX31855's own
 * 32-bit word layout (ported from Adafruit's own real MAX31855 library,
 * cross-checked the same way): D31 sign + D30-D18 magnitude (14-bit
 * thermocouple temperature, 0.25 degC/LSB, sign-extended when negative),
 * D17 reserved, D16 fault flag, D15 sign + D14-D4 magnitude (12-bit cold-
 * junction/internal temperature, 0.0625 degC/LSB — read but unused here,
 * the chip's own cold-junction compensation already folds it into the
 * thermocouple reading), D3 reserved, D2/D1/D0 = SCV/SCG/OC individual
 * fault bits (a real, controller-visible fault clears the reading to
 * TemperatureMeasurement's own null "no valid reading" state rather than
 * reporting a fabricated number). MeasuredValue's own 0-40000 (0-400.00
 * degC) Min/Max range is a plain, honest bound for a real cook-surface/pan
 * probe reading, well inside both chips' own real measurement range.
 * Build-verified in Docker for both chip choices; not hardware-tested (no
 * MAX6675/MAX31855 module or K-type thermocouple probe physically
 * available when written).
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <esp_rom_sys.h> /* esp_rom_delay_us() — optional thermocouple bit-bang */
#include <cstring>

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app/clusters/temperature-control-server/TemperatureControlCluster.h>
#include <app/clusters/temperature-control-server/supported-temperature-levels-manager.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <data_model_provider/esp_matter_data_model_provider.h>

static const char *TAG = "matter_cooktop";

/* --- GPIO pin map -----------------------------------------------------------
 * All non-strapping pins on classic ESP32 (WROOM-32). "Always check your
 * specific relay/SSR module" — polarity isn't universal. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define COOKTOP_POWER_BUTTON_GPIO GPIO_NUM_4   /* pulled up: GND -> button -> GPIO, same wiring convention as firmware/switch/'s own button */
#define COOKTOP_ZONE_RELAY_GPIO GPIO_NUM_16    /* active-LOW, drives the zone's relay/SSR */

#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Local power button debounce (plain N-consistent-samples poll, same shape
 * firmware/refrigerator/'s own door_task() already uses for its one digital
 * input). */
#define COOKTOP_BUTTON_POLL_INTERVAL_MS 20
#define COOKTOP_BUTTON_DEBOUNCE_SAMPLES 3

/* See the header comment above for the full sourcing on this technique —
 * reused verbatim from firmware/microwave-oven/'s own duty-cycle window. */
#define COOKTOP_DUTY_CYCLE_WINDOW_SEC 10

#define COOKTOP_NUM_LEVELS 3
static const uint8_t kLevelDutyPercent[COOKTOP_NUM_LEVELS] = {33, 66, 100}; /* Low, Medium, High */
static const char *const kLevelNames[COOKTOP_NUM_LEVELS] = {"Low", "Medium", "High"};

/* --- Optional TemperatureMeasurement (K-type thermocouple) — see the
 * header comment above ("Later extended") for the full sourcing and
 * design detail. Off by default (unchanged default build). */
#define COOKTOP_HAS_TEMPERATURE_MEASUREMENT 0

#define COOKTOP_TEMP_SENSOR_CHIP_MAX6675 1
#define COOKTOP_TEMP_SENSOR_CHIP_MAX31855 2
#define COOKTOP_TEMP_SENSOR_CHIP COOKTOP_TEMP_SENSOR_CHIP_MAX6675

#if COOKTOP_HAS_TEMPERATURE_MEASUREMENT
/* Both chips share the exact same 3-wire, read-only bit-banged interface
 * (no MOSI/data-in pin exists on either chip) — one shared GPIO pin set. */
#define COOKTOP_TEMP_SENSOR_CS_GPIO GPIO_NUM_5
#define COOKTOP_TEMP_SENSOR_SCK_GPIO GPIO_NUM_18
#define COOKTOP_TEMP_SENSOR_SO_GPIO GPIO_NUM_19

#define COOKTOP_TEMP_SENSOR_POLL_INTERVAL_MS 2000
#endif

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
/* Only the `_span` string-literal operator, not all of `chip::` — see
 * firmware/robot-vacuum/'s own header comment for the exact namespace-
 * ambiguity compile error a blanket `using namespace chip;` caused there.
 * Not actually needed by this file's own command payloads (this is a
 * server-side device, no client command JSON built here) but kept for
 * consistency with every other file in this repo that touches
 * `chip::`/`chip::app::` types. */
using namespace chip::literals;

static uint16_t cooktop_endpoint_id = 0;
static uint16_t zone_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
static bool g_cooktop_on = false;

/* --- SupportedTemperatureLevels delegate -----------------------------------
 * See the header comment above for why this repo needs a real one here
 * (unlike firmware/refrigerator/'s TN-only case, which never reaches the
 * delegate-gated code paths at all). Simplified from the real reference
 * (`examples/refrigerator-app/.../static-supported-temperature-levels.h`)
 * for a single fixed level list on a single endpoint — `mEndpoint`
 * (inherited from the base class, set by its own `Reset()`) is unused. */
class CookSurfaceLevelsDelegate : public TemperatureControl::SupportedTemperatureLevelsIteratorDelegate
{
public:
    uint8_t Size() override { return COOKTOP_NUM_LEVELS; }

    CHIP_ERROR Next(chip::MutableCharSpan &item) override
    {
        if (mIndex >= COOKTOP_NUM_LEVELS) {
            return CHIP_ERROR_PROVIDER_LIST_EXHAUSTED;
        }
        const char *text = kLevelNames[mIndex];
        mIndex++;
        return chip::CopyCharSpanToMutableCharSpan(chip::CharSpan(text, strlen(text)), item);
    }
};

static CookSurfaceLevelsDelegate cook_surface_levels_delegate;

/* --- Registry-lookup-and-cast helper ---------------------------------------
 * Same pattern this repo's other code-driven-cluster access already uses
 * (see e.g. firmware/refrigerator/'s own get_temperature_control_cluster()). */
static TemperatureControlCluster *get_temperature_control_cluster(uint16_t endpoint_id)
{
    chip::app::ConcreteClusterPath path(endpoint_id, TemperatureControl::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        return nullptr;
    }
    return static_cast<TemperatureControlCluster *>(iface);
}

#if COOKTOP_HAS_TEMPERATURE_MEASUREMENT
/* --- K-type thermocouple driver (MAX6675/MAX31855) -------------------------
 * See the header comment above ("Later extended") for the full sourcing —
 * a bit-banged 3-wire read, deliberately not ESP-IDF's real SPI peripheral,
 * to sidestep a genuine SPI-mode ambiguity in the available sources. Exact
 * edge sequence ported from Adafruit's own real MAX6675 library: CS low,
 * then per bit — SCK low + settle, sample SO, SCK high + settle — CS high
 * once all bits are shifted in. */
#define COOKTOP_TEMP_SENSOR_BIT_DELAY_US 10

static uint32_t cooktop_temp_sensor_read_raw(uint8_t num_bits)
{
    uint32_t value = 0;

    gpio_set_level(COOKTOP_TEMP_SENSOR_CS_GPIO, 0);
    esp_rom_delay_us(COOKTOP_TEMP_SENSOR_BIT_DELAY_US);

    for (uint8_t i = 0; i < num_bits; i++) {
        gpio_set_level(COOKTOP_TEMP_SENSOR_SCK_GPIO, 0);
        esp_rom_delay_us(COOKTOP_TEMP_SENSOR_BIT_DELAY_US);
        value <<= 1;
        if (gpio_get_level(COOKTOP_TEMP_SENSOR_SO_GPIO)) {
            value |= 1;
        }
        gpio_set_level(COOKTOP_TEMP_SENSOR_SCK_GPIO, 1);
        esp_rom_delay_us(COOKTOP_TEMP_SENSOR_BIT_DELAY_US);
    }

    gpio_set_level(COOKTOP_TEMP_SENSOR_CS_GPIO, 1);
    return value;
}

/* Returns the temperature in centidegrees C via `*out_centidegrees`, or
 * false on a detected fault (open thermocouple, or — MAX31855 only — a
 * short to VCC/GND) — the caller reports a null MeasuredValue in that
 * case, never a fabricated number. */
static bool cooktop_temp_sensor_read(int32_t *out_centidegrees)
{
#if COOKTOP_TEMP_SENSOR_CHIP == COOKTOP_TEMP_SENSOR_CHIP_MAX6675
    /* 16-bit word: D15 dummy sign (0), D14-D3 = 12-bit temperature
     * (0.25 degC/LSB, 0-1024 degC only), D2 = open-thermocouple fault,
     * D1/D0 = device ID/state (ignored). */
    uint32_t raw = cooktop_temp_sensor_read_raw(16);
    if (raw & 0x4) {
        return false; /* open thermocouple */
    }
    uint32_t temp_12bit = (raw >> 3) & 0xFFF;
    *out_centidegrees = (int32_t)(temp_12bit * 25); /* x0.25 degC, in centidegrees */
    return true;
#elif COOKTOP_TEMP_SENSOR_CHIP == COOKTOP_TEMP_SENSOR_CHIP_MAX31855
    /* 32-bit word: D31 sign + D30-D18 = 14-bit thermocouple temperature
     * (0.25 degC/LSB), D17 reserved, D16 = fault flag, D15-D4 = 12-bit
     * cold-junction temperature (read but unused — the chip's own cold-
     * junction compensation already folds it into the thermocouple
     * reading above), D3 reserved, D2/D1/D0 = SCV/SCG/OC fault bits. */
    uint32_t raw = cooktop_temp_sensor_read_raw(32);
    if (raw & 0x7) {
        return false; /* SCV, SCG, or OC fault */
    }
    int32_t temp_14bit;
    if (raw & 0x80000000) {
        /* Negative — sign-extend the 14-bit field into a 32-bit value. */
        temp_14bit = (int32_t)(0xFFFFC000u | ((raw >> 18) & 0x3FFFu));
    } else {
        temp_14bit = (int32_t)(raw >> 18);
    }
    *out_centidegrees = temp_14bit * 25; /* x0.25 degC, in centidegrees */
    return true;
#else
#error "Unknown COOKTOP_TEMP_SENSOR_CHIP"
#endif
}

/* Code-driven cluster setter — same registry-lookup-and-cast pattern
 * firmware/smoke-co-alarm/'s own update_temperature() already establishes. */
static void cooktop_update_zone_temperature(chip::app::DataModel::Nullable<int16_t> value)
{
    lock::ScopedChipStackLock stack_lock(portMAX_DELAY);
    chip::app::ConcreteClusterPath path(zone_endpoint_id, TemperatureMeasurement::Id);
    chip::app::ServerClusterInterface *iface = esp_matter::data_model::provider::get_instance().registry().Get(path);
    if (!iface) {
        ESP_LOGE(TAG, "TemperatureMeasurement cluster not found on endpoint %u", zone_endpoint_id);
        return;
    }
    static_cast<chip::app::Clusters::TemperatureMeasurementCluster *>(iface)->SetMeasuredValue(value);
}

static void cooktop_temp_sensor_task(void *arg)
{
    while (true) {
        int32_t centidegrees = 0;
        if (cooktop_temp_sensor_read(&centidegrees)) {
            cooktop_update_zone_temperature(chip::app::DataModel::Nullable<int16_t>((int16_t)centidegrees));
        } else {
            ESP_LOGW(TAG, "Thermocouple fault detected — reporting null MeasuredValue");
            cooktop_update_zone_temperature(chip::app::DataModel::Nullable<int16_t>());
        }
        vTaskDelay(pdMS_TO_TICKS(COOKTOP_TEMP_SENSOR_POLL_INTERVAL_MS));
    }
}
#endif /* COOKTOP_HAS_TEMPERATURE_MEASUREMENT */

/* --- Duty-cycle output task -------------------------------------------------
 * See the header comment above for the full sourcing on this time-
 * proportioning technique. Polls the live SelectedTemperatureLevel once per
 * window (this is a discrete level a controller sets occasionally, not a
 * continuously-regulated value — no need for a tighter poll) and forces 0%
 * duty whenever the cooktop's own root On/Off is false, regardless of level. */
static void zone_duty_task(void *arg)
{
    while (true) {
        uint8_t level = 0;
        TemperatureControlCluster *ctrl = get_temperature_control_cluster(zone_endpoint_id);
        if (ctrl) {
            level = ctrl->GetSelectedTemperatureLevel();
            if (level >= COOKTOP_NUM_LEVELS) {
                level = 0;
            }
        }

        uint8_t duty_percent = g_cooktop_on ? kLevelDutyPercent[level] : 0;
        uint32_t window_ms = COOKTOP_DUTY_CYCLE_WINDOW_SEC * 1000UL;
        uint32_t on_ms = (window_ms * duty_percent) / 100;
        uint32_t off_ms = window_ms - on_ms;

        if (on_ms > 0) {
            gpio_set_level(COOKTOP_ZONE_RELAY_GPIO, 0); /* active-LOW: on */
            vTaskDelay(pdMS_TO_TICKS(on_ms));
        }
        if (off_ms > 0) {
            gpio_set_level(COOKTOP_ZONE_RELAY_GPIO, 1); /* off */
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

/* --- Local power button -----------------------------------------------------
 * See the header comment above for why this is the ONLY way to turn the
 * cooktop on at all (OffOnly blocks a remote controller's On command at the
 * protocol level). Toggles the root On/Off attribute directly on a
 * debounced press edge — same "app writes the attribute in response to a
 * local physical event" pattern firmware/outlet/'s own button already uses,
 * unaffected by OffOnly since that restriction only narrows remote
 * *commands*, not local attribute writes. */
static void power_button_task(void *arg)
{
    bool last_sample_pressed = false;
    int consistent_count = 0;
    bool was_pressed = false;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(COOKTOP_BUTTON_POLL_INTERVAL_MS));

        bool sample_pressed = (gpio_get_level(COOKTOP_POWER_BUTTON_GPIO) == 0);
        if (sample_pressed == last_sample_pressed) {
            consistent_count++;
        } else {
            consistent_count = 1;
            last_sample_pressed = sample_pressed;
        }

        if (consistent_count >= COOKTOP_BUTTON_DEBOUNCE_SAMPLES) {
            if (sample_pressed && !was_pressed) {
                g_cooktop_on = !g_cooktop_on;
                esp_matter_attr_val_t val = esp_matter_bool(g_cooktop_on);
                attribute::update(cooktop_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
                ESP_LOGI(TAG, "Cooktop power button: %s", g_cooktop_on ? "ON" : "OFF");
            }
            was_pressed = sample_pressed;
        }
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

/* Tracks the root On/Off attribute (see the header comment above for why —
 * a remote Off command and the local power button's own direct write both
 * funnel through here). */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && endpoint_id == cooktop_endpoint_id &&
        cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        g_cooktop_on = val->val.b;
        ESP_LOGI(TAG, "Cooktop on/off attribute -> %s", g_cooktop_on ? "ON" : "OFF");
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

    /* 2. Configure the local power button (pulled up: released=HIGH,
     * pressed=LOW). */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << COOKTOP_POWER_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&button_io_conf);

    /* 2b. Configure the zone relay/SSR output — boot off (de-energized),
     * same "boot to known safe state" convention every other device type
     * here follows. */
    gpio_config_t relay_io_conf = {};
    relay_io_conf.pin_bit_mask = (1ULL << COOKTOP_ZONE_RELAY_GPIO);
    relay_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&relay_io_conf);
    gpio_set_level(COOKTOP_ZONE_RELAY_GPIO, 1); /* active-LOW: 1 = off */

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

#if COOKTOP_HAS_TEMPERATURE_MEASUREMENT
    /* 2d. Configure the thermocouple amplifier's 3-wire read-only
     * interface — CS/SO as plain GPIO, SCK output starts high (idle),
     * matching the bit-bang sequence in cooktop_temp_sensor_read_raw(). */
    gpio_config_t temp_cs_conf = {};
    temp_cs_conf.pin_bit_mask = (1ULL << COOKTOP_TEMP_SENSOR_CS_GPIO);
    temp_cs_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&temp_cs_conf);
    gpio_set_level(COOKTOP_TEMP_SENSOR_CS_GPIO, 1);

    gpio_config_t temp_sck_conf = {};
    temp_sck_conf.pin_bit_mask = (1ULL << COOKTOP_TEMP_SENSOR_SCK_GPIO);
    temp_sck_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&temp_sck_conf);
    gpio_set_level(COOKTOP_TEMP_SENSOR_SCK_GPIO, 1);

    gpio_config_t temp_so_conf = {};
    temp_so_conf.pin_bit_mask = (1ULL << COOKTOP_TEMP_SENSOR_SO_GPIO);
    temp_so_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&temp_so_conf);
#endif

    /* 3. Register the SupportedTemperatureLevels delegate — a `static`
     * class method with no ordering dependency on endpoint/cluster
     * construction (see the header comment above), so this can safely run
     * before any endpoint exists yet. */
    TemperatureControlCluster::SetDelegate(&cook_surface_levels_delegate);

    /* 4. Build the Matter data model: one node, one Cooktop root endpoint
     * (Descriptor + On/Off[OffOnly] via the helper, plus Identify added
     * manually), and one Cook Surface child endpoint (Descriptor +
     * TemperatureControl[TL] via the helper) — see the header comment above
     * for the full detail. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint::cooktop::config_t cooktop_config;
    endpoint_t *cooktop_endpoint = endpoint::cooktop::create(node, &cooktop_config, ENDPOINT_FLAG_NONE, NULL);
    if (!cooktop_endpoint) {
        ESP_LOGE(TAG, "Failed to create cooktop endpoint");
        return;
    }
    cooktop_endpoint_id = endpoint::get_id(cooktop_endpoint);
    ESP_LOGI(TAG, "Cooktop endpoint id: %u", cooktop_endpoint_id);

    /* 4a. Identify onto the root endpoint — optionalConform, so
     * cooktop::create() doesn't add it. */
    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kActuator);
    cluster::identify::create(cooktop_endpoint, &identify_config, CLUSTER_FLAG_SERVER);

    /* 4b. Cook Surface zone — TemperatureControl[TL] only (see the header
     * comment above for why TemperatureMeasurement is skipped). Boots at
     * the lowest level ("Low"), not whatever a controller last set, matching
     * every other device type's own boot-to-known-safe-state convention. */
    endpoint::cook_surface::config_t zone_config;
    zone_config.with_temperature_control().features.temperature_level.selected_temp_level = 0;

    endpoint_t *zone_endpoint = endpoint::cook_surface::create(node, &zone_config, ENDPOINT_FLAG_NONE, NULL);
    if (!zone_endpoint) {
        ESP_LOGE(TAG, "Failed to create cook surface endpoint");
        return;
    }
    zone_endpoint_id = endpoint::get_id(zone_endpoint);
    ESP_LOGI(TAG, "Cook surface endpoint id: %u", zone_endpoint_id);

    err = set_parent_endpoint(zone_endpoint, cooktop_endpoint);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set cook surface's parent endpoint: %d", err);
        return;
    }

#if COOKTOP_HAS_TEMPERATURE_MEASUREMENT
    /* 4c. TemperatureMeasurement onto the SAME Cook Surface endpoint — see
     * the header comment above ("Later extended") for the full detail.
     * Code-driven (registry-lookup + SetMeasuredValue()), same check every
     * other code-driven cluster in this repo uses (a real
     * temperature_measurement/ folder exists under
     * data_model_provider/clusters/). Starts at null (no reading yet) until
     * the first poll. */
    cluster::temperature_measurement::config_t cook_temp_config;
    cook_temp_config.measured_value = nullable<int16_t>();
    cook_temp_config.min_measured_value = nullable<int16_t>(0);
    cook_temp_config.max_measured_value = nullable<int16_t>(40000);
    cluster::temperature_measurement::create(zone_endpoint, &cook_temp_config, CLUSTER_FLAG_SERVER);
#endif

    /* 5. Start Matter — begins BLE advertising so a controller can commission it. */
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

    /* 6. Start the control tasks — the power button's own debounced poll,
     * and the zone's duty-cycle output. */
    xTaskCreate(power_button_task, "power_button_task", 3072, NULL, 5, NULL);
    xTaskCreate(zone_duty_task, "zone_duty_task", 3072, NULL, 5, NULL);
#if COOKTOP_HAS_TEMPERATURE_MEASUREMENT
    xTaskCreate(cooktop_temp_sensor_task, "cooktop_temp_sensor_task", 3072, NULL, 5, NULL);
#endif

    ESP_LOGI(TAG, "Matter cooktop started. Scan the QR code to commission.");
}
