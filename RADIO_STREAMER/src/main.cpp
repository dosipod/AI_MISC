#include <WiFi.h>
#include <Update.h>
#include <Wire.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <Preferences.h>
#include "font.h"

#define AUDIO_PWM_PIN 26

const char* FIRMWARE_VERSION = "v9.5.1";
DNSServer dnsServer;
WebServer server(80);
Preferences preferences;

const byte DNS_PORT = 53;
IPAddress apIP(4, 3, 2, 1);
String savedSSID = "";
String savedPass = "";

const char* streamHost = "10.0.6.22"; 
const char* streamPath = "/voice-sample.pcm"; 
WiFiClient audioClient;

String currentStatusText = "STATION STANDBY";
bool isPlayingAudio = false;

volatile int activeAudioMode = 0; 
uint8_t oledFB[1024]; // FIXED: True 1024-byte display matrix frame tracking array configuration

// Stream buffer + playback config
const size_t STREAMBUF_SIZE = 16 * 1024;
StreamBufferHandle_t sb = NULL;
const unsigned int SAMPLE_RATE = 8000; // Hz
const int LEDC_CHANNEL = 0;
const int LEDC_RESOLUTION_BITS = 8;

void sendOLEDCommand(uint8_t cmd) {
    Wire.beginTransmission(0x3C); Wire.write(0x00); Wire.write(cmd); Wire.endTransmission();
}

void initOLED() {
    Wire.begin(5, 4, 400000); delay(10);
    sendOLEDCommand(0xAE); sendOLEDCommand(0xD5); sendOLEDCommand(0x80); sendOLEDCommand(0xA8); sendOLEDCommand(0x3F); sendOLEDCommand(0xD3);
    sendOLEDCommand(0x00); sendOLEDCommand(0x40); sendOLEDCommand(0x8D); sendOLEDCommand(0x14); sendOLEDCommand(0x20); sendOLEDCommand(0x00);
    sendOLEDCommand(0xA1); sendOLEDCommand(0xC8); sendOLEDCommand(0xDA); sendOLEDCommand(0x12); sendOLEDCommand(0x81); sendOLEDCommand(0xCF);
    sendOLEDCommand(0xD9); sendOLEDCommand(0xF1); sendOLEDCommand(0xDB); sendOLEDCommand(0x40); sendOLEDCommand(0xA4); sendOLEDCommand(0xA6); sendOLEDCommand(0xAF);
}

void clearOLEDFB() { memset(oledFB, 0, sizeof(oledFB)); }
void renderFBToDisplay() {
    sendOLEDCommand(0x21); sendOLEDCommand(0); sendOLEDCommand(127); sendOLEDCommand(0x22); sendOLEDCommand(0); sendOLEDCommand(7);
    for (int i = 0; i < 1024; i++) {
        Wire.beginTransmission(0x3C); Wire.write(0x40);
        for (int j = 0; j < 16 && i < 1024; j++, i++) { Wire.write(oledFB[i]); }
        i--; Wire.endTransmission();
    }
}

void setFBLocalPixel(int x, int y, bool turnOn) {
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int byteIdx = x + (y / 8) * 128; int bitIdx = y % 8;
    if (turnOn) oledFB[byteIdx] |= (1 << bitIdx); else oledFB[byteIdx] &= ~(1 << bitIdx);
}

void printOLEDString(const char* str, uint8_t line) {
    if (line > 7) return;
    for (int x = 0; x < 128; x++) { int byteIdx = x + line * 128; if (byteIdx < 1024) oledFB[byteIdx] = 0; }
    int charCount = 0;
    while (*str && charCount < 16) {
        char val = *str++; int idx = 0;
        if (val >= 32 && val <= 90) idx = val - 32; else if (val >= 97 && val <= 122) idx = val - 64; 
        for (int i = 0; i < 5; i++) {
            int targetX = (charCount * 6) + i;
            for (int bit = 0; bit < 8; bit++) { if ((ssd1306_font[idx][i] >> bit) & 0x01) { setFBLocalPixel(targetX, (line * 8) + bit, true); } }
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
    html += ".btn-toggle{background:#4cc9f0 !important; color:#fff; font-size:18px; padding:15px; width:90%; border-radius:4px; border:none; cursor:pointer;}";
    html += ".btn-diag{background:#ffb703 !important; color:#111; font-weight:bold; width:43%; margin:5px; padding:12px;}";
    html += ".box{background:#333;padding:15px;margin:15px auto;width:90%;border-radius:4px;text-align:left;}</style></head>";
    html += "<body><h2>ESP32 Real Audio Streamer</h2>";
    html += "<span style='font-size:14px;color:#888;'>Running: " + String(FIRMWARE_VERSION) + "</span><br><br>";
    
    html += "<form action='/toggle' method='POST'><input type='submit' class='btn-toggle' value='Start / Stop Live Music Stream'></form><br>";
    
    html += "<div style='margin:10px auto; width:90%;'>";
    html += "<form action='/diag-sine' method='POST' style='display:inline;'><input type='submit' class='btn-toggle btn-diag' value='Test Beep Tone'></form>";
    html += "<form action='/diag-voice' method='POST' style='display:inline;'><input type='submit' class='btn-toggle btn-diag' value='Play Tune Melody'></form></div><br>";
    
    html += "<form action='/reboot' method='POST'><input type='submit' class='btn-reboot' value='Force System Reboot'></form>";
    html += "<div class='box'><b>Hardware Sound Status:</b><br>Local IP Address: " + WiFi.localIP().toString() + "<br>";
    html += "Core Output Mode: <span style='color:#00f5d4; font-family:monospace;'>" + currentStatusText + "</span></div>";
    
    html += "<form action='/save' method='POST'>";
    html += "<h3>1. Network Profile</h3>";
    html += "SSID:<br><input type='text' name='ssid' value='" + savedSSID + "'><br>";
    html += "Password:<br><input type='password' name='pass' value='" + savedPass + "'><br>";
    html += "<input type='submit' value='Apply Parameters & Reboot'></form><br><hr style='border:1px solid #444; width:90%;'>";
    
    html += "<h3>2. Wireless OTA Update</h3>";
    html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
    html += "<input type='file' name='update'><br>";
    html += "<input type='submit' value='Upload New Binary File'></form></body></html>";
    server.send(200, "text/html", html);
}
void handleToggleStream() {
    activeAudioMode = 0;
    isPlayingAudio = !isPlayingAudio;
    if (isPlayingAudio) {
        // Let the fetcher task handle connection setup to avoid blocking here
        if (audioClient.connected()) audioClient.stop();
        currentStatusText = "STARTING STREAM...";
    } else {
        if (audioClient.connected()) audioClient.stop();
        ledcWriteTone(LEDC_CHANNEL, 0);
        currentStatusText = "STREAM STOPPED";
        clearOLEDFB(); printOLEDString("STREAM STOPPED", 3);
    }
    server.sendHeader("Location", "/"); server.send(303);
}

void handleSavePortal() { String ssid = server.arg("ssid"); String pass = server.arg("pass"); preferences.begin("wled_conf", false); if(ssid.length() > 0) preferences.putString("ssid", ssid); if(pass.length() > 0) preferences.putString("pass", pass); preferences.end(); server.send(200, "text/html", "Saved!"); delay(1000); ESP.restart(); }
void handleReboot() { server.send(200, "text/html", "Rebooting..."); delay(1000); ESP.restart(); }
// Simple blocking square-wave tone generator using PWM duty toggling
void playToneBlocking(unsigned int freqHz, unsigned long durationMs) {
    if (freqHz == 0) { ledcWrite(LEDC_CHANNEL, 128); vTaskDelay(pdMS_TO_TICKS(durationMs)); return; }
    unsigned int halfPeriodUs = 500000 / freqHz; // microseconds for half period
    unsigned long end = millis() + durationMs;
    while (millis() < end) {
        ledcWrite(LEDC_CHANNEL, 255);
        ets_delay_us(halfPeriodUs);
        ledcWrite(LEDC_CHANNEL, 0);
        ets_delay_us(halfPeriodUs);
        taskYIELD();
    }
    ledcWrite(LEDC_CHANNEL, 128);
}

void handleDiagSine() { isPlayingAudio = false; activeAudioMode = 1; currentStatusText = "BEEP TONE ACTIVE"; playToneBlocking(523, 400); server.sendHeader("Location", "/"); server.send(303); }
void handleDiagVoice() { isPlayingAudio = false; activeAudioMode = 2; currentStatusText = "PLAYING TUNE MELO"; server.sendHeader("Location", "/"); server.send(303); int notes[] = {440, 554, 659, 880}; for (int i = 0; i < 4; i++) { playToneBlocking(notes[i], 160); } ledcWrite(LEDC_CHANNEL, 128); activeAudioMode = 0; currentStatusText = "TUNE COMPLETE"; }

void audioStreamWorkerTask(void * pvParameters) {
    const size_t CHUNK = 256;
    uint8_t chunkCache[CHUNK];

    while (1) {
        if (isPlayingAudio) {
            if (!audioClient.connected()) {
                if (audioClient.connect(streamHost, 80)) {
                    audioClient.print(String("GET ") + streamPath + " HTTP/1.0\r\nHost: " + streamHost + "\r\nUser-Agent: ESP32PCM/1.0\r\nConnection: close\r\n\r\n");
                    unsigned long headerTimeout = millis() + 3000;
                    while (audioClient.connected() && !audioClient.available() && millis() < headerTimeout) {
                        vTaskDelay(pdMS_TO_TICKS(1));
                    }
                    // Drain headers
                    while (audioClient.connected() && audioClient.available()) {
                        String h = audioClient.readStringUntil('\n');
                        if (h == "\r" || h.length() == 0) break;
                    }
                    currentStatusText = "STREAMING PC VOICE";
                } else {
                    currentStatusText = "CONNECTION FAILED";
                    isPlayingAudio = false;
                    vTaskDelay(pdMS_TO_TICKS(500));
                    continue;
                }
            }

            int bytesAvailable = audioClient.available();
            if (bytesAvailable > 0) {
                int toRead = min((int)CHUNK, bytesAvailable);
                int r = audioClient.read(chunkCache, toRead);
                if (r > 0) {
                    int written = 0;
                    while (written < r) {
                        size_t res = xStreamBufferSend(sb, chunkCache + written, r - written, pdMS_TO_TICKS(100));
                        if (res == 0) {
                            vTaskDelay(pdMS_TO_TICKS(2));
                            continue;
                        }
                        written += res;
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

void audioPlaybackTask(void * pvParameters) {
    uint8_t sample = 128;
    const unsigned int samplePeriodUs = 1000000UL / SAMPLE_RATE;
    for (;;) {
        size_t r = xStreamBufferReceive(sb, &sample, 1, pdMS_TO_TICKS(50));
        if (r == 0) sample = 128; // silence
        ledcWrite(LEDC_CHANNEL, sample);
        ets_delay_us(samplePeriodUs);
    }
}

void setup() {
    Serial.begin(115200); initOLED(); clearOLEDFB(); printOLEDString("BOOTING RADIO...", 2);
    
    ledcSetup(LEDC_CHANNEL, 40000, LEDC_RESOLUTION_BITS); ledcAttachPin(AUDIO_PWM_PIN, LEDC_CHANNEL);

    preferences.begin("wled_conf", true); savedSSID = preferences.getString("ssid", ""); savedPass = preferences.getString("pass", ""); preferences.end();
    if (savedSSID.length() > 0) { WiFi.mode(WIFI_AP_STA); WiFi.begin(savedSSID.c_str(), savedPass.c_str()); long startMs = millis(); while (WiFi.status() != WL_CONNECTED && millis() - startMs < 8000) { delay(500); } }
    if (WiFi.status() != WL_CONNECTED) { WiFi.mode(WIFI_AP_STA); IPAddress gateway(4, 3, 2, 1); IPAddress subnet(255, 255, 255, 0); WiFi.softAPConfig(apIP, gateway, subnet); WiFi.softAP("esp32_hw724"); dnsServer.start(DNS_PORT, "*", apIP); printOLEDString("CONNECT TO AP", 1); printOLEDString("esp32_hw724", 3); printOLEDString("GO TO: 4.3.2.1", 5); } 
    else { clearOLEDFB(); printOLEDString("STREAM UNIT OK", 1); printOLEDString("IP ADDRESS:", 3); printOLEDString(WiFi.localIP().toString().c_str(), 5); }
    
    server.on("/", handleRootPortal); server.on("/save", HTTP_POST, handleSavePortal); server.on("/reboot", HTTP_POST, handleReboot); server.on("/toggle", HTTP_POST, handleToggleStream); server.on("/diag-sine", HTTP_POST, handleDiagSine); server.on("/diag-voice", HTTP_POST, handleDiagVoice);
    
    server.on("/update", HTTP_POST, []() {
        server.sendHeader("Connection", "close");
        String redirectPayload = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
        redirectPayload += "<script>setTimeout(function(){ window.location.href = 'http://' + window.location.hostname + '/'; }, 6000);</script></head>";
        redirectPayload += "<body><h2>Flash Success! Reloading...</h2></body></html>";
        server.send(200, "text/html", redirectPayload); delay(1000); ESP.restart();
    }, []() {
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) { Update.begin(UPDATE_SIZE_UNKNOWN); } 
        else if (upload.status == UPLOAD_FILE_WRITE) { Update.write(upload.buf, upload.currentSize); } 
        else if (upload.status == UPLOAD_FILE_END) { Update.end(true); }
    });

    server.onNotFound(handleRootPortal); server.begin();

    // Create stream buffer and audio tasks
    sb = xStreamBufferCreate(STREAMBUF_SIZE, 1);
    if (!sb) {
        Serial.println("Failed to create stream buffer");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    xTaskCreatePinnedToCore(audioStreamWorkerTask, "AudioFetcher", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(audioPlaybackTask, "AudioPlay", 4096, NULL, 3, NULL, 0);
}

void loop() {
    server.handleClient();
    if (WiFi.status() != WL_CONNECTED) { dnsServer.processNextRequest(); delay(2); return; }
    
    static unsigned long lastRefresh = 0;
    if (millis() - lastRefresh > 100) {
        lastRefresh = millis(); clearOLEDFB();
        // Display basic status and stream buffer fill
        char bufLine[32];
        if (isPlayingAudio) {
            printOLEDString("STREAMING AUDIO.", 1);
            static float wavePhase = 0;
            for (int x = 0; x < 128; x++) { int y = 45 + (int)(12.0 * sin((x * 0.15) + wavePhase)); setFBLocalPixel(x, y, true); setFBLocalPixel(x, y + 1, true); }
            wavePhase += 0.2;
        } else if (activeAudioMode == 1) {
            printOLEDString("BEEP NOTE ACTIVE", 1);
        } else {
            printOLEDString(" SYSTEM READY", 1);
            printOLEDString("AWAITING TRIGGER", 4);
        }

        // Show IP on line 3 and stream buffer fill on line 5
        printOLEDString(WiFi.localIP().toString().c_str(), 3);
        if (sb) {
            size_t spaces = xStreamBufferSpacesAvailable(sb);
            size_t filled = (STREAMBUF_SIZE > spaces) ? (STREAMBUF_SIZE - spaces) : 0;
            int pct = (int)((filled * 100) / (float)STREAMBUF_SIZE);
            snprintf(bufLine, sizeof(bufLine), "Buf: %u/%u (%d%%)", (unsigned)filled, (unsigned)STREAMBUF_SIZE, pct);
            printOLEDString(bufLine, 5);
        }
        renderFBToDisplay();
    }
}
