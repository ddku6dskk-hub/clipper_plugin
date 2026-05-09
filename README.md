# K Clipper / K Slammer

Open-source JUCE-based audio plug-ins: a transparent mastering clipper (**K Clipper**)
and a transient-preserving drum/bass clipper (**K Slammer**).

Formats: **VST3 / AU / AAX / Standalone** (macOS, Windows target planned).

---

## Plug-ins

| Plug-in | Intended use | Oversampling | Look-ahead | Characteristic |
|---|---|---|---|---|
| **K Clipper** | Mastering | 16x | 0.2 ms | Transparent loudness maximisation, ISP-aware |
| **K Slammer** | Drums / Bass | 4x | — | Transient-preserving, more aggressive saturation |

Both share a C2-Hermite soft-clip shaper and three Modes:

- **B.Wall** – shaper only (cleanest)
- **Open** – high-shelf (+5.96 dB @ 5979 Hz, linked pre/post) → shaper
- **LF** – dynamic low-shelf @ 2604 Hz → shaper

### UI

Four sliders (Threshold / Knee / Input / Output), a Mode selector,
and a GR meter (0–12 dB, 30 Hz refresh, peak hold 1 s).

The meter shows latency-aligned input/output peaks plus an HA-style **CLIP**
indicator that lights when the output exceeds 0 dBFS. The input readout turns
red when the input reaches 0 dBFS.

---

## Installation (macOS, AAX)

The release zip contains `K Clipper.aaxplugin` and `K Slammer.aaxplugin`.
Copy them into Pro Tools' AAX folder:

```sh
sudo ditto "K Clipper.aaxplugin"  "/Library/Application Support/Avid/Audio/Plug-Ins/K Clipper.aaxplugin"
sudo ditto "K Slammer.aaxplugin"  "/Library/Application Support/Avid/Audio/Plug-Ins/K Slammer.aaxplugin"
```

Because the binaries are signed with a free **Apple Development** certificate
(not notarized — this is a non-commercial GPL build), macOS attaches a
quarantine attribute when the zip is downloaded via a browser. This causes
Pro Tools' AAX Trust to reject the bundle on launch and move it to
`Plug-Ins (Unused)`. Clear quarantine after copying:

```sh
sudo xattr -cr "/Library/Application Support/Avid/Audio/Plug-Ins/K Clipper.aaxplugin"
sudo xattr -cr "/Library/Application Support/Avid/Audio/Plug-Ins/K Slammer.aaxplugin"
```

Restart Pro Tools and accept the AAX Trust dialog if shown.

> **Tip:** always extract the release zip with `ditto -x -k <zip> <dest>`
> rather than the Finder's archive utility — Finder's expander dereferences
> the PACE signature symlinks and silently breaks the resource seal.

---

## Build

Requires:

- CMake 3.22+
- Xcode 15+ (macOS) or MSVC (Windows)
- [JUCE](https://github.com/juce-framework/JUCE) as a sibling directory (`../JUCE`)
- (Optional) AAX SDK 2.9.0+ for AAX target — see `CMakeLists.txt` for auto-detection paths

```sh
cmake -G Xcode -B build
cmake --build build --config Release
```

AAX target is built automatically if `~/SDKs/AAX_SDK_*` or `-DAAX_SDK_PATH=...` is supplied.

---

## License

Licensed under **GNU General Public License v3.0 or later** — see [`LICENSE`](LICENSE).

This project links against the AAX SDK under its Open Source licensing terms and
against JUCE under its GPLv3 option.

---

## Non-Commercial Notice

This project is developed for personal / non-commercial use only.
No binaries are sold, distributed as donationware, or bundled with commercial
services. Contributions are welcome under the same GPLv3 terms.

---

## Author

**Kyohei Hayakawa** — audio engineer / developer.
