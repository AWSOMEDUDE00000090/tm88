#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>

//////////////////// USER SETTINGS ////////////////////
static const IPAddress ETH_LOCAL_IP(192,168,50,1);
static const IPAddress ETH_GATEWAY(192,168,50,1);
static const IPAddress ETH_SUBNET (255,255,255,0);

static const char* AP_SSID     = "PrintBox";
static const char* AP_PASSWORD = "printprint";

static const IPAddress PRINTER_IP(192,168,50,2);  // set your printer IP here
static const uint16_t PRINTER_PORT = 9100;

static const int   PRINT_WIDTH_DOTS = 512; // safe width for TM-T88IV 80mm
///////////////////////////////////////////////////////

AsyncWebServer server(80);

// --- Ethernet event hook (optional diagnostics) ---
static bool eth_up = false;
void WiFiEvent(WiFiEvent_t event) {
  if (event == ARDUINO_EVENT_ETH_GOT_IP) {
    eth_up = true;
    Serial.print("ETH IP: "); Serial.println(ETH.localIP());
  } else if (event == ARDUINO_EVENT_ETH_DISCONNECTED) {
    eth_up = false;
    Serial.println("ETH disconnected");
  }
}

// --- RAW TCP send helper ---
bool sendRawToPrinter(const uint8_t* data, size_t len) {
  WiFiClient client;
  if (!client.connect(PRINTER_IP, PRINTER_PORT)) return false;
  size_t sent = client.write(data, len);
  client.flush();
  client.stop();
  return sent == len;
}

// --- ESC/POS helpers ---
void esc_init(Stream &s)               { s.write(0x1B); s.write('@'); }             // ESC @
void esc_align_center(Stream &s, bool c){ s.write(0x1B); s.write('a'); s.write(c?1:0); }
void esc_bold(Stream &s, bool on)      { s.write(0x1B); s.write('E'); s.write(on?1:0); }
void esc_cut_full(Stream &s)           { s.write(0x1D); s.write('V'); s.write((uint8_t)0); }
void esc_linefeed(Stream &s, int n=1)  { for (int i=0;i<n;i++) s.write('\n'); }

// Build a small buffer of ESC/POS text and send
bool printText(const String &txt, bool center=false, bool bold=false) {
  // build in RAM then ship once
  String payload;
  payload.reserve(1024 + txt.length() + 8);
  payload += String("\x1B@"); // init
  payload += String("\x1Ba") + (char)(center?1:0);
  payload += String("\x1BE") + (char)(bold?1:0);
  payload += txt;
  payload += "\n\n";
  payload += String("\x1BE") + (char)0; // bold off
  payload += String("\x1Ba") + (char)0; // left
  payload += "\x1D" "V" "\x00";         // full cut

  return sendRawToPrinter((const uint8_t*)payload.c_str(), payload.length());
}

// Pack 1-bit image row-major (MSB left) to ESC/POS raster (GS v 0)
// img: 8-bit grayscale (0=black, 255=white)
// width <= PRINT_WIDTH_DOTS; height arbitrary
bool printBitmap(const uint8_t* img, int width, int height) {
  if (width <= 0 || height <= 0) return false;
  int bytes_per_row = (width + 7) / 8;
  const size_t raster_len = (size_t)bytes_per_row * height;
  std::unique_ptr<uint8_t[]> raster(new (std::nothrow) uint8_t[raster_len]);
  if (!raster) return false;

  // Threshold to 1-bit and pack
  for (int y=0; y<height; ++y) {
    uint8_t byte = 0; int bit = 7;
    for (int x=0; x<width; ++x) {
      uint8_t px = img[y*width + x];   // 0..255
      bool black = (px < 128);
      if (black) byte |= (1 << bit);
      if (--bit < 0) {
        raster[y*bytes_per_row + (x>>3)] = byte;
        byte = 0; bit = 7;
      }
    }
    if (bit != 7) { // flush partial
      raster[y*bytes_per_row + (width>>3)] = byte;
    }
  }

  // Build ESC/POS header + raster payload (GS v 0, m=0)
  // GS v 0 m xL xH yL yH [data]
  uint16_t x = bytes_per_row;
  uint16_t y = height;
  uint8_t header[8] = { 0x1D, 'v', '0', 0x00,
                        (uint8_t)(x & 0xFF), (uint8_t)(x >> 8),
                        (uint8_t)(y & 0xFF), (uint8_t)(y >> 8) };

  WiFiClient client;
  if (!client.connect(PRINTER_IP, PRINTER_PORT)) return false;
  client.write("\x1B@", 2); // init
  client.write(header, 8);
  client.write(raster.get(), raster_len);
  client.write("\n\n", 2);
  client.write("\x1D" "V" "\x00", 3); // cut
  client.flush();
  client.stop();
  return true;
}

// Very small PNG-to-grayscale decoder is out of scope here.
// Simpler: upload raw 1-bit or grayscale from the browser (client-side converted).
// For now, accept a tiny grayscale PNG and rely on client to send raw bytes.

///////////////////////////////////////////////////////

void setup() {
  Serial.begin(115200);
  delay(500);

  // Filesystem for the web UI
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }

  // Bring up Ethernet
  WiFi.onEvent(WiFiEvent);
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_CLK_MODE, ETH_PHY_TYPE);
  // Give it a moment before configuring static IP
  delay(300);
  ETH.config(ETH_LOCAL_IP, ETH_GATEWAY, ETH_SUBNET);

  // Wi-Fi AP for your phone/laptop
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  // Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send(LittleFS, "/index.html", "text/html");
  });

  // Print message
  server.on("/api/print-message", HTTP_POST, [](AsyncWebServerRequest* req) {},
    NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      // data is raw body. Expect JSON: { "text":"...", "center":true, "bold":false }
      StaticJsonDocument<2048> doc;
      DeserializationError e = deserializeJson(doc, data, len);
      if (e) { req->send(400, "application/json", "{\"err\":\"bad json\"}"); return; }
      String text = doc["text"] | "";
      bool center = doc["center"] | false;
      bool bold = doc["bold"] | false;
      bool ok = printText(text, center, bold);
      req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }
  );

  // Print to-do list (one item per line). We’ll format with a title and bullets.
  server.on("/api/print-todo", HTTP_POST, [](AsyncWebServerRequest* req) {},
    NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      StaticJsonDocument<4096> doc;
      if (deserializeJson(doc, data, len)) { req->send(400, "application/json", "{\"err\":\"bad json\"}"); return; }
      String title = doc["title"] | "To-Do";
      String items = doc["items"] | ""; // lines separated by \n
      String out;
      out.reserve(items.length() + 128);
      out += title + "\n";
      out += "------------------------------\n";
      int start = 0;
      while (true) {
        int nl = items.indexOf('\n', start);
        String line = (nl >= 0) ? items.substring(start, nl) : items.substring(start);
        line.trim();
        if (line.length() > 0) out += "[ ] " + line + "\n";
        if (nl < 0) break;
        start = nl + 1;
      }
      out += "\n";
      bool ok = printText(out, false, false);
      req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }
  );

  // Print preprocessed 1-bit bitmap from browser (client sends width,height, and raw bytes)
  server.on("/api/print-bitmap", HTTP_POST, [](AsyncWebServerRequest* req) {},
    NULL,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
      // Expect binary body: 4 bytes width (LE), 4 bytes height (LE), then width*height bytes grayscale 0..255
      if (len < 8) { req->send(400, "text/plain", "short"); return; }
      int w = (int)(data[0] | (data[1]<<8) | (data[2]<<16) | (data[3]<<24));
      int h = (int)(data[4] | (data[5]<<8) | (data[6]<<16) | (data[7]<<24));
      if (w <= 0 || h <= 0 || w > PRINT_WIDTH_DOTS) { req->send(400, "text/plain", "bad dims"); return; }
      size_t need = (size_t)w * h;
      if (len < 8 + need) { req->send(400, "text/plain", "short img"); return; }
      bool ok = printBitmap((const uint8_t*)(data+8), w, h);
      req->send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    }
  );

  server.begin();
  Serial.println("Server started");
}

void loop() {
  // nothing; AsyncWebServer handles requests
}
