#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>

#include "config.h"

// ---------------------------------------------------------------------------
// Addresses and credentials live in config.h, which is not committed.
// Copy src/config.example.h to src/config.h and fill it in.
// ---------------------------------------------------------------------------

const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASS = WIFI_PASS_VALUE;

const char* SNMP_COMMUNITY = SNMP_COMMUNITY_VALUE;

enum UnitType { UNIT_XML, UNIT_CSV, UNIT_UPS };

struct Unit {
  const char* name;
  UnitType    type;
  const char* host;
  const char* alarmPath;  // AC only, polled every round
  const char* tempPath;   // AC only, polled on the slower interval
};

#define CSV_ALARM_PATH "/anonymous/getvar.csv?name=Al_AlarmEvt.Active&name=Al_AlarmOff.Active"
#define CSV_TEMP_PATH  "/anonymous/getvar.csv?name=SupPrb.Temp.Val&name=SupSetP_Act"
#define XML_ALARM_PATH "/tmp/statusreport.xml"
#define XML_TEMP_PATH  "/index.html"

Unit units[] = {
  { "AC-2",  UNIT_XML, AC2_HOST,  XML_ALARM_PATH, XML_TEMP_PATH },
  { "AC-3",  UNIT_XML, AC3_HOST,  XML_ALARM_PATH, XML_TEMP_PATH },
  { "AC-4",  UNIT_XML, AC4_HOST,  XML_ALARM_PATH, XML_TEMP_PATH },
  { "AC-5",  UNIT_XML, AC5_HOST,  XML_ALARM_PATH, XML_TEMP_PATH },
  { "AC-6",  UNIT_XML, AC6_HOST,  XML_ALARM_PATH, XML_TEMP_PATH },
  { "AC-1",  UNIT_CSV, AC1_HOST,  CSV_ALARM_PATH, CSV_TEMP_PATH },
  { "AC-7",  UNIT_CSV, AC7_HOST,  CSV_ALARM_PATH, CSV_TEMP_PATH },
  { "AC-8",  UNIT_CSV, AC8_HOST,  CSV_ALARM_PATH, CSV_TEMP_PATH },
  { "AC-9",  UNIT_CSV, AC9_HOST,  CSV_ALARM_PATH, CSV_TEMP_PATH },
  { "AC-10", UNIT_CSV, AC10_HOST, CSV_ALARM_PATH, CSV_TEMP_PATH },
  { "UPS-1", UNIT_UPS, UPS1_HOST, "", "" },
  { "UPS-2", UNIT_UPS, UPS2_HOST, "", "" },
  { "UPS-3", UNIT_UPS, UPS3_HOST, "", "" },
  { "UPS-4", UNIT_UPS, UPS4_HOST, "", "" },
  { "UPS-5", UNIT_UPS, UPS5_HOST, "", "" },
};

const int UNIT_COUNT = sizeof(units) / sizeof(units[0]);

// Old AC units: XML Item ids in /index.html
const char* XML_ID_TEMP     = "361";   // Return Air Temperature
const char* XML_ID_SETPOINT = "356";   // Return Air Temperature Setpoint

// New AC units: variable names in getvar.csv
const char* CSV_VAR_TEMP     = "SupPrb.Temp.Val";
const char* CSV_VAR_SETPOINT = "SupSetP_Act";

// Standard UPS MIB (RFC 1628)
const char* OID_BATT_CHARGE = "1.3.6.1.2.1.33.1.2.4.0";   // percent
const char* OID_BATT_TEMP   = "1.3.6.1.2.1.33.1.2.7.0";   // degrees C

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

const int  BUZZER_PIN      = 23;
const int  SDA_PIN         = 21;
const int  SCL_PIN         = 22;

const int  HTTP_PORT       = 80;
const int  CONNECT_TIMEOUT = 35000;   // old AC units stall for up to ~30s
const int  READ_TIMEOUT    = 35000;
const int  MAX_BYTES       = 4096;
const int  CSV_MAX_BYTES   = 1024;
const int  TEMP_MAX_BYTES  = 8192;

const int  SNMP_PORT       = 161;
const int  SNMP_TIMEOUT    = 3000;
const int  SNMP_RETRIES    = 2;

// AC rule: alarm at setpoint + 3, clear again below setpoint + 2.
// The gap stops a reading sitting on the line from beeping every round.
const float TEMP_ALARM_OFFSET = 3.0;
const float TEMP_CLEAR_OFFSET = 2.0;

// UPS rules. No hysteresis: the values are whole numbers already, so a
// one-degree gap would just delay the all-clear for no reason.
const int  UPS_TEMP_ALARM   = 30;   // alarm at this and above
const int  UPS_CHARGE_ALARM = 95;   // alarm below this

const unsigned long ROUND_INTERVAL_MS = 90000;
const unsigned long TEMP_INTERVAL_MS  = 180000;
const unsigned long UNIT_GAP_MS       = 1000;
const unsigned long ALARM_HOLD_MS     = 600000;
const int  FAIL_THRESHOLD  = 5;

const bool BEEP_ON_UNREACHABLE = true;

// ---------------------------------------------------------------------------

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Preferences prefs;
WiFiUDP     udp;

struct UnitState {
  String  lastModified;
  String  knownStamp;
  bool    alarmFlag;
  bool    tempFlag;        // AC threshold exceeded
  bool    upsTempFlag;
  bool    upsChargeFlag;
  float   lastTemp;
  float   lastSetpoint;
  int     upsTemp;
  int     upsCharge;
  bool    valuesValid;
  bool    firstRun;
  int     consecutiveFails;
  bool    warned;
};

UnitState state[UNIT_COUNT];

bool          alarmActive     = false;
int           alarmUnitIndex  = -1;
String        alarmEventName  = "";
String        alarmEventTime  = "";
unsigned long alarmShownAt    = 0;

unsigned long lastRoundEnd = 0;
unsigned long lastTempRun  = 0;
bool          tempThisRound = false;
String        statusLine   = "starting up";

// ---------------------------------------------------------------------------
// Buzzer
// ---------------------------------------------------------------------------

void buzzerSilent() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepOnce() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(700);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepWarning() {
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(120);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void wrapText(const String& text, int width, String& line1, String& line2) {
  line1 = "";
  line2 = "";

  if ((int)text.length() <= width) {
    line1 = text;
    return;
  }

  int cut = -1;
  for (int i = width; i > 0; i--) {
    if (text.charAt(i) == ' ') { cut = i; break; }
  }
  if (cut == -1) cut = width;

  line1 = text.substring(0, cut);
  line2 = text.substring(cut + 1);
  if ((int)line2.length() > width) line2 = line2.substring(0, width);
}

void drawAlarmScreen() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(0, 14, units[alarmUnitIndex].name);

  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 28, "ALARM");

  String name = alarmEventName;
  if (name.startsWith("Event: ")) name = name.substring(7);

  String l1, l2;
  wrapText(name, 21, l1, l2);
  u8g2.drawStr(0, 42, l1.c_str());
  if (l2.length() > 0) u8g2.drawStr(0, 52, l2.c_str());

  if (alarmEventTime.length() > 0) {
    String t = alarmEventTime;
    if (t.length() >= 16) t = t.substring(11, 16);
    u8g2.drawStr(0, 63, t.c_str());
  }

  u8g2.sendBuffer();
}

void drawIdleScreen() {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(0, 14, "AC MONITOR");

  u8g2.setFont(u8g2_font_6x12_tr);

  String down = "";
  for (int i = 0; i < UNIT_COUNT; i++) {
    if (state[i].consecutiveFails >= FAIL_THRESHOLD) {
      if (down.length() > 0) down += " ";
      down += units[i].name;
    }
  }

  char line[26];

  if (down.length() > 0) {
    u8g2.drawStr(0, 27, "UNREACHABLE:");
    if ((int)down.length() > 21) down = down.substring(0, 21);
    u8g2.drawStr(0, 39, down.c_str());
  } else {
    // Hottest AC unit, so one number covers all ten.
    float hottest = -1000;
    int   hottestIdx = -1;
    for (int i = 0; i < UNIT_COUNT; i++) {
      if (units[i].type == UNIT_UPS) continue;
      if (state[i].valuesValid && state[i].lastTemp > hottest) {
        hottest = state[i].lastTemp;
        hottestIdx = i;
      }
    }

    if (hottestIdx >= 0) {
      snprintf(line, sizeof(line), "max %s %.1fC", units[hottestIdx].name, hottest);
    } else {
      snprintf(line, sizeof(line), "max AC --");
    }
    u8g2.drawStr(0, 27, line);

    // All five UPS battery temperatures on one row.
    String ups = "UPS";
    for (int i = 0; i < UNIT_COUNT; i++) {
      if (units[i].type != UNIT_UPS) continue;
      ups += " ";
      ups += state[i].valuesValid ? String(state[i].upsTemp) : String("--");
    }
    if ((int)ups.length() > 21) ups = ups.substring(0, 21);
    u8g2.drawStr(0, 39, ups.c_str());
  }

  if (lastRoundEnd == 0) {
    u8g2.drawStr(0, 51, "first round running");
  } else {
    unsigned long ago = (millis() - lastRoundEnd) / 1000;
    snprintf(line, sizeof(line), "last check %lus ago", ago);
    u8g2.drawStr(0, 51, line);
  }

  u8g2.drawStr(0, 63, statusLine.c_str());
  u8g2.sendBuffer();
}

void refreshScreen() {
  if (alarmActive) drawAlarmScreen();
  else drawIdleScreen();
}

void setStatus(const String& s) {
  statusLine = s;
  if (!alarmActive) drawIdleScreen();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void loadState() {
  prefs.begin("acmon", false);

  for (int i = 0; i < UNIT_COUNT; i++) {
    state[i].lastModified     = "";
    state[i].consecutiveFails = 0;
    state[i].warned           = false;
    state[i].alarmFlag        = false;
    state[i].tempFlag         = false;
    state[i].upsTempFlag      = false;
    state[i].upsChargeFlag    = false;
    state[i].lastTemp         = 0;
    state[i].lastSetpoint     = 0;
    state[i].upsTemp          = 0;
    state[i].upsCharge        = 0;
    state[i].valuesValid      = false;
    state[i].firstRun         = false;

    if (units[i].type == UNIT_XML) {
      String key = String("s_") + units[i].name;
      state[i].knownStamp = prefs.getString(key.c_str(), "");
      state[i].firstRun   = (state[i].knownStamp.length() == 0);

      Serial.print(units[i].name);
      Serial.print(" [xml] stored stamp: ");
      Serial.println(state[i].knownStamp.length() ? state[i].knownStamp : "(none)");
    } else if (units[i].type == UNIT_CSV) {
      String key = String("f_") + units[i].name;
      state[i].alarmFlag = prefs.getBool(key.c_str(), false);

      Serial.print(units[i].name);
      Serial.print(" [csv] stored flag: ");
      Serial.println(state[i].alarmFlag ? "ALARM" : "clear");
    } else {
      Serial.print(units[i].name);
      Serial.println(" [ups] snmp v1");
    }
  }
}

void saveStamp(int i) {
  String key = String("s_") + units[i].name;
  prefs.putString(key.c_str(), state[i].knownStamp);
}

void saveFlag(int i) {
  String key = String("f_") + units[i].name;
  prefs.putBool(key.c_str(), state[i].alarmFlag);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    tries++;
  }

  Serial.println(WiFi.status() == WL_CONNECTED ? "wifi connected" : "wifi failed");
}

// ---------------------------------------------------------------------------
// SNMP v1 client. Two GETs per UPS per round, so the packet flood that
// killed the earlier SNMP attempt cannot happen here.
// ---------------------------------------------------------------------------

int berLength(uint8_t* out, int len) {
  if (len < 128) { out[0] = (uint8_t)len; return 1; }
  out[0] = 0x82;
  out[1] = (uint8_t)((len >> 8) & 0xFF);
  out[2] = (uint8_t)(len & 0xFF);
  return 3;
}

int berSubId(uint8_t* out, uint32_t v) {
  if (v < 128) { out[0] = (uint8_t)v; return 1; }
  uint8_t tmp[5];
  int n = 0;
  while (v > 0) { tmp[n++] = (uint8_t)(v & 0x7F); v >>= 7; }
  for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i] | (i < n - 1 ? 0x80 : 0x00);
  return n;
}

// Encodes a dotted OID string. The first two sub-ids collapse into one byte.
int encodeOid(const char* oid, uint8_t* out, int outLen) {
  uint32_t parts[32];
  int count = 0;
  uint32_t cur = 0;
  bool haveDigit = false;

  for (const char* p = oid; ; p++) {
    if (*p >= '0' && *p <= '9') {
      cur = cur * 10 + (*p - '0');
      haveDigit = true;
    } else {
      if (haveDigit && count < 32) parts[count++] = cur;
      cur = 0;
      haveDigit = false;
      if (*p == '\0') break;
    }
  }

  if (count < 2) return -1;

  int o = 0;
  out[o++] = (uint8_t)(parts[0] * 40 + parts[1]);
  for (int i = 2; i < count; i++) {
    if (o + 5 > outLen) return -1;
    o += berSubId(out + o, parts[i]);
  }
  return o;
}

int buildSnmpGet(uint8_t* buf, int bufLen, uint32_t requestId, const char* oidStr) {
  uint8_t oid[64];
  int o = encodeOid(oidStr, oid, sizeof(oid));
  if (o < 0) return -1;

  uint8_t vb[96];
  int v = 0;
  vb[v++] = 0x06;
  v += berLength(vb + v, o);
  memcpy(vb + v, oid, o);
  v += o;
  vb[v++] = 0x05;
  vb[v++] = 0x00;

  uint8_t vbSeq[112];
  int vs = 0;
  vbSeq[vs++] = 0x30;
  vs += berLength(vbSeq + vs, v);
  memcpy(vbSeq + vs, vb, v);
  vs += v;

  uint8_t vbList[128];
  int vl = 0;
  vbList[vl++] = 0x30;
  vl += berLength(vbList + vl, vs);
  memcpy(vbList + vl, vbSeq, vs);
  vl += vs;

  uint8_t pdu[160];
  int p = 0;
  pdu[p++] = 0x02; pdu[p++] = 0x04;
  pdu[p++] = (uint8_t)((requestId >> 24) & 0xFF);
  pdu[p++] = (uint8_t)((requestId >> 16) & 0xFF);
  pdu[p++] = (uint8_t)((requestId >> 8) & 0xFF);
  pdu[p++] = (uint8_t)(requestId & 0xFF);
  pdu[p++] = 0x02; pdu[p++] = 0x01; pdu[p++] = 0x00;
  pdu[p++] = 0x02; pdu[p++] = 0x01; pdu[p++] = 0x00;
  memcpy(pdu + p, vbList, vl);
  p += vl;

  uint8_t pduWrap[192];
  int pw = 0;
  pduWrap[pw++] = 0xA0;
  pw += berLength(pduWrap + pw, p);
  memcpy(pduWrap + pw, pdu, p);
  pw += p;

  int commLen = strlen(SNMP_COMMUNITY);

  uint8_t body[256];
  int b = 0;
  body[b++] = 0x02; body[b++] = 0x01; body[b++] = 0x00;   // version 0 = v1
  body[b++] = 0x04;
  b += berLength(body + b, commLen);
  memcpy(body + b, SNMP_COMMUNITY, commLen);
  b += commLen;
  memcpy(body + b, pduWrap, pw);
  b += pw;

  int total = 0;
  buf[total++] = 0x30;
  total += berLength(buf + total, b);
  if (total + b > bufLen) return -1;
  memcpy(buf + total, body, b);
  total += b;

  return total;
}

int berReadLength(const uint8_t* buf, int len, int* pos) {
  if (*pos >= len) return -1;
  uint8_t b = buf[(*pos)++];
  if (!(b & 0x80)) return b;

  int count = b & 0x7F;
  if (count == 0 || count > 3) return -1;
  if (*pos + count > len) return -1;

  int result = 0;
  for (int i = 0; i < count; i++) result = (result << 8) | buf[(*pos)++];
  return result;
}

// Walks the reply structure rather than scanning for bytes, because 0x06
// can appear inside the encoded OID itself.
bool parseSnmpInteger(const uint8_t* buf, int len, long* out) {
  int p = 0;

  if (p >= len || buf[p++] != 0x30) return false;
  if (berReadLength(buf, len, &p) < 0) return false;

  if (p >= len || buf[p++] != 0x02) return false;
  int vlen = berReadLength(buf, len, &p);
  if (vlen < 0) return false;
  p += vlen;

  if (p >= len || buf[p++] != 0x04) return false;
  int clen = berReadLength(buf, len, &p);
  if (clen < 0) return false;
  p += clen;

  if (p >= len) return false;
  uint8_t pduTag = buf[p++];
  if ((pduTag & 0xE0) != 0xA0) return false;
  if (berReadLength(buf, len, &p) < 0) return false;

  for (int i = 0; i < 3; i++) {
    if (p >= len || buf[p++] != 0x02) return false;
    int n = berReadLength(buf, len, &p);
    if (n < 0) return false;
    if (i == 1) {
      long err = 0;
      for (int k = 0; k < n; k++) err = (err << 8) | buf[p + k];
      if (err != 0) return false;
    }
    p += n;
  }

  if (p >= len || buf[p++] != 0x30) return false;
  if (berReadLength(buf, len, &p) < 0) return false;
  if (p >= len || buf[p++] != 0x30) return false;
  if (berReadLength(buf, len, &p) < 0) return false;

  if (p >= len || buf[p++] != 0x06) return false;
  int oidLen = berReadLength(buf, len, &p);
  if (oidLen < 0) return false;
  p += oidLen;

  if (p >= len) return false;
  uint8_t tag = buf[p++];
  int n = berReadLength(buf, len, &p);
  if (n < 0 || n > 4 || p + n > len) return false;

  // INTEGER, Gauge32 and TimeTicks all decode the same way here.
  if (tag != 0x02 && tag != 0x41 && tag != 0x42 && tag != 0x43) return false;

  long v = 0;
  bool neg = (tag == 0x02) && (buf[p] & 0x80);
  for (int k = 0; k < n; k++) v = (v << 8) | buf[p + k];
  if (neg && n < 4) v -= ((long)1 << (8 * n));

  *out = v;
  return true;
}

bool snmpGet(const char* host, const char* oid, long* value) {
  static uint32_t requestId = 2000;
  uint8_t packet[320];
  uint8_t reply[512];

  for (int attempt = 0; attempt < SNMP_RETRIES; attempt++) {
    requestId++;
    int len = buildSnmpGet(packet, sizeof(packet), requestId, oid);
    if (len < 0) return false;

    if (!udp.beginPacket(host, SNMP_PORT)) return false;
    udp.write(packet, len);
    udp.endPacket();

    unsigned long deadline = millis() + SNMP_TIMEOUT;
    while (millis() < deadline) {
      int sz = udp.parsePacket();
      if (sz <= 0) { delay(5); continue; }

      int n = udp.read(reply, sizeof(reply));
      if (n <= 0) break;

      if (parseSnmpInteger(reply, n, value)) return true;
      break;
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

bool openAndSkipHeaders(WiFiClient& client, int i, const char* method,
                        const char* path, unsigned long* deadlineOut) {
  unsigned long connStart = millis();

  if (!client.connect(units[i].host, HTTP_PORT, CONNECT_TIMEOUT)) {
    Serial.print("    connect failed after ");
    Serial.print(millis() - connStart);
    Serial.println(" ms");
    return false;
  }

  client.print(String(method) + " " + path + " HTTP/1.0\r\n" +
               "Host: " + units[i].host + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long deadline = millis() + READ_TIMEOUT;
  bool headersDone = false;

  while (client.connected() && millis() < deadline) {
    if (!client.available()) { delay(10); continue; }
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) { headersDone = true; break; }
  }

  if (!headersDone) {
    Serial.print("    header read cut short after ");
    Serial.print(millis() - connStart);
    Serial.println(" ms");
    return false;
  }

  *deadlineOut = deadline;
  return true;
}

// ---------------------------------------------------------------------------
// AC temperature reading
// ---------------------------------------------------------------------------

struct TempResult {
  bool  ok;
  float temp;
  float setpoint;
};

TempResult fetchCsvTemp(int i) {
  TempResult r = { false, 0, 0 };

  WiFiClient client;
  client.setTimeout(READ_TIMEOUT / 1000);

  unsigned long deadline = 0;
  if (!openAndSkipHeaders(client, i, "GET", units[i].tempPath, &deadline)) {
    client.stop();
    return r;
  }

  bool gotTemp = false, gotSp = false;
  int  bytesRead = 0;

  while (client.connected() && bytesRead < CSV_MAX_BYTES && millis() < deadline) {
    if (!client.available()) { delay(5); continue; }

    String line = client.readStringUntil('\n');
    bytesRead += line.length() + 1;
    line.trim();
    if (line.length() == 0 || line.startsWith("name,id")) continue;

    int nameStart = line.indexOf('"');
    int nameEnd   = line.indexOf('"', nameStart + 1);
    int valEnd    = line.lastIndexOf('"');
    int valStart  = line.lastIndexOf('"', valEnd - 1);
    if (nameStart < 0 || nameEnd <= nameStart || valStart <= nameEnd) continue;

    String vname = line.substring(nameStart + 1, nameEnd);
    String value = line.substring(valStart + 1, valEnd);

    if (vname == CSV_VAR_TEMP)     { r.temp = value.toFloat();     gotTemp = true; }
    if (vname == CSV_VAR_SETPOINT) { r.setpoint = value.toFloat(); gotSp = true; }
  }

  client.stop();
  r.ok = gotTemp && gotSp;
  return r;
}

TempResult fetchXmlTemp(int i) {
  TempResult r = { false, 0, 0 };

  WiFiClient client;
  client.setTimeout(READ_TIMEOUT / 1000);

  unsigned long deadline = 0;
  if (!openAndSkipHeaders(client, i, "GET", units[i].tempPath, &deadline)) {
    client.stop();
    return r;
  }

  String buffer = "";
  int  bytesRead = 0;
  bool gotTemp = false, gotSp = false;

  while (client.connected() && bytesRead < TEMP_MAX_BYTES && millis() < deadline
         && !(gotTemp && gotSp)) {
    if (!client.available()) { delay(5); continue; }

    char chunk[257];
    int  n = client.readBytes(chunk, 256);
    if (n <= 0) continue;
    chunk[n] = '\0';
    bytesRead += n;
    buffer += chunk;

    while (true) {
      int end = buffer.indexOf("</Item>");
      if (end == -1) break;

      int start = buffer.indexOf("<Item ");
      String block = (start != -1 && start < end) ? buffer.substring(start, end) : "";
      buffer = buffer.substring(end + 7);
      if (block.length() == 0) continue;

      int idPos = block.indexOf("id=\"");
      if (idPos < 0) continue;
      int idEnd = block.indexOf('"', idPos + 4);
      if (idEnd < 0) continue;
      String id = block.substring(idPos + 4, idEnd);

      int vs = block.indexOf("<Value");
      if (vs < 0) continue;
      int vOpen = block.indexOf('>', vs);
      int vEnd  = block.indexOf("</Value>", vOpen);
      if (vOpen < 0 || vEnd < 0) continue;
      String value = block.substring(vOpen + 1, vEnd);
      value.trim();

      if (value == "---") continue;   // probe not fitted

      if (id == XML_ID_TEMP)     { r.temp = value.toFloat();     gotTemp = true; }
      if (id == XML_ID_SETPOINT) { r.setpoint = value.toFloat(); gotSp = true; }
    }
  }

  client.stop();
  r.ok = gotTemp && gotSp;
  return r;
}

// ---------------------------------------------------------------------------
// Alarm-flag reading, newer AC units
// ---------------------------------------------------------------------------

struct CsvResult {
  bool   ok;
  bool   anyAlarm;
  String firstAlarmName;
  int    bytesRead;
};

CsvResult fetchCsvFlags(int i) {
  CsvResult r = { false, false, "", 0 };

  WiFiClient client;
  client.setTimeout(READ_TIMEOUT / 1000);

  unsigned long deadline = 0;
  if (!openAndSkipHeaders(client, i, "GET", units[i].alarmPath, &deadline)) {
    client.stop();
    return r;
  }

  bool sawHeader = false;

  while (client.connected() && r.bytesRead < CSV_MAX_BYTES && millis() < deadline) {
    if (!client.available()) { delay(5); continue; }

    String line = client.readStringUntil('\n');
    r.bytesRead += line.length() + 1;
    line.trim();
    if (line.length() == 0) continue;

    if (line.startsWith("name,id")) { sawHeader = true; continue; }

    int nameStart = line.indexOf('"');
    int nameEnd   = line.indexOf('"', nameStart + 1);
    int valEnd    = line.lastIndexOf('"');
    int valStart  = line.lastIndexOf('"', valEnd - 1);
    if (nameStart < 0 || nameEnd <= nameStart || valStart <= nameEnd) continue;

    String vname = line.substring(nameStart + 1, nameEnd);
    String value = line.substring(valStart + 1, valEnd);

    if (value == "1") {
      r.anyAlarm = true;
      if (r.firstAlarmName.length() == 0) {
        String shown = vname;
        if (shown.startsWith("Al_")) shown = shown.substring(3);
        int dot = shown.indexOf('.');
        if (dot > 0) shown = shown.substring(0, dot);
        r.firstAlarmName = shown;
      }
    }
  }

  client.stop();
  r.ok = sawHeader;
  return r;
}

// ---------------------------------------------------------------------------
// Alarm-log reading, older AC units
// ---------------------------------------------------------------------------

String headRequest(int i) {
  WiFiClient client;
  client.setTimeout(READ_TIMEOUT / 1000);

  if (!client.connect(units[i].host, HTTP_PORT, CONNECT_TIMEOUT)) return "";

  client.print(String("HEAD ") + units[i].alarmPath + " HTTP/1.0\r\n" +
               "Host: " + units[i].host + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long deadline = millis() + READ_TIMEOUT;
  String lastMod = "";
  bool   ok      = false;

  while (client.connected() && millis() < deadline) {
    if (!client.available()) { delay(10); continue; }

    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;

    if (line.startsWith("HTTP/") && line.indexOf(" 200") > 0) ok = true;

    if (line.startsWith("Last-Modified:")) {
      lastMod = line.substring(14);
      lastMod.trim();
    }
  }

  client.stop();
  return ok ? lastMod : "";
}

struct ParseResult {
  bool   ok;
  int    alarmCount;
  String firstAlarmName;
  String firstAlarmTime;
  String newestStamp;
  int    newRecords;
  int    bytesRead;
};

ParseResult fetchAndParse(int i) {
  ParseResult r = { false, 0, "", "", "", 0, 0 };

  WiFiClient client;
  client.setTimeout(READ_TIMEOUT / 1000);

  unsigned long deadline = 0;
  if (!openAndSkipHeaders(client, i, "GET", units[i].alarmPath, &deadline)) {
    client.stop();
    return r;
  }

  String buffer  = "";
  bool   stopNow = false;

  while (client.connected() && r.bytesRead < MAX_BYTES && millis() < deadline && !stopNow) {
    if (!client.available()) { delay(5); continue; }

    char chunk[257];
    int  n = client.readBytes(chunk, 256);
    if (n <= 0) continue;
    chunk[n] = '\0';
    r.bytesRead += n;
    buffer += chunk;

    while (true) {
      int endIdx = buffer.indexOf("</sren>");
      if (endIdx == -1) break;

      int startIdx = buffer.indexOf("<sren>");
      String block = (startIdx != -1 && startIdx < endIdx)
                       ? buffer.substring(startIdx, endIdx)
                       : "";
      buffer = buffer.substring(endIdx + 7);
      if (block.length() == 0) continue;

      int ts = block.indexOf("<type>");
      int te = block.indexOf("</type>");
      int ms = block.indexOf("<time>");
      int me = block.indexOf("</time>");
      int ns = block.indexOf("<name>");
      int ne = block.indexOf("</name>");
      if (ts == -1 || te == -1 || ms == -1 || me == -1) continue;

      String type = block.substring(ts + 6, te);
      String time = block.substring(ms + 6, me);
      String name = (ns != -1 && ne != -1) ? block.substring(ns + 6, ne) : "";

      if (r.newestStamp.length() == 0) r.newestStamp = time;

      if (state[i].knownStamp.length() > 0 && time <= state[i].knownStamp) {
        stopNow = true;
        break;
      }

      r.newRecords++;

      if (type == "Alarm") {
        r.alarmCount++;
        if (r.firstAlarmName.length() == 0) {
          r.firstAlarmName = name;
          r.firstAlarmTime = time;
        }
      }
    }
  }

  client.stop();
  r.ok = true;
  return r;
}

// ---------------------------------------------------------------------------
// Shared alarm handling
// ---------------------------------------------------------------------------

void clearAlarm(const char* reason) {
  Serial.print("alarm screen cleared: ");
  Serial.println(reason);
  alarmActive    = false;
  alarmUnitIndex = -1;
  alarmEventName = "";
  alarmEventTime = "";
}

void raiseAlarm(int i, const String& name, const String& when) {
  alarmActive    = true;
  alarmUnitIndex = i;
  alarmEventName = name;
  alarmEventTime = when;
  alarmShownAt   = millis();

  Serial.print("### ALARM  ");
  Serial.print(units[i].name);
  Serial.print("  ");
  Serial.print(when);
  Serial.print("  ");
  Serial.println(name);

  drawAlarmScreen();
  beepOnce();
}

void registerFailure(int i, const char* what, unsigned long ms) {
  state[i].consecutiveFails++;
  Serial.print(what);
  Serial.print(" failed (");
  Serial.print(state[i].consecutiveFails);
  Serial.print(" in a row, ");
  Serial.print(ms);
  Serial.println(" ms)");

  if (state[i].consecutiveFails >= FAIL_THRESHOLD && !state[i].warned) {
    state[i].warned = true;
    Serial.print(units[i].name);
    Serial.println("  !!! unreachable");
    if (BEEP_ON_UNREACHABLE) beepWarning();
  }
}

// ---------------------------------------------------------------------------
// AC temperature check
// ---------------------------------------------------------------------------

void checkTemperature(int i) {
  TempResult t = (units[i].type == UNIT_XML) ? fetchXmlTemp(i) : fetchCsvTemp(i);

  if (!t.ok) {
    Serial.println("    temp read failed");
    return;
  }

  state[i].lastTemp     = t.temp;
  state[i].lastSetpoint = t.setpoint;
  state[i].valuesValid  = true;

  float alarmAt = t.setpoint + TEMP_ALARM_OFFSET;
  float clearAt = t.setpoint + TEMP_CLEAR_OFFSET;

  Serial.print("    temp ");
  Serial.print(t.temp, 1);
  Serial.print(" C, setpoint ");
  Serial.print(t.setpoint, 1);
  Serial.print(" C, alarm at ");
  Serial.print(alarmAt, 1);
  Serial.println(" C");

  // Setpoint zero means this unit does not control on this probe.
  if (t.setpoint <= 0.0) return;

  if (!state[i].tempFlag && t.temp >= alarmAt) {
    state[i].tempFlag = true;
    char label[24];
    snprintf(label, sizeof(label), "TEMP %.1fC", t.temp);
    raiseAlarm(i, String(label), "");
    return;
  }

  if (state[i].tempFlag && t.temp < clearAt) {
    state[i].tempFlag = false;
    Serial.print(units[i].name);
    Serial.println("  temperature back within range");
  }
}

// ---------------------------------------------------------------------------
// Polling
// ---------------------------------------------------------------------------

void pollXmlUnit(int i) {
  unsigned long start  = millis();
  String        lm     = headRequest(i);
  unsigned long headMs = millis() - start;

  if (lm.length() == 0) {
    registerFailure(i, "HEAD", headMs);
    return;
  }

  state[i].consecutiveFails = 0;
  state[i].warned           = false;

  bool changed = (lm != state[i].lastModified);

  Serial.print("HEAD ");
  Serial.print(headMs);
  Serial.print(" ms  ");
  Serial.println(changed ? ">> CHANGED" : "unchanged");

  if (!changed) return;

  state[i].lastModified = lm;

  ParseResult r = fetchAndParse(i);

  if (!r.ok) {
    Serial.print(units[i].name);
    Serial.println("  GET failed, will retry next round");
    state[i].lastModified = "";
    return;
  }

  Serial.print(units[i].name);
  Serial.print("  GET ");
  Serial.print(r.bytesRead);
  Serial.print(" bytes, new records: ");
  Serial.print(r.newRecords);
  Serial.print(", alarms: ");
  Serial.println(r.alarmCount);

  if (r.newestStamp.length() > 0) {
    state[i].knownStamp = r.newestStamp;
    saveStamp(i);
  }

  if (state[i].firstRun) {
    state[i].firstRun = false;
    Serial.print(units[i].name);
    Serial.println("  first run, history stored silently");
    return;
  }

  if (r.alarmCount > 0) {
    raiseAlarm(i, r.firstAlarmName, r.firstAlarmTime);
    return;
  }

  if (alarmActive && alarmUnitIndex == i && r.newRecords > 0) {
    if (millis() - alarmShownAt >= ALARM_HOLD_MS) {
      clearAlarm("newer event from the same unit");
      drawIdleScreen();
    } else {
      unsigned long left = (ALARM_HOLD_MS - (millis() - alarmShownAt)) / 1000;
      Serial.print("alarm screen held, ");
      Serial.print(left);
      Serial.println(" s of minimum hold left");
    }
  }
}

void pollCsvUnit(int i) {
  unsigned long start = millis();
  CsvResult     r     = fetchCsvFlags(i);
  unsigned long ms    = millis() - start;

  if (!r.ok) {
    registerFailure(i, "CSV", ms);
    return;
  }

  state[i].consecutiveFails = 0;
  state[i].warned           = false;

  Serial.print("CSV ");
  Serial.print(ms);
  Serial.print(" ms  ");
  Serial.print(r.bytesRead);
  Serial.print(" bytes, alarm: ");
  Serial.println(r.anyAlarm ? "YES" : "no");

  bool wasSet = state[i].alarmFlag;

  if (r.anyAlarm != wasSet) {
    state[i].alarmFlag = r.anyAlarm;
    saveFlag(i);
  }

  if (r.anyAlarm && !wasSet) {
    raiseAlarm(i, r.firstAlarmName.length() ? r.firstAlarmName : String("active"), "");
    return;
  }

  if (!r.anyAlarm && wasSet && alarmActive && alarmUnitIndex == i) {
    if (millis() - alarmShownAt >= ALARM_HOLD_MS) {
      clearAlarm("unit reports alarm cleared");
      drawIdleScreen();
    } else {
      unsigned long left = (ALARM_HOLD_MS - (millis() - alarmShownAt)) / 1000;
      Serial.print("alarm screen held, ");
      Serial.print(left);
      Serial.println(" s of minimum hold left");
    }
  }
}

void pollUpsUnit(int i) {
  unsigned long start = millis();

  long charge = 0, temp = 0;
  bool gotCharge = snmpGet(units[i].host, OID_BATT_CHARGE, &charge);
  bool gotTemp   = snmpGet(units[i].host, OID_BATT_TEMP, &temp);

  unsigned long ms = millis() - start;

  if (!gotCharge || !gotTemp) {
    registerFailure(i, "SNMP", ms);
    return;
  }

  state[i].consecutiveFails = 0;
  state[i].warned           = false;
  state[i].upsCharge        = (int)charge;
  state[i].upsTemp          = (int)temp;
  state[i].valuesValid      = true;

  Serial.print("SNMP ");
  Serial.print(ms);
  Serial.print(" ms  battery ");
  Serial.print(charge);
  Serial.print("%, ");
  Serial.print(temp);
  Serial.println(" C");

  if (!state[i].upsTempFlag && temp >= UPS_TEMP_ALARM) {
    state[i].upsTempFlag = true;
    char label[24];
    snprintf(label, sizeof(label), "TEMP %ldC", temp);
    raiseAlarm(i, String(label), "");
    return;
  }
  if (state[i].upsTempFlag && temp < UPS_TEMP_ALARM) {
    state[i].upsTempFlag = false;
    Serial.print(units[i].name);
    Serial.println("  battery temperature back within range");
  }

  if (!state[i].upsChargeFlag && charge < UPS_CHARGE_ALARM) {
    state[i].upsChargeFlag = true;
    char label[24];
    snprintf(label, sizeof(label), "BATT %ld%%", charge);
    raiseAlarm(i, String(label), "");
    return;
  }
  if (state[i].upsChargeFlag && charge >= UPS_CHARGE_ALARM) {
    state[i].upsChargeFlag = false;
    Serial.print(units[i].name);
    Serial.println("  battery charge back within range");
  }
}

void pollUnit(int i) {
  setStatus(String("checking ") + units[i].name);

  Serial.print(units[i].name);
  Serial.print("  ");

  if      (units[i].type == UNIT_XML) pollXmlUnit(i);
  else if (units[i].type == UNIT_CSV) pollCsvUnit(i);
  else                                pollUpsUnit(i);

  // Only when the unit answered, so a dead one is not asked twice.
  if (units[i].type != UNIT_UPS && tempThisRound && state[i].consecutiveFails == 0) {
    checkTemperature(i);
  }
}

void runRound() {
  tempThisRound = (lastTempRun == 0) || (millis() - lastTempRun >= TEMP_INTERVAL_MS);

  for (int i = 0; i < UNIT_COUNT; i++) {
    pollUnit(i);
    if (i < UNIT_COUNT - 1) delay(UNIT_GAP_MS);
  }

  if (tempThisRound) lastTempRun = millis();

  lastRoundEnd = millis();
  setStatus("idle");

  Serial.print("--- round done, free heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.print(", largest block: ");
  Serial.print(ESP.getMaxAllocHeap());
  Serial.print(", wifi: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "ok" : "DOWN");
}

// ---------------------------------------------------------------------------

void setup() {
  buzzerSilent();

  Serial.begin(115200);
  delay(1000);

  Wire.begin(SDA_PIN, SCL_PIN);
  u8g2.begin();

  setStatus("connecting wifi");
  connectWifi();

  udp.begin(0);

  setStatus("loading state");
  loadState();

  runRound();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("wifi lost, reconnecting");
    WiFi.disconnect();
    connectWifi();
  }

  if (millis() - lastRoundEnd >= ROUND_INTERVAL_MS) {
    runRound();
  }

  refreshScreen();
  delay(1000);
}
