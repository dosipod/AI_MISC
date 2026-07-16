#include <WiFi.h>
#include <WiFiUdp.h>
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

#define WLED_UDP_PORT 21324
#define BUFFER_SIZE 1500

const char* FIRMWARE_VERSION = "v2.2.0";
IPAddress multicastIP(239, 0, 0, 1);

WiFiUDP udp;
uint8_t packetBuffer[BUFFER_SIZE];
DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

const byte DNS_PORT = 53;
IPAddress apIP(4, 3, 2, 1);
String savedSSID = "";
String savedPass = "";
String targetWledIP = "0.0.0.0";

volatile uint32_t packetCount = 0;
volatile int lastPacketSize = 0;
String lastSenderIP = "None Detected";
String detectedProtocol = "Listening on working WLED Port 21324...";

bool clearBootScreenFlag = true;

// 1024-Byte Local Offline Framebuffer mapping array (128x64 / 8)
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
        Wire.beginTransmission(OLED_ADDR);
        Wire.write(0x40);
        for (int j = 0; j < 16 && i < 1024; j++, i++) {
            Wire.write(oledFB[i]);
        }
        i--;
        Wire.endTransmission();
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
    html += ".debug{background:#111;border-left:5px solid #ffb703;color:#00f5d4;font-family:monospace;padding:12px;margin:15px auto;width:90%;border-radius:4px;text-align:left;}</style></head>";
    html += "<body><h2>HW-724 Dashboard</h2>";
    html += "<span style='font-size:14px;color:#888;'>Running: " + String(FIRMWARE_VERSION) + "</span><br>";
    html += "<form action='/reboot' method='POST'><input type='submit' class='btn-reboot' value='Force System Reboot'></form>";
    html += "<div class='box'><b>Status Parameters:</b><br>";
    html += "Local IP Address: " + WiFi.localIP().toString() + "<br>";
    html += "Configured Target WLED IP: " + targetWledIP + "</div>";
    html += "<div class='debug'><b>[ UDP Port 21324 Live Monitor ]</b><br>";
    html += "  Total Received Packets: " + String(packetCount) + "<br>";
    html += "  Last Packet Data Size: " + String(lastPacketSize) + " bytes<br>";
    html += "  Last Active Sender IP: " + lastSenderIP + "<br>";
    html += "  Detected Stream Mode : " + detectedProtocol + "</div>";
    html += "<form action='/save' method='POST'>";
    html += "<h3>1. Network Profile</h3>";
    html += "Wi-Fi SSID:<br><input type='text' name='ssid' value='" + savedSSID + "' placeholder='Network Name'><br>";
    html += "Password:<br><input type='password' name='pass' value='" + savedPass + "' placeholder='Password'><br>";
    html += "<h3>2. WLED Capture Settings</h3>";
    html += "Master WLED IP Address:<br><input type='text' name='wled_ip' value='" + targetWledIP + "' placeholder='e.g., 10.0.6.50'><br>";
    html += "<input type='submit' value='Apply Parameters & Reboot'></form><br><hr style='border:1px solid #444; width:90%;'>";
    html += "<h3>3. Wireless OTA Firmware Update</h3>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data' id='upload_form'>";
    html += "Select firmware.bin file:<br><input type='file' name='update'><br>";
    html += "<input type='submit' value='Upload New Binary File'></form></body></html>";
    server.send(200, "text/html", html);
}

void handleSavePortal() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    String wled_ip = server.arg("wled_ip");
    
    preferences.begin("wled_conf", false);
    if(ssid.length() > 0) preferences.putString("ssid", ssid);
    if(pass.length() > 0) preferences.putString("pass", pass);
    if(wled_ip.length() > 0) preferences.putString("target_ip", wled_ip);
    preferences.end();
    
    String redirectHtml = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:40px;}</style>";
    redirectHtml += "<script>setTimeout(function(){ window.location.href = '/'; }, 5000);</script></head>";
    redirectHtml += "<body><h3>Configurations Saved!</h3><p>Applying parameters. Device resetting. Redirecting back in 5 seconds...</p></body></html>";
    server.send(200, "text/html", redirectHtml);
    delay(1000); ESP.restart();
}

void handleReboot() {
    String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:40px;}</style>";
    html += "<script>setTimeout(function(){ window.location.href = '/'; }, 6000);</script></head>";
    html += "<body><h3>Reboot Command Sent!</h3><p>Hardware resetting. Reconnecting back automatically in 6 seconds...</p></body></html>";
    server.send(200, "text/html", html);
    delay(1000); ESP.restart();
}

void setup() {
    Serial.begin(115200);
    initOLED(); clearOLEDFB();
    printOLEDString("BOOTING PANEL...", 2);

    preferences.begin("wled_conf", true);
    savedSSID = preferences.getString("ssid", "");
    savedPass = preferences.getString("pass", "");
    targetWledIP = preferences.getString("target_ip", "0.0.0.0");
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
        printOLEDString("SYSTEM ONLINE", 1);
        printOLEDString("IP ADDRESS:", 3);
        printOLEDString(WiFi.localIP().toString().c_str(), 5);
        printOLEDString(FIRMWARE_VERSION, 7);
    }

    server.on("/", handleRootPortal);
    server.on("/save", HTTP_POST, handleSavePortal);
    server.on("/reboot", HTTP_POST, handleReboot);
    server.on("/generate_204", handleRootPortal);
    server.on("/fwlink", handleRootPortal);
    
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        String otaHtml = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif;background:#222;color:#fff;text-align:center;padding:40px;}</style>";
        if (Update.hasError()) {
            otaHtml += "<script>setTimeout(function(){ window.location.href = '/'; }, 4000);</script></head><body><h3>Update Failed!</h3><p>Returning in 4 seconds...</p></body></html>";
        } else {
            otaHtml += "<script>setTimeout(function(){ window.location.href = 'http://" + WiFi.localIP().toString() + "/'; }, 6000);</script></head><body><h3>Update Success!</h3><p>Flashing firmware and reloading. Redirecting automatically in 6 seconds...</p></body></html>";
        }
        server.send(200, "text/html", otaHtml);
        delay(1000); if (!Update.hasError()) ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { Update.printError(Serial); }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { Update.printError(Serial); }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (Update.end(true)) { Serial.println("OTA Complete"); } else { Update.printError(Serial); }
        }
    });

    server.onNotFound(handleRootPortal); server.begin();
    
    if (WiFi.status() == WL_CONNECTED) {
        udp.beginMulticast(multicastIP, WLED_UDP_PORT);
    } else {
        udp.begin(WLED_UDP_PORT);
    }
}

void loop() {
    server.handleClient();
    if (WiFi.status() != WL_CONNECTED) { dnsServer.processNextRequest(); delay(2); return; }

    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        packetCount++;
        lastPacketSize = packetSize;
        lastSenderIP = udp.remoteIP().toString();

        if (targetWledIP != "0.0.0.0" && lastSenderIP != targetWledIP) { 
            udp.flush(); 
            return; 
        }

        int len = udp.read(packetBuffer, BUFFER_SIZE);
        if (len > 2) {
            uint8_t protocolType = packetBuffer[0];
            int headerOffset = 2; 

            clearOLEDFB(); 

            // Forced fallback parsing: If the packet length matches your 256-pixel payload, force decode it immediately
            if (len == 1026 || protocolType == 1 || protocolType == 0) {
                detectedProtocol = "Active 32x8 Matrix Geometric Stream Decoder Executing";
                int availableDataBytes = len - headerOffset;
                int potentialPixels = availableDataBytes / 4;

                for (int i = 0; i < potentialPixels && i < 256; i++) {
                    int dataStart = headerOffset + (i * 4);
                    uint8_t ledIndex = packetBuffer[dataStart]; 
                    uint8_t r        = packetBuffer[dataStart + 1];
                    uint8_t g        = packetBuffer[dataStart + 2];
                    uint8_t b        = packetBuffer[dataStart + 3];

                    int panel = ledIndex / 64;
                    int local_i = ledIndex % 64;
                    int local_x = local_i / 8;
                    int local_y = 7 - (local_i % 8);
                    
                    int x = ((3 - panel) * 8) + local_x;
                    int y = local_y;

                    uint8_t luminance = (r + g + b) / 3;
                    bool pixelOn = (luminance > 40); // Increased sensitivity threshold to register lower-dim effects clearly

                    int oledYStart = 16 + (y * 4);
                    int oledXStart = x * 4;
                    for (int dy = 0; dy < 4; dy++) {
                        for (int dx = 0; dx < 4; dx++) {
                            setFBLocalPixel(oledXStart + dx, oledYStart + dy, pixelOn);
                        }
                    }
                }
            } 
            else if (protocolType == 2) {
                detectedProtocol = "DRGB Matrix Mapping Decoder Active";
                int totalPixels = (len - headerOffset) / 3;
                for (int i = 0; i < totalPixels && i < 256; i++) {
                    int r = packetBuffer[headerOffset + (i * 3)]; 
                    int g = packetBuffer[headerOffset + (i * 3) + 1]; 
                    int b = packetBuffer[headerOffset + (i * 3) + 2];
                    
                    int panel = i / 64;
                    int local_i = i % 64;
                    int local_x = local_i / 8;
                    int local_y = 7 - (local_i % 8);
                    
                    int x = ((3 - panel) * 8) + local_x;
                    int y = local_y;

                    uint8_t luminance = (r + g + b) / 3;
                    bool pixelOn = (luminance > 40);

                    int oledYStart = 16 + (y * 4);
                    int oledXStart = x * 4;
                    for (int dy = 0; dy < 4; dy++) {
                        for (int dx = 0; dx < 4; dx++) {
                            setFBLocalPixel(oledXStart + dx, oledYStart + dy, pixelOn);
                        }
                    }
                }
            }
            else {
                detectedProtocol = "WLED System Control Signal Frame Block [" + String(protocolType) + "]";
            }
            
            renderFBToDisplay(); 
        }
    }
}
