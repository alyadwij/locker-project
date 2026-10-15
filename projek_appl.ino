#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Firebase_ESP_Client.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h> 
#include <RTClib.h>

// Provide the token generation process info.
#include "addons/TokenHelper.h"
// Provide the RTDB payload printing info and other helper functions.
#include "addons/RTDBHelper.h"

// RFID Setup
#define SDA_PIN 2
#define RST_PIN 5
#define SCK_PIN 18
#define MOSI_PIN 23
#define MISO_PIN 19
#define LED_PIN 13
#define SERVO_PIN 26

// Insert your network credentials
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Insert Firebase project API Key
#define API_KEY "YOUR_FIREBASE_API_KEY"

// Insert RTDB URL
#define DATABASE_URL "https://your-rtdb-url"

//Insert Telegram Bot Token
#define BOT_TOKEN "YOUR_BOT_TOKEN"

//Insert Telegram Chat ID
#define CHAT_ID "YOUR_CHAT_UD"

// Define Firebase Data object
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

WiFiClientSecure client;
UniversalTelegramBot bot(BOT_TOKEN, client);

MFRC522 mfrc522(SDA_PIN, RST_PIN); // Create MFRC522 instance
Servo myServo; // Use ESP32Servo instead of Servo
RTC_DS3231 rtc;

unsigned long sendDataPrevMillis = 0;
bool signupOK = false;

const String lockerID = "Locker1"; // Example locker ID, you can modify this as needed

void setup() {
  Serial.begin(115200);
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT); //Add root certificate for api.telegram.org
  
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SDA_PIN); // Init SPI bus
  mfrc522.PCD_Init(); // Init MFRC522
  
  myServo.attach(SERVO_PIN); // Attach servo to GPIO 26 (adjust as needed)
  myServo.write(90); // Initial position (unlocked)
  
  pinMode(LED_PIN, OUTPUT); //Set LED as output
  digitalWrite(LED_PIN, HIGH); //Initially, LED is ON (unlocked)
  
  Serial.println("Scan your card to lock/unlock the locker...");

  // Initialize RTC
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC");
    while (1);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  bot.sendMessage(CHAT_ID, "Bot started up", "");

  // Assign the API key (required)
  config.api_key = API_KEY;

  // Assign the RTDB URL (required)
  config.database_url = DATABASE_URL;

  // Sign up
  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase Sign Up OK");
    signupOK = true;
  } else {
    Serial.printf("Firebase Sign Up Failed: %s\n", config.signer.signupError.message.c_str());
  }

  // Assign the callback function for the long running token generation task
  config.token_status_callback = tokenStatusCallback; // see addons/TokenHelper.h

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Check Firebase connection
  if (!Firebase.ready()) {
    Serial.println("Failed to connect to Firebase");
    Serial.println(fbdo.errorReason());
    while (true);
  }

  // Ensure the initial state in Firebase matches the initial state of the servo
  String lockStatusPath = "/lockers/" + lockerID + "/status";
  String currentUIDPath = "/lockers/" + lockerID + "/currentUID";
  if (Firebase.RTDB.getString(&fbdo, lockStatusPath)) {
    if (fbdo.stringData() == "locked") {
      // If Firebase says locked but servo is unlocked, correct it
      myServo.write(0); // Lock
      digitalWrite(LED_PIN, LOW); //Turn off LED (locked)
    } else {
      // Update Firebase to reflect the unlocked state
      Firebase.RTDB.setString(&fbdo, lockStatusPath, "unlocked");
      Firebase.RTDB.setString(&fbdo, currentUIDPath, "");
      digitalWrite(LED_PIN, HIGH); //Turn on LED (unlocked)
    }
  } else {
    // If status not found in Firebase, initialize it
    Firebase.RTDB.setString(&fbdo, lockStatusPath, "unlocked");
    Firebase.RTDB.setString(&fbdo, currentUIDPath, "");
    digitalWrite(LED_PIN, HIGH); //Turn on LED (unlocked)
  }
}

void loop() {
  // Look for new cards
  if (!mfrc522.PICC_IsNewCardPresent()) {
    return;
  }

  // Select one of the cards
  if (!mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  // Show UID on serial monitor
  Serial.print("UID tag: ");
  String uid = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
    Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
    Serial.print(mfrc522.uid.uidByte[i], HEX);
    uid += String(mfrc522.uid.uidByte[i], HEX);
  }
  Serial.println();

  if (signupOK) {
    // Check if the locker is currently locked
    String lockStatusPath = "/lockers/" + lockerID + "/status";
    String currentUIDPath = "/lockers/" + lockerID + "/currentUID";
    
    if (Firebase.RTDB.getString(&fbdo, lockStatusPath)) {
      String lockStatus = fbdo.stringData();
      if (lockStatus == "locked") {
        // Check if the current card is the one that locked the locker
        if (Firebase.RTDB.getString(&fbdo, currentUIDPath)) {
          String currentUID = fbdo.stringData();
          if (currentUID == uid) {
            // Unlock the locker
            Serial.println("Unlocking the locker");
            myServo.write(90); // Unlock
            digitalWrite(LED_PIN, HIGH); //Turn on LED (unlocked)
            delay(5000); // Keep unlocked for 5 seconds
            myServo.write(90); // Stay unlocked

            // Update Firebase to reflect the unlocked state
            Firebase.RTDB.setString(&fbdo, lockStatusPath, "unlocked");
            Firebase.RTDB.setString(&fbdo, currentUIDPath, "");

            // Log the unlock event
            String logPath = "/access_logs/" + String(millis());
            DateTime now = rtc.now();
            String timestamp = String(now.year()) + "/" + String(now.month()) + "/" + String(now.day()) + " " + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
            Firebase.RTDB.setString(&fbdo, logPath + "/UID", uid);
            Firebase.RTDB.setString(&fbdo, logPath + "/lockerID", lockerID);
            Firebase.RTDB.setString(&fbdo, logPath + "/action", "unlock");
            Firebase.RTDB.setString(&fbdo, logPath + "/time", timestamp);
            Serial.print(timestamp);

            //Send Telegram message
            bot.sendMessage(CHAT_ID, lockerID + " unlocked\nUID: " + uid, "");
          } else {
            Serial.println("Access denied: Locker is locked by another card");
            bot.sendMessage(CHAT_ID, lockerID + " attempted to be opened by another card\nUID: " + uid, "");
          }
        }
      } else {
        // Lock the locker
        Serial.println("Locking the locker");
        myServo.write(0); // Lock
        digitalWrite(LED_PIN, LOW); //Turn off LED (locked)
        delay(1000);

        // Update Firebase to reflect the locked state
        Firebase.RTDB.setString(&fbdo, lockStatusPath, "locked");
        Firebase.RTDB.setString(&fbdo, currentUIDPath, uid);

        // Log the lock event
        String logPath = "/access_logs/" + String(millis());
        DateTime now = rtc.now();
        String timestamp = String(now.year()) + "/" + String(now.month()) + "/" + String(now.day()) + " " + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
        Firebase.RTDB.setString(&fbdo, logPath + "/UID", uid);        
        Firebase.RTDB.setString(&fbdo, logPath + "/lockerID", lockerID);
        Firebase.RTDB.setString(&fbdo, logPath + "/action", "lock");
        Firebase.RTDB.setString(&fbdo, logPath + "/time", timestamp);
        Serial.print(timestamp);

        //Send Telegram Message
         bot.sendMessage(CHAT_ID, lockerID + " locked\nUID: " + uid, "");
      }
    } else {
      Serial.println("Failed to get lock status from Firebase");
      Serial.println(fbdo.errorReason());
    }
  }

  delay(1000); // Add a delay to avoid multiple registrations for the same scan
}
