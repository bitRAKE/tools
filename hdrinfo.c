/*
 * hdrinfo.c — consolidated HDR display capability report
 *
 * build: cl /O2 /W4 hdrinfo.c /link dxgi.lib user32.lib
 *
 * Combines two data sources:
 *   1. DXGI (IDXGIOutput6)    — luminance range, color primaries, bit depth
 *   2. DisplayConfig (CCD)    — HDR supported/enabled, SDR white level,
 *                                color encoding, friendly monitor name
 */

#define WINVER        0x0A00
#define _WIN32_WINNT  0x0A00
#define COBJMACROS
#include <windows.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "user32.lib")

/* ---- helpers --------------------------------------------------------- */

static void print_wide(const WCHAR *ws)
{
    /* Convert to UTF-8 for narrow printf — avoids mixing printf/wprintf. */
    int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return;
    char *buf = (char *)_alloca(n);
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, buf, n, NULL, NULL);
    fputs(buf, stdout);
}

static const char *color_space_name(DXGI_COLOR_SPACE_TYPE cs)
{
    switch (cs) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:   return "sRGB (SDR)";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "HDR10 (PQ / BT.2020)";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:   return "scRGB (linear sRGB)";
    default: {
        static char tmp[32];
        snprintf(tmp, sizeof(tmp), "DXGI_COLOR_SPACE %d", (int)cs);
        return tmp;
    }
    }
}

static const char *color_encoding_name(DISPLAYCONFIG_COLOR_ENCODING enc)
{
    switch (enc) {
    case DISPLAYCONFIG_COLOR_ENCODING_RGB:       return "RGB";
    case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444:  return "YCbCr 4:4:4";
    case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422:  return "YCbCr 4:2:2";
    case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420:  return "YCbCr 4:2:0";
    case DISPLAYCONFIG_COLOR_ENCODING_INTENSITY:  return "Intensity";
    default: return "Unknown";
    }
}

/* ---- DXGI: luminance, primaries, bit depth per output ---------------- */

typedef struct {
    WCHAR   device_name[32]; /* e.g. \\.\DISPLAY1 — key for matching */
    UINT    bits_per_color;
    float   min_lum, max_lum, max_full_frame_lum;
    float   red[2], green[2], blue[2], white[2];
    DXGI_COLOR_SPACE_TYPE color_space;
    BOOL    valid;
} DxgiOutputInfo;

static DxgiOutputInfo *g_dxgi;
static int             g_dxgi_count;

static void gather_dxgi_info(void)
{
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory)))
        return;

    /* count outputs first */
    int cap = 0;
    IDXGIAdapter1 *adapter;
    for (UINT a = 0; IDXGIFactory1_EnumAdapters1(factory, a, &adapter) !=
         DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput *output;
        for (UINT o = 0; IDXGIAdapter1_EnumOutputs(adapter, o, &output) !=
             DXGI_ERROR_NOT_FOUND; o++) {
            cap++;
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }

    g_dxgi = (DxgiOutputInfo *)calloc(cap, sizeof(DxgiOutputInfo));

    int idx = 0;
    for (UINT a = 0; IDXGIFactory1_EnumAdapters1(factory, a, &adapter) !=
         DXGI_ERROR_NOT_FOUND; a++) {
        IDXGIOutput *output;
        for (UINT o = 0; IDXGIAdapter1_EnumOutputs(adapter, o, &output) !=
             DXGI_ERROR_NOT_FOUND; o++) {
            IDXGIOutput6 *out6 = NULL;
            if (SUCCEEDED(IDXGIOutput_QueryInterface(output,
                              &IID_IDXGIOutput6, (void**)&out6))) {
                DXGI_OUTPUT_DESC1 d;
                if (SUCCEEDED(IDXGIOutput6_GetDesc1(out6, &d))) {
                    DxgiOutputInfo *info = &g_dxgi[idx];
                    memcpy(info->device_name, d.DeviceName,
                           sizeof(info->device_name));
                    info->bits_per_color      = d.BitsPerColor;
                    info->min_lum             = d.MinLuminance;
                    info->max_lum             = d.MaxLuminance;
                    info->max_full_frame_lum  = d.MaxFullFrameLuminance;
                    info->red[0]   = d.RedPrimary[0];
                    info->red[1]   = d.RedPrimary[1];
                    info->green[0] = d.GreenPrimary[0];
                    info->green[1] = d.GreenPrimary[1];
                    info->blue[0]  = d.BluePrimary[0];
                    info->blue[1]  = d.BluePrimary[1];
                    info->white[0] = d.WhitePoint[0];
                    info->white[1] = d.WhitePoint[1];
                    info->color_space = d.ColorSpace;
                    info->valid = TRUE;
                    idx++;
                }
                IDXGIOutput6_Release(out6);
            }
            IDXGIOutput_Release(output);
        }
        IDXGIAdapter1_Release(adapter);
    }
    g_dxgi_count = idx;
    IDXGIFactory1_Release(factory);
}

/* Find DXGI info by GDI device name (e.g. \\.\DISPLAY1) */
static const DxgiOutputInfo *find_dxgi(const WCHAR *gdi_name)
{
    for (int i = 0; i < g_dxgi_count; i++)
        if (g_dxgi[i].valid && wcscmp(g_dxgi[i].device_name, gdi_name) == 0)
            return &g_dxgi[i];
    return NULL;
}

/* ---- DisplayConfig: enumerate active monitors + HDR state ------------ */

static void report_displayconfig(void)
{
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,
            &path_count, &mode_count) != ERROR_SUCCESS)
        return;

    DISPLAYCONFIG_PATH_INFO *paths =
        malloc(path_count * sizeof(DISPLAYCONFIG_PATH_INFO));
    DISPLAYCONFIG_MODE_INFO *modes =
        malloc(mode_count * sizeof(DISPLAYCONFIG_MODE_INFO));

    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
            &path_count, paths, &mode_count, modes, NULL) != ERROR_SUCCESS) {
        free(paths); free(modes);
        return;
    }

    int display_num = 0;

    for (UINT32 i = 0; i < path_count; i++) {
        /* skip duplicate targets */
        BOOL seen = FALSE;
        for (UINT32 j = 0; j < i; j++) {
            if (paths[i].targetInfo.adapterId.LowPart  == paths[j].targetInfo.adapterId.LowPart &&
                paths[i].targetInfo.adapterId.HighPart == paths[j].targetInfo.adapterId.HighPart &&
                paths[i].targetInfo.id == paths[j].targetInfo.id) {
                seen = TRUE; break;
            }
        }
        if (seen) continue;

        display_num++;

        /* --- friendly name --- */
        DISPLAYCONFIG_TARGET_DEVICE_NAME tdn = {0};
        tdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tdn.header.size      = sizeof(tdn);
        tdn.header.adapterId = paths[i].targetInfo.adapterId;
        tdn.header.id        = paths[i].targetInfo.id;

        char friendly[128] = "(unknown)";
        if (DisplayConfigGetDeviceInfo(&tdn.header) == ERROR_SUCCESS &&
            tdn.flags.friendlyNameFromEdid) {
            WideCharToMultiByte(CP_UTF8, 0, tdn.monitorFriendlyDeviceName,
                                -1, friendly, sizeof(friendly), NULL, NULL);
        }

        /* --- GDI device name (e.g. \\.\DISPLAY1) for DXGI matching --- */
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn = {0};
        sdn.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sdn.header.size      = sizeof(sdn);
        sdn.header.adapterId = paths[i].sourceInfo.adapterId;
        sdn.header.id        = paths[i].sourceInfo.id;

        WCHAR gdi_name[32] = {0};
        if (DisplayConfigGetDeviceInfo(&sdn.header) == ERROR_SUCCESS)
            memcpy(gdi_name, sdn.viewGdiDeviceName, sizeof(gdi_name));

        /* --- header --- */
        printf("Display %d: %s", display_num, friendly);
        if (gdi_name[0]) { printf(" ("); print_wide(gdi_name); printf(")"); }
        printf("\n");

        /* --- advanced color (HDR) --- */
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO aci = {0};
        aci.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        aci.header.size      = sizeof(aci);
        aci.header.adapterId = paths[i].targetInfo.adapterId;
        aci.header.id        = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&aci.header) == ERROR_SUCCESS) {
            printf("  HDR supported     : %s\n",
                   aci.advancedColorSupported ? "YES" : "no");
            printf("  HDR enabled       : %s\n",
                   aci.advancedColorEnabled  ? "YES" : "no");
            printf("  Wide color gamut  : %s\n",
                   aci.wideColorEnforced     ? "YES" : "no");
            printf("  Bits per channel  : %u bpc\n",
                   aci.bitsPerColorChannel);
            printf("  Color encoding    : %s\n",
                   color_encoding_name(aci.colorEncoding));
        }

        /* --- SDR white level --- */
        DISPLAYCONFIG_SDR_WHITE_LEVEL swl = {0};
        swl.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        swl.header.size      = sizeof(swl);
        swl.header.adapterId = paths[i].targetInfo.adapterId;
        swl.header.id        = paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&swl.header) == ERROR_SUCCESS) {
            /* SDRWhiteLevel is in units of 1/1000 relative to 80 nits */
            double sdr_nits = (swl.SDRWhiteLevel / 1000.0) * 80.0;
            printf("  SDR white level   : %.0f nits (slider %.1f%%)\n",
                   sdr_nits, swl.SDRWhiteLevel / 10.0);
        }

        /* --- DXGI: luminance + color primaries --- */
        const DxgiOutputInfo *dx = find_dxgi(gdi_name);
        if (dx) {
            printf("  DXGI color space  : %s\n",
                   color_space_name(dx->color_space));
            printf("  DXGI bits/color   : %u\n", dx->bits_per_color);
            printf("  Luminance (nits)  : %.4f min, %.1f max, %.1f full-frame\n",
                   dx->min_lum, dx->max_lum, dx->max_full_frame_lum);
            printf("  Primaries  R      : (%.4f, %.4f)\n",
                   dx->red[0], dx->red[1]);
            printf("             G      : (%.4f, %.4f)\n",
                   dx->green[0], dx->green[1]);
            printf("             B      : (%.4f, %.4f)\n",
                   dx->blue[0], dx->blue[1]);
            printf("  White point       : (%.4f, %.4f)\n",
                   dx->white[0], dx->white[1]);
        }

        printf("\n");
    }

    if (display_num == 0)
        printf("  (no active displays found)\n\n");

    free(paths);
    free(modes);
}

/* ---- GPU adapter summary --------------------------------------------- */

static void report_adapters(void)
{
    IDXGIFactory1 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void**)&factory)))
        return;

    IDXGIAdapter1 *adapter;
    for (UINT a = 0; IDXGIFactory1_EnumAdapters1(factory, a, &adapter) !=
         DXGI_ERROR_NOT_FOUND; a++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(adapter, &desc);
        printf("GPU %u: ", a);
        print_wide(desc.Description);
        printf(" (VRAM: %llu MB)\n",
               (unsigned long long)(desc.DedicatedVideoMemory >> 20));
        IDXGIAdapter1_Release(adapter);
    }
    printf("\n");
    IDXGIFactory1_Release(factory);
}

/* ---- main ------------------------------------------------------------ */

int main(void)
{
    /* enable UTF-8 console output */
    SetConsoleOutputCP(CP_UTF8);

    printf("=== Monitor HDR Capability Report ===\n\n");

    report_adapters();
    gather_dxgi_info();
    report_displayconfig();

    free(g_dxgi);
    return 0;
}
