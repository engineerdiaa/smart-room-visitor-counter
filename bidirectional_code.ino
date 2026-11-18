// Smart Room Visitor Counter by @diaawastaken · github.com/engineerdiaa/smart-room-visitor-counter · MIT

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

// =========================================
// PIN DEFINITIONS
// =========================================
const int LDR1_PIN   = 34; // Entry side LDR
const int LDR2_PIN   = 35; // Inside LDR
const int LASER1_PIN = 32;
const int LASER2_PIN = 33;
const int FAN_PIN    = 25;
const int SERVO_PIN  = 26;
const int BUZZER_PIN = 4;

// LED Pairs
const int LED1 = 27; const int LED2 = 17; // Pair 1
const int LED3 = 23; const int LED4 = 16; // Pair 2
const int LED5 = 19; const int LED6 = 14; // Pair 3
const int LED7 = 18; const int LED8 = 13; // Pair 4

// =========================================
// OBJECTS & GLOBALS
// =========================================
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
Servo doorServo;

// Room Statistics
int peopleIn = 0;
int totalEntered = 0;
int totalLeft = 0;

// State Tracking
bool fanState = false;
bool isLocked = false;
int lastDisplayedCount = -1;

// Adjusted for ambient light dropout (preventing zero-count freeze)
const int LDR_THRESHOLD = 2200;

// SUPERIOR STATE MACHINE LOGIC
enum CrossingState {
  IDLE,
  ENTERING,
  EXITING,
  ENTER_CONFIRMED,
  EXIT_CONFIRMED
};
CrossingState crossingState = IDLE;
unsigned long sequenceTimeout = 0;

// Event Popup Variables
String popupMessage = "";
unsigned long popupTimer = 0;
bool isPopupActive = false;

// Debugging Timer
unsigned long lastDebugPrint = 0;

// Gamma Correction Table (8-bit)
const uint8_t gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,
    2,  3,  3,  3,  3,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,
    7,  7,  7,  8,  8,  8,  9,  9,  9, 10, 10, 11, 11, 11, 12, 12,
   13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20, 21,
   21, 22, 22, 23, 24, 24, 25, 26, 26, 27, 28, 28, 29, 30, 31, 31,
   32, 33, 34, 34, 35, 36, 37, 38, 38, 39, 40, 41, 42, 43, 44, 45,
   46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 62,
   63, 64, 65, 67, 68, 69, 70, 72, 73, 74, 76, 77, 78, 80, 81, 83,
   84, 86, 87, 89, 90, 92, 93, 95, 96, 98,100,101,103,105,106,108,
  110,112,114,115,117,119,121,123,125,127,129,131,133,135,137,139,
  141,144,146,148,150,153,155,157,160,162,165,167,170,172,175,177,
  180,183,185,188,191,194,196,199,202,205,208,211,214,217,220,223,
  226,229,232,235,239,242,245,248,252,255
};

// Lighting Fade Variables
int fadeProgress = 0; // Ranges 0 to 1020 (4 pairs * 255)
unsigned long lastFadeTime = 0;

// =========================================
// FUNCTION PROTOTYPES
// =========================================
void triggerPopup(String msg, int beepFreq, int beepDur);
void updateLogic();
void processLDRs();
void updateLights();
void updateLCD();
void handleRoot();

// =========================================
// SETUP
// =========================================
void setup() {
  Serial.begin(115200);

  // === 1. SERVO GETS ABSOLUTE PRIORITY (TIMER 3) ===
  // Lock a dedicated timer and force 50Hz so the LEDs don't interfere
  ESP32PWM::allocateTimer(3);
  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN);
  doorServo.write(80); // Open position
  delay(1000);         // Give the physical motor a full second to move

  // === 2. INIT PINS ===
  pinMode(LASER1_PIN, OUTPUT); pinMode(LASER2_PIN, OUTPUT);
  digitalWrite(LASER1_PIN, HIGH); digitalWrite(LASER2_PIN, HIGH);

  // ACTIVELY HOLD FAN OFF ON BOOT
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  pinMode(BUZZER_PIN, OUTPUT);

  // === 3. SETUP ESP32 PWM FOR LEDS ===
  ledcAttach(LED1, 5000, 8); ledcAttach(LED2, 5000, 8);
  ledcAttach(LED3, 5000, 8); ledcAttach(LED4, 5000, 8);
  ledcAttach(LED5, 5000, 8); ledcAttach(LED6, 5000, 8);
  ledcAttach(LED7, 5000, 8); ledcAttach(LED8, 5000, 8);

  // Init LCD
  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Booting System..");

  // Network Setup
  WiFi.softAP("Smart Room", "12345678");
  server.on("/", handleRoot);
  server.begin();

  // === POWER ARMOR ===
  Serial.println("Waiting 3 seconds for power rails to stabilize...");
  delay(3000);

  // Power Up Feedback
  tone(BUZZER_PIN, 1000, 150); delay(200);
  tone(BUZZER_PIN, 1500, 150); delay(200);
  tone(BUZZER_PIN, 2000, 300);

  lcd.clear();
  triggerPopup("System Online!", 0, 0);
  Serial.println("System Fully Online!");
}

// =========================================
// MAIN LOOP
// =========================================
void loop() {
  server.handleClient(); // Handle Web Traffic
  processLDRs();         // Check Enter/Leave sequence
  updateLogic();         // Fan & Servo Logic
  updateLights();        // Gradual LED fading
  updateLCD();           // Manage Display & Popups

  // --- CONTINUOUS LDR & COUNT DEBUG PRINT ---
  if (millis() - lastDebugPrint > 200) {
    Serial.print("LDR1: ");
    Serial.print(analogRead(LDR1_PIN));
    Serial.print("   |   LDR2: ");
    Serial.print(analogRead(LDR2_PIN));
    Serial.print("   |   Current Count: ");
    Serial.println(peopleIn);
    lastDebugPrint = millis();
  }

  delay(10);
}

// =========================================
// CORE LOGIC FUNCTIONS
// =========================================

void processLDRs() {
  bool l1_cut = analogRead(LDR1_PIN) < LDR_THRESHOLD;
  bool l2_cut = analogRead(LDR2_PIN) < LDR_THRESHOLD;
  int delta = 0;

  // --- TIMEOUT FAILSAFE ---
  if (crossingState != IDLE && millis() - sequenceTimeout > 3000) {
    crossingState = IDLE;
    Serial.println("Sequence Timeout - System Un-frozen");
  }

  switch (crossingState) {
    case IDLE:
      if (l1_cut && !l2_cut) { crossingState = ENTERING; sequenceTimeout = millis(); Serial.println("Entering started"); }
      else if (l2_cut && !l1_cut) { crossingState = EXITING; sequenceTimeout = millis(); Serial.println("Leaving started"); }
      break;

    case ENTERING:
      if (l2_cut) crossingState = ENTER_CONFIRMED;
      else if (!l1_cut) crossingState = IDLE;
      break;

    case EXITING:
      if (l1_cut) crossingState = EXIT_CONFIRMED;
      else if (!l2_cut) crossingState = IDLE;
      break;

    case ENTER_CONFIRMED:
      if (!l1_cut && !l2_cut) { delta = +1; crossingState = IDLE; }
      break;

    case EXIT_CONFIRMED:
      if (!l1_cut && !l2_cut) { delta = -1; crossingState = IDLE; }
      break;
  }

  if (delta == +1) {
    if (peopleIn < 10) {
      peopleIn++;
      totalEntered++;
      Serial.println("Counted: ENTRY. Total: " + String(peopleIn));
      triggerPopup("Person Entered", 2000, 100);
    }
  } else if (delta == -1) {
    if (peopleIn > 0) {
      peopleIn--;
      totalLeft++;
      Serial.println("Counted: EXIT. Total: " + String(peopleIn));
      triggerPopup("Person Left", 1000, 100);
    }
  }
}

void updateLogic() {
  // Fan Logic
  if (peopleIn >= 5 && !fanState) {
    fanState = true;
    triggerPopup("Fan Turned ON", 1500, 150); // 1. Talk to the LCD first
    delay(100);                               // 2. Let the I2C bus finish and breathe
    digitalWrite(FAN_PIN, HIGH);              // 3. THEN hit it with the motor power spike
  } else if (peopleIn < 5 && fanState) {
    fanState = false;
    triggerPopup("Fan Turned OFF", 1200, 150);
    delay(100);
    digitalWrite(FAN_PIN, LOW);
  }

  // Servo Lock Logic (Soft Start & 5-Degree Buffer Zone)
  if (peopleIn == 10 && !isLocked) {
    isLocked = true;
    doorServo.attach(SERVO_PIN);

    // SLOW SWEEP: Moves from 80 down to 5 smoothly (prevents end-stop stall)
    for (int pos = 80; pos >= 5; pos -= 1) {
      doorServo.write(pos);
      delay(15);
    }

    triggerPopup("Room FULL", 500, 400);
  } else if (peopleIn <= 9 && isLocked) {
    isLocked = false;
    doorServo.attach(SERVO_PIN);

    // SLOW SWEEP: Moves from 5 up to 80 smoothly (prevents end-stop stall)
    for (int pos = 5; pos <= 80; pos += 1) {
      doorServo.write(pos);
      delay(15);
    }

    triggerPopup("Unlocked", 1800, 200);
  }
}

void updateLights() {
  unsigned long currentTime = millis();
  unsigned long elapsed = currentTime - lastFadeTime;

  if (elapsed >= 2) {
    int steps = elapsed / 2;
    lastFadeTime = currentTime;

    if (peopleIn > 0 && fadeProgress < 1020) {
      fadeProgress += steps;
      if (fadeProgress > 1020) fadeProgress = 1020;
    } else if (peopleIn == 0 && fadeProgress > 0) {
      fadeProgress -= steps;
      if (fadeProgress < 0) fadeProgress = 0;
    }

    int p1 = constrain(fadeProgress, 0, 255);
    int p2 = constrain(fadeProgress - 255, 0, 255);
    int p3 = constrain(fadeProgress - 510, 0, 255);
    int p4 = constrain(fadeProgress - 765, 0, 255);

    ledcWrite(LED1, gamma8[p1]); ledcWrite(LED2, gamma8[p1]);
    ledcWrite(LED3, gamma8[p2]); ledcWrite(LED4, gamma8[p2]);
    ledcWrite(LED5, gamma8[p3]); ledcWrite(LED6, gamma8[p3]);
    ledcWrite(LED7, gamma8[p4]); ledcWrite(LED8, gamma8[p4]);
  }
}

void updateLCD() {
  if (isPopupActive) {
    if (millis() - popupTimer > 1500) {
      isPopupActive = false;
      lcd.clear();
      lastDisplayedCount = -1;
    }
  } else {
    if (peopleIn != lastDisplayedCount) {
      lcd.setCursor(0, 0);
      lcd.print("In Room: ");
      lcd.print(peopleIn);
      lcd.print("   ");

      lcd.setCursor(0, 1);
      lcd.print("Lock: ");
      lcd.print(isLocked ? "YES" : "NO ");

      lastDisplayedCount = peopleIn;
    }
  }
}

void triggerPopup(String msg, int beepFreq, int beepDur) {
  popupMessage = msg;
  popupTimer = millis();
  isPopupActive = true;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("*** EVENT ***");
  lcd.setCursor(0, 1);
  lcd.print(msg);

  if (beepFreq > 0) {
    tone(BUZZER_PIN, beepFreq, beepDur);
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>Smart Room Stats</title>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "<style>body{font-family: Arial; text-align: center; margin-top: 50px; background-color: #222; color: #fff;}";
  html += "h1{color: #4CAF50;} .card{background: #333; padding: 20px; border-radius: 10px; display: inline-block;}</style>";
  html += "</head><body>";

  html += "<div class='card'>";
  html += "<h1>Smart Room Statistics</h1>";
  html += "<hr>";
  html += "<h2>Currently Inside: <strong>" + String(peopleIn) + " / 10</strong></h2>";
  html += "<h3>Total Entered: " + String(totalEntered) + "</h3>";
  html += "<h3>Total Left: " + String(totalLeft) + "</h3>";
  html += "<h3>Status: " + String(isLocked ? "<span style='color:red;'>LOCKED</span>" : "<span style='color:green;'>OPEN</span>") + "</h3>";
  html += "</div>";
  html += "</body></html>";

  server.send(200, "text/html", html);
}
