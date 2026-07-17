#include <WiFi.h>
#include <WiFiClientSecure.h>
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

const char* FIRMWARE_VERSION = "v4.1.0";
DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

const byte DNS_PORT = 53;
IPAddress apIP(4, 3, 2, 1);
String savedSSID = "";
String savedPass = "";

// News Aggregator Control Variables
String headlineText = "AWAITING HEADLINES DATA UPDATE LOCK...";
int scrollSpeedMs = 60; 
int scrollSpeedPercent = 50; 
unsigned long lastNewsFetchTime = 0;
int scrollPosition = 0;

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

void renderScrollTicker(String text, int pixelOffset) {
    clearOLEDFB();
    for(int x = 0; x < 128; x++) {
        setFBLocalPixel(x, 12, true);
        setFBLocalPixel(x, 50, true);
    }
    int charWidth = 6;
    for (int i = 0; i < text.length(); i++) {
        char val = text[i];
        int idx = 0;
        if (val >= 32 && val <= 90) idx = val - 32;
        else if (val >= 97 && val <= 122) idx = val - 64; 
        int charPixelXStart = (i * charWidth) - pixelOffset;
        if (charPixelXStart >= -6 && charPixelXStart < 128) {
            for (int col = 0; col < 5; col++) {
                int targetX = charPixelXStart + col;
                for (int bit = 0; bit < 8; bit++) {
                    if ((ssd1306_font[idx][col] >> bit) & 0x01) {
                        setFBLocalPixel(targetX, 26 + bit, true);
                    }
                }
            }
        }
    }
    renderFBToDisplay();
}

void handleRootPortal() {
    String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:20px;}";
    html += "input[type=text],input[type=password],input[type=file]{width:90%;padding:10px;margin:10px 0;border-radius:4px;border:none;}";
    html += "input[type=submit]{background:#00b4d8;color:#fff;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;width:90%;font-size:16px; margin-top:10px;}";
    html += ".slider-box{background:#333;padding:20px;margin:15px auto;width:85%;border-radius:4px;text-align:left;}";
    html += "input[type=range]{width:100%;margin-top:10px;cursor:pointer;}</style></head>";
    html += "<body><h2>NEWS_READER Ticker</h2>";
    html += "<span style='font-size:14px;color:#888;'>Running: " + String(FIRMWARE_VERSION) + "</span><br><br>";
    
    // Cleaned up the text encoding symbols
    html += "<div class='slider-box'><form action='/speed' method='POST'>";
    html += "<b>Text Scroll Speed Velocity:</b> " + String(scrollSpeedPercent) + "%<br>";
    html += "<input type='range' name='speed_val' min='10' max='100' value='" + String(scrollSpeedPercent) + "' onchange='this.form.submit()'>";
    html += "<input type='submit' style='display:none;'></form></div>";
    
    html += "<div class='slider-box'><b>Active Feed Headline:</b><br><span style='color:#00f5d4; font-family:monospace;'>" + headlineText + "</span></div>";
    
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

void handleSpeedControl() {
    if (server.hasArg("speed_val")) {
        scrollSpeedPercent = server.arg("speed_val").toInt();
        scrollSpeedMs = map(scrollSpeedPercent, 10, 100, 150, 15);
    }
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleSavePortal() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    preferences.begin("wled_conf", false);
    if(ssid.length() > 0) preferences.putString("ssid", ssid);
    if(pass.length() > 0) preferences.putString("pass", pass);
    preferences.end();
    server.send(200, "text/html", "<html><body><h3>Settings Saved!</h3></body></html>");
    delay(1000); ESP.restart();
}

// Independent FreeRTOS Task locked strictly to Core 0 for smooth scrolling animation
void tickerWorkerTask(void * pvParameters) {
    while(1) {
        int textPixelWidth = headlineText.length() * 6;
        scrollPosition++;
        if (scrollPosition > textPixelWidth) {
            scrollPosition = -128; 
        }
        renderScrollTicker(headlineText, scrollPosition);
        vTaskDelay(pdMS_TO_TICKS(scrollSpeedMs)); // Safe multi-threaded OS delay block
    }
}

void setup() {
    Serial.begin(115200);
    initOLED(); clearOLEDFB();

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
        headlineText = "AP MODE ACTIVE - GO TO 4.3.2.1";
    } else {
        headlineText = "SYSTEM ONLINE - IP: " + WiFi.localIP().toString();
    }

    server.on("/", handleRootPortal);
    server.on("/save", HTTP_POST, handleSavePortal);
    server.on("/speed", HTTP_POST, handleSpeedControl);
    server.on("/generate_204", handleRootPortal);
    server.on("/fwlink", handleRootPortal);
    
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        server.send(200, "text/html", "<h3>Success!</h3>");
        delay(1000); ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); }
        else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); }
        else if (upload.status == UPLOAD_FILE_END) { Update.end(true); }
    });

    server.onNotFound(handleRootPortal); server.begin();

    // Spawn the ticker worker task on Core 0
    xTaskCreatePinnedToCore(tickerWorkerTask, "TickerTask", 4096, NULL, 1, NULL, 0);
}

void loop() {
    server.handleClient();
    if (WiFi.status() != WL_CONNECTED) { dnsServer.processNextRequest(); delay(2); return; }

    // Core 1 is now 100% free to fetch RSS feeds over HTTPS cleanly every 60 seconds
    if (millis() - lastNewsFetchTime > 60000 || lastNewsFetchTime == 0) {
        lastNewsFetchTime = millis();
        
        WiFiClientSecure client;
        client.setInsecure(); 
        
        HTTPClient http;
        http.begin(client, "https://rss2json.com");
        int httpCode = http.GET();
        
        if (httpCode == HTTP_CODE_OK) {
            String payload = http.getString();
            int titleIdx = payload.indexOf("\"title\":\"");
            if (titleIdx != -1) {
                int firstStoryIdx = payload.indexOf("\"title\":\"", titleIdx + 10);
                if (firstStoryIdx != -1) {
                    int endQuote = payload.indexOf("\"", firstStoryIdx + 9);
                    String newsItem = payload.substring(firstStoryIdx + 9, endQuote);
                    
                    newsItem.replace("\\\"", "\"");
                    newsItem.replace("\\u0027", "'");
                    newsItem.trim();
                    
                    if (newsItem.length() > 0) {
                        headlineText = "BREAKING NEWS: " + newsItem + "  ***  ";
                        headlineText.toUpperCase(); 
                        scrollPosition = -128;      
                    }
                }
            }
        }
        http.end();
    }
}
