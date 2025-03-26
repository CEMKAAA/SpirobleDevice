#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Arduino.h>
#include <WiFi.h>

// Device info
String DEVICE_NAME = "Spirometer";

// BLE UUIDs - THESE MUST MATCH YOUR FLUTTER APP
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define FLOW_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // Notify characteristic
#define RX_UUID "e3223119-9445-4e96-a4a1-85358c4046a2"    // Write characteristic

// BLE objects
BLEServer* pServer = NULL;
BLECharacteristic* flowCharacteristic = NULL;
BLECharacteristic* rxCharacteristic = NULL;
BLE2902* pBLE2902;

// Connection state
bool deviceConnected = false;
bool oldDeviceConnected = false;
unsigned long lastAdvertisingTime = 0;

// Sensor and measurement variables
int count1 = 0;
int count2 = 0;
bool cw;
unsigned long zaman = 0;
float flowrate = 0.0;
float volume = 0;
float pulseliter = 50;  // Calibration factor (rotations per liter)
int period = 100;       // Measurement period in ms

// Flow control
bool basla = false;
bool timerRunning = false;
unsigned long startTime = 0;
unsigned long stopTime = 0;
String incoming_str = "";
char buffer[50];

// Pin definitions
const int SENSOR_PIN_1 = 17;
const int SENSOR_PIN_2 = 27;

// BLE Server Callbacks
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("✓ Client connected");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("✗ Client disconnected");
  }
};

// RX Characteristic Callbacks
// Modify this in your ESP32 code
class CharacteristicCallBack: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string rxValue = pChar->getValue();
    
    Serial.print("RAW DATA RECEIVED (bytes): ");
    Serial.println(rxValue.length());
    
    // Debug raw bytes
    Serial.print("Hex values: ");
    for (int i = 0; i < rxValue.length(); i++) {
      Serial.print((uint8_t)rxValue[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
    if (rxValue.length() > 0) {
      incoming_str = String(rxValue.c_str());
      Serial.print("Received command: '");
      Serial.print(incoming_str);
      Serial.println("'");
      
      // Test direct byte comparison for '1'
      if (rxValue.length() == 1 && rxValue[0] == '1') {
        Serial.println("➤➤➤ COMMAND '1' DETECTED! ➤➤➤");
        Serial.println("Starting flow measurement from BLE");
        startTime = millis();
        stopTime = startTime + 10000;
        basla = true;
        timerRunning = true;
        volume = 0;
        
        // SEND TEST DATA IMMEDIATELY
        flowrate = 3.5;  // Static test value
        volume = 1.0;    // Static test value
        
        if (deviceConnected) {
          snprintf(buffer, sizeof(buffer), "{%.2f,%.2f,%lu}", flowrate, volume, millis());
          flowCharacteristic->setValue(buffer);
          flowCharacteristic->notify();
          Serial.print("SENT TEST DATA: ");
          Serial.println(buffer);
        }
      } 
      else if (incoming_str == "0") {
        Serial.println("Stopping flow measurement");
        basla = false;
        timerRunning = false;
      }
    }
  }
};

// Function to measure pulses
void count_pulse() {
  zaman = millis();
  count1 = 0;
  count2 = 0;
  
  while (millis() - zaman <= period) {
    if (digitalRead(SENSOR_PIN_1) == false && digitalRead(SENSOR_PIN_2) == false) {
      while (digitalRead(SENSOR_PIN_1) == false && digitalRead(SENSOR_PIN_2) == false) {
        if (millis() - zaman >= period) {
          break;
        }
      }
      
      if (digitalRead(SENSOR_PIN_1) == true) {
        cw = 1;
        count1++;
      } else if (digitalRead(SENSOR_PIN_2) == true) {
        cw = 0;
        count2++;
      }
    }
  }
}

// Function to restart advertising if needed
void checkAndRestartAdvertising() {
  // If disconnected and was previously connected, restart advertising
  if (!deviceConnected && oldDeviceConnected) {
    unsigned long currentTime = millis();
    // Add delay to give BLE stack time to get ready
    if (currentTime - lastAdvertisingTime > 500) {
      pServer->startAdvertising();
      Serial.println("↻ Restarting advertising");
      lastAdvertisingTime = currentTime;
      oldDeviceConnected = deviceConnected;
    }
  }
  
  // If connected and was previously disconnected, handle new connection
  if (deviceConnected && !oldDeviceConnected) {
    Serial.println("→ New connection established");
    oldDeviceConnected = deviceConnected;
  }
}

void setup() {
  // Set pin modes
  pinMode(SENSOR_PIN_1, INPUT);
  pinMode(SENSOR_PIN_2, INPUT);
  
  // Initialize serial
  Serial.begin(115200);
  Serial.println("\n=== ESP32 BLE SPIROMETER ===");
  
  // Get device MAC address
  String macAddress = WiFi.macAddress();
  DEVICE_NAME = "Spirometer-" + macAddress.substring(macAddress.length() - 5);
  
  Serial.print("MAC Address: ");
  Serial.println(macAddress);
  Serial.print("Device Name: ");
  Serial.println(DEVICE_NAME);

  // Initialize BLE
  BLEDevice::init(DEVICE_NAME.c_str());
  Serial.println("BLE Device initialized");
  
  // Create BLE Server
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  Serial.println("BLE Server created");

  // Create BLE Service
  BLEService* pService = pServer->createService(SERVICE_UUID);
  Serial.println("BLE Service created");

  // Create Flow Notification Characteristic
  flowCharacteristic = pService->createCharacteristic(
    FLOW_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  
  // Create RX Characteristic (for receiving commands)
  rxCharacteristic = pService->createCharacteristic(
    RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ
  );
  
  // Add descriptor
  pBLE2902 = new BLE2902();
  pBLE2902->setNotifications(true);
  flowCharacteristic->addDescriptor(pBLE2902);
  
  // Set callback for RX characteristic
  rxCharacteristic->setCallbacks(new CharacteristicCallBack());
  
  // Start the service
  pService->start();
  Serial.println("BLE Service started");
  
  // Start advertising
  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.println("BLE Advertising started");
  Serial.println("Ready for connections...");
}

void loop() {
  // Always check for reconnection/advertising needs
  checkAndRestartAdvertising();
  
  // Auto-stop timer if active
  if (timerRunning && millis() >= stopTime) {
    basla = false;
    timerRunning = false;
    Serial.println("Flow measurement auto-stopped after 10 seconds");
  }

  // Process serial input (for debugging)
  if (Serial.available()) {
    String serialCmd = Serial.readStringUntil('\n');
    Serial.print("Serial command: ");
    Serial.println(serialCmd);
    
    if (serialCmd == "1") {
      Serial.println("Starting flow measurement from serial");
      startTime = millis();
      stopTime = startTime + 10000;
      basla = true;
      timerRunning = true;
      volume = 0;
    } 
    else if (serialCmd == "0") {
      Serial.println("Stopping flow measurement from serial");
      basla = false;
      timerRunning = false;
    }
    else if (serialCmd == "status") {
      Serial.println("=== STATUS ===");
      Serial.print("Connected: ");
      Serial.println(deviceConnected ? "Yes" : "No");
      Serial.print("Measuring: ");
      Serial.println(basla ? "Yes" : "No");
      Serial.print("Flow rate: ");
      Serial.println(flowrate);
      Serial.print("Volume: ");
      Serial.println(volume);
    }
  }

  // Flow measurement logic
  if (basla) {
    // Measure pulse count
    count_pulse();
    
    // Calculate flow rate based on pulse count
    if (cw == 1) {
      flowrate = (1000.0 / period) * (count1 / pulseliter);
    } else if (cw == 0) {
      flowrate = -(1000.0 / period) * (count2 / pulseliter);
    }
    
    // Update volume
    volume += (flowrate / (1000.0 / period));
    
    // Send data if connected
    if (deviceConnected) {
      snprintf(buffer, sizeof(buffer), "{%.2f,%.2f,%lu}", flowrate, volume, millis());
      flowCharacteristic->setValue(buffer);
      flowCharacteristic->notify();
      
      // Debugging - print data occasionally
      if (millis() % 1000 < 100) {  // Print roughly every second
        Serial.print("Sending: ");
        Serial.println(buffer);
      }
    }
  }
  
  // Small delay to prevent CPU hogging
  delay(10);
}