#include "oled_ssd1306.h"

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace oled_ssd1306 {
namespace {

constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr int kSidebarWidth = 16;
constexpr int kTopMargin = 5;
constexpr int kBodyX = kSidebarWidth + 4;
constexpr int kBodyWidth = 34;

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
  int displayHeight = g_display.height();
  g_display.fillRect(0, 0, kSidebarWidth, displayHeight, SSD1306_BLACK);

  g_display.drawFastVLine(kSidebarWidth, 0, displayHeight, SSD1306_WHITE);

  // Active tab: Bluetooth.
  g_display.drawRoundRect(1, kTopMargin, 13, 18, 3, SSD1306_WHITE);
  g_display.fillRoundRect(2, kTopMargin + 1, 11, 16, 3, SSD1306_WHITE);
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_BLACK);
  g_display.setCursor(4, kTopMargin + 6);
  g_display.print("B");
}

void drawBody(const Status &s) {
  int h = g_display.height();
  g_display.fillRect(kSidebarWidth + 2, 0, kBodyWidth, h, SSD1306_BLACK);
  g_display.setTextSize(1);
  g_display.setTextColor(SSD1306_WHITE);
  g_display.setTextWrap(false);

  g_display.setCursor(kBodyX, kTopMargin + 1);
  g_display.print("stat");
  g_display.setCursor(kBodyX, kTopMargin + 13);
  g_display.print("con:");
  g_display.print(s.connected ? "y" : "n");

  g_display.setCursor(kBodyX, kTopMargin + 33);
  g_display.print("slot");

  g_display.setCursor(kBodyX, kTopMargin + 47);
  if (s.profile && s.profile[0] != '\0') {
    if (strcmp(s.profile, "Mac") == 0) {
      g_display.print("mac");
    } else if (strcmp(s.profile, "Windows") == 0) {
      g_display.print("win");
    } else if (strcmp(s.profile, "Linux") == 0) {
      g_display.print("linux");
    } else if (strcmp(s.profile, "Android") == 0) {
      g_display.print("phone");
    } else {
      char profileBuf[6];
      strCopyTrunc(profileBuf, sizeof(profileBuf), s.profile);
      g_display.print(profileBuf);
    }
  } else {
    g_display.print("unk");
  }

  g_display.setCursor(kBodyX, h - 10);
  g_display.print("     ");
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
  // Rotate clockwise so the physical yellow strip sits on the left sidebar.
  g_display.setRotation(1);
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
  g_display.setCursor(kSidebarWidth + 4, 24);
  g_display.print("Sleeping...");
  g_display.display();
  g_lastDrawMs = millis();
}

} // namespace oled_ssd1306
