/*
 * Pasapáginas DIY compatible con Kindle — control BLE para pasar páginas
 * Placa: DOIT ESP32 DevKit V1 (ESP32 clásico)
 * Librería: NimBLE-Arduino 2.x (Gestor de librerías → "NimBLE-Arduino" de h2zero)
 *
 * El Kindle Click oficial es un teclado BLE (HID over GATT) que envía
 * Page Down (avanzar) y Page Up (retroceder). El Kindle solo lo acepta como
 * "page turner" si el anuncio BLE y el servicio Device Information traen el
 * VID/PID del Click oficial, así que aquí lo anunciamos igual.
 * Fuente: https://kindlemodding.org/kindle-hacking/pageturner.html
 *
 * Proyecto independiente, sin relación con Amazon. Licencia MIT.
 */

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

// ---------- Configuración ----------
// PARA CAMBIAR EL NOMBRE MOSTRADO, CAMBIA AQUÍ:
//   ADV_NAME    → nombre que se ve al buscar dispositivos Bluetooth (máximo 16 caracteres,
//                 para que quepa en los 31 bytes del anuncio).
//   DEVICE_NAME → nombre que se lee una vez conectado.
// IMPORTANTE: en el Kindle siempre aparece como "Kindle Click", sin importar estos nombres,
// porque lo reconoce por su VID/PID (reportado en github.com/patricksuo/zectrix-note4-esp32-kindle-page-turner).
// Cambiar el nombre solo se nota en otros dispositivos, como un celular o una PC.
const char* DEVICE_NAME = "Amazon Kindle Remote";
const char* ADV_NAME    = "Kindle Click";

const uint8_t  PNP_VID_SOURCE = 0x01;   // 0x01 = Bluetooth SIG, 0x02 = USB-IF. Si no empareja, prueba 0x02.
const uint16_t PNP_VID        = 0x0171; // Amazon
const uint16_t PNP_PID        = 0x0443; // Kindle Click
const uint16_t PNP_VERSION    = 0x0100;

const int PIN_NEXT = 32;  // botón "siguiente página" → GND
const int PIN_PREV = 33;  // botón "página anterior" → GND
const int PIN_LED  = 2;   // LED azul de la placa

const unsigned long DEBOUNCE_MS = 30;

// Códigos HID (Keyboard/Keypad page 0x07)
const uint8_t KEY_PAGE_UP   = 0x4B;
const uint8_t KEY_PAGE_DOWN = 0x4E;

// ---------- Descriptor HID: teclado estándar, report ID 1 ----------
static const uint8_t reportMap[] = {
  0x05, 0x01,  // Usage Page (Generic Desktop)
  0x09, 0x06,  // Usage (Keyboard)
  0xA1, 0x01,  // Collection (Application)
  0x85, 0x01,  //   Report ID (1)
  0x05, 0x07,  //   Usage Page (Keyboard/Keypad)
  0x19, 0xE0,  //   Usage Minimum (Left Control)
  0x29, 0xE7,  //   Usage Maximum (Right GUI)
  0x15, 0x00,  //   Logical Minimum (0)
  0x25, 0x01,  //   Logical Maximum (1)
  0x75, 0x01,  //   Report Size (1)
  0x95, 0x08,  //   Report Count (8)
  0x81, 0x02,  //   Input (Data, Var, Abs) — modificadores
  0x95, 0x01,  //   Report Count (1)
  0x75, 0x08,  //   Report Size (8)
  0x81, 0x01,  //   Input (Const) — byte reservado
  0x95, 0x06,  //   Report Count (6)
  0x75, 0x08,  //   Report Size (8)
  0x15, 0x00,  //   Logical Minimum (0)
  0x25, 0xFF,  //   Logical Maximum (255)
  0x05, 0x07,  //   Usage Page (Keyboard/Keypad)
  0x19, 0x00,  //   Usage Minimum (0)
  0x29, 0xFF,  //   Usage Maximum (255)
  0x81, 0x00,  //   Input (Data, Array) — hasta 6 teclas
  0xC0         // End Collection
};

NimBLEHIDDevice* hid;
NimBLECharacteristic* keyboardInput;
volatile bool connected = false;
volatile unsigned long encryptedAt = 0;

// El Kindle descarta las teclas que llegan mientras todavía está descubriendo el HID.
const unsigned long KINDLE_READY_MS = 2500;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    Serial.printf("Conectado: %s\n", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    connected = false;
    Serial.printf("Desconectado (razón %d), anunciando de nuevo\n", reason);
    NimBLEDevice::startAdvertising();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    // Solo mandamos teclas cuando el enlace está cifrado; el Kindle no las acepta antes.
    connected = connInfo.isEncrypted();
    encryptedAt = millis();
    Serial.printf("Emparejado: %s\n", connected ? "sí" : "no (falló el cifrado)");
  }
};

struct Button {
  int pin;
  uint8_t key;
  bool stablePressed;
  bool lastReading;
  unsigned long lastChange;
};

Button buttons[] = {
  { PIN_NEXT, KEY_PAGE_DOWN, false, false, 0 },
  { PIN_PREV, KEY_PAGE_UP,   false, false, 0 },
};

void sendKey(uint8_t key) {
  uint8_t report[8] = { 0, 0, key, 0, 0, 0, 0, 0 };
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
  delay(10);

  memset(report, 0, sizeof(report));
  keyboardInput->setValue(report, sizeof(report));
  keyboardInput->notify();
}

void updateLed() {
  if (connected) {
    digitalWrite(PIN_LED, HIGH);
  } else {
    digitalWrite(PIN_LED, (millis() / 500) % 2);  // parpadeo = esperando al Kindle
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  for (Button& b : buttons) {
    pinMode(b.pin, INPUT_PULLUP);
  }

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, sin MITM ("Just Works"), Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  // No compartimos nuestra IRK, pero sí pedimos la del Kindle: se reconecta con dirección privada.
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  hid = new NimBLEHIDDevice(server);
  hid->setManufacturer("DIY");
  hid->setPnp(PNP_VID_SOURCE, PNP_VID, PNP_PID, PNP_VERSION);
  hid->setHidInfo(0x00, 0x03);  // RemoteWake | NormallyConnectable
  hid->setReportMap((uint8_t*)reportMap, sizeof(reportMap));
  keyboardInput = hid->getInputReport(1);
  hid->setBatteryLevel(100);
  hid->startServices();

  // El Kindle filtra el escaneo por el manufacturer data del anuncio, no por el PnP ID:
  // VID 0x0171 en little-endian y PID 0x0443 en big-endian → 71 01 04 43.
  // Fuente: https://github.com/LHBosssss/esp32-ble-page-turner (HANDOFF.md)
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);  // sin esto el Kindle lo ignora
  advData.setManufacturerData(std::string("\x71\x01\x04\x43", 4));
  advData.setCompleteServices(NimBLEUUID((uint16_t)0x1812));
  advData.setName(ADV_NAME);  // el escaneo del Kindle es pasivo: el nombre va en el paquete principal

  NimBLEAdvertisementData scanData;
  scanData.setAppearance(0x03C1);  // HID Keyboard

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->setAdvertisementData(advData);
  advertising->setScanResponseData(scanData);
  advertising->start();

  Serial.printf("Anunciando como \"%s\" (%s)...\n", ADV_NAME, NimBLEDevice::getAddress().toString().c_str());
}

void loop() {
  unsigned long now = millis();

  for (Button& b : buttons) {
    bool reading = digitalRead(b.pin) == LOW;
    if (reading != b.lastReading) {
      b.lastReading = reading;
      b.lastChange = now;
    }
    if (now - b.lastChange >= DEBOUNCE_MS && reading != b.stablePressed) {
      b.stablePressed = reading;
      if (b.stablePressed) {
        Serial.printf("Botón GPIO %d → tecla 0x%02X\n", b.pin, b.key);
        if (connected && now - encryptedAt >= KINDLE_READY_MS) {
          sendKey(b.key);
        }
      }
    }
  }

  updateLed();
  delay(5);
}
