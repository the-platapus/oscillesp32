# oscillesp32 〰️

**An oscilloscope made from an ESP32, a tiny OLED, and stubbornness.**

You do electronics. You don't own a scope. Your multimeter says "3.3 V" with
total confidence while the signal underneath is doing interpretive dance.
This project turns the parts in your drawer into a single-channel
oscilloscope, frequency counter and signal generator. It shows waveforms on a
128×64 OLED and streams them over WiFi to a proper scope UI on your phone or
laptop.

It is not a Rigol. It does, however, cost less than lunch and fit in a mint
tin.

```
   ┌──┐  ┌──┐  ┌──┐  ┌──┐
   │  │  │  │  │  │  │  │     1.000kHz  3.00Vpp  50%
 ──┘  └──┘  └──┘  └──┘  └──   (actual OLED readout, minus the pixels)
```

## Shopping list (you probably already own it)

- 1 × ESP32 dev board (NodeMCU-32S / DevKitC / WROOM-32, the common kind)
- 1 × SSD1306 128×64 I2C OLED
- A few resistors for the input front-end (see below)
- A jumper wire, for the self-test
- Optional: 3 push buttons, if you like knobs. The browser UI does everything
  without them.

## Will it scope? 🔍

| Thing you want to look at | Verdict |
|---|---|
| Audio, PWM, motor drivers, servo pulses | ✅ yes, happily |
| Power-supply ripple and sag, battery behaviour under load | ✅ yes |
| 555 timers, oscillators, "is this clock even running?" | ✅ and it reads the frequency crystal-accurately |
| Sensors, relays, solenoids, button bounce | ✅ yes, roll mode is great for slow stuff |
| UART up to ~50 kbaud | ✅ squint a little |
| SMPS switching nodes, fast SPI/I2C edges, USB | ❌ too fast for a ~1 MS/s ADC, no firmware can fix physics |
| Anything on the mains side | ❌❌ absolutely not. See the serious section. |

### The numbers

| | |
|---|---|
| Sample rate | ~1 MS/s of real, evenly spaced samples (the reference board measured 992,188 S/s). `selftest` finds your chip's best setting, up to ~2 MS/s if it can manage it. |
| Resolution | 12-bit, averaged up to sub-LSB on slow timebases |
| Timebase | 5 µs/div … 5 s/div, roll mode from 100 ms/div |
| Bandwidth | waveform shape up to ~100 kHz; frequency counter up to ~400 kHz |
| Frequency counter | runs on every sample, crystal-accurate after `selftest` (~1 ppm) |
| Input | 0–3.1 V direct, about ±15 V with a 3-resistor front-end |
| Trigger | rising/falling edge, Auto / Normal / Single, 5–95 % pre-trigger, sub-sample interpolation (no jitter dance) |
| Measurements | frequency, period, duty, Vpp, Vmax, Vmin, Vavg, Vrms, AC rms, DC |
| Extras | autoset, FFT, cursors, persistence, CSV/PNG export, multimeter-style meter view |
| Signal generator | square/PWM 1 Hz–10 MHz on GPIO26 (crystal accurate), sine 130 Hz–55 kHz on GPIO25 (~1.6 Vpp, roughly the frequency you asked for) |

## ⚠️ The one serious section

Everything else in this README is allowed to be silly. This bit is not.

- **Never connect it to mains**, or to anything on the mains side of a power
  supply. That includes the big capacitor and the switching transistor in
  switch-mode supplies. That job needs an isolated/differential probe and a
  rated instrument.
- **The probe ground is the ESP32's GND**, which is your laptop's USB ground,
  which is often mains earth. Clip it only to the circuit's ground. Clip it
  anywhere else and you've just shorted that point to earth (sometimes with
  sparks).
  (Real bench scopes have exactly the same rule.)
- **A power bank plus the WiFi UI isolates you from your laptop.** It does not
  make mains work safe: the board then floats at whatever voltage the circuit
  is at.

## Wiring

```
                 ESP32 dev board
             ┌──────────────────────────────┐
  OLED SDA ──┤ GPIO21                       │
  OLED SCL ──┤ GPIO22           GPIO34  ├── probe input (through a front-end)
  OLED VCC ──┤ 3V3              GND     ├── probe ground clip
  OLED GND ──┤ GND              GPIO26  ├── square / PWM out (also the test signal)
             │                  GPIO25  ├── sine out
  optional:  │ GPIO27 ── button ── GND  (SEL)
  buttons    │ GPIO14 ── button ── GND  (UP)
             │ GPIO13 ── button ── GND  (DOWN)
             │ GPIO0  = the BOOT button already on the board
             └──────────────────────────────┘
```

Wired your OLED somewhere else? It also hunts on 22/23, 4/15, 5/4 and 18/19
until something answers. Every pin lives in `src/config.h`.

### Front-end A: direct (0 to 3.1 V)

```
probe ──[ 1 kΩ ]──┬── GPIO34
                  │
               (optional 100 pF to GND)
probe GND ─────────── GND
```

The default (`fe direct`). Only for signals already inside 0–3.1 V. The 1 kΩ
is a seatbelt.

### Front-end B: bias divider (about ±15 V, AC or DC)

```
                    3V3
                     │
                  [ 20 kΩ ]
                     │
probe ──[ 100 kΩ ]───┼──────── GPIO34
                     │    │
                  [ 20 kΩ ] (100 pF)
                     │    │
                    GND  GND
```

The pin sees `1.5 V + Vin / 11`, so roughly −15 V … +18 V lands in the ADC's
happy zone, and negative voltages work. Input impedance is ~110 kΩ. The
100 kΩ keeps fault current under a milliamp even at ±30 V. The 100 pF is
optional: it calms the ADC's sample-and-hold and rolls off around 170 kHz,
which is about where the ADC gives up anyway. Resistor tolerances don't
matter, because calibration eats them.

Want 1 MΩ input like a real scope? Put an MCP6002 (rail-to-rail, 3.3 V) as a
buffer between a 1 MΩ divider and GPIO34, then recalibrate.

## Build and flash

You need ESP-IDF 4.4. Don't have it? No problem: `build.sh` runs the build in
a Docker image with ESP-IDF 4.4.8 baked in (`sle118/squeezelite-esp32-idfv448:4.4.8`).
If you do have a local `idf.py`, it uses that instead.

```bash
./build.sh
```

```bash
./flash.sh /dev/ttyUSB0
```

```bash
./monitor.sh /dev/ttyUSB0
```

`./build.sh --flash /dev/ttyUSB0` does both. `flash.sh` writes in 128 KB slices
at 115200 baud with retries, because cheap USB-serial chips get bored halfway
through long transfers. Prefer a web flasher? Use
`build/oscillesp32_factory_0x0.bin` at offset 0x0.

## First boot: let it calibrate itself

1. Flash it, plug in the OLED. You'll see "calibrating ADC…", then a scope.
2. **Jumper GPIO26 to GPIO34.** On the usual 38-pin boards both are on the EN
   side: counting from EN, the pins go VP(36), VN(39), **34**, 35, 32, 33, 25,
   **26**. (Yes, it's easy to grab 35 by mistake. Ask us how we know.)
3. Type `selftest` in the serial monitor, or press *Self-test* in the web UI.
   It takes about 10 seconds and it:
   - checks the wire with plain logic levels first (`jumpercheck` runs just this bit);
   - uses GPIO26 as a DAC to put known voltages on the input, then tries ADC
     rates and timings until it finds the fastest one that still reads them
     like a slow, careful reference;
   - spots whether your chip is sending each sample twice (some do, see below);
   - checks sample order and polarity;
   - measures a crystal-accurate 10 kHz signal and corrects the timebase;
   - saves all of it.
4. Pull the jumper. You now own a calibrated oscilloscope.

Built front-end B? Calibrate it too:

```
fe bias
calzero          ← with the probe shorted to its ground clip
calgain 5.02     ← with the probe on a known voltage (use your multimeter's reading)
```

## Driving it

### From your phone or laptop

Join WiFi **ESP32-Scope** (password `scope1234`) and open **http://192.168.4.1**.

Prefer it on your home network? Use *WiFi → Join network* on the page, or type
`wifi join "SSID" "password"` on the serial console. It then lives at
**http://scope.local**, and the OLED Info screen shows its IP. If it can't
join, it sulks back to its own access point.

On the waveform:

- drag the **right edge** to move the trigger level
- drag the **left edge** to move the trace up and down
- drag the **top edge** to move the trigger point
- **scroll** for time/div, **Shift + scroll** for volts/div

There's also Run/Stop, Single, Autoset, cursors, FFT, persistence, CSV/PNG
export, the signal generator, calibration and a console.

### From the board itself

| Button | Click | Double-click | Hold |
|---|---|---|---|
| BOOT | next screen: Scope → Meter → FFT → Info | Run / Stop | Autoset |
| SEL (GPIO27) | next setting | | previous setting |
| UP / DOWN (GPIO14 / GPIO13) | change it | | change it faster |

Only got the BOOT button? Hold it for autoset and flip to the Meter screen.
Big digits, frequency, Vpp, Vrms, duty. That covers half of all repair jobs.

On the Scope screen, the top line shows time/div, volts/div, trigger
mode/edge/level and state (TRIG / AUTO / WAIT / ROLL / STOP). The bottom line
shows frequency, Vpp and duty. The left arrow marks 0 V, the right arrow the
trigger level.

### Commands

Same commands on the serial console, the web console, and
`http://<ip>/cmd?c=<command>` if you want to script it:

```
run | stop | single | autoset
tdiv <idx|+|-|1ms>      vdiv <idx|+|-|0.5V>
vpos <V>  hpos <0.05..0.95>  coupling dc|ac|toggle
tmode auto|normal|single  tedge rise|fall|toggle  tlevel <V>
view scope|meter|fft|info|next
fe direct|bias|custom   calzero   calgain <V>   cal <zero_mV> <gain>
gen sq <Hz> [duty%] | gen sq off | gen sine <Hz> | gen sine off
wifi ap | wifi off | wifi join "ssid" "password"
selftest | jumpercheck | adcdiag | adccfg <rate> [sar_div] [cycles] [dup]
status | save | factory | help
```

`adcdiag` shows exactly what the ADC is delivering right now, including the
real sample rate and how hard the CPU is working. The reference board runs the
sampling task at 43 % of one core.

## Things the ESP32 didn't mention

Bringing this up on real hardware turned up some surprises. `selftest`
handles them all for you, but in case you're building your own:

- **The I2S DMA swaps every pair of samples.** Sample 1 arrives before sample 0.
  Every time. Undo it or your square waves grow spikes.
- **Some settings send each sample twice.** At one setting the "1 MS/s" stream
  was really 500 kS/s with every value doubled. Worse, whether a given setting
  doubles can change from one start to the next. The firmware checks at every
  start and uses one sample of each pair when needed, which is evenly spaced
  either way.
- **The rate is 256/258 of what you asked for.** The ADC takes a 2-sample
  breather every 256 conversions. 1 MS/s requested = 992,248 S/s delivered.
- **ESP-IDF 4.4 turns your DACs off behind your back.** `i2s_driver_uninstall()`
  disables both DAC outputs. If you restart the ADC, re-enable them, or your
  sine generator quietly stops.
- **A floating ADC pin reads ~0.2 V with random spikes.** If the self-test says
  your 3.3 V square peaks at 985 codes, check which pin your jumper is in
  before blaming silicon. (See: "ask us how we know".)

## How it works

- `acquisition.cpp`: I2S0 drives ADC1 channel 6 (GPIO34) into DMA,
  continuously. Every sample goes through:
  - a frequency counter with an automatic threshold, hysteresis and sub-sample
    edge timing, averaged over whole periods;
  - a boxcar decimator for slow timebases (free noise reduction);
  - an edge trigger with a pre-trigger ring buffer and fractional trigger
    position, so the trace sits still.

  It also holds the self-tuner described above.
- `measure.cpp`: codes → volts via the chip's eFuse calibration plus your
  front-end calibration; measurements; FFT.
- `web.cpp` + `webpage.h`: WiFi, HTTP, and a WebSocket at `/ws` streaming
  binary frames at up to 25 fps to a single-page UI.
- `display.cpp` + `ssd1306.h`: the OLED.
- `commands.cpp`: one command language for serial, web and buttons.
- Settings live in NVS, saved two seconds after you stop fiddling.

## Tests (no hardware required)

```bash
./test/run_host_sim.sh
```

```bash
./test/run_host_oled.sh
```

The first compiles the real acquisition code for your PC (with fake ESP-IDF
headers) and feeds it synthetic signals with noise, swapped pairs and doubled
samples. It checks counter accuracy, trigger stability, roll, single shot,
auto/normal, autoset, AC coupling and FFT: 46 checks. The second renders every
OLED screen to `build/oled_preview/*.pbm`, so you can admire the pixels
without soldering anything.

## Project layout

```
src/            firmware (ESP-IDF component)
test/           host simulation + OLED preview, with stub ESP-IDF headers
build.sh        build (local ESP-IDF or Docker)
flash.sh        flash in slices with retries
monitor.sh      serial console
sdkconfig.defaults   the only ESP-IDF config you need to keep
```

## License

MIT © 2026 Abdullah Aamir. Go build one. Then go fix something with it.
