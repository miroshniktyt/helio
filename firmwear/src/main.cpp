#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <time.h>
#include <Preferences.h>
#include <SolarCalculator.h>

/* ========= WIFI ========= */
const char* ssid = "wifi";
const char* password = "pass";

/* ========= STEPPER PINS ========= */
#define STEP_X 13
#define DIR_X  12
#define STEP_Y 18
#define DIR_Y  17
#define SLEEP_RESET_STEP_PIN  6

/* ========= GEAR CALIBRATION (microsteps per degree) ========= */
// Defaults: Azimuth 17:144, Elevation 21:64. Adjust via Preferences for backlash.
#define DEFAULT_MICROSTEPS_PER_DEG_AZ  (3200.0 * 144.0 / 17.0 / 360.0)
#define DEFAULT_MICROSTEPS_PER_DEG_EL  (3200.0 * 64.0 / 21.0 / 360.0)
float microstepsPerDegAz = DEFAULT_MICROSTEPS_PER_DEG_AZ;
float microstepsPerDegEl = DEFAULT_MICROSTEPS_PER_DEG_EL;

/* ========= SERVERS ========= */
WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);
Preferences prefs;

/* ========= CONFIG (from Preferences) ========= */
float configLat = 48.21;
float configLon = 16.37;
int configGmtOffsetSec = 3600;   // UTC+1
int configDstOffsetSec = 3600;   // DST
bool configSetupDone = false;

/* ========= STEPPER STATE ========= */
bool motorX_Running = false;
bool motorY_Running = false;
unsigned long lastStepX = 0;
unsigned long lastStepY = 0;
unsigned long stepInterval = 1000;

/* ========= TRACKING STATE ========= */
bool trackingActive = false;
float currentAzDeg = 0;   // Current mirror azimuth (degrees)
float currentElDeg = 0;   // Current mirror elevation (degrees)
long currentAzMicrosteps = 0;
long currentElMicrosteps = 0;
unsigned long lastTrackUpdate = 0;
#define TRACK_UPDATE_INTERVAL_MS 5000

/* ========= STEPPER FUNCTIONS ========= */
void stepMotor(int pin) {
  digitalWrite(pin, HIGH);
  delayMicroseconds(2);
  digitalWrite(pin, LOW);
}

void updateSteppers() {
  unsigned long now = micros();
  if (motorX_Running && (now - lastStepX >= stepInterval)) {
    lastStepX = now;
    stepMotor(STEP_X);
  }
  if (motorY_Running && (now - lastStepY >= stepInterval)) {
    lastStepY = now;
    stepMotor(STEP_Y);
  }
}

/* ========= NTP & TIME ========= */
void initNTP() {
  configTime(configGmtOffsetSec, configDstOffsetSec, "pool.ntp.org");
}

bool getNTPTime(struct tm* timeinfo) {
  return getLocalTime(timeinfo);
}

time_t getUtcTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return 0;
  time_t local = mktime(&timeinfo);
  return local - configGmtOffsetSec - configDstOffsetSec;
}

/* ========= SUN POSITION ========= */
bool getSunPosition(double& azimuth, double& elevation) {
  time_t utc = getUtcTime();
  if (utc == 0) return false;
  calcHorizontalCoordinates((unsigned long)utc, configLat, configLon, azimuth, elevation);
  return true;
}

/* ========= TRACKING ========= */
unsigned long lastSunUpdate = 0;
unsigned long lastTrackStep = 0;
double targetSunAz = 0, targetSunEl = 0;
#define SUN_UPDATE_INTERVAL_MS 60000
#define TRACK_STEP_INTERVAL_US 2000

void updateTracking() {
  if (!trackingActive || !configSetupDone) return;

  unsigned long now = millis();
  if (now - lastSunUpdate >= SUN_UPDATE_INTERVAL_MS) {
    lastSunUpdate = now;
    if (!getSunPosition(targetSunAz, targetSunEl)) return;
    if (targetSunEl < 0) return;
  }

  unsigned long nowUs = micros();
  if (nowUs - lastTrackStep < TRACK_STEP_INTERVAL_US) return;
  lastTrackStep = nowUs;

  long targetAzMicrosteps = (long)(targetSunAz * microstepsPerDegAz);
  long targetElMicrosteps = (long)(targetSunEl * microstepsPerDegEl);

  long diffAz = targetAzMicrosteps - currentAzMicrosteps;
  long diffEl = targetElMicrosteps - currentElMicrosteps;

  if (abs(diffAz) > 2) {
    digitalWrite(DIR_X, diffAz > 0 ? HIGH : LOW);
    stepMotor(STEP_X);
    currentAzMicrosteps += (diffAz > 0 ? 1 : -1);
  }
  if (abs(diffEl) > 2) {
    digitalWrite(DIR_Y, diffEl > 0 ? HIGH : LOW);
    stepMotor(STEP_Y);
    currentElMicrosteps += (diffEl > 0 ? 1 : -1);
  }

  currentAzDeg = (float)currentAzMicrosteps / microstepsPerDegAz;
  currentElDeg = (float)currentElMicrosteps / microstepsPerDegEl;
}

/* ========= LOAD / SAVE CONFIG ========= */
void loadConfig() {
  prefs.begin("heliostat", true);
  configSetupDone = prefs.getBool("setup", false);
  configLat = prefs.getFloat("lat", 48.21);
  configLon = prefs.getFloat("lon", 16.37);
  configGmtOffsetSec = prefs.getInt("gmt", 3600);
  configDstOffsetSec = prefs.getInt("dst", 3600);
  microstepsPerDegAz = prefs.getFloat("calAz", DEFAULT_MICROSTEPS_PER_DEG_AZ);
  microstepsPerDegEl = prefs.getFloat("calEl", DEFAULT_MICROSTEPS_PER_DEG_EL);
  prefs.end();
}

void saveConfig(float lat, float lon, int gmtSec, int dstSec) {
  prefs.begin("heliostat", false);
  prefs.putBool("setup", true);
  prefs.putFloat("lat", lat);
  prefs.putFloat("lon", lon);
  prefs.putInt("gmt", gmtSec);
  prefs.putInt("dst", dstSec);
  prefs.putFloat("calAz", microstepsPerDegAz);
  prefs.putFloat("calEl", microstepsPerDegEl);
  prefs.end();
  configLat = lat;
  configLon = lon;
  configGmtOffsetSec = gmtSec;
  configDstOffsetSec = dstSec;
  configSetupDone = true;
}

void resetSetup() {
  prefs.begin("heliostat", false);
  prefs.putBool("setup", false);
  prefs.end();
  configSetupDone = false;
}

/* ========= WEBSOCKET HANDLER ========= */
void sendStatus(uint8_t num) {
  double sunAz = 0, sunEl = 0;
  getSunPosition(sunAz, sunEl);

  struct tm timeinfo;
  char timeStr[32] = "unknown";
  if (getLocalTime(&timeinfo)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }

  String json = "{\"status\":{";
  json += "\"tracking\":" + String(trackingActive ? "true" : "false") + ",";
  json += "\"setupDone\":" + String(configSetupDone ? "true" : "false") + ",";
  json += "\"sunAz\":" + String(sunAz, 2) + ",";
  json += "\"sunEl\":" + String(sunEl, 2) + ",";
  json += "\"mirrorAz\":" + String(currentAzDeg, 2) + ",";
  json += "\"mirrorEl\":" + String(currentElDeg, 2) + ",";
  json += "\"time\":\"" + String(timeStr) + "\"";
  json += "}}";
  webSocket.sendTXT(num, json);
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_TEXT) {
    String msg = (char*)payload;

    if (msg == "X_fwd") { digitalWrite(DIR_X, HIGH); motorX_Running = true; }
    else if (msg == "X_rev") { digitalWrite(DIR_X, LOW); motorX_Running = true; }
    else if (msg == "X_stop") { motorX_Running = false; }
    else if (msg == "Y_fwd") { digitalWrite(DIR_Y, HIGH); motorY_Running = true; }
    else if (msg == "Y_rev") { digitalWrite(DIR_Y, LOW); motorY_Running = true; }
    else if (msg == "Y_stop") { motorY_Running = false; }

    else if (msg == "get_status") { sendStatus(num); }

    else if (msg == "start_track") {
      trackingActive = true;
      lastSunUpdate = 0;
      sendStatus(num);
    }
    else if (msg == "stop_track") {
      trackingActive = false;
      motorX_Running = false;
      motorY_Running = false;
      sendStatus(num);
    }

    else if (msg.startsWith("setup_complete:")) {
      int sep1 = msg.indexOf(',', 15);
      int sep2 = msg.indexOf(',', sep1 + 1);
      int sep3 = msg.indexOf(',', sep2 + 1);
      if (sep1 > 0 && sep2 > 0 && sep3 > 0) {
        float lat = msg.substring(15, sep1).toFloat();
        float lon = msg.substring(sep1 + 1, sep2).toFloat();
        int gmtSec = msg.substring(sep2 + 1, sep3).toInt();
        int dstSec = msg.substring(sep3 + 1).toInt();
        saveConfig(lat, lon, gmtSec, dstSec);
        initNTP();
        sendStatus(num);
      }
    }

    else if (msg == "reset_setup") {
      resetSetup();
      trackingActive = false;
      motorX_Running = false;
      motorY_Running = false;
      sendStatus(num);
    }
  }
}

/* ========= HTML PAGE ========= */
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    * { box-sizing: border-box; }
    body { font-family: system-ui, sans-serif; background: #1a1a2e; color: #eee; margin: 0; padding: 20px; }
    .container { max-width: 500px; margin: 0 auto; }
    h2 { margin-top: 0; color: #e94560; }
    .mode-toggle { display: flex; gap: 8px; margin-bottom: 20px; }
    .mode-toggle button { flex: 1; padding: 12px; border: none; border-radius: 8px; cursor: pointer; font-size: 14px; }
    .mode-toggle button.active { background: #e94560; color: white; }
    .mode-toggle button:not(.active) { background: #333; color: #aaa; }
    .panel { display: none; padding: 16px; background: #16213e; border-radius: 10px; margin-bottom: 16px; }
    .panel.visible { display: block; }
    .instructions { background: #0f3460; padding: 12px; border-radius: 8px; margin-bottom: 12px; font-size: 14px; line-height: 1.5; }
    .instructions ol { margin: 8px 0 0 8px; padding-left: 16px; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin: 12px 0; }
    button { font-size: 18px; padding: 25px; border-radius: 10px; border: none; background: #333; color: white; cursor: pointer; }
    button:active { background: #e94560; }
    button.primary { background: #e94560; padding: 14px 20px; font-size: 16px; width: 100%; margin-top: 8px; }
    button.primary:hover { background: #ff6b6b; }
    input, select { padding: 10px; border-radius: 6px; border: 1px solid #444; background: #1a1a2e; color: #eee; width: 100%; }
    label { display: block; margin: 8px 0 4px; font-size: 13px; color: #aaa; }
    .status-row { display: flex; justify-content: space-between; padding: 6px 0; font-size: 14px; }
    .status-row span { color: #e94560; }
  </style>
</head>
<body>
  <div class="container">
    <h2>Heliostat Sun Tracker</h2>
    <div class="mode-toggle">
      <button id="btnSetup" class="active">Setup</button>
      <button id="btnManual">Manual</button>
      <button id="btnTrack">Tracking</button>
    </div>

    <div id="panelSetup" class="panel visible">
      <div class="instructions">
        <strong>Setup Instructions</strong>
        <ol>
          <li>Point the mirror north (vertical) using a phone compass. Use the Manual buttons below to align.</li>
          <li>Enter your location or use "Use my location".</li>
          <li>Select your timezone.</li>
          <li>Click "Finish Setup" to save.</li>
        </ol>
      </div>
      <label>Latitude</label>
      <input type="number" id="lat" step="0.0001" placeholder="e.g. 48.21" value="48.21">
      <label>Longitude</label>
      <input type="number" id="lon" step="0.0001" placeholder="e.g. 16.37" value="16.37">
      <button type="button" onclick="useMyLocation()" style="margin: 8px 0; padding: 10px; font-size: 14px;">Use my location</button>
      <label>Timezone (UTC offset)</label>
      <select id="tz">
        <option value="-43200">UTC-12</option>
        <option value="-39600">UTC-11</option>
        <option value="-36000">UTC-10</option>
        <option value="-32400">UTC-9</option>
        <option value="-28800">UTC-8</option>
        <option value="-25200">UTC-7</option>
        <option value="-21600">UTC-6</option>
        <option value="-18000">UTC-5</option>
        <option value="-14400">UTC-4</option>
        <option value="-10800">UTC-3</option>
        <option value="-7200">UTC-2</option>
        <option value="-3600">UTC-1</option>
        <option value="0">UTC</option>
        <option value="3600" selected>UTC+1</option>
        <option value="7200">UTC+2</option>
        <option value="10800">UTC+3</option>
        <option value="14400">UTC+4</option>
        <option value="18000">UTC+5</option>
        <option value="21600">UTC+6</option>
        <option value="25200">UTC+7</option>
        <option value="28800">UTC+8</option>
        <option value="32400">UTC+9</option>
        <option value="36000">UTC+10</option>
        <option value="39600">UTC+11</option>
        <option value="43200">UTC+12</option>
      </select>
      <label>Daylight Saving Time (seconds)</label>
      <select id="dst">
        <option value="0">No DST</option>
        <option value="3600" selected>+1 hour</option>
      </select>
      <button class="primary" id="btnFinishSetup">Finish Setup</button>
      <button type="button" id="btnResetSetup" style="background: #555; margin-top: 8px; padding: 10px; font-size: 14px;">Reset Setup</button>
      <p id="setupMsg" style="font-size: 13px; margin-top: 8px; color: #4ade80;"></p>
    </div>

    <div id="panelManual" class="panel">
      <div class="instructions">Use these buttons to align the mirror north. Combine directions for diagonal movement.</div>
      <div id="dpadContainer" style="display: grid; grid-template-columns: repeat(3, 1fr); grid-template-rows: repeat(3, 1fr); gap: 5px; width: 250px; height: 250px; margin: 30px auto;">
        <button id="upLeft" style="font-size: 20px;">&nwarr;</button>
        <button id="up" style="font-size: 24px;">&uarr;</button>
        <button id="upRight" style="font-size: 20px;">&nearr;</button>
        <button id="left" style="font-size: 24px;">&larr;</button>
        <button style="background: #222; border: none; cursor: default;"></button> <!-- Center filler -->
        <button id="right" style="font-size: 24px;">&rarr;</button>
        <button id="downLeft" style="font-size: 20px;">&swarr;</button>
        <button id="down" style="font-size: 24px;">&darr;</button>
        <button id="downRight" style="font-size: 20px;">&searr;</button>
      </div>
    </div>

    <div id="panelTrack" class="panel">
      <div class="instructions">Start tracking to point the mirror at the sun.</div>
      <div id="statusBox" style="background: #0f3460; padding: 12px; border-radius: 8px; margin-bottom: 12px;">
        <div class="status-row">Sun Azimuth: <span id="sunAz">-</span></div>
        <div class="status-row">Sun Elevation: <span id="sunEl">-</span></div>
        <div class="status-row">Mirror Az: <span id="mirrorAz">-</span></div>
        <div class="status-row">Mirror El: <span id="mirrorEl">-</span></div>
        <div class="status-row">Time: <span id="time">-</span></div>
      </div>
      <div class="grid">
        <button class="primary" id="btnStartTrack">Start Tracking</button>
        <button class="primary" id="btnStopTrack" style="background: #555;">Stop</button>
      </div>
    </div>
  </div>

<script>
const ws = new WebSocket("ws://" + location.hostname + ":81");

function send(msg) { if (ws.readyState === 1) ws.send(msg); }

function bindControl(id, startMsg, stopMsg) {
  const btn = document.getElementById(id);
  const start = (e) => { e.preventDefault(); send(startMsg); };
  const stop = (e) => { e.preventDefault(); send(stopMsg); };
  btn.addEventListener("mousedown", start);
  btn.addEventListener("mouseup", stop);
  btn.addEventListener("touchstart", start);
  btn.addEventListener("touchend", stop);
  btn.addEventListener("mouseleave", stop);
}

bindControl("right", "X_fwd", "X_stop");
bindControl("left", "X_rev", "X_stop");
bindControl("up", "Y_fwd", "Y_stop");
bindControl("down", "Y_rev", "Y_stop");

// Diagonal buttons: press both axes simultaneously
const diagonalBindings = {
  "upLeft": ["Y_fwd", "X_rev"],
  "upRight": ["Y_fwd", "X_fwd"],
  "downLeft": ["Y_rev", "X_rev"],
  "downRight": ["Y_rev", "X_fwd"]
};

Object.entries(diagonalBindings).forEach(([id, [msgY, msgX]]) => {
  const btn = document.getElementById(id);
  btn.addEventListener("mousedown", (e) => {
    e.preventDefault();
    send(msgY);
    send(msgX);
  });
  btn.addEventListener("mouseup", (e) => {
    e.preventDefault();
    send("X_stop");
    send("Y_stop");
  });
  btn.addEventListener("touchstart", (e) => {
    e.preventDefault();
    send(msgY);
    send(msgX);
  });
  btn.addEventListener("touchend", (e) => {
    e.preventDefault();
    send("X_stop");
    send("Y_stop");
  });
  btn.addEventListener("mouseleave", (e) => {
    e.preventDefault();
    send("X_stop");
    send("Y_stop");
  });
});

function useMyLocation() {
  if (!navigator.geolocation) {
    document.getElementById("setupMsg").textContent = "Geolocation not supported.";
    return;
  }
  document.getElementById("setupMsg").textContent = "Getting location...";
  navigator.geolocation.getCurrentPosition(
    (pos) => {
      document.getElementById("lat").value = pos.coords.latitude.toFixed(4);
      document.getElementById("lon").value = pos.coords.longitude.toFixed(4);
      document.getElementById("setupMsg").textContent = "Location set.";
    },
    (err) => { document.getElementById("setupMsg").textContent = "Geolocation failed: " + err.message; }
  );
}

function showPanel(id) {
  document.querySelectorAll(".panel").forEach(p => p.classList.remove("visible"));
  document.querySelectorAll(".mode-toggle button").forEach(b => b.classList.remove("active"));
  document.getElementById("panel" + id).classList.add("visible");
  document.getElementById("btn" + id).classList.add("active");
}

document.getElementById("btnSetup").onclick = () => showPanel("Setup");
document.getElementById("btnManual").onclick = () => showPanel("Manual");
document.getElementById("btnTrack").onclick = () => showPanel("Track");

document.getElementById("btnResetSetup").onclick = function() {
  send("reset_setup");
  document.getElementById("setupMsg").textContent = "Setup reset. Configure again.";
};

document.getElementById("btnFinishSetup").onclick = function() {
  const lat = parseFloat(document.getElementById("lat").value);
  const lon = parseFloat(document.getElementById("lon").value);
  const gmtSec = parseInt(document.getElementById("tz").value);
  const dstSec = parseInt(document.getElementById("dst").value);
  if (isNaN(lat) || isNaN(lon)) {
    document.getElementById("setupMsg").textContent = "Please enter valid lat/lon.";
    return;
  }
  send("setup_complete:" + lat + "," + lon + "," + gmtSec + "," + dstSec);
  document.getElementById("setupMsg").textContent = "Setup saved!";
};

document.getElementById("btnStartTrack").onclick = () => send("start_track");
document.getElementById("btnStopTrack").onclick = () => send("stop_track");

let statusInterval;
ws.onopen = function() {
  send("get_status");
  statusInterval = setInterval(() => send("get_status"), 2000);
};

ws.onmessage = function(e) {
  try {
    const msg = JSON.parse(e.data);
    if (msg.status) {
      const s = msg.status;
      document.getElementById("sunAz").textContent = (s.sunAz != null ? s.sunAz : 0).toFixed(2) + "°";
      document.getElementById("sunEl").textContent = (s.sunEl != null ? s.sunEl : 0).toFixed(2) + "°";
      document.getElementById("mirrorAz").textContent = (s.mirrorAz != null ? s.mirrorAz : 0).toFixed(2) + "°";
      document.getElementById("mirrorEl").textContent = (s.mirrorEl != null ? s.mirrorEl : 0).toFixed(2) + "°";
      document.getElementById("time").textContent = s.time || "-";
    }
  } catch (_) {}
};

ws.onclose = () => clearInterval(statusInterval);
</script>
</body>
</html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", htmlPage); }

void setup() {
  pinMode(STEP_X, OUTPUT);
  pinMode(DIR_X, OUTPUT);
  pinMode(STEP_Y, OUTPUT);
  pinMode(DIR_Y, OUTPUT);
  pinMode(SLEEP_RESET_STEP_PIN, OUTPUT);
  digitalWrite(SLEEP_RESET_STEP_PIN, HIGH);

  Serial.begin(115200);
  delay(2000);
  Serial.println("Heliostat starting...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  loadConfig();
  initNTP();

  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries++ < 10) {
    Serial.println("Waiting for NTP...");
    delay(1000);
  }
  if (getLocalTime(&timeinfo)) {
    Serial.printf("Time: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  double az, el;
  if (getSunPosition(az, el)) {
    Serial.printf("Sun (test): az=%.2f el=%.2f\n", az, el);
  }

  server.on("/", handleRoot);
  server.begin();
  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);
}

void loop() {
  server.handleClient();
  webSocket.loop();
  updateSteppers();

  if (trackingActive && (millis() - lastTrackUpdate >= TRACK_UPDATE_INTERVAL_MS)) {
    lastTrackUpdate = millis();
    updateTracking();
  }
}
