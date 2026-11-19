#include <Arduino.h>

// ════════════════════════════════════════════════════════════════════════════
//  MediCart firmware 3.4  ·  IUB Robotics Team  ·  Arduino Mega 2560  ·  115200 baud
//
//  Line following AND the Serial Monitor manual mode = the LFR code that ran on the robot,
//  unchanged: same pins, speeds, gains, position filter, slew rate, lost-line search,
//  p / w a s d x keys, and its SMOOTH stop (stopMotors() ramps down at 300 PWM/s).
//  Its functions are copied as they were (marked "LFR"). Every stop uses that same
//  smooth stopMotors(): Pi STOP (person / emergency), the 750 ms timeout, x, p, end of l.
//
//  [3.4] Reverse turning from the Nov 11 LFR code, added on top (REVERSE_TURN_ASSIST):
//    only S2 sees the line -> slide left : left wheel BACK 15 (SLIDE_BRAKE_SPEED), right wheel forward 50
//    only S1 sees the line -> sharp left : left wheel BACK 50, right wheel forward 50 (pivot)
//    only S4 / only S5     -> the same to the right
//  Everything else is today's code unchanged, and every change still goes through driveSmoothly(),
//  so the turns ramp in and out smoothly. REVERSE_TURN_ASSIST = false gives exactly today's code.
//  Around it, the MediCart architecture: the Pi decides WHEN the cart may line-follow.
//
//  Pi commands (protocol unchanged):
//    SAFE / MODE:AUTO      path clear -> line following      STOP   stop now (person / emergency)
//    MODE:MANUAL           manual mode                       DRIVE:<left>,<right>  wheel PWM (max 180)
//    QR:1..10 / RESET_QR   bed output pins                   No command for 750 ms -> motors stop
//
//  Serial Monitor (115200, any line ending):
//    p           manual mode on/off (as in the LFR code)    w a s d  drive (latched)   x or space  stop
//    l           line following WITHOUT the Pi for 60 s (l again = 60 s more; x stops)
//    v           live view on/off     i  sensors once     h  help     STAT  status line (used by the Pi)
//  There is NO camera brake in p-manual or the l run. Keep someone next to the robot.
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
//  LFR CODE (values unchanged)
// ════════════════════════════════════════════════════════════════════════════

// Motor driver pins
const uint8_t MOTOR_L_PIN1 = 2;
const uint8_t MOTOR_L_PIN2 = 3;
const uint8_t MOTOR_R_PIN1 = 4;
const uint8_t MOTOR_R_PIN2 = 5;

// IR sensors: far left, left, center, right, far right
const uint8_t IR_PINS[5] = {A8, A9, A10, A11, A12};

// true for a black line on a white surface; false for a white line on black
const bool BLACK_LINE = true;

// Line-following tuning
const int LINE_BASE_SPEED = 50;
const int LINE_STEERING_GAIN = 20;
const int LINE_MAX_STEERING = 40;
const int MAX_MOTOR_SPEED = 50;
const int SEARCH_SPEED = 50;
const int MANUAL_SPEED = 50;
const int MOTOR_SLEW_RATE_PER_SECOND = 300;
const unsigned long POSITION_FILTER_MS = 25;
const unsigned long LOST_LINE_SEARCH_MS = 1500;

int lastLineSide = 0; // -1 = left, +1 = right
int currentLeftSpeed = 0;
int currentRightSpeed = 0;
float filteredPosition = 0.0f;
bool positionFilterInitialized = false;
bool lineLost = false;
bool manualMode = false;
int manualLeftTarget = 0;
int manualRightTarget = 0;
unsigned long lostLineStartMs = 0;
unsigned long lastMotorUpdateMs = 0;
unsigned long motorSlewRemainder = 0;
unsigned long lastPositionUpdateMs = 0;

// ════════════════════════════════════════════════════════════════════════════
//  [3.4] REVERSE TURNING (from the Nov 11 LFR code)
// ════════════════════════════════════════════════════════════════════════════

const bool REVERSE_TURN_ASSIST = true;           // false = today's LFR code exactly
// Active slide brake speed: inside wheel spins slightly backward (PWM 15) for ultra-hard cornering on both sides
const int SLIDE_BRAKE_SPEED = 15;

// ════════════════════════════════════════════════════════════════════════════
//  MEDICART ARCHITECTURE
// ════════════════════════════════════════════════════════════════════════════

const long SERIAL_BAUD = 115200;                 // the Pi (medicart.py) talks at 115200
const int BUZZER_PIN = 25;
const int QR_OUTPUT_PINS[10] = {6, 7, 8, 9, 10, 11, 12, 13, 22, 23};
// QR 1 -> pin 6,  QR 2 -> pin 7,  QR 3 -> pin 8,  QR 4 -> pin 9,  QR 5 -> pin 10
// QR 6 -> pin 11, QR 7 -> pin 12, QR 8 -> pin 13, QR 9 -> pin 22, QR 10 -> pin 23

const unsigned long COMMAND_TIMEOUT_MS = 750;    // no Pi command for 750 ms -> stop
const int MAX_PI_SPEED = 180;                    // DRIVE:l,r from the Pi is capped here
const unsigned long VIEW_EVERY_MS = 200;         // live view: 5 lines a second
const bool VIEW_ON_AT_BOOT = true;               // the Pi sends "VIEW 0" when it connects
const unsigned long LFR_TEST_MS = 60000;         // "l" without the Pi: 60 s per press
const unsigned long DBG_MOTOR_EVERY_MS = 500;    // "DBG motor=" lines for the Pi log
const unsigned long LINE_LOST_BEEP_PERIOD_MS = 500;   // line lost: 100 ms beep every 500 ms
const unsigned long LINE_LOST_BEEP_ON_MS = 100;

enum ControlMode { AUTO_MODE, MANUAL_MODE };
ControlMode controlMode = AUTO_MODE;             // what the Pi asked for
bool obstacleDetected = true;                    // starts stopped until the Pi says SAFE
int piLeftTarget = 0;
int piRightTarget = 0;
unsigned long lastSafetyHeartbeatMs = 0;
unsigned long lfrTestUntilMs = 0;
bool wasFollowing = false;
bool qrOutputs[10] = {false};
String serialBuffer = "";
unsigned long lastCharMs = 0;
bool viewOn = VIEW_ON_AT_BOOT;
unsigned long lastViewMs = 0;
bool prevObstacleDebug = true;
bool prevLineLostDebug = false;
unsigned long lastDbgMotorMs = 0;
int prevLeftDbg = 1000, prevRightDbg = 1000;

bool timeBefore(unsigned long until)             // true while millis() < until (rollover-safe)
{
    return until != 0 && (long)(millis() - until) < 0;
}

// ════════════════════════════════════════════════════════════════════════════
//  LFR CODE: motors, sensors, filter (copied as they were)
// ════════════════════════════════════════════════════════════════════════════

void setLeftMotor(int speed)
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

void setRightMotor(int speed)
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

void driveSmoothly(int leftTarget, int rightTarget);

void stopMotors()
{
    driveSmoothly(0, 0);
}

int approachSpeed(int current, int target, unsigned long maxChange)
{
    int step = (int)maxChange;
    if (target > current)
    {
        return target - current > step ? current + step : target;
    }
    if (target < current)
    {
        return current - target > step ? current - step : target;
    }
    return current;
}

void driveSmoothly(int leftTarget, int rightTarget)
{
    unsigned long now = millis();
    unsigned long elapsed = now - lastMotorUpdateMs;
    lastMotorUpdateMs = now;

    motorSlewRemainder += (unsigned long)MOTOR_SLEW_RATE_PER_SECOND * elapsed;
    unsigned long maxChange = motorSlewRemainder / 1000;
    motorSlewRemainder %= 1000;

    if (maxChange > 0)
    {
        currentLeftSpeed = approachSpeed(currentLeftSpeed, leftTarget, maxChange);
        currentRightSpeed = approachSpeed(currentRightSpeed, rightTarget, maxChange);
    }

    setLeftMotor(currentLeftSpeed);
    setRightMotor(currentRightSpeed);
}

bool sensorSeesLine(uint8_t pin)
{
    int reading = digitalRead(pin);
    return BLACK_LINE ? reading == LOW : reading == HIGH;
}

bool readLinePosition(float &position)
{
    const float sensorPositions[5] = {-2.0f, -1.0f, 0.0f, 1.0f, 2.0f};
    float positionSum = 0.0f;
    int activeSensors = 0;

    for (uint8_t i = 0; i < 5; i++)
    {
        if (sensorSeesLine(IR_PINS[i]))
        {
            positionSum += sensorPositions[i];
            activeSensors++;
        }
    }

    if (activeSensors == 0)
    {
        return false;
    }

    position = positionSum / activeSensors;
    return true;
}

float smoothPosition(float position)
{
    unsigned long now = millis();
    unsigned long elapsed = now - lastPositionUpdateMs;
    lastPositionUpdateMs = now;

    if (!positionFilterInitialized)
    {
        filteredPosition = position;
        positionFilterInitialized = true;
    }
    else
    {
        float alpha = (float)elapsed / (POSITION_FILTER_MS + elapsed);
        filteredPosition += alpha * (position - filteredPosition);
    }
    return filteredPosition;
}

// The LFR code's reset when line following starts again (its 'p' manual -> auto branch)
void resetLineFollowing()
{
    lineLost = false;
    lastLineSide = 0;
    positionFilterInitialized = false;
    lastPositionUpdateMs = millis();
}

// [3.4] Nov 11 reverse turning: when only one side sensor sees the line, the inner wheel
// turns backward and the outer wheel drives forward at full line speed.
void reverseTurnAssist(int &leftTarget, int &rightTarget)
{
    if (!REVERSE_TURN_ASSIST)
    {
        return;
    }
    uint8_t pattern = 0;
    for (uint8_t i = 0; i < 5; i++)
    {
        pattern = (pattern << 1) | (sensorSeesLine(IR_PINS[i]) ? 1 : 0);   // bit 4 = S1 ... bit 0 = S5
    }
    switch (pattern)
    {
        case 0b01000:                                      // slide left
            leftTarget = -SLIDE_BRAKE_SPEED;
            rightTarget = MAX_MOTOR_SPEED;
            break;
        case 0b10000:                                      // sharp left (pivot)
            leftTarget = -MAX_MOTOR_SPEED;
            rightTarget = MAX_MOTOR_SPEED;
            break;
        case 0b00010:                                      // slide right
            leftTarget = MAX_MOTOR_SPEED;
            rightTarget = -SLIDE_BRAKE_SPEED;
            break;
        case 0b00001:                                      // sharp right (pivot)
            leftTarget = MAX_MOTOR_SPEED;
            rightTarget = -MAX_MOTOR_SPEED;
            break;
    }
}

// LFR code: one pass of its loop() line following (copied as it was, plus the [3.4] line)
void lineFollowStep()
{
    float position;
    unsigned long now = millis();

    if (!readLinePosition(position))
    {
        if (!lineLost)
        {
            lineLost = true;
            lostLineStartMs = now;
        }

        if (lastLineSide != 0 && now - lostLineStartMs < LOST_LINE_SEARCH_MS)
        {
            if (lastLineSide < 0)
            {
                driveSmoothly(-SEARCH_SPEED, SEARCH_SPEED);
            }
            else
            {
                driveSmoothly(SEARCH_SPEED, -SEARCH_SPEED);
            }
        }
        else
        {
            stopMotors();
        }
        return;
    }

    lineLost = false;
    position = smoothPosition(position);

    if (position < -0.1f)
    {
        lastLineSide = -1;
    }
    else if (position > 0.1f)
    {
        lastLineSide = 1;
    }

    int steering = (int)(position * LINE_STEERING_GAIN);
    steering = constrain(steering, -LINE_MAX_STEERING, LINE_MAX_STEERING);

    int baseSpeed = LINE_BASE_SPEED - (int)(abs(position) * 12.0f);
    int leftTarget = constrain(baseSpeed + steering, 0, MAX_MOTOR_SPEED);
    int rightTarget = constrain(baseSpeed - steering, 0, MAX_MOTOR_SPEED);
    reverseTurnAssist(leftTarget, rightTarget);        // [3.4] Nov 11 reverse turning
    driveSmoothly(leftTarget, rightTarget);
}

// ════════════════════════════════════════════════════════════════════════════
//  SERIAL MONITOR KEYS (the LFR code's p / w a s d x keys, plus l v i h)
// ════════════════════════════════════════════════════════════════════════════

void printSensors()
{
    Serial.print("S1-S5 ");
    for (uint8_t i = 0; i < 5; i++) Serial.print(sensorSeesLine(IR_PINS[i]) ? '1' : '0');
}

void printHelp()
{
    Serial.println("== MediCart 3.4 Serial Monitor ==");
    Serial.println(" p manual on/off   w a s d drive   x / space stop   (the LFR code's keys)");
    Serial.println(" l line following 60 s without the Pi   v live view on/off   i sensors   h help   STAT");
    Serial.println(" (no camera brake in p-manual or the l run - keep someone next to the robot)");
}

void handleKey(char key)
{
    if (key >= 'A' && key <= 'Z')
    {
        key = key - 'A' + 'a';
    }

    if (key == 'p')                                   // LFR code: manual on/off
    {
        lfrTestUntilMs = 0;
        manualMode = !manualMode;
        manualLeftTarget = 0;
        manualRightTarget = 0;
        stopMotors();

        if (!manualMode)
        {
            lineLost = false;
            lastLineSide = 0;
            positionFilterInitialized = false;
            lastPositionUpdateMs = millis();
        }
        Serial.println(manualMode ? "MANUAL on (w a s d, x stop, p off)" : "MANUAL off");
        return;
    }
    if (key == 'l')
    {
        manualMode = false;
        lfrTestUntilMs = millis() + LFR_TEST_MS;
        if (lfrTestUntilMs == 0) lfrTestUntilMs = 1;
        Serial.println("LFR 60 s: line following without the Pi (x = stop, l again = 60 s more)");
        return;
    }
    if (key == 'v') { viewOn = !viewOn; Serial.println(viewOn ? "VIEW on (v = off)" : "VIEW off (v = on)"); return; }
    if (key == 'i') { printSensors(); Serial.println("   (1 = line)"); return; }
    if (key == 'h' || key == '?') { printHelp(); return; }
    if ((key == 'x' || key == ' ') && !manualMode && lfrTestUntilMs)
    {
        lfrTestUntilMs = 0;
        stopMotors();
        Serial.println("LFR stopped");
        return;
    }

    if (!manualMode)
    {
        return;
    }

    switch (key)                                      // LFR code: manual keys
    {
        case 'w':
            manualLeftTarget = MANUAL_SPEED;
            manualRightTarget = MANUAL_SPEED;
            break;
        case 'a':
            manualLeftTarget = 0;
            manualRightTarget = MANUAL_SPEED;
            break;
        case 'd':
            manualLeftTarget = MANUAL_SPEED;
            manualRightTarget = 0;
            break;
        case 's':
            manualLeftTarget = -MANUAL_SPEED;
            manualRightTarget = -MANUAL_SPEED;
            break;
        case 'x':
        case ' ':
            manualLeftTarget = 0;
            manualRightTarget = 0;
            stopMotors();
            break;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  STATUS OUTPUT
// ════════════════════════════════════════════════════════════════════════════

const char *modeName()
{
    if (manualMode) return "KEYS-MANUAL";
    if (timeBefore(lfrTestUntilMs)) return "LFR-TEST";
    if (controlMode == MANUAL_MODE) return "MANUAL";
    return "AUTO";
}

void printPattern()
{
    for (uint8_t i = 0; i < 5; i++) Serial.print(sensorSeesLine(IR_PINS[i]) ? '1' : '0');
}

void printView()
{
    unsigned long hb = millis() - lastSafetyHeartbeatMs;
    Serial.print("VIEW ");
    printSensors();
    Serial.print(" | ");
    Serial.print(modeName());
    Serial.print(manualMode || timeBefore(lfrTestUntilMs) ? "" : (obstacleDetected ? " STOPPED" : " GO"));
    Serial.print(" | L ");
    Serial.print(currentLeftSpeed);
    Serial.print(" R ");
    Serial.print(currentRightSpeed);
    Serial.print(" | pos ");
    Serial.print(filteredPosition, 2);
    Serial.print(" | hb ");
    Serial.print(hb > 99999UL ? 99999UL : hb);
    Serial.print("ms");
    if (lineLost) Serial.print(" | LINE LOST");
    Serial.println();
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
    Serial.println(" fw=3.4");
}

void debugReport()
{
    unsigned long now = millis();
    if (obstacleDetected != prevObstacleDebug)
    {
        Serial.print("DBG obstacle=");
        Serial.print((int)obstacleDetected);
        Serial.print(" hbAge=");
        Serial.println(now - lastSafetyHeartbeatMs);
        prevObstacleDebug = obstacleDetected;
    }
    else if (lineLost != prevLineLostDebug)
    {
        Serial.print("DBG lineLost=");
        Serial.println((int)lineLost);
        prevLineLostDebug = lineLost;
    }
    else if ((currentLeftSpeed != prevLeftDbg || currentRightSpeed != prevRightDbg) && now - lastDbgMotorMs >= DBG_MOTOR_EVERY_MS)
    {
        Serial.print("DBG motor=");
        Serial.print(currentLeftSpeed);
        Serial.print(',');
        Serial.println(currentRightSpeed);
        prevLeftDbg = currentLeftSpeed;
        prevRightDbg = currentRightSpeed;
        lastDbgMotorMs = now;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  BUZZER, QR
// ════════════════════════════════════════════════════════════════════════════

void buzzerControl(bool benchDriving)
{
    if (benchDriving)
    {
        digitalWrite(BUZZER_PIN, LOW);
        return;
    }
    if (lineLost && !obstacleDetected)                // line lost: short beeps
    {
        bool beepOn = (millis() % LINE_LOST_BEEP_PERIOD_MS) < LINE_LOST_BEEP_ON_MS;
        digitalWrite(BUZZER_PIN, beepOn ? HIGH : LOW);
        return;
    }
    digitalWrite(BUZZER_PIN, obstacleDetected ? HIGH : LOW);   // safety stop / no Pi: solid tone
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
//  PI COMMANDS
// ════════════════════════════════════════════════════════════════════════════

// Returns true when the line was a Pi command (or STAT / VIEW).
bool runPiCommand(const String &command)
{
    bool pi = command == "SAFE" || command == "STOP" || command.startsWith("MODE:") || command.startsWith("DRIVE:");
    if (pi && (manualMode || lfrTestUntilMs))         // the Pi is in charge again: end the Serial Monitor runs
    {
        manualMode = false;
        manualLeftTarget = 0;
        manualRightTarget = 0;
        lfrTestUntilMs = 0;
        stopMotors();
    }

    if (command == "STOP")
    {
        lastSafetyHeartbeatMs = millis();
        obstacleDetected = true;
        piLeftTarget = 0;
        piRightTarget = 0;
    }
    else if (command == "SAFE" || command == "MODE:AUTO")
    {
        lastSafetyHeartbeatMs = millis();
        controlMode = AUTO_MODE;
        obstacleDetected = false;
    }
    else if (command == "MODE:MANUAL")
    {
        lastSafetyHeartbeatMs = millis();
        if (controlMode != MANUAL_MODE)
        {
            piLeftTarget = 0;
            piRightTarget = 0;
        }
        controlMode = MANUAL_MODE;
        obstacleDetected = false;
    }
    else if (command.startsWith("DRIVE:"))
    {
        int comma = command.indexOf(',', 6);
        if (controlMode == MANUAL_MODE && comma > 6)
        {
            piLeftTarget = constrain(command.substring(6, comma).toInt(), -MAX_PI_SPEED, MAX_PI_SPEED);
            piRightTarget = constrain(command.substring(comma + 1).toInt(), -MAX_PI_SPEED, MAX_PI_SPEED);
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
    else if (command == "STAT")
    {
        printStat();
    }
    else if (command == "VIEW 0" || command == "VIEW 1")
    {
        viewOn = (command == "VIEW 1");
    }
    else
    {
        return false;
    }
    return true;
}

void runLine(String command)
{
    command.trim();
    if (command.length() == 0)
    {
        return;
    }
    String u = command;
    u.toUpperCase();
    if (runPiCommand(u))
    {
        return;
    }
    for (unsigned int i = 0; i < command.length(); i++)   // anything else: keys, one by one (like the LFR code)
    {
        handleKey(command.charAt(i));
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
            runLine(command);
        }
        else if (incoming == ' ' && serialBuffer.length() == 0)
        {
            handleKey(' ');                            // space = stop, right away
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
        runLine(command);
    }

    if ((millis() - lastSafetyHeartbeatMs) >= COMMAND_TIMEOUT_MS)
    {
        obstacleDetected = true;                       // no Pi: stop (the Serial Monitor runs don't need it)
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ════════════════════════════════════════════════════════════════════════════

void setup()
{
    Serial.begin(SERIAL_BAUD);
    serialBuffer.reserve(32);

    for (uint8_t i = 0; i < 5; i++)
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
    lastPositionUpdateMs = millis();
    lastSafetyHeartbeatMs = millis();
    digitalWrite(BUZZER_PIN, LOW);

    Serial.println("MediCart ready");
    Serial.println("FW medicart 3.4");             // the Pi checks this
    Serial.println("Type l to line-follow without the Pi, p for manual keys, h for help, v to stop/start the live view.");
}

void loop()
{
    processSerialCommands();

    if (lfrTestUntilMs != 0 && !timeBefore(lfrTestUntilMs))
    {
        lfrTestUntilMs = 0;
        stopMotors();
        Serial.println("LFR done (l = 60 s more)");
    }

    bool following = false;
    bool bench = manualMode || lfrTestUntilMs != 0;

    if (manualMode)                                    // LFR code: Serial Monitor manual mode
    {
        driveSmoothly(manualLeftTarget, manualRightTarget);
    }
    else if (lfrTestUntilMs != 0)                      // "l": line following without the Pi
    {
        following = true;
    }
    else if (obstacleDetected)                         // person in front, emergency stop, or no Pi
    {
        stopMotors();                                  // LFR code's smooth stop: called every loop, ramps to 0
    }
    else if (controlMode == MANUAL_MODE)               // the Pi drives
    {
        driveSmoothly(piLeftTarget, piRightTarget);
    }
    else                                               // the Pi says the path is clear
    {
        following = true;
    }

    if (following)
    {
        if (!wasFollowing)
        {
            resetLineFollowing();
        }
        lineFollowStep();
    }
    else
    {
        lineLost = false;
    }
    wasFollowing = following;

    buzzerControl(bench);
    debugReport();
    updateQROutputs();

    if (viewOn && millis() - lastViewMs >= VIEW_EVERY_MS)
    {
        lastViewMs = millis();
        printView();
    }
}
