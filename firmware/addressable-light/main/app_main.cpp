/*
 * Minimal Matter Addressable Color Light — tenth device type, and this
 * repo's first over addressable/digital LED protocols (single-wire NRZ via
 * RMT, SPI for APA102, or bit-banged 2-wire for SM2335EGH) rather than
 * plain PWM (firmware/color-light/'s RGB/RGBW/RGBWW modes,
 * firmware/dimmable-light/).
 *
 * Built on the open-source esp-matter SDK. Everything here is plain, readable
 * C++ — there is no hidden framework layer and no telemetry. Matter is
 * local-first: commissioning happens over Bluetooth + your LAN, and control
 * runs over your local network. Nothing leaves your home unless you choose to
 * add a cloud hub (Google/Apple/Alexa). With Home Assistant it stays local.
 *
 * Target: ESP32 (WROOM-32) by default, matching the StudioPieters dev setup.
 * Works on other ESP32 chips too — see the README for how to switch target.
 *
 * --- Why "addressable" doesn't mean per-pixel control here -----------------
 * The six pixel-chain chips here (everything except SM2335EGH — see its own
 * note below) are called "addressable" because each LED has its own driver
 * chip and can, in principle, show its own color — that's what lets products
 * marketed as "RGBIC" do chases/gradients/rainbow effects across one strip.
 * This firmware does NOT expose that: it fills every pixel with the exact
 * same color, because Matter itself gives a controller no way to ask for
 * anything else. Checked directly in connectedhomeip's own
 * `src/controller/data_model/controller-clusters.matter`: there IS a cluster
 * for this — `provisional cluster DynamicLighting = 773` (0x0305), with
 * `EffectStruct`/`EffectColorStruct` types that look exactly like what a
 * real per-pixel/gradient effect would need — but it's marked `provisional`
 * and doesn't appear in any ratified data_model spec folder (checked 1.0
 * through 1.6, all shipped versions) — not something a real, certified
 * controller (Apple Home, Google Home, Home Assistant) can command today,
 * and esp-matter has no cluster support for it either. So this device is,
 * from Matter's point of view, exactly as capable as firmware/color-light/:
 * one Hue/Saturation/Level color (or, for the two RGBCCT chips supported
 * here, one color temperature) for the whole accessory. The only thing that
 * changes here is the physical layer driving that one result out, instead
 * of to LEDC PWM channels.
 * Revisit once DynamicLighting (or an equivalent) ships in a ratified
 * Matter spec version and esp-matter grows support for it.
 *
 * --- Same hand-assembled ExtendedColorLight endpoint as firmware/color-light/ ---
 * Identify + Groups + OnOff + LevelControl + ColorControl[HueSaturation,
 * +ColorTemperature for the two RGBCCT chips] + ScenesManagement, device
 * type ExtendedColorLight (0x010D) — see firmware/color-light/main/app_main.cpp's
 * header comment for the full rationale (not repeated here); this file's
 * endpoint-assembly code is a direct copy of that one, changed only where
 * the addressable output needs something different from LEDC PWM (see below).
 *
 * --- ADDRESSABLE_LIGHT_CHIP: eight chips across three protocol families ----
 * Every chip below was checked against its own manufacturer datasheet
 * directly where one exists (Worldsemi, for the six pixel-chain chips), not
 * a secondary source, per this repo's established practice — downloaded as
 * a PDF and read with `pdftotext`, since a plain web search turned up
 * conflicting numbers for more than one of these in the process (see the
 * per-chip notes below). For the two chips with no real public protocol
 * datasheet (APA102, SM2335EGH), this file instead follows the best
 * available, independently-verified, community reverse-engineering source
 * for each — the same "best available source, explicitly flagged" approach
 * this repo already uses for CSE7759 in firmware/outlet/ (assumed to share
 * HLW8012's family; own datasheet unobtainable).
 *
 * Six of the eight (everything except APA102 and SM2335EGH) are single-wire
 * NRZ protocols: each bit is one HIGH-then-LOW pulse pair whose relative
 * HIGH/LOW duration (not amplitude or frequency) encodes 0 vs 1, sent
 * MSB-first, one pixel's full color data back-to-back, followed by a LOW
 * "reset" pulse that latches the whole frame. All are driven via ESP-IDF's
 * `driver/rmt_tx.h` — a real ESP32 hardware peripheral built exactly for
 * generating precise pulse-timed waveforms like this, not a bit-banged GPIO
 * loop (unlike firmware/temperature-sensor/'s DHT11/DHT22/DS18B20 drivers,
 * which predate this repo's first use of RMT and bit-bang instead). The
 * exact API pattern — `rmt_new_tx_channel()`, `rmt_new_simple_encoder()`
 * with a byte-by-byte callback, `rmt_transmit()`, `rmt_tx_wait_all_done()` —
 * is checked directly against Espressif's own official reference example
 * (`examples/peripherals/rmt/led_strip_simple_encoder` in this repo's
 * pinned ESP-IDF v5.5.4, which targets classic ESP32 among its supported
 * boards); only the per-chip timing constants and pixel byte order differ
 * from that example, sourced from the datasheets below, not that example's
 * own round numbers. ADDRESSABLE_LIGHT_RESET_US is a single constant
 * (300us) used for every RMT-driven chip here, deliberately above every one
 * of their own documented minimums (50-280us depending on chip, see below)
 * — the reset only costs time once per full-strip update, so one generous
 * shared value is simpler than six tuned ones and costs nothing.
 *
 * WS2812B (24-bit, 3 bytes/pixel, GRB) — Worldsemi's own WS2812B datasheet
 *   ("Data transfer time" table): T0H=0.4us±150ns, T0L=0.85us±150ns,
 *   T1H=0.8us±150ns, T1L=0.45us±150ns, RES(reset)>=50us. Byte order from
 *   the same datasheet's "Composition of 24bit data" diagram: G7..G0,
 *   R7..R0, B7..B0. A second, separately-circulated Worldsemi WS2812B
 *   datasheet revision documents up to ">300us" reset for newer "-V5"
 *   silicon — the shared 300us ADDRESSABLE_LIGHT_RESET_US covers both.
 *
 * WS2813 / WS2815 (24-bit, 3 bytes/pixel, GRB) — same Worldsemi "dual
 *   signal" chip family (a second, chip-to-chip-only backup data line
 *   improves reliability; the ESP32 still only ever drives one DIN pin,
 *   exactly like WS2812B). WS2815 is the 12V variant of the same design.
 *   Their own datasheets ("WS2813B-Mini/Standard" and WS2815 revisions)
 *   cite noticeably WIDER tolerance windows than WS2812B's, and —
 *   importantly — windows that do NOT simply contain WS2812B's own values:
 *   T0H 220-380ns (WS2812B's 400ns is just outside this), T1H 580ns-1.6us,
 *   T0L 580ns-1.6us, T1L 220-420ns (WS2812B's 450ns is just outside this
 *   too), RES>280us. So this file uses its own tailored values for this
 *   pair — T0H=300ns, T0L=800ns, T1H=800ns, T1L=300ns — chosen to sit
 *   comfortably inside both chips' actual cited windows rather than
 *   assuming WS2812B-compatibility and simply reusing its numbers, which
 *   the numeric ranges show would NOT actually be safe. Byte order (GRB)
 *   confirmed directly in both chips' own datasheets.
 *
 * SK6812 (24-bit, 3 bytes/pixel, GRB) / SK6812_RGBW (32-bit, 4 bytes/pixel,
 *   RGBW) — Worldsemi's own SK6812 and SK6812RGBW datasheets (the RGBW one:
 *   Document No. SPC/SK6812RGBW Rev.01). Both variants share the same
 *   timing: T0H=0.3us±0.15us, T0L=0.9us±0.15us, T1H=0.6us±0.15us,
 *   T1L=0.6us±0.15us, Trst(reset)=80us — confirmed as genuinely identical
 *   across both datasheets, not assumed from one to the other. Byte order:
 *   plain SK6812 is G7..G0/R7..R0/B7..B0 (GRB); SK6812_RGBW is
 *   R7..R0/G7..G0/B7..B0/W7..W0 (RGBW) — flagged explicitly because it's a
 *   real point of disagreement in the wild: several widely-used
 *   Arduino/ESPHome libraries default to treating SK6812 RGBW strips as
 *   GRBW order instead — if your strip's colors come out visibly swapped,
 *   that library-vs-datasheet discrepancy (not a bug in this file) is the
 *   first thing to check.
 *
 * WS2805 (40-bit, 5 bytes/pixel, RGBW1W2, RGBCCT) — Worldsemi's own
 *   official WS2805 datasheet (fetched directly from world-semi.com, not a
 *   distributor mirror): T0H=220-380ns, T1H=580ns-1us, T0L=580ns-1us,
 *   T1L=580ns-1us, RES>280us — note T1L is NOT short here the way it is
 *   for the WS2812B-style chips above (T1L shares WS2805's "long"
 *   580ns-1us window with T0L/T1H instead), so this file uses its own
 *   values for it too: T0H=300ns, T0L=800ns, T1H=800ns, T1L=800ns (all
 *   confirmed inside WS2805's own windows). Byte order confirmed in the
 *   datasheet's own "Composition of 40bit Data" diagram:
 *   R7..R0/G7..G0/B7..B0/W1(7..0)/W2(7..0). The datasheet documents W1/W2
 *   as two independent constant-current outputs but does not itself label
 *   which is "warm" and which is "cool" — this file follows the
 *   W1=warm/W2=cool convention used in most vendor documentation for
 *   WS2805 strips (e.g. BTF-Lighting's), flagged here as a convention, not
 *   a datasheet-confirmed fact.
 *
 * APA102 (a.k.a. "DotStar") — a real 2-wire clock+data interface
 *   (essentially SPI without a chip-select pin), driven via ESP-IDF's
 *   `driver/spi_master.h` instead of RMT — the correct tool for a genuine
 *   clock-based protocol, same reasoning as using RMT instead of
 *   bit-banging for the NRZ chips above. APA102's own official datasheet is
 *   notoriously thin on protocol detail; this file follows the widely-cited,
 *   independently-verified reverse-engineering writeup at
 *   cpldcpu.com/2014/11/30/understanding-the-apa102-superled/
 *   (cross-referenced against Adafruit's and SparkFun's own APA102/DotStar
 *   guides, which agree with it) rather than the datasheet. Frame format: a
 *   32-bit all-zero start frame, then one 32-bit frame per pixel (3 fixed
 *   high bits + 5-bit brightness + 8-bit blue + 8-bit green + 8-bit red —
 *   i.e. brightness-prefixed BGR, not RGB), then an end frame of at least
 *   ceil(pixel_count/2) one-bits (rounded up to whole bytes here) — the
 *   original datasheet's fixed 32-bit end frame is only reliable up to 64
 *   LEDs per the same source, so this file always computes the longer,
 *   pixel-count-aware end frame instead. Per that source's own explicit
 *   recommendation ("I strongly suggest to only use the full brightness
 *   setting (31) to reduce flicker"), the 5-bit brightness field is always
 *   sent as its maximum (31) here — brightness is carried entirely in the
 *   R/G/B byte values instead (via HSV's "V", exactly like every other
 *   chip in this file), never APA102's own separate brightness field.
 *
 * SM2335EGH — architecturally different from every other chip in this
 *   file: it is NOT a pixel-chain chip at all. It's a single-fixture,
 *   5-channel (RGB + cool white + warm white) constant-current LED driver
 *   used inside smart bulbs/downlights as a digital replacement for direct
 *   PWM drive (closer in spirit to firmware/color-light/'s single-fixture
 *   model than to the pixel-chain chips above) — so
 *   ADDRESSABLE_LIGHT_PIXEL_COUNT does not apply to it at all (there is
 *   only ever one fixture, never a chain of N), and the wizard hides that
 *   field when this chip is selected. Its manufacturer (Shenzhen Sunmoon
 *   Micro / chinaasic.com) publishes only a one-page feature summary with
 *   no protocol/timing detail at all — confirmed directly by fetching it —
 *   and multiple independent open-source driver authors (ESPHome, the
 *   sm2335egh-rs Rust crate) have documented asking the manufacturer for a
 *   real protocol datasheet and not receiving one. This file therefore
 *   follows ESPHome's own open-source, real-hardware-tested implementation
 *   (`esphome/components/sm10bit_base/sm10bit_base.cpp`, fetched directly)
 *   verbatim: a bit-banged 2-wire (DATA+CLK) protocol, ~2us per bit
 *   (matching ESPHome's own SM10BIT_DELAY). A "start" condition is DATA
 *   low then CLK low; each of 12 bytes is clocked out MSB-first (DATA set,
 *   then CLK pulsed high-then-low per bit), followed by one extra
 *   "ACK" clock pulse per byte with DATA released to input (the value is
 *   never actually checked, matching ESPHome's own implementation exactly);
 *   a "stop" condition is CLK high then DATA high. The 12-byte buffer is
 *   [0]=model_id(0xC0, SM2335EGH's own value per ESPHome's
 *   `sm2335/__init__.py`: `set_model(0xC0)`) + start-address(0x18, "all 5
 *   channels" per ESPHome's own SM10BIT_ADDR_START_5CH), [1]=(color
 *   gain<<4)|white gain (both 0-15; this file uses ESPHome's own defaults,
 *   2=30mA for the RGB channels and 4=25mA for the two white channels —
 *   see ADDRESSABLE_LIGHT_SM2335_COLOR_GAIN/_WHITE_GAIN below), then 5
 *   channels x 2 bytes each (10-bit values, 0-1023, high byte first) in
 *   R,G,B,W1,W2 order (OUT1..OUT5) — which physical white LED is W1 vs W2
 *   depends on how your specific bulb/module wired OUT4/OUT5, same
 *   flagged-as-convention caveat as WS2805's W1/W2 above. Because SM2335EGH
 *   is genuine RGBCCT (independent warm/cool white outputs, not just "one
 *   more white channel"), it reuses the exact same ColorTemperature/
 *   interlock Matter-side design as WS2805 below (and, ultimately,
 *   firmware/color-light/'s COLOR_LIGHT_MODE_RGBWW) — see that entry's own
 *   note for the full explanation, not repeated per-chip here.
 *
 *   Because WS2805 and SM2335EGH are genuinely RGBCCT (independent
 *   warm/cool white, not just "one more white channel" the way
 *   SK6812_RGBW is), both reuse firmware/color-light/'s
 *   COLOR_LIGHT_MODE_RGBWW design wholesale rather than the simpler
 *   "extract common white" trick: Matter's ColorControl ColorTemperature
 *   feature is added alongside HueSaturation, and the firmware locally
 *   latches which color space (light_color_source below) a controller
 *   most recently commanded — driving either the R/G/B bytes or the
 *   W1/W2 bytes, never both — the same "color_interlock" concept
 *   ESPHome's own rgbww light component documents for this class of
 *   hardware, and the identical mireds->channel-duty formula from
 *   firmware/color-light/'s header comment (sourced from ESPHome's own
 *   light_call.cpp). See that file's header comment for the full
 *   explanation; not re-derived here, just re-applied against pixel/frame
 *   bytes instead of LEDC channels.
 *
 * --- Output -------------------------------------------------------------
 * For the pixel-chain chips, ADDRESSABLE_LIGHT_PIXEL_COUNT pixels are all
 * set to the same on/off + level + hue/saturation (or color temperature)
 * result together — same "off doesn't forget where it was" behavior as
 * firmware/color-light/ and firmware/dimmable-light/: turning back on
 * restores the last color, not a reset one. SM2335EGH (a single fixture,
 * no pixel count) gets the same treatment for its one set of 5 channels.
 * The full output is re-sent on every relevant attribute change, same as
 * firmware/color-light/ re-computing LEDC duty on every change — there's
 * no partial/incremental update, unnecessary at this scale.
 *
 * --- Sharing the Identify LED with the strip/fixture's own data pin -------
 * IDENTIFY_LED_GPIO defaults to the same GPIO as the data pin
 * (ADDRESSABLE_LIGHT_GPIO) — deliberately, so a product with no separate
 * identify LED wired up still gets a working Identify. This can NOT be
 * done the way firmware/color-light/ shares a pin between its own Identify
 * LED and a color channel (both plain GPIO/LEDC calls, safe to alternate)
 * — here, the data pin is owned by a whole peripheral (the RMT channel,
 * the SPI bus for APA102, or the bit-banged protocol for SM2335EGH), and a
 * second, separate `gpio_config()` call on that same pin would fight the
 * peripheral for pin ownership, corrupting the output. Instead, whenever
 * IDENTIFY_LED_GPIO equals the data pin (checked once at startup, not via
 * the preprocessor — GPIO_NUM_* are plain C enum values, not macros, so an
 * `#if` comparison between them is not meaningful and would silently
 * evaluate as if both were 0), Identify is implemented by flashing the
 * whole output white/off via the exact same transmit path set_output()
 * already uses, instead of toggling a separate LED. Set IDENTIFY_LED_GPIO
 * to any other pin (with a real LED wired to it) to get the classic
 * separate-LED behavior back.
 */

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_timer.h>
#include <math.h>
#include <string.h>

#include <esp_matter.h>

static const char *TAG = "matter_addressable_light";

/* Change this to the GPIO your strip's DIN (data in) pin is wired to — for
 * APA102/SM2335EGH this is the DATA line specifically (see
 * ADDRESSABLE_LIGHT_CLOCK_GPIO below for their second pin). Any GPIO works
 * — these are all software-timed protocols, not fixed peripheral pins.
 * GPIO 2 is a plain, unreserved GPIO on classic ESP32 (WROOM-32), matching
 * this repo's other single-pin device types' default. Adjust to match your
 * board. */
#define ADDRESSABLE_LIGHT_GPIO GPIO_NUM_2

/* Number of physically wired pixels on your strip — MUST match reality:
 * fewer configured than wired just leaves the extra ones dark (harmless),
 * but more configured than wired sends extra bits/frames that the last
 * real pixel on the strip passes through as if meant for a pixel after it,
 * which does nothing useful but also isn't harmful. 8 is a small, common
 * test-strip length; adjust to your actual strip. Not used at all for
 * SM2335EGH — see the header comment on why that chip has no pixel-chain
 * concept. */
#define ADDRESSABLE_LIGHT_PIXEL_COUNT 12

/* ADDRESSABLE_LIGHT_CHIP selects which addressable chip/protocol to
 * generate for — see the header comment above for the full explanation and
 * exact protocol sourcing of every constant below. */
#define ADDRESSABLE_LIGHT_CHIP_WS2812B 0
#define ADDRESSABLE_LIGHT_CHIP_WS2813 1
#define ADDRESSABLE_LIGHT_CHIP_WS2815 2
#define ADDRESSABLE_LIGHT_CHIP_SK6812 3       /* plain, 3-channel RGB */
#define ADDRESSABLE_LIGHT_CHIP_SK6812_RGBW 4  /* 4-channel RGBW */
#define ADDRESSABLE_LIGHT_CHIP_WS2805 5       /* 5-channel RGB + warm/cool white (RGBCCT), pixel chain */
#define ADDRESSABLE_LIGHT_CHIP_APA102 6       /* SPI (clock+data), BGR + brightness, pixel chain */
#define ADDRESSABLE_LIGHT_CHIP_SM2335 7       /* bit-banged 2-wire, RGBCCT, single fixture (no pixel chain) */
#define ADDRESSABLE_LIGHT_CHIP ADDRESSABLE_LIGHT_CHIP_WS2812B

/* True for the two genuinely RGBCCT chips (independent warm/cool white,
 * not just "one more white channel") — both reuse the exact same
 * ColorTemperature/interlock design; see the header comment. */
#define ADDRESSABLE_LIGHT_IS_RGBCCT \
    (ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2805 || ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335)

/* APA102/SM2335EGH only: the CLOCK line — their second pin, alongside
 * ADDRESSABLE_LIGHT_GPIO as DATA. Unused (and not configured) for every
 * other chip here, which only need the one data pin. GPIO 4 is a plain,
 * unreserved GPIO on classic ESP32 (WROOM-32) that doesn't collide with
 * the data pin above. */
#define ADDRESSABLE_LIGHT_CLOCK_GPIO GPIO_NUM_4

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2812B
#define ADDRESSABLE_LIGHT_T0H_US 0.4f
#define ADDRESSABLE_LIGHT_T0L_US 0.85f
#define ADDRESSABLE_LIGHT_T1H_US 0.8f
#define ADDRESSABLE_LIGHT_T1L_US 0.45f
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 3
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2813 || ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2815
#define ADDRESSABLE_LIGHT_T0H_US 0.3f
#define ADDRESSABLE_LIGHT_T0L_US 0.8f
#define ADDRESSABLE_LIGHT_T1H_US 0.8f
#define ADDRESSABLE_LIGHT_T1L_US 0.3f
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 3
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812
#define ADDRESSABLE_LIGHT_T0H_US 0.3f
#define ADDRESSABLE_LIGHT_T0L_US 0.9f
#define ADDRESSABLE_LIGHT_T1H_US 0.6f
#define ADDRESSABLE_LIGHT_T1L_US 0.6f
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 3
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812_RGBW
#define ADDRESSABLE_LIGHT_T0H_US 0.3f
#define ADDRESSABLE_LIGHT_T0L_US 0.9f
#define ADDRESSABLE_LIGHT_T1H_US 0.6f
#define ADDRESSABLE_LIGHT_T1L_US 0.6f
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 4
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2805
#define ADDRESSABLE_LIGHT_T0H_US 0.3f
#define ADDRESSABLE_LIGHT_T0L_US 0.8f
#define ADDRESSABLE_LIGHT_T1H_US 0.8f
#define ADDRESSABLE_LIGHT_T1L_US 0.8f
#define ADDRESSABLE_LIGHT_BYTES_PER_PIXEL 5
#endif

/* Shared by every RMT-driven chip (everything except APA102/SM2335EGH) —
 * see the header comment for why one generous constant instead of six
 * tuned ones. */
#define ADDRESSABLE_LIGHT_RESET_US 300

/* Color temperature range — mandatory ColorControl feature for every
 * ExtendedColorLight (see the header comment on Matter's own conformance
 * requirement), so this is no longer gated to the RGBCCT chips only. For
 * WS2805/SM2335EGH (real independent warm/cool white channels) this drives
 * those channels directly via mireds_to_w1_w2() below. For every other
 * chip (plain RGB or RGBW, no dedicated white channel) a commanded color
 * temperature is instead rendered as an *approximate* RGB color via
 * mireds_to_rgb_approx() below — the same thing WLED/Home Assistant do for
 * RGB-only bulbs that still want to accept a color-temperature command.
 * 6500K/2700K are the same two most common "daylight"/"warm white" LED bin
 * ratings firmware/color-light/'s own RGBWW mode uses — adjust to your
 * actual hardware's rated color temperature if known. */
#define ADDRESSABLE_LIGHT_COOL_WHITE_KELVIN 6500
#define ADDRESSABLE_LIGHT_WARM_WHITE_KELVIN 2700
#define ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS (1000000 / ADDRESSABLE_LIGHT_COOL_WHITE_KELVIN) /* ~154 */
#define ADDRESSABLE_LIGHT_WARM_WHITE_MIREDS (1000000 / ADDRESSABLE_LIGHT_WARM_WHITE_KELVIN) /* ~370 */

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
/* SM2335EGH protocol constants — see the header comment for full sourcing
 * (ESPHome's sm10bit_base.cpp / sm2335/__init__.py, the best available
 * source given no real protocol datasheet exists for this chip). */
#define ADDRESSABLE_LIGHT_SM2335_MODEL_ID 0xC0
#define ADDRESSABLE_LIGHT_SM2335_ADDR_START_5CH 0x18
#define ADDRESSABLE_LIGHT_SM2335_COLOR_GAIN 2  /* 0-15; 2 = 30mA, ESPHome's own default for RGB */
#define ADDRESSABLE_LIGHT_SM2335_WHITE_GAIN 4  /* 0-15; 4 = 25mA, ESPHome's own default for CW/WW */
#define ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US 2
#endif

/* Identify LED — see the header comment on why this defaults to the SAME
 * GPIO as the data pin (flashing the strip/fixture itself for Identify)
 * rather than a separate LED, and why that's safe here but would NOT be
 * safe to do via a second gpio_config() call the way firmware/color-light/
 * shares a pin. Set to a different GPIO (with a real LED wired to it) for
 * the classic separate-LED behavior instead. */
#define IDENTIFY_LED_GPIO GPIO_NUM_2
#define IDENTIFY_BLINK_INTERVAL_MS 500

/* Quick-power-cycle factory reset — see firmware/light/main/app_main.cpp's
 * header comment for the full mechanism and its sourcing. */
#define FACTORY_RESET_NVS_NAMESPACE "boot_info"
#define FACTORY_RESET_NVS_KEY "boot_count"
#define FACTORY_RESET_BOOT_COUNT_THRESHOLD 3
#define FACTORY_RESET_CONFIRM_DELAY_MS 10000

/* RMT resolution — 10MHz (1 tick = 0.1us), same as Espressif's own
 * reference example; fine enough to represent every timing value above to
 * within its documented tolerance. Unused for APA102/SM2335EGH. */
#define ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ 10000000

/* APA102 SPI clock speed — well within what real APA102/SK9822 strips are
 * commonly driven at (multi-MHz); the "official" datasheet gives no
 * documented min/max (see header comment), so this is a conservative,
 * widely-used value rather than a datasheet-sourced one. */
#define ADDRESSABLE_LIGHT_APA102_SPI_HZ 4000000

/* Initial brightness/hue/saturation — same defaults and reasoning as
 * firmware/color-light/'s (a visibly "on and colored" first boot). */
#define ADDRESSABLE_LIGHT_DEFAULT_LEVEL 128
#define ADDRESSABLE_LIGHT_DEFAULT_HUE 21          /* ~30 degrees: 21 * 360 / 254 */
#define ADDRESSABLE_LIGHT_DEFAULT_SATURATION 254  /* fully saturated */

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_APA102
#include <driver/spi_master.h>
#define ADDRESSABLE_LIGHT_APA102_SPI_HOST SPI2_HOST
/* Start frame (4 zero bytes) + one 4-byte frame per pixel + an end frame
 * of at least ceil(pixel_count/2) one-bits, rounded up to whole bytes —
 * see the header comment for why the end frame is computed this way
 * instead of the datasheet's fixed 32-bit recommendation. */
#define ADDRESSABLE_LIGHT_APA102_END_FRAME_BYTES ((ADDRESSABLE_LIGHT_PIXEL_COUNT + 15) / 16)
#define ADDRESSABLE_LIGHT_APA102_BUFFER_BYTES \
    (4 + (ADDRESSABLE_LIGHT_PIXEL_COUNT * 4) + ADDRESSABLE_LIGHT_APA102_END_FRAME_BYTES)
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
#include <esp_rom_sys.h> /* esp_rom_delay_us() */
#else
#include <driver/rmt_tx.h>
#endif

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t addressable_light_endpoint_id = 0;
static esp_timer_handle_t identify_led_timer = NULL;
/* See the header comment on sharing the Identify LED with the data pin —
 * computed once at startup (a plain runtime comparison, not `#if`, since
 * GPIO_NUM_* are C enum values, not preprocessor macros). */
static bool identify_via_strip = false;

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_APA102
static spi_device_handle_t addressable_light_spi_dev = NULL;
static uint8_t addressable_light_pixels[ADDRESSABLE_LIGHT_APA102_BUFFER_BYTES];
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
/* No pixel-chain buffer at all — SM2335EGH drives one fixture's worth of
 * channels directly from transmit_pixel(), see the header comment. */
#else
static rmt_channel_handle_t addressable_light_rmt_chan = NULL;
static rmt_encoder_handle_t addressable_light_rmt_encoder = NULL;
static uint8_t addressable_light_pixels[ADDRESSABLE_LIGHT_PIXEL_COUNT * ADDRESSABLE_LIGHT_BYTES_PER_PIXEL];
#endif

/* Mirrors the Matter OnOff/LevelControl/ColorControl attributes' current
 * values — same pattern as firmware/color-light/'s. */
static bool light_on = false;
static uint8_t light_level = ADDRESSABLE_LIGHT_DEFAULT_LEVEL;
static uint8_t light_hue = ADDRESSABLE_LIGHT_DEFAULT_HUE;
static uint8_t light_saturation = ADDRESSABLE_LIGHT_DEFAULT_SATURATION;

/* Which color space was most recently commanded — HS/XY/CT are mutually
 * exclusive ColorMode values in Matter's own ColorControl cluster (a
 * controller picks one at a time via MoveToHue/MoveToColor/
 * MoveToColorTemperature), so this local interlock just remembers which one
 * to render with. Same interlock concept firmware/color-light/'s
 * COLOR_LIGHT_MODE_RGBWW already used for HS-vs-CT, generalized to all
 * three color spaces now that XY and ColorTemperature are both mandatory
 * ColorControl features for the ExtendedColorLight device type (see the
 * header comment) rather than optional extras. */
enum color_source_t { COLOR_SOURCE_HS, COLOR_SOURCE_XY, COLOR_SOURCE_CT };
static color_source_t light_color_source = COLOR_SOURCE_HS;
static uint16_t light_x = 24939; /* esp-matter's own xy feature config_t default */
static uint16_t light_y = 24701;
static uint16_t light_mireds = ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS;

/* Textbook HSV -> RGB conversion — identical to firmware/color-light/'s
 * hsv_to_rgb(); see that file's header comment for why this needs no
 * SDK-specific verification (deterministic math, one well-known answer). */
static void hsv_to_rgb(float h, float s, float v, float *r, float *g, float *b)
{
    float c = v * s;
    float h_prime = fmodf(h / 60.0f, 6.0f);
    float x = c * (1.0f - fabsf(fmodf(h_prime, 2.0f) - 1.0f));
    float m = v - c;
    float r1, g1, b1;

    if (h_prime < 1.0f) { r1 = c; g1 = x; b1 = 0; }
    else if (h_prime < 2.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (h_prime < 3.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (h_prime < 4.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (h_prime < 5.0f) { r1 = x; g1 = 0; b1 = c; }
    else { r1 = c; g1 = 0; b1 = x; }

    *r = r1 + m;
    *g = g1 + m;
    *b = b1 + m;
}

/* CIE xyY -> sRGB, for the XY ColorControl feature — Philips' own published
 * Hue conversion algorithm (the standard reference for this exact
 * transform, reused as-is by Home Assistant's color utility and countless
 * open-source Hue/Matter libraries — not invented for this file, same
 * "known, widely-used algorithm" sourcing standard this repo already
 * applies to e.g. WLED's/ESPHome's formulas elsewhere). x/y are the
 * fractions CurrentX/CurrentY decode to (raw attribute value / 65536,
 * confirmed against the ZCL/Matter spec's own documented scaling); v is
 * brightness [0,1]. Includes the algorithm's own gamma (sRGB companding)
 * step, without which colors come out visibly washed out. */
static void xy_to_rgb(float x, float y, float v, float *r, float *g, float *b)
{
    if (y <= 0.0f) {
        *r = *g = *b = 0.0f;
        return;
    }
    float z = 1.0f - x - y;
    float Y = v;
    float X = (Y / y) * x;
    float Z = (Y / y) * z;

    float r_lin = X * 1.656492f - Y * 0.354851f - Z * 0.255038f;
    float g_lin = -X * 0.707196f + Y * 1.655397f + Z * 0.036152f;
    float b_lin = X * 0.051713f - Y * 0.121364f + Z * 1.011530f;

    float *chans[3] = {&r_lin, &g_lin, &b_lin};
    for (int i = 0; i < 3; i++) {
        float c = *chans[i];
        c = (c <= 0.0031308f) ? (12.92f * c) : (1.055f * powf(c, 1.0f / 2.4f) - 0.055f);
        *chans[i] = fminf(fmaxf(c, 0.0f), 1.0f);
    }
    *r = r_lin;
    *g = g_lin;
    *b = b_lin;
}

/* Correlated color temperature (Kelvin) -> approximate sRGB, for rendering
 * a ColorTemperature command on chips with no dedicated warm/cool white
 * channel (everything except WS2805/SM2335EGH, which use the real
 * mireds_to_w1_w2() channels instead — see that function). This is Tanner
 * Helland's widely-used blackbody-radiation RGB approximation ("How to
 * Convert Temperature (K) to RGB: Algorithm and Sample Code") — the same
 * practical approximation reused across the maker community (WLED's own
 * Kelvin white-balance slider among others) for exactly this "no physical
 * white LED, approximate it in RGB instead" case. Output channels are
 * [0,1] fractions of `v` (brightness). */
static void mireds_to_rgb_approx(uint16_t mireds, float v, float *r, float *g, float *b)
{
    float clamped_mireds = fminf(fmaxf((float)mireds, (float)ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS),
                                  (float)ADDRESSABLE_LIGHT_WARM_WHITE_MIREDS);
    float kelvin = 1000000.0f / clamped_mireds;
    float t = kelvin / 100.0f;
    float rf, gf, bf;

    if (t <= 66.0f) {
        rf = 255.0f;
    } else {
        rf = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
    }
    if (t <= 66.0f) {
        gf = 99.4708025861f * logf(t) - 161.1195681661f;
    } else {
        gf = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
    }
    if (t >= 66.0f) {
        bf = 255.0f;
    } else if (t <= 19.0f) {
        bf = 0.0f;
    } else {
        bf = 138.5177312231f * logf(t - 10.0f) - 305.0447927307f;
    }

    *r = fminf(fmaxf(rf, 0.0f), 255.0f) / 255.0f * v;
    *g = fminf(fmaxf(gf, 0.0f), 255.0f) / 255.0f * v;
    *b = fminf(fmaxf(bf, 0.0f), 255.0f) / 255.0f * v;
}

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812_RGBW
/* RGB -> RGBW via the standard "extract common white" technique — see the
 * header comment above for sourcing (same algorithm as
 * firmware/color-light/'s RGBW mode). Inputs and outputs are all in [0,1]. */
static void rgb_to_rgbw(float r, float g, float b, float *r_out, float *g_out, float *b_out, float *w_out)
{
    float w = fminf(r, fminf(g, b));
    *r_out = r - w;
    *g_out = g - w;
    *b_out = b - w;
    *w_out = w;
}
#endif

#if ADDRESSABLE_LIGHT_IS_RGBCCT
/* Target color temperature (mireds, clamped to the warm/cool physical
 * range) + brightness -> warm-white/cool-white byte values. Identical
 * formula (and sourcing) to firmware/color-light/'s mireds_to_cw_ww() —
 * see that file's header comment. Outputs are [0,1] fractions of
 * `value_fraction` (brightness). */
static void mireds_to_w1_w2(uint16_t mireds, float value_fraction, float *warm_out, float *cool_out)
{
    float clamped = fminf(fmaxf((float)mireds, (float)ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS),
                           (float)ADDRESSABLE_LIGHT_WARM_WHITE_MIREDS);
    float range = (float)(ADDRESSABLE_LIGHT_WARM_WHITE_MIREDS - ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS);
    float warm_fraction = (clamped - (float)ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS) / range;
    float cool_fraction = 1.0f - warm_fraction;
    float max_fraction = fmaxf(warm_fraction, cool_fraction);
    *warm_out = value_fraction * (warm_fraction / max_fraction);
    *cool_out = value_fraction * (cool_fraction / max_fraction);
}
#endif

#if ADDRESSABLE_LIGHT_CHIP != ADDRESSABLE_LIGHT_CHIP_APA102 && ADDRESSABLE_LIGHT_CHIP != ADDRESSABLE_LIGHT_CHIP_SM2335
/* RMT symbol pair (HIGH pulse + LOW pulse) for a single "0" or "1" bit,
 * plus the reset symbol — computed once from the datasheet timing
 * constants above, in the same style as Espressif's own reference
 * example. */
static const rmt_symbol_word_t addressable_light_bit0 = {
    .duration0 = (uint16_t)(ADDRESSABLE_LIGHT_T0H_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level0 = 1,
    .duration1 = (uint16_t)(ADDRESSABLE_LIGHT_T0L_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
};
static const rmt_symbol_word_t addressable_light_bit1 = {
    .duration0 = (uint16_t)(ADDRESSABLE_LIGHT_T1H_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level0 = 1,
    .duration1 = (uint16_t)(ADDRESSABLE_LIGHT_T1L_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
};
static const rmt_symbol_word_t addressable_light_reset = {
    .duration0 = (uint16_t)((uint64_t)ADDRESSABLE_LIGHT_RESET_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000 / 2),
    .level0 = 0,
    .duration1 = (uint16_t)((uint64_t)ADDRESSABLE_LIGHT_RESET_US * ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ / 1000000 / 2),
    .level1 = 0,
};

/* RMT "simple encoder" callback — serializes addressable_light_pixels[]
 * (whatever byte order the caller filled it with) MSB-first per byte, then
 * emits the reset symbol once all bytes are done. Pattern confirmed
 * against Espressif's own reference example (see header comment); only
 * the bit0/bit1/reset symbol VALUES differ, sourced from the datasheets
 * above rather than that example's round numbers. */
static size_t addressable_light_rmt_encode(const void *data, size_t data_size, size_t symbols_written,
                                           size_t symbols_free, rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    if (symbols_free < 8) {
        return 0; /* need room for a whole byte's worth of symbols before encoding one */
    }
    size_t data_pos = symbols_written / 8;
    const uint8_t *data_bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (int bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (data_bytes[data_pos] & bitmask) ? addressable_light_bit1 : addressable_light_bit0;
        }
        return symbol_pos;
    }
    symbols[0] = addressable_light_reset;
    *done = true;
    return 1;
}
#endif

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
/* SM2335EGH bit-bang primitives — verbatim port of ESPHome's own
 * sm10bit_base.cpp (see header comment for sourcing). */
static void sm2335_write_bit(bool value)
{
    gpio_set_level(ADDRESSABLE_LIGHT_GPIO, value);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 1);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 0);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
}

static void sm2335_write_byte(uint8_t data)
{
    for (uint8_t mask = 0x80; mask; mask >>= 1) {
        sm2335_write_bit(data & mask);
    }
    /* ACK cycle: release DATA to input (the chip may pull it, but this
     * value is never actually read/checked — matching ESPHome's own
     * reference implementation exactly), then clock it once. */
    gpio_set_direction(ADDRESSABLE_LIGHT_GPIO, GPIO_MODE_INPUT);
    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 1);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 0);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
    gpio_set_direction(ADDRESSABLE_LIGHT_GPIO, GPIO_MODE_OUTPUT);
}
#endif

/* Computes the current on/off + level + color (whichever of HS/XY/CT was
 * most recently commanded — see light_color_source) result into up to 5
 * [0,1] fractions — shared by both the strip-flash Identify path and the
 * real set_output() below, so Identify always flashes using the same color
 * math this device everywhere else uses. */
static void compute_pixel_fractions(float *r, float *g, float *b, float *w, float *w2)
{
    *r = *g = *b = *w = *w2 = 0.0f;
    if (!light_on) {
        return;
    }
    float value_fraction = (float)light_level / 254.0f;

#if ADDRESSABLE_LIGHT_IS_RGBCCT
    /* Real independent warm/cool white channels — a commanded color
     * temperature drives those directly, not an RGB approximation. */
    if (light_color_source == COLOR_SOURCE_CT) {
        mireds_to_w1_w2(light_mireds, value_fraction, w, w2);
        return;
    }
#endif

    if (light_color_source == COLOR_SOURCE_XY) {
        float x_fraction = (float)light_x / 65536.0f;
        float y_fraction = (float)light_y / 65536.0f;
        xy_to_rgb(x_fraction, y_fraction, value_fraction, r, g, b);
#if !ADDRESSABLE_LIGHT_IS_RGBCCT
    } else if (light_color_source == COLOR_SOURCE_CT) {
        /* No dedicated white channel on this chip — approximate the
         * commanded color temperature in RGB instead (see the header
         * comment on mireds_to_rgb_approx()). */
        mireds_to_rgb_approx(light_mireds, value_fraction, r, g, b);
#endif
    } else {
        float hue_degrees = (float)light_hue * 360.0f / 254.0f;
        float saturation_fraction = (float)light_saturation / 254.0f;
        hsv_to_rgb(hue_degrees, saturation_fraction, value_fraction, r, g, b);
    }
#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812_RGBW
    rgb_to_rgbw(*r, *g, *b, r, g, b, w);
#endif
}

#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_APA102
/* Fills addressable_light_pixels[] with a full APA102 frame (start + one
 * 4-byte frame per pixel, brightness-prefixed BGR + a pixel-count-aware
 * end frame) and clocks it out over SPI — see the header comment for the
 * exact frame format and its sourcing. */
static void transmit_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    memset(addressable_light_pixels, 0x00, 4); /* start frame */
    for (int i = 0; i < ADDRESSABLE_LIGHT_PIXEL_COUNT; i++) {
        uint8_t *pixel = &addressable_light_pixels[4 + i * 4];
        pixel[0] = 0xE0 | 31; /* 3 fixed '1' bits + max brightness (31) — see header comment on why always max */
        pixel[1] = b;
        pixel[2] = g;
        pixel[3] = r;
    }
    memset(&addressable_light_pixels[4 + ADDRESSABLE_LIGHT_PIXEL_COUNT * 4], 0xFF,
           ADDRESSABLE_LIGHT_APA102_END_FRAME_BYTES);

    spi_transaction_t t = {};
    t.length = sizeof(addressable_light_pixels) * 8;
    t.tx_buffer = addressable_light_pixels;
    esp_err_t err = spi_device_transmit(addressable_light_spi_dev, &t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_device_transmit failed: %d", err);
    }
}
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
/* Sends one full 12-byte SM2335EGH command (RGB + warm/cool white, 10-bit
 * each) — see the header comment for the exact protocol/buffer layout and
 * its sourcing. No pixel chain: this is the whole fixture's state. */
static void transmit_pixel(uint16_t r, uint16_t g, uint16_t b, uint16_t w, uint16_t w2)
{
    uint16_t channels[5] = { r, g, b, w, w2 };
    uint8_t data[12];
    data[0] = ADDRESSABLE_LIGHT_SM2335_MODEL_ID + ADDRESSABLE_LIGHT_SM2335_ADDR_START_5CH;
    data[1] = (ADDRESSABLE_LIGHT_SM2335_COLOR_GAIN << 4) | ADDRESSABLE_LIGHT_SM2335_WHITE_GAIN;
    for (int i = 0; i < 5; i++) {
        data[2 + i * 2] = (uint8_t)(channels[i] >> 8);
        data[2 + i * 2 + 1] = (uint8_t)(channels[i] & 0xFF);
    }

    gpio_set_level(ADDRESSABLE_LIGHT_GPIO, 0); /* start condition */
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 0);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);

    for (int i = 0; i < 12; i++) {
        sm2335_write_byte(data[i]);
    }

    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 1); /* stop condition */
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
    gpio_set_level(ADDRESSABLE_LIGHT_GPIO, 1);
    esp_rom_delay_us(ADDRESSABLE_LIGHT_SM2335_BIT_DELAY_US);
}
#else
/* Fills addressable_light_pixels[] (every pixel identical — see the header
 * comment on why "addressable" doesn't mean per-pixel control here) and
 * transmits it via RMT. */
static void transmit_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w, uint8_t w2)
{
    for (int i = 0; i < ADDRESSABLE_LIGHT_PIXEL_COUNT; i++) {
        uint8_t *pixel = &addressable_light_pixels[i * ADDRESSABLE_LIGHT_BYTES_PER_PIXEL];
#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2812B || \
    ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2813 || \
    ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2815 || \
    ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812
        /* GRB order — confirmed in each chip's own datasheet, see header comment. */
        pixel[0] = g;
        pixel[1] = r;
        pixel[2] = b;
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SK6812_RGBW
        /* RGBW order — confirmed in SK6812RGBW's own datasheet, see header
         * comment (including the note on library-vs-datasheet disagreement). */
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = w;
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_WS2805
        /* RGBW1W2 order — confirmed in WS2805's own datasheet, see header
         * comment (including the W1=warm/W2=cool convention this assumes). */
        pixel[0] = r;
        pixel[1] = g;
        pixel[2] = b;
        pixel[3] = w;
        pixel[4] = w2;
#endif
    }

    rmt_transmit_config_t tx_config = {};
    tx_config.loop_count = 0;
    esp_err_t err = rmt_transmit(addressable_light_rmt_chan, addressable_light_rmt_encoder,
                                  addressable_light_pixels, sizeof(addressable_light_pixels), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %d", err);
        return;
    }
    rmt_tx_wait_all_done(addressable_light_rmt_chan, pdMS_TO_TICKS(1000));
}
#endif

/* Drives the strip/fixture from the current on/off + level + hue/saturation
 * (or color temperature) state — the normal path, used by every attribute
 * change. */
static void set_output(void)
{
    float rf, gf, bf, wf, w2f;
    compute_pixel_fractions(&rf, &gf, &bf, &wf, &w2f);
#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_APA102
    transmit_pixel((uint8_t)(rf * 255.0f + 0.5f), (uint8_t)(gf * 255.0f + 0.5f), (uint8_t)(bf * 255.0f + 0.5f));
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
    /* SM2335EGH channels are 10-bit (0-1023), not 8-bit like every other
     * chip here — see the header comment. */
    transmit_pixel((uint16_t)(rf * 1023.0f + 0.5f), (uint16_t)(gf * 1023.0f + 0.5f), (uint16_t)(bf * 1023.0f + 0.5f),
                    (uint16_t)(wf * 1023.0f + 0.5f), (uint16_t)(w2f * 1023.0f + 0.5f));
#else
    transmit_pixel((uint8_t)(rf * 255.0f + 0.5f), (uint8_t)(gf * 255.0f + 0.5f), (uint8_t)(bf * 255.0f + 0.5f),
                    (uint8_t)(wf * 255.0f + 0.5f), (uint8_t)(w2f * 255.0f + 0.5f));
#endif
}

/* Flashes the whole strip/fixture white (on) or back to the real current
 * state (off) — used instead of a separate GPIO LED when identify_via_strip
 * is true. See the header comment on sharing the Identify LED with the
 * data pin for why. */
static void set_identify_flash(bool flash_on)
{
    if (!flash_on) {
        set_output(); /* restore actual on/off + color state */
        return;
    }
#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_APA102
    transmit_pixel(255, 255, 255);
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
    transmit_pixel(1023, 1023, 1023, 0, 0); /* full RGB white — leave CW/WW off */
#else
    transmit_pixel(255, 255, 255, 255, 255);
#endif
}

/* Toggles the identify indicator each time the timer fires — either the
 * strip/fixture itself (identify_via_strip) or a separate LED. */
static void identify_led_timer_cb(void *arg)
{
    static bool identify_led_state = false;
    identify_led_state = !identify_led_state;
    if (identify_via_strip) {
        set_identify_flash(identify_led_state);
    } else {
        gpio_set_level(IDENTIFY_LED_GPIO, identify_led_state ? 1 : 0);
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

/* Called whenever a controller changes an attribute — same
 * attribute::PRE_UPDATE pattern as firmware/color-light/'s (all attributes
 * here are plain ember attributes, no Delegate needed). */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                         uint32_t cluster_id, uint32_t attribute_id,
                                         esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != attribute::PRE_UPDATE || endpoint_id != addressable_light_endpoint_id) {
        return ESP_OK;
    }

    if (cluster_id == OnOff::Id && attribute_id == OnOff::Attributes::OnOff::Id) {
        light_on = val->val.b;
        set_output();
        ESP_LOGI(TAG, "Light turned %s", light_on ? "ON" : "OFF");
    } else if (cluster_id == LevelControl::Id && attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
        if (!val->val.u8) {
            return ESP_OK; /* null CurrentLevel has no defined brightness — ignore, keep last value */
        }
        light_level = val->val.u8;
        set_output();
        ESP_LOGI(TAG, "Light level set to %u/254", light_level);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentHue::Id) {
        light_hue = val->val.u8;
        light_color_source = COLOR_SOURCE_HS;
        set_output();
        ESP_LOGI(TAG, "Hue set to %u/254", light_hue);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentSaturation::Id) {
        light_saturation = val->val.u8;
        light_color_source = COLOR_SOURCE_HS;
        set_output();
        ESP_LOGI(TAG, "Saturation set to %u/254", light_saturation);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentX::Id) {
        light_x = val->val.u16;
        light_color_source = COLOR_SOURCE_XY;
        set_output();
        ESP_LOGI(TAG, "Color x set to %u/65536", light_x);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::CurrentY::Id) {
        light_y = val->val.u16;
        light_color_source = COLOR_SOURCE_XY;
        set_output();
        ESP_LOGI(TAG, "Color y set to %u/65536", light_y);
    } else if (cluster_id == ColorControl::Id && attribute_id == ColorControl::Attributes::ColorTemperatureMireds::Id) {
        light_mireds = val->val.u16;
        light_color_source = COLOR_SOURCE_CT;
        set_output();
        ESP_LOGI(TAG, "Color temperature set to %u mireds", light_mireds);
    }
    return ESP_OK;
}

/* Called when a controller asks the device to "identify" itself — starts
 * or stops the identify indicator accordingly (strip-flash or separate
 * LED, see identify_via_strip). */
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
        if (identify_via_strip) {
            set_output(); /* restore actual state instead of a bare "off" */
        } else {
            gpio_set_level(IDENTIFY_LED_GPIO, 0);
        }
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

    identify_via_strip = (IDENTIFY_LED_GPIO == ADDRESSABLE_LIGHT_GPIO);

    /* 2. Configure the peripheral that drives the strip/fixture — RMT for
     * the six pixel-chain NRZ chips, real SPI for APA102, bit-banged GPIO
     * for SM2335EGH — see the header comment for why, and for the exact
     * API pattern's sourcing. */
#if ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_APA102
    spi_bus_config_t spi_bus_config = {};
    spi_bus_config.mosi_io_num = ADDRESSABLE_LIGHT_GPIO;
    spi_bus_config.sclk_io_num = ADDRESSABLE_LIGHT_CLOCK_GPIO;
    spi_bus_config.miso_io_num = -1;
    spi_bus_config.quadwp_io_num = -1;
    spi_bus_config.quadhd_io_num = -1;
    spi_bus_config.max_transfer_sz = sizeof(addressable_light_pixels);
    ESP_ERROR_CHECK(spi_bus_initialize(ADDRESSABLE_LIGHT_APA102_SPI_HOST, &spi_bus_config, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_dev_config = {};
    spi_dev_config.clock_speed_hz = ADDRESSABLE_LIGHT_APA102_SPI_HZ;
    spi_dev_config.mode = 0;
    spi_dev_config.spics_io_num = -1; /* APA102 has no chip-select pin */
    spi_dev_config.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(ADDRESSABLE_LIGHT_APA102_SPI_HOST, &spi_dev_config, &addressable_light_spi_dev));
#elif ADDRESSABLE_LIGHT_CHIP == ADDRESSABLE_LIGHT_CHIP_SM2335
    gpio_config_t sm2335_io_conf = {};
    sm2335_io_conf.pin_bit_mask = (1ULL << ADDRESSABLE_LIGHT_GPIO) | (1ULL << ADDRESSABLE_LIGHT_CLOCK_GPIO);
    sm2335_io_conf.mode = GPIO_MODE_OUTPUT;
    gpio_config(&sm2335_io_conf);
    /* Idle state is both lines high — matches ESPHome's own setup(). */
    gpio_set_level(ADDRESSABLE_LIGHT_GPIO, 1);
    gpio_set_level(ADDRESSABLE_LIGHT_CLOCK_GPIO, 1);
#else
    rmt_tx_channel_config_t tx_chan_config = {};
    tx_chan_config.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_chan_config.gpio_num = ADDRESSABLE_LIGHT_GPIO;
    tx_chan_config.mem_block_symbols = 64;
    tx_chan_config.resolution_hz = ADDRESSABLE_LIGHT_RMT_RESOLUTION_HZ;
    tx_chan_config.trans_queue_depth = 4;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &addressable_light_rmt_chan));

    rmt_simple_encoder_config_t simple_encoder_config = {};
    simple_encoder_config.callback = addressable_light_rmt_encode;
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&simple_encoder_config, &addressable_light_rmt_encoder));

    ESP_ERROR_CHECK(rmt_enable(addressable_light_rmt_chan));
#endif
#if ADDRESSABLE_LIGHT_CHIP != ADDRESSABLE_LIGHT_CHIP_SM2335
    memset(addressable_light_pixels, 0, sizeof(addressable_light_pixels));
#endif
    set_output(); /* light_on starts false, so this drives everything off. */

    /* 2b. Configure the identify LED + its blink timer (not started yet —
     * only runs while a controller has an identify request active). Only
     * gpio_config's a separate pin when NOT sharing the data pin — see the
     * header comment for why configuring both would conflict. */
    if (!identify_via_strip) {
        gpio_config_t identify_io_conf = {};
        identify_io_conf.pin_bit_mask = (1ULL << IDENTIFY_LED_GPIO);
        identify_io_conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&identify_io_conf);
        gpio_set_level(IDENTIFY_LED_GPIO, 0);
    }

    const esp_timer_create_args_t identify_timer_args = {
        .callback = &identify_led_timer_cb,
        .name = "identify_led",
    };
    esp_timer_create(&identify_timer_args, &identify_led_timer);

    /* 3. Build the Matter data model: one node, one hand-assembled
     * Extended Color Light endpoint (Identify + Groups + OnOff +
     * LevelControl + ColorControl[HueSaturation, +ColorTemperature for the
     * two RGBCCT chips] + ScenesManagement) — identical composition to
     * firmware/color-light/'s, see that file's header comment for why this
     * isn't endpoint::extended_color_light::create(). */
    node::config_t node_config;
    /* NodeLabel (Basic Information cluster) — left empty by default in
     * esp-matter's own config_t, which is exactly why Apple Home proposed
     * the generic "Matter Accessory" as this device's suggested name
     * instead of anything specific. Harmless (a controller can always
     * rename it), but a better default costs nothing. */
    strncpy(node_config.root_node.basic_information.node_label, "Addressable Light",
            sizeof(node_config.root_node.basic_information.node_label) - 1);
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    endpoint_t *ep = endpoint::create(node, ENDPOINT_FLAG_NONE, NULL);
    if (!ep) {
        ESP_LOGE(TAG, "Failed to create endpoint");
        return;
    }

    /* Every one of esp-matter's own top-level endpoint helpers
     * (endpoint::on_off_light::create(), endpoint::contact_sensor::create(),
     * etc.) goes through a shared `common::create<T>()` template that
     * creates a Descriptor cluster BEFORE calling the device type's own
     * add() — confirmed by reading esp_matter_endpoint.cpp's own
     * `common::create<T>()` directly. This endpoint is hand-assembled from
     * lower-level free functions instead (see the header comment on why),
     * which skipped that step entirely — the one thing every other device
     * type in this repo gets for free by using a complete top-level helper.
     * Without a Descriptor cluster, a controller has no standard way to
     * discover what device type/clusters even exist on this endpoint —
     * add_device_type() below only updates this SDK's own internal
     * bookkeeping (confirmed by reading its implementation: it just appends
     * to the endpoint's device_types[] array), it does NOT itself create or
     * populate a Descriptor cluster object a controller can read. This is
     * almost certainly the real reason Apple Home's HomeKit-Matter bridge
     * showed this device as "Niet geschikt"/"Not compatible" with no
     * control tile across every attempt so far, regardless of which
     * ColorControl attributes/features were also fixed above — Home
     * Assistant's more lenient Matter implementation apparently tolerated
     * the missing Descriptor cluster, which is why this was never caught
     * there. */
    cluster::descriptor::config_t descriptor_config;
    cluster_t *descriptor_cluster = cluster::descriptor::create(ep, &descriptor_config, CLUSTER_FLAG_SERVER);
    if (!descriptor_cluster) {
        ESP_LOGE(TAG, "Failed to create descriptor cluster");
        return;
    }

    add_device_type(ep, ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_ID, ESP_MATTER_EXTENDED_COLOR_LIGHT_DEVICE_TYPE_VERSION);

    cluster::identify::config_t identify_config;
    identify_config.identify_type = chip::to_underlying(Identify::IdentifyTypeEnum::kVisibleIndicator);
    cluster_t *identify_cluster = cluster::identify::create(ep, &identify_config, CLUSTER_FLAG_SERVER);
    cluster::identify::command::create_trigger_effect(identify_cluster);

    cluster::groups::config_t groups_config;
    cluster::groups::create(ep, &groups_config, CLUSTER_FLAG_SERVER);

    cluster::on_off::config_t on_off_config;
    cluster::on_off::feature::lighting::config_t on_off_lighting_config;
    cluster_t *on_off_cluster = cluster::on_off::create(ep, &on_off_config, CLUSTER_FLAG_SERVER);
    cluster::on_off::feature::lighting::add(on_off_cluster, &on_off_lighting_config);
    cluster::on_off::command::create_on(on_off_cluster);
    cluster::on_off::command::create_toggle(on_off_cluster);

    cluster::level_control::config_t level_control_config;
    level_control_config.current_level = nullable<uint8_t>(ADDRESSABLE_LIGHT_DEFAULT_LEVEL);
    /* OnLevel deliberately left null (nullable<uint8_t>()'s default
     * constructor — confirmed to actually produce a null value by reading
     * esp_matter_attribute_utils.h directly), NOT a concrete level. A real,
     * reported bug: setting a concrete OnLevel here (as this line used to)
     * means connectedhomeip's own OnOff/LevelControl interaction handler
     * (emberAfOnOffClusterLevelControlEffectCallback(), confirmed by
     * reading codegen/level-control.cpp directly) forces CurrentLevel to
     * OnLevel on every single OnOff::On — so dimming to e.g. 21%, turning
     * off, then back on, would always jump back to whatever OnLevel was
     * instead of staying at 21%. With OnLevel null, that same handler
     * instead restores the level that was set right before the light went
     * off — the "remember brightness" behavior every real dimmable light
     * has, confirmed by reading that exact fallback path in the SDK. */
    level_control_config.on_level = nullable<uint8_t>();
    cluster::level_control::feature::lighting::config_t level_control_lighting_config;
    level_control_lighting_config.start_up_current_level = nullable<uint8_t>(ADDRESSABLE_LIGHT_DEFAULT_LEVEL);
    cluster_t *level_control_cluster = cluster::level_control::create(ep, &level_control_config, CLUSTER_FLAG_SERVER);
    cluster::level_control::feature::on_off::add(level_control_cluster);
    cluster::level_control::feature::lighting::add(level_control_cluster, &level_control_lighting_config);

    /* ColorControl's XY and ColorTemperature features are BOTH mandatory
     * conformance for the ExtendedColorLight device type — confirmed
     * directly against the CSA's own data_model/1.6/device_types/
     * ExtendedColorLight.xml (`<feature code="XY"><mandatoryConform/>`,
     * `<feature code="CT"><mandatoryConform/>`; HueSaturation itself is
     * only `<optionalConform/>`), fetched from inside this SDK image
     * rather than assumed. This was NOT implemented at first — deliberately
     * HueSaturation-only, reasoning that most controllers' color wheels
     * drive Hue/Saturation directly (still true, and still supported here
     * as an extra) — which passed Home Assistant's lenient Matter
     * integration but was real hardware testing via Apple Home that
     * surfaced the gap: HomeKit's Matter bridge enforces device-type
     * conformance strictly and refused to expose ANY control tile at all
     * (paired fine, then showed "Niet geschikt" / "Not compatible", no
     * error a controller-side log would show) for an endpoint declaring
     * ExtendedColorLight without its two mandatory color features. All
     * three (HS/XY/CT) are now implemented; see light_color_source and
     * xy_to_rgb()/mireds_to_rgb_approx() above for how each renders to the
     * actual output. */
    cluster::color_control::config_t color_control_config;
    color_control_config.color_mode = chip::to_underlying(ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation);
    color_control_config.enhanced_color_mode = chip::to_underlying(ColorControl::ColorModeEnum::kCurrentHueAndCurrentSaturation);
    color_control_config.color_capabilities = chip::to_underlying(ColorControl::ColorCapabilitiesBitmap::kHueSaturation) |
        chip::to_underlying(ColorControl::ColorCapabilitiesBitmap::kXy) |
        chip::to_underlying(ColorControl::ColorCapabilitiesBitmap::kColorTemperature);
    cluster_t *color_control_cluster = cluster::color_control::create(ep, &color_control_config, CLUSTER_FLAG_SERVER);
    cluster::color_control::feature::hue_saturation::config_t hue_saturation_config;
    hue_saturation_config.current_hue = ADDRESSABLE_LIGHT_DEFAULT_HUE;
    hue_saturation_config.current_saturation = ADDRESSABLE_LIGHT_DEFAULT_SATURATION;
    cluster::color_control::feature::hue_saturation::add(color_control_cluster, &hue_saturation_config);
    cluster::color_control::feature::xy::config_t xy_config;
    xy_config.current_x = light_x;
    xy_config.current_y = light_y;
    cluster::color_control::feature::xy::add(color_control_cluster, &xy_config);
    cluster::color_control::feature::color_temperature::config_t color_temperature_config;
    color_temperature_config.color_temperature_mireds = ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS;
    color_temperature_config.color_temp_physical_min_mireds = ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS;
    color_temperature_config.color_temp_physical_max_mireds = ADDRESSABLE_LIGHT_WARM_WHITE_MIREDS;
    color_temperature_config.couple_color_temp_to_level_min_mireds = ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS;
    color_temperature_config.start_up_color_temperature_mireds = nullable<uint16_t>((uint16_t)ADDRESSABLE_LIGHT_COOL_WHITE_MIREDS);
    cluster::color_control::feature::color_temperature::add(color_control_cluster, &color_temperature_config);

    /* RemainingTime is a global ColorControl attribute (ms left in an
     * in-progress color/level transition, independent of which color
     * feature is enabled) — mandatory per the Matter spec, and esp-matter's
     * own endpoint::extended_color_light::add() always creates it
     * explicitly via color_control::attribute::create_remaining_time()
     * (confirmed by reading esp_matter_endpoint.cpp directly), separate
     * from anything feature::hue_saturation::add()/xy::add()/
     * color_temperature::add() set up on their own. This hand-assembled
     * endpoint had been missing it — harmless against Home Assistant, but
     * real hardware testing via Apple Home surfaced it: Apple's stricter
     * HomeKit-Matter bridge silently refused to expose ANY control tile at
     * all for a ColorControl cluster missing this mandatory attribute
     * (paired fine, then showed "Niet geschikt" / "Not compatible" and
     * auto-removed its own fabric — no error a controller-side log would
     * show, only visible by comparing against esp-matter's own reference
     * endpoint construction). 0 = no transition currently in progress. */
    cluster::color_control::attribute::create_remaining_time(color_control_cluster, 0);

    cluster::scenes_management::config_t scenes_management_config;
    cluster_t *scenes_management_cluster = cluster::scenes_management::create(ep, &scenes_management_config, CLUSTER_FLAG_SERVER);
    cluster::scenes_management::command::create_copy_scene(scenes_management_cluster);
    cluster::scenes_management::command::create_copy_scene_response(scenes_management_cluster);

    addressable_light_endpoint_id = endpoint::get_id(ep);
    ESP_LOGI(TAG, "Addressable light endpoint id: %u", addressable_light_endpoint_id);

    /* Occupancy Sensing (client) — optionalConform on ExtendedColorLight.xml
     * (Matter Device Types Reference audit, see CLAUDE.md's own "Open next
     * steps"). Same NULL-config/CLUSTER_FLAG_CLIENT shape firmware/light/'s
     * own identical addition already establishes — see that file's own
     * comment for the full detail on why no app code reacts to it directly. */
    cluster::occupancy_sensing::create(ep, NULL, CLUSTER_FLAG_CLIENT);

    /* CurrentLevel/CurrentHue/CurrentSaturation/CurrentX/CurrentY/
     * ColorTemperatureMireds all change rapidly while a controller is
     * actively dragging a brightness/color slider — deferring their NVS
     * persistence avoids writing flash on every single step. Same call
     * firmware/color-light/ and firmware/dimmable-light/ make, for the
     * same reason. */
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, LevelControl::Id,
                                                        LevelControl::Attributes::CurrentLevel::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentHue::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentSaturation::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentX::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::CurrentY::Id));
    attribute::set_deferred_persistence(attribute::get(addressable_light_endpoint_id, ColorControl::Id,
                                                        ColorControl::Attributes::ColorTemperatureMireds::Id));

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

    ESP_LOGI(TAG, "Matter addressable light started. Scan the QR code to commission.");
}
