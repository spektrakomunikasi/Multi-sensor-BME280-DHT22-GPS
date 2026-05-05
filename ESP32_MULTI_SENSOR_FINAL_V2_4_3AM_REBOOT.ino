#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <FS.h>
#include <time.h>
#include <math.h>
#include <TinyGPSPlus.h>
#include "esp_wifi.h"

// =========================
// AP default
// =========================
const char* AP_SSID = "MULTI SENSOR";
const char* AP_PASS = "12345678";

// =========================
// NTP WIB
// =========================
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.nist.gov";
const long  GMT_OFFSET_SEC = 7 * 3600;
const int   DAYLIGHT_OFFSET_SEC = 0;

// =========================
// Interval sistem
// =========================
const unsigned long SENSOR_INTERVAL_MS  = 1000;
const unsigned long LOG_INTERVAL_MS     = 5000;
const unsigned long BASELINE_MS         = 15000;

// LCD super halus
const unsigned long LCD20_TIME_MS   = 1000; // row 0
const unsigned long LCD20_DATA_MS   = 2000; // row 1..3
const unsigned long LCD16_STATUS_MS = 3000; // 16x2

// =========================
// Log retention (configurable)
// =========================
int logRetentionDays = 7; // 1..60

// =========================
// Auto reboot harian jam 03:00
// =========================
const int AUTO_REBOOT_HOUR = 3;
const int AUTO_REBOOT_MIN  = 0;
bool rebootDoneToday = false;
int  rebootDayOfYear = -1;

// =========================
// Hardware
// =========================
LiquidCrystal_I2C lcd20(0x27, 20, 4); // main
LiquidCrystal_I2C lcd16(0x26, 16, 2); // aux
Adafruit_BME280 bme;
DHT dht(23, DHT22);

// GPS UART2: RX=16 TX=17 (ESP32)
HardwareSerial GPSserial(2);
TinyGPSPlus gps;

// =========================
// Services
// =========================
WebServer server(80);
Preferences prefs;

// =========================
// GPS Auto Baud
// =========================
const uint32_t GPS_BAUD_LIST[] = {9600, 38400, 115200};
const int GPS_BAUD_COUNT = sizeof(GPS_BAUD_LIST) / sizeof(GPS_BAUD_LIST[0]);
uint32_t gpsBaudActive = 9600;
bool gpsBaudLocked = false;

// =========================
// Runtime
// =========================
String staSSID = "", staPASS = "";
bool bmeOK=false, dhtOK=false, ntpSynced=false;

float tBME=NAN, hBME=NAN, p_hPa=NAN, altAbsM=NAN;
float tDHT=NAN, hDHT=NAN;
float seaLevelPressure_hPa = 1013.25f;

bool baselineReady=false;
unsigned long baselineStart=0;
double p0Sum=0.0;
uint32_t p0Count=0;
float p0_hPa=NAN;
float altRelM=NAN;

bool gpsFix=false;
double gpsLat=NAN, gpsLon=NAN, gpsAltM=NAN, gpsSpd=NAN, gpsHdop=NAN;
uint32_t gpsSat=0;
uint32_t gpsChars=0, gpsSentences=0, gpsFailed=0;

unsigned long lastSensor=0, lastLog=0;
unsigned long lastLcd20Time=0, lastLcd20Data=0, lastLcd16=0;

// LCD cache anti-flicker
String lcd20_r0="", lcd20_r1="", lcd20_r2="", lcd20_r3="";
String lcd16_r0="", lcd16_r1="";

// =========================
// Helpers
// =========================
String apIP(){ return WiFi.softAPIP().toString(); }
String staIP(){ return (WiFi.status()==WL_CONNECTED)?WiFi.localIP().toString():"-"; }
String staStatus(){ return (WiFi.status()==WL_CONNECTED)?"CONNECTED":"DISCONNECTED"; }

String nowString(){
  struct tm t;
  if(getLocalTime(&t,50)){
    char b[24]; strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",&t); return String(b);
  }
  return "1970-01-01 00:00:00";
}

String todayFileName(){
  struct tm t;
  if(getLocalTime(&t,50)){
    char b[24]; strftime(b,sizeof(b),"/log_%Y%m%d.csv",&t); return String(b);
  }
  return "/log_19700101.csv";
}

String jsonNum(float v,int d=2){ if(isnan(v)||isinf(v)) return "null"; return String(v,d); }
String jsonNumD(double v,int d=6){ if(isnan(v)||isinf(v)) return "null"; return String(v,d); }

String padRight(String s, uint8_t w) {
  if (s.length() > w) return s.substring(0, w);
  while (s.length() < w) s += " ";
  return s;
}
void printLineIfChanged(LiquidCrystal_I2C &lcd, uint8_t col, uint8_t row, String txt, String &cache, uint8_t width) {
  txt = padRight(txt, width);
  if (txt != cache) {
    lcd.setCursor(col, row);
    lcd.print(txt);
    cache = txt;
  }
}

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void loadWiFiCreds(){
  prefs.begin("wifi", true);
  staSSID = prefs.getString("ssid","");
  staPASS = prefs.getString("pass","");
  prefs.end();
}
void saveWiFiCreds(const String& ssid,const String& pass){
  prefs.begin("wifi", false);
  prefs.putString("ssid",ssid);
  prefs.putString("pass",pass);
  prefs.end();
  staSSID=ssid; staPASS=pass;
}
void clearWiFiCreds(){
  prefs.begin("wifi", false);
  prefs.remove("ssid"); prefs.remove("pass");
  prefs.end();
  staSSID=""; staPASS="";
}

void loadRetentionSetting(){
  Preferences p;
  p.begin("cfg", true);
  int v = p.getInt("ret_days", 7);
  p.end();
  if(v < 1) v = 1;
  if(v > 60) v = 60;
  logRetentionDays = v;
}

void saveRetentionSetting(int days){
  if(days < 1) days = 1;
  if(days > 60) days = 60;
  Preferences p;
  p.begin("cfg", false);
  p.putInt("ret_days", days);
  p.end();
  logRetentionDays = days;
}

void connectSTA(){
  if(staSSID.isEmpty()) return;
  WiFi.begin(staSSID.c_str(), staPASS.c_str());
  unsigned long t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<12000) delay(250);
}

void initNTP(){
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  struct tm t; ntpSynced = getLocalTime(&t,3000);
}

float altRelFromP0(float p,float p0){
  if(isnan(p)||isnan(p0)||p<=0||p0<=0) return NAN;
  return 44330.0f * (1.0f - powf(p/p0,0.1903f));
}

void startBaseline(){
  baselineStart=millis();
  p0Sum=0.0; p0Count=0; p0_hPa=NAN; baselineReady=false; altRelM=NAN;
}

void updateBaseline(){
  if(baselineReady || isnan(p_hPa)) return;
  if(millis()-baselineStart<=BASELINE_MS){
    p0Sum += p_hPa; p0Count++;
  } else if(p0Count>=8){
    p0_hPa = (float)(p0Sum/(double)p0Count);
    baselineReady = true;
  }
}

// ===== Log retention helpers =====
bool parseLogDate(const String& fn, int &y, int &m, int &d) {
  if (!fn.startsWith("/log_") || !fn.endsWith(".csv") || fn.length() < 17) return false;
  String s = fn.substring(5, 13); // YYYYMMDD
  y = s.substring(0,4).toInt();
  m = s.substring(4,6).toInt();
  d = s.substring(6,8).toInt();
  return (y>=2020 && m>=1 && m<=12 && d>=1 && d<=31);
}

time_t makeTimeLocal(int y, int m, int d) {
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon  = m - 1;
  t.tm_mday = d;
  t.tm_hour = 0;
  t.tm_min  = 0;
  t.tm_sec  = 0;
  return mktime(&t);
}

void cleanupOldLogs() {
  struct tm nowTm;
  if (!getLocalTime(&nowTm, 1000)) {
    Serial.println("[LOG] cleanup skip: waktu NTP belum valid");
    return;
  }

  time_t nowT = time(nullptr);
  if (nowT < 1700000000) {
    Serial.println("[LOG] cleanup skip: epoch belum valid");
    return;
  }

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fn = file.name();
    int y, m, d;
    if (parseLogDate(fn, y, m, d)) {
      time_t ft = makeTimeLocal(y, m, d);
      double ageDays = difftime(nowT, ft) / 86400.0;
      if (ageDays > logRetentionDays) {
        Serial.printf("[LOG] delete old: %s (%.1f days)\n", fn.c_str(), ageDays);
        SPIFFS.remove(fn);
      }
    }
    file = root.openNextFile();
  }
}

// ===== Auto reboot scheduler =====
void handleAutoRebootSchedule() {
  struct tm t;
  if(!getLocalTime(&t, 50)) return;

  if (rebootDayOfYear != t.tm_yday) {
    rebootDayOfYear = t.tm_yday;
    rebootDoneToday = false;
  }

  if (!rebootDoneToday &&
      t.tm_hour == AUTO_REBOOT_HOUR &&
      t.tm_min  == AUTO_REBOOT_MIN) {
    rebootDoneToday = true;
    Serial.println("[SYS] Auto reboot 03:00 triggered");
    delay(300);
    ESP.restart();
  }
}

bool testGPSBaud(uint32_t baud, uint32_t testMs = 2200) {
  TinyGPSPlus probe;
  GPSserial.end();
  delay(80);
  GPSserial.begin(baud, SERIAL_8N1, 16, 17); // RX,TX ESP32

  uint32_t t0 = millis();
  uint32_t chars0 = 0;
  while (millis() - t0 < testMs) {
    while (GPSserial.available()) {
      char c = (char)GPSserial.read();
      probe.encode(c);
      chars0++;
    }
    delay(2);
  }

  bool ok = (chars0 > 80);
  Serial.printf("[GPS] test baud %lu -> chars=%lu => %s\n",
                (unsigned long)baud, (unsigned long)chars0, ok ? "OK" : "NO");
  return ok;
}

void autoDetectGPSBaud() {
  gpsBaudLocked = false;

  for (int i = 0; i < GPS_BAUD_COUNT; i++) {
    uint32_t b = GPS_BAUD_LIST[i];
    if (testGPSBaud(b)) {
      gpsBaudActive = b;
      gpsBaudLocked = true;
      break;
    }
  }

  if (!gpsBaudLocked) {
    gpsBaudActive = 9600;
    Serial.println("[GPS] Auto-baud gagal, fallback 9600");
  } else {
    Serial.printf("[GPS] Auto-baud LOCKED: %lu\n", (unsigned long)gpsBaudActive);
  }

  GPSserial.end();
  delay(80);
  GPSserial.begin(gpsBaudActive, SERIAL_8N1, 16, 17);
}

void updateGPS(){
  while(GPSserial.available()){
    char c = (char)GPSserial.read();
    gps.encode(c);
  }

  gpsFix = gps.location.isValid();
  gpsSat = gps.satellites.isValid()?gps.satellites.value():0;
  gpsLat = gps.location.isValid()?gps.location.lat():NAN;
  gpsLon = gps.location.isValid()?gps.location.lng():NAN;
  gpsAltM= gps.altitude.isValid()?gps.altitude.meters():NAN;
  gpsSpd = gps.speed.isValid()?gps.speed.kmph():NAN;
  gpsHdop= gps.hdop.isValid()?(gps.hdop.value()/100.0):NAN;

  gpsChars     = gps.charsProcessed();
  gpsSentences = gps.sentencesWithFix();
  gpsFailed    = gps.failedChecksum();
}

void readSensors(){
  if(bmeOK){
    tBME=bme.readTemperature();
    hBME=bme.readHumidity();
    p_hPa=bme.readPressure()/100.0f;
    altAbsM=bme.readAltitude(seaLevelPressure_hPa);
    if(isnan(tBME)||isnan(hBME)||isnan(p_hPa)) bmeOK=false;
  }else{
    if(bme.begin(0x76) || bme.begin(0x77)) bmeOK=true;
  }

  float td=dht.readTemperature(), hd=dht.readHumidity();
  if(!isnan(td)&&!isnan(hd)){ tDHT=td; hDHT=hd; dhtOK=true; } else dhtOK=false;

  updateBaseline();
  if(baselineReady) altRelM = altRelFromP0(p_hPa,p0_hPa);

  updateGPS();
}

// =========================
// LCD super halus
// =========================
void drawLCD20_TimeRow() {
  String ts = nowString();
  char b0[32];
  snprintf(b0,sizeof(b0),"%8s STA:%-2s GPS:%s",
           ts.substring(11,19).c_str(),
           (WiFi.status()==WL_CONNECTED)?"OK":"NO",
           gpsFix?"OK":"NO");
  printLineIfChanged(lcd20, 0, 0, String(b0), lcd20_r0, 20);
}

void drawLCD20_DataRows() {
  float tShow=!isnan(tBME)?tBME:tDHT;
  float hShow=!isnan(hBME)?hBME:hDHT;
  float inHg=!isnan(p_hPa)?(p_hPa*0.0295299830714f):NAN;

  char b1[32], b2[32], b3[32];
  if(!isnan(tShow)&&!isnan(hShow)) snprintf(b1,sizeof(b1),"T:%5.1fC  H:%3.0f%%",tShow,hShow);
  else snprintf(b1,sizeof(b1),"T/H: OFFLINE");

  if(!isnan(p_hPa)&&!isnan(inHg)) snprintf(b2,sizeof(b2),"P:%6.1f %5.2finHg",p_hPa,inHg);
  else snprintf(b2,sizeof(b2),"Pressure: OFFLINE");

  if(!baselineReady){
    unsigned long rem=(BASELINE_MS>(millis()-baselineStart))?(BASELINE_MS-(millis()-baselineStart)):0;
    snprintf(b3,sizeof(b3),"ALT REL: CAL.. %2lus", rem/1000UL);
  } else if(!isnan(altRelM)){
    snprintf(b3,sizeof(b3),"ALT REL:%7.1f m",altRelM);
  } else snprintf(b3,sizeof(b3),"ALT REL: OFFLINE");

  printLineIfChanged(lcd20, 0, 1, String(b1), lcd20_r1, 20);
  printLineIfChanged(lcd20, 0, 2, String(b2), lcd20_r2, 20);
  printLineIfChanged(lcd20, 0, 3, String(b3), lcd20_r3, 20);
}

void drawLCD16_Smooth() {
  char b0[24], b1[24];
  snprintf(b0,sizeof(b0),"LOG:ON GPS:%s", gpsFix?"FIX":"NOF");

  if(gpsFix && !isnan(gpsLat)) snprintf(b1,sizeof(b1),"LAT:%8.5f", gpsLat);
  else snprintf(b1,sizeof(b1),"LAT: NO FIX");

  printLineIfChanged(lcd16, 0, 0, String(b0), lcd16_r0, 16);
  printLineIfChanged(lcd16, 0, 1, String(b1), lcd16_r1, 16);
}

void initLcdSmoothBoot() {
  lcd20.clear();
  lcd16.clear();
  lcd20_r0=""; lcd20_r1=""; lcd20_r2=""; lcd20_r3="";
  lcd16_r0=""; lcd16_r1="";
  drawLCD20_TimeRow();
  drawLCD20_DataRows();
  drawLCD16_Smooth();
}

void updateLcdSmoothScheduler() {
  unsigned long now = millis();

  if (now - lastLcd20Time >= LCD20_TIME_MS) {
    lastLcd20Time = now;
    drawLCD20_TimeRow();
  }
  if (now - lastLcd20Data >= LCD20_DATA_MS) {
    lastLcd20Data = now;
    drawLCD20_DataRows();
  }
  if (now - lastLcd16 >= LCD16_STATUS_MS) {
    lastLcd16 = now;
    drawLCD16_Smooth();
  }
}

// =========================
// Log
// =========================
void ensureHeader(const String& fn){
  if(!SPIFFS.exists(fn)){
    File f=SPIFFS.open(fn,FILE_WRITE);
    if(f){
      f.println("timestamp,bme_ok,dht_ok,temp_c,hum_pct,pressure_hpa,alt_rel_m,p0_hpa,baseline_ready,gps_fix,gps_sat,gps_lat,gps_lon,gps_alt_m,gps_chars,gps_sentences_fix,gps_failed_checksum,gps_baud,gps_baud_locked,sta_status,sta_ip");
      f.close();
    }
  }
}

void appendLog(){
  String fn=todayFileName();
  ensureHeader(fn);

  File f=SPIFFS.open(fn,FILE_APPEND);
  if(!f) return;

  float tShow=!isnan(tBME)?tBME:tDHT;
  float hShow=!isnan(hBME)?hBME:hDHT;

  String line;
  line.reserve(420);
  line += nowString(); line += ",";
  line += (bmeOK?"1":"0"); line += ",";
  line += (dhtOK?"1":"0"); line += ",";
  line += String(tShow,2); line += ",";
  line += String(hShow,2); line += ",";
  line += String(p_hPa,2); line += ",";
  line += String(altRelM,2); line += ",";
  line += String(p0_hPa,2); line += ",";
  line += (baselineReady?"1":"0"); line += ",";
  line += (gpsFix?"1":"0"); line += ",";
  line += String(gpsSat); line += ",";
  line += String(gpsLat,6); line += ",";
  line += String(gpsLon,6); line += ",";
  line += String(gpsAltM,2); line += ",";
  line += String(gpsChars); line += ",";
  line += String(gpsSentences); line += ",";
  line += String(gpsFailed); line += ",";
  line += String(gpsBaudActive); line += ",";
  line += (gpsBaudLocked?"1":"0"); line += ",";
  line += staStatus(); line += ",";
  line += staIP();

  f.println(line);
  f.close();
}

// =========================
// Web handlers
// =========================
void handleApi(){
  float tShow=!isnan(tBME)?tBME:tDHT;
  float hShow=!isnan(hBME)?hBME:hDHT;

  String j="{";
  j += "\"time\":\""+nowString()+"\",";
  j += "\"ap_ip\":\""+apIP()+"\",";
  j += "\"sta_status\":\""+staStatus()+"\",";
  j += "\"sta_ip\":\""+staIP()+"\",";
  j += "\"temperature_c\":"+jsonNum(tShow,2)+",";
  j += "\"humidity_pct\":"+jsonNum(hShow,2)+",";
  j += "\"pressure_hpa\":"+jsonNum(p_hPa,2)+",";
  j += "\"alt_rel_m\":"+jsonNum(altRelM,2)+",";
  j += "\"baseline_ready\":" + String(baselineReady?"true":"false") + ",";
  j += "\"gps_fix\":" + String(gpsFix?"true":"false") + ",";
  j += "\"gps_sat\":" + String(gpsSat) + ",";
  j += "\"gps_lat\":" + jsonNumD(gpsLat,6) + ",";
  j += "\"gps_lon\":" + jsonNumD(gpsLon,6) + ",";
  j += "\"gps_chars\":" + String(gpsChars) + ",";
  j += "\"gps_sentences_fix\":" + String(gpsSentences) + ",";
  j += "\"gps_failed_checksum\":" + String(gpsFailed) + ",";
  j += "\"gps_baud\":" + String(gpsBaudActive) + ",";
  j += "\"gps_baud_locked\":" + String(gpsBaudLocked ? "true" : "false") + ",";
  j += "\"log_retention_days\":" + String(logRetentionDays);
  j += "}";

  sendNoCacheHeaders();
  server.send(200,"application/json; charset=utf-8",j);
}

void handleSetRetention(){
  String d = server.arg("days");
  int days = d.toInt();
  if(days < 1 || days > 60){
    sendNoCacheHeaders();
    server.send(400, "text/plain", "days must be 1..60");
    return;
  }

  saveRetentionSetting(days);
  cleanupOldLogs();

  sendNoCacheHeaders();
  server.sendHeader("Location","/");
  server.send(303);
}

void handleRoot() {
  const char* page = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8"/><meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>MULTI SENSOR Dashboard</title>
<style>
:root{--bg:#0b1220;--card:#172033;--text:#eaf2ff;--muted:#9fb3d1;--line:#2a3858}
body{margin:0;font-family:Arial;background:var(--bg);color:var(--text)}
.wrap{max-width:1220px;margin:16px auto;padding:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin:10px 0}
.grid{display:grid;gap:12px}.g4{grid-template-columns:repeat(auto-fit,minmax(240px,1fr))}
.g2{grid-template-columns:repeat(auto-fit,minmax(360px,1fr))}
.row{display:flex;justify-content:space-between;flex-wrap:wrap;gap:8px}
.kpi{font-size:28px;font-weight:700}.muted{color:var(--muted)}
input,button,select{padding:10px;border-radius:8px;border:1px solid var(--line);background:#0f1a2d;color:#fff}
button{cursor:pointer}.ok{color:#62e18a}.bad{color:#ff7f7f}
.fallback{display:none}
a.mapbtn{display:inline-block;padding:8px 12px;border-radius:8px;border:1px solid var(--line);text-decoration:none;color:#fff;background:#0f1a2d}
a.mapbtn.disabled{opacity:.45;pointer-events:none}
</style>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
</head><body><div class="wrap">
<h2>MULTI SENSOR</h2>

<div class="card"><div class="row">
<div>Time: <b id="timeTxt">-</b></div>
<div>STA: <b id="staTxt">-</b></div>
<div>GPS: <b id="gpsTxt">-</b></div>
<div>SAT: <b id="satTxt">0</b></div>
<div>LAT: <b id="latTxt">-</b></div>
<div>LON: <b id="lonTxt">-</b></div>
<div>BAUD: <b id="baudTxt">-</b></div>
<div>RETENTION: <b id="retTxt">7</b> hari</div>
<div>AP IP: <b id="apIpTxt">-</b></div>
<div>API: <b id="apiStat" class="bad">WAIT</b></div>
<div><a id="mapBtn" class="mapbtn disabled" href="#" target="_blank" rel="noopener">Open Google Maps</a></div>
</div></div>

<div id="richUI">
  <div class="grid g4">
    <div class="card"><div class="muted">Temperature (°C)</div><canvas id="gTemp" width="240" height="160"></canvas><div class="kpi" id="tempVal">-</div></div>
    <div class="card"><div class="muted">Humidity (%)</div><canvas id="gHum" width="240" height="160"></canvas><div class="kpi" id="humVal">-</div></div>
    <div class="card"><div class="muted">Pressure (hPa)</div><canvas id="gPres" width="240" height="160"></canvas><div class="kpi" id="presVal">-</div></div>
    <div class="card"><div class="muted">ALT REL (m)</div><canvas id="gAlt" width="240" height="160"></canvas><div class="kpi" id="altVal">-</div></div>
  </div>

  <div class="grid g2">
    <div class="card"><div class="muted">Live Temp / Hum</div><canvas id="chartTH" height="120"></canvas></div>
    <div class="card"><div class="muted">Live Pressure / ALT REL</div><canvas id="chartPA" height="120"></canvas></div>
  </div>
</div>

<div id="fallbackUI" class="fallback">
  <div class="card"><b>Chart CDN gagal dimuat.</b> Dashboard fallback aktif.</div>
  <div class="grid g4">
    <div class="card"><div class="muted">Temp</div><div class="kpi" id="fbTemp">-</div></div>
    <div class="card"><div class="muted">Hum</div><div class="kpi" id="fbHum">-</div></div>
    <div class="card"><div class="muted">Pressure</div><div class="kpi" id="fbPres">-</div></div>
    <div class="card"><div class="muted">ALT REL</div><div class="kpi" id="fbAlt">-</div></div>
  </div>
</div>

<div class="card">
  <h3>Daily Log</h3>
  <form method="GET" action="/download"><select name="f" id="file1"></select> <button type="submit">Download</button></form><br>
  <form method="GET" action="/viewlog"><select name="f" id="file2"></select> <button type="submit">View Last 30</button></form><br>
  <form method="POST" action="/clearlogtoday"><button type="submit">Clear Today Log</button></form><br>

  <form method="POST" action="/setretention">
    <label>Log retention (hari, 1..60):</label><br>
    <input type="number" name="days" min="1" max="60" value="7" required>
    <button type="submit">Save Retention</button>
  </form>
</div>

<div class="card">
  <h3>WiFi Manager</h3>
  <form method="POST" action="/savewifi">SSID<br><input name="ssid" required><br><br>Password<br><input name="pass" type="password"><br><br><button type="submit">Save & Connect</button></form><br>
  <form method="POST" action="/clearwifi"><button type="submit">Clear Saved WiFi</button></form><br>
  <form method="POST" action="/recalibrate"><button type="submit">Recalibrate ALT Baseline (15s)</button></form>
</div>
</div>

<script>
const hasChart = typeof Chart !== 'undefined';
const apiStat = document.getElementById('apiStat');
const mapBtn = document.getElementById('mapBtn');

function drawGauge(id,v,min,max,color){
  const c=document.getElementById(id); if(!c) return;
  const x=c.getContext('2d'),w=c.width,h=c.height;
  x.clearRect(0,0,w,h);
  const cx=w/2,cy=h-10,r=Math.min(w*0.38,h*0.75),s=Math.PI;
  x.lineWidth=16;x.strokeStyle='#2a3858';x.beginPath();x.arc(cx,cy,r,Math.PI,2*Math.PI);x.stroke();
  let p=(v-min)/(max-min); if(!isFinite(p)) p=0; p=Math.max(0,Math.min(1,p));
  x.strokeStyle=color;x.beginPath();x.arc(cx,cy,r,s,s+p*Math.PI);x.stroke();
  const a=s+p*Math.PI,nx=cx+Math.cos(a)*(r-4),ny=cy+Math.sin(a)*(r-4);
  x.lineWidth=3;x.strokeStyle='#eaf2ff';x.beginPath();x.moveTo(cx,cy);x.lineTo(nx,ny);x.stroke();
  x.fillStyle='#eaf2ff';x.beginPath();x.arc(cx,cy,4,0,Math.PI*2);x.fill();
}

let chartTH=null, chartPA=null, labels=[], maxPoints=40;

if(hasChart){
  chartTH=new Chart(document.getElementById('chartTH'),{
    type:'line',
    data:{labels,datasets:[
      {label:'Temp',data:[],borderColor:'#ff8c42',tension:.25},
      {label:'Hum',data:[],borderColor:'#4cd964',tension:.25}
    ]},
    options:{animation:false,plugins:{legend:{labels:{color:'#eaf2ff'}}},scales:{x:{ticks:{color:'#9fb3d1'}},y:{ticks:{color:'#9fb3d1'}}}}
  });

  chartPA=new Chart(document.getElementById('chartPA'),{
    type:'line',
    data:{labels,datasets:[
      {label:'Pressure',data:[],borderColor:'#4da3ff',tension:.25},
      {label:'ALT REL',data:[],borderColor:'#ffcc00',tension:.25}
    ]},
    options:{animation:false,plugins:{legend:{labels:{color:'#eaf2ff'}}},scales:{x:{ticks:{color:'#9fb3d1'}},y:{ticks:{color:'#9fb3d1'}}}}
  });
}else{
  document.getElementById('richUI').style.display='none';
  document.getElementById('fallbackUI').style.display='block';
}

function push(ch,l,v1,v2){
  if(!ch) return;
  if(ch.data.labels.length>=maxPoints){
    ch.data.labels.shift(); ch.data.datasets[0].data.shift(); ch.data.datasets[1].data.shift();
  }
  ch.data.labels.push(l); ch.data.datasets[0].data.push(v1); ch.data.datasets[1].data.push(v2); ch.update();
}

async function loadLogs(){
  try{
    const r=await fetch('/listlogs', {cache:'no-store'}); const d=await r.json();
    const a=document.getElementById('file1'), b=document.getElementById('file2');
    a.innerHTML=''; b.innerHTML='';
    (d.files||[]).forEach(f=>{
      const o1=document.createElement('option'); o1.value=f; o1.textContent=f; a.appendChild(o1);
      const o2=document.createElement('option'); o2.value=f; o2.textContent=f; b.appendChild(o2);
    });
  }catch(e){}
}

async function tick(){
  try{
    const r=await fetch('/api', {cache:'no-store'});
    const d=await r.json();

    apiStat.textContent='OK'; apiStat.className='ok';
    document.getElementById('timeTxt').textContent=d.time||'-';
    document.getElementById('staTxt').textContent=(d.sta_status||'-')+' / '+(d.sta_ip||'-');
    document.getElementById('gpsTxt').textContent=d.gps_fix?'FIX':'NO FIX';
    document.getElementById('satTxt').textContent=d.gps_sat??0;

    const latValid = !(d.gps_lat === null || d.gps_lat === undefined);
    const lonValid = !(d.gps_lon === null || d.gps_lon === undefined);
    const lat = latValid ? Number(d.gps_lat).toFixed(6) : '-';
    const lon = lonValid ? Number(d.gps_lon).toFixed(6) : '-';
    document.getElementById('latTxt').textContent = lat;
    document.getElementById('lonTxt').textContent = lon;

    if (d.gps_fix && latValid && lonValid) {
      mapBtn.href = `https://maps.google.com/?q=${lat},${lon}`;
      mapBtn.classList.remove('disabled');
    } else {
      mapBtn.href = '#';
      mapBtn.classList.add('disabled');
    }

    document.getElementById('baudTxt').textContent=(d.gps_baud_locked?'LOCK ':'SCAN ') + (d.gps_baud??'-');
    document.getElementById('apIpTxt').textContent=d.ap_ip||'-';

    const retDays = Number(d.log_retention_days);
    if(isFinite(retDays)){
      document.getElementById('retTxt').textContent = retDays;
      const retInp = document.querySelector('input[name="days"]');
      if(retInp) retInp.value = retDays;
    }

    const t=Number(d.temperature_c), h=Number(d.humidity_pct), p=Number(d.pressure_hpa), a=Number(d.alt_rel_m);
    const altText = d.baseline_ready ? (isFinite(a)?a.toFixed(1)+' m':'OFF') : 'CAL...';

    if(hasChart){
      document.getElementById('tempVal').textContent=isFinite(t)?t.toFixed(1)+' °C':'OFF';
      document.getElementById('humVal').textContent=isFinite(h)?h.toFixed(0)+' %':'OFF';
      document.getElementById('presVal').textContent=isFinite(p)?p.toFixed(1)+' hPa':'OFF';
      document.getElementById('altVal').textContent=altText;

      drawGauge('gTemp',t,0,60,'#ff8c42');
      drawGauge('gHum',h,0,100,'#4cd964');
      drawGauge('gPres',p,900,1100,'#4da3ff');
      drawGauge('gAlt',a,-100,30000,'#ffcc00');

      const lbl=(d.time||'--').slice(11,19);
      push(chartTH,lbl,t,h);
      push(chartPA,lbl,p,a);
    }else{
      document.getElementById('fbTemp').textContent=isFinite(t)?t.toFixed(1)+' °C':'OFF';
      document.getElementById('fbHum').textContent=isFinite(h)?h.toFixed(0)+' %':'OFF';
      document.getElementById('fbPres').textContent=isFinite(p)?p.toFixed(1)+' hPa':'OFF';
      document.getElementById('fbAlt').textContent=altText;
    }
  }catch(e){
    apiStat.textContent='FAIL'; apiStat.className='bad';
  }
}

loadLogs(); tick();
setInterval(tick, 3000);
setInterval(loadLogs, 20000);
</script>
</body></html>
)HTML";

  sendNoCacheHeaders();
  server.send(200, "text/html; charset=utf-8", page);
}

void handleListLogs(){
  String j="{\"files\":[";
  bool first=true;
  File root=SPIFFS.open("/");
  File file=root.openNextFile();
  while(file){
    String n=file.name();
    if(n.startsWith("/log_") && n.endsWith(".csv")){
      if(!first) j+=",";
      j += "\"" + n + "\"";
      first=false;
    }
    file=root.openNextFile();
  }
  j += "]}";
  sendNoCacheHeaders();
  server.send(200,"application/json; charset=utf-8",j);
}

void handleSaveWiFi(){
  String ssid=server.arg("ssid");
  String pass=server.arg("pass");
  if(ssid.isEmpty()){ sendNoCacheHeaders(); server.send(400,"text/plain","SSID kosong"); return; }

  saveWiFiCreds(ssid,pass);
  WiFi.disconnect(true,true); delay(300);
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID,AP_PASS);
  connectSTA(); initNTP();

  sendNoCacheHeaders();
  server.sendHeader("Location","/");
  server.send(303);
}
void handleClearWiFi(){
  clearWiFiCreds();
  WiFi.disconnect(true,true); delay(300);
  WiFi.mode(WIFI_AP_STA); WiFi.softAP(AP_SSID,AP_PASS);
  sendNoCacheHeaders();
  server.sendHeader("Location","/");
  server.send(303);
}
void handleRecalibrate(){
  startBaseline();
  sendNoCacheHeaders();
  server.sendHeader("Location","/");
  server.send(303);
}
void handleDownload(){
  String f=server.arg("f");
  if(f.isEmpty()) f=todayFileName();
  if(!SPIFFS.exists(f)){ sendNoCacheHeaders(); server.send(404,"text/plain","file not found"); return; }
  File file=SPIFFS.open(f,FILE_READ);
  sendNoCacheHeaders();
  server.streamFile(file,"text/csv");
  file.close();
}
void handleViewLog(){
  String f=server.arg("f");
  if(f.isEmpty()) f=todayFileName();
  if(!SPIFFS.exists(f)){ sendNoCacheHeaders(); server.send(404,"text/plain","file not found"); return; }

  File file=SPIFFS.open(f,FILE_READ);
  if(!file){ sendNoCacheHeaders(); server.send(500,"text/plain","open failed"); return; }
  String content=file.readString(); file.close();

  int lines=0;
  for(int i=content.length()-1;i>=0;--i){
    if(content[i]=='\n') lines++;
    if(lines>=30){ content=content.substring(i+1); break; }
  }
  sendNoCacheHeaders();
  server.send(200,"text/html; charset=utf-8","<pre>"+content+"</pre><br><a href='/'>Back</a>");
}
void handleClearTodayLog(){
  String f=todayFileName();
  if(SPIFFS.exists(f)) SPIFFS.remove(f);
  ensureHeader(f);
  sendNoCacheHeaders();
  server.sendHeader("Location","/");
  server.send(303);
}

// =========================
// Setup / Loop
// =========================
void setup(){
  Serial.begin(115200);
  delay(400);

  // ECO ringan agar lebih adem
  setCpuFrequencyMhz(80);
  WiFi.setSleep(true);
  esp_wifi_set_max_tx_power(34);

  Wire.begin(21,22);

  lcd20.init(); lcd20.backlight();
  lcd16.init(); lcd16.backlight();

  dht.begin();
  bmeOK = bme.begin(0x76) || bme.begin(0x77);

  // GPS auto baud
  autoDetectGPSBaud();

  SPIFFS.begin(true);
  loadRetentionSetting();
  ensureHeader(todayFileName());
  cleanupOldLogs();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  loadWiFiCreds();
  if(!staSSID.isEmpty()) connectSTA();

  initNTP();
  startBaseline();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleApi);
  server.on("/listlogs", HTTP_GET, handleListLogs);
  server.on("/savewifi", HTTP_POST, handleSaveWiFi);
  server.on("/clearwifi", HTTP_POST, handleClearWiFi);
  server.on("/recalibrate", HTTP_POST, handleRecalibrate);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/viewlog", HTTP_GET, handleViewLog);
  server.on("/clearlogtoday", HTTP_POST, handleClearTodayLog);
  server.on("/setretention", HTTP_POST, handleSetRetention);

  Serial.print("[HEAP] Free before server.begin: ");
  Serial.println(ESP.getFreeHeap());

  server.begin();

  readSensors();
  initLcdSmoothBoot();
}

void loop(){
  server.handleClient();
  updateGPS();

  unsigned long now=millis();

  if(now-lastSensor>=SENSOR_INTERVAL_MS){
    lastSensor=now;
    readSensors();
  }

  updateLcdSmoothScheduler();

  if(now-lastLog>=LOG_INTERVAL_MS){
    lastLog=now;
    appendLog();
  }

  static unsigned long lastRetry=0;
  if(WiFi.status()!=WL_CONNECTED && !staSSID.isEmpty()){
    if(now-lastRetry>10000){
      lastRetry=now;
      WiFi.disconnect();
      WiFi.begin(staSSID.c_str(), staPASS.c_str());
    }
  }

  static unsigned long lastNtpCheck=0;
  if(!ntpSynced && now-lastNtpCheck>15000){
    lastNtpCheck=now;
    struct tm t; ntpSynced=getLocalTime(&t,100);
  }

  // Auto reboot scheduler (03:00 setiap hari)
  handleAutoRebootSchedule();

  // cleanup log lama tiap 24 jam
  static unsigned long lastCleanupMs=0;
  if(now-lastCleanupMs>86400000UL){
    lastCleanupMs=now;
    cleanupOldLogs();
  }

  static unsigned long lastDiag=0;
  if(now-lastDiag>5000){
    lastDiag=now;
    Serial.printf("[GPS] baud=%lu lock=%s chars=%lu fix=%s sat=%lu\n",
                  (unsigned long)gpsBaudActive,
                  gpsBaudLocked?"YES":"NO",
                  (unsigned long)gpsChars,
                  gpsFix?"YES":"NO",
                  (unsigned long)gpsSat);
    Serial.printf("[LOG] retention=%d days\n", logRetentionDays);
    Serial.printf("[HEAP] free=%u\n", ESP.getFreeHeap());
  }
}
