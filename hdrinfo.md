# hdrinfo — Monitor HDR Capability Report

A single-file C tool that queries every active display and prints a combined
HDR capability report from two independent Windows APIs.

## Build

```
cl /O2 /W4 hdrinfo.c /link dxgi.lib dxguid.lib user32.lib
```

Requires the Windows 10 SDK (ships with Visual Studio).  No third-party
dependencies.

## Usage

```
hdrinfo.exe
```

No arguments.  Prints to stdout and exits.  Pipe to a file or `clip` if you
need to share the output.

## Data Sources

| API | What it provides |
|-----|-----------------|
| **DisplayConfig** (CCD) | Friendly EDID name, HDR supported/enabled, SDR white level, bits per channel, color encoding |
| **DXGI** (IDXGIOutput6) | DXGI color space, luminance range (min / max / full-frame), color primaries (R/G/B CIE xy), white point |

Only `QDC_ONLY_ACTIVE_PATHS` displays are shown — disconnected or phantom
outputs are excluded.

## Example Output

```
=== Monitor HDR Capability Report ===

GPU 0: NVIDIA GeForce GTX 1080 Ti (VRAM: 11107 MB)

Display 1: LG ULTRAGEAR (\\.\DISPLAY3)
  HDR supported     : YES
  HDR enabled       : YES
  Wide color gamut  : no
  Bits per channel  : 10 bpc
  Color encoding    : RGB
  SDR white level   : 240 nits (slider 300.0%)
  DXGI color space  : HDR10 (PQ / BT.2020)
  DXGI bits/color   : 10
  Luminance (nits)  : 0.3454 min, 351.3 max, 351.3 full-frame
  Primaries  R      : (0.6475, 0.3359)
             G      : (0.3154, 0.6299)
             B      : (0.1543, 0.0771)
  White point       : (0.3135, 0.3291)
```

## Field Reference

### DisplayConfig fields

| Field | Meaning |
|-------|---------|
| **HDR supported** | The panel hardware advertises HDR capability in its EDID. |
| **HDR enabled** | The user has toggled "Use HDR" on in Windows Settings > Display. |
| **Wide color gamut** | Windows is enforcing wide-gamut compositing for this output. |
| **Bits per channel** | Wire bit depth the OS is driving (8, 10, or 12 bpc). |
| **Color encoding** | Signal format on the link: RGB, YCbCr 4:4:4, 4:2:2, or 4:2:0. |
| **SDR white level** | Brightness that SDR-white content is mapped to when HDR is active.  The `slider` percentage corresponds directly to the Windows HDR brightness slider (100% = 80 nits, the sRGB reference). |

### DXGI fields

| Field | Meaning |
|-------|---------|
| **DXGI color space** | The color space the desktop compositor is using for this output. `HDR10 (PQ / BT.2020)` when HDR is active, `sRGB (SDR)` otherwise. |
| **DXGI bits/color** | Panel-reported bit depth from EDID (may differ from the wire depth above if dithering is in play). |
| **Luminance min** | Black-level luminance in nits.  Lower is better for contrast ratio. |
| **Luminance max** | Peak luminance for small highlights in nits (specular detail budget). |
| **Luminance full-frame** | Sustained luminance when the entire screen is lit (APL limit). |
| **Primaries R/G/B** | CIE 1931 xy chromaticity coordinates of the panel's red, green, and blue sub-pixels.  Defines the physical color gamut. |
| **White point** | CIE 1931 xy of the panel's white point.  D65 is (0.3127, 0.3290). |

## How to Use the Information

### Choosing a swap chain format

| Scenario | Recommended format |
|----------|-------------------|
| HDR supported + enabled, bits/channel >= 10 | `DXGI_FORMAT_R16G16B16A16_FLOAT` (FP16 scRGB) for full dynamic range, or `DXGI_FORMAT_R10G10B10A2_UNORM` for HDR10 PQ output. |
| HDR supported but disabled | `DXGI_FORMAT_R10G10B10A2_UNORM` gives better gradient precision than 8-bit even in SDR mode. |
| 8 bpc / no HDR support | `DXGI_FORMAT_R8G8B8A8_UNORM` — anything higher will be dithered down by the driver. |

### Interpreting luminance for tone mapping

- **max luminance** is the ceiling for specular highlights.  Content brighter
  than this clips to white on the panel.  Use it as the upper bound for PQ or
  scRGB tone mapping.
- **max full-frame luminance** is usually lower and represents the sustained
  brightness the backlight can maintain.  Large bright areas (sky, UI chrome)
  should be mapped to this value, not the peak.
- **min luminance** determines visible shadow detail.  Content darker than this
  disappears into the panel's black level.

Example tone-mapping anchor points for a panel reporting
`min=0.01, max=1499, full-frame=799`:

```
Scene-referred   ->   Display-referred
0.0 nits              0.01 nits   (black crush floor)
80 nits (SDR white)   SDR white level from report (e.g. 144 nits)
800+ nits             799 nits    (full-frame clamp)
1500+ nits            1499 nits   (specular peak clamp)
```

### SDR white level and scRGB

When compositing in scRGB (`DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709`), the
value `1.0` maps to 80 nits (the sRGB reference white).  The SDR white level
tells you where Windows is placing legacy SDR content on the HDR brightness
scale.  At 240 nits / slider 300%, SDR white = `240 / 80 = 3.0` in scRGB
linear units.  UI and text rendered at `(1,1,1)` will appear at 80 nits
unless you scale by this factor.

To match the OS SDR level, query `DISPLAYCONFIG_SDR_WHITE_LEVEL` at runtime
and multiply your SDR output by `(SDRWhiteLevel / 1000.0)`.

### Understanding color primaries

The R/G/B chromaticity coordinates define the triangle of reproducible colors
on the CIE diagram.  Compare them against standard gamuts:

| Gamut | R | G | B |
|-------|---|---|---|
| sRGB / BT.709 | (0.64, 0.33) | (0.30, 0.60) | (0.15, 0.06) |
| DCI-P3 | (0.68, 0.32) | (0.27, 0.69) | (0.15, 0.06) |
| BT.2020 | (0.71, 0.29) | (0.17, 0.80) | (0.13, 0.05) |

If the panel's primaries are close to BT.709, sending wide-gamut BT.2020
content won't produce visible benefit — the driver will clip to the panel
gamut.  If the primaries exceed sRGB (common on "wide gamut" IPS panels),
DCI-P3 or BT.2020 workflows will show a visible difference.

### Diagnosing common issues

| Symptom | What to check |
|---------|---------------|
| Washed-out colors in HDR | SDR white level is too high relative to panel max luminance.  Lower the HDR brightness slider. |
| Banding in dark gradients | Bits per channel is 8 — check display settings for 10-bit output, or verify cable bandwidth (HDMI 2.0 may force 8-bit at 4K 60 Hz). |
| Color encoding shows YCbCr | The link fell back from RGB due to bandwidth limits.  Try a higher-spec cable, lower resolution, or lower refresh rate. |
| HDR supported but not enabled | The user (or group policy) has not toggled HDR on in Windows Settings > System > Display. |
| max luminance = full-frame luminance | Panel has no local dimming — the peak brightness is the same regardless of content APL.  Tone mapping can be simpler. |
| Primaries close to sRGB | Wide-gamut content will be gamut-mapped to sRGB by the panel.  No benefit from DCI-P3 or BT.2020 workflows. |
