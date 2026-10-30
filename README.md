# RSC Remote Analyzer

Automatic Sub-GHz remote control analyzer for the Flipper Zero, built for the **Momentum** firmware.

Press and hold the button of an unknown gate, barrier or other RF remote: the app finds the frequency,
tries the four receive presets one after another (**AM270, AM650, FM238, FM476**) with every protocol
decoder of the firmware, and shows what it found. If the remote stops transmitting before every
preset was tested, it asks for another press and continues with the results it already has.

The app is **receive only** – it never transmits. The UI is Hungarian (without accents, the Flipper
fonts have none); the user guide is [HASZNALAT.md](HASZNALAT.md), the original specification is
[SPECIFIKACIO.md](SPECIFIKACIO.md).

## How it works

1. **Frequency search** – coarse RSSI sweep over the Sub-GHz frequency list (`subghz/assets/setting_user`)
   with the OOK 650 kHz preset; on a hit above the threshold a confirmation read, then a fine sweep
   (±300 kHz in 20 kHz steps, then ±20 kHz in 5 kHz steps) with the same preset narrowed to a 58 kHz
   channel filter. The result is snapped to the nearest list frequency within 75 kHz.
2. **Modulation search** – for each preset: async RX on the found frequency, `SubGhzWorker` →
   `SubGhzReceiver` with all decodable protocols (KeeLoq keystore and rainbow tables loaded as in the
   Sub-GHz app; weather stations and TPMS ignored). A preset is finished after two decodes, after the
   window (0.6 / 1 / 1.6 s of signal), or when the signal ends after at least 250 ms of it. Signal
   presence comes from the RSSI sampled every 5 ms; a 350 ms silence means the remote stopped.
3. **Result** – the protocol with the most decodes wins; its most successful preset is the modulation.
   The protocol type (`Static` / `Dynamic`) gives fixed code / rolling code. Without a decode the result
   is "unknown protocol": the modulation is only a guess from the pulse statistics, and nothing is
   claimed about fixed or rolling code.

## Saving

- `apps_data/rsc_remote_analyzer/reports/<name>.txt` – analysis report (always).
- `subghz/rsc_remote_analyzer/<name>.sub` – a standard Flipper SubGhz Key File for a decoded
  **fixed-code** protocol, or a Flipper SubGhz RAW File of the best-looking capture for an unknown one.
  Rolling-code protocols get the report only.

## Build

```
ufbt
ufbt launch
```

Built with ufbt against the Momentum dev SDK. On Windows, build from a path without `!` (ufbt's
compilation database step rejects it).

## Licence

MIT – see [LICENSE](LICENSE).
