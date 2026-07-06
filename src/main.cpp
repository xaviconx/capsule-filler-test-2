#include <Arduino.h>
#include <AccelStepper.h>
#include <HX711.h>

struct PinConfig {
  uint8_t stepPin;
  uint8_t dirPin;
  uint8_t enablePin;
  uint8_t hx711DataPin;
  uint8_t hx711ClockPin;
  bool enableActiveLow;
};

#if defined(ARDUINO_AVR_LEONARDO)
const PinConfig PINS = {A0, A1, A2, 9, 10, true};
#else
// Keep all board-specific pins in one place so the sketch can be ported to ESP32.
const PinConfig PINS = {A0, A1, A2, 9, 10, true};
#endif

enum class SystemState : uint8_t {
  Idle,
  Jogging,
  RunningRevs,
  StepValidation,
  Pattern,
  DosingFast,
  DosingFine,
  Settling,
  Done,
  Error,
};

enum class PatternMode : uint8_t {
  None,
  Bounce,
  VariableSpeed,
};

struct Config {
  float speedSps = 800.0f;
  float fineSpeedSps = 160.0f;
  float accelSps2 = 900.0f;
  long microsteps = 16;
  long stepsPerRev = 3200;
  float revolutions = 1.0f;
  long targetMg = 500;
  long fineWindowMg = 50;
  long settleMs = 600;
  long toleranceMg = 3;
  long fineBurstSteps = 80;
  float simMgPerStep = 0.02f;
  bool directionForward = true;
};

struct ScaleState {
  bool simulated = true;
  bool ready = false;
  bool stable = false;
  long raw = 0;
  long tareRaw = 0;
  float calibrationRawPerMg = 1.0f;
  float weightMg = 0.0f;
  float filteredMg = 0.0f;
  float lastFilteredMg = 0.0f;
  unsigned long lastReadAt = 0;
  unsigned long lastReadyAt = 0;
  unsigned long stableSince = 0;
};

AccelStepper stepper(AccelStepper::DRIVER, PINS.stepPin, PINS.dirPin);
HX711 scale;

Config config;
ScaleState scaleState;
SystemState state = SystemState::Idle;
SystemState stateAfterSettling = SystemState::Idle;
PatternMode patternMode = PatternMode::None;
String lastError = "";
String lineBuffer = "";

bool driverEnabled = false;
bool doneOvershoot = false;
long startPosition = 0;
long lastSimPosition = 0;
uint16_t bounceCyclesTarget = 0;
uint16_t bounceCyclesDone = 0;
float bounceForwardRevs = 0.0f;
float bounceReverseRevs = 0.0f;
bool bounceForwardLeg = true;
float variableMinSps = 0.0f;
float variableMaxSps = 0.0f;
unsigned long variableStartedAt = 0;
unsigned long variableDurationMs = 0;
unsigned long settlingStartedAt = 0;
unsigned long lastStatusAt = 0;
long validationStepsRemaining = 0;
bool validationForward = true;
bool validationPulseHigh = false;
unsigned long validationIntervalMs = 500;
unsigned long validationLastStepAt = 0;
unsigned long validationPulseStartedAt = 0;

const unsigned long SCALE_INTERVAL_MS = 80;
const unsigned long STATUS_INTERVAL_MS = 250;
const unsigned long SCALE_TIMEOUT_MS = 1200;
const unsigned long STABLE_WINDOW_MS = 350;
const unsigned long VALIDATION_PULSE_MS = 20;
const float STABLE_DELTA_MG = 2.0f;
const uint8_t LED_PIN = LED_BUILTIN;

bool ledState = false;
unsigned long lastLedAt = 0;

void setLed(bool on);

const char *stateName() {
  switch (state) {
    case SystemState::Jogging:
      return "jogging";
    case SystemState::RunningRevs:
      return "running_revs";
    case SystemState::StepValidation:
      return "step_validation";
    case SystemState::Pattern:
      return "pattern";
    case SystemState::DosingFast:
      return "dosing_fast";
    case SystemState::DosingFine:
      return "dosing_fine";
    case SystemState::Settling:
      return "settling";
    case SystemState::Done:
      return doneOvershoot ? "done_overshoot" : "done";
    case SystemState::Error:
      return "error";
    default:
      return "idle";
  }
}

long revsToSteps(float revs) {
  return lround(revs * config.stepsPerRev);
}

long signedSteps(float revs, bool forward) {
  long steps = revsToSteps(revs);
  return forward ? steps : -steps;
}

void setDriverEnabled(bool enabled) {
  driverEnabled = enabled;
  digitalWrite(PINS.enablePin, enabled == PINS.enableActiveLow ? LOW : HIGH);
}

void applyMotionConfig(float speed) {
  stepper.setMaxSpeed(abs(speed));
  stepper.setAcceleration(config.accelSps2);
}

void stopMotion(SystemState nextState = SystemState::Idle) {
  stepper.stop();
  stepper.setCurrentPosition(stepper.currentPosition());
  setDriverEnabled(false);
  patternMode = PatternMode::None;
  validationStepsRemaining = 0;
  validationPulseHigh = false;
  digitalWrite(PINS.stepPin, LOW);
  state = nextState;
}

void setError(const String &message) {
  lastError = message;
  stopMotion(SystemState::Error);
}

void updateSimulatedScale() {
  long position = stepper.currentPosition();
  long delta = position - lastSimPosition;
  lastSimPosition = position;
  if (scaleState.simulated && delta > 0) {
    scaleState.weightMg += delta * config.simMgPerStep;
  }
}

void updateScale() {
  unsigned long now = millis();
  if (now - scaleState.lastReadAt < SCALE_INTERVAL_MS) {
    return;
  }
  scaleState.lastReadAt = now;

  if (scaleState.simulated) {
    scaleState.ready = true;
    scaleState.raw = scaleState.tareRaw + lround(scaleState.weightMg * scaleState.calibrationRawPerMg);
  } else {
    if (!scale.is_ready()) {
      scaleState.ready = false;
      if (state != SystemState::Idle && state != SystemState::Done && now - scaleState.lastReadyAt > SCALE_TIMEOUT_MS) {
        setError("hx711_timeout");
      }
      return;
    }
    scaleState.ready = true;
    scaleState.lastReadyAt = now;
    scaleState.raw = scale.read();
    scaleState.weightMg = (scaleState.raw - scaleState.tareRaw) / scaleState.calibrationRawPerMg;
  }

  scaleState.lastFilteredMg = scaleState.filteredMg;
  scaleState.filteredMg = (scaleState.filteredMg * 0.75f) + (scaleState.weightMg * 0.25f);

  if (abs(scaleState.filteredMg - scaleState.lastFilteredMg) <= STABLE_DELTA_MG) {
    if (scaleState.stableSince == 0) {
      scaleState.stableSince = now;
    }
    scaleState.stable = now - scaleState.stableSince >= STABLE_WINDOW_MS;
  } else {
    scaleState.stableSince = now;
    scaleState.stable = false;
  }
}

void tareScale() {
  if (scaleState.simulated) {
    scaleState.tareRaw = 0;
    scaleState.weightMg = 0;
    scaleState.filteredMg = 0;
    scaleState.lastFilteredMg = 0;
    lastSimPosition = stepper.currentPosition();
    return;
  }

  if (!scale.is_ready()) {
    setError("hx711_not_ready");
    return;
  }

  scaleState.tareRaw = scale.read_average(10);
  scaleState.weightMg = 0;
  scaleState.filteredMg = 0;
  scaleState.lastFilteredMg = 0;
}

void calibrateScale(long knownMg) {
  if (knownMg <= 0) {
    setError("invalid_calibration_weight");
    return;
  }

  if (scaleState.simulated) {
    scaleState.calibrationRawPerMg = 1.0f;
    return;
  }

  if (!scale.is_ready()) {
    setError("hx711_not_ready");
    return;
  }

  long raw = scale.read_average(15);
  float factor = (raw - scaleState.tareRaw) / static_cast<float>(knownMg);
  if (abs(factor) < 0.001f) {
    setError("invalid_calibration_factor");
    return;
  }
  scaleState.calibrationRawPerMg = factor;
}

String readToken(String &input) {
  input.trim();
  int splitAt = input.indexOf(' ');
  if (splitAt < 0) {
    String token = input;
    input = "";
    token.trim();
    return token;
  }

  String token = input.substring(0, splitAt);
  input = input.substring(splitAt + 1);
  token.trim();
  return token;
}

void printQuoted(const String &value) {
  Serial.print('"');
  for (uint16_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"' || c == '\\') {
      Serial.print('\\');
    }
    Serial.print(c);
  }
  Serial.print('"');
}

void emitStatus() {
  Serial.print(F("{\"state\":\""));
  Serial.print(stateName());
  Serial.print(F("\",\"driver_enabled\":"));
  Serial.print(driverEnabled ? F("true") : F("false"));
  Serial.print(F(",\"position_steps\":"));
  Serial.print(stepper.currentPosition());
  Serial.print(F(",\"target_steps\":"));
  Serial.print(stepper.targetPosition());
  Serial.print(F(",\"distance_to_go\":"));
  Serial.print(stepper.distanceToGo());
  Serial.print(F(",\"validation_steps_remaining\":"));
  Serial.print(validationStepsRemaining);
  Serial.print(F(",\"current_speed_sps\":"));
  Serial.print(stepper.speed(), 2);
  Serial.print(F(",\"weight_mg\":"));
  Serial.print(scaleState.filteredMg, 2);
  Serial.print(F(",\"raw_weight_mg\":"));
  Serial.print(scaleState.weightMg, 2);
  Serial.print(F(",\"scale_raw\":"));
  Serial.print(scaleState.raw);
  Serial.print(F(",\"scale_ready\":"));
  Serial.print(scaleState.ready ? F("true") : F("false"));
  Serial.print(F(",\"scale_stable\":"));
  Serial.print(scaleState.stable ? F("true") : F("false"));
  Serial.print(F(",\"scale_simulated\":"));
  Serial.print(scaleState.simulated ? F("true") : F("false"));
  Serial.print(F(",\"done_overshoot\":"));
  Serial.print(doneOvershoot ? F("true") : F("false"));
  Serial.print(F(",\"config\":{"));
  Serial.print(F("\"speed_sps\":"));
  Serial.print(config.speedSps, 2);
  Serial.print(F(",\"fine_speed_sps\":"));
  Serial.print(config.fineSpeedSps, 2);
  Serial.print(F(",\"accel_sps2\":"));
  Serial.print(config.accelSps2, 2);
  Serial.print(F(",\"microsteps\":"));
  Serial.print(config.microsteps);
  Serial.print(F(",\"steps_per_rev\":"));
  Serial.print(config.stepsPerRev);
  Serial.print(F(",\"revolutions\":"));
  Serial.print(config.revolutions, 3);
  Serial.print(F(",\"target_mg\":"));
  Serial.print(config.targetMg);
  Serial.print(F(",\"fine_window_mg\":"));
  Serial.print(config.fineWindowMg);
  Serial.print(F(",\"settle_ms\":"));
  Serial.print(config.settleMs);
  Serial.print(F(",\"tolerance_mg\":"));
  Serial.print(config.toleranceMg);
  Serial.print(F(",\"fine_burst_steps\":"));
  Serial.print(config.fineBurstSteps);
  Serial.print(F(",\"sim_mg_per_step\":"));
  Serial.print(config.simMgPerStep, 5);
  Serial.print(F("},\"error\":"));
  printQuoted(lastError);
  Serial.println(F("}"));
}

void startJog(float revs, bool forward) {
  doneOvershoot = false;
  lastError = "";
  applyMotionConfig(config.speedSps);
  setDriverEnabled(true);
  stepper.move(signedSteps(revs, forward));
  state = SystemState::Jogging;
}

void startContinuous(bool forward) {
  doneOvershoot = false;
  lastError = "";
  applyMotionConfig(config.speedSps);
  setDriverEnabled(true);
  stepper.setSpeed(forward ? config.speedSps : -config.speedSps);
  state = SystemState::RunningRevs;
}

void startStepValidation(long steps, bool forward, unsigned long intervalMs) {
  doneOvershoot = false;
  lastError = "";
  patternMode = PatternMode::None;
  validationStepsRemaining = max(0L, steps);
  validationForward = forward;
  validationIntervalMs = max(50UL, intervalMs);
  validationPulseHigh = false;
  validationLastStepAt = millis() - validationIntervalMs;
  validationPulseStartedAt = 0;
  digitalWrite(PINS.dirPin, validationForward ? HIGH : LOW);
  digitalWrite(PINS.stepPin, LOW);
  setLed(false);
  setDriverEnabled(true);
  state = validationStepsRemaining > 0 ? SystemState::StepValidation : SystemState::Done;
}

void startBounce(uint16_t cycles, float forwardRevs, float reverseRevs) {
  doneOvershoot = false;
  lastError = "";
  bounceCyclesTarget = cycles;
  bounceCyclesDone = 0;
  bounceForwardRevs = forwardRevs;
  bounceReverseRevs = reverseRevs;
  bounceForwardLeg = true;
  patternMode = PatternMode::Bounce;
  applyMotionConfig(config.speedSps);
  setDriverEnabled(true);
  stepper.move(revsToSteps(bounceForwardRevs));
  state = SystemState::Pattern;
}

void startVariable(float minSps, float maxSps, unsigned long seconds) {
  doneOvershoot = false;
  lastError = "";
  variableMinSps = minSps;
  variableMaxSps = maxSps;
  variableDurationMs = seconds * 1000UL;
  variableStartedAt = millis();
  patternMode = PatternMode::VariableSpeed;
  setDriverEnabled(true);
  state = SystemState::Pattern;
}

void startDose(long targetMg, long fineWindowMg) {
  doneOvershoot = false;
  lastError = "";
  config.targetMg = targetMg;
  config.fineWindowMg = fineWindowMg;
  applyMotionConfig(config.speedSps);
  setDriverEnabled(true);
  stepper.setSpeed(config.speedSps);
  state = SystemState::DosingFast;
}

void updateBounce() {
  if (stepper.distanceToGo() != 0) {
    stepper.run();
    return;
  }

  if (bounceForwardLeg) {
    bounceForwardLeg = false;
    stepper.move(-revsToSteps(bounceReverseRevs));
    return;
  }

  bounceCyclesDone++;
  if (bounceCyclesDone >= bounceCyclesTarget) {
    stopMotion(SystemState::Done);
    return;
  }

  bounceForwardLeg = true;
  stepper.move(revsToSteps(bounceForwardRevs));
}

void updateVariable() {
  unsigned long elapsed = millis() - variableStartedAt;
  if (elapsed >= variableDurationMs) {
    stopMotion(SystemState::Done);
    return;
  }

  float phase = variableDurationMs == 0 ? 1.0f : elapsed / static_cast<float>(variableDurationMs);
  float wave = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
  float speed = variableMinSps + ((variableMaxSps - variableMinSps) * wave);
  stepper.setSpeed(speed);
  stepper.runSpeed();
}

void enterFineSettling() {
  settlingStartedAt = millis();
  stateAfterSettling = SystemState::DosingFine;
  state = SystemState::Settling;
}

void updateDosing() {
  float stopThreshold = config.targetMg - config.toleranceMg;
  if (scaleState.filteredMg >= stopThreshold) {
    doneOvershoot = scaleState.filteredMg > config.targetMg;
    stopMotion(SystemState::Done);
    return;
  }

  if (state == SystemState::DosingFast) {
    float fineStartMg = config.targetMg - config.fineWindowMg;
    if (scaleState.filteredMg >= fineStartMg) {
      applyMotionConfig(config.fineSpeedSps);
      stepper.move(config.fineBurstSteps);
      state = SystemState::DosingFine;
      return;
    }
    stepper.runSpeed();
    return;
  }

  if (state == SystemState::DosingFine) {
    if (stepper.distanceToGo() != 0) {
      stepper.run();
      return;
    }
    enterFineSettling();
    return;
  }

  if (state == SystemState::Settling) {
    if (millis() - settlingStartedAt < static_cast<unsigned long>(config.settleMs)) {
      return;
    }
    if (scaleState.filteredMg >= stopThreshold) {
      doneOvershoot = scaleState.filteredMg > config.targetMg;
      stopMotion(SystemState::Done);
      return;
    }
    applyMotionConfig(config.fineSpeedSps);
    setDriverEnabled(true);
    stepper.move(config.fineBurstSteps);
    state = stateAfterSettling;
  }
}

bool applyConfig(const String &key, const String &value) {
  float numeric = value.toFloat();
  if (key == "speed_sps") {
    config.speedSps = max(1.0f, numeric);
  } else if (key == "fine_speed_sps") {
    config.fineSpeedSps = max(1.0f, numeric);
  } else if (key == "accel_sps2") {
    config.accelSps2 = max(1.0f, numeric);
  } else if (key == "microsteps") {
    config.microsteps = max(1L, value.toInt());
    config.stepsPerRev = 200L * config.microsteps;
  } else if (key == "steps_per_rev") {
    config.stepsPerRev = max(1L, value.toInt());
  } else if (key == "revolutions") {
    config.revolutions = max(0.0f, numeric);
  } else if (key == "target_mg") {
    config.targetMg = max(0L, value.toInt());
  } else if (key == "fine_window_mg") {
    config.fineWindowMg = max(0L, value.toInt());
  } else if (key == "settle_ms") {
    config.settleMs = max(0L, value.toInt());
  } else if (key == "tolerance_mg") {
    config.toleranceMg = max(0L, value.toInt());
  } else if (key == "fine_burst_steps") {
    config.fineBurstSteps = max(1L, value.toInt());
  } else if (key == "sim_mg_per_step") {
    config.simMgPerStep = max(0.0f, numeric);
  } else {
    return false;
  }
  applyMotionConfig(config.speedSps);
  return true;
}

void handleCommand(String command) {
  command.trim();
  command.toLowerCase();
  if (command.length() == 0) {
    return;
  }

  String action = readToken(command);

  if (action == "status") {
    emitStatus();
  } else if (action == "stop") {
    doneOvershoot = false;
    lastError = "";
    stopMotion(SystemState::Idle);
    emitStatus();
  } else if (action == "enable") {
    setDriverEnabled(readToken(command).toInt() != 0);
    emitStatus();
  } else if (action == "config") {
    String key = readToken(command);
    String value = readToken(command);
    if (!applyConfig(key, value)) {
      setError("unknown_config");
    }
    emitStatus();
  } else if (action == "jog") {
    float revs = readToken(command).toFloat();
    String direction = readToken(command);
    startJog(revs, direction != "reverse" && direction != "rev" && direction != "-1");
    emitStatus();
  } else if (action == "run") {
    String direction = readToken(command);
    startContinuous(direction != "reverse" && direction != "rev" && direction != "-1");
    emitStatus();
  } else if (action == "validate") {
    long steps = readToken(command).toInt();
    String direction = readToken(command);
    unsigned long intervalMs = readToken(command).toInt();
    startStepValidation(steps, direction != "reverse" && direction != "rev" && direction != "-1", intervalMs);
    emitStatus();
  } else if (action == "pattern") {
    String mode = readToken(command);
    if (mode == "bounce") {
      uint16_t cycles = readToken(command).toInt();
      float forwardRevs = readToken(command).toFloat();
      float reverseRevs = readToken(command).toFloat();
      startBounce(cycles, forwardRevs, reverseRevs);
    } else if (mode == "variable") {
      float minSps = readToken(command).toFloat();
      float maxSps = readToken(command).toFloat();
      unsigned long seconds = readToken(command).toInt();
      startVariable(minSps, maxSps, seconds);
    } else {
      setError("unknown_pattern");
    }
    emitStatus();
  } else if (action == "tare") {
    tareScale();
    emitStatus();
  } else if (action == "calibrate") {
    calibrateScale(readToken(command).toInt());
    emitStatus();
  } else if (action == "scale") {
    String subcommand = readToken(command);
    if (subcommand == "sim") {
      scaleState.simulated = readToken(command).toInt() != 0;
      tareScale();
    }
    emitStatus();
  } else if (action == "dose") {
    long targetMg = readToken(command).toInt();
    long fineWindowMg = readToken(command).toInt();
    startDose(targetMg, fineWindowMg);
    emitStatus();
  } else {
    Serial.print(F("{\"error\":\"unknown_command\",\"command\":"));
    printQuoted(action);
    Serial.println(F("}"));
  }
}

void updateStepValidation() {
  unsigned long now = millis();

  if (validationPulseHigh) {
    if (now - validationPulseStartedAt >= VALIDATION_PULSE_MS) {
      digitalWrite(PINS.stepPin, LOW);
      setLed(false);
      validationPulseHigh = false;
      validationStepsRemaining--;
      stepper.setCurrentPosition(stepper.currentPosition() + (validationForward ? 1 : -1));
      if (validationStepsRemaining <= 0) {
        stopMotion(SystemState::Done);
      }
    }
    return;
  }

  if (validationStepsRemaining > 0 && now - validationLastStepAt >= validationIntervalMs) {
    validationLastStepAt = now;
    validationPulseStartedAt = now;
    digitalWrite(PINS.dirPin, validationForward ? HIGH : LOW);
    digitalWrite(PINS.stepPin, HIGH);
    setLed(true);
    validationPulseHigh = true;
  }
}

void updateMotion() {
  if (state == SystemState::Jogging) {
    stepper.run();
    if (stepper.distanceToGo() == 0) {
      stopMotion(SystemState::Done);
    }
  } else if (state == SystemState::RunningRevs) {
    stepper.runSpeed();
  } else if (state == SystemState::StepValidation) {
    updateStepValidation();
  } else if (state == SystemState::Pattern) {
    if (patternMode == PatternMode::Bounce) {
      updateBounce();
    } else if (patternMode == PatternMode::VariableSpeed) {
      updateVariable();
    }
  } else if (state == SystemState::DosingFast || state == SystemState::DosingFine || state == SystemState::Settling) {
    updateDosing();
  }
}

void setLed(bool on) {
  ledState = on;
  digitalWrite(LED_PIN, on ? HIGH : LOW);
}

void blinkLed(unsigned long onMs, unsigned long offMs) {
  unsigned long now = millis();
  unsigned long interval = ledState ? onMs : offMs;
  if (now - lastLedAt >= interval) {
    lastLedAt = now;
    setLed(!ledState);
  }
}

void pulseLedByDirection() {
  if (stepper.speed() >= 0) {
    blinkLed(80, 220);
  } else {
    blinkLed(220, 80);
  }
}

void updateLed() {
  switch (state) {
    case SystemState::Idle:
    case SystemState::Done:
      blinkLed(60, 1400);
      break;
    case SystemState::Jogging:
    case SystemState::RunningRevs:
    case SystemState::Pattern:
      pulseLedByDirection();
      break;
    case SystemState::StepValidation:
      break;
    case SystemState::DosingFast:
      blinkLed(80, 120);
      break;
    case SystemState::DosingFine:
      blinkLed(60, 420);
      break;
    case SystemState::Settling:
      blinkLed(700, 700);
      break;
    case SystemState::Error:
      blinkLed(80, 80);
      break;
  }
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  setLed(false);

  pinMode(PINS.stepPin, OUTPUT);
  pinMode(PINS.dirPin, OUTPUT);
  digitalWrite(PINS.stepPin, LOW);

  pinMode(PINS.enablePin, OUTPUT);
  setDriverEnabled(false);

  applyMotionConfig(config.speedSps);
  stepper.setMinPulseWidth(3);
  scale.begin(PINS.hx711DataPin, PINS.hx711ClockPin);

  Serial.begin(115200);
  while (!Serial && millis() < 3000) {
  }

  Serial.println(F("{\"ready\":true,\"board\":\"romeo_leonardo\",\"protocol\":\"stepper-loadcell-v1\"}"));
  emitStatus();
}

void loop() {
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineBuffer.length() > 0) {
        handleCommand(lineBuffer);
        lineBuffer = "";
      }
    } else if (lineBuffer.length() < 120) {
      lineBuffer += c;
    }
  }

  updateMotion();
  updateSimulatedScale();
  updateScale();
  updateLed();

  if (millis() - lastStatusAt >= STATUS_INTERVAL_MS) {
    lastStatusAt = millis();
    emitStatus();
  }
}
