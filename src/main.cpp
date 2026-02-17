#include <Arduino.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// --- BLE Dependencies ---
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <NimBLEHIDDevice.h>

// --- USB Host Dependencies ---
#include "usb/usb_host.h"
#include "hid_host.h"

// ================= CONFIGURATION =================
#define NUM_SLOTS 4
#define LED_PIN 48         
#define LED_BRIGHTNESS 20 

// POWER SAVING SETTINGS
#define IDLE_TIME_ECO_MS      10000   // 10 Seconds -> Switch to Low Power Bluetooth
#define IDLE_TIME_SLEEP_MS    1200000 // 20 Minutes -> Deep Sleep (Turn Off)

// Key Codes
#define KEY_MOD_LSHIFT  0x02
#define KEY_MOD_RSHIFT  0x20
#define KEY_INSERT      0x49
#define KEY_1           0x1E
#define KEY_2           0x1F
#define KEY_3           0x20
#define KEY_4           0x21
#define KEY_0           0x27

// ================= GLOBALS =================
Adafruit_NeoPixel pixels(1, LED_PIN, NEO_GRB + NEO_KHZ800);

enum ConnectionState {
    STATE_DISCONNECTED_RECONNECTING,
    STATE_DISCONNECTED_PAIRING,     
    STATE_CONNECTED                  
};

// Persistent State (Saved in RTC Memory during Deep Sleep)
RTC_DATA_ATTR int storedSlot = 0; 

volatile ConnectionState appState = STATE_DISCONNECTED_RECONNECTING;
volatile int currentSlot = 0; 
volatile bool isSwitching = false;
volatile bool isConnected = false;

// Power Management Globals
unsigned long lastKeyTime = 0;
bool isEcoMode = false;

// Colors: Red, Green, Blue, Yellow
uint32_t slotColors[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00}; 
uint8_t baseMac[6];

NimBLEServer* pServer = nullptr;
NimBLEHIDDevice* pHidDev = nullptr;
NimBLECharacteristic* pInputChar = nullptr; 
QueueHandle_t hidQueue = nullptr;

// ================= STRUCTS =================
typedef struct {
    uint8_t* rawData; 
    size_t len;
} hid_event_t;

const uint8_t hidReportMap[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 
    0x81, 0x03, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 
    0x95, 0x01, 0x75, 0x03, 0x91, 0x03, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0
};

// ================= FORWARD DECLARATIONS =================
void startBLE(int slot);
void stopBLE();
void updateLED();
void processHID(hid_event_t* evt);
void handleSlotSwitch(int newSlot, bool pairingMode);
void handleFactoryReset();
void checkPowerManagement(); // New function

// ================= SECURITY CALLBACKS =================
class MySecurityCallbacks : public NimBLESecurityCallbacks {
    bool onConfirmPIN(uint32_t pin) override { return true; }
    bool onSecurityRequest() override { return true; }
    void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
        if (!desc->sec_state.encrypted) {
            Serial.println("[SECURITY] Encryption FAILED - Retrying...");
            pServer->disconnect(desc->conn_handle); 
        } else {
            Serial.println("[SECURITY] Bonded/Encrypted Successfully");
        }
    }
    uint32_t onPassKeyRequest() override { return 123456; }
    void onPassKeyNotify(uint32_t pass_key) override { }
};

// ================= SERVER CALLBACKS =================
class MyServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) override {
        if (appState != STATE_DISCONNECTED_PAIRING && !NimBLEDevice::isBonded(desc->peer_ota_addr)) {
            Serial.println("[SECURITY] Rejecting unknown device in Reconnect Mode.");
            pServer->disconnect(desc->conn_handle);
            return;
        }

        Serial.printf("[BLE] Connected to Slot %d\n", currentSlot + 1);
        isConnected = true;
        appState = STATE_CONNECTED;
        updateLED();
        
        // Start with HIGH PERFORMANCE (Gaming Mode)
        pServer->updateConnParams(desc->conn_handle, 6, 6, 0, 60); 
        lastKeyTime = millis(); // Reset idle timer
        isEcoMode = false;
    }

    void onDisconnect(NimBLEServer* pServer) override {
        isConnected = false;
        if (isSwitching) return;
        
        Serial.println("[BLE] Disconnected.");
        if (appState == STATE_CONNECTED) {
            appState = STATE_DISCONNECTED_RECONNECTING; 
        }
        
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
                hid_device_handle, data_buffer, buffer_size, &data_len
            );

            if (err == ESP_OK && data_len > 0) {
                hid_event_t evt;
                evt.len = data_len;
                evt.rawData = data_buffer; 
                
                if (xQueueSend(hidQueue, &evt, 0) != pdTRUE) free(data_buffer); 
            } else {
                free(data_buffer);
            }
        }
    }
    else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        Serial.println("[USB] Keyboard Removed.");
        hid_host_device_close(hid_device_handle);
    }
}

void hid_host_driver_callback(hid_host_device_handle_t hid_device_handle, 
                              const hid_host_driver_event_t event, 
                              void *arg) {
    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        hid_host_dev_params_t dev_params;
        hid_host_device_get_params(hid_device_handle, &dev_params);
        
        if (dev_params.proto == HID_PROTOCOL_KEYBOARD) {
            Serial.println("[USB] Keyboard Detected");
            const hid_host_device_config_t dev_config = {
                .callback = hid_host_interface_callback,
                .callback_arg = NULL
            };
            hid_host_device_open(hid_device_handle, &dev_config);
            hid_host_device_start(hid_device_handle);
        } else {
            hid_host_device_close(hid_device_handle); 
        }
    }
}

// ================= LOGIC & HELPERS =================

void updateLED() {
    if (appState == STATE_CONNECTED) {
        if(isEcoMode) {
            // Dim LED in Eco Mode to save power
            uint32_t c = slotColors[currentSlot];
            pixels.setPixelColor(0, pixels.Color((c>>16)/10, (c>>8)/10, (c&0xFF)/10)); 
        } else {
            pixels.setPixelColor(0, slotColors[currentSlot]);
        }
    } else {
        pixels.setPixelColor(0, 0); 
    }
    pixels.show();
}

void startBLE(int slot) {
    isSwitching = false; 
    currentSlot = slot;
    storedSlot = slot; // Update RTC memory

    uint8_t newMac[6];
    memcpy(newMac, baseMac, 6);
    newMac[5] = 0x10 + slot; 
    
    esp_base_mac_addr_set(newMac); 
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);

    NimBLEDevice::setSecurityAuth(true, false, true); 
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityCallbacks(new MySecurityCallbacks()); 
    
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    pHidDev = new NimBLEHIDDevice(pServer);
    pHidDev->reportMap((uint8_t*)hidReportMap, sizeof(hidReportMap));
    pInputChar = pHidDev->inputReport(1); 
    
    pHidDev->manufacturer()->setValue("ESP-Custom");
    pHidDev->pnp(0x02, 0xe502, 0xa111, 0x0211);
    pHidDev->hidInfo(0x00, 0x01);

    NimBLEAdvertising* pAdvertising = pServer->getAdvertising();
    pAdvertising->setAppearance(HID_KEYBOARD);
    
    char name[20];
    sprintf(name, "ESP-Slot-%d", slot + 1);
    pAdvertising->setName(name);
    pAdvertising->addServiceUUID(pHidDev->hidService()->getUUID());

    pAdvertising->setScanResponse(true);
    pAdvertising->setMinInterval(32); 
    pAdvertising->setMaxInterval(48); 
    pAdvertising->setMinPreferred(0x06); 
    pAdvertising->setMaxPreferred(0x12); 
    
    pHidDev->startServices();
    pAdvertising->start();
    Serial.printf("Started Slot %d (Mac ends :%02X)\n", slot+1, newMac[5]);
    
    lastKeyTime = millis(); // Initialize timer
}

void stopBLE() {
    isSwitching = true; 
    if(pServer) {
        if (pServer->getConnectedCount() > 0) {
            pServer->disconnect(0);
            unsigned long start = millis();
            while(isConnected && millis() - start < 500) delay(10);
        }
        if (pServer->getAdvertising()->isAdvertising()) {
            pServer->getAdvertising()->stop();
        }
        NimBLEDevice::deinit(true);
        pServer = nullptr;
        pHidDev = nullptr;
        pInputChar = nullptr;
        isConnected = false;
    }
}

void handleSlotSwitch(int newSlot, bool pairingMode) {
    if (newSlot == currentSlot && pairingMode == (appState == STATE_DISCONNECTED_PAIRING)) return; 
    Serial.printf(">>> SWITCHING: Slot %d | Mode: %s <<<\n", newSlot+1, pairingMode ? "PAIRING" : "RECONNECT");
    stopBLE();
    delay(600); 
    currentSlot = newSlot;
    appState = pairingMode ? STATE_DISCONNECTED_PAIRING : STATE_DISCONNECTED_RECONNECTING;
    startBLE(currentSlot);
}

void handleFactoryReset() {
    Serial.println("!!! FACTORY RESET !!!");
    stopBLE();
    for(int i=0; i<10; i++) {
        pixels.setPixelColor(0, 0xFF0000); pixels.show(); delay(100);
        pixels.setPixelColor(0, 0); pixels.show(); delay(100);
    }
    NimBLEDevice::init(""); 
    NimBLEDevice::deleteAllBonds();
    ESP.restart();
}

void enterDeepSleep() {
    Serial.println(">>> ENTERING DEEP SLEEP <<<");
    stopBLE();
    
    // Turn off LED
    pixels.clear();
    pixels.show();
    
    // Configure Wakeup on BOOT Button (GPIO 0)
    // 0 = Low Level (Button Pressed)
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    
    esp_deep_sleep_start();
}

void processHID(hid_event_t* evt) {
    // Activity Detected!
    lastKeyTime = millis();
    
    // If we were in Eco Mode, wake up to Performance Mode
    if (isEcoMode && isConnected) {
        Serial.println("[PWR] Activity detected - High Performance Mode");
        pServer->updateConnParams(pServer->getPeerInfo(0).getConnHandle(), 6, 6, 0, 60); 
        isEcoMode = false;
        updateLED(); // Restore LED brightness
    }

    if (evt->len == 8) {
        uint8_t mod = evt->rawData[0];
        bool isShift = (mod & KEY_MOD_LSHIFT) || (mod & KEY_MOD_RSHIFT);
        bool isInsert = false;
        int numKey = -1;

        for(int i=2; i<8; i++) {
            if (evt->rawData[i] == KEY_INSERT) isInsert = true;
            if (evt->rawData[i] == KEY_1) numKey = 0;
            if (evt->rawData[i] == KEY_2) numKey = 1;
            if (evt->rawData[i] == KEY_3) numKey = 2;
            if (evt->rawData[i] == KEY_4) numKey = 3;
            if (evt->rawData[i] == KEY_0) numKey = 99;
        }

        if (isInsert && numKey != -1) {
            if (isShift && numKey == 99) { free(evt->rawData); handleFactoryReset(); return; }
            if (isShift && numKey <= 3) handleSlotSwitch(numKey, true); 
            else if (numKey <= 3) handleSlotSwitch(numKey, false); 
            free(evt->rawData);
            return; 
        }

        if (appState == STATE_CONNECTED && pInputChar) {
            pInputChar->setValue(evt->rawData, 8);
            pInputChar->notify();
        }
    }
    free(evt->rawData);
}

// Power Management Logic
void checkPowerManagement() {
    unsigned long now = millis();
    
    // 1. Check for Deep Sleep (Auto-Off)
    if (now - lastKeyTime > IDLE_TIME_SLEEP_MS) {
        enterDeepSleep();
    }

    // 2. Check for Eco Mode (Light Sleep logic via BLE latency)
    // Only if connected, not already in Eco, and idle for > 10s
    if (isConnected && !isEcoMode && (now - lastKeyTime > IDLE_TIME_ECO_MS)) {
        Serial.println("[PWR] Idle detected - Entering Eco Mode (Low Power Bluetooth)");
        // Update to 150ms connection interval (saves huge battery)
        pServer->updateConnParams(pServer->getPeerInfo(0).getConnHandle(), 120, 120, 0, 400); 
        isEcoMode = true;
        updateLED(); // Dim LED
    }
}

// ================= SYSTEM TASKS =================

void usb_lib_task(void* arg) {
    while (1) { usb_host_lib_handle_events(portMAX_DELAY, NULL); }
}

void hid_host_task(void* arg) {
    const usb_host_config_t host_config = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    usb_host_install(&host_config);
    xTaskCreate(usb_lib_task, "usb_events", 4096, NULL, 2, NULL);

    const hid_host_driver_config_t hid_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_driver_callback,
        .callback_arg = NULL
    };
    hid_host_install(&hid_config);
    vTaskDelete(NULL); 
}

// ================= SETUP/LOOP =================

void setup() {
    Serial.begin(115200);
    pixels.begin();
    pixels.setBrightness(LED_BRIGHTNESS);
    
    // Wakeup Check
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("Woke up from Deep Sleep via Button!");
        // We restore the slot from RTC memory
        currentSlot = storedSlot;
    }

    esp_read_mac(baseMac, ESP_MAC_WIFI_STA);
    
    hidQueue = xQueueCreate(20, sizeof(hid_event_t));
    xTaskCreate(hid_host_task, "hid_task", 4096, NULL, 2, NULL);
    
    startBLE(currentSlot); // Start the correct slot
}

void loop() {
    hid_event_t evt;
    if (xQueueReceive(hidQueue, &evt, 10)) {
        processHID(&evt);
    }

    checkPowerManagement(); // Check idle timers

    // LED PATTERNS
    static unsigned long lastUpdate = 0;
    unsigned long now = millis();
    uint32_t c = slotColors[currentSlot];

    if (appState == STATE_CONNECTED) {
        if (now - lastUpdate > 1000) { 
            // Refresh logic handled in updateLED()
            lastUpdate = now;
        }
    }
    else if (appState == STATE_DISCONNECTED_PAIRING) {
        int b = (now % 2000) > 1000 ? 2000 - (now%2000) : (now%2000);
        b = map(b, 0, 1000, 0, 100); 
        pixels.setPixelColor(0, pixels.Color((c>>16)*b/255, (c>>8)*b/255, (c&0xFF)*b/255));
        pixels.show();
    }
    else if (appState == STATE_DISCONNECTED_RECONNECTING) {
        long cycle = now % 2000; 
        bool on = (cycle < 100) || (cycle > 200 && cycle < 300) || (cycle > 400 && cycle < 500);
        
        if(on) pixels.setPixelColor(0, c);
        else pixels.setPixelColor(0, 0);
        pixels.show();
    }
}