#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Wi-Fi
const char* ssid = "Lorenzo 2G";
const char* password = "essy54321";
const char* webip = "192.168.100.32";

// UUIDs do serviço BLE
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "87654321-4321-4321-4321-cba987654321"
#define RSSI_CHARACTERISTIC_UUID "11111111-2222-3333-4444-555555555555"

// Variáveis dinâmicas do beacon
String beaconId = "BEACON 2";
String deviceName = "BEACON COZINHA";

// Ponteiros BLE
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;
BLECharacteristic* pRSSICharacteristic = NULL;
BLEAdvertising* pAdvertising = NULL;

// Estados
bool deviceConnected = false;
bool oldDeviceConnected = false;
bool advertisingAtivo = true;

// Tarefas
TaskHandle_t advertisingTaskHandle = NULL;
TaskHandle_t gattServerTaskHandle = NULL;
TaskHandle_t heartbeatTaskHandle = NULL;
TaskHandle_t httpTaskHandle = NULL;

// URL encode
String urlencode(String str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

// Callbacks BLE
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("Cliente conectado");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("Cliente desconectado");
  }
};

class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0) {
      Serial.print("Valor recebido: ");
      for (int i = 0; i < rxValue.length(); i++) Serial.print(rxValue[i]);
      Serial.println();
    }
  }
  void onRead(BLECharacteristic *pCharacteristic) {
    Serial.println("Característica lida pelo cliente");
  }
};

// Tarefa de configuração dinâmica
void httpConfigTask(void *parameter) {
  Serial.println("Tarefa de configuração HTTP iniciada");

  while (true) {
    if (WiFi.status() == WL_CONNECTED) {
      String url = "http://" + String(webip) + "/api/config.php?id=" + urlencode(beaconId);
      HTTPClient http;
      http.begin(url);

      int httpCode = http.GET();
      if (httpCode == 200) {
        String payload = http.getString();
        Serial.println("Configuração recebida: " + payload);

        ArduinoJson::JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (!error) {
          beaconId = doc["beacon_id"].as<String>();
          String status = doc["status"].as<String>();
          String mensagem = doc["mensagem"].as<String>();
          deviceName = doc["device_name"].as<String>();

          String json = "{\"id\":\"" + beaconId + "\",\"status\":\"" + status + "\",\"mensagem\":\"" + mensagem + "\"}";
          pCharacteristic->setValue(json.c_str());

        BLEAdvertisementData advData;
        advData.setName(deviceName.c_str());
        advData.setCompleteServices(BLEUUID(SERVICE_UUID));
        pAdvertising->stop();  // Para o advertising atual
        delay(100);            // Pequeno delay de segurança
        pAdvertising->setAdvertisementData(advData);
        pAdvertising->start(); // Reinicia o advertising com novo nome

        Serial.println("Advertising reiniciado com novo nome: " + deviceName);

        } else {
          Serial.println("Erro ao decodificar JSON");
        }
      } else {
        Serial.printf("Erro HTTP: %d\n", httpCode);
      }
      http.end();
    } else {
      Serial.println("Wi-Fi desconectado. Tentando reconectar...");
      WiFi.reconnect();
    }

    vTaskDelay(pdMS_TO_TICKS(15000));
  }
}

// Registro no servidor
void registerTask(void *parameter) {
  Serial.println("Tarefa de registro HTTP iniciada");

  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  HTTPClient http;
  http.begin("http://" + String(webip) + "/api/register.php");
  http.addHeader("Content-Type", "application/json");

String json = "{\"beacon_id\":\"" + beaconId + "\","
              "\"status\":\"ativo\","
              "\"mensagem\":\"Inicializado\","
              "\"versao\":\"1.0\","
              "\"device_name\":\"" + deviceName + "\"}";
  int httpCode = http.POST(json);

  if (httpCode > 0) Serial.println("Beacon registrado no servidor.");
  else Serial.printf("Falha ao registrar beacon: %d\n", httpCode);

  http.end();
  vTaskDelete(NULL);
}

// Advertising
void advertisingTask(void *parameter) {
  while (1) {
    if (!deviceConnected && !advertisingAtivo) {
      pAdvertising->start();
      advertisingAtivo = true;
    }

    String beaconData = "{\"id\":\"" + beaconId + "\",\"timestamp\":" + String(millis()) + ",\"status\":\"active\"}";
    pCharacteristic->setValue(beaconData.c_str());
    if (deviceConnected) pCharacteristic->notify();

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// GATT monitor
void gattServerTask(void *parameter) {
  while (1) {
    if (!deviceConnected && oldDeviceConnected) {
      delay(500);
      pServer->startAdvertising();
      advertisingAtivo = true;
      oldDeviceConnected = deviceConnected;
    }

    if (deviceConnected && !oldDeviceConnected) {
      oldDeviceConnected = deviceConnected;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// Heartbeat
void heartbeatTask(void *parameter) {
  while (1) {
    Serial.printf("Heartbeat - Heap livre: %lu bytes - Conectado: %s\n",
                  ESP.getFreeHeap(), deviceConnected ? "Sim" : "Não");

    int16_t rssi = -40 + random(-20, 20);
    pRSSICharacteristic->setValue(String(rssi).c_str());
    if (deviceConnected) pRSSICharacteristic->notify();

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

// Setup
void setup() {
  Serial.begin(115200);
  Serial.println("Iniciando ESP32 BLE Beacon...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi conectado!");

  BLEDevice::init(deviceName.c_str());
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());

  pRSSICharacteristic = pService->createCharacteristic(
    RSSI_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pRSSICharacteristic->addDescriptor(new BLE2902());

  String json = "{\"id\":\"" + beaconId + "\",\"type\":\"beacon\",\"version\":\"1.0\"}";
  pCharacteristic->setValue(json.c_str());
  pRSSICharacteristic->setValue("-50");

  pService->start();

  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);

  BLEAdvertisementData advData;
  advData.setName(deviceName.c_str());
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  std::string manufacturerData = "BEACON_001";
  advData.setManufacturerData(manufacturerData);
  pAdvertising->setAdvertisementData(advData);

  BLEDevice::startAdvertising();
  advertisingAtivo = true;

  xTaskCreatePinnedToCore(registerTask, "RegisterTask", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(httpConfigTask, "HTTPConfigTask", 4096, NULL, 1, &httpTaskHandle, 1);
  xTaskCreatePinnedToCore(advertisingTask, "AdvertisingTask", 4096, NULL, 2, &advertisingTaskHandle, 0);
  xTaskCreatePinnedToCore(gattServerTask, "GATTServerTask", 4096, NULL, 2, &gattServerTaskHandle, 1);
  xTaskCreatePinnedToCore(heartbeatTask, "HeartbeatTask", 2048, NULL, 1, &heartbeatTaskHandle, 0);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
