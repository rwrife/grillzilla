#include <Wire.h>
#include <Adafruit_MLX90614.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include "esp_sleep.h"
#include <Preferences.h>
#include <ESPmDNS.h>

// ==== CONFIG ====
const char* endpoint = "https://eoehka5jb7s112m.m.pipedream.net/";
const int sleepMinutes = 30;

// ==== Globals ====
Adafruit_MLX90614 mlx = Adafruit_MLX90614();
bool serialConnected = false;
WebServer server(80);
float currentAmbient = 0.0;
float currentObject = 0.0;
Preferences prefs;

// ==== Function Prototypes ====
void connectWiFi(const char* ssid, const char* pass);
void takeAndSendReading();
void sendToEndpoint(float ambient, float object);
void goToDeepSleep();
bool waitForSerial(unsigned long timeoutMs);
void startAccessPoint();
void handleRoot();

// ==== Setup ====
void setup() {
  prefs.begin("grillzilla", false);

  Serial.begin(9600);
  delay(100);
  Wire.begin(6, 7);  // SDA/SCL

  Serial.println("Starting grill monitor...");

  // Print saved credentials
  String debugSSID = prefs.getString("ssid", "(none)");
  String debugPass = prefs.getString("pass", "(none)");
  Serial.print("Saved SSID: "); Serial.println(debugSSID);
  Serial.print("Saved Password: "); Serial.println(debugPass);

  serialConnected = waitForSerial(10000);

  if (!mlx.begin()) {
    Serial.println("Failed to initialize MLX90614!");
    goToDeepSleep();
  }

  if (!serialConnected) {
    String storedSSID = prefs.getString("ssid", "");
    String storedPass = prefs.getString("pass", "");

    if (storedSSID != "" && storedPass != "") {
      connectWiFi(storedSSID.c_str(), storedPass.c_str());
      takeAndSendReading();
      goToDeepSleep();
    } else {
      goToDeepSleep();  // no credentials saved
    }
  } else {
    Serial.println("USB connected. Starting Access Point...");
    startAccessPoint();
  }
}

// ==== Loop ====
void loop() {
  if (!serialConnected) return;

  server.handleClient();

  // Detect USB disconnect
  static bool wasConnected = true;
  if (wasConnected && !Serial) {
    ESP.restart();
  }

  static unsigned long lastRead = 0;
  if (millis() - lastRead > 5000) {
    currentAmbient = mlx.readAmbientTempC();
    currentObject = mlx.readObjectTempC();
    lastRead = millis();
  }
}

// ==== Wi-Fi Client Mode ====
void connectWiFi(const char* ssid, const char* pass) {
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

// ==== Access Point Mode ====
void startAccessPoint() {
  WiFi.softAP("Grillzilla");
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  if (!MDNS.begin("grillzilla.local")) {
    Serial.println("Error starting mDNS responder!");
  } else {
    Serial.println("mDNS responder started: http://grillzilla.local");
  }

  server.on("/", handleRoot);

  server.on("/api/data", []() {
    String json = String("{\"ambient_c\":") + currentAmbient +
                  ",\"object_c\":" + currentObject + "}";
    server.send(200, "application/json", json);
  });

  server.on("/api/config", HTTP_POST, []() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
      prefs.putString("ssid", server.arg("ssid"));
      prefs.putString("pass", server.arg("pass"));
      server.send(200, "text/plain", "Saved. Will use on next boot.");
    } else {
      server.send(400, "text/plain", "Missing ssid or pass.");
    }
  });

  server.on("/api/reboot", HTTP_POST, []() {
    server.send(200, "text/plain", "Rebooting in 3 seconds...");
    delay(3000);  // give browser time to receive response
    ESP.restart();
  });

  server.begin();
  Serial.println("Web server started.");
}

void handleRoot() {
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");

  String html = R"rawliteral(
    <html>
    <head>
      <title>Grillzilla</title>
      <script>
        async function fetchData() {
          const res = await fetch('/api/data');
          const json = await res.json();
          document.getElementById('ambient').textContent = json.ambient_c.toFixed(2);
          document.getElementById('object').textContent = json.object_c.toFixed(2);
        }

        async function saveConfig(e) {
          e.preventDefault();
          const ssid = document.getElementById('ssid').value;
          const pass = document.getElementById('pass').value;

          const formData = new URLSearchParams();
          formData.append('ssid', ssid);
          formData.append('pass', pass);

          const res = await fetch('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: formData.toString()
          });

          alert(await res.text());
        }

        setInterval(fetchData, 3000);
        window.onload = fetchData;
      </script>
    </head>
    <body>
      <h1>Grillzilla</h1>
      <h4>Temperature Readings</h4>
      <p>Ambient: <span id="ambient">--</span>°C</p>
      <p>Object: <span id="object">--</span>°C</p>
      <hr>
      <h4>Wi-Fi Settings</h4>
      <form onsubmit="saveConfig(event)">
        <label>SSID: <input type="text" id="ssid" value=")rawliteral" + savedSSID + R"rawliteral(" /></label><br/>
        <label>Password: <input type="password" id="pass" value=")rawliteral" + savedPass + R"rawliteral(" /></label><br/>
        <button type="submit">Save Settings</button>
      </form>
      <hr>
      <h4>Maintenance</h4>
      <button onclick="reboot()">Reboot</button>
      <script>
        async function reboot() {
          const res = await fetch('/api/reboot', { method: 'POST' });
          const text = await res.text();
          alert(text);
        }
      </script>      
    </body>
    </html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// ==== Sleep Mode Behavior ====
void takeAndSendReading() {
  float ambient = 0.0;
  float object = 0.0;

  for (int i = 0; i < 3; i++) {
    ambient = mlx.readAmbientTempC();
    object = mlx.readObjectTempC();
    delay(1000);
  }

  sendToEndpoint(ambient, object);
}

void sendToEndpoint(float ambient, float object) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(endpoint);
  http.addHeader("Content-Type", "application/json");

  String payload = String("{\"ambient_c\":") + ambient +
                   ",\"object_c\":" + object + "}";

  http.POST(payload);
  http.end();
}

void goToDeepSleep() {
  esp_sleep_enable_timer_wakeup((int64_t)sleepMinutes * 60 * 1000000LL);
  esp_deep_sleep_start();
}

bool waitForSerial(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (!Serial && millis() - start < timeoutMs) {
    delay(10);
  }
  return Serial;
}
