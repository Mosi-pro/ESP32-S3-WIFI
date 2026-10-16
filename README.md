# WiFi Hotspot

Ein WLAN-Repeater für das **DIYMORE ESP32-S3 DevKitC-1 N16R8** (16 MB Flash, 8 MB PSRAM). Der ESP32 verbindet sich mit einem bestehenden WLAN (Schule, zu Hause, …) und sendet dessen Internetzugang unter einem eigenen, immer gleich benannten WLAN **„WiFi Hotspot“** weiter – wie ein Reise-Router. **Alles in einer einzigen Datei:**

```
WiFi_Hotspot/WiFi_Hotspot.ino
```

Kein PlatformIO, kein Node.js, kein Python, keine zusätzlichen Bibliotheken.

| Standard | Wert |
|---|---|
| Hotspot-WLAN | `WiFi Hotspot` (offen, **kein** WLAN-Passwort) |
| Zugangspasswort (Login-Seite) | `hotspot123` (**bitte beim ersten Start ändern**) |
| Verwaltung | http://192.168.4.1/admin (oder `http://wifi-hotspot.local/admin`) |
| Admin-Login | `admin` / `admin123` (**bitte beim ersten Start ändern**) |

## Funktionen

Verbindet sich mit mehreren **bekannten WLANs** (merkt sich jedes, mit dem es schon einmal verbunden war) und wiederholt automatisch das gerade erreichbare davon über den eigenen Access Point · eigener Hotspot immer unter demselben Namen „WiFi Hotspot“, offen (kein WLAN-Passwort) · eigene **Login-Seite** verlangt ein Zugangspasswort, bevor ein Gerät Internet bekommt · Verwaltung (bekannte WLANs, Zugangspasswort, Admin-Login, Systeminfos) jederzeit unter `/admin` erreichbar · Captive Portal · Login mit erzwungenem Hinweis bei Standardpasswörtern.

Bewusst **nicht** enthalten: Jamming, Deauthentication, Beacon-Flooding, Evil Twin, Credential Harvesting oder Angriffe auf fremde Geräte/Netze. Das Hotspot-WLAN gibt sich **nicht als ein echter, dritter Anbieter/Dienst aus** – es ist klar als eigenes Gerät benannt und zeigt eine eigene, generische Login-Seite statt die eines echten Anbieters nachzubauen.

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
| **Partition Scheme** | `16M Flash (3MB APP/9.9MB FATFS)` |
| **PSRAM** | `OPI PSRAM` ← wichtig: N16R8 hat *Octal*-PSRAM. „QSPI PSRAM“ würde beim Start abstürzen |
| **Upload Mode** | `UART0 / Hardware CDC` |
| **USB Mode** | `Hardware CDC and JTAG` |
| **Upload Speed** | `921600` (bei Problemen `460800` oder `115200`) |
| **Port** | der neu erscheinende Port (z. B. `COM5` oder `/dev/ttyUSB0`) |

Die anderen Einträge (Events/Arduino Runs On, JTAG Adapter, Zigbee …) bleiben auf Standard.

**Zwei USB-C-Buchsen:** Das Board hat meist eine Buchse „UART“ (Wandlerchip, sehr zuverlässig – **empfohlen**) und eine Buchse „USB“ (direkt am ESP32-S3). Nimm für das erste Flashen die Buchse `UART` mit *USB CDC On Boot = Disabled*.

## B. Benötigte Bibliotheken

**Keine.** Alles (WiFi, WebServer, Preferences, ESPmDNS) steckt im Board-Paket.
Nötig ist nur das Board-Paket **„esp32 by Espressif Systems“ in Version 3.3.12 oder neuer**.

## C. Installation Schritt für Schritt

1. **Arduino IDE installieren:** https://www.arduino.cc/en/software (Version 2.x).
2. **ESP32-Unterstützung installieren:** *Datei → Einstellungen → „Zusätzliche Boardverwalter-URLs“*:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   → OK → *Werkzeuge → Board → Boardverwalter…* → nach `esp32` suchen → **„esp32 by Espressif Systems“** installieren (mind. 3.3.12; der Download ist groß, das dauert).
3. **Bibliotheken:** keine nötig.
4. **Projekt öffnen:** Ordner `WiFi_Hotspot` herunterladen und darin `WiFi_Hotspot.ino` per Doppelklick öffnen (Ordner- und Dateiname müssen gleich heißen).
5. **Board auswählen:** *Werkzeuge → Board → esp32 → ESP32S3 Dev Module*.
6. **Einstellungen setzen:** wie in Abschnitt A.
7. **ESP32 per USB-C anschließen** (Buchse „UART“).
8. **COM-Port wählen:** *Werkzeuge → Port*.
9. **Upload:** Pfeil-Button (→) drücken. Erstes Kompilieren dauert 1–3 Minuten. Bei „Connecting…“ ggf. die **BOOT**-Taste gedrückt halten.
10. **Verwaltung öffnen:** Mit `WiFi Hotspot` verbinden (kein Passwort), dann im Browser `http://192.168.4.1/admin` öffnen. Anmelden mit `admin` / `admin123` und **beide Passwörter ändern** (Admin-Login unter „Admin-Zugang“, Zugangspasswort unter „Zugangspasswort“).
11. **Ein WLAN zum Wiederholen hinzufügen:** In der Verwaltung unter „Bekannte WLANs“ → „WLANs suchen“ → das gewünschte WLAN anklicken → Passwort eingeben → „Hinzufügen“. Der ESP32 verbindet sich sofort damit.

Nach dem Upload kannst du im Seriellen Monitor (115200 Baud) die Start-Meldungen sehen.

## D. Fehlerbehebung

| Problem | Lösung |
|---|---|
| **COM-Port wird nicht angezeigt** | Anderes USB-Kabel probieren (viele Kabel sind nur Ladekabel!). Andere Buchse (UART ↔ USB) testen. Treiber für CP210x bzw. CH340 installieren (Silicon Labs / WCH). Zum Erzwingen: **BOOT** halten, **RST** kurz drücken, BOOT loslassen. |
| **Upload funktioniert nicht** („Failed to connect“, „Timed out“) | BOOT-Taste während „Connecting…“ halten. Upload Speed auf `115200` senken. Richtigen Port wählen. Bei Buchse „USB“ ggf. *USB CDC On Boot = Enabled*. |
| **ESP32 startet ständig neu** | Meist falsches PSRAM: *PSRAM = OPI PSRAM* setzen (nicht „QSPI“). Außerdem *Flash Size = 16MB*, *Flash Mode = QIO 80MHz*. Serieller Monitor (115200) zeigt den Grund. |
| **Verwaltung öffnet sich nicht** | Mit `WiFi Hotspot` verbunden? Adresse **`http://`** (nicht https) `192.168.4.1/admin` von Hand eintippen. VPN/„Private DNS“ ausschalten. |
| **PSRAM-Fehler** („PSRAM ID read error“, Absturz beim Start) | *PSRAM = OPI PSRAM*. |
| **Flash-/Partition-Fehler** | *Partition Scheme = 16M Flash (3MB APP/9.9MB FATFS)* und *Flash Size = 16MB*. |
| **Kein Internet am Hotspot** | Unter „Status“ in der Verwaltung prüfen, ob ein bekanntes WLAN gerade erreichbar ist. Ist keines in der Nähe, zeigt der Status „Suche WLAN…“ - Internet gibt es erst, wenn eines gefunden wird. |
| **Zugangspasswort vergessen** | In der Verwaltung unter „Zugangspasswort“ ein neues setzen (Admin-Login nötig). |
| **Admin-Login/alles vergessen** | *Verwaltung → System → Werkseinstellungen* (setzt auch das Zugangspasswort und alle bekannten WLANs zurück). |

## E. Mehrere Orte: wie das automatische Wiederholen funktioniert

Der ESP32 merkt sich bis zu 6 WLANs (SSID + Passwort), mit denen er schon einmal über „Bekannte WLANs hinzufügen“ verbunden wurde. Beim Start – und immer, wenn die aktuelle Verbindung abreißt – probiert er sie der Reihe nach durch, bis eines erreichbar ist, und wiederholt dessen Internetzugang dann über den eigenen Hotspot. Dadurch funktioniert „WiFi Hotspot“ unverändert an mehreren Orten (z. B. Schule und zu Hause), ohne nach jedem Ortswechsel neu eingerichtet werden zu müssen – es dauert nur kurz (wenige Sekunden pro bekanntem WLAN), bis das gerade erreichbare gefunden ist.

Ein WLAN, das nicht mehr wiederholt werden soll, lässt sich in der Verwaltung unter „Bekannte WLANs“ jederzeit entfernen.

## F. Login-Seite und Zugangspasswort

Das Hotspot-WLAN selbst ist **offen** (kein WLAN-Passwort) – jedes Gerät kann sich verbinden. Bevor es aber Internet bekommt, zeigt der ESP32 jedem neuen Gerät automatisch eine eigene Login-Seite (Captive Portal) mit einem Feld für das **Zugangspasswort**. Erst nach richtiger Eingabe wird das Gerät für 12 Stunden freigeschaltet.

Das Zugangspasswort ist unabhängig vom Admin-Login und zum Weitergeben gedacht (z. B. an Klassenkameraden) – ändere es in der Verwaltung unter „Zugangspasswort“, bevor du es weitergibst. Die Verwaltung selbst bleibt über `/admin` immer erreichbar, unabhängig davon, ob ein Gerät das Zugangspasswort schon eingegeben hat.

**Wichtiger Hinweis zum Design:** Die Login-Seite ist bewusst ein **eigenes, generisches Design**. Es wird **kein** echter Anbieter nachgebaut – eine solche Kopie (oder ein Name, der sich als echter Dienst ausgibt) wäre eine Phishing-Falle: Nutzer könnten denken, sie seien im Netz eines echten, offiziellen Anbieters, und dort z. B. Zugangsdaten eingeben, die eigentlich für etwas anderes gelten. Das schließt dieses Projekt konsequent aus.

## G. Internet teilen (Repeater-Funktion)

Sobald ein bekanntes WLAN verbunden ist, teilt der ESP32 dessen Internetzugang automatisch über seinen eigenen Access Point – ganz ohne separate Einstellung, das ist der Normalbetrieb von „WiFi Hotspot“. Am fremden WLAN meldet sich dabei nur der ESP32 mit einer einzigen Geräte-MAC-Adresse an; alle Geräte, die sich mit dem Hotspot verbinden (und das Zugangspasswort eingegeben haben), teilen sich diese eine Verbindung.

**Technischer Hintergrund:** Möglich wird das durch NAT/IP-Weiterleitung (NAPT), die im ESP32-Boardpaket bereits aktiviert ist (`esp_netif_napt_enable`) – es ist **keine** zusätzliche Bibliothek und kein Custom-Build nötig, das läuft mit der normalen Arduino-IDE-Installation aus Abschnitt C.

**Grenzen:** Ein ESP32 ist kein vollwertiger Router – für viele gleichzeitige Geräte ist die Bandbreite begrenzt. Prüfe außerdem vorher, ob das Teilen eines WLANs (z. B. des Schul-WLANs) über ein eigenes Gerät erlaubt ist – viele Netzwerk-Richtlinien untersagen das Einbringen zusätzlicher Access Points.

## H. Was geprüft wurde – und was nicht

In der Entwicklungsumgebung stand **keine ESP32-Toolchain** zur Verfügung (Download-Server gesperrt). Deshalb gilt:

- ✅ Firmware-Quelltext gegen die **echten Header des ESP32-Cores 3.3.12 / ESP-IDF 5.5** gelesen, Signaturen abgeglichen, mit `g++ -Wall -Wextra` gegen daraus abgeleitete Stubs syntaktisch geprüft (auch gegen einen älteren Core).
- ✅ Reine Logik (Passwort-Hash, Zugangspasswort-Prüfung/Ablauf, JSON-Ausgabe) auf dem PC mit AddressSanitizer/UndefinedBehaviorSanitizer getestet.
- ✅ Weboberfläche in Chromium (Desktop + Smartphone-Größe) gegen einen Mock-Server durchgetestet: Admin-Login, Status, WiFi-Hotspot-Einstellungen, Zugangspasswort ändern, bekannte WLANs suchen/hinzufügen/entfernen, Admin-Zugang ändern, Werkseinstellungen, Abmelden – sowie die Login-Seite selbst (falsches/richtiges Zugangspasswort, Abmelden).
- ✅ Internet teilen (NAT): Die `esp_netif_napt_enable`/`esp_netif_set_default_netif`-Aufrufe wurden gegen den echten ESP-IDF-Quelltext (Beispiel `examples/wifi/softap_sta`) geprüft – NAPT ist in den vorkompilierten Bibliotheken des Boardpakets bereits aktiviert (`CONFIG_LWIP_IPV4_NAPT=y`). Das selbstgebaute DNS-Antwortpaket (Captive-Portal-Redirect) wurde per Host-Test byteweise geprüft.
- ⚠️ **Nicht auf echter Hardware getestet:** Kompilierung mit der echten Toolchain, das tatsächliche automatische Wechseln zwischen bekannten WLANs an unterschiedlichen Orten, Captive-Portal-Verhalten auf echten Geräten, und der tatsächliche Datendurchsatz/die Stabilität beim Teilen unter mehreren gleichzeitigen Geräten. Sollte der Compiler etwas beanstanden, schick mir die Fehlermeldung.

## I. Vorschau ohne ESP32

- `Vorschau/Vorschau.html` – die komplette Verwaltungsoberfläche gegen einen simulierten ESP32 (Login `admin` / `admin123`).
- `Vorschau/Login-Seite-Vorschau.html` – die Login-Seite, die Geräte am Hotspot sehen (Zugangspasswort `hotspot123`).

Beide Dateien lassen sich einfach per Doppelklick im Browser öffnen, es wird nichts an einen ESP32 gesendet.
