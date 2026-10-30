# RSC Remote Analyzer – user guide

Flipper Zero app (Momentum firmware) for analysing unknown gate, barrier and other RF remotes. Press and
hold the remote's button: the Flipper finds the frequency by itself, tries the four receive presets and
tries to identify the protocol.

The app **only receives, it never transmits**. On the Flipper: *Apps › Sub-GHz › RSC Remote Analyzer*.

The UI is **English** by default; Hungarian (Magyar) or German (Deutsch) can be chosen in
*Settings › Language* (the same label in every language). German guide: [ANLEITUNG.md](ANLEITUNG.md),
Hungarian: [HASZNALAT.md](HASZNALAT.md).

## Main screen

- **Automatic scan** (the big button) – frequency, modulation and protocol on their own.
- **Manual scan** – you pick the frequency; the modulation is *Auto* (all four) or one chosen preset;
  start with the "Start (OK)" row.
- **Saved scans** – the reports of earlier saves.
- **Settings** – see below.

Up/down selects, OK starts, Back exits.

## Automatic scan step by step

1. Start it, hold the remote close to the Flipper (10–50 cm) and **press and hold** its button.
2. **Frequency search:** the Flipper sweeps the Sub-GHz frequency list (`subghz/assets/setting_user`),
   refines around the strongest signal with a narrow filter and snaps to the nearby standard frequency
   (e.g. 433.92 MHz). A remote held very close is also heard on its double frequency (433.92 → about
   868 MHz); the app recognises this and takes the base frequency.
3. **Modulation:** it tries **AM270, AM650, FM238, FM476** one after another, each with every decoder of
   the firmware. Of the four boxes on the screen the filled one is being tried; below them a **solid
   bar** = decoded signal, a **dotted line** = tested, no decode.
4. When a preset gets two decodes it moves on at once; otherwise it listens until the end of the window
   (Settings › Window).
5. **If the remote stops transmitting meanwhile**, the bottom of the screen shows **"Press the remote
   button again!"** (and vibrates). Press it again – the scan goes on where it stopped, earlier results
   are kept.
6. If nothing comes on the found frequency for 5 seconds, the Flipper searches the frequency again
   ("Hold the remote button!"); the presets already tested and the decodes are kept. If it finds a
   different frequency this time, the presets are tested again from the start on the new one.
7. **OK** at any time: done, shows the result from what was found so far. **Back**: cancel.

## Result

- **Known protocol:** name, fixed code / rolling code, frequency, most likely modulation, confidence
  (*high*: at least 3 decodes or decodes on two presets; *good*: 2; *low (1x)*: 1) and the first line of the
  code.
- **"Unknown protocol – further analysis needed."** – there was a signal, but no decoder recognised it.
  The modulation is then only *likely* (from the pulse pattern), and the app does not say whether it is
  fixed or rolling code – it cannot know.
- **"No signal received"** – no transmission came during the scan.

Buttons: **New** (another scan the same way), **Save**, **More** (Detailed data, Modulations – the result
of all four presets –, Compatibility).

## Fixed code and rolling code

**How does it know?** Not by measuring the signal but from the recognised protocol: every decoder of the
firmware knows whether its protocol is fixed or rolling code (protocol type *Static* / *Dynamic*), so one
successful decode is enough. For an unknown protocol there is nothing to go by, and the app claims nothing.

- **Fixed code** protocol: every press sends the same code. The saved `.sub` file opens in the Sub-GHz
  app (*Saved*). Transmit only on your own system and within the local rules.
- **Rolling code** protocol: every press sends a new code, so it **cannot be copied** by simple recording
  and replay; a new remote is used by pairing it with the receiver as its manual describes. Saving then
  writes **the report only**, no `.sub` file.
- The *Same repeats* and *Code changes* counts in *Detailed data* help: if two presses give different
  codes, that points to rolling code (or to two different buttons).

## Saving

After *Save* you can give a name. The offered name (month, day, hour, minute: e.g. `RA_1030_0915`) is
selected, so typing replaces it; OK keeps it. What goes to the SD card:

| File | When |
| --- | --- |
| `apps_data/rsc_remote_analyzer/reports/<name>.txt` – the report (time, frequency, modulation, protocol, the result of the four presets, the decoded data) | always |
| `subghz/<name>.sub` – Flipper SubGhz Key File | recognised fixed-code protocol |
| `subghz/<name>.sub` – Flipper SubGhz RAW File (for analysis) | unknown protocol, if there was a capture |

The `.sub` files are in the Flipper's own format; the Sub-GHz app and ProtoView open them. The reports
can be read in *Saved scans* on the main screen. The report file has the same English keys and values in
every UI language.

## Settings

| Setting | Meaning |
| --- | --- |
| Language | **English**, Magyar (Hungarian) or Deutsch (German) |
| Sensitivity | what counts as a signal: **Auto** (below), or fixed Low −65 dBm, Normal −75 dBm, High −85 dBm (Low in a noisy place, High for a weak remote) |
| Window | how long it listens with one preset: Short 0.6 s, **Normal 1 s**, Long 1.6 s |
| All 4 presets | **Yes**: always tries all four; No: stops at the first sure decode |
| Feedback | vibration and LED (decode, request for another press, result) |
| Debug log | **Off**; On: every scan writes a log to `apps_data/rsc_remote_analyzer/debug_logs/` (scan time, signal levels and steps of the last ~16 s); the last 5 scans are kept, `debug_log_1.txt` is the newest – to report a problem |

**Auto sensitivity (default):** a signal is measured against the noise of its own frequency and preset,
not against a fixed dBm value. In the frequency search every frequency counts a signal from 12 dB over its
own noise (a noisy frequency gets a higher threshold); during the scan a new press counts from 10 dB over
the preset's noise. **A successful decode always counts as a signal**, even when the level is weaker, so a
remote received weakly from further away does not ask for needless extra presses.

With a fixed sensitivity a new press counts from 10 dB under the level the frequency search found, but
never stricter than the setting and at most 10 dB looser (Normal: between −75 and −85 dBm). A fixed
setting never goes closer than 6 dB to the measured noise – High in a noisy place would otherwise take
the noise for a remote.

Sensitivity does not extend the range: what falls under the noise on the Flipper's small antenna is not
received with any setting.

## Limits

- The Flipper's CC1101 radio receives one modulation at a time, so the presets follow each other: a
  remote that transmits only briefly needs several presses.
- Finding the frequency and the modulation does not mean the protocol can be identified.
- This version uses the internal radio (an external CC1101 module is not supported).
- It recognises only the protocols the Momentum firmware has a decoder for.
- KeeLoq is recognised and shown as rolling code, but without the manufacturer name: the manufacturer
  keystore is not loaded, because with it the app left too little memory for a scan (the Sub-GHz app
  shows the manufacturer).
