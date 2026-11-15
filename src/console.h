// console.h — a text-mode virtual console: a fixed character grid plus a small VT100/ANSI
// terminal emulator, rendered into an RGBA framebuffer with an embedded 8x13 font drawn in
// 8x16 cells. This is the "console write path" the Linux VT driver would otherwise own: the
// guest's /dev/ttyN writes (already cooked by the pty line discipline) land here and become
// pixels.
#ifndef RDRIFT_CONSOLE_H
#define RDRIFT_CONSOLE_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CON_MAX_COLS 512
#define CON_MAX_ROWS 256

typedef struct {
    uint32_t ch;     // font glyph: 0x20-0x7e ASCII or 0xa0-0xff Latin-1 (Unicode mapped in console.c)
    uint8_t  attr;   // low nibble = fg (0..15), high nibble = bg (0..15)
} con_cell;

typedef struct {
    int cols, rows;
    int cur_x, cur_y;
    uint8_t cur_attr;
    int cursor_on;      // blink phase; toggled by console_blink_tick()
    int cursor_visible; // hidden by DECTCEM (ESC[?25l)
    int dirty;          // set whenever the grid changed since the last render

    con_cell* cells;    // cols*rows

    // escape-sequence parser state
    int esc_state;      // 0 = none, 1 = saw ESC, 2 = saw ESC[ (CSI), 3 = OSC (ESC ])
    int esc_priv;       // set when CSI starts with '?'
    int esc_argc;
    int esc_args[16];
    int esc_curarg;     // for OSC we accumulate raw bytes until BEL/ST
    int esc_saved_x, esc_saved_y;
    uint8_t esc_saved_attr;
    uint32_t utf8_codepoint;
    int utf8_remaining;
} console;

void console_init(console* c, int cols, int rows);
void console_free(console* c);
void console_reset(console* c);                       // clear + home + default attr
void console_resize(console* c, int cols, int rows);
// Feed a byte stream: UTF-8 is decoded to codepoints and Unicode glyphs are mapped onto the
// embedded font (box-drawing -> ASCII, arrows/checks/dots -> ASCII), and 256-color/truecolor
// SGR (38;5;N / 48;5;N / 38;2;R;G;B / ...) is mapped to the nearest 16-color palette entry.
void console_put_bytes(console* c, const uint8_t* data, size_t n);
void console_blink_tick(console* c);                  // flip cursor_on
// Render the grid into an RGBA8 buffer. `stride` is bytes per row; `width`/`height` are the
// buffer dimensions (must be >= cols*8 x rows*16). Pixels outside the grid are left untouched.
void console_render(const console* c, uint8_t* rgba, int width, int height, int stride);
// Dump the visible text (rows joined by '\n', trailing spaces trimmed) for logging/validation.
size_t console_text_snapshot(const console* c, char* out, size_t cap);
// True if any cell is non-space (i.e. the console has non-blank content).
int console_has_content(const console* c);

#ifdef __cplusplus
}
#endif
#endif
