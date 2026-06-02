#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ESP32-S3 N16R8 Slave Module
// Slave receives the master GPI state mask over ESP-NOW and mirrors it on 4 GPO outputs.
//
// GPIO pinout examples (change pins as needed for your module):
//   OLED SDA -> GPIO 8
//   OLED SCL -> GPIO 9
//   GPO1      -> GPIO 4
//   GPO2      -> GPIO 5
//   GPO3      -> GPIO 13
//   GPO4      -> GPIO 14
//
// If your outputs are active-low relays or LEDs, set GPO_ACTIVE_LOW to true.

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int NUM_CHANNELS = 4;
const int SLAVE_GPO_PINS[NUM_CHANNELS] = {4, 5, 13, 14};
const bool GPO_ACTIVE_LOW = false;

const int OLED_SDA = 8;
const int OLED_SCL = 9;
const uint8_t OLED_ADDRESS = 0x3C;

// Replace this with the master module MAC address in hex form.
uint8_t masterMacAddress[6] = {0xE0, 0x72, 0xA1, 0xD5, 0x2A, 0xF0};

struct GpiMessage {
  uint8_t mask;
  uint32_t counter;
  uint32_t timestamp;
};

volatile uint8_t receivedMask = 0;
volatile uint32_t lastCounter = 0;
volatile bool hasNewPacket = false;
volatile unsigned long lastPacketTime = 0;

const unsigned long MASTER_TIMEOUT_MS = 10000;

void getMacString(char *buffer, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buffer, len, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("FOX 1440 STAGE B CAMS");
  display.println("Waiting for master...");
  
  char macStr[18];
  getMacString(macStr, sizeof(macStr));
  display.setCursor(0, 56);
  display.println(macStr);
  display.display();
}

void setOutputs(uint8_t mask) {
  for (int i = 0; i < NUM_CHANNELS; i++) {
    bool on = mask & (1 << i);
    if (GPO_ACTIVE_LOW) {
      digitalWrite(SLAVE_GPO_PINS[i], on ? LOW : HIGH);
    } else {
      digitalWrite(SLAVE_GPO_PINS[i], on ? HIGH : LOW);
    }
  }
}

void formatMask(uint8_t mask, char *buffer, size_t len) {
  snprintf(buffer, len, "%c %c %c %c",
           (mask & 0x01) ? '1' : '0',
           (mask & 0x02) ? '1' : '0',
           (mask & 0x04) ? '1' : '0',
           (mask & 0x08) ? '1' : '0');
}

void updateDisplay(uint8_t mask, const char *note) {
  char bits[16];
  formatMask(mask, bits, sizeof(bits));

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("FOX 1440 STAGE B CAMS");
  display.println();
  display.print("Outputs: ");
  display.println(bits);
  display.print("Note: ");
  display.println(note);
  
  char macStr[18];
  getMacString(macStr, sizeof(macStr));
  display.setCursor(0, 56);
  display.println(macStr);
  display.display();
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len != sizeof(GpiMessage)) return;
  GpiMessage packet;
  memcpy(&packet, incomingData, sizeof(packet));

  if (packet.counter == lastCounter) {
    return; // duplicate packet
  }

  lastCounter = packet.counter;
  receivedMask = packet.mask;
  hasNewPacket = true;  lastPacketTime = millis();}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(SLAVE_GPO_PINS[i], OUTPUT);
    if (GPO_ACTIVE_LOW) {
      digitalWrite(SLAVE_GPO_PINS[i], HIGH);
    } else {
      digitalWrite(SLAVE_GPO_PINS[i], LOW);
    }
  }

  initOLED();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    updateDisplay(0, "ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, masterMacAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add master peer");
    updateDisplay(0, "Peer add failed");
  }
  
  lastPacketTime = millis();
}

void loop() {
  unsigned long now = millis();
  bool masterOnline = (now - lastPacketTime) < MASTER_TIMEOUT_MS;
  
  if (hasNewPacket) {
    noInterrupts();
    uint8_t mask = receivedMask;
    hasNewPacket = false;
    interrupts();

    setOutputs(mask);
    updateDisplay(mask, masterOnline ? "RECEIVED" : "OFFLINE");
    Serial.printf("Mirror mask=0x%02X Master=%s\n", mask, masterOnline ? "ONLINE" : "OFFLINE");
  } else if (!masterOnline && lastPacketTime > 0) {
    static unsigned long lastCheck = 0;
    if (now - lastCheck > 1000) {
      updateDisplay(receivedMask, "OFFLINE");
      Serial.println("Master timeout detected");
      lastCheck = now;
    }
  }

  delay(50);
}
