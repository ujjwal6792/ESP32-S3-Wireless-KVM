#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "oled_ssd1306.h"
#include "tft_display.h"

// --- BLE Dependencies ---
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#if defined(CONFIG_NIMBLE_CPP_IDF)
#include "host/ble_hs_id.h"
#else
#include "nimble/nimble/host/include/host/ble_hs_id.h"
#endif

// --- USB Host Dependencies ---
#include "hid_host.h"
#include "usb/usb_host.h"

// ================= CONFIGURATION =================
#define NUM_SLOTS 4
#define LED_PIN 48
#define LED_BRIGHTNESS 1

// OLED SSD1306 (I2C)
// Override at build time via platformio.ini build_flags:
// -DOLED_SDA_PIN=8 -DOLED_SCL_PIN=18
#ifndef OLED_SDA_PIN
#define OLED_SDA_PIN 8
#endif
#ifndef OLED_SCL_PIN
#define OLED_SCL_PIN 18
#endif

// TFT 320x240 SPI
// Current runtime driver is ST7789 using the working wiring:
// CS=10, DC=14, RST=15, SCK=12, MOSI=11, MISO=13, BL=21.
#ifndef TFT_CS_PIN
#define TFT_CS_PIN 10
#endif
#ifndef TFT_DC_PIN
#define TFT_DC_PIN 14
#endif
#ifndef TFT_RST_PIN
#define TFT_RST_PIN 15
#endif
#ifndef TFT_SCK_PIN
#define TFT_SCK_PIN 12
#endif
#ifndef TFT_MOSI_PIN
#define TFT_MOSI_PIN 11
#endif
#ifndef TFT_MISO_PIN
#define TFT_MISO_PIN 13
#endif
#ifndef TFT_BL_PIN
#define TFT_BL_PIN 21
#endif

// POWER SAVING
#define IDLE_TIME_ECO_MS 10000
#define IDLE_TIME_SLEEP_MS 1800000

// Button controls
#define BOOT_BUTTON_PIN 0
#define BOOT_SHORT_PRESS_MS 60
#define BOOT_LONG_PRESS_MS 1500

// Key Codes
#define KEY_MOD_LSHIFT 0x02
#define KEY_MOD_RSHIFT 0x20
#define KEY_INSERT 0x49
#define KEY_1 0x1E
#define KEY_2 0x1F
#define KEY_3 0x20
#define KEY_4 0x21
#define KEY_0 0x27
#define KEY_A 0x04
#define KEY_L 0x0F
#define KEY_M 0x10
#define KEY_W 0x1A

// ================= GLOBALS =================
extern "C" int ble_store_config_set_namespace(const char *name);

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
volatile bool ledDirty = true;

unsigned long lastKeyTime = 0;
bool isEcoMode = false;
uint8_t baseMac[6];
constexpr uint16_t INVALID_CONN_HANDLE = 0xFFFF;

typedef struct {
  const char *label;
  const char *shortcut;
  uint8_t aliasKey;
  uint32_t color;
} slot_profile_t;

const slot_profile_t slotProfiles[NUM_SLOTS] = {
    {"Mac", "Insert+M / Insert+1", KEY_M, 0x04A5E5},
    {"Windows", "Insert+W / Insert+2", KEY_W, 0xFE640B},
    {"Linux", "Insert+L / Insert+3", KEY_L, 0xD20F39},
    {"Android", "Insert+A / Insert+4", KEY_A, 0x40A02B},
};

NimBLEServer *pServer = nullptr;
NimBLEHIDDevice *pHidDev = nullptr;
NimBLECharacteristic *pInputChar = nullptr;
NimBLECharacteristic *pConsumerChar = nullptr;
NimBLECharacteristic *pOutputChar = nullptr;
NimBLECharacteristic *pBootInputChar = nullptr;
NimBLECharacteristic *pBootOutputChar = nullptr;
QueueHandle_t hidQueue = nullptr;
volatile uint16_t activeConnHandle = INVALID_CONN_HANDLE;
Preferences prefs;
bool prefsReady = false;

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
void markLEDDirty();
void processHID(hid_event_t *evt);
void handleSlotSwitch(int newSlot, bool pairingMode);
void handleFactoryReset();
void checkPowerManagement();
void handleBootButton();
void clearBondsAndEnterPairing();
void buildSlotBleAddress(int slot, uint8_t out[6]);
bool configureSlotBleIdentity(int slot);
const char *appStateName(ConnectionState state);
const char *resetReasonName(esp_reset_reason_t reason);
void logConnDesc(const char *prefix, ble_gap_conn_desc *desc);
int logGapEvent(ble_gap_event *event, void *arg);
void initSlotStorage();
void saveCurrentSlot(int slot);
void logBondSummary(const char *prefix);
void logSlotProfiles();
int decodeSlotShortcut(uint8_t keycode);
void buildSlotName(int slot, char *name, size_t nameLen);
void buildSlotNamespace(int slot, char *name, size_t nameLen);
bool configureSlotStorageNamespace(int slot);

class HidCharCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic,
              ble_gap_conn_desc *desc) override {
    Serial.printf("[HID] read uuid=%s handle=%u conn=%u\n",
                  pCharacteristic->getUUID().toString().c_str(),
                  pCharacteristic->getHandle(), desc->conn_handle);
  }

  void onWrite(NimBLECharacteristic *pCharacteristic,
               ble_gap_conn_desc *desc) override {
    NimBLEAttValue value = pCharacteristic->getValue();
    std::string text = value.getValue<std::string>();
    Serial.printf("[HID] write uuid=%s handle=%u conn=%u len=%u",
                  pCharacteristic->getUUID().toString().c_str(),
                  pCharacteristic->getHandle(), desc->conn_handle,
                  (unsigned)value.size());
    for (size_t i = 0; i < value.size(); i++) {
      Serial.printf(" %02X", (uint8_t)text[i]);
    }
    Serial.println();
  }
};

HidCharCallbacks hidCharCallbacks;

// ================= CALLBACKS =================
class MySecurityCallbacks : public NimBLESecurityCallbacks {
  bool onConfirmPIN(uint32_t pin) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    logConnDesc("[BLE] auth complete", desc);
    logBondSummary("[BLE] bonds after auth");
    if (!desc->sec_state.encrypted) {
      Serial.printf("[BLE] auth failed, disconnecting handle=%u\n",
                    desc->conn_handle);
      pServer->disconnect(desc->conn_handle);
    }
  }
  uint32_t onPassKeyRequest() override {
    Serial.println("[BLE] passkey requested");
    return 123456;
  }
  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("[BLE] passkey notify=%06lu\n", (unsigned long)pass_key);
  }
};

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
    bool wasPairing = (appState == STATE_DISCONNECTED_PAIRING);
    activeConnHandle = desc->conn_handle;
    isConnected = true;
    appState = STATE_CONNECTED;
    markLEDDirty();
    lastKeyTime = millis();
    isEcoMode = false;
    logConnDesc("[BLE] connected", desc);
    int rc = NimBLEDevice::startSecurity(desc->conn_handle);
    Serial.printf("[BLE] startSecurity handle=%u rc=%d mode=%s\n",
                  desc->conn_handle, rc, wasPairing ? "pairing" : "reconnect");
  }

  void onDisconnect(NimBLEServer *pServer) override {
    activeConnHandle = INVALID_CONN_HANDLE;
    isConnected = false;
    if (isSwitching)
      return;
    if (appState == STATE_CONNECTED)
      appState = STATE_DISCONNECTED_RECONNECTING;
    pServer->getAdvertising()->start();
    markLEDDirty();
  }

  void onDisconnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
    logConnDesc("[BLE] disconnected", desc);
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

const char *appStateName(ConnectionState state) {
  switch (state) {
  case STATE_DISCONNECTED_RECONNECTING:
    return "reconnecting";
  case STATE_DISCONNECTED_PAIRING:
    return "pairing";
  case STATE_CONNECTED:
    return "connected";
  default:
    return "unknown";
  }
}

const char *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
  case ESP_RST_POWERON:
    return "poweron";
  case ESP_RST_SW:
    return "software";
  case ESP_RST_PANIC:
    return "panic";
  case ESP_RST_INT_WDT:
    return "interrupt_wdt";
  case ESP_RST_TASK_WDT:
    return "task_wdt";
  case ESP_RST_WDT:
    return "other_wdt";
  case ESP_RST_DEEPSLEEP:
    return "deepsleep";
  case ESP_RST_BROWNOUT:
    return "brownout";
  case ESP_RST_SDIO:
    return "sdio";
  default:
    return "unknown";
  }
}

void logConnDesc(const char *prefix, ble_gap_conn_desc *desc) {
  NimBLEAddress peerId(desc->peer_id_addr);
  NimBLEAddress peerOta(desc->peer_ota_addr);
  Serial.printf(
      "%s handle=%u peer_id=%s peer_ota=%s encrypted=%d bonded=%d mtu=%u\n",
      prefix, desc->conn_handle, peerId.toString().c_str(),
      peerOta.toString().c_str(), desc->sec_state.encrypted,
      desc->sec_state.bonded, ble_att_mtu(desc->conn_handle));
}

int logGapEvent(ble_gap_event *event, void *arg) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    Serial.printf("[GAP] connect status=%d handle=%u\n", event->connect.status,
                  event->connect.conn_handle);
    break;
  case BLE_GAP_EVENT_DISCONNECT:
    Serial.printf("[GAP] disconnect reason=%d handle=%u\n",
                  event->disconnect.reason, event->disconnect.conn.conn_handle);
    break;
  case BLE_GAP_EVENT_CONN_UPDATE:
    Serial.printf("[GAP] conn_update status=%d handle=%u\n",
                  event->conn_update.status, event->conn_update.conn_handle);
    break;
  case BLE_GAP_EVENT_TERM_FAILURE:
    Serial.printf("[GAP] term_failure status=%d handle=%u\n",
                  event->term_failure.status, event->term_failure.conn_handle);
    break;
  case BLE_GAP_EVENT_ENC_CHANGE:
    Serial.printf("[GAP] enc_change status=%d handle=%u\n",
                  event->enc_change.status, event->enc_change.conn_handle);
    break;
  case BLE_GAP_EVENT_PASSKEY_ACTION:
    Serial.printf("[GAP] passkey_action action=%u handle=%u\n",
                  event->passkey.params.action, event->passkey.conn_handle);
    break;
  case BLE_GAP_EVENT_SUBSCRIBE:
    Serial.printf(
        "[GAP] subscribe handle=%u attr=%u notify=%u->%u indicate=%u->%u\n",
        event->subscribe.conn_handle, event->subscribe.attr_handle,
        event->subscribe.prev_notify, event->subscribe.cur_notify,
        event->subscribe.prev_indicate, event->subscribe.cur_indicate);
    break;
  case BLE_GAP_EVENT_MTU:
    Serial.printf("[GAP] mtu handle=%u value=%u\n", event->mtu.conn_handle,
                  event->mtu.value);
    break;
  case BLE_GAP_EVENT_REPEAT_PAIRING:
    Serial.printf("[GAP] repeat_pairing handle=%u\n",
                  event->repeat_pairing.conn_handle);
    break;
  case BLE_GAP_EVENT_IDENTITY_RESOLVED:
    Serial.printf("[GAP] identity_resolved handle=%u\n",
                  event->identity_resolved.conn_handle);
    break;
  default:
    break;
  }

  return 0;
}

void markLEDDirty() { ledDirty = true; }

void initSlotStorage() {
  prefsReady = prefs.begin("blekbd", false);
  if (!prefsReady) {
    Serial.println("[SYS] failed to open preferences");
    return;
  }

  int savedSlot = prefs.getUChar("slot", storedSlot);
  if (savedSlot >= 0 && savedSlot < NUM_SLOTS) {
    currentSlot = savedSlot;
    storedSlot = savedSlot;
  }
}

void saveCurrentSlot(int slot) {
  if (!prefsReady)
    return;
  prefs.putUChar("slot", slot);
}

int decodeSlotShortcut(uint8_t keycode) {
  switch (keycode) {
  case KEY_1:
    return 0;
  case KEY_2:
    return 1;
  case KEY_3:
    return 2;
  case KEY_4:
    return 3;
  default:
    break;
  }

  for (int i = 0; i < NUM_SLOTS; i++) {
    if (slotProfiles[i].aliasKey == keycode) {
      return i;
    }
  }

  return -1;
}

void buildSlotName(int slot, char *name, size_t nameLen) {
  snprintf(name, nameLen, "ESP-%s", slotProfiles[slot].label);
}

void logBondSummary(const char *prefix) {
  int bondCount = NimBLEDevice::getNumBonds();
  Serial.printf("%s count=%d\n", prefix, bondCount);
  for (int i = 0; i < bondCount; i++) {
    NimBLEAddress addr = NimBLEDevice::getBondedAddress(i);
    Serial.printf("%s[%d]=%s\n", prefix, i, addr.toString().c_str());
  }
}

void logSlotProfiles() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    Serial.printf("[SLOT] %d label=%s bind=%s\n", i + 1, slotProfiles[i].label,
                  slotProfiles[i].shortcut);
  }
}

void buildSlotNamespace(int slot, char *name, size_t nameLen) {
  snprintf(name, nameLen, "nimble_s%d", slot + 1);
}

bool configureSlotStorageNamespace(int slot) {
  char slotNamespace[16];
  buildSlotNamespace(slot, slotNamespace, sizeof(slotNamespace));

  int rc = ble_store_config_set_namespace(slotNamespace);
  if (rc != 0) {
    Serial.printf("[BLE] failed to set slot namespace=%s rc=%d\n",
                  slotNamespace, rc);
    return false;
  }

  Serial.printf("[BLE] slot %d store=%s\n", slot + 1, slotNamespace);
  return true;
}

void buildSlotBleAddress(int slot, uint8_t out[6]) {
  memcpy(out, baseMac, sizeof(baseMac));

  // Use a stable static-random address per slot instead of rewriting the ESP
  // base MAC at runtime, which destabilizes bonding and reconnects.
  out[0] = (out[0] & 0xF0) | ((slot + 1) * 3);
  out[5] = (out[5] & 0x3F) | 0xC0;
}

bool configureSlotBleIdentity(int slot) {
  uint8_t slotAddress[6];
  buildSlotBleAddress(slot, slotAddress);

  int rc = ble_hs_id_set_rnd(slotAddress);
  if (rc != 0) {
    Serial.printf("Failed to set BLE address for slot %d, rc=%d\n", slot + 1,
                  rc);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
    return false;
  }

  // Android typically reconnects from a resolvable private address. Using an
  // RPA with a stable random identity lets NimBLE restore bonded reconnects
  // through the resolving list while keeping each slot as a distinct device.
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RPA_RANDOM_DEFAULT);

  NimBLEAddress bleAddress(slotAddress, BLE_ADDR_RANDOM);
  Serial.printf("[BLE] slot %d addr=%s\n", slot + 1,
                bleAddress.toString().c_str());

  return true;
}

void updateLED() {
  if (appState == STATE_CONNECTED) {
    if (isEcoMode) {
      // Eco Mode: Dim Rosewater (RGB: 24, 22, 22). White channel explicitly 0.
      pixels.setPixelColor(0, pixels.Color(129, 200, 190, 20));
    } else {
      // Decodes Hex & Forces the blinding White LED to remain OFF
      uint32_t hex = slotProfiles[currentSlot].color;
      uint8_t r = (hex >> 16) & 0xFF;
      uint8_t g = (hex >> 8) & 0xFF;
      uint8_t b_val = hex & 0xFF;
      pixels.setPixelColor(0, pixels.Color(r, g, b_val, 0));
    }
  } else {
    pixels.setPixelColor(0, 0);
  }
  pixels.show();
  ledDirty = false;
}

void startBLE(int slot) {
  isSwitching = false;
  currentSlot = slot;
  storedSlot = slot;
  saveCurrentSlot(slot);
  activeConnHandle = INVALID_CONN_HANDLE;

  char name[20];
  buildSlotName(slot, name, sizeof(name));

  configureSlotStorageNamespace(slot);
  NimBLEDevice::init(name);
  NimBLEDevice::setCustomGapHandler(logGapEvent);
  configureSlotBleIdentity(slot);

  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC |
                                   BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

  pServer = NimBLEDevice::createServer();
  pServer->advertiseOnDisconnect(false);
  pServer->setCallbacks(new MyServerCallbacks());

  pHidDev = new NimBLEHIDDevice(pServer);
  pHidDev->reportMap((uint8_t *)hidReportMap, sizeof(hidReportMap));

  pInputChar = pHidDev->inputReport(1);
  pOutputChar = pHidDev->outputReport(1);
  pConsumerChar = pHidDev->inputReport(2);
  pBootInputChar = pHidDev->bootInput();
  pBootOutputChar = pHidDev->bootOutput();
  pOutputChar->setCallbacks(&hidCharCallbacks);
  pBootOutputChar->setCallbacks(&hidCharCallbacks);
  pHidDev->protocolMode()->setCallbacks(&hidCharCallbacks);
  pHidDev->hidControl()->setCallbacks(&hidCharCallbacks);

  pHidDev->manufacturer()->setValue("ESP-Custom");
  pHidDev->pnp(0x02, 0xe502, 0xa111, 0x0211);
  pHidDev->hidInfo(0x00, 0x03);
  pHidDev->setBatteryLevel(100);

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
  markLEDDirty();
  logBondSummary("[BLE] bonds at start");
  Serial.printf("[BLE] start slot=%d label=%s mode=%s name=%s\n",
                currentSlot + 1, slotProfiles[currentSlot].label,
                appStateName((ConnectionState)appState), name);
}

void stopBLE() {
  isSwitching = true;
  Serial.printf("[BLE] stop slot=%d connected=%d state=%s\n", currentSlot + 1,
                isConnected, appStateName((ConnectionState)appState));
  if (pServer) {
    std::vector<uint16_t> peers = pServer->getPeerDevices();
    for (uint16_t connHandle : peers) {
      Serial.printf("[BLE] disconnecting handle=%u for slot switch/stop\n",
                    connHandle);
      pServer->disconnect(connHandle);
    }

    if (activeConnHandle != INVALID_CONN_HANDLE && peers.empty()) {
      pServer->disconnect(activeConnHandle);
    }

    if (pServer->getConnectedCount() > 0) {
      unsigned long start = millis();
      while (pServer->getConnectedCount() > 0 && millis() - start < 1000)
        delay(10);
    }
    if (pServer->getAdvertising()->isAdvertising())
      pServer->getAdvertising()->stop();
    NimBLEDevice::deinit(true);
    pServer = nullptr;
    pHidDev = nullptr;
    pInputChar = nullptr;
    pConsumerChar = nullptr;
    pOutputChar = nullptr;
    pBootInputChar = nullptr;
    pBootOutputChar = nullptr;
    activeConnHandle = INVALID_CONN_HANDLE;
    isConnected = false;
  }
}

void handleSlotSwitch(int newSlot, bool pairingMode) {
  if (newSlot == currentSlot &&
      pairingMode == (appState == STATE_DISCONNECTED_PAIRING))
    return;
  Serial.printf("[SLOT] switch %d(%s) -> %d(%s) mode=%s\n", currentSlot + 1,
                slotProfiles[currentSlot].label, newSlot + 1,
                slotProfiles[newSlot].label,
                pairingMode ? "pairing" : "reconnect");
  stopBLE();
  delay(600);
  currentSlot = newSlot;
  appState = pairingMode ? STATE_DISCONNECTED_PAIRING
                         : STATE_DISCONNECTED_RECONNECTING;
  startBLE(currentSlot);
}

void handleFactoryReset() {
  Serial.println("[SYS] factory reset requested");
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
  for (int slot = 0; slot < NUM_SLOTS; slot++) {
    configureSlotStorageNamespace(slot);
    NimBLEDevice::init("");
    NimBLEDevice::deleteAllBonds();
    NimBLEDevice::deinit(true);
  }
  currentSlot = 0;
  storedSlot = 0;
  saveCurrentSlot(0);
  ESP.restart();
}

void clearBondsAndEnterPairing() {
  Serial.printf("[BLE] clear bonds and enter pairing on slot %d\n",
                currentSlot + 1);
  stopBLE();
  configureSlotStorageNamespace(currentSlot);
  NimBLEDevice::init("");
  NimBLEDevice::deleteAllBonds();
  NimBLEDevice::deinit(true);
  delay(200);
  appState = STATE_DISCONNECTED_PAIRING;
  markLEDDirty();
  startBLE(currentSlot);
}

void enterDeepSleep() {
  Serial.println("[SYS] entering deep sleep");
  oled_ssd1306::Status s;
  s.slot_1based = (uint8_t)(currentSlot + 1);
  s.profile = slotProfiles[currentSlot].label;
  s.state = "SLEEP";
  s.eco = isEcoMode;
  s.connected = isConnected;
  oled_ssd1306::showSleep(s);
  tft_display::Status ts;
  ts.slot_1based = s.slot_1based;
  ts.profile = s.profile;
  ts.state = s.state;
  ts.eco = s.eco;
  ts.connected = s.connected;
  tft_display::showSleep(ts);
  delay(50);
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
    markLEDDirty();
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
      int shortcutSlot = decodeSlotShortcut(evt->rawData[i]);
      if (shortcutSlot != -1)
        numKey = shortcutSlot;
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
      if (pBootInputChar) {
        pBootInputChar->setValue(evt->rawData, 8);
        pBootInputChar->notify();
      }
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
    markLEDDirty();
  }
}

void handleBootButton() {
  static bool prevPressed = false;
  static unsigned long pressedAt = 0;
  static bool initialized = false;
  static unsigned long readyAt = 0;

  bool pressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);

  if (!initialized) {
    initialized = true;
    readyAt = millis() + 1200;
    prevPressed = pressed;
    return;
  }

  if (millis() < readyAt) {
    prevPressed = pressed;
    return;
  }

  if (pressed && !prevPressed) {
    pressedAt = millis();
  }

  if (!pressed && prevPressed) {
    unsigned long heldMs = millis() - pressedAt;
    if (heldMs >= BOOT_LONG_PRESS_MS) {
      clearBondsAndEnterPairing();
    } else if (heldMs >= BOOT_SHORT_PRESS_MS) {
      int nextSlot = (currentSlot + 1) % NUM_SLOTS;
      handleSlotSwitch(nextSlot, false);
    }
  }

  prevPressed = pressed;
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
  delay(200);
  pixels.begin();
  pixels.setBrightness(LED_BRIGHTNESS);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  initSlotStorage();

  Serial.printf("[SYS] boot reset_reason=%s(%d) wakeup=%d stored_slot=%d\n",
                resetReasonName(esp_reset_reason()), esp_reset_reason(),
                esp_sleep_get_wakeup_cause(), storedSlot + 1);

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    currentSlot = storedSlot;
  }

  esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
  Serial.printf(
      "[SYS] base_mac=%02X:%02X:%02X:%02X:%02X:%02X current_slot=%d\n",
      baseMac[0], baseMac[1], baseMac[2], baseMac[3], baseMac[4], baseMac[5],
      currentSlot + 1);
  logSlotProfiles();

  bool oledOk = oled_ssd1306::begin(OLED_SDA_PIN, OLED_SCL_PIN,
                                    0); // auto-detect 0x3C/0x3D
  if (oledOk) {
    Serial.printf("[OLED] ok addr=0x%02X SDA=%d SCL=%d\n",
                  oled_ssd1306::address(), (int)OLED_SDA_PIN,
                  (int)OLED_SCL_PIN);
    oled_ssd1306::setBaseMac(baseMac);
    oled_ssd1306::Status s;
    s.slot_1based = (uint8_t)(currentSlot + 1);
    s.profile = slotProfiles[currentSlot].label;
    s.state = appStateName((ConnectionState)appState);
    s.eco = isEcoMode;
    s.connected = isConnected;
    oled_ssd1306::showStatus(s);
  } else {
    Serial.printf(
        "[OLED] init failed (no I2C ACK at 0x3C/0x3D on SDA=%d SCL=%d)\n",
        (int)OLED_SDA_PIN, (int)OLED_SCL_PIN);
  }

  tft_display::Pins tftPins = {
      .cs = TFT_CS_PIN,
      .dc = TFT_DC_PIN,
      .rst = TFT_RST_PIN,
      .sck = TFT_SCK_PIN,
      .mosi = TFT_MOSI_PIN,
      .miso = TFT_MISO_PIN,
      .backlight = TFT_BL_PIN,
  };
  bool tftOk = tft_display::begin(tftPins);
  if (tftOk) {
    Serial.printf(
        "[TFT] ok CS=%d DC=%d RST=%d SCK=%d MOSI=%d MISO=%d BL=%d 320x240\n",
        (int)TFT_CS_PIN, (int)TFT_DC_PIN, (int)TFT_RST_PIN, (int)TFT_SCK_PIN,
        (int)TFT_MOSI_PIN, (int)TFT_MISO_PIN, (int)TFT_BL_PIN);
    tft_display::showBootScreen();
  } else {
    Serial.printf("[TFT] init failed on CS=%d DC=%d RST=%d SCK=%d MOSI=%d "
                  "MISO=%d BL=%d\n",
                  (int)TFT_CS_PIN, (int)TFT_DC_PIN, (int)TFT_RST_PIN,
                  (int)TFT_SCK_PIN, (int)TFT_MOSI_PIN, (int)TFT_MISO_PIN,
                  (int)TFT_BL_PIN);
  }

  // Boot-time visual hint (works even if Serial monitor is not connected):
  // green = OLED acknowledged on I2C, red = no OLED found on I2C.
  pixels.setPixelColor(0, oledOk ? pixels.Color(0, 40, 0, 0)
                                 : pixels.Color(40, 0, 0, 0));
  pixels.show();
  delay(2600);
  pixels.setPixelColor(0, 0);
  pixels.show();

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

  handleBootButton();
  checkPowerManagement();

  static unsigned long lastUpdate = 0;
  static unsigned long lastOledUpdate = 0;
  unsigned long now = millis();

  if (now - lastOledUpdate > 250) {
    lastOledUpdate = now;
    oled_ssd1306::Status s;
    s.slot_1based = (uint8_t)(currentSlot + 1);
    s.profile = slotProfiles[currentSlot].label;
    s.state = appStateName((ConnectionState)appState);
    s.eco = isEcoMode;
    s.connected = isConnected;
    oled_ssd1306::showStatus(s);

    tft_display::Status ts;
    ts.slot_1based = s.slot_1based;
    ts.profile = s.profile;
    ts.state = s.state;
    ts.eco = s.eco;
    ts.connected = s.connected;
    tft_display::showStatus(ts);
  }

  if (appState == STATE_CONNECTED) {
    if (ledDirty)
      updateLED();
    if (now - lastUpdate > 1000)
      lastUpdate = now;
  } else if (appState == STATE_DISCONNECTED_PAIRING) {
    int b = (now % 2000) > 1000 ? 2000 - (now % 2000) : (now % 2000);
    b = map(b, 0, 1000, 0, 255);

    uint32_t hex = slotProfiles[currentSlot].color;
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
      uint32_t hex = slotProfiles[currentSlot].color;
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
