#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <ArduinoJson.h>

// ─────────────────── YOUR WIFI & SERVER ───────────────────
const char* WIFI_SSID   = "YourFarmWiFi";
const char* WIFI_PASS   = "YourPassword";
const char* SERVER_URL  = "http://192.168.1.100:3000/upload";

// ─────────────────── PIN DEFINITIONS ───────────────────
#define PIN_MOTOR_1A  27
#define PIN_MOTOR_2A  14
#define PIN_MOTOR_3A  12
#define PIN_MOTOR_4A  13
#define PIN_MOTOR_EN  22

#define PIN_RELAY     3
#define PIN_BUZZER    15
#define PIN_SERVO     33

#define PIN_SOUND     32
#define PIN_LDR       35
#define PIN_DHT       19
#define PIN_OPTO_IN   34
#define PIN_VIBRATION 23
#define PIN_TOUCH     18

#define PIN_TRIG      25
#define PIN_ECHO      26

// CD4051 MUX
#define MUX_A  4
#define MUX_B  2
#define MUX_C  5
#define MUX_IN 36

// ─────────────────── CONFIGURATION ───────────────────
#define SOUND_THRESHOLD      1500
#define MOISTURE_THRESH      30.0
#define DAY_LIGHT_MIN        2000
#define PROBE_RETRIES        6
#define ULTRA_DURATION_MS    8000
#define PLANT_IR_THRESHOLD   1500   // TCRT5000: triggers when raw < this value
#define PLANT_DEBOUNCE_MS    500    // min ms between plant counts
#define SOUND_COOLDOWN_MS    10000  // min ms between animal deterrent triggers
#define DHT_INTERVAL_MS      2000   // cache DHT readings every 2s
#define MOISTURE_INTERVAL_MS 30000  // auto probe interval in auto mode

// ─────────────────── MUX CHANNELS ───────────────────
#define CH_PLANT_COUNT  0
#define CH_MOISTURE     2
#define CH_LINE_LEFT    4
#define CH_LINE_RIGHT   5

// ─────────────────── OBJECTS ───────────────────
Servo    probeServo;
DHT      dht(PIN_DHT, DHT11);
WebServer server(80);

// ─────────────────── GLOBAL STATE ───────────────────
int   plantCount       = 0;
bool  lastPlantState   = false;
bool  manualMode       = true;
bool  autoMode         = false;
float lastMoisture     = 0.0;

// Cached sensor values (updated in background)
float cachedTemp       = 0.0;
float cachedHum        = 0.0;
int   cachedLDR        = 0;
int   cachedSound      = 0;

// Timestamps for non-blocking operations
unsigned long lastDHTRead        = 0;
unsigned long lastMoistureCheck  = 0;
unsigned long lastPlantTrigger   = 0;
unsigned long lastSoundTrigger   = 0;

// ══════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);

  // Motor pins
  pinMode(PIN_MOTOR_1A, OUTPUT);
  pinMode(PIN_MOTOR_2A, OUTPUT);
  pinMode(PIN_MOTOR_3A, OUTPUT);
  pinMode(PIN_MOTOR_4A, OUTPUT);
  pinMode(PIN_MOTOR_EN, OUTPUT);
  digitalWrite(PIN_MOTOR_EN, HIGH);

  // Peripherals
  pinMode(PIN_RELAY,     OUTPUT);
  pinMode(PIN_BUZZER,    OUTPUT);
  pinMode(PIN_TRIG,      OUTPUT);
  pinMode(PIN_ECHO,      INPUT);
  pinMode(PIN_TOUCH,     INPUT);
  pinMode(PIN_VIBRATION, INPUT);
  pinMode(PIN_OPTO_IN,   INPUT);

  // MUX select lines
  pinMode(MUX_A, OUTPUT);
  pinMode(MUX_B, OUTPUT);
  pinMode(MUX_C, OUTPUT);

  probeServo.attach(PIN_SERVO);
  probeServo.write(10);   // servo up / retracted

  dht.begin();

  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS mount failed");
  }

  // Wi-Fi connect
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WARN] WiFi not connected – web UI unavailable");
  }

  // HTTP routes
  server.on("/",        handleRoot);
  server.on("/command", handleCommand);
  server.on("/data",    handleSensorData);   // JSON endpoint for live polling
  server.on("/upload",  handleManualUpload);
  server.begin();
  Serial.println("Web server started");
}

// ══════════════════════════════════════════════════════
//  MAIN LOOP  (NO delay() calls here)
// ══════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  // 1. Update cached sensor readings (non-blocking)
  updateSensors();

  // 2. Movement
  if (!manualMode) {
    followValley();
  }

  // 3. Always-on tasks
  countPlant();
  checkAnimalSound();

  // 4. Periodic moisture probe in auto mode
  if (!manualMode && (millis() - lastMoistureCheck > MOISTURE_INTERVAL_MS)) {
    lastMoistureCheck = millis();
    doProbeSequence();
  }
}

// ══════════════════════════════════════════════════════
//  NON-BLOCKING SENSOR CACHE
// ══════════════════════════════════════════════════════
void updateSensors() {
  unsigned long now = millis();
  if (now - lastDHTRead > DHT_INTERVAL_MS) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) cachedTemp  = t;
    if (!isnan(h)) cachedHum   = h;
    cachedLDR   = analogRead(PIN_LDR);
    cachedSound = analogRead(PIN_SOUND);
    lastDHTRead = now;
  }
}

// ══════════════════════════════════════════════════════
//  WEB PAGE  (dark theme, JS polling for live data)
// ══════════════════════════════════════════════════════
void handleRoot() {
  String html = R"rawlit(<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta charset="UTF-8">
<title>Farm Rover</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,-apple-system,sans-serif;background:#0d0d0d;color:#e0e0e0;padding:16px;min-height:100vh}
  h1{text-align:center;font-size:22px;font-weight:700;margin-bottom:18px;color:#fff;letter-spacing:0.5px}
  h1 span{color:#4ade80}

  /* ── Cards ── */
  .grid{display:grid;grid-template-columns:repeat(3,1fr);gap:10px;margin-bottom:16px}
  .card{background:#161622;border:1px solid #1f1f30;border-radius:14px;padding:14px 10px;text-align:center}
  .card .icon{font-size:18px;margin-bottom:6px}
  .card .lbl{font-size:10px;text-transform:uppercase;letter-spacing:0.6px;color:#5f6b7a;margin-bottom:4px}
  .card .val{font-size:20px;font-weight:700;color:#fff}
  .card .val.green{color:#4ade80}
  .card .val.yellow{color:#fbbf24}
  .card .val.red{color:#f87171}

  /* ── D-pad ── */
  .dpad{display:grid;grid-template-areas:'. up .' 'left stop right' '. down .';gap:8px;max-width:260px;margin:0 auto 16px}
  .btn{background:#1a1a2e;color:#fff;border:1px solid #25253a;padding:16px;border-radius:14px;font-size:20px;cursor:pointer;-webkit-tap-highlight-color:transparent;user-select:none;touch-action:manipulation;transition:background 0.1s}
  .btn:active,.btn.pressed{background:#2a2a44}
  .up{grid-area:up}.down{grid-area:down}.left{grid-area:left}.right{grid-area:right}
  .stop{grid-area:stop;background:#2d1220;border-color:#4a1c30;color:#f87171}
  .stop:active{background:#3d1e2a}

  /* ── Action buttons ── */
  .actions{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-bottom:12px}
  .abtn{padding:13px 8px;border-radius:13px;background:#1a1a2e;color:#fff;border:1px solid #25253a;font-size:13px;font-weight:500;cursor:pointer;touch-action:manipulation;-webkit-tap-highlight-color:transparent;transition:background 0.1s}
  .abtn:active{background:#2a2a44}
  .abtn.danger{background:#2d1220;border-color:#4a1c30;color:#f87171}
  .abtn.active-mode{border-color:#4ade80;color:#4ade80}

  /* ── Status bar ── */
  .status{text-align:center;font-size:11px;color:#3f4a58;margin-top:10px}
</style>
</head>
<body>
<h1>&#127806; Farm <span>Rover</span></h1>

<div class="grid">
  <div class="card"><div class="icon">&#127777;</div><div class="lbl">Temp</div><div class="val" id="v-temp">--</div></div>
  <div class="card"><div class="icon">&#128166;</div><div class="lbl">Humidity</div><div class="val" id="v-hum">--</div></div>
  <div class="card"><div class="icon">&#9728;</div><div class="lbl">Light</div><div class="val" id="v-light">--</div></div>
  <div class="card"><div class="icon">&#127807;</div><div class="lbl">Moisture</div><div class="val yellow" id="v-moist">--</div></div>
  <div class="card"><div class="icon">&#127807;</div><div class="lbl">Plants</div><div class="val green" id="v-plants">0</div></div>
  <div class="card"><div class="icon">&#128266;</div><div class="lbl">Sound</div><div class="val" id="v-sound">--</div></div>
</div>

<div class="dpad">
  <button class="btn up"    id="btn-F" ontouchstart="hold('F')" ontouchend="rel()" onmousedown="hold('F')" onmouseup="rel()">&#8679;</button>
  <button class="btn left"  id="btn-L" ontouchstart="hold('L')" ontouchend="rel()" onmousedown="hold('L')" onmouseup="rel()">&#8678;</button>
  <button class="btn stop"  onclick="cmd('STOP')">&#9632;</button>
  <button class="btn right" id="btn-R" ontouchstart="hold('R')" ontouchend="rel()" onmousedown="hold('R')" onmouseup="rel()">&#8680;</button>
  <button class="btn down"  id="btn-B" ontouchstart="hold('B')" ontouchend="rel()" onmousedown="hold('B')" onmouseup="rel()">&#8681;</button>
</div>

<div class="actions">
  <button class="abtn" onclick="cmd('PROBE')">&#128269; Measure</button>
  <button class="abtn danger" onclick="cmd('DETER')">&#9889; Scare</button>
  <button class="abtn" id="btn-auto"   onclick="cmd('AUTO')">&#129302; Auto</button>
  <button class="abtn active-mode" id="btn-manual" onclick="cmd('MANUAL')">&#127918; Manual</button>
  <button class="abtn danger" style="grid-column:span 2" onclick="cmd('UPLOAD')">&#9729; Upload Data</button>
</div>

<div class="status" id="status">Connecting...</div>

<script>
var holdTimer = null;
var currentCmd = '';

function cmd(c){
  fetch('/command?cmd='+c).catch(()=>{});
  if(c==='AUTO'){document.getElementById('btn-auto').classList.add('active-mode');document.getElementById('btn-manual').classList.remove('active-mode');}
  if(c==='MANUAL'){document.getElementById('btn-manual').classList.add('active-mode');document.getElementById('btn-auto').classList.remove('active-mode');}
}

function hold(c){
  currentCmd=c;
  cmd(c);
  holdTimer=setInterval(function(){cmd(c);},150);
}

function rel(){
  clearInterval(holdTimer);
  holdTimer=null;
  cmd('STOP');
  currentCmd='';
}

// Prevent context menu on long press
document.querySelectorAll('.btn').forEach(function(b){
  b.addEventListener('contextmenu',function(e){e.preventDefault();});
});

// Poll sensor data every 2 seconds
function pollData(){
  fetch('/data')
    .then(function(r){return r.json();})
    .then(function(d){
      document.getElementById('v-temp').textContent   = d.temp.toFixed(1)+'°C';
      document.getElementById('v-hum').textContent    = d.hum.toFixed(1)+'%';
      document.getElementById('v-light').textContent  = d.ldr>2000?'Day':'Night';
      document.getElementById('v-moist').textContent  = d.moisture.toFixed(1)+'%';
      document.getElementById('v-plants').textContent = d.plants;
      document.getElementById('v-sound').textContent  = d.sound;
      document.getElementById('status').textContent   = 'Updated '+new Date().toLocaleTimeString();
    })
    .catch(function(){
      document.getElementById('status').textContent='Connection lost – retrying...';
    });
}
pollData();
setInterval(pollData, 2000);
</script>
</body>
</html>)rawlit";

  server.send(200, "text/html", html);
}

// ─── JSON endpoint polled by browser every 2s ───
void handleSensorData() {
  StaticJsonDocument<200> doc;
  doc["temp"]     = cachedTemp;
  doc["hum"]      = cachedHum;
  doc["ldr"]      = cachedLDR;
  doc["sound"]    = cachedSound;
  doc["moisture"] = lastMoisture;
  doc["plants"]   = plantCount;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// ══════════════════════════════════════════════════════
//  COMMAND HANDLER
// ══════════════════════════════════════════════════════
void handleCommand() {
  String c = server.arg("cmd");
  Serial.println("[CMD] " + c);

  if      (c == "F")      goStraight();
  else if (c == "B")      goBackward();
  else if (c == "L")      turnLeft();
  else if (c == "R")      turnRight();
  else if (c == "STOP")   stopMotors();
  else if (c == "PROBE")  doProbeSequence();
  else if (c == "DETER")  emitUltrasound(ULTRA_DURATION_MS);
  else if (c == "AUTO")   { manualMode = false; autoMode = true;  stopMotors(); }
  else if (c == "MANUAL") { manualMode = true;  autoMode = false; stopMotors(); }
  else if (c == "UPLOAD") uploadData();

  server.send(200, "text/plain", "OK");
}

// ══════════════════════════════════════════════════════
//  MOTOR FUNCTIONS  (no delay – called from handler)
// ══════════════════════════════════════════════════════
void goStraight() {
  digitalWrite(PIN_MOTOR_1A, HIGH); digitalWrite(PIN_MOTOR_2A, LOW);
  digitalWrite(PIN_MOTOR_3A, HIGH); digitalWrite(PIN_MOTOR_4A, LOW);
}
void goBackward() {
  digitalWrite(PIN_MOTOR_1A, LOW);  digitalWrite(PIN_MOTOR_2A, HIGH);
  digitalWrite(PIN_MOTOR_3A, LOW);  digitalWrite(PIN_MOTOR_4A, HIGH);
}
void turnLeft() {
  digitalWrite(PIN_MOTOR_1A, LOW);  digitalWrite(PIN_MOTOR_2A, HIGH);
  digitalWrite(PIN_MOTOR_3A, HIGH); digitalWrite(PIN_MOTOR_4A, LOW);
}
void turnRight() {
  digitalWrite(PIN_MOTOR_1A, HIGH); digitalWrite(PIN_MOTOR_2A, LOW);
  digitalWrite(PIN_MOTOR_3A, LOW);  digitalWrite(PIN_MOTOR_4A, HIGH);
}
void stopMotors() {
  digitalWrite(PIN_MOTOR_1A, LOW); digitalWrite(PIN_MOTOR_2A, LOW);
  digitalWrite(PIN_MOTOR_3A, LOW); digitalWrite(PIN_MOTOR_4A, LOW);
}

// ══════════════════════════════════════════════════════
//  VALLEY / LINE FOLLOWING  (auto mode)
// ══════════════════════════════════════════════════════
void followValley() {
  bool left  = readMuxDigital(CH_LINE_LEFT);
  bool right = readMuxDigital(CH_LINE_RIGHT);

  if      ( left && !right) turnRight();
  else if (!left &&  right) turnLeft();
  else if (!left && !right) goStraight();
  else                      stopMotors();
}

// ══════════════════════════════════════════════════════
//  PLANT COUNTING  (FIX: debounce + no Serial spam)
// ══════════════════════════════════════════════════════
void countPlant() {
  int  raw   = readMuxAnalog(CH_PLANT_COUNT);
  bool state = (raw < PLANT_IR_THRESHOLD);   // TCRT5000: LOW = object detected
  unsigned long now = millis();

  if (state && !lastPlantState && (now - lastPlantTrigger > PLANT_DEBOUNCE_MS)) {
    plantCount++;
    lastPlantTrigger = now;
    Serial.print("[PLANT] Count: "); Serial.println(plantCount);

    StaticJsonDocument<128> doc;
    doc["event"] = "plant";
    doc["count"] = plantCount;
    doc["lat"]   = 0.0;
    doc["lng"]   = 0.0;
    String json; serializeJson(doc, json);
    logToSPIFFS(json);
  }
  lastPlantState = state;
}

// ══════════════════════════════════════════════════════
//  ANIMAL DETECTION  (FIX: cooldown so relay doesn't chatter)
// ══════════════════════════════════════════════════════
void checkAnimalSound() {
  unsigned long now = millis();
  if (now - lastSoundTrigger < SOUND_COOLDOWN_MS) return;  // still in cooldown

  int level = cachedSound;  // use already-read value, no extra ADC call
  if (level > SOUND_THRESHOLD) {
    lastSoundTrigger = now;
    Serial.print("[ANIMAL] Sound level: "); Serial.println(level);

    emitUltrasound(ULTRA_DURATION_MS);
    digitalWrite(PIN_RELAY, HIGH);
    delay(ULTRA_DURATION_MS);          // unavoidable wait here (hardware deter)
    digitalWrite(PIN_RELAY, LOW);

    StaticJsonDocument<128> doc;
    doc["event"] = "animal";
    doc["sound"] = level;
    doc["lat"]   = 0.0;
    doc["lng"]   = 0.0;
    String json; serializeJson(doc, json);
    logToSPIFFS(json);
  }
}

void emitUltrasound(int durationMs) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_TRIG, 40000, 8);
  ledcWrite(PIN_TRIG, 127);
  delay(durationMs);
  ledcDetach(PIN_TRIG);
#else
  ledcSetup(0, 40000, 8);
  ledcAttachPin(PIN_TRIG, 0);
  ledcWrite(0, 127);
  delay(durationMs);
  ledcDetachPin(PIN_TRIG);
#endif
  pinMode(PIN_TRIG, OUTPUT);
}

// ══════════════════════════════════════════════════════
//  SOIL MOISTURE PROBE
// ══════════════════════════════════════════════════════
void doProbeSequence() {
  int   attempts = 0;
  bool  success  = false;

  while (attempts < PROBE_RETRIES && !success) {
    probeServo.write(80);
    delay(400);                        // servo settle – acceptable one-shot

    if (digitalRead(PIN_TOUCH) == HIGH) {
      // Obstacle – retract and nudge forward
      probeServo.write(10);
      delay(300);
      goStraight();
      delay(500);
      stopMotors();
      attempts++;
    } else {
      // Good contact – average 5 reads
      float total = 0;
      for (int i = 0; i < 5; i++) {
        total += readMoisture(CH_MOISTURE);
        delay(50);
      }
      lastMoisture = total / 5.0;
      success = true;
      probeServo.write(10);
      delay(300);

      if (isDaytime() && lastMoisture < MOISTURE_THRESH) {
        digitalWrite(PIN_BUZZER, HIGH);
        delay(2000);
        digitalWrite(PIN_BUZZER, LOW);
      }

      StaticJsonDocument<128> doc;
      doc["event"] = "moisture";
      doc["value"] = lastMoisture;
      doc["lat"]   = 0.0;
      doc["lng"]   = 0.0;
      String json; serializeJson(doc, json);
      logToSPIFFS(json);

      Serial.print("[PROBE] Moisture: "); Serial.println(lastMoisture);
    }
  }

  if (!success) {
    Serial.println("[PROBE] Failed after max retries");
    StaticJsonDocument<96> doc;
    doc["event"] = "probe_fail";
    doc["lat"]   = 0.0;
    doc["lng"]   = 0.0;
    String json; serializeJson(doc, json);
    logToSPIFFS(json);
  }
}

// ══════════════════════════════════════════════════════
//  CD4051 MUX HELPERS
// ══════════════════════════════════════════════════════
void setMuxChannel(int ch) {
  digitalWrite(MUX_A, bitRead(ch, 0));
  digitalWrite(MUX_B, bitRead(ch, 1));
  digitalWrite(MUX_C, bitRead(ch, 2));
  delayMicroseconds(50);   // tiny settle, not a full ms delay
}

int readMuxAnalog(int ch) {
  setMuxChannel(ch);
  return analogRead(MUX_IN);
}

bool readMuxDigital(int ch) {
  return readMuxAnalog(ch) > 2000;
}

float readMoisture(int ch) {
  int raw = readMuxAnalog(ch);
  return map(raw, 0, 4095, 100, 0);  // resistive: dry=high ADC → low %
}

bool isDaytime() {
  return cachedLDR > DAY_LIGHT_MIN;
}

// ══════════════════════════════════════════════════════
//  DATA LOGGING & UPLOAD
// ══════════════════════════════════════════════════════
void logToSPIFFS(const String& json) {
  File f = SPIFFS.open("/datalog.json", FILE_APPEND);
  if (f) {
    f.println(json);
    f.close();
  } else {
    Serial.println("[SPIFFS] Write failed");
  }
}

void uploadData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[UPLOAD] WiFi not connected");
    return;
  }

  File f = SPIFFS.open("/datalog.json", FILE_READ);
  if (!f || f.size() == 0) {
    Serial.println("[UPLOAD] No data to send");
    if (f) f.close();
    return;
  }

  String logArray = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;
    if (!first) logArray += ",";
    logArray += line;
    first = false;
  }
  logArray += "]";
  f.close();

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(logArray);
  Serial.print("[UPLOAD] HTTP response: "); Serial.println(code);

  if (code == 200) {
    SPIFFS.remove("/datalog.json");
    Serial.println("[UPLOAD] Done – log cleared");
  }
  http.end();
}

void handleManualUpload() {
  uploadData();
  server.send(200, "text/plain", "Upload attempted");
}
