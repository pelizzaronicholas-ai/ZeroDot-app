/**
 * epaper-bridge.ino
 * XIAO nRF52840 — BLE → Serial bridge per e-Paper Waveshare 2.7"
 * Board package: Seeed nRF52 (usa Adafruit Bluefruit nRF52 stack)
 *
 * Nordic UART Service (NUS):
 *   Service  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX char  6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (write, centrale → periferica)
 *   TX char  6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (notify, periferica → centrale)
 *
 * Protocollo:
 *   - La centrale invia chunk di byte via RX (max MTU ~244 byte/chunk)
 *   - Il firmware assembla i chunk in bmpBuf[5808]
 *   - Il terminatore 0xFF 0xFF segnala la fine del trasferimento
 *   - Su terminatore: log Serial, LED verde fisso, buffer pronto per SPI (fase 2)
 *
 * LED XIAO nRF52840 (attivi LOW, definiti dal board package):
 *   LED_RED   P0.26
 *   LED_GREEN P0.30
 *   LED_BLUE  P0.06
 */

#include <bluefruit.h>

// ─── Bitmap ──────────────────────────────────────────────────
static const int BMP_W        = 176;
static const int BMP_H        = 264;
static const int BITMAP_BYTES = (BMP_W * BMP_H) / 8;  // 5808

static uint8_t bmpBuf[BITMAP_BYTES];
static int     bmpIdx    = 0;
static bool    recvDone  = false;
static uint8_t prevByte  = 0x00;

// ─── LED ─────────────────────────────────────────────────────
// Il board package Seeed nRF52840 definisce LED_RED/GREEN/BLUE (attivi LOW).
// Se non definiti, usiamo i pin fisici del XIAO nRF52840.
#ifndef LED_RED
  #define LED_RED   26
#endif
#ifndef LED_GREEN
  #define LED_GREEN 30
#endif
#ifndef LED_BLUE
  #define LED_BLUE   6
#endif

inline void ledOff()   { digitalWrite(LED_RED,HIGH); digitalWrite(LED_GREEN,HIGH); digitalWrite(LED_BLUE,HIGH); }
inline void ledBlue()  { ledOff(); digitalWrite(LED_BLUE,LOW); }
inline void ledGreen() { ledOff(); digitalWrite(LED_GREEN,LOW); }

static unsigned long lastBlink  = 0;
static bool          blinkState = false;

// ─── NUS Service + Characteristic ───────────────────────────
BLEService        nusService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
BLECharacteristic rxChar("6E400003-B5A3-F393-E0A9-E50E24DCCA9E");
BLECharacteristic txChar("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");

// ─── Prototipi ───────────────────────────────────────────────
void onConnect(uint16_t conn_hdl);
void onDisconnect(uint16_t conn_hdl, uint8_t reason);
void onRxWritten(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len);
void resetBuffer();

// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // Attende Serial USB (CDC) fino a 3 s; continua comunque se non aperto
  while (!Serial && millis() < 3000) delay(10);
  Serial.println("[epaper-bridge] boot");

  // LED
  pinMode(LED_RED,   OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE,  OUTPUT);
  ledOff();

  // Bluefruit init
  Bluefruit.begin();
  Bluefruit.setName("ZERODOT-ePaper");
  Bluefruit.Periph.setConnectCallback(onConnect);
  Bluefruit.Periph.setDisconnectCallback(onDisconnect);

  // TX characteristic: notify + read
  txChar.setProperties(CHR_PROPS_NOTIFY | CHR_PROPS_READ);
  txChar.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  txChar.setMaxLen(20);
  txChar.begin();

  // RX characteristic: write + write without response
  rxChar.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  rxChar.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  rxChar.setMaxLen(244);  // BLE 4.2+ extended MTU
  rxChar.setWriteCallback(onRxWritten);

  // Avvia il service (deve stare dopo le characteristic)
  nusService.begin();
  rxChar.begin();  // begin() dopo nusService.begin()

  resetBuffer();

  // Advertising
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(nusService);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);  // 20 ms / 152.5 ms
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);  // 0 = no timeout

  Serial.println("[BLE] advertising — ZERODOT-ePaper");
  Serial.print("[BLE] buffer bitmap = ");
  Serial.print(BITMAP_BYTES);
  Serial.println(" byte");
}

// ─────────────────────────────────────────────────────────────
void loop() {
  // Blink blu durante ricezione attiva
  if (Bluefruit.connected() && !recvDone) {
    unsigned long now = millis();
    if (now - lastBlink >= 300) {
      lastBlink  = now;
      blinkState = !blinkState;
      if (blinkState) ledBlue(); else ledOff();
    }
  }
  // Nessun busy-wait: gli eventi BLE arrivano via callback FreeRTOS
}

// ─── Callback connessione ────────────────────────────────────
void onConnect(uint16_t conn_hdl) {
  BLEConnection* conn = Bluefruit.Connection(conn_hdl);
  char peer[18] = {0};
  conn->getPeerName(peer, sizeof(peer));
  Serial.print("[BLE] connesso: ");
  Serial.println(peer[0] ? peer : "(no name)");
  ledBlue();
}

// ─── Callback disconnessione ─────────────────────────────────
void onDisconnect(uint16_t conn_hdl, uint8_t reason) {
  (void)conn_hdl;
  Serial.print("[BLE] disconnesso, reason=0x");
  Serial.println(reason, HEX);
  if (!recvDone) ledOff();
}

// ─── Callback RX write ───────────────────────────────────────
void onRxWritten(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  (void)conn_hdl;
  (void)chr;

  if (recvDone) return;

  for (uint16_t i = 0; i < len; i++) {
    uint8_t b = data[i];

    // Terminatore 0xFF 0xFF
    if (prevByte == 0xFF && b == 0xFF) {
      if (bmpIdx > 0) bmpIdx--;  // rimuovi il primo 0xFF già nel buffer

      recvDone = true;
      ledGreen();

      Serial.print("[RX] BITMAP RICEVUTA OK — ");
      Serial.print(bmpIdx);
      Serial.println(" byte");

      // ACK via TX notify
      uint8_t ack[2] = {'O', 'K'};
      txChar.notify(ack, 2);
      return;
    }

    if (bmpIdx < BITMAP_BYTES) {
      bmpBuf[bmpIdx++] = b;
    } else {
      // Buffer saturo senza terminatore
      recvDone = true;
      ledGreen();
      Serial.print("[RX] BUFFER PIENO — ");
      Serial.print(bmpIdx);
      Serial.println(" byte (nessun terminatore)");
      return;
    }

    prevByte = b;
  }

  // Progresso ogni ~512 byte
  if ((bmpIdx % 512) < (int)len) {
    Serial.print("[RX] ");
    Serial.print(bmpIdx);
    Serial.print(" / ");
    Serial.print(BITMAP_BYTES);
    Serial.println(" byte");
  }
}

// ─── Reset buffer ────────────────────────────────────────────
void resetBuffer() {
  memset(bmpBuf, 0x00, BITMAP_BYTES);
  bmpIdx     = 0;
  recvDone   = false;
  prevByte   = 0x00;
  blinkState = false;
  lastBlink  = 0;
  Serial.println("[BUF] buffer azzerato");
}
