// console.c — text console emulator + RGBA renderer. See console.h.
#include "console.h"
#include "console_font.h"
#include <string.h>
#include <stdlib.h>

// Standard VGA 16-color palette (RGB). Index 0..15.
static const uint8_t vga_pal[16][3] = {
    {  0,  0,  0}, {170,  0,  0}, {  0,170,  0}, {170, 85,  0},
    {  0,  0,170}, {170,  0,170}, {  0,170,170}, {170,170,170},
    { 85, 85, 85}, {255, 85, 85}, { 85,255, 85}, {255,255, 85},
    { 85, 85,255}, {255, 85,255}, { 85,255,255}, {255,255,255},
};

#define ATTR_DEFAULT 0x07   // light gray on black

void console_init(console* c, int cols, int rows) {
    memset(c, 0, sizeof *c);
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > CON_MAX_COLS) cols = CON_MAX_COLS;
    if (rows > CON_MAX_ROWS) rows = CON_MAX_ROWS;
    c->cols = cols; c->rows = rows;
    c->cells = (con_cell*)calloc((size_t)cols * (size_t)rows, sizeof(con_cell));
    c->cursor_visible = 1;
    c->cursor_on = 1;
    console_reset(c);
}

void console_free(console* c) {
    free(c->cells);
    c->cells = NULL;
    c->cols = c->rows = 0;
}

static void console_clear_grid(console* c) {
    con_cell blank;
    blank.ch = ' ';
    blank.attr = c->cur_attr;
    for (int y = 0; y < c->rows; y++)
        for (int x = 0; x < c->cols; x++)
            c->cells[y * c->cols + x] = blank;
}

void console_reset(console* c) {
    c->cur_attr = ATTR_DEFAULT;
    c->cur_x = 0; c->cur_y = 0;
    c->cursor_visible = 1;
    c->cursor_on = 1;
    c->esc_state = 0; c->esc_argc = 0; c->esc_priv = 0;
    c->esc_curarg = 0;
    memset(c->esc_args, 0, sizeof c->esc_args);
    c->utf8_codepoint = 0; c->utf8_remaining = 0;
    console_clear_grid(c);
    c->dirty = 1;
}

void console_resize(console* c, int cols, int rows) {
    if (cols == c->cols && rows == c->rows) return;
    int oldw = c->cols;
    con_cell* nc = (con_cell*)calloc((size_t)cols * (size_t)rows, sizeof(con_cell));
    if (!nc) return;
    con_cell blank; blank.ch = ' '; blank.attr = ATTR_DEFAULT;
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < cols; x++)
            nc[y * cols + x] = (y < c->rows && x < oldw) ? c->cells[y * oldw + x] : blank;
    free(c->cells);
    c->cells = nc;
    c->cols = cols; c->rows = rows;
    if (c->cur_x >= cols) c->cur_x = cols - 1;
    if (c->cur_y >= rows) c->cur_y = rows - 1;
    c->dirty = 1;
}

static void console_scroll(console* c) {
    int stride = c->cols;
    size_t rowbytes = (size_t)stride * sizeof(con_cell);
    memmove(c->cells, c->cells + stride, rowbytes * (size_t)(c->rows - 1));
    con_cell blank; blank.ch = ' '; blank.attr = c->cur_attr;
    con_cell* last = c->cells + stride * (c->rows - 1);
    for (int x = 0; x < stride; x++) last[x] = blank;
    c->dirty = 1;
}

static void console_put_raw(console* c, uint32_t ch) {
    if (c->cur_x >= c->cols) { c->cur_x = 0; if (++c->cur_y >= c->rows) { console_scroll(c); c->cur_y = c->rows - 1; } }
    con_cell* cell = &c->cells[c->cur_y * c->cols + c->cur_x];
    cell->ch = (ch >= 32 && ch <= 255) ? ch : (ch < 32 ? ' ' : '?');
    cell->attr = c->cur_attr;
    c->cur_x++;
    c->dirty = 1;
}

static void console_newline(console* c) {
    c->cur_y++;
    if (c->cur_y >= c->rows) { console_scroll(c); c->cur_y = c->rows - 1; }
}

static void console_erase_display(console* c, int mode) {
    int y0, y1;
    if (mode == 0) { y0 = c->cur_y; y1 = c->rows - 1; }
    else if (mode == 1) { y0 = 0; y1 = c->cur_y; }
    else { y0 = 0; y1 = c->rows - 1; }
    con_cell blank; blank.ch = ' '; blank.attr = c->cur_attr;
    for (int y = y0; y <= y1; y++) {
        int x0 = 0, x1 = c->cols - 1;
        if (mode == 0 && y == c->cur_y) x0 = c->cur_x;
        if (mode == 1 && y == c->cur_y) x1 = c->cur_x;
        for (int x = x0; x <= x1; x++) c->cells[y * c->cols + x] = blank;
    }
    c->dirty = 1;
}

static void console_erase_line(console* c, int mode) {
    int x0, x1;
    if (mode == 0) { x0 = c->cur_x; x1 = c->cols - 1; }
    else if (mode == 1) { x0 = 0; x1 = c->cur_x; }
    else { x0 = 0; x1 = c->cols - 1; }
    con_cell blank; blank.ch = ' '; blank.attr = c->cur_attr;
    for (int x = x0; x <= x1; x++) c->cells[c->cur_y * c->cols + x] = blank;
    c->dirty = 1;
}

// xterm 256-color index -> 8-bit RGB (0..255 each).
static void xterm256_rgb(int idx, int* r, int* g, int* b) {
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    if (idx < 16) {
        // xterm's 16 base colors (slightly brighter than VGA; nearest-VGA maps them back).
        static const uint8_t base[16][3] = {
            {  0,  0,  0}, {205,  0,  0}, {  0,205,  0}, {205,205,  0},
            {  0,  0,238}, {205,  0,205}, {  0,205,205}, {229,229,229},
            {127,127,127}, {255,  0,  0}, {  0,255,  0}, {255,255,  0},
            { 92, 92,255}, {255,  0,255}, {  0,255,255}, {255,255,255},
        };
        *r = base[idx][0]; *g = base[idx][1]; *b = base[idx][2];
    } else if (idx < 232) {
        int v = idx - 16;                     // 6x6x6 RGB cube
        int rr = v / 36, gg = (v / 6) % 6, bb = v % 6;
        *r = rr ? 55 + rr * 40 : 0;
        *g = gg ? 55 + gg * 40 : 0;
        *b = bb ? 55 + bb * 40 : 0;
    } else {
        int lv = 8 + (idx - 232) * 10;        // 24-step grayscale
        *r = *g = *b = lv;
    }
}

// Nearest of the 16 VGA palette entries to an 8-bit RGB triple (squared Euclidean).
static int rgb_to_16(int r, int g, int b) {
    int best = 0;
    long bestd = 1L << 30;
    for (int i = 0; i < 16; i++) {
        int dr = r - vga_pal[i][0], dg = g - vga_pal[i][1], db = b - vga_pal[i][2];
        long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// Map an xterm 256-color index to the nearest of the 16 VGA palette entries.
static int xterm256_to_16(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    if (idx < 16) return idx;              // xterm 0-15 == VGA 0-15 (dark + bright), identity
    int r, g, b; xterm256_rgb(idx, &r, &g, &b);
    return rgb_to_16(r, g, b);
}

static void console_sgr(console* c, int code) {
    if (code == 0) { c->cur_attr = ATTR_DEFAULT; return; }
    if (code == 1) { c->cur_attr = (uint8_t)((c->cur_attr & 0xf0) | (c->cur_attr & 0x07) | 0x08); return; } // bold -> bright fg
    if (code == 7) { uint8_t f = c->cur_attr & 0x0f, b = (c->cur_attr >> 4) & 0x0f; c->cur_attr = (uint8_t)((f << 4) | b); return; }
    if (code == 21 || code == 22) { c->cur_attr = (uint8_t)(c->cur_attr & ~0x08); return; }                 // bold off
    if (code == 27) { uint8_t f = c->cur_attr & 0x0f, b = (c->cur_attr >> 4) & 0x0f; c->cur_attr = (uint8_t)((f << 4) | b); return; }
    if (code >= 30 && code <= 37) { c->cur_attr = (uint8_t)((c->cur_attr & 0xf0) | (code - 30)); return; }
    if (code >= 90 && code <= 97) { c->cur_attr = (uint8_t)((c->cur_attr & 0xf0) | (code - 90 + 8)); return; }
    if (code >= 40 && code <= 47) { c->cur_attr = (uint8_t)((c->cur_attr & 0x0f) | ((code - 40) << 4)); return; }
    if (code >= 100 && code <= 107) { c->cur_attr = (uint8_t)((c->cur_attr & 0x0f) | ((code - 100 + 8) << 4)); return; }
}

// Full SGR `m` sequence, including 256-color (38;5;N / 48;5;N) and truecolor
// (38;2;R;G;B / 48;2;R;G;B) — mapped to the nearest of the 16 VGA colors so the
// extended-color arguments are consumed and never leak into the text grid.
static void console_sgr_seq(console* c) {
    int n = c->esc_argc;
    if (n == 0) { console_sgr(c, 0); return; }
    int i = 0;
    while (i < n) {
        int p = c->esc_args[i++];
        if (p == 38 || p == 48) {
            int bg = (p == 48);
            if (i + 1 < n && c->esc_args[i] == 5) {          // 38;5;N / 48;5;N
                int idx = c->esc_args[i + 1];
                i += 2;
                int col = xterm256_to_16(idx);
                c->cur_attr = bg ? (uint8_t)((c->cur_attr & 0x0f) | (col << 4))
                                 : (uint8_t)((c->cur_attr & 0xf0) | col);
            } else if (i + 2 < n && c->esc_args[i] == 2) {   // 38;2;R;G;B / 48;2;R;G;B
                int r = c->esc_args[i + 1], g = c->esc_args[i + 2], b = c->esc_args[i + 3];
                i += 4;
                int col = rgb_to_16(r, g, b);
                c->cur_attr = bg ? (uint8_t)((c->cur_attr & 0x0f) | (col << 4))
                                 : (uint8_t)((c->cur_attr & 0xf0) | col);
            }
            // malformed 38/48 (bare, or unknown color space): swallow this param only
            continue;
        }
        if (p == 39) { c->cur_attr = (uint8_t)((c->cur_attr & 0xf0) | (ATTR_DEFAULT & 0x0f)); continue; }            // default fg
        if (p == 49) { c->cur_attr = (uint8_t)((c->cur_attr & 0x0f) | ((ATTR_DEFAULT >> 4) & 0x0f)); continue; }      // default bg
        console_sgr(c, p);
    }
}

static void console_csi(console* c, uint8_t final) {
    (void)final;
    int n = c->esc_argc ? c->esc_args[0] : 1;
    if (n < 1) n = 1;

    switch (final) {
    case 'r': /* set scrolling region; we use the full window always */ break;
    case 'H': case 'f': /* cursor position, 1-based */
        c->cur_y = (c->esc_argc ? c->esc_args[0] : 1) - 1;
        c->cur_x = (c->esc_argc > 1 ? c->esc_args[1] : 1) - 1;
        if (c->cur_y < 0) c->cur_y = 0; if (c->cur_y >= c->rows) c->cur_y = c->rows - 1;
        if (c->cur_x < 0) c->cur_x = 0; if (c->cur_x >= c->cols) c->cur_x = c->cols - 1;
        break;
    case 'A': c->cur_y -= n; if (c->cur_y < 0) c->cur_y = 0; break;
    case 'B': case 'e': c->cur_y += n; if (c->cur_y >= c->rows) c->cur_y = c->rows - 1; break;
    case 'C': case 'a': c->cur_x += n; if (c->cur_x >= c->cols) c->cur_x = c->cols - 1; break;
    case 'D': c->cur_x -= n; if (c->cur_x < 0) c->cur_x = 0; break;
    case 'G': case '`': c->cur_x = n - 1; if (c->cur_x < 0) c->cur_x = 0; if (c->cur_x >= c->cols) c->cur_x = c->cols - 1; break;
    case 'd': c->cur_y = n - 1; if (c->cur_y < 0) c->cur_y = 0; if (c->cur_y >= c->rows) c->cur_y = c->rows - 1; break;
    case 'J': console_erase_display(c, c->esc_argc ? c->esc_args[0] : 0); break;
    case 'K': console_erase_line(c, c->esc_argc ? c->esc_args[0] : 0); break;
    case 'm':
        console_sgr_seq(c);
        break;
    case 'h': case 'l': {
        // mode set/reset: the only one we track is DECTCEM (?25) cursor visibility
        if (c->esc_priv && (c->esc_argc == 0 || c->esc_args[0] == 25))
            c->cursor_visible = (final == 'h');
        break;
    }
    default:
        break;
    }
}

// Nearest ASCII for one of the box-drawing codepoints U+2500..U+257F.
static uint32_t map_box_drawing(uint32_t cp) {
    switch (cp) {
    case 0x2571: return '/';   // ╱
    case 0x2572: return '\\';  // ╲
    case 0x2573: return 'X';   // ╳
    case 0x2574: case 0x2576: case 0x2578: case 0x257A: return '-';  // left/right half lines
    case 0x2575: case 0x2577: case 0x2579: case 0x257B: return '|';  // up/down half lines
    case 0x257C: case 0x257E: return '=';                            // heavy left/right
    case 0x257D: case 0x257F: return '|';                            // heavy up/down
    }
    switch (cp) {  // horizontal lines (light/dashed/heavy/double)
    case 0x2500: case 0x2504: case 0x2505: case 0x2508: case 0x2509:
    case 0x254C: return '-';
    case 0x2501: case 0x254D: case 0x2550: return '=';
    }
    switch (cp) {  // vertical lines (light/dashed/heavy/double)
    case 0x2502: case 0x2503: case 0x2506: case 0x2507: case 0x250A:
    case 0x250B: case 0x254E: case 0x254F: case 0x2551: return '|';
    }
    return '+';    // corners, tees, crosses, arcs
}

// Map a Unicode codepoint to 1..4 cell glyphs (ASCII, or Latin-1 the font already has).
// Always writes at least one glyph; returns the count. Unknown codepoints become '?'.
static int console_map_glyph(uint32_t cp, uint32_t out[4]) {
    if (cp >= 0x20 && cp <= 0x7e) { out[0] = cp; return 1; }   // printable ASCII identity

    switch (cp) {
    case 0x00a0: out[0] = ' '; return 1;                         // NBSP -> space
    case 0x00b7: out[0] = '.'; return 1;                         // · middle dot
    case 0x2013: case 0x2014: case 0x2015: case 0x2212:
        out[0] = '-'; return 1;                                  // – — ― −
    case 0x2018: case 0x2019: case 0x201a: case 0x201b:
        out[0] = '\''; return 1;                                 // ‘ ’ ‚ ‛
    case 0x201c: case 0x201d: case 0x201e:
        out[0] = '"'; return 1;                                  // “ ” „
    case 0x2022: case 0x2219: case 0x2027:
        out[0] = '*'; return 1;                                  // • ∙ ‧
    case 0x2026: out[0] = '.'; out[1] = '.'; out[2] = '.'; return 3;  // …
    case 0x2190: out[0] = '<'; out[1] = '-'; return 2;           // ←
    case 0x2191: out[0] = '^'; return 1;                         // ↑
    case 0x2192: out[0] = '-'; out[1] = '>'; return 2;           // →
    case 0x2193: out[0] = 'v'; return 1;                         // ↓
    case 0x21d0: out[0] = '<'; out[1] = '='; return 2;           // ⇐
    case 0x21d2: out[0] = '='; out[1] = '>'; return 2;           // ⇒
    case 0x2713: case 0x2714: out[0] = 'v'; return 1;            // ✓ ✔
    case 0x2715: case 0x2717: case 0x2718: out[0] = 'x'; return 1;  // ✕ ✗ ✘
    case 0x2605: case 0x2606: out[0] = '*'; return 1;            // ★ ☆
    case 0x25a0: case 0x25a1: out[0] = '#'; return 1;            // ■ □
    case 0x25cf: out[0] = '*'; return 1;                         // ● black circle (systemd status)
    case 0x25cb: out[0] = 'o'; return 1;                         // ○ white circle
    case 0x25b6: case 0x25ba: out[0] = '>'; return 1;            // ▶ ►
    case 0x25c0: case 0x25c4: out[0] = '<'; return 1;            // ◀ ◄
    case 0x2610: out[0] = '['; out[1] = ']'; return 2;           // ☐
    case 0x2611: out[0] = '['; out[1] = 'x'; out[2] = ']'; return 3;  // ☑
    case 0x2612: out[0] = '['; out[1] = 'X'; out[2] = ']'; return 3;  // ☒
    case 0x2122: out[0] = '('; out[1] = 't'; out[2] = 'm'; out[3] = ')'; return 4;  // ™
    case 0x2039: out[0] = '<'; return 1;                         // ‹
    case 0x203a: out[0] = '>'; return 1;                         // ›
    }

    if (cp >= 0x2500 && cp <= 0x257f) { out[0] = map_box_drawing(cp); return 1; }
    if (cp >= 0xa0 && cp <= 0xff) { out[0] = cp; return 1; }     // Latin-1 identity (font has these)

    out[0] = '?';
    return 1;
}

// Emit one decoded codepoint into the grid (mapping Unicode to font glyphs first).
static void console_emit(console* c, uint32_t cp) {
    uint32_t g[4];
    int n = console_map_glyph(cp, g);
    for (int i = 0; i < n; i++) console_put_raw(c, g[i]);
}

// Handle one ASCII byte through the ANSI/CSI state machine + control/print dispatch.
// (Non-ASCII bytes are UTF-8 and never reach here: they are decoded by console_put_bytes.)
static void console_put_byte(console* c, uint8_t b) {
    if (c->esc_state == 2) { /* inside CSI */
        if (b >= '0' && b <= '9') {
            if (c->esc_argc < 16) c->esc_args[c->esc_argc] = c->esc_args[c->esc_argc] * 10 + (b - '0');
        } else if (b == ';') {
            if (c->esc_argc < 15) c->esc_argc++;
        } else if (b == '?') {
            c->esc_priv = 1;
        } else if (b >= 0x40 && b <= 0x7e) {
            if (c->esc_argc) c->esc_argc++;  /* count the final (empty) param */
            console_csi(c, b);
            c->esc_state = 0; c->esc_priv = 0; c->esc_argc = 0;
            memset(c->esc_args, 0, sizeof c->esc_args);
        }
        // else: ignore other intermediate bytes
        return;
    }

    if (c->esc_state == 1) { /* saw ESC */
        if (b == '[') { c->esc_state = 2; c->esc_argc = 0; c->esc_priv = 0; memset(c->esc_args, 0, sizeof c->esc_args); return; }
        if (b == ']') { c->esc_state = 3; c->esc_curarg = 0; return; }
        if (b == 'c') { console_reset(c); c->esc_state = 0; return; }
        if (b == '7') { c->esc_saved_x = c->cur_x; c->esc_saved_y = c->cur_y; c->esc_saved_attr = c->cur_attr; c->esc_state = 0; return; }
        if (b == '8') { c->cur_x = c->esc_saved_x; c->cur_y = c->esc_saved_y; c->cur_attr = c->esc_saved_attr; c->esc_state = 0; return; }
        if (b == '(' || b == ')') { c->esc_state = 4; return; }  // charset select, skip next
        c->esc_state = 0;
        return;
    }

    if (c->esc_state == 3 || c->esc_state == 5) { /* OSC: BEL, C1 ST, or ESC \ */
        if (b == 0x07 || b == 0x9c || (c->esc_state == 5 && b == '\\'))
            c->esc_state = 0;
        else
            c->esc_state = b == 0x1b ? 5 : 3;
        return;
    }

    if (c->esc_state == 4) { c->esc_state = 0; return; }  // skip charset designation

    switch (b) {
    case 0x1b: c->esc_state = 1; break;
    case '\r': c->cur_x = 0; break;
    case '\n': console_newline(c); break;
    case '\b': if (c->cur_x > 0) c->cur_x--; break;
    case '\t': c->cur_x = (c->cur_x + 8) & ~7; if (c->cur_x >= c->cols) c->cur_x = c->cols - 1; break;
    case 0x07: break; /* bell */
    case 0x0b: case 0x0c: break; /* vertical tab / form feed */
    default:
        if (b >= 0x20) console_put_raw(c, b);
        break;
    }
}

// Feed a byte stream to the console. UTF-8 sequences are decoded into codepoints BEFORE
// dispatch (ESC/CSI/OSC bytes are ASCII and so pass through the decoder unchanged, keeping
// the ANSI state machine intact); invalid or stray continuation bytes render as a single '?'.
void console_put_bytes(console* c, const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t b = data[i];
        // OSC payloads are opaque (including UTF-8); do not render or decode them.
        // Preserve ESC pending state across read boundaries for the 7-bit ST terminator.
        if (c->esc_state == 3 || c->esc_state == 5) {
            console_put_byte(c, b);
            continue;
        }

        if (c->utf8_remaining > 0) {
            if ((b & 0xc0) == 0x80) {                       // continuation byte
                c->utf8_codepoint = (c->utf8_codepoint << 6) | (b & 0x3f);
                if (--c->utf8_remaining == 0) {
                    uint32_t cp = c->utf8_codepoint;
                    c->utf8_codepoint = 0;
                    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                        console_emit(c, '?');              // surrogates / out of range
                    else
                        console_emit(c, cp);
                }
                continue;
            }
            // Not a continuation: the sequence is broken. Emit '?' and reprocess this byte.
            console_emit(c, '?');
            c->utf8_remaining = 0;
            c->utf8_codepoint = 0;
            // fall through to handle b as a fresh byte
        }

        if (b < 0x80) { console_put_byte(c, b); continue; }

        if ((b & 0xe0) == 0xc0) {                          // 2-byte lead
            uint32_t cp = b & 0x1f;
            if (cp < 2) { console_emit(c, '?'); continue; }  // 0xC0/0xC1 overlong
            c->utf8_codepoint = cp; c->utf8_remaining = 1;
            continue;
        }
        if ((b & 0xf0) == 0xe0) {                          // 3-byte lead
            c->utf8_codepoint = b & 0x0f; c->utf8_remaining = 2;
            continue;
        }
        if ((b & 0xf8) == 0xf0 && b <= 0xf4) {             // 4-byte lead (0xF5-0xFF invalid)
            c->utf8_codepoint = b & 0x07; c->utf8_remaining = 3;
            continue;
        }
        console_emit(c, '?');                              // stray continuation / invalid lead
    }
}

void console_blink_tick(console* c) {
    c->cursor_on = !c->cursor_on;
}

static const uint8_t* glyph_for(uint32_t ch) {
    if (ch < 256) return console_font8x13[ch];
    return console_font8x13['?'];
}

void console_render(const console* c, uint8_t* rgba, int width, int height, int stride) {
    (void)width; (void)height;
    const int fw = CONSOLE_FONT_W, fh = CONSOLE_FONT_H, ch = CONSOLE_CELL_H;
    for (int row = 0; row < c->rows; row++) {
        int py = row * ch;
        if (py + ch > height) break;
        for (int col = 0; col < c->cols; col++) {
            int px = col * fw;
            if (px + fw > width) break;
            const con_cell* cell = &c->cells[row * c->cols + col];
            int fg = cell->attr & 0x0f, bg = (cell->attr >> 4) & 0x0f;
            const uint8_t* glyph = glyph_for(cell->ch);
            int draw_cursor = c->cursor_visible && c->cursor_on && row == c->cur_y && col == c->cur_x;
            for (int yy = 0; yy < ch; yy++) {
                uint8_t rowbits = (yy < fh) ? glyph[fh - 1 - yy] : 0;
                uint8_t* p = rgba + (size_t)(py + yy) * stride + (size_t)px * 4;
                for (int xx = 0; xx < fw; xx++) {
                    int set = (rowbits & (0x80 >> xx)) != 0;
                    // block cursor: invert the whole cell background to fg color
                    if (draw_cursor) set = 1;
                    const uint8_t* rgb = vga_pal[set ? fg : bg];
                    p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2]; p[3] = 0xff;
                    p += 4;
                }
            }
        }
    }
}

size_t console_text_snapshot(const console* c, char* out, size_t cap) {
    size_t used = 0;
    for (int row = 0; row < c->rows; row++) {
        int last = c->cols - 1;
        while (last >= 0 && c->cells[row * c->cols + last].ch == ' ') last--;
        if (last < 0) {
            if (used + 2 <= cap) { out[used++] = '\n'; }
            continue;
        }
        for (int x = 0; x <= last; x++) {
            uint32_t ch = c->cells[row * c->cols + x].ch;
            if (ch < 32) ch = ' ';
            if (used + 1 < cap) out[used++] = (char)ch;
        }
        if (used + 1 < cap) out[used++] = '\n';
    }
    if (used < cap) out[used] = 0; else if (cap) out[cap - 1] = 0;
    return used;
}

int console_has_content(const console* c) {
    for (int row = 0; row < c->rows; row++)
        for (int x = 0; x < c->cols; x++)
            if (c->cells[row * c->cols + x].ch != ' ') return 1;
    return 0;
}
