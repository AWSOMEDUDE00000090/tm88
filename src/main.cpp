#include <Arduino.h>
#include <ETH.h>

#define PRINTER_IP   IPAddress(192,168,50,2)
#define PRINTER_PORT 9100

// Build flags provide these:
// ETH_PHY_TYPE=ETH_PHY_LAN8720
// ETH_PHY_ADDR=1
// ETH_PHY_MDC=23
// ETH_PHY_MDIO=18
// ETH_PHY_POWER=16
// ETH_CLK_MODE=ETH_CLOCK_GPIO0_IN

void WiFiEvent(WiFiEvent_t e) {
  if (e == ARDUINO_EVENT_ETH_GOT_IP) {
    Serial.print("ETH IP: "); Serial.println(ETH.localIP());

    // Send a hello as soon as link is up
    WiFiClient c;
    if (c.connect(PRINTER_IP, PRINTER_PORT)) {
      c.print("Hello from WT32-ETH01\n\n\n");
      // Full cut: GS V 0
      c.write(0x1D); c.write('V'); c.write((uint8_t)0);
      c.flush(); c.stop();
      Serial.println("Printed hello.");
    } else {
      Serial.println("Connect to printer failed.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.onEvent(WiFiEvent);

  bool ok = ETH.begin(ETH_PHY_ADDR,
                      ETH_PHY_POWER,
                      ETH_PHY_MDC,
                      ETH_PHY_MDIO,
                      (eth_clock_mode_t)ETH_CLK_MODE,
                      (eth_phy_type_t)ETH_PHY_TYPE);
  if (!ok) Serial.println("ETH.begin failed");

  delay(300);
  ETH.config(IPAddress(192,168,50,1), IPAddress(192,168,50,1), IPAddress(255,255,255,0));
  Serial.println("ETH starting…");
}

void loop() {}
