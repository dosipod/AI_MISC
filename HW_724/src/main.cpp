#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Screen Dimensions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// HW-724 Specific I2C Display Pinout
#define OLED_SDA 5
#define OLED_SCL 4
#define OLED_ADDR 0x3C

// Networking Protocol Definitions
#define DDP_PORT 4048
#define BUFFER_SIZE 1500

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiUDP udp;
uint8_t packetBuffer[BUFFER_SIZE];

void setupDisplay() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        for(;;); // System lock if display is unreachable
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("Initializing...");
    display.display();
}

void setupOTA() {
    ArduinoOTA.setHostname("hw724_wled_receiver");
    
    ArduinoOTA.onStart([]() {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("OTA Update Started");
        display.display();
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.printf("Updating: %u%%", (progress / (total / 100)));
        display.display();
    });

    ArduinoOTA.onEnd([]() {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("Update Complete!");
        display.println("Rebooting...");
        display.display();
    });

    ArduinoOTA.onError([](ota_error_t error) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.printf("OTA Error[%u]", error);
        display.display();
    });

    ArduinoOTA.begin();
}

void setup() {
    Serial.begin(115200);
    setupDisplay();

    // WiFiManager Setup
    WiFiManager wm;
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Connect to AP:");
    display.println("ESP32_WLED_SETUP");
    display.display();

    // Device starts in Access Point mode if it has no saved credentials
    if(!wm.autoConnect("ESP32_WLED_SETUP")) {
        Serial.println("Failed to connect, resetting...");
        ESP.restart();
    }

    // Connected state
    display.clearDisplay();
    display.setCursor(0,0);
    display.println("Connected!");
    display.println("IP Address:");
    display.println(WiFi.localIP().toString());
    display.display();

    setupOTA();
    udp.begin(DDP_PORT);
}

void loop() {
    ArduinoOTA.handle();

    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
        int len = udp.read(packetBuffer, BUFFER_SIZE);
        
        // Basic DDP parsing logic: Header is typically 10 bytes long
        // Byte 0 contains flags, Byte 3 contains data type / pixel format
        if (len > 10) {
            uint16_t offset = (packetBuffer[4] << 24) | (packetBuffer[5] << 16) | 
                              (packetBuffer[6] << 8) | packetBuffer[7];
            
            // DDP payload contains raw RGB sequence data
            uint8_t* pixelData = &packetBuffer[10];
            int totalBytes = len - 10;
            int totalPixels = totalBytes / 3; // 3 bytes per pixel (R, G, B)

            display.clearDisplay();
            
            // Map the inbound streaming RGB pixel data to the monochrome OLED grid
            for (int i = 0; i < totalPixels && i < (SCREEN_WIDTH * SCREEN_HEIGHT); i++) {
                int r = pixelData[i * 3];
                int g = pixelData[i * 3 + 1];
                int b = pixelData[i * 3 + 2];
                
                // Convert RGB data into binary luminance for the SSD1306 panel
                uint8_t luminance = (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
                
                int x = i % SCREEN_WIDTH;
                int y = i / SCREEN_WIDTH;
                
                if (luminance > 127 && y < SCREEN_HEIGHT) {
                    display.drawPixel(x, y, SSD1306_WHITE);
                }
            }
            display.display();
        }
    }
}
