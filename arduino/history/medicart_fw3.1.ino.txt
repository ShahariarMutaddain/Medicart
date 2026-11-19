#include <Arduino.h>
#include <EEPROM.h>   // saves the wheel fixes (FLIPL / FLIPR / SWAP)

// ════════════════════════════════════════════════════════════════════════════
//  MediCart firmware 3.1  ·  IUB Robotics Team  ·  Arduino Mega 2560  ·  115200 baud
//
//  Line following = the LFR code that ran on the robot, unchanged:
//    same pins, MOTOR_SPEED 38, SLIDE_BRAKE_SPEED 15, inverted IR reading (!digitalRead),
//    same pattern table, same glitch filter (2 cycles), same 50 ms cycle, same Serial text.
//  Around it, the MediCart architecture: the Pi decides WHEN the cart may move.
//
//  Pi commands (protocol unchanged from firmware 3.0):
//    SAFE / MODE:AUTO      path clear -> line following      STOP   stop now (person / emergency)
//    MODE:MANUAL           manual mode                       DRIVE:<left>,<right>  wheel PWM (max 180)
//    QR:1..10 / RESET_QR   bed output pins                   No command for 750 ms -> motors stop
//
//  Serial Monitor (115200, any line ending):
//    l or 1      line following WITHOUT the Pi for 60 s (type again to continue)   x  STOP
//    w a s d     forward / pivot left / back / pivot right, 1 s per press (speed 38)
//    + / -       key speed            v  live view on/off       i  sensors once
//    t           finger test: wheels lifted, cover sensors, wheels do what the LFR would do
//    TESTL / TESTR   spin one wheel    FLIPL / FLIPR  reverse a wheel   SWAP  swap sides
//    SETTINGS    show everything       FACTORY        clear the wheel fixes
//    STAT        status line (used by the Pi)          VIEW 0 / VIEW 1
//  There is NO camera brake during l / w a s d / t. Keep someone next to the robot.
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
//  FROM THE LFR CODE THAT RAN ON THE ROBOT (values unchanged)
// ════════════════════════════════════════════════════════════════════════════

// Motor Control Pins (Pins 2, 3, 4, 5 handle both PWM speed and direction)
const int MOTOR_L_PIN1 = 2; // Left Motor Forward / PWM
const int MOTOR_L_PIN2 = 3; // Left Motor Backward / PWM
const int MOTOR_R_PIN1 = 4; // Right Motor Forward / PWM
const int MOTOR_R_PIN2 = 5; // Right Motor Backward / PWM

const int IR_PINS[5] = {A8, A9, A10, A11, A12};  // S1 (leftmost) to S5 (rightmost)
const int BUZZER_PIN = 25;

// Speed: 15% speed -> PWM 38 out of 255
const int MOTOR_SPEED = 38;

// Active slide brake speed: inside wheel spins slightly backward (PWM 15) for ultra-hard cornering on both sides
const int SLIDE_BRAKE_SPEED = 15;

// Variables to track current PWM values for Serial printing
int currentLeftPWM = 0;
int currentRightPWM = 0;

// Anti-glitch counter for temporary line dropouts
int offLineCount = 0;
const int GLITCH_THRESHOLD = 2; // Must stay off-line for 2 cycles before stopping

// The original loop ended with delay(50). Here the same 50 ms cycle runs without blocking,
// so Pi commands (STOP!) are still read instantly.
const unsigned long LFR_CYCLE_MS = 50;

// ════════════════════════════════════════════════════════════════════════════
//  MEDICART ARCHITECTURE
// ════════════════════════════════════════════════════════════════════════════

const long SERIAL_BAUD = 115200;                 // the Pi (medicart.py) talks at 115200
const int QR_OUTPUT_PINS[10] = {6, 7, 8, 9, 10, 11, 12, 13, 22, 23};
// QR 1 -> pin 6,  QR 2 -> pin 7,  QR 3 -> pin 8,  QR 4 -> pin 9,  QR 5 -> pin 10
// QR 6 -> pin 11, QR 7 -> pin 12, QR 8 -> pin 13, QR 9 -> pin 22, QR 10 -> pin 23

const unsigned long COMMAND_TIMEOUT_MS = 750;    // no Pi command for 750 ms -> stop
const int MAX_MANUAL_SPEED = 180;                // DRIVE:l,r is capped here

const unsigned long VIEW_EVERY_MS = 200;         // live view while NOT line following
const bool VIEW_ON_AT_BOOT = true;               // the Pi sends "VIEW 0" when it connects
const unsigned long KEY_HOLD_MS = 1000;          // one w/a/s/d press drives this long
const int KEY_SPEED_START = MOTOR_SPEED;         // keys use the LFR speed (38)
const int KEY_SPEED_STEP = 10;
const unsigned long LFR_TEST_MS = 60000;         // "l" without the Pi: 60 s per press
const unsigned long FINGER_TEST_MS = 120000;     // "t" finger test: 2 min

const bool DEBUG_SAFETY = true;                  // "DBG ..." lines for the Pi log
const unsigned long DEBUG_MIN_INTERVAL_MS = 50;
const unsigned long LINE_LOST_BEEP_PERIOD_MS = 500;   // line lost: 100 ms beep every 500 ms
const unsigned long LINE_LOST_BEEP_ON_MS = 100;

// ---- state ----
bool obstacleDetected = true;                    // starts stopped until the Pi says SAFE
bool qrOutputs[10] = {false};
String serialBuffer = "";
unsigned long lastCharMs = 0;
unsigned long lastSafetyHeartbeatMs = 0;
enum ControlMode { AUTO_MODE, MANUAL_MODE };
ControlMode controlMode = AUTO_MODE;
int manualLeftSpeed = 0;
int manualRightSpeed = 0;
int currentLeftSpeed = 0;                        // signed, for STAT / DBG (+ forward, - backward)
int currentRightSpeed = 0;

unsigned long lastLfrCycleMs = 0;
bool lfrDueNow = true;
bool lineLost = false;                           // confirmed off-line (the LFR stopped)
bool junction = false;                           // 1 1 1 1 1
uint8_t currentPattern = 0;                      // bit 4 = S1 ... bit 0 = S5

bool viewOn = VIEW_ON_AT_BOOT;
unsigned long lastViewMs = 0;
unsigned long keyUntilMs = 0;
unsigned long lfrTestUntilMs = 0;
int keySpeed = KEY_SPEED_START;
bool fingerTest = false;
unsigned long fingerUntilMs = 0;
byte fingerBase = 0;
bool flipA = false, flipB = false, swapSides = false;   // A = pins 2/3, B = pins 4/5
const byte FIX_MAGIC = 0xB1;                     // new value: settings saved by older firmware are ignored
const int EE_FIX = 32;

bool prevObstacleDebug = true;
bool prevLineLostDebug = false;
int prevLeftSpeedDebug = 1000;
int prevRightSpeedDebug = 1000;
unsigned long lastDebugPrintMs = 0;

bool timeBefore(unsigned long until)             // true while millis() < until (rollover-safe)
{
    return until != 0 && (long)(millis() - until) < 0;
}
bool localHold()                                 // a Serial Monitor test is driving: no Pi timeout
{
    return timeBefore(keyUntilMs) || timeBefore(lfrTestUntilMs) || fingerTest;
}

// ════════════════════════════════════════════════════════════════════════════
//  MOTORS (the original functions)
// ════════════════════════════════════════════════════════════════════════════

void setMotorPWM(int leftForward, int leftBackward, int rightForward, int rightBackward) {
  // Track active speed for Serial display
  currentLeftPWM = (leftForward > 0) ? leftForward : leftBackward;
  currentRightPWM = (rightForward > 0) ? rightForward : rightBackward;
  currentLeftSpeed = leftForward - leftBackward;
  currentRightSpeed = rightForward - rightBackward;

  // Wheel fixes (FLIPL / FLIPR / SWAP). All OFF by default = exactly the original wiring.
  int lf = leftForward, lb = leftBackward, rf = rightForward, rb = rightBackward, t;
  if (swapSides) { t = lf; lf = rf; rf = t; t = lb; lb = rb; rb = t; }
  if (flipA) { t = lf; lf = lb; lb = t; }
  if (flipB) { t = rf; rf = rb; rb = t; }

  analogWrite(MOTOR_L_PIN1, lf);
  analogWrite(MOTOR_L_PIN2, lb);
  analogWrite(MOTOR_R_PIN1, rf);
  analogWrite(MOTOR_R_PIN2, rb);
}

void moveForward() {
  setMotorPWM(MOTOR_SPEED, 0, MOTOR_SPEED, 0);
}

void slideLeft() {
  // Left wheel actively brakes/reverses slightly, right wheel pushes forward
  setMotorPWM(0, SLIDE_BRAKE_SPEED, MOTOR_SPEED, 0);
}

void turnLeft() {
  setMotorPWM(0, MOTOR_SPEED, MOTOR_SPEED, 0); // Sharp pivot turn left
}

void slideRight() {
  // Right wheel actively brakes/reverses slightly, left wheel pushes forward
  setMotorPWM(MOTOR_SPEED, 0, 0, SLIDE_BRAKE_SPEED);
}

void turnRight() {
  setMotorPWM(MOTOR_SPEED, 0, 0, MOTOR_SPEED); // Sharp pivot turn right
}

void stopMotors() {
  setMotorPWM(0, 0, 0, 0);
}

// Signed wheel speeds (manual driving from the Pi, w a s d). + forward, - backward.
void drive(int left, int right)
{
    setMotorPWM(left > 0 ? left : 0, left < 0 ? -left : 0, right > 0 ? right : 0, right < 0 ? -right : 0);
}

// ════════════════════════════════════════════════════════════════════════════
//  LINE FOLLOWING (the original loop body, one 50 ms cycle)
// ════════════════════════════════════════════════════════════════════════════

void readLine(int &s1, int &s2, int &s3, int &s4, int &s5)
{
  // Inverted readings: '!' flips 0 to 1 and 1 to 0
  s1 = !digitalRead(IR_PINS[0]); // Leftmost
  s2 = !digitalRead(IR_PINS[1]); // Mid-Left
  s3 = !digitalRead(IR_PINS[2]); // Center
  s4 = !digitalRead(IR_PINS[3]); // Mid-Right
  s5 = !digitalRead(IR_PINS[4]); // Rightmost
}

void printSensorLine(int s1, int s2, int s3, int s4, int s5)
{
  // Print sensors, current PWM values, and status to Serial Monitor
  Serial.print("S1: "); Serial.print(s1);
  Serial.print("\tS2: "); Serial.print(s2);
  Serial.print("\tS3: "); Serial.print(s3);
  Serial.print("\tS4: "); Serial.print(s4);
  Serial.print("\tS5: "); Serial.print(s5);
  Serial.print("\t| PWM_L: "); Serial.print(currentLeftPWM);
  Serial.print("\tPWM_R: "); Serial.print(currentRightPWM);
  Serial.println();
}

// The original decision table. Drives the wheels and returns the original Serial text.
const char *lfrDecide(int s1, int s2, int s3, int s4, int s5)
{
  currentPattern = (uint8_t)((s1 << 4) | (s2 << 3) | (s3 << 2) | (s4 << 1) | s5);
  junction = false;
  lineLost = false;

  // Check if all sensors see white (off line / potential glitch)
  if (s1 == 0 && s2 == 0 && s3 == 0 && s4 == 0 && s5 == 0) {
    offLineCount++;
    if (offLineCount >= GLITCH_THRESHOLD) {
      stopMotors();
      lineLost = true;
      return "stop (confirmed off-line)";
    } else {
      // Ignore the momentary glitch and maintain forward momentum briefly
      moveForward();
      return "forwrd (glitch ignored)";
    }
  }

  // Reset glitch counter as soon as a valid signal is detected
  offLineCount = 0;

  // State matching logic
  if (s1 == 0 && s2 == 0 && s3 == 1 && s4 == 0 && s5 == 0) {
    moveForward();
    return "forwrd";
  }
  else if (s1 == 0 && s2 == 1 && s3 == 0 && s4 == 0 && s5 == 0) {
    slideLeft();
    return "slide left (active reverse brake)";
  }
  else if (s1 == 1 && s2 == 0 && s3 == 0 && s4 == 0 && s5 == 0) {
    turnLeft();
    return "left (sharp)";
  }
  else if (s1 == 0 && s2 == 0 && s3 == 0 && s4 == 1 && s5 == 0) {
    slideRight();
    return "slide right (active reverse brake)";
  }
  else if (s1 == 0 && s2 == 0 && s3 == 0 && s4 == 0 && s5 == 1) {
    turnRight();
    return "right (sharp)";
  }
  else if (s1 == 1 && s2 == 1 && s3 == 1 && s4 == 1 && s5 == 1) {
    stopMotors();
    junction = true;
    return "stop (line junction)";
  }
  else {
    moveForward();
    return "forwrd (default)";
  }
}

void resetLineState()
{
    offLineCount = 0;
    lineLost = false;
    junction = false;
    lfrDueNow = true;                    // the next line-following cycle runs at once
}

// One cycle every 50 ms, exactly like the original loop (but without blocking).
void lineFollowing()
{
    unsigned long now = millis();
    if (!lfrDueNow && now - lastLfrCycleMs < LFR_CYCLE_MS)
    {
        return;                          // between cycles the wheels keep the last command
    }
    lfrDueNow = false;
    lastLfrCycleMs = now;

    int s1, s2, s3, s4, s5;
    readLine(s1, s2, s3, s4, s5);
    const char *what = lfrDecide(s1, s2, s3, s4, s5);
    if (viewOn)                          // same Serial output as the original code
    {
        Serial.println(what);
        printSensorLine(s1, s2, s3, s4, s5);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  BUZZER, DEBUG, QR
// ════════════════════════════════════════════════════════════════════════════

void buzzerControl()
{
    bool timedOut = (millis() - lastSafetyHeartbeatMs) >= COMMAND_TIMEOUT_MS && !localHold();
    if (timedOut)
    {
        obstacleDetected = true;
    }
    digitalWrite(BUZZER_PIN, (obstacleDetected || timedOut) ? HIGH : LOW);   // safety stop: solid tone
}

void lineLostBeep()
{
    if (lineLost && !obstacleDetected)
    {
        bool beepOn = (millis() % LINE_LOST_BEEP_PERIOD_MS) < LINE_LOST_BEEP_ON_MS;
        digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);
    }
}

void debugReport()
{
    if (!DEBUG_SAFETY) return;
    unsigned long now = millis();
    if (now - lastDebugPrintMs < DEBUG_MIN_INTERVAL_MS) return;

    if (obstacleDetected != prevObstacleDebug)
    {
        Serial.print("DBG obstacle=");
        Serial.print((int)obstacleDetected);
        Serial.print(" hbAge=");
        Serial.println(now - lastSafetyHeartbeatMs);
        prevObstacleDebug = obstacleDetected;
        lastDebugPrintMs = now;
    }
    else if (lineLost != prevLineLostDebug)
    {
        Serial.print("DBG lineLost=");
        Serial.println((int)lineLost);
        prevLineLostDebug = lineLost;
        lastDebugPrintMs = now;
    }
    else if (currentLeftSpeed != prevLeftSpeedDebug || currentRightSpeed != prevRightSpeedDebug)
    {
        Serial.print("DBG motor=");
        Serial.print(currentLeftSpeed);
        Serial.print(',');
        Serial.println(currentRightSpeed);
        prevLeftSpeedDebug = currentLeftSpeed;
        prevRightSpeedDebug = currentRightSpeed;
        lastDebugPrintMs = now;
    }
}

void updateQROutputs()
{
    for (int i = 0; i < 10; i++)
    {
        digitalWrite(QR_OUTPUT_PINS[i], qrOutputs[i] ? HIGH : LOW);
    }
}

void resetQROutputs()
{
    for (int i = 0; i < 10; i++)
    {
        qrOutputs[i] = false;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  SERIAL MONITOR TOOLS
// ════════════════════════════════════════════════════════════════════════════

void saveFixes()
{
    EEPROM.update(EE_FIX, FIX_MAGIC);
    EEPROM.update(EE_FIX + 1, (byte)((flipA ? 1 : 0) | (flipB ? 2 : 0) | (swapSides ? 4 : 0)));
}

void loadFixes()
{
    if (EEPROM.read(EE_FIX) == FIX_MAGIC)
    {
        byte f = EEPROM.read(EE_FIX + 1);
        flipA = f & 1; flipB = f & 2; swapSides = f & 4;
    }
}

const char *modeName()
{
    if (fingerTest) return "FINGER-TEST";
    if (timeBefore(lfrTestUntilMs)) return "LFR-TEST";
    if (timeBefore(keyUntilMs)) return "KEYS";
    if (controlMode == MANUAL_MODE) return "MANUAL";
    return "AUTO";
}

void printPattern()
{
    int s1, s2, s3, s4, s5;
    readLine(s1, s2, s3, s4, s5);
    Serial.print(s1); Serial.print(s2); Serial.print(s3); Serial.print(s4); Serial.print(s5);
}

// Live view while the line follower is NOT running (it prints its own lines when it runs)
void printView()
{
    int s1, s2, s3, s4, s5;
    readLine(s1, s2, s3, s4, s5);
    unsigned long hb = millis() - lastSafetyHeartbeatMs;
    Serial.print("VIEW ");
    Serial.print(modeName());
    Serial.print(obstacleDetected ? " STOPPED" : " GO");
    Serial.print(" hb ");
    Serial.print(hb > 99999UL ? 99999UL : hb);
    Serial.print("ms\t");
    printSensorLine(s1, s2, s3, s4, s5);
}

void printStat()
{
    Serial.print("STAT mode=");
    Serial.print(controlMode == MANUAL_MODE ? "MANUAL" : "AUTO");
    Serial.print(" obstacle=");
    Serial.print((int)obstacleDetected);
    Serial.print(" hbAge=");
    Serial.print(millis() - lastSafetyHeartbeatMs);
    Serial.print(" motor=");
    Serial.print(currentLeftSpeed);
    Serial.print(',');
    Serial.print(currentRightSpeed);
    Serial.print(" line=");
    printPattern();
    Serial.print(" lost=");
    Serial.print((int)lineLost);
    Serial.println(" fw=3.1");
}

void printSettings()
{
    Serial.print("SETTINGS LFR speed ");
    Serial.print(MOTOR_SPEED);
    Serial.print(", slide brake ");
    Serial.print(SLIDE_BRAKE_SPEED);
    Serial.print(", glitch ");
    Serial.print(GLITCH_THRESHOLD);
    Serial.print(" cycles of ");
    Serial.print(LFR_CYCLE_MS);
    Serial.print(" ms | left wheel=pins ");
    Serial.print(swapSides ? "4/5" : "2/3");
    Serial.print((swapSides ? flipB : flipA) ? " reversed" : " normal");
    Serial.print(", right wheel=pins ");
    Serial.print(swapSides ? "2/3" : "4/5");
    Serial.print((swapSides ? flipA : flipB) ? " reversed" : " normal");
    Serial.print(" | key speed ");
    Serial.println(keySpeed);
}

void printHelp()
{
    Serial.println("== MediCart 3.1 Serial Monitor ==");
    Serial.println(" l/1 line following 60 s (no Pi)   x STOP   w a s d drive 1 s   + - key speed");
    Serial.println(" v live view on/off   i sensors once   t finger test (wheels lifted)");
    Serial.println(" TESTL TESTR FLIPL FLIPR SWAP   SETTINGS   FACTORY   STAT   h help");
    Serial.println(" (no camera brake during l / keys / t - keep someone next to the robot)");
}

void stopEverything(const char *why)
{
    keyUntilMs = 0;
    lfrTestUntilMs = 0;
    fingerTest = false;
    manualLeftSpeed = 0;
    manualRightSpeed = 0;
    obstacleDetected = true;
    stopMotors();
    Serial.print("STOPPED ");
    Serial.println(why);
}

void startKeyDrive(int l, int r, const char *name)
{
    lfrTestUntilMs = 0;
    fingerTest = false;
    controlMode = MANUAL_MODE;
    obstacleDetected = false;
    resetLineState();
    manualLeftSpeed = constrain(l, -MAX_MANUAL_SPEED, MAX_MANUAL_SPEED);
    manualRightSpeed = constrain(r, -MAX_MANUAL_SPEED, MAX_MANUAL_SPEED);
    keyUntilMs = millis() + KEY_HOLD_MS;
    if (keyUntilMs == 0) keyUntilMs = 1;
    Serial.print("KEY ");
    Serial.print(name);
    Serial.print(" L=");
    Serial.print(manualLeftSpeed);
    Serial.print(" R=");
    Serial.println(manualRightSpeed);
}

void startLfrTest()
{
    keyUntilMs = 0;
    fingerTest = false;
    controlMode = AUTO_MODE;
    obstacleDetected = false;
    resetLineState();
    lfrTestUntilMs = millis() + LFR_TEST_MS;
    if (lfrTestUntilMs == 0) lfrTestUntilMs = 1;
    Serial.println("LFR 60 s: line following without the Pi (x = stop, l again = 60 s more)");
}

// ---- "t" finger test: no fingers = the empty reading; a finger flips that sensor = the line there ----
byte readRawBits()
{
    byte b = 0;
    for (int n = 0; n < 5; n++) if (digitalRead(IR_PINS[n]) == HIGH) b |= (1 << n);
    return b;
}

void startFingerTest()
{
    stopEverything("(finger test)");
    fingerBase = readRawBits();
    fingerTest = true;
    fingerUntilMs = millis() + FINGER_TEST_MS;
    if (fingerUntilMs == 0) fingerUntilMs = 1;
    resetLineState();
    Serial.print("FINGER TEST 2 min. WHEELS LIFTED. No fingers reads raw ");
    for (int n = 0; n < 5; n++) Serial.print((fingerBase >> n) & 1);
    Serial.println(" -> a finger on a sensor = the line there. x = stop");
    if (fingerBase != 0 && fingerBase != 0x1F)
    {
        Serial.println("FINGER WARNING: the sensors don't agree. Take fingers away, nothing near the array, then t again.");
    }
}

void fingerStep()
{
    if (!timeBefore(fingerUntilMs))
    {
        fingerTest = false;
        obstacleDetected = true;
        stopMotors();
        Serial.println("FINGER TEST done (t = again)");
        return;
    }
    unsigned long now = millis();
    if (!lfrDueNow && now - lastLfrCycleMs < LFR_CYCLE_MS) return;
    lfrDueNow = false;
    lastLfrCycleMs = now;

    byte on = readRawBits() ^ fingerBase;            // bit n = finger on S(n+1)
    int s1 = (on & 1) ? 1 : 0, s2 = (on & 2) ? 1 : 0, s3 = (on & 4) ? 1 : 0, s4 = (on & 8) ? 1 : 0, s5 = (on & 16) ? 1 : 0;
    const char *what = lfrDecide(s1, s2, s3, s4, s5);
    Serial.print("FINGER ");
    Serial.println(what);
    printSensorLine(s1, s2, s3, s4, s5);
}

// Returns true when the line was a Serial Monitor tool command.
bool handleToolCommand(String command)
{
    String u = command;
    u.toUpperCase();

    // single keys; "wwww" (the same key repeated) counts as one press
    bool sameKey = command.length() >= 1 && command.length() <= 8;
    for (unsigned int n = 1; n < command.length() && sameKey; n++)
    {
        if (u.charAt(n) != u.charAt(0)) sameKey = false;
    }
    if (sameKey && strchr("WASDX+-VIH?L1T", u.charAt(0)) != NULL)
    {
        switch (u.charAt(0))
        {
            case 'W': startKeyDrive(keySpeed, keySpeed, "w forward"); return true;
            case 'S': startKeyDrive(-keySpeed, -keySpeed, "s back"); return true;
            case 'A': startKeyDrive(-keySpeed, keySpeed, "a pivot left"); return true;
            case 'D': startKeyDrive(keySpeed, -keySpeed, "d pivot right"); return true;
            case 'X': stopEverything("(x)"); return true;
            case '+': keySpeed = min(MAX_MANUAL_SPEED, keySpeed + KEY_SPEED_STEP); Serial.print("KEY speed "); Serial.println(keySpeed); return true;
            case '-': keySpeed = max(KEY_SPEED_STEP, keySpeed - KEY_SPEED_STEP); Serial.print("KEY speed "); Serial.println(keySpeed); return true;
            case 'V': viewOn = !viewOn; Serial.println(viewOn ? "VIEW on (v = off)" : "VIEW off (v = on)"); return true;
            case 'I':
            {
                int s1, s2, s3, s4, s5;
                readLine(s1, s2, s3, s4, s5);
                printSensorLine(s1, s2, s3, s4, s5);
                return true;
            }
            case 'T': startFingerTest(); return true;
            case 'H': case '?': printHelp(); return true;
            case 'L': case '1': startLfrTest(); return true;
            default: return false;
        }
    }
    if (u == "VIEW 0" || u == "VIEW 1") { viewOn = (u == "VIEW 1"); return true; }
    if (u == "STAT") { printStat(); return true; }
    if (u == "SETTINGS") { printSettings(); return true; }
    if (u == "TESTL" || u == "TESTR")
    {
        bool left = (u == "TESTL");
        startKeyDrive(left ? keySpeed : 0, left ? 0 : keySpeed,
                      left ? "TESTL only the LEFT wheel, forward (wrong wheel: SWAP, backward: FLIPL)"
                           : "TESTR only the RIGHT wheel, forward (wrong wheel: SWAP, backward: FLIPR)");
        return true;
    }
    if (u == "FLIPL") { if (swapSides) flipB = !flipB; else flipA = !flipA; saveFixes(); printSettings(); return true; }
    if (u == "FLIPR") { if (swapSides) flipA = !flipA; else flipB = !flipB; saveFixes(); printSettings(); return true; }
    if (u == "SWAP")  { swapSides = !swapSides; saveFixes(); printSettings(); return true; }
    if (u == "FACTORY")
    {
        flipA = flipB = swapSides = false;
        saveFixes();
        Serial.println("FACTORY: wheel fixes cleared");
        printSettings();
        return true;
    }
    return false;
}

// ════════════════════════════════════════════════════════════════════════════
//  PI COMMANDS
// ════════════════════════════════════════════════════════════════════════════

void runCommand(String command)
{
    command.trim();
    if (command.length() == 0) return;

    if (command == "SAFE" || command == "STOP" || command.startsWith("MODE:") || command.startsWith("DRIVE:"))
    {
        keyUntilMs = 0;                      // the Pi is in charge again
        lfrTestUntilMs = 0;
        if (fingerTest) { fingerTest = false; stopMotors(); }
    }

    if (handleToolCommand(command)) return;

    if (command == "STOP")
    {
        lastSafetyHeartbeatMs = millis();
        obstacleDetected = true;
        manualLeftSpeed = 0;
        manualRightSpeed = 0;
    }
    else if (command == "SAFE" || command == "MODE:AUTO")
    {
        lastSafetyHeartbeatMs = millis();
        if (controlMode != AUTO_MODE || obstacleDetected) resetLineState();
        controlMode = AUTO_MODE;
        obstacleDetected = false;
    }
    else if (command == "MODE:MANUAL")
    {
        lastSafetyHeartbeatMs = millis();
        if (controlMode != MANUAL_MODE)
        {
            manualLeftSpeed = 0;
            manualRightSpeed = 0;
        }
        controlMode = MANUAL_MODE;
        obstacleDetected = false;
        resetLineState();
    }
    else if (command.startsWith("DRIVE:"))
    {
        int comma = command.indexOf(',', 6);
        if (controlMode == MANUAL_MODE && comma > 6)
        {
            manualLeftSpeed = constrain(command.substring(6, comma).toInt(), -MAX_MANUAL_SPEED, MAX_MANUAL_SPEED);
            manualRightSpeed = constrain(command.substring(comma + 1).toInt(), -MAX_MANUAL_SPEED, MAX_MANUAL_SPEED);
            lastSafetyHeartbeatMs = millis();
        }
    }
    else if (command == "RESET_QR")
    {
        resetQROutputs();
    }
    else if (command.startsWith("QR:"))
    {
        int qrValue = command.substring(3).toInt();   // QR commands never count as a heartbeat
        if (qrValue >= 1 && qrValue <= 10)
        {
            resetQROutputs();
            qrOutputs[qrValue - 1] = true;
        }
    }
}

void processSerialCommands()
{
    while (Serial.available() > 0)
    {
        char incoming = Serial.read();
        if (incoming == '\n' || incoming == '\r')
        {
            String command = serialBuffer;
            serialBuffer = "";
            runCommand(command);
        }
        else
        {
            serialBuffer += incoming;
            lastCharMs = millis();
            if (serialBuffer.length() > 64) serialBuffer = "";   // garbage without a newline
        }
    }

    // "No line ending" in the Serial Monitor: run what arrived after a short pause
    if (serialBuffer.length() > 0 && millis() - lastCharMs > 80)
    {
        String command = serialBuffer;
        serialBuffer = "";
        command.trim();
        if (command.length() > 0) handleToolCommand(command);
    }

    if ((millis() - lastSafetyHeartbeatMs) >= COMMAND_TIMEOUT_MS && !localHold())
    {
        obstacleDetected = true;
        stopMotors();
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(SERIAL_BAUD);
  serialBuffer.reserve(32);

  // Configure motor control pins as outputs
  pinMode(MOTOR_L_PIN1, OUTPUT);
  pinMode(MOTOR_L_PIN2, OUTPUT);
  pinMode(MOTOR_R_PIN1, OUTPUT);
  pinMode(MOTOR_R_PIN2, OUTPUT);

  // Configure IR sensor pins as inputs
  for (int i = 0; i < 5; i++) {
    pinMode(IR_PINS[i], INPUT);
  }

  pinMode(BUZZER_PIN, OUTPUT);

  for (int i = 0; i < 10; i++)
  {
    pinMode(QR_OUTPUT_PINS[i], OUTPUT);
    digitalWrite(QR_OUTPUT_PINS[i], LOW);
  }

  stopMotors();
  digitalWrite(BUZZER_PIN, LOW);
  lastSafetyHeartbeatMs = millis();
  loadFixes();

  Serial.println("MediCart ready");
  Serial.println("FW medicart 3.1");             // the Pi checks this
  Serial.println("Type l to line-follow without the Pi, h for help, v to stop/start the live view.");
}

void loop() {
  processSerialCommands();

  if (fingerTest)                                // "t": no Pi timeout, no buzzer
  {
    fingerStep();
    digitalWrite(BUZZER_PIN, LOW);
    updateQROutputs();
    return;
  }
  if (keyUntilMs != 0 && !timeBefore(keyUntilMs))   // key press time is up
  {
    keyUntilMs = 0;
    manualLeftSpeed = 0;
    manualRightSpeed = 0;
  }
  if (lfrTestUntilMs != 0 && !timeBefore(lfrTestUntilMs))
  {
    lfrTestUntilMs = 0;
    Serial.println("LFR done (l = 60 s more)");
  }
  buzzerControl();

  bool following = false;
  if (obstacleDetected)
  {
    stopMotors();
    resetLineState();                            // a safety stop is not "line lost"
  }
  else if (controlMode == MANUAL_MODE)
  {
    drive(manualLeftSpeed, manualRightSpeed);
    resetLineState();
  }
  else
  {
    lineFollowing();
    following = true;
  }

  lineLostBeep();
  debugReport();
  updateQROutputs();

  if (viewOn && !following && millis() - lastViewMs >= VIEW_EVERY_MS)
  {
    lastViewMs = millis();
    printView();
  }
}
