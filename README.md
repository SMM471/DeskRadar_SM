# DeskRadar – Nachbau-Anleitung

Ein ESP32 mit rundem 1.28"-Display zeigt Flugzeuge in deiner Umgebung live als
Radar an (Daten von [adsb.fi](https://adsb.fi), kostenlos, keine Anmeldung
nötig). Diese Anleitung richtet sich an Leute, die **noch nie mit einem ESP32
gearbeitet haben**.

Das passende 3D-druckbare Gehäuse findest du auf MakerWorld (Link von
Steffen Moll, siehe Projektbeschreibung).

---

## 1. Was du brauchst

### Hardware
- 1× ESP32-WROOM-32 DevKit (30-Pin, mit USB-C) – generisches Board, wie im
  Bild
- 1× rundes 1.28" SPI-TFT-Display, 240×240 Pixel, Treiber-IC **GC9A01**
- 7 Jumper-/Litzenkabel (Display → ESP32)
- 1× USB-C-Kabel (Daten, nicht nur Ladekabel!) zum Flashen und Stromversorgen
- Optional: 3D-gedrucktes Gehäuse

### Software
- [Arduino IDE](https://www.arduino.cc/en/software) (Version 2.x empfohlen)

---

## 2. Verkabelung

| Display-Pin | ESP32-Pin      | Bedeutung                      |
|--------------|----------------|---------------------------------|
| VCC          | 3V3            | Stromversorgung                |
| GND          | GND            | Masse                           |
| SCL          | GPIO18         | SPI Clock                       |
| SDA          | GPIO23         | SPI Data (MOSI)                 |
| CS           | GPIO15         | Chip Select                     |
| DC           | GPIO2          | Data/Command                    |
| RST          | GPIO4          | Reset                           |
| BLK (Backlight) | GPIO21      | Hintergrundbeleuchtung (wird im Code direkt geschaltet) |

Der eingebaute **BOOT-Taster** auf dem ESP32-Board (GPIO0) wird im Betrieb als
Settings-Taster wiederverwendet – dafür musst du nichts extra verkabeln.

> Falls dein Board/Display abweichend beschriftet ist: die Pin-Namen auf der
> Display-Platine (VCC, GND, SCL, SDA, CS, DC, RST) sind Standard und sollten
> bei jedem GC9A01-Display gleich heißen.

---

## 3. Arduino IDE vorbereiten

### 3.1 ESP32-Unterstützung installieren
1. Arduino IDE öffnen → **Datei → Voreinstellungen** (Windows/Linux) bzw.
   **Arduino IDE → Einstellungen** (Mac).
2. Bei **„Zusätzliche Boardverwalter-URLs"** einfügen:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. **Werkzeuge → Board → Boardverwalter**, nach `esp32` suchen, das Paket
   **„esp32 by Espressif Systems"** installieren.
4. **Werkzeuge → Board** → „ESP32 Dev Module" auswählen.

### 3.2 Benötigte Bibliotheken installieren
**Werkzeuge → Bibliotheken verwalten**, jeweils installieren:
- `TFT_eSPI` (von Bodmer)
- `ArduinoJson` (von Benoit Blanchon, Version 7.x)

(`WiFi`, `WebServer`, `HTTPClient`, `Preferences`, `FS`, `SPI` sind bereits
im ESP32-Boardpaket enthalten, dafür musst du nichts extra installieren.)

### 3.3 TFT_eSPI für dieses Display konfigurieren

Das ist der Schritt, an dem die meisten Einsteiger hängen bleiben – die
Bibliothek weiß von sich aus nicht, welches Display und welche Pins du
verwendest. Das wird über eine Konfigurationsdatei namens `User_Setup.h`
festgelegt.

1. Finde den Ordner der TFT_eSPI-Bibliothek. Normalerweise liegt er unter:
   - Windows: `Dokumente\Arduino\libraries\TFT_eSPI`
   - Mac: `~/Documents/Arduino/libraries/TFT_eSPI`
2. Öffne die Datei `User_Setup_Select.h` in diesem Ordner mit einem Texteditor.
3. Kommentiere die Standardzeile
   ```cpp
   #include <User_Setup.h>
   ```
   **nicht** aus – wir bearbeiten stattdessen direkt `User_Setup.h`.
4. Öffne `User_Setup.h` und ersetze den Inhalt durch folgende Einstellungen
   (bzw. füge sie so ein, dass nur diese Treiber-/Pin-Definitionen aktiv
   sind):

   ```cpp
   #define GC9A01_DRIVER

   #define TFT_WIDTH  240
   #define TFT_HEIGHT 240

   #define TFT_MISO -1
   #define TFT_MOSI 23
   #define TFT_SCLK 18
   #define TFT_CS   15
   #define TFT_DC    2
   #define TFT_RST   4
   #define TFT_BL   21
   #define TFT_BACKLIGHT_ON HIGH

   #define LOAD_GLCD
   #define LOAD_FONT2
   #define LOAD_FONT4

   #define SPI_FREQUENCY  40000000
   ```

   Diese Werte entsprechen exakt der Verkabelung aus Abschnitt 2.

---

## 4. Code hochladen

1. Öffne `DeskRadar.ino` in der Arduino IDE.
2. ESP32 per USB-C mit dem Rechner verbinden.
3. **Werkzeuge → Port** → den passenden COM-/USB-Port auswählen (bei manchen
   Boards muss dafür ein CP2102/CH340-USB-Treiber installiert sein – die IDE
   zeigt einen Hinweis, falls das nötig ist).
4. Auf den **Upload-Pfeil** (→) klicken.
5. Falls der Upload nicht startet: BOOT-Taster am Board gedrückt halten,
   während der Upload beginnt, dann loslassen (nur bei manchen Boards nötig).

Nach erfolgreichem Upload bootet das Display und zeigt kurz `SYSTEM BOOT...`.

---

## 5. Ersteinrichtung (WLAN & Standort)

Beim allerersten Start ist noch kein WLAN gespeichert, das Gerät öffnet
automatisch einen eigenen Access Point:

1. Auf dem Display steht `WIFI FAILED` → `PORTAL ACTIVE`.
2. Mit Handy/Laptop im WLAN-Menü nach **`DeskRadar-Setup-bySteffen Moll`**
   suchen und verbinden.
3. Browser öffnen, Adresse `192.168.4.1` aufrufen (öffnet sich bei manchen
   Handys automatisch als Popup).
4. Formular ausfüllen:
   - **Device PIN**: `1991`
   - **WLAN auswählen** (aus der Liste) + Passwort eingeben
   - **Latitude / Longitude**: Koordinaten deines Standorts (z. B. von
     [google maps](https://maps.google.com) rechte Maustaste → Koordinaten
     kopieren)
   - **Range (KM)**: Radius, in dem Flugzeuge angezeigt werden sollen
   - Häkchen für Sweep-Animation / Metrisches System nach Wunsch
5. **SAVE & REBOOT** klicken. Das Gerät startet neu und verbindet sich mit
   deinem WLAN.

Danach lädt das Radar automatisch Flugdaten von adsb.fi und zeigt sie an.

---

## 6. Einstellungen später ändern

Den **BOOT-Taster** am ESP32 (bzw. den Taster am Gehäuse, falls verbaut)
**3 Sekunden gedrückt halten** → Gerät startet neu und öffnet wieder das
Setup-Portal wie in Schritt 5, mit allen bisherigen Werten vorausgefüllt.

---

## 7. Problembehebung

| Problem | Lösung |
|---|---|
| Display bleibt schwarz | Verkabelung prüfen (v. a. CS = GPIO15, nicht der oft in Tutorials genannte GPIO5), `User_Setup.h` prüfen |
| Upload schlägt fehl / Port fehlt | USB-Treiber (CP2102/CH340) installieren, anderes USB-Kabel probieren (Datenkabel, kein reines Ladekabel) |
| "WIFI FAILED" bei jedem Boot | WLAN-Passwort falsch gespeichert → Settings-Taster 3s halten, neu einrichten |
| Keine Flugzeuge sichtbar | Range (km) erhöhen, prüfen ob gerade wirklich Flugverkehr in der Nähe ist, Internetverbindung des ESP32 prüfen |
