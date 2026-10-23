# EquiHDRPlugin

Experimental Equicord user plugin for correcting Discord Go Live streams captured from an HDR desktop/game when SDR viewers receive a badly overexposed image.

## Install through Equicord UserPluginInstaller

Paste this clone URL into **Install Plugin**:

```text
https://github.com/PunishedSnake/EquiHDRPlugin.git
```

The repository deliberately keeps `index.ts` and `native.ts` in the repository root because Equicord's UserPluginInstaller clones the repository directly into its userplugins directory and only loads root-level plugin entry/native files.

## v0.3: real HDR -> SDR tone mapping

Forcing Discord's own `hdrCaptureMode = "never"`, Windows Graphics Capture, and disabling the Video Hook was not enough on the test system: viewers still received an overexposed stream.

EquiHDRPlugin therefore now uses a native Windows helper for the actual correction stage:

```text
HDR desktop/game
    -> Windows Graphics Capture
    -> R16G16B16A16_FLOAT scRGB
    -> D3D11 GPU SDR-white normalization / tone mapping
    -> sRGB SDR mirror window
    -> Discord captures that mirror as ordinary SDR
```

The helper is built by this repository's GitHub Actions from a pinned revision of the MIT-licensed [HDR Corrector](https://github.com/Oppiie/HDR-Corrector), with a minimal patch that:

- gives it a separate EquiHDR mutex/name,
- exposes an `--mirror` startup mode,
- renames the stream target to **EquiHDR SDR Mirror**.

The plugin's `native.ts` downloads the helper from this repository's `helper-latest` Release, verifies its SHA-256, stores the upstream MIT license alongside it, and launches the mirror automatically by default.

## First v0.3 test

1. Update/reinstall the plugin from the repository.
2. Keep Windows HDR enabled.
3. Leave these plugin settings enabled:
   - **Auto Launch Mirror:** On
   - **Force SDR:** On
   - **Override HDR Experiment:** On
   - **Capture Backend:** Windows Graphics Capture
4. Restart Equicord/Discord if the plugin was already loaded before updating.
5. An **EquiHDR SDR Mirror** window should appear.
6. In Discord Go Live, share **EquiHDR SDR Mirror**, not the original HDR game/monitor.
7. Compare the viewer-side image with the original direct HDR capture.

The first start downloads the helper. Later starts reuse the verified local copy.

## Discord capture-path patches

The plugin still intercepts Discord's Go Live desktop capture configuration and can override:

- `hdrCaptureMode`
- `useGraphicsCapture`
- `useVideoHook`
- Discord's `2026-02-go-live-hdr` experiment result

The recommended final SDR mirror capture resolves to roughly:

```text
hdrCaptureMode      = "never"
useGraphicsCapture  = true
useVideoHook        = false
```

These flags alone did **not** fix direct HDR capture on the test system. They are now used to keep Discord from applying another HDR capture path to the already tone-mapped SDR mirror.

## Debugging

With **Debug Logging** enabled, DevTools should show entries such as:

```text
EquiHDRPlugin hdrCaptureMode
requested: "always"
resolved: "never"
```

and when Windows Graphics Capture is forced:

```text
EquiHDRPlugin useGraphicsCapture
resolved: true

EquiHDRPlugin useVideoHook
resolved: false
```

The plugin also logs whether the native helper was downloaded/launched successfully.

## Helper storage

The helper is stored below Discord/Equicord's Electron `userData` directory in:

```text
EquiHDRPlugin/EquiHDRHelper.exe
```

The SHA-256 record and upstream `HDR-Corrector-LICENSE.txt` are stored beside it.

## Attribution

The native capture/tone-mapping helper is built from the MIT-licensed HDR Corrector project by Oppiie/c-mpayette. EquiHDRPlugin itself remains GPL-3.0-or-later.
