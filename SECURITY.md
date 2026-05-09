# Security Policy

## Reporting a vulnerability

If you discover a security issue in K Clipper or K Slammer, please open a
[private security advisory](https://github.com/ddku6dskk-hub/clipper_plugin/security/advisories/new)
on GitHub. Do **not** file a public issue or pull request describing the
problem first.

## Distribution and verification

Releases are distributed as `.dmg` images on the
[GitHub Releases](https://github.com/ddku6dskk-hub/clipper_plugin/releases)
page. Each `.aaxplugin` is signed with:

- **PACE Eden Tools** (Sign-Only wrap)
- **Apple Development** certificate (TeamIdentifier `8C2GG9NU82`)

You can verify a downloaded plug-in locally:

```sh
codesign --verify --deep --strict --verbose=2 "K Clipper.aaxplugin"
wraptool   verify --in                          "K Clipper.aaxplugin"
```

The expected Apple authority chain is
`Apple Development → Apple WWDR → Apple Root CA`,
and the PACE Signer GUID is `5B029032-B705-8064-2AE4-E98BBCD9D88A`
(K Audio publisher).

## Build provenance

- The universal binary (`x86_64` + `arm64`) is built from a tagged
  commit on `main` using CMake / Xcode and the AAX SDK 2.9.0.
- `-ffile-prefix-map` and `-fmacro-prefix-map` are applied so binaries
  contain no absolute `/Users/...` developer paths.
- Releases ship without debug symbols (`.dSYM`), static intermediates
  (`.a`, `.o`), or hidden metadata (`.DS_Store`, `__MACOSX`).
- The distribution `.dmg` is produced via `hdiutil create -format UDZO`
  and validated with `hdiutil verify` before upload.

## Known limitations

- Binaries are **not Apple-notarized.** This is an unpaid GPL project,
  so first-time install requires
  ``sudo xattr -cr "/Library/Application Support/Avid/Audio/Plug-Ins/K Clipper.aaxplugin"``
  to clear the macOS quarantine attribute.
- macOS only — no Windows build is provided.
