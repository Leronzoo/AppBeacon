#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// UUIDs para o serviço e características
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-cba987654321"
#define RSSI_CHARACTERISTIC_UUID "11111111-2222-3333-4444-555555555555"

// Configurações do beacon
#define BEACON_ID "BEACON 1"
#define DEVICE_NAME "BEACON SALA"

BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
BLECharacteristic* pRSSICharacteristic = NULL;
BLEAdvertising* pAdvertising = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool advertisingAtivo = true;

TaskHandle_t advertisingTaskHandle = NULL;
TaskHandle_t gattServerTaskHandle = NULL;
TaskHandle_t heartbeatTaskHandle = NULL;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("Cliente conectado");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("Cliente desconectado");
  }
};

class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      Serial.println("*********");
      Serial.print("Valor recebido: ");
      for (int i = 0; i < rxValue.length(); i++)
        Serial.print(rxValue[i]);
      Serial.println();
      Serial.println("*********");
    }
  }

  void onRead(BLECharacteristic *pCharacteristic) {
    Serial.println("Característica lida pelo cliente");
  }
};

void advertisingTask(void *parameter) {
  Serial.println("Tarefa de Advertising iniciada");

  while (1) {
    if (!deviceConnected && !advertisingAtivo) {
      Serial.println("Reiniciando advertising...");
      pAdvertising->start();
      advertisingAtivo = true;
    }

    String beaconData = "{\"id\":\"" + String(BEACON_ID) + "\",\"timestamp\":" + String(millis()) + ",\"status\":\"active\"}";
    pCharacteristic->setValue(beaconData.c_str());

    if (deviceConnected) {
      pCharacteristic->notify();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void gattServerTask(void *parameter) {
  Serial.println("Tarefa do GATT Server iniciada");

  while (1) {
    if (!deviceConnected && oldDeviceConnected) {
      delay(500);
      pServer->startAdvertising();
      advertisingAtivo = true;
      Serial.println("Iniciando advertising após desconexão");
      oldDeviceConnected = deviceConnected;
    }

    if (deviceConnected && !oldDeviceConnected) {
      oldDeviceConnected = deviceConnected;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void heartbeatTask(void *parameter) {
  Serial.println("Tarefa de Heartbeat iniciada");

  while (1) {
    Serial.print("Heartbeat - Heap livre: ");
    Serial.print(ESP.getFreeHeap());
    Serial.print(" bytes - Conectado: ");
    Serial.println(deviceConnected ? "Sim" : "Não");

    int16_t rssi = -40 + random(-20, 20);
    String rssiValue = String(rssi);
    pRSSICharacteristic->setValue(rssiValue.c_str());

    if (deviceConnected) {
      pRSSICharacteristic->notify();
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Iniciando ESP32 BLE Beacon...");

  BLEDevice::init(DEVICE_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pRSSICharacteristic = pService->createCharacteristic(
    RSSI_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pRSSICharacteristic->addDescriptor(new BLE2902());

  String initialData = "{\"id\":\"" + String(BEACON_ID) + "\",\"type\":\"beacon\",\"version\":\"1.0\"}";
  pCharacteristic->setValue(initialData.c_str());
  pRSSICharacteristic->setValue("-50");

  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);

  BLEAdvertisementData advertisementData;
  advertisementData.setName(DEVICE_NAME);
  advertisementData.setCompleteServices(BLEUUID(SERVICE_UUID));
  
  // Novo manufacturer data (formato binário)
  std::string manufacturerData = "BEACON_001";
  advertisementData.setManufacturerData(std::string((char *)manufacturerData.data(), manufacturerData.length()));
  
  pAdvertising->setAdvertisementData(advertisementData);

  BLEDevice::startAdvertising();
  advertisingAtivo = true;
  Serial.println("Aguardando conexões de clientes...");

  xTaskCreatePinnedToCore(advertisingTask, "AdvertisingTask", 4096, NULL, 2, &advertisingTaskHandle, 0);
  xTaskCreatePinnedToCore(gattServerTask, "GATTServerTask", 4096, NULL, 2, &gattServerTaskHandle, 1);
  xTaskCreatePinnedToCore(heartbeatTask, "HeartbeatTask", 2048, NULL, 1, &heartbeatTaskHandle, 0);

  Serial.println("Todas as tarefas FreeRTOS criadas com sucesso!");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
