# Line6 Floorboard ESP32-S3 Wireless MIDI Controller

Turn a Line6 Floorboard into a wireless **BLE MIDI** foot controller using an **ESP32-S3** and a few hardware mods.  
This firmware can also be used on a custom footswitch rig. You just need your own switches, LEDs, expression pedals, and the proper resistors where needed.

## Features

- **Wireless BLE MIDI** controller for Windows
- **9 footswitch inputs** with matching LED feedback
- **2 expression pedal inputs** with smoothing and stable MIDI output
- **Persistent pedal calibration** stored in flash
- **Runtime calibration mode** triggered directly from the pedalboard
- **Per-pedal filtering** using calibrated millivolt readings
- Sends **undefined CC messages**, ideal for MIDI learn in DAWs, amp sims, and plugins

## Hardware Modifications

### Step 1: Footswitch Board

1. **Remove resistors** at the highlighted locations (in the central part of the board) and solder wires to the points indicated. These wires become your switch inputs. Pads with the same SW number are already shorted together, so you can use whichever one is most convenient.
   ![Footswitch board](pics/line6_board_switches.jpg)
2. **Connect each switch wire** to ESP32-S3 GPIO pins of your choice. Be sure to read the **Pin Notes** section at the end before wiring them.
3. **LEDs**: Use continuity / diode mode on your multimeter between GND and each pin of the onboard LED connector to identify LED pins. If an LED doesn’t light up, probe between GND and the solder pad on the board side of its resistor, not directly at the LED.  
   Each LED already has a current-limiting resistor, so you only need GPIO, power, and GND.

### Step 2: Expression Pedals & Wah

1. **Solder wires** to the pedal board’s potentiometer output, VCC, and GND. Also tap the onboard connector for the wah LED pin.  
   ![Expression board](pics/line6_board_pedals.jpg)
2. **Connect** all pedal wires to the ESP32-S3 pins used by the sketch.

The completed board should look something like this. More closeups are available in the `hw modifications` folder.  
![](pics/view_1.jpg)

## Software Setup

1. Install **Arduino IDE** and the **ESP32 board package**.
2. Select **ESP32S3 Dev Module**.
3. Install the updated BLE MIDI library for this project:  
   **[ESP32-BLE-MIDI-NimBLE2](https://github.com/znk-ee/ESP32-BLE-MIDI-NimBLE2)**  
   This is an updated fork of the original ESP32 BLE MIDI library, made compatible with **NimBLE-Arduino 2.x** while preserving the original API style and usage.
4. Install **BLE-MIDI Connect** from the Microsoft Store:  
   [https://apps.microsoft.com/detail/9NVMLZTTWWVL](https://apps.microsoft.com/detail/9NVMLZTTWWVL)
5. Install **Windows MIDI Services**:  
   [https://microsoft.github.io/MIDI/](https://microsoft.github.io/MIDI/)

### Notes for Windows

Recent versions of **BLE-MIDI Connect** make **loopMIDI unnecessary for this project**, so it is no longer part of the recommended setup.  
Windows MIDI Services is required for the current Windows workflow and also provides newer MIDI infrastructure, tools, loopbacks, and diagnostics.

## Firmware

Upload `HACKEDLine6Floorboard.ino` to the board.

### What the firmware does

- Sends **CC 102 to 110** for the 9 footswitches
- Sends:
  - **CC 1** for the wah / modulation pedal
  - **CC 7** for the volume pedal
- Uses **`analogReadMilliVolts()`** with explicit ADC configuration
- Filters pedal input with:
  - oversampling
  - trimmed averaging
  - moving average smoothing
- Stores calibration values in non-volatile flash so they survive reboot and power loss

## Calibration

Calibration is built into the firmware and no manual code editing is needed. The calibration mode can be entered holding **TUNER + CHANNEL SEL** together, both during booth or while the unit is already running. After holding the combo for about 1.5 seconds, calibration mode starts. The calibration procedure is the following:

1. Move both pedals to **one end-stop**
2. Press **TUNER** to capture the first point
3. Move both pedals to the **opposite end-stop**
4. Press **CHANNEL SEL** to capture the second point
5. The firmware saves the values automatically

During calibration, the **WAH LED blinks the whole time**, the **TUNER LED blinks for the first checkpoint**, the **CHANNEL SEL LED blinks for the second checkpoint**, and **all LEDs blink at the end** to confirm completion. Calibration is performed in **millivolts** instead of hardcoded raw ADC values, which gives much more consistent pedal behavior and avoids range differences between reboots.

## Usage

1. Power on the controller
2. Open **BLE-MIDI Connect**
3. Connect to `Line6Floorboard`
4. In your DAW or plugin host, select the BLE MIDI endpoint as input
5. Use MIDI learn to assign the switches and pedals

That’s it.  
![](pics/midi-ble-screenshot.png)

## Board Configuration and Pin Notes

Use **`ESP32S3 Dev Module`** and make sure the Arduino IDE settings match the actual module variant printed on your board. For example, an **N16R8** module has **16 MB flash** and **8 MB PSRAM**, so a good starting point is:

- **Flash Size:** `16MB`
- **Partition Scheme:** one of the **16M Flash** layouts
- **PSRAM:** enabled, matching the module’s PSRAM configuration
- **CPU Frequency:** `240MHz`

If PSRAM causes instability, check your wiring and pin assignments before disabling it. Also, always verify **reserved pins** in the datasheet and board documentation before wiring anything, especially on ESP32-S3 boards with flash and PSRAM, because some GPIOs may be used internally and must not be reused. On some ESP32-S3 WROOM variants, for example, **GPIO35, GPIO36, and GPIO37** may be reserved, and using them can cause crashes, watchdog resets, boot failures, or strange BLE/ADC behavior. More generally, do not assume every exposed header pin is safe: check whether a pin is used for **flash, PSRAM, USB, boot strapping, or other special functions**. Calibration values are stored in flash, so they survive reboots and power cycles.

## Changelog

### Latest

- Added **proper pedal calibration** directly in firmware
- Added **persistent storage** for calibration data using flash preferences
- Switched pedal reading to **`analogReadMilliVolts()`**
- Added **explicit ADC setup**
- Refactored pedal handling into dedicated per-pedal logic
- Improved pedal stability with **oversampling + trimmed mean + moving average**
- Added **runtime calibration trigger**
- Added **calibration LED guidance**:
  - WAH blinks during calibration
  - TUNER blinks for the first capture
  - CHANNEL SEL blinks for the second capture
  - all LEDs blink when calibration finishes
- Updated Windows setup:
  - **loopMIDI is no longer required**
  - **Windows MIDI Services is now part of the recommended setup**
- Updated the recommended BLE MIDI library to **ESP32-BLE-MIDI-NimBLE2** for compatibility with current **NimBLE-Arduino 2.x** releases

## Credits

- BLE MIDI transport for ESP32-S3
- Original Line6 Floorboard hardware reuse
- BLE-MIDI Connect by locomorange
- Windows MIDI Services by Microsoft
- **ESP32-BLE-MIDI-NimBLE2** compatibility fork for newer NimBLE versions
