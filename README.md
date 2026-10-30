# RSC Remote Analyzer

Automatic Sub-GHz remote control analyzer for the Flipper Zero, built for the **Momentum** firmware.

Press and hold the button of an unknown gate, barrier or other RF remote: the app finds the frequency,
tries the four receive presets one after another (**AM270, AM650, FM238, FM476**) with every protocol
decoder of the firmware, and shows what it found. If the remote stops transmitting before every
preset was tested, it asks for another press and continues with the results it already has.

The app is **receive only** – it never transmits. The UI is **English** by default, with Hungarian
selectable in *Settings › Language/Nyelv* (Hungarian is shown without accents, the Flipper fonts have
none). User guide: [USAGE.md](USAGE.md) (English), [HASZNALAT.md](HASZNALAT.md) (Hungarian); the original
specification (Hungarian) is [SPECIFIKACIO.md](SPECIFIKACIO.md).

## How it works

1. **Frequency search** – coarse RSSI sweep over the Sub-GHz frequency list (`subghz/assets/setting_user`)
   with the OOK 650 kHz preset, keeping a slowly fading peak per frequency across passes (a single read
   can fall into a gap of an OOK signal). If the strongest frequency is in the upper band and half of it
   carries a signal right now (measured directly), the half wins – a near 433.92 MHz remote is also
   heard on its 867.84 MHz harmonic. With *Auto* sensitivity (default) every list frequency counts a
   signal from 12 dB over its own noise estimate (the lowest reading, rising slowly).
   On a hit above the threshold a confirmation read, then a fine sweep (±300 kHz in 20 kHz steps, then
   ±20 kHz in 5 kHz steps) with the same preset narrowed to a 58 kHz channel filter. Like the coarse
   sweep it runs in quick rounds keeping the peak of every step, until three rounds heard the remote:
   a short press that ends halfway still leaves the right peak. If the first fine sweep heard nothing,
   the search starts again with the next press; if only the second one did not, the first result
   stands. The result is snapped to the nearest list frequency within 75 kHz. When a later search finds another frequency, the
   presets are tested again from the start.
2. **Modulation search** – for each preset: async RX on the found frequency, `SubGhzWorker` →
   `SubGhzReceiver` with all decodable protocols (KeeLoq keystore and rainbow tables loaded as in the
   Sub-GHz app; weather stations and TPMS ignored). A preset is finished after two decodes, after the
   window (0.6 / 1 / 1.6 s of signal), or when the signal ends after at least 250 ms of it. Signal
   presence comes from the RSSI sampled every 5 ms – in Auto against the preset's own noise + 10 dB,
   with a fixed sensitivity against 10 dB under the level the frequency search found (clamped to the
   setting and at most 10 dB below it) – and every decode counts as signal too; a 350 ms
   silence means the remote stopped. After 5 s without a press the frequency is searched again, keeping
   the presets already tested. With *Debug log* on, each scan writes
   `apps_data/rsc_remote_analyzer/debug_logs/debug_log_1.txt`; the last 5 scans are kept (`_1` is the
   newest).
3. **Result** – the protocol with the most decodes wins; its most successful preset is the modulation.
   The protocol type (`Static` / `Dynamic`) gives fixed code / rolling code. Without a decode the result
   is "unknown protocol": the modulation is only a guess from the pulse statistics, and nothing is
   claimed about fixed or rolling code.

## Saving

- `apps_data/rsc_remote_analyzer/reports/<name>.txt` – analysis report (always).
- `subghz/rsc_remote_analyzer/<name>.sub` – a standard Flipper SubGhz Key File for a decoded
  **fixed-code** protocol, or a Flipper SubGhz RAW File of the best-looking capture for an unknown one.
  Rolling-code protocols get the report only.

The report keeps English keys and values in both UI languages.

## Translations

Every UI string is an English/Hungarian pair in the source, picked by the `L(en, hu)` macro from the
language setting (`g_lang`); string arrays are indexed by `[LANG_COUNT]`.

## Build

```
ufbt
ufbt launch
```

Built with ufbt against the Momentum dev SDK. On Windows, build from a path without `!` (ufbt's
compilation database step rejects it).

## Licence

MIT – see [LICENSE](LICENSE).
