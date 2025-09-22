#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Adafruit_GFX.h>
#include <BluetoothSerial.h>

#include <WiFi.h>
#include <WebServer.h>

// ===== Panel setup =====
#define PANEL_RES_X 64   // width of ONE panel
#define PANEL_RES_Y 64   // height of ONE panel
#define PANEL_CHAIN 1    // number of panels chained

// Display
MatrixPanel_I2S_DMA *dma_display = nullptr;

// ===== Bluetooth (SPP) =====
BluetoothSerial SerialBT;
static String btAccum;
static bool kNewLivePending = false; // trigger to start dissolve->thinking->typewriter on new text

// ===== Wi-Fi (STA) + HTTP server =====
const char* WIFI_SSID = "TodayYouAreYou-ThatIsTruerThanTrue";
const char* WIFI_PASS = "bunnyBunny1!";
WebServer server(80);

// ===== Live text buffer (6 lines x 10 chars + NUL) =====
static char kLiveLines[6][11] = {{0}};  // current active text from BT
static bool kHasLive = false;           // when true, prefer kLiveLines over canned sets

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
static const uint8_t kTargetBrightness = 60;

// ===== Utilities =====
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

// Parse a 6-line body into kLiveLines; return true on success
bool setLiveFromBody(const String& body) {
  int line = 0, idx = 0; char tmp[11];
  for (size_t i = 0; i < body.length() && line < 6; ++i) {
    char c = body[i];
    if (c == '\r') continue;
    if (c == '\n') {
      while (idx < 10) tmp[idx++] = ' ';
      tmp[10] = '\0'; memcpy(kLiveLines[line], tmp, 11);
      line++; idx = 0;
    } else if (idx < 10) {
      tmp[idx++] = c;
    }
  }
  if (line < 6 && idx > 0) {
    while (idx < 10) tmp[idx++] = ' ';
    tmp[10] = '\0'; memcpy(kLiveLines[line++], tmp, 11);
  }
  if (line == 6) { kHasLive = true; return true; }
  return false;
}

// Accept POST body with 6 lines, set as live text and trigger sequence
void handlePost() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain", "no body");
    return;
  }
  String body = server.arg("plain");
  if (setLiveFromBody(body)) {
    kNewLivePending = true;           // dissolve -> pause -> "thinking_" -> typewriter
    server.send(200, "text/plain", "ok");
  } else {
    server.send(400, "text/plain", "need 6 lines");
  }
}

// Read from Bluetooth and capture the first 6 newline-terminated lines
void processBluetooth() {
  while (SerialBT.available()) {
    char c = (char)SerialBT.read();
    btAccum += c;
    if (btAccum.length() > 2048) btAccum.remove(0, btAccum.length() - 1024);
  }
  int nl = 0; int cutoff = -1;
  for (int i = 0; i < (int)btAccum.length(); ++i) {
    if (btAccum[i] == '\n') { nl++; if (nl == 6) { cutoff = i + 1; break; } }
  }
  if (cutoff > 0) {
    String payload = btAccum.substring(0, cutoff);
    btAccum.remove(0, cutoff);
    if (setLiveFromBody(payload)) {
      kNewLivePending = true; // trigger sequence
    }
  }
}

// Read 6 newline-terminated lines from USB Serial
void processUSB() {
  static String usbAccum;
  while (Serial.available()) {
    char c = (char)Serial.read();
    usbAccum += c;
    if (usbAccum.length() > 2048) usbAccum.remove(0, usbAccum.length() - 1024);
  }
  int nl = 0; int cutoff = -1;
  for (int i = 0; i < (int)usbAccum.length(); ++i) {
    if (usbAccum[i] == '\n') { nl++; if (nl == 6) { cutoff = i + 1; break; } }
  }
  if (cutoff > 0) {
    String payload = usbAccum.substring(0, cutoff);
    usbAccum.remove(0, cutoff);
    if (setLiveFromBody(payload)) {
      kNewLivePending = true; // trigger dissolve->thinking->typewriter
    }
  }
}

// Draw the six lines with their colors, 10px spacing
void drawSixLines() {
  dma_display->fillScreen(0);
  uint16_t colors[6] = {
    dma_display->color565(255, 0, 0),   // red
    dma_display->color565(0, 255, 0),   // green
    dma_display->color565(0, 0, 255),   // blue
    dma_display->color565(255, 255, 0), // yellow
    dma_display->color565(0, 255, 255), // cyan
    dma_display->color565(255, 0, 255)  // magenta
  };
  int y = 0;
  for (int i = 0; i < 6; i++) {
    dma_display->setCursor(0, y);
    dma_display->setTextColor(colors[i]);
    if (kHasLive) {
      dma_display->print(kLiveLines[i]);
    } else {
      dma_display->print(kPhilosophies[currentPhilo][i]);
    }
    y += 10;
  }
}

// Render "thinking" at bottom with optional flashing cursor
void renderThining(bool cursorOn) {
  const int textH = 8; // default font height
  const int y = PANEL_RES_Y - textH;
  dma_display->fillRect(0, y, PANEL_RES_X, textH, 0); // clear bottom strip
  dma_display->setCursor(0, y);
  dma_display->setTextColor(dma_display->color565(255, 255, 0)); // yellow
  dma_display->print("thinking");
  if (cursorOn) dma_display->print("_");
}

// ===== Minimal panel config (pins) =====
static void initPanel() {
  HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
  cfg.i2sspeed = HUB75_I2S_CFG::HZ_10M;
  cfg.clkphase = false;
  cfg.driver   = HUB75_I2S_CFG::ICN2038S;

  // Color/ctrl pins
  cfg.gpio.r1 = 25;  cfg.gpio.g1 = 26;  cfg.gpio.b1 = 27;
  cfg.gpio.r2 = 14;  cfg.gpio.g2 = 12;  cfg.gpio.b2 = 13;
  cfg.gpio.clk= 16;  cfg.gpio.lat= 4;   cfg.gpio.oe  = 15;

  // Address lines (these values match one of your earlier working variants)
  cfg.gpio.a = 23;   // fixed
  cfg.gpio.b = 19;
  cfg.gpio.c = 5;    // fixed
  cfg.gpio.d = 18;
  cfg.gpio.e = 17;

  dma_display = new MatrixPanel_I2S_DMA(cfg);
  dma_display->begin();
}

void setup() {
  Serial.begin(115200);
  randomSeed((uint32_t)micros());

  initPanel();
  dma_display->setBrightness8(kTargetBrightness);
  dma_display->fillScreen(0);

  // Pick a random starting set and draw
  currentPhilo = random(kNumPhilos);
  drawSixLines();

  // Bluetooth SPP
  SerialBT.begin("MatrixPanel");

  // --- Wi-Fi station bring-up ---
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

// --- HTTP endpoint ---
server.on("/post", HTTP_POST, handlePost);
server.begin();
}

void loop() {
  server.handleClient();
  processUSB();
  processBluetooth();

  enum ScreenState { STATE_WAIT_60S, STATE_DISSOLVING, STATE_POST_DISSOLVE_PAUSE,
                     STATE_THINING, STATE_TYPEWRITER, STATE_DONE };
  static ScreenState state = STATE_WAIT_60S;
  static unsigned long tMark = millis();  // phase start time

  // Typewriter progress
  static int twLine = 0;      // 0..5
  static int twChar = 0;      // 0..9 (10 chars per line)
  static unsigned long twLast = 0;
  const uint16_t twDelayMs = 70; // per-character delay

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
      // Chunky, obvious dissolve using 4x4 tiles over ~1.5s
      dissolveClearBlocks(PANEL_RES_X, PANEL_RES_Y, 1500, 4);
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
        twLine = 0; twChar = 0; twLast = 0;
        state = STATE_TYPEWRITER;
      }
      delay(30); // gentle pace for redraws
    } break;

    case STATE_TYPEWRITER: {
      // Colors for the six lines
      static uint16_t colors[6] = {
        dma_display->color565(255, 0, 0),   // red
        dma_display->color565(0, 255, 0),   // green
        dma_display->color565(0, 0, 255),   // blue
        dma_display->color565(255, 255, 0), // yellow
        dma_display->color565(0, 255, 255), // cyan
        dma_display->color565(255, 0, 255)  // magenta
      };

      if (millis() - twLast >= twDelayMs) {
        twLast = millis();
        int x = twChar * 6;          // default 5x7 font + 1px spacing
        int y = twLine * 10;         // 10px line spacing
        dma_display->setCursor(x, y);
        dma_display->setTextColor(colors[twLine]);
        char ch = kHasLive ? kLiveLines[twLine][twChar]
                           : kPhilosophies[currentPhilo][twLine][twChar];
        dma_display->print(ch);

        twChar++;
        if (twChar >= 10) {
          twChar = 0;
          twLine++;
          if (twLine >= 6) {
            state = STATE_DONE; // finished all text
          }
        }
      }
      delay(5);
    } break;

    case STATE_DONE: {
      // Hold briefly, then choose a new canned set and wait again
      delay(2000);
      int prev = currentPhilo;
      if (kNumPhilos > 1) {
        do { currentPhilo = random(kNumPhilos); } while (currentPhilo == prev);
      }
      tMark = millis();
      state = STATE_WAIT_60S;
    } break;
  }
}