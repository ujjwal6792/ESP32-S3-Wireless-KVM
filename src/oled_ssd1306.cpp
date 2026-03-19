#include "oled_ssd1306.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace oled_ssd1306 {
namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr int kHeaderHeight = 16; // yellow band on common 0.96" bicolor modules

Adafruit_SSD1306 g_display(kWidth, kHeight, &Wire, -1);
bool g_ready = false;
uint8_t g_addr = 0;

uint8_t g_mac_tail[3] = {0, 0, 0};
bool g_has_mac = false;

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

void drawHeader(const Status &s) {
  g_display.fillRect(0, 0, kWidth, kHeaderHeight, SSD1306_BLACK);
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_WHITE);
  g_display.setCursor(0, 0);

  // Keep header short so it fits the 16px band.
  g_display.print("S");
  g_display.print((int)s.slot_1based);
  g_display.print(" ");
  if (s.profile && s.profile[0] != '\0') {
    g_display.print(s.profile);
  } else {
    g_display.print("Profile");
  }

  // Right-side indicator: C / P / R
  const char *indicator = "R";
  if (s.connected) {
    indicator = "C";
  } else if (s.state && (strstr(s.state, "pair") || strstr(s.state, "PAIR"))) {
    indicator = "P";
  }

  int16_t x1, y1;
  uint16_t w, h;
  g_display.getTextBounds(indicator, 0, 0, &x1, &y1, &w, &h);
  g_display.setCursor(kWidth - (int)w - 2, 0);
  g_display.print(indicator);

  g_display.drawFastHLine(0, kHeaderHeight, kWidth, SSD1306_WHITE);
}

void drawBody(const Status &s) {
  g_display.fillRect(0, kHeaderHeight + 1, kWidth, kHeight - kHeaderHeight - 1,
                     SSD1306_BLACK);
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_WHITE);

  // Line 1: state
  g_display.setCursor(0, kHeaderHeight + 4);
  if (s.state && s.state[0] != '\0') {
    g_display.print(s.state);
  } else {
    g_display.print("...");
  }

  // Line 2: eco/normal
  g_display.setCursor(0, kHeaderHeight + 16);
  g_display.print(s.eco ? "ECO" : "ACTIVE");
  g_display.print("  ");
  g_display.print(s.connected ? "LINK" : "ADV");

  // Footer: MAC tail
  g_display.setCursor(0, kHeight - 8);
  if (g_has_mac) {
    char buf[16];
    snprintf(buf, sizeof(buf), "MAC ..:%02X:%02X:%02X", g_mac_tail[0],
             g_mac_tail[1], g_mac_tail[2]);
    g_display.print(buf);
  } else {
    g_display.print("MAC ..:..:..:..");
  }
}

void renderStatus(const Status &s) {
  drawHeader(s);
  drawBody(s);
  g_display.display();
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
    g_last.has_value = true;
    return true;
  }

  // Periodic refresh in case the panel glitches or contrast changes.
  return (millis() - g_lastDrawMs) > 5000;
}

bool probeAddr(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

} // namespace

bool begin(uint8_t sdaPin, uint8_t sclPin, uint8_t i2cAddr) {
  Wire.begin((int)sdaPin, (int)sclPin);
  // Start conservative; long dupont wires + breadboards often fail at 400kHz.
  Wire.setClock(100000);

  // Some SSD1306 modules are 0x3C, others are 0x3D.
  uint8_t addrToTry[2] = {i2cAddr, (uint8_t)(i2cAddr ^ 0x01)};
  if (i2cAddr == 0 || i2cAddr == 0xFF) {
    addrToTry[0] = 0x3C;
    addrToTry[1] = 0x3D;
  }

  uint8_t detected = 0;
  for (uint8_t a : addrToTry) {
    if (a != 0 && probeAddr(a)) {
      detected = a;
      break;
    }
  }

  if (detected == 0) {
    g_ready = false;
    g_addr = 0;
    return false;
  }

  if (!g_display.begin(SSD1306_SWITCHCAPVCC, detected)) {
    g_ready = false;
    g_addr = 0;
    return false;
  }

  g_display.clearDisplay();
  g_display.invertDisplay(false);
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_WHITE);
  g_display.setCursor(0, 0);
  g_display.print("OLED init...");
  g_display.display();

  g_ready = true;
  g_addr = detected;
  g_last.has_value = false;
  g_lastDrawMs = millis();
  return true;
}

uint8_t address() { return g_addr; }

void setBaseMac(const uint8_t mac[6]) {
  if (!mac)
    return;
  g_mac_tail[0] = mac[3];
  g_mac_tail[1] = mac[4];
  g_mac_tail[2] = mac[5];
  g_has_mac = true;
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
  g_display.clearDisplay();
  drawHeader(s);
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_WHITE);
  g_display.setCursor(0, kHeaderHeight + 10);
  g_display.print("Sleeping...");
  g_display.display();
  g_lastDrawMs = millis();
}

} // namespace oled_ssd1306
