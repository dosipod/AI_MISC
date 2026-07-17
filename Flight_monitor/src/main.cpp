#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Wire.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include "WiFiManager.h"
#include "font.h"

#define OLED_SDA 5
#define OLED_SCL 4
#define OLED_ADDR 0x3C

const char* FIRMWARE_VERSION = "v3.0.0";
DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

const byte DNS_PORT = 53;
IPAddress apIP(4, 3, 2, 1);
String savedSSID = "";
String savedPass = "";

int activeFlightsFound = 0;
String leadCallsign = "NONE";
String leadAltitude = "0m";
unsigned long lastFlightFetchTime = 0;

uint8_t oledFB[1024];

void sendOLEDCommand(uint8_t cmd) {
    Wire.beginTransmission(OLED_ADDR);
    Wire.write(0x00); Wire.write(cmd);
    Wire.endTransmission();
}

void initOLED() {
    Wire.begin(OLED_SDA, OLED_SCL, 400000);
    delay(10);
    sendOLEDCommand(0xAE); sendOLEDCommand(0xD5); sendOLEDCommand(0x80);
    sendOLEDCommand(0xA8); sendOLEDCommand(0x3F); sendOLEDCommand(0xD3);
    sendOLEDCommand(0x00); sendOLEDCommand(0x40); sendOLEDCommand(0x8D);
    sendOLEDCommand(0x14); sendOLEDCommand(0x20); sendOLEDCommand(0x00);
    sendOLEDCommand(0xA1); sendOLEDCommand(0xC8); sendOLEDCommand(0xDA);
    sendOLEDCommand(0x12); sendOLEDCommand(0x81); sendOLEDCommand(0xCF);
    sendOLEDCommand(0xD9); sendOLEDCommand(0xF1); sendOLEDCommand(0xDB);
    sendOLEDCommand(0x40); sendOLEDCommand(0xA4); sendOLEDCommand(0xA6);
    sendOLEDCommand(0xAF);
}

void clearOLEDFB() {
    memset(oledFB, 0, sizeof(oledFB));
}

void renderFBToDisplay() {
    sendOLEDCommand(0x21); sendOLEDCommand(0); sendOLEDCommand(127);
    sendOLEDCommand(0x22); sendOLEDCommand(0); sendOLEDCommand(7);
    for (int i = 0; i < 1024; i++) {
        Wire.beginTransmission(OLED_ADDR); Wire.write(0x40);
        for (int j = 0; j < 16 && i < 1024; j++, i++) { Wire.write(oledFB[i]); }
        i--; Wire.endTransmission();
    }
}

void setFBLocalPixel(int x, int y, bool turnOn) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int byteIdx = x + (y / 8) * 128;
    int bitIdx = y % 8;
    if (turnOn) oledFB[byteIdx] |= (1 << bitIdx);
    else        oledFB[byteIdx] &= ~(1 << bitIdx);
}

void printOLEDString(const char* str, uint8_t line) {
    if (line > 7) return;
    for (int x = 0; x < 128; x++) {
        int byteIdx = x + line * 128;
        if (byteIdx < 1024) oledFB[byteIdx] = 0;
    }
    int charCount = 0;
    while (*str && charCount < 16) {
        char val = *str++;
        int idx = 0;
        if (val >= 32 && val <= 90) idx = val - 32;
        else if (val >= 97 && val <= 122) idx = val - 64; 
        for (int i = 0; i < 5; i++) {
            int targetX = (charCount * 6) + i;
            for (int bit = 0; bit < 8; bit++) {
                if ((ssd1306_font[idx][i] >> bit) & 0x01) {
                    setFBLocalPixel(targetX, (line * 8) + bit, true);
                }
            }
        }
        charCount++;
    }
    renderFBToDisplay();
}

void handleRootPortal() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:20px;}";
    html += "input[type=text],input[type=password],input[type=file]{width:90%;padding:10px;margin:10px 0;border-radius:4px;border:none;}";
    html += "input[type=submit]{background:#00b4d8;color:#fff;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;width:90%;font-size:16px; margin-top:10px;}";
    html += ".btn-reboot{background:#d90429 !important; margin-bottom: 20px;}";
    html += ".box{background:#333;padding:15px;margin:15px auto;width:90%;border-radius:4px;text-align:left;}";
    html += ".tracker{background:#111;border-left:5px solid #00f5d4;color:#ffb703;font-family:monospace;padding:12px;margin:15px auto;width:90%;border-radius:4px;text-align:left;}</style></head>";
    html += "<body><h2>London Flight Monitor</h2>";
    html += "<span style='font-size:14px;color:#888;'>Running: " + String(FIRMWARE_VERSION) + "</span><br>";
    html += "<form action='/reboot' method='POST'><input type='submit' class='btn-reboot' value='Force System Reboot'></form>";
    html += "<div class='box'><b>Status Parameters:</b><br>Local IP: " + WiFi.localIP().toString() + "</div>";
    html += "<div class='tracker'><b>✈ Live Sky Radar Analytics:</b><br>";
    html += "• Flights in Airspace: " + String(activeFlightsFound) + "<br>";
    html += "• Nearest Callsign   : " + leadCallsign + "<br>";
    html += "• Transponder Altitude: " + leadAltitude + "</div>";
    html += "<form action='/save' method='POST'>";
    html += "<h3>1. Network Profile</h3>";
    html += "SSID:<br><input type='text' name='ssid' value='" + savedSSID + "' placeholder='Network Name'><br>";
    html += "Password:<br><input type='password' name='pass' value='" + savedPass + "' placeholder='Password'><br>";
    html += "<input type='submit' value='Apply Parameters & Reboot'></form><br><hr style='border:1px solid #444; width:90%;'>";
    html += "<h3>2. Wireless OTA Update</h3>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update'><br>";
    html += "<input type='submit' value='Upload New Binary File'></form></body></html>";
    server.send(200, "text/html", html);
}

void handleSavePortal() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    preferences.begin("wled_conf", false);
    if(ssid.length() > 0) preferences.putString("ssid", ssid);
    if(pass.length() > 0) preferences.putString("pass", pass);
    preferences.end();
    
    String redirectHtml = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:40px;}</style>";
    redirectHtml += "<script>setTimeout(function(){ window.location.href = '/'; }, 5000);</script></head>";
    redirectHtml += "<body><h3>Configurations Saved!</h3><p>Applying parameters. Resetting device...</p></body></html>";
    server.send(200, "text/html", redirectHtml);
    delay(1000); ESP.restart();
}

void handleReboot() {
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:40px;}</style>";
    html += "<script>setTimeout(function(){ window.location.href = '/'; }, 6000);</script></head>";
    html += "<body><h3>Rebooting...</h3></body></html>";
    server.send(200, "text/html", html);
    delay(1000); ESP.restart();
}

void setup() {
    Serial.begin(115200);
    initOLED(); clearOLEDFB();
    printOLEDString("BOOTING RADAR...", 2);

    preferences.begin("wled_conf", true);
    savedSSID = preferences.getString("ssid", "");
    savedPass = preferences.getString("pass", "");
    preferences.end();

    if (savedSSID.length() > 0) {
        WiFi.mode(WIFI_AP_STA); WiFi.begin(savedSSID.c_str(), savedPass.c_str());
        long startMs = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startMs < 8000) { delay(500); }
    }

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_AP_STA);
        IPAddress gateway(4, 3, 2, 1); IPAddress subnet(255, 255, 255, 0);
        WiFi.softAPConfig(apIP, gateway, subnet); WiFi.softAP("esp32_hw724");
        dnsServer.start(DNS_PORT, "*", apIP);
        clearOLEDFB();
        printOLEDString("CONNECT TO AP", 1);
        printOLEDString("esp32_hw724", 3);
        printOLEDString("GO TO: 4.3.2.1", 5);
    } else {
        clearOLEDFB();
        printOLEDString("RADAR ONLINE", 1);
        printOLEDString("IP ADDRESS:", 3);
        printOLEDString(WiFi.localIP().toString().c_str(), 5);
    }

    server.on("/", handleRootPortal);
    server.on("/save", HTTP_POST, handleSavePortal);
    server.on("/reboot", HTTP_POST, handleReboot);
    server.on("/generate_204", handleRootPortal);
    server.on("/fwlink", handleRootPortal);
    
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        String otaHtml = "<html><head><script>setTimeout(function(){ window.location.href = '/'; }, 6000);</script></head><body><h3>Update Success!</h3></body></html>";
        server.send(200, "text/html", otaHtml);
        delay(1000); ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
        else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); }
        else if (upload.status == UPLOAD_FILE_END) { Update.end(true); }
    });

    server.onNotFound(handleRootPortal); server.begin();
}

void loop() {
    server.handleClient();
    if (WiFi.status() != WL_CONNECTED) { dnsServer.processNextRequest(); delay(2); return; }

    if (millis() - lastFlightFetchTime > 20000 || lastFlightFetchTime == 0) {
        lastFlightFetchTime = millis();
        HTTPClient http;
        // Bounding Box strictly framing Greater London Airspace parameters
        http.begin("http://opensky-network.org");
        int httpCode = http.GET();
        
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            int searchPos = 0; int count = 0;
            while ((searchPos = payload.indexOf("[\"", searchPos)) != -1) { count++; searchPos += 2; if (count > 200) break; }
            activeFlightsFound = count;
            
            int firstCallsignPos = payload.indexOf("[\"");
            if (firstCallsignPos != -1) {
                int startQuote = payload.indexOf("\"", firstCallsignPos + 2);
                if (startQuote != -1) {
                    leadCallsign = payload.substring(firstCallsignPos + 2, startQuote);
                    leadCallsign.trim();
                    if(leadCallsign.length() == 0) leadCallsign = "N/A";
                }
                int commaCount = 0; int altSeek = firstCallsignPos;
                while (commaCount < 7 && altSeek < payload.length()) { altSeek = payload.indexOf(",", altSeek + 1); commaCount++; }
                if (altSeek != -1) {
                    int nextComma = payload.indexOf(",", altSeek + 1);
                    String altRaw = payload.substring(altSeek + 1, nextComma);
                    altRaw.trim();
                    if (altRaw != "null" && altRaw.length() > 0) { leadAltitude = altRaw + "m"; }
                    else { leadAltitude = "GND"; }
                }
            } else {
                leadCallsign = "NONE"; leadAltitude = "0m";
            }
            clearOLEDFB();
            printOLEDString("LONDON RADAR", 0);
            String countStr = "ACTIVE AC: " + String(activeFlightsFound);
            printOLEDString(countStr.c_str(), 2);
            String callStr = "CALL: " + leadCallsign;
            printOLEDString(callStr.c_str(), 4);
            String altStr = "ALT : " + leadAltitude;
            printOLEDString(altStr.c_str(), 6);
        }
        http.end();
    }
}
