#include "hexdump.h"

static const char kHexCharsUpper[] = { "0123456789ABCDEF" };

#define isprint(c) (((u32)(c)-0x20) < 0x5f) /* SP-~ */

static u32 ndigits16(u64 v) {
    if (v < 0x10)
        return 1;
    if (v < 0x100)
        return 2;
    if (v < 0x1000)
        return 3;
    if (v < 0x1000000000000UL) {
        if (v < 0x100000000UL) {
            if (v < 0x1000000) {
                if (v < 0x10000)
                    return 4;
                return 5 + (v >= 0x100000);
            }
            return 7 + (v >= 0x10000000UL);
        }
        if (v < 0x10000000000UL)
            return 9 + (v >= 0x1000000000UL);
        return 11 + (v >= 0x100000000000UL);
    }
    return 12 + ndigits16(v / 0x1000000000000UL);
}

void hexdump(FILE* write_fp, const void* datap, usize len, uintptr start_addr, int bytes_per_row) {
#define MAX_BYTES_PER_ROW 32

    if (bytes_per_row < 1)
        bytes_per_row = 16;
    if (bytes_per_row > MAX_BYTES_PER_ROW)
        bytes_per_row = MAX_BYTES_PER_ROW;

#define G 4 // bytes per group
#define S 5 // bytes for fancy style (2x needed per group)

    // Hex formatting, e.g. XX XX XX XX  XX ...
    // With MAX_BYTES_PER_ROW=32 we need 103 bytes for visible chars, i.e.
    //     " 00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00"
    //     "  00 00 00 00  00 00 00 00  00 00 00 00  00 00 00 00"
    // 32*3 for each "00 "
    // 32/4*6 for each group, where:
    //   32/4=8 for each group space " "
    //   32/4*5 for fancy style per group "\x1b[22m"
    // 1 for NUL
    char      hexbuf[MAX_BYTES_PER_ROW * 3 + MAX_BYTES_PER_ROW / G * 6 + 1];
    char      asciibuf[MAX_BYTES_PER_ROW + 1]; // +1 for NUL
    const u8* data = datap;
    bool      fancy = isatty(fileno(write_fp));
    const int offsw = MAX(8, ndigits16(len + start_addr));

    for (usize i = 0; i < len; i += bytes_per_row) {
        usize nbyte = MIN(len - i, (usize)bytes_per_row);
        usize bufi = 0;
        usize bufi_vis = 0;
        usize nzero = 0; // tracks number of zero bytes in one group

        // " XX XX XX XX  XX ..."
        for (usize j = 0; j < nbyte; j++) {
            // grouping, e.g. " XX XX XX XX  XX XX XX XX ..."
            //                             ^
            if (j && j % G == 0) {
                hexbuf[bufi++] = ' ';
                bufi_vis++;
                nzero = 0;
            }

            // one byte e.g. " XX"
            u8 c = data[i + j];
            hexbuf[bufi++] = ' ';
            hexbuf[bufi++] = kHexCharsUpper[c >> 4];
            hexbuf[bufi++] = kHexCharsUpper[c & 0xf];
            bufi_vis += 3;

            nzero += c == 0;
            if (nzero == G) {
                const char* style_open = "\x1b[2m";
                const char* style_close = "\x1b[22m";

                // bufi_past points to: " XX XX XX XX"
                //                       ^
                usize bufi_past = bufi - G * 3;
                usize style_open_len = strlen(style_open);
                memmove(&hexbuf[bufi_past + style_open_len], &hexbuf[bufi_past], G * 3);
                memcpy(&hexbuf[bufi_past], style_open, style_open_len);
                bufi += style_open_len;

                usize style_close_len = strlen(style_close);
                memcpy(&hexbuf[bufi], style_close, style_close_len);
                bufi += style_close_len;
            }
        }

        // pad out remainder of last row of hex, if needed
        usize hexlen = (bytes_per_row * 3) + ((bytes_per_row - 1) / G);
        usize taillen = hexlen - bufi_vis;
        memset(&hexbuf[bufi], ' ', taillen);
        hexbuf[bufi + taillen] = 0;

        // ASCII
        char* asciip = asciibuf;
        for (const u8 *p = &data[i], *end = p + nbyte; p < end; p++)
            *asciip++ = isprint(*p) ? *p : '.';
        *asciip = '\0';

        fprintf(write_fp, "%0*lx %s  %s\x1b[0m\n", offsw, i + start_addr, hexbuf, asciibuf);
    }
}
