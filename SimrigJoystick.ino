/*
 * Raspberry Pi Pico USB Joystick Button Controller
 * 
 * Features:
 * - Direct mapped buttons with debouncing
 * - Edge-triggered momentary buttons (300ms pulse)
 * - Combo button groups with 200ms detection window
 * - H-pattern shifter with analog stick and reverse
 * - ADS1115 analog pedals with auto-calibration and EEPROM storage
 */

#include <ezButton.h>
#include <Joystick.h>
#include <Wire.h>
#include <ADS1115_WE.h>
#include <EEPROM.h>

// ===== CONFIGURATION =====

// Direct Mapped Buttons - State sent directly to joystick
// Format: {pin, joystick_button_number}
struct DirectButton {
  int pin;
  int joyBtn;
};

DirectButton directButtons[] = {
  // Example: {2, 9},   // Pin GP2 -> Joystick Button 9
  // Example: {3, 10},  // Pin GP3 -> Joystick Button 10
  // Add your direct button mappings here
};
const int NUM_DIRECT_BUTTONS = sizeof(directButtons) / sizeof(directButtons[0]);

// Edge-Triggered Buttons - Press joystick button for 300ms on pin state change
// Format: {pin, joystick_button_number}
struct EdgeButton {
  int pin;
  int joyBtn;
};

EdgeButton edgeButtons[] = {
  // Example: {13, 16}  // Pin GP13 edge -> Joystick Button 16 for 300ms
  // Add your edge button mappings here
};
const int NUM_EDGE_BUTTONS = sizeof(edgeButtons) / sizeof(edgeButtons[0]);

// Combo Button Groups - Two pins with 200ms detection logic
// Each group uses 2 pins and controls 4 joystick buttons
struct ComboGroup {
  int pin1;
  int pin2;
  int joyBtn1;  // P1 only after 200ms
  int joyBtn2;  // P1 + P2 both after 200ms (P1 pressed first)
  int joyBtn3;  // P2 only after 200ms
  int joyBtn4;  // P2 + P1 both after 200ms (P2 pressed first)
};

ComboGroup comboGroups[] = {
  // Example: {5, 6, 1, 2, 3, 4},  // Group 1: Pins GP5,GP6 -> Joystick Buttons 1-4
  // Example: {7, 8, 5, 6, 7, 8},  // Group 2: Pins GP7,GP8 -> Joystick Buttons 5-8
  // Add your combo groups here
};
const int NUM_COMBO_GROUPS = sizeof(comboGroups) / sizeof(comboGroups[0]);

const unsigned long COMBO_DETECT_TIME = 200;  // 200ms for combo detection
const unsigned long EDGE_PULSE_TIME = 300;    // 300ms for edge-triggered buttons

// Feature Enable/Disable
#define ENABLE_SHIFTER true   // Set to false to disable shifter functionality
#define ENABLE_PEDALS true    // Set to false to disable pedal/ADS1115 functionality

// Shifter Configuration - Analog H-pattern shifter with reverse
struct ShifterConfig {
  int xAxisPin;
  int yAxisPin;
  int reversePin;
  int xLeftThreshold;
  int xRightThreshold;
  int yTopThreshold;
  int yBottomThreshold;
  int gearButtons[8];  // Joystick buttons for gears 0-7 (0=neutral)
};

ShifterConfig shifter = {
  26,    // X_AXIS_PIN (GP26/ADC0)
  27,    // Y_AXIS_PIN (GP27/ADC1)
  22,    // REVERSE_PIN (GP22)
  320,   // X_LEFT_THRESHOLD
  600,   // X_RIGHT_THRESHOLD
  650,   // Y_TOP_THRESHOLD
  250,   // Y_BOTTOM_THRESHOLD
  {17, 18, 19, 20, 21, 22, 23, 24}  // Joystick buttons for gears 0-7
  // Gear mapping: 0=Neutral, 1=1st, 2=2nd, 3=3rd, 4=4th, 5=5th, 6=6th, 7=Reverse
};

// ADS1115 Configuration for pedals
#define ADS1115_I2C_ADDR 0x48
const uint8_t SDA_PIN = 20;
const uint8_t SCL_PIN = 21;

// Pedal definitions
#define NUM_PEDALS 3

struct PedalConfig {
  int adsChannel;       // ADS1115 channel (0-3 for A0-A3)
  int joystickAxis;     // 0=X, 1=Y, 2=Z, 3=Rx, 4=Ry, 5=Rz
  int deadzonePercent;  // Deadzone percentage for this pedal
};

PedalConfig pedals[NUM_PEDALS] = {
  {0, 3, 5},  // Pedal 1 on ADS1115 A0, mapped to Rx axis, 5% deadzone
  {2, 5, 3},  // Pedal 2 on ADS1115 A2, mapped to Rz axis, 3% deadzone
  {1, 4, 3}   // Pedal 3 on ADS1115 A1, mapped to Ry axis, 3% deadzone
};

const long int PEDAL_RANGE_LIMITp = 32767;  // 16-bit positive range limit
const long int PEDAL_RANGE_LIMITm = -32767; // 16-bit negative range limit

// EEPROM Configuration
#define EEPROM_SIZE 256
#define EEPROM_MAGIC 0xAB12  // Magic number to verify valid EEPROM data
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_PEDAL_DATA 2
const unsigned long EEPROM_SAVE_INTERVAL = 60000; // 60 seconds in milliseconds

// EEPROM structure for pedal calibration
struct PedalCalibration {
  uint16_t magic;
  int pedalMins[NUM_PEDALS];
  int pedalMaxs[NUM_PEDALS];
};

// Debounce time for all buttons (in milliseconds)
const int BUTTON_DEBOUNCE_TIME = 30;

// Reset calibration feature
// Hold directButton[0] for this duration to reset pedal calibration
const unsigned long RESET_HOLD_TIME = 10000; // 10 seconds

// ===== INTERNAL STRUCTURES =====

// Direct button state
ezButton** directBtnObjects;

// Edge button state
struct EdgeButtonState {
  ezButton* btn;
  bool lastState;
  unsigned long pulseStartTime;
  bool pulsing;
};
EdgeButtonState* edgeButtonStates;

// Combo button state
enum ComboState {
  IDLE,
  PIN1_WAITING,
  PIN2_WAITING,
  PIN1_ACTIVE,
  PIN2_ACTIVE,
  COMBO12_ACTIVE,
  COMBO21_ACTIVE
};

struct ComboGroupState {
  ezButton* btn1;
  ezButton* btn2;
  ComboState state;
  unsigned long waitStartTime;
};
ComboGroupState* comboGroupStates;

// Shifter state
int currentGear = 0;

// ADS1115 ADC object
ADS1115_WE adc = ADS1115_WE(ADS1115_I2C_ADDR);

// Pedal calibration state
int pedalMins[NUM_PEDALS];
int pedalMaxs[NUM_PEDALS];
int lastPedalValues[NUM_PEDALS];

// EEPROM save tracking
bool pedalCalibrationChanged = false;
unsigned long lastCalibrationChangeTime = 0;
unsigned long lastEEPROMSaveTime = 0;

// Reset button tracking
unsigned long resetButtonPressTime = 0;
bool resetButtonHeld = false;

// ===== SETUP =====

void setup() {
  // Initialize Joystick
  Joystick.begin();
  Joystick.useManualSend(true);  // We'll send updates manually for efficiency
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Initialize Direct Buttons
  if (NUM_DIRECT_BUTTONS > 0) {
    directBtnObjects = new ezButton*[NUM_DIRECT_BUTTONS];
    for (int i = 0; i < NUM_DIRECT_BUTTONS; i++) {
      directBtnObjects[i] = new ezButton(directButtons[i].pin);
      directBtnObjects[i]->setDebounceTime(BUTTON_DEBOUNCE_TIME);
    }
  }
  
  // Initialize Edge Buttons
  if (NUM_EDGE_BUTTONS > 0) {
    edgeButtonStates = new EdgeButtonState[NUM_EDGE_BUTTONS];
    for (int i = 0; i < NUM_EDGE_BUTTONS; i++) {
      edgeButtonStates[i].btn = new ezButton(edgeButtons[i].pin);
      edgeButtonStates[i].btn->setDebounceTime(BUTTON_DEBOUNCE_TIME);
      edgeButtonStates[i].lastState = false;
      edgeButtonStates[i].pulsing = false;
      edgeButtonStates[i].pulseStartTime = 0;
    }
  }
  
  // Initialize Combo Groups
  if (NUM_COMBO_GROUPS > 0) {
    comboGroupStates = new ComboGroupState[NUM_COMBO_GROUPS];
    for (int i = 0; i < NUM_COMBO_GROUPS; i++) {
      comboGroupStates[i].btn1 = new ezButton(comboGroups[i].pin1);
      comboGroupStates[i].btn2 = new ezButton(comboGroups[i].pin2);
      comboGroupStates[i].btn1->setDebounceTime(BUTTON_DEBOUNCE_TIME);
      comboGroupStates[i].btn2->setDebounceTime(BUTTON_DEBOUNCE_TIME);
      comboGroupStates[i].state = IDLE;
      comboGroupStates[i].waitStartTime = 0;
    }
  }
  
  // Initialize Shifter
  #if ENABLE_SHIFTER
  pinMode(shifter.xAxisPin, INPUT);
  pinMode(shifter.yAxisPin, INPUT);
  pinMode(shifter.reversePin, INPUT);  // Changed from INPUT_PULLUP to INPUT
  #endif
  
  // Initialize I2C for ADS1115
  #if ENABLE_PEDALS
  Wire.setSDA(SDA_PIN);
  Wire.setSCL(SCL_PIN);
  Wire.begin();
  
  // Initialize ADS1115
  if(!adc.init()) {
    // ADS1115 initialization failed - could add error handling here
  }
  
  // Set ADS1115 voltage range to ±4.096V (better match for 3.3V supply)
  adc.setVoltageRange_mV(ADS1115_RANGE_4096);
  
  // Load pedal calibration from EEPROM or use defaults
  loadPedalCalibration();
  
  // Configure joystick to use 16-bit mode for pedals
  Joystick.use16bit();
  #endif
}

// ===== MAIN LOOP =====

void loop() {
  bool needsUpdate = false;
  
  // Process Direct Buttons
  needsUpdate |= processDirectButtons();
  
  // Process Edge Buttons
  needsUpdate |= processEdgeButtons();
  
  // Process Combo Groups
  needsUpdate |= processComboGroups();
  
  // Process Shifter
  #if ENABLE_SHIFTER
  needsUpdate |= processShifter();
  #endif
  
  // Process Pedals
  #if ENABLE_PEDALS
  needsUpdate |= processPedals();
  
  // Check if we need to save pedal calibration to EEPROM
  checkAndSavePedalCalibration();
  #endif
  
  // Send joystick update if anything changed
  if (needsUpdate) {
    Joystick.send_now();
  }
}

// ===== DIRECT BUTTON PROCESSING =====

bool processDirectButtons() {
  bool changed = false;
  
  for (int i = 0; i < NUM_DIRECT_BUTTONS; i++) {
    directBtnObjects[i]->loop();
    
    if (directBtnObjects[i]->isPressed()) {
      Joystick.setButton(directButtons[i].joyBtn - 1, true);
      changed = true;
      
      // Start tracking for reset if this is button 0
      if (i == 0) {
        resetButtonPressTime = millis();
        resetButtonHeld = true;
      }
    }
    
    if (directBtnObjects[i]->isReleased()) {
      Joystick.setButton(directButtons[i].joyBtn - 1, false);
      changed = true;
      
      // Stop tracking for reset if this is button 0
      if (i == 0) {
        resetButtonHeld = false;
      }
    }
    
    // Check if button 0 has been held for RESET_HOLD_TIME
    if (i == 0 && resetButtonHeld) {
      unsigned long currentTime = millis();
      if (currentTime - resetButtonPressTime >= RESET_HOLD_TIME) {
        resetPedalCalibration();
        resetButtonHeld = false; // Prevent multiple resets
      }
    }
  }
  
  return changed;
}

// ===== EDGE BUTTON PROCESSING =====

bool processEdgeButtons() {
  bool changed = false;
  unsigned long currentTime = millis();
  
  for (int i = 0; i < NUM_EDGE_BUTTONS; i++) {
    edgeButtonStates[i].btn->loop();
    
    // Detect state change (edge)
    bool currentPressed = edgeButtonStates[i].btn->getState() == LOW;
    
    if (currentPressed != edgeButtonStates[i].lastState) {
      // Edge detected - start pulse
      edgeButtonStates[i].pulsing = true;
      edgeButtonStates[i].pulseStartTime = currentTime;
      Joystick.setButton(edgeButtons[i].joyBtn - 1, true);
      changed = true;
    }
    
    edgeButtonStates[i].lastState = currentPressed;
    
    // Check if pulse duration has elapsed
    if (edgeButtonStates[i].pulsing) {
      if (currentTime - edgeButtonStates[i].pulseStartTime >= EDGE_PULSE_TIME) {
        edgeButtonStates[i].pulsing = false;
        Joystick.setButton(edgeButtons[i].joyBtn - 1, false);
        changed = true;
      }
    }
  }
  
  return changed;
}

// ===== COMBO GROUP PROCESSING =====

bool processComboGroups() {
  bool changed = false;
  unsigned long currentTime = millis();
  
  for (int i = 0; i < NUM_COMBO_GROUPS; i++) {
    comboGroupStates[i].btn1->loop();
    comboGroupStates[i].btn2->loop();
    
    bool p1Pressed = comboGroupStates[i].btn1->getState() == LOW;
    bool p2Pressed = comboGroupStates[i].btn2->getState() == LOW;
    
    ComboState& state = comboGroupStates[i].state;
    unsigned long& waitStart = comboGroupStates[i].waitStartTime;
    unsigned long elapsed = currentTime - waitStart;
    
    switch (state) {
      case IDLE:
        if (p1Pressed && !p2Pressed) {
          state = PIN1_WAITING;
          waitStart = currentTime;
        } else if (p2Pressed && !p1Pressed) {
          state = PIN2_WAITING;
          waitStart = currentTime;
        }
        break;
        
      case PIN1_WAITING:
        if (!p1Pressed) {
          // P1 released before 200ms or combo
          state = IDLE;
        } else if (p2Pressed) {
          // P2 also pressed - activate combo button 2 immediately
          Joystick.setButton(comboGroups[i].joyBtn2 - 1, true);
          state = COMBO12_ACTIVE;
          changed = true;
        } else if (elapsed >= COMBO_DETECT_TIME) {
          // 200ms elapsed, only P1 - activate button 1
          Joystick.setButton(comboGroups[i].joyBtn1 - 1, true);
          state = PIN1_ACTIVE;
          changed = true;
        }
        break;
        
      case PIN2_WAITING:
        if (!p2Pressed) {
          // P2 released before 200ms or combo
          state = IDLE;
        } else if (p1Pressed) {
          // P1 also pressed - activate combo button 4 immediately
          Joystick.setButton(comboGroups[i].joyBtn4 - 1, true);
          state = COMBO21_ACTIVE;
          changed = true;
        } else if (elapsed >= COMBO_DETECT_TIME) {
          // 200ms elapsed, only P2 - activate button 3
          Joystick.setButton(comboGroups[i].joyBtn3 - 1, true);
          state = PIN2_ACTIVE;
          changed = true;
        }
        break;
        
      case PIN1_ACTIVE:
        if (!p1Pressed) {
          // P1 released - deactivate button 1
          Joystick.setButton(comboGroups[i].joyBtn1 - 1, false);
          state = IDLE;
          changed = true;
        }
        break;
        
      case PIN2_ACTIVE:
        if (!p2Pressed) {
          // P2 released - deactivate button 3
          Joystick.setButton(comboGroups[i].joyBtn3 - 1, false);
          state = IDLE;
          changed = true;
        }
        break;
        
      case COMBO12_ACTIVE:
        if (!p1Pressed && !p2Pressed) {
          // Both released - deactivate combo button 2
          Joystick.setButton(comboGroups[i].joyBtn2 - 1, false);
          state = IDLE;
          changed = true;
        }
        break;
        
      case COMBO21_ACTIVE:
        if (!p1Pressed && !p2Pressed) {
          // Both released - deactivate combo button 4
          Joystick.setButton(comboGroups[i].joyBtn4 - 1, false);
          state = IDLE;
          changed = true;
        }
        break;
    }
  }
  
  return changed;
}

// ===== SHIFTER PROCESSING =====

int determineGear(int x, int y, bool reverse) {
  // Top row (gears 1, 3, 5)
  if (y > shifter.yTopThreshold) {
    if (x < shifter.xLeftThreshold) return 1;
    if (x < shifter.xRightThreshold) return 3;
    return 5;
  }
  
  // Bottom row (gears 2, 4, 6/R)
  if (y < shifter.yBottomThreshold) {
    if (x < shifter.xLeftThreshold) return 2;
    if (x < shifter.xRightThreshold) return 4;
    return reverse ? 7 : 6;  // 6th or Reverse
  }
  
  // Center (neutral)
  return 0;
}

bool processShifter() {
  int x = analogRead(shifter.xAxisPin);
  int y = analogRead(shifter.yAxisPin);
  bool reverse = digitalRead(shifter.reversePin) == HIGH;  // HIGH = Reverse, LOW = 6th gear
  
  int newGear = determineGear(x, y, reverse);
  
  if (currentGear != newGear) {
    // Depress old gear button (only if not neutral)
    if (currentGear > 0 && currentGear <= 7) {
      Joystick.setButton(shifter.gearButtons[currentGear] - 1, false);
    }
    
    // Press new gear button (only if not neutral)
    if (newGear > 0 && newGear <= 7) {
      Joystick.setButton(shifter.gearButtons[newGear] - 1, true);
    }
    
    currentGear = newGear;
    return true;
  }
  
  return false;
}

// ===== PEDAL PROCESSING =====

// Returns a remapped value of the 'raw' position with automatic calibration and deadzone
int getMappedPedalPosition(int pedal, int rawPosition, int deadzonePercent) {
  // Store old values to detect changes
  int oldMin = pedalMins[pedal];
  int oldMax = pedalMaxs[pedal];
  
  // Store (new) extremes of the pedal for auto-calibration
  pedalMins[pedal] = min(rawPosition, pedalMins[pedal]);
  pedalMaxs[pedal] = max(rawPosition, pedalMaxs[pedal]);
  
  // Check if calibration changed
  if (pedalMins[pedal] != oldMin || pedalMaxs[pedal] != oldMax) {
    pedalCalibrationChanged = true;
    lastCalibrationChangeTime = millis();
  }
  
  // Calculate the range
  int range = pedalMaxs[pedal] - pedalMins[pedal];
  
  // Avoid division by zero
  if (range == 0) {
    return 0;
  }
  
  // Calculate deadzone thresholds
  int deadzoneLow = pedalMins[pedal] + (range * deadzonePercent / 100);
  int deadzoneHigh = pedalMaxs[pedal] - (range * deadzonePercent / 100);
  
  // Apply deadzone
  int adjustedPosition = rawPosition;
  if (rawPosition <= deadzoneLow) {
    adjustedPosition = pedalMins[pedal];
  } else if (rawPosition >= deadzoneHigh) {
    adjustedPosition = pedalMaxs[pedal];
  }
  
  // Remap to -32767 to 32767 range (full 16-bit signed range for joystick axes)
  // Map from min-max to positive-to-negative range
  int mappedPosition = map(adjustedPosition, pedalMins[pedal], pedalMaxs[pedal], PEDAL_RANGE_LIMITp, PEDAL_RANGE_LIMITm);
  
  // Constrain to valid range
  mappedPosition = constrain(mappedPosition, PEDAL_RANGE_LIMITm, PEDAL_RANGE_LIMITp);
  
  return mappedPosition;
}

// Read ADS1115 channel and return raw 16-bit signed value
int readADSChannel(int channel) {
  // Set the channel for single-ended measurement
  ADS1115_MUX muxChannel;
  switch(channel) {
    case 0: muxChannel = ADS1115_COMP_0_GND; break;
    case 1: muxChannel = ADS1115_COMP_1_GND; break;
    case 2: muxChannel = ADS1115_COMP_2_GND; break;
    case 3: muxChannel = ADS1115_COMP_3_GND; break;
    default: return 0;
  }
  
  // Configure channel and start measurement
  adc.setCompareChannels(muxChannel);
  adc.startSingleMeasurement();
  
  // Wait for measurement to complete
  while(adc.isBusy()) {
    delay(1);
  }
  
  // Get raw 16-bit result directly
  int16_t rawResult = adc.getRawResult();
  
  // Return as int (will be 16-bit signed value from ADS1115)
  return (int)rawResult;
}

bool processPedals() {
  bool changed = false;
  
  for (int i = 0; i < NUM_PEDALS; i++) {
    // Read raw 16-bit position from ADS1115
    int rawPosition = readADSChannel(pedals[i].adsChannel);
    
    // Get calibrated position with individual deadzone (returns -32767 to 32767)
    int mappedPosition = getMappedPedalPosition(i, rawPosition, pedals[i].deadzonePercent);
    
    // Only update if value changed significantly (reduce USB traffic)
    // For 16-bit, use larger threshold (about 0.3% of range)
    if (abs(mappedPosition - lastPedalValues[i]) > 100) {
      lastPedalValues[i] = mappedPosition;
      
      // Send to appropriate joystick axis
      // The Joystick library will handle the 16-bit values correctly
      switch(pedals[i].joystickAxis) {
        case 0: Joystick.X(mappedPosition); break;
        case 1: Joystick.Y(mappedPosition); break;
        case 2: Joystick.Z(mappedPosition); break;
        case 3: Joystick.sliderLeft(mappedPosition); break;  // Rx
        case 4: Joystick.sliderRight(mappedPosition); break; // Ry
        case 5: Joystick.Zrotate(mappedPosition); break;     // Rz
      }
      
      changed = true;
    }
  }
  
  return changed;
}

// ===== EEPROM FUNCTIONS =====

// Load pedal calibration from EEPROM
void loadPedalCalibration() {
  PedalCalibration cal;
  
  // Read magic number
  uint16_t magic;
  EEPROM.get(EEPROM_ADDR_MAGIC, magic);
  
  // Check if valid data exists in EEPROM
  if (magic == EEPROM_MAGIC) {
    // Load calibration data
    EEPROM.get(EEPROM_ADDR_PEDAL_DATA, cal);
    
    // Copy to working arrays
    for (int i = 0; i < NUM_PEDALS; i++) {
      pedalMins[i] = cal.pedalMins[i];
      pedalMaxs[i] = cal.pedalMaxs[i];
      lastPedalValues[i] = -32768;
    }
  } else {
    // No valid data, use defaults
    for (int i = 0; i < NUM_PEDALS; i++) {
      pedalMins[i] = 32767;   // Start with max positive value
      pedalMaxs[i] = -32768;  // Start with max negative value
      lastPedalValues[i] = -32768;
    }
  }
}

// Check if calibration needs to be saved and save to EEPROM
void checkAndSavePedalCalibration() {
  if (!pedalCalibrationChanged) {
    return;
  }
  
  unsigned long currentTime = millis();
  unsigned long timeSinceChange = currentTime - lastCalibrationChangeTime;
  
  // Save if EEPROM_SAVE_INTERVAL has passed since last calibration change
  if (timeSinceChange >= EEPROM_SAVE_INTERVAL) {
    PedalCalibration cal;
    cal.magic = EEPROM_MAGIC;
    
    // Copy current calibration
    for (int i = 0; i < NUM_PEDALS; i++) {
      cal.pedalMins[i] = pedalMins[i];
      cal.pedalMaxs[i] = pedalMaxs[i];
    }
    
    // Write to EEPROM
    EEPROM.put(EEPROM_ADDR_MAGIC, cal.magic);
    EEPROM.put(EEPROM_ADDR_PEDAL_DATA, cal);
    EEPROM.commit();
    
    lastEEPROMSaveTime = millis();
    pedalCalibrationChanged = false;
  }
}

// Reset pedal calibration to defaults
void resetPedalCalibration() {
  // Reset to default values
  for (int i = 0; i < NUM_PEDALS; i++) {
    pedalMins[i] = 32767;   // Start with max positive value
    pedalMaxs[i] = -32768;  // Start with max negative value
  }
  
  // Clear EEPROM by writing invalid magic number
  uint16_t invalidMagic = 0x0000;
  EEPROM.put(EEPROM_ADDR_MAGIC, invalidMagic);
  EEPROM.commit();
  
  // Reset tracking flags
  pedalCalibrationChanged = false;
  lastCalibrationChangeTime = 0;
}