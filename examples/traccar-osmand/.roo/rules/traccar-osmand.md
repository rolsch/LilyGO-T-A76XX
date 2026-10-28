# traccar-osmand
- idf.py ist vorhanden
- Vor allen Hardwareänderungen HARDWARE.md lesen.
- Niemals GPIOs erfinden.
- Vor Flash immer zuerst:
  idf.py build
- Flash nur nach ausdrücklicher Zustimmung.
- Niemals idf.py monitor ohne timeout.
- Maximal 3 automatische Build-Fix-Zyklen.
- Target niemals eigenständig ändern.
- Offizielle ESP-Component-Registry bevorzugen.
- Bei Mehrplatinen-Vorlagen nur das explizit genannte Board verwenden.
- Vor Änderungen README.md, HARDWARE.md,
  sdkconfig und CMakeLists.txt prüfen.
- Keine interaktiven Terminalprogramme starten, alle Befehle müssen automatisch terminieren.
- Niemals das gesamte Projekt einlesen.
- Nur Dateien öffnen, die direkt vom aktuellen Fehler betroffen sind.
- Build-Ausgaben auf die ersten 20 und letzten 100 relevanten Zeilen begrenzen.
- .pio, build, managed_components und .git ignorieren.
