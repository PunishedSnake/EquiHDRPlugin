# EquiHDRPlugin

Experimental Equicord user plugin for correcting Discord Go Live streams captured from an HDR desktop/game when SDR viewers receive a badly overexposed image.

## Install through Equicord UserPluginInstaller

Paste this clone URL into **Install Plugin**:

```text
https://github.com/PunishedSnake/EquiHDRPlugin.git
```

`index.ts` and `native.ts` deliberately live in the repository root because Equicord's UserPluginInstaller clones the repository directly into its userplugins directory.

## Current architecture: in-process D3D11 hook

Earlier tests established that all of these were insufficient on the test system:

- forcing Discord `hdrCaptureMode = "never"`,
- forcing Windows Graphics Capture,
- disabling Discord Video Hook,
- feeding Discord a separately tone-mapped SDR mirror window.

EquiHDRPlugin therefore now corrects the frame inside the Discord renderer process, before Discord's normal D3D11 video-processing/encoding path consumes it.

```text
HDR game / desktop
    -> Discord Windows Graphics Capture
    -> FP16 scRGB texture (R16G16B16A16_FLOAT)
    -> EquiHDR ID3D11VideoContext::VideoProcessorBlt hook
    -> configurable GPU tone mapper
    -> same FP16 texture, corrected in place
    -> original Discord VideoProcessorBlt
    -> Discord encoder / Go Live
```

The native addon hooks the stable D3D11 COM method `ID3D11VideoContext::VideoProcessorBlt` using MinHook. It does not depend on a hard-coded RVA inside `discord_voice.node`.

For an FP16/scRGB input candidate, it:

1. copies the original source texture to a shader-readable FP16 texture,
2. applies the selected tone-mapping curve in a D3D11 pixel shader,
3. renders the corrected result to another FP16 texture,
4. copies the result back into the original source texture,
5. lets Discord's original `VideoProcessorBlt` continue normally.

This keeps Discord's resource identity, encoder selection and normal Go Live pipeline intact. There is no mirror window or virtual camera in the current path.

## Tone-map controls

The plugin exposes live controls for:

- **Tone mapper**
  - ACES approximation (default)
  - Reinhard with white point
  - Hable / Uncharted 2
  - hard clip, mainly for diagnostics
- **Exposure EV**
- **Paper white**, in nits
- **Peak luminance**, in nits
- **Output gain**
- **Gamma compensation**
- **Saturation**

Changes to the tone-map controls are sent to the native addon while Discord is running. They do not require rebuilding the plugin.

## Recommended first configuration

```text
Tone Map Enabled:       On
Tone Mapper:            ACES approximation
Exposure EV:            0.0
Paper White:            203 nits
Peak Nits:              1000 nits
Output Gain:            1.00
Gamma Compensation:     1.00
Saturation:             1.00

HDR Capture Mode:       Always HDR for native hook
Override HDR Experiment: On
Capture Backend:        Windows Graphics Capture
Debug Logging:          On
```

`HDR Capture Mode = Always` is intentional. The native hook needs the HDR/scRGB FP16 source before Discord or Windows performs an unwanted SDR conversion.

## Diagnosing whether the hook is actually in the frame path

EquiHDRPlugin adds two toolbox actions:

- **Log EquiHDR hook stats**
- **Reset EquiHDR hook stats**

Start an HDR Go Live stream, let it run for a few seconds, then use **Log EquiHDR hook stats**.

The important counters are:

```text
bltCalls
fp16Candidates
processedFrames
inputFormatName
outputFormatName
width / height
lastError
```

### Expected successful path

```text
bltCalls        > 0
fp16Candidates  > 0
processedFrames > 0
inputFormatName = R16G16B16A16_FLOAT
```

Interpretation:

- `bltCalls == 0`: Discord is bypassing `ID3D11VideoContext::VideoProcessorBlt` on this machine/build. A different native interception point is required.
- `bltCalls > 0`, `fp16Candidates == 0`: the hook is active, but Discord is not feeding FP16/scRGB into this video-processor path.
- `fp16Candidates > 0`, `processedFrames == 0`: the expected source was found but the shader/copy stage failed. Check `lastError`.
- `processedFrames > 0`: the custom tone mapper is definitely modifying frames before Discord's original video-processor call. At that point brightness can be tuned with the live controls rather than guessed through capture flags.

## Native addon delivery

The Windows native addon is built in GitHub Actions and published under the stable `hook-latest` release tag.

The plugin's `native.ts`:

- downloads the addon automatically,
- verifies its SHA-256 before loading,
- stores each build under a hash-versioned filename so Windows file locking does not prevent updates,
- loads it into the Discord renderer's isolated world so the hook runs in the same process as Discord's media pipeline.

## Discord capture-path patches

The plugin still controls Discord's Go Live desktop description so the experiment and capture backend match the native hook:

- `hdrCaptureMode`
- `useGraphicsCapture`
- `useVideoHook`
- Discord's `2026-02-go-live-hdr` experiment result

The recommended native-hook path is approximately:

```text
hdrCaptureMode      = "always"
useGraphicsCapture  = true
useVideoHook        = false
```

## Native dependencies

The hook uses:

- Windows D3D11 / DXGI / D3DCompiler
- MinHook, built from the pinned upstream release by this repository's CI
- Node-API through `node-addon-api`

EquiHDRPlugin itself is GPL-3.0-or-later. Third-party licenses are retained with the corresponding build/release assets.
