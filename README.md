# FLOOR//01 — Bass Overdrive

A self-contained live groove box for Linux. C++17 / Qt 6 Widgets / SDL2.
No browser, external samples server or network connection required to play —
all drums are the bundled CC0 one-shots plus the built-in synth voices.

When you want a second, touch screen to play along from an iPad, the app can
expose a tiny read/write control page over your local network (see below).

## Build

Requirements: CMake, a C++17 compiler, Qt6 Widgets/Network, SDL2,
libqrencode, pkg-config.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/floor01 --self-test demo.wav
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=generic SDL_AUDIODRIVER=dummy ./build/floor01 --smoke-test
```

The self-test renders audio, verifies bounds / scene switching / swing timing /
WAV output and the CC0 sample playback path.

## Controls

- Space / Play: start & stop (stop also silences delays).
- Keys 1–8 (scene buttons): next-bar scene change while running, immediate when stopped.
- Step click: ON/OFF · right-click: normal / accent.
- Track name click: mute · mixer sliders: per-track level.
- Below each mixer track: SMP / SYN — switch between the bundled open-source
  samples and the built-in synth voice.
- Swing / Drive / Master / Low cut, XY filter/delay pad, HOLD FILL.
- INIT: restore all patterns, mixer and FX to the factory presets.
- EXPORT WAV: render the current scene (8 bars, 48 kHz / 16-bit stereo).

Preset scenes (1–8): MAIN FLOOR, DUBSTEP, UK GARAGE, DRUM & BASS, TRAP,
ELECTRO, BIG ROOM, BREAKS. All presets are original patterns.

## iPad / second-device touch control (Cable Sync)

Start the built-in LAN share, read the QR code with the second device and open
the URL in Safari. The page mirrors the machine state: steps, accents, mutes,
scenes, BPM, mixers, XY/FILL, transport and per-track sample/synth selection.
Audio always plays from the PC.

```sh
./build/floor01 --lan
./build/floor01 --lan --lan-address 192.168.1.10 --lan-port 18080
```

The share binds only to a private / link-local IPv4 (or IPv6) address on the
machine, never to all interfaces. Test with:

```sh
python tests/lan_test.py
node tests/browser_test.mjs   # uses installed Firefox, no npm deps
```

## Sound sources

Percussion one-shots (kick, snare, hats, perc, 808 bass, cymbal) are bundled in
`samples/` and licensed **CC0** — see `samples/CREDITS.txt` for the origin kit
and its TR-808 derivations. The synth voices (kick fallback, chord stabs, etc.)
are generated in real time in the audio callback. Sample playback supports
per-scene pitch mapping for the 808 bass; closed hats choke open hats, and the
kick side-chains the bass for a pumping feel.

## License

Code is released under the MIT License (see `LICENSE`).
Bundled samples are CC0 1.0 (see `samples/CREDITS.txt`).
