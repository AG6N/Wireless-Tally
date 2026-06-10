#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int NUM_CHANNELS = 4;
const int MASTER_GPI_PINS[NUM_CHANNELS] = {4, 5, 13, 14};
const bool GPI_ACTIVE_LOW = true;
const int OLED_SDA = 8;
const int OLED_SCL = 9;
const uint8_t OLED_ADDRESS = 0x3C;

uint8_t slaveMacs[4][6] = {
  {0xA0, 0xF2, 0x62, 0xF5, 0x60, 0x8C},
  {0xA0, 0xF2, 0x62, 0xF4, 0x28, 0xB0},
  {0xA0, 0xF2, 0x62, 0xF4, 0x67, 0xE4},
  {0xA0, 0xF2, 0x62, 0xEE, 0xDC, 0x44}
};

struct GpiMessage {
  uint8_t mask;
  uint32_t counter;
};

GpiMessage message = {0, 0};
uint8_t lastMask = 0;
unsigned long lastSendTime = 0;
const unsigned long HEARTBEAT_MS = 2000;

void getMacString(char *buffer, size_t len) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(buffer, len, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void initOLED() {
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("SSD1306 init failed");
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("FOX 1440 STAGE B TX");
  display.println("Waiting for input...");

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

void showStatus(uint8_t mask, const char *note) {
  char bits[16];
  snprintf(bits, sizeof(bits), "%c %c %c %c",
           (mask & 0x01) ? '1' : '0',
           (mask & 0x02) ? '1' : '0',
           (mask & 0x04) ? '1' : '0',
           (mask & 0x08) ? '1' : '0');

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("FOX 1440 STAGE B TX");
  display.println();
  display.print("Inputs: ");
  display.println(bits);
  display.print("Note: ");
  display.println(note);

  char macStr[18];
  getMacString(macStr, sizeof(macStr));
  display.setCursor(0, 56);
  display.println(macStr);
  display.display();
}

void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.printf("ESP-NOW send %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void sendMaskToSlaves(uint8_t mask) {
  message.mask = mask;
  message.counter++;
  for (int i = 0; i < 4; i++) {
    esp_err_t err = esp_now_send(slaveMacs[i], (uint8_t *)&message, sizeof(message));
    if (err != ESP_OK) {
      Serial.printf("Send to slave %d failed %d\n", i, err);
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
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    showStatus(0, "ESP-NOW failed");
    return;
  }
  esp_now_register_send_cb(onDataSent);

  for (int i = 0; i < 4; i++) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, slaveMacs[i], 6);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 1;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    Serial.printf("Add peer %d result=%d\n", i, err);
  }

  lastMask = readInputs();
  sendMaskToSlaves(lastMask);
  lastSendTime = millis();
  showStatus(lastMask, "INITIAL");
}

void loop() {
  uint8_t currentMask = readInputs();
  unsigned long now = millis();

  if (currentMask != lastMask) {
    lastMask = currentMask;
    sendMaskToSlaves(currentMask);
    lastSendTime = now;
    showStatus(currentMask, "CHANGED");
    Serial.printf("Mask=%02X\n", currentMask);
  } else if (now - lastSendTime >= HEARTBEAT_MS) {
    sendMaskToSlaves(currentMask);
    lastSendTime = now;
    Serial.println("Heartbeat sent");
  }

  delay(50);
}
