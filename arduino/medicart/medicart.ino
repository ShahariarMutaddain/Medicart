#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
//  MediCart firmware 2.1  =  Rifat's line-following firmware  +  manual drive
//
//  AUTO mode (default at power-on) is Rifat's original behaviour, unchanged:
//    SAFE / STOP heartbeats from medicart.py, 2 s watchdog, QR:n / RESET_QR.
//  Everything added for the control system is marked  [CTRL-2.1].
//
//  [CTRL-2.1] new serial commands (sent by the Pi's medicart-control service):
//    MAN:<left>,<right>  wheel PWM -255..255 (+ = forward), clamped to MANUAL_MAX_PWM.
//                        Enters MANUAL mode. Must repeat at least every
//                        MANUAL_TIMEOUT_MS (0.5 s) or the wheels stop by themselves.
//    AUTO                leave MANUAL: motors stop; line following resumes on the next SAFE.
//    STAT                prints one line:
//                        STAT mode=AUTO obstacle=1 hbAge=120 man=0,0 manAge=0 line=00100 lost=0
//    STOP                (original) in MANUAL it also stops the wheels at once.
// ════════════════════════════════════════════════════════════════════════════

// [FIX-2] Optional AVR hardware watchdog. OFF by default.
#define USE_HW_WATCHDOG 0
#if USE_HW_WATCHDOG && defined(ARDUINO_ARCH_AVR)
#include <avr/wdt.h>
#endif

// ==========================================
// MOTOR PIN DEFINITIONS
// ==========================================

// Left Motor - BTS Driver
const int MOTOR_L_PIN1 = 2;   // Forward PWM
const int MOTOR_L_PIN2 = 3;   // Reverse PWM

// Right Motor - BTS Driver
const int MOTOR_R_PIN1 = 4;   // Forward PWM
const int MOTOR_R_PIN2 = 5;   // Reverse PWM

// ==========================================
// 5-CHANNEL IR SENSOR ARRAY
// ==========================================

// [0] Far Left   = S1
// [1] Left       = S2
// [2] Center     = S3
// [3] Right      = S4
// [4] Far Right  = S5
const int IR_PINS[5] = {A8, A9, A10, A11, A12};

// ==========================================
// BUZZER AND QR OUTPUT PINS
// ==========================================

const int BUZZER_PIN = 24;

const int QR_OUTPUT_PINS[10] = {6, 7, 8, 9, 10, 11, 12, 13, 22, 23};

// QR 1 -> pin 6,  QR 2 -> pin 7,  QR 3 -> pin 8,  QR 4 -> pin 9,  QR 5 -> pin 10
// QR 6 -> pin 11, QR 7 -> pin 12, QR 8 -> pin 13, QR 9 -> pin 22, QR 10 -> pin 23

// ==========================================
// SETTINGS
// ==========================================

// true  = black line on white surface
// false = white line on black surface
const bool BLACK_LINE = true;

// ---- Motor speeds (0-255) used by the line-following table ----
const int FULL_SPEED          = 180;  // 0 0 1 0 0  perfect center
const int FORWARD_SPEED       = 150;  // 0 1 1 1 0  wide center, and the "fast" side of every turn
const int SOFT_INNER_SPEED    = 120;  // inner wheel on a soft turn
const int MEDIUM_INNER_SPEED  = 60;   // inner wheel on a medium turn
const int HARD_INNER_SPEED    = 0;    // inner wheel on a hard turn
const int HARD_OUTER_SPEED    = 180;  // outer wheel on a hard turn
const int SEARCH_SPEED        = 110;  // pivot speed while searching for a lost line

// ---- Intersection (1 1 1 1 1) ----
// Stop for INTERSECTION_PAUSE_MS (your "execute preset" goes here), then drive
// straight across for INTERSECTION_CROSS_MS, then stop until the pattern changes.
const unsigned long INTERSECTION_PAUSE_MS = 1500;
const unsigned long INTERSECTION_CROSS_MS = 800;

// ---- Lost line (0 0 0 0 0) ----
// Pivot toward the side where the line was last seen for up to LOST_SEARCH_MS,
// then stop. Set LOST_LINE_RECOVERY to false for "just stop immediately".
const bool LOST_LINE_RECOVERY = true;
const unsigned long LOST_SEARCH_MS = 1500;

const unsigned long COMMAND_TIMEOUT_MS = 2000;
const bool DEBUG_IR_SENSORS = false;

// ---- [CTRL-2.1] Manual drive ----
const int MANUAL_MAX_PWM = 200;                // hard cap for manual wheel PWM
const unsigned long MANUAL_TIMEOUT_MS = 500;   // no MAN: for this long -> wheels stop
const int MANUAL_ACCEL_PER_MS = 1;             // speed-up limit: 0 -> 200 PWM in 0.2 s
const int MANUAL_DECEL_PER_MS = 3;             // slow-down limit when the operator lets go
const bool MANUAL_MOVE_BEEP = false;           // true = short chirp every second while driving manually

// Debug output for diagnosis. Python prints these as "[ARDUINO] ...".
const bool DEBUG_SAFETY = true;
// Prints "DBG sensors=00100" whenever the sensor pattern changes (can be chatty).
const bool DEBUG_SENSORS = false;
const unsigned long DEBUG_MIN_INTERVAL_MS = 50;

// "Line lost" beep pattern: 100 ms ON, 400 ms OFF (safety stop stays a SOLID tone).
const unsigned long LINE_LOST_BEEP_PERIOD_MS = 500;
const unsigned long LINE_LOST_BEEP_ON_MS = 100;

// ==========================================
// GLOBAL STATE
// ==========================================

bool obstacleDetected = true;

// [CTRL-2.1] manual-mode state
bool manualMode = false;
int manTargetL = 0, manTargetR = 0;      // last MAN: request
int manCurL = 0, manCurR = 0;            // ramped values actually applied
unsigned long lastManualMs = 0;
unsigned long lastRampMs = 0;
bool qrOutputs[10] = {false};
String serialBuffer = "";
unsigned long lastSafetyHeartbeatMs = 0;
unsigned long lastIRDebugMs = 0;

// Set by lineFollowing() when all 5 IR sensors see no line.
bool lineLost = false;
unsigned long lastLineLostMs = 0;

// ---- Line-following memory ----
int lastLineSide = 0;                    // -1 = line was last on the left, 0 = center/unknown, +1 = right
bool intersectionActive = false;         // true while the pattern is 1 1 1 1 1
unsigned long intersectionStartMs = 0;
unsigned long lostStartMs = 0;           // when the current lost-line episode began
uint8_t currentPattern = 0;              // latest sensor pattern, bit 4 = S1 ... bit 0 = S5

// Debug bookkeeping: what we last printed.
bool prevObstacleDebug = true;
bool prevLineLostDebug = false;
uint8_t prevPatternDebug = 0xFF;
unsigned long lastDebugPrintMs = 0;

// ==========================================
// MOTOR CONTROL
// ==========================================

void leftMotor(int speed)
{
    if (speed > 0)
    {
        analogWrite(MOTOR_L_PIN1, speed);
        analogWrite(MOTOR_L_PIN2, 0);
    }
    else if (speed < 0)
    {
        analogWrite(MOTOR_L_PIN1, 0);
        analogWrite(MOTOR_L_PIN2, -speed);
    }
    else
    {
        analogWrite(MOTOR_L_PIN1, 0);
        analogWrite(MOTOR_L_PIN2, 0);
    }
}

void rightMotor(int speed)
{
    if (speed > 0)
    {
        analogWrite(MOTOR_R_PIN1, speed);
        analogWrite(MOTOR_R_PIN2, 0);
    }
    else if (speed < 0)
    {
        analogWrite(MOTOR_R_PIN1, 0);
        analogWrite(MOTOR_R_PIN2, -speed);
    }
    else
    {
        analogWrite(MOTOR_R_PIN1, 0);
        analogWrite(MOTOR_R_PIN2, 0);
    }
}

void drive(int leftSpeed, int rightSpeed)
{
    leftMotor(leftSpeed);
    rightMotor(rightSpeed);
}

void stopMotors()
{
    leftMotor(0);
    rightMotor(0);
}

// ---- Steering helpers (turning LEFT = slow down the LEFT wheel) ----

void forwardFull()   { drive(FULL_SPEED, FULL_SPEED); }
void forwardNormal() { drive(FORWARD_SPEED, FORWARD_SPEED); }

void turnLeftSoft()   { drive(SOFT_INNER_SPEED,   FORWARD_SPEED); }
void turnLeftMedium() { drive(MEDIUM_INNER_SPEED, FORWARD_SPEED); }
void turnLeftHard()   { drive(HARD_INNER_SPEED,   HARD_OUTER_SPEED); }

void turnRightSoft()   { drive(FORWARD_SPEED, SOFT_INNER_SPEED); }
void turnRightMedium() { drive(FORWARD_SPEED, MEDIUM_INNER_SPEED); }
void turnRightHard()   { drive(HARD_OUTER_SPEED, HARD_INNER_SPEED); }

void pivotLeft(int speed)  { drive(-speed, speed); }   // spin in place, counter-clockwise
void pivotRight(int speed) { drive(speed, -speed); }   // spin in place, clockwise

// ==========================================
// READ SENSOR
// ==========================================

// true = this sensor sees the line (the "1" in your table)
bool sensorActive(int index)
{
    int value = digitalRead(IR_PINS[index]);

    if (BLACK_LINE)
    {
        return value == LOW;
    }
    else
    {
        return value == HIGH;
    }
}

void readIRSensors(bool &s0, bool &s1, bool &s2, bool &s3, bool &s4)
{
    s0 = sensorActive(0);
    s1 = sensorActive(1);
    s2 = sensorActive(2);
    s3 = sensorActive(3);
    s4 = sensorActive(4);
}

// ==========================================
// LINE FOLLOWING
// ==========================================

// Clears the timers/flags so a resume after a safety stop starts fresh.
void resetLineState()
{
    lineLost = false;
    intersectionActive = false;
}

// Used ONLY for patterns that are not in your table (for example 1 1 1 0 0 or
// 0 1 1 0 0). It works out where the line is on average:
//   S1..S5 count as positions -2, -1, 0, +1, +2
// and then picks the closest of your table's actions.
void steerByPosition(bool s0, bool s1, bool s2, bool s3, bool s4)
{
    int count = (int)s0 + (int)s1 + (int)s2 + (int)s3 + (int)s4;
    float position = (-2.0f * s0 - 1.0f * s1 + 1.0f * s3 + 2.0f * s4) / (float)count;

    if (position <= -1.75f)      { turnLeftHard();    lastLineSide = -1; }
    else if (position <= -1.25f) { turnLeftMedium();  lastLineSide = -1; }
    else if (position <= -0.5f)  { turnLeftSoft();    lastLineSide = -1; }
    else if (position < 0.5f)    { forwardNormal();   lastLineSide = 0;  }
    else if (position < 1.25f)   { turnRightSoft();   lastLineSide = 1;  }
    else if (position < 1.75f)   { turnRightMedium(); lastLineSide = 1;  }
    else                         { turnRightHard();   lastLineSide = 1;  }
}

void lineFollowing()
{
    if (obstacleDetected)
    {
        stopMotors();
        return;
    }

    bool s0, s1, s2, s3, s4;
    readIRSensors(s0, s1, s2, s3, s4);

    if (DEBUG_IR_SENSORS && millis() - lastIRDebugMs >= 1000)
    {
        Serial.print(s0);
        Serial.print(s1);
        Serial.print(s2);
        Serial.print(s3);
        Serial.println(s4);
        lastIRDebugMs = millis();
    }

    unsigned long now = millis();
    bool wasLost = lineLost;   // remember last pass, then assume "line found" until proven otherwise
    lineLost = false;

    // Pack the 5 sensors into one number so it reads like your table:
    // pattern 0b01000 means S1=0 S2=1 S3=0 S4=0 S5=0
    currentPattern = (uint8_t)((s0 << 4) | (s1 << 3) | (s2 << 2) | (s3 << 1) | (s4 << 0));

    // ---- 1 1 1 1 1 : Intersection / Stop / Execute Preset ----
    if (currentPattern == 0b11111)
    {
        if (!intersectionActive)
        {
            intersectionActive = true;
            intersectionStartMs = now;
        }
        unsigned long elapsed = now - intersectionStartMs;
        if (elapsed < INTERSECTION_PAUSE_MS)
        {
            stopMotors();           // pause here; put your preset action in this spot
        }
        else if (elapsed < INTERSECTION_PAUSE_MS + INTERSECTION_CROSS_MS)
        {
            forwardNormal();        // drive straight across the crossing
        }
        else
        {
            stopMotors();           // still all-black: stop instead of driving blindly
        }
        return;
    }
    intersectionActive = false;

    // ---- 0 0 0 0 0 : Lost line -> recovery using memory of last position ----
    if (currentPattern == 0b00000)
    {
        lineLost = true;            // drives the beep + "DBG lineLost" output
        lastLineLostMs = now;
        if (!wasLost)
        {
            lostStartMs = now;      // a new lost-line episode just began
        }

        if (LOST_LINE_RECOVERY && lastLineSide != 0 && (now - lostStartMs) < LOST_SEARCH_MS)
        {
            if (lastLineSide < 0)
            {
                pivotLeft(SEARCH_SPEED);    // line was last on the left: search left
            }
            else
            {
                pivotRight(SEARCH_SPEED);   // line was last on the right: search right
            }
        }
        else
        {
            stopMotors();
        }
        return;
    }

    // ---- The rest of your table ----
    switch (currentPattern)
    {
        case 0b00100: forwardFull();   lastLineSide = 0;  break;   // Perfect center
        case 0b01110: forwardNormal(); lastLineSide = 0;  break;   // Center (wide)

        case 0b01000: turnLeftSoft();    lastLineSide = -1; break; // Slight left
        case 0b11000: turnLeftMedium();  lastLineSide = -1; break; // Medium left
        case 0b10000: turnLeftHard();    lastLineSide = -1; break; // Sharp left

        case 0b00010: turnRightSoft();   lastLineSide = 1;  break; // Slight right
        case 0b00011: turnRightMedium(); lastLineSide = 1;  break; // Medium right
        case 0b00001: turnRightHard();   lastLineSide = 1;  break; // Sharp right

        default:
            // A pattern your table doesn't list: steer by where the line is on average.
            steerByPosition(s0, s1, s2, s3, s4);
            break;
    }
}

// ==========================================
// BUZZER AND QR OUTPUT CONTROL
// ==========================================

void buzzerControl()
{
    unsigned long now = millis();
    bool timedOut = (now - lastSafetyHeartbeatMs) >= COMMAND_TIMEOUT_MS;
    if (timedOut)
    {
        obstacleDetected = true;
    }
    // Safety stop or watchdog timeout: SOLID buzzer.
    bool buzzerOn = obstacleDetected || timedOut;
    digitalWrite(BUZZER_PIN, buzzerOn ? HIGH : LOW);
}

// Line lost (and NOT a safety stop): short beep, 100 ms on / 400 ms off.
void lineLostBeep()
{
    if (lineLost && !obstacleDetected)
    {
        bool beepOn = (millis() % LINE_LOST_BEEP_PERIOD_MS) < LINE_LOST_BEEP_ON_MS;
        digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);
    }
}

// Print only when something changed, and never faster than DEBUG_MIN_INTERVAL_MS.
void debugReport()
{
    if (!DEBUG_SAFETY)
    {
        return;
    }

    unsigned long now = millis();
    if (now - lastDebugPrintMs < DEBUG_MIN_INTERVAL_MS)
    {
        return;
    }

    if (obstacleDetected != prevObstacleDebug)
    {
        // hbAge < 2000  -> an explicit STOP/SAFE command caused it
        // hbAge >= 2000 -> the 2 s watchdog timed out (no heartbeats arriving)
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
    else if (DEBUG_SENSORS && currentPattern != prevPatternDebug)
    {
        Serial.print("DBG sensors=");
        for (int bit = 4; bit >= 0; bit--)
        {
            Serial.print((currentPattern >> bit) & 1);
        }
        Serial.println();
        prevPatternDebug = currentPattern;
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

// ==========================================
// [CTRL-2.1] MANUAL DRIVE
// ==========================================

void manualHalt()
{
    manTargetL = manTargetR = 0;
    manCurL = manCurR = 0;
    stopMotors();
}

void enterManual()
{
    if (!manualMode)
    {
        manualMode = true;
        manualHalt();
        resetLineState();
        lastRampMs = millis();
        digitalWrite(BUZZER_PIN, LOW);
        Serial.println("DBG mode=MANUAL");
    }
}

void leaveManual()
{
    if (manualMode)
    {
        Serial.println("DBG mode=AUTO");
    }
    manualMode = false;
    manualHalt();
    obstacleDetected = true;      // wait for a fresh SAFE from medicart.py before moving
    resetLineState();
}

int rampToward(int current, int target, unsigned long dt)
{
    // speeding up (|target| > |current| in the same direction) is slower than slowing down
    bool speedingUp = (target > 0 && target > current && current >= 0) ||
                      (target < 0 && target < current && current <= 0);
    long step = (long)dt * (speedingUp ? MANUAL_ACCEL_PER_MS : MANUAL_DECEL_PER_MS);
    if (step < 1) step = 1;
    if (target > current) return (int)min((long)target, (long)current + step);
    if (target < current) return (int)max((long)target, (long)current - step);
    return current;
}

void manualDrive()
{
    unsigned long now = millis();
    unsigned long dt = now - lastRampMs;
    if (dt == 0)
    {
        return;
    }
    lastRampMs = now;

    if (now - lastManualMs >= MANUAL_TIMEOUT_MS)
    {
        manualHalt();             // operator link silent: stop at once, no ramp
    }
    else
    {
        manCurL = rampToward(manCurL, manTargetL, dt);
        manCurR = rampToward(manCurR, manTargetR, dt);
        drive(manCurL, manCurR);
    }

    bool moving = (manCurL != 0 || manCurR != 0);
    bool chirp = MANUAL_MOVE_BEEP && moving && (now % 1000UL) < 60;
    digitalWrite(BUZZER_PIN, chirp ? HIGH : LOW);
}

void printStatus()
{
    unsigned long now = millis();
    bool s0, s1, s2, s3, s4;
    readIRSensors(s0, s1, s2, s3, s4);
    Serial.print("STAT mode=");
    Serial.print(manualMode ? "MANUAL" : "AUTO");
    Serial.print(" obstacle=");
    Serial.print((int)obstacleDetected);
    Serial.print(" hbAge=");
    Serial.print(now - lastSafetyHeartbeatMs);
    Serial.print(" man=");
    Serial.print(manCurL);
    Serial.print(",");
    Serial.print(manCurR);
    Serial.print(" manAge=");
    Serial.print(manualMode ? (now - lastManualMs) : 0UL);
    Serial.print(" line=");
    Serial.print((int)s0); Serial.print((int)s1); Serial.print((int)s2); Serial.print((int)s3); Serial.print((int)s4);
    Serial.print(" lost=");
    Serial.println((int)lineLost);
}

// ==========================================
// SERIAL COMMAND PARSING
// ==========================================

void processSerialCommands()
{
    while (Serial.available() > 0)
    {
        char incoming = Serial.read();

        if (incoming == '\n' || incoming == '\r')
        {
            String command = serialBuffer;
            serialBuffer = "";
            command.trim();

            if (command.length() == 0)
            {
                continue;
            }

            if (command == "STOP")
            {
                lastSafetyHeartbeatMs = millis();
                obstacleDetected = true;
                if (manualMode)                       // [CTRL-2.1] E-stop in manual: halt now
                {
                    manualHalt();
                }
            }
            else if (command == "SAFE")
            {
                lastSafetyHeartbeatMs = millis();
                obstacleDetected = false;
            }
            else if (command == "RESET_QR")
            {
                resetQROutputs();
            }
            else if (command.startsWith("MAN:"))     // [CTRL-2.1]
            {
                int comma = command.indexOf(',', 4);
                if (comma > 4)
                {
                    int l = command.substring(4, comma).toInt();
                    int r = command.substring(comma + 1).toInt();
                    enterManual();
                    manTargetL = constrain(l, -MANUAL_MAX_PWM, MANUAL_MAX_PWM);
                    manTargetR = constrain(r, -MANUAL_MAX_PWM, MANUAL_MAX_PWM);
                    lastManualMs = millis();
                }
            }
            else if (command == "AUTO")              // [CTRL-2.1]
            {
                leaveManual();
            }
            else if (command == "STAT")              // [CTRL-2.1]
            {
                printStatus();
            }
            else if (command.startsWith("QR:"))
            {
                // QR commands never touch lastSafetyHeartbeatMs.
                int qrValue = command.substring(3).toInt();
                if (qrValue >= 1 && qrValue <= 10)
                {
                    resetQROutputs();
                    qrOutputs[qrValue - 1] = true;
                }
            }
        }
        else
        {
            serialBuffer += incoming;
            if (serialBuffer.length() > 64)          // [CTRL-2.1] garbage without newline: drop it
            {
                serialBuffer = "";
            }
        }
    }

    if ((millis() - lastSafetyHeartbeatMs) >= COMMAND_TIMEOUT_MS)
    {
        obstacleDetected = true;
        if (!manualMode)                             // [CTRL-2.1] manual has its own 0.5 s watchdog
        {
            stopMotors();
        }
    }
}

// ==========================================
// SETUP
// ==========================================

void setup()
{
#if USE_HW_WATCHDOG && defined(ARDUINO_ARCH_AVR)
    MCUSR = 0;
    wdt_disable();
#endif

    Serial.begin(115200);
    serialBuffer.reserve(32);

    for (int i = 0; i < 5; i++)
    {
        pinMode(IR_PINS[i], INPUT);
    }

    pinMode(MOTOR_L_PIN1, OUTPUT);
    pinMode(MOTOR_L_PIN2, OUTPUT);
    pinMode(MOTOR_R_PIN1, OUTPUT);
    pinMode(MOTOR_R_PIN2, OUTPUT);

    pinMode(BUZZER_PIN, OUTPUT);

    for (int i = 0; i < 10; i++)
    {
        pinMode(QR_OUTPUT_PINS[i], OUTPUT);
        digitalWrite(QR_OUTPUT_PINS[i], LOW);
    }

    stopMotors();
    digitalWrite(BUZZER_PIN, LOW);
    lastSafetyHeartbeatMs = millis();

    Serial.println("MediCart ready");
    Serial.println("FW medicart-ctrl 2.1");      // [CTRL-2.1] lets the Pi know manual drive is available

#if USE_HW_WATCHDOG && defined(ARDUINO_ARCH_AVR)
    wdt_enable(WDTO_2S);
#endif
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop()
{
#if USE_HW_WATCHDOG && defined(ARDUINO_ARCH_AVR)
    wdt_reset();
#endif

    processSerialCommands();

    if (manualMode)                 // [CTRL-2.1] manual: Pi drives the wheels directly
    {
        manualDrive();
    }
    else                            // AUTO: Rifat's original loop, unchanged
    {
        buzzerControl();

        if (obstacleDetected)
        {
            stopMotors();
            resetLineState();   // a safety stop is not "line lost"; also restarts intersection/search timers
        }
        else
        {
            lineFollowing();
        }

        lineLostBeep();
    }
    debugReport();
    updateQROutputs();
}
