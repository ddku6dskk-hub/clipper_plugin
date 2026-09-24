# K Clipper / K Slammer

Open-source JUCE-based audio plug-ins: a transparent mastering clipper (**K Clipper**)
and a transient-preserving drum/bass clipper (**K Slammer**).

Formats: **VST3 / AU / AAX** (macOS only).

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
and a GR meter (0–9 dB, 30 Hz refresh, peak hold 1 s).

Below the meter, a readout shows the input peak (dBFS, after the Input gain)
and the gain-reduction peak (dB). An HA-style **CLIP** indicator lights when
the output (a true-peak estimate, after the Output gain) exceeds 0 dBFS. The
input readout turns red when the input reaches 0 dBFS.

#### Analyser (scrolling visualiser)

The left panel scrolls roughly the last 10 seconds in two independent lanes
(L/R; a single lane on mono tracks), at a 10 ms frame resolution:

- **cyan** – level after gain reduction
- **amber** – how much the limiter/shaper took off, drawn as a cap on top
- **yellow line** – the current Threshold

The analyser is **input-referred**: levels are taken *after* the Input gain and
*before* the Output gain, so the peaks line up with the Threshold line directly.
Moving Output therefore does not move the display — it is a picture of what the
clipper is doing to the signal, not of the plugin's final output level (watch
the host's meter for that; the CLIP indicator lights if it exceeds 0 dBFS).
Per-frame gain reduction is measured inside the oversampled domain from the
gain coefficients actually applied — the same source as the GR meter, just at
a finer time resolution, and it fades out with the Bypass crossfade exactly
like the GR meter does.

Below it, an info line shows the session maxima: `Peak: x dBFS | Max GR: y dB`
(both measured, not predicted). **Clicking the info line resets those two
readouts** without clearing the scrolling history, so you can re-measure a
section while still watching the graph.

---

### Bypass

The host's bypass button drives the plug-in's own **Bypass** parameter — the
AAX, AU and VST3 wrappers all hand it over (in Pro Tools it is the plug-in's
Master Bypass). Engaging and releasing bypass both crossfade over 15 ms between
the processed signal and the dry signal, delayed by the same latency, so
nothing jumps in time. The processing keeps running underneath, so switching
back is seamless; the GR meter and the analyser fade out with it.

Because the processing keeps running, bypass does **not** reduce the CPU load —
a bypassed instance costs about as much as an active one. To free the CPU in
Pro Tools, make the plug-in inactive instead.

A separate hard-bypass path exists only as a fallback for hosts that call the
plug-in's bypassed-processing callback directly; none of the shipped formats
do. It stops the chain and rebuilds it on the way back: the dry signal is held
for the reported latency, then crossfaded into the processed signal over 15 ms.

## Installation (macOS, AAX)

The release `.dmg` contains `K Clipper.aaxplugin` and `K Slammer.aaxplugin`.
Mount it, then copy the plug-ins into Pro Tools' AAX folder:

```sh
sudo ditto "K Clipper.aaxplugin"  "/Library/Application Support/Avid/Audio/Plug-Ins/K Clipper.aaxplugin"
sudo ditto "K Slammer.aaxplugin"  "/Library/Application Support/Avid/Audio/Plug-Ins/K Slammer.aaxplugin"
```

Because the binaries are signed with a free **Apple Development** certificate
(not notarized — this is a non-commercial GPL build), macOS attaches a
quarantine attribute when the `.dmg` is downloaded via a browser. This causes
Pro Tools' AAX Trust to reject the bundle on launch and move it to
`Plug-Ins (Unused)`. Clear quarantine after copying:

```sh
sudo xattr -cr "/Library/Application Support/Avid/Audio/Plug-Ins/K Clipper.aaxplugin"
sudo xattr -cr "/Library/Application Support/Avid/Audio/Plug-Ins/K Slammer.aaxplugin"
```

Restart Pro Tools and accept the AAX Trust dialog if shown.

> **Tip:** copy the plug-ins out of the mounted `.dmg` with `sudo ditto`
> (as shown above) rather than dragging in the Finder — `ditto` preserves the
> PACE signature symlinks, so the resource seal stays intact.

---

## Build

Requires:

- CMake 3.22+
- Xcode 15+ (macOS)
- [JUCE](https://github.com/juce-framework/JUCE) as a sibling directory (`../JUCE`),
  or anywhere else via `-DJUCE_PATH=/path/to/JUCE`
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
