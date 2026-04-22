/*
  CYD Bluetooth HID Media Controller
  Hardware: ESP32-2432S028 (Cheap Yellow Display)
  Display:  ILI9341 240x320 via TFT_eSPI (HSPI)
  Touch:    XPT2046 via XPT2046_Touchscreen (VSPI, custom pins)
  BLE:      ESP32-BLE-Keyboard (classic Bluedroid stack)

  Pairs with Mac / Linux as "CYD Media Remote" Bluetooth HID keyboard.
  Sends standard media keycodes: play/pause, next, previous,
  volume up/down, mute.
*/

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <BleKeyboard.h>
#include <BLEDevice.h>   // for BLESecurity — security override after begin()

// ---------------------------------------------------------------------------
// Touch SPI pins (VSPI — separate bus from the display)
// Source: ESP32-2432S028 schematic / Rui Santos reference sketch
// ---------------------------------------------------------------------------
#define XPT2046_IRQ  36   // T_IRQ  (input only — no pull-up needed)
#define XPT2046_MOSI 32   // T_DIN
#define XPT2046_MISO 39   // T_OUT  (input only)
#define XPT2046_CLK  25   // T_CLK
#define XPT2046_CS   33   // T_CS

// ---------------------------------------------------------------------------
// Screen dimensions (landscape)
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

// ---------------------------------------------------------------------------
// Touch calibration — raw ADC → pixel
// Matched to reference sketch: map(p.x, 200, 3700, 1, SCREEN_W)
// ---------------------------------------------------------------------------
#define TOUCH_X_MIN  200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN  240
#define TOUCH_Y_MAX 3800

// ---------------------------------------------------------------------------
// Layout constants
// 3 columns × 2 rows filling 320×240
// Col widths: 106 | 108 | 106  (sum = 320)
// Row heights: 120 | 120       (sum = 240)
// ---------------------------------------------------------------------------
#define COLS        3
#define ROWS        2
#define BTN_W      (SCREEN_W / COLS)   // 106 px (integer division)
#define BTN_H      (SCREEN_H / ROWS)   // 120 px
#define BORDER      2                  // px gap drawn as border
#define LABEL_PAD  12                  // px from bottom of cell for label

// ---------------------------------------------------------------------------
// Colors (RGB565)
// ---------------------------------------------------------------------------
#define COL_BG          TFT_BLACK
#define COL_BTN         0x2104   // very dark grey
#define COL_BTN_BORDER  0x4A69   // mid grey
#define COL_BTN_PRESS   0x0600   // dark green flash
#define COL_TEXT        TFT_WHITE
#define COL_SYMBOL      0x07FF   // cyan — symbols stand out from white labels
#define COL_DOT_CONN    TFT_GREEN
#define COL_DOT_WAIT    0xFD20   // orange

// ---------------------------------------------------------------------------
// Button descriptor
// ---------------------------------------------------------------------------
struct Button {
  const char* symbol;   // large glyph drawn centre of cell
  const char* label;    // small label drawn near bottom of cell
  const uint8_t* key;   // BLE media keycode (2 bytes)
};

// Button grid — row-major order: [row][col]
// Row 0: PREV | PLAY/PAUSE | NEXT
// Row 1: MUTE | VOL DOWN   | VOL UP
static const Button BUTTONS[ROWS][COLS] = {
  {
    { "<<",  "PREV",     KEY_MEDIA_PREVIOUS_TRACK },
    { ">||",  "PLAY/PAUSE", KEY_MEDIA_PLAY_PAUSE   },
    { ">>",  "NEXT",     KEY_MEDIA_NEXT_TRACK     },
  },
  {
    { "<",   "MUTE",     KEY_MEDIA_MUTE           },
    { "< )",  "VOL -",    KEY_MEDIA_VOLUME_DOWN    },
    { "< ))",  "VOL +",    KEY_MEDIA_VOLUME_UP      },
  },
};

// ---------------------------------------------------------------------------
// Global objects
// ---------------------------------------------------------------------------
TFT_eSPI          tft;
SPIClass          touchSPI(VSPI);
XPT2046_Touchscreen touch(XPT2046_CS, XPT2046_IRQ);
BleKeyboard       bleKeyboard("CYD Media Remote", "ESP32", 100);

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
enum AppState { STATE_WAITING, STATE_CONNECTED };
static AppState   appState        = STATE_WAITING;
static bool       prevConnected   = false;
static uint32_t   waitDotTimer    = 0;
static uint8_t    waitDotPhase    = 0;   // for animated ellipsis

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void drawWaitingScreen();
void drawConnectedScreen();
void drawButton(int row, int col, bool pressed);
void drawStatusDot(bool connected);
int  getTouchedButton(int tx, int ty, int* outRow, int* outCol);

// ===========================================================================
// Drawing helpers
// ===========================================================================

// Return pixel x of left edge of column col
inline int colX(int col) { return col * BTN_W; }

// Return pixel y of top edge of row row
inline int rowY(int row) { return row * BTN_H; }

// Draw one button.  pressed=true → highlight background.
void drawButton(int row, int col, bool pressed) {
  int x = colX(col) + BORDER;
  int y = rowY(row) + BORDER;
  int w = BTN_W - BORDER * 2;
  int h = BTN_H - BORDER * 2;

  const Button& btn = BUTTONS[row][col];

  // Background
  uint32_t bg = pressed ? COL_BTN_PRESS : COL_BTN;
  tft.fillRoundRect(x, y, w, h, 6, bg);

  // Border
  tft.drawRoundRect(x, y, w, h, 6, COL_BTN_BORDER);

  // Symbol — large, centred in upper 2/3 of cell
  tft.setTextColor(COL_SYMBOL, bg);
  tft.setTextDatum(MC_DATUM);  // middle-centre
  tft.drawString(btn.symbol, x + w / 2, y + h / 2 - 12, 4);

  // Label — small, near bottom of cell
  tft.setTextColor(COL_TEXT, bg);
  tft.setTextDatum(BC_DATUM);  // bottom-centre
  tft.drawString(btn.label, x + w / 2, y + h - LABEL_PAD, 2);
}

// Draw status indicator dot + text in top-right corner (over the grid).
// Called after drawConnectedScreen so it paints on top.
void drawStatusDot(bool connected) {
  // Small area: top-right 90×14 px (sits inside the NEXT button cell)
  int dotX = SCREEN_W - 10;
  int dotY = 6;
  int r    = 4;
  tft.fillCircle(dotX, dotY, r, connected ? COL_DOT_CONN : COL_DOT_WAIT);
}

// Draw the waiting / pairing screen.
void drawWaitingScreen() {
  tft.fillScreen(COL_BG);

  int cx = SCREEN_W / 2;

  // Title
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("CYD Media Remote", cx, 70, 4);

  // Bluetooth symbol placeholder (simple 'B' in blue circle)
  tft.fillCircle(cx, 128, 28, 0x001F);            // blue fill
  tft.drawCircle(cx, 128, 28, 0x07FF);            // cyan border
  tft.setTextColor(TFT_WHITE, 0x001F);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("BT", cx, 128, 4);

  // Status text
  tft.setTextColor(COL_DOT_WAIT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Waiting for connection...", cx, 192, 2);

  waitDotTimer = millis();
  waitDotPhase = 0;
}

// Draw the full 6-button connected UI.
void drawConnectedScreen() {
  tft.fillScreen(COL_BG);

  for (int r = 0; r < ROWS; r++) {
    for (int c = 0; c < COLS; c++) {
      drawButton(r, c, false);
    }
  }

  drawStatusDot(true);
}

// Hit-test a touch point against the button grid.
// Returns 0 on hit (sets *outRow, *outCol), -1 on miss.
int getTouchedButton(int tx, int ty, int* outRow, int* outCol) {
  if (tx < 0 || tx >= SCREEN_W || ty < 0 || ty >= SCREEN_H) return -1;
  *outCol = tx / BTN_W;
  *outRow = ty / BTN_H;
  if (*outRow >= ROWS || *outCol >= COLS) return -1;
  return 0;
}

// Animate waiting-screen ellipsis dots so the screen doesn't look frozen.
void tickWaitingAnimation() {
  if (millis() - waitDotTimer < 600) return;
  waitDotTimer = millis();
  waitDotPhase = (waitDotPhase + 1) % 4;

  // Build "Waiting..." with 0–3 trailing dots
  char msg[32];
  const char* dots[] = { "   ", ".  ", ".. ", "..." };
  snprintf(msg, sizeof(msg), "Waiting%s", dots[waitDotPhase]);

  tft.setTextColor(COL_DOT_WAIT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(msg, SCREEN_W / 2, 192, 2);
}

// ===========================================================================
// setup
// ===========================================================================
void setup() {
  Serial.begin(115200);

  // --- Touchscreen (VSPI with non-default pins) ---
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);   // landscape, matches tft rotation

  // --- Display (HSPI — pins configured via platformio.ini build flags) ---
  tft.init();
  tft.setRotation(1);     // landscape: 320 wide × 240 tall
  tft.fillScreen(COL_BG);

  // Turn on backlight (GPIO 21, active HIGH on this board)
  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);

  // --- BLE keyboard ---
  // Keep the default Apple VID/PID (0x05AC / 0x820A).
  // iOS requires the Apple VID to correctly route Consumer Control (media key)
  // HID reports — a generic VID causes iOS to accept the connection but silently
  // ignore all media key presses. The macOS pairing stall was caused by the
  // security mode (SC_MITM_BOND), not the VID/PID — that is fixed below.
  bleKeyboard.begin();

  // Override the security mode set by the library (ESP_LE_AUTH_REQ_SC_MITM_BOND).
  // MITM + Secure Connections causes macOS to stall mid-pairing while the ESP32
  // fires onConnect() immediately at link layer — making the device appear
  // connected on screen while macOS spins forever.
  // Dropping to bonding-only (Just Works, no MITM) is standard for HID devices
  // and works correctly on both macOS and Linux.
  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_BOND);

  // --- Initial screen ---
  drawWaitingScreen();

  Serial.println("CYD Media Remote starting — advertising BLE...");
}

// ===========================================================================
// loop
// ===========================================================================
void loop() {
  bool connected = bleKeyboard.isConnected();

  // --- State transitions ---
  if (connected && !prevConnected) {
    // Just connected
    appState = STATE_CONNECTED;
    drawConnectedScreen();
    Serial.println("BLE connected.");
  } else if (!connected && prevConnected) {
    // Just disconnected
    appState = STATE_WAITING;
    drawWaitingScreen();
    Serial.println("BLE disconnected — back to waiting.");
  }
  prevConnected = connected;

  // --- State behaviour ---
  if (appState == STATE_WAITING) {
    tickWaitingAnimation();
    return;
  }

  // --- Touch polling (only in CONNECTED state) ---
  if (!touch.tirqTouched() || !touch.touched()) return;

  TS_Point p = touch.getPoint();

  // Map raw ADC → screen pixels (calibration from Rui Santos reference)
  int tx = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 1, SCREEN_W);
  int ty = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 1, SCREEN_H);
  tx = constrain(tx, 0, SCREEN_W - 1);
  ty = constrain(ty, 0, SCREEN_H - 1);

  Serial.printf("Touch raw=(%d,%d) mapped=(%d,%d)\n", p.x, p.y, tx, ty);

  int row, col;
  if (getTouchedButton(tx, ty, &row, &col) != 0) return;

  const Button& btn = BUTTONS[row][col];
  Serial.printf("Button [%d][%d] — %s\n", row, col, btn.label);

  // Visual feedback: flash button highlight
  drawButton(row, col, true);
  delay(150);
  drawButton(row, col, false);

  // Send BLE media key.
  // btn.key is const uint8_t* (2 bytes). Cast to MediaKeyReport (uint8_t[2]) and
  // dereference so the write(const MediaKeyReport) overload is selected, not
  // write(uint8_t) or write(const uint8_t*, size_t), which send regular keyboard
  // character reports instead of Consumer Control reports and do nothing on iOS.
  if (connected) {
    bleKeyboard.write(*(const MediaKeyReport*)btn.key);
  }

  // Re-stamp status dot (button redraw may have partially overlapped it)
  drawStatusDot(true);

  // Debounce — ignore subsequent touch events for a short window
  delay(80);
}
