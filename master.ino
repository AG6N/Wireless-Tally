#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ESP32-S3 N16R8 Master Module
// Master reads 4 GPI inputs and sends the 4-bit state to 4 slave modules over ESP-NOW.
// Each slave mirrors the state onto 4 GPO outputs.
//
// GPIO pinout examples (change pins as needed for your module):
//   OLED SDA -> GPIO 8
//   OLED SCL -> GPIO 9
//   GPI1      -> GPIO 4
//   GPI2      -> GPIO 5
//   GPI3      -> GPIO 13
//   GPI4      -> GPIO 14
//
// If your inputs are momentary pushbuttons, use INPUT_PULLUP and active-low wiring.
// If you wire sensors that are active-high, set GPI_ACTIVE_LOW to false.

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int NUM_CHANNELS = 4;
const int MASTER_GPI_PINS[NUM_CHANNELS] = {4, 5, 13, 14};
const bool GPI_ACTIVE_LOW = true;   // true for INPUT_PULLUP + active-low buttons

const int OLED_SDA = 8;
const int OLED_SCL = 9;
const uint8_t OLED_ADDRESS = 0x3C;

// Replace these with the actual MAC addresses of your 4 slave modules.
uint8_t slaveMacs[4][6] = {
  {0xE0, 0x72, 0xA1, 0xD6, 0x79, 0x34},
  {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x02},
  {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x03},
  {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0x04}
};

struct GpiMessage {
  uint8_t mask;       // bit0..bit3 correspond to channels 1..4
  uint32_t counter;   // packet counter for duplicate suppression
  uint32_t timestamp; // millis() at send time
};

GpiMessage message = {0, 0, 0};
uint8_t lastMask = 0;
unsigned long lastTransmit = 0;
const unsigned long PERIODIC_SEND_MS = 10000;

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
  display.println("FOX 1440 STAGE B TX");
  display.println("Waiting for changes...");
  
  char macStr[18];
  getMacString(macStr, sizeof(macStr));
  display.setCursor(0, 56);
  display.println(macStr);
  display.display();
}

uint8_t readInputs() {
  uint8_t mask = 0;
  for (int i = 0; i < NUM_CHANNELS; i++) {
    bool raw = digitalRead(MASTER_GPI_PINS[i]);
    bool active = GPI_ACTIVE_LOW ? !raw : raw;
    if (active) {
      mask |= (1 << i);
    }
  }
  return mask;
}

void formatMask(uint8_t mask, char *buffer, size_t len) {
  snprintf(buffer, len, "%c %c %c %c",
           (mask & 0x01) ? '1' : '0',
           (mask & 0x02) ? '1' : '0',
           (mask & 0x04) ? '1' : '0',
           (mask & 0x08) ? '1' : '0');
}

void updateDisplay(uint8_t mask, const char *note, const char *sendStatus = "") {
  char bits[16];
  formatMask(mask, bits, sizeof(bits));

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("FOX 1440 STAGE B TX");
  display.println();
  display.print("Inputs: ");
  display.println(bits);
  display.print("Note: ");
  display.println(note);
  if (sendStatus[0] != '\0') {
    display.print("Send: ");
    display.println(sendStatus);
  }
  
  char macStr[18];
  getMacString(macStr, sizeof(macStr));
  display.setCursor(0, 56);
  display.println(macStr);
  display.display();
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("Sent status=%s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void sendMaskToAll(uint8_t mask) {
  message.mask = mask;
  message.counter++;
  message.timestamp = millis();

  for (int i = 0; i < 4; i++) {
    esp_err_t result = esp_now_send(slaveMacs[i], (uint8_t *)&message, sizeof(message));
    if (result != ESP_OK) {
      Serial.printf("Send fail idx=%d err=%d\n", i, result);
    }
  }
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(MASTER_GPI_PINS[i], GPI_ACTIVE_LOW ? INPUT_PULLUP : INPUT);
  }

  initOLED();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    updateDisplay(0, "ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onDataSent);

  for (int i = 0; i < 4; i++) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, slaveMacs[i], 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.printf("Failed to add peer %d\n", i);
      updateDisplay(0, "Peer add failed");
    }
  }

  lastMask = readInputs();
  sendMaskToAll(lastMask);
  updateDisplay(lastMask, "Sent initial", "OK");
  lastTransmit = millis();
}

void loop() {
  static uint8_t stableMask = lastMask;
  uint8_t currentMask = readInputs();

  if (currentMask != stableMask) {
    stableMask = currentMask;
    lastMask = currentMask;
    sendMaskToAll(currentMask);
    updateDisplay(currentMask, "CHANGED", "OK");
    Serial.printf("Changed to %02X\n", currentMask);
    lastTransmit = millis();
  } else {
    unsigned long now = millis();
    if (now - lastTransmit >= PERIODIC_SEND_MS) {
      sendMaskToAll(currentMask);
      updateDisplay(currentMask, "PERIODIC", "OK");
      lastTransmit = now;
    }
  }

  delay(50);
}
