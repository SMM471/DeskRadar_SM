#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <cmath>
#include <Preferences.h>

// --- Settings & Security ---
const int SETTINGS_BUTTON_PIN = 0;
const char* DEVICE_PIN = "1991";

// --- Configurable State Parameters ---
float radarLat, radarLon, maxRadarRangeKm;
String storedSsid = "", storedPass = "";

bool enableSweep = true; // Toggle for radar sweep animation
bool useMetric = true;   // Toggle for Metric (Meters/KMH) vs Imperial (Feet/Knots)

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft); // Sprite buffer for flicker-free rendering

Preferences preferences;
WebServer server(80);

int pollInterval = 11000; // adsb.fi is free/keyless; using the original Auth-tier interval as the default
bool isConfigured = false, needsReboot = false;
String dynamicWifiOptions = "";

// Guards targets[]/numTargets, which are written by fetchTask (core 0) and
// read by loop()'s animation/render code (core 1) at the same time.
SemaphoreHandle_t targetsMutex;

// --- Animation & Memory Arrays ---
struct Target {
  float x;
  float y;
  String callsign;
  String type;    // Aircraft type, e.g. "A320" (from adsb.fi "t" field)
  String altStr;  // Formatted altitude string, e.g. "3450m" or "GND"
  float angle;    // Angle from radar center (for sweep fading)
  float heading;  // Direction plane is flying (for the triangle)
  float speedKmh; // Ground speed in km/h (used to interpolate movement between fetches)
};
Target targets[30];
int numTargets = 0;
float sweepAngle = 0.0;
unsigned long lastDrawTime = 0;

// --- Timing for Startup Title ---
bool firstFetchDone = false;
unsigned long firstFetchTime = 0;

void drawRadarGrid();
void fetchAndMapFlights();
void loadConfiguration();
void runNetworkScan();
void renderRadarFrame();
void advanceTargets(float dtMs);
void fetchTask(void *parameter);

void updateStatusScreen(String line1, String line2, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(color, TFT_BLACK);
  tft.drawCentreString(line1, 120, 100, 2);
  tft.drawCentreString(line2, 120, 130, 2);
}

void setup() {
  Serial.begin(115200);
  pinMode(SETTINGS_BUTTON_PIN, INPUT_PULLUP);
  pinMode(21, OUTPUT); digitalWrite(21, HIGH); // Backlight

  delay(1000);

  tft.init(); tft.setRotation(0); tft.fillScreen(TFT_BLACK);

  // Initialize 8-bit Sprite (uses less RAM to protect WiFi/JSON stability)
  spr.setColorDepth(8);
  spr.createSprite(240, 240);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawCentreString("SYSTEM BOOT...", 120, 110, 2);

  loadConfiguration();

  preferences.begin("radar-config", true);
  isConfigured = preferences.getBool("configured", false);
  bool forcePortal = preferences.getBool("force_portal", false);
  preferences.end();

  if (forcePortal) {
    preferences.begin("radar-config", false);
    preferences.putBool("force_portal", false);
    preferences.end();
    isConfigured = false;
  }

  // 3. Isolated Connection Phase
  if (isConfigured && storedSsid.length() > 0) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("DeskRadar");

    for (int attempt = 1; attempt <= 3; attempt++) {
      if (attempt == 1) {
        updateStatusScreen("CONNECTING...", storedSsid, TFT_GREEN);
      } else {
        updateStatusScreen("REBOOTING RADIO...", "Attempt " + String(attempt) + "/3", TFT_GREEN);
      }

      WiFi.disconnect();
      delay(500);
      WiFi.begin(storedSsid.c_str(), storedPass.c_str());

      int timeout = 0;
      while (WiFi.status() != WL_CONNECTED && timeout < 24) {
        delay(500);
        timeout++;
      }

      if (WiFi.status() == WL_CONNECTED) break;
    }
  }

  // 4. Fallback to Portal Phase
  if (WiFi.status() != WL_CONNECTED) {
    isConfigured = false;
    updateStatusScreen("WIFI FAILED", "Scanning Rooms...", TFT_GREEN);
    runNetworkScan();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("DeskRadar-Setup-bySteffen Moll");

    server.on("/", [](){
      String defaultOption = (storedSsid.length() > 0) ? "<option value=''>-- Keep Saved (" + storedSsid + ") --</option>" : "<option value=''>-- Select Network --</option>";
      String sweepChecked = enableSweep ? "checked" : "";
      String metricChecked = useMetric ? "checked" : "";

      String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'>"
                    "<style>body{background:#000;color:#0f0;font-family:sans-serif;padding:20px;} input, select, button {margin-top:5px; padding:8px;} hr{border-color:#0f0;}</style></head><body>"
                    "<h2>DeskRadar-Setup-bySteffenMoll</h2><form action='/save' method='POST'>"
                    "<label>Device PIN (on the housing):</label><br><input type='password' name='pin' style='width:100%;' required><hr>"

                    "<label>Select WiFi Network:</label><br>"
                    "<select name='ssid' style='width:100%;'>" + defaultOption + dynamicWifiOptions + "</select><br><br>"

                    "<label>WiFi Password:</label><br>"
                    "<div style='display:flex; align-items:center;'>"
                    "<input type='password' id='wp' name='pass' style='flex-grow:1;'>"
                    "<input type='checkbox' onclick='document.getElementById(\"wp\").type=this.checked?\"text\":\"password\"' style='margin-left:10px;'> Show"
                    "</div><hr>"

                    "<label>Latitude:</label><br><input type='text' name='lat' value='" + String(radarLat, 4) + "' style='width:100%;' required><br><br>"
                    "<label>Longitude:</label><br><input type='text' name='lon' value='" + String(radarLon, 4) + "' style='width:100%;' required><br><br>"
                    "<label>Range (KM) (less KM = less planes, less crowded):</label><br><input type='number' name='range' value='" + String((int)maxRadarRangeKm) + "' style='width:100%;' required><br><br>"

                    "<label><input type='checkbox' name='sweep' value='1' " + sweepChecked + "> Enable Radar Sweep Animation</label><br><br>"
                    "<label><input type='checkbox' name='metric' value='1' " + metricChecked + "> Use Metric System (Meters/KMH)</label><hr>"

                    "<button type='submit' style='background:#0f0;color:#000;font-weight:bold;width:100%;font-size:18px;'>SAVE & REBOOT</button></form></body></html>";
      server.send(200, "text/html", html);
    });

    server.on("/save", [](){
      if(server.arg("pin") != String(DEVICE_PIN)) { server.send(200, "text/html", "Invalid PIN"); return; }

      preferences.begin("radar-config", false);
      preferences.putFloat("lat", atof(server.arg("lat").c_str()));
      preferences.putFloat("lon", atof(server.arg("lon").c_str()));
      preferences.putFloat("range", atof(server.arg("range").c_str()));
      preferences.putBool("configured", true);
      preferences.putBool("sweep", server.hasArg("sweep"));
      preferences.putBool("metric", server.hasArg("metric"));

      if (server.arg("ssid").length() > 0) {
        preferences.putString("ssid", server.arg("ssid"));
        preferences.putString("pass", server.arg("pass"));
      }
      preferences.end();

      server.send(200, "text/html", "<h3 style='color:green;'>Settings Saved. Device is Rebooting...</h3>");
      needsReboot = true;
    });

    server.begin();
    updateStatusScreen("PORTAL ACTIVE", "192.168.4.1", TFT_GREEN);
  } else {
    tft.fillScreen(TFT_BLACK);
    isConfigured = true;
    pollInterval = 11000;
  }

  // Fetching runs on core 0 so the sweep animation / rendering on core 1
  // (loop()) never freezes while waiting on the network or JSON parsing.
  targetsMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(fetchTask, "fetchTask", 8192, NULL, 1, NULL, 0);
}

void loop() {
  // --- 1. INSTANT BUTTON CHECK ---
  if (digitalRead(SETTINGS_BUTTON_PIN) == LOW) {
    unsigned long pressTime = millis();
    bool actuallyHeld = true;

    while (millis() - pressTime < 3000) {
      if (digitalRead(SETTINGS_BUTTON_PIN) == HIGH) { actuallyHeld = false; break; }
      delay(10);
    }

    if (actuallyHeld) {
      updateStatusScreen("SETTINGS MODE", "Rebooting...", TFT_GREEN);
      preferences.begin("radar-config", false);
      preferences.putBool("force_portal", true);
      preferences.end();
      delay(1000);
      ESP.restart();
    }
  }

  // --- 2. SETUP PORTAL LOGIC ---
  if (!isConfigured) {
    server.handleClient();
    if (needsReboot) { delay(2000); ESP.restart(); }
    return;
  }

  // --- 3. RENDER RADAR SCREEN (Approx 25 FPS) ---
  // Both WiFi (re)connecting and fetching now happen solely in fetchTask() on
  // the other core - loop() never touches the WiFi stack or blocks on the
  // network, so it's free to animate every frame without racing that task.
  if (millis() - lastDrawTime > 40) {
    float dt = millis() - lastDrawTime; // ms since last frame

    xSemaphoreTake(targetsMutex, portMAX_DELAY);
    advanceTargets(dt); // Move planes smoothly based on their speed/heading

    if (enableSweep) {
      sweepAngle += 0.05; // Radar rotation speed
      if (sweepAngle > 2 * PI) sweepAngle -= 2 * PI;
    }
    renderRadarFrame();
    xSemaphoreGive(targetsMutex);

    lastDrawTime = millis();
  }
}

// Runs on core 0: independently fetches flight data on its own schedule so
// the animation on core 1 (loop()) never has to wait on the network/JSON parsing.
void fetchTask(void *parameter) {
  for (;;) {
    if (isConfigured) {
      // Sole owner of the WiFi connection - loop() on the other core never
      // calls WiFi.disconnect()/begin() itself anymore, avoiding the race
      // that caused constant reconnects.
      if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        delay(500);
        WiFi.begin(storedSsid.c_str(), storedPass.c_str());
        for (int i = 0; i < 20; i++) {
          if (WiFi.status() == WL_CONNECTED) break;
          delay(500);
        }
      }

      if (WiFi.status() == WL_CONNECTED) {
        fetchAndMapFlights();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(pollInterval));
  }
}

// NEW: interpolate each target's position between real API fetches using its
// ground speed and heading, so planes appear to fly instead of jump.
void advanceTargets(float dtMs) {
  float pixelsPerKm = 100.0 / maxRadarRangeKm;
  for (int i = 0; i < numTargets; i++) {
    float hRad = targets[i].heading * PI / 180.0;
    float deltaKm = targets[i].speedKmh * (dtMs / 3600000.0);
    float deltaPx = deltaKm * pixelsPerKm;
    targets[i].x += deltaPx * sin(hRad);
    targets[i].y -= deltaPx * cos(hRad);
  }
}

void runNetworkScan() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  int n = WiFi.scanNetworks();
  dynamicWifiOptions = "";

  if (n > 0) {
    for (int i = 0; i < n; ++i) {
      if(WiFi.SSID(i).length() > 0) {
        dynamicWifiOptions += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
      }
    }
  }
  WiFi.scanDelete();
}

void renderRadarFrame() {
  spr.fillSprite(TFT_BLACK);
  drawRadarGrid();

  // --- Draw Targets ---
  const int MIN_BRIGHTNESS = 55; // Planes never fully disappear, just dim, between sweep passes

  for (int i = 0; i < numTargets; i++) {
    int brightness = 255; // Default full brightness

    if (enableSweep) {
      float angleDiff = sweepAngle - targets[i].angle;
      if (angleDiff < 0) angleDiff += 2 * PI;

      // Fade smoothly across the full rotation: brightest right as the sweep
      // passes over, dimming down to MIN_BRIGHTNESS just before it comes back around
      brightness = 255 - (int)((angleDiff / (2 * PI)) * (255 - MIN_BRIGHTNESS));
      if (brightness < MIN_BRIGHTNESS) brightness = MIN_BRIGHTNESS;
    }

    if (brightness > 0) {
      // A380 (ICAO type code "A388") is highlighted in red so it stands out from every other plane
      bool isA380 = targets[i].type.startsWith("A38");
      uint16_t planeColor = isA380 ? tft.color565(brightness, 0, 0) : tft.color565(brightness, brightness, brightness);
      uint16_t textColor  = isA380 ? tft.color565(brightness, 0, 0) : tft.color565(0, brightness, 0);

      float hRad = targets[i].heading * PI / 180.0;

      // Calculate Triangle Vertices
      int nx = targets[i].x + 6 * sin(hRad);
      int ny = targets[i].y - 6 * cos(hRad);
      int rx = targets[i].x + 4 * sin(hRad + 2.35);
      int ry = targets[i].y - 4 * cos(hRad + 2.35);
      int lx = targets[i].x + 4 * sin(hRad - 2.35);
      int ly = targets[i].y - 4 * cos(hRad - 2.35);

      spr.fillTriangle(nx, ny, rx, ry, lx, ly, planeColor);

      spr.setTextColor(textColor, TFT_BLACK);
      spr.drawString(targets[i].callsign, (int)targets[i].x + 6, (int)targets[i].y - 12, 1);
      spr.drawString(targets[i].type,     (int)targets[i].x + 6, (int)targets[i].y, 1);
      spr.drawString(targets[i].altStr,   (int)targets[i].x + 6, (int)targets[i].y + 10, 1);
    }
  }

  // --- Draw Sweep Line (If Enabled) ---
  if (enableSweep) {
    const int sweepRadius = 118;  // Reaches the outer ring, like before
    const int trailSteps = 45;    // Narrower beam (~30 degrees) instead of the old ~62 degrees

    for (int i = 0; i < trailSteps; i++) {
      float trailAngle = sweepAngle - (i * 0.012);
      if (trailAngle < 0) trailAngle += 2 * PI;

      int tx = 120 + sweepRadius * sin(trailAngle);
      int ty = 120 - sweepRadius * cos(trailAngle);

      int lineBrightness = 255 - (int)(i * (255.0 / (trailSteps - 1)));
      if (lineBrightness < 0) lineBrightness = 0;

      uint16_t trailColor = tft.color565(0, lineBrightness, 0);
      spr.drawLine(120, 120, tx, ty, trailColor);
    }

    int endX = 120 + sweepRadius * sin(sweepAngle);
    int endY = 120 - sweepRadius * cos(sweepAngle);
    spr.drawLine(120, 120, endX, endY, TFT_GREEN);
  }

  // --- Draw Startup Title (Disappears after 5 seconds) ---
  if (!firstFetchDone || (millis() - firstFetchTime < 5000)) {
    spr.setTextColor(TFT_GREEN, TFT_BLACK);
    spr.drawCentreString("Desk Radar", 120, 25, 2);
    spr.drawCentreString("by Steffen Moll", 120, 45, 2);
  }

  spr.pushSprite(0, 0);
}

void drawRadarGrid() {
  uint16_t darkGreen = tft.color565(0, 80, 0);
  uint16_t dimText = tft.color565(0, 150, 0);

  // Circles and Crosshairs
  spr.drawCircle(120, 120, 36, darkGreen);
  spr.drawCircle(120, 120, 72, darkGreen);
  spr.drawCircle(120, 120, 118, TFT_GREEN);
  spr.drawLine(120, 2, 120, 238, darkGreen);
  spr.drawLine(2, 120, 238, 120, darkGreen);
  spr.fillRect(118, 118, 4, 4, TFT_WHITE);

  // Compass Markings
  spr.setTextColor(dimText, TFT_BLACK);
  spr.drawCentreString("N", 120, 3, 1);
  spr.drawCentreString("S", 120, 227, 1);
  spr.drawString("W", 4, 116, 1);
  spr.drawString("E", 228, 116, 1);

  // Range label on the East crosshair line (like the reference photo)
  spr.setTextColor(TFT_GREEN, TFT_BLACK);
  spr.drawString(String((int)maxRadarRangeKm) + "km", 176, 110, 1);
}

void fetchAndMapFlights() {
  HTTPClient http;
  // adsb.fi's dist parameter is nautical miles
  String url = "https://opendata.adsb.fi/api/v3/lat/" + String(radarLat, 6) +
               "/lon/" + String(radarLon, 6) +
               "/dist/" + String(maxRadarRangeKm / 1.852, 1);
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode == 200) {
    pollInterval = 11000;

    // Mark first fetch as done to start the logo removal timer
    if (!firstFetchDone) {
      firstFetchDone = true;
      firstFetchTime = millis();
    }

    JsonDocument doc; deserializeJson(doc, http.getString());

    JsonArray aircraft = doc["ac"].as<JsonArray>();

    // Build into a local buffer first, so the shared targets[] array is only
    // touched briefly at the end (under the mutex) instead of during the
    // whole parse, which keeps the render loop on the other core unblocked.
    static Target localTargets[30];
    int localCount = 0;

    for (JsonObject plane : aircraft) {
      if (localCount >= 30) break; // Memory limit protection
      if (plane["lat"].isNull() || plane["lon"].isNull()) continue; // No position yet

      float lat = plane["lat"].as<float>();
      float lon = plane["lon"].as<float>();

      float dY = (lat - radarLat) * 111.1;
      float dX = (lon - radarLon) * 111.1 * cos(radarLat * PI / 180.0);
      float r = sqrt(dX*dX + dY*dY);

      if (r > maxRadarRangeKm) continue;

      // Calculate plane coordinates relative to screen center
      float tAngle = atan2(dX, dY);
      if (tAngle < 0) tAngle += 2 * PI; // Normalize to 0-360 standard

      float x = 120 + (r/maxRadarRangeKm * 100.0) * sin(tAngle);
      float y = 120 - (r/maxRadarRangeKm * 100.0) * cos(tAngle);

      String callsign = plane["flight"].as<String>();
      callsign.trim();
      if (callsign == "" || callsign == "null") {
        callsign = plane["hex"].as<String>();
        callsign.trim();
      }
      if (callsign == "") callsign = "UNK";

      String type = plane["t"].as<String>();
      type.trim();
      if (type == "" || type == "null") type = "UNK";

      // alt_baro is either a number (feet) or the string "ground"
      String altStr;
      if (plane["alt_baro"].is<const char*>()) {
        altStr = "GND";
      } else if (!plane["alt_baro"].isNull()) {
        float alt_ft = plane["alt_baro"].as<float>();
        altStr = useMetric ? String((int)(alt_ft * 0.3048)) + "m" : String((int)alt_ft) + "ft";
      } else {
        altStr = useMetric ? "0m" : "0ft";
      }

      float gsKnots = 0.0;
      if (!plane["gs"].isNull()) {
        gsKnots = plane["gs"].as<float>();
      }

      // GET HEADING/TRACK from API
      float pHeading = 0.0;
      if (!plane["track"].isNull()) {
        pHeading = plane["track"].as<float>();
      }

      // Store all data in the local buffer
      localTargets[localCount].x = x;
      localTargets[localCount].y = y;
      localTargets[localCount].callsign = callsign;
      localTargets[localCount].type = type;      // Middle line: aircraft type (e.g. A320)
      localTargets[localCount].altStr = altStr;  // Bottom line: altitude
      localTargets[localCount].angle = tAngle;
      localTargets[localCount].heading = pHeading; // Save for drawing triangles
      localTargets[localCount].speedKmh = gsKnots * 1.852; // Save for smooth interpolation between fetches
      localCount++;
    }

    // Publish the freshly parsed targets to the shared array in one short,
    // locked step instead of incrementally while parsing.
    xSemaphoreTake(targetsMutex, portMAX_DELAY);
    for (int i = 0; i < localCount; i++) targets[i] = localTargets[i];
    numTargets = localCount;
    xSemaphoreGive(targetsMutex);
  } else if (httpCode == 429) {
    pollInterval = 60000;
  } else {
    pollInterval = 15000;
  }
  http.end();
}

void loadConfiguration() {
  preferences.begin("radar-config", true);
  radarLat = preferences.getFloat("lat", 56.9236);
  radarLon = preferences.getFloat("lon", 23.9711);
  maxRadarRangeKm = preferences.getFloat("range", 50.0);
  storedSsid = preferences.getString("ssid", "");
  storedPass = preferences.getString("pass", "");

  enableSweep = preferences.getBool("sweep", true);   // Load sweep preference
  useMetric = preferences.getBool("metric", true);     // Load metric preference

  preferences.end();
}
