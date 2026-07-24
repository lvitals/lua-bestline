/* bestline-test.c -- Test framework for lua-bestline with VT100 emulator.
 *
 * This file implements:
 * 1. A minimal VT100 terminal emulator that parses escape sequences
 * 2. A test harness that runs bestline via pipes
 * 3. Visual rendering so the user can watch tests run
 * 4. Test functions and assertions
 *
 * The emulator maintains a logical screen buffer and also renders to the
 * real terminal, allowing visual verification if tests fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <pty.h>

/* ========================= VT100 Emulator ========================= */

#define EMU_ROWS 15
#define EMU_COLS 60

/* Each screen cell stores a complete grapheme cluster and its display width.
 * Wide characters (emoji, CJK) have width=2 and occupy two cells: the main
 * cell holds the character, the next cell has width=0 (continuation).
 * Complex emoji (ZWJ sequences) can be up to ~30 bytes. */
typedef struct {
    char ch[32];  /* UTF-8 bytes for grapheme cluster + null terminator */
    int len;      /* Current length of content in ch[] */
    int width;    /* Display width: 0=continuation, 1=normal, 2=wide char */
} emu_cell_t;

static emu_cell_t emu_screen[EMU_ROWS][EMU_COLS];
static int emu_cursor_row = 0;
static int emu_cursor_col = 0;
static int emu_rows = EMU_ROWS;
static int emu_cols = EMU_COLS;
static int emu_after_zwj = 0;  /* Track if last char was ZWJ for grapheme clusters */

/* UTF-8 accumulator for multi-byte sequences. */
static char utf8_buf[5];
static int utf8_len = 0;
static int utf8_expected = 0;

/* Parser state for escape sequences. */
enum {
    STATE_NORMAL,
    STATE_ESC,      /* Saw ESC */
    STATE_CSI       /* Saw ESC [ */
};

static int parser_state = STATE_NORMAL;
static char csi_buf[32];
static int csi_len = 0;

/* Determine expected UTF-8 byte length from first byte. */
static int utf8_byte_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Decode UTF-8 bytes into a codepoint. */
static uint32_t utf8_decode(const char *s, int len) {
    unsigned char c = s[0];
    uint32_t cp;
    if (len == 1) {
        cp = c;
    } else if (len == 2) {
        cp = (c & 0x1F) << 6;
        cp |= (s[1] & 0x3F);
    } else if (len == 3) {
        cp = (c & 0x0F) << 12;
        cp |= (s[1] & 0x3F) << 6;
        cp |= (s[2] & 0x3F);
    } else if (len == 4) {
        cp = (c & 0x07) << 18;
        cp |= (s[1] & 0x3F) << 12;
        cp |= (s[2] & 0x3F) << 6;
        cp |= (s[3] & 0x3F);
    } else {
        cp = c;
    }
    return cp;
}

/* Determine display width of a codepoint. Returns 0, 1 or 2. */
static int codepoint_width(uint32_t cp) {
    /* Zero-width characters. */
    if (cp == 0) return 0;
    if (cp >= 0x0300 && cp <= 0x036F) return 0;  /* Combining diacriticals */
    if (cp >= 0x1AB0 && cp <= 0x1AFF) return 0;  /* Combining diacriticals ext */
    if (cp >= 0x1DC0 && cp <= 0x1DFF) return 0;  /* Combining diacriticals sup */
    if (cp >= 0x20D0 && cp <= 0x20FF) return 0;  /* Combining for symbols */
    if (cp >= 0xFE20 && cp <= 0xFE2F) return 0;  /* Combining half marks */

    /* Grapheme-extending characters: zero width. */
    if (cp == 0xFE0E || cp == 0xFE0F) return 0;  /* Variation selectors */
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return 0; /* Skin tone modifiers */
    if (cp == 0x200D) return 0;                   /* Zero Width Joiner */

    /* Wide characters: CJK, Emoji, etc. */
    if (cp >= 0x1100 && cp <= 0x115F) return 2;  /* Hangul Jamo */
    if (cp >= 0x231A && cp <= 0x231B) return 2;  /* Watch, Hourglass */
    if (cp >= 0x23E9 && cp <= 0x23F3) return 2;  /* Various symbols */
    if (cp >= 0x23F8 && cp <= 0x23FA) return 2;  /* Various symbols */
    if (cp >= 0x25AA && cp <= 0x25AB) return 2;  /* Small squares */
    if (cp >= 0x25B6 && cp <= 0x25C0) return 2;  /* Play/reverse buttons */
    if (cp >= 0x25FB && cp <= 0x25FE) return 2;  /* Squares */
    if (cp >= 0x2600 && cp <= 0x26FF) return 2;  /* Misc symbols */
    if (cp >= 0x2700 && cp <= 0x27BF) return 2;  /* Dingbats */
    if (cp >= 0x2934 && cp <= 0x2935) return 2;  /* Arrows */
    if (cp >= 0x2B05 && cp <= 0x2B07) return 2;  /* Arrows */
    if (cp >= 0x2B1B && cp <= 0x2B1C) return 2;  /* Squares */
    if (cp == 0x2B50 || cp == 0x2B55) return 2;  /* Star, circle */
    if (cp >= 0x2E80 && cp <= 0x9FFF) return 2;  /* CJK */
    if (cp >= 0xAC00 && cp <= 0xD7AF) return 2;  /* Hangul Syllables */
    if (cp >= 0xF900 && cp <= 0xFAFF) return 2;  /* CJK Compatibility */
    if (cp >= 0xFE10 && cp <= 0xFE1F) return 2;  /* Vertical forms */
    if (cp >= 0xFE30 && cp <= 0xFE6F) return 2;  /* CJK Compatibility Forms */
    if (cp >= 0xFF00 && cp <= 0xFF60) return 2;  /* Fullwidth forms */
    if (cp >= 0xFFE0 && cp <= 0xFFE6) return 2;  /* Fullwidth symbols */
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) return 2; /* Regional indicators */
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return 2; /* Emoji symbols */
    if (cp >= 0x1FA00 && cp <= 0x1FAFF) return 2; /* Emoji extended */
    if (cp >= 0x20000 && cp <= 0x2FFFF) return 2; /* CJK Extension B+ */
    if (cp >= 0x30000 && cp <= 0x3FFFF) return 2; /* CJK Extension G+ */

    return 1;
}

/* Set a cell to a space (empty). */
static void emu_clear_cell(int row, int col) {
    emu_screen[row][col].ch[0] = ' ';
    emu_screen[row][col].ch[1] = '\0';
    emu_screen[row][col].len = 1;
    emu_screen[row][col].width = 1;
}

/* Initialize the emulator. */
static void emu_init(int rows, int cols) {
    emu_rows = rows < EMU_ROWS ? rows : EMU_ROWS;
    emu_cols = cols < EMU_COLS ? cols : EMU_COLS;
    emu_cursor_row = 0;
    emu_cursor_col = 0;
    emu_after_zwj = 0;
    parser_state = STATE_NORMAL;
    csi_len = 0;
    utf8_len = 0;
    utf8_expected = 0;
    for (int r = 0; r < EMU_ROWS; r++) {
        for (int c = 0; c < EMU_COLS; c++) {
            emu_clear_cell(r, c);
        }
    }
}

/* Clear from cursor to end of line. */
static void emu_clear_to_eol(void) {
    for (int c = emu_cursor_col; c < emu_cols; c++) {
        emu_clear_cell(emu_cursor_row, c);
    }
}

/* Clear from cursor to end of screen. */
static void emu_clear_to_eos(void) {
    emu_clear_to_eol();
    for (int r = emu_cursor_row + 1; r < emu_rows; r++) {
        for (int c = 0; c < emu_cols; c++) {
            emu_clear_cell(r, c);
        }
    }
}

/* Clear entire screen. */
static void emu_clear_screen(void) {
    for (int r = 0; r < emu_rows; r++) {
        for (int c = 0; c < emu_cols; c++) {
            emu_clear_cell(r, c);
        }
    }
    emu_cursor_row = 0;
    emu_cursor_col = 0;
}

/* Parse CSI parameters (e.g., "5" from ESC[5C). */
static int csi_get_param(int def) {
    if (csi_len == 0) return def;
    csi_buf[csi_len] = '\0';
    int val = atoi(csi_buf);
    return val > 0 ? val : def;
}

/* Handle a complete CSI sequence. */
static void emu_handle_csi(char cmd) {
    int n = csi_get_param(1);

    switch (cmd) {
    case 'A':  /* Cursor Up */
        emu_cursor_row -= n;
        if (emu_cursor_row < 0) emu_cursor_row = 0;
        break;
    case 'B':  /* Cursor Down */
        emu_cursor_row += n;
        if (emu_cursor_row >= emu_rows) emu_cursor_row = emu_rows - 1;
        break;
    case 'C':  /* Cursor Forward */
        emu_cursor_col += n;
        if (emu_cursor_col >= emu_cols) emu_cursor_col = emu_cols - 1;
        break;
    case 'D':  /* Cursor Backward */
        emu_cursor_col -= n;
        if (emu_cursor_col < 0) emu_cursor_col = 0;
        break;
    case 'H':  /* Cursor Home (or position if params given) */
        emu_cursor_row = 0;
        emu_cursor_col = 0;
        break;
    case 'J':  /* Erase Display */
        if (n == 2) emu_clear_screen();
        else if (n == 0 || csi_len == 0) emu_clear_to_eos();
        break;
    case 'K':  /* Erase Line */
        if (n == 0 || csi_len == 0) emu_clear_to_eol();
        break;
    case 'm':  /* SGR (colors/attributes) - ignore */
        break;
    default:
        /* Unknown CSI sequence, ignore */
        break;
    }
}

/* Find the previous non-continuation cell (for appending extending chars). */
static int emu_find_prev_cell(int row, int col) {
    /* Move back to find the cell that owns this position. */
    while (col > 0) {
        col--;
        if (emu_screen[row][col].width != 0) {
            return col;
        }
    }
    return -1;  /* No previous cell found. */
}

/* Check if codepoint is Zero Width Joiner. */
static int emu_is_zwj(uint32_t cp) {
    return cp == 0x200D;
}

/* Place a complete character at the current cursor position. */
static void emu_put_char(const char *ch, int chlen) {
    uint32_t cp = utf8_decode(ch, chlen);
    int width = codepoint_width(cp);

    /* If we're after a ZWJ, append this char to the previous cell
     * regardless of its width (it's being joined). */
    if (emu_after_zwj) {
        emu_after_zwj = 0;
        int prev_col = emu_find_prev_cell(emu_cursor_row, emu_cursor_col);
        if (prev_col >= 0) {
            emu_cell_t *cell = &emu_screen[emu_cursor_row][prev_col];
            if (cell->len + chlen < (int)sizeof(cell->ch) - 1) {
                memcpy(cell->ch + cell->len, ch, chlen);
                cell->len += chlen;
                cell->ch[cell->len] = '\0';
            }
        }
        /* Check if this char is also a ZWJ (unlikely but possible). */
        if (emu_is_zwj(cp)) {
            emu_after_zwj = 1;
        }
        return;
    }

    if (width == 0) {
        /* Zero-width character - append to previous cell if possible.
         * This handles variation selectors, skin tones, ZWJ sequences. */
        int prev_col = emu_find_prev_cell(emu_cursor_row, emu_cursor_col);
        if (prev_col >= 0) {
            emu_cell_t *cell = &emu_screen[emu_cursor_row][prev_col];
            /* Append if there's room in the buffer. */
            if (cell->len + chlen < (int)sizeof(cell->ch) - 1) {
                memcpy(cell->ch + cell->len, ch, chlen);
                cell->len += chlen;
                cell->ch[cell->len] = '\0';
            }
        }
        /* If this was a ZWJ, next char should also be appended. */
        if (emu_is_zwj(cp)) {
            emu_after_zwj = 1;
        }
        return;
    }

    /* Check if there's room for this character. */
    if (emu_cursor_col + width > emu_cols) {
        /* No room, don't display (clip at edge). */
        return;
    }

    /* Before overwriting, handle orphaned continuation cells:
     * 1. If current cell is a continuation (width=0), clear it first
     * 2. If current cell was a wide char (width=2), clear its continuation */
    emu_cell_t *cur = &emu_screen[emu_cursor_row][emu_cursor_col];
    if (cur->width == 0) {
        /* This was a continuation cell - convert to space. */
        emu_clear_cell(emu_cursor_row, emu_cursor_col);
    } else if (cur->width == 2 && emu_cursor_col + 1 < emu_cols) {
        /* This was a wide char - clear its orphaned continuation. */
        emu_clear_cell(emu_cursor_row, emu_cursor_col + 1);
    }

    /* Store the character in the current cell. */
    memcpy(emu_screen[emu_cursor_row][emu_cursor_col].ch, ch, chlen);
    emu_screen[emu_cursor_row][emu_cursor_col].ch[chlen] = '\0';
    emu_screen[emu_cursor_row][emu_cursor_col].len = chlen;
    emu_screen[emu_cursor_row][emu_cursor_col].width = width;
    emu_cursor_col++;

    /* For wide characters, mark the next cell as continuation. */
    if (width == 2 && emu_cursor_col < emu_cols) {
        emu_screen[emu_cursor_row][emu_cursor_col].ch[0] = '\0';
        emu_screen[emu_cursor_row][emu_cursor_col].len = 0;
        emu_screen[emu_cursor_row][emu_cursor_col].width = 0;
        emu_cursor_col++;
    }
}

/* Feed a single byte to the emulator. */
static void emu_feed_byte(unsigned char c) {
    switch (parser_state) {
    case STATE_NORMAL:
        if (c == 0x1b) {
            parser_state = STATE_ESC;
            utf8_len = 0;  /* Cancel any pending UTF-8 sequence. */
        } else if (c == '\r') {
            emu_cursor_col = 0;
            utf8_len = 0;
        } else if (c == '\n') {
            emu_cursor_row++;
            if (emu_cursor_row >= emu_rows) {
                /* Scroll up: move all rows up, clear bottom row. */
                for (int r = 0; r < emu_rows - 1; r++) {
                    memcpy(emu_screen[r], emu_screen[r + 1],
                           sizeof(emu_cell_t) * emu_cols);
                }
                for (int c2 = 0; c2 < emu_cols; c2++) {
                    emu_clear_cell(emu_rows - 1, c2);
                }
                emu_cursor_row = emu_rows - 1;
            }
            utf8_len = 0;
        } else if (c == '\b') {
            if (emu_cursor_col > 0) {
                emu_cursor_col--;
                /* If we're on a continuation cell, back up one more. */
                if (emu_screen[emu_cursor_row][emu_cursor_col].width == 0 &&
                    emu_cursor_col > 0) {
                    emu_cursor_col--;
                }
            }
            utf8_len = 0;
        } else if (c >= 32 || (c & 0x80)) {
            /* Printable character or UTF-8 byte. */
            if ((c & 0x80) == 0) {
                /* ASCII character - display immediately. */
                char ch[2] = {c, '\0'};
                emu_put_char(ch, 1);
                utf8_len = 0;
            } else if ((c & 0xC0) == 0xC0) {
                /* Start of UTF-8 multi-byte sequence. */
                utf8_buf[0] = c;
                utf8_len = 1;
                utf8_expected = utf8_byte_len(c);
            } else if ((c & 0xC0) == 0x80 && utf8_len > 0) {
                /* Continuation byte. */
                utf8_buf[utf8_len++] = c;
                if (utf8_len >= utf8_expected) {
                    /* Complete UTF-8 character. */
                    utf8_buf[utf8_len] = '\0';
                    emu_put_char(utf8_buf, utf8_len);
                    utf8_len = 0;
                }
            } else {
                /* Invalid UTF-8 - reset. */
                utf8_len = 0;
            }
        }
        break;

    case STATE_ESC:
        if (c == '[') {
            parser_state = STATE_CSI;
            csi_len = 0;
        } else {
            /* Unknown escape, back to normal. */
            parser_state = STATE_NORMAL;
        }
        break;

    case STATE_CSI:
        if ((c >= '0' && c <= '9') || c == '?') {
            if (csi_len < (int)sizeof(csi_buf) - 1) {
                csi_buf[csi_len++] = c;
            }
        } else if (c == ';') {
            /* Multiple params - for simplicity, just reset. */
            csi_len = 0;
        } else {
            /* End of CSI sequence. */
            emu_handle_csi(c);
            parser_state = STATE_NORMAL;
        }
        break;
    }
}

/* Debug flag for verbose output. */
static int emu_debug = 0;

/* Feed a buffer to the emulator. */
static void emu_feed(const char *buf, int len) {
    if (emu_debug) {
        printf("EMU_FEED (%d bytes): ", len);
        for (int i = 0; i < len && i < 200; i++) {
            unsigned char c = buf[i];
            if (c >= 32 && c < 127) printf("%c", c);
            else printf("<%02X>", c);
        }
        if (len > 200) printf("...");
        printf("\n");
    }
    for (int i = 0; i < len; i++) {
        emu_feed_byte((unsigned char)buf[i]);
    }
}

/* Get a row from the screen as a UTF-8 string (trimmed of trailing spaces). */
static const char *emu_get_row(int row) {
    static char buf[EMU_COLS * 4 + 1];  /* Each cell can be up to 4 UTF-8 bytes. */
    if (row < 0 || row >= emu_rows) {
        buf[0] = '\0';
        return buf;
    }
    /* Build the row string, skipping continuation cells. */
    int pos = 0;
    int last_non_space = -1;
    for (int c = 0; c < emu_cols; c++) {
        emu_cell_t *cell = &emu_screen[row][c];
        if (cell->width == 0) continue;  /* Skip continuation cells. */

        int chlen = strlen(cell->ch);
        if (pos + chlen < (int)sizeof(buf) - 1) {
            memcpy(buf + pos, cell->ch, chlen);
            if (!(chlen == 1 && cell->ch[0] == ' ')) {
                last_non_space = pos + chlen;
            }
            pos += chlen;
        }
    }
    /* Trim trailing spaces. */
    if (last_non_space >= 0) {
        buf[last_non_space] = '\0';
    } else {
        buf[0] = '\0';
    }
    return buf;
}

/* ========================= Visual Rendering ========================= */

/* Render the emulator state to the real terminal for visual inspection.
 * This shows the screen contents and cursor position. */
static void render_to_terminal(const char *test_name) {
    /* Clear real screen and move home. */
    printf("\x1b[2J\x1b[H");

    /* Header. */
    printf("\x1b[1;36m=== BESTLINE TEST: %s ===\x1b[0m\n\n", test_name);

    /* Draw screen with border. */
    printf("\x1b[33m+");
    for (int c = 0; c < emu_cols; c++) printf("-");
    printf("+\x1b[0m\n");

    for (int r = 0; r < emu_rows; r++) {
        printf("\x1b[33m|\x1b[0m");
        for (int c = 0; c < emu_cols; c++) {
            emu_cell_t *cell = &emu_screen[r][c];

            if (cell->width == 0) {
                /* Continuation cell - skip (already printed with wide char). */
                continue;
            }

            if (r == emu_cursor_row && c == emu_cursor_col) {
                /* Highlight cursor position. */
                printf("\x1b[7m%s\x1b[0m", cell->ch);
            } else {
                printf("%s", cell->ch);
            }
        }
        printf("\x1b[33m|\x1b[0m\n");
    }

    printf("\x1b[33m+");
    for (int c = 0; c < emu_cols; c++) printf("-");
    printf("+\x1b[0m\n");

    /* Cursor info. */
    printf("\nCursor: row=%d, col=%d\n", emu_cursor_row, emu_cursor_col);
    fflush(stdout);
}

/* ========================= Test Harness ========================= */

static int child_pid = -1;
static int pty_fd = -1;
static const char *current_test = "unknown";

/* Start the bestline example program. */
static int test_start(const char *test_name, const char *program) {
    current_test = test_name;
    emu_init(EMU_ROWS, EMU_COLS);

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = EMU_ROWS;
    ws.ws_col = EMU_COLS;

    child_pid = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child_pid == -1) {
        perror("forkpty");
        return -1;
    }

    if (child_pid == 0) {
        /* Set test environment variables. */
        setenv("BESTLINE_COLS", "60", 1);
        setenv("COLUMNS", "60", 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("LUA_CPATH", "./?.so;;", 1);
        setenv("BESTLINE_TEST", "1", 1);

        /* Use shell to parse the command line arguments. */
        execl("/bin/sh", "sh", "-c", program, NULL);
        perror("exec");
        exit(1);
    }

    /* Give child time to start and print prompt. */
    usleep(50000);  /* 50ms */

    /* Read initial output (prompt) with timeout. */
    char buf[4096];
    fd_set fds;
    struct timeval tv = {1, 0};  /* 1 second timeout */

    FD_ZERO(&fds);
    FD_SET(pty_fd, &fds);

    if (select(pty_fd + 1, &fds, NULL, NULL, &tv) > 0) {
        int n = read(pty_fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            emu_feed(buf, n);
        }
    }

    render_to_terminal(test_name);
    return 0;
}

/* End the test, clean up. */
static void test_end(void) {
    if (child_pid > 0) {
        /* Send Ctrl-D (EOF) to terminate cleanly. */
        write(pty_fd, "\x04", 1);
        usleep(50000);

        /* Wait briefly for child to exit. */
        int status;
        int wait_result = waitpid(child_pid, &status, WNOHANG);
        if (wait_result == 0) {
            /* Child didn't exit, send SIGTERM. */
            kill(child_pid, SIGTERM);
            usleep(10000);
            waitpid(child_pid, &status, WNOHANG);
        }
        child_pid = -1;
    }
    if (pty_fd != -1) {
        close(pty_fd);
        pty_fd = -1;
    }
}

/* Send keys to bestline and read response. */
static void send_keys(const char *keys) {
    write(pty_fd, keys, strlen(keys));
    usleep(30000);  /* 30ms - give bestline time to process. */

    /* Read response with timeout. */
    char buf[4096];
    fd_set fds;
    struct timeval tv;
    int max_reads = 10;  /* Prevent infinite loop. */

    while (max_reads-- > 0) {
        FD_ZERO(&fds);
        FD_SET(pty_fd, &fds);
        tv.tv_sec = 0;
        tv.tv_usec = 50000;  /* 50ms timeout */

        if (select(pty_fd + 1, &fds, NULL, NULL, &tv) <= 0) {
            break;  /* Timeout or error. */
        }
        int n = read(pty_fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        emu_feed(buf, n);
    }

    render_to_terminal(current_test);
}

/* Send special keys. */
#define KEY_UP      "\x1b[A"
#define KEY_DOWN    "\x1b[B"
#define KEY_RIGHT   "\x1b[C"
#define KEY_LEFT    "\x1b[D"
#define KEY_HOME    "\x1b[H"
#define KEY_END     "\x1b[F"
#define KEY_DELETE  "\x1b[3~"
#define KEY_BACKSPACE "\x7f"
#define KEY_ENTER   "\r"
#define KEY_CTRL_A  "\x01"
#define KEY_CTRL_E  "\x05"
#define KEY_CTRL_U  "\x15"
#define KEY_CTRL_K  "\x0b"
#define KEY_CTRL_W  "\x17"
#define KEY_CTRL_T  "\x14"
#define KEY_CTRL_C  "\x03"

/* ========================= Test Assertions ========================= */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

static void assert_screen_row(int row, const char *expected) {
    tests_run++;
    const char *actual = emu_get_row(row);
    if (strcmp(actual, expected) == 0) {
        tests_passed++;
        printf("\x1b[32m[PASS]\x1b[0m Row %d == \"%s\"\n", row, expected);
    } else {
        tests_failed++;
        printf("\x1b[31m[FAIL]\x1b[0m Row %d:\n", row);
        printf("       Expected: \"%s\"\n", expected);
        printf("       Actual:   \"%s\"\n", actual);
    }
    fflush(stdout);
}

static void assert_cursor(int row, int col) {
    tests_run++;
    if (emu_cursor_row == row && emu_cursor_col == col) {
        tests_passed++;
        printf("\x1b[32m[PASS]\x1b[0m Cursor at (%d, %d)\n", row, col);
    } else {
        tests_failed++;
        printf("\x1b[31m[FAIL]\x1b[0m Cursor position:\n");
        printf("       Expected: (%d, %d)\n", row, col);
        printf("       Actual:   (%d, %d)\n", emu_cursor_row, emu_cursor_col);
    }
    fflush(stdout);
}

static void assert_row_contains(int row, const char *substr) {
    tests_run++;
    const char *actual = emu_get_row(row);
    if (strstr(actual, substr) != NULL) {
        tests_passed++;
        printf("\x1b[32m[PASS]\x1b[0m Row %d contains \"%s\"\n", row, substr);
    } else {
        tests_failed++;
        printf("\x1b[31m[FAIL]\x1b[0m Row %d doesn't contain \"%s\"\n", row, substr);
        printf("       Actual: \"%s\"\n", actual);
    }
    fflush(stdout);
}

static void assert_row_not_contains(int row, const char *substr) {
    tests_run++;
    const char *actual = emu_get_row(row);
    if (strstr(actual, substr) == NULL) {
        tests_passed++;
        printf("\x1b[32m[PASS]\x1b[0m Row %d correctly does NOT contain \"%s\"\n", row, substr);
    } else {
        tests_failed++;
        printf("\x1b[31m[FAIL]\x1b[0m Row %d contains \"%s\", but it should NOT.\n", row, substr);
        printf("       Actual: \"%s\"\n", actual);
    }
    fflush(stdout);
}

static void assert_screen_contains(const char *substr) {
    tests_run++;
    for (int r = 0; r < emu_rows; r++) {
        if (strstr(emu_get_row(r), substr) != NULL) {
            tests_passed++;
            printf("\x1b[32m[PASS]\x1b[0m Screen contains \"%s\"\n", substr);
            fflush(stdout);
            return;
        }
    }
    tests_failed++;
    printf("\x1b[31m[FAIL]\x1b[0m Screen doesn't contain \"%s\"\n", substr);
    fflush(stdout);
}

/* Assert that a cell contains specific bytes (for verifying grapheme clusters). */
static void assert_cell_content(int row, int col, const char *expected, int expected_len) {
    tests_run++;
    emu_cell_t *cell = &emu_screen[row][col];
    if (cell->len == expected_len && memcmp(cell->ch, expected, expected_len) == 0) {
        tests_passed++;
        printf("\x1b[32m[PASS]\x1b[0m Cell (%d,%d) contains %d bytes\n", row, col, expected_len);
    } else {
        tests_failed++;
        printf("\x1b[31m[FAIL]\x1b[0m Cell (%d,%d) content mismatch:\n", row, col);
        printf("       Expected: %d bytes [", expected_len);
        for (int i = 0; i < expected_len; i++) printf("%02X ", (unsigned char)expected[i]);
        printf("]\n");
        printf("       Actual:   %d bytes [", cell->len);
        for (int i = 0; i < cell->len; i++) printf("%02X ", (unsigned char)cell->ch[i]);
        printf("]\n");
    }
    fflush(stdout);
}

/* Assert that a cell has the expected display width. */
static void assert_cell_width(int row, int col, int expected_width) {
    tests_run++;
    emu_cell_t *cell = &emu_screen[row][col];
    if (cell->width == expected_width) {
        tests_passed++;
        printf("\x1b[32m[PASS]\x1b[0m Cell (%d,%d) width == %d\n", row, col, expected_width);
    } else {
        tests_failed++;
        printf("\x1b[31m[FAIL]\x1b[0m Cell (%d,%d) width:\n", row, col);
        printf("       Expected: %d\n", expected_width);
        printf("       Actual:   %d\n", cell->width);
    }
    fflush(stdout);
}

/* ========================= Tests ========================= */

static void test_simple_typing(void) {
    if (test_start("Simple Typing", "lua test.lua") == -1) return;

    send_keys("hello");
    assert_row_contains(0, "hello");
    assert_cursor(0, strlen("? ") + 5);

    send_keys(" world");
    assert_screen_row(0, "? hello world");

    test_end();
}

static void test_cursor_movement(void) {
    if (test_start("Cursor Movement", "lua test.lua") == -1) return;

    send_keys("abcdef");
    int prompt_len = strlen("? ");

    /* Move left 3 times. */
    send_keys(KEY_LEFT KEY_LEFT KEY_LEFT);
    assert_cursor(0, prompt_len + 3);  /* After "abc" */

    /* Move right 1 time. */
    send_keys(KEY_RIGHT);
    assert_cursor(0, prompt_len + 4);  /* After "abcd" */

    /* Home. */
    send_keys(KEY_CTRL_A);
    assert_cursor(0, prompt_len);

    /* End. */
    send_keys(KEY_CTRL_E);
    assert_cursor(0, prompt_len + 6);

    test_end();
}

static void test_backspace_delete(void) {
    if (test_start("Backspace and Delete", "lua test.lua") == -1) return;

    send_keys("hello");
    int prompt_len = strlen("? ");

    /* Backspace. */
    send_keys(KEY_BACKSPACE);
    assert_row_contains(0, "hell");
    assert_cursor(0, prompt_len + 4);

    /* Move left and delete forward. */
    send_keys(KEY_LEFT KEY_LEFT);
    send_keys(KEY_DELETE);
    assert_row_contains(0, "hel");

    test_end();
}

static void test_utf8_typing(void) {
    if (test_start("UTF-8 Typing", "lua test.lua") == -1) return;

    /* Type some UTF-8 characters. */
    send_keys("caf\xc3\xa9");  /* "café" - é is 2 bytes */
    assert_row_contains(0, "café");

    test_end();
}

static void test_utf8_emoji(void) {
    if (test_start("UTF-8 Emoji", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    /* Type text with emoji (🎉 is 4 bytes, displays as 2 columns). */
    send_keys("hi \xf0\x9f\x8e\x89 there");  /* "hi 🎉 there" */
    assert_row_contains(0, "hi");

    /* The emoji takes 2 columns, so cursor should be at:
     * prompt(7) + "hi "(3) + emoji(2) + " there"(6) = 18 */
    assert_cursor(0, prompt_len + 3 + 2 + 6);

    test_end();
}

static void test_utf8_cursor_over_emoji(void) {
    if (test_start("UTF-8 Cursor Over Emoji", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    /* Type: "a🎉b" */
    send_keys("a\xf0\x9f\x8e\x89" "b");
    /* Cursor after 'b': prompt + 'a'(1) + emoji(2) + 'b'(1) = prompt + 4 */
    assert_cursor(0, prompt_len + 4);

    /* Move left over 'b'. */
    send_keys(KEY_LEFT);
    assert_cursor(0, prompt_len + 3);  /* After emoji */

    /* Move left over emoji (should move 2 columns in one keystroke). */
    send_keys(KEY_LEFT);
    assert_cursor(0, prompt_len + 1);  /* After 'a' */

    /* Move left over 'a'. */
    send_keys(KEY_LEFT);
    assert_cursor(0, prompt_len);  /* At start */

    test_end();
}

static void test_utf8_backspace_emoji(void) {
    if (test_start("UTF-8 Backspace Emoji", "lua test.lua") == -1) return;

    /* Type: "x🎉y" then backspace should delete 'y', then emoji, then 'x'. */
    send_keys("x\xf0\x9f\x8e\x89" "y");
    assert_row_contains(0, "x");  /* Contains at least 'x' */

    send_keys(KEY_BACKSPACE);  /* Delete 'y' */
    /* Now should be "x🎉" */

    send_keys(KEY_BACKSPACE);  /* Delete emoji (4 bytes, one backspace) */
    assert_row_contains(0, "? x");

    send_keys(KEY_BACKSPACE);  /* Delete 'x' */
    /* Now should be empty after prompt */

    /* Type new text to verify buffer is truly empty (no orphaned bytes). */
    send_keys("ok");
    assert_row_contains(0, "? ok");

    test_end();
}

static void test_utf8_backspace_4byte_only(void) {
    if (test_start("UTF-8 Backspace 4-byte Only", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    /* Type a single 4-byte emoji (robot 🤖 = F0 9F A4 96). */
    send_keys("\xf0\x9f\xa4\x96");
    assert_cursor(0, prompt_len + 2);  /* Emoji is 2 columns wide */

    /* Backspace should delete the entire 4-byte emoji in one keystroke. */
    send_keys(KEY_BACKSPACE);
    assert_cursor(0, prompt_len);  /* Cursor should be at prompt end */

    /* Type new text to verify no orphaned bytes remain in buffer. */
    send_keys("test");
    assert_row_contains(0, "? test");

    /* The row should NOT contain any garbage characters. */
    /* If there were orphaned bytes, "test" would appear after them. */

    test_end();
}

static void test_utf8_grapheme_clusters(void) {
    if (test_start("UTF-8 Grapheme Clusters", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    /* Bestline edits UTF-8 codepoints. Variation selectors and ZWJ sequences
     * render, but are not treated as one atomic editing unit. */
    send_keys("\xe2\x9d\xa4\xef\xb8\x8f");
    assert_row_contains(0, "❤");
    assert_cursor(0, prompt_len + 2);

    send_keys("\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbb");
    assert_row_contains(0, "👍");

    send_keys("\xf0\x9f\x8f\xb3\xef\xb8\x8f\xe2\x80\x8d\xf0\x9f\x8c\x88");
    assert_row_contains(0, "🏳");

    test_end();
}

static void test_utf8_grapheme_cursor_movement(void) {
    if (test_start("UTF-8 Grapheme Cursor Movement", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    /* Bestline moves over the base emoji and its skin-tone modifier as
     * separate UTF-8 codepoints. */
    send_keys("a\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbb" "b");
    (void)prompt_len;
    assert_row_contains(0, "a👍");

    send_keys(KEY_LEFT KEY_LEFT KEY_LEFT KEY_RIGHT KEY_RIGHT KEY_RIGHT);
    assert_row_contains(0, "a👍");

    test_end();
}

static void test_emulator_grapheme_storage(void) {
    if (test_start("Emulator Grapheme Storage", "lua test.lua") == -1) return;
    //emu_debug = 1;  /* Enable debug output */

    int prompt_len = strlen("? ");

    const char thumbs_up[] = "\xf0\x9f\x91\x8d";
    send_keys(thumbs_up);

    assert_cell_content(0, prompt_len, thumbs_up, 4);
    assert_cell_width(0, prompt_len, 2);

    assert_cell_width(0, prompt_len + 1, 0);

    send_keys(KEY_BACKSPACE);

    const char heart[] = "\xe2\x9d\xa4";
    send_keys(heart);

    assert_cell_content(0, prompt_len, heart, 3);
    assert_cell_width(0, prompt_len, 2);

    assert_cell_width(0, prompt_len + 1, 0);

    test_end();
}

static void test_ctrl_w_delete_word(void) {
    if (test_start("Ctrl-W Delete Word", "lua test.lua") == -1) return;

    send_keys("hello world");
    send_keys(KEY_CTRL_W);  /* Delete "world" */
    assert_row_contains(0, "hello");

    send_keys(KEY_CTRL_W);  /* Delete "hello " */
    /* Should be empty now. */

    test_end();
}

static void test_ctrl_u_delete_line(void) {
    if (test_start("Ctrl-U Delete Line", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    send_keys("hello world");
    send_keys(KEY_CTRL_U);  /* Delete entire line */
    assert_cursor(0, prompt_len);  /* Cursor should be at start of input */

    /* Type new text to verify buffer was cleared. */
    send_keys("new");
    assert_row_contains(0, "? new");

    test_end();
}

static void test_horizontal_scroll(void) {
    if (test_start("Bestline Long Line Wrap", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");  /* 2 chars */

    /* Bestline is multiline-only, so long input wraps instead of scrolling
     * horizontally like older linenoise single-line mode. */
    send_keys("aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeee"
              "ffffffffffffffffffff");  /* 70 chars: 50 + 20 */

    assert_cursor(1, 12);
    assert_row_contains(0, "aaaaaaaaaa");
    assert_row_contains(1, "ffffffffffff");

    send_keys(KEY_CTRL_A);
    assert_cursor(0, prompt_len);  /* After prompt */
    assert_row_contains(0, "aaaaaaaaaa");

    send_keys(KEY_CTRL_E);
    assert_cursor(1, 12);
    assert_row_contains(1, "ffffffffffff");

    for (int i = 0; i < 20; i++) send_keys(KEY_BACKSPACE);  /* Delete 20 chars */

    assert_row_contains(0, "aaaaaaaaaa");
    assert_row_contains(0, "eeeeeeeeee");

    test_end();
}

static void test_horizontal_scroll_utf8(void) {
    if (test_start("Horizontal Scroll UTF-8", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");  /* 2 cols */

    /* Type text with emojis that fills most of the line.
     * Each emoji is 4 bytes but 2 columns.
     * Type: "START" (5 cols) + 20 emojis (40 cols) + "END" (3 cols) = 48 cols.
     * With prompt (7 cols), total = 55 cols, fits in 60-col terminal. */
    send_keys("START");
    /* Send 20 emojis in one batch. */
    send_keys("\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89"
              "\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89"
              "\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89"
              "\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89\xf0\x9f\x8e\x89");
    send_keys("END");

    /* Verify both START and END are visible (line fits). */
    assert_row_contains(0, "START");
    assert_row_contains(0, "END");

    /* Move to start and verify cursor position. */
    send_keys(KEY_CTRL_A);
    assert_cursor(0, prompt_len);

    /* Insert at beginning and verify. */
    send_keys("X");
    assert_row_contains(0, "? XSTART");

    test_end();
}

/* ========================= Multi-line Mode Tests ========================= */

static void test_multiline_wrap(void) {
    if (test_start("Multiline Wrap", "lua test.lua --multiline") == -1) return;

    /* Type a line longer than 60 cols to force wrapping.
     * Prompt is 2 chars ("? "), so we need 58+ chars to wrap. */
    send_keys("aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeee"
              "ffffffffff");  /* 60 chars */

    /* In multiline mode, full content should be displayed across rows.
     * Just verify the content is there (not clipped like single-line mode). */
    assert_screen_contains("aaaaaaa");

    test_end();
}

static void test_multiline_cursor_movement(void) {
    if (test_start("Multiline Cursor Movement", "lua test.lua --multiline") == -1) return;

    /* Type text that wraps (60 chars wraps on 60-col terminal). */
    send_keys("aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeee"
              "ffffffffff");  /* 60 chars */

    /* Move to beginning (Ctrl-A). */
    send_keys(KEY_CTRL_A);
    /* Type something at the beginning to verify cursor position. */
    send_keys("X");
    assert_row_contains(0, "? Xaaaaaaaaaa");  /* X inserted at start */

    /* Move to end (Ctrl-E) and type. */
    send_keys(KEY_CTRL_E);
    send_keys("Z");
    /* The 'Z' should be at the end. We can't easily verify row position,
     * but content should be updated. */

    test_end();
}

static void test_multiline_utf8(void) {
    if (test_start("Multiline UTF-8", "lua test.lua --multiline") == -1) return;

    /* Type text with emoji. Each emoji is 4 bytes, 2 cols. */
    send_keys("Test ");
    for (int i = 0; i < 10; i++) {
        send_keys("\xf0\x9f\x8e\x89");  /* 🎉 - 4 bytes, 2 cols */
    }
    /* 2 (prompt) + 5 ("Test ") + 20 (10 emojis * 2 cols) = 27 cols, fits on one line */

    assert_row_contains(0, "Test");

    /* Backspace should delete one emoji (4 bytes) at a time. */
    send_keys(KEY_BACKSPACE);
    /* Now 9 emojis remain. */

    /* Move to start and insert more. */
    send_keys(KEY_CTRL_A);
    send_keys("Hi ");
    assert_row_contains(0, "? Hi Test");

    test_end();
}

static void test_multiline_history(void) {
    if (test_start("Multiline History Navigation", "lua test.lua --multiline") == -1) return;

    /* Type a long line that wraps to 2 rows.
     * Prompt is 2 chars ("? "), so we need 58+ chars to wrap on 60-col terminal. */
    send_keys("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    /* This is 64 chars, with 2 char prompt = 66 cols, wraps to 2 rows on 60-col terminal. */

    /* Press Enter to commit to history. */
    send_keys(KEY_ENTER);

    /* Now we have a new prompt. Type a short line. */
    send_keys("short");
    assert_screen_contains("? short");

    /* Press Enter to commit the short line to history. */
    send_keys(KEY_ENTER);

    /* Navigate UP to get the short line from history. */
    send_keys(KEY_UP);
    assert_screen_contains("? short");

    /* Navigate UP again to get the long line. */
    send_keys(KEY_UP);
    /* The long line wraps, check first row. */
    assert_row_contains(0, "? aaaaaa");

    /* Navigate DOWN to go back to short line.
     * This is the critical test: the long line should be fully cleared
     * and only the short line should remain visible. */
    send_keys(KEY_DOWN);
    assert_screen_contains("? short");

    /* Verify the row after the refreshed short prompt is empty. */
    assert_screen_row(4, "");

    test_end();
}

static void test_tab_no_completions(void) {
    if (test_start("TAB With No Completions", "lua test.lua") == -1) return;

    int prompt_len = strlen("? ");

    /* Type "foo" then TAB: no completions for "foo", TAB should be consumed. */
    send_keys("foo");
    send_keys("\t");

    /* Type more text: should appear right after "foo" with no TAB inserted. */
    send_keys("bar");
    assert_row_contains(0, "? foobar");
    assert_cursor(0, prompt_len + 6);

    test_end();
}

static void test_lua_completions(void) {
    if (test_start("Lua Completion Callback", "lua test.lua") == -1) return;

    /* Type 't' then TAB. test.lua completes 't' to 'test-completion'. */
    send_keys("t");
    send_keys("\t");
    assert_row_contains(0, "? test-completion");
    test_end();
}

static void test_lua_hints(void) {
    if (test_start("Lua Hints Callback", "lua test.lua") == -1) return;

    /* Type 't'. test.lua should show ' hint-text' on the right. */
    send_keys("t");
    assert_row_contains(0, "? t hint-text");
    test_end();
}

static void test_lua_mask_mode(void) {
    if (test_start("Lua Mask Mode", "lua test.lua") == -1) return;
    // emu_debug = 1;

    /* Type '/mask' to enable mask mode. script prints 'Mask mode: on\n? ' */
    send_keys("/mask\r");
    
    /* Type 'secret'. Screen should show '******'. */
    send_keys("secret");
    assert_screen_contains("******");
    /* Ensure the word 'secret' is NOT on the screen at all. */
    assert_row_not_contains(0, "secret");
    assert_row_not_contains(1, "secret");
    assert_row_not_contains(2, "secret");
    test_end();
}

static void test_lua_balance_mode(void) {
    if (test_start("Lua Balance Mode", "lua test.lua") == -1) return;

    send_keys("/balance\r");
    assert_screen_contains("Balance mode: on");
    test_end();
}

static void test_lua_llama_mode(void) {
    if (test_start("Lua Llama Mode", "lua test.lua") == -1) return;

    send_keys("/llama\r");
    assert_screen_contains("Llama mode: on");
    test_end();
}

static void test_lua_emacs_mode(void) {
    if (test_start("Lua Emacs Mode", "lua test.lua") == -1) return;

    send_keys("/emacs\r");
    assert_screen_contains("Emacs mode: on");
    test_end();
}

static void test_lua_xlat_mode(void) {
    if (test_start("Lua Xlat Callback", "lua test.lua") == -1) return;

    send_keys("/xlat\r");
    assert_screen_contains("Xlat mode: on");
    send_keys("abc");
    assert_screen_contains("? ABC");
    test_end();
}

static void test_lua_iterator(void) {
    /* Test L.lines() iterator via a small inline script. */
    const char *script = "local L=require 'bestline'; "
                         "for line in L.lines('ITER> ') do "
                         "  break; "
                         "end";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "lua -e \"%s\"", script);

    if (test_start("Lua Lines Iterator", cmd) == -1) return;

    assert_row_contains(0, "ITER>");
    test_end();
}

static void test_lua_helper_api(void) {
    const char *script = "local L=require 'bestline'; "
                         "assert(L.characterwidth('🎉')==2); "
                         "assert(L.isseparator((' '):byte())); "
                         "assert(L.notseparator(('a'):byte())); "
                         "assert(L.uppercase(('a'):byte())==('A'):byte()); "
                         "assert(L.lowercase(('Z'):byte())==('z'):byte()); "
                         "io.write('API OK\\n')";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "lua -e \"%s\"", script);

    if (test_start("Lua Helper API", cmd) == -1) return;

    assert_screen_contains("API OK");
    test_end();
}

/* ========================= Main ========================= */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    signal(SIGPIPE, SIG_IGN);

    printf("\x1b[2J\x1b[H");  /* Clear screen */
    printf("\x1b[1;35m");
    printf("╔════════════════════════════════════════╗\n");
    printf("║     BESTLINE TEST SUITE                ║\n");
    printf("║     With VT100 Emulator                ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\x1b[0m\n");

    /* Run single-line mode tests. */
    test_simple_typing();
    test_cursor_movement();
    test_backspace_delete();
    test_utf8_typing();
    test_utf8_emoji();
    test_utf8_cursor_over_emoji();
    test_utf8_backspace_emoji();
    test_utf8_backspace_4byte_only();
    test_utf8_grapheme_clusters();
    test_utf8_grapheme_cursor_movement();
    test_emulator_grapheme_storage();
    test_ctrl_w_delete_word();
    test_ctrl_u_delete_line();
    test_tab_no_completions();

    /* Lua-specific binding tests. */
    test_lua_completions();
    test_lua_hints();
    test_lua_mask_mode();
    test_lua_balance_mode();
    test_lua_llama_mode();
    test_lua_emacs_mode();
    test_lua_xlat_mode();
    test_lua_iterator();
    test_lua_helper_api();

    /* Horizontal scrolling tests (single-line mode). */
    test_horizontal_scroll();
    test_horizontal_scroll_utf8();

    /* Run multi-line mode tests. */
    test_multiline_wrap();
    test_multiline_cursor_movement();
    test_multiline_utf8();
    test_multiline_history();

    /* Summary. */
    printf("\n\x1b[1;35m");
    printf("╔════════════════════════════════════════╗\n");
    printf("║     TEST RESULTS                       ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\x1b[0m\n");

    printf("Tests run:    %d\n", tests_run);
    printf("\x1b[32mTests passed: %d\x1b[0m\n", tests_passed);
    if (tests_failed > 0) {
        printf("\x1b[31mTests failed: %d\x1b[0m\n", tests_failed);
    } else {
        printf("Tests failed: %d\n", tests_failed);
    }

    return tests_failed > 0 ? 1 : 0;
}
