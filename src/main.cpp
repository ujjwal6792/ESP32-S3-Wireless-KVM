#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// --- BLE Dependencies ---
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>

// --- USB Host Dependencies ---
#include "hid_host.h"
#include "usb/usb_host.h"

// ================= CONFIGURATION =================
#define NUM_SLOTS 4
#define LED_PIN 48
#define LED_BRIGHTNESS 255

// POWER SAVING
#define IDLE_TIME_ECO_MS 10000
#define IDLE_TIME_SLEEP_MS 1800000

// Key Codes
#define KEY_MOD_LSHIFT 0x02
#define KEY_MOD_RSHIFT 0x20
#define KEY_INSERT 0x49
#define KEY_1 0x1E
#define KEY_2 0x1F
#define KEY_3 0x20
#define KEY_4 0x21
#define KEY_0 0x27

// ================= GLOBALS =================
// ---> FIX: Changed to NEO_GRBW to support your SK6812 4-Channel LED
Adafruit_NeoPixel pixels(1, LED_PIN, NEO_GRBW + NEO_KHZ800);

enum ConnectionState {
  STATE_DISCONNECTED_RECONNECTING,
  STATE_DISCONNECTED_PAIRING,
  STATE_CONNECTED
};

RTC_DATA_ATTR int storedSlot = 0;

volatile ConnectionState appState = STATE_DISCONNECTED_RECONNECTING;
volatile int currentSlot = 0;
volatile bool isSwitching = false;
volatile bool isConnected = false;

unsigned long lastKeyTime = 0;
bool isEcoMode = false;

// Your Custom Catppuccin Hex Colors
uint32_t slotColors[4] = {0x04A5E5, 0xFE640B, 0xD20F39, 0x40A02B};
uint8_t baseMac[6];

NimBLEServer *pServer = nullptr;
NimBLEHIDDevice *pHidDev = nullptr;
NimBLECharacteristic *pInputChar = nullptr;
NimBLECharacteristic *pConsumerChar = nullptr;
QueueHandle_t hidQueue = nullptr;

typedef struct {
  uint8_t *rawData;
  size_t len;
} hid_event_t;

// --- 1-TO-1 UNIVERSAL HID DESCRIPTOR ---
const uint8_t hidReportMap[] = {
    // 1. Standard Keyboard (ID 1)
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0,
    0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08,
    0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00,
    0x29, 0x65, 0x81, 0x00, 0xC0,

    // 2. Consumer Control / Media (ID 2)
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x02,       //   Report ID (2)
    0x19, 0x00,       //   Usage Minimum (0)
    0x2A, 0xFF, 0x03, //   Usage Maximum (1023)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x03, //   Logical Maximum (1023)
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x10,       //   Report Size (16 bits)
    0x81, 0x00,       //   Input (Data, Array, Absolute)
    0xC0              // End Collection
};

// ================= FORWARD DECLARATIONS =================
void startBLE(int slot);
void stopBLE();
void updateLED();
void processHID(hid_event_t *evt);
void handleSlotSwitch(int newSlot, bool pairingMode);
void handleFactoryReset();
void checkPowerManagement();

// ================= CALLBACKS =================
class MySecurityCallbacks : public NimBLESecurityCallbacks {
  bool onConfirmPIN(uint32_t pin) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    if (!desc->sec_state.encrypted) {
      pServer->disconnect(desc->conn_handle);
    }
  }
  uint32_t onPassKeyRequest() override { return 123456; }
  void onPassKeyNotify(uint32_t pass_key) override {}
};

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
    isConnected = true;
    appState = STATE_CONNECTED;
    updateLED();
    lastKeyTime = millis();
    isEcoMode = false;
  }

  void onDisconnect(NimBLEServer *pServer) override {
    isConnected = false;
    if (isSwitching)
      return;
    if (appState == STATE_CONNECTED)
      appState = STATE_DISCONNECTED_RECONNECTING;
    pServer->getAdvertising()->start();
    updateLED();
  }
};

// ================= USB CALLBACKS =================
void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                 const hid_host_interface_event_t event,
                                 void *arg) {
  if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
    const size_t buffer_size = 64;
    uint8_t *data_buffer = (uint8_t *)malloc(buffer_size);
    size_t data_len = 0;

    if (data_buffer) {
      esp_err_t err = hid_host_device_get_raw_input_report_data(
          hid_device_handle, data_buffer, buffer_size, &data_len);

      if (err == ESP_OK && data_len > 0) {
        hid_event_t evt;
        evt.len = data_len;
        evt.rawData = data_buffer;

        if (hidQueue != NULL) {
          if (xQueueSend(hidQueue, &evt, 0) != pdTRUE) {
            free(data_buffer);
          }
        } else {
          free(data_buffer);
        }
      } else {
        free(data_buffer);
      }
    }
  } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
    hid_host_device_close(hid_device_handle);
  }
}

void hid_host_driver_callback(hid_host_device_handle_t hid_device_handle,
                              const hid_host_driver_event_t event, void *arg) {
  if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
    const hid_host_device_config_t dev_config = {
        .callback = hid_host_interface_callback, .callback_arg = NULL};
    hid_host_device_open(hid_device_handle, &dev_config);
    hid_host_device_start(hid_device_handle);
  }
}

// ================= LOGIC =================

void updateLED() {
  if (appState == STATE_CONNECTED) {
    if (isEcoMode) {
      // Eco Mode: Dim Rosewater (RGB: 24, 22, 22). White channel explicitly 0.
      pixels.setPixelColor(0, pixels.Color(129, 200, 190, 20));
    } else {
      // Decodes Hex & Forces the blinding White LED to remain OFF
      uint32_t hex = slotColors[currentSlot];
      uint8_t r = (hex >> 16) & 0xFF;
      uint8_t g = (hex >> 8) & 0xFF;
      uint8_t b_val = hex & 0xFF;
      pixels.setPixelColor(0, pixels.Color(r, g, b_val, 0));
    }
  } else {
    pixels.setPixelColor(0, 0);
  }
  pixels.show();
}

void startBLE(int slot) {
  isSwitching = false;
  currentSlot = slot;
  storedSlot = slot;

  char name[20];
  sprintf(name, "ESP-Slot-%d", slot + 1);

  uint8_t newMac[6];
  memcpy(newMac, baseMac, 6);
  newMac[5] = 0x40 + slot;
  esp_base_mac_addr_set(newMac);

  NimBLEDevice::init(name);

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  pHidDev = new NimBLEHIDDevice(pServer);
  pHidDev->reportMap((uint8_t *)hidReportMap, sizeof(hidReportMap));

  pInputChar = pHidDev->inputReport(1);
  pConsumerChar = pHidDev->inputReport(2);

  pHidDev->manufacturer()->setValue("ESP-Custom");
  pHidDev->pnp(0x02, 0xe502, 0xa111, 0x0211);
  pHidDev->hidInfo(0x00, 0x01);

  NimBLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->setAppearance(HID_KEYBOARD);
  pAdvertising->setName(name);
  pAdvertising->addServiceUUID(pHidDev->hidService()->getUUID());
  pAdvertising->setScanResponse(true);

  pAdvertising->setMinInterval(32);
  pAdvertising->setMaxInterval(48);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMaxPreferred(0x0C);

  pHidDev->startServices();
  pAdvertising->start();
  lastKeyTime = millis();
}

void stopBLE() {
  isSwitching = true;
  if (pServer) {
    if (pServer->getConnectedCount() > 0) {
      pServer->disconnect(0);
      unsigned long start = millis();
      while (isConnected && millis() - start < 500)
        delay(10);
    }
    if (pServer->getAdvertising()->isAdvertising())
      pServer->getAdvertising()->stop();
    NimBLEDevice::deinit(true);
    pServer = nullptr;
    pHidDev = nullptr;
    pInputChar = nullptr;
    isConnected = false;
  }
}

void handleSlotSwitch(int newSlot, bool pairingMode) {
  if (newSlot == currentSlot &&
      pairingMode == (appState == STATE_DISCONNECTED_PAIRING))
    return;
  stopBLE();
  delay(600);
  currentSlot = newSlot;
  appState = pairingMode ? STATE_DISCONNECTED_PAIRING
                         : STATE_DISCONNECTED_RECONNECTING;
  startBLE(currentSlot);
}

void handleFactoryReset() {
  stopBLE();
  for (int i = 0; i < 5; i++) {
    // Red visual reset indicator
    pixels.setPixelColor(0, pixels.Color(255, 0, 0, 0));
    pixels.show();
    delay(100);
    pixels.setPixelColor(0, 0);
    pixels.show();
    delay(100);
  }
  NimBLEDevice::init("");
  NimBLEDevice::deleteAllBonds();
  ESP.restart();
}

void enterDeepSleep() {
  stopBLE();
  pixels.clear();
  pixels.show();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  esp_deep_sleep_start();
}

void processHID(hid_event_t *evt) {
  lastKeyTime = millis();
  if (isEcoMode && isConnected) {
    // We disabled updateConnParams here to fix Error 2 spam.
    // The OS handles wakeup latency automatically.
    isEcoMode = false;
    updateLED();
  }

  // --- STANDARD KEYBOARD (ID 1) ---
  if (evt->len == 8) {
    uint8_t mod = evt->rawData[0];
    bool isShift = (mod & KEY_MOD_LSHIFT) || (mod & KEY_MOD_RSHIFT);
    bool isInsert = false;
    int numKey = -1;

    for (int i = 2; i < 8; i++) {
      if (evt->rawData[i] == KEY_INSERT)
        isInsert = true;
      if (evt->rawData[i] == KEY_1)
        numKey = 0;
      if (evt->rawData[i] == KEY_2)
        numKey = 1;
      if (evt->rawData[i] == KEY_3)
        numKey = 2;
      if (evt->rawData[i] == KEY_4)
        numKey = 3;
      if (evt->rawData[i] == KEY_0)
        numKey = 99;
    }

    if (isInsert && numKey != -1) {
      if (isShift && numKey == 99) {
        free(evt->rawData);
        handleFactoryReset();
        return;
      }
      if (isShift && numKey <= 3)
        handleSlotSwitch(numKey, true);
      else if (numKey <= 3)
        handleSlotSwitch(numKey, false);
      free(evt->rawData);
      return;
    }

    if (appState == STATE_CONNECTED && pInputChar) {
      pInputChar->setValue(evt->rawData, 8);
      pInputChar->notify();
    }
  }

  // --- MEDIA KEYS & VOLUME KNOB ---
  else if (evt->len >= 3 && evt->rawData[0] == 0x03) {
    if (appState == STATE_CONNECTED && pConsumerChar) {
      uint8_t mediaReport[2];
      mediaReport[0] = evt->rawData[1];
      mediaReport[1] = evt->rawData[2];

      pConsumerChar->setValue(mediaReport, 2);
      pConsumerChar->notify();
    }
  }

  free(evt->rawData);
}

void checkPowerManagement() {
  unsigned long now = millis();
  if (now - lastKeyTime > IDLE_TIME_SLEEP_MS)
    enterDeepSleep();

  if (isConnected && !isEcoMode && (now - lastKeyTime > IDLE_TIME_ECO_MS)) {
    // Disabled updateConnParams to fix Error 2 spam
    isEcoMode = true;
    updateLED();
  }
}

// ================= TASKS =================
void usb_lib_task(void *arg) {
  while (1)
    usb_host_lib_handle_events(portMAX_DELAY, NULL);
}
void hid_host_task(void *arg) {
  const usb_host_config_t host_config = {.skip_phy_setup = false,
                                         .intr_flags = ESP_INTR_FLAG_LEVEL1};
  usb_host_install(&host_config);
  xTaskCreate(usb_lib_task, "usb_events", 4096, NULL, 2, NULL);
  const hid_host_driver_config_t hid_config = {.create_background_task = true,
                                               .task_priority = 5,
                                               .stack_size = 4096,
                                               .core_id = 0,
                                               .callback =
                                                   hid_host_driver_callback,
                                               .callback_arg = NULL};
  hid_host_install(&hid_config);
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  pixels.begin();
  pixels.setBrightness(LED_BRIGHTNESS);
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0)
    currentSlot = storedSlot;
  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);

  hidQueue = xQueueCreate(20, sizeof(hid_event_t));
  xTaskCreate(hid_host_task, "hid_task", 4096, NULL, 2, NULL);
  startBLE(currentSlot);
}

void loop() {
  hid_event_t evt;
  int drainCount = 0;

  while (hidQueue != NULL && xQueueReceive(hidQueue, &evt, 0) &&
         drainCount < 10) {
    processHID(&evt);
    drainCount++;
  }

  checkPowerManagement();

  static unsigned long lastUpdate = 0;
  unsigned long now = millis();

  if (appState == STATE_CONNECTED) {
    if (now - lastUpdate > 1000)
      lastUpdate = now;
  } else if (appState == STATE_DISCONNECTED_PAIRING) {
    int b = (now % 2000) > 1000 ? 2000 - (now % 2000) : (now % 2000);
    b = map(b, 0, 1000, 0, 255);

    uint32_t hex = slotColors[currentSlot];
    uint8_t r = ((hex >> 16) & 0xFF) * b / 255;
    uint8_t g = ((hex >> 8) & 0xFF) * b / 255;
    uint8_t b_val = (hex & 0xFF) * b / 255;

    pixels.setPixelColor(0, pixels.Color(r, g, b_val, 0));
    pixels.show();
  } else if (appState == STATE_DISCONNECTED_RECONNECTING) {
    long cycle = now % 2000;
    bool on = (cycle < 100) || (cycle > 200 && cycle < 300) ||
              (cycle > 400 && cycle < 500);
    if (on) {
      uint32_t hex = slotColors[currentSlot];
      uint8_t r = (hex >> 16) & 0xFF;
      uint8_t g = (hex >> 8) & 0xFF;
      uint8_t b_val = hex & 0xFF;
      pixels.setPixelColor(0, pixels.Color(r, g, b_val, 0));
    } else {
      pixels.setPixelColor(0, 0);
    }
    pixels.show();
  }
}
