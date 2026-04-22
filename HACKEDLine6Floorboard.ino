#include <Arduino.h>
#include <ezButton.h>
#include <BLEMidi.h>
#include <Preferences.h>
#include <esp32-hal-cpu.h>

// MIDI settings
constexpr uint8_t MIDI_CHANNEL = 15;      // 0-15 => MIDI ch 1-16, so this is channel 16
constexpr uint8_t CC_MODULATION = 1;      // Wah
constexpr uint8_t CC_VOLUME = 7;          // Volume
constexpr uint8_t BUTTON_CC_BASE = 102;   // Footswitches send CC 102..110

// Buttons / LEDs
// TUNER - CHANNEL SEL - A - B - C - UP - DOWN - D - WAH
constexpr uint8_t BUTTON_NUM = 9;
constexpr uint8_t LED_NUM = 9;

constexpr uint8_t BTN_TUNER = 0;
constexpr uint8_t BTN_CHANNEL_SEL = 1;
constexpr uint8_t BTN_WAH = 8;

// Button pins
const int buttonPins[BUTTON_NUM] = {4, 5, 6, 7, 15, 16, 17, 18, 8};

// LED pins
const int ledPins[LED_NUM] = {1, 39, 38, 2, 21, 48, 45, 47, 40};
constexpr int LED_CONN_PIN = 42;

// Pedal pins
constexpr uint8_t WAH_PEDAL_PIN = 12;
constexpr uint8_t VOLUME_PEDAL_PIN = 13;

// ADC / pedal tuning
constexpr uint8_t ADC_BITS = 12;
constexpr uint8_t PEDAL_COUNT = 2;
constexpr uint8_t PEDAL_OVERSAMPLES = 7;         // Discard lowest + highest, average the middle 5
constexpr uint8_t PEDAL_AVG_WINDOW = 8;          // Moving average window in mV
constexpr uint8_t PEDAL_SEND_DEADBAND = 2;       // Resend only after this much MIDI-value movement
constexpr uint32_t PEDAL_SAMPLE_PERIOD_MS = 4;   // ~250 Hz
constexpr float PEDAL_ENDSTOP_SNAP = 0.025f;     // Snap bottom/top 2.5% to exact 0/127
constexpr uint16_t MIN_VALID_PEDAL_SPAN_MV = 250;

// Calibration and LED timing
constexpr uint32_t CALIBRATION_COMBO_HOLD_MS = 1500;
constexpr uint16_t CALIBRATION_BLINK_MS = 180;
constexpr uint16_t CONNECTION_BLINK_MS = 500;

// Safe defaults if nothing is calibrated yet
constexpr uint16_t DEFAULT_MIN_MV = 200;
constexpr uint16_t DEFAULT_MAX_MV = 2800;

// The generic wait helper uses these small constants instead of an enum so the
// Arduino preprocessor does not generate bad prototypes for custom enum types.
constexpr uint8_t WAIT_PRESS = 0;
constexpr uint8_t WAIT_RELEASE = 1;
constexpr uint8_t WAIT_COMBO_RELEASE = 2;

template <typename T, size_t N>
constexpr size_t countOf(const T (&)[N]) {
  return N;
}

constexpr uint16_t ledMask(uint8_t index) {
  return uint16_t(1) << index;
}

// Globals
bool isConnected = false;
bool forcePedalResend = false;
uint16_t buttonStateMask = 0;   // One bit per footswitch LED/state.
uint32_t lastPedalSampleMs = 0;

ezButton buttonArray[BUTTON_NUM] = {
  ezButton(buttonPins[0]), ezButton(buttonPins[1]), ezButton(buttonPins[2]),
  ezButton(buttonPins[3]), ezButton(buttonPins[4]), ezButton(buttonPins[5]),
  ezButton(buttonPins[6]), ezButton(buttonPins[7]), ezButton(buttonPins[8])
};

Preferences prefs;

// Generic hardware helpers
void setPinModes(const int* pins, size_t count, uint8_t mode) {   // Applies the same pinMode to a pin list.
  for (size_t i = 0; i < count; ++i) {
    pinMode(pins[i], mode);
  }
}

void serviceButtons() {   // Updates ezButton debounce state for every footswitch.
  for (auto& button : buttonArray) {
    button.loop();
  }
}

void flushButtonEvents() {   // Clears queued press/release edges after blocking waits.
  for (auto& button : buttonArray) {
    (void)button.isPressed();
    (void)button.isReleased();
  }
}

bool buttonHeld(uint8_t index) {   // Reads the raw active-low held state for combo detection.
  return digitalRead(buttonPins[index]) == LOW;   // Active-low because buttons use INPUT_PULLUP.
}

bool calibrationComboHeld() {   // True while TUNER and CHANNEL SEL are both physically held.
  return buttonHeld(BTN_TUNER) && buttonHeld(BTN_CHANNEL_SEL);
}

// LED rendering
void renderLeds(uint16_t steadyMask, uint16_t blinkMask = 0, uint16_t blinkMs = 0) {   // Draws all LEDs from steady/blinking bitmasks.
  bool blinkOn = blinkMs > 0 && ((millis() / blinkMs) % 2) == 0;
  bool connectionOn = isConnected || ((millis() / CONNECTION_BLINK_MS) % 2) == 0;

  for (uint8_t i = 0; i < LED_NUM; ++i) {
    // A blinking bit owns the LED while active, otherwise the saved state shows.
    bool on = (blinkMask & ledMask(i)) ? blinkOn : (steadyMask & ledMask(i));
    digitalWrite(ledPins[i], on ? HIGH : LOW);
  }

  digitalWrite(LED_CONN_PIN, connectionOn ? HIGH : LOW);
}

void renderRuntimeLeds() {   // Restores normal footswitch states; LED_CONN handles connection status.
  renderLeds(buttonStateMask);
}

void flashAllButtonLeds(uint8_t times, uint16_t ms) {   // Brief completion/startup feedback on all footswitch LEDs.
  uint16_t allLeds = ledMask(LED_NUM) - 1;

  for (uint8_t i = 0; i < times; ++i) {
    renderLeds(allLeds);
    delay(ms);
    renderLeds(0);
    delay(ms);
  }
}

// ADC reading
void sortU16(uint16_t* values, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    for (size_t j = i + 1; j < count; ++j) {
      if (values[j] < values[i]) {
        uint16_t t = values[i];
        values[i] = values[j];
        values[j] = t;
      }
    }
  }
}

uint16_t readTrimmedMilliVolts(uint8_t pin) {
  uint16_t samples[PEDAL_OVERSAMPLES];

  for (uint8_t i = 0; i < PEDAL_OVERSAMPLES; ++i) {
    samples[i] = static_cast<uint16_t>(analogReadMilliVolts(pin));
    delayMicroseconds(180);
  }

  sortU16(samples, PEDAL_OVERSAMPLES);

  uint32_t sum = 0;
  for (uint8_t i = 1; i < PEDAL_OVERSAMPLES - 1; ++i) {
    sum += samples[i];
  }

  return static_cast<uint16_t>(sum / (PEDAL_OVERSAMPLES - 2));
}

// Pedals
struct Pedal {
  const char* name;
  uint8_t pin;
  uint8_t cc;
  const char* minKey;
  const char* maxKey;
  bool invert;
  uint16_t defaultMinMv;
  uint16_t defaultMaxMv;
  uint16_t calMinMv;
  uint16_t calMaxMv;
  uint16_t mvWindow[PEDAL_AVG_WINDOW];
  uint32_t mvTotal;
  uint8_t mvIndex;
  int lastSentMidi;

  bool validCalibration() const {
    return calMaxMv > calMinMv && (calMaxMv - calMinMv) >= MIN_VALID_PEDAL_SPAN_MV;
  }

  void loadCalibration() {
    calMinMv = prefs.getUShort(minKey, defaultMinMv);
    calMaxMv = prefs.getUShort(maxKey, defaultMaxMv);

    if (!validCalibration()) {
      calMinMv = defaultMinMv;
      calMaxMv = defaultMaxMv;
    }
  }

  void saveCalibration() const {
    prefs.putUShort(minKey, calMinMv);
    prefs.putUShort(maxKey, calMaxMv);
  }

  void resetFilter() {
    mvTotal = 0;
    mvIndex = 0;
    lastSentMidi = -1;

    for (uint8_t i = 0; i < PEDAL_AVG_WINDOW; ++i) {
      mvWindow[i] = 0;
    }
  }

  void primeFilter() {
    resetFilter();
    uint16_t firstMv = readTrimmedMilliVolts(pin);

    for (uint8_t i = 0; i < PEDAL_AVG_WINDOW; ++i) {
      mvWindow[i] = firstMv;
      mvTotal += firstMv;
    }
  }

  uint16_t readFilteredMv() {
    uint16_t mv = readTrimmedMilliVolts(pin);

    mvTotal -= mvWindow[mvIndex];
    mvWindow[mvIndex] = mv;
    mvTotal += mv;
    mvIndex = (mvIndex + 1) % PEDAL_AVG_WINDOW;

    return static_cast<uint16_t>(mvTotal / PEDAL_AVG_WINDOW);
  }

  int mvToMidi(uint16_t mv) const {
    if (!validCalibration()) {
      return 0;
    }

    int clamped = constrain(static_cast<int>(mv), static_cast<int>(calMinMv), static_cast<int>(calMaxMv));
    float position = float(clamped - calMinMv) / float(calMaxMv - calMinMv);

    if (invert) {
      position = 1.0f - position;
    }

    if (position <= PEDAL_ENDSTOP_SNAP) return 0;
    if (position >= 1.0f - PEDAL_ENDSTOP_SNAP) return 127;

    return constrain(static_cast<int>(lroundf(position * 127.0f)), 0, 127);
  }

  int readMidi() {
    return mvToMidi(readFilteredMv());
  }
};

Pedal pedals[PEDAL_COUNT] = {
  {"WAH", WAH_PEDAL_PIN, CC_MODULATION, "wahMin", "wahMax", true,
   DEFAULT_MIN_MV, DEFAULT_MAX_MV, 0, 0, {}, 0, 0, -1},
  {"VOLUME", VOLUME_PEDAL_PIN, CC_VOLUME, "volMin", "volMax", true,
   DEFAULT_MIN_MV, DEFAULT_MAX_MV, 0, 0, {}, 0, 0, -1}
};

// Calibration
struct CalibrationStep {
  const char* moveText;
  const char* buttonText;
  const char* label;
  uint8_t button;
  uint16_t blinkMask;
};

const CalibrationStep calibrationSteps[] = {
  {"Step 1: Move BOTH pedals to one end-stop.",
   "Press TUNER to capture the first endpoint.",
   "First", BTN_TUNER, ledMask(BTN_WAH) | ledMask(BTN_TUNER)},
  {"Step 2: Move BOTH pedals to the opposite end-stop.",
   "Press CHANNEL SEL to capture the second endpoint and save.",
   "Second", BTN_CHANNEL_SEL, ledMask(BTN_WAH) | ledMask(BTN_CHANNEL_SEL)}
};

bool calibrationRequestedAtBoot() {
  if (!calibrationComboHeld()) {
    return false;
  }

  Serial.println();
  Serial.println("Calibration combo detected at boot. Keep holding...");

  uint32_t startMs = millis();
  while (calibrationComboHeld()) {
    if (millis() - startMs >= CALIBRATION_COMBO_HOLD_MS) {
      Serial.println("Entering calibration.");
      return true;
    }
    delay(1);
  }

  return false;
}

void waitForInput(uint8_t mode, uint8_t button, uint16_t blinkMask) {   // Blocking wait used by all calibration prompts.
  // Every blocking calibration pause uses this same loop: service debounce,
  // show the requested blinking LEDs, and wait for the requested condition.
  while (true) {
    serviceButtons();
    renderLeds(0, blinkMask, CALIBRATION_BLINK_MS);

    if (mode == WAIT_PRESS && button < BUTTON_NUM && buttonArray[button].isPressed()) break;
    if (mode == WAIT_RELEASE && button < BUTTON_NUM && !buttonHeld(button)) break;
    if (mode == WAIT_COMBO_RELEASE && !calibrationComboHeld()) break;

    delay(1);
  }

  delay(40);
}

uint16_t captureMilliVolts(uint8_t pin, uint16_t blinkMask) {   // Averages one stable endpoint reading while feedback LEDs blink.
  constexpr uint8_t CAPTURE_COUNT = 24;
  uint32_t sum = 0;

  for (uint8_t i = 0; i < CAPTURE_COUNT; ++i) {
    renderLeds(0, blinkMask, CALIBRATION_BLINK_MS);
    sum += readTrimmedMilliVolts(pin);
    delay(2);
  }

  return static_cast<uint16_t>(sum / CAPTURE_COUNT);
}

void runPedalCalibration() {   // Two-step endpoint calibration for every pedal in pedals[].
  Serial.println();
  Serial.println("=== PEDAL CALIBRATION ===");

  uint16_t captures[countOf(calibrationSteps)][PEDAL_COUNT];

  for (uint8_t stepIndex = 0; stepIndex < countOf(calibrationSteps); ++stepIndex) {
    const CalibrationStep& step = calibrationSteps[stepIndex];

    Serial.println(step.moveText);
    Serial.println(step.buttonText);

    if (stepIndex == 0) {
      waitForInput(WAIT_COMBO_RELEASE, 0, step.blinkMask);
      flushButtonEvents();
    }

    waitForInput(WAIT_PRESS, step.button, step.blinkMask);
    waitForInput(WAIT_RELEASE, step.button, step.blinkMask);

    Serial.printf("%s capture:", step.label);
    for (uint8_t pedalIndex = 0; pedalIndex < PEDAL_COUNT; ++pedalIndex) {
      captures[stepIndex][pedalIndex] = captureMilliVolts(pedals[pedalIndex].pin, step.blinkMask);
      Serial.printf(" %s=%u mV", pedals[pedalIndex].name, captures[stepIndex][pedalIndex]);
    }
    Serial.println();
  }

  bool ok = true;

  for (uint8_t i = 0; i < PEDAL_COUNT; ++i) {
    pedals[i].calMinMv = min(captures[0][i], captures[1][i]);
    pedals[i].calMaxMv = max(captures[0][i], captures[1][i]);

    if (!pedals[i].validCalibration()) {
      ok = false;
      Serial.printf("%s calibration failed. Span too small: %u mV\n",
                    pedals[i].name,
                    static_cast<unsigned>(pedals[i].calMaxMv - pedals[i].calMinMv));

      pedals[i].calMinMv = pedals[i].defaultMinMv;
      pedals[i].calMaxMv = pedals[i].defaultMaxMv;
      continue;
    }

    pedals[i].saveCalibration();
    Serial.printf("%s saved: min=%u mV, max=%u mV, span=%u mV\n",
                  pedals[i].name,
                  pedals[i].calMinMv,
                  pedals[i].calMaxMv,
                  static_cast<unsigned>(pedals[i].calMaxMv - pedals[i].calMinMv));
  }

  flashAllButtonLeds(5, ok ? 60 : 180);
  Serial.println(ok ? "Calibration saved." : "Calibration failed; defaults restored.");
  renderRuntimeLeds();
  flushButtonEvents();
}

// Init
void initLeds() {
  setPinModes(ledPins, LED_NUM, OUTPUT);
  pinMode(LED_CONN_PIN, OUTPUT);
  renderLeds(0);
}

void initButtons() {
  setPinModes(buttonPins, BUTTON_NUM, INPUT_PULLUP);

  for (auto& button : buttonArray) {
    button.setDebounceTime(35);
  }
}

void initAdc() {
  analogReadResolution(ADC_BITS);

  for (auto& pedal : pedals) {
    analogSetPinAttenuation(pedal.pin, ADC_11db);
  }
}

void initPedals() {
  bool needsCalibration = false;

  for (auto& pedal : pedals) {
    pedal.loadCalibration();
    needsCalibration = needsCalibration || !pedal.validCalibration();
  }

  if (needsCalibration || calibrationRequestedAtBoot()) {
    runPedalCalibration();
  }

  for (auto& pedal : pedals) {
    pedal.primeFilter();
  }
}

void initBleMidi() {
  BLEMidiServer.begin("Line6Floorboard");

  BLEMidiServer.setOnConnectCallback([]() {
    isConnected = true;
    forcePedalResend = true;
    renderRuntimeLeds();
  });

  BLEMidiServer.setOnDisconnectCallback([]() {
    isConnected = false;
    renderRuntimeLeds();
  });

  Serial.println("BLE MIDI Advertising started");
}

// Runtime
void handleButtonActions() {
  for (uint8_t i = 0; i < BUTTON_NUM; ++i) {
    if (!buttonArray[i].isPressed()) {
      continue;
    }

    buttonStateMask ^= ledMask(i);
    renderRuntimeLeds();

    if (isConnected) {
      BLEMidiServer.controlChange(
        MIDI_CHANNEL,
        static_cast<uint8_t>(BUTTON_CC_BASE + i),
        (buttonStateMask & ledMask(i)) ? 127 : 0
      );
    }
  }
}

bool handleRuntimeCalibrationRequest() {
  static uint32_t comboStartMs = 0;

  if (calibrationComboHeld()) {
    if (comboStartMs == 0) {
      comboStartMs = millis();
    }

    if (millis() - comboStartMs >= CALIBRATION_COMBO_HOLD_MS) {
      Serial.println();
      Serial.println("Runtime calibration requested.");

      runPedalCalibration();
      for (auto& pedal : pedals) {
        pedal.primeFilter();
      }

      forcePedalResend = true;
      waitForInput(WAIT_COMBO_RELEASE, 0, 0);
      flushButtonEvents();
      comboStartMs = 0;
    }

    return true;   // Suppress normal button handling while the combo is held.
  }

  if (comboStartMs != 0) {
    comboStartMs = 0;
    flushButtonEvents();
    return true;   // Combo started but was released before the hold threshold.
  }

  return false;
}

void handlePedals() {
  if (millis() - lastPedalSampleMs < PEDAL_SAMPLE_PERIOD_MS) {
    return;
  }

  lastPedalSampleMs = millis();

  for (auto& pedal : pedals) {
    int midiValue = pedal.readMidi();

    if (!isConnected) {
      continue;
    }

    bool shouldSend =
      forcePedalResend ||
      pedal.lastSentMidi < 0 ||
      abs(midiValue - pedal.lastSentMidi) >= PEDAL_SEND_DEADBAND;

    if (shouldSend) {
      BLEMidiServer.controlChange(MIDI_CHANNEL, pedal.cc, static_cast<uint8_t>(midiValue));
      pedal.lastSentMidi = midiValue;
    }
  }

  forcePedalResend = false;
}

void setup() {
  Serial.begin(115200);
  delay(150);
  setCpuFrequencyMhz(240);

  initLeds();
  initButtons();
  prefs.begin("floorboard", false);
  initAdc();
  initPedals();
  initBleMidi();

  flashAllButtonLeds(5, 200);
  Serial.println("Ready.");
}

void loop() {
  serviceButtons();

  if (!handleRuntimeCalibrationRequest()) {
    handleButtonActions();
  }

  renderRuntimeLeds();
  handlePedals();
  delay(1);
}
