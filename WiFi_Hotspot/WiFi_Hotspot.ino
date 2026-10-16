/*
  ============================================================================
   WiFi Hotspot                                                Version 2.0.0
   Board: DIYMORE ESP32-S3 DevKitC-1 N16R8 (ESP32-S3, 16 MB Flash, 8 MB PSRAM)
  ============================================================================

   Diese eine Datei ist das komplette Projekt (Firmware + Weboberflaeche).

   KONZEPT: Der ESP32 verbindet sich mit einem bestehenden WLAN (Schule, zu
   Hause, ...) und sendet dessen Internetzugang unter einem EIGENEN, immer
   gleich benannten WLAN "WiFi Hotspot" weiter - wie ein Reise-Router/
   Repeater. Das Hotspot-WLAN selbst hat KEIN WLAN-Passwort; wer sich
   verbindet, landet auf einer eigenen Login-Seite und muss dort ein
   Zugangspasswort eingeben, um Internet zu bekommen. Die Verwaltung
   (welches WLAN wiederholt wird, Zugangspasswort, Admin-Login) ist ueber
   dasselbe WLAN unter /admin jederzeit erreichbar.

   Der ESP32 merkt sich mehrere WLANs, mit denen er schon einmal verbunden
   war, und sucht sich beim Start automatisch das gerade erreichbare davon
   aus - so funktioniert "WiFi Hotspot" an mehreren Orten (Schule, zu
   Hause, ...), ohne nach jedem Ortswechsel neu eingerichtet werden zu
   muessen.

   ARDUINO-IDE-EINSTELLUNGEN (Menue "Werkzeuge"):
     Board:              ESP32S3 Dev Module
     USB CDC On Boot:    Disabled   (wenn du den Port "UART" benutzt)
                         Enabled    (wenn du den Port "USB" benutzt)
     CPU Frequency:      240MHz (WiFi)
     Flash Mode:         QIO 80MHz
     Flash Size:         16MB (128Mb)
     Partition Scheme:   16M Flash (3MB APP/9.9MB FATFS)
     PSRAM:              OPI PSRAM                            <- wichtig!
     Upload Mode:        UART0 / Hardware CDC
     USB Mode:           Hardware CDC and JTAG
     Upload Speed:       921600 (bei Problemen 460800 oder 115200)
     Core Debug Level:   None

   BENOETIGT: Board-Paket "esp32 by Espressif Systems" ab Version 3.3.12
   (Boardverwalter). Es sind KEINE weiteren Bibliotheken noetig.

   NUTZUNG:
     WLAN "WiFi Hotspot" (offen, kein WLAN-Passwort) -> Browser oeffnet
     automatisch die Login-Seite, dort Zugangspasswort eingeben (Standard:
     siehe DEFAULT_ACCESS_PASS unten).
     Verwaltung: http://192.168.4.1/admin , Login admin / admin123
     (bitte beides nach dem ersten Start aendern!)

   NUR FUER EIGENE NETZE UND GERAETE: Dieses Projekt enthaelt bewusst KEINE
   Funktionen zum Stoeren, Trennen oder Angreifen fremder WLANs/Geraete
   (kein Jamming, keine Deauth, kein Beacon-Flooding, kein Evil Twin). Das
   eigene Hotspot-WLAN ist klar als eigenes Geraet erkennbar benannt - es
   gibt sich nicht als ein echter, dritter Anbieter/Dienst aus.
  ============================================================================
*/

// ============================================================================
//  1) EINSTELLUNGEN, DIE DU ANPASSEN KANNST
// ============================================================================

#define FIRMWARE_VERSION      "2.0.0"
#define FIRMWARE_NAME         "WiFi Hotspot"
#define BOARD_NAME            "ESP32-S3 N16R8"

// --- Simulator (Velxio, Wokwi ...): dort gibt es keinen erreichbaren Access Point.
// Trage hier das Simulator-WLAN ein, dann verbindet sich der ESP32 als Station
// damit (kein eigener AP) und zeigt im Seriellen Monitor die klickbare IP-Adresse.
#define SIMULATOR_WIFI_SSID   ""
#define SIMULATOR_WIFI_PASS   ""

// --- Standardwerte beim ersten Start / nach Werkseinstellungen ---------------
#define DEFAULT_AP_SSID       "WiFi Hotspot"   // immer gleich - offenes WLAN, kein Passwort
#define DEFAULT_AP_CHANNEL    6                 // 1..11
#define DEFAULT_ACCESS_PASS   "hotspot123"      // Zugangspasswort auf der Login-Seite
#define DEFAULT_WEB_USER      "admin"
#define DEFAULT_WEB_PASS      "admin123"
#define MDNS_NAME             "wifi-hotspot"    // erreichbar als wifi-hotspot.local

// ============================================================================
//  2) BIBLIOTHEKEN (alle im ESP32-Board-Paket enthalten)
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "Bitte im Boardverwalter 'esp32 by Espressif Systems' Version 3.3.12 oder neuer installieren."
#endif

// ============================================================================
//  3) KONSTANTEN
// ============================================================================

#define MAX_WIFI_NETS   40   // Ergebnisse eines Scans (zum Hinzufuegen eines WLANs)
#define MAX_KNOWN        6   // gespeicherte WLANs, die wiederholt werden koennen
#define MAX_ACCESS      20   // gleichzeitig angemeldete Geraete am Hotspot
#define MAX_SESSIONS     4   // Admin-Sitzungen
#define AP_MAX_CLIENTS   8
#define SESSION_TTL_MS  (12UL * 3600UL * 1000UL)
#define ACCESS_TTL_S    (12UL * 3600UL)
#define STA_TRY_MS      12000UL
#define STA_CYCLE_PAUSE_MS 20000UL

// Vorwaertsdeklaration der Weboberflaeche (steht ganz unten in dieser Datei)
extern const char ADMIN_HTML[] PROGMEM;
extern const char GUEST_HTML[] PROGMEM;

// ============================================================================
//  4) DATENSTRUKTUREN UND GLOBALE VARIABLEN
// ============================================================================

struct Config {
  char apSsid[33];
  uint8_t apCh;
  char accessPass[65];
  bool accessDefault;   // Zugangspasswort ist noch das Standardpasswort
  char webUser[33];
  char webSalt[17];
  char webHash[65];
  bool authEnabled;
  bool pwDefault;        // Admin-Passwort ist noch das Standardpasswort
};

struct WifiNet {
  char ssid[33];
  char bssid[18];
  int8_t rssi;
  uint8_t ch;
  uint8_t auth;
  bool hidden;
};

struct KnownNet {
  char ssid[33];
  char pass[65];
};

struct AccessAuth {
  uint32_t ip;
  uint32_t expiresAt;   // uptimeSec()
};

struct Session {
  char tok[33];
  uint32_t last;
};

enum StaState { STA_IDLE = 0, STA_CONNECTING, STA_CONNECTED };

static Config g_cfg;
static WebServer g_server(80);
static Preferences g_prefs;

static Session g_sess[MAX_SESSIONS];
static uint8_t g_failCount = 0;
static uint32_t g_lockUntil = 0;

// Access Point ("WiFi Hotspot" - immer an, offen, kein WLAN-Passwort)
static bool g_apUp = false;
static uint32_t g_apRestartAt = 0;
static uint32_t g_restartAt = 0;

// WLAN-Scan (nur zum Hinzufuegen eines zu wiederholenden WLANs)
static WifiNet g_nets[MAX_WIFI_NETS];
static int g_netN = 0;
static uint8_t g_scanState = 0;   // 0 idle, 1 laeuft, 2 fertig
static uint32_t g_scanStartMs = 0;

// Bekannte WLANs, die wiederholt werden ("merkt sich, wo es schon war")
static KnownNet g_known[MAX_KNOWN];
static uint8_t g_knownN = 0;
static uint8_t g_knownTryIdx = 0;
static uint8_t g_knownCycleStart = 0;

// Upstream-Verbindung (Station)
static uint8_t g_staState = STA_IDLE;
static uint32_t g_staT0 = 0;
static uint32_t g_staRetryAt = 0;

// NAT: Internet vom Upstream-WLAN ueber den eigenen Hotspot teilen
static bool g_routerActive = false;
static TaskHandle_t g_dnsTaskHandle = NULL;

// Angemeldete Geraete am Hotspot (haben das Zugangspasswort eingegeben)
static AccessAuth g_access[MAX_ACCESS];

static char g_notice[192] = "";

// ============================================================================
//  5) KLEINE HILFSFUNKTIONEN
// ============================================================================

static uint32_t uptimeSec() {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static void addLog(uint8_t lvl, const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%lu] %s\n", (unsigned long)uptimeSec(), buf);
  (void)lvl;
}

static void macToStr(const uint8_t *mac, char *out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
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

static uint32_t requesterIp() {
  return (uint32_t)g_server.client().remoteIP();
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
  g_prefs.putUChar("ap_ch", g_cfg.apCh);
}

static void saveAccessCfg() {
  g_prefs.putString("acc_pass", g_cfg.accessPass);
  g_prefs.putBool("acc_def", g_cfg.accessDefault);
}

static void saveKnown() {
  g_prefs.putUChar("knownN", g_knownN);
  char k[8];
  for (int i = 0; i < g_knownN; i++) {
    snprintf(k, sizeof(k), "k%ds", i);
    g_prefs.putString(k, g_known[i].ssid);
    snprintf(k, sizeof(k), "k%dp", i);
    g_prefs.putString(k, g_known[i].pass);
  }
}

static void loadKnown() {
  g_knownN = g_prefs.getUChar("knownN", 0);
  if (g_knownN > MAX_KNOWN) g_knownN = MAX_KNOWN;
  char k[8];
  String s;
  for (int i = 0; i < g_knownN; i++) {
    snprintf(k, sizeof(k), "k%ds", i);
    s = g_prefs.getString(k, "");
    strlcpy(g_known[i].ssid, s.c_str(), sizeof(g_known[i].ssid));
    snprintf(k, sizeof(k), "k%dp", i);
    s = g_prefs.getString(k, "");
    strlcpy(g_known[i].pass, s.c_str(), sizeof(g_known[i].pass));
  }
}

static void loadConfig() {
  g_prefs.begin("hotspot", false);
  String s;

  s = g_prefs.getString("ap_ssid", DEFAULT_AP_SSID);
  if (s.length() < 1 || s.length() > 32) s = DEFAULT_AP_SSID;
  strlcpy(g_cfg.apSsid, s.c_str(), sizeof(g_cfg.apSsid));

  g_cfg.apCh = g_prefs.getUChar("ap_ch", DEFAULT_AP_CHANNEL);
  if (g_cfg.apCh < 1 || g_cfg.apCh > 11) g_cfg.apCh = DEFAULT_AP_CHANNEL;

  s = g_prefs.getString("acc_pass", DEFAULT_ACCESS_PASS);
  if (s.length() < 4 || s.length() > 63) s = DEFAULT_ACCESS_PASS;
  strlcpy(g_cfg.accessPass, s.c_str(), sizeof(g_cfg.accessPass));
  g_cfg.accessDefault = g_prefs.getBool("acc_def", true);

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

  loadKnown();
}

// ============================================================================
//  7) ADMIN-LOGIN / SITZUNGEN
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
//  8) GERAETE-ZUGANG ZUM HOTSPOT (Login-Seite mit Zugangspasswort)
// ============================================================================

static bool isAccessAuthorized(uint32_t ip) {
  uint32_t now = uptimeSec();
  for (int i = 0; i < MAX_ACCESS; i++)
    if (g_access[i].ip == ip && (int32_t)(now - g_access[i].expiresAt) < 0) return true;
  return false;
}

static void addAccessAuth(uint32_t ip) {
  uint32_t now = uptimeSec();
  int slot = -1;
  for (int i = 0; i < MAX_ACCESS; i++) {
    if (g_access[i].ip == ip) { slot = i; break; }
    if (slot < 0 && (int32_t)(now - g_access[i].expiresAt) >= 0) slot = i;  // abgelaufener Platz
  }
  if (slot < 0) slot = 0;  // Not-Ausweg: aeltesten Platz ueberschreiben
  g_access[slot].ip = ip;
  g_access[slot].expiresAt = now + ACCESS_TTL_S;
}

static void removeAccessAuth(uint32_t ip) {
  for (int i = 0; i < MAX_ACCESS; i++)
    if (g_access[i].ip == ip) g_access[i].ip = 0;
}

// ============================================================================
//  9) ACCESS POINT ("WiFi Hotspot")
// ============================================================================

static bool applyAp(const char *ssid, int ch) {
  WiFi.enableAP(true);
  bool ok = WiFi.softAP(ssid, "", ch, 0, AP_MAX_CLIENTS);  // kein Passwort - offenes WLAN
  g_apUp = ok;
  return ok;
}

static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
      char m[18];
      macToStr(info.wifi_ap_staconnected.mac, m);
      addLog(0, "Gerät verbunden: %s", m);
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

// ============================================================================
//  10) UPSTREAM-WLAN: bekannte Netze automatisch wiederholen
// ============================================================================
//  Der ESP32 merkt sich mehrere WLANs (z.B. Schule, zu Hause). Beim Start -
//  und immer, wenn die Verbindung abreisst - probiert er der Reihe nach alle
//  bekannten WLANs durch, bis eines erreichbar ist, und wiederholt dessen
//  Internetzugang dann ueber den eigenen Hotspot.

static const char *staStateName() {
  switch (g_staState) {
    case STA_CONNECTING: return "connecting";
    case STA_CONNECTED: return "connected";
    default: return "idle";
  }
}

static void staConnectKnown(uint8_t idx) {
  if (idx >= g_knownN) return;
  g_knownTryIdx = idx;
  WiFi.setAutoReconnect(false);
  WiFi.begin(g_known[idx].ssid, g_known[idx].pass[0] ? g_known[idx].pass : NULL);
  g_staState = STA_CONNECTING;
  g_staT0 = millis();
  addLog(0, "Verbinde mit '%s' ...", g_known[idx].ssid);
}

static void staStartSearch() {
  if (g_knownN == 0) { g_staState = STA_IDLE; return; }
  g_knownCycleStart = 0;
  staConnectKnown(0);
}

static void staTryNext() {
  if (g_knownN == 0) { g_staState = STA_IDLE; g_staRetryAt = millis() + STA_CYCLE_PAUSE_MS; return; }
  uint8_t next = (uint8_t)((g_knownTryIdx + 1) % g_knownN);
  if (next == g_knownCycleStart) {
    // einmal komplett durch, keines erreichbar - kurze Pause vor dem naechsten Versuch
    g_staState = STA_IDLE;
    g_staRetryAt = millis() + STA_CYCLE_PAUSE_MS;
    return;
  }
  staConnectKnown(next);
}

static void tickSta() {
  switch (g_staState) {
    case STA_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        g_staState = STA_CONNECTED;
        addLog(0, "Verbunden mit '%s', IP %s", g_known[g_knownTryIdx].ssid, WiFi.localIP().toString().c_str());
      } else if ((uint32_t)(millis() - g_staT0) > STA_TRY_MS) {
        WiFi.disconnect(false, false);
        addLog(1, "'%s' nicht erreichbar", g_known[g_knownTryIdx].ssid);
        staTryNext();
      }
      break;
    case STA_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        addLog(1, "Verbindung zu '%s' verloren", g_known[g_knownTryIdx].ssid);
        staTryNext();
      }
      break;
    case STA_IDLE:
      if (g_knownN > 0 && (int32_t)(millis() - g_staRetryAt) >= 0) staStartSearch();
      break;
  }
}

// ============================================================================
//  11) INTERNET TEILEN (NAT) UND EIGENER DNS-SERVER (Captive Portal)
// ============================================================================
//  NAT/IP-Weiterleitung zwischen dem Upstream-WLAN (Station) und dem eigenen
//  Access Point - moeglich, weil das ESP32-Boardpaket lwIP bereits mit
//  aktivierter NAPT-Unterstuetzung mitbringt (kein Custom-Build noetig).
//  Internet bekommt nur, wer auf der Login-Seite das Zugangspasswort
//  eingegeben hat UND der Upstream gerade verbunden ist - alle anderen
//  Anfragen zeigen per DNS immer zur eigenen Login-Seite (klassisches
//  Captive Portal).

static bool routableNow() {
  return g_staState == STA_CONNECTED;
}

// Wichtig: Der DNS-Server, den der eigene Access Point per DHCP an Geraete
// verteilt, bleibt IMMER die eigene IP-Adresse (Standardverhalten von
// WiFi.softAP()) - Geraete fragen also immer unseren eigenen dnsTask() und
// NIE direkt den echten Upstream-DNS-Server. So laeuft die Namensaufloesung
// ueber unseren eigenen, zuverlaessigen Relay (dnsForward(), eine normale
// Anfrage des ESP32 selbst) statt ueber NAT/NAPT fuer UDP-Pakete - NAPT
// wird dadurch nur fuer die eigentlichen Verbindungen (TCP/HTTPS) benoetigt.
static void applyRouterMode(bool on) {
  esp_netif_t *apNif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  esp_netif_t *staNif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!apNif) return;
  if (on) {
    if (staNif) esp_netif_set_default_netif(staNif);
    if (esp_netif_napt_enable(apNif) == ESP_OK) {
      g_routerActive = true;
      addLog(0, "Internet von '%s' wird geteilt", g_known[g_knownTryIdx].ssid);
    } else {
      addLog(2, "Internet teilen fehlgeschlagen (NAPT)");
    }
  } else {
    if (g_routerActive) esp_netif_napt_disable(apNif);
    g_routerActive = false;
  }
}

static void tickRouter() {
  bool want = routableNow();
  if (want != g_routerActive) applyRouterMode(want);
}

static void dnsSendA(WiFiUDP &udp, const uint8_t *req, int len, IPAddress answer, IPAddress dst, uint16_t dstPort) {
  if (len < 17 || len > 480) return;
  int i = 12;
  while (i < len && req[i] != 0) i += req[i] + 1;
  if (i >= len) return;
  int qNameLen = i - 12 + 1;
  if (12 + qNameLen + 4 > len) return;
  uint8_t resp[512];
  int rl = 0;
  resp[rl++] = req[0]; resp[rl++] = req[1];  // Transaktions-ID uebernehmen
  resp[rl++] = 0x81; resp[rl++] = 0x80;      // Antwort, Rekursion verfuegbar
  resp[rl++] = req[4]; resp[rl++] = req[5];  // QDCOUNT uebernehmen
  resp[rl++] = 0; resp[rl++] = 1;            // ANCOUNT = 1
  resp[rl++] = 0; resp[rl++] = 0;
  resp[rl++] = 0; resp[rl++] = 0;
  memcpy(resp + rl, req + 12, qNameLen + 4);
  rl += qNameLen + 4;
  resp[rl++] = 0xC0; resp[rl++] = 0x0C;  // Namenszeiger auf Offset 12
  resp[rl++] = 0; resp[rl++] = 1;        // TYPE A
  resp[rl++] = 0; resp[rl++] = 1;        // CLASS IN
  resp[rl++] = 0; resp[rl++] = 0; resp[rl++] = 0; resp[rl++] = 30;  // TTL 30s
  resp[rl++] = 0; resp[rl++] = 4;        // RDLENGTH
  resp[rl++] = answer[0]; resp[rl++] = answer[1]; resp[rl++] = answer[2]; resp[rl++] = answer[3];
  udp.beginPacket(dst, dstPort);
  udp.write(resp, rl);
  udp.endPacket();
}

// Reicht eine DNS-Anfrage unveraendert an den echten DNS-Server weiter und
// schickt die Antwort an den urspruenglichen Absender zurueck. Liefert true,
// wenn eine Antwort kam (und damit an den Client weitergeschickt wurde).
static bool dnsForward(WiFiUDP &udp, const uint8_t *req, int len, IPAddress upstream, IPAddress dst, uint16_t dstPort) {
  WiFiUDP fwd;
  if (!fwd.begin(0)) return false;
  fwd.beginPacket(upstream, 53);
  fwd.write(req, len);
  fwd.endPacket();
  uint32_t t0 = millis();
  int rl = 0;
  while ((millis() - t0) < 700UL) {
    rl = fwd.parsePacket();
    if (rl > 0) break;
    delay(5);
  }
  bool ok = false;
  if (rl > 0 && rl <= 512) {
    uint8_t rbuf[512];
    int n = fwd.read(rbuf, rl);
    if (n > 0) {
      udp.beginPacket(dst, dstPort);
      udp.write(rbuf, n);
      udp.endPacket();
      ok = true;
    }
  }
  fwd.stop();
  return ok;
}

static void dnsTask(void *arg) {
  WiFiUDP udp;
  if (!udp.begin(53)) {
    addLog(2, "DNS-Server konnte nicht gestartet werden");
    vTaskDelete(NULL);
    return;
  }
  // Fallback, falls das Upstream-WLAN per DHCP keinen (oder einen nicht
  // erreichbaren) DNS-Server mitteilt - sonst wuerden Namen nie aufgeloest,
  // obwohl die eigentliche Internetverbindung (NAT) funktioniert.
  const IPAddress FALLBACK_DNS(1, 1, 1, 1);
  uint8_t buf[512];
  for (;;) {
    int len = udp.parsePacket();
    if (len > 0) {
      if (len > (int)sizeof(buf)) len = sizeof(buf);
      int n = udp.read(buf, len);
      IPAddress src = udp.remoteIP();
      uint16_t sport = udp.remotePort();
      if (n > 0) {
        bool allowed = routableNow() && isAccessAuthorized((uint32_t)src);
        if (allowed) {
          IPAddress up = WiFi.dnsIP();
          bool done = ((uint32_t)up != 0) && dnsForward(udp, buf, n, up, src, sport);
          if (!done) dnsForward(udp, buf, n, FALLBACK_DNS, src, sport);
        } else {
          dnsSendA(udp, buf, n, WiFi.softAPIP(), src, sport);
        }
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

static void startDnsTask() {
  if (g_dnsTaskHandle) return;
  xTaskCreatePinnedToCore(dnsTask, "dns", 5120, NULL, 1, &g_dnsTaskHandle, 0);
}

// ============================================================================
//  12) WLAN-SCAN (nur zum Hinzufuegen eines zu wiederholenden WLANs)
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

static bool wifiScanStart() {
  if (g_scanState == 1) return true;
  g_scanStartMs = millis();
  WiFi.scanDelete();
  int16_t r = WiFi.scanNetworks(true, true, false, 300);
  if (r == WIFI_SCAN_FAILED) return false;
  g_scanState = 1;
  addLog(0, "WLAN-Scan gestartet");
  return true;
}

static void wifiScanStop() {
  if (g_scanState != 1) return;
  esp_wifi_scan_stop();
  WiFi.scanDelete();
  g_scanState = g_netN > 0 ? 2 : 0;
  addLog(0, "WLAN-Scan gestoppt");
}

static void tickWifiScan() {
  if (g_scanState != 1) return;
  int16_t n = WiFi.scanComplete();
  if (n >= 0) {
    copyScanResults(n);
    g_scanState = 2;
    addLog(0, "%d Netzwerke gefunden", g_netN);
    WiFi.scanDelete();
  } else if (n == WIFI_SCAN_FAILED || (uint32_t)(millis() - g_scanStartMs) > 25000UL) {
    WiFi.scanDelete();
    g_scanState = g_netN > 0 ? 2 : 0;
    addLog(2, "WLAN-Scan fehlgeschlagen");
  }
}

// ============================================================================
//  13) WEBSERVER - KLEINE HELFER
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

// ============================================================================
//  14) API: OEFFENTLICH (Login-Seite, kein Admin-Login noetig)
// ============================================================================

static void handlePublic() {
  String o = "{";
  jS(o, "name", FIRMWARE_NAME);
  jS(o, "fw", FIRMWARE_VERSION);
  jB(o, "auth", g_cfg.authEnabled);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleAccessInfo() {
  String o = "{";
  jS(o, "title", FIRMWARE_NAME);
  jB(o, "online", isAccessAuthorized(requesterIp()));
  jB(o, "internet", routableNow());
  jClose(o, '}');
  sendJson(200, o);
}

static void handleAccessLogin() {
  uint32_t now = millis();
  if (g_lockUntil && (int32_t)(now - g_lockUntil) < 0) {
    sendErr(429, "Zu viele Versuche - bitte kurz warten.");
    return;
  }
  String p = g_server.arg("pass");
  if (!constEq(p.c_str(), g_cfg.accessPass)) {
    if (++g_failCount >= 8) {
      g_lockUntil = now + 30000UL;
      g_failCount = 0;
    }
    sendErr(401, "Falsches Zugangspasswort");
    return;
  }
  g_failCount = 0;
  g_lockUntil = 0;
  addAccessAuth(requesterIp());
  addLog(0, "Gerät am Hotspot angemeldet");
  sendOk("Verbunden.");
}

static void handleAccessLogout() {
  removeAccessAuth(requesterIp());
  sendOk("");
}

// ============================================================================
//  15) API: ADMIN-LOGIN
// ============================================================================

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
  addLog(0, "Admin-Login erfolgreich");
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

// ============================================================================
//  16) API: ADMIN - STATUS UND EINSTELLUNGEN
// ============================================================================

static void handleStatus() {
  NEED_AUTH();
  String o;
  o.reserve(1200);
  o = "{";
  jS(o, "fw", FIRMWARE_VERSION);
  jS(o, "board", BOARD_NAME);
  jI(o, "up", uptimeSec());
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
  jClose(o, '}');
  o += ',';

  jKey(o, "mem");
  o += '{';
  jI(o, "heap", ESP.getHeapSize());
  jI(o, "free", ESP.getFreeHeap());
  jI(o, "psram", ESP.getPsramSize());
  jI(o, "flash", ESP.getFlashChipSize());
  jClose(o, '}');
  o += ',';

  jKey(o, "wifi");
  o += '{';
  jB(o, "ap", g_apUp);
  jS(o, "apSsid", g_cfg.apSsid);
  jI(o, "apCh", g_apUp ? WiFi.channel() : g_cfg.apCh);
  jS(o, "apIp", WiFi.softAPIP().toString().c_str());
  jI(o, "clients", WiFi.softAPgetStationNum());
  jKey(o, "repeat");
  o += '{';
  jS(o, "ssid", g_knownN > 0 ? g_known[g_knownTryIdx].ssid : "");
  jS(o, "state", staStateName());
  jI(o, "rssi", g_staState == STA_CONNECTED ? WiFi.RSSI() : 0);
  jB(o, "internet", routableNow());
  jClose(o, '}');
  jClose(o, '}');
  o += ',';

  jKey(o, "warn");
  o += '{';
  jB(o, "defaultWeb", g_cfg.pwDefault);
  jB(o, "defaultAccess", g_cfg.accessDefault);
  jClose(o, '}');
  jClose(o, '}');
  sendJson(200, o);
}

static void handleSettingsGet() {
  NEED_AUTH();
  String o = "{";
  jS(o, "apSsid", g_cfg.apSsid);
  jI(o, "apCh", g_cfg.apCh);
  jS(o, "accessPass", g_cfg.accessPass);
  jB(o, "accessDefault", g_cfg.accessDefault);
  jS(o, "webUser", g_cfg.webUser);
  jB(o, "authEnabled", g_cfg.authEnabled);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleApSave() {
  NEED_AUTH();
  String ssid = g_server.arg("ssid");
  int ch = g_server.arg("ch").toInt();
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() > 32) { sendErr(400, "Name muss 1 bis 32 Zeichen lang sein"); return; }
  if (ch < 1 || ch > 11) { sendErr(400, "Kanal muss zwischen 1 und 11 liegen"); return; }
  strlcpy(g_cfg.apSsid, ssid.c_str(), sizeof(g_cfg.apSsid));
  g_cfg.apCh = (uint8_t)ch;
  saveApCfg();
  addLog(0, "Einstellungen gespeichert (WiFi Hotspot)");
  g_apRestartAt = millis() + 800UL;
  sendOk("Gespeichert. Das WLAN wird gleich neu gestartet.");
}

static void handleAccessSave() {
  NEED_AUTH();
  String p = g_server.arg("pass");
  if (p.length() < 4 || p.length() > 63) { sendErr(400, "Zugangspasswort muss 4 bis 63 Zeichen lang sein"); return; }
  strlcpy(g_cfg.accessPass, p.c_str(), sizeof(g_cfg.accessPass));
  g_cfg.accessDefault = false;
  saveAccessCfg();
  addLog(0, "Zugangspasswort geändert");
  sendOk("Gespeichert.");
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
  addLog(0, "Einstellungen gespeichert (Admin-Zugang)");
  sendOk("Gespeichert.");
}

// ============================================================================
//  17) API: ADMIN - BEKANNTE WLANS (zum Wiederholen)
// ============================================================================

static void handleKnownGet() {
  NEED_AUTH();
  String o = "{\"nets\":[";
  for (int i = 0; i < g_knownN; i++) {
    o += '{';
    jS(o, "ssid", g_known[i].ssid);
    jB(o, "active", (uint8_t)i == g_knownTryIdx && g_staState != STA_IDLE);
    jClose(o, '}');
    o += ',';
  }
  jClose(o, ']');
  o += ',';
  jI(o, "max", MAX_KNOWN);
  jClose(o, '}');
  sendJson(200, o);
}

static void handleKnownAdd() {
  NEED_AUTH();
  String ssid = g_server.arg("ssid");
  String pass = g_server.arg("pass");
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() > 32) { sendErr(400, "Name muss 1 bis 32 Zeichen lang sein"); return; }
  if (pass.length() > 63) { sendErr(400, "Passwort zu lang"); return; }
  int idx = -1;
  for (int i = 0; i < g_knownN; i++)
    if (strcmp(g_known[i].ssid, ssid.c_str()) == 0) { idx = i; break; }
  if (idx < 0) {
    if (g_knownN >= MAX_KNOWN) { sendErr(400, "Maximale Anzahl WLANs erreicht - bitte zuerst eins entfernen"); return; }
    idx = g_knownN++;
  }
  strlcpy(g_known[idx].ssid, ssid.c_str(), sizeof(g_known[idx].ssid));
  strlcpy(g_known[idx].pass, pass.c_str(), sizeof(g_known[idx].pass));
  saveKnown();
  addLog(0, "WLAN zum Wiederholen hinzugefügt: %s", g_known[idx].ssid);
  staConnectKnown((uint8_t)idx);
  sendOk("Gespeichert. Verbinde...");
}

static void handleKnownRemove() {
  NEED_AUTH();
  String ssid = g_server.arg("ssid");
  int idx = -1;
  for (int i = 0; i < g_knownN; i++)
    if (strcmp(g_known[i].ssid, ssid.c_str()) == 0) { idx = i; break; }
  if (idx < 0) { sendErr(404, "WLAN nicht gefunden"); return; }
  bool wasActive = ((uint8_t)idx == g_knownTryIdx && g_staState != STA_IDLE);
  for (int j = idx; j < g_knownN - 1; j++) g_known[j] = g_known[j + 1];
  g_knownN--;
  saveKnown();
  addLog(0, "WLAN entfernt: %s", ssid.c_str());
  if (wasActive) {
    WiFi.disconnect(false, false);
    staStartSearch();
  }
  sendOk("");
}

// ============================================================================
//  18) API: ADMIN - WLAN-SCAN (zum Hinzufuegen eines neuen WLANs)
// ============================================================================

static void handleWifiScanGet() {
  NEED_AUTH();
  String o;
  o.reserve(400 + g_netN * 150);
  o = "{";
  jS(o, "state", g_scanState == 1 ? "running" : (g_scanState == 2 ? "done" : "idle"));
  jI(o, "count", g_netN);
  jKey(o, "nets");
  o += '[';
  for (int i = 0; i < g_netN; i++) {
    const WifiNet &n = g_nets[i];
    o += '{';
    jS(o, "ssid", n.ssid);
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
  if (!wifiScanStart()) { sendErr(500, "Scan konnte nicht gestartet werden"); return; }
  sendOk("");
}

static void handleWifiScanStop() {
  NEED_AUTH();
  wifiScanStop();
  sendOk("");
}

// ============================================================================
//  19) API: ADMIN - SYSTEM
// ============================================================================

static void handleNoticeDismiss() {
  NEED_AUTH();
  g_notice[0] = 0;
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
  addLog(1, "Werkseinstellungen wiederhergestellt");
  sendOk("");
  g_restartAt = millis() + 1000UL;
}

// ============================================================================
//  20) SEITEN UND CAPTIVE PORTAL
// ============================================================================

static void redirectToRoot() {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  g_server.send(302, "text/plain", "");
}

static void handleRoot() {
  g_server.sendHeader("Cache-Control", "no-cache");
  g_server.send_P(200, "text/html; charset=utf-8", GUEST_HTML);
}

// Verwaltung ist hier immer erreichbar, unabhaengig vom Zugangspasswort.
static void handleAdmin() {
  g_server.sendHeader("Cache-Control", "no-cache");
  g_server.send_P(200, "text/html; charset=utf-8", ADMIN_HTML);
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
  g_server.on("/admin", HTTP_GET, handleAdmin);
  g_server.on("/favicon.ico", HTTP_GET, []() { g_server.send(204, "text/plain", ""); });

  // Captive-Portal-Erkennung von Android, iOS, Windows, Firefox
  static const char *cp[] = {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html", "/connecttest.txt",
                             "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt", "/fwlink"};
  for (size_t i = 0; i < sizeof(cp) / sizeof(cp[0]); i++) g_server.on(cp[i], HTTP_ANY, redirectToRoot);

  g_server.on("/api/public", HTTP_GET, handlePublic);
  g_server.on("/api/access/info", HTTP_GET, handleAccessInfo);
  g_server.on("/api/access/login", HTTP_POST, handleAccessLogin);
  g_server.on("/api/access/logout", HTTP_POST, handleAccessLogout);

  g_server.on("/api/login", HTTP_POST, handleLogin);
  g_server.on("/api/logout", HTTP_POST, handleLogout);
  g_server.on("/api/status", HTTP_GET, handleStatus);

  g_server.on("/api/settings", HTTP_GET, handleSettingsGet);
  g_server.on("/api/settings/web", HTTP_POST, handleSettingsWeb);
  g_server.on("/api/ap/save", HTTP_POST, handleApSave);
  g_server.on("/api/access/save", HTTP_POST, handleAccessSave);

  g_server.on("/api/known", HTTP_GET, handleKnownGet);
  g_server.on("/api/known/add", HTTP_POST, handleKnownAdd);
  g_server.on("/api/known/remove", HTTP_POST, handleKnownRemove);

  g_server.on("/api/wifi/scan", HTTP_GET, handleWifiScanGet);
  g_server.on("/api/wifi/scan/start", HTTP_POST, handleWifiScanStart);
  g_server.on("/api/wifi/scan/stop", HTTP_POST, handleWifiScanStop);

  g_server.on("/api/notice/dismiss", HTTP_POST, handleNoticeDismiss);
  g_server.on("/api/system/restart", HTTP_POST, handleRestart);
  g_server.on("/api/system/factory", HTTP_POST, handleFactory);

  g_server.onNotFound(handleNotFound);
}

// ============================================================================
//  21) SETUP UND LOOP
// ============================================================================

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // nicht blockieren, wenn kein PC am USB haengt
#endif
  delay(200);
  addLog(0, "System gestartet (v%s)", FIRMWARE_VERSION);

  loadConfig();

  WiFi.persistent(false);  // WLAN-Zugangsdaten verwalten wir selbst (Preferences)
  WiFi.setHostname(MDNS_NAME);
  WiFi.onEvent(onWifiEvent);

  if (SIMULATOR_WIFI_SSID[0]) {
    // Simulator-Betrieb: nur Station, kein Access Point
    WiFi.mode(WIFI_STA);
    strlcpy(g_known[0].ssid, SIMULATOR_WIFI_SSID, sizeof(g_known[0].ssid));
    strlcpy(g_known[0].pass, SIMULATOR_WIFI_PASS, sizeof(g_known[0].pass));
    g_knownN = 1;
    addLog(1, "SIMULATOR-MODUS: verbinde mit '%s' (kein Access Point)", SIMULATOR_WIFI_SSID);
    staStartSearch();
  } else {
    WiFi.mode(WIFI_AP_STA);
    if (applyAp(g_cfg.apSsid, g_cfg.apCh)) {
      addLog(0, "WiFi Hotspot gestartet: %s", g_cfg.apSsid);
    } else {
      addLog(2, "WiFi Hotspot konnte nicht gestartet werden");
    }
    staStartSearch();
  }

  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);

  startDnsTask();
  setupRoutes();
  g_server.begin();
  addLog(0, "Webserver bereit (http://192.168.4.1)");
  if (g_cfg.accessDefault) addLog(1, "Standard-Zugangspasswort in Benutzung - bitte ändern");
  if (g_cfg.pwDefault) addLog(1, "Standard-Admin-Passwort in Benutzung - bitte ändern");
}

void loop() {
  g_server.handleClient();
  tickWifiScan();
  tickSta();
  tickRouter();

  if (g_apRestartAt && (int32_t)(millis() - g_apRestartAt) >= 0) {
    g_apRestartAt = 0;
    if (applyAp(g_cfg.apSsid, g_cfg.apCh)) addLog(0, "WiFi Hotspot gestartet: %s", g_cfg.apSsid);
  }

  if (g_restartAt && (int32_t)(millis() - g_restartAt) >= 0) {
    g_restartAt = 0;
    delay(100);
    ESP.restart();
  }
  delay(2);
}

// ============================================================================
//  22) WEBOBERFLAECHE (HTML, CSS, JavaScript) - liegt im Flash des ESP32
// ============================================================================
const char ADMIN_HTML[] PROGMEM =
"<!DOCTYPE html>\n"
"<html lang=\"de\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n"
"<meta name=\"theme-color\" content=\"#20242b\">\n"
"<title>Verwaltung - WiFi Hotspot</title>\n"
"<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Ccircle cx='12' cy='12' r='11' fill='%232f5d50'/%3E%3Cpath d='M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0' stroke='white' stroke-width='2' fill='none' stroke-linecap='round'/%3E%3Ccircle cx='12' cy='19' r='1.4' fill='white'/%3E%3C/svg%3E\">\n"
"<style>\n"
":root{--ink:#20242b;--paper:#f3efe6;--card:#fffdf8;--line:#c9c2b4;--mut:#6b6457;--accent:#2f5d50;--accent2:#24473d;--err:#8a2f2f;--ok:#2f5d50;--warn:#8a5a16}\n"
"*{box-sizing:border-box}\n"
"html,body{margin:0;background:var(--paper);color:var(--ink);font:15px/1.5 -apple-system,\"Segoe UI\",Roboto,Arial,sans-serif;-webkit-text-size-adjust:100%}\n"
"h1,h2,h3{font-family:Georgia,\"Times New Roman\",serif;margin:0;font-weight:600}\n"
"header{background:var(--ink);color:#f3efe6;padding:14px 20px;display:flex;align-items:center;gap:12px;border-bottom:3px solid var(--accent);position:sticky;top:0;z-index:5}\n"
"header .ic{width:24px;height:24px;fill:none;stroke:#f3efe6;stroke-width:1.8;flex:none}\n"
"header h1{font-size:18px;flex:1}\n"
"header .sub{font-size:12px;color:#c9c2b4;font-family:inherit}\n"
"#lout{background:none;border:1px solid #f3efe6;color:#f3efe6;padding:7px 14px;border-radius:4px;cursor:pointer;font-size:13px}\n"
"#lout:hover{background:rgba(255,255,255,.1)}\n"
"main{max-width:760px;margin:0 auto;padding:22px 16px 60px}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:20px 22px;margin-bottom:18px}\n"
".card h2{font-size:17px;margin-bottom:4px}\n"
".card .hint{color:var(--mut);font-size:13px;margin:4px 0 14px}\n"
"label{display:block;font-size:12.5px;color:var(--mut);margin:14px 0 6px;font-weight:600;text-transform:uppercase;letter-spacing:.3px}\n"
"label:first-of-type{margin-top:0}\n"
"input[type=text],input[type=password],select{width:100%;max-width:340px;padding:10px 11px;border:1px solid var(--line);border-radius:4px;font-size:15px;background:#fff;color:var(--ink)}\n"
"input:focus,select:focus{outline:2px solid var(--accent);outline-offset:1px}\n"
".bar{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-top:14px}\n"
"button.btn{padding:10px 16px;border:1px solid var(--accent2);border-radius:4px;background:var(--accent);color:#fff;font-size:14px;font-weight:600;cursor:pointer}\n"
"button.btn:hover{background:var(--accent2)}\n"
"button.btn.ghost{background:#fff;color:var(--ink);border-color:var(--line)}\n"
"button.btn.ghost:hover{background:#f3efe6}\n"
"button.btn.dng{background:#fff;color:var(--err);border-color:var(--err)}\n"
"button.btn.dng:hover{background:#f6e9e9}\n"
"button.btn.sm{padding:6px 11px;font-size:12.5px}\n"
"button:disabled{opacity:.55;cursor:default}\n"
".kv{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;font-size:14px}\n"
".kv div:nth-child(odd){color:var(--mut)}\n"
".mono{font-family:ui-monospace,Consolas,monospace;font-size:13px}\n"
".tag{display:inline-flex;align-items:center;gap:6px;padding:3px 10px;border-radius:12px;font-size:12.5px;font-weight:600;background:#e7e2d4;color:var(--mut)}\n"
".tag.ok{background:#e4ede9;color:var(--ok)}\n"
".tag.err{background:#f6e9e9;color:var(--err)}\n"
".dot{width:7px;height:7px;border-radius:50%;background:var(--ok);display:inline-block}\n"
".dot.off{background:#a39b8a}\n"
".banner{display:flex;gap:10px;align-items:flex-start;padding:12px 14px;border-radius:4px;border:1px solid var(--warn);background:#f8efe0;color:#5b3c0c;font-size:13.5px;margin-bottom:16px}\n"
".banner b{display:block;margin-bottom:2px}\n"
".tbl{width:100%;border-collapse:collapse;font-size:14px}\n"
".tbl th{text-align:left;color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.3px;padding:0 8px 8px;border-bottom:1px solid var(--line)}\n"
".tbl td{padding:9px 8px;border-bottom:1px solid #e7e2d4}\n"
".empty{color:var(--mut);font-size:13.5px;padding:10px 0}\n"
".err-msg{margin-top:10px;padding:9px 12px;border:1px solid var(--err);border-radius:4px;background:#f6e9e9;color:var(--err);font-size:13.5px}\n"
".wlrow{display:flex;align-items:center;gap:10px;padding:9px 10px;border:1px solid var(--line);border-radius:4px;margin-bottom:6px;cursor:pointer;background:#fff}\n"
".wlrow:hover{background:#f3efe6}\n"
".wlrow b{flex:1;font-size:14px;font-weight:600}\n"
".wlrow .mut{color:var(--mut);font-size:12px}\n"
"#login{min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}\n"
".toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);background:var(--ink);color:#fff;padding:10px 18px;border-radius:5px;font-size:13.5px;z-index:50;box-shadow:0 2px 10px rgba(0,0,0,.25)}\n"
".toast.err{background:var(--err)}\n"
".hidden{display:none !important}\n"
"@media(max-width:480px){input[type=text],input[type=password],select{max-width:100%}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"\n"
"<div id=\"login\" class=\"hidden\">\n"
"<div class=\"card\" style=\"max-width:360px;width:100%\">\n"
"<h2 style=\"margin-bottom:4px\">WiFi Hotspot</h2>\n"
"<p class=\"hint\" style=\"margin-top:0\">Verwaltung</p>\n"
"<form id=\"lf\">\n"
"<label for=\"lu\">Benutzername</label><input id=\"lu\" type=\"text\" autocomplete=\"username\">\n"
"<label for=\"lp\">Passwort</label><input id=\"lp\" type=\"password\" autocomplete=\"current-password\">\n"
"<div id=\"lerr\" class=\"err-msg hidden\"></div>\n"
"<button class=\"btn\" type=\"submit\" style=\"margin-top:16px;width:100%\">Anmelden</button>\n"
"</form>\n"
"</div>\n"
"</div>\n"
"\n"
"<div id=\"app\" class=\"hidden\">\n"
"<header>\n"
"<svg class=\"ic\" viewBox=\"0 0 24 24\"><path d=\"M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0\"/><circle cx=\"12\" cy=\"19\" r=\"1.2\" fill=\"#f3efe6\" stroke=\"none\"/></svg>\n"
"<div><h1>WiFi Hotspot</h1><div class=\"sub\">Verwaltung</div></div>\n"
"<button id=\"lout\">Abmelden</button>\n"
"</header>\n"
"<main id=\"main\"></main>\n"
"</div>\n"
"\n"
"<script>\n"
"\"use strict\";\n"
"const $=(s,r)=>(r||document).querySelector(s);\n"
"const $$=(s,r)=>Array.from((r||document).querySelectorAll(s));\n"
"const esc=s=>String(s==null?'':s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));\n"
"const sleep=ms=>new Promise(r=>setTimeout(r,ms));\n"
"let TOKEN='';\n"
"try{ TOKEN=sessionStorage.getItem('tok')||''; }catch(e){}\n"
"function setToken(t){ TOKEN=t||''; try{ if(t) sessionStorage.setItem('tok',t); else sessionStorage.removeItem('tok'); }catch(e){} }\n"
"function toast(msg,kind){\n"
"  const d=document.createElement('div'); d.className='toast'+(kind==='err'?' err':''); d.textContent=msg;\n"
"  document.body.appendChild(d); setTimeout(()=>d.remove(),3200);\n"
"}\n"
"async function api(path,opts){\n"
"  opts=opts||{};\n"
"  const headers=Object.assign({},opts.headers||{});\n"
"  if(TOKEN) headers['X-Token']=TOKEN;\n"
"  const r=await fetch(path+((path.indexOf('?')<0)?'?':'&')+'t='+encodeURIComponent(TOKEN),Object.assign({},opts,{headers}));\n"
"  let d=null; try{ d=await r.json(); }catch(e){}\n"
"  if(r.status===401){ showLogin(); throw new Error('auth'); }\n"
"  if(!r.ok || (d && d.ok===false)) throw new Error((d&&d.error)||('HTTP '+r.status));\n"
"  return d;\n"
"}\n"
"async function act(path,params,okMsg){\n"
"  try{\n"
"    const body=new URLSearchParams(params||{}).toString();\n"
"    const d=await api(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});\n"
"    if(okMsg!==false) toast(okMsg||(d&&d.msg)||'Gespeichert.');\n"
"    return d;\n"
"  }catch(e){ if(e.message!=='auth') toast(e.message,'err'); return null; }\n"
"}\n"
"\n"
"function showLogin(){\n"
"  setToken('');\n"
"  $('#app').classList.add('hidden');\n"
"  $('#login').classList.remove('hidden');\n"
"}\n"
"function showApp(){\n"
"  $('#login').classList.add('hidden');\n"
"  $('#app').classList.remove('hidden');\n"
"  paintMain();\n"
"  refreshStatus();\n"
"}\n"
"\n"
"$('#lf').addEventListener('submit', async ev=>{\n"
"  ev.preventDefault();\n"
"  const e=$('#lerr'); e.classList.add('hidden');\n"
"  try{\n"
"    const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'user='+encodeURIComponent($('#lu').value)+'&pass='+encodeURIComponent($('#lp').value)});\n"
"    const d=await r.json();\n"
"    if(!r.ok || !d.ok) throw new Error(d.error||'Anmeldung fehlgeschlagen');\n"
"    setToken(d.token);\n"
"    showApp();\n"
"  }catch(err){ e.textContent=err.message; e.classList.remove('hidden'); }\n"
"});\n"
"$('#lout').addEventListener('click', async ()=>{\n"
"  await api('/api/logout',{method:'POST'}).catch(()=>{});\n"
"  showLogin();\n"
"});\n"
"\n"
"let ST=null, SET=null;\n"
"\n"
"function fmtB(n){\n"
"  if(!n) return '0 B';\n"
"  const u=['B','KB','MB','GB']; let i=0;\n"
"  while(n>=1024 && i<u.length-1){ n/=1024; i++; }\n"
"  return n.toFixed(i?1:0)+' '+u[i];\n"
"}\n"
"function fmtUp(s){\n"
"  const h=Math.floor(s/3600), m=Math.floor((s%3600)/60);\n"
"  return h?(h+' Std. '+m+' Min.'):(m+' Min.');\n"
"}\n"
"\n"
"function paintMain(){\n"
"  $('#main').innerHTML =\n"
"    '<div id=\"banners\"></div>'+\n"
"    '<div class=\"card\" id=\"c-status\"><h2>Status</h2><div class=\"hint\">Wird geladen...</div></div>'+\n"
"    '<div class=\"card\" id=\"c-ap\"><h2>WiFi Hotspot</h2><div class=\"hint\">Name und Funkkanal des eigenen WLANs. Das Hotspot-WLAN ist immer offen (kein WLAN-Passwort) - Zugang wird über die Login-Seite geregelt.</div>'+\n"
"      '<label for=\"apSsid\">Name (SSID)</label><input id=\"apSsid\" maxlength=\"32\">'+\n"
"      '<label for=\"apCh\">Kanal</label><select id=\"apCh\">'+Array.from({length:11},(_,i)=>'<option>'+(i+1)+'</option>').join('')+'</select>'+\n"
"      '<div class=\"bar\"><button class=\"btn\" id=\"apSave\">Speichern &amp; neu starten</button></div></div>'+\n"
"    '<div class=\"card\" id=\"c-access\"><h2>Zugangspasswort</h2><div class=\"hint\">Dieses Passwort verlangt die Login-Seite, bevor ein Gerät Internet bekommt. Es ist unabhängig vom Admin-Login und zum Weitergeben gedacht.</div>'+\n"
"      '<label for=\"accCur\">Aktuell</label><input id=\"accCur\" type=\"text\" readonly style=\"background:#f3efe6\">'+\n"
"      '<label for=\"accNew\">Neues Zugangspasswort (4-63 Zeichen)</label><input id=\"accNew\" type=\"text\" maxlength=\"63\" placeholder=\"unverändert\">'+\n"
"      '<div class=\"bar\"><button class=\"btn\" id=\"accSave\">Speichern</button></div></div>'+\n"
"    '<div class=\"card\" id=\"c-known\"><h2>Bekannte WLANs</h2><div class=\"hint\">WLANs, die der ESP32 automatisch sucht und wiederholt. An jedem Ort wird automatisch das gerade erreichbare benutzt.</div>'+\n"
"      '<table class=\"tbl\"><thead><tr><th>Name</th><th>Status</th><th></th></tr></thead><tbody id=\"knTb\"></tbody></table>'+\n"
"      '<div class=\"bar\"><button class=\"btn ghost sm\" id=\"knScan\">WLANs suchen</button><span id=\"knScanInfo\" class=\"hint\" style=\"margin:0\"></span></div>'+\n"
"      '<div id=\"knList\" style=\"margin-top:10px\"></div>'+\n"
"      '<div id=\"knForm\" style=\"display:none;margin-top:12px;padding-top:12px;border-top:1px solid var(--line)\">'+\n"
"        '<div class=\"hint\" style=\"margin:0 0 8px\">Hinzufügen: <b id=\"knPicked\"></b></div>'+\n"
"        '<input type=\"hidden\" id=\"knSsid\">'+\n"
"        '<div id=\"knPassWrap\"><label for=\"knPass\">Passwort</label><input id=\"knPass\" type=\"text\" maxlength=\"63\"></div>'+\n"
"        '<div class=\"bar\"><button class=\"btn sm\" id=\"knAdd\">Hinzufügen</button><button class=\"btn ghost sm\" id=\"knCancel\">Abbrechen</button></div></div>'+\n"
"      '<details style=\"margin-top:12px\"><summary style=\"cursor:pointer;color:var(--mut);font-size:13px\">Verstecktes WLAN manuell eingeben</summary>'+\n"
"        '<label for=\"knSsidM\">Name (SSID)</label><input id=\"knSsidM\" maxlength=\"32\">'+\n"
"        '<label for=\"knPassM\">Passwort</label><input id=\"knPassM\" type=\"text\" maxlength=\"63\">'+\n"
"        '<div class=\"bar\"><button class=\"btn sm\" id=\"knAddM\">Hinzufügen</button></div></details></div>'+\n"
"    '<div class=\"card\" id=\"c-web\"><h2>Admin-Zugang</h2>'+\n"
"      '<label style=\"display:flex;align-items:center;gap:8px;text-transform:none;font-weight:400;color:var(--ink)\"><input type=\"checkbox\" id=\"wAuth\" style=\"width:auto\"> Login für die Verwaltung aktivieren</label>'+\n"
"      '<label for=\"wUser\">Benutzername</label><input id=\"wUser\" type=\"text\" maxlength=\"32\" autocomplete=\"username\">'+\n"
"      '<label for=\"wOld\">Aktuelles Passwort (zur Bestätigung)</label><input id=\"wOld\" type=\"password\" autocomplete=\"current-password\">'+\n"
"      '<label for=\"wNew\">Neues Passwort (mind. 6 Zeichen, leer = unverändert)</label><input id=\"wNew\" type=\"password\" autocomplete=\"new-password\">'+\n"
"      '<div class=\"bar\"><button class=\"btn\" id=\"wSave\">Speichern</button></div></div>'+\n"
"    '<div class=\"card\" id=\"c-sys\"><h2>System</h2><div class=\"bar\"><button class=\"btn ghost\" id=\"rst\">Neustart</button><button class=\"btn dng\" id=\"fac\">Werkseinstellungen</button></div></div>';\n"
"\n"
"  api('/api/settings').then(s=>{\n"
"    SET=s;\n"
"    $('#apSsid').value=s.apSsid; $('#apCh').value=s.apCh;\n"
"    $('#accCur').value=s.accessPass;\n"
"    $('#wUser').value=s.webUser; $('#wAuth').checked=s.authEnabled;\n"
"  }).catch(()=>{});\n"
"\n"
"  loadKnown();\n"
"\n"
"  $('#apSave').addEventListener('click', async ()=>{\n"
"    await act('/api/ap/save',{ssid:$('#apSsid').value,ch:$('#apCh').value});\n"
"  });\n"
"  $('#accSave').addEventListener('click', async ()=>{\n"
"    const v=$('#accNew').value;\n"
"    if(!v){ toast('Bitte ein neues Zugangspasswort eingeben','err'); return; }\n"
"    const r=await act('/api/access/save',{pass:v});\n"
"    if(r){ $('#accNew').value=''; refreshStatus(); api('/api/settings').then(s=>{SET=s;$('#accCur').value=s.accessPass;}).catch(()=>{}); }\n"
"  });\n"
"  $('#wSave').addEventListener('click', async ()=>{\n"
"    const r=await act('/api/settings/web',{user:$('#wUser').value,oldpass:$('#wOld').value,newpass:$('#wNew').value,auth:$('#wAuth').checked?'1':'0'});\n"
"    if(r){ $('#wOld').value=''; $('#wNew').value=''; refreshStatus(); }\n"
"  });\n"
"  $('#rst').addEventListener('click', async ()=>{\n"
"    if(!confirm('ESP32 jetzt neu starten?')) return;\n"
"    await act('/api/system/restart',{});\n"
"  });\n"
"  $('#fac').addEventListener('click', async ()=>{\n"
"    if(!confirm('ALLE Einstellungen (WLANs, Passwörter) auf Werkseinstellungen zurücksetzen?')) return;\n"
"    if(prompt('Zur Bestätigung RESET eintippen:')!=='RESET') return;\n"
"    await act('/api/system/factory',{confirm:'RESET'});\n"
"    setToken(''); toast('Werkseinstellungen werden hergestellt...');\n"
"  });\n"
"\n"
"  let nets=[];\n"
"  const paintNets=()=>{\n"
"    $('#knList').innerHTML = nets.length ? nets.map(n=>\n"
"      '<div class=\"wlrow\" data-ssid=\"'+esc(n.ssid)+'\" data-open=\"'+(n.auth==='Offen'?1:0)+'\"><b>'+esc(n.ssid)+'</b><span class=\"mut\">'+esc(n.auth)+' &middot; Kanal '+n.ch+' &middot; '+n.rssi+' dBm</span></div>'\n"
"    ).join('') : '<div class=\"empty\">Noch keine Ergebnisse.</div>';\n"
"    $$('.wlrow',$('#knList')).forEach(r=>r.addEventListener('click', ()=>pickNet(r.dataset.ssid, r.dataset.open==='1')));\n"
"  };\n"
"  const pickNet=(ssid,isOpen)=>{\n"
"    $('#knSsid').value=ssid; $('#knPicked').textContent=ssid;\n"
"    $('#knPass').value=''; $('#knPassWrap').style.display=isOpen?'none':'';\n"
"    $('#knForm').style.display='';\n"
"  };\n"
"  $('#knCancel').addEventListener('click', ()=>{ $('#knForm').style.display='none'; });\n"
"  $('#knAdd').addEventListener('click', async ()=>{\n"
"    const r=await act('/api/known/add',{ssid:$('#knSsid').value,pass:$('#knPass').value});\n"
"    if(r){ $('#knForm').style.display='none'; loadKnown(); }\n"
"  });\n"
"  $('#knAddM').addEventListener('click', async ()=>{\n"
"    const ssid=$('#knSsidM').value.trim();\n"
"    if(!ssid){ toast('Bitte einen WLAN-Namen eingeben','err'); return; }\n"
"    const r=await act('/api/known/add',{ssid,pass:$('#knPassM').value});\n"
"    if(r){ $('#knSsidM').value=''; $('#knPassM').value=''; loadKnown(); }\n"
"  });\n"
"  $('#knScan').addEventListener('click', async ()=>{\n"
"    $('#knScan').disabled=true; $('#knScanInfo').textContent='Suche läuft...';\n"
"    if(!await act('/api/wifi/scan/start',{},false)){ $('#knScan').disabled=false; $('#knScanInfo').textContent=''; return; }\n"
"    for(let i=0;i<40;i++){\n"
"      await sleep(700);\n"
"      const d=await api('/api/wifi/scan').catch(()=>null);\n"
"      if(d && d.state!=='running'){ nets=d.nets.slice().sort((a,b)=>b.rssi-a.rssi); paintNets(); break; }\n"
"    }\n"
"    $('#knScan').disabled=false; $('#knScanInfo').textContent=nets.length+' WLANs gefunden';\n"
"  });\n"
"}\n"
"\n"
"async function loadKnown(){\n"
"  try{\n"
"    const d=await api('/api/known');\n"
"    $('#knTb').innerHTML = d.nets.length ? d.nets.map(n=>\n"
"      '<tr><td>'+esc(n.ssid)+'</td><td>'+(n.active?'<span class=\"tag ok\"><span class=\"dot\"></span>aktiv</span>':'<span class=\"tag\">gespeichert</span>')+'</td>'+\n"
"      '<td><button class=\"btn sm dng\" data-ssid=\"'+esc(n.ssid)+'\">Entfernen</button></td></tr>'\n"
"    ).join('') : '<tr><td colspan=\"3\" class=\"empty\">Noch kein WLAN gespeichert.</td></tr>';\n"
"    $$('#knTb [data-ssid]').forEach(b=>b.addEventListener('click', async ()=>{\n"
"      if(!confirm('WLAN \"'+b.dataset.ssid+'\" nicht mehr wiederholen?')) return;\n"
"      await act('/api/known/remove',{ssid:b.dataset.ssid},'Entfernt');\n"
"      loadKnown();\n"
"    }));\n"
"  }catch(e){}\n"
"}\n"
"\n"
"function paintBanners(){\n"
"  if(!ST) return;\n"
"  let h='';\n"
"  if(ST.warn.defaultWeb) h+='<div class=\"banner\"><b>Standard-Admin-Passwort aktiv</b>Bitte unter \"Admin-Zugang\" ein eigenes Passwort setzen.</div>';\n"
"  if(ST.warn.defaultAccess) h+='<div class=\"banner\"><b>Standard-Zugangspasswort aktiv</b>Bitte unter \"Zugangspasswort\" ein eigenes setzen, bevor du es weitergibst.</div>';\n"
"  $('#banners').innerHTML=h;\n"
"}\n"
"\n"
"function paintStatus(){\n"
"  if(!ST) return;\n"
"  const w=ST.wifi, r=w.repeat;\n"
"  const rLbl = r.state==='connected' ? 'Verbunden' : (r.state==='connecting' ? 'Verbinde...' : 'Suche WLAN...');\n"
"  const rCls = r.state==='connected' ? 'ok' : (r.state==='connecting' ? '' : 'err');\n"
"  $('#c-status').innerHTML =\n"
"    '<h2>Status</h2>'+\n"
"    '<div class=\"kv\" style=\"margin-top:10px\">'+\n"
"    '<div>Wiederholtes WLAN</div><div>'+(r.ssid?esc(r.ssid):'<span class=\"mut\">keines gespeichert</span>')+' <span class=\"tag '+rCls+'\">'+rLbl+'</span>'+(r.state==='connected'?' &middot; '+r.rssi+' dBm':'')+'</div>'+\n"
"    '<div>Internetzugang</div><div>'+(r.internet?'<span class=\"tag ok\"><span class=\"dot\"></span>Aktiv</span>':'<span class=\"tag err\"><span class=\"dot off\"></span>Nicht verfügbar</span>')+'</div>'+\n"
"    '<div>WiFi Hotspot</div><div>'+esc(w.apSsid)+' &middot; Kanal '+w.apCh+' &middot; <span class=\"mono\">'+esc(w.apIp)+'</span></div>'+\n"
"    '<div>Verbundene Geräte</div><div>'+w.clients+'</div>'+\n"
"    '<div>Laufzeit</div><div>'+fmtUp(ST.up)+'</div>'+\n"
"    '<div>Firmware</div><div>v'+esc(ST.fw)+' &middot; '+esc(ST.board)+'</div>'+\n"
"    '</div>';\n"
"}\n"
"\n"
"async function refreshStatus(){\n"
"  try{ ST=await api('/api/status'); paintStatus(); paintBanners(); }catch(e){}\n"
"}\n"
"setInterval(()=>{ if(!$('#app').classList.contains('hidden')) refreshStatus(); }, 3000);\n"
"\n"
"(async ()=>{\n"
"  if(TOKEN){\n"
"    try{ await api('/api/status'); showApp(); return; }catch(e){}\n"
"  }\n"
"  showLogin();\n"
"})();\n"
"</script>\n"
"</body>\n"
"</html>\n";

const char GUEST_HTML[] PROGMEM =
"<!DOCTYPE html>\n"
"<html lang=\"de\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n"
"<meta name=\"theme-color\" content=\"#20242b\">\n"
"<title>WiFi Hotspot</title>\n"
"<link rel=\"icon\" href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Ccircle cx='12' cy='12' r='11' fill='%232f5d50'/%3E%3Cpath d='M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0' stroke='white' stroke-width='2' fill='none' stroke-linecap='round'/%3E%3Ccircle cx='12' cy='19' r='1.4' fill='white'/%3E%3C/svg%3E\">\n"
"<style>\n"
":root{--ink:#20242b;--paper:#f3efe6;--card:#fffdf8;--line:#c9c2b4;--mut:#6b6457;--accent:#2f5d50;--accent2:#24473d;--err:#8a2f2f;--ok:#2f5d50}\n"
"*{box-sizing:border-box}\n"
"html,body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.5 -apple-system,\"Segoe UI\",Roboto,Arial,sans-serif;-webkit-text-size-adjust:100%}\n"
"body{min-height:100vh;display:flex;flex-direction:column}\n"
"header{background:var(--ink);color:#f3efe6;padding:16px 20px;display:flex;align-items:center;gap:12px;border-bottom:3px solid var(--accent)}\n"
"header .ic{width:26px;height:26px;fill:none;stroke:#f3efe6;stroke-width:1.8;flex:none}\n"
"header h1{margin:0;font:600 19px/1.2 Georgia,\"Times New Roman\",serif;letter-spacing:.2px}\n"
"main{flex:1;display:flex;align-items:center;justify-content:center;padding:28px 16px}\n"
".card{background:var(--card);border:1px solid var(--line);border-radius:6px;padding:30px 28px;max-width:380px;width:100%;box-shadow:0 1px 0 rgba(0,0,0,.04)}\n"
".card h2{margin:0 0 6px;font:600 20px/1.3 Georgia,\"Times New Roman\",serif}\n"
".card p.lead{margin:0 0 20px;color:var(--mut);font-size:14.5px}\n"
"label{display:block;font-size:13px;color:var(--mut);margin:0 0 6px;font-weight:600;text-transform:uppercase;letter-spacing:.4px}\n"
"input[type=password],input[type=text]{width:100%;padding:11px 12px;border:1px solid var(--line);border-radius:4px;font-size:16px;background:#fff;color:var(--ink)}\n"
"input:focus{outline:2px solid var(--accent);outline-offset:1px}\n"
"button{width:100%;margin-top:16px;padding:12px;border:1px solid var(--accent2);border-radius:4px;background:var(--accent);color:#fff;font-size:15px;font-weight:600;cursor:pointer}\n"
"button:hover{background:var(--accent2)}\n"
"button:disabled{opacity:.6;cursor:default}\n"
".err{margin-top:12px;padding:10px 12px;border:1px solid var(--err);border-radius:4px;background:#f6e9e9;color:var(--err);font-size:13.5px;display:none}\n"
".ok-box{text-align:center}\n"
".ok-box .mark{width:52px;height:52px;border-radius:50%;background:var(--ok);display:flex;align-items:center;justify-content:center;margin:0 auto 14px}\n"
".ok-box .mark svg{width:28px;height:28px;stroke:#fff;fill:none;stroke-width:3;stroke-linecap:round;stroke-linejoin:round}\n"
".ok-box p{color:var(--mut);font-size:14px;margin:4px 0 0}\n"
".linklike{background:none;border:none;color:var(--accent2);text-decoration:underline;font-size:13px;margin-top:14px;padding:0;width:auto;font-weight:600}\n"
".linklike:hover{background:none;color:var(--ink)}\n"
"footer{padding:14px 16px;text-align:center;color:var(--mut);font-size:12px}\n"
".hidden{display:none !important}\n"
".spin{display:inline-block;width:14px;height:14px;border:2px solid rgba(255,255,255,.4);border-top-color:#fff;border-radius:50%;animation:sp .7s linear infinite;vertical-align:-2px;margin-right:6px}\n"
"@keyframes sp{to{transform:rotate(360deg)}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<header>\n"
"<svg class=\"ic\" viewBox=\"0 0 24 24\"><path d=\"M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0\"/><circle cx=\"12\" cy=\"19\" r=\"1.2\" fill=\"#f3efe6\" stroke=\"none\"/></svg>\n"
"<h1>WiFi Hotspot</h1>\n"
"</header>\n"
"<main>\n"
"<div class=\"card\" id=\"box\">\n"
"<div class=\"empty\">Lade...</div>\n"
"</div>\n"
"</main>\n"
"<footer>Eigenständiges Gerät &middot; kein offizieller Netzbetreiber-Dienst</footer>\n"
"<script>\n"
"\"use strict\";\n"
"const $=s=>document.querySelector(s);\n"
"function escapeHtml(s){return String(s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}\n"
"function renderForm(title){\n"
"  $('#box').innerHTML =\n"
"    '<h2>Zugang zum WLAN</h2>'+\n"
"    '<p class=\"lead\">'+escapeHtml(title)+' verlangt ein Zugangspasswort, bevor Internet freigegeben wird.</p>'+\n"
"    '<form id=\"f\"><label for=\"p\">Zugangspasswort</label><input id=\"p\" type=\"password\" autocomplete=\"current-password\" autofocus>'+\n"
"    '<div class=\"err\" id=\"e\"></div>'+\n"
"    '<button type=\"submit\" id=\"go\">Zugang freischalten</button></form>';\n"
"  $('#f').addEventListener('submit', async ev=>{\n"
"    ev.preventDefault();\n"
"    const btn=$('#go'); const e=$('#e');\n"
"    e.style.display='none';\n"
"    btn.disabled=true; btn.innerHTML='<span class=\"spin\"></span>Prüfe...';\n"
"    try{\n"
"      const r=await fetch('/api/access/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pass='+encodeURIComponent($('#p').value)});\n"
"      const d=await r.json().catch(()=>({}));\n"
"      if(r.ok && d.ok){ load(); }\n"
"      else { e.textContent=d.error||'Anmeldung fehlgeschlagen'; e.style.display='block'; btn.disabled=false; btn.textContent='Zugang freischalten'; }\n"
"    }catch(err){ e.textContent='Verbindung fehlgeschlagen'; e.style.display='block'; btn.disabled=false; btn.textContent='Zugang freischalten'; }\n"
"  });\n"
"}\n"
"function renderOk(internet){\n"
"  $('#box').innerHTML =\n"
"    '<div class=\"ok-box\"><div class=\"mark\"><svg viewBox=\"0 0 24 24\"><path d=\"M5 13l4 4L19 7\"/></svg></div>'+\n"
"    '<h2>Verbunden</h2>'+\n"
"    '<p>'+(internet?'Internetzugang ist aktiv.':'Angemeldet - das wiederholte WLAN ist momentan nicht erreichbar, bitte kurz warten.')+'</p>'+\n"
"    '<button class=\"linklike\" id=\"out\" type=\"button\">Abmelden</button></div>';\n"
"  $('#out').addEventListener('click', async ()=>{\n"
"    await fetch('/api/access/logout',{method:'POST'}).catch(()=>{});\n"
"    load();\n"
"  });\n"
"}\n"
"async function load(){\n"
"  try{\n"
"    const r=await fetch('/api/access/info');\n"
"    const d=await r.json();\n"
"    if(d.online) renderOk(d.internet);\n"
"    else renderForm(d.title||'WiFi Hotspot');\n"
"  }catch(e){\n"
"    renderForm('WiFi Hotspot');\n"
"  }\n"
"}\n"
"load();\n"
"</script>\n"
"</body>\n"
"</html>\n";
