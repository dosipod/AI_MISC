#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Audio.h"

// Hardware Configurations
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Network Credentials
const char* ssid = "toi"; 
const char* password = "YOUR_WIFI_PASSWORD_HERE";

// Radio Audio Processing Core Engine Instance
Audio audio;

// Target Streaming Link (98.3 The Seagull - Commercial Free Variety)
const char* radio_station = "http://internet-radio.com";

// Global tracking variables for display updates
String current_title = "Connecting...";
String current_stream_status = "BUFFERING";

void updateDisplay() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    
    // Header Zone
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("ESP32 RADIO SYSTEM");
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    
    // Status Information Blocks
    display.setCursor(0, 15);
    display.print("Status: ");
    display.print(current_stream_status);
    
    display.setCursor(0, 28);
    display.print("IP: ");
    display.print(WiFi.localIP().toString());
    
    // Dynamic Rolling Text Block (Track Names/Metadata)
    display.setCursor(0, 42);
    display.print("Playing:");
    display.setCursor(0, 52);
    display.setTextSize(1);
    // Truncate text block to prevent drawing screen runout bugs
    display.print(current_title.substring(0, 21)); 
    
    display.display();
}

void setup() {
    Serial.begin(115200);
    
    // Initialize Local I2C Bus for the OLED Panels
    Wire.begin(21, 22);
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
        Serial.println(F("SSD1306 allocation failed"));
    }
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,20);
    display.println("Booting System...");
    display.display();

    // Establish Secure Network Connection Links
    Serial.print("Connecting to Wi-Fi");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected!");
    
    current_stream_status = "ONLINE";
    updateDisplay();

    // Configure the Audio Pipeline to use Internal DAC Channel 2 (GPIO 26)
    audio.setPinout(0, 0, 0); // Clear physical external I2S pins out
    audio.setInternalDAC(true, false); // Route stream directly to internal 8-bit DAC on Pin 26
    audio.setVolume(12); // Safe startup gain levels (Range: 0-21)
    
    // Kick off data parsing stream
    audio.connecttohost(radio_station);
}

void loop() {
    // Keep processing streaming chunks continuously inside the loop runtime block
    audio.loop();
}

// ==================== METADATA CALLBACK ROUTINES ====================

void audio_showstation(const char *info){
    Serial.print("Station: "); Serial.println(info);
}

void audio_showstreamtitle(const char *info){
    Serial.print("StreamTitle: "); Serial.println(info);
    current_title = String(info);
    current_stream_status = "PLAYING";
    updateDisplay(); // Force paint refreshed status strings down to panel
}

void audio_eof_stream(const char *info){
    Serial.print("EOS reached: "); Serial.println(info);
    current_stream_status = "STREAM ENDED";
    updateDisplay();
}
