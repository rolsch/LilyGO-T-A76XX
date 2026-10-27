# lilygo_t_a7670

# ESP-IDF Autonomous Agent Rules

1. **Umgebung:**
   - Die ESP-IDF v5.3 Umgebung ist bereits global aktiv (`idf.py` ist direkt im PATH verfügbar).

2. **Build & Autonome Fehlerkorrektur:**
   - Führe zum Kompilieren `idf.py build` im jeweiligen Projektordner aus.
   - Stelle sicher, dass `compile_commands.json` im `build/`-Ordner generiert wird (Standard bei `idf.py`).
   - Wenn `idf.py build` fehlschlägt, analysiere den Compiler-/CMake-Output, korrigiere den C/C++-Code oder die `main/CMakeLists.txt` (`REQUIRES` / `PRIV_REQUIRES` in ESP-ID>

3. **Flashen & Serieller Monitor (WICHTIG - Terminal-Blockade vermeiden):**
   - Frage immer ob Flash ausgeführt werden muss, und nutze `idf.py flash` statt `idf.py build`.
   - Starte NIEMALS `idf.py monitor` ohne Zeitlimit, da dies das Terminal dauerhaft blockiert!
   - Nutze zum Flashen: `idf.py -p /dev/ttyUSB0 flash` (oder `/dev/ttyACM0`).
   - Um Boot-Logs oder Guru Meditation Errors auszulesen, nutze zwingend `timeout`:
     `timeout 10s idf.py -p /dev/ttyUSB0 monitor || true` oder `python3 -m serial.tools.miniterm /dev/ttyUSB0 115200 --raw | head -n 100`.

4. **Hardware-Kontext (`HARDWARE.md`):**
   - Prüfe vor dem Anlegen oder Ändern von Code IMMER, ob im Projektverzeichnis eine `HARDWARE.md` liegt.
   - Verwende ausschließlich die dort definierten GPIO-Pins, das dort genannte `idf.py set-target <chip>` sowie die angegebenen Einschaltsequenzen (z. B. Modem PWRKEY).
   - Erfinde niemals eigene GPIO-Belegungen für Drittanbieter-Boards (wie LilyGo, Heltec, Waveshare). Wenn ein Pin in `HARDWARE.md` fehlt, frage den Nutzer.
   - Nutze für externe Module bevorzugt offizielle Pakete aus der ESP Component Registry (`idf.py add-dependency ...`).

5. **Hardware-Extraktion aus Vorlageprojekten:**
   - Wenn du angewiesen wirst, aus einem Vorlageprojekt oder Git-Repo eine `HARDWARE.md` zu erstellen, suche gezielt nach `utilities.h`, `pins.h`, `board_def.h`, `platformio>
   - **ACHTUNG bei Multi-Board-Vorlagen:** Enthalten Dateien wie `utilities.h` mehrere `#ifdef`-Blöcke für verschiedene Board-Modelle oder PCB-Revisionen (z. B. v1.1 vs. v1.>
   - Übersetze Arduino-Einschaltsequenzen (`pinMode`, `digitalWrite`, `delay`) in der `HARDWARE.md` direkt in präzise Angaben für ESP-IDF (`gpio_set_level`, `vTaskDelay(pdMS>
   - Leite aus `platformio.ini` (z. B. `qio_opi` vs. `qio_qspi`) direkt die korrekten ESP-IDF Kconfig-Parameter (`CONFIG_SPIRAM_MODE_OCT=y` vs. `CONFIG_SPIRAM_MODE_QUAD=y`) >
EOF
