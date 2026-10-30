# RSC Remote Analyzer – Anleitung

Flipper-Zero-App (Momentum-Firmware) zur Analyse unbekannter Tor-, Schranken- und anderer
Funkfernbedienungen. Die Taste der Fernbedienung drücken und gedrückt halten: Der Flipper findet die
Frequenz selbst, probiert die vier Empfangs-Presets durch und versucht, das Protokoll zu erkennen.

Die App **empfängt nur, sie sendet nie**. Auf dem Flipper: *Apps › Sub-GHz › RSC Remote Analyzer*.

Die Oberfläche ist nach der Installation **englisch**; Deutsch wird unter *Settings › Language* gewählt
(die Zeile heißt in jeder Sprache gleich). Diese Anleitung zitiert die deutschen Texte; englisch:
[USAGE.md](USAGE.md), ungarisch: [HASZNALAT.md](HASZNALAT.md).

> Die Schrift des Flippers kennt keine Umlaute, darum stehen auf dem Display ae, oe, ue und ss
> („Taste gedrueckt halten!“).

## Hauptbildschirm

- **Automatische Analyse** (die große Taste) – Frequenz, Modulation und Protokoll von selbst.
- **Manuelle Analyse** – die Frequenz wählst du; die Modulation ist *Auto* (alle vier) oder ein
  gewähltes Preset; gestartet wird mit der Zeile „Start (OK)“.
- **Gespeicherte Analysen** – die Berichte früherer Speicherungen.
- **Einstellungen** – siehe unten.

Hoch/runter wählt, OK startet, Back beendet.

## Automatische Analyse Schritt für Schritt

1. Starten, die Fernbedienung nah an den Flipper halten (10–50 cm) und ihre Taste **drücken und
   gedrückt halten**.
2. **Frequenzsuche:** Der Flipper durchsucht die Sub-GHz-Frequenzliste (`subghz/assets/setting_user`),
   verfeinert um das stärkste Signal mit einem schmalen Filter und rundet auf die nahe
   Standardfrequenz (z. B. 433,92 MHz). Eine sehr nah gehaltene Fernbedienung ist auch auf der
   doppelten Frequenz zu hören (433,92 → etwa 868 MHz); die App erkennt das und nimmt die Grundfrequenz.
3. **Modulation:** Sie probiert **AM270, AM650, FM238, FM476** nacheinander, jedes mit allen Decodern
   der Firmware. Von den vier Kästchen ist das gefüllte das gerade getestete; darunter **voller
   Balken** = dekodiertes Signal, **punktierte Linie** = getestet, nicht dekodiert.
4. Bekommt ein Preset zwei Dekodierungen, geht es sofort weiter; sonst hört es bis zum Ende des
   Fensters (Einstellungen › Fenster).
5. **Hört die Fernbedienung zwischendurch auf zu senden**, erscheint unten **„Bitte die Taste erneut
   druecken!“** (und der Flipper vibriert). Erneut drücken – die Analyse geht dort weiter, wo sie
   stand; frühere Ergebnisse bleiben.
6. Kommt auf der gefundenen Frequenz 5 Sekunden lang nichts, sucht der Flipper die Frequenz neu
   („Taste gedrueckt halten!“); getestete Presets und Dekodierungen bleiben. Findet er diesmal eine
   andere Frequenz, testet er die Presets dort von vorn.
7. **OK** jederzeit: fertig, zeigt das Ergebnis aus dem bisher Gefundenen. **Back**: Abbruch.

## Ergebnis

- **Bekanntes Protokoll:** Name, Festcode / Rolling Code, Frequenz, wahrscheinlichste Modulation,
  Sicherheit (*hoch*: mindestens 3 Dekodierungen oder auf zwei Presets; *gut*: 2; *niedrig (1x)*: 1)
  und die erste Zeile des Codes.
- **„Unbekanntes Protokoll – weitere Analyse noetig.“** – Es gab ein Signal, aber kein Decoder hat es
  erkannt. Die Modulation ist dann nur *vermutlich* (aus dem Pulsmuster), und die App sagt nicht, ob
  Fest- oder Rolling Code – sie kann es nicht wissen.
- **„Kein Signal empfangen“** – während der Analyse kam keine Sendung.

Tasten: **Neu** (noch eine Analyse auf die gleiche Art), **Speichern**, **Mehr** (Detaildaten,
Modulationen – das Ergebnis aller vier Presets –, Kompatibilität).

## Festcode und Rolling Code

**Woher weiß die App das?** Nicht aus einer Messung des Signals, sondern aus dem erkannten Protokoll:
Jeder Decoder der Firmware weiß, ob sein Protokoll Fest- oder Rolling Code ist (Protokolltyp *Static* /
*Dynamic*), darum genügt eine erfolgreiche Dekodierung. Bei einem unbekannten Protokoll gibt es nichts,
worauf sich das stützen ließe, und die App behauptet nichts.

- **Festcode**-Protokoll: Jeder Druck sendet denselben Code. Die gespeicherte `.sub`-Datei öffnet sich in
  der Sub-GHz-App (*Saved*). Senden nur am eigenen System und nach den örtlichen Regeln.
- **Rolling-Code**-Protokoll: Jeder Druck sendet einen neuen Code, darum lässt es sich durch einfaches
  Aufnehmen und Abspielen **nicht kopieren**; eine neue Fernbedienung wird laut Anleitung am Empfänger
  angelernt. Gespeichert wird dann **nur der Bericht**, keine `.sub`-Datei.
- Die Zähler *Gleicher Code* und *Codewechsel* in den *Detaildaten* helfen: Geben zwei Drücke
  verschiedene Codes, deutet das auf Rolling Code (oder auf zwei verschiedene Tasten).

## Speichern

Nach *Speichern* kannst du einen Namen vergeben. Der angebotene Name (Monat, Tag, Stunde, Minute:
z. B. `RA_1030_0915`) ist markiert: Tippen ersetzt ihn, OK behält ihn. Auf die SD-Karte kommt:

| Datei | Wann |
| --- | --- |
| `apps_data/rsc_remote_analyzer/reports/<Name>.txt` – der Bericht (Zeit, Frequenz, Modulation, Protokoll, das Ergebnis der vier Presets, die dekodierten Daten) | immer |
| `subghz/<Name>.sub` – Flipper SubGhz Key File | erkanntes Festcode-Protokoll |
| `subghz/<Name>.sub` – Flipper SubGhz RAW File (zur Analyse) | unbekanntes Protokoll, wenn es eine Aufnahme gab |

Die `.sub`-Dateien sind im eigenen Format des Flippers; die Sub-GHz-App und ProtoView öffnen sie. Die
Berichte sind unter *Gespeicherte Analysen* auf dem Hauptbildschirm lesbar. Schlüssel und Werte des
Berichts sind in jeder Sprache der Oberfläche englisch (z. B. `Confidence: high`), damit jede Speicherung
gleich aufgebaut ist.

## Einstellungen

| Einstellung | Bedeutung |
| --- | --- |
| Language | **English**, Magyar oder Deutsch |
| Empfindlichkeit | was als Signal zählt: **Auto** (unten), oder fest Niedrig −65 dBm, Normal −75 dBm, Hoch −85 dBm (Niedrig an lauten Orten, Hoch für eine schwache Fernbedienung) |
| Fenster | wie lange mit einem Preset gehört wird: Kurz 0,6 s, **Normal 1 s**, Lang 1,6 s |
| Alle 4 Presets | **Ja**: immer alle vier; Nein: hält bei der ersten sicheren Dekodierung an |
| Rückmeldung | Vibration und LED (Dekodierung, Bitte um erneuten Druck, Ergebnis) |
| Debug-Log | **Aus**; An: jede Analyse schreibt ein Protokoll nach `apps_data/rsc_remote_analyzer/debug_logs/` (Zeit, Signalpegel und Schritte der letzten etwa 16 s); die letzten 5 Analysen bleiben, `debug_log_1.txt` ist die neueste – für Fehlermeldungen |

**Auto-Empfindlichkeit (Standard):** Ein Signal wird am Rauschen seiner eigenen Frequenz und seines
Presets gemessen, nicht an einem festen dBm-Wert. Bei der Frequenzsuche zählt jede Frequenz ein Signal ab
12 dB über ihrem eigenen Rauschen (eine laute Frequenz bekommt eine höhere Schwelle); während der Analyse
zählt ein erneuter Druck ab 10 dB über dem Rauschen des Presets. **Eine erfolgreiche Dekodierung zählt
immer als Signal**, auch wenn der Pegel schwächer ist – so fragt eine weiter entfernte, schwach empfangene
Fernbedienung nicht unnötig nach weiteren Drücken.

Bei fester Empfindlichkeit zählt ein erneuter Druck ab 10 dB unter dem Pegel, den die Frequenzsuche
gefunden hat, aber nie strenger als die Einstellung und höchstens 10 dB lockerer (Normal: zwischen −75
und −85 dBm). Eine feste Einstellung kommt nie näher als 6 dB an das gemessene Rauschen – Hoch würde an
einem lauten Ort sonst das Rauschen für eine Fernbedienung halten.

Die Empfindlichkeit erhöht die Reichweite nicht: Was an der kleinen Antenne des Flippers unter das
Rauschen fällt, wird mit keiner Einstellung empfangen.

## Grenzen

- Das CC1101-Funkmodul des Flippers empfängt jeweils nur eine Modulation, darum folgen die Presets
  nacheinander: Eine Fernbedienung, die nur kurz sendet, braucht mehrere Drücke.
- Frequenz und Modulation zu finden heißt nicht, dass sich das Protokoll erkennen lässt.
- Diese Version nutzt das interne Funkmodul (ein externes CC1101-Modul wird nicht unterstützt).
- Erkannt werden nur Protokolle, für die die Momentum-Firmware einen Decoder hat.
- KeeLoq wird erkannt und als Rolling Code angezeigt, aber ohne Herstellernamen: Der Herstellerschlüssel-
  speicher wird nicht geladen, weil mit ihm zu wenig Speicher für eine Analyse blieb (die Sub-GHz-App
  zeigt den Hersteller).
