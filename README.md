# ESP32 Network Toolbox

Eine komplette Netzwerk-Werkzeugkiste für das **DIYMORE ESP32-S3 DevKitC-1 N16R8** (16 MB Flash, 8 MB PSRAM).
Der ESP32 spannt ein eigenes WLAN auf und liefert eine moderne Dark-Mode-Weboberfläche aus – **alles in einer einzigen Datei**:

```
ESP32_Network_Toolbox/ESP32_Network_Toolbox.ino
```

Kein PlatformIO, kein Node.js, kein Python, keine zusätzlichen Bibliotheken.

| Standard | Wert |
|---|---|
| WLAN-Name | `ESP32-NetworkTool` |
| WLAN-Passwort | `esp32setup` |
| Webinterface | http://192.168.4.1 (oder `http://esp32-toolbox.local`) |
| Login | `admin` / `admin123` (**bitte beim ersten Start ändern**) |

## Funktionen

Dashboard · WLAN-Scanner (Suche, Filter, Sortierung) · Kanalübersicht als Grafik · RSSI-Verlauf einzelner Netze ·
eigener Access Point (starten/stoppen, SSID/Passwort/Kanal, dauerhaft gespeichert) · Geräte am eigenen AP (anzeigen, sperren, entsperren) ·
**Router-Modus** (Internet vom Heim-WLAN über den eigenen AP teilen) · **Gäste-Portal** (eigene Anmeldeseite mit Presets „Einfach" und „Ticket-System", Ticket-Verwaltung) ·
Heim-WLAN verbinden wie am Handy (Liste gefundener Netze) · Test-WLANs · BLE-Scanner · Logs · Einstellungen · Login · Captive Portal · Systeminfos · Firmware-Update über GitHub (+ manueller .bin-Upload) · Demo-Modus.

Bewusst **nicht** enthalten: Jamming, Deauthentication, Beacon-Flooding, Evil Twin, Credential Harvesting, Bluetooth-Störung/-Flooding oder Angriffe auf fremde Geräte. Das Projekt ist für Lernen, Diagnose, eigene Testnetze und eigene Geräte gedacht.

---

## A. Arduino-IDE-Einstellungen

Menü **Werkzeuge** (Arduino IDE 2.x). Die Menünamen wurden gegen die `boards.txt` des ESP32-Cores 3.3.12 geprüft.

| Einstellung | Wert |
|---|---|
| **Board** | `ESP32S3 Dev Module` |
| **USB CDC On Boot** | `Disabled` bei Anschluss am Port **UART** · `Enabled` bei Anschluss am Port **USB** |
| **CPU Frequency** | `240MHz (WiFi)` |
| **Core Debug Level** | `None` |
| **Flash Mode** | `QIO 80MHz` |
| **Flash Size** | `16MB (128Mb)` |
| **Partition Scheme** | `16M Flash (3MB APP/9.9MB FATFS)` ← wichtig: enthält zwei 3-MB-App-Slots für OTA |
| **PSRAM** | `OPI PSRAM` ← wichtig: N16R8 hat *Octal*-PSRAM. „QSPI PSRAM“ würde beim Start abstürzen |
| **Upload Mode** | `UART0 / Hardware CDC` |
| **USB Mode** | `Hardware CDC and JTAG` |
| **Upload Speed** | `921600` (bei Problemen `460800` oder `115200`) |
| **Port** | der neu erscheinende Port (z. B. `COM5` oder `/dev/ttyUSB0`) |

Die anderen Einträge (Events/Arduino Runs On, JTAG Adapter, Zigbee …) bleiben auf Standard.

**Zwei USB-C-Buchsen:** Das Board hat meist eine Buchse „UART“ (Wandlerchip, sehr zuverlässig – **empfohlen**) und eine Buchse „USB“ (direkt am ESP32-S3). Nimm für das erste Flashen die Buchse `UART` mit *USB CDC On Boot = Disabled*.

## B. Benötigte Bibliotheken

**Keine.** Alles (WiFi, WebServer, DNSServer, Preferences, ESPmDNS, HTTPClient, Update, BLE) steckt im Board-Paket.
Nötig ist nur das Board-Paket **„esp32 by Espressif Systems“ in Version 3.3.12 oder neuer** (Version 3.x ist Pflicht, sonst bricht das Kompilieren mit einer verständlichen Meldung ab; für die HTTPS-Update-Funktion wird 3.3.12+ benötigt, alles andere läuft auch mit älterem 3.x).

## C. Installation Schritt für Schritt

1. **Arduino IDE installieren:** https://www.arduino.cc/en/software (Version 2.x).
2. **ESP32-Unterstützung installieren:** *Datei → Einstellungen → „Zusätzliche Boardverwalter-URLs“*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   → OK → *Werkzeuge → Board → Boardverwalter…* → nach `esp32` suchen → **„esp32 by Espressif Systems“** installieren (mind. 3.3.12; der Download ist groß, das dauert).
3. **Bibliotheken:** keine nötig.
4. **Projekt öffnen:** Ordner `ESP32_Network_Toolbox` herunterladen und darin `ESP32_Network_Toolbox.ino` per Doppelklick öffnen (Ordner- und Dateiname müssen gleich heißen).
5. **Board auswählen:** *Werkzeuge → Board → esp32 → ESP32S3 Dev Module*.
6. **Einstellungen setzen:** wie in Abschnitt A.
7. **ESP32 per USB-C anschließen** (Buchse „UART“).
8. **COM-Port wählen:** *Werkzeuge → Port*.
9. **Upload:** Pfeil-Button (→) drücken. Erstes Kompilieren dauert 1–3 Minuten. Bei „Connecting…“ ggf. die **BOOT**-Taste gedrückt halten.
10. **ESP32-WLAN suchen:** Am Handy/PC nach `ESP32-NetworkTool` suchen.
11. **Verbinden** mit Passwort `esp32setup`.
12. **Browser öffnen:** `http://192.168.4.1` (bei Handys öffnet sich die Seite oft automatisch – Captive Portal). Anmelden mit `admin` / `admin123` und **Passwörter ändern**.

Nach dem Upload kannst du im Seriellen Monitor (115200 Baud) die Start-Meldungen sehen.

## D. Fehlerbehebung

| Problem | Lösung |
|---|---|
| **COM-Port wird nicht angezeigt** | Anderes USB-Kabel probieren (viele Kabel sind nur Ladekabel!). Andere Buchse (UART ↔ USB) testen. Treiber für CP210x bzw. CH340 installieren (Silicon Labs / WCH). Zum Erzwingen: **BOOT** halten, **RST** kurz drücken, BOOT loslassen. |
| **Upload funktioniert nicht** („Failed to connect“, „Timed out“) | BOOT-Taste während „Connecting…“ halten. Upload Speed auf `115200` senken. Richtigen Port wählen. Bei Buchse „USB“ ggf. *USB CDC On Boot = Enabled*. |
| **ESP32 startet ständig neu** | Meist falsches PSRAM: *PSRAM = OPI PSRAM* setzen (nicht „QSPI“). Außerdem *Flash Size = 16MB*, *Flash Mode = QIO 80MHz*. Serieller Monitor (115200) zeigt den Grund. Notfalls *Werkzeuge → Erase All Flash Before Sketch Upload → Enabled* einmal hochladen, danach wieder *Disabled*. |
| **Webinterface öffnet sich nicht** | Mit `ESP32-NetworkTool` verbunden? Mobile Daten am Handy ausschalten, „WLAN ohne Internet beibehalten“ bestätigen. Adresse **`http://`** (nicht https) `192.168.4.1` von Hand eintippen. VPN/„Private DNS“ ausschalten. |
| **PSRAM-Fehler** („PSRAM ID read error“, Absturz beim Start) | *PSRAM = OPI PSRAM*. Auf dem Dashboard steht PSRAM „8.0 MB“, wenn es erkannt wurde. |
| **Flash-/Partition-Fehler** („Sketch too big“, Update „Partition Scheme prüfen“) | *Partition Scheme = 16M Flash (3MB APP/9.9MB FATFS)* und *Flash Size = 16MB*. Bei „Default 4MB“ passt das Programm nicht. |
| **WLAN wird nicht angezeigt** | Nur 2,4 GHz. 5 Minuten warten, Handy-WLAN aus/an. Ist WLAN-Name/Passwort geändert worden? Passwort mind. 8 Zeichen. Notfalls **Werkseinstellungen** (Einstellungen → System) oder über die Weboberfläche nicht erreichbar: neu flashen mit *Erase All Flash* = Enabled. |
| **BLE funktioniert nicht** | Erst „BLE scannen“ klicken (BLE wird erst dann gestartet). Nur Geräte, die gerade senden, erscheinen. Bei Fehlermeldung ESP32 neu starten. Während eines Firmware-Updates ist BLE gesperrt. |
| **„Update nicht möglich“** | Zuerst unter *Einstellungen → Internet* ein Heim-WLAN mit Internet eintragen. Prüfen, ob `GITHUB_USER` im Code gesetzt ist. |

Hinweise zum Verhalten:
- **Scans unterbrechen kurz das eigene WLAN** (der Funkchip springt dabei über alle Kanäle). Das ist normal.
- **Ist der ESP32 mit einem Heim-WLAN verbunden, übernimmt sein Access Point dessen Kanal** (Hardware-Eigenschaft).
- **„WLAN stoppen“ trennt dich von der Weboberfläche.** Ein Neustart (Reset-Taste) startet das WLAN immer wieder (der Stopp wird nicht gespeichert).

## E. Test-WLANs – wichtige Hardware-Einschränkung

Der ESP32 hat **ein** WLAN-Funkmodul und kann nur **ein** eigenes WLAN gleichzeitig senden. „Test-WLANs starten“ erzeugt deshalb echte, passwortgeschützte Access Points mit Zufallsnamen (`ESP32-Test-4821`, `ESP32-LAB-1938`, `ESP32-DEMO-7712`) und schaltet sie bei mehr als einem **nacheinander** (alle 15/30/60 s) durch. Das Passwort ist das deines Access Points, die Weboberfläche bleibt darüber unter `192.168.4.1` erreichbar. Nach 30 Minuten oder mit „Alle stoppen“ kehrt der ESP32 zu deinem normalen WLAN zurück. Gleichzeitig sichtbare Fake-Netze (Beacon-Spam) gibt es bewusst nicht.

## F. Testmodus ohne echte WLAN-/BLE-Umgebung

Ganz oben in der `.ino`:

```cpp
#define DEMO_MODE 1
```

Dann liefert der ESP32 simulierte WLANs, BLE-Geräte, Clients und ein simuliertes Update (v9.9.9, es wird nichts geflasht). Die Weboberfläche zeigt einen „DEMO-Modus“-Hinweis. Für den echten Betrieb wieder auf `0` setzen.

**Ohne ESP32 – Browser-Vorschau:** Die Datei `Vorschau/Vorschau.html` per Doppelklick im Browser öffnen (Login `admin` / `admin123`). Dort läuft die komplette Weboberfläche gegen einen simulierten ESP32 (Fake-WLANs, BLE-Geräte, Update-Ablauf). Es wird nichts an einen ESP32 gesendet; Datei-Upload ist in der Vorschau deaktiviert.

**Simulatoren (Wokwi, Velxio …):** Die Adresse `192.168.4.1` ist dort von deinem Browser aus **nicht** erreichbar – der Access Point existiert nur im Simulator, nicht in deinem Netzwerk. Zwei Möglichkeiten:

1. **Simulator-Modus:** Oben in der `.ino` einstellen
   ```cpp
   #define SIMULATOR_WIFI_SSID "Velxio-GUEST"   // Wokwi: "Wokwi-GUEST"
   #define DEMO_MODE 1                          // simulierte WLAN-/BLE-Daten
   ```
   Der ESP32 verbindet sich dann als Station mit dem Simulator-WLAN (kein eigener Access Point) und schreibt im Seriellen Monitor `Webinterface: http://192.168.x.x`. Diese Adresse im Simulator anklicken. Für den echten ESP32 wieder `""` und `0` einstellen.
2. Die Browser-Vorschau oben oder den echten ESP32 verwenden.

**Wokwi (Simulator):** Kompilierung und der ESP32-S3 selbst werden simuliert, ebenso der Webserver im Demo-Modus. **Nicht** simulierbar sind: der eigene Access Point samt verbundener Geräte, das echte Scannen fremder WLANs, BLE, das Captive Portal und echte Geräte-Funkdaten. Diese Funktionen laufen erst auf der echten Hardware. Der Zugriff auf den Webserver im Simulator erfordert die Port-Weiterleitung von Wokwi (Gateway) und ist Wokwi-abhängig – nicht getestet.

## G. Heim-WLAN wie am Handy verbinden

*Einstellungen → Internet (Heim-WLAN)* → **„WLANs suchen“**. Der ESP32 zeigt die gefundenen Netze mit Signalstärke, tippe eines an. Bei einem verschlüsselten Netz erscheint ein Passwortfeld, bei einem offenen nicht - genau wie am Smartphone. Versteckte WLANs (senden keinen Namen) trägst du über „Verstecktes WLAN manuell eingeben“ ein.


## H. Router-Modus: Internet aus einem WLAN auf ein eigenes verteilen

Der ESP32 kann sich mit einem WLAN verbinden (z. B. dem Schul-WLAN, unter *Einstellungen → Internet*) und dessen Internetzugang über seinen **eigenen** Access Point weitergeben - wie ein Reise-Router. Am fremden WLAN meldet sich dabei nur der ESP32 mit einer einzigen Geräte-MAC-Adresse an; alle Geräte, die sich mit dem ESP32-WLAN verbinden, teilen sich diese eine Verbindung.

**Einrichtung:** *Einstellungen → Internet (Heim-WLAN)* verbinden, dann *Einstellungen → Access Point* → Haken bei „Internet über eigenes WLAN teilen (Router-Modus)" setzen und speichern. Das Dashboard zeigt danach eine Kachel „Router-Modus: Aktiv".

**Technischer Hintergrund:** Möglich wird das durch NAT/IP-Weiterleitung (NAPT), die im ESP32-Boardpaket bereits aktiviert ist (`esp_netif_napt_enable`) - es ist **keine** zusätzliche Bibliothek und kein Custom-Build nötig, das läuft mit der normalen Arduino-IDE-Installation aus Abschnitt C.

**Grenzen:** Ein ESP32 ist kein vollwertiger Router - für eine ganze Schulklasse gleichzeitig ist die Bandbreite und die Anzahl möglicher Verbindungen begrenzt (siehe `AP_MAX_CLIENTS` im Code). Prüfe außerdem vorher, ob das Teilen des Schul-WLANs über ein eigenes Gerät von deiner Schule/IT-Abteilung erlaubt ist - viele Netzwerk-Richtlinien untersagen das Einbringen zusätzlicher Access Points.

## I. Gäste-Portal: eigene Anmeldeseite für Geräte am Access Point

Unter *Einstellungen → Gäste-Portal* lässt sich eine eigene, öffentliche Anmeldeseite aktivieren, die Geräte am ESP32-WLAN zuerst sehen, bevor sie Internetzugang bekommen (braucht aktiven Router-Modus, siehe oben). Drei Modi:

* **Aus** - heutiges Verhalten, direkt zur normalen Oberfläche (kein Regressionsrisiko für den reinen Diagnose-Betrieb).
* **Einfach** - ein Knopf „Verbinden", danach ist das Gerät angemeldet.
* **Ticket-System** - Geräte müssen einen 6-stelligen, **einmal gültigen** Anmeldecode eingeben. Unter *Einstellungen → Gäste-Portal → Tickets* erstellst du Tickets (mit optionalem Namen/Kommentar), siehst ihren Status (frei/verwendet) und kannst sie als Textdatei herunterladen, um sie z. B. auszudrucken oder einzeln zu verschicken - das Verschicken selbst übernimmt der ESP32 nicht, das machst du wie gewohnt (WhatsApp, E-Mail, Zettel).

**Eigenes WLAN für Gäste - getrennt vom Steuerungs-WLAN:** Unter *Einstellungen → Gäste-Portal* trägst du einen eigenen WLAN-Namen und ein eigenes Passwort für Gäste ein (Standard: `ESP32-Gäste-WLAN`). Sobald ein Modus (Einfach oder Ticket-System) aktiv ist, sendet der ESP32 **dieses** WLAN - nicht mehr das Steuerungs-WLAN aus den Access-Point-Einstellungen. **Wichtig:** Der ESP32 hat nur ein einziges WLAN-Funkmodul und kann deshalb nicht zwei Netze gleichzeitig senden (dieselbe Einschränkung wie bei den Test-WLANs). „Getrennt" heißt hier: ein eigener Name und ein eigenes Passwort, nicht zwei gleichzeitig sendende Funknetze.

**Admin-Zugriff bleibt immer möglich:** Auch wenn gerade das Gäste-WLAN gesendet wird, erreichst du die normale Verwaltungsoberfläche über `http://192.168.4.1/admin` - verbinde dich dafür einfach mit dem aktuell gesendeten WLAN (Dashboard zeigt an, welches das ist) und öffne `/admin` statt `/`.

**Wichtiger Hinweis zum Design:** Die Anmeldeseite ist bewusst ein **eigenes, generisches Design** (blau/weiß, ohne fremde Logos). Es wird **kein** echter Anbieter nachgebaut (z. B. Bayern-WLAN oder ähnliche Dienste) - eine solche Kopie wäre eine Phishing-Falle: Nutzer könnten denken, sie seien im echten, offiziellen Netz, und dort z. B. echte Zugangsdaten eingeben. Das schließt dieses Projekt konsequent aus (siehe Abschnitt „Sicherheit" oben im Projektauftrag).

**Wie die Anmeldung technisch funktioniert:** Der ESP32 betreibt einen eigenen, kleinen DNS-Server. Nicht angemeldete Geräte bekommen auf jede Anfrage die eigene IP-Adresse zurück (klassisches Captive Portal, wie schon zuvor). Angemeldete Geräte werden bei aktivem Router-Modus an den echten DNS-Server des Heim-WLANs weitergereicht und bekommen damit echten Internetzugang. Eine angemeldete Sitzung gilt 8 Stunden; unter *Einstellungen → Gäste-Portal → Gerade online* siehst du alle aktuell angemeldeten Geräte und kannst sie einzeln trennen.

**Vorschau ohne ESP32:** `Vorschau/Gaeste-Portal-Vorschau.html` zeigt die Anmeldeseite mit einem vorbereiteten Test-Ticket (PIN `123456`) zum Ausprobieren.
## J. Firmware-Update über GitHub

1. In der `.ino` oben eintragen:
   ```cpp
   #define GITHUB_USER "dein-github-name"
   #define GITHUB_REPO "ESP32-Network-Toolbox"
   ```
2. `FIRMWARE_VERSION` erhöhen (z. B. `"1.0.1"`) – **die Version in der Firmware muss zur Release-Nummer passen**, sonst würde der ESP32 das Update immer wieder anbieten (er zeigt dann einen Hinweis).
3. In der Arduino IDE: *Sketch → Kompilierte Binärdatei exportieren*. Im Sketch-Ordner entsteht `build/…/ESP32_Network_Toolbox.ino.bin`. Verwende **nur diese Datei** (nicht `.merged.bin`, `.bootloader.bin`, `.partitions.bin`).
4. Datei umbenennen: `ESP32_Network_Toolbox_v1.0.1.bin`.
5. Auf GitHub: *Releases → Draft a new release*, Tag **`v1.0.1`**, die `.bin` als Asset hochladen, veröffentlichen (Release muss öffentlich sein, kein Pre-Release/Entwurf).
6. Am ESP32: *Einstellungen → Internet (Heim-WLAN)* verbinden, dann *Einstellungen → Firmware-Update → Nach Updates suchen → Update installieren*.

Sicherheit und Ablauf:
- Nur **HTTPS** zu `api.github.com` / `github.com` / `*.githubusercontent.com` (jede Weiterleitung wird geprüft), Zertifikate werden gegen die eingebauten Root-Zertifikate geprüft (kein `setInsecure`).
- Installiert wird nur eine Datei mit dem exakten Namen `ESP32_Network_Toolbox_v<Version>.bin` aus dem festgelegten Repository, nur wenn die Version **höher** ist.
- **SHA-256** ist Pflicht (`REQUIRE_SHA256 1`): GitHub liefert die Prüfsumme bei Release-Dateien automatisch mit; alternativ eine Datei `…bin.sha256` hochladen. Stimmt sie nicht, wird das Update verworfen.
- Der Download wird direkt in die **zweite OTA-Partition** geschrieben. Bei Abbruch/Fehler bleibt die laufende Firmware unangetastet.
- Startet die neue Firmware nicht stabil (3 Fehlstarts innerhalb der ersten 60 s), aktiviert sie beim nächsten Start automatisch die **vorherige Firmware**.
- Ohne Internet oder GitHub läuft alles andere unverändert; die Update-Prüfung meldet dann „Keine Internetverbindung – Updateprüfung nicht möglich.“
- Alternativ: *Einstellungen → Firmware-Update → Manuell aktualisieren* lädt eine `.bin` direkt aus dem Browser hoch (nur mit Login).

## K. Was geprüft wurde – und was nicht

In der Entwicklungsumgebung stand **keine ESP32-Toolchain** zur Verfügung (Download-Server gesperrt). Deshalb gilt:

- ✅ Firmware-Quelltext gegen die **echten Header des ESP32-Cores 3.3.12 / ESP-IDF 5.5** gelesen, Signaturen abgeglichen, mit `g++ -Wall -Wextra` gegen daraus abgeleitete Stubs syntaktisch geprüft (auch mit `DEMO_MODE 1`, `UPDATE_ENABLED 0`, altem Core).
- ✅ Reine Logik (JSON-Auswertung der GitHub-Antwort inkl. kaputter Eingaben, Versionsvergleich, SHA-256-Passwort-Hash, MAC-Format, JSON-Ausgabe, BLE-Liste, Log-Puffer) auf dem PC mit AddressSanitizer getestet.
- ✅ Weboberfläche in Chromium (Desktop + Smartphone-Größe) gegen einen Mock-Server durchgetestet: Login, Scanner-Filter, Kanäle, RSSI, Sperren, Test-WLANs, BLE, Logs, Einstellungen, kompletter Update-Ablauf inkl. Fortschritt und Neustart.
- ✅ Router-Modus/Gäste-Portal: Die `esp_netif_napt_enable`/`esp_netif_set_default_netif`-Aufrufe wurden gegen den echten ESP-IDF-Quelltext (Beispiel `examples/wifi/softap_sta`) geprüft - NAPT ist in den vorkompilierten Bibliotheken des Boardpakets bereits aktiviert (`CONFIG_LWIP_IPV4_NAPT=y`), keine Zusatzbibliothek nötig. Das selbstgebaute DNS-Antwortpaket (Redirect zur Anmeldeseite) wurde per Host-Test byteweise geprüft. Ticket-Erstellung/-Einlösung/-Persistenz und die Gäste-Freigabeliste wurden ebenfalls per Host-Test geprüft. Das komplette Zusammenspiel (Router-Modus einschalten, Ticket erstellen, auf der Gasteseite einlösen, Gast in der Liste sehen und trennen) wurde Ende-zu-Ende gegen einen Mock-Server durchgespielt.
- ⚠️ **Nicht auf echter Hardware getestet:** Kompilierung mit der echten Toolchain, WLAN/BLE/Captive Portal, HTTPS-Download, OTA, und insbesondere der tatsächliche Datendurchsatz/die Stabilität des Router-Modus unter mehreren gleichzeitigen Geräten. Sollte der Compiler etwas beanstanden, schick mir die Fehlermeldung.
