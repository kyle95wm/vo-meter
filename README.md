# VOMeter

A loudness finisher for the end of a voice-over track. It measures the **dialogue-gated
integrated loudness** of your narration and, through a side chain, of the programme (OV)
it sits in. **Match** lands the narration on the target, and an automatable **Trim** lets you
ride it from there. A true-peak limiter holds the output under **-2.5 dBTP**. It is available
as a VST3 on all platforms, plus an AU on macOS.

![VOMeter plugin window](docs/screenshot.png)

## Workflow

1. Put VOMeter last on the narration track. In its **Side Chain** menu (top of the plug-in
   window in Logic), choose the programme / OV track.
2. **Set the target from the programme.** Press Reset Meter. Bypass anything on the OV that
   reacts to the narration (VODucker), so the programme is measured un-ducked. Then do an
   offline bounce of the project or section: File → Bounce → Project or Section, Mode:
   Offline. You can throw the bounced file away. Press **Use** to take the programme's
   dialogue loudness as the target (or type a target, or pick a preset).
3. Press **Match**. The Match gain now lands the narration on the target with the trim at
   0 dB, and the meter resets for the next pass. Switch VODucker back on.
4. **Ride the trim** (automate it) through the mix: up to sit the narration over busy
   scenes instead of ducking the programme hard, down where it's too forward. Match never
   touches the trim or its automation.
5. Bounce again to check (reset first if you've played since Match): the target turns green when the narration's dialogue
   loudness is within ±2 LU of it. With rides, that's the level you deliver, rides included.

**Mono narration in a stereo mix.** A mono track panned centre plays in both channels of a
stereo mix, and BS.1770 then measures it 3 dB louder than the mono track on its own: a
narration track reading -30 sits at -27 in the mix. In a 5.1 mix it goes to the centre
channel alone, with no offset. So on a mono track, VOMeter asks once, when you first open it:
**Is this mix stereo or 5.1?** The answer is saved with the session. From then on every
narration reading is given as heard in that mix, so you always use the programme's own target
(-27 for a -27 OV). To change the answer, use the **Mix** section at the bottom of the
Presets menu. Stereo tracks are never asked, because the answer makes no difference to them.
The stereo offset assumes Logic's default pan law, where a centre-panned mono track stays at
unity in each channel. With the plain "-3 dB" pan law there's no offset, so answer 5.1.

**How Logic's side chain works:** a side chain taken from a track is post-insert and
pre-fader. VOMeter hears the OV after its plug-ins (including VODucker's ducking, hence the
bypass in step 2), but not its fader. Likewise, VODucker keyed from the narration hears
VOMeter's trim but not the narration fader. Riding VOMeter's trim up also makes VODucker
duck harder; riding the narration fader doesn't.

## What it measures

- **Dialogue loudness** follows ITU-R BS.1770 (K-weighting, 400 ms blocks every 100 ms,
  -70 LUFS absolute gate), but it uses speech gating instead of the -10 LU relative level
  gate: only blocks that a speech detector marks as dialogue count. On a real VO take this
  matches Youlean's dialogue-gated reading to within 0.1 LU. The standard integrated reading
  and LRA keep the usual gates. Pauses, breaths, room tone and steady sounds such as tones or noise beds
  are left out. The detector looks at three things: level above the noise floor, the share of
  energy in the voice band (250 Hz to 3.5 kHz), and the syllable-by-syllable movement of the
  level. It is *not* Dolby Dialogue Intelligence (which is proprietary), so expect small
  differences from Dolby Media Meter or Youlean's dialogue gate on mixed programme. On clean VO
  the methods agree closely.
- Three measurements run at once:
  - the narration's **output**, after gain and limiter (the big readout)
  - the narration's **input**, before any gain (gives the Match gain)
  - the **programme** on the side chain (gives the target)
- It measures **only while the transport plays**, or while the host renders offline (Logic's
  offline bounce). It runs over the whole pass (it is a true integrated reading with no time
  window) until you press Reset Meter.
- On plain BS.1770 the meter agrees with the EBU Tech 3341 test signals and with
  `ffmpeg -af ebur128` to within 0.1 LU.

## Controls

| Control | What it does |
|---------|--------------|
| Target | Dialogue loudness to aim for, -36 to -10 LUFS (default -24). Click the number to type a value, or pick a preset. The menu is grouped into Broadcast: -23 (EBU R128) and -24 (ATSC A/85); Streaming: -27 (Netflix dialogue); and Online: -16 (Apple Podcasts) and -14 (Spotify / YouTube). On mono tracks it also has a Mix section (Stereo / 5.1). Shown in green when the output is within ±2 LU of it. |
| Use | Sets the target to the programme's dialogue loudness from the side chain |
| Match | Sets the **Match gain** (up to ±24 dB) so the narration lands on the target with the trim at 0 dB, then resets the meter. It never touches the trim. |
| Clear | Sets the Match gain back to 0 dB and resets the meter |
| Trim | Your ride on top of the Match gain, -18 to +18 dB (default 0). Automatable. **Alt-click** (or double-click) resets it to 0 dB. |
| Reset Meter | Clears every reading, including the programme's. It doesn't touch the trim, Match gain or target. |
| Bypass | Click-free bypass, also linked to your DAW's bypass. It is latency-compensated, so the output is bit-for-bit the input. |

| Readout | Meaning |
|---------|---------|
| Output dialogue | Dialogue-gated integrated loudness of the narration output since the last reset |
| Programme | Dialogue-gated integrated loudness of the side-chain programme. Hidden until a side chain brings programme audio (Logic's "None" sends the track's own audio, which VOMeter ignores) |
| Max true peak | Highest output true peak since the reset (8× oversampled) |
| Gain reduction | How many dB the -2.5 dBTP limiter is turning the audio down right now (held briefly, then falls back). Red while it is working; "None" when it isn't |
| LRA | Loudness range of the output (EBU Tech 3342) |
| Speech | Share of the measured time that counted as dialogue |
| Input dialogue / Match gain | Dialogue loudness before any gain, the current Match gain, and what Match would set it to |

**Limiter:** a stereo-linked lookahead brickwall limiter. It detects inter-sample peaks with
8× oversampling and holds the output at -2.55 dBTP, a 0.05 dB margin so other true-peak meters
agree. It adds about 1.8 ms of latency, which the plugin reports to the host. On normal VO
levels it does nothing until a peak would cross the ceiling.

**Tips**

- If the Match gain changes during a pass, the window asks you to reset and replay. Trim rides
  are expected; the window just notes they're included.
- If Match shows "(limit)", the take needs more than ±24 dB. Fix the gain earlier in the chain.
- The test tool can measure a real file and render it at a target:
  `VOMeterTest --render take.wav -24 -27` writes `take_VOMeter-24.wav` etc. and prints the
  readings. Mono files are read as heard in a stereo mix.

## Install

Download the zip for your system from the
[latest release](https://github.com/kyle95wm/vo-meter/releases/latest), extract it, and run
the included installer, or copy the plugin yourself:

| OS      | Installer             | Manual location |
|---------|-----------------------|-----------------|
| macOS   | `./install.sh`        | `VOMeter.vst3` → `~/Library/Audio/Plug-Ins/VST3`, `VOMeter.component` → `~/Library/Audio/Plug-Ins/Components` |
| Windows | `install.ps1` (right-click → Run with PowerShell) | `VOMeter.vst3` → `C:\Program Files\Common Files\VST3` |
| Linux   | `./install.sh`        | `VOMeter.vst3` → `~/.vst3` |

One-line install of the latest release:

```bash
curl -fsSL https://raw.githubusercontent.com/kyle95wm/vo-meter/main/scripts/install.sh | bash
```

```powershell
# Windows, from an Administrator PowerShell
irm https://raw.githubusercontent.com/kyle95wm/vo-meter/main/scripts/install.ps1 | iex
```

**Logic Pro** only loads Audio Units, so use the AU (`install.sh` installs both). VOMeter
appears under **Audio Units → kyle95wm**, next to the other kyle95wm plugins.

**macOS note:** the builds are ad-hoc signed but not notarised. `install.sh` removes the
quarantine flag for you. If you copy the plugin by hand and macOS blocks it, run
`xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/VOMeter.vst3` (and the same for the `.component`).

## Updates

The plugin window checks this repo's GitHub Releases (at most once a day) and shows
**Update available** with a Download button when there's a newer version. You can turn off
automatic checks with the *Check automatically* box, or check by hand with *Check for updates*.
The check is a single anonymous request to `api.github.com`. Nothing else is sent.

## Building from source

You need CMake 3.22+, a C++17 compiler, and Git. JUCE is downloaded automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The plugins end up in `build/VOMeter_artefacts/Release/`. Add `-DVO_METER_COPY_AFTER_BUILD=ON`
to install them into your user plug-in folders after each build.

- **macOS:** Xcode or the Command Line Tools. Add `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for a universal build.
- **Windows:** Visual Studio 2022 or newer with the C++ workload.
- **Linux (Debian/Ubuntu):**
  ```bash
  sudo apt install build-essential cmake git libasound2-dev libjack-jackd2-dev libcurl4-openssl-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev
  ```

Options: `-DVO_METER_BUILD_AU=OFF`, `-DVO_METER_BUILD_STANDALONE=ON`, `-DVO_METER_JUCE_PATH=/path/to/JUCE`.

To refresh the README screenshot after changing the UI, build with
`-DVO_METER_BUILD_SCREENSHOT_TOOL=ON` and run
`./build/VOMeterScreenshot_artefacts/Release/VOMeterScreenshot docs/screenshot.png`.

## Releasing

1. Push a tag: `git tag v1.0.0 && git push origin v1.0.0`
2. GitHub Actions builds Windows, macOS (universal) and Linux, runs the tests, and publishes a
   Release with the zips and install scripts. The version inside the plugin comes from the
   tag, so the update checker always compares against the right number.

## License

AGPL-3.0, as required by JUCE's open-source license. VST is a trademark of Steinberg Media Technologies GmbH.
