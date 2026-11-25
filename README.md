# VOMeter

One or two sentences on what VOMeter does and who it's for. It is available as a VST3 on all platforms, plus an AU on macOS.

![VOMeter plugin window](docs/screenshot.png)

Describe the problem it solves in a voice-over / audio-description session, and its main idea.

## Controls

| Control | What it does |
|---------|--------------|
| Gain    | Replace with the plugin's real controls |
| Bypass  | Click-free bypass, also linked to your DAW's bypass |

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
