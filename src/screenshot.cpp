// screenshot.cpp - back-buffer capture plus a cheap verdict on what the frame is.
//
// The verdict is not decoration: the whole point is to stop spending runs on "the scan found
// nothing" when the honest answer was "the game was showing a menu". Four measurements, all from
// one sparse pass over the frame:
//
//   distinct  colours quantised to 5 bits per channel (32 levels), from a set/bitmap. A menu or a
//             loading screen is a few dozen colours; a rendered scene is thousands.
//   black%    pixels that are exactly (0,0,0). Loading screens and fade-outs are almost entirely
//             this; a lit scene is a few percent.
//   lit%      pixels that are not near-black (luma > 24). The complement of black% with a
//             tolerance, so a dark scene and a blank one are distinguishable.
//   skin%     pixels in a broad skin-tone box - only ever a hint (this game's characters are in
//             shot a lot), which is why it is logged as a number and not used as a verdict on
//             its own.
//
// The sampled pixels are quantised to one per 4x4 block, which keeps this at about a millisecond
// on a 3840x2160 frame and cannot be skewed by a thin overlay (a HUD line covers whole blocks).

#include "screenshot.h"

#include <cstdio>
#include <cstring>

#include "d3d9_min.h"
#include "log.h"

namespace re6vr {
namespace {

volatile LONG g_request = 0;
char g_label[64] = "scan";
int g_written = 0;
int g_max_writes = 8;          // a handful of MB-scale files per session is plenty

struct Stats {
    unsigned distinct;
    unsigned black_pct;
    unsigned lit_pct;
    unsigned skin_pct;
    unsigned mean_luma;
};

// Kept object-free: `__try` cannot coexist with anything needing stack unwinding.
bool sample_frame(const uint8_t *pixels, uint32_t pitch, uint32_t w, uint32_t h, Stats *out) {
    __try {
        static unsigned char seen[32768];
        memset(seen, 0, sizeof(seen));
        unsigned long long n = 0, black = 0, lit = 0, skin = 0, luma_sum = 0;
        const uint32_t step = 4;
        for (uint32_t y = 0; y + step <= h; y += step) {
            const uint32_t *row = (const uint32_t *)(pixels + (size_t)y * pitch);
            for (uint32_t x = 0; x + step <= w; x += step) {
                const uint32_t px = row[x];
                const unsigned b = px & 0xFFu, g = (px >> 8) & 0xFFu, r = (px >> 16) & 0xFFu;
                const unsigned luma = (r * 299u + g * 587u + b * 114u) / 1000u;
                luma_sum += luma;
                ++n;
                if (px == 0) ++black;
                if (luma > 24u) ++lit;
                if (r > 95u && g > 40u && b > 20u && r > g && r > b && (r - g) > 15u &&
                    (r - b) > 15u && luma < 240u) {
                    ++skin;
                }
                seen[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)] = 1;
            }
        }
        unsigned long long d = 0;
        for (size_t i = 0; i < sizeof(seen); ++i) d += seen[i];
        if (!n) return false;
        out->distinct = (unsigned)d;
        out->black_pct = (unsigned)(black * 100ull / n);
        out->lit_pct = (unsigned)(lit * 100ull / n);
        out->skin_pct = (unsigned)(skin * 100ull / n);
        out->mean_luma = (unsigned)(luma_sum / n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A BMP, so the image opens without a conversion step. 24-bit, bottom-up rows.
bool write_bmp(const wchar_t *path, const uint8_t *pixels, uint32_t pitch, uint32_t w,
               uint32_t h) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint32_t row_bytes = w * 3;
    const uint32_t pad = (4 - (row_bytes % 4)) % 4;
    const uint32_t image_bytes = (row_bytes + pad) * h;
    unsigned char header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t file_size = 54 + image_bytes;
    memcpy(header + 2, &file_size, 4);
    const uint32_t offset = 54;
    memcpy(header + 10, &offset, 4);
    const uint32_t dib = 40;
    memcpy(header + 14, &dib, 4);
    memcpy(header + 18, &w, 4);
    memcpy(header + 22, &h, 4);
    const uint16_t planes = 1, bpp = 24;
    memcpy(header + 26, &planes, 2);
    memcpy(header + 28, &bpp, 2);
    memcpy(header + 34, &image_bytes, 4);
    DWORD written = 0;
    WriteFile(f, header, sizeof(header), &written, nullptr);
    static unsigned char rowbuf[16 * 1024];
    for (int y = (int)h - 1; y >= 0; --y) {
        const uint8_t *s = pixels + (size_t)y * pitch;
        size_t o = 0;
        for (uint32_t x = 0; x < w; ++x) {
            if (o + 3 > sizeof(rowbuf)) break;
            rowbuf[o++] = s[x * 4 + 0];      // B
            rowbuf[o++] = s[x * 4 + 1];      // G
            rowbuf[o++] = s[x * 4 + 2];      // R
        }
        for (uint32_t p = 0; p < pad; ++p) rowbuf[o++] = 0;
        WriteFile(f, rowbuf, (DWORD)o, &written, nullptr);
    }
    CloseHandle(f);
    return true;
}

// The sentence that decides whether a scan result means anything.
const char *verdict(const Stats &s) {
    if (s.black_pct > 95) return "almost entirely black - a loading screen or a fade";
    if (s.distinct < 150 && s.lit_pct < 55) return "few colours, mostly dark - a MENU or "
                                                  "loading screen, no playable camera";
    if (s.distinct < 400 && s.lit_pct > 80) return "few colours, mostly bright - a MENU or a "
                                                   "static screen, no playable camera";
    if (s.distinct < 900) return "moderate colour range - a cutscene, a menu, or a very dark "
                                 "scene";
    return "rich colour range - this looks like RENDERED GAMEPLAY, a scan here is meaningful";
}

} // namespace

void screenshot_request(const char *label) {
    if (label && label[0]) {
        strncpy_s(g_label, sizeof(g_label), label, _TRUNCATE);
    }
    InterlockedExchange(&g_request, 1);
}

int screenshot_count() { return g_written; }

void screenshot_service_from_device(IDirect3DDevice9 *device) {
    if (!device || InterlockedCompareExchange(&g_request, 1, 1) == 0) return;   // nothing asked

    IDirect3DSurface9 *back = nullptr;
    if (FAILED(device->lpVtbl->GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
                                             (void **)&back)) || !back) {
        return;
    }
    D3DSURFACE_DESC desc = {};
    if (FAILED(back->lpVtbl->GetDesc(back, &desc))) {
        back->lpVtbl->Release(back);
        return;
    }
    // The game renders to a plain render target here; anything else means this hook is early in
    // start-up and the surface is not the frame yet.
    if (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) {
        back->lpVtbl->Release(back);
        return;
    }
    D3DLOCKED_RECT locked = {};
    if (FAILED(back->lpVtbl->LockRect(back, &locked, nullptr, D3DLOCK_READONLY))) {
        back->lpVtbl->Release(back);
        return;
    }
    screenshot_service((const uint8_t *)locked.pBits, (uint32_t)locked.Pitch, desc.Width,
                       desc.Height);
    back->lpVtbl->UnlockRect(back);
    back->lpVtbl->Release(back);
}

// Synthetic check of the capture path: two frames whose verdict is known by construction are
// pushed through the real sampler, and the verdicts are compared with what they must be. Run
// with RE6VR_SHOT_SELFTEST=1 (the offline harness does this), because a wrong verdict here would
// silently mislabel every scan of a real run - and "the game was in a menu" is exactly the kind
// of conclusion this project has learned to distrust.
bool screenshot_selftest() {
    const uint32_t w = 640, h = 360;
    static uint8_t buf[640 * 360 * 4];
    bool pass = true;

    // 1) a "gameplay" frame: a rich gradient with detail, only a few percent black.
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *p = buf + ((size_t)y * w + x) * 4;
            const unsigned noise = ((x * 7 + y * 13) * 31) & 0x3F;
            p[0] = (uint8_t)((x * 255 / w + noise) & 0xFF);              // B
            p[1] = (uint8_t)((y * 255 / h + noise * 2) & 0xFF);          // G
            p[2] = (uint8_t)(((x + y) * 255 / (w + h) + noise) & 0xFF);  // R
            p[3] = 255;
        }
    }
    Stats game = {0, 0, 0, 0, 0};
    const bool game_ok = sample_frame(buf, w * 4, w, h, &game);
    const char *game_verdict = verdict(game);
    const bool game_pass = game_ok && game.distinct >= 900 && game.black_pct <= 20;
    VRLOG("shot: selftest gameplay frame: distinct=%u black=%u%% -> %s", game.distinct,
          game.black_pct, game_verdict);
    if (!game_pass) pass = false;

    // 2) a "menu" frame: a black field with a small pale panel, few colours.
    memset(buf, 0, sizeof(buf));
    for (uint32_t y = h / 4; y < h / 2; ++y) {
        for (uint32_t x = w / 4; x < w * 3 / 4; ++x) {
            uint8_t *p = buf + ((size_t)y * w + x) * 4;
            p[0] = 200; p[1] = 200; p[2] = 200; p[3] = 255;
        }
    }
    Stats menu = {0, 0, 0, 0, 0};
    const bool menu_ok = sample_frame(buf, w * 4, w, h, &menu);
    const char *menu_verdict = verdict(menu);
    const bool menu_pass = menu_ok && menu.distinct < 150 && menu.black_pct > 50;
    VRLOG("shot: selftest menu frame: distinct=%u black=%u%% -> %s", menu.distinct, menu.black_pct,
          menu_verdict);
    if (!menu_pass) pass = false;

    // 3) the file writer, on the same buffer.
    wchar_t path[MAX_PATH] = L"re6vr_shot_selftest.bmp";
    const wchar_t *log_path = vrlog::path();
    if (log_path && log_path[0]) {
        wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)),
                            L"re6vr_shot_selftest.bmp");
    }
    const bool wrote = write_bmp(path, buf, w * 4, w, h);
    VRLOG("shot: selftest: wrote %ls: %s", path, wrote ? "ok" : "FAILED");
    if (!wrote) pass = false;

    VRLOG("shot: selftest: gameplay verdict %s, menu verdict %s -> %s",
          game_pass ? "correct" : "WRONG", menu_pass ? "correct" : "WRONG",
          pass ? "PASS" : "FAIL");
    return pass;
}

bool screenshot_service(const uint8_t *pixels, uint32_t pitch, uint32_t w, uint32_t h) {
    if (InterlockedExchange(&g_request, 0) == 0) return false;
    if (!pixels || !w || !h) return false;

    // The numbers are always logged, even when no file is written: they are the cheap signal.
    Stats st = {0, 0, 0, 0, 0};
    const bool ok = sample_frame(pixels, pitch, w, h, &st);
    if (!ok) {
        VRLOG("shot: '%s' frame could not be sampled", g_label);
        return false;
    }

    wchar_t path[MAX_PATH] = L"re6vr_shot.bmp";
    if (g_written < g_max_writes) {
        const wchar_t *log_path = vrlog::path();
        if (log_path && log_path[0]) {
            wcsncpy_s(path, MAX_PATH, log_path, _TRUNCATE);
            wchar_t *slash = wcsrchr(path, L'\\');
            if (slash) {
                wchar_t name[64] = L"re6vr_shot.bmp";
                _snwprintf_s(name, 64, _TRUNCATE, L"re6vr_shot_%02d_%hs.bmp",
                             g_written, g_label);
                // Keep the name filesystem-safe: the label may contain spaces or slashes.
                for (wchar_t *p = name; *p; ++p) {
                    if (*p == L' ' || *p == L'/' || *p == L'\\' || *p == L':') *p = L'_';
                }
                wcscpy_s(slash + 1, (size_t)(MAX_PATH - (slash + 1 - path)), name);
            }
        }
        if (write_bmp(path, pixels, pitch, w, h)) ++g_written;
    }

    VRLOG("shot: '%s' %ux%u  distinct=%u  black=%u%%  lit=%u%%  skin=%u%%  mean luma=%u  -> %s",
          g_label, w, h, st.distinct, st.black_pct, st.lit_pct, st.skin_pct, st.mean_luma,
          verdict(st));
    if (g_written <= g_max_writes) {
        VRLOG("shot:   image written to %ls", path);
    }
    return true;
}

} // namespace re6vr
