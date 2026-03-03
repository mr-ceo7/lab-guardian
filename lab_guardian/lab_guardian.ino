/**
 * Smart Lab Guardian - Robust Edition + Bluetooth
 *
 * 24/7 Gas & Fire Safety System for School Chemistry Labs.
 * Monitors MQ-2 (LPG/Smoke) and MQ-135 (Air Quality).
 * SMS alerts via SIM800L, Bluetooth app via HC-05.
 *
 * Hardware:
 * - MQ-135: D12       - MQ-2: D11
 * - Buzzer: D13       - LED:  D4
 * - SIM800L RST: D7   - SIM TX->D8   - SIM RX<-D9
 * - HC-05:  TX->D5    - RX<-D6
 * - LCD: I2C (SDA/SCL)
 */

#include <EEPROM.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>
#include <Wire.h>

// ===================== PIN DEFINITIONS =====================
#define PIN_MQ135 12
#define PIN_MQ2 11
#define PIN_BUZZER 13
#define PIN_LED 4
#define PIN_SIM_RST 7
#define PIN_GSM_RX 8 // Arduino RX <- SIM800L TX
#define PIN_GSM_TX 9 // Arduino TX -> SIM800L RX
#define PIN_BT_RX 5  // Arduino RX <- HC-05 TX
#define PIN_BT_TX 6  // Arduino TX -> HC-05 RX

// ===================== CONFIGURATION =======================
// Emergency Contacts stored in EEPROM
// EEPROM layout: [count(1 byte)] [phone1(16 bytes)] [phone2(16 bytes)]
// [phone3(16 bytes)]
#define EEPROM_PHONE_ADDR 0
#define MAX_PHONES 3
#define PHONE_LEN 16 // max digits + null
char phoneNumbers[MAX_PHONES][PHONE_LEN];
byte numContacts = 0;

// Hardcoded fallback number (used when no EEPROM contacts set)
const char FALLBACK_PHONE[] PROGMEM = "+254746957502";

// Timing
const unsigned long SMS_COOLDOWN = 60000UL;     // 1 min between SMS bursts
const unsigned long HEALTH_INTERVAL = 300000UL; // 5 min health check
const unsigned int AT_TIMEOUT = 2000;           // 2s default AT timeout
const unsigned int SMS_TIMEOUT = 10000;         // 10s for SMS send
const byte MAX_SMS_RETRIES = 3;

// ===================== GLOBALS =============================
LiquidCrystal_I2C lcd(0x3F, 16, 2);
SoftwareSerial gsm(PIN_GSM_RX, PIN_GSM_TX);
SoftwareSerial bt(PIN_BT_RX, PIN_BT_TX);

// Response buffer (keep small for RAM)
char gsmBuf[64];

// Bluetooth command buffer (needs to fit CMD:PHONE:+254xxxxxxxxx)
char btCmdBuf[32];
byte btCmdIdx = 0;

bool alarmActive = false;
bool buzzerSilenced = false; // Silenced from app (LED stays on)
bool gsmReady = false;
unsigned long lastSmsTime = 0;
unsigned long lastHealthTime = 0;
unsigned long lastDebugTime = 0;
const unsigned long DEBUG_INTERVAL =
    2000UL; // Print sensor + BT status every 2s

// Buzzer pattern: beep-beep-pause (500 on, 300 off, 500 on, 800 off)
unsigned long buzzerPatternStart = 0;
const unsigned int BUZZER_PATTERN[] = {500, 300, 500, 800}; // on,off,on,off
const byte BUZZER_STEPS = 4;
byte buzzerStep = 0;

// Last known sensor signal strength (cached from initGSM)
int lastSignal = 0;

// ===================== BLUETOOTH FUNCTIONS =================

/**
 * Switch to Bluetooth serial as active listener.
 */
void btListen() { bt.listen(); }

/**
 * Switch to GSM serial as active listener.
 */
void gsmListen() { gsm.listen(); }

/**
 * Send JSON status to the app over Bluetooth.
 */
void btSendStatus(bool mq2, bool mq135) {
  bt.print(F("{\"mq2\":"));
  bt.print(mq2 ? 1 : 0);
  bt.print(F(",\"mq135\":"));
  bt.print(mq135 ? 1 : 0);
  bt.print(F(",\"alm\":"));
  bt.print(alarmActive ? 1 : 0);
  bt.print(F(",\"sil\":"));
  bt.print(buzzerSilenced ? 1 : 0);
  bt.print(F(",\"gsm\":"));
  bt.print(gsmReady ? 1 : 0);
  bt.print(F(",\"sig\":"));
  bt.print(lastSignal);
  bt.print(F(",\"up\":"));
  bt.print(millis() / 1000);
  bt.println(F("}"));
}

/**
 * Check for incoming Bluetooth commands.
 * Commands: CMD:SILENCE, CMD:TEST_SMS, CMD:RESET, CMD:STATUS
 */
void btCheckCommands() {
  while (bt.available()) {
    char c = bt.read();
    if (c == '\n' || c == '\r') {
      if (btCmdIdx > 0) {
        btCmdBuf[btCmdIdx] = '\0';
        Serial.print(F("[BT] Cmd: "));
        Serial.println(btCmdBuf);

        if (strcmp(btCmdBuf, "CMD:SILENCE") == 0) {
          buzzerSilenced = true;
          digitalWrite(PIN_BUZZER, LOW);
          Serial.println(F("[BT] Buzzer silenced."));
          bt.println(F("{\"ack\":\"SILENCE_OK\"}"));
        } else if (strcmp(btCmdBuf, "CMD:RESET") == 0) {
          buzzerSilenced = false;
          alarmActive = false;
          digitalWrite(PIN_BUZZER, LOW);
          digitalWrite(PIN_LED, LOW);
          lcd.clear();
          lcd.print(F("System Ready"));
          lcd.setCursor(0, 1);
          lcd.print(F("Monitoring..."));
          Serial.println(F("[BT] System reset."));
          bt.println(F("{\"ack\":\"RESET_OK\"}"));
        } else if (strcmp(btCmdBuf, "CMD:TEST_SMS") == 0) {
          Serial.println(F("[BT] Test SMS requested."));
          bt.println(F("{\"ack\":\"SMS_SENDING\"}"));
          gsmListen();
          sendSMS("BT TEST");
          btListen();
          bt.println(F("{\"ack\":\"SMS_DONE\"}"));
        } else if (strcmp(btCmdBuf, "CMD:STATUS") == 0) {
          bool mq2 = (digitalRead(PIN_MQ2) == LOW);
          bool mq135 = (digitalRead(PIN_MQ135) == LOW);
          btSendStatus(mq2, mq135);
        } else if (strncmp(btCmdBuf, "CMD:PHONE:", 10) == 0) {
          // Set phone number: CMD:PHONE:+254xxxxxxxxx
          char *number = btCmdBuf + 10;
          if (strlen(number) > 0 && numContacts < MAX_PHONES) {
            strncpy(phoneNumbers[numContacts], number, PHONE_LEN - 1);
            phoneNumbers[numContacts][PHONE_LEN - 1] = '\0';
            savePhone(numContacts, phoneNumbers[numContacts]);
            numContacts++;
            savePhoneCount(numContacts);
            Serial.print(F("[BT] Added phone: "));
            Serial.println(number);
            bt.println(F("{\"ack\":\"PHONE_SAVED\"}"));
            btSendPhones();
          } else {
            bt.println(F("{\"ack\":\"PHONE_FULL\"}"));
          }
        } else if (strcmp(btCmdBuf, "CMD:GETPHONES") == 0) {
          btSendPhones();
        } else if (strcmp(btCmdBuf, "CMD:CLEARPHONES") == 0) {
          numContacts = 0;
          savePhoneCount(0);
          Serial.println(F("[BT] Cleared all phones."));
          bt.println(F("{\"ack\":\"PHONES_CLEARED\"}"));
        }
        btCmdIdx = 0;
      }
    } else {
      if (btCmdIdx < sizeof(btCmdBuf) - 1) {
        btCmdBuf[btCmdIdx++] = c;
      }
    }
  }
}

// ===================== EEPROM PHONE FUNCTIONS ==============

/**
 * Load phone numbers from EEPROM.
 */
void loadPhones() {
  numContacts = EEPROM.read(EEPROM_PHONE_ADDR);
  if (numContacts > MAX_PHONES)
    numContacts = 0; // corrupt data
  for (byte i = 0; i < numContacts; i++) {
    for (byte j = 0; j < PHONE_LEN; j++) {
      phoneNumbers[i][j] =
          EEPROM.read(EEPROM_PHONE_ADDR + 1 + (i * PHONE_LEN) + j);
    }
    phoneNumbers[i][PHONE_LEN - 1] = '\0'; // safety
  }
  Serial.print(F("[EEPROM] Loaded "));
  Serial.print(numContacts);
  Serial.println(F(" contacts."));
  for (byte i = 0; i < numContacts; i++) {
    Serial.print(F("  #"));
    Serial.print(i + 1);
    Serial.print(F(": "));
    Serial.println(phoneNumbers[i]);
  }
}

/**
 * Save a phone number to EEPROM at slot index.
 */
void savePhone(byte index, const char *number) {
  for (byte j = 0; j < PHONE_LEN; j++) {
    char c = (j < strlen(number)) ? number[j] : '\0';
    EEPROM.update(EEPROM_PHONE_ADDR + 1 + (index * PHONE_LEN) + j, c);
  }
}

/**
 * Save contact count to EEPROM.
 */
void savePhoneCount(byte count) { EEPROM.update(EEPROM_PHONE_ADDR, count); }

/**
 * Send phone list to app as JSON.
 */
void btSendPhones() {
  bt.print(F("{\"phones\":["));
  for (byte i = 0; i < numContacts; i++) {
    if (i > 0)
      bt.print(',');
    bt.print('"');
    bt.print(phoneNumbers[i]);
    bt.print('"');
  }
  bt.println(F("]}"));
}

// ===================== GSM CORE FUNCTIONS ==================

/**
 * Flush any pending data from the GSM serial buffer.
 */
void gsmFlush() {
  while (gsm.available())
    gsm.read();
}

/**
 * Send an AT command to the GSM module and wait for an expected
 * response string. Returns true if the expected string is found
 * within the timeout period.
 *
 * @param cmd       The AT command to send (NULL to skip sending)
 * @param expected  The response substring to look for (e.g. "OK", ">")
 * @param timeout   Max time to wait in ms
 * @return true if expected response found
 */
bool sendAT(const char *cmd, const char *expected, unsigned int timeout) {
  gsmFlush();

  if (cmd != NULL) {
    Serial.print(F(">> "));
    Serial.println(cmd);
    gsm.println(cmd);
  }

  unsigned long start = millis();
  byte idx = 0;
  memset(gsmBuf, 0, sizeof(gsmBuf));

  while (millis() - start < timeout) {
    while (gsm.available()) {
      char c = gsm.read();
      if (idx < sizeof(gsmBuf) - 1) {
        gsmBuf[idx++] = c;
      }
    }

    // Check if expected string is in buffer
    if (expected != NULL && strstr(gsmBuf, expected) != NULL) {
      Serial.print(F("<< "));
      Serial.println(gsmBuf);
      return true;
    }
  }

  // Timeout
  Serial.print(F("<< TIMEOUT: "));
  Serial.println(gsmBuf);
  return false;
}

/**
 * Hardware reset the SIM800L module via the RST pin.
 * Waits for module to boot and synchronizes baud rate.
 */
void gsmHardReset() {
  Serial.println(F("[GSM] Hardware Reset..."));
  lcd.clear();
  lcd.print(F("GSM Resetting..."));

  digitalWrite(PIN_SIM_RST, LOW);
  delay(200);
  digitalWrite(PIN_SIM_RST, HIGH);

  // Wait for module to power up before any serial
  delay(5000);
  lcd.setCursor(0, 1);
  lcd.print(F("Syncing..."));

  // Auto-baud sync: send AT several times to let the module
  // lock onto our baud rate
  for (byte i = 0; i < 5; i++) {
    gsm.println(F("AT"));
    delay(500);
    gsmFlush(); // discard any garbage
  }

  // Now wait for "SMS Ready" or "Call Ready" (up to 25s more)
  lcd.setCursor(0, 1);
  lcd.print(F("Waiting net...  "));

  unsigned long start = millis();
  byte idx = 0;
  char bootBuf[48];
  memset(bootBuf, 0, sizeof(bootBuf));

  while (millis() - start < 25000UL) {
    while (gsm.available()) {
      char c = gsm.read();
      if (idx < sizeof(bootBuf) - 1) {
        bootBuf[idx++] = c;
      } else {
        // Shift buffer left to make room
        memmove(bootBuf, bootBuf + 1, sizeof(bootBuf) - 2);
        bootBuf[sizeof(bootBuf) - 2] = c;
      }

      if (strstr(bootBuf, "SMS Ready") != NULL) {
        Serial.println(F("[GSM] SMS Ready received."));
        delay(500);
        goto boot_done;
      }
    }
  }
  Serial.println(F("[GSM] Timeout waiting for SMS Ready — continuing anyway."));

boot_done:
  gsmFlush();
}

/**
 * Initialize the GSM module. Performs reset, verifies communication,
 * sets text mode, and checks signal/network.
 * Returns true if module is ready to send SMS.
 */
bool initGSM() {
  Serial.println(F("[GSM] Initializing..."));

  gsmHardReset();

  // Step 1: Auto-baud + communication check (try up to 5 times)
  bool atOk = false;
  for (byte i = 0; i < 5; i++) {
    if (sendAT("AT", "OK", AT_TIMEOUT)) {
      atOk = true;
      break;
    }
    delay(1000);
  }

  if (!atOk) {
    Serial.println(F("[GSM] FAIL: No AT response after retries."));
    lcd.clear();
    lcd.print(F("GSM: No Response"));
    return false;
  }

  // Step 2: Disable echo
  sendAT("ATE0", "OK", AT_TIMEOUT);

  // Step 3: Set text mode
  if (!sendAT("AT+CMGF=1", "OK", AT_TIMEOUT)) {
    Serial.println(F("[GSM] FAIL: Can't set text mode."));
    return false;
  }

  // Step 4: Check signal
  if (sendAT("AT+CSQ", "+CSQ:", AT_TIMEOUT)) {
    // Parse signal strength
    char *p = strstr(gsmBuf, "+CSQ:");
    if (p) {
      int sig = atoi(p + 6);
      Serial.print(F("[GSM] Signal: "));
      Serial.println(sig);
      lcd.clear();
      lcd.print(F("GSM Signal: "));
      lcd.print(sig);

      if (sig == 0 || sig == 99) {
        Serial.println(F("[GSM] WARNING: No signal!"));
        lcd.setCursor(0, 1);
        lcd.print(F("NO SIGNAL!"));
        delay(2000);
        // Continue anyway — signal may improve
      }
    }
  }

  // Step 5: Check network registration
  if (sendAT("AT+CREG?", "+CREG:", AT_TIMEOUT)) {
    char *p = strstr(gsmBuf, "+CREG:");
    if (p) {
      // Format: +CREG: mode,status
      char *comma = strchr(p, ',');
      if (comma) {
        int status = atoi(comma + 1);
        Serial.print(F("[GSM] Network status: "));
        Serial.println(status);
        // 1=registered home, 5=registered roaming
        if (status == 1 || status == 5) {
          Serial.println(F("[GSM] Network registered OK."));
          gsmReady = true;
          lcd.setCursor(0, 1);
          lcd.print(F("Network OK!    "));
          delay(1000);
          return true;
        }
      }
    }
    Serial.println(F("[GSM] Not registered on network."));
    lcd.setCursor(0, 1);
    lcd.print(F("No Network      "));
    delay(2000);
  }

  // Even if network check failed, mark as ready if AT works
  // (network may register later)
  gsmReady = true;
  return true;
}

/**
 * Check if the GSM module is still alive. If not, reset it.
 * Returns true if module is responsive.
 */
bool gsmHealthCheck() {
  Serial.println(F("[GSM] Health check..."));

  if (sendAT("AT", "OK", AT_TIMEOUT)) {
    Serial.println(F("[GSM] Health: OK"));
    return true;
  }

  // Module unresponsive — attempt recovery
  Serial.println(F("[GSM] Health: FAILED — resetting."));
  return initGSM();
}

// ===================== SMS FUNCTIONS =======================

/**
 * Attempt to send a single SMS to a single phone number.
 * Returns true if the module accepted the message.
 */
bool sendSingleSMS(const char *number, const char *msg) {
  Serial.print(F("[SMS] To: "));
  Serial.println(number);

  // Ensure text mode
  if (!sendAT("AT+CMGF=1", "OK", AT_TIMEOUT)) {
    Serial.println(F("[SMS] FAIL: Text mode."));
    return false;
  }

  // Build the CMGS command
  char cmgsCmd[32];
  snprintf(cmgsCmd, sizeof(cmgsCmd), "AT+CMGS=\"%s\"", number);

  // Send CMGS and wait for '>' prompt
  if (!sendAT(cmgsCmd, ">", 5000)) {
    Serial.println(F("[SMS] FAIL: No '>' prompt."));
    // Send ESC to cancel any partial command
    gsm.write(27);
    delay(500);
    gsmFlush();
    return false;
  }

  // Send message body
  gsm.print(F("LAB GUARDIAN ALERT: "));
  gsm.print(msg);
  gsm.write(26); // Ctrl+Z

  // Wait for +CMGS: (success) or ERROR
  if (sendAT(NULL, "+CMGS:", SMS_TIMEOUT)) {
    Serial.println(F("[SMS] SUCCESS!"));
    return true;
  }

  Serial.println(F("[SMS] FAIL: No +CMGS response."));
  return false;
}

/**
 * Send SMS to all contacts with retries and automatic recovery.
 * This is the main entry point for sending alerts.
 */
void sendSMS(const char *msg) {
  if (numContacts == 0) {
    // Use hardcoded fallback
    Serial.println(F("[SMS] No EEPROM contacts — using fallback."));
    char fallback[PHONE_LEN];
    strncpy_P(fallback, FALLBACK_PHONE, PHONE_LEN);
    Serial.println(F("=== SMS ALERT START ==="));
    lcd.setCursor(0, 0);
    lcd.print(F("Sending SMS...  "));
    bool sent = false;
    for (byte attempt = 1; attempt <= MAX_SMS_RETRIES; attempt++) {
      if (sendSingleSMS(fallback, msg)) {
        sent = true;
        break;
      }
      delay(2000);
    }
    Serial.print(sent ? F("[SMS] Sent to fallback ")
                      : F("[SMS] FAILED fallback "));
    Serial.println(fallback);
    Serial.println(F("=== SMS ALERT END ==="));
    return;
  }
  Serial.println(F("=== SMS ALERT START ==="));

  lcd.setCursor(0, 0);
  lcd.print(F("Sending SMS...  "));

  for (byte i = 0; i < numContacts; i++) {
    bool sent = false;

    // Try up to MAX_SMS_RETRIES times
    for (byte attempt = 1; attempt <= MAX_SMS_RETRIES; attempt++) {
      Serial.print(F("[SMS] Attempt "));
      Serial.print(attempt);
      Serial.print(F("/"));
      Serial.println(MAX_SMS_RETRIES);

      lcd.setCursor(0, 1);
      lcd.print(F("Try "));
      lcd.print(attempt);
      lcd.print(F("/"));
      lcd.print(MAX_SMS_RETRIES);
      lcd.print(F("        "));

      if (sendSingleSMS(phoneNumbers[i], msg)) {
        sent = true;
        break;
      }

      // Wait before retry
      delay(2000);
    }

    // If all retries failed, hard reset and try once more
    if (!sent) {
      Serial.println(F("[SMS] All retries failed. Hard resetting GSM..."));
      lcd.clear();
      lcd.print(F("GSM Recovery..."));

      if (initGSM()) {
        Serial.println(F("[SMS] Recovery complete. Final attempt..."));
        lcd.clear();
        lcd.print(F("Final SMS try..."));
        sent = sendSingleSMS(phoneNumbers[i], msg);
      }
    }

    // Log result
    if (sent) {
      Serial.print(F("[SMS] Delivered to "));
      Serial.println(phoneNumbers[i]);
      lcd.setCursor(0, 1);
      lcd.print(F("SMS Sent!       "));
    } else {
      Serial.print(F("[SMS] FAILED to "));
      Serial.println(phoneNumbers[i]);
      lcd.setCursor(0, 1);
      lcd.print(F("SMS FAILED!     "));
    }
    delay(1000);
  }

  Serial.println(F("=== SMS ALERT END ==="));

  // Restore alert display
  lcd.setCursor(0, 0);
  lcd.print(F("!!! ALERT !!!   "));
}

// ===================== ALARM FUNCTIONS =====================

void triggerAlarm(const char *message) {
  if (!alarmActive) {
    Serial.println(F("[ALARM] >>> TRIGGERED <<<"));
    Serial.print(F("[ALARM] Reason: "));
    Serial.println(message);
    buzzerSilenced = false; // New alarm resets silence
    // Reset buzzer pattern to start fresh
    buzzerStep = 0;
    buzzerPatternStart = millis();
    digitalWrite(PIN_BUZZER, HIGH); // Start beeping immediately
    // Notify app
    bt.print(F("{\"alert\":\""));
    bt.print(message);
    bt.println(F("\"}"));
  }
  alarmActive = true;

  // Audible/Visual
  // Buzzer pattern handled by updateBuzzer() in loop
  digitalWrite(PIN_LED, HIGH);

  // LCD
  lcd.setCursor(0, 0);
  lcd.print(F("!!! ALERT !!!   "));
  lcd.setCursor(0, 1);
  lcd.print(message);
  // Pad to clear old text
  byte len = strlen(message);
  for (byte i = len; i < 16; i++)
    lcd.print(' ');

  // SMS (rate limited)
  if (millis() - lastSmsTime > SMS_COOLDOWN) {
    sendSMS(message);
    lastSmsTime = millis();
  }
}

void resetAlarm() {
  if (alarmActive) {
    Serial.println(F("[ALARM] Cleared — back to monitoring."));
    alarmActive = false;
    buzzerSilenced = false;
    buzzerStep = 0;
    digitalWrite(PIN_BUZZER, LOW);
    digitalWrite(PIN_LED, LOW);

    lcd.clear();
    lcd.print(F("System Ready"));
    lcd.setCursor(0, 1);
    lcd.print(F("Monitoring..."));
    bt.println(F("{\"alert\":\"CLEAR\"}"));
  }
}

/**
 * Non-blocking buzzer pattern: beep-beep-pause
 */
void updateBuzzer() {
  if (!alarmActive || buzzerSilenced) {
    digitalWrite(PIN_BUZZER, LOW);
    return;
  }
  if (millis() - buzzerPatternStart >= BUZZER_PATTERN[buzzerStep]) {
    buzzerStep = (buzzerStep + 1) % BUZZER_STEPS;
    buzzerPatternStart = millis();
  }
  // Even steps = ON, odd steps = OFF
  digitalWrite(PIN_BUZZER, (buzzerStep % 2 == 0) ? HIGH : LOW);
}

// ===================== SETUP & LOOP ========================

void setup() {
  Serial.begin(9600);
  Serial.println(F("=== Lab Guardian Starting ==="));
  Serial.println(F("Pin Config:"));
  Serial.print(F("  MQ135=D"));
  Serial.println(PIN_MQ135);
  Serial.print(F("  MQ2=D"));
  Serial.println(PIN_MQ2);
  Serial.print(F("  Buzzer=D"));
  Serial.println(PIN_BUZZER);
  Serial.print(F("  LED=D"));
  Serial.println(PIN_LED);
  Serial.print(F("  GSM RST=D"));
  Serial.println(PIN_SIM_RST);
  Serial.print(F("  GSM RX=D"));
  Serial.println(PIN_GSM_RX);
  Serial.print(F("  GSM TX=D"));
  Serial.println(PIN_GSM_TX);

  // Pin modes
  pinMode(PIN_MQ135, INPUT);
  pinMode(PIN_MQ2, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_SIM_RST, OUTPUT);

  // Ensure outputs off
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED, LOW);
  digitalWrite(PIN_SIM_RST, HIGH);

  // Startup buzzer test (quick beep to confirm hardware)
  Serial.println(F("[BUZZER] Test beep..."));
  digitalWrite(PIN_BUZZER, HIGH);
  delay(200);
  digitalWrite(PIN_BUZZER, LOW);
  delay(100);
  digitalWrite(PIN_BUZZER, HIGH);
  delay(200);
  digitalWrite(PIN_BUZZER, LOW);
  Serial.println(F("[BUZZER] Test done."));

  // Load emergency contacts from EEPROM
  loadPhones();

  // GSM serial
  gsm.begin(9600);

  // Bluetooth serial (HC-05)
  bt.begin(9600);
  bt.println(F("{\"status\":\"booting\"}"));
  Serial.println(F("[BT] HC-05 initialized."));

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.print(F("Lab Guardian"));
  lcd.setCursor(0, 1);
  lcd.print(F("Starting..."));
  delay(1000);

  // Initialize GSM (handles reset + wait internally)
  initGSM();

  // Sensor warmup
  lcd.clear();
  lcd.print(F("Sensors Warmup"));
  lcd.setCursor(0, 1);
  lcd.print(F("Please wait..."));
  delay(3000);

  // Ready
  lcd.clear();
  lcd.print(F("System Ready"));
  lcd.setCursor(0, 1);
  lcd.print(F("Monitoring..."));
  Serial.println(F("=== System Ready ==="));

  lastHealthTime = millis();

  // Switch to BT as default listener
  btListen();
  bt.println(F("{\"status\":\"ready\"}"));
}

void loop() {
  // --- Sensor Reading ---
  bool mq135Triggered = (digitalRead(PIN_MQ135) == LOW);
  bool mq2Triggered = (digitalRead(PIN_MQ2) == LOW);

  // --- Bluetooth: check for incoming commands ---
  btCheckCommands();

  // --- Debug Logging + BT status (every 2s) ---
  if (millis() - lastDebugTime > DEBUG_INTERVAL) {
    Serial.print(F("[SENSOR] MQ2="));
    Serial.print(mq2Triggered ? F("TRIGGERED") : F("OK"));
    Serial.print(F(" | MQ135="));
    Serial.print(mq135Triggered ? F("TRIGGERED") : F("OK"));
    Serial.print(F(" | Alarm="));
    Serial.print(alarmActive ? F("ON") : F("OFF"));
    Serial.print(F(" | GSM="));
    Serial.print(gsmReady ? F("READY") : F("DOWN"));
    Serial.print(F(" | Uptime="));
    Serial.print(millis() / 1000);
    Serial.println(F("s"));

    // Send JSON to app over Bluetooth
    btSendStatus(mq2Triggered, mq135Triggered);

    lastDebugTime = millis();
  }

  if (mq135Triggered || mq2Triggered) {
    if (mq135Triggered && mq2Triggered)
      triggerAlarm("GAS+AIR CRITICAL");
    else if (mq2Triggered)
      triggerAlarm("GAS/FIRE DANGER");
    else
      triggerAlarm("AIR QUALITY WARN");
  } else {
    resetAlarm();
  }

  // --- Periodic GSM Health Check (every 5 min) ---
  if (millis() - lastHealthTime > HEALTH_INTERVAL) {
    if (!alarmActive) {
      gsmListen();
      gsmHealthCheck();
      btListen();
      lcd.clear();
      lcd.print(F("System Ready"));
      lcd.setCursor(0, 1);
      lcd.print(F("Monitoring..."));
    }
    lastHealthTime = millis();
  }

  // --- Buzzer pattern ---
  updateBuzzer();

  delay(100);
}
