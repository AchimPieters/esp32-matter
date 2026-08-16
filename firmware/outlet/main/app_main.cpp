/*
 * Minimal Matter On/Off Plug-in Unit — configurable output polarity, plus
 * optional power monitoring (BL0942 or BL0937).
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
 * What this device does, and why it's a separate device type from
 * firmware/switch/: esp-matter's `on_off_light_switch` (used by
 * firmware/switch/) is a Matter *client* — it's a remote control that sends
 * commands to other devices via the Binding cluster, and has no on/off state
 * of its own for a controller to show or toggle. That's spec-correct for a
 * "remote switch", but it means Apple/Google Home display it as a generic,
 * uncontrollable "Matter Accessory" — they have no UI for setting up
 * Bindings, and there's no local attribute for them to render.
 *
 * This device uses `on_off_plug_in_unit` instead — the same *server-side*
 * OnOff pattern as firmware/light/, just with a "switch/outlet" device type
 * instead of "light" (different icon, same mechanics: same on_off cluster
 * handling in esp_matter_endpoint.cpp; `on_off::create(endpoint, ...,
 * CLUSTER_FLAG_SERVER)`). Apple/Google Home render it as a real, controllable
 * on/off tile: press the physical button and it toggles the Matter OnOff
 * attribute directly (attribute::update(), the exact call firmware/light/
 * uses), which any commissioned controller sees and can also toggle from its
 * own side. Nothing here talks to another device — for that, use
 * firmware/switch/'s Binding-based remote control instead.
 *
 * Apple/Google Home label this an "Outlet"/"Stopcontact", not a "Switch" —
 * that's expected, not a bug: the Matter device type library has no
 * separate device type for "a wall switch with its own on/off state"
 * distinct from a plug-in outlet (checked the spec directly, in
 * connectedhomeip's data_model/<version>/device_types/ folder: every
 * device type with "Switch" in the name — OnOffLightSwitch, DimmerSwitch,
 * ColorDimmerSwitch, GenericSwitch — is a client/input device, none of
 * them a controllable on/off output). This repo follows the Matter device
 * type library as specified rather than picking a device type for its
 * Apple Home icon; on_off_plug_in_unit is the spec-correct type for
 * exactly this device, icon included.
 *
 * --- Output type (OUTLET_OUTPUT_TYPE) ---------------------------------
 * A real, functional difference, not cosmetic: many common low-cost relay
 * modules (the ubiquitous single-channel opto-isolated breakout boards
 * built around something like an SRD-05VDC-SL-C relay) trigger
 * active-LOW — their IN pin has to go LOW to energize the relay, backwards
 * from a plain LED wired the "obvious" way (GPIO HIGH = on). Getting this
 * wrong doesn't just invert the logic, it means the outlet is physically
 * ON whenever the ESP32 is unpowered or mid-boot, which is a real safety
 * consideration for anything switching mains. OUTLET_OUTPUT_TYPE picks
 * which polarity set_output() below actually drives:
 *   OUTLET_OUTPUT_RELAY (default) — active-LOW. A relay is what an actual
 *                        power outlet/smart plug normally switches with —
 *                        this is the realistic default, not the LED that
 *                        was this option's only choice before it existed.
 *                        Always double-check your specific relay module's
 *                        own documentation — "most common low-cost
 *                        modules" is not a guarantee for yours.
 *   OUTLET_OUTPUT_LED    — active-HIGH, matches every version of this file
 *                        before this option existed. Mainly useful for
 *                        breadboard testing without a relay module on hand.
 *
 * --- Status LED (OUTLET_STATUS_LED_GPIO) --------------------------------
 * Optional, off by default (GPIO_NUM_NC — ESP-IDF's "not connected"
 * sentinel). Some real plug/outlet hardware has its own small indicator
 * LED, wired to its own GPIO, that continuously mirrors the outlet's
 * actual on/off state — different from IDENTIFY_LED_GPIO below, which
 * only blinks temporarily in response to a controller's Identify command
 * and says nothing about on/off state. Set this to a real GPIO to enable
 * it; set_output() then drives it (plain active-HIGH: GPIO HIGH when the
 * outlet is on) every time the on/off state changes, from any source —
 * button press, remote controller command, or Identify's own STOP action
 * restoring the real state afterwards.
 *
 * --- Power monitoring (OUTLET_POWER_MONITOR) ---------------------------
 * Optional, off by default (OUTLET_POWER_MONITOR_NONE). When enabled, adds
 * a second Matter endpoint (esp-matter's `electrical_sensor` device type,
 * id 0x0510) carrying the ElectricalPowerMeasurement and
 * ElectricalEnergyMeasurement clusters, alongside the outlet's own
 * on_off_plug_in_unit endpoint — the same composition real power-metering
 * smart plugs use (e.g. Sonoff S31, Athom smart plugs). Six real chips
 * are supported, falling into four genuinely different protocol families:
 *
 * Every chip's protocol/formula below was checked against its own
 * manufacturer's datasheet directly (not just a secondary source like
 * ESPHome) except where noted per-chip — this actually caught two real
 * bugs during development: BL0942's response packet had current and
 * voltage swapped (and power/energy at the wrong byte offsets entirely)
 * versus what an earlier draft assumed from a secondary source, and
 * CSE7766's Adj status byte was mischaracterized as raw measurement-
 * validity bits when it's actually cycle-completeness bits (same
 * skip-invalid-reading behavior in the end, different — now correctly
 * documented — reason). Both were found by fetching the actual PDF
 * datasheets and reading their byte-numbered tables directly, not by
 * inspection.
 *
 *   OUTLET_POWER_MONITOR_BL0942 — UART, request/response, 4800 baud 8N1.
 *     Sends a "read full packet" request (0x58|address then 0xAA) and
 *     parses a 23-byte response, exact layout per Shanghai Belling's own
 *     BL0942 datasheet (V1.10, section 3.2.5): byte0 = 0x55 header,
 *     bytes1-3 = I_RMS (u24), bytes4-6 = V_RMS (u24), bytes7-9 =
 *     I_FAST_RMS (unused), bytes10-12 = active power WATT (i24, signed),
 *     bytes13-15 = CF_CNT energy pulse counter (u24, monotonic, wraps at
 *     2^24), byte22 = checksum (sum of every preceding byte plus the
 *     request's first byte, XOR 0xFF — matches the datasheet's own
 *     "bitwise inverted" wording, which is equivalent for 8-bit values).
 *     Raw register values convert to real units by dividing by fixed
 *     reference constants (BL0942_UREF/IREF/PREF/EREF below) — these
 *     specific constants are sourced from ESPHome's bl0942 component,
 *     not independently re-derived from the datasheet's own pin-level
 *     millivolt formulas (which need a specific reference PCB's exact
 *     shunt/divider values to reduce to these numbers) — BL0942 is
 *     marketed as "calibration-free" for exactly this reason: these
 *     constants are tied to one common reference hardware design, not a
 *     universal constant of the chip silicon itself; recalibrate against
 *     a known load if your board's shunt/divider differs. FREQ (bytes
 *     16-17) is deliberately not decoded — no verified conversion
 *     formula for it was found, and this repo doesn't report values it
 *     can't source.
 *
 *   OUTLET_POWER_MONITOR_BL0937 / _HLW8012 / _CSE7759 — no communication
 *     protocol at all: two GPIO pins (CF, CF1) each output a pulse train
 *     whose *frequency* is proportional to a measurement — CF is always
 *     active power; CF1 is multiplexed between current RMS (SEL pin LOW)
 *     and voltage RMS (SEL pin HIGH) by a third GPIO this firmware
 *     drives. Frequency is measured by counting edges over a fixed
 *     window (see power_monitor_task() in this #if branch below).
 *     BL0937's own multiplier formula (Vref=1.218V) is independently
 *     confirmed against Shanghai Belling's own BL0937 datasheet (V1.02,
 *     section "CF、CF1 frequency", formulas F_CF/F_CFU/F_CFI with
 *     constants 1721506/15397/94638) — algebraically rearranging the
 *     datasheet's own F_CF = 1721506*V(V)*V(I)/Vref² (etc.) to solve for
 *     real-world power/voltage/current from a measured frequency
 *     reproduces PULSE_METER's power/voltage/current_multiplier formulas
 *     below exactly. HLW8012's own multiplier formula (Vref=2.43V,
 *     clock=3.579MHz) is likewise independently confirmed against
 *     Hiliwi Technology's own HLW8012 user manual (REV1.3, section 3.2,
 *     formulas F_CF/F_CFI/F_CFU) the same way. CSE7759 is assumed to
 *     share HLW8012's exact formula (both chips, both from Chipsea/
 *     compatible lineage) per ESPHome's own component comments, but
 *     that specific claim wasn't independently re-verified against a
 *     readable CSE7759 datasheet (attempted; the fetched PDF didn't
 *     yield usable text) — flagged as the one still-secondary-sourced
 *     detail in this family. PULSE_METER_VOLTAGE_DIVIDER/CURRENT_
 *     RESISTOR below are calibration constants tied to the specific
 *     resistors on whatever breakout board you're using (this part is
 *     genuinely board-specific, unlike BL0942/CSE7766's on-chip
 *     calibration) — the values here are common reference defaults, not
 *     a guarantee for your exact board; see the comment above them.
 *
 *   OUTLET_POWER_MONITOR_CSE7766 — UART, but *auto-report*, not
 *     request/response like BL0942: the chip transmits a full 24-byte
 *     packet unprompted, multiple times a second, so this firmware only
 *     ever reads, never writes. Frames use 8E1 (even parity), not 8N1.
 *     Packet layout and the Adj status byte's bit meanings are both per
 *     Chipsea's own CSE7766 User Manual (Rev.1.2, sections 3.4-3.7,
 *     tables 3-1/3-2/3-4 and the reference flowcharts in 3.7): byte0 =
 *     header1 (0xAA = not calibrated, 0x55 = normal, 0xFx = out-of-range
 *     or abnormal circuit/chip fault), byte1 = header2 (fixed 0x5A),
 *     bytes2-4/5-7 = voltage coefficient/cycle (u24 each, high byte
 *     first), bytes8-10/11-13 = current coefficient/cycle, bytes14-16/
 *     17-19 = power coefficient/cycle, byte20 = Adj (bits 6/5/4 = the
 *     voltage/current/power cycle just reported is a *complete*
 *     measurement cycle, not a partial one sent early because the real
 *     cycle is running long), bytes21-22 = CF pulse count (u16), byte23
 *     = checksum (sum of bytes 2-22). Unlike every other chip here, unit
 *     conversion doesn't use a fixed external reference constant — each
 *     measurement is simply coefficient/cycle (assuming your board
 *     matches CSE7766's own default delivery calibration: 1mOhm current
 *     shunt, 1Mohm voltage divider — see the datasheet's V1R/V2R note in
 *     section 3.5.2), using values read straight out of the packet (this
 *     chip stores its own factory calibration on-die and reports it
 *     every packet).
 *
 *   OUTLET_POWER_MONITOR_ADE7953 — the only I2C chip of the six, and the
 *     only one not from the BL09xx/HLW8012/CSE77xx pulse-or-UART family
 *     — a completely different vendor (Analog Devices) and register-
 *     based protocol, reusing the same driver/i2c_master.h new-style I2C
 *     API firmware/temperature-sensor's I2C sensors already use. Needs a
 *     documented "unlock" sequence (write 0xAD to register 0x00FE, then
 *     0x0030 to register 0x0120) before any config register write takes
 *     effect, then clearing the CONFIG register's (0x0102) lock bit.
 *     Reads VRMS (0x031C), IRMS channel A (0x031A), and instantaneous
 *     active power channel A (0x0312), each divided by a fixed constant
 *     (ADE7953_VOLTAGE_DIVISOR/CURRENT_DIVISOR/POWER_DIVISOR below).
 *     Sourced from ESPHome's ade7953_base component — flagged as the
 *     least-certain of the six drivers here: an attempt was made to
 *     verify this one against Analog Devices' own ADE7953 datasheet
 *     directly (unlike the other five, which all got at least one clean
 *     primary-source confirmation), but the fetched content only yielded
 *     partial, sometimes-inconsistent register data across several
 *     attempts, not a full independent confirmation. ADE7953's full
 *     register map is also considerably larger than what a simple
 *     plug-in power monitor needs (two current channels, harmonic
 *     analysis, no-load detection thresholds, and more) — only the small
 *     slice actually used here was extracted. Double check against
 *     Analog Devices' own datasheet before relying on this one. ADE7953
 *     also doesn't have a confidently-sourced cumulative-energy register
 *     formula in this repo's research, so (unlike the other five chips)
 *     its energy figure is derived by integrating polled power readings
 *     over elapsed time rather than from an on-chip energy accumulator —
 *     see ade7953's power_monitor_task() for exactly how.
 *
 * None of the six chips has been tested against real hardware in this
 * repo (no module of any of them was on hand when this was written) —
 * each implemented carefully from the sourced formulas/protocol above,
 * same standard as this repo's other unverified sensor drivers (SHT4x,
 * AHT20, DS18B20, BME280, LDR, BH1750), flagged as such here and in the
 * wizard rather than presented as equally proven as the hardware-tested
 * parts of this firmware.
 *
 * Cumulative energy accounting for the pulse-frequency chips (BL0937/
 * HLW8012/CSE7759) derives from first principles rather than a
 * separately-sourced formula: a metering chip's pulse output is defined
 * so that pulse *count* over any time window is proportional to energy
 * consumed in that window (power x time = energy, and pulse-count/time =
 * frequency, so count = frequency x time is already proportional to
 * power x time). That means each individual pulse represents a fixed,
 * constant amount of energy — computed once from each chip's own
 * power-conversion constant (divided by 3600 to go from watt-seconds to
 * watt-hours) — and cumulative energy is just that per-pulse constant
 * times the running pulse count, tallied by this firmware's own software
 * counter on the CF pin. BL0942 and CSE7766 instead expose their own
 * energy pulse counter directly (CF_CNT / CFm:CFl) — no separate
 * derivation needed there. ADE7953's approach is different again — see
 * its own paragraph above.
 *
 * Like firmware/light/'s OnOff attribute, ElectricalPowerMeasurement here
 * uses a genuinely different esp-matter integration pattern than every
 * other cluster in this repo: it's neither a plain ember attribute
 * (attribute::update()) nor the newer "code-driven cluster + registry-
 * lookup + SetMeasuredValue()" pattern firmware/contact-sensor and
 * firmware/temperature-sensor established. It uses a *Delegate* interface
 * (chip::app::Clusters::ElectricalPowerMeasurement::Delegate) — the
 * cluster pulls current values from delegate getters on demand rather
 * than the app pushing them into a generic store. This firmware's own
 * OutletElectricalPowerMeasurementDelegate below implements that
 * interface, adapted directly from esp-matter's own reference
 * implementation (examples/all_device_types_app/main/electrical_measurement/,
 * verified by reading it directly rather than guessing at the Delegate
 * interface's shape) rather than written from scratch. ElectricalEnergy-
 * Measurement, by contrast, ships a complete ready-made implementation in
 * esp-matter itself — driven entirely through free functions
 * (SetMeasurementAccuracy(), NotifyCumulativeEnergyMeasured(), ...) in
 * <data_model_provider/clusters/electrical_energy_measurement/integration.h>
 * with no custom Delegate needed at all, confirmed by reading that header
 * directly.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <esp_matter.h>
/* Deliberately no explicit #include for electrical_sensor_device.h (the
 * generated endpoint::electrical_sensor header) — <esp_matter.h> already
 * transitively provides it (confirmed by checking esp-matter's own
 * examples/all_device_types_app, which calls endpoint::electrical_sensor
 * ::create() without any such include either); adding it explicitly here
 * caused every one of its own transitive cluster headers (descriptor.h,
 * power_topology.h, electrical_power_measurement.h, electrical_energy_
 * measurement.h) to be reachable via two different search-path spellings
 * at once and get parsed twice in the same translation unit — real
 * "redefinition"/"conflicting declaration" errors, not a hypothetical
 * risk, caught by actually building and reading them. */
#include <data_model_provider/clusters/electrical_power_measurement/integration.h>
#include <data_model_provider/clusters/electrical_energy_measurement/integration.h>
#include <app/reporting/reporting.h> /* MatterReportingAttributeChangeCallback() */

static const char *TAG = "matter_outlet";

/* --- Output type — see the header comment above for why this is a real
 * functional difference (relay polarity), not cosmetic. --- */
#define OUTLET_OUTPUT_LED 1
#define OUTLET_OUTPUT_RELAY 2
#define OUTLET_OUTPUT_TYPE OUTLET_OUTPUT_RELAY

#if OUTLET_OUTPUT_TYPE == OUTLET_OUTPUT_RELAY
#define OUTLET_OUTPUT_ACTIVE_LOW 1
#else
#define OUTLET_OUTPUT_ACTIVE_LOW 0
#endif

/* Change this to the GPIO your output (LED / relay) is wired to — this is
 * the actual on/off state, driven by the Matter OnOff attribute regardless
 * of whether the change came from the physical button or a remote
 * controller. GPIO 2 is commonly the onboard/user LED on classic ESP32
 * (WROOM-32) devkits. Adjust to match your board. */
#define OUTLET_GPIO GPIO_NUM_2

/* Change this to the GPIO your button is wired to. Reference wiring is a
 * breadboard pushbutton: GND -> button -> GPIO (no external resistor
 * needed — the internal pull-up below keeps the pin HIGH until the button
 * pulls it to GND on press). GPIO 4 is a plain, unreserved GPIO on classic
 * ESP32 (WROOM-32) — deliberately NOT the onboard BOOT/PROG button
 * (GPIO 0): that pin is also used for boot-mode selection and turned out
 * unreliable as an external input on the board this was tested against
 * (see CLAUDE.md's open next steps). Adjust to match your board if you
 * wire it elsewhere. */
#define OUTLET_BUTTON_GPIO GPIO_NUM_4

/* Separate LED for the Matter "Identify" cluster — blinks so you can
 * physically find this device when a controller asks it to identify
 * itself, independent of its own on/off state. Shares OUTLET_GPIO by
 * default (matches this repo's other reference wiring, which only has one
 * LED wired up) — the two will fight over that LED during an identify
 * request, which is harmless and purely cosmetic. Wire a second LED to a
 * free GPIO and change this if you want them independent. Also blinks for
 * an Identify request sent to the (optional) power-monitoring endpoint —
 * one physical LED serves both endpoints on this node, same as
 * firmware/temperature-sensor's shared identify LED across its two
 * endpoints. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Optional separate on/off status indicator — see the header comment above
 * for how this differs from IDENTIFY_LED_GPIO. GPIO_NUM_NC ("not
 * connected") disables it; not every board has this LED wired up. */
#define OUTLET_STATUS_LED_GPIO GPIO_NUM_NC

/* Optional RGB status LED — a genuinely different, coexisting feature from
 * OUTLET_STATUS_LED_GPIO above (single-color, mirrors on/off state): this
 * one shows Matter's own defined commissioning and Identify states with
 * color + a blink/breathe pattern engine. Off by default (GPIO_NUM_NC on
 * all three). GPIO 32/33/14 avoid every one of this file's own other
 * defaults, including its 6 power-monitor chips' pins (which use
 * 16/17/21/22/25/26/27 variously — see the header comment above). See
 * firmware/light/main/app_main.cpp's header comment for the full state
 * list and its exact sourcing, not repeated here. */
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

/* --- Power monitoring — see the header comment above for the full
 * protocol/formula explanation and sourcing for each chip. Six real
 * chips are supported, falling into three genuinely different protocol
 * families (pulse-frequency GPIO, request/response UART, auto-report
 * UART, and I2C register access) — see each block below for exactly
 * which family a given chip belongs to. --- */
#define OUTLET_POWER_MONITOR_NONE 0
#define OUTLET_POWER_MONITOR_BL0942 1
#define OUTLET_POWER_MONITOR_BL0937 2
#define OUTLET_POWER_MONITOR_HLW8012 3
#define OUTLET_POWER_MONITOR_CSE7759 4
#define OUTLET_POWER_MONITOR_CSE7766 5
#define OUTLET_POWER_MONITOR_ADE7953 6
#define OUTLET_POWER_MONITOR OUTLET_POWER_MONITOR_NONE

#if OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_BL0942
/* UART, request/response: this firmware sends a "read full packet"
 * request and parses the reply. UART pins — GPIO16/17 are ESP-IDF's own
 * conventional UART2 default pins on classic ESP32, unused elsewhere in
 * this firmware. Adjust to match your board. */
#define BL0942_UART_PORT UART_NUM_1
#define BL0942_UART_RX_GPIO GPIO_NUM_16
#define BL0942_UART_TX_GPIO GPIO_NUM_17
#define BL0942_UART_BAUD_RATE 4800
#define BL0942_DEVICE_ADDRESS 0 /* default; BL0942 supports addressing multiple chips on one bus, unused here */
#define BL0942_READ_COMMAND 0x58
#define BL0942_FULL_PACKET 0xAA
#define BL0942_RESPONSE_LEN 23
/* Reference constants — raw register value / constant = real unit
 * (V, A, W, and pulses-per-Wh for energy). Sourced from ESPHome's bl0942
 * component defaults, not derived here — see the header comment above. */
#define BL0942_UREF 15883.34116
#define BL0942_IREF 251065.6814
#define BL0942_PREF 623.0270705
#define BL0942_EREF 5347.484240
#define BL0942_POLL_INTERVAL_MS 10000

#elif OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_BL0937 || \
      OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_HLW8012 || \
      OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_CSE7759
/* Pulse-frequency family, no communication protocol at all: CF/CF1 pins
 * output pulse trains whose *frequency* is proportional to a
 * measurement, multiplexed by a SEL pin this firmware drives — see the
 * header comment above for the full explanation. BL0937 and HLW8012/
 * CSE7759 share this exact pin protocol but use different reference
 * voltages and different multiplier formulas (confirmed by reading
 * ESPHome's hlw8012 component, which explicitly branches on chip model
 * for this reason) — HLW8012 and CSE7759 use the *same* formula as each
 * other, just different from BL0937's, so they share one branch below.
 * SEL/CF/CF1 pins — free general-purpose GPIOs on classic ESP32, unused
 * elsewhere in this firmware. Adjust to match your board. */
#define PULSE_METER_SEL_GPIO GPIO_NUM_25
#define PULSE_METER_CF_GPIO GPIO_NUM_26  /* active power pulse output */
#define PULSE_METER_CF1_GPIO GPIO_NUM_27 /* current/voltage RMS pulse output, multiplexed by SEL */
/* Calibration constants tied to the specific resistors on your breakout
 * board — these are common reference defaults (matching typical
 * Sonoff-style modules), NOT a guarantee for your exact board.
 * voltage_divider: ratio of the mains-side voltage divider feeding V2N.
 * current_resistor: value (ohms) of the current-sense shunt resistor. */
#define PULSE_METER_VOLTAGE_DIVIDER 1981.0f
#define PULSE_METER_CURRENT_RESISTOR 0.001f
/* How long each SEL window (current or voltage measurement) lasts.
 * Power is measured every window (SEL doesn't affect CF); current and
 * voltage alternate. */
#define PULSE_METER_WINDOW_MS 2000

#if OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_BL0937
/* BL0937's own fixed internal reference voltage (not board-specific —
 * this one really is a constant of the chip itself, per its datasheet). */
#define PULSE_METER_REFERENCE_VOLTAGE 1.218f
#else /* HLW8012 / CSE7759 */
/* HLW8012/CSE7759's own fixed internal reference voltage and clock
 * frequency (chip constants, not board-specific) — sourced from
 * ESPHome's hlw8012 component, same as BL0937's constant above but this
 * family's own documented values. */
#define PULSE_METER_REFERENCE_VOLTAGE 2.43f
#define HLW8012_CLOCK_FREQUENCY 3579000.0f
#endif

#elif OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_CSE7766
/* UART, auto-report: unlike BL0942, this chip transmits a full
 * measurement packet unprompted, several times a second, without ever
 * being sent a request — this firmware only ever reads. Also unlike
 * every other UART sensor in this repo, CSE7766's frames use 8E1 (even
 * parity), not 8N1 — confirmed via both its own datasheet and ESPHome's
 * cse7766 component, which configures it the same way. UART pins —
 * reusing the same GPIO16/17 pair BL0942 uses (never both wired at
 * once, since only one power-monitor chip is selected at a time). */
#define CSE7766_UART_PORT UART_NUM_1
#define CSE7766_UART_RX_GPIO GPIO_NUM_16
#define CSE7766_UART_TX_GPIO GPIO_NUM_17 /* unused — CSE7766 only transmits, this firmware only reads */
#define CSE7766_UART_BAUD_RATE 4800
#define CSE7766_PACKET_LEN 24

#elif OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_ADE7953
/* I2C register access — the only one of these six chips that isn't a
 * pulse-frequency or UART design; reused via the same driver/i2c_master.h
 * new-style I2C API firmware/temperature-sensor's I2C sensors already
 * use. Register addresses/init sequence/conversion divisors sourced from
 * ESPHome's ade7953_base component; flagged as the least-certain of the
 * six drivers here (ADE7953's register map is considerably larger than
 * what a plug-in power monitor needs, and only the small slice actually
 * used — voltage, current, instantaneous active power on channel A — was
 * extracted and cross-checked, not the whole datasheet) — see the header
 * comment above. Default I2C pins match firmware/temperature-sensor's
 * I2C default (GPIO 21/22). */
#define ADE7953_I2C_ADDR 0x38
#define ADE7953_SDA_GPIO GPIO_NUM_21
#define ADE7953_SCL_GPIO GPIO_NUM_22
#define ADE7953_I2C_FREQ_HZ 100000
#define ADE7953_POLL_INTERVAL_MS 10000
/* Register addresses, from ESPHome's ade7953_base component. */
#define ADE7953_REG_UNLOCK_8 0x00FE   /* write 0xAD to unlock config access */
#define ADE7953_REG_UNLOCK_16 0x0120  /* write 0x0030 right after, completes the unlock sequence */
#define ADE7953_REG_CONFIG_16 0x0102  /* bit 15 (0x8000) is a config lock bit; default reset value 0x8004 */
#define ADE7953_REG_VRMS_32 0x031C
#define ADE7953_REG_IRMS_A_32 0x031A
#define ADE7953_REG_AWATT_A_32 0x0312 /* instantaneous active power, channel A */
/* Conversion divisors: raw register value / divisor = real unit. */
#define ADE7953_VOLTAGE_DIVISOR 26000.0f
#define ADE7953_CURRENT_DIVISOR 100000.0f
#define ADE7953_POWER_DIVISOR 154.0f
#endif

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t outlet_endpoint_id = 0;
static QueueHandle_t button_evt_queue = NULL;
static esp_timer_handle_t identify_led_timer = NULL;
/* Mirrors the Matter OnOff attribute's current value — kept in sync solely
 * by app_attribute_update_cb() below, which fires for every change to that
 * attribute regardless of source (our own button, or a remote write from a
 * controller). button_task() reads this to know what to toggle to; nothing
 * else writes it directly, so there's exactly one source of truth. */
static bool outlet_state = false;

/* Set once in app_main() by setup_status_led(), below — GPIO_NUM_NC can't be
 * checked with #if (it's a gpio_num_t enumerator, not a preprocessor
 * macro), so whether the status LED is wired up at all is a runtime check,
 * not a compile-time one like OUTLET_POWER_MONITOR's #if branches. */
static bool status_led_enabled = false;

static void set_output(bool on)
{
#if OUTLET_OUTPUT_ACTIVE_LOW
    gpio_set_level(OUTLET_GPIO, on ? 0 : 1);
#else
    gpio_set_level(OUTLET_GPIO, on ? 1 : 0);
#endif
    if (status_led_enabled) {
        gpio_set_level(OUTLET_STATUS_LED_GPIO, on ? 1 : 0);
    }
}

/* --- RGB status LED pattern engine ---------------------------------------
 * A second, independent LED feature from OUTLET_STATUS_LED_GPIO above —
 * see the header comment on STATUS_LED_RED/GREEN/BLUE_GPIO for what it
 * shows and why. Prefixed `rgb_status_led_*` here specifically (unlike
 * every other device type's identical `status_led_*` naming) to avoid
 * colliding with this file's own pre-existing `status_led_enabled`/
 * `setup_status_led()` for OUTLET_STATUS_LED_GPIO above. */
typedef enum {
    RGB_STATUS_LED_PATTERN_OFF,
    RGB_STATUS_LED_PATTERN_SOLID,
    RGB_STATUS_LED_PATTERN_BLINK,
    RGB_STATUS_LED_PATTERN_BREATHE,
} rgb_status_led_pattern_t;

static bool rgb_status_led_enabled = false;
static esp_timer_handle_t rgb_status_led_timer = NULL;
static uint8_t rgb_status_led_r = 0, rgb_status_led_g = 0, rgb_status_led_b = 0;
static rgb_status_led_pattern_t rgb_status_led_pattern = RGB_STATUS_LED_PATTERN_OFF;
static uint32_t rgb_status_led_period_ms = 1000;
static uint32_t rgb_status_led_elapsed_ms = 0;
static uint32_t rgb_status_led_duration_ms = 0;

static void rgb_status_led_set(uint8_t r, uint8_t g, uint8_t b, rgb_status_led_pattern_t pattern,
                                uint32_t period_ms, uint32_t duration_ms)
{
    if (!rgb_status_led_enabled) {
        return;
    }
    rgb_status_led_r = r;
    rgb_status_led_g = g;
    rgb_status_led_b = b;
    rgb_status_led_pattern = pattern;
    rgb_status_led_period_ms = period_ms > 0 ? period_ms : 1000;
    rgb_status_led_duration_ms = duration_ms;
    rgb_status_led_elapsed_ms = 0;
}

static void rgb_status_led_off(void)
{
    rgb_status_led_set(0, 0, 0, RGB_STATUS_LED_PATTERN_OFF, 1000, 0);
}

static void rgb_status_led_tick_cb(void *arg)
{
    if (!rgb_status_led_enabled) {
        return;
    }
    rgb_status_led_elapsed_ms += STATUS_LED_TICK_MS;
    if (rgb_status_led_duration_ms > 0 && rgb_status_led_elapsed_ms >= rgb_status_led_duration_ms) {
        rgb_status_led_off();
    }

    float phase = fmodf((float)rgb_status_led_elapsed_ms, (float)rgb_status_led_period_ms) / (float)rgb_status_led_period_ms;
    float brightness;
    switch (rgb_status_led_pattern) {
    case RGB_STATUS_LED_PATTERN_SOLID:
        brightness = 1.0f;
        break;
    case RGB_STATUS_LED_PATTERN_BLINK:
        brightness = (phase < 0.5f) ? 1.0f : 0.0f;
        break;
    case RGB_STATUS_LED_PATTERN_BREATHE:
        brightness = (sinf(phase * 2.0f * STATUS_LED_PI - STATUS_LED_PI / 2.0f) + 1.0f) / 2.0f;
        break;
    case RGB_STATUS_LED_PATTERN_OFF:
    default:
        brightness = 0.0f;
        break;
    }

    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL, (uint32_t)(rgb_status_led_r * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_RED_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL, (uint32_t)(rgb_status_led_g * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_GREEN_CHANNEL);
    ledc_set_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL, (uint32_t)(rgb_status_led_b * brightness));
    ledc_update_duty(STATUS_LED_LEDC_MODE, STATUS_LED_LEDC_BLUE_CHANNEL);
}

/* Configures OUTLET_STATUS_LED_GPIO as an output if it's actually been set
 * to a real pin (GPIO_NUM_NC, the shipped default, means "not wired up" —
 * skip entirely). Call once from app_main(), before the first set_output(). */
static void setup_status_led(void)
{
    if (OUTLET_STATUS_LED_GPIO == GPIO_NUM_NC) {
        return;
    }
    gpio_config_t status_led_conf = {};
    status_led_conf.pin_bit_mask = (1ULL << OUTLET_STATUS_LED_GPIO);
    status_led_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&status_led_conf);
    status_led_enabled = true;
    gpio_set_level(OUTLET_STATUS_LED_GPIO, 0);
}

/* Toggles the identify LED each time the timer fires — the actual blink. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
}

/* Runs in interrupt context — do the minimum: hand the event to a task. */
static void IRAM_ATTR button_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)(uintptr_t)arg;
    xQueueSendFromISR(button_evt_queue, &gpio_num, NULL);
}

/* Debounces the button and, on a confirmed press, toggles the Matter OnOff
 * attribute via attribute::update() — the same server-side call
 * firmware/light/ uses, which is why this shows up as a real controllable
 * tile in Apple/Google Home (unlike firmware/switch/'s client-only
 * approach). Debounce logic itself is identical to firmware/switch/'s
 * button — see the comments there for the reasoning (contact bounce,
 * why continuous-low sampling instead of one fixed delay, why the queue
 * gets reset after each cycle). */
static void button_task(void *arg)
{
    uint32_t io_num;

    for (;;) {
        if (xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Edge detected on GPIO %lu — debouncing", (unsigned long)io_num);

        bool confirmed = true;
        char samples[9] = {0};
        for (int i = 0; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            int level = gpio_get_level((gpio_num_t)io_num);
            samples[i] = level ? 'H' : 'L';
            if (level != 0) {
                confirmed = false;
            }
        }
        ESP_LOGI(TAG, "Samples (5ms apart): %s (%s)", samples, confirmed ? "ALL LOW" : "mixed/HIGH");
        if (!confirmed) {
            ESP_LOGI(TAG, "Debounce rejected — not continuously held low");
            xQueueReset(button_evt_queue);
            continue;
        }

        bool new_state = !outlet_state;
        ESP_LOGI(TAG, "Button pressed — turning outlet %s", new_state ? "ON" : "OFF");

        esp_matter_attr_val_t val = esp_matter_bool(new_state);
        attribute::update(outlet_endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);

        /* Wait for release before re-arming, so one press = one toggle. */
        while (gpio_get_level((gpio_num_t)io_num) == 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        xQueueReset(button_evt_queue);
    }
}

/* Lifecycle events from the Matter stack (commissioning, connectivity, ...). */
static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEAdvertisingChange:
        if (event->CHIPoBLEAdvertisingChange.Result == chip::DeviceLayer::kActivity_Started) {
            rgb_status_led_set(0, 255, 0, RGB_STATUS_LED_PATTERN_BREATHE, 2000, 0); /* Setup mode */
        }
        break;
    case chip::DeviceLayer::DeviceEventType::kSecureSessionEstablished:
        rgb_status_led_set(255, 255, 0, RGB_STATUS_LED_PATTERN_BREATHE, 1000, 0); /* Setup started */
        break;
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete — device is now paired");
        rgb_status_led_off(); /* Setup complete */
        break;
    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        rgb_status_led_set(255, 0, 0, RGB_STATUS_LED_PATTERN_SOLID, 1000, 0); /* Setup failed */
        break;
    default:
        break;
    }
}

/* Called whenever the OnOff attribute changes — from our own button
 * (attribute::update() above) or a remote controller's write/command. This
 * is the single place that drives the physical output and the local state
 * mirror, so both stay correct no matter which side triggered the change. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == attribute::PRE_UPDATE && endpoint_id == outlet_endpoint_id &&
        cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        outlet_state = val->val.b;
        set_output(outlet_state);
        ESP_LOGI(TAG, "Outlet turned %s", outlet_state ? "ON" : "OFF");
    }
    return ESP_OK;
}

/* Called when a controller asks the device to "identify" itself — starts
 * or stops the identify LED blinking accordingly. Fires for either
 * endpoint on this node (outlet or the optional power-monitoring one) —
 * see IDENTIFY_LED_GPIO's comment above for why one shared LED for both
 * is fine. */
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    switch (type) {
    case identification::START:
        ESP_LOGI(TAG, "Identify started on endpoint %u", endpoint_id);
        esp_timer_start_periodic(identify_led_timer, IDENTIFY_BLINK_INTERVAL_MS * 1000);
        rgb_status_led_set(255, 0, 0, RGB_STATUS_LED_PATTERN_BLINK, 1000, 0);
        break;
    case identification::STOP:
        ESP_LOGI(TAG, "Identify stopped on endpoint %u", endpoint_id);
        esp_timer_stop(identify_led_timer);
        set_output(outlet_state);
        rgb_status_led_off();
        break;
    case identification::EFFECT:
        ESP_LOGI(TAG, "Identify effect %u (variant %u) on endpoint %u",
                 effect_id, effect_variant, endpoint_id);
        if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBlink)) {
            rgb_status_led_set(255, 255, 255, RGB_STATUS_LED_PATTERN_BLINK, 1000, 1000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kBreathe)) {
            rgb_status_led_set(255, 255, 255, RGB_STATUS_LED_PATTERN_BREATHE, 1000, 15000);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kOkay)) {
            rgb_status_led_set(0, 255, 0, RGB_STATUS_LED_PATTERN_BLINK, 2000, 0);
        } else if (effect_id == chip::to_underlying(Identify::EffectIdentifierEnum::kChannelChange)) {
            rgb_status_led_set(255, 255, 0, RGB_STATUS_LED_PATTERN_BLINK, 16000, 0);
        } else {
            rgb_status_led_off();
        }
        break;
    }
    return ESP_OK;
}

#if OUTLET_POWER_MONITOR != OUTLET_POWER_MONITOR_NONE
/* ======================================================================
 * Power monitoring — ElectricalPowerMeasurement + ElectricalEnergyMeasurement
 * on a second Matter endpoint. See the file header comment for the full
 * explanation of both chips' protocols and why the Matter integration
 * pattern here (a hand-implemented Delegate for power, ready-made free
 * functions for energy) differs from every other cluster in this repo.
 * ====================================================================== */

static uint16_t power_endpoint_id = 0;
static int64_t cumulative_energy_mwh = 0; /* accumulated since boot; not persisted across reboots */

/* Adapted from esp-matter's own reference implementation
 * (examples/all_device_types_app/main/electrical_measurement/) — trimmed
 * to only the measurement types this firmware actually reports
 * (ActivePower, RMSVoltage, RMSCurrent), since neither BL0942 nor BL0937
 * give this firmware verified data for reactive/apparent power, power
 * factor, frequency, or per-harmonic measurements. */
namespace chip { namespace app { namespace Clusters { namespace ElectricalPowerMeasurement {

class OutletPowerDelegate : public Delegate {
public:
    PowerModeEnum GetPowerMode() override { return PowerModeEnum::kAc; }
    uint8_t GetNumberOfMeasurementTypes() override { return 3; } /* ActivePower, RMSVoltage, RMSCurrent */

    CHIP_ERROR StartAccuracyRead() override { return CHIP_NO_ERROR; }
    CHIP_ERROR GetAccuracyByIndex(uint8_t index, Structs::MeasurementAccuracyStruct::Type &accuracy) override
    {
        /* One reasonably wide accuracy range per measurement type — good
         * enough to be spec-valid without the multi-tier ranges
         * esp-matter's own reference example uses (that level of detail
         * needs a certified meter's datasheet, which neither BL0942 nor
         * BL0937's own datasheet gives as a simple single number). */
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

    /* Setters — same change-detection + MatterReportingAttributeChangeCallback
     * pattern as esp-matter's own reference delegate, so subscribed
     * controllers get live updates, not just polled reads. */
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

static chip::app::Clusters::ElectricalPowerMeasurement::OutletPowerDelegate power_delegate;
static chip::app::Clusters::ElectricalPowerMeasurement::Instance *power_instance = NULL;

/* Reports one new set of readings: updates the pull-based power delegate
 * (live, every call) and accumulates + reports cumulative energy (at most
 * once a second, per spec — see ElectricalEnergyMeasurementCluster's own
 * kMinReportInterval — so this is safe to call more often than that). */
static void report_power(double watts, double volts, double amps, int64_t delta_energy_mwh)
{
    power_delegate.SetActivePower(chip::app::DataModel::MakeNullable((int64_t)(watts * 1000.0)));
    power_delegate.SetRMSVoltage(chip::app::DataModel::MakeNullable((int64_t)(volts * 1000.0)));
    power_delegate.SetRMSCurrent(chip::app::DataModel::MakeNullable((int64_t)(amps * 1000.0)));

    if (delta_energy_mwh > 0) {
        cumulative_energy_mwh += delta_energy_mwh;
    }

    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::EnergyMeasurementStruct::Type imported = {};
    imported.energy = cumulative_energy_mwh;
    imported.endSystime = chip::MakeOptional(static_cast<uint64_t>(
        chip::System::SystemClock().GetMonotonicTimestamp().count()));
    chip::app::DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> imported_nullable(imported);
    chip::app::DataModel::Nullable<Structs::EnergyMeasurementStruct::Type> exported_nullable; /* no export/reverse metering */
    NotifyCumulativeEnergyMeasured(power_endpoint_id, imported_nullable, exported_nullable);

    ESP_LOGI(TAG, "Power: %.1f W, %.1f V, %.3f A — cumulative %.3f kWh",
             watts, volts, amps, (double)cumulative_energy_mwh / 1000000.0);
}

#if OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_BL0942

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

/* Reads one full packet from the BL0942, validates its checksum, and
 * converts it to real units. See the file header comment for the exact
 * packet layout and where the reference constants come from. */
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

    /* Packet layout per Shanghai Belling's own BL0942 datasheet (V1.10,
     * section 3.2.5 "Packet Reading Mode", page 26) — CURRENT comes
     * before VOLTAGE, and WATT/CF_CNT sit at bytes 10-15, not 7-12 as an
     * earlier, secondary-sourced draft of this driver had it (caught by
     * checking the original datasheet directly, not by inspection):
     *   byte 0        = 0x55 header
     *   bytes 1-3     = I_RMS (u24)
     *   bytes 4-6     = V_RMS (u24)
     *   bytes 7-9     = I_FAST_RMS (u24) — over-current-only, not used here
     *   bytes 10-12   = WATT (i24, signed — bit 23 is the sign)
     *   bytes 13-15   = CF_CNT (u24) — energy pulse counter
     *   bytes 16-17   = FREQ, byte 18 = 0x00
     *   bytes 19-21   = STATUS + padding
     *   byte 22       = CHECKSUM */
    uint32_t i_rms_raw = ((uint32_t)resp[3] << 16) | ((uint32_t)resp[2] << 8) | resp[1];
    uint32_t v_rms_raw = ((uint32_t)resp[6] << 16) | ((uint32_t)resp[5] << 8) | resp[4];
    /* Active power is signed 24-bit — sign-extend from bit 23, per the
     * datasheet's own WATT register description (section 2.2). */
    int32_t watt_raw = ((int32_t)resp[12] << 16) | ((int32_t)resp[11] << 8) | resp[10];
    if (watt_raw & 0x800000) {
        watt_raw |= (int32_t)0xFF000000;
    }
    uint32_t cf_cnt_raw = ((uint32_t)resp[15] << 16) | ((uint32_t)resp[14] << 8) | resp[13];

    *out_volts = (double)v_rms_raw / BL0942_UREF;
    *out_amps = (double)i_rms_raw / BL0942_IREF;
    *out_watts = (double)watt_raw / BL0942_PREF;

    /* cf_cnt is a monotonic 24-bit counter that wraps — track the delta
     * since the last read, handling wraparound, rather than the raw
     * count itself. */
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

#elif OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_BL0937 || \
      OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_HLW8012 || \
      OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_CSE7759

static volatile uint32_t pulse_meter_cf_edges = 0;  /* power pulses since last read */
static volatile uint32_t pulse_meter_cf1_edges = 0; /* current/voltage pulses since last SEL window */

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
    gpio_set_level(PULSE_METER_SEL_GPIO, 0); /* start in current-measurement mode */

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

/* Alternates the SEL pin between current and voltage measurement windows
 * every PULSE_METER_WINDOW_MS, measuring CF (power) every window
 * regardless of SEL. Shared by BL0937/HLW8012/CSE7759 — only the
 * multiplier formula differs between BL0937 and the HLW8012/CSE7759
 * pair, computed once below via the #if picking PULSE_METER_REFERENCE_
 * VOLTAGE and (for HLW8012/CSE7759 only) HLW8012_CLOCK_FREQUENCY set
 * further up. See the file header comment for where both formulas come
 * from. */
static void power_monitor_task(void *arg)
{
    if (!pulse_meter_gpio_setup()) {
        ESP_LOGE(TAG, "Pulse meter GPIO setup failed — power monitoring disabled");
        vTaskDelete(NULL);
        return;
    }

#if OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_BL0937
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
    /* Each single CF pulse represents this many watt-hours — see the file
     * header comment ("Cumulative energy accounting...") for the derivation. */
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

        /* Flip SEL for the next window. */
        gpio_set_level(PULSE_METER_SEL_GPIO, measuring_voltage ? 0 : 1);
    }
}

#elif OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_CSE7766

static bool cse7766_uart_setup(void)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = CSE7766_UART_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_EVEN; /* unlike BL0942/CSE7766's siblings, this chip uses 8E1 */
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

/* CSE7766 transmits unprompted — this just waits for and parses whatever
 * packet arrives next, no request to send. Field layout, checksum range,
 * and the meaning of the Adj byte's bits are all taken directly from
 * Chipsea's own CSE7766 User Manual (Rev.1.2, sections 3.4-3.7) — an
 * earlier draft of this driver had the field order guessed from a
 * secondary source and got it wrong (voltage/current/power/CF at the
 * wrong byte offsets entirely); this version was checked against the
 * primary datasheet's own byte-numbered table (3-1/3-2) and flowcharts
 * (Figures 5-7) instead. Unlike BL0942, the coefficient values needed
 * for unit conversion are read straight out of each packet
 * (factory-calibrated per chip), not a fixed external reference
 * constant — and unlike BL0937/HLW8012/CSE7759, no board-specific
 * voltage_divider/current_resistor constants are needed either, PROVIDED
 * your board matches CSE7766's own default delivery calibration
 * (1mOhm current-sense resistor, 1Mohm voltage divider — see the
 * datasheet's "V1R"/"V2R" note in section 3.5.2, both =1 for that
 * reference design; a different resistor value scales the result
 * linearly and would need an explicit V1R/V2R correction this driver
 * doesn't implement). */
static bool cse7766_read(double *out_watts, double *out_volts, double *out_amps, int64_t *out_delta_energy_mwh)
{
    uint8_t pkt[CSE7766_PACKET_LEN];
    int read = uart_read_bytes(CSE7766_UART_PORT, pkt, sizeof(pkt), pdMS_TO_TICKS(2000));
    if (read != CSE7766_PACKET_LEN) {
        ESP_LOGW(TAG, "CSE7766 read timed out or short (%d/%d bytes)", read, CSE7766_PACKET_LEN);
        return false;
    }
    /* Packet header 1 (byte 0): 0xAA means the chip hasn't been
     * calibrated (shouldn't happen on a chip that's already shipped —
     * CSE7766 is calibrated once before delivery — but checked anyway
     * rather than assumed); 0xFx (top nibble 1111) means an out-of-range
     * measurement cycle or an abnormal external circuit/chip fault,
     * with the low nibble's bits identifying which (datasheet Table
     * 3-3); the normal case is 0x55. Anything else isn't decoded, just
     * rejected. */
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

    /* Adj (byte 20) bits 6/5/4 don't mean "valid/invalid" (an earlier
     * draft of this driver assumed that) — per the datasheet they flag
     * whether the voltage/current/power cycle just reported is a
     * *complete* measurement cycle or a partial one sent early because
     * the real cycle is running long (over ~1s; CSE7766 sends a partial
     * reading rather than make the caller wait). This driver still
     * skips incomplete-cycle readings, same as the datasheet's own
     * reference flowcharts (Figures 5-7) do, just for the accurate
     * reason: waiting for a settled reading, not discarding bad data. */
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

    /* Each CF pulse represents power_coeff / 1e6 / 3600 Wh — same
     * first-principles derivation as the pulse-frequency chips above,
     * using this packet's own power_coeff instead of a fixed constant
     * (this chip reports its own calibrated power_coeff every packet,
     * per the datasheet's own power formula in section 3.5.2). Per the
     * datasheet's "Special attention" note in 3.6.1, a CFl of exactly 0
     * means CF pulses/Adj.7 are meaningless this packet — harmless here
     * either way since it just contributes 0 pulses this round. */
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
        /* No delay here — CSE7766 sends multiple packets a second
         * unprompted; cse7766_read() naturally paces this loop by
         * blocking on uart_read_bytes() until the next one arrives. */
    }
}

#elif OUTLET_POWER_MONITOR == OUTLET_POWER_MONITOR_ADE7953

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

/* Register addresses/init sequence/divisors sourced from ESPHome's
 * ade7953_base component — see the header comment above ADE7953_I2C_ADDR
 * for why this driver is flagged as the least-certain of the six. */
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

    /* Documented ADE7953 unlock sequence (per Analog Devices' own app
     * notes for this chip) — required before any other config register
     * write takes effect. */
    if (!ade7953_write8(ADE7953_REG_UNLOCK_8, 0xAD)) {
        return false;
    }
    if (!ade7953_write16(ADE7953_REG_UNLOCK_16, 0x0030)) {
        return false;
    }
    /* Clear the CONFIG register's lock bit (0x8000), keeping the rest of
     * its default reset value (0x8004) so measurement continues normally. */
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
            /* ADE7953's own energy accumulation registers weren't
             * confidently sourced (see the header comment) — cumulative
             * energy here is derived the same first-principles way as
             * the pulse-frequency chips above, integrating this
             * driver's own poll interval instead of a pulse count. */
            int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);
            int64_t elapsed_ms = last_report_ms == 0 ? 0 : (now_ms - last_report_ms);
            last_report_ms = now_ms;
            int64_t delta_energy_mwh = (int64_t)(watts * ((double)elapsed_ms / 3600000.0) * 1000.0);
            report_power(watts, volts, amps, delta_energy_mwh);
        }
        vTaskDelay(pdMS_TO_TICKS(ADE7953_POLL_INTERVAL_MS));
    }
}

#endif /* OUTLET_POWER_MONITOR == ... */

/* Creates the second endpoint (esp-matter's `electrical_sensor` device
 * type) and wires up both measurement clusters — energy via esp-matter's
 * own ready-made free-function API, power via this file's own Delegate
 * (see the file header comment for why the two clusters need different
 * integration approaches in this SDK version). */
static bool power_monitoring_setup(node_t *node)
{
    endpoint::electrical_sensor::config_t sensor_config;
    sensor_config.with_electrical_energy_measurement(); /* enables the ready-made energy path only */
    endpoint_t *sensor_endpoint = endpoint::electrical_sensor::create(node, &sensor_config, ENDPOINT_FLAG_NONE, NULL);
    if (!sensor_endpoint) {
        ESP_LOGE(TAG, "Failed to create electrical sensor endpoint");
        return false;
    }
    power_endpoint_id = endpoint::get_id(sensor_endpoint);
    ESP_LOGI(TAG, "Electrical sensor endpoint id: %u", power_endpoint_id);

    using namespace chip::app::Clusters::ElectricalEnergyMeasurement;
    Structs::MeasurementAccuracyStruct::Type energy_accuracy = {};
    energy_accuracy.measurementType = MeasurementTypeEnum::kElectricalEnergy;
    energy_accuracy.measured = true;
    energy_accuracy.minMeasuredValue = 0;
    energy_accuracy.maxMeasuredValue = 1000000000000000LL;
    SetMeasurementAccuracy(power_endpoint_id, energy_accuracy);

    /* Both ElectricalEnergyMeasurement and ElectricalPowerMeasurement
     * declare their own `Feature` enum — the `using namespace` for energy
     * above is still in scope here (it isn't block-scoped to just the
     * accuracy code above it), so an unqualified `Feature` is genuinely
     * ambiguous. Qualified explicitly below instead of relying on a
     * third `using namespace` to "win". */
    power_delegate.SetEndpointId(power_endpoint_id);
    chip::BitMask<ElectricalPowerMeasurement::Feature> features(ElectricalPowerMeasurement::Feature::kAlternatingCurrent);
    chip::BitMask<ElectricalPowerMeasurement::OptionalAttributes> optional_attrs(
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSVoltage,
        ElectricalPowerMeasurement::OptionalAttributes::kOptionalAttributeRMSCurrent);
    power_instance = new ElectricalPowerMeasurement::Instance(power_endpoint_id, power_delegate, features, optional_attrs);
    CHIP_ERROR err = power_instance->Init();
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ElectricalPowerMeasurement Instance::Init failed: %" CHIP_ERROR_FORMAT, err.Format());
        return false;
    }

    return true;
}

#endif /* OUTLET_POWER_MONITOR != OUTLET_POWER_MONITOR_NONE */


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

    /* 2. Configure the output (LED/relay), plus the optional status LED
     * (no-op if OUTLET_STATUS_LED_GPIO is still GPIO_NUM_NC — see
     * setup_status_led()) — status_led_enabled has to be set before the
     * first set_output() call below so that call's own LED level is
     * correct too, not just the ones that follow it. */
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << OUTLET_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io_conf);
    setup_status_led();
    set_output(false);

    /* 2b. Configure the button input + its interrupt. */
    gpio_config_t button_io_conf = {};
    button_io_conf.pin_bit_mask = (1ULL << OUTLET_BUTTON_GPIO);
    button_io_conf.mode = GPIO_MODE_INPUT;
    button_io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    button_io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&button_io_conf);

    button_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "Button GPIO %d idle level: %d (expect 1/HIGH — 0 here means the pull-up isn't winning, check wiring)",
             OUTLET_BUTTON_GPIO, gpio_get_level(OUTLET_BUTTON_GPIO));

    /* These two silently doing nothing was a real bug in firmware/switch:
     * unchecked, a failure means no interrupt is ever attached and every
     * button press produces zero log output, which looks identical to
     * "nothing is wired up" from the outside. */
    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    if (isr_svc_err != ESP_OK && isr_svc_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(isr_svc_err));
    }
    esp_err_t isr_add_err = gpio_isr_handler_add(OUTLET_BUTTON_GPIO, button_isr_handler, (void *)(uintptr_t)OUTLET_BUTTON_GPIO);
    if (isr_add_err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(isr_add_err));
    }

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

    /* 2d. Configure the optional RGB status LED + its pattern-engine timer
     * — only if at least one of its 3 GPIOs is actually wired up. See the
     * header comment on rgb_status_led_enabled for why this is a
     * differently-named feature from OUTLET_STATUS_LED_GPIO above. */
    rgb_status_led_enabled = (STATUS_LED_RED_GPIO != GPIO_NUM_NC) ||
                              (STATUS_LED_GREEN_GPIO != GPIO_NUM_NC) ||
                              (STATUS_LED_BLUE_GPIO != GPIO_NUM_NC);
    if (rgb_status_led_enabled) {
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

        const esp_timer_create_args_t rgb_status_led_timer_args = {
            .callback = &rgb_status_led_tick_cb,
            .name = "rgb_status_led",
        };
        esp_timer_create(&rgb_status_led_timer_args, &rgb_status_led_timer);
        esp_timer_start_periodic(rgb_status_led_timer, STATUS_LED_TICK_MS * 1000);
    }

    /* 3. Build the Matter data model: one node, one On/Off Plug-in Unit
     * endpoint, plus a second Electrical Sensor endpoint if power
     * monitoring is enabled. */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    on_off_plug_in_unit::config_t outlet_config;
    endpoint_t *endpoint = on_off_plug_in_unit::create(node, &outlet_config, ENDPOINT_FLAG_NONE, NULL);
    if (!endpoint) {
        ESP_LOGE(TAG, "Failed to create outlet endpoint");
        return;
    }

    outlet_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Outlet endpoint id: %u", outlet_endpoint_id);

#if OUTLET_POWER_MONITOR != OUTLET_POWER_MONITOR_NONE
    if (!power_monitoring_setup(node)) {
        ESP_LOGE(TAG, "Power monitoring setup failed — continuing without it");
    }
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

#if OUTLET_POWER_MONITOR != OUTLET_POWER_MONITOR_NONE
    /* 5. Start polling the power monitor now that the data model + Matter
     * stack both exist — power_monitor_task() writes into the cluster
     * created above. */
    if (power_endpoint_id != 0) {
        xTaskCreate(power_monitor_task, "power_monitor_task", 4096, NULL, 5, NULL);
    }
#endif

    ESP_LOGI(TAG, "Matter outlet started. Scan the QR code to commission.");
}
