#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <string>



#include <WiFi.h>
#include <WebServer.h>

// (BLE headers included later inside the ENABLE_BT block after flags are set)

// ===== Feature toggles (default: USB only) =====
#ifndef ENABLE_WIFI
#define ENABLE_WIFI 0
#endif
#ifndef ENABLE_BT
#define ENABLE_BT 1
#endif
#ifndef ENABLE_HTTP_SERVER
#define ENABLE_HTTP_SERVER ENABLE_WIFI
#endif

// ===== Panel setup =====
#define PANEL_RES_X 64   // width of ONE panel
#define PANEL_RES_Y 64   // height of ONE panel
#define PANEL_CHAIN 2    // two 64x64 panels chained horizontally -> 128x64 total

// Allow board-specific HUB75 pin remapping without editing logic below.
#ifndef HUB75_R1_PIN
#define HUB75_R1_PIN 25
#endif
#ifndef HUB75_G1_PIN
#define HUB75_G1_PIN 26
#endif
#ifndef HUB75_B1_PIN
#define HUB75_B1_PIN 27
#endif
#ifndef HUB75_R2_PIN
#define HUB75_R2_PIN 14
#endif
#ifndef HUB75_G2_PIN
#define HUB75_G2_PIN 12
#endif
#ifndef HUB75_B2_PIN
#define HUB75_B2_PIN 13
#endif
#ifndef HUB75_CLK_PIN
#define HUB75_CLK_PIN 16
#endif
#ifndef HUB75_LAT_PIN
#define HUB75_LAT_PIN 4
#endif
#ifndef HUB75_OE_PIN
#define HUB75_OE_PIN 15
#endif
#ifndef HUB75_A_PIN
#define HUB75_A_PIN 23
#endif
#ifndef HUB75_B_PIN
#define HUB75_B_PIN 19
#endif
#ifndef HUB75_C_PIN
#define HUB75_C_PIN 5
#endif
#ifndef HUB75_D_PIN
#define HUB75_D_PIN 17
#endif
#ifndef HUB75_E_PIN
#define HUB75_E_PIN 18
#endif
#ifndef HUB75_DRIVER
#define HUB75_DRIVER HUB75_I2S_CFG::ICN2038S
#endif
#ifndef HUB75_CLK_PHASE
#define HUB75_CLK_PHASE false
#endif

// Display
MatrixPanel_I2S_DMA *dma_display = nullptr;

// ===== Bluetooth (BLE) =====
#if ENABLE_BT
#include <NimBLEDevice.h>
// Nordic UART Service UUIDs
static const char* NUS_SERVICE_UUID      = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_CHAR_UUID_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; // write from central -> ESP32
static const char* NUS_CHAR_UUID_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; // notify from ESP32 -> central

static NimBLEServer*          gBleServer         = nullptr;
static NimBLECharacteristic*  gBleTxChar         = nullptr;
static NimBLEAdvertising*     gBleAdvertising    = nullptr;
static std::string            bleAccum; // accumulate until newline

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*connInfo*/) override;
};
static RxCallbacks gRxCallbacks;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* /*srv*/, NimBLEConnInfo& /*ci*/) override {
    if (Serial) Serial.println("[BLE] central connected");
  }
  void onDisconnect(NimBLEServer* /*srv*/, NimBLEConnInfo& /*ci*/, int reason) override {
    if (Serial) { Serial.print("[BLE] central disconnected, reason="); Serial.println(reason); }
    if (gBleAdvertising) {
      gBleAdvertising->start(); // resume advertising so scanners can see it again
      if (Serial) Serial.println("[BLE] advertising restarted");
    }
  }
};
static ServerCallbacks gServerCallbacks;

static void initBLE() {
  // Init BLE stack
  NimBLEDevice::init("MatrixPanel");
  if (Serial) Serial.println("[BLE] init: name=MatrixPanel");
  NimBLEDevice::setPower(ESP_PWR_LVL_P7); // max tx power for stability
  NimBLEDevice::setMTU(185);              // allow larger writes from macOS

  gBleServer = NimBLEDevice::createServer();
  gBleServer->setCallbacks(&gServerCallbacks);

  NimBLEService* svc = gBleServer->createService(NUS_SERVICE_UUID);

  // TX: notify to central
  gBleTxChar = svc->createCharacteristic(
    NUS_CHAR_UUID_TX,
    NIMBLE_PROPERTY::NOTIFY
  );

  // RX: write from central
  NimBLECharacteristic* rx = svc->createCharacteristic(
    NUS_CHAR_UUID_RX,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  rx->setCallbacks(&gRxCallbacks);

  svc->start();

  // Advertise NUS service so macOS CoreBluetooth tools can find it
  gBleAdvertising = NimBLEDevice::getAdvertising();
  gBleAdvertising->addServiceUUID(NUS_SERVICE_UUID);
  gBleAdvertising->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  gBleAdvertising->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  gBleAdvertising->enableScanResponse(true);
  gBleAdvertising->setName("MatrixPanel");
  NimBLEDevice::setDeviceName("MatrixPanel");
  gBleAdvertising->start();
  if (Serial) Serial.println("[BLE] advertising started (NUS)");
}
#endif
static bool kNewLivePending = false; // trigger to start dissolve->thinking->typewriter on new text

// ===== Wi-Fi (STA) + HTTP server =====
const char* WIFI_SSID = "TodayYouAreYou-ThatIsTruerThanTrue";
const char* WIFI_PASS = "bunnyBunny1!";
WebServer server(80);

// ===== Wrapped text mode (Option A) =====
static String gLiveText;         // incoming free-form text (no fixed line count)
static bool   gHasLiveText = false;

#if ENABLE_BT
// Define the onWrite now that globals above are declared
void RxCallbacks::onWrite(NimBLECharacteristic* c, NimBLEConnInfo& /*connInfo*/) {
  std::string v = c->getValue();
  if (v.empty()) return;
  // Append incoming bytes and look for a newline to mark a complete message
  bleAccum.append(v);
  if (bleAccum.find('\n') != std::string::npos) {
    gLiveText = String(bleAccum.c_str());
    gHasLiveText = true;
    kNewLivePending = true;
    bleAccum.clear();
  }
}
#endif

// Combined canned sentences (built from existing 6-line sets)
static String gCannedText[16];   // supports up to 16 canned sets; actual count built in setup
static int    gCannedCount = 0;

// ===== Text geometry =====
// Two chained 64x64 panels = 128px wide. Using 5x7 font + 1px spacing ≈ 6 px/char -> ~21 cols
static const uint8_t kCols = 21;        // used for cursor advance only (wrap is automatic)

// ===== Trite philosophies (each line exactly 10 chars) =====
static const char* kPhilosophies[][6] = {
  { // Set 1
    "Life is   ",
    "mostly fog",
    "and echoes",
    "of old tea",
    "cooling so",
    "again hmm."
  },
  { // Set 2
    "Truth: meh",
    "we nod now",
    "meaning is",
    "soft so so",
    "for a bit.",
    "then naps."
  },
  { // Set 3
    "Time hums.",
    "like a fan",
    "in a small",
    "we call it",
    "and stays.",
    "same as me"
  },
  { // Set 4
    "Hope shows",
    "then hides",
    "we shrug a",
    "little bit",
    "and sip we",
    "again sure"
  },
  { // Set 5
    "Meaning is",
    "just a map",
    "of places ",
    "we drew on",
    "in the fog",
    "last night"
  },
  { // Set 6
    "Mind drift",
    "over pools",
    "of bright ",
    "dot we map",
    "then we nap",
    "by morning"
  }
};
static const int kNumPhilos = sizeof(kPhilosophies) / sizeof(kPhilosophies[0]);
static int currentPhilo = 0; // index into kPhilosophies

// Target brightness for normal view
// Increase for daylight readability (0..255). Was 60.
static const uint8_t kTargetBrightness = 120;

// ===== Dynamic per-line color palette =====
static uint16_t gLineColors[6] = {0};

static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t /*0..255*/) {
  return (uint8_t)(((uint16_t)a * (255 - t) + (uint16_t)b * t + 127) / 255);
}

// Build a palette where line 0 is white and line 5 is the solid base color.
static void makePaletteFromBase(uint8_t br, uint8_t bg, uint8_t bb) {
  for (int i = 0; i < 6; ++i) {
    // t from 0 (top/white) to 255 (bottom/base)
    uint8_t t = (uint8_t)(i * (255 / 5));
    uint8_t r = lerp8(255, br, t);
    uint8_t g = lerp8(255, bg, t);
    uint8_t b = lerp8(255, bb, t);
    gLineColors[i] = dma_display->color565(r, g, b);
  }
}

// Pick a saturated random base color and build the palette.
static void randomizePalette() {
  // Ensure it's not too close to white/grey: pick one channel high, others random.
  uint8_t which = (uint8_t)random(3); // 0=r,1=g,2=b
  uint8_t r = (which == 0) ? 255 : (uint8_t)random(40, 221);
  uint8_t g = (which == 1) ? 255 : (uint8_t)random(40, 221);
  uint8_t b = (which == 2) ? 255 : (uint8_t)random(40, 221);
  makePaletteFromBase(r, g, b);
}

// ===== Utilities =====
// Render multi-line text with a white->base gradient per visual line.
// If revealChars >= 0, only the first `revealChars` characters across all lines are drawn (typewriter).
static void drawWrappedGradient(const String& text, int32_t revealChars /* -1 = full */) {
  dma_display->fillScreen(0);
  dma_display->setTextWrap(false); // we manage wrapping upstream (Python) to avoid word splits

  int y = 0;                 // line baseline, 10px per row with 5x7 font
  int lineIdx = 0;           // visual line index for gradient color
  int shown = 0;             // total characters drawn so far

  int start = 0;
  const int n = text.length();
  while (start < n) {
    int end = text.indexOf('\n', start);
    if (end < 0) end = n;
    String line = text.substring(start, end);

    // Determine how many chars of this line to draw under the reveal limit
    int toShow = line.length();
    if (revealChars >= 0) {
      int remaining = revealChars - shown;
      if (remaining <= 0) break;
      if (toShow > remaining) toShow = remaining;
    }

    // Color for this visual line (clamp past 5 to the base color)
    uint16_t col = gLineColors[(lineIdx < 6) ? lineIdx : 5];
    dma_display->setCursor(0, y);
    dma_display->setTextColor(col);
    for (int i = 0; i < toShow; ++i) {
      dma_display->print(line[i]);
    }

    shown += toShow;
    if (revealChars >= 0 && shown >= revealChars) break;

    y += 10;     // advance one text row (approx 8px tall font + spacing)
    lineIdx++;
    start = end + 1; // skip the newline we consumed
  }
}

// Random-pixel dissolve that clears the screen over duration_ms
void dissolveClear(uint16_t w, uint16_t h, uint32_t duration_ms) {
  const uint32_t N = (uint32_t)w * (uint32_t)h;         // 4096 for 64×64
  uint16_t *idx = (uint16_t*)malloc(N * sizeof(uint16_t));
  if (!idx) return;
  for (uint32_t i = 0; i < N; ++i) idx[i] = (uint16_t)i;
  for (uint32_t i = N - 1; i > 0; --i) {
    uint32_t j = (uint32_t)random(i + 1);
    uint16_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }
  uint32_t us_per_px = (duration_ms * 1000UL) / (N ? N : 1);
  if (us_per_px == 0) us_per_px = 1;
  for (uint32_t k = 0; k < N; ++k) {
    uint16_t p = idx[k];
    int16_t x = p % w;
    int16_t y = p / w;
    dma_display->drawPixel(x, y, 0);
    delayMicroseconds(us_per_px);
  }
  free(idx);
}

// Clear screen in random blocks for a very visible dissolve.
// block = tile size (e.g., 4 px), duration_ms is total animation time.
void dissolveClearBlocks(uint16_t w, uint16_t h, uint32_t duration_ms, uint8_t block = 4) {
  const uint16_t nx = (w + block - 1) / block;
  const uint16_t ny = (h + block - 1) / block;
  const uint32_t N  = (uint32_t)nx * (uint32_t)ny;

  uint16_t *idx = (uint16_t*)malloc(N * sizeof(uint16_t));
  if (!idx) return;

  for (uint32_t i = 0; i < N; ++i) idx[i] = (uint16_t)i;

  // Fisher–Yates shuffle
  for (uint32_t i = N - 1; i > 0; --i) {
    uint32_t j = (uint32_t)random(i + 1);
    uint16_t t = idx[i]; idx[i] = idx[j]; idx[j] = t;
  }

  // Time per block (>= 1 us)
  uint32_t us_per_blk = (duration_ms * 1000UL) / (N ? N : 1);
  if (us_per_blk == 0) us_per_blk = 1;

  for (uint32_t k = 0; k < N; ++k) {
    uint16_t p = idx[k];
    int16_t bx = (p % nx) * block;
    int16_t by = (p / nx) * block;
    uint16_t bw = (bx + block > w) ? (w - bx) : block;
    uint16_t bh = (by + block > h) ? (h - by) : block;
    dma_display->fillRect(bx, by, bw, bh, 0); // black tile
    delayMicroseconds(us_per_blk);
  }
  free(idx);
}

// Accept POST body with 6 lines, set as live text and trigger sequence
void handlePost() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "no body");
    return;
  }
  gLiveText = server.arg("plain");
  gHasLiveText = true;
  kNewLivePending = true; // trigger dissolve -> thinking -> typewriter
  server.send(200, "text/plain", "ok");
}

// BLE RX is handled in RxCallbacks::onWrite; nothing to poll here.
void processBluetooth() {
#if ENABLE_BT
  // BLE RX is handled in RxCallbacks::onWrite; nothing to poll here.
#endif
}

// Read 6 newline-terminated lines from USB Serial
void processUSB() {
  static String usbAccum;
  while (Serial.available()) {
    char c = (char)Serial.read();
    usbAccum += c;
    if (usbAccum.length() > 4096) usbAccum.remove(0, usbAccum.length() - 2048);
  }
  if (usbAccum.indexOf('\n') != -1) {
    gLiveText = usbAccum;
    gHasLiveText = true;
    kNewLivePending = true;
    usbAccum = "";
  }
}

// Draw the six lines with their colors, 10px spacing
void drawSixLines() {
  const String &src = gHasLiveText ? gLiveText : gCannedText[currentPhilo];
  drawWrappedGradient(src, -1); // full text, gradient per visual line
}

// Helper to build combined canned sentences from the 6-line arrays
static void buildCannedCombined() {
  gCannedCount = 0;
  for (int i = 0; i < kNumPhilos && i < 16; ++i) {
    String s;
    for (int l = 0; l < 6; ++l) {
      if (l) s += ' ';
      s += kPhilosophies[i][l];
    }
    gCannedText[gCannedCount++] = s;
  }
}

// Render "thinking" at bottom with optional flashing cursor
void renderThining(bool cursorOn) {
  const int textH = 8; // default font height
  const int y = PANEL_RES_Y - textH;
  dma_display->fillRect(0, y, dma_display->width(), textH, 0); // clear bottom strip across both panels
  dma_display->setCursor(0, y);
  dma_display->setTextColor(dma_display->color565(255, 255, 0)); // yellow
  dma_display->print("thinking");
  if (cursorOn) dma_display->print("_");
}

// ===== Minimal panel config (pins) =====
static void initPanel() {
  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  cfg.i2sspeed        = HUB75_I2S_CFG::HZ_20M;   // or HZ_40M if stable
  cfg.min_refresh_rate= 240;                     // bump target refresh
  cfg.clkphase        = HUB75_CLK_PHASE;         // toggle via build flag if rows are shifted
  cfg.driver          = HUB75_DRIVER;

  // Color/ctrl pins
  cfg.gpio.r1 = HUB75_R1_PIN;  cfg.gpio.g1 = HUB75_G1_PIN;  cfg.gpio.b1 = HUB75_B1_PIN;
  cfg.gpio.r2 = HUB75_R2_PIN;  cfg.gpio.g2 = HUB75_G2_PIN;  cfg.gpio.b2 = HUB75_B2_PIN;
  cfg.gpio.clk= HUB75_CLK_PIN; cfg.gpio.lat= HUB75_LAT_PIN; cfg.gpio.oe  = HUB75_OE_PIN;

  // Address lines (override via build flags when swapping panel driver boards)
  cfg.gpio.a = HUB75_A_PIN;
  cfg.gpio.b = HUB75_B_PIN;
  cfg.gpio.c = HUB75_C_PIN;
  cfg.gpio.d = HUB75_D_PIN;
  cfg.gpio.e = HUB75_E_PIN;

  dma_display = new MatrixPanel_I2S_DMA(cfg);
  dma_display->begin();
}

void setup() {
  Serial.begin(115200);
  randomSeed((uint32_t)micros());

  initPanel();
  dma_display->setBrightness8(kTargetBrightness);
  dma_display->fillScreen(0);

  randomizePalette();

  buildCannedCombined();

  // Pick a random starting set and draw
  currentPhilo = random(kNumPhilos);
  drawSixLines();

  // Bluetooth BLE (NimBLE UART / NUS)
  #if ENABLE_BT
  initBLE();
  if (Serial) Serial.println("[BLE] setup complete; scanning from a phone or Mac should show 'MatrixPanel'.");
  #endif

  // --- Wi-Fi station bring-up ---
  #if ENABLE_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000UL) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
    // Show IP on the LED panel for 5 seconds
    dma_display->fillScreen(0);
    dma_display->setCursor(0, 0);
    dma_display->setTextColor(dma_display->color565(255, 255, 255));
    dma_display->print("IP: ");
    dma_display->println(WiFi.localIP());
    delay(5000);

    // Restore initial display
    drawSixLines();
  } else {
    Serial.println("Wi-Fi not connected (continuing; BT will still work).");
  }
  #endif

// --- HTTP endpoint ---
  #if ENABLE_HTTP_SERVER
  server.on("/post", HTTP_POST, handlePost);
  server.begin();
  #endif
}

void loop() {
  #if ENABLE_HTTP_SERVER
  server.handleClient();
  #endif
  processUSB();
  #if ENABLE_BT
  processBluetooth();
  #endif

  enum ScreenState { STATE_WAIT_60S, STATE_DISSOLVING, STATE_POST_DISSOLVE_PAUSE,
                     STATE_THINING, STATE_TYPEWRITER, STATE_DONE };
  static ScreenState state = STATE_WAIT_60S;
  static unsigned long tMark = millis();  // phase start time

  // Typewriter progress
  static size_t twIdx = 0;    // number of characters revealed
  static unsigned long twLast = 0;
  const uint16_t twDelayMs = 30; // per-character delay (faster feels better when wrapping)

  // Check Bluetooth; if new text, start dissolve immediately
  if (kNewLivePending) {
    kNewLivePending = false;
    tMark = millis();
    state = STATE_DISSOLVING;
  }

  switch (state) {
    case STATE_WAIT_60S: {
      if (millis() - tMark >= 60000UL) {
        state = STATE_DISSOLVING; // run a ~2s dissolve next
      }
    } break;

    case STATE_DISSOLVING: {
      Serial.println("[STATE] DISSOLVING");
      // Chunky, obvious dissolve using 4x4 tiles over ~1.5s across full chained width
      dissolveClearBlocks((uint16_t)dma_display->width(), (uint16_t)dma_display->height(), 1500, 4);
      tMark = millis();
      state = STATE_POST_DISSOLVE_PAUSE;            // 1s pause
    } break;

    case STATE_POST_DISSOLVE_PAUSE: {
      if (millis() - tMark >= 1000UL) {
        tMark = millis();
        state = STATE_THINING;
      }
    } break;

    

    case STATE_THINING: {
      // Blink cursor every ~500ms
      bool cursorOn = ((millis() / 500UL) % 2) == 0;
      renderThining(cursorOn);

      // After 10s, begin typewriter reveal of current text
      if (millis() - tMark >= 10000UL) {
        dma_display->setBrightness8(kTargetBrightness);
        dma_display->fillScreen(0);
        twIdx = 0; twLast = 0;
        randomizePalette();
        state = STATE_TYPEWRITER;
      }
      delay(30); // gentle pace for redraws
    } break;

    case STATE_TYPEWRITER: {
      if (millis() - twLast >= twDelayMs) {
        twLast = millis();
        const String &src = gHasLiveText ? gLiveText : gCannedText[currentPhilo];
        if (twIdx < src.length()) {
          // Redraw substring [0..twIdx] with per-line gradient
          drawWrappedGradient(src, (int32_t)(twIdx + 1));
          twIdx++;
        } else {
          state = STATE_DONE; // finished
        }
      }
      delay(10);
    } break;

    case STATE_DONE: {
      // Hold briefly, then choose a new canned set and wait again
      delay(2000);
      int prev = currentPhilo;
      if (kNumPhilos > 1) {
        do { currentPhilo = random(kNumPhilos); } while (currentPhilo == prev);
      }
      gHasLiveText = false; // return to canned cycle after showing live once
      tMark = millis();
      state = STATE_WAIT_60S;
    } break;
  }
}
