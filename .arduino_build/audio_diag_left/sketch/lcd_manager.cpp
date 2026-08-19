#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\lcd_manager.cpp"
#include "lcd_manager.h"

#include <Wire.h>

#include "config.h"
#include "pins.h"

LcdManager::LcdManager()
    : lcd_(Config::LCD_I2C_ADDRESS, Config::LCD_COLUMNS, Config::LCD_ROWS),
      currentLine1_(""),
      currentLine2_("") {}

void LcdManager::begin() {
  Wire.begin(Pins::LCD_SDA, Pins::LCD_SCL);
  lcd_.init();
  lcd_.backlight();
  lcd_.clear();
}

void LcdManager::showBooting() {
  showLines("eLingo Hub", "Booting...");
}

void LcdManager::showConnecting() {
  showLines("Connecting...", "");
}

void LcdManager::showReady() {
  showLines("eLingo Hub", "Ready");
}

void LcdManager::showServerOffline() {
  showLines("Server", "Offline");
}

void LcdManager::showJoin(const String &sessionId) {
  showLines("Join", sessionId);
}

void LcdManager::showRecording(const String &sessionId) {
  showLines("REC", sessionId);
}

void LcdManager::showEnding() {
  showLines("Ending", "Session...");
}

void LcdManager::showUploading(uint32_t chunkNumber) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "Chunk %lu", static_cast<unsigned long>(chunkNumber));
  showLines("Uploading", buffer);
}

void LcdManager::showUploadRetry(uint32_t chunkNumber) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "Retry %lu", static_cast<unsigned long>(chunkNumber));
  showLines("Upload Failed", buffer);
}

void LcdManager::showWifiLost() {
  showLines("WiFi", "Reconnecting");
}

void LcdManager::showMessage(const String &line1, const String &line2) {
  showLines(line1, line2);
}

void LcdManager::showLines(const String &line1, const String &line2) {
  String fittedLine1 = fitLine(line1);
  String fittedLine2 = fitLine(line2);

  if (fittedLine1 == currentLine1_ && fittedLine2 == currentLine2_) {
    return;
  }

  lcd_.setCursor(0, 0);
  lcd_.print(fittedLine1);
  lcd_.setCursor(0, 1);
  lcd_.print(fittedLine2);

  currentLine1_ = fittedLine1;
  currentLine2_ = fittedLine2;
}

String LcdManager::fitLine(const String &line) const {
  String fitted = line;
  if (fitted.length() > Config::LCD_COLUMNS) {
    fitted = fitted.substring(0, Config::LCD_COLUMNS);
  }

  while (fitted.length() < Config::LCD_COLUMNS) {
    fitted += ' ';
  }

  return fitted;
}
