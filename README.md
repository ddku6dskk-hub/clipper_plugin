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
