#line 1 "C:\\Users\\Wits student\\Desktop\\eLingo Hub\\elingohubMVP\\firmware\\esp32_hub\\lcd_manager.h"
#ifndef ELINGO_LCD_MANAGER_H
#define ELINGO_LCD_MANAGER_H

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>

class LcdManager {
 public:
  LcdManager();

  void begin();
  void showBooting();
  void showConnecting();
  void showReady();
  void showServerOffline();
  void showJoin(const String &sessionId);
  void showRecording(const String &sessionId);
  void showEnding();
  void showUploading(uint32_t chunkNumber);
  void showUploadRetry(uint32_t chunkNumber);
  void showWifiLost();
  void showMessage(const String &line1, const String &line2);

 private:
  void showLines(const String &line1, const String &line2);
  String fitLine(const String &line) const;

  LiquidCrystal_I2C lcd_;
  String currentLine1_;
  String currentLine2_;
};

#endif
