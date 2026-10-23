# EquiHDRPlugin

Experimental Equicord user plugin for fixing or diagnosing Discord Go Live streams captured from an HDR desktop/game when viewers receive a badly overexposed SDR image.

## Install through Equicord UserPluginInstaller

Paste this clone URL into **Install Plugin**:

```text
https://github.com/PunishedSnake/EquiHDRPlugin.git
```

The repository deliberately keeps `index.ts` in the repository root because Equicord's UserPluginInstaller clones the repository directly into its userplugins directory and only loads root-level plugin entry files.

## Recommended settings

Start with:

- **Force SDR:** On
- **Override HDR Experiment:** On
- **Capture Backend:** Windows Graphics Capture
- **Debug Logging:** On

Fully stop the current Go Live session and start a new one after changing capture settings.

## What it currently does

The plugin intercepts Discord's Go Live desktop capture configuration and can override:

- `hdrCaptureMode`
- `useGraphicsCapture`
- `useVideoHook`
- Discord's `2026-02-go-live-hdr` experiment result

The default test configuration resolves to roughly:

```text
hdrCaptureMode      = "never"
useGraphicsCapture  = true
useVideoHook        = false
```

## Diagnostic matrix

### A. Windows Graphics Capture + forced SDR

```text
Force SDR: ON
Override HDR Experiment: ON
Capture Backend: Windows Graphics Capture
```

### B. Discord Video Hook + forced SDR

```text
Force SDR: ON
Override HDR Experiment: ON
Capture Backend: Discord Video Hook
```

### C. Discord default backend + forced SDR

```text
Force SDR: ON
Override HDR Experiment: ON
Capture Backend: Discord default
```

Always stop and restart the Go Live stream between tests.

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

If those messages do not appear, Discord has probably changed the relevant minified module and the patch matcher needs updating.

## Current scope

This is still an experimental capture-path fix. If both Windows Graphics Capture and Discord Video Hook remain overexposed while the debug logs prove the overrides are applied, the likely next step is an actual GPU HDR/scRGB -> SDR tone-mapping stage rather than brightness/gamma compensation.
