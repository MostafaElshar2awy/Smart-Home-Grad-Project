// Smart Lock System with Fingerprint, Password, and Access Logging
// Uses DS1302 RTC for timekeeping, EEPROM for log storage, and LCD for user interface

#include <Adafruit_Fingerprint.h> // Library for fingerprint sensor operations
#include <Keypad.h>              // Library for 4x4 keypad input
#include <Wire.h>                // Library for I2C communication
#include <LiquidCrystal_I2C.h>   // Library for I2C LCD display
#include <RtcDS1302.h>           // Library for DS1302 RTC module
#include <EEPROM.h>              // Library for EEPROM storage

// ----------------------------------------------------------------------------
// Hardware Configuration
// ----------------------------------------------------------------------------

// DS1302 RTC Configuration
// Connects to Arduino Mega: DAT (IO) -> Pin 26, CLK (SCLK) -> Pin 25, RST (CE) -> Pin 27
ThreeWire myWire(26, 25, 27);
RtcDS1302<ThreeWire> Rtc(myWire);

// Keypad Configuration (4x4 matrix)
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[ROWS] = {9, 8, 7, 6}; // Keypad row pins
byte colPins[COLS] = {5, 4, 3, 2}; // Keypad column pins
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Fingerprint Sensor Configuration
// Uses Serial1 (pins 19 RX, 18 TX on Arduino Mega)
#define mySerial Serial1
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// LCD Configuration
// I2C LCD with address 0x27 (or 0x3F for some modules), 16x2 characters
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Pin Definitions
const int RELAY_PIN = 11;  // Relay pin for controlling door lock
const int BUZZER_PIN = 10; // Buzzer pin for audible feedback
const int LED_PIN = 13;    // LED pin for visual status indication

// ----------------------------------------------------------------------------
// System Configuration
// ----------------------------------------------------------------------------

// User Passwords
String masterPassword = "9999";   // Admin password for system access
String fatherPassword = "1111";   // Father user password
String motherPassword = "2222";   // Mother user password
String sonPassword = "3333";      // Son user password
String daughterPassword = "4444"; // Daughter user password

// System State Variables
String inputPassword = "";          // Stores user-entered password
byte systemMode = 0;               // System mode: 0=selection, 1=fingerprint, 2=password
byte failedAttempts = 0;           // Tracks failed login attempts
const byte maxAttempts = 3;        // Maximum allowed failed attempts
unsigned long lockoutTime = 10000; // Lockout duration in milliseconds (10 seconds)
unsigned long lockoutStart = 0;    // Timestamp when lockout started
bool systemLocked = false;          // Flag indicating if system is locked
bool adminMode = false;            // Flag indicating if admin mode is active

// Fingerprint IDs
int registeredFingers[] = {1, 2, 3, 4, 5}; // IDs: Admin(1), Father(2), Mother(3), Son(4), Daughter(5)
const int fingerCount = 5;                 // Number of registered fingerprints

// Access Log Configuration
struct AccessLog {
  String user;      // User name (e.g., "Father")
  String timestamp; // Timestamp (e.g., "2025/06/27 22:24:00")
};
AccessLog accessLog[10];       // Array to store up to 10 log entries
int logIndex = 0;              // Current index in accessLog (circular buffer)
const int maxLogs = 10;        // Maximum number of log entries
const int logEntrySize = 28;   // EEPROM size per log entry (8 for user + 19 for timestamp + 1 for null)
const int logIndexAddress = 280; // EEPROM address for storing logIndex

// ----------------------------------------------------------------------------
// Setup Function
// ----------------------------------------------------------------------------

// Initializes hardware components and system state
void setup() {
  // Initialize Serial for debugging
  Serial.begin(9600);
  
  // Initialize output pins
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Ensure door is locked
  digitalWrite(LED_PIN, LOW);   // Turn off status LED
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Smart Lock System");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(2000);
  
  // Initialize DS1302 RTC
  Rtc.Begin();
  if (!Rtc.GetIsRunning()) {
    lcd.clear();
    lcd.print("RTC Error!");
    lcd.setCursor(0, 1);
    lcd.print("Check Connection");
    while (1) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
  }
  // Set RTC to current date and time (run once, then comment out)
  Rtc.SetDateTime(RtcDateTime(2025, 6, 30, 7, 35, 0)); // 7:35 pm, June 30, 2025
  
  // Initialize fingerprint sensor
  finger.begin(57600);
  if (finger.verifyPassword()) {
    lcd.clear();
    lcd.print("Fingerprint Sensor");
    lcd.setCursor(0, 1);
    lcd.print("Ready!");
    delay(1000);
  } else {
    lcd.clear();
    lcd.print("Sensor Error!");
    lcd.setCursor(0, 1);
    lcd.print("Check Connection");
    while (1) {
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);
      delay(200);
    }
  }
  
  // Initialize accessLog to empty to prevent random data
  for (int i = 0; i < maxLogs; i++) {
    accessLog[i].user = "";
    accessLog[i].timestamp = "";
  }
  
  // Load access logs from EEPROM
  loadAccessLogFromEEPROM();
  
  // Display initial mode selection screen
  showModeSelection();
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------

// Main loop to handle system modes and lock status
void loop() {
  // Check if system is locked due to excessive failed attempts
  checkLockStatus();
  
  // Handle system modes
  if (systemMode == 0) {
    handleModeSelection();
  } else if (systemMode == 1) {
    handleFingerprintMode();
  } else if (systemMode == 2) {
    handlePasswordMode();
  }
}

// ----------------------------------------------------------------------------
// System Status Functions
// ----------------------------------------------------------------------------

// Checks and handles system lockout status
void checkLockStatus() {
  if (systemLocked) {
    if (millis() - lockoutStart >= lockoutTime) {
      // Unlock system after lockout period
      systemLocked = false;
      failedAttempts = 0;
      lcd.clear();
      lcd.print("System Unlocked");
      delay(1500);
      showModeSelection();
    } else {
      // Display remaining lockout time
      unsigned long remaining = (lockoutTime - (millis() - lockoutStart)) / 1000;
      lcd.clear();
      lcd.print("System Locked!");
      lcd.setCursor(0, 1);
      lcd.print("Wait ");
      lcd.print(remaining);
      lcd.print(" sec");
    }
  }
}

// ----------------------------------------------------------------------------
// User Interface Functions
// ----------------------------------------------------------------------------

// Displays mode selection screen
void showModeSelection() {
  systemMode = 0;
  lcd.clear();
  lcd.print("Select Mode:");
  lcd.setCursor(0, 1);
  lcd.print("1:Finger 2:Pass");
}

// Handles mode selection (Fingerprint, Password, Admin, or Time display)
void handleModeSelection() {
  char key = keypad.getKey();
  if (key == '1') {
    // Enter fingerprint mode
    systemMode = 1;
    lcd.clear();
    lcd.print("Fingerprint Mode");
    lcd.setCursor(0, 1);
    lcd.print("Place Finger...");
  } else if (key == '2') {
    // Enter password mode
    systemMode = 2;
    lcd.clear();
    lcd.print("Password Mode");
    lcd.setCursor(0, 1);
    lcd.print("Enter Password:");
  } else if (key == 'A' && systemMode == 0) {
    // Enter admin mode with fingerprint
    lcd.clear();
    lcd.print("Admin Fingerprint");
    lcd.setCursor(0, 1);
    lcd.print("Place Finger...");
    unsigned long startTime = millis();
    const unsigned long timeout = 10000;
    while (millis() - startTime < timeout) {
      int fingerID = getFingerprintID();
      if (fingerID > 0) {
        if (isAdminFinger(fingerID)) {
          adminMode = true;
          lcd.clear();
          lcd.print("Hello Admin!");
          successBeep();
          delay(1000);
          showAdminMenu();
          return;
        } else {
          lcd.clear();
          lcd.print("Access Denied!");
          lcd.setCursor(0, 1);
          lcd.print("Not Admin Finger");
          errorBeep();
          delay(1500);
          showModeSelection();
          return;
        }
      }
      delay(50);
    }
    lcd.clear();
    lcd.print("Timeout!");
    lcd.setCursor(0, 1);
    lcd.print("No Finger Detected");
    errorBeep();
    delay(1500);
    showModeSelection();
  } else if (key == 'B') {
    // Display current time using DS1302 RTC
    RtcDateTime now = Rtc.GetDateTime();
    char timestamp[20];
    snprintf_P(timestamp, 
               sizeof(timestamp), 
               PSTR("%04u/%02u/%02u %02u:%02u:%02u"),
               now.Year(), now.Month(), now.Day(),
               now.Hour(), now.Minute(), now.Second());
    lcd.clear();
    lcd.print("Current Time:");
    lcd.setCursor(0, 1);
    lcd.print(timestamp);
    delay(3000);
    showModeSelection();
  }
}

// ----------------------------------------------------------------------------
// Fingerprint Authentication Functions
// ----------------------------------------------------------------------------

// Handles fingerprint authentication mode
void handleFingerprintMode() {
  int fingerID = getFingerprintID();
  if (fingerID > 0) {
    // Successful fingerprint match
    String userName = getUserNameFromFingerID(fingerID);
    lcd.clear();
    lcd.print("Hello ");
    lcd.print(userName);
    lcd.print("!");
    lcd.setCursor(0, 1);
    lcd.print("ID: ");
    lcd.print(fingerID);
    successBeep();
    logAccess(userName);
    unlockDoor();
    delay(2000);
    showModeSelection();
  } else if (fingerID == -1) {
    // Unknown fingerprint
    lcd.clear();
    lcd.print("Access Denied!");
    lcd.setCursor(0, 1);
    lcd.print("Unknown Finger");
    failedAttempts++;
    errorBeep();
    if (failedAttempts >= maxAttempts) {
      lockSystem();
    } else {
      delay(1500);
      lcd.clear();
      lcd.print("Fingerprint Mode");
      lcd.setCursor(0, 1);
      lcd.print("Place Finger...");
    }
  }
  char key = keypad.getKey();
  if (key == '*') {
    // Return to mode selection
    showModeSelection();
  }
}

// Retrieves fingerprint ID from sensor
// Returns: ID if match found, -1 if no match, 0 if no finger detected
int getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return 0;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return 0;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return -1;
  return finger.fingerID;
}

// Checks if fingerprint ID is admin (ID=1)
bool isAdminFinger(int fingerID) {
  return (fingerID == 1);
}

// ----------------------------------------------------------------------------
// Password Authentication Functions
// ----------------------------------------------------------------------------

// Handles password authentication mode
void handlePasswordMode() {
  char key = keypad.getKey();
  if (key) {
    if (key == '#') {
      // Verify entered password
      verifyPassword();
    } else if (key == '*') {
      // Clear entered password
      inputPassword = "";
      lcd.clear();
      lcd.print("Password Mode");
      lcd.setCursor(0, 1);
      lcd.print("Enter Password:");
    } else if (key == 'A' && inputPassword.length() == 0) {
      // Change user password
      changeUserPassword();
    } else {
      // Append key to input password and display asterisks
      inputPassword += key;
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
      for (int i = 0; i < inputPassword.length(); i++) {
        lcd.print("*");
      }
    }
  }
}

// Verifies entered password and grants access or locks system
void verifyPassword() {
  String userName = "";
  if (inputPassword == masterPassword) {
    // Admin access
    userName = "Admin";
    adminMode = true;
    lcd.clear();
    lcd.print("Hello Admin!");
    successBeep();
    delay(1000);
    showAdminMenu();
  } else if (inputPassword == fatherPassword) {
    // Father access
    userName = "Father";
    lcd.clear();
    lcd.print("Hello Father!");
    successBeep();
    logAccess(userName);
    unlockDoor();
    delay(2000);
    showModeSelection();
  } else if (inputPassword == motherPassword) {
    // Mother access
    userName = "Mother";
    lcd.clear();
    lcd.print("Hello Mother!");
    successBeep();
    logAccess(userName);
    unlockDoor();
    delay(2000);
    showModeSelection();
  } else if (inputPassword == sonPassword) {
    // Son access
    userName = "Son";
    lcd.clear();
    lcd.print("Hello Son!");
    successBeep();
    logAccess(userName);
    unlockDoor();
    delay(2000);
    showModeSelection();
  } else if (inputPassword == daughterPassword) {
    // Daughter access
    userName = "Daughter";
    lcd.clear();
    lcd.print("Hello Daughter!");
    successBeep();
    logAccess(userName);
    unlockDoor();
    delay(2000);
    showModeSelection();
  } else {
    // Incorrect password
    lcd.clear();
    lcd.print("Wrong Password!");
    failedAttempts++;
    errorBeep();
    if (failedAttempts >= maxAttempts) {
      lockSystem();
    } else {
      delay(1500);
      lcd.clear();
      lcd.print("Password Mode");
      lcd.setCursor(0, 1);
      lcd.print("Enter Password:");
    }
  }
  inputPassword = "";
}

// ----------------------------------------------------------------------------
// Admin Menu Functions
// ----------------------------------------------------------------------------

// Displays admin menu for managing fingerprints, passwords, logs, and EEPROM
void showAdminMenu() {
  while (adminMode) {
    lcd.clear();
    lcd.print("Admin Menu:");
    lcd.setCursor(0, 1);
    lcd.print("1:Finger 2:Pass 3:Log 4:Clear");
    char key = keypad.getKey();
    if (key == '1') {
      manageFingerprints();
    } else if (key == '2') {
      managePasswords();
    } else if (key == '3') {
      displayAccessLog();
    } else if (key == '4') {
      clearEEPROM();
    } else if (key == '*') {
      adminMode = false;
      showModeSelection();
      return;
    }
  }
}

// Manages fingerprint addition and deletion
void manageFingerprints() {
  while (true) {
    lcd.clear();
    lcd.print("Fingerprint Admin:");
    lcd.setCursor(0, 1);
    lcd.print("1:Add 2:Del *:Back");
    char key = keypad.getKey();
    if (key == '1') {
      enrollFingerprint();
    } else if (key == '2') {
      deleteFingerprint();
    } else if (key == '*') {
      return;
    }
  }
}

// Manages password changes and deletions
void managePasswords() {
  while (true) {
    lcd.clear();
    lcd.print("Password Admin:");
    lcd.setCursor(0, 1);
    lcd.print("1:Change 2:Del *:Back");
    char key = keypad.getKey();
    if (key == '1') {
      changeUserPassword();
    } else if (key == '2') {
      deleteUserPassword();
    } else if (key == '*') {
      return;
    }
  }
}

// ----------------------------------------------------------------------------
// Fingerprint Management Functions
// ----------------------------------------------------------------------------

// Enrolls a new fingerprint
void enrollFingerprint() {
  lcd.clear();
  lcd.print("Enroll Finger");
  lcd.setCursor(0, 1);
  lcd.print("Press # to start");
  while (keypad.getKey() != '#') delay(50);
  lcd.clear();
  lcd.print("Enter Finger ID");
  lcd.setCursor(0, 1);
  lcd.print("(1-127):");
  int id = readNumber();
  if (id < 1 || id > 127) {
    lcd.clear();
    lcd.print("Invalid ID!");
    errorBeep();
    delay(1500);
    return;
  }
  lcd.clear();
  lcd.print("Enrolling ID:");
  lcd.print(id);
  while (!getFingerprintEnroll(id));
  lcd.clear();
  lcd.print("Enroll Success!");
  successBeep();
  delay(2000);
}

// Processes fingerprint enrollment
// Parameters: id - Fingerprint ID to enroll (1-127)
// Returns: true if successful, false otherwise
uint8_t getFingerprintEnroll(int id) {
  int p = -1;
  lcd.setCursor(0, 1);
  lcd.print("Place Finger");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
    else if (p != FINGERPRINT_OK) {
      lcd.clear();
      lcd.print("Error 1");
      errorBeep();
      return p;
    }
  }
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Error 2");
    errorBeep();
    return p;
  }
  lcd.setCursor(0, 1);
  lcd.print("Remove Finger");
  delay(2000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
  }
  p = -1;
  lcd.setCursor(0, 1);
  lcd.print("Place Same Finger");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) continue;
    else if (p != FINGERPRINT_OK) {
      lcd.clear();
      lcd.print("Error 3");
      errorBeep();
      return p;
    }
  }
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Error 4");
    errorBeep();
    return p;
  }
  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Error 5");
    errorBeep();
    return p;
  }
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Error 6");
    errorBeep();
    return p;
  }
  return true;
}

// Deletes a fingerprint
void deleteFingerprint() {
  lcd.clear();
  lcd.print("Delete Finger");
  lcd.setCursor(0, 1);
  lcd.print("Enter ID to delete:");
  int id = readNumber();
  if (id < 1 || id > 127) {
    lcd.clear();
    lcd.print("Invalid ID!");
    errorBeep();
    delay(1500);
    showModeSelection();
    return;
  }
  if (id == 1) {
    // Protect admin fingerprint with master password
    lcd.clear();
    lcd.print("Admin Finger");
    lcd.setCursor(0, 1);
    lcd.print("Enter Master Pass:");
    String enteredPassword = readPassword();
    if (enteredPassword != masterPassword) {
      lcd.clear();
      lcd.print("Wrong Password!");
      errorBeep();
      delay(1500);
      showModeSelection();
      return;
    }
  }
  uint8_t p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    lcd.clear();
    lcd.print("Finger Deleted!");
    successBeep();
  } else {
    lcd.clear();
    lcd.print("Delete Failed!");
    errorBeep();
  }
  delay(1500);
}

// ----------------------------------------------------------------------------
// Password Management Functions
// ----------------------------------------------------------------------------

// Changes a user's password
void changeUserPassword() {
  lcd.clear();
  lcd.print("Change Password");
  lcd.setCursor(0, 1);
  lcd.print("Select User:");
  delay(1000);
  lcd.clear();
  lcd.print("1:Father 2:Mother");
  lcd.setCursor(0, 1);
  lcd.print("3:Son 4:Daughter");
  char key;
  String* currentPassword = NULL;
  String user = "";
  while (true) {
    key = keypad.getKey();
    if (key == '1') { currentPassword = &fatherPassword; user = "Father"; break; }
    if (key == '2') { currentPassword = &motherPassword; user = "Mother"; break; }
    if (key == '3') { currentPassword = &sonPassword; user = "Son"; break; }
    if (key == '4') { currentPassword = &daughterPassword; user = "Daughter"; break; }
    if (key == '*') return;
  }
  lcd.clear();
  lcd.print(user);
  lcd.print(" Old Password");
  lcd.setCursor(0, 1);
  lcd.print("Enter Old Pass:");
  String oldPass = readPassword();
  if (oldPass != *currentPassword) {
    lcd.clear();
    lcd.print("Wrong Old Pass!");
    errorBeep();
    delay(1500);
    showModeSelection();
    return;
  }
  lcd.clear();
  lcd.print(user);
  lcd.print(" New Password");
  lcd.setCursor(0, 1);
  lcd.print("Enter New Pass:");
  String newPass = readPassword();
  *currentPassword = newPass;
  lcd.clear();
  lcd.print(user);
  lcd.print(" Password");
  lcd.setCursor(0, 1);
  lcd.print("Changed Success!");
  successBeep();
  delay(2000);
}

// Deletes a user's password
void deleteUserPassword() {
  lcd.clear();
  lcd.print("Delete Password");
  lcd.setCursor(0, 1);
  lcd.print("Select User:");
  delay(1000);
  lcd.clear();
  lcd.print("1:Father 2:Mother");
  lcd.setCursor(0, 1);
  lcd.print("3:Son 4:Daughter");
  char key;
  String* currentPassword = NULL;
  String user = "";
  while (true) {
    key = keypad.getKey();
    if (key == '1') { currentPassword = &fatherPassword; user = "Father"; break; }
    if (key == '2') { currentPassword = &motherPassword; user = "Mother"; break; }
    if (key == '3') { currentPassword = &sonPassword; user = "Son"; break; }
    if (key == '4') { currentPassword = &daughterPassword; user = "Daughter"; break; }
    if (key == '*') return;
  }
  lcd.clear();
  lcd.print("Delete ");
  lcd.print(user);
  lcd.print(" Pass?");
  lcd.setCursor(0, 1);
  lcd.print("#:Yes *:Cancel");
  while (true) {
    key = keypad.getKey();
    if (key == '#') {
      *currentPassword = "";
      lcd.clear();
      lcd.print(user);
      lcd.print(" Password");
      lcd.setCursor(0, 1);
      lcd.print("Deleted!");
      successBeep();
      delay(2000);
      return;
    } else if (key == '*') {
      return;
    }
  }
}

// ----------------------------------------------------------------------------
// Access Log Functions
// ----------------------------------------------------------------------------

// Logs an access event with user and timestamp
// Parameters: user - Name of the user accessing the system
void logAccess(String user) {
  // Get current time from DS1302 RTC
  RtcDateTime now = Rtc.GetDateTime();
  char timestamp[20];
  snprintf_P(timestamp, 
             sizeof(timestamp), 
             PSTR("%04u/%02u/%02u %02u:%02u:%02u"),
             now.Year(), now.Month(), now.Day(),
             now.Hour(), now.Minute(), now.Second());
  
  // Debug: Print timestamp to Serial Monitor
  Serial.print("RTC Time: ");
  Serial.println(timestamp);
  
  // Pad user name to 8 characters for consistent storage
  String paddedUser = user;
  while (paddedUser.length() < 8) paddedUser += " ";
  accessLog[logIndex].user = paddedUser.substring(0, 8);
  accessLog[logIndex].timestamp = String(timestamp);
  
  // Save log to EEPROM
  saveAccessLogToEEPROM();
  
  // Debug: Print log entry to Serial Monitor
  Serial.print("Access Log: ");
  Serial.print(user);
  Serial.print(" at ");
  Serial.println(timestamp);
  
  // Update log index (circular buffer)
  logIndex = (logIndex + 1) % maxLogs;
  EEPROM.update(logIndexAddress, logIndex);
}

// Saves current log entry to EEPROM
void saveAccessLogToEEPROM() {
  int address = logIndex * logEntrySize;
  String logEntry = accessLog[logIndex].user + accessLog[logIndex].timestamp;
  for (int i = 0; i < logEntry.length() && i < logEntrySize - 1; i++) {
    EEPROM.update(address + i, logEntry[i]);
  }
  EEPROM.update(address + logEntrySize - 1, 0); // Null terminator
}

// Loads access logs from EEPROM
void loadAccessLogFromEEPROM() {
  logIndex = EEPROM.read(logIndexAddress);
  if (logIndex >= maxLogs) logIndex = 0; // Safety check
  
  for (int i = 0; i < maxLogs; i++) {
    int address = i * logEntrySize;
    char buffer[logEntrySize];
    for (int j = 0; j < logEntrySize - 1; j++) {
      buffer[j] = EEPROM.read(address + j);
      if (buffer[j] == 0) break; // Stop at null terminator
    }
    buffer[logEntrySize - 1] = 0; // Ensure null termination
    
    String logEntry = String(buffer);
    // Validate log entry (must have user and timestamp)
    if (logEntry.length() >= 8 && logEntry[8] != 0) {
      accessLog[i].user = logEntry.substring(0, 8);
      accessLog[i].timestamp = logEntry.substring(8);
      accessLog[i].user.trim();
    } else {
      accessLog[i].user = "";
      accessLog[i].timestamp = "";
    }
  }
}

// Displays access logs on LCD
void displayAccessLog() {
  if (logIndex == 0) {
    lcd.clear();
    lcd.print("No Logs Available");
    delay(1500);
    return;
  }
  
  // Display valid logs in reverse order
  for (int i = 0; i < logIndex; i++) {
    int index = (logIndex - 1 - i + maxLogs) % maxLogs;
    if (accessLog[index].user != "" && accessLog[index].timestamp != "") {
      lcd.clear();
      lcd.print(accessLog[index].user);
      lcd.setCursor(0, 1);
      lcd.print(accessLog[index].timestamp.substring(5)); // Skip year to fit 16x2 LCD
      delay(3000);
    }
  }
  lcd.clear();
  lcd.print("End of Log");
  delay(1500);
}

// Clears EEPROM to reset logs
void clearEEPROM() {
  for (int i = 0; i < logIndexAddress + 1; i++) {
    EEPROM.update(i, 0);
  }
  logIndex = 0;
  for (int i = 0; i < maxLogs; i++) {
    accessLog[i].user = "";
    accessLog[i].timestamp = "";
  }
  lcd.clear();
  lcd.print("EEPROM Cleared!");
  delay(2000);
}

// ----------------------------------------------------------------------------
// Utility Functions
// ----------------------------------------------------------------------------

// Maps fingerprint ID to user name
String getUserNameFromFingerID(int fingerID) {
  switch (fingerID) {
    case 1: return "Admin";
    case 2: return "Father";
    case 3: return "Mother";
    case 4: return "Son";
    case 5: return "Daughter";
    default: return "Unknown";
  }
}

// Unlocks the door using relay
void unlockDoor() {
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);
  delay(5000); // Keep door unlocked for 5 seconds
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

// Locks system after too many failed attempts
void lockSystem() {
  systemLocked = true;
  lockoutStart = millis();
  lcd.clear();
  lcd.print("System Locked!");
  lcd.setCursor(0, 1);
  lcd.print("Try again later");
  alarmBeep();
}

// Plays success sound on buzzer
void successBeep() {
  tone(BUZZER_PIN, 1000, 200);
  delay(200);
  tone(BUZZER_PIN, 1500, 300);
}

// Plays error sound on buzzer
void errorBeep() {
  tone(BUZZER_PIN, 500, 500);
}

// Plays alarm sound for lockout
void alarmBeep() {
  for (int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 1000, 300);
    delay(500);
  }
}

// Reads password from keypad
// Returns: Entered password as String
String readPassword() {
  String pass = "";
  char key;
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  while (true) {
    key = keypad.getKey();
    if (key == '#') {
      return pass;
    } else if (key == '*') {
      pass = "";
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
    } else if (key) {
      pass += key;
      lcd.print("*");
    }
  }
}

// Reads number from keypad (for fingerprint ID)
// Returns: Entered number as integer
int readNumber() {
  String numStr = "";
  char key;
  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  while (true) {
    key = keypad.getKey();
    if (key == '#') {
      if (numStr.length() > 0) {
        return numStr.toInt();
      }
    } else if (key == '*') {
      numStr = "";
      lcd.setCursor(0, 1);
      lcd.print("                ");
      lcd.setCursor(0, 1);
    } else if (key >= '0' && key <= '9') {
      numStr += key;
      lcd.print(key);
    }
  }
}

