#ifndef WiFiManager_h
#define WiFiManager_h
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
class WiFiManager {
public:
    WiFiManager() {}
    bool autoConnect(const char* apName) {
        Serial.print("Starting AP: ");
        Serial.println(apName);
        WiFi.mode(WIFI_AP_STA);
        
        // Force Custom 4.3.2.1 AP Network layout
        IPAddress apIP(4, 3, 2, 1);
        IPAddress gateway(4, 3, 2, 1);
        IPAddress subnet(255, 255, 255, 0);
        WiFi.softAPConfig(apIP, gateway, subnet);
        WiFi.softAP(apName);
        
        long startTime = millis();
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
            if (millis() - startTime > 5000) {
                Serial.println("\nApModeActive at 4.3.2.1");
                return true;
            }
        }
        return true;
    }
};
#endif