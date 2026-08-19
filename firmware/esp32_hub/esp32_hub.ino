#include "api_manager.h"
#include "audio_manager.h"
#include "button_manager.h"
#include "lcd_manager.h"
#include "led_manager.h"
#include "session_manager.h"
#include "state_machine.h"
#include "wifi_manager.h"

LedManager ledManager;
LcdManager lcdManager;
ButtonManager buttonManager;
WifiManager wifiManager;
ApiManager apiManager;
SessionManager sessionManager;
AudioManager audioManager;

StateMachine stateMachine(
    wifiManager,
    apiManager,
    lcdManager,
    ledManager,
    buttonManager,
    audioManager,
    sessionManager);

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("eLingo Hub firmware starting");

  stateMachine.begin();
}

void loop() {
  stateMachine.update();
}
