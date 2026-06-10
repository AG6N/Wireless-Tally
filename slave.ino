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
const int SLAVE_GPO_PINS[NUM_CHANNELS] = {1, 2, 3, 4};
const bool GPO_ACTIVE_LOW = false;
const int OLED_SDA = 12;
const int OLED_SCL = 13;
const uint8_t OLED_ADDRESS = 0x3C;

uint8_t masterMacAddress[6] = {0xE0, 0x72, 0xA1, 0xD5, 0x2A, 0xF0};

struct GpiMessage {
  uint8_t mask;
  uint32_t counter;
};

volatile uint8_t receivedMask = 0;
volatile uint32_t lastCounter = 0;
volatile bool newPacket = false;
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
    Serial.println("SSD1306 init failed");
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
    digitalWrite(SLAVE_GPO_PINS[i], GPO_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
  }
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
  if (len != sizeof(GpiMessage)) {
    return;
  }

  if (memcmp(info->src_addr, masterMacAddress, 6) != 0) {
    return;
  }

  GpiMessage packet;
  memcpy(&packet, incomingData, sizeof(packet));

  if (packet.counter == lastCounter) {
    return;
  }

  lastCounter = packet.counter;
  receivedMask = packet.mask;
  newPacket = true;
  lastPacketTime = millis();
}

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < NUM_CHANNELS; i++) {
    pinMode(SLAVE_GPO_PINS[i], OUTPUT);
    digitalWrite(SLAVE_GPO_PINS[i], GPO_ACTIVE_LOW ? HIGH : LOW);
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
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, masterMacAddress, 6);
  peer.ifidx = WIFI_IF_STA;
  peer.channel = 1;
  peer.encrypt = false;
  esp_err_t err = esp_now_add_peer(&peer);
  Serial.printf("Add master peer result=%d\n", err);

  lastPacketTime = millis();
}

void loop() {
  unsigned long now = millis();

  if (newPacket) {
    noInterrupts();
    uint8_t mask = receivedMask;
    newPacket = false;
    interrupts();

    setOutputs(mask);
    showStatus(mask, "RECEIVED");
    Serial.printf("Mask=0x%02X\n", mask);
  } else if (now - lastPacketTime > MASTER_TIMEOUT_MS) {
    showStatus(receivedMask, "OFFLINE");
  }

  delay(50);
}
