// ════════════════════════════════════════════════════════════════════════════
//  MediCart TEST sketch — IR array viewer + manual driving + line following
//  IUB Robotics Team · Arduino Mega 2560 · Serial Monitor 115200
//
//  Stand-alone bench tool for a new IR sensor array. No Raspberry Pi, no camera:
//  there is NO camera brake in this sketch. Keep someone next to the robot.
//
//  The sensors are printed all the time (5 times a second) while you drive by
//  hand or while line following runs.
//
//  Type a command and press Enter (any line ending setting works):
//    w forward   s back   a turn left   d turn right   x STOP (everything)
//        each drive key runs for MANUAL_HOLD_MS (3 s); type it again to keep going
//    l           line following ON / OFF
//    + / -       faster / slower
//    c           calibrate: for 5 s slide the robot slowly across the line so every
//                sensor sees white AND black. Saved in EEPROM.
//    i           invert (if the line shows as white and the floor as black)
//    p           pause / resume the sensor printout
//    h or ?      help
//    SPEED 150   set the speed (0-255)
//    KP 35       steering strength          KD 20   steering damping
//    RESET       forget calibration and settings
//
//  Printout:
//    IR 00100 | A  880  870  120  860  875 | pos +0.00 | LFR L=150 R=150
//       └ 1 = line          └ raw analogRead          └ -2 far left … +2 far right
// ════════════════════════════════════════════════════════════════════════════
#include <Arduino.h>
#include <EEPROM.h>

// ---------- Pins (same robot as medicart.ino) ----------
const int MOTOR_L_PIN1 = 2;   // left  BTS forward PWM
const int MOTOR_L_PIN2 = 3;   // left  BTS reverse PWM
const int MOTOR_R_PIN1 = 4;   // right BTS forward PWM
const int MOTOR_R_PIN2 = 5;   // right BTS reverse PWM
const int BUZZER_PIN = 24;

// IR array, left to right. Change the count and pins here for an array with more channels.
const int NUM_SENSORS = 5;
const int IR_PINS[NUM_SENSORS] = {A8, A9, A10, A11, A12};

// ---------- Tuning (can also be changed from the Serial Monitor) ----------
int speedBase = 140;                         // PWM for straight driving / line following
float Kp = 35.0;                             // PWM per unit of position error
float Kd = 20.0;                             // PWM per unit of error change (per loop step)
const int MAX_PWM = 230;
const unsigned long MANUAL_HOLD_MS = 3000;   // a drive key runs this long, then stops
const unsigned long LOST_SEARCH_MS = 1200;   // line lost: keep turning toward it this long, then stop
const unsigned long PRINT_EVERY_MS = 200;
const unsigned long LFR_STEP_MS = 10;        // steering update rate (100 Hz)
const unsigned long CAL_MS = 5000;

// ---------- Calibration (saved in EEPROM) ----------
struct Cal {
    byte magic;
    int minV[NUM_SENSORS];
    int maxV[NUM_SENSORS];
    bool lineIsHigh;      // true: the line reads HIGHER than the floor (typical for IR: black reflects less)
    bool valid;
    int speed;
    float kp, kd;
};
const byte CAL_MAGIC = 0x5C;
Cal cal;

// ---------- State ----------
enum Mode { MODE_STOP, MODE_MANUAL, MODE_LFR, MODE_CAL };
Mode mode = MODE_STOP;
int manL = 0, manR = 0;
unsigned long manualUntil = 0;
bool printing = true;
unsigned long lastPrint = 0, lastStep = 0, lostSince = 0, calStart = 0;
float lastError = 0, lastSeenError = 0;
bool lineSeen[NUM_SENSORS];
int rawV[NUM_SENSORS];
float position = 0;
bool anyLine = false;
int outL = 0, outR = 0;
String buf;
unsigned long lastCharMs = 0;
long calSum[NUM_SENSORS];
long calCount = 0;

// ════════════════ Motors ════════════════
void motor(int pin1, int pin2, int speed)
{
    speed = constrain(speed, -255, 255);
    if (speed > 0)      { analogWrite(pin1, speed); analogWrite(pin2, 0); }
    else if (speed < 0) { analogWrite(pin1, 0);     analogWrite(pin2, -speed); }
    else                { analogWrite(pin1, 0);     analogWrite(pin2, 0); }
}

void drive(int l, int r)
{
    outL = constrain(l, -MAX_PWM, MAX_PWM);
    outR = constrain(r, -MAX_PWM, MAX_PWM);
    motor(MOTOR_L_PIN1, MOTOR_L_PIN2, outL);
    motor(MOTOR_R_PIN1, MOTOR_R_PIN2, outR);
}

void stopAll(const char *why)
{
    drive(0, 0);
    mode = MODE_STOP;
    manualUntil = 0;
    if (why) { Serial.print(F(">> STOP ")); Serial.println(why); }
}

// ════════════════ Sensors ════════════════
void readSensors()
{
    float weighted = 0;
    int count = 0;
    for (int n = 0; n < NUM_SENSORS; n++)
    {
        rawV[n] = analogRead(IR_PINS[n]);
        bool line;
        if (cal.valid)
        {
            int mid = (cal.minV[n] + cal.maxV[n]) / 2;
            line = cal.lineIsHigh ? (rawV[n] > mid) : (rawV[n] < mid);
        }
        else
        {
            line = cal.lineIsHigh ? (rawV[n] > 512) : (rawV[n] < 512);   // works for digital-output arrays too
        }
        lineSeen[n] = line;
        if (line)
        {
            weighted += n - (NUM_SENSORS - 1) / 2.0;   // -2 … +2 for five sensors
            count++;
        }
    }
    anyLine = count > 0;
    if (anyLine)
    {
        position = weighted / count;
    }
}

// ════════════════ Line following (PD) ════════════════
void lineFollowStep()
{
    unsigned long now = millis();
    if (anyLine)
    {
        lostSince = 0;
        float error = position;                 // + = line is to the right
        float corr = Kp * error + Kd * (error - lastError);
        lastError = error;
        lastSeenError = error;
        // slow down in sharp turns so the line isn't lost
        int base = speedBase - (int)(abs(error) * speedBase * 0.25);
        drive(base + (int)corr, base - (int)corr);
    }
    else
    {
        if (lostSince == 0) lostSince = now;
        if (now - lostSince < LOST_SEARCH_MS && lastSeenError != 0)
        {
            int turn = speedBase * 3 / 4;       // spin toward the side the line was last seen
            if (lastSeenError > 0) drive(turn, -turn); else drive(-turn, turn);
        }
        else
        {
            drive(0, 0);                        // give up: stays in LFR, restarts when the line is seen again
        }
    }
}

// ════════════════ Calibration ════════════════
void saveCal()
{
    cal.magic = CAL_MAGIC;
    cal.speed = speedBase;
    cal.kp = Kp;
    cal.kd = Kd;
    EEPROM.put(0, cal);
}

void loadCal()
{
    EEPROM.get(0, cal);
    if (cal.magic != CAL_MAGIC)
    {
        cal.valid = false;
        cal.lineIsHigh = true;
        for (int n = 0; n < NUM_SENSORS; n++) { cal.minV[n] = 0; cal.maxV[n] = 1023; }
        return;
    }
    speedBase = constrain(cal.speed, 0, 255);
    if (cal.kp >= 0 && cal.kp < 500) Kp = cal.kp;
    if (cal.kd >= 0 && cal.kd < 500) Kd = cal.kd;
}

void startCal()
{
    drive(0, 0);
    mode = MODE_CAL;
    calStart = millis();
    calCount = 0;
    for (int n = 0; n < NUM_SENSORS; n++) { cal.minV[n] = 1023; cal.maxV[n] = 0; calSum[n] = 0; }
    Serial.println(F(">> CALIBRATING 5 s: slide the robot slowly across the line, back and forth"));
}

void calStep()
{
    for (int n = 0; n < NUM_SENSORS; n++)
    {
        int v = analogRead(IR_PINS[n]);
        cal.minV[n] = min(cal.minV[n], v);
        cal.maxV[n] = max(cal.maxV[n], v);
        calSum[n] += v;
    }
    calCount++;
    if (millis() - calStart < CAL_MS) return;

    // The floor is under the sensors most of the time, so the average sits on the floor's side:
    // if the average is below the middle, the line reads higher than the floor.
    int votesHigh = 0, weak = 0;
    for (int n = 0; n < NUM_SENSORS; n++)
    {
        int mid = (cal.minV[n] + cal.maxV[n]) / 2;
        long avg = calSum[n] / max(1L, calCount);
        if (avg < mid) votesHigh++;
        if (cal.maxV[n] - cal.minV[n] < 100) weak++;
    }
    cal.lineIsHigh = votesHigh * 2 >= NUM_SENSORS;
    cal.valid = true;
    saveCal();
    mode = MODE_STOP;
    Serial.println(F(">> CALIBRATION SAVED"));
    for (int n = 0; n < NUM_SENSORS; n++)
    {
        Serial.print(F("   S")); Serial.print(n + 1);
        Serial.print(F(" min ")); Serial.print(cal.minV[n]);
        Serial.print(F(" max ")); Serial.print(cal.maxV[n]);
        if (cal.maxV[n] - cal.minV[n] < 100) Serial.print(F("   <- barely changed: did it pass over the line? check height/screw"));
        Serial.println();
    }
    Serial.print(F("   line reads ")); Serial.println(cal.lineIsHigh ? F("HIGHER than the floor") : F("LOWER than the floor"));
    if (weak) Serial.println(F("   Some sensors barely changed. Redo c and make sure every sensor crosses the line."));
}

// ════════════════ Serial commands ════════════════
void help()
{
    Serial.println(F("\n== MediCart TEST: IR viewer + manual + line following (no camera brake!) =="));
    Serial.println(F(" w forward  s back  a left  d right  x STOP   (each drive key runs 3 s)"));
    Serial.println(F(" l line following on/off   + / - speed   c calibrate (5 s, slide across line)"));
    Serial.println(F(" i invert line colour   p pause/resume printout   h help"));
    Serial.println(F(" SPEED n   KP n   KD n   RESET"));
    Serial.print(F(" speed=")); Serial.print(speedBase);
    Serial.print(F(" Kp=")); Serial.print(Kp);
    Serial.print(F(" Kd=")); Serial.print(Kd);
    Serial.print(F(" calibrated=")); Serial.print(cal.valid ? F("yes") : F("no"));
    Serial.print(F(" line=")); Serial.println(cal.lineIsHigh ? F("high") : F("low"));
}

void manual(int l, int r, const char *name)
{
    mode = MODE_MANUAL;
    manL = l; manR = r;
    manualUntil = millis() + MANUAL_HOLD_MS;
    drive(l, r);
    Serial.print(F(">> ")); Serial.print(name);
    Serial.print(F(" L=")); Serial.print(l); Serial.print(F(" R=")); Serial.println(r);
}

void key(char k)
{
    int turn = speedBase * 3 / 4;
    switch (k)
    {
        case 'w': manual(speedBase, speedBase, "FORWARD"); break;
        case 's': manual(-speedBase, -speedBase, "BACK"); break;
        case 'a': manual(-turn, turn, "LEFT"); break;
        case 'd': manual(turn, -turn, "RIGHT"); break;
        case 'x': stopAll("(x)"); break;
        case 'l':
            if (mode == MODE_LFR) { stopAll("line following OFF"); }
            else
            {
                mode = MODE_LFR; lostSince = 0; lastError = 0; lastSeenError = 0;
                Serial.println(F(">> LINE FOLLOWING ON (x or l to stop)"));
            }
            break;
        case '+': speedBase = min(255, speedBase + 10); saveCal(); Serial.print(F(">> speed ")); Serial.println(speedBase); break;
        case '-': speedBase = max(0, speedBase - 10);   saveCal(); Serial.print(F(">> speed ")); Serial.println(speedBase); break;
        case 'c': startCal(); break;
        case 'i': cal.lineIsHigh = !cal.lineIsHigh; saveCal(); Serial.print(F(">> line now reads ")); Serial.println(cal.lineIsHigh ? F("HIGH") : F("LOW")); break;
        case 'p': printing = !printing; Serial.println(printing ? F(">> printout ON") : F(">> printout paused (p to resume)")); break;
        case 'h': case '?': help(); break;
        default: break;
    }
}

void command(String c)
{
    c.trim();
    if (c.length() == 0) return;
    String u = c;
    u.toUpperCase();
    if (u.startsWith("SPEED ")) { speedBase = constrain(u.substring(6).toInt(), 0, 255); saveCal(); Serial.print(F(">> speed ")); Serial.println(speedBase); return; }
    if (u.startsWith("KP "))    { Kp = u.substring(3).toFloat(); saveCal(); Serial.print(F(">> Kp ")); Serial.println(Kp); return; }
    if (u.startsWith("KD "))    { Kd = u.substring(3).toFloat(); saveCal(); Serial.print(F(">> Kd ")); Serial.println(Kd); return; }
    if (u == "RESET")
    {
        stopAll(nullptr);
        EEPROM.put(0, (byte)0xFF);
        loadCal(); speedBase = 140; Kp = 35; Kd = 20;
        Serial.println(F(">> settings and calibration cleared"));
        return;
    }
    // single keys; "wwww" = the same key several times (keeps it going)
    for (unsigned int n = 0; n < c.length(); n++)
    {
        char k = c.charAt(n);
        if (k >= 'A' && k <= 'Z') k = k - 'A' + 'a';
        key(k);
    }
}

void readSerial()
{
    while (Serial.available())
    {
        char ch = Serial.read();
        lastCharMs = millis();
        if (ch == '\n' || ch == '\r')
        {
            command(buf);
            buf = "";
        }
        else if (buf.length() < 32)
        {
            buf += ch;
        }
    }
    // "No line ending" in the Serial Monitor: run what arrived after a short pause
    if (buf.length() && millis() - lastCharMs > 60)
    {
        command(buf);
        buf = "";
    }
}

// ════════════════ Printout ════════════════
void printStatus()
{
    Serial.print(F("IR "));
    for (int n = 0; n < NUM_SENSORS; n++) Serial.print(lineSeen[n] ? '1' : '0');
    Serial.print(F(" | A"));
    for (int n = 0; n < NUM_SENSORS; n++)
    {
        Serial.print(' ');
        if (rawV[n] < 1000) Serial.print(' ');
        if (rawV[n] < 100) Serial.print(' ');
        if (rawV[n] < 10) Serial.print(' ');
        Serial.print(rawV[n]);
    }
    Serial.print(F(" | pos "));
    if (anyLine) { if (position >= 0) Serial.print('+'); Serial.print(position, 2); }
    else Serial.print(F(" ---"));
    Serial.print(F(" | "));
    Serial.print(mode == MODE_LFR ? F("LFR") : mode == MODE_MANUAL ? F("MAN") : mode == MODE_CAL ? F("CAL") : F("STOP"));
    Serial.print(F(" L=")); Serial.print(outL);
    Serial.print(F(" R=")); Serial.println(outR);
}

// ════════════════ Setup / loop ════════════════
void setup()
{
    pinMode(MOTOR_L_PIN1, OUTPUT); pinMode(MOTOR_L_PIN2, OUTPUT);
    pinMode(MOTOR_R_PIN1, OUTPUT); pinMode(MOTOR_R_PIN2, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    for (int n = 0; n < NUM_SENSORS; n++) pinMode(IR_PINS[n], INPUT);
    drive(0, 0);
    Serial.begin(115200);
    buf.reserve(34);
    loadCal();
    Serial.println(F("MediCart TEST ready"));
    help();
}

void loop()
{
    unsigned long now = millis();
    readSerial();

    if (mode == MODE_CAL)
    {
        calStep();
    }
    else
    {
        readSensors();
        if (mode == MODE_MANUAL && (long)(now - manualUntil) >= 0)
        {
            stopAll("(manual time up)");
        }
        if (mode == MODE_LFR && now - lastStep >= LFR_STEP_MS)
        {
            lastStep = now;
            lineFollowStep();
        }
    }

    if (printing && mode != MODE_CAL && now - lastPrint >= PRINT_EVERY_MS)
    {
        lastPrint = now;
        printStatus();
    }
}
