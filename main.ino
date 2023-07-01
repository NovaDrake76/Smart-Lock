#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Keypad.h>
#include <FS.h>
#include <SPIFFS.h>

#define ROW_NUM     4 // four rows
#define COLUMN_NUM  4 // four columns
#define IO_USERNAME  ""*******""
#define IO_KEY       "*******"
#define SS_PIN 21
#define RST_PIN 22
#define RELAY 4
#define BUZZER 5

char keys[ROW_NUM][COLUMN_NUM] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte pin_rows[ROW_NUM]      = {12, 14, 27, 26};
byte pin_column[COLUMN_NUM] = {25, 33, 32, 35};

Keypad keypad = Keypad( makeKeymap(keys), pin_rows, pin_column, ROW_NUM, COLUMN_NUM );
String keyboardPassword = "7890"; // This password will be changed by reading from SPIFFS
String input_password;

MFRC522 rfid(SS_PIN, RST_PIN);
bool programmingMode = false; // This mode allows you to add new RFID cards
unsigned long programmingModeStartTime;

const char* ssid = "imd0902";
const char* password = "imd0902iot";
const char* mqttServer = "io.adafruit.com";
const int mqttPort = 1883;
const char* mqttUser = ""*******"";
const char* mqttPassword = ""*******"";
const int TONE_PWM_CHANNEL = 0;

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastMsg = 0;
#define MSG_BUFFER_SIZE  (50)
char msg[MSG_BUFFER_SIZE];
int value = 0;

void setup_wifi() {
  delay(10);
  SPI.begin();
  rfid.PCD_Init();
  pinMode(RELAY, OUTPUT);

  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  randomSeed(micros());

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if ((char)payload[0] == '1') {
    digitalWrite(RELAY, LOW);  
  } else {
    digitalWrite(RELAY, HIGH);  
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");

    String clientId = "ESP32Client-";
    clientId += String(random(0xffff), HEX);

    if (client.connect(clientId.c_str(), mqttUser, mqttPassword)) {
      Serial.println("connected");
      client.subscribe("davivcl/feeds/led");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  pinMode(2, OUTPUT);     
  pinMode(BUZZER, OUTPUT);     
  pinMode(RELAY, INPUT);
  digitalWrite(RELAY, HIGH);

  Serial.begin(115200);
  setup_wifi();
  client.setServer(mqttServer, 1883);
  client.setCallback(callback);
  input_password.reserve(32);
  ledcAttachPin(BUZZER, TONE_PWM_CHANNEL);

  // initialize SPIFFS
  if (!SPIFFS.begin()) {
    Serial.println("SPIFFS initialisation failed!");
    while (1) yield();
  }
  else {
    Serial.println("SPIFFS initialised.");
  }

  // Read password from SPIFFS
  File passwordFile = SPIFFS.open("/password.txt");
  if (!passwordFile) {
    Serial.println("Failed to open password file");
  } else {
    keyboardPassword = passwordFile.readString();
    Serial.println("Password read from file:");
    Serial.println(keyboardPassword);
  }
  passwordFile.close();
}

void unlockDoor() {
  client.publish("davivcl/feeds/led", "1");
  digitalWrite(RELAY, LOW);
  beepUnlock();
  delay(5000); // Keep the door unlocked for 5 seconds
  client.publish("davivcl/feeds/led", "0");
}

void beep() {
  ledcWriteTone(TONE_PWM_CHANNEL, 100);
  delay(50);
  ledcWriteTone(TONE_PWM_CHANNEL, 0);
  delay(50);
}

void beepUnlock() {
  ledcWriteNote(TONE_PWM_CHANNEL, NOTE_B, 4);
  delay(100);
  ledcWriteNote(TONE_PWM_CHANNEL, NOTE_D, 2);
  delay(100);
  ledcWriteTone(TONE_PWM_CHANNEL, 0);
}

void beepError() {
  ledcWriteTone(TONE_PWM_CHANNEL, 800);
  delay(100);
  ledcWriteTone(TONE_PWM_CHANNEL, 800);
  delay(50);
  ledcWriteTone(TONE_PWM_CHANNEL, 0);
}

void getPassword(char key) {
  if (key) {
    Serial.println(key);
    beep();

    if (key == '*') {
      input_password = "";
    } else if (key == '#') {
      if (keyboardPassword == input_password) {
        Serial.println("The password is correct, ACCESS GRANTED!");
        
        unlockDoor();
        
        programmingMode = true;
        programmingModeStartTime = millis();
        Serial.println("Entering programming mode. Please present a new RFID card.");
      } else {
        Serial.println("The password is incorrect, ACCESS DENIED!");
        beepError();
      }

      input_password = "";
    } else {
      input_password += key;
    }
  }
}

void loop() {
  if (programmingMode && millis() - programmingModeStartTime > 10000) {
    programmingMode = false;
    Serial.println("Programming mode timed out.");
  }

  char key = keypad.getKey();

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  getPassword(key);

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    char scannedCard[9];
    for (byte i = 0; i < rfid.uid.size; i++) {
      sprintf(&scannedCard[i * 2], "%02X", rfid.uid.uidByte[i]);
    }
    Serial.print("Scanned card UID: ");
    Serial.println(scannedCard);

    if (programmingMode) {
      Serial.println("Programming mode active. Writing card UID to memory.");

      // Open the card ID file for writing
      File cardFile = SPIFFS.open("/card.txt", "w");
      if (!cardFile) {
        Serial.println("Failed to open card file for writing");
      } else {
        cardFile.println(scannedCard);
        Serial.println("Card UID written to file:");
        Serial.println(scannedCard);
      }
      cardFile.close();
      programmingMode = false;
      Serial.println("Exiting programming mode.");
    } else {
      // Try to read the card ID from the file
      File cardFile = SPIFFS.open("/card.txt", "r");
      if (!cardFile) {
        Serial.println("Failed to open card file for reading");
      } else {
        String cardId = cardFile.readString();
        Serial.println("Card ID read from file:");
        Serial.println(cardId);

        if (cardId.indexOf(scannedCard) >= 0) {
          unlockDoor();
        } else {
          Serial.println("Invalid Card");
          beepError();
          delay(500);
        }
      }
      cardFile.close();
    }
  }
}
