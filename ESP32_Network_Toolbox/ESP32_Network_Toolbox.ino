/*
  ============================================================================
   ESP32 Network Toolbox                                       Version 1.0.0
   Board: DIYMORE ESP32-S3 DevKitC-1 N16R8 (ESP32-S3, 16 MB Flash, 8 MB PSRAM)
  ============================================================================

   Diese eine Datei ist das komplette Projekt (Firmware + Weboberflaeche).

   ARDUINO-IDE-EINSTELLUNGEN (Menue "Werkzeuge"):
     Board:              ESP32S3 Dev Module
     USB CDC On Boot:    Disabled   (wenn du den Port "UART" benutzt)
                         Enabled    (wenn du den Port "USB" benutzt)
     CPU Frequency:      240MHz (WiFi)
     Flash Mode:         QIO 80MHz
     Flash Size:         16MB (128Mb)
     Partition Scheme:   16M Flash (3MB APP/9.9MB FATFS)     <- wichtig fuer OTA
     PSRAM:              OPI PSRAM                            <- wichtig!
     Upload Mode:        UART0 / Hardware CDC
     USB Mode:           Hardware CDC and JTAG
     Upload Speed:       921600 (bei Problemen 460800 oder 115200)
     Core Debug Level:   None

   BENOETIGT: Board-Paket "esp32 by Espressif Systems" ab Version 3.3.12
   (Boardverwalter). Es sind KEINE weiteren Bibliotheken noetig.

   NUTZUNG:
     WLAN "ESP32-NetworkTool" (Passwort: esp32setup) -> http://192.168.4.1
     Login: admin / admin123   (bitte nach dem ersten Start aendern!)

   NUR FUER EIGENE NETZE UND GERAETE: Dieses Projekt enthaelt bewusst KEINE
   Funktionen zum Stoeren, Trennen oder Angreifen fremder WLANs/Bluetooth-
   Geraete (kein Jamming, keine Deauth, kein Beacon-Flooding, kein Evil Twin).
  ============================================================================
*/

// ============================================================================
//  1) EINSTELLUNGEN, DIE DU ANPASSEN KANNST
// ============================================================================

#define FIRMWARE_VERSION      "1.0.0"     // vor jedem Release erhoehen! (x.y.z)
#define FIRMWARE_NAME         "ESP32 Network Toolbox"
#define BOARD_NAME            "ESP32-S3 N16R8"

// --- Testmodus: 1 = Fake-Daten (WLANs, BLE-Geraete, Clients, Update) ---------
// So kannst du die Weboberflaeche ausprobieren, ohne echte Umgebung.
#define DEMO_MODE             0

// --- Simulator (Velxio, Wokwi ...): dort gibt es keinen erreichbaren Access Point.
// Trage hier das Simulator-WLAN ein, dann verbindet sich der ESP32 als Station
// damit (kein eigener AP) und zeigt im Seriellen Monitor die klickbare IP-Adresse.
//   Velxio: "Velxio-GUEST"    Wokwi: "Wokwi-GUEST"    (beide ohne Passwort)
// Fuer den echten ESP32 leer lassen: ""
#define SIMULATOR_WIFI_SSID   ""
#define SIMULATOR_WIFI_PASS   ""

// --- Standardwerte beim ersten Start / nach Werkseinstellungen ---------------
#define DEFAULT_AP_SSID       "ESP32-NetworkTool"
#define DEFAULT_AP_PASS       "esp32setup"      // mind. 8 Zeichen
#define DEFAULT_AP_CHANNEL    6                 // 1..11
#define DEFAULT_WEB_USER      "admin"
#define DEFAULT_WEB_PASS      "admin123"
#define MDNS_NAME             "esp32-toolbox"   // erreichbar als esp32-toolbox.local

// --- GitHub-Update: HIER nur Benutzername und Repository eintragen -----------
//   Release-Datei muss heissen:  <FIRMWARE_ASSET_PREFIX>_v<Version>.bin
//   Beispiel: ESP32_Network_Toolbox_v1.0.1.bin
#define UPDATE_ENABLED        1
#define GITHUB_USER           "USERNAME"
#define GITHUB_REPO           "ESP32-Network-Toolbox"
#define FIRMWARE_ASSET_PREFIX "ESP32_Network_Toolbox"
#define REQUIRE_SHA256        1   // 1 = ohne SHA-256-Pruefsumme wird NICHT installiert

// ============================================================================
//  2) BIBLIOTHEKEN (alle im ESP32-Board-Paket enthalten)
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <time.h>
#include <stdarg.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Bitte im Boardverwalter 'esp32 by Espressif Systems' Version 3.3.12 oder neuer installieren."
#endif

// HTTPS mit eingebauter Zertifikatspruefung (Mozilla-Root-Zertifikate) gibt es
// ab Core 3.3.12. Bei aelteren Cores ist nur die Update-Funktion deaktiviert.
#if UPDATE_ENABLED && (ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 12))
#define HAVE_TLS 1
#else
#define HAVE_TLS 0
#endif

// ============================================================================
//  3) KONSTANTEN
// ============================================================================

#define MAX_WIFI_NETS   64
#define MAX_BLE_DEVS    80
#define MAX_LOG         80
#define MAX_BLOCKED     16
#define MAX_SESSIONS    4
#define AP_MAX_CLIENTS  6
#define SESSION_TTL_MS  (12UL * 3600UL * 1000UL)
#define STA_RETRY_MS    (300UL * 1000UL)
#define TEST_MAX_MS     (30UL * 60UL * 1000UL)

// Vorwaertsdeklaration der Weboberflaeche (steht ganz unten in dieser Datei)
extern const char INDEX_HTML[] PROGMEM;

// ============================================================================
//  4) DATENSTRUKTUREN UND GLOBALE VARIABLEN
// ============================================================================

struct Config {
  char apSsid[33];
  char apPass[65];
  uint8_t apCh;
  char webUser[33];
  char webSalt[17];
  char webHash[65];
  bool authEnabled;
  bool pwDefault;      // Web-Passwort ist noch das Standardpasswort
  char staSsid[33];
  char staPass[65];
};

struct LogEntry {
  uint32_t t;
  uint8_t lvl;         // 0 Info, 1 Warnung, 2 Fehler
  char msg[96];
};

struct WifiNet {
  char ssid[33];
  char bssid[18];
  int8_t rssi;
  uint8_t ch;
  uint8_t auth;
  bool hidden;
};

struct BleDev {
  char addr[18];
  char name[33];
  int16_t rssi;
  int32_t mfr;         // Hersteller-ID oder -1
  uint8_t nsvc;
  char svc[3][37];
  uint32_t seen;       // Uptime in Sekunden
};

struct ClientInfo {
  uint8_t mac[6];
  char ip[16];
  int8_t rssi;
  bool connected;
  bool blocked;
};

struct Session {
  char tok[33];
  uint32_t last;
};

struct UpdInfo {
  bool checked;
  bool available;
  bool canInstall;
  bool hasSha;
  int8_t internet;        // -1 unbekannt, 0 nein, 1 ja
  char github[80];        // Status der GitHub-Verbindung (Text)
  char latest[24];        // z. B. "1.0.1"
  char date[16];
  char notes[512];
  char assetName[80];
  char sha[65];
  uint32_t size;
  char lastCheck[24];
};

struct Release {
  char tag[24];
  char published[32];
  char notes[512];
  bool haveAsset;
  bool haveShaFile;
  uint32_t size;
  char sha[65];
};

enum UpdPhase { UPD_IDLE = 0, UPD_CHECKING, UPD_DOWNLOADING, UPD_INSTALLING, UPD_DONE, UPD_ERROR };
enum StaState { STA_IDLE = 0, STA_CONNECTING, STA_CONNECTED, STA_FAILED };

static Config g_cfg;
static WebServer g_server(80);
static DNSServer g_dns;
static Preferences g_prefs;
static Preferences g_updPrefs;

static LogEntry g_logs[MAX_LOG];
static uint16_t g_logCount = 0;
static uint16_t g_logHead = 0;
static portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;

static Session g_sess[MAX_SESSIONS];
static uint8_t g_failCount = 0;
static uint32_t g_lockUntil = 0;

// Access Point
static bool g_apWanted = true;
static bool g_apUp = false;
static char g_apCurSsid[33] = "";
static uint32_t g_apRestartAt = 0;
static uint32_t g_restartAt = 0;
static uint8_t g_blocked[MAX_BLOCKED][6];
static uint8_t g_blockedN = 0;
static uint32_t g_lastEnforce = 0;

// Testmodus (eigene Test-Access-Points, nacheinander)
static bool g_testActive = false;
static uint8_t g_testCount = 0;
static uint8_t g_testIdx = 0;
static uint16_t g_testInterval = 30;
static char g_testNames[5][33];
static uint32_t g_testSwitchAt = 0;
static uint32_t g_testEndAt = 0;

// WLAN-Scanner
static WifiNet g_nets[MAX_WIFI_NETS];
static int g_netN = 0;
static uint8_t g_scanState = 0;          // 0 idle, 1 laeuft, 2 fertig
static bool g_scanIsTrack = false;
static uint32_t g_scanStartMs = 0;
static uint32_t g_scanDoneSec = 0;
static bool g_trackFound = false;
static int g_trackRssi = -127;
static uint32_t g_trackSeq = 0;
static uint8_t g_trackBssid[6];

// Heim-WLAN (optional, nur fuer Updates)
static uint8_t g_staState = STA_IDLE;
static uint32_t g_staT0 = 0;
static uint32_t g_staRetryAt = 0;
static bool g_staWanted = false;
static bool g_needNtp = false;

// BLE
static BleDev g_ble[MAX_BLE_DEVS];
static int g_bleN = 0;
static SemaphoreHandle_t g_bleMutex = NULL;
static volatile uint8_t g_bleState = 0;  // 0 bereit, 1 scannt, 2 Fehler
static volatile bool g_bleStop = false;
static volatile bool g_bleInit = false;
static uint16_t g_bleDur = 10;
static uint32_t g_bleEndMs = 0;

// Update
static UpdInfo g_upd;
static portMUX_TYPE g_updMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool g_updBusy = false;
static volatile uint8_t g_updPhase = UPD_IDLE;
static volatile uint8_t g_updPct = 0;
static char g_updMsg[128] = "";
static char g_notice[192] = "";
static bool g_bootPending = false;
static bool g_uploadOk = false;
static bool g_uploadAuthFail = false;
static char g_uploadErr[64] = "";

// ============================================================================
//  5) KLEINE HILFSFUNKTIONEN
// ============================================================================

static uint32_t uptimeSec() {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void addLog(uint8_t lvl, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&g_logMux);
  g_logs[g_logHead].t = uptimeSec();
  g_logs[g_logHead].lvl = lvl;
  strlcpy(g_logs[g_logHead].msg, buf, sizeof(g_logs[g_logHead].msg));
  g_logHead = (g_logHead + 1) % MAX_LOG;
  if (g_logCount < MAX_LOG) g_logCount++;
  portEXIT_CRITICAL(&g_logMux);
  Serial.printf("[%lu] %s\n", (unsigned long)uptimeSec(), buf);
}

static bool timeValid() {
  return time(nullptr) > 1700000000L;
}

static void fmtLocalTime(char *out, size_t n) {
  time_t t = time(nullptr);
  struct tm tmv;
  localtime_r(&t, &tmv);
  strftime(out, n, "%d.%m.%Y %H:%M", &tmv);
}

static void macToStr(const uint8_t *mac, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parseMac(const char *s, uint8_t *mac) {
  unsigned int v[6];
  if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) {
    if (v[i] > 255) return false;
    mac[i] = (uint8_t)v[i];
  }
  return true;
}

static void hexEncode(const uint8_t *in, size_t n, char *out) {
  static const char *H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = H[in[i] >> 4];
    out[i * 2 + 1] = H[in[i] & 15];
  }
  out[n * 2] = 0;
}

static void randomHex(char *out, size_t bytes) {
  uint8_t b[16];
  if (bytes > 16) bytes = 16;
  for (size_t i = 0; i < bytes; i++) b[i] = (uint8_t)(esp_random() & 0xFF);
  hexEncode(b, bytes, out);
}

static bool constEq(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  if (la != lb) return false;
  uint8_t d = 0;
  for (size_t i = 0; i < la; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

static void hashPw(const char *salt, const char *pass, char *out65) {
  uint8_t h[32];
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, (const unsigned char *)salt, strlen(salt));
  mbedtls_sha256_update(&c, (const unsigned char *)":", 1);
  mbedtls_sha256_update(&c, (const unsigned char *)pass, strlen(pass));
  mbedtls_sha256_finish(&c, h);
  mbedtls_sha256_free(&c);
  hexEncode(h, 32, out65);
}

// ---- JSON-Ausgabe (einfach und ohne Bibliothek) ----------------------------

static void jsonStr(String &o, const char *s) {
  o += '"';
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  o += '"';
}

static void jS(String &o, const char *k, const char *v) {
  o += '"'; o += k; o += "\":";
  jsonStr(o, v);
  o += ',';
}

static void jI(String &o, const char *k, long v) {
  o += '"'; o += k; o += "\":";
  o += String(v);
  o += ',';
}

static void jB(String &o, const char *k, bool v) {
  o += '"'; o += k; o += "\":";
  o += v ? "true" : "false";
  o += ',';
}

static void jKey(String &o, const char *k) {
  o += '"'; o += k; o += "\":";
}

// Schliesst ein Objekt/Array: entfernt das letzte Komma und haengt das Zeichen an
static void jClose(String &o, char c) {
  if (o.length() && o[o.length() - 1] == ',') o.remove(o.length() - 1);
  o += c;
}

// ---- Mini-JSON-Leser (fuer die GitHub-Antwort) ------------------------------

static const char *jsWs(const char *p) {
  while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
  return p;
}

static const char *jsSkipStr(const char *p) {
  p++;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) p++;
    p++;
  }
  return *p ? p + 1 : p;
}

static const char *jsSkipVal(const char *p) {
  p = jsWs(p);
  if (*p == '"') return jsSkipStr(p);
  if (*p == '{' || *p == '[') {
    int d = 0;
    while (*p) {
      if (*p == '"') { p = jsSkipStr(p); continue; }
      if (*p == '{' || *p == '[') d++;
      else if (*p == '}' || *p == ']') {
        d--;
        if (d == 0) return p + 1;
      }
      p++;
    }
    return p;
  }
  while (*p && *p != ',' && *p != '}' && *p != ']') p++;
  return p;
}

// Sucht 'key' im Objekt, das bei 'obj' mit '{' beginnt. Liefert Zeiger auf den Wert.
static const char *jsFind(const char *obj, const char *key) {
  if (!obj) return NULL;
  const char *p = jsWs(obj);
  if (*p != '{') return NULL;
  p++;
  size_t kl = strlen(key);
  while (true) {
    p = jsWs(p);
    if (*p == ',') { p++; continue; }
    if (*p != '"') return NULL;
    const char *ks = p + 1;
    const char *after = jsSkipStr(p);
    if (after <= ks) return NULL;
    const char *ke = after - 1;
    p = jsWs(after);
    if (*p != ':') return NULL;
    p = jsWs(p + 1);
    if ((size_t)(ke - ks) == kl && strncmp(ks, key, kl) == 0) return p;
    p = jsSkipVal(p);
  }
}

static bool jsStr(const char *v, char *out, size_t cap) {
  if (!v || *v != '"' || cap == 0) return false;
  v++;
  size_t n = 0;
  while (*v && *v != '"') {
    char c = *v++;
    if (c == '\\') {
      char e = *v;
      if (!e) break;
      v++;
      switch (e) {
        case 'n': c = '\n'; break;
        case 'r': c = ' '; break;
        case 't': c = ' '; break;
        case 'u':
          for (int i = 0; i < 4 && *v; i++) v++;
          c = '?';
          break;
        default: c = e;
      }
    }
    if (n + 1 < cap) out[n++] = c;
  }
  out[n] = 0;
  return true;
}

static long jsNum(const char *v) {
  return v ? atol(v) : 0;
}

// ---- Versionsvergleich ("v1.2.3" oder "1.2.3") ------------------------------

static bool parseVersion(const char *s, uint32_t *out) {
  if (!s) return false;
  if (*s == 'v' || *s == 'V') s++;
  unsigned a = 0, b = 0, c = 0;
  char tail = 0;
  int n = sscanf(s, "%u.%u.%u%c", &a, &b, &c, &tail);
  if (n != 3 || a > 999 || b > 999 || c > 999) return false;
  *out = a * 1000000UL + b * 1000UL + c;
  return true;
}

// ============================================================================
//  6) EINSTELLUNGEN (Preferences = dauerhaft im Flash gespeichert)
// ============================================================================

static void saveWebCreds() {
  g_prefs.putString("web_user", g_cfg.webUser);
  g_prefs.putString("web_salt", g_cfg.webSalt);
  g_prefs.putString("web_hash", g_cfg.webHash);
  g_prefs.putBool("auth_en", g_cfg.authEnabled);
  g_prefs.putBool("pw_def", g_cfg.pwDefault);
}

static void saveApCfg() {
  g_prefs.putString("ap_ssid", g_cfg.apSsid);
  g_prefs.putString("ap_pass", g_cfg.apPass);
  g_prefs.putUChar("ap_ch", g_cfg.apCh);
}

static void saveStaCfg() {
  g_prefs.putString("sta_ssid", g_cfg.staSsid);
  g_prefs.putString("sta_pass", g_cfg.staPass);
}

static void saveBlocked() {
  String s;
  char b[18];
  for (int i = 0; i < g_blockedN; i++) {
    macToStr(g_blocked[i], b);
    if (i) s += ';';
    s += b;
  }
  g_prefs.putString("blocked", s);
}

static void loadBlocked() {
  g_blockedN = 0;
  String s = g_prefs.getString("blocked", "");
  int start = 0;
  while (start < (int)s.length() && g_blockedN < MAX_BLOCKED) {
    int end = s.indexOf(';', start);
    if (end < 0) end = s.length();
    String one = s.substring(start, end);
    if (parseMac(one.c_str(), g_blocked[g_blockedN])) g_blockedN++;
    start = end + 1;
  }
}

static void loadConfig() {
  g_prefs.begin("toolbox", false);
  String s;

  s = g_prefs.getString("ap_ssid", DEFAULT_AP_SSID);
  if (s.length() < 1 || s.length() > 32) s = DEFAULT_AP_SSID;
  strlcpy(g_cfg.apSsid, s.c_str(), sizeof(g_cfg.apSsid));

  s = g_prefs.getString("ap_pass", DEFAULT_AP_PASS);
  if (s.length() < 8 || s.length() > 63) s = DEFAULT_AP_PASS;
  strlcpy(g_cfg.apPass, s.c_str(), sizeof(g_cfg.apPass));

  g_cfg.apCh = g_prefs.getUChar("ap_ch", DEFAULT_AP_CHANNEL);
  if (g_cfg.apCh < 1 || g_cfg.apCh > 11) g_cfg.apCh = DEFAULT_AP_CHANNEL;

  s = g_prefs.getString("sta_ssid", "");
  strlcpy(g_cfg.staSsid, s.c_str(), sizeof(g_cfg.staSsid));
  s = g_prefs.getString("sta_pass", "");
  strlcpy(g_cfg.staPass, s.c_str(), sizeof(g_cfg.staPass));

  s = g_prefs.getString("web_user", DEFAULT_WEB_USER);
  if (s.length() < 1 || s.length() > 32) s = DEFAULT_WEB_USER;
  strlcpy(g_cfg.webUser, s.c_str(), sizeof(g_cfg.webUser));
  g_cfg.authEnabled = g_prefs.getBool("auth_en", true);
  g_cfg.pwDefault = g_prefs.getBool("pw_def", true);

  if (!g_prefs.isKey("web_hash")) {
    randomHex(g_cfg.webSalt, 8);
    hashPw(g_cfg.webSalt, DEFAULT_WEB_PASS, g_cfg.webHash);
    g_cfg.pwDefault = true;
    saveWebCreds();
  } else {
    s = g_prefs.getString("web_salt", "");
    strlcpy(g_cfg.webSalt, s.c_str(), sizeof(g_cfg.webSalt));
    s = g_prefs.getString("web_hash", "");
    strlcpy(g_cfg.webHash, s.c_str(), sizeof(g_cfg.webHash));
  }
  loadBlocked();
}

// ============================================================================
//  7) LOGIN / SITZUNGEN
// ============================================================================

static bool checkAuth() {
  if (!g_cfg.authEnabled) return true;
  String t = g_server.header("X-Token");
  if (t.length() != 32) t = g_server.arg("t");  // Ersatz, falls ein Proxy den Header entfernt
  if (t.length() != 32) return false;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (g_sess[i].tok[0] && t.equals(g_sess[i].tok)) {
      if ((uint32_t)(millis() - g_sess[i].last) > SESSION_TTL_MS) {
        g_sess[i].tok[0] = 0;
        return false;
      }
      g_sess[i].last = millis();
      return true;
    }
  }
  return false;
}

static void newSession(char *tokOut) {
  int slot = 0;
  uint32_t oldest = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!g_sess[i].tok[0]) { slot = i; oldest = 0xFFFFFFFF; break; }
    uint32_t age = millis() - g_sess[i].last;
    if (age >= oldest) { oldest = age; slot = i; }
  }
  randomHex(g_sess[slot].tok, 16);
  g_sess[slot].last = millis();
  strlcpy(tokOut, g_sess[slot].tok, 33);
}

static void dropSessionsExcept(const char *keep) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!keep || !g_sess[i].tok[0] || !constEq(g_sess[i].tok, keep)) g_sess[i].tok[0] = 0;
  }
}

static bool verifyPassword(const char *pass) {
  char h[65];
  hashPw(g_cfg.webSalt, pass, h);
  return constEq(h, g_cfg.webHash);
}

// ============================================================================
//  8) ACCESS POINT, HEIM-WLAN, CAPTIVE PORTAL
// ============================================================================

static bool applyAp(const char *ssid, const char *pass, int ch) {
  WiFi.enableAP(true);
  bool ok = WiFi.softAP(ssid, pass, ch, 0, AP_MAX_CLIENTS);
  g_apUp = ok;
  if (ok) {
    strlcpy(g_apCurSsid, ssid, sizeof(g_apCurSsid));
    g_dns.stop();
    g_dns.start(53, "*", WiFi.softAPIP());
  } else {
    g_apCurSsid[0] = 0;
  }
  return ok;
}

static void stopAp() {
  g_dns.stop();
  WiFi.softAPdisconnect(true);
  g_apUp = false;
  g_apCurSsid[0] = 0;
}

static bool isBlocked(const uint8_t *mac) {
  for (int i = 0; i < g_blockedN; i++)
    if (memcmp(g_blocked[i], mac, 6) == 0) return true;
  return false;
}

// Wirft gesperrte Geraete aus dem EIGENEN Access Point.
static void enforceBlocks() {
  if (!g_blockedN || !g_apUp) return;
  wifi_sta_list_t l;
  memset(&l, 0, sizeof(l));
  if (esp_wifi_ap_get_sta_list(&l) != ESP_OK) return;
  for (int i = 0; i < l.num; i++) {
    if (isBlocked(l.sta[i].mac)) {
      uint16_t aid = 0;
      if (esp_wifi_ap_get_sta_aid(l.sta[i].mac, &aid) == ESP_OK && aid) esp_wifi_deauth_sta(aid);
    }
  }
}

static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      char m[18];
      macToStr(info.wifi_ap_staconnected.mac, m);
      addLog(0, "Gerät verbunden: %s", m);
      g_lastEnforce = 0;  // Sperrliste sofort pruefen
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
      char m[18];
      macToStr(info.wifi_ap_stadisconnected.mac, m);
      addLog(0, "Gerät getrennt: %s", m);
      break;
    }
    default: break;
  }
}

static void staConnect() {
  if (!g_cfg.staSsid[0]) return;
  g_staWanted = true;
  WiFi.setAutoReconnect(false);
  WiFi.begin(g_cfg.staSsid, g_cfg.staPass[0] ? g_cfg.staPass : NULL);
  g_staState = STA_CONNECTING;
  g_staT0 = millis();
  addLog(0, "Verbinde mit Heim-WLAN '%s'...", g_cfg.staSsid);
}

static void staDisconnect() {
  g_staWanted = false;
  g_staState = STA_IDLE;
  WiFi.disconnect(false, false);
}

static void tickSta() {
  switch (g_staState) {
    case STA_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        g_staState = STA_CONNECTED;
        g_needNtp = true;
        addLog(0, "Heim-WLAN verbunden, IP %s", WiFi.localIP().toString().c_str());
        addLog(0, "Webinterface: http://%s", WiFi.localIP().toString().c_str());
      } else if ((uint32_t)(millis() - g_staT0) > 20000UL) {
        WiFi.disconnect(false, false);
        g_staState = STA_FAILED;
        g_staRetryAt = millis() + STA_RETRY_MS;
        addLog(1, "Heim-WLAN nicht erreichbar (naechster Versuch in 5 Min.)");
      }
      break;
    case STA_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        g_staState = STA_FAILED;
        g_staRetryAt = millis() + 15000UL;
        addLog(1, "Heim-WLAN-Verbindung verloren");
      }
      break;
    case STA_FAILED:
      if (g_staWanted && (int32_t)(millis() - g_staRetryAt) >= 0) staConnect();
      break;
    default: break;
  }
  if (g_needNtp && g_staState == STA_CONNECTED) {
    g_needNtp = false;
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
  }
}

static const char *staStateName() {
  switch (g_staState) {
    case STA_CONNECTING: return "connecting";
    case STA_CONNECTED: return "connected";
    case STA_FAILED: return "failed";
    default: return "idle";
  }
}

// ---- Test-WLANs: eigene Test-Access-Points, nacheinander ---------------------
//  Der ESP32 hat nur EIN WLAN-Funkmodul und kann nur EIN eigenes WLAN
//  gleichzeitig aussenden. Bei mehreren Test-WLANs werden sie deshalb der Reihe
//  nach aktiviert. Es werden nur eigene, zufaellig benannte Netze mit Passwort
//  erzeugt - keine fremden Namen, keine Rohdaten-Frames.

static bool ssidSeenInScan(const char *s) {
  for (int i = 0; i < g_netN; i++)
    if (strcmp(g_nets[i].ssid, s) == 0) return true;
  return false;
}

static void testSwitch(uint8_t idx) {
  g_testIdx = idx;
  applyAp(g_testNames[idx], g_cfg.apPass, g_cfg.apCh);
  g_testSwitchAt = millis() + (uint32_t)g_testInterval * 1000UL;
  addLog(0, "Test-WLAN aktiv: %s", g_testNames[idx]);
}

static void testStart(int count, int intervalSec) {
  static const char *prefix[3] = {"ESP32-Test-", "ESP32-LAB-", "ESP32-DEMO-"};
  if (count < 1) count = 1;
  if (count > 5) count = 5;
  if (intervalSec < 10) intervalSec = 10;
  if (intervalSec > 300) intervalSec = 300;
  g_testCount = (uint8_t)count;
  g_testInterval = (uint16_t)intervalSec;
  for (int i = 0; i < count; i++) {
    for (int tries = 0; tries < 20; tries++) {
      snprintf(g_testNames[i], sizeof(g_testNames[i]), "%s%04u", prefix[i % 3], (unsigned)(1000 + esp_random() % 9000));
      bool dup = ssidSeenInScan(g_testNames[i]);
      for (int j = 0; j < i; j++)
        if (strcmp(g_testNames[i], g_testNames[j]) == 0) dup = true;
      if (!dup) break;
    }
  }
  g_testActive = true;
  g_apWanted = true;
  g_testEndAt = millis() + TEST_MAX_MS;
  addLog(0, "Test-WLANs gestartet (%d)", count);
  testSwitch(0);
}

static void testStop() {
  if (!g_testActive) return;
  g_testActive = false;
  addLog(0, "Test-WLANs gestoppt");
  if (g_apWanted) applyAp(g_cfg.apSsid, g_cfg.apPass, g_cfg.apCh);
}

static void tickTest() {
  if (!g_testActive) return;
  if ((int32_t)(millis() - g_testEndAt) >= 0) {
    addLog(0, "Test-WLANs automatisch beendet (Zeitlimit)");
    testStop();
    return;
  }
  if (g_testCount > 1 && (int32_t)(millis() - g_testSwitchAt) >= 0) testSwitch((g_testIdx + 1) % g_testCount);
}

// ============================================================================
//  9) WLAN-SCANNER
// ============================================================================

static const char *authName(uint8_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "Offen";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "Andere";
  }
}

static void demoFillNets() {
  static const char *names[] = {"FRITZ!Box 7590 XY", "Vodafone-A1B2C3", "Telekom_FON", "WLAN-Buero", "TP-Link_5F2A", "Gast-Netz",
                                "DIRECT-roku-123", "Kabel-8842", "Homematic-AP", "ESP_Sensor_01", "Nachbar-WLAN", "iPhone von Max"};
  static const uint8_t auths[] = {WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_WPA2_PSK,
                                  WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA2_PSK};
  static const uint8_t chans[] = {1, 6, 11, 6, 3, 1, 9, 11, 13, 4, 6, 1};
  g_netN = 0;
  for (int i = 0; i < 12 && g_netN < MAX_WIFI_NETS; i++) {
    WifiNet &n = g_nets[g_netN++];
    strlcpy(n.ssid, names[i], sizeof(n.ssid));
    n.hidden = false;
    n.ch = chans[i];
    n.auth = auths[i];
    n.rssi = (int8_t)(-38 - i * 4 - (int)(esp_random() % 6));
    uint8_t mac[6] = {0x24, 0x0A, 0xC4, (uint8_t)(i * 17), (uint8_t)(i * 5 + 1), (uint8_t)(0xA0 + i)};
    macToStr(mac, n.bssid);
  }
}

static void copyScanResults(int cnt) {
  g_netN = 0;
  for (int i = 0; i < cnt && g_netN < MAX_WIFI_NETS; i++) {
    WifiNet &n = g_nets[g_netN];
    String s = WiFi.SSID(i);
    n.hidden = (s.length() == 0);
    strlcpy(n.ssid, n.hidden ? "(versteckt)" : s.c_str(), sizeof(n.ssid));
    strlcpy(n.bssid, WiFi.BSSIDstr(i).c_str(), sizeof(n.bssid));
    n.rssi = (int8_t)WiFi.RSSI(i);
    n.ch = (uint8_t)WiFi.channel(i);
    n.auth = (uint8_t)WiFi.encryptionType(i);
    g_netN++;
  }
}

// track=true: nur ein Kanal / ein Access Point wird abgefragt (RSSI-Monitor)
static bool wifiScanStart(bool track, int ch, const uint8_t *bssid) {
  if (g_updBusy) return false;
  if (g_scanState == 1) return true;
  g_scanIsTrack = track;
  g_scanStartMs = millis();
  if (DEMO_MODE) {
    g_scanState = 1;
    return true;
  }
  WiFi.scanDelete();
  int16_t r;
  if (track) r = WiFi.scanNetworks(true, true, false, 150, (uint8_t)ch, nullptr, bssid);
  else r = WiFi.scanNetworks(true, true, false, 300);
  if (r == WIFI_SCAN_FAILED) return false;
  g_scanState = 1;
  if (!track) addLog(0, "WLAN-Scan gestartet");
  return true;
}

static void wifiScanStop() {
  if (g_scanState != 1) return;
  if (!DEMO_MODE) {
    esp_wifi_scan_stop();
    WiFi.scanDelete();
  }
  g_scanState = g_netN > 0 ? 2 : 0;
  g_trackSeq++;
  addLog(0, "WLAN-Scan gestoppt");
}

static void finishTrack(int cnt) {
  g_trackFound = false;
  for (int i = 0; i < cnt; i++) {
    uint8_t *b = WiFi.BSSID(i);
    if (b && memcmp(b, g_trackBssid, 6) == 0) {
      g_trackFound = true;
      g_trackRssi = WiFi.RSSI(i);
      break;
    }
  }
  g_trackSeq++;
}

static void tickWifiScan() {
  if (g_scanState != 1) return;
  if (DEMO_MODE) {
    if ((uint32_t)(millis() - g_scanStartMs) < (g_scanIsTrack ? 300UL : 1800UL)) return;
    if (g_scanIsTrack) {
      g_trackFound = true;
      g_trackRssi = -55 + (int)(esp_random() % 15) - 7;
      g_trackSeq++;
      g_scanState = g_netN > 0 ? 2 : 0;
    } else {
      demoFillNets();
      g_scanDoneSec = uptimeSec();
      g_scanState = 2;
      addLog(0, "%d Netzwerke gefunden", g_netN);
    }
    return;
  }
  int16_t n = WiFi.scanComplete();
  if (n >= 0) {
    if (g_scanIsTrack) {
      finishTrack(n);
      g_scanState = g_netN > 0 ? 2 : 0;
    } else {
      copyScanResults(n);
      g_scanDoneSec = uptimeSec();
      g_scanState = 2;
      addLog(0, "%d Netzwerke gefunden", g_netN);
    }
    WiFi.scanDelete();
  } else if (n == WIFI_SCAN_FAILED || (uint32_t)(millis() - g_scanStartMs) > 25000UL) {
    WiFi.scanDelete();
    g_scanState = g_netN > 0 ? 2 : 0;
    if (g_scanIsTrack) {
      g_trackFound = false;
      g_trackSeq++;
    } else {
      addLog(2, "WLAN-Scan fehlgeschlagen");
    }
  }
}

// ============================================================================
//  10) GERAETE AM EIGENEN ACCESS POINT
// ============================================================================

static int getClients(ClientInfo *out, int maxN) {
  int n = 0;
  if (DEMO_MODE) {
    static const uint8_t m1[6] = {0xA4, 0x83, 0xE7, 0x12, 0x34, 0x56};
    static const uint8_t m2[6] = {0x3C, 0x22, 0xFB, 0x9A, 0xBC, 0xDE};
    memcpy(out[0].mac, m1, 6);
    strlcpy(out[0].ip, "192.168.4.2", sizeof(out[0].ip));
    out[0].rssi = -48; out[0].connected = true;
    memcpy(out[1].mac, m2, 6);
    strlcpy(out[1].ip, "192.168.4.3", sizeof(out[1].ip));
    out[1].rssi = -67; out[1].connected = true;
    n = 2;
  } else if (g_apUp) {
    wifi_sta_list_t l;
    memset(&l, 0, sizeof(l));
    if (esp_wifi_ap_get_sta_list(&l) == ESP_OK) {
      esp_netif_pair_mac_ip_t pairs[ESP_WIFI_MAX_CONN_NUM];
      memset(pairs, 0, sizeof(pairs));
      int cnt = l.num;
      if (cnt > ESP_WIFI_MAX_CONN_NUM) cnt = ESP_WIFI_MAX_CONN_NUM;
      for (int i = 0; i < cnt; i++) memcpy(pairs[i].mac, l.sta[i].mac, 6);
      esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
      bool haveIp = (nif != NULL) && cnt > 0 && (esp_netif_dhcps_get_clients_by_mac(nif, cnt, pairs) == ESP_OK);
      for (int i = 0; i < cnt && n < maxN; i++) {
        memcpy(out[n].mac, l.sta[i].mac, 6);
        out[n].rssi = l.sta[i].rssi;
        out[n].connected = true;
        if (haveIp && pairs[i].ip.addr != 0) {
          snprintf(out[n].ip, sizeof(out[n].ip), IPSTR, IP2STR(&pairs[i].ip));
        } else {
          strlcpy(out[n].ip, "-", sizeof(out[n].ip));
        }
        n++;
      }
    }
  }
  for (int i = 0; i < n; i++) out[i].blocked = isBlocked(out[i].mac);
  // gesperrte, aktuell nicht verbundene Geraete ebenfalls anzeigen (zum Entsperren)
  for (int b = 0; b < g_blockedN && n < maxN; b++) {
    bool found = false;
    for (int i = 0; i < n; i++)
      if (memcmp(out[i].mac, g_blocked[b], 6) == 0) found = true;
    if (!found) {
      memcpy(out[n].mac, g_blocked[b], 6);
      strlcpy(out[n].ip, "-", sizeof(out[n].ip));
      out[n].rssi = 0;
      out[n].connected = false;
      out[n].blocked = true;
      n++;
    }
  }
  return n;
}

// ============================================================================
//  11) BLUETOOTH LOW ENERGY (BLE-Scanner)
// ============================================================================

static const char *mfrName(int32_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0002: return "Intel";
    case 0x0059: return "Nordic Semiconductor";
    case 0x02E5: return "Espressif";
    case 0x038F: return "Xiaomi";
    case 0x027D: return "Huawei";
    case 0x0087: return "Garmin";
    case 0x009E: return "Bose";
    case 0x012D: return "Sony";
    case 0x000F: return "Broadcom";
    case 0x000D: return "Texas Instruments";
    default: return "";
  }
}

// 128-Bit-UUID mit Bluetooth-Basis in Kurzform ("0x180F") umwandeln
static void shortUuid(const char *full, char *out, size_t cap) {
  size_t l = strlen(full);
  if (l == 36 && strncmp(full, "0000", 4) == 0 && strcasecmp(full + 8, "-0000-1000-8000-00805f9b34fb") == 0) {
    snprintf(out, cap, "0x%.4s", full + 4);
    for (char *p = out + 2; *p; p++) *p = toupper(*p);
  } else {
    strlcpy(out, full, cap);
  }
}

static void bleAddDevice(const char *addr, const char *name, int rssi, int32_t mfr, int nsvc, char svc[][37]) {
  if (!g_bleMutex || xSemaphoreTake(g_bleMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  int idx = -1;
  for (int i = 0; i < g_bleN; i++)
    if (strcmp(g_ble[i].addr, addr) == 0) { idx = i; break; }
  if (idx < 0 && g_bleN < MAX_BLE_DEVS) {
    idx = g_bleN++;
    memset(&g_ble[idx], 0, sizeof(BleDev));
    strlcpy(g_ble[idx].addr, addr, sizeof(g_ble[idx].addr));
    g_ble[idx].mfr = -1;
  }
  if (idx >= 0) {
    BleDev &d = g_ble[idx];
    d.rssi = (int16_t)rssi;
    d.seen = uptimeSec();
    if (name && name[0]) strlcpy(d.name, name, sizeof(d.name));
    if (mfr >= 0) d.mfr = mfr;
    if (nsvc > 0 && d.nsvc == 0) {
      d.nsvc = (uint8_t)(nsvc > 3 ? 3 : nsvc);
      for (int i = 0; i < d.nsvc; i++) strlcpy(d.svc[i], svc[i], sizeof(d.svc[i]));
    }
  }
  xSemaphoreGive(g_bleMutex);
}

class ToolboxBleCallbacks : public BLEAdvertisedDeviceCallbacks {
public:
  void onResult(BLEAdvertisedDevice dev) override {
    String addr = dev.getAddress().toString();
    addr.toUpperCase();
    String name = dev.haveName() ? dev.getName() : String("");
    int32_t mfr = -1;
    if (dev.haveManufacturerData()) {
      String md = dev.getManufacturerData();
      if (md.length() >= 2) mfr = (int32_t)((uint8_t)md[0] | ((uint8_t)md[1] << 8));
    }
    char svc[3][37];
    int ns = 0;
    if (dev.haveServiceUUID()) {
      int cnt = dev.getServiceUUIDCount();
      for (int i = 0; i < cnt && ns < 3; i++) {
        String u = dev.getServiceUUID(i).toString();
        shortUuid(u.c_str(), svc[ns], sizeof(svc[ns]));
        ns++;
      }
    }
    bleAddDevice(addr.c_str(), name.c_str(), dev.getRSSI(), mfr, ns, svc);
  }
};

static ToolboxBleCallbacks g_bleCallbacks;

static void demoBleFill() {
  struct D { const char *addr; const char *name; int rssi; int mfr; const char *svc; };
  static const D demo[] = {
    {"5C:1B:F4:12:AA:01", "iPhone von Max", -52, 0x004C, ""},
    {"C8:2B:96:33:10:7E", "ESP32-Sensor", -61, 0x02E5, "0x181A"},
    {"F4:12:FA:88:19:C3", "Galaxy Watch", -70, 0x0075, "0x180F"},
    {"A0:D0:5B:44:5D:02", "Mi Band 7", -74, 0x038F, "0x180D"},
    {"E4:5F:01:AB:CD:EF", "", -83, 0x004C, ""},
    {"24:6F:28:0A:0B:0C", "Beacon-01", -66, -1, "0xFEAA"},
    {"D8:A0:1D:5E:6F:70", "JBL Flip 5", -58, -1, "0x110B"},
  };
  static uint8_t step = 0;
  if (step >= sizeof(demo) / sizeof(demo[0])) step = 0;
  const D &d = demo[step++];
  char svc[3][37];
  int ns = 0;
  if (d.svc[0]) { strlcpy(svc[0], d.svc, sizeof(svc[0])); ns = 1; }
  bleAddDevice(d.addr, d.name, d.rssi - (int)(esp_random() % 5), d.mfr, ns, svc);
}

static void bleTask(void *arg) {
  uint32_t deadline = millis() + (uint32_t)g_bleDur * 1000UL;
  g_bleEndMs = deadline;
  if (DEMO_MODE) {
    while ((int32_t)(millis() - deadline) < 0 && !g_bleStop) {
      demoBleFill();
      vTaskDelay(pdMS_TO_TICKS(700));
    }
  } else {
    if (!g_bleInit) {
      if (BLEDevice::init("ESP32-Toolbox")) {
        g_bleInit = true;
      } else {
        g_bleState = 2;
        addLog(2, "BLE konnte nicht gestartet werden");
        vTaskDelete(NULL);
        return;
      }
    }
    BLEScan *scan = BLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&g_bleCallbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(60);
    scan->start(g_bleDur, false);  // kann blockieren oder sofort zurueckkehren
    while ((int32_t)(millis() - deadline) < 0 && !g_bleStop && scan->isScanning()) vTaskDelay(pdMS_TO_TICKS(100));
    scan->stop();
    scan->clearResults();
  }
  g_bleState = 0;
  addLog(0, "BLE-Scan beendet (%d Geräte)", g_bleN);
  vTaskDelete(NULL);
}

static bool bleScanStart(int seconds) {
  if (g_bleState == 1 || g_updBusy) return g_bleState == 1;
  if (seconds < 3) seconds = 3;
  if (seconds > 120) seconds = 120;
  g_bleDur = (uint16_t)seconds;
  g_bleStop = false;
  g_bleState = 1;
  if (xTaskCreate(bleTask, "ble_scan", 8192, NULL, 1, NULL) != pdPASS) {
    g_bleState = 0;
    return false;
  }
  addLog(0, "BLE-Scan gestartet");
  return true;
}

static void bleScanStop() {
  if (g_bleState != 1) return;
  g_bleStop = true;
  if (!DEMO_MODE && g_bleInit) BLEDevice::getScan()->stop();
}

static void bleClear() {
  if (g_bleMutex && xSemaphoreTake(g_bleMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    g_bleN = 0;
    xSemaphoreGive(g_bleMutex);
  }
  addLog(0, "BLE-Ergebnisse gelöscht");
}

// ============================================================================
//  12) FIRMWARE-UPDATE UEBER GITHUB (HTTPS, SHA-256, Rollback-Schutz)
// ============================================================================

static void updSetMsg(uint8_t phase, uint8_t pct, const char *msg) {
  portENTER_CRITICAL(&g_updMux);
  g_updPhase = phase;
  g_updPct = pct;
  strlcpy(g_updMsg, msg, sizeof(g_updMsg));
  portEXIT_CRITICAL(&g_updMux);
}

static void updSetGithub(int8_t internet, const char *text) {
  portENTER_CRITICAL(&g_updMux);
  g_upd.internet = internet;
  strlcpy(g_upd.github, text, sizeof(g_upd.github));
  portEXIT_CRITICAL(&g_updMux);
}

static bool updConfigured() {
  return strcmp(GITHUB_USER, "USERNAME") != 0 && strlen(GITHUB_USER) > 0 && strlen(GITHUB_REPO) > 0;
}

static void updExpectedAsset(const char *ver, char *out, size_t cap) {
  snprintf(out, cap, "%s_v%s.bin", FIRMWARE_ASSET_PREFIX, ver);
}

#if HAVE_TLS
// Nur HTTPS und nur GitHub-Hosts erlauben.
static bool hostAllowed(const String &url) {
  if (!url.startsWith("https://")) return false;
  int s = 8;
  int e = url.indexOf('/', s);
  if (e < 0) e = url.length();
  String host = url.substring(s, e);
  int colon = host.indexOf(':');
  if (colon >= 0) host = host.substring(0, colon);
  host.toLowerCase();
  return host == "api.github.com" || host == "github.com" || host.endsWith(".githubusercontent.com");
}

// GET mit sicherer, manueller Weiterleitung (jeder Sprung wird geprueft).
static int httpsGet(const String &startUrl, HTTPClient &http, NetworkClientSecure &client) {
  String url = startUrl;
  for (int hop = 0; hop < 5; hop++) {
    if (!hostAllowed(url)) return -100;
    http.end();
    client.stop();
    if (!http.begin(client, url)) return -101;
    http.useHTTP10(true);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent("ESP32-Network-Toolbox/" FIRMWARE_VERSION);
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
      url = http.getLocation();
      if (!url.length()) return -102;
      continue;
    }
    return code;
  }
  return -103;
}

static bool readBody(HTTPClient &http, String &body, size_t maxLen) {
  Stream *s = http.getStreamPtr();
  int total = http.getSize();
  if (total > 0 && (size_t)total > maxLen) return false;
  body = "";
  body.reserve(total > 0 ? total + 1 : 8192);
  uint8_t buf[512];
  uint32_t t0 = millis();
  while ((millis() - t0) < 15000UL) {
    int av = s->available();
    if (av > 0) {
      int n = s->readBytes(buf, av > (int)sizeof(buf) ? sizeof(buf) : av);
      if (n <= 0) break;
      if (body.length() + n > maxLen) return false;
      body.concat((const char *)buf, n);
      t0 = millis();
      if (total > 0 && (int)body.length() >= total) break;
    } else if (!http.connected()) {
      break;
    } else {
      delay(5);
    }
  }
  return body.length() > 0;
}

static bool waitForTime() {
  for (int i = 0; i < 40 && !timeValid(); i++) delay(250);
  return timeValid();
}
#endif  // HAVE_TLS

static const char *httpErrText(int code) {
  static char b[48];
  if (code == 404) return "GitHub: Release/Repository nicht gefunden (HTTP 404)";
  if (code == 403 || code == 429) return "GitHub: Anfrage-Limit erreicht, später erneut versuchen";
  if (code == -100) return "Unerlaubte Adresse (nur GitHub erlaubt)";
  if (code < 0) {
    snprintf(b, sizeof(b), "Verbindungsfehler (%d)", code);
    return b;
  }
  snprintf(b, sizeof(b), "GitHub: HTTP-Fehler %d", code);
  return b;
}

// Liest die GitHub-Release-Antwort. Rueckgabe: 0 = ok, 1 = kein tag_name, 2 = Version ungueltig
static int parseRelease(const char *root, Release *r) {
  memset(r, 0, sizeof(*r));
  if (!jsStr(jsFind(root, "tag_name"), r->tag, sizeof(r->tag))) return 1;
  jsStr(jsFind(root, "published_at"), r->published, sizeof(r->published));
  jsStr(jsFind(root, "body"), r->notes, sizeof(r->notes));
  uint32_t v = 0;
  if (!parseVersion(r->tag, &v)) return 2;
  const char *ver = (r->tag[0] == 'v' || r->tag[0] == 'V') ? r->tag + 1 : r->tag;
  char expected[80], expectedSha[96];
  updExpectedAsset(ver, expected, sizeof(expected));
  snprintf(expectedSha, sizeof(expectedSha), "%s.sha256", expected);
  const char *assets = jsFind(root, "assets");
  if (assets && *assets == '[') {
    const char *p = jsWs(assets + 1);
    while (*p && *p != ']') {
      if (*p == '{') {
        char nm[96] = "";
        jsStr(jsFind(p, "name"), nm, sizeof(nm));
        if (strcmp(nm, expected) == 0) {
          r->haveAsset = true;
          r->size = (uint32_t)jsNum(jsFind(p, "size"));
          char dg[100] = "";
          if (jsStr(jsFind(p, "digest"), dg, sizeof(dg)) && strncmp(dg, "sha256:", 7) == 0 && strlen(dg + 7) == 64) strlcpy(r->sha, dg + 7, sizeof(r->sha));
        } else if (strcmp(nm, expectedSha) == 0) {
          r->haveShaFile = true;
        }
      }
      p = jsSkipVal(p);
      p = jsWs(p);
      if (*p == ',') p = jsWs(p + 1);
    }
  }
  return 0;
}

static void updDoCheck() {
  updSetMsg(UPD_CHECKING, 0, "Suche nach Updates...");
  char stamp[24] = "";
  if (DEMO_MODE) {
    delay(1200);
    portENTER_CRITICAL(&g_updMux);
    g_upd.checked = true;
    g_upd.available = true;
    g_upd.canInstall = true;
    g_upd.hasSha = true;
    g_upd.internet = 1;
    strlcpy(g_upd.github, "Demo: GitHub simuliert", sizeof(g_upd.github));
    strlcpy(g_upd.latest, "9.9.9", sizeof(g_upd.latest));
    strlcpy(g_upd.date, "01.01.2027", sizeof(g_upd.date));
    strlcpy(g_upd.notes, "Demo-Update:\n- Neues Design\n- Schnellerer WLAN-Scan\n- Fehlerbehebungen", sizeof(g_upd.notes));
    strlcpy(g_upd.assetName, "ESP32_Network_Toolbox_v9.9.9.bin", sizeof(g_upd.assetName));
    g_upd.size = 1500000;
    strlcpy(g_upd.lastCheck, "Demo", sizeof(g_upd.lastCheck));
    portEXIT_CRITICAL(&g_updMux);
    updSetMsg(UPD_IDLE, 0, "Neue Version verfügbar: v9.9.9");
    return;
  }
#if !HAVE_TLS
  (void)stamp;
  updSetGithub(-1, "Nicht unterstützt (ESP32-Core zu alt)");
  updSetMsg(UPD_ERROR, 0, "Update-Funktion benötigt ESP32-Core 3.3.12 oder neuer.");
  return;
#else
  if (!updConfigured()) {
    updSetGithub(-1, "Nicht konfiguriert");
    updSetMsg(UPD_ERROR, 0, "GitHub-Benutzer ist nicht eingetragen (GITHUB_USER am Anfang der .ino).");
    return;
  }
  if (g_staState != STA_CONNECTED) {
    updSetGithub(0, "Nicht erreichbar");
    updSetMsg(UPD_ERROR, 0, "Keine Internetverbindung – Updateprüfung nicht möglich.");
    return;
  }
  IPAddress ip;
  if (!WiFi.hostByName("api.github.com", ip)) {
    updSetGithub(0, "Nicht erreichbar");
    updSetMsg(UPD_ERROR, 0, "Keine Internetverbindung – Updateprüfung nicht möglich.");
    return;
  }
  if (!waitForTime()) {
    updSetGithub(-1, "Uhrzeit fehlt");
    updSetMsg(UPD_ERROR, 0, "Uhrzeit (NTP) nicht verfügbar – sichere Verbindung nicht möglich.");
    return;
  }
  fmtLocalTime(stamp, sizeof(stamp));
  if (ESP.getMaxAllocHeap() < 45000) {
    updSetMsg(UPD_ERROR, 0, "Zu wenig freier Speicher – bitte ESP32 neu starten.");
    return;
  }

  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(20);
  HTTPClient http;
  String url = String("https://api.github.com/repos/") + GITHUB_USER + "/" + GITHUB_REPO + "/releases/latest";
  int code = httpsGet(url, http, client);
  if (code != 200) {
    updSetGithub(code > 0 ? 1 : 0, httpErrText(code));
    updSetMsg(UPD_ERROR, 0, httpErrText(code));
    http.end();
    return;
  }
  String body;
  bool okBody = readBody(http, body, 90000);
  http.end();
  client.stop();
  if (!okBody) {
    updSetGithub(1, "Antwort ungültig oder zu groß");
    updSetMsg(UPD_ERROR, 0, "GitHub-Antwort konnte nicht gelesen werden.");
    return;
  }

  Release rel;
  int pr = parseRelease(body.c_str(), &rel);
  if (pr == 1) {
    updSetGithub(1, "Antwort ohne Versionsnummer");
    updSetMsg(UPD_ERROR, 0, "Kein gültiges Release gefunden.");
    return;
  }
  uint32_t vNew = 0, vCur = 0;
  if (!parseVersion(rel.tag, &vNew) || !parseVersion(FIRMWARE_VERSION, &vCur)) {
    updSetGithub(1, "Ungültige Versionsnummer");
    updSetMsg(UPD_ERROR, 0, "Versionsnummer des Releases ist ungültig (erwartet: v1.2.3).");
    return;
  }
  const char *ver = (rel.tag[0] == 'v' || rel.tag[0] == 'V') ? rel.tag + 1 : rel.tag;
  char expected[80], expectedSha[96];
  updExpectedAsset(ver, expected, sizeof(expected));
  snprintf(expectedSha, sizeof(expectedSha), "%s.sha256", expected);
  const char *tag = rel.tag;
  const char *published = rel.published;
  const char *notes = rel.notes;
  bool haveAsset = rel.haveAsset, haveShaFile = rel.haveShaFile;
  uint32_t size = rel.size;
  char sha[65];
  strlcpy(sha, rel.sha, sizeof(sha));

  // Pruefsumme aus separater .sha256-Datei holen, falls GitHub keine liefert
  if (haveAsset && !sha[0] && haveShaFile) {
    String surl = String("https://github.com/") + GITHUB_USER + "/" + GITHUB_REPO + "/releases/download/" + tag + "/" + expectedSha;
    int c2 = httpsGet(surl, http, client);
    if (c2 == 200) {
      String sb;
      if (readBody(http, sb, 512) && sb.length() >= 64) {
        bool ok = true;
        for (int i = 0; i < 64; i++)
          if (!isxdigit((unsigned char)sb[i])) ok = false;
        if (ok) {
          strlcpy(sha, sb.substring(0, 64).c_str(), sizeof(sha));
          for (char *q = sha; *q; q++) *q = tolower(*q);
        }
      }
    }
    http.end();
    client.stop();
  }

  char date[16] = "";
  if (strlen(published) >= 10) snprintf(date, sizeof(date), "%.2s.%.2s.%.4s", published + 8, published + 5, published);

  portENTER_CRITICAL(&g_updMux);
  g_upd.checked = true;
  g_upd.internet = 1;
  strlcpy(g_upd.github, "Verbunden", sizeof(g_upd.github));
  strlcpy(g_upd.latest, ver, sizeof(g_upd.latest));
  strlcpy(g_upd.date, date, sizeof(g_upd.date));
  strlcpy(g_upd.notes, notes, sizeof(g_upd.notes));
  strlcpy(g_upd.assetName, expected, sizeof(g_upd.assetName));
  strlcpy(g_upd.sha, sha, sizeof(g_upd.sha));
  strlcpy(g_upd.lastCheck, stamp, sizeof(g_upd.lastCheck));
  g_upd.hasSha = sha[0] != 0;
  g_upd.size = size;
  g_upd.available = (vNew > vCur);
  g_upd.canInstall = g_upd.available && haveAsset && size > 100000 && (g_upd.hasSha || !REQUIRE_SHA256);
  portEXIT_CRITICAL(&g_updMux);

  if (!(vNew > vCur)) {
    updSetMsg(UPD_IDLE, 0, "Du verwendest bereits die aktuellste Version.");
  } else if (!haveAsset) {
    char m[192];
    snprintf(m, sizeof(m), "Release v%s enthält die Datei %s nicht.", ver, expected);
    updSetMsg(UPD_ERROR, 0, m);
  } else if (REQUIRE_SHA256 && !sha[0]) {
    updSetMsg(UPD_ERROR, 0, "Release hat keine SHA-256-Prüfsumme – Installation aus Sicherheitsgründen gesperrt.");
  } else {
    char m[64];
    snprintf(m, sizeof(m), "Neue Version verfügbar: v%s", ver);
    updSetMsg(UPD_IDLE, 0, m);
  }
  addLog(0, "Update-Prüfung: neueste Version v%s", ver);
#endif  // HAVE_TLS
}

static void updFail(const char *msg) {
  if (Update.isRunning()) Update.abort();
  updSetMsg(UPD_ERROR, 0, msg);
  addLog(2, "Update fehlgeschlagen: %s", msg);
}

static void updDoInstall() {
  UpdInfo u;
  portENTER_CRITICAL(&g_updMux);
  u = g_upd;
  portEXIT_CRITICAL(&g_updMux);
  if (!u.available || !u.canInstall) {
    updSetMsg(UPD_ERROR, 0, "Kein installierbares Update vorhanden – bitte zuerst nach Updates suchen.");
    return;
  }
  addLog(0, "Update auf v%s gestartet", u.latest);

  if (DEMO_MODE) {
    for (int p = 0; p <= 100; p += 4) {
      updSetMsg(UPD_DOWNLOADING, (uint8_t)p, "Firmware wird heruntergeladen...");
      delay(120);
    }
    updSetMsg(UPD_INSTALLING, 100, "Firmware wird installiert...");
    delay(1500);
    updSetMsg(UPD_DONE, 100, "Demo: Update simuliert – es wurde nichts installiert.");
    return;
  }
#if !HAVE_TLS
  updFail("Update-Funktion benötigt ESP32-Core 3.3.12 oder neuer.");
  return;
#else
  if (g_staState != STA_CONNECTED) {
    updFail("Keine Internetverbindung.");
    return;
  }
  if (u.size > ESP.getFreeSketchSpace()) {
    updFail("Firmware ist größer als die Update-Partition (Partition Scheme prüfen).");
    return;
  }
  if (ESP.getMaxAllocHeap() < 45000) {
    updFail("Zu wenig freier Speicher – bitte ESP32 neu starten.");
    return;
  }
  if (!waitForTime()) {
    updFail("Uhrzeit (NTP) nicht verfügbar.");
    return;
  }

  updSetMsg(UPD_DOWNLOADING, 0, "Firmware wird heruntergeladen...");
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  client.setHandshakeTimeout(20);
  HTTPClient http;
  String tag = String("v") + u.latest;
  String url = String("https://github.com/") + GITHUB_USER + "/" + GITHUB_REPO + "/releases/download/" + tag + "/" + u.assetName;
  int code = httpsGet(url, http, client);
  if (code != 200) {
    http.end();
    updFail(httpErrText(code));
    return;
  }
  int total = http.getSize();
  if (total <= 0 || (uint32_t)total != u.size) {
    http.end();
    updFail("Dateigröße stimmt nicht mit dem Release überein.");
    return;
  }
  if (!Update.begin((size_t)total, U_FLASH)) {
    http.end();
    updFail("Update-Speicher konnte nicht vorbereitet werden.");
    return;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  uint8_t *buf = (uint8_t *)malloc(2048);
  if (!buf) {
    mbedtls_sha256_free(&sha);
    http.end();
    updFail("Zu wenig Speicher.");
    return;
  }
  Stream *stream = http.getStreamPtr();
  int written = 0;
  uint32_t lastData = millis();
  uint8_t lastPct = 255;
  bool ok = true;
  while (written < total) {
    int av = stream->available();
    if (av <= 0) {
      if (!http.connected() || (millis() - lastData) > 20000UL) { ok = false; break; }
      delay(2);
      continue;
    }
    int want = total - written;
    if (want > 2048) want = 2048;
    if (want > av) want = av;
    int n = stream->readBytes(buf, want);
    if (n <= 0) { ok = false; break; }
    if (Update.write(buf, n) != (size_t)n) { ok = false; break; }
    mbedtls_sha256_update(&sha, buf, n);
    written += n;
    lastData = millis();
    uint8_t pct = (uint8_t)((uint64_t)written * 100 / (uint64_t)total);
    if (pct != lastPct) {
      lastPct = pct;
      updSetMsg(UPD_DOWNLOADING, pct, "Firmware wird heruntergeladen...");
    }
  }
  free(buf);
  http.end();
  client.stop();
  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  if (!ok || written != total) {
    updFail("Download unvollständig oder unterbrochen – aktuelle Firmware bleibt erhalten.");
    return;
  }

  updSetMsg(UPD_INSTALLING, 100, "Firmware wird geprüft und installiert...");
  char hex[65];
  hexEncode(digest, 32, hex);
  if (u.sha[0]) {
    if (strcasecmp(hex, u.sha) != 0) {
      updFail("SHA-256-Prüfsumme stimmt nicht überein – Update verworfen.");
      return;
    }
  } else if (REQUIRE_SHA256) {
    updFail("Keine SHA-256-Prüfsumme vorhanden.");
    return;
  }
  if (!Update.end(true)) {
    updFail("Firmware ungültig oder Installation fehlgeschlagen.");
    return;
  }
  g_updPrefs.putBool("pend", true);
  g_updPrefs.putUChar("boots", 0);
  g_updPrefs.putString("target", u.latest);
  g_updPrefs.putString("from", FIRMWARE_VERSION);
  addLog(0, "Update auf v%s installiert, Neustart...", u.latest);
  updSetMsg(UPD_DONE, 100, "Update erfolgreich – ESP32 startet neu...");
  g_restartAt = millis() + 3000UL;
#endif  // HAVE_TLS
}

static void updTask(void *arg) {
  int job = (int)(intptr_t)arg;
  if (job == 1) updDoCheck();
  else updDoInstall();
  g_updBusy = false;
  vTaskDelete(NULL);
}

static bool updStart(int job) {
  if (g_updBusy || g_scanState == 1 || g_bleState == 1) return false;
  g_updBusy = true;
  if (xTaskCreatePinnedToCore(updTask, "upd", 16384, (void *)(intptr_t)job, 1, NULL, 0) != pdPASS) {
    g_updBusy = false;
    return false;
  }
  return true;
}

// Beim Start: pruefen, ob gerade ein Update eingespielt wurde, und ob die neue
// Firmware stabil laeuft. Wenn nicht (3 Fehlstarts), wird automatisch die
// vorherige Firmware wieder aktiviert.
static void bootGuard() {
  g_updPrefs.begin("upd", false);
  g_bootPending = g_updPrefs.getBool("pend", false);
  String notice = g_updPrefs.getString("notice", "");
  if (g_bootPending) {
    uint8_t boots = g_updPrefs.getUChar("boots", 0) + 1;
    g_updPrefs.putUChar("boots", boots);
    if (boots > 3) {
      g_updPrefs.putBool("pend", false);
      g_bootPending = false;
      const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
      if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
        g_updPrefs.putString("notice", "Die neue Firmware lief nicht stabil - die vorherige Version wurde wiederhergestellt.");
        delay(200);
        ESP.restart();
      }
      notice = "Die neue Firmware lief nicht stabil, die alte Version konnte nicht automatisch wiederhergestellt werden.";
      g_updPrefs.putString("notice", notice);
    } else if (boots == 1) {
      String target = g_updPrefs.getString("target", "");
      if (target == "manuell") notice = String("Firmware manuell aktualisiert (v") + FIRMWARE_VERSION + ")";
      else if (target == FIRMWARE_VERSION) notice = String("Update erfolgreich auf v") + target + " aktualisiert";
      else notice = String("Update installiert, aber die Firmware meldet v") + FIRMWARE_VERSION + " statt v" + target + ". FIRMWARE_VERSION im Code anpassen!";
      g_updPrefs.putString("notice", notice);
    }
  }
  strlcpy(g_notice, notice.c_str(), sizeof(g_notice));
}

static void tickBootConfirm() {
  if (g_bootPending && uptimeSec() >= 60) {
    g_bootPending = false;
    g_updPrefs.putBool("pend", false);
    g_updPrefs.putUChar("boots", 0);
    addLog(0, "Neue Firmware läuft stabil");
  }
}

// ============================================================================
//  13) WEB-API (alle Endpunkte)
// ============================================================================

static void sendJson(int code, const String &body) {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(code, "application/json", body);
}

static void sendErr(int code, const char *msg) {
  String o = "{\"ok\":false,";
  jS(o, "error", msg);
  jClose(o, '}');
  sendJson(code, o);
}

static void sendOk(const char *msg) {
  String o = "{\"ok\":true,";
  if (msg && msg[0]) jS(o, "msg", msg);
  jClose(o, '}');
  sendJson(200, o);
}

#define NEED_AUTH()                          \
  if (!checkAuth()) {                        \
    sendErr(401, "Nicht angemeldet");        \
    return;                                  \
  }

static void appendSta(String &o) {
  jKey(o, "sta");
  o += '{';
  jS(o, "state", staStateName());
  jS(o, "ssid", g_cfg.staSsid);
  bool up = (g_staState == STA_CONNECTED);
  jS(o, "ip", up ? WiFi.localIP().toString().c_str() : "");
  jI(o, "rssi", up ? WiFi.RSSI() : 0);
  jClose(o, '}');
  o += ',';
}

static void handlePublic() {
  String o = "{";
  jS(o, "name", FIRMWARE_NAME);
  jS(o, "fw", FIRMWARE_VERSION);
  jB(o, "auth", g_cfg.authEnabled);
  jB(o, "demo", DEMO_MODE);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleLogin() {
  uint32_t now = millis();
  if (g_lockUntil && (int32_t)(now - g_lockUntil) < 0) {
    sendErr(429, "Zu viele Versuche - bitte kurz warten.");
    return;
  }
  String u = g_server.arg("user");
  String p = g_server.arg("pass");
  bool ok = (u == g_cfg.webUser) && verifyPassword(p.c_str());
  if (!ok) {
    if (++g_failCount >= 5) {
      g_lockUntil = now + 60000UL;
      g_failCount = 0;
      addLog(1, "Login gesperrt (zu viele Fehlversuche)");
    } else {
      addLog(1, "Fehlgeschlagener Login-Versuch");
    }
    sendErr(401, "Benutzername oder Passwort falsch");
    return;
  }
  g_failCount = 0;
  g_lockUntil = 0;
  char tok[33];
  newSession(tok);
  addLog(0, "Login erfolgreich");
  String o = "{\"ok\":true,";
  jS(o, "token", tok);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleLogout() {
  String t = g_server.header("X-Token");
  if (t.length() != 32) t = g_server.arg("t");
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (g_sess[i].tok[0] && t.equals(g_sess[i].tok)) g_sess[i].tok[0] = 0;
  sendOk("");
}

static void handleStatus() {
  NEED_AUTH();
  String o;
  o.reserve(1800);
  o = "{";
  jS(o, "fw", FIRMWARE_VERSION);
  jS(o, "board", BOARD_NAME);
  jI(o, "up", uptimeSec());
  jB(o, "demo", DEMO_MODE);
  jB(o, "auth", g_cfg.authEnabled);
  jS(o, "notice", g_notice);

  jKey(o, "chip");
  o += '{';
  jS(o, "model", ESP.getChipModel());
  jI(o, "rev", ESP.getChipRevision());
  jI(o, "cores", ESP.getChipCores());
  jI(o, "mhz", ESP.getCpuFreqMHz());
  jS(o, "mac", WiFi.macAddress().c_str());
  jS(o, "sdk", ESP.getSdkVersion());
  jS(o, "core", ESP_ARDUINO_VERSION_STR);
  jKey(o, "temp");
  o += String(temperatureRead(), 1);
  o += ',';
  jClose(o, '}');
  o += ',';

  jKey(o, "mem");
  o += '{';
  jI(o, "heap", ESP.getHeapSize());
  jI(o, "free", ESP.getFreeHeap());
  jI(o, "minFree", ESP.getMinFreeHeap());
  jI(o, "psram", ESP.getPsramSize());
  jI(o, "psramFree", ESP.getFreePsram());
  jI(o, "flash", ESP.getFlashChipSize());
  jI(o, "sketch", ESP.getSketchSize());
  jI(o, "sketchFree", ESP.getFreeSketchSpace());
  jClose(o, '}');
  o += ',';

  jKey(o, "wifi");
  o += '{';
  jB(o, "ap", g_apUp);
  jS(o, "ssid", g_apCurSsid);
  jI(o, "ch", g_apUp ? WiFi.channel() : g_cfg.apCh);
  jS(o, "ip", WiFi.softAPIP().toString().c_str());
  jI(o, "clients", DEMO_MODE ? 2 : WiFi.softAPgetStationNum());
  jB(o, "test", g_testActive);
  jI(o, "found", g_netN);
  jB(o, "scanning", g_scanState == 1 && !g_scanIsTrack);
  appendSta(o);
  jClose(o, '}');
  o += ',';

  jKey(o, "ble");
  o += '{';
  jS(o, "state", g_bleState == 1 ? "scanning" : (g_bleState == 2 ? "error" : "ready"));
  jI(o, "found", g_bleN);
  jClose(o, '}');
  o += ',';

  jKey(o, "warn");
  o += '{';
  jB(o, "defaultWeb", g_cfg.pwDefault);
  jB(o, "defaultAp", strcmp(g_cfg.apPass, DEFAULT_AP_PASS) == 0);
  jClose(o, '}');
  jClose(o, '}');
  sendJson(200, o);
}

// ---- WLAN-Scanner ----

static void handleWifiScanGet() {
  NEED_AUTH();
  String o;
  o.reserve(400 + g_netN * 150);
  o = "{";
  jS(o, "state", g_scanState == 1 ? "running" : (g_scanState == 2 ? "done" : "idle"));
  jB(o, "track", g_scanIsTrack);
  jI(o, "count", g_netN);
  jI(o, "age", g_scanDoneSec ? (long)(uptimeSec() - g_scanDoneSec) : -1);
  jKey(o, "nets");
  o += '[';
  for (int i = 0; i < g_netN; i++) {
    const WifiNet &n = g_nets[i];
    o += '{';
    jS(o, "ssid", n.ssid);
    jS(o, "bssid", n.bssid);
    jI(o, "rssi", n.rssi);
    jI(o, "ch", n.ch);
    jS(o, "auth", authName(n.auth));
    jB(o, "hidden", n.hidden);
    jClose(o, '}');
    o += ',';
  }
  jClose(o, ']');
  o += ',';
  jClose(o, '}');
  sendJson(200, o);
}

static void handleWifiScanStart() {
  NEED_AUTH();
  if (g_updBusy) { sendErr(409, "Während eines Updates ist kein Scan möglich."); return; }
  if (!wifiScanStart(false, 0, NULL)) { sendErr(500, "Scan konnte nicht gestartet werden"); return; }
  sendOk("");
}

static void handleWifiScanStop() {
  NEED_AUTH();
  wifiScanStop();
  sendOk("");
}

static void handleTrackGet() {
  NEED_AUTH();
  String o = "{";
  jB(o, "running", g_scanState == 1 && g_scanIsTrack);
  jB(o, "found", g_trackFound);
  jI(o, "rssi", g_trackRssi);
  jI(o, "seq", g_trackSeq);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleTrackStart() {
  NEED_AUTH();
  uint8_t mac[6];
  int ch = g_server.arg("ch").toInt();
  if (!parseMac(g_server.arg("bssid").c_str(), mac) || ch < 1 || ch > 14) { sendErr(400, "Ungültige Angaben"); return; }
  if (g_scanState == 1) { sendErr(409, "Scan läuft bereits"); return; }
  memcpy(g_trackBssid, mac, 6);
  if (!wifiScanStart(true, ch, mac)) { sendErr(500, "Scan konnte nicht gestartet werden"); return; }
  sendOk("");
}

// ---- Eigener Access Point ----

static void handleApGet() {
  NEED_AUTH();
  String o = "{";
  jB(o, "running", g_apUp);
  jS(o, "ssid", g_apCurSsid);
  jS(o, "cfgSsid", g_cfg.apSsid);
  jI(o, "ch", g_apUp ? WiFi.channel() : g_cfg.apCh);
  jI(o, "cfgCh", g_cfg.apCh);
  jS(o, "ip", WiFi.softAPIP().toString().c_str());
  jS(o, "mac", WiFi.softAPmacAddress().c_str());
  jI(o, "clients", DEMO_MODE ? 2 : WiFi.softAPgetStationNum());
  jKey(o, "test");
  o += '{';
  jB(o, "active", g_testActive);
  jI(o, "idx", g_testIdx);
  jI(o, "count", g_testCount);
  jI(o, "interval", g_testInterval);
  jI(o, "left", g_testActive ? (long)(((int32_t)(g_testEndAt - millis())) / 1000) : 0);
  jI(o, "nextIn", (g_testActive && g_testCount > 1) ? (long)(((int32_t)(g_testSwitchAt - millis())) / 1000) : 0);
  jKey(o, "names");
  o += '[';
  for (int i = 0; g_testActive && i < g_testCount; i++) {
    jsonStr(o, g_testNames[i]);
    o += ',';
  }
  jClose(o, ']');
  o += ',';
  jClose(o, '}');
  o += ',';
  jClose(o, '}');
  sendJson(200, o);
}

static void handleApStart() {
  NEED_AUTH();
  g_apWanted = true;
  if (g_testActive) testStop();
  else if (!applyAp(g_cfg.apSsid, g_cfg.apPass, g_cfg.apCh)) { sendErr(500, "Access Point konnte nicht gestartet werden"); return; }
  addLog(0, "Access Point gestartet");
  sendOk("");
}

static void handleApStop() {
  NEED_AUTH();
  g_apWanted = false;
  g_testActive = false;
  addLog(1, "Access Point wird gestoppt");
  sendOk("Der Access Point wird gleich gestoppt. Ein Neustart des ESP32 stellt ihn wieder her.");
  g_apRestartAt = millis() + 700UL;  // verzoegert, damit die Antwort noch ausgeliefert wird
}

static void handleApSave() {
  NEED_AUTH();
  String ssid = g_server.arg("ssid");
  String pass = g_server.arg("pass");
  int ch = g_server.arg("ch").toInt();
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() > 32) { sendErr(400, "SSID muss 1 bis 32 Zeichen lang sein"); return; }
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) { sendErr(400, "Passwort muss 8 bis 63 Zeichen lang sein"); return; }
  if (ch < 1 || ch > 11) { sendErr(400, "Kanal muss zwischen 1 und 11 liegen"); return; }
  strlcpy(g_cfg.apSsid, ssid.c_str(), sizeof(g_cfg.apSsid));
  if (pass.length()) strlcpy(g_cfg.apPass, pass.c_str(), sizeof(g_cfg.apPass));
  g_cfg.apCh = (uint8_t)ch;
  saveApCfg();
  addLog(0, "Einstellungen gespeichert (Access Point)");
  g_apWanted = true;
  g_testActive = false;
  g_apRestartAt = millis() + 800UL;  // Neustart des AP nach der Antwort
  sendOk("Gespeichert. Das WLAN wird neu gestartet - bitte neu verbinden.");
}

static void handleClientsGet() {
  NEED_AUTH();
  ClientInfo cl[MAX_BLOCKED + ESP_WIFI_MAX_CONN_NUM];
  int n = getClients(cl, MAX_BLOCKED + ESP_WIFI_MAX_CONN_NUM);
  String o = "{\"clients\":[";
  for (int i = 0; i < n; i++) {
    char m[18];
    macToStr(cl[i].mac, m);
    o += '{';
    jS(o, "mac", m);
    jS(o, "ip", cl[i].ip);
    jI(o, "rssi", cl[i].rssi);
    jB(o, "connected", cl[i].connected);
    jB(o, "blocked", cl[i].blocked);
    jS(o, "status", cl[i].blocked ? "gesperrt" : "verbunden");
    jClose(o, '}');
    o += ',';
  }
  jClose(o, ']');
  jClose(o, '}');
  sendJson(200, o);
}

static void handleBlock() {
  NEED_AUTH();
  uint8_t mac[6];
  if (!parseMac(g_server.arg("mac").c_str(), mac)) { sendErr(400, "Ungültige MAC-Adresse"); return; }
  uint8_t own[6];
  esp_read_mac(own, ESP_MAC_WIFI_SOFTAP);
  if (memcmp(own, mac, 6) == 0) { sendErr(400, "Nicht möglich"); return; }
  if (!isBlocked(mac)) {
    if (g_blockedN >= MAX_BLOCKED) { sendErr(400, "Sperrliste ist voll"); return; }
    memcpy(g_blocked[g_blockedN++], mac, 6);
    saveBlocked();
    char m[18];
    macToStr(mac, m);
    addLog(0, "Gerät gesperrt: %s", m);
  }
  g_lastEnforce = 0;
  sendOk("");
}

static void handleUnblock() {
  NEED_AUTH();
  uint8_t mac[6];
  if (!parseMac(g_server.arg("mac").c_str(), mac)) { sendErr(400, "Ungültige MAC-Adresse"); return; }
  for (int i = 0; i < g_blockedN; i++) {
    if (memcmp(g_blocked[i], mac, 6) == 0) {
      for (int j = i; j < g_blockedN - 1; j++) memcpy(g_blocked[j], g_blocked[j + 1], 6);
      g_blockedN--;
      saveBlocked();
      char m[18];
      macToStr(mac, m);
      addLog(0, "Gerät entsperrt: %s", m);
      break;
    }
  }
  sendOk("");
}

static void handleTestStart() {
  NEED_AUTH();
  int count = g_server.arg("count").toInt();
  int interval = g_server.arg("interval").toInt();
  if (count != 1 && count != 2 && count != 3 && count != 5) { sendErr(400, "Anzahl muss 1, 2, 3 oder 5 sein"); return; }
  testStart(count, interval ? interval : 30);
  sendOk("");
}

static void handleTestStop() {
  NEED_AUTH();
  testStop();
  sendOk("");
}

// ---- BLE ----

static void handleBleGet() {
  NEED_AUTH();
  String o;
  o.reserve(400 + g_bleN * 260);
  o = "{";
  jS(o, "state", g_bleState == 1 ? "scanning" : (g_bleState == 2 ? "error" : "ready"));
  jI(o, "dur", g_bleDur);
  jI(o, "left", g_bleState == 1 ? (long)(((int32_t)(g_bleEndMs - millis())) / 1000) : 0);
  jKey(o, "devs");
  o += '[';
  if (g_bleMutex && xSemaphoreTake(g_bleMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    for (int i = 0; i < g_bleN; i++) {
      const BleDev &d = g_ble[i];
      o += '{';
      jS(o, "addr", d.addr);
      jS(o, "name", d.name);
      jI(o, "rssi", d.rssi);
      jI(o, "mfr", d.mfr);
      jS(o, "mfrName", d.mfr >= 0 ? mfrName(d.mfr) : "");
      jI(o, "seen", d.seen);
      jKey(o, "svc");
      o += '[';
      for (int k = 0; k < d.nsvc; k++) {
        jsonStr(o, d.svc[k]);
        o += ',';
      }
      jClose(o, ']');
      o += ',';
      jClose(o, '}');
      o += ',';
    }
    xSemaphoreGive(g_bleMutex);
  }
  jClose(o, ']');
  o += ',';
  jI(o, "up", uptimeSec());
  jClose(o, '}');
  sendJson(200, o);
}

static void handleBleStart() {
  NEED_AUTH();
  if (g_updBusy) { sendErr(409, "Während eines Updates ist kein Scan möglich."); return; }
  int secs = g_server.arg("secs").toInt();
  if (!bleScanStart(secs ? secs : 10)) { sendErr(500, "BLE-Scan konnte nicht gestartet werden"); return; }
  sendOk("");
}

static void handleBleStop() {
  NEED_AUTH();
  bleScanStop();
  sendOk("");
}

static void handleBleClear() {
  NEED_AUTH();
  bleClear();
  sendOk("");
}

// ---- Logs ----

static void handleLogsGet() {
  NEED_AUTH();
  String o;
  o.reserve(400 + g_logCount * 130);
  o = "{\"logs\":[";
  portENTER_CRITICAL(&g_logMux);
  uint16_t cnt = g_logCount;
  uint16_t start = (g_logHead + MAX_LOG - cnt) % MAX_LOG;
  static LogEntry copy[MAX_LOG];
  for (uint16_t i = 0; i < cnt; i++) copy[i] = g_logs[(start + i) % MAX_LOG];
  portEXIT_CRITICAL(&g_logMux);
  for (uint16_t i = 0; i < cnt; i++) {
    o += '{';
    jI(o, "t", copy[i].t);
    jI(o, "l", copy[i].lvl);
    jS(o, "m", copy[i].msg);
    jClose(o, '}');
    o += ',';
  }
  jClose(o, ']');
  jClose(o, '}');
  sendJson(200, o);
}

static void handleLogsClear() {
  NEED_AUTH();
  portENTER_CRITICAL(&g_logMux);
  g_logCount = 0;
  g_logHead = 0;
  portEXIT_CRITICAL(&g_logMux);
  addLog(0, "Logs gelöscht");
  sendOk("");
}

// ---- Einstellungen ----

static void handleSettingsGet() {
  NEED_AUTH();
  String o = "{";
  jS(o, "apSsid", g_cfg.apSsid);
  jI(o, "apCh", g_cfg.apCh);
  jS(o, "webUser", g_cfg.webUser);
  jB(o, "authEnabled", g_cfg.authEnabled);
  jS(o, "staSsid", g_cfg.staSsid);
  jB(o, "hasStaPass", g_cfg.staPass[0] != 0);
  appendSta(o);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleSettingsWeb() {
  NEED_AUTH();
  String user = g_server.arg("user");
  String oldp = g_server.arg("oldpass");
  String newp = g_server.arg("newpass");
  bool auth = g_server.arg("auth") == "1";
  user.trim();
  if (user.length() < 1 || user.length() > 32) { sendErr(400, "Benutzername muss 1 bis 32 Zeichen lang sein"); return; }
  if (g_cfg.authEnabled && !verifyPassword(oldp.c_str())) { sendErr(403, "Aktuelles Passwort ist falsch"); return; }
  if (newp.length() && (newp.length() < 6 || newp.length() > 64)) { sendErr(400, "Neues Passwort muss 6 bis 64 Zeichen lang sein"); return; }
  strlcpy(g_cfg.webUser, user.c_str(), sizeof(g_cfg.webUser));
  g_cfg.authEnabled = auth;
  if (newp.length()) {
    randomHex(g_cfg.webSalt, 8);
    hashPw(g_cfg.webSalt, newp.c_str(), g_cfg.webHash);
    g_cfg.pwDefault = false;
    String t = g_server.header("X-Token");
    if (t.length() != 32) t = g_server.arg("t");
    dropSessionsExcept(t.c_str());
  }
  saveWebCreds();
  addLog(0, "Einstellungen gespeichert (Webinterface)");
  sendOk("Gespeichert.");
}

static void handleStaConnect() {
  NEED_AUTH();
  String ssid = g_server.arg("ssid");
  String pass = g_server.arg("pass");
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() > 32) { sendErr(400, "SSID muss 1 bis 32 Zeichen lang sein"); return; }
  if (pass.length() > 63) { sendErr(400, "Passwort zu lang"); return; }
  strlcpy(g_cfg.staSsid, ssid.c_str(), sizeof(g_cfg.staSsid));
  if (g_server.arg("keep") != "1") strlcpy(g_cfg.staPass, pass.c_str(), sizeof(g_cfg.staPass));
  saveStaCfg();
  addLog(0, "Einstellungen gespeichert (Heim-WLAN)");
  staConnect();
  sendOk("");
}

static void handleStaDisconnect() {
  NEED_AUTH();
  staDisconnect();
  g_cfg.staSsid[0] = 0;
  g_cfg.staPass[0] = 0;
  saveStaCfg();
  addLog(0, "Heim-WLAN getrennt und vergessen");
  sendOk("");
}

static void handleNoticeDismiss() {
  NEED_AUTH();
  g_notice[0] = 0;
  g_updPrefs.remove("notice");
  sendOk("");
}

static void handleRestart() {
  NEED_AUTH();
  addLog(0, "Neustart angefordert");
  sendOk("");
  g_restartAt = millis() + 800UL;
}

static void handleFactory() {
  NEED_AUTH();
  if (g_server.arg("confirm") != "RESET") { sendErr(400, "Bestätigung fehlt"); return; }
  g_prefs.clear();
  g_updPrefs.clear();
  addLog(1, "Werkseinstellungen wiederhergestellt");
  sendOk("");
  g_restartAt = millis() + 1000UL;
}

// ---- Firmware-Update ----

static void handleUpdateGet() {
  NEED_AUTH();
  UpdInfo u;
  char msg[128];
  uint8_t phase, pct;
  portENTER_CRITICAL(&g_updMux);
  u = g_upd;
  phase = g_updPhase;
  pct = g_updPct;
  strlcpy(msg, g_updMsg, sizeof(msg));
  portEXIT_CRITICAL(&g_updMux);

  static const char *phaseNames[] = {"idle", "checking", "downloading", "installing", "done", "error"};
  char expected[80];
  updExpectedAsset("X.Y.Z", expected, sizeof(expected));

  String o;
  o.reserve(1600);
  o = "{";
  jS(o, "installed", FIRMWARE_VERSION);
  jS(o, "latest", u.checked ? u.latest : "");
  jB(o, "checked", u.checked);
  jB(o, "available", u.available);
  jB(o, "canInstall", u.canInstall && !g_updBusy);
  jB(o, "hasSha", u.hasSha);
  jS(o, "date", u.date);
  jS(o, "notes", u.notes);
  jS(o, "lastCheck", u.lastCheck);
  jS(o, "github", u.github[0] ? u.github : "Noch nicht geprüft");
  jI(o, "internet", u.internet);
  jB(o, "staUp", g_staState == STA_CONNECTED);
  jS(o, "phase", phaseNames[phase < 6 ? phase : 0]);
  jI(o, "pct", pct);
  jS(o, "msg", msg);
  jB(o, "busy", g_updBusy);
  jB(o, "configured", updConfigured() || DEMO_MODE);
  jB(o, "supported", (bool)HAVE_TLS || DEMO_MODE);
  jS(o, "repo", updConfigured() ? GITHUB_USER "/" GITHUB_REPO : "(nicht eingetragen)");
  jS(o, "assetPattern", expected);
  jB(o, "requireSha", REQUIRE_SHA256);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleUpdateCheck() {
  NEED_AUTH();
  if (!updStart(1)) { sendErr(409, "Bitte warten - es läuft bereits eine andere Aktion."); return; }
  sendOk("");
}

static void handleUpdateInstall() {
  NEED_AUTH();
  if (!updStart(2)) { sendErr(409, "Bitte warten - es läuft bereits eine andere Aktion."); return; }
  sendOk("");
}

// Manueller Firmware-Upload (.bin aus dem Browser) - nur mit Login
static void handleUploadDone() {
  bool authFail = g_uploadAuthFail;
  bool ok = g_uploadOk;
  char err[64];
  strlcpy(err, g_uploadErr, sizeof(err));
  g_uploadAuthFail = false;  // fuer den naechsten Upload zuruecksetzen
  g_uploadOk = false;
  g_uploadErr[0] = 0;
  if (authFail) { sendErr(401, "Nicht angemeldet"); return; }
  if (!ok) { sendErr(400, err[0] ? err : "Upload fehlgeschlagen"); return; }
  sendOk("Firmware installiert - ESP32 startet neu.");
  g_restartAt = millis() + 2000UL;
}

static void handleUploadData() {
  HTTPUpload &up = g_server.upload();
  if (up.status == UPLOAD_FILE_START) {
    g_uploadOk = false;
    g_uploadAuthFail = false;
    g_uploadErr[0] = 0;
    if (!checkAuth()) { g_uploadAuthFail = true; return; }
    if (g_updBusy) { strlcpy(g_uploadErr, "Es läuft bereits ein Update", sizeof(g_uploadErr)); return; }
    addLog(0, "Manuelles Firmware-Update gestartet");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) strlcpy(g_uploadErr, "Update-Speicher nicht verfügbar", sizeof(g_uploadErr));
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (g_uploadAuthFail || g_uploadErr[0] || !Update.isRunning()) return;
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      strlcpy(g_uploadErr, "Schreibfehler (Datei zu groß oder ungültig)", sizeof(g_uploadErr));
      Update.abort();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (g_uploadAuthFail || g_uploadErr[0] || !Update.isRunning()) return;
    if (Update.end(true)) {
      g_uploadOk = true;
      g_updPrefs.putBool("pend", true);
      g_updPrefs.putUChar("boots", 0);
      g_updPrefs.putString("target", "manuell");
      addLog(0, "Manuelles Firmware-Update installiert");
    } else {
      strlcpy(g_uploadErr, "Ungültige Firmware-Datei", sizeof(g_uploadErr));
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (Update.isRunning()) Update.abort();
  }
}

// ---- Captive Portal ----

static void redirectToRoot() {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.sendHeader("Location", String("http://") + (g_apUp ? WiFi.softAPIP().toString() : WiFi.localIP().toString()) + "/", true);
  g_server.send(302, "text/plain", "");
}

static void handleRoot() {
  g_server.sendHeader("Cache-Control", "no-cache");
  g_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

static void handleNotFound() {
  String uri = g_server.uri();
  if (uri.startsWith("/api/")) { sendErr(404, "Unbekannter Endpunkt"); return; }
  String host = g_server.hostHeader();
  bool domainHost = false;
  for (size_t i = 0; i < host.length(); i++)
    if (isalpha((unsigned char)host[i])) domainHost = true;
  if (domainHost && !host.startsWith(MDNS_NAME)) { redirectToRoot(); return; }
  g_server.send(404, "text/plain", "Nicht gefunden");
}

static void setupRoutes() {
  static const char *hdrs[] = {"X-Token"};
  g_server.collectHeaders(hdrs, 1);

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/index.html", HTTP_GET, handleRoot);
  g_server.on("/favicon.ico", HTTP_GET, []() { g_server.send(204, "text/plain", ""); });

  // Captive-Portal-Erkennung von Android, iOS, Windows, Firefox
  static const char *cp[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
                             "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt", "/fwlink"};
  for (size_t i = 0; i < sizeof(cp) / sizeof(cp[0]); i++) g_server.on(cp[i], HTTP_ANY, redirectToRoot);

  g_server.on("/api/public", HTTP_GET, handlePublic);
  g_server.on("/api/login", HTTP_POST, handleLogin);
  g_server.on("/api/logout", HTTP_POST, handleLogout);
  g_server.on("/api/status", HTTP_GET, handleStatus);

  g_server.on("/api/wifi/scan", HTTP_GET, handleWifiScanGet);
  g_server.on("/api/wifi/scan/start", HTTP_POST, handleWifiScanStart);
  g_server.on("/api/wifi/scan/stop", HTTP_POST, handleWifiScanStop);
  g_server.on("/api/wifi/track", HTTP_GET, handleTrackGet);
  g_server.on("/api/wifi/track", HTTP_POST, handleTrackStart);

  g_server.on("/api/ap", HTTP_GET, handleApGet);
  g_server.on("/api/ap/start", HTTP_POST, handleApStart);
  g_server.on("/api/ap/stop", HTTP_POST, handleApStop);
  g_server.on("/api/ap/save", HTTP_POST, handleApSave);
  g_server.on("/api/ap/clients", HTTP_GET, handleClientsGet);
  g_server.on("/api/ap/block", HTTP_POST, handleBlock);
  g_server.on("/api/ap/unblock", HTTP_POST, handleUnblock);
  g_server.on("/api/test/start", HTTP_POST, handleTestStart);
  g_server.on("/api/test/stop", HTTP_POST, handleTestStop);

  g_server.on("/api/ble", HTTP_GET, handleBleGet);
  g_server.on("/api/ble/start", HTTP_POST, handleBleStart);
  g_server.on("/api/ble/stop", HTTP_POST, handleBleStop);
  g_server.on("/api/ble/clear", HTTP_POST, handleBleClear);

  g_server.on("/api/logs", HTTP_GET, handleLogsGet);
  g_server.on("/api/logs/clear", HTTP_POST, handleLogsClear);

  g_server.on("/api/settings", HTTP_GET, handleSettingsGet);
  g_server.on("/api/settings/web", HTTP_POST, handleSettingsWeb);
  g_server.on("/api/sta/connect", HTTP_POST, handleStaConnect);
  g_server.on("/api/sta/disconnect", HTTP_POST, handleStaDisconnect);
  g_server.on("/api/notice/dismiss", HTTP_POST, handleNoticeDismiss);
  g_server.on("/api/system/restart", HTTP_POST, handleRestart);
  g_server.on("/api/system/factory", HTTP_POST, handleFactory);

  g_server.on("/api/update", HTTP_GET, handleUpdateGet);
  g_server.on("/api/update/check", HTTP_POST, handleUpdateCheck);
  g_server.on("/api/update/install", HTTP_POST, handleUpdateInstall);
  g_server.on("/api/update/upload", HTTP_POST, handleUploadDone, handleUploadData);

  g_server.onNotFound(handleNotFound);
}

// ============================================================================
//  14) SETUP UND LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // nicht blockieren, wenn kein PC am USB haengt
#endif
  delay(200);
  g_bleMutex = xSemaphoreCreateMutex();
  g_upd.internet = -1;
  addLog(0, "System gestartet (v%s)", FIRMWARE_VERSION);

  loadConfig();
  bootGuard();

  WiFi.persistent(false);  // WLAN-Zugangsdaten verwalten wir selbst (Preferences)
  WiFi.setHostname(MDNS_NAME);
  WiFi.onEvent(onWifiEvent);

  if (SIMULATOR_WIFI_SSID[0]) {
    // Simulator-Betrieb: nur Station, kein Access Point
    WiFi.mode(WIFI_STA);
    g_apWanted = false;
    strlcpy(g_cfg.staSsid, SIMULATOR_WIFI_SSID, sizeof(g_cfg.staSsid));
    strlcpy(g_cfg.staPass, SIMULATOR_WIFI_PASS, sizeof(g_cfg.staPass));
    addLog(1, "SIMULATOR-MODUS: verbinde mit '%s' (kein Access Point)", SIMULATOR_WIFI_SSID);
    staConnect();
  } else {
    WiFi.mode(WIFI_AP_STA);
    if (applyAp(g_cfg.apSsid, g_cfg.apPass, g_cfg.apCh)) {
      addLog(0, "Access Point gestartet: %s", g_cfg.apSsid);
    } else {
      addLog(2, "Access Point konnte nicht gestartet werden");
    }
    if (g_cfg.staSsid[0]) staConnect();
  }

  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  setupRoutes();
  g_server.begin();
  addLog(0, "Webserver bereit (Access Point: http://192.168.4.1)");
  if (DEMO_MODE) addLog(1, "DEMO-MODUS aktiv: WLAN/BLE/Update-Daten sind simuliert");
  if (strcmp(g_cfg.apPass, DEFAULT_AP_PASS) == 0) addLog(1, "Standard-WLAN-Passwort in Benutzung - bitte ändern");
}

void loop() {
  g_server.handleClient();
  g_dns.processNextRequest();
  tickWifiScan();
  tickSta();
  tickTest();
  tickBootConfirm();

  if ((uint32_t)(millis() - g_lastEnforce) > 1000UL) {
    g_lastEnforce = millis();
    enforceBlocks();
  }

  if (g_apRestartAt && (int32_t)(millis() - g_apRestartAt) >= 0) {
    g_apRestartAt = 0;
    if (g_apWanted) {
      if (applyAp(g_cfg.apSsid, g_cfg.apPass, g_cfg.apCh)) addLog(0, "Access Point gestartet: %s", g_cfg.apSsid);
    } else {
      stopAp();
      addLog(0, "Access Point gestoppt");
    }
  }

  if (g_restartAt && (int32_t)(millis() - g_restartAt) >= 0) {
    g_restartAt = 0;
    delay(100);
    ESP.restart();
  }
  delay(2);
}

// ============================================================================
//  15) WEBOBERFLAECHE (HTML, CSS, JavaScript) - liegt im Flash des ESP32
// ============================================================================

const char INDEX_HTML[] PROGMEM =
"<!DOCTYPE html>\n"
"<html lang=\"de\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n"
"<meta name=\"theme-color\" content=\"#0b1220\">\n"
"<title>ESP32 Network Toolbox</title>\n"
"<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Ccircle cx='12' cy='12' r='11' fill='%234f8cff'/%3E%3Cpath d='M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0' stroke='white' stroke-width='2' fill='none' stroke-linecap='round'/%3E%3Ccircle cx='12' cy='19' r='1.4' fill='white'/%3E%3C/svg%3E\">\n"
"<style>\n"
":root{--bg:#0b1220;--bg2:#0f1729;--card:#141d31;--card2:#1b2740;--line:#25324d;--tx:#e8eefb;--mut:#8fa0c0;--acc:#4f8cff;--ok:#2fd07f;--warn:#f5b942;--err:#ff5d6c;--r:16px}\n"
"*{box-sizing:border-box}\n"
"html,body{margin:0;background:var(--bg);color:var(--tx);font:15px/1.45 system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;-webkit-text-size-adjust:100%}\n"
"body{min-height:100vh}\n"
"h1,h2,h3{margin:0;font-weight:650}\n"
"h2{font-size:17px}\n"
"h3{font-size:15px;color:var(--mut);font-weight:600}\n"
"a{color:var(--acc)}\n"
".ic{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;flex:none;vertical-align:middle}\n"
".sprite{position:absolute;width:0;height:0;overflow:hidden}\n"
".app{display:grid;grid-template-columns:232px minmax(0,1fr);min-height:100vh}\n"
".side{position:sticky;top:0;height:100vh;padding:18px 12px;background:var(--bg2);border-right:1px solid var(--line);display:flex;flex-direction:column;gap:4px}\n"
".brand{display:flex;align-items:center;gap:10px;padding:6px 10px 18px;font-weight:700;font-size:16px}\n"
".brand i{display:grid;place-items:center;width:34px;height:34px;border-radius:11px;background:linear-gradient(135deg,#4f8cff,#7a5cff);color:#fff}\n"
".nav{display:flex;flex-direction:column;gap:4px}\n"
".nav a{display:flex;align-items:center;gap:12px;padding:11px 14px;border-radius:12px;color:var(--mut);text-decoration:none;cursor:pointer;white-space:nowrap}\n"
".nav a:hover{background:rgba(255,255,255,.04);color:var(--tx)}\n"
".nav a.on{background:rgba(79,140,255,.17);color:#fff}\n"
".foot{margin-top:auto;padding:10px;color:var(--mut);font-size:12px}\n"
"main{padding:22px 26px 40px;min-width:0;max-width:1200px;width:100%}\n"
".top{display:flex;align-items:center;gap:12px;margin-bottom:18px}\n"
".top h1{font-size:22px;flex:1}\n"
".dot{width:10px;height:10px;border-radius:50%;background:var(--ok);display:inline-block;flex:none}\n"
".dot.off{background:var(--err)}\n"
".dot.warn{background:var(--warn)}\n"
".dot.busy{background:var(--acc)}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:var(--r);padding:18px;min-width:0}\n"
".card+.card,.grid+.card,.card+.grid,.banner+.card{margin-top:14px}\n"
".grid{display:grid;gap:14px}\n"
".grid>.card{margin-top:0!important}\n"
".g4{grid-template-columns:repeat(auto-fit,minmax(150px,1fr))}\n"
".g2{grid-template-columns:repeat(auto-fit,minmax(300px,1fr))}\n"
".stat .lbl{color:var(--mut);font-size:13px;display:flex;align-items:center;gap:8px}\n"
".stat .val{font-size:26px;font-weight:700;margin:6px 0 2px;display:flex;align-items:center;gap:8px}\n"
".stat .sub{color:var(--mut);font-size:12.5px;min-height:18px}\n"
".meter{height:8px;border-radius:9px;background:#0e1628;overflow:hidden;margin-top:10px}\n"
".meter i{display:block;height:100%;border-radius:9px;background:var(--acc);transition:width .4s}\n"
".meter i.mid{background:var(--warn)}.meter i.hi{background:var(--err)}\n"
".bar{display:flex;flex-wrap:wrap;gap:10px;align-items:center}\n"
".btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;border:1px solid var(--line);background:var(--card2);color:var(--tx);padding:10px 16px;border-radius:12px;font:inherit;font-weight:550;cursor:pointer;touch-action:manipulation}\n"
".btn:hover{border-color:#3a4c73}\n"
".btn.pri{background:var(--acc);border-color:var(--acc);color:#fff}\n"
".btn.dng{background:rgba(255,93,108,.13);border-color:rgba(255,93,108,.4);color:#ff9aa4}\n"
".btn.sm{padding:6px 11px;font-size:13px;border-radius:10px}\n"
".btn:disabled{opacity:.45;cursor:not-allowed}\n"
"input,select{background:#0e1628;border:1px solid var(--line);color:var(--tx);border-radius:10px;padding:10px 12px;font:inherit;min-width:0;max-width:100%}\n"
"input:focus,select:focus{outline:2px solid rgba(79,140,255,.5);border-color:var(--acc)}\n"
"input[type=checkbox]{width:18px;height:18px;padding:0;accent-color:var(--acc)}\n"
"label{display:block;color:var(--mut);font-size:13px;margin:12px 0 5px}\n"
".filters{display:flex;flex-wrap:wrap;gap:10px;margin-top:14px}\n"
".srch{position:relative;flex:1 1 200px}\n"
".srch .ic{position:absolute;left:11px;top:11px;color:var(--mut);width:18px;height:18px}\n"
".srch input{width:100%;padding-left:36px}\n"
".scroll{overflow-x:auto;padding:6px 6px}\n"
".tbl{width:100%;border-collapse:collapse;min-width:460px}\n"
".tbl th{color:var(--mut);font-weight:600;font-size:12.5px;text-align:left;padding:10px 12px;border-bottom:1px solid var(--line);white-space:nowrap}\n"
".tbl td{padding:11px 12px;border-bottom:1px solid rgba(37,50,77,.6);vertical-align:middle}\n"
".tbl tr:last-child td{border-bottom:0}\n"
".mut{color:var(--mut)}\n"
".mono{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:13px}\n"
".sig{display:flex;align-items:center;gap:8px;min-width:120px}\n"
".sig b{font-weight:600;min-width:56px}\n"
".sig span{flex:1;height:7px;background:#0e1628;border-radius:9px;overflow:hidden;min-width:40px}\n"
".sig span i{display:block;height:100%;border-radius:9px}\n"
".tag{display:inline-block;padding:2px 9px;border-radius:99px;font-size:12px;font-weight:600;background:rgba(143,160,192,.15);color:var(--mut);white-space:nowrap}\n"
".tag.ok{background:rgba(47,208,127,.15);color:var(--ok)}\n"
".tag.warn{background:rgba(245,185,66,.15);color:var(--warn)}\n"
".tag.err{background:rgba(255,93,108,.15);color:var(--err)}\n"
".tag.acc{background:rgba(79,140,255,.16);color:#8fb4ff}\n"
".banner{display:flex;gap:12px;align-items:center;flex-wrap:wrap;padding:12px 16px;border-radius:14px;margin-bottom:12px;border:1px solid;font-size:14px}\n"
".banner.warn{background:rgba(245,185,66,.1);border-color:rgba(245,185,66,.4);color:#ffd88a}\n"
".banner.ok{background:rgba(47,208,127,.1);border-color:rgba(47,208,127,.4);color:#8ff0bd}\n"
".banner.info{background:rgba(79,140,255,.1);border-color:rgba(79,140,255,.4);color:#a9c5ff}\n"
".banner span{flex:1;min-width:200px}\n"
".chart{display:flex;align-items:flex-end;gap:6px;height:190px;padding:6px 0 0}\n"
".chart .col{flex:1;display:flex;flex-direction:column;align-items:center;justify-content:flex-end;height:100%;min-width:0}\n"
".chart .col b{font-size:12px;color:var(--mut);margin-bottom:4px;font-weight:600}\n"
".chart .col .bx{width:100%;max-width:38px;border-radius:8px 8px 3px 3px;background:linear-gradient(180deg,#6aa0ff,#3563d4);min-height:3px;transition:height .4s}\n"
".chart .col .bx.hot{background:linear-gradient(180deg,#ffcf6b,#e0902a)}\n"
".chart .col .bx.zero{background:#1d2a45}\n"
".chart .col span{font-size:12px;margin-top:6px;color:var(--mut)}\n"
".chart .col.rec span{color:var(--ok);font-weight:700}\n"
"canvas{width:100%;height:260px;display:block;background:#0e1628;border-radius:12px}\n"
".kv{display:grid;grid-template-columns:minmax(120px,auto) 1fr;gap:9px 16px}\n"
".kv div:nth-child(odd){color:var(--mut)}\n"
".kv div:nth-child(even){word-break:break-word}\n"
".prog{height:14px;border-radius:99px;background:#0e1628;overflow:hidden;margin:12px 0 6px}\n"
".prog i{display:block;height:100%;width:0;background:linear-gradient(90deg,#4f8cff,#7aa8ff);border-radius:99px;transition:width .35s}\n"
".prog.busy i{background:repeating-linear-gradient(45deg,#4f8cff 0 12px,#6b9dff 12px 24px);background-size:34px 34px;animation:mv 1s linear infinite}\n"
"@keyframes mv{to{background-position:34px 0}}\n"
"pre.notes{white-space:pre-wrap;word-break:break-word;background:#0e1628;border-radius:12px;padding:12px 14px;margin:8px 0 0;font:13px/1.5 ui-monospace,Menlo,Consolas,monospace;max-height:220px;overflow:auto}\n"
".log{font:13px/1.55 ui-monospace,Menlo,Consolas,monospace;max-height:70vh;overflow:auto}\n"
".log div{padding:3px 0;border-bottom:1px solid rgba(37,50,77,.5);display:flex;gap:12px}\n"
".log .t{color:var(--mut);flex:none}\n"
".log .l1{color:var(--warn)}.log .l2{color:var(--err)}\n"
".hint{color:var(--mut);font-size:13px;margin-top:10px}\n"
".sec{display:flex;align-items:center;gap:10px;margin-bottom:12px}\n"
".sec .ic{color:var(--acc)}\n"
".sub-nav{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:14px}\n"
".sub-nav a{padding:7px 14px;border-radius:99px;background:var(--card);border:1px solid var(--line);color:var(--mut);text-decoration:none;font-size:13.5px;cursor:pointer}\n"
".sub-nav a:hover{color:var(--tx)}\n"
".ovl{position:fixed;inset:0;background:rgba(5,9,18,.82);display:grid;place-items:center;z-index:50;padding:16px;backdrop-filter:blur(6px)}\n"
".ovl .card{width:100%;max-width:380px}\n"
".toast{position:fixed;left:50%;bottom:90px;transform:translateX(-50%);background:#1e2c49;border:1px solid var(--line);padding:11px 18px;border-radius:12px;z-index:60;max-width:92vw;box-shadow:0 8px 30px rgba(0,0,0,.4)}\n"
".toast.err{border-color:var(--err);color:#ffb0b8}.toast.ok{border-color:var(--ok)}\n"
".empty{text-align:center;color:var(--mut);padding:26px 10px}\n"
".pill-row{display:flex;flex-wrap:wrap;gap:8px}\n"
".act{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}\n"
".act .btn{justify-content:flex-start;text-align:left;padding:13px 16px}\n"
"@media(max-width:860px){\n"
"  .app{grid-template-columns:minmax(0,1fr)}\n"
"  .side{position:fixed;top:auto;bottom:0;left:0;right:0;height:auto;flex-direction:row;overflow-x:auto;padding:6px 8px calc(6px + env(safe-area-inset-bottom));border-right:0;border-top:1px solid var(--line);z-index:20;gap:2px}\n"
"  .brand,.foot{display:none}\n"
"  .nav{flex-direction:row;gap:2px}\n"
"  .nav a{flex-direction:column;gap:3px;font-size:11px;padding:7px 11px;min-width:66px;text-align:center}\n"
"  main{padding:16px 14px 96px}\n"
"  .hm{display:none}\n"
"  .toast{bottom:100px}\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<svg class=\"sprite\" aria-hidden=\"true\"><defs>\n"
"<symbol id=\"i-home\" viewBox=\"0 0 24 24\"><path d=\"M3 11l9-8 9 8v10a1 1 0 0 1-1 1h-5v-7H9v7H4a1 1 0 0 1-1-1z\"/></symbol>\n"
"<symbol id=\"i-wifi\" viewBox=\"0 0 24 24\"><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"/><path d=\"M1.42 9a16 16 0 0 1 21.16 0\"/><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"/><path d=\"M12 20h.01\"/></symbol>\n"
"<symbol id=\"i-bars\" viewBox=\"0 0 24 24\"><path d=\"M12 20V10M18 20V4M6 20v-4\"/></symbol>\n"
"<symbol id=\"i-activity\" viewBox=\"0 0 24 24\"><path d=\"M22 12h-4l-3 9L9 3l-3 9H2\"/></symbol>\n"
"<symbol id=\"i-radio\" viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"2\"/><path d=\"M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14\"/></symbol>\n"
"<symbol id=\"i-bt\" viewBox=\"0 0 24 24\"><path d=\"M6.5 6.5l11 11L12 23V1l5.5 5.5-11 11\"/></symbol>\n"
"<symbol id=\"i-list\" viewBox=\"0 0 24 24\"><path d=\"M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01\"/></symbol>\n"
"<symbol id=\"i-gear\" viewBox=\"0 0 24 24\"><path d=\"M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6\"/></symbol>\n"
"<symbol id=\"i-power\" viewBox=\"0 0 24 24\"><path d=\"M18.36 6.64a9 9 0 1 1-12.73 0M12 2v10\"/></symbol>\n"
"<symbol id=\"i-refresh\" viewBox=\"0 0 24 24\"><path d=\"M23 4v6h-6\"/><path d=\"M20.49 15a9 9 0 1 1-2.12-9.36L23 10\"/></symbol>\n"
"<symbol id=\"i-download\" viewBox=\"0 0 24 24\"><path d=\"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4\"/><path d=\"M7 10l5 5 5-5M12 15V3\"/></symbol>\n"
"<symbol id=\"i-search\" viewBox=\"0 0 24 24\"><circle cx=\"11\" cy=\"11\" r=\"7\"/><path d=\"M21 21l-4.35-4.35\"/></symbol>\n"
"<symbol id=\"i-play\" viewBox=\"0 0 24 24\"><path d=\"M6 4l14 8-14 8z\"/></symbol>\n"
"<symbol id=\"i-stop\" viewBox=\"0 0 24 24\"><rect x=\"5\" y=\"5\" width=\"14\" height=\"14\" rx=\"2\"/></symbol>\n"
"<symbol id=\"i-trash\" viewBox=\"0 0 24 24\"><path d=\"M3 6h18M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6M10 11v6M14 11v6M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2\"/></symbol>\n"
"<symbol id=\"i-lock\" viewBox=\"0 0 24 24\"><rect x=\"4\" y=\"11\" width=\"16\" height=\"10\" rx=\"2\"/><path d=\"M8 11V7a4 4 0 0 1 8 0v4\"/></symbol>\n"
"<symbol id=\"i-unlock\" viewBox=\"0 0 24 24\"><rect x=\"4\" y=\"11\" width=\"16\" height=\"10\" rx=\"2\"/><path d=\"M8 11V7a4 4 0 0 1 7.5-2\"/></symbol>\n"
"<symbol id=\"i-out\" viewBox=\"0 0 24 24\"><path d=\"M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4M16 17l5-5-5-5M21 12H9\"/></symbol>\n"
"<symbol id=\"i-flask\" viewBox=\"0 0 24 24\"><path d=\"M9 3h6M10 3v6L4.5 19a2 2 0 0 0 1.8 3h11.4a2 2 0 0 0 1.8-3L14 9V3\"/></symbol>\n"
"<symbol id=\"i-users\" viewBox=\"0 0 24 24\"><path d=\"M17 21v-2a4 4 0 0 0-4-4H5a4 4 0 0 0-4 4v2\"/><circle cx=\"9\" cy=\"7\" r=\"4\"/><path d=\"M23 21v-2a4 4 0 0 0-3-3.87M16 3.13a4 4 0 0 1 0 7.75\"/></symbol>\n"
"<symbol id=\"i-cpu\" viewBox=\"0 0 24 24\"><rect x=\"5\" y=\"5\" width=\"14\" height=\"14\" rx=\"2\"/><rect x=\"9\" y=\"9\" width=\"6\" height=\"6\"/><path d=\"M9 1v4M15 1v4M9 19v4M15 19v4M1 9h4M1 15h4M19 9h4M19 15h4\"/></symbol>\n"
"<symbol id=\"i-shield\" viewBox=\"0 0 24 24\"><path d=\"M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z\"/></symbol>\n"
"<symbol id=\"i-globe\" viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><path d=\"M2 12h20M12 2a15 15 0 0 1 0 20M12 2a15 15 0 0 0 0 20\"/></symbol>\n"
"<symbol id=\"i-upload\" viewBox=\"0 0 24 24\"><path d=\"M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M17 8l-5-5-5 5M12 3v12\"/></symbol>\n"
"</defs></svg>\n"
"\n"
"<div class=\"app\">\n"
"  <aside class=\"side\">\n"
"    <div class=\"brand\"><i><svg class=\"ic\"><use href=\"#i-wifi\"/></svg></i><span>Network Toolbox</span></div>\n"
"    <nav class=\"nav\" id=\"nav\"></nav>\n"
"    <div class=\"foot\" id=\"foot\">ESP32-S3 N16R8</div>\n"
"  </aside>\n"
"  <main>\n"
"    <div class=\"top\"><h1 id=\"pt\">Dashboard</h1><span id=\"conn\" class=\"dot\" title=\"Verbindung\"></span><button class=\"btn sm\" id=\"btnOut\" title=\"Abmelden\" style=\"display:none\"><svg class=\"ic\"><use href=\"#i-out\"/></svg><span class=\"hm\">Abmelden</span></button></div>\n"
"    <div id=\"banners\"></div>\n"
"    <div id=\"view\"></div>\n"
"  </main>\n"
"</div>\n"
"<div id=\"ovl\"></div>\n"
"\n"
"<script>\n"
"\"use strict\";\n"
"// ---------------------------------------------------------------- Helfer ---\n"
"const $ = (s, r) => (r || document).querySelector(s);\n"
"const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));\n"
"const esc = s => String(s == null ? '' : s).replace(/[&<>\"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));\n"
"const ic = n => '<svg class=\"ic\"><use href=\"#i-' + n + '\"/></svg>';\n"
"const sleep = ms => new Promise(r => setTimeout(r, ms));\n"
"const clamp = (v, a, b) => Math.max(a, Math.min(b, v));\n"
"let TOKEN = '';\n"
"try { TOKEN = sessionStorage.getItem('tok') || ''; } catch (e) {}\n"
"let PUB = {auth: true, fw: '', demo: false};\n"
"let ST = null;\n"
"let timers = [];\n"
"let curPage = '';\n"
"let pending = null;\n"
"let connOk = true;\n"
"\n"
"function setToken(t) {\n"
"  TOKEN = t || '';\n"
"  try { if (TOKEN) sessionStorage.setItem('tok', TOKEN); else sessionStorage.removeItem('tok'); } catch (e) {}\n"
"  $('#btnOut').style.display = (TOKEN && PUB.auth) ? '' : 'none';\n"
"}\n"
"function setConn(ok) {\n"
"  connOk = ok;\n"
"  $('#conn').className = 'dot' + (ok ? '' : ' off');\n"
"  $('#conn').title = ok ? 'Verbunden' : 'Keine Verbindung zum ESP32';\n"
"}\n"
"async function api(path, data) {\n"
"  const opt = {headers: {'X-Token': TOKEN}, cache: 'no-store'};\n"
"  if (data !== undefined) { opt.method = 'POST'; opt.body = new URLSearchParams(data); }\n"
"  // Token zusätzlich als URL-Parameter mitsenden (falls ein Proxy den Header entfernt)\n"
"  const url = TOKEN && path !== '/api/login' ? path + (path.indexOf('?') < 0 ? '?' : '&') + 't=' + encodeURIComponent(TOKEN) : path;\n"
"  let r;\n"
"  try { r = await fetch(url, opt); } catch (e) { setConn(false); throw new Error('Keine Verbindung zum ESP32'); }\n"
"  setConn(true);\n"
"  if (r.status === 401 && path !== '/api/login') { showLogin('Sitzung ungültig oder abgelaufen - bitte neu anmelden.'); throw new Error('Nicht angemeldet'); }\n"
"  let j = {};\n"
"  try { j = await r.json(); } catch (e) {}\n"
"  if (!r.ok || j.ok === false) throw new Error(j.error || ('Fehler ' + r.status));\n"
"  return j;\n"
"}\n"
"let toastT = 0;\n"
"function toast(msg, type) {\n"
"  let t = $('#toast');\n"
"  if (!t) { t = document.createElement('div'); t.id = 'toast'; document.body.appendChild(t); }\n"
"  t.className = 'toast ' + (type || '');\n"
"  t.textContent = msg;\n"
"  t.style.display = '';\n"
"  clearTimeout(toastT);\n"
"  toastT = setTimeout(() => { t.style.display = 'none'; }, type === 'err' ? 5000 : 2800);\n"
"}\n"
"async function act(path, data, okMsg) {\n"
"  try { const r = await api(path, data || {}); if (okMsg !== false) toast(okMsg || r.msg || 'OK', 'ok'); return r; }\n"
"  catch (e) { toast(e.message, 'err'); return null; }\n"
"}\n"
"function clearTimers() { timers.forEach(clearInterval); timers = []; }\n"
"function every(fn, ms) { const w = async () => { try { await fn(); } catch (e) {} }; w(); const t = setInterval(w, ms); timers.push(t); return t; }\n"
"function fmtB(n) {\n"
"  n = Number(n) || 0;\n"
"  if (n >= 1048576) return (n / 1048576).toFixed(n >= 10485760 ? 0 : 1) + ' MB';\n"
"  if (n >= 1024) return (n / 1024).toFixed(0) + ' KB';\n"
"  return n + ' B';\n"
"}\n"
"function fmtUp(s) {\n"
"  s = Number(s) || 0;\n"
"  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60), x = s % 60;\n"
"  const p = v => String(v).padStart(2, '0');\n"
"  return (d ? d + ' T ' : '') + p(h) + ':' + p(m) + ':' + p(x);\n"
"}\n"
"function pctColor(p) { return p >= 90 ? 'hi' : (p >= 70 ? 'mid' : ''); }\n"
"function sigColor(r) { return r >= -60 ? 'var(--ok)' : (r >= -75 ? 'var(--warn)' : 'var(--err)'); }\n"
"function sigPct(r) { return clamp(Math.round((r + 100) * 2), 4, 100); }\n"
"function sigBar(r) {\n"
"  return '<div class=\"sig\"><b>' + r + ' dBm</b><span><i style=\"width:' + sigPct(r) + '%;background:' + sigColor(r) + '\"></i></span></div>';\n"
"}\n"
"let routeId = 'dashboard';\n"
"// Interne Navigation (kein Seitenwechsel per URL: funktioniert auch in Vorschaufenstern/iframes)\n"
"function go(p) { routeId = p; try { history.replaceState(null, '', '#' + p); } catch (e) {} route(); }\n"
"function meter(p) { return '<div class=\"meter\"><i class=\"' + pctColor(p) + '\" style=\"width:' + clamp(p, 0, 100) + '%\"></i></div>'; }\n"
"\n"
"// ----------------------------------------------------------------- Login ---\n"
"function showLogin(msg) {\n"
"  setToken('');\n"
"  clearTimers();\n"
"  if ($('#login')) { if (msg) $('#lerr').textContent = msg; return; }\n"
"  $('#ovl').innerHTML = '<div class=\"ovl\" id=\"login\"><form class=\"card\" id=\"lf\" autocomplete=\"on\"><div class=\"sec\">' + ic('lock') + '<h2>Anmelden</h2></div>' +\n"
"    '<label>Benutzername</label><input id=\"lu\" name=\"username\" autocomplete=\"username\" autocapitalize=\"none\" style=\"width:100%\">' +\n"
"    '<label>Passwort</label><input id=\"lp\" name=\"password\" type=\"password\" autocomplete=\"current-password\" style=\"width:100%\">' +\n"
"    '<div id=\"lerr\" class=\"hint\" style=\"color:var(--err);min-height:20px\">' + esc(msg || '') + '</div>' +\n"
"    '<button class=\"btn pri\" style=\"width:100%;margin-top:6px\" type=\"submit\">Anmelden</button></form></div>';\n"
"  $('#lu').focus();\n"
"  $('#lf').onsubmit = async ev => {\n"
"    ev.preventDefault();\n"
"    try {\n"
"      const r = await api('/api/login', {user: $('#lu').value, pass: $('#lp').value});\n"
"      setToken(r.token);\n"
"      $('#ovl').innerHTML = '';\n"
"      boot();\n"
"    } catch (e) { $('#lerr').textContent = e.message; }\n"
"  };\n"
"}\n"
"\n"
"// --------------------------------------------------- Banner & Statusleiste ---\n"
"function paintBanners() {\n"
"  if (!ST) return;\n"
"  let h = '';\n"
"  if (ST.demo) h += '<div class=\"banner info\">' + ic('flask') + '<span><b>DEMO-Modus:</b> WLAN-, BLE- und Update-Daten sind simuliert.</span></div>';\n"
"  if (ST.notice) h += '<div class=\"banner ok\">' + ic('download') + '<span>' + esc(ST.notice) + '</span><button class=\"btn sm\" id=\"bnDismiss\">OK</button></div>';\n"
"  if (ST.warn && ST.warn.defaultWeb && ST.auth) h += '<div class=\"banner warn\">' + ic('shield') + '<span><b>Sicherheitshinweis:</b> Es wird noch das Standard-Passwort des Webinterfaces verwendet. Bitte jetzt ändern.</span><button class=\"btn sm\" id=\"bnPw\">Passwort ändern</button></div>';\n"
"  if (ST.warn && ST.warn.defaultAp) h += '<div class=\"banner warn\">' + ic('wifi') + '<span><b>Hinweis:</b> Das Standard-WLAN-Passwort ist noch aktiv. Bitte unter Einstellungen ändern.</span><button class=\"btn sm\" id=\"bnAp\">WLAN-Passwort ändern</button></div>';\n"
"  const b = $('#banners');\n"
"  if (b.dataset.h !== h) {\n"
"    b.dataset.h = h;\n"
"    b.innerHTML = h;\n"
"    const d = $('#bnDismiss'); if (d) d.onclick = async () => { await act('/api/notice/dismiss', {}, false); ST.notice = ''; paintBanners(); };\n"
"    const p = $('#bnPw'); if (p) p.onclick = () => { pending = 'web'; go('settings'); };\n"
"    const a = $('#bnAp'); if (a) a.onclick = () => { pending = 'ap'; go('settings'); };\n"
"  }\n"
"}\n"
"async function refreshStatus() {\n"
"  ST = await api('/api/status');\n"
"  paintBanners();\n"
"  if (curPage === 'dashboard') paintDash();\n"
"}\n"
"\n"
"// -------------------------------------------------------- Neustart-Warten ---\n"
"async function waitForDevice(text) {\n"
"  clearTimers();\n"
"  $('#ovl').innerHTML = '<div class=\"ovl\"><div class=\"card\" style=\"text-align:center\">' + ic('refresh') + '<h2 style=\"margin:10px 0 6px\">' + esc(text || 'ESP32 startet neu...') + '</h2><div class=\"hint\">Diese Seite lädt automatisch neu. Falls nicht: WLAN-Verbindung prüfen und die Seite neu laden.</div></div></div>';\n"
"  await sleep(4000);\n"
"  for (let i = 0; i < 90; i++) {\n"
"    try {\n"
"      const r = await fetch('/api/public', {cache: 'no-store'});\n"
"      if (r.ok) { location.reload(); return; }\n"
"    } catch (e) {}\n"
"    await sleep(1500);\n"
"  }\n"
"  $('#ovl').innerHTML = '<div class=\"ovl\"><div class=\"card\"><h2>Keine Verbindung</h2><div class=\"hint\">Verbinde dich wieder mit dem WLAN des ESP32 und lade die Seite neu.</div><button class=\"btn pri\" style=\"margin-top:12px\" onclick=\"location.reload()\">Neu laden</button></div></div>';\n"
"}\n"
"async function doRestart() {\n"
"  if (!confirm('ESP32 jetzt neu starten?')) return;\n"
"  if (await act('/api/system/restart', {}, false)) waitForDevice('ESP32 startet neu...');\n"
"}\n"
"\n"
"// ------------------------------------------------------------- Dashboard ---\n"
"const VIEWS = {};\n"
"function statCard(icon, label, val, sub, extra) {\n"
"  return '<div class=\"card stat\"><div class=\"lbl\">' + ic(icon) + label + '</div><div class=\"val\">' + val + '</div><div class=\"sub\">' + (sub || '') + '</div>' + (extra || '') + '</div>';\n"
"}\n"
"function paintDash() {\n"
"  const el = $('#dashbody');\n"
"  if (!el || !ST) return;\n"
"  const s = ST, w = s.wifi, m = s.mem;\n"
"  const ramP = m.heap ? Math.round((m.heap - m.free) * 100 / m.heap) : 0;\n"
"  const psP = m.psram ? Math.round((m.psram - m.psramFree) * 100 / m.psram) : 0;\n"
"  const flP = m.flash ? Math.round(m.sketch * 100 / m.flash) : 0;\n"
"  let wl, wc;\n"
"  if (w.test) { wl = 'Testmodus'; wc = 'warn'; } else if (w.ap) { wl = 'Aktiv'; wc = ''; } else { wl = 'Gestoppt'; wc = 'off'; }\n"
"  let bl = 'Bereit', bc = '';\n"
"  if (s.ble.state === 'scanning') { bl = 'Scannt...'; bc = 'busy'; } else if (s.ble.state === 'error') { bl = 'Fehler'; bc = 'off'; }\n"
"  let internet = w.sta.state === 'connected' ? 'Heim-WLAN: ' + esc(w.sta.ssid) : (w.sta.ssid ? 'Heim-WLAN: ' + w.sta.state : 'Kein Heim-WLAN (offline)');\n"
"  el.innerHTML =\n"
"    '<div class=\"card\" style=\"display:flex;gap:16px;align-items:center;flex-wrap:wrap\"><div style=\"width:52px;height:52px;border-radius:15px;display:grid;place-items:center;background:linear-gradient(135deg,#4f8cff,#7a5cff)\">' + ic('cpu').replace('class=\"ic\"', 'class=\"ic\" style=\"width:28px;height:28px\"') + '</div>' +\n"
"    '<div style=\"flex:1;min-width:200px\"><h2>' + esc(s.chip.model) + ' &middot; N16R8</h2><div class=\"mut\">' + esc(s.board) + ' &middot; Firmware v' + esc(s.fw) + ' &middot; Laufzeit ' + fmtUp(s.up) + '</div></div>' +\n"
"    '<span class=\"tag acc\">' + esc(s.chip.mhz) + ' MHz &middot; ' + esc(s.chip.cores) + ' Kerne</span></div>' +\n"
"    '<div class=\"grid g4\" style=\"margin-top:14px\">' +\n"
"    statCard('wifi', 'WLAN', '<span class=\"dot ' + wc + '\"></span>' + wl, esc(w.ssid || '-') + ' &middot; Kanal ' + w.ch) +\n"
"    statCard('bt', 'BLE', '<span class=\"dot ' + bc + '\"></span>' + bl, s.ble.found + ' Geräte gefunden') +\n"
"    statCard('cpu', 'RAM', ramP + ' %', fmtB(m.heap - m.free) + ' von ' + fmtB(m.heap), meter(ramP)) +\n"
"    statCard('cpu', 'PSRAM', m.psram ? psP + ' %' : 'n/a', m.psram ? fmtB(m.psram - m.psramFree) + ' von ' + fmtB(m.psram) : 'nicht erkannt (Einstellung \"OPI PSRAM\"?)', m.psram ? meter(psP) : '') +\n"
"    statCard('download', 'Flash', flP + ' %', 'Programm ' + fmtB(m.sketch) + ' von ' + fmtB(m.flash), meter(flP)) +\n"
"    statCard('users', 'Geräte am AP', w.clients, 'verbunden mit eigenem WLAN') +\n"
"    statCard('wifi', 'Gefundene WLANs', w.found, w.scanning ? 'Scan läuft...' : 'aus letztem Scan') +\n"
"    statCard('globe', 'Internet', w.sta.state === 'connected' ? 'Online' : 'Offline', internet) +\n"
"    '</div>' +\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('cpu') + '<h2>Systeminformationen</h2></div><div class=\"kv\">' +\n"
"    '<div>Chip-Modell</div><div>' + esc(s.chip.model) + ' (Revision ' + esc(s.chip.rev) + ')</div>' +\n"
"    '<div>CPU</div><div>' + esc(s.chip.mhz) + ' MHz, ' + esc(s.chip.cores) + ' Kerne</div>' +\n"
"    '<div>Flash</div><div>' + fmtB(m.flash) + '</div>' +\n"
"    '<div>PSRAM</div><div>' + (m.psram ? fmtB(m.psram) : 'nicht erkannt') + '</div>' +\n"
"    '<div>Freier RAM</div><div>' + fmtB(m.free) + ' (Minimum seit Start: ' + fmtB(m.minFree) + ')</div>' +\n"
"    '<div>Temperatur</div><div>' + Number(s.chip.temp).toFixed(1) + ' °C</div>' +\n"
"    '<div>Laufzeit</div><div>' + fmtUp(s.up) + '</div>' +\n"
"    '<div>Firmware</div><div>v' + esc(s.fw) + '</div>' +\n"
"    '<div>MAC</div><div class=\"mono\">' + esc(s.chip.mac) + '</div>' +\n"
"    '<div>ESP-IDF / Core</div><div>' + esc(s.chip.sdk) + ' / ' + esc(s.chip.core) + '</div></div></div>';\n"
"}\n"
"VIEWS.dashboard = el => {\n"
"  el.innerHTML = '<div id=\"dashbody\"><div class=\"card empty\">Lade...</div></div>' +\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('play') + '<h2>Schnellaktionen</h2></div><div class=\"act\">' +\n"
"    '<button class=\"btn\" data-q=\"wifi\">' + ic('wifi') + 'WLAN scannen</button>' +\n"
"    '<button class=\"btn\" data-q=\"ble\">' + ic('bt') + 'BLE scannen</button>' +\n"
"    '<button class=\"btn\" data-q=\"apstart\">' + ic('radio') + 'Eigenes WLAN starten</button>' +\n"
"    '<button class=\"btn\" data-q=\"test\">' + ic('flask') + 'Test-WLANs starten</button>' +\n"
"    '<button class=\"btn\" data-q=\"logs\">' + ic('list') + 'Logs anzeigen</button>' +\n"
"    '<button class=\"btn\" data-q=\"settings\">' + ic('gear') + 'Einstellungen</button>' +\n"
"    '<button class=\"btn dng\" data-q=\"restart\">' + ic('power') + 'ESP32 neu starten</button></div></div>';\n"
"  paintDash();\n"
"  $$('[data-q]', el).forEach(b => b.onclick = async () => {\n"
"    const q = b.dataset.q;\n"
"    if (q === 'wifi') { pending = 'scan'; go('wifi'); }\n"
"    else if (q === 'ble') { pending = 'scan'; go('ble'); }\n"
"    else if (q === 'apstart') { await act('/api/ap/start', {}, 'Eigenes WLAN gestartet'); refreshStatus().catch(() => {}); }\n"
"    else if (q === 'test') go('myap');\n"
"    else if (q === 'logs') go('logs');\n"
"    else if (q === 'settings') go('settings');\n"
"    else if (q === 'restart') doRestart();\n"
"  });\n"
"};\n"
"\n"
"// ----------------------------------------------------------- WLAN-Scanner ---\n"
"let WF = {nets: [], state: 'idle', age: -1};\n"
"const ENC_OPTS = ['', 'Offen', 'WEP', 'WPA', 'WPA2', 'WPA3', 'Enterprise'];\n"
"function encMatch(a, f) {\n"
"  if (!f) return true;\n"
"  if (f === 'Offen') return a === 'Offen';\n"
"  if (f === 'Enterprise') return a.indexOf('Enterprise') >= 0;\n"
"  return a.indexOf(f) >= 0;\n"
"}\n"
"function encTag(a) {\n"
"  const c = a === 'Offen' ? 'err' : (a === 'WEP' || a === 'WPA' ? 'warn' : (a.indexOf('WPA3') >= 0 ? 'ok' : ''));\n"
"  return '<span class=\"tag ' + c + '\">' + esc(a) + '</span>';\n"
"}\n"
"VIEWS.wifi = el => {\n"
"  el.innerHTML =\n"
"    '<div class=\"card\"><div class=\"bar\"><button class=\"btn pri\" id=\"wsStart\">' + ic('play') + 'Scan starten</button><button class=\"btn\" id=\"wsStop\">' + ic('stop') + 'Scan stoppen</button><span id=\"wsInfo\" class=\"mut\"></span></div>' +\n"
"    '<div class=\"filters\"><div class=\"srch\">' + ic('search') + '<input id=\"fq\" placeholder=\"SSID suchen...\" autocomplete=\"off\"></div>' +\n"
"    '<select id=\"fsig\"><option value=\"-200\">Signal: alle</option><option value=\"-50\">stärker als -50 dBm</option><option value=\"-60\">stärker als -60 dBm</option><option value=\"-70\">stärker als -70 dBm</option><option value=\"-80\">stärker als -80 dBm</option></select>' +\n"
"    '<select id=\"fch\"><option value=\"0\">Kanal: alle</option>' + Array.from({length: 14}, (_, i) => '<option value=\"' + (i + 1) + '\">Kanal ' + (i + 1) + '</option>').join('') + '</select>' +\n"
"    '<select id=\"fenc\">' + ENC_OPTS.map(o => '<option value=\"' + o + '\">' + (o || 'Verschlüsselung: alle') + '</option>').join('') + '</select>' +\n"
"    '<select id=\"fsort\"><option value=\"strong\">Sortierung: stärkstes Signal</option><option value=\"weak\">schwächstes Signal</option><option value=\"ch\">Kanal</option><option value=\"name\">Name</option></select></div></div>' +\n"
"    '<div class=\"card scroll\"><table class=\"tbl\"><thead><tr><th>Name (SSID)</th><th>Signal</th><th>Kanal</th><th>Verschlüsselung</th><th class=\"hm\">BSSID</th><th></th></tr></thead><tbody id=\"wtb\"></tbody></table></div>';\n"
"  const paint = () => {\n"
"    const q = $('#fq').value.trim().toLowerCase(), sig = +$('#fsig').value, ch = +$('#fch').value, enc = $('#fenc').value, so = $('#fsort').value;\n"
"    let list = WF.nets.filter(n => n.rssi >= sig && (!ch || n.ch === ch) && encMatch(n.auth, enc) && (!q || n.ssid.toLowerCase().indexOf(q) >= 0 || n.bssid.toLowerCase().indexOf(q) >= 0));\n"
"    list.sort((a, b) => so === 'weak' ? a.rssi - b.rssi : so === 'ch' ? (a.ch - b.ch || b.rssi - a.rssi) : so === 'name' ? a.ssid.localeCompare(b.ssid) : b.rssi - a.rssi);\n"
"    $('#wtb').innerHTML = list.length ? list.map(n =>\n"
"      '<tr><td><b>' + esc(n.ssid) + '</b>' + (n.hidden ? ' <span class=\"tag\">versteckt</span>' : '') + '</td><td>' + sigBar(n.rssi) + '</td><td>' + n.ch + '</td><td>' + encTag(n.auth) + '</td><td class=\"hm mono\">' + esc(n.bssid) + '</td>' +\n"
"      '<td><button class=\"btn sm\" data-b=\"' + esc(n.bssid) + '\" data-c=\"' + n.ch + '\" data-s=\"' + esc(n.ssid) + '\">' + ic('activity') + 'Verfolgen</button></td></tr>').join('')\n"
"      : '<tr><td colspan=\"6\" class=\"empty\">' + (WF.nets.length ? 'Keine Netzwerke für diesen Filter.' : 'Noch keine Ergebnisse. Starte einen Scan.') + '</td></tr>';\n"
"    $$('[data-b]', el).forEach(b => b.onclick = () => { pending = {bssid: b.dataset.b, ch: +b.dataset.c, ssid: b.dataset.s}; go('signal'); });\n"
"    const running = WF.state === 'running' && !WF.track;\n"
"    $('#wsStart').disabled = running; $('#wsStop').disabled = !running;\n"
"    $('#wsInfo').textContent = running ? 'Scan läuft...' : (WF.nets.length + ' Netzwerke' + (WF.age >= 0 ? ' (vor ' + WF.age + ' s)' : '') + (list.length !== WF.nets.length ? ', ' + list.length + ' angezeigt' : ''));\n"
"  };\n"
"  let poller = null;\n"
"  const load = async () => {\n"
"    WF = await api('/api/wifi/scan');\n"
"    paint();\n"
"    if (WF.state !== 'running' && poller) { clearInterval(poller); poller = null; }\n"
"  };\n"
"  const startPoll = () => { if (!poller) { poller = setInterval(() => load().catch(() => {}), 1200); timers.push(poller); } };\n"
"  $('#wsStart').onclick = async () => { if (await act('/api/wifi/scan/start', {}, 'Scan gestartet')) { WF.state = 'running'; WF.track = false; paint(); startPoll(); } };\n"
"  $('#wsStop').onclick = async () => { await act('/api/wifi/scan/stop', {}, 'Scan gestoppt'); load().catch(() => {}); };\n"
"  ['fq', 'fsig', 'fch', 'fenc', 'fsort'].forEach(i => $('#' + i).addEventListener(i === 'fq' ? 'input' : 'change', paint));\n"
"  load().then(() => { if (WF.state === 'running') startPoll(); if (pending === 'scan') { pending = null; $('#wsStart').click(); } }).catch(() => {});\n"
"};\n"
"\n"
"// ---------------------------------------------------------- Kanaluebersicht ---\n"
"VIEWS.channels = el => {\n"
"  el.innerHTML = '<div class=\"card\"><div class=\"bar\"><button class=\"btn pri\" id=\"csStart\">' + ic('play') + 'Scan starten</button><span id=\"csInfo\" class=\"mut\"></span></div></div>' +\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('bars') + '<h2>Netzwerke pro WLAN-Kanal (2,4 GHz)</h2></div><div class=\"chart\" id=\"chart\"></div><div class=\"hint\" id=\"chHint\"></div></div>' +\n"
"    '<div class=\"card\"><h3>Netzwerke pro Kanal</h3><div id=\"chList\" style=\"margin-top:10px\"></div></div>';\n"
"  const paint = () => {\n"
"    const cnt = Array(14).fill(0), best = Array(14).fill(-200), names = Array.from({length: 14}, () => []);\n"
"    WF.nets.forEach(n => { if (n.ch >= 1 && n.ch <= 14) { cnt[n.ch - 1]++; best[n.ch - 1] = Math.max(best[n.ch - 1], n.rssi); names[n.ch - 1].push(n.ssid); } });\n"
"    const mx = Math.max(1, ...cnt);\n"
"    // Empfehlung: unter den ueberlappungsfreien Kanaelen 1/6/11 den am wenigsten belegten waehlen\n"
"    const w = [1, .7, .35, .12];\n"
"    let rec = 0, recScore = 1e9;\n"
"    [1, 6, 11].forEach(c => { let s = 0; WF.nets.forEach(n => { const d = Math.abs(n.ch - c); if (d < w.length) s += w[d] * (n.rssi > -75 ? 1.5 : 1); }); if (s < recScore) { recScore = s; rec = c; } });\n"
"    $('#chart').innerHTML = cnt.slice(0, 13).map((c, i) => '<div class=\"col' + (WF.nets.length && rec === i + 1 ? ' rec' : '') + '\"><b>' + (c || '') + '</b><div class=\"bx ' + (c === 0 ? 'zero' : (c === mx && c > 1 ? 'hot' : '')) + '\" style=\"height:' + (c ? 8 + c / mx * 82 : 2) + '%\"></div><span>' + (i + 1) + '</span></div>').join('');\n"
"    $('#chHint').textContent = WF.nets.length ? 'Empfehlung für den eigenen Access Point: Kanal ' + rec + ' (am wenigsten Störung durch andere Netze). Nur die Kanäle 1, 6 und 11 überlappen sich nicht.' : 'Starte einen Scan, um die Kanalbelegung zu sehen.';\n"
"    $('#chList').innerHTML = cnt.slice(0, 13).map((c, i) => c ? '<div style=\"padding:8px 0;border-bottom:1px solid var(--line)\"><b>Kanal ' + (i + 1) + '</b> <span class=\"tag acc\">' + c + '</span> <span class=\"mut\">stärkstes Signal ' + best[i] + ' dBm</span><div class=\"mut\" style=\"font-size:13px\">' + names[i].map(esc).join(', ') + '</div></div>' : '').join('') || '<div class=\"empty\">Keine Daten.</div>';\n"
"    const running = WF.state === 'running' && !WF.track;\n"
"    $('#csStart').disabled = running;\n"
"    $('#csInfo').textContent = running ? 'Scan läuft...' : (WF.nets.length + ' Netzwerke');\n"
"  };\n"
"  let poller = null;\n"
"  const load = async () => { WF = await api('/api/wifi/scan'); paint(); if (WF.state !== 'running' && poller) { clearInterval(poller); poller = null; } };\n"
"  $('#csStart').onclick = async () => { if (await act('/api/wifi/scan/start', {}, 'Scan gestartet')) { WF.state = 'running'; WF.track = false; paint(); if (!poller) { poller = setInterval(() => load().catch(() => {}), 1200); timers.push(poller); } } };\n"
"  load().then(() => { if (WF.state === 'running' && !poller) { poller = setInterval(() => load().catch(() => {}), 1200); timers.push(poller); } }).catch(() => {});\n"
"};\n"
"\n"
"// -------------------------------------------------------------------- RSSI ---\n"
"let TRK = {on: false, target: null, hist: []};\n"
"VIEWS.signal = el => {\n"
"  el.innerHTML = '<div class=\"card\"><div class=\"bar\"><select id=\"tsel\" style=\"flex:1 1 260px\"><option value=\"\">Netzwerk wählen...</option></select><button class=\"btn\" id=\"tscan\">' + ic('search') + 'Netzwerke suchen</button><button class=\"btn pri\" id=\"tgo\">' + ic('play') + 'Start</button></div><div class=\"hint\">Der ESP32 fragt das gewählte Netzwerk jede Sekunde ab (kurze Abfrage nur auf dessen Kanal).</div></div>' +\n"
"    '<div class=\"grid g4\" style=\"margin-top:14px\" id=\"tstats\"></div>' +\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('activity') + '<h2 id=\"tname\">Signalverlauf</h2></div><canvas id=\"cv\"></canvas></div>';\n"
"  const sel = $('#tsel');\n"
"  const fill = () => {\n"
"    const keep = TRK.target ? TRK.target.bssid : sel.value;\n"
"    sel.innerHTML = '<option value=\"\">Netzwerk wählen...</option>' + WF.nets.slice().sort((a, b) => b.rssi - a.rssi).map(n => '<option value=\"' + esc(n.bssid) + '\" data-c=\"' + n.ch + '\" data-s=\"' + esc(n.ssid) + '\">' + esc(n.ssid) + ' (Kanal ' + n.ch + ', ' + n.rssi + ' dBm)</option>').join('');\n"
"    if (TRK.target && !WF.nets.some(n => n.bssid === TRK.target.bssid)) sel.innerHTML += '<option value=\"' + esc(TRK.target.bssid) + '\" data-c=\"' + TRK.target.ch + '\" data-s=\"' + esc(TRK.target.ssid) + '\">' + esc(TRK.target.ssid) + ' (Kanal ' + TRK.target.ch + ')</option>';\n"
"    sel.value = keep;\n"
"  };\n"
"  const draw = () => {\n"
"    const cv = $('#cv'); if (!cv) return;\n"
"    const dpr = window.devicePixelRatio || 1, W = cv.clientWidth, H = cv.clientHeight;\n"
"    cv.width = W * dpr; cv.height = H * dpr;\n"
"    const g = cv.getContext('2d'); g.scale(dpr, dpr);\n"
"    g.clearRect(0, 0, W, H);\n"
"    const L = 44, R = 10, T = 12, B = 22, lo = -100, hi = -20;\n"
"    const y = v => T + (hi - v) / (hi - lo) * (H - T - B);\n"
"    g.font = '11px system-ui'; g.textBaseline = 'middle';\n"
"    for (let v = -100; v <= -20; v += 20) { g.strokeStyle = '#25324d'; g.beginPath(); g.moveTo(L, y(v)); g.lineTo(W - R, y(v)); g.stroke(); g.fillStyle = '#8fa0c0'; g.fillText(v + '', 6, y(v)); }\n"
"    const h = TRK.hist, N = 120;\n"
"    const x = i => L + (W - L - R) * (i / (N - 1));\n"
"    const off = Math.max(0, N - h.length);\n"
"    g.strokeStyle = '#4f8cff'; g.lineWidth = 2.2; g.lineJoin = 'round';\n"
"    g.beginPath(); let pen = false;\n"
"    h.forEach((v, i) => { if (v === null) { pen = false; return; } const px = x(i + off), py = y(clamp(v, lo, hi)); if (!pen) { g.moveTo(px, py); pen = true; } else g.lineTo(px, py); });\n"
"    g.stroke();\n"
"    const last = h.length ? h[h.length - 1] : null;\n"
"    if (last !== null) { g.fillStyle = sigColor(last); g.beginPath(); g.arc(x(h.length - 1 + off), y(clamp(last, lo, hi)), 4.5, 0, 7); g.fill(); }\n"
"    g.fillStyle = '#8fa0c0'; g.textBaseline = 'alphabetic'; g.fillText('Zeit →   (letzte ' + h.length + ' Messungen)', L, H - 5);\n"
"  };\n"
"  const stats = () => {\n"
"    const v = TRK.hist.filter(x => x !== null), last = TRK.hist.length ? TRK.hist[TRK.hist.length - 1] : null;\n"
"    const c = (l, t, col) => '<div class=\"card stat\"><div class=\"lbl\">' + l + '</div><div class=\"val\" style=\"' + (col ? 'color:' + col : '') + '\">' + t + '</div></div>';\n"
"    $('#tstats').innerHTML = c('Aktuell', last === null ? (TRK.hist.length ? 'nicht gefunden' : '-') : last + ' dBm', last === null ? '' : sigColor(last)) +\n"
"      c('Stärkstes', v.length ? Math.max(...v) + ' dBm' : '-') + c('Schwächstes', v.length ? Math.min(...v) + ' dBm' : '-') +\n"
"      c('Durchschnitt', v.length ? Math.round(v.reduce((a, b) => a + b, 0) / v.length) + ' dBm' : '-');\n"
"    $('#tname').textContent = TRK.target ? 'Signalverlauf: ' + TRK.target.ssid : 'Signalverlauf';\n"
"    $('#tgo').innerHTML = TRK.on ? ic('stop') + 'Stopp' : ic('play') + 'Start';\n"
"    draw();\n"
"  };\n"
"  TRK.paint = stats;\n"
"  const loop = async () => {\n"
"    const mine = TRK.run = (TRK.run || 0) + 1;\n"
"    while (TRK.on && TRK.run === mine) {\n"
"      try {\n"
"        try { await api('/api/wifi/track', {bssid: TRK.target.bssid, ch: TRK.target.ch}); }\n"
"        catch (e) { if (e.message.indexOf('läuft') < 0) throw e; }\n"
"        let d = null;\n"
"        for (let i = 0; i < 40 && TRK.on && TRK.run === mine; i++) { await sleep(250); d = await api('/api/wifi/track'); if (!d.running) break; }\n"
"        if (d && !d.running && TRK.run === mine) { TRK.hist.push(d.found ? d.rssi : null); if (TRK.hist.length > 120) TRK.hist.shift(); if ($('#cv')) stats(); }\n"
"      } catch (e) { if (e.message === 'Nicht angemeldet') { TRK.on = false; break; } await sleep(2000); }\n"
"      await sleep(600);\n"
"    }\n"
"  };\n"
"  $('#tgo').onclick = () => {\n"
"    if (TRK.on) { TRK.on = false; TRK.paint && TRK.paint(); return; }\n"
"    const o = sel.selectedOptions[0];\n"
"    if (!o || !o.value) { toast('Bitte zuerst ein Netzwerk wählen', 'err'); return; }\n"
"    if (!TRK.target || TRK.target.bssid !== o.value) { TRK.hist = []; }\n"
"    TRK.target = {bssid: o.value, ch: +o.dataset.c, ssid: o.dataset.s};\n"
"    TRK.on = true; stats(); loop();\n"
"  };\n"
"  $('#tscan').onclick = async () => {\n"
"    if (TRK.on) { toast('Bitte zuerst die Messung stoppen', 'err'); return; }\n"
"    if (!await act('/api/wifi/scan/start', {}, 'Suche läuft...')) return;\n"
"    for (let i = 0; i < 40; i++) { await sleep(1000); const d = await api('/api/wifi/scan').catch(() => null); if (d && d.state !== 'running') { WF = d; fill(); toast(d.count + ' Netzwerke gefunden', 'ok'); break; } }\n"
"  };\n"
"  window.addEventListener('resize', draw);\n"
"  if (pending && pending.bssid) {\n"
"    TRK.target = pending; TRK.hist = []; pending = null;\n"
"    fill(); sel.value = TRK.target.bssid; TRK.on = true; stats(); loop();\n"
"  } else {\n"
"    api('/api/wifi/scan').then(d => { WF = d; fill(); stats(); }).catch(() => {}); stats();\n"
"  }\n"
"};\n"
"\n"
"// --------------------------------------------------------------- Mein WLAN ---\n"
"VIEWS.myap = el => {\n"
"  el.innerHTML =\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('radio') + '<h2>Eigenes WLAN (Access Point)</h2></div><div id=\"apInfo\" class=\"kv\"></div><div class=\"bar\" style=\"margin-top:14px\"><button class=\"btn pri\" id=\"apStart\">' + ic('play') + 'WLAN starten</button><button class=\"btn dng\" id=\"apStop\">' + ic('stop') + 'WLAN stoppen</button><button class=\"btn\" id=\"apSet\">' + ic('gear') + 'Name / Passwort / Kanal ändern</button></div>' +\n"
"    '<div class=\"hint\">Wichtig: Stoppst du das WLAN, verlierst du die Verbindung zu dieser Seite. Ein Neustart des ESP32 (Reset-Taste) startet es wieder.</div></div>' +\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('users') + '<h2>Verbundene Geräte</h2></div><div class=\"scroll\"><table class=\"tbl\"><thead><tr><th>IP-Adresse</th><th>MAC-Adresse</th><th>Signal</th><th>Status</th><th></th></tr></thead><tbody id=\"cl\"></tbody></table></div>' +\n"
"    '<div class=\"hint\">Gilt ausschließlich für Geräte an deinem eigenen ESP32-WLAN. „Sperren“ wirft ein Gerät aus diesem WLAN und verhindert das erneute Verbinden. Handys mit „privater MAC-Adresse“ nutzen pro WLAN eine feste, aber andere MAC.</div></div>' +\n"
"    '<div class=\"card\"><div class=\"sec\">' + ic('flask') + '<h2>Test-WLANs</h2></div>' +\n"
"    '<div class=\"hint\" style=\"margin:0 0 12px\">Der ESP32 erzeugt eigene Test-Access-Points mit zufälligen Namen (z. B. ESP32-Test-4821). Er hat nur <b>ein</b> Funkmodul und kann deshalb nur <b>ein</b> Test-WLAN gleichzeitig senden. Bei mehreren werden sie reihum aktiviert. Sie nutzen das Passwort deines Access Points; diese Weboberfläche bleibt darüber unter 192.168.4.1 erreichbar. Keine fremden Namen, kein Störsender.</div>' +\n"
"    '<div class=\"bar\"><label style=\"margin:0\">Anzahl</label><select id=\"tcount\"><option>1</option><option>2</option><option>3</option><option>5</option></select><label style=\"margin:0\">Wechsel alle</label><select id=\"tint\"><option value=\"15\">15 s</option><option value=\"30\" selected>30 s</option><option value=\"60\">60 s</option></select>' +\n"
"    '<button class=\"btn pri\" id=\"tStart\">' + ic('play') + 'Test-WLANs starten</button><button class=\"btn dng\" id=\"tStop\">' + ic('stop') + 'Alle Test-WLANs stoppen</button></div><div id=\"tInfo\" style=\"margin-top:14px\"></div></div>';\n"
"  const load = async () => {\n"
"    const [a, c] = await Promise.all([api('/api/ap'), api('/api/ap/clients')]);\n"
"    $('#apInfo').innerHTML = '<div>Status</div><div>' + (a.test ? '' : '') + (a.running ? '<span class=\"tag ok\">Aktiv</span>' : '<span class=\"tag err\">Gestoppt</span>') + (a.test.active ? ' <span class=\"tag warn\">Testmodus</span>' : '') + '</div>' +\n"
"      '<div>Name (SSID)</div><div>' + esc(a.ssid || a.cfgSsid) + '</div><div>Kanal</div><div>' + a.ch + (a.ch !== a.cfgCh && a.running ? ' <span class=\"mut\">(eingestellt: ' + a.cfgCh + ' - folgt dem Heim-WLAN)</span>' : '') + '</div><div>Adresse</div><div class=\"mono\">http://' + esc(a.ip) + '</div><div>MAC</div><div class=\"mono\">' + esc(a.mac) + '</div><div>Geräte</div><div>' + a.clients + '</div>';\n"
"    $('#cl').innerHTML = c.clients.length ? c.clients.map(d => '<tr><td>' + esc(d.ip) + '</td><td class=\"mono\">' + esc(d.mac) + '</td><td>' + (d.connected ? sigBar(d.rssi) : '<span class=\"mut\">-</span>') + '</td><td><span class=\"tag ' + (d.blocked ? 'err' : 'ok') + '\">' + esc(d.status) + (d.blocked && !d.connected ? ' (offline)' : '') + '</span></td><td><button class=\"btn sm\" data-m=\"' + esc(d.mac) + '\" data-k=\"' + (d.blocked ? 'unblock' : 'block') + '\">' + ic(d.blocked ? 'unlock' : 'lock') + (d.blocked ? 'Entsperren' : 'Sperren') + '</button></td></tr>').join('') : '<tr><td colspan=\"5\" class=\"empty\">Kein Gerät verbunden.</td></tr>';\n"
"    $$('[data-m]', el).forEach(b => b.onclick = async () => { if (b.dataset.k === 'block' && !confirm('Dieses Gerät aus deinem WLAN werfen und sperren?')) return; await act('/api/ap/' + b.dataset.k, {mac: b.dataset.m}, b.dataset.k === 'block' ? 'Gerät gesperrt' : 'Gerät entsperrt'); load().catch(() => {}); });\n"
"    const t = a.test;\n"
"    $('#tInfo').innerHTML = t.active ? '<div class=\"pill-row\">' + t.names.map((n, i) => '<span class=\"tag ' + (i === t.idx ? 'ok' : '') + '\">' + (i === t.idx ? '● ' : '') + esc(n) + '</span>').join('') + '</div><div class=\"hint\">' + (t.count > 1 ? 'Nächster Wechsel in ' + Math.max(0, t.nextIn) + ' s. ' : '') + 'Endet automatisch in ' + Math.max(0, Math.round(t.left / 60)) + ' Min.</div>' : '<div class=\"mut\">Kein Test-WLAN aktiv.</div>';\n"
"    $('#apStart').disabled = a.running && !t.active; $('#apStop').disabled = !a.running;\n"
"  };\n"
"  $('#apStart').onclick = async () => { await act('/api/ap/start', {}, 'WLAN gestartet'); load().catch(() => {}); };\n"
"  $('#apStop').onclick = async () => { if (!confirm('Eigenes WLAN wirklich stoppen? Du verlierst die Verbindung zu dieser Seite.')) return; await act('/api/ap/stop', {}, 'WLAN wird gestoppt'); };\n"
"  $('#apSet').onclick = () => { pending = 'ap'; go('settings'); };\n"
"  $('#tStart').onclick = async () => {\n"
"    if (!confirm('Test-WLANs starten? Dein aktuelles WLAN wechselt dabei den Namen - verbinde dich danach mit dem angezeigten Test-WLAN (gleiches Passwort).')) return;\n"
"    await act('/api/test/start', {count: $('#tcount').value, interval: $('#tint').value}, 'Test-WLANs gestartet'); load().catch(() => {});\n"
"  };\n"
"  $('#tStop').onclick = async () => { await act('/api/test/stop', {}, 'Test-WLANs gestoppt'); load().catch(() => {}); };\n"
"  every(load, 3000);\n"
"};\n"
"\n"
"// --------------------------------------------------------------- Bluetooth ---\n"
"let BL = {devs: [], state: 'ready'};\n"
"VIEWS.ble = el => {\n"
"  el.innerHTML = '<div class=\"card\"><div class=\"bar\"><select id=\"bdur\"><option value=\"5\">5 s</option><option value=\"10\" selected>10 s</option><option value=\"30\">30 s</option><option value=\"60\">60 s</option></select><button class=\"btn pri\" id=\"bsStart\">' + ic('play') + 'BLE scannen</button><button class=\"btn\" id=\"bsStop\">' + ic('stop') + 'Scan stoppen</button><button class=\"btn dng\" id=\"bsClear\">' + ic('trash') + 'Ergebnisse löschen</button><span id=\"bsInfo\" class=\"mut\"></span></div>' +\n"
"    '<div class=\"filters\"><div class=\"srch\">' + ic('search') + '<input id=\"bq\" placeholder=\"Name, Adresse oder Hersteller suchen...\" autocomplete=\"off\"></div><select id=\"bsort\"><option value=\"strong\">Sortierung: stärkstes Signal</option><option value=\"weak\">schwächstes Signal</option><option value=\"name\">Name</option><option value=\"mfr\">Hersteller</option></select></div></div>' +\n"
"    '<div class=\"card scroll\"><table class=\"tbl\"><thead><tr><th>Gerät</th><th>Adresse</th><th>Signal</th><th>Hersteller</th><th class=\"hm\">Services (UUID)</th></tr></thead><tbody id=\"btb\"></tbody></table></div>';\n"
"  const paint = () => {\n"
"    const q = $('#bq').value.trim().toLowerCase(), so = $('#bsort').value;\n"
"    const list = BL.devs.filter(d => !q || (d.name + ' ' + d.addr + ' ' + d.mfrName + ' ' + d.svc.join(' ')).toLowerCase().indexOf(q) >= 0);\n"
"    list.sort((a, b) => so === 'weak' ? a.rssi - b.rssi : so === 'name' ? (a.name || '￿').localeCompare(b.name || '￿') : so === 'mfr' ? (a.mfrName || '￿').localeCompare(b.mfrName || '￿') : b.rssi - a.rssi);\n"
"    $('#btb').innerHTML = list.length ? list.map(d => '<tr><td><b>' + (d.name ? esc(d.name) : '<span class=\"mut\">(ohne Name)</span>') + '</b></td><td class=\"mono\">' + esc(d.addr) + '</td><td>' + sigBar(d.rssi) + '</td><td>' + (d.mfrName ? esc(d.mfrName) : (d.mfr >= 0 ? '<span class=\"mut\">ID 0x' + d.mfr.toString(16).toUpperCase().padStart(4, '0') + '</span>' : '<span class=\"mut\">-</span>')) + '</td><td class=\"hm mono\">' + (d.svc.length ? d.svc.map(esc).join(', ') : '<span class=\"mut\">-</span>') + '</td></tr>').join('')\n"
"      : '<tr><td colspan=\"5\" class=\"empty\">' + (BL.devs.length ? 'Keine Treffer.' : 'Noch keine Geräte. Starte einen Scan.') + '</td></tr>';\n"
"    const sc = BL.state === 'scanning';\n"
"    $('#bsStart').disabled = sc; $('#bsStop').disabled = !sc;\n"
"    $('#bsInfo').textContent = BL.state === 'error' ? 'BLE-Fehler - bitte neu starten' : (sc ? 'Scan läuft... noch ' + Math.max(0, BL.left) + ' s' : BL.devs.length + ' Geräte');\n"
"  };\n"
"  const load = async () => { BL = await api('/api/ble'); paint(); };\n"
"  $('#bsStart').onclick = async () => { if (await act('/api/ble/start', {secs: $('#bdur').value}, 'BLE-Scan gestartet')) { BL.state = 'scanning'; BL.left = +$('#bdur').value; paint(); } };\n"
"  $('#bsStop').onclick = async () => { await act('/api/ble/stop', {}, 'Scan wird gestoppt'); };\n"
"  $('#bsClear').onclick = async () => { await act('/api/ble/clear', {}, 'Ergebnisse gelöscht'); load().catch(() => {}); };\n"
"  ['bq', 'bsort'].forEach(i => $('#' + i).addEventListener(i === 'bq' ? 'input' : 'change', paint));\n"
"  every(load, 1500);\n"
"  if (pending === 'scan') { pending = null; setTimeout(() => $('#bsStart') && $('#bsStart').click(), 300); }\n"
"};\n"
"\n"
"// -------------------------------------------------------------------- Logs ---\n"
"VIEWS.logs = el => {\n"
"  el.innerHTML = '<div class=\"card\"><div class=\"bar\"><button class=\"btn dng\" id=\"lgClear\">' + ic('trash') + 'Logs löschen</button><button class=\"btn\" id=\"lgRef\">' + ic('refresh') + 'Aktualisieren</button><span class=\"mut\" id=\"lgInfo\"></span></div></div><div class=\"card\"><div class=\"log\" id=\"lg\"></div></div>';\n"
"  const load = async () => {\n"
"    const d = await api('/api/logs');\n"
"    const rows = d.logs.slice().reverse();\n"
"    $('#lg').innerHTML = rows.length ? rows.map(l => '<div class=\"l' + l.l + '\"><span class=\"t\">' + fmtUp(l.t) + '</span><span>' + esc(l.m) + '</span></div>').join('') : '<div class=\"empty\">Keine Einträge.</div>';\n"
"    $('#lgInfo').textContent = d.logs.length + ' Einträge (Zeit = Laufzeit seit Start)';\n"
"  };\n"
"  $('#lgClear').onclick = async () => { await act('/api/logs/clear', {}, 'Logs gelöscht'); load().catch(() => {}); };\n"
"  $('#lgRef').onclick = () => load().catch(() => {});\n"
"  every(load, 2500);\n"
"};\n"
"\n"
"// ----------------------------------------------------------- Einstellungen ---\n"
"let UP = null;\n"
"VIEWS.settings = el => {\n"
"  el.innerHTML =\n"
"    '<div class=\"sub-nav\"><a data-s=\"s-ap\">Access Point</a><a data-s=\"s-web\">Webinterface</a><a data-s=\"s-sta\">Internet (Heim-WLAN)</a><a data-s=\"s-sys\">System</a><a data-s=\"s-upd\">Firmware-Update</a></div>' +\n"
"    '<div class=\"card\" id=\"s-ap\"><div class=\"sec\">' + ic('radio') + '<h2>Access Point</h2></div>' +\n"
"    '<label>Name des WLANs (SSID)</label><input id=\"apSsid\" maxlength=\"32\" style=\"width:100%;max-width:380px\"><label>Passwort (8-63 Zeichen, leer = unverändert)</label><input id=\"apPass\" type=\"password\" autocomplete=\"new-password\" maxlength=\"63\" placeholder=\"unverändert\" style=\"width:100%;max-width:380px\"><label>Kanal</label><select id=\"apCh\">' + Array.from({length: 11}, (_, i) => '<option>' + (i + 1) + '</option>').join('') + '</select>' +\n"
"    '<div class=\"hint\">Ist der ESP32 mit einem Heim-WLAN verbunden, übernimmt der Access Point automatisch dessen Kanal.</div><div style=\"margin-top:14px\"><button class=\"btn pri\" id=\"apSave\">Speichern &amp; WLAN neu starten</button></div></div>' +\n"
"    '<div class=\"card\" id=\"s-web\"><div class=\"sec\">' + ic('shield') + '<h2>Webinterface</h2></div>' +\n"
"    '<label style=\"display:flex;gap:10px;align-items:center;color:var(--tx);font-size:15px\"><input type=\"checkbox\" id=\"wAuth\"> Login aktivieren</label>' +\n"
"    '<label>Benutzername</label><input id=\"wUser\" maxlength=\"32\" autocomplete=\"username\" style=\"width:100%;max-width:380px\"><label>Aktuelles Passwort (zur Bestätigung)</label><input id=\"wOld\" type=\"password\" autocomplete=\"current-password\" style=\"width:100%;max-width:380px\"><label>Neues Passwort (mind. 6 Zeichen, leer = unverändert)</label><input id=\"wNew\" type=\"password\" autocomplete=\"new-password\" style=\"width:100%;max-width:380px\">' +\n"
"    '<div style=\"margin-top:14px\"><button class=\"btn pri\" id=\"wSave\">Speichern</button></div></div>' +\n"
"    '<div class=\"card\" id=\"s-sta\"><div class=\"sec\">' + ic('globe') + '<h2>Internet (Heim-WLAN, optional)</h2></div><div class=\"hint\" style=\"margin:0 0 6px\">Nur nötig für Firmware-Updates. Ohne Internet funktioniert alles andere weiterhin.</div><div id=\"staInfo\" class=\"kv\" style=\"margin:10px 0\"></div>' +\n"
"    '<div class=\"bar\"><button class=\"btn pri\" id=\"staScan\">' + ic('search') + 'WLANs suchen</button><span id=\"staScanInfo\" class=\"mut\"></span></div>' +\n"
"    '<div id=\"staList\" style=\"margin-top:12px\"></div>' +\n"
"    '<div id=\"staPickForm\" style=\"display:none;margin-top:14px;padding-top:14px;border-top:1px solid var(--line)\">' +\n"
"    '<div class=\"hint\" style=\"margin:0 0 10px\">Verbinden mit: <b id=\"pickedName\"></b></div><input type=\"hidden\" id=\"sSsid\">' +\n"
"    '<div id=\"staPassWrap\"><label>Passwort</label><input id=\"sPass\" type=\"password\" autocomplete=\"new-password\" maxlength=\"63\" style=\"width:100%;max-width:380px\"></div>' +\n"
"    '<div class=\"bar\" style=\"margin-top:14px\"><button class=\"btn pri\" id=\"sGo\">Verbinden</button><button class=\"btn\" id=\"sCancel\">Abbrechen</button></div></div>' +\n"
"    '<details style=\"margin-top:14px\"><summary style=\"cursor:pointer;color:var(--mut);font-size:13px\">Verstecktes WLAN manuell eingeben</summary>' +\n"
"    '<label>WLAN-Name (SSID)</label><input id=\"sSsidManual\" maxlength=\"32\" style=\"width:100%;max-width:380px\"><label>Passwort</label><input id=\"sPassManual\" type=\"password\" autocomplete=\"new-password\" maxlength=\"63\" style=\"width:100%;max-width:380px\">' +\n"
"    '<div class=\"bar\" style=\"margin-top:14px\"><button class=\"btn\" id=\"sGoManual\">Verbinden</button></div></details>' +\n"
"    '<div class=\"bar\" style=\"margin-top:14px\"><button class=\"btn dng\" id=\"sOff\">Trennen &amp; vergessen</button></div></div>' +\n"
"    '<div class=\"card\" id=\"s-sys\"><div class=\"sec\">' + ic('cpu') + '<h2>System</h2></div><div class=\"kv\" id=\"sysInfo\"></div><div class=\"bar\" style=\"margin-top:14px\"><button class=\"btn\" id=\"rst\">' + ic('power') + 'Neustart</button><button class=\"btn dng\" id=\"fac\">' + ic('trash') + 'Werkseinstellungen</button></div></div>' +\n"
"    '<div class=\"card\" id=\"s-upd\"><div class=\"sec\">' + ic('download') + '<h2>Firmware-Update</h2></div><div id=\"updBody\"><div class=\"empty\">Lade...</div></div></div>';\n"
"  $$('[data-s]', el).forEach(a => a.onclick = () => { const t = $('#' + a.dataset.s); if (t) t.scrollIntoView({behavior: 'smooth', block: 'start'}); });\n"
"\n"
"  api('/api/settings').then(s => {\n"
"    $('#apSsid').value = s.apSsid; $('#apCh').value = s.apCh; $('#wUser').value = s.webUser; $('#wAuth').checked = s.authEnabled;\n"
"    paintSta(s.sta);\n"
"  }).catch(() => {});\n"
"  let staNets = [];\n"
"  const paintStaList = () => {\n"
"    const seen = new Set(); const uniq = [];\n"
"    staNets.forEach(n => { if (!n.hidden && !seen.has(n.ssid)) { seen.add(n.ssid); uniq.push(n); } });\n"
"    $('#staList').innerHTML = uniq.length ? uniq.map(n => '<div class=\"wlrow\" data-ssid=\"' + esc(n.ssid) + '\" data-open=\"' + (n.auth === 'Offen' ? 1 : 0) + '\" style=\"display:flex;align-items:center;gap:12px;padding:12px 14px;border:1px solid var(--line);border-radius:12px;margin-bottom:8px;cursor:pointer\">' + ic(n.auth === 'Offen' ? 'unlock' : 'lock') + '<div style=\"flex:1;min-width:0\"><b>' + esc(n.ssid) + '</b><div class=\"mut\" style=\"font-size:12px\">Kanal ' + n.ch + ' · ' + esc(n.auth) + '</div></div>' + sigBar(n.rssi) + '</div>').join('') : '<div class=\"empty\">Noch keine WLANs. „WLANs suchen“ klicken.</div>';\n"
"    $$('.wlrow', el).forEach(r => r.onclick = () => selectSta(r.dataset.ssid, r.dataset.open === '1'));\n"
"  };\n"
"  const selectSta = (ssid, isOpen) => {\n"
"    $('#sSsid').value = ssid; $('#pickedName').textContent = ssid;\n"
"    $('#sPass').value = ''; $('#staPassWrap').style.display = isOpen ? 'none' : '';\n"
"    $('#staPickForm').style.display = '';\n"
"    if (!isOpen) $('#sPass').focus();\n"
"    $('#staPickForm').scrollIntoView({behavior: 'smooth', block: 'nearest'});\n"
"  };\n"
"  $('#staScan').onclick = async () => {\n"
"    $('#staScan').disabled = true; $('#staScanInfo').textContent = 'Suche läuft...';\n"
"    if (!await act('/api/wifi/scan/start', {}, false)) { $('#staScan').disabled = false; $('#staScanInfo').textContent = ''; return; }\n"
"    for (let i = 0; i < 40; i++) { await sleep(700); const d = await api('/api/wifi/scan').catch(() => null); if (d && d.state !== 'running') { staNets = d.nets.slice().sort((a, b) => b.rssi - a.rssi); paintStaList(); break; } }\n"
"    $('#staScan').disabled = false; $('#staScanInfo').textContent = staNets.length + ' WLANs gefunden';\n"
"  };\n"
"  const paintSta = sta => {\n"
"    const names = {idle: 'Nicht verbunden', connecting: 'Verbinde...', connected: 'Verbunden', failed: 'Verbindung fehlgeschlagen'};\n"
"    $('#staInfo').innerHTML = '<div>Status</div><div><span class=\"tag ' + (sta.state === 'connected' ? 'ok' : (sta.state === 'failed' ? 'err' : '')) + '\">' + names[sta.state] + '</span></div>' + (sta.state === 'connected' ? '<div>Netzwerk</div><div>' + esc(sta.ssid) + '</div><div>IP-Adresse</div><div class=\"mono\">' + esc(sta.ip) + '</div><div>Signal</div><div>' + sta.rssi + ' dBm</div>' : '');\n"
"  };\n"
"  const sysPaint = () => {\n"
"    if (!ST) return;\n"
"    $('#sysInfo').innerHTML = '<div>Chip</div><div>' + esc(ST.chip.model) + ' Rev. ' + ST.chip.rev + '</div><div>CPU</div><div>' + ST.chip.mhz + ' MHz</div><div>Flash</div><div>' + fmtB(ST.mem.flash) + '</div><div>PSRAM</div><div>' + (ST.mem.psram ? fmtB(ST.mem.psram) : 'nicht erkannt') + '</div><div>Freier RAM</div><div>' + fmtB(ST.mem.free) + '</div><div>Laufzeit</div><div>' + fmtUp(ST.up) + '</div><div>Firmware</div><div>v' + esc(ST.fw) + '</div>';\n"
"    if (ST.wifi) paintSta(ST.wifi.sta);\n"
"  };\n"
"  every(sysPaint, 3000);\n"
"\n"
"  $('#apSave').onclick = async () => {\n"
"    const r = await act('/api/ap/save', {ssid: $('#apSsid').value, pass: $('#apPass').value, ch: $('#apCh').value});\n"
"    if (r) { $('#apPass').value = ''; toast('Gespeichert. WLAN startet neu - bitte neu verbinden.', 'ok'); }\n"
"  };\n"
"  $('#wSave').onclick = async () => {\n"
"    const r = await act('/api/settings/web', {user: $('#wUser').value, oldpass: $('#wOld').value, newpass: $('#wNew').value, auth: $('#wAuth').checked ? '1' : '0'});\n"
"    if (r) { $('#wOld').value = ''; $('#wNew').value = ''; refreshStatus().catch(() => {}); }\n"
"  };\n"
"  $('#sCancel').onclick = () => { $('#staPickForm').style.display = 'none'; };\n"
"  $('#sGo').onclick = async () => {\n"
"    const ssid = $('#sSsid').value;\n"
"    if (!ssid) { toast('Bitte zuerst ein WLAN auswählen', 'err'); return; }\n"
"    const r = await act('/api/sta/connect', {ssid, pass: $('#sPass').value}, 'Verbinde...');\n"
"    if (r) { $('#sPass').value = ''; $('#staPickForm').style.display = 'none'; }\n"
"  };\n"
"  $('#sGoManual').onclick = async () => {\n"
"    const ssid = $('#sSsidManual').value.trim();\n"
"    if (!ssid) { toast('Bitte einen WLAN-Namen eingeben', 'err'); return; }\n"
"    const r = await act('/api/sta/connect', {ssid, pass: $('#sPassManual').value}, 'Verbinde...');\n"
"    if (r) $('#sPassManual').value = '';\n"
"  };\n"
"  $('#sOff').onclick = async () => { if (confirm('Heim-WLAN trennen und Zugangsdaten löschen?')) await act('/api/sta/disconnect', {}, 'Getrennt'); };\n"
"  $('#rst').onclick = doRestart;\n"
"  $('#fac').onclick = async () => {\n"
"    if (!confirm('ALLE Einstellungen (WLAN, Passwörter, Sperrliste) auf Werkseinstellungen zurücksetzen?')) return;\n"
"    if (prompt('Zur Bestätigung RESET eintippen:') !== 'RESET') return;\n"
"    if (await act('/api/system/factory', {confirm: 'RESET'}, false)) { setToken(''); waitForDevice('Werkseinstellungen werden hergestellt...'); }\n"
"  };\n"
"  initUpdate();\n"
"  if (pending === 'web' || pending === 'ap') { const t = $(pending === 'web' ? '#s-web' : '#s-ap'); pending = null; setTimeout(() => t && t.scrollIntoView({behavior: 'smooth', block: 'start'}), 200); }\n"
"};\n"
"\n"
"// ---- Firmware-Update (Abschnitt in den Einstellungen) ----\n"
"function initUpdate() {\n"
"  const body = $('#updBody');\n"
"  const phaseTxt = {idle: '', checking: 'Suche nach Updates...', downloading: 'Firmware wird heruntergeladen...', installing: 'Firmware wird installiert...', done: 'Update erfolgreich – ESP32 startet neu...', error: ''};\n"
"  let rebooting = false, prevPhase = null;\n"
"  const paint = d => {\n"
"    UP = d;\n"
"    if (!$('#updBody')) return;\n"
"    const active = d.phase === 'downloading' || d.phase === 'installing' || d.phase === 'done' || d.phase === 'checking';\n"
"    let statusCls = d.phase === 'error' ? 'err' : (d.phase === 'done' ? 'ok' : (active ? 'acc' : ''));\n"
"    let msg = d.msg || phaseTxt[d.phase] || '';\n"
"    if (d.phase === 'downloading') msg = 'Firmware wird heruntergeladen... ' + d.pct + ' %';\n"
"    const inet = d.internet === 1 ? '<span class=\"tag ok\">Online</span>' : (d.internet === 0 ? '<span class=\"tag err\">Offline</span>' : (d.staUp ? '<span class=\"tag ok\">Heim-WLAN verbunden</span>' : '<span class=\"tag\">Unbekannt</span>'));\n"
"    const gh = d.internet === 1 && d.github === 'Verbunden' ? '<span class=\"tag ok\">Verbunden</span>' : '<span class=\"tag ' + (d.github && d.github !== 'Noch nicht geprüft' ? 'err' : '') + '\">' + esc(d.github) + '</span>';\n"
"    body.innerHTML =\n"
"      (!d.supported ? '<div class=\"banner warn\"><span>Die Update-Funktion benötigt den ESP32-Core 3.3.12 oder neuer. Alle anderen Funktionen sind nicht betroffen.</span></div>' : '') +\n"
"      (!d.configured ? '<div class=\"banner warn\"><span>Es ist noch kein GitHub-Benutzer eingetragen. Trage <span class=\"mono\">GITHUB_USER</span> und <span class=\"mono\">GITHUB_REPO</span> am Anfang der .ino ein und lade sie neu hoch.</span></div>' : '') +\n"
"      '<div class=\"kv\"><div>Installierte Version</div><div><b>v' + esc(d.installed) + '</b></div>' +\n"
"      '<div>Neueste Version</div><div>' + (d.checked ? '<b>v' + esc(d.latest) + '</b>' + (d.available ? ' <span class=\"tag acc\">Update verfügbar</span>' : ' <span class=\"tag ok\">aktuell</span>') + (d.date ? ' <span class=\"mut\">veröffentlicht am ' + esc(d.date) + '</span>' : '') : '<span class=\"mut\">noch nicht geprüft</span>') + '</div>' +\n"
"      '<div>Internetstatus</div><div>' + inet + '</div><div>GitHub-Verbindung</div><div>' + gh + '</div>' +\n"
"      '<div>Repository</div><div class=\"mono\">' + esc(d.repo) + '</div>' +\n"
"      '<div>Letzte Prüfung</div><div>' + (d.lastCheck ? esc(d.lastCheck) : '<span class=\"mut\">nie</span>') + '</div>' +\n"
"      (d.checked && d.available ? '<div>SHA-256</div><div>' + (d.hasSha ? '<span class=\"tag ok\">Prüfsumme vorhanden</span>' : '<span class=\"tag err\">fehlt</span>') + '</div>' : '') + '</div>' +\n"
"      '<div class=\"bar\" style=\"margin-top:16px\"><button class=\"btn pri\" id=\"uChk\"' + (d.busy || !d.configured || !d.supported ? ' disabled' : '') + '>' + ic('refresh') + 'Nach Updates suchen</button><button class=\"btn\" id=\"uInst\"' + (d.canInstall ? '' : ' disabled') + '>' + ic('download') + 'Update installieren</button></div>' +\n"
"      (active && d.phase !== 'checking' ? '<div class=\"prog' + (d.phase === 'downloading' ? '' : ' busy') + '\"><i style=\"width:' + (d.phase === 'downloading' ? d.pct : 100) + '%\"></i></div>' : '') +\n"
"      (msg ? '<div style=\"margin-top:10px\" class=\"' + (statusCls ? 'tag ' + statusCls : '') + '\" id=\"uMsg\">' + esc(msg) + '</div>' : '') +\n"
"      (d.checked && d.available && d.notes ? '<h3 style=\"margin-top:16px\">Changelog</h3><pre class=\"notes\">' + esc(d.notes) + '</pre>' : '') +\n"
"      '<div class=\"hint\">Updates werden nur von <span class=\"mono\">github.com/' + esc(d.repo) + '</span> per HTTPS geladen, mit SHA-256 geprüft und in die zweite Firmware-Partition geschrieben. Bei einem Fehler bleibt die aktuelle Firmware unverändert.</div>' +\n"
"      '<div style=\"margin-top:18px;padding-top:14px;border-top:1px solid var(--line)\"><h3>Manuell aktualisieren (ohne Internet)</h3><div class=\"bar\" style=\"margin-top:8px\"><input type=\"file\" id=\"uFile\" accept=\".bin\"><button class=\"btn\" id=\"uUp\">' + ic('upload') + 'Datei hochladen</button></div><div class=\"prog\" id=\"uUpP\" style=\"display:none\"><i></i></div><div class=\"hint\">Nur die Datei <span class=\"mono\">…ino.bin</span> aus „Sketch → Kompilierte Binärdatei exportieren“ verwenden – nicht die Datei mit „merged“ im Namen.</div></div>';\n"
"    const chk = $('#uChk'); if (chk) chk.onclick = async () => { chk.disabled = true; await act('/api/update/check', {}, false); poll(); };\n"
"    const ins = $('#uInst'); if (ins) ins.onclick = async () => {\n"
"      if (!confirm('Update auf v' + d.latest + ' installieren? Der ESP32 startet danach neu.')) return;\n"
"      ins.disabled = true; await act('/api/update/install', {}, false); poll();\n"
"    };\n"
"    const up = $('#uUp'); if (up) up.onclick = manualUpload;\n"
"    const justDone = d.phase === 'done' && prevPhase !== null && prevPhase !== 'done';\n"
"    prevPhase = d.phase;\n"
"    if (justDone && !rebooting && !/Demo/.test(d.msg)) { rebooting = true; waitForDevice('Update erfolgreich – ESP32 startet neu...'); }\n"
"  };\n"
"  const manualUpload = () => {\n"
"    const f = $('#uFile').files[0];\n"
"    if (!f) { toast('Bitte zuerst eine .bin-Datei wählen', 'err'); return; }\n"
"    if (!/\\.bin$/i.test(f.name) || /merged|bootloader|partitions/i.test(f.name)) { toast('Falsche Datei: bitte die ...ino.bin verwenden (nicht merged/bootloader/partitions).', 'err'); return; }\n"
"    if (!confirm('Firmware \"' + f.name + '\" jetzt installieren?')) return;\n"
"    const fd = new FormData(); fd.append('firmware', f, f.name);\n"
"    const x = new XMLHttpRequest();\n"
"    x.open('POST', '/api/update/upload?t=' + encodeURIComponent(TOKEN));\n"
"    x.setRequestHeader('X-Token', TOKEN);\n"
"    const bar = $('#uUpP'); bar.style.display = '';\n"
"    x.upload.onprogress = e => { if (e.lengthComputable) bar.firstChild.style.width = Math.round(e.loaded * 100 / e.total) + '%'; };\n"
"    x.onload = () => {\n"
"      let j = {}; try { j = JSON.parse(x.responseText); } catch (e) {}\n"
"      if (x.status === 200 && j.ok) waitForDevice('Firmware installiert – ESP32 startet neu...');\n"
"      else toast(j.error || 'Upload fehlgeschlagen', 'err');\n"
"    };\n"
"    x.onerror = () => toast('Upload fehlgeschlagen (Verbindung)', 'err');\n"
"    x.send(fd);\n"
"  };\n"
"  let busyPoll = null;\n"
"  const load = async () => { const d = await api('/api/update'); paint(d); return d; };\n"
"  const poll = () => {\n"
"    if (busyPoll) return;\n"
"    busyPoll = setInterval(async () => {\n"
"      try { const d = await load(); if (!d.busy && d.phase !== 'downloading' && d.phase !== 'installing' && d.phase !== 'checking') { clearInterval(busyPoll); busyPoll = null; } }\n"
"      catch (e) { }\n"
"    }, 800);\n"
"    timers.push(busyPoll);\n"
"  };\n"
"  load().then(d => { if (d.busy || d.phase === 'downloading' || d.phase === 'installing' || d.phase === 'checking') poll(); }).catch(() => {});\n"
"}\n"
"\n"
"// -------------------------------------------------------- Navigation / Start ---\n"
"const PAGES = [['dashboard', 'Dashboard', 'home'], ['wifi', 'WLAN-Scanner', 'wifi'], ['channels', 'Kanäle', 'bars'], ['signal', 'Signal', 'activity'], ['myap', 'Mein WLAN', 'radio'], ['ble', 'Bluetooth', 'bt'], ['logs', 'Logs', 'list'], ['settings', 'Einstellungen', 'gear']];\n"
"function route() {\n"
"  if (!TOKEN && PUB.auth) return;\n"
"  const p = PAGES.find(x => x[0] === routeId) || PAGES[0];\n"
"  clearTimers();\n"
"  if (curPage === 'signal' && p[0] !== 'signal') TRK.on = false;\n"
"  curPage = p[0];\n"
"  $$('#nav a').forEach(a => a.classList.toggle('on', a.dataset.p === curPage));\n"
"  $('#pt').textContent = p[1];\n"
"  document.title = p[1] + ' · ESP32 Network Toolbox';\n"
"  const el = $('#view');\n"
"  el.innerHTML = '';\n"
"  VIEWS[curPage](el);\n"
"  window.scrollTo(0, 0);\n"
"  timers.push(setInterval(() => refreshStatus().catch(() => {}), 4000));\n"
"}\n"
"async function boot() {\n"
"  try { PUB = await (await fetch('/api/public', {cache: 'no-store'})).json(); setConn(true); } catch (e) { setConn(false); setTimeout(boot, 3000); return; }\n"
"  $('#foot').innerHTML = 'ESP32-S3 N16R8<br>Firmware v' + esc(PUB.fw);\n"
"  setToken(TOKEN);\n"
"  if (PUB.auth && !TOKEN) { showLogin(); return; }\n"
"  try { await refreshStatus(); } catch (e) { if (e.message === 'Nicht angemeldet') return; }\n"
"  route();\n"
"}\n"
"$('#nav').innerHTML = PAGES.map(p => '<a href=\"#' + p[0] + '\" data-p=\"' + p[0] + '\">' + ic(p[2]) + '<span>' + p[1] + '</span></a>').join('');\n"
"$('#nav').addEventListener('click', ev => { const a = ev.target.closest('a[data-p]'); if (a) { ev.preventDefault(); go(a.dataset.p); } });\n"
"$('#btnOut').onclick = async () => { try { await api('/api/logout', {}); } catch (e) {} setToken(''); clearTimers(); routeId = 'dashboard'; showLogin(); };\n"
"window.addEventListener('hashchange', () => { const h = (location.hash || '').slice(1); if (h && h !== routeId && PAGES.some(x => x[0] === h)) { routeId = h; route(); } });\n"
"try { const h0 = (location.hash || '').slice(1); if (PAGES.some(x => x[0] === h0)) routeId = h0; } catch (e) {}\n"
"boot();\n"
"</script>\n"
"</body>\n"
"</html>\n";
