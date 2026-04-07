#include "tft_display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

namespace tft_display {
namespace {

constexpr int kPanelWidth = 320;
constexpr int kPanelHeight = 240;

constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kPanel = 0x0000;
constexpr uint16_t kGlow = 0x07E0;
constexpr uint16_t kGlowDim = 0x03E0;
constexpr uint16_t kGlowDark = 0x01A0;

SPIClass g_spi(FSPI);
Adafruit_ST7789 *g_tft = nullptr;
Pins g_pins = {};
bool g_ready = false;
bool g_staticDrawn = false;
unsigned long g_lastAnimMs = 0;
uint8_t g_animPhase = 0;

struct RenderedState {
  uint8_t slot_1based = 0;
  bool eco = false;
  bool connected = false;
  char profile[20] = {0};
  char state[28] = {0};
  bool has_value = false;
};

RenderedState g_last;
unsigned long g_lastDrawMs = 0;

bool strEq(const char *a, const char *b) {
  if (a == nullptr)
    a = "";
  if (b == nullptr)
    b = "";
  return strcmp(a, b) == 0;
}

void strCopyTrunc(char *dst, size_t dstSize, const char *src) {
  if (dstSize == 0)
    return;
  if (src == nullptr)
    src = "";
  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

const char *linkLabel(const Status &s) {
  return s.connected ? "LOCKED" : "SEARCH";
}

const char *modeLabel(const Status &s) {
  return s.eco ? "ECO" : "ACTIVE";
}

uint16_t stateColor(const Status &s) {
  if (s.eco)
    return kGlowDim;
  return kGlow;
}

void drawBackground() {
  g_tft->fillScreen(kBg);
}

void drawFrame() {
  g_tft->drawRect(8, 8, kPanelWidth - 16, kPanelHeight - 16, kGlow);
  g_tft->drawRect(12, 12, kPanelWidth - 24, kPanelHeight - 24, kGlowDark);
  g_tft->fillRect(18, 18, kPanelWidth - 36, 24, kPanel);
  g_tft->drawRect(18, 18, kPanelWidth - 36, 24, kGlow);
  g_tft->fillRect(18, 54, 188, 152, kPanel);
  g_tft->drawRect(18, 54, 188, 152, kGlow);
  g_tft->fillRect(218, 54, 84, 152, kPanel);
  g_tft->drawRect(218, 54, 84, 152, kGlow);
}

void drawSignalBars(uint8_t slot1, bool connected) {
  int bars = connected ? 4 : (slot1 % 4) + 1;
  for (int i = 0; i < 4; i++) {
    int h = 14 + i * 16;
    int x = 242 + i * 12;
    int y = 188 - h;
    uint16_t color = (i < bars) ? kGlow : kGlowDark;
    g_tft->fillRect(x, y, 6, h, color);
  }
}

void clearRect(int16_t x, int16_t y, int16_t w, int16_t h) {
  g_tft->fillRect(x, y, w, h, kPanel);
}

void drawHeartbeat() {
  clearRect(230, 110, 54, 48);
  for (int i = 0; i < 3; i++) {
    int radius = (g_animPhase == i) ? 5 : 2;
    uint16_t color = (g_animPhase == i) ? kGlow : kGlowDark;
    g_tft->fillCircle(242 + i * 16, 132, radius, color);
  }
}

void drawStaticLayout() {
  drawBackground();
  drawFrame();

  g_tft->setTextWrap(false);
  g_tft->setTextColor(kGlow);
  g_tft->setTextSize(2);
  g_tft->setCursor(28, 22);
  g_tft->print("VAULT-TEC LINK");

  g_tft->setTextSize(1);
  g_tft->setCursor(28, 64);
  g_tft->print("PROFILE");

  g_tft->setTextSize(1);
  g_tft->setTextColor(kGlow);
  g_tft->setCursor(28, 140);
  g_tft->print("USB HOST TO BLE RELAY ONLINE");

  g_tft->setTextSize(1);
  g_tft->setTextColor(kGlowDim);
  g_tft->setCursor(28, 164);
  g_tft->print("STATE");
  g_tft->setCursor(28, 188);
  g_tft->print("MODE");
  g_tft->setCursor(28, 212);
  g_tft->print("LINK");
  g_tft->setCursor(230, 64);
  g_tft->print("SIGNAL");
  g_tft->setCursor(230, 96);
  g_tft->print("STATUS");
  g_tft->setCursor(230, 120);
  g_tft->print("ANIM");
  g_staticDrawn = true;
}

void drawProfile(const Status &s) {
  clearRect(28, 92, 174, 34);
  g_tft->setTextSize(3);
  g_tft->setTextColor(kGlow);
  g_tft->setCursor(28, 92);
  g_tft->print("S");
  g_tft->print((int)s.slot_1based);
  g_tft->print(" ");
  g_tft->print((s.profile && s.profile[0]) ? s.profile : "Unknown");
}

void drawStateField(const Status &s) {
  clearRect(84, 160, 118, 18);
  g_tft->setTextSize(2);
  g_tft->setTextColor(stateColor(s));
  g_tft->setCursor(84, 160);
  g_tft->print((s.state && s.state[0]) ? s.state : "...");
}

void drawModeField(const Status &s) {
  clearRect(84, 184, 118, 18);
  g_tft->setTextColor(s.eco ? kGlowDim : kGlow);
  g_tft->setCursor(84, 184);
  g_tft->print(modeLabel(s));
}

void drawLinkField(const Status &s) {
  clearRect(84, 208, 118, 18);
  g_tft->setTextColor(s.connected ? kGlow : kGlowDim);
  g_tft->setCursor(84, 208);
  g_tft->print(linkLabel(s));
}

void drawStatusField(const Status &s) {
  clearRect(230, 180, 72, 18);
  g_tft->setCursor(230, 180);
  g_tft->setTextColor(s.connected ? kGlow : kGlowDim);
  g_tft->print(s.connected ? "ON" : "SCAN");
}

void renderStatus(const Status &s) {
  if (!g_staticDrawn) {
    drawStaticLayout();
  }
  drawProfile(s);
  drawStateField(s);
  drawModeField(s);
  drawLinkField(s);
  clearRect(238, 110, 54, 84);
  drawSignalBars(s.slot_1based, s.connected);
  drawStatusField(s);
  drawHeartbeat();
  g_lastDrawMs = millis();
}

bool shouldRedraw(const Status &s) {
  RenderedState next;
  next.slot_1based = s.slot_1based;
  next.eco = s.eco;
  next.connected = s.connected;
  strCopyTrunc(next.profile, sizeof(next.profile), s.profile);
  strCopyTrunc(next.state, sizeof(next.state), s.state);

  if (!g_last.has_value) {
    g_last = next;
    g_last.has_value = true;
    return true;
  }

  bool changed = false;
  changed |= (g_last.slot_1based != next.slot_1based);
  changed |= (g_last.eco != next.eco);
  changed |= (g_last.connected != next.connected);
  changed |= !strEq(g_last.profile, next.profile);
  changed |= !strEq(g_last.state, next.state);

  if (changed) {
    g_last = next;
    return true;
  }

  if (millis() - g_lastAnimMs >= 250) {
    g_lastAnimMs = millis();
    g_animPhase = (g_animPhase + 1) % 3;
    drawHeartbeat();
  }

  return false;
}

} // namespace

bool begin(const Pins &pins) {
  g_pins = pins;
  pinMode(g_pins.cs, OUTPUT);
  pinMode(g_pins.dc, OUTPUT);
  digitalWrite(g_pins.cs, HIGH);
  digitalWrite(g_pins.dc, HIGH);

  if (g_pins.rst >= 0) {
    pinMode(g_pins.rst, OUTPUT);
    digitalWrite(g_pins.rst, HIGH);
  }

  if (g_pins.backlight >= 0) {
    pinMode(g_pins.backlight, OUTPUT);
    digitalWrite(g_pins.backlight, HIGH);
  }

  g_spi.begin(g_pins.sck, g_pins.miso, g_pins.mosi, g_pins.cs);
  if (g_tft != nullptr) {
    delete g_tft;
    g_tft = nullptr;
  }

  g_tft = new Adafruit_ST7789(&g_spi, g_pins.cs, g_pins.dc, g_pins.rst);
  g_tft->init(240, 320);
  g_tft->setRotation(1);
  g_tft->invertDisplay(false);
  g_tft->fillScreen(kBg);

  g_ready = true;
  g_staticDrawn = false;
  g_last.has_value = false;
  g_lastDrawMs = 0;
  g_lastAnimMs = millis();
  g_animPhase = 0;
  return true;
}

bool ready() { return g_ready; }

void showBootScreen() {
  if (!g_ready)
    return;

  drawStaticLayout();
  g_tft->setTextWrap(false);
  g_tft->setTextColor(kGlow);
  g_tft->setTextSize(3);
  g_tft->setCursor(64, 72);
  g_tft->print("PIP-LINK");
  g_tft->setTextSize(2);
  g_tft->setCursor(86, 108);
  g_tft->print("BOOTING...");
  g_tft->setTextSize(1);
  g_tft->setCursor(74, 146);
  g_tft->print("USB KEYBOARD BRIDGE");
  g_tft->setCursor(74, 160);
  g_tft->print("ST7789 320x240 SPI");
  g_tft->setCursor(74, 174);
  g_tft->print("CS10 DC14 RST15");
  g_tft->setCursor(74, 188);
  g_tft->print("SCK12 MOSI11 MISO13");
  g_tft->setCursor(74, 202);
  g_tft->print("BL21");
  g_staticDrawn = false;
  g_last.has_value = false;
  g_lastDrawMs = millis();
}

void showStatus(const Status &s) {
  if (!g_ready)
    return;
  if (shouldRedraw(s)) {
    renderStatus(s);
  }
}

void showSleep(const Status &s) {
  if (!g_ready)
    return;

  drawStaticLayout();
  g_tft->setTextWrap(false);
  g_tft->setTextColor(kGlowDim);
  g_tft->setTextSize(3);
  g_tft->setCursor(74, 84);
  g_tft->print("STANDBY");
  g_tft->setTextSize(2);
  g_tft->setCursor(86, 122);
  g_tft->print("SLOT ");
  g_tft->print((int)s.slot_1based);
  g_tft->setCursor(86, 150);
  g_tft->print((s.profile && s.profile[0]) ? s.profile : "Profile");
  g_tft->setTextSize(1);
  g_tft->setCursor(84, 186);
  g_tft->print("WAKE WITH BOOT BUTTON");
  g_staticDrawn = false;
  g_last.has_value = false;
  g_lastDrawMs = millis();
}

} // namespace tft_display
