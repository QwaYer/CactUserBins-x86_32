/*
 * editor.c — ced, a full-screen nano-like text editor for CactOS.
 *
 * The console gives us raw keystrokes (there is no kernel line discipline: no
 * echo, no canonical mode, one read() per key) and understands a small ANSI
 * subset — CSI H to move, J/K to erase, m for colours, and SGR 7 reverse video,
 * which is how the bars and the block cursor are drawn.  Cursor and editing
 * keys arrive as the xterm CSI sequences the USB HID layer emits.  Nothing here
 * needs termios: the console is already in raw mode by construction.
 *
 * Layout (0-based rows):
 *   0            title bar            (reverse video)
 *   1 .. rows-3  text area, soft-wrapped at cols columns
 *   rows-2       status / message line
 *   rows-1       shortcut bar        (reverse video)
 *
 * The buffer is an array of lines, each a byte string owned by us.  Edits
 * memmove within a line and splice the line array, which is plenty for the file
 * sizes this console can usefully edit.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stat.h>
#include <errno.h>

#define ROWS_MAX 64          /* most rows we will ever draw */
#define COLS_MAX 220         /* widest line we will ever draw */
#define ROWBUF   (COLS_MAX * 10 + 64)

#define KEY_UP    0x101
#define KEY_DOWN  0x102
#define KEY_LEFT  0x103
#define KEY_RIGHT 0x104
#define KEY_HOME  0x105
#define KEY_END   0x106
#define KEY_PGUP  0x107
#define KEY_PGDN  0x108
#define KEY_DEL   0x109
#define KEY_INS   0x10A
#define KEY_ESC   0x10B
#define KEY_EOF   0x10C
#define KEY_INT   0x10D      /* synthetic: a signal (Ctrl-C) arrived */
#define KEY_NONE  0x100      /* unhandled key: ignore this event */

typedef struct {
    char *p;
    int   len;
} line_t;

static line_t *L;
static int     nlines, lcap;

static int  cy, cx;          /* cursor: line index, byte offset within it */
static int  want_cx;         /* column kept while moving vertically */
static int  top_line, top_seg;   /* first displayed (line, wrap segment) */
static int  rows = 25, cols = 80, text_w = 80, text_h = 22;
static int  overwrite, dirty;
static char fname[512];
static char msg[160];
static char needle[128];
static int  have_needle;
static char *cbuf;
static int  clen;             /* cut buffer */
static int  pending = -1;     /* one-key pushback, -1 = empty */
static volatile int got_int;  /* set by the SIGINT/SIGQUIT handler */

static char *rcache[ROWS_MAX];
static int   rcache_len[ROWS_MAX];
static int   rcache_valid;

static const char shortcuts[] =
    "^G Help ^O Save ^W Search ^T GoTo ^Q Undo ^K Cut ^U Paste ^X Exit";

static const char usage[] =
    "usage: ced [FILE]\n"
    "Full-screen text editor (a nano-like editor for the CactOS console).\n"
    "  arrow keys, Home/End, PgUp/PgDn, Delete, Insert\n"
    "  ^A/^E line start/end    ^B/^F one char left/right\n"
    "  ^P/^N one line up/down  ^V/^Y one page down/up\n"
    "  ^K cut line             ^U paste (uncut)  ^Q undo\n"
    "  ^W search               ^T go to line    ^R insert file\n"
    "  ^O save (keeps a FILE~ backup)           ^X exit\n"
    "  ^G this help            ^L redraw        ^C cursor position\n"
    "Ctrl-Z parks the editor (resume with `fg`, then ^L); Ctrl-C never drops the buffer.\n";

/* ------------------------------------------------------------------ utils */

static void out_of_memory(void) {
    write(STDERR_FILENO, "ced: out of memory\n", 19);
    exit(1);
}

static void *xmalloc(int n) {
    void *p = malloc((size_t)(n > 0 ? n : 1));
    if (!p) out_of_memory();
    return p;
}

static void *xrealloc(void *p, int n) {
    void *q = realloc(p, (size_t)(n > 0 ? n : 1));
    if (!q) out_of_memory();
    return q;
}

static int slen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static char printable(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 0x20 && u < 0x7f) ? (char)u : '?';
}

static void set_msg(const char *s) {
    int i = 0;
    for (; s[i] && i < (int)sizeof(msg) - 1; i++) msg[i] = printable(s[i]);
    msg[i] = '\0';
}

static void clear_msg(void) { msg[0] = '\0'; }

/* Write n bytes, retrying short writes.  0 on success, -1 on error. */
static int write_all_checked(int fd, const char *p, int n) {
    while (n > 0) {
        int r = (int)write(fd, p, (unsigned)n);
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    return 0;
}

static int  read_key(void);
static int  prompt_line(const char *label, char *buf, int cap);
static int  save(int force_prompt);
static void draw_plain_row(int r, const char *text);
static void clamp_cursor(void);

/* --------------------------------------------------------------- file I/O */

/* Read a whole file into a fresh buffer (NUL-terminated, *n = byte count).
 * Returns NULL when the file cannot be read. */
static char *read_all(const char *path, int *n) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;

    struct stat st;
    int size_hint = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                    ? (int)st.st_size : 0;

    int cap = (size_hint > 0) ? size_hint + 1 : 8192;
    char *b = xmalloc(cap);
    int len = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap = (cap < 4096) ? 8192 : cap * 2;
            b = xrealloc(b, cap);
        }
        int r = (int)read(fd, b + len, 4096);
        if (r < 0) { free(b); close(fd); return NULL; }
        if (r == 0) break;
        len += r;
    }
    close(fd);
    b[len] = '\0';
    *n = len;
    return b;
}

/* ---------------------------------------------------------- line helpers */

static void lines_reserve(int need) {
    if (need <= lcap) return;
    int nc = lcap ? lcap : 64;
    while (nc < need) nc *= 2;
    L = xrealloc(L, nc * (int)sizeof(line_t));
    lcap = nc;
}

static void lset(int i, const char *s, int n) {
    L[i].p = xrealloc(L[i].p, n + 1);
    if (n > 0) memcpy(L[i].p, s, (unsigned)n);
    L[i].p[n] = '\0';
    L[i].len = n;
}

static void lfree(int i) {
    free(L[i].p);
    L[i].p = NULL;
    L[i].len = 0;
}

static void linsert(int at, const char *s, int n) {
    lines_reserve(nlines + 1);
    memmove(&L[at + 1], &L[at],
            (unsigned)((nlines - at) * (int)sizeof(line_t)));
    L[at].p = NULL;
    L[at].len = 0;
    lset(at, s, n);
    nlines++;
}

static void lremove(int at) {
    lfree(at);
    memmove(&L[at], &L[at + 1],
            (unsigned)((nlines - at - 1) * (int)sizeof(line_t)));
    nlines--;
}

static void ins_char(int li, int col, char c) {
    line_t *l = &L[li];
    l->p = xrealloc(l->p, l->len + 2);
    memmove(l->p + col + 1, l->p + col, (unsigned)(l->len - col));
    l->p[col] = c;
    l->len++;
    l->p[l->len] = '\0';
}

static void del_char(int li, int col) {
    line_t *l = &L[li];
    if (col < 0 || col >= l->len) return;
    memmove(l->p + col, l->p + col + 1, (unsigned)(l->len - col - 1));
    l->len--;
    l->p[l->len] = '\0';
}

static void split_line(int li, int col) {
    line_t *l = &L[li];
    int tail = l->len - col;
    char *tmp = xmalloc(tail + 1);
    if (tail > 0) memcpy(tmp, l->p + col, (unsigned)tail);
    tmp[tail] = '\0';
    l->len = col;
    l->p[col] = '\0';
    linsert(li + 1, tmp, tail);
    free(tmp);
}

static void join_lines(int li) {      /* append line li+1 onto line li */
    line_t *a = &L[li];
    line_t *b = &L[li + 1];
    a->p = xrealloc(a->p, a->len + b->len + 1);
    if (b->len > 0) memcpy(a->p + a->len, b->p, (unsigned)b->len);
    a->len += b->len;
    a->p[a->len] = '\0';
    lremove(li + 1);
}

/* -------------------------------------------------------------------- undo */

/* Every user edit pushes one snapshot of the whole buffer, which makes undo
 * exact no matter which primitive made the change.  Consecutive edits of the
 * same kind on the same line coalesce, so typing a word is one step; the stack
 * is capped by a byte budget and drops the oldest snapshot first.  Snapshots are
 * best effort: when the allocation fails the edit simply is not undoable. */

typedef struct {
    line_t *L;
    int     nlines;
    int     cy, cx, want_cx;
    int     dirty;
    int     bytes;
} snap_t;

#define UNDO_BUDGET (8 * 1024 * 1024)
#define UNDO_MAX    256

static snap_t *ustack;
static int     ucount, ucap, ubytes;
static const char *u_kind;    /* kind of the last snapshot pushed */
static int     u_line = -1;

static void *try_malloc(int n) {
    return malloc((size_t)(n > 0 ? n : 1));
}

static void lines_free(line_t *l, int n) {
    if (!l) return;
    for (int i = 0; i < n; i++) free(l[i].p);
    free(l);
}

static line_t *lines_dup(const line_t *src, int n) {
    line_t *d = try_malloc(n * (int)sizeof(line_t));
    if (!d) return NULL;
    for (int i = 0; i < n; i++) {
        d[i].p = try_malloc(src[i].len + 1);
        if (!d[i].p) {
            lines_free(d, i);
            return NULL;
        }
        if (src[i].len > 0) memcpy(d[i].p, src[i].p, (unsigned)src[i].len);
        d[i].p[src[i].len] = '\0';
        d[i].len = src[i].len;
    }
    return d;
}

static void snap_release(snap_t *s) {
    lines_free(s->L, s->nlines);
}

static void push_undo(const char *kind) {
    if (kind == u_kind && cy == u_line) return;   /* still the same edit run */

    snap_t s;
    s.L = lines_dup(L, nlines);
    if (!s.L) { u_kind = NULL; return; }
    s.nlines = nlines;
    s.cy = cy;
    s.cx = cx;
    s.want_cx = want_cx;
    s.dirty = dirty;
    s.bytes = nlines * (int)sizeof(line_t);
    for (int i = 0; i < nlines; i++) s.bytes += L[i].len + 1;

    while (ucount > 0 &&
           (ucount >= UNDO_MAX || ubytes + s.bytes > UNDO_BUDGET)) {
        ubytes -= ustack[0].bytes;
        snap_release(&ustack[0]);
        memmove(&ustack[0], &ustack[1],
                (unsigned)((ucount - 1) * (int)sizeof(snap_t)));
        ucount--;
    }

    if (ucount == ucap) {
        int nc = ucap ? ucap * 2 : 32;
        if (nc > UNDO_MAX) nc = UNDO_MAX;
        snap_t *ns = try_malloc(nc * (int)sizeof(snap_t));
        if (!ns) { snap_release(&s); u_kind = NULL; return; }
        if (ucount > 0) memcpy(ns, ustack, (unsigned)(ucount * (int)sizeof(snap_t)));
        free(ustack);
        ustack = ns;
        ucap = nc;
    }

    ustack[ucount++] = s;
    ubytes += s.bytes;
    u_kind = kind;
    u_line = cy;
}

static void undo(void) {
    if (ucount == 0) { set_msg("Nothing to undo"); return; }

    snap_t s = ustack[--ucount];
    ubytes -= s.bytes;

    lines_free(L, nlines);
    L = s.L;
    nlines = s.nlines;
    lcap = s.nlines;          /* the copy has exactly this many slots */
    cy = s.cy;
    cx = s.cx;
    want_cx = s.want_cx;
    dirty = s.dirty;

    u_kind = NULL;            /* the next edit starts a fresh run */
    u_line = -1;
    clamp_cursor();
    set_msg("Undo");
}

/* ------------------------------------------------------------- geometry */

static int dispw(const char *s, int n) {
    int d = 0;
    for (int i = 0; i < n; i++)
        d += (s[i] == '\t') ? (8 - (d % 8)) : 1;
    return d;
}

static int disp_col_of(int li, int col) {
    return dispw(L[li].p, col);
}

static int nseg(int li) {
    return dispw(L[li].p, L[li].len) / text_w + 1;
}

/* Global display row of (line, segment): rows of every earlier line plus seg. */
static int grow(int li, int seg) {
    int r = seg;
    for (int i = 0; i < li && i < nlines; i++) r += nseg(i);
    return r;
}

/* Inverse of grow(): the (line, segment) shown on global row r. */
static void row_pos(int r, int *li, int *seg) {
    if (r < 0) r = 0;
    for (int i = 0; i < nlines; i++) {
        int s = nseg(i);
        if (r < s) { *li = i; *seg = r; return; }
        r -= s;
    }
    *li = nlines - 1;
    *seg = nseg(nlines - 1) - 1;
}

static void query_winsize(void) {
    struct winsize ws;
    int have = 0;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
        have = 1;
    }
    const char *e;
    if ((e = getenv("LINES")) != NULL && atoi(e) > 0)   { rows = atoi(e); have = 1; }
    if ((e = getenv("COLUMNS")) != NULL && atoi(e) > 0) { cols = atoi(e); have = 1; }
    if (!have) { rows = 25; cols = 80; }
    if (rows < 4) rows = 4;
    if (rows > ROWS_MAX) rows = ROWS_MAX;
    if (cols < 20) cols = 20;
    if (cols > COLS_MAX) cols = COLS_MAX;
    text_w = cols;
    text_h = rows - 3;
    if (text_h < 1) text_h = 1;
}

/* -------------------------------------------------------------- rendering */

/* One row is one write(): the console resets its colour/reverse state at the
 * start of every console_puts() chunk, so a coloured run must not be split. */
static void flush_row(int r, const char *data, int len) {
    if (rcache_valid && rcache_len[r] == len && rcache[r] != NULL &&
        memcmp(rcache[r], data, (unsigned)len) == 0)
        return;
    write(STDOUT_FILENO, data, (unsigned)len);
    rcache[r] = xrealloc(rcache[r], len + 1);
    if (len > 0) memcpy(rcache[r], data, (unsigned)len);
    rcache_len[r] = len;
}

static void invalidate_rows(void) {
    rcache_valid = 0;
}

/* Reverse-video bar.  The bottom-right cell is skipped on the last row: drawing
 * it advances the console cursor past the last line, which scrolls the screen. */
static void draw_bar(int r, const char *text) {
    char out[ROWBUF];
    int width = (r == rows - 1) ? cols - 1 : cols;
    int o = snprintf(out, sizeof(out), "\033[%d;1H\033[7m", r + 1);
    int n = 0;
    for (; text[n] && n < width; n++) out[o++] = printable(text[n]);
    while (n++ < width) out[o++] = ' ';
    out[o++] = '\033'; out[o++] = '['; out[o++] = '0'; out[o++] = 'm';
    flush_row(r, out, o);
}

static void draw_plain_row(int r, const char *text) {
    char out[ROWBUF];
    int o = snprintf(out, sizeof(out), "\033[%d;1H\033[K", r + 1);
    int width = (r == rows - 1) ? cols - 1 : cols;
    int n = 0;
    for (; text[n] && n < width; n++) out[o++] = printable(text[n]);
    flush_row(r, out, o);
}

/* Display cells of one wrap segment of a line.  Tabs become spaces at 8-column
 * stops, control bytes become '.', so every cell is exactly one column. */
static int seg_cells(int li, int seg, char *cells, int maxc) {
    const line_t *l = &L[li];
    int start = seg * text_w;
    int end = start + maxc;
    int d = 0, n = 0, i = 0;
    while (i < l->len && d < end) {
        unsigned char c = (unsigned char)l->p[i];
        int w = (c == '\t') ? (8 - (d % 8)) : 1;
        int base = d;
        d += w;
        i++;
        if (d <= start) continue;
        for (int k = 0; k < w && n < maxc; k++) {
            int col = base + k;
            if (col < start) continue;
            cells[n++] = (c == '\t') ? ' ' : printable((char)c);
        }
    }
    return n;
}

static void draw_text_row(int r, int li, int seg) {
    char cells[COLS_MAX + 1];
    int n = seg_cells(li, seg, cells, text_w);

    int cur_x = -1;
    if (li == cy) {
        int cdc = disp_col_of(cy, cx);
        if (cdc / text_w == seg) cur_x = cdc % text_w;
    }

    char out[ROWBUF];
    int o = snprintf(out, sizeof(out), "\033[%d;1H\033[K", r + 1);
    for (int x = 0; x < text_w; x++) {
        char c = (x < n) ? cells[x] : ' ';
        if (x == cur_x) {
            out[o++] = '\033'; out[o++] = '['; out[o++] = '7'; out[o++] = 'm';
            out[o++] = c;
            out[o++] = '\033'; out[o++] = '['; out[o++] = '0'; out[o++] = 'm';
        } else {
            out[o++] = c;
        }
    }
    flush_row(r, out, o);
}

static void scroll_to_cursor(int cseg) {
    if (top_line >= nlines) { top_line = nlines - 1; top_seg = 0; }
    if (top_line < 0) top_line = 0;
    if (top_seg < 0) top_seg = 0;
    if (top_seg > nseg(top_line) - 1) top_seg = nseg(top_line) - 1;

    int G = grow(cy, cseg);
    int T = grow(top_line, top_seg);

    if (G < T) {
        top_line = cy;
        top_seg = cseg;
    } else if (G >= T + text_h) {
        row_pos(G - text_h + 1, &top_line, &top_seg);
    }
}

static void build_title(char *t, int cap) {
    char base[600], right[48];
    snprintf(base, sizeof(base), "  ced %s%s",
             fname[0] ? fname : "[New Buffer]", dirty ? " *" : "");
    snprintf(right, sizeof(right), "line %d/%d", cy + 1, nlines);

    int bl = slen(base), rl = slen(right);
    int fill = cols - bl - rl - 2;
    if (fill < 1) { snprintf(t, (size_t)cap, "%s", base); return; }

    int n = 0;
    for (int i = 0; base[i] && n < cap - 1; i++) t[n++] = base[i];
    for (int i = 0; i < fill && n < cap - 1; i++) t[n++] = ' ';
    for (int i = 0; i < rl && n < cap - 1; i++) t[n++] = right[i];
    t[n] = '\0';
}

static void render(void) {
    int cseg = disp_col_of(cy, cx) / text_w;
    scroll_to_cursor(cseg);

    char title[700];
    build_title(title, (int)sizeof(title));
    draw_bar(0, title);

    int cur_row = 1, cur_col = 1;
    int li = top_line, seg = top_seg;
    for (int r = 1; r <= text_h; r++) {
        while (li < nlines && seg >= nseg(li)) { seg -= nseg(li); li++; }
        if (li >= nlines) {
            draw_plain_row(r, "");
        } else {
            if (li == cy && seg == cseg) {
                cur_row = r + 1;
                cur_col = disp_col_of(cy, cx) % text_w + 1;
            }
            draw_text_row(r, li, seg);
            seg++;
        }
    }

    draw_plain_row(rows - 2, msg);
    draw_bar(rows - 1, shortcuts);

    /* Park the console cursor on the editing position: anything the kernel or
     * the shell prints next (a log line, the "Stopped" notice on Ctrl-Z) then
     * lands here instead of at the end of the bottom bar, where it would wrap
     * and scroll the whole screen away. */
    char pos[24];
    int n = snprintf(pos, sizeof(pos), "\033[%d;%dH", cur_row, cur_col);
    write(STDOUT_FILENO, pos, (unsigned)n);
}

/* ------------------------------------------------------------- navigation */

/* Keep the cursor inside the buffer.  want_cx deliberately survives: it is the
 * column a vertical move returns to once the line is long enough again. */
static void clamp_cursor(void) {
    if (cy < 0) cy = 0;
    if (cy >= nlines) cy = nlines - 1;
    if (cx < 0) cx = 0;
    if (cx > L[cy].len) cx = L[cy].len;
}

static void move_vert(int dir) {
    cy += dir;
    clamp_cursor();
    cx = want_cx;
    clamp_cursor();
}

static void move_left(void) {
    if (cx > 0) cx--;
    else if (cy > 0) { cy--; cx = L[cy].len; }
    want_cx = cx;
}

static void move_right(void) {
    if (cx < L[cy].len) cx++;
    else if (cy < nlines - 1) { cy++; cx = 0; }
    want_cx = cx;
}

static void page(int dir) {
    cy += dir * text_h;
    clamp_cursor();
    cx = want_cx;
    clamp_cursor();
}

/* ---------------------------------------------------------------- editing */

static void type_char(char c) {
    push_undo("type");
    if (overwrite && cx < L[cy].len) del_char(cy, cx);
    ins_char(cy, cx, c);
    cx++;
    want_cx = cx;
    dirty = 1;
}

static void do_enter(void) {
    push_undo("enter");
    split_line(cy, cx);
    cy++;
    cx = 0;
    want_cx = 0;
    dirty = 1;
}

static void do_backspace(void) {
    if (cx == 0 && cy == 0) return;
    push_undo("backspace");
    if (cx > 0) {
        del_char(cy, cx - 1);
        cx--;
    } else {
        int tail = L[cy - 1].len;
        join_lines(cy - 1);
        cy--;
        cx = tail;
    }
    want_cx = cx;
    dirty = 1;
}

static void do_delete(void) {
    if (cx >= L[cy].len && cy >= nlines - 1) return;
    push_undo("delete");
    if (cx < L[cy].len) del_char(cy, cx);
    else join_lines(cy);
    dirty = 1;
}

static void cut_line(void) {
    if (nlines == 1 && L[0].len == 0) return;
    push_undo("cut");

    int add_nl = (cy < nlines - 1);
    cbuf = xrealloc(cbuf, L[cy].len + add_nl + 1);
    if (L[cy].len > 0) memcpy(cbuf, L[cy].p, (unsigned)L[cy].len);
    clen = L[cy].len;
    if (add_nl) cbuf[clen++] = '\n';

    if (nlines == 1) lset(0, "", 0);
    else {
        lremove(cy);
        if (cy >= nlines) cy = nlines - 1;
    }
    cx = 0;
    want_cx = 0;
    dirty = 1;
    set_msg("Cut line");
}

static void paste(void) {
    if (!cbuf || clen == 0) { set_msg("Cut buffer is empty"); return; }
    push_undo("paste");
    for (int i = 0; i < clen; i++) {
        if (cbuf[i] == '\n') {
            split_line(cy, cx);
            cy++;
            cx = 0;
        } else {
            ins_char(cy, cx, cbuf[i]);
            cx++;
        }
    }
    want_cx = cx;
    dirty = 1;
}

/* --------------------------------------------------------------- search */

static char lower_of(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int ci_find(const char *hay, int hn, const char *nee, int nn, int from) {
    if (nn <= 0 || nn > hn) return -1;
    for (int i = from; i + nn <= hn; i++) {
        int k = 0;
        while (k < nn && lower_of(hay[i + k]) == lower_of(nee[k])) k++;
        if (k == nn) return i;
    }
    return -1;
}

static void search(void) {
    char q[128];
    q[0] = '\0';
    if (!prompt_line("Search: ", q, (int)sizeof(q))) { clear_msg(); return; }
    if (q[0]) {
        int i = 0;
        for (; q[i] && i < (int)sizeof(needle) - 1; i++) needle[i] = printable(q[i]);
        needle[i] = '\0';
        have_needle = 1;
    }
    if (!have_needle) { set_msg("Search cancelled"); return; }

    int nn = slen(needle);
    int from = cx + 1;
    for (int li = cy; li < nlines; li++) {
        int hit = ci_find(L[li].p, L[li].len, needle, nn,
                          (li == cy) ? from : 0);
        if (hit >= 0) {
            cy = li;
            cx = hit;
            want_cx = cx;
            clear_msg();
            return;
        }
    }
    for (int li = 0; li <= cy && li < nlines; li++) {
        int hit = ci_find(L[li].p, L[li].len, needle, nn, 0);
        if (hit >= 0 && (li < cy || hit < from)) {
            cy = li;
            cx = hit;
            want_cx = cx;
            set_msg("Search wrapped");
            return;
        }
    }
    set_msg("Search failed");
}

static void goto_line(void) {
    char q[32];
    q[0] = '\0';
    if (!prompt_line("Go To Line: ", q, (int)sizeof(q))) return;
    int v = atoi(q);
    if (v < 1) v = 1;
    if (v > nlines) v = nlines;
    cy = v - 1;
    cx = 0;
    want_cx = 0;
    clear_msg();
}

static void insert_file(void) {
    char q[512];
    q[0] = '\0';
    if (!prompt_line("File to insert: ", q, (int)sizeof(q))) return;
    int n = 0;
    char *b = read_all(q, &n);
    if (!b) { set_msg("Cannot read file"); return; }
    push_undo("read");
    for (int i = 0; i < n; i++) {
        if (b[i] == '\n') {
            split_line(cy, cx);
            cy++;
            cx = 0;
        } else if (b[i] != '\r') {
            ins_char(cy, cx, b[i]);
            cx++;
        }
    }
    free(b);
    want_cx = cx;
    dirty = 1;
    set_msg("Inserted file");
}

/* ---------------------------------------------------------------- saving */

/* Copy the file we are about to overwrite to "<name>~", so a bad save is always
 * recoverable.  Best effort: a backup that cannot be written must not stop the
 * save itself.  Returns 1 when a backup was written. */
static int write_backup(const char *path) {
    int in = open(path, O_RDONLY, 0);
    if (in < 0) return 0;

    struct stat st;
    if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
        close(in);
        return 0;
    }

    char bpath[sizeof(fname) + 2];
    snprintf(bpath, sizeof(bpath), "%s~", path);
    int out = open(bpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return 0; }

    char buf[4096];
    int ok = 1, r;
    while ((r = (int)read(in, buf, sizeof(buf))) > 0) {
        if (write_all_checked(out, buf, r) != 0) { ok = 0; break; }
    }
    if (r < 0) ok = 0;
    close(out);
    close(in);
    return ok;
}

static int save(int force_prompt) {
    if (!fname[0] || force_prompt) {
        char q[512];
        q[0] = '\0';
        if (!prompt_line("File Name to Write: ", q, (int)sizeof(q))) {
            set_msg("Save cancelled");
            return 0;
        }
        if (!q[0]) { set_msg("No file name given"); return 0; }
        int i = 0;
        for (; q[i] && i < (int)sizeof(fname) - 1; i++) fname[i] = printable(q[i]);
        fname[i] = '\0';
    }

    int backed_up = write_backup(fname);

    int fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { set_msg("Cannot open file for writing"); return 0; }

    char ob[4096];
    int on = 0, bad = 0;
    for (int i = 0; i < nlines && !bad; i++) {
        if (L[i].len > 0) {
            if (L[i].len > (int)sizeof(ob)) {         /* pathological line */
                if (write_all_checked(fd, ob, on) != 0) { bad = 1; break; }
                on = 0;
                if (write_all_checked(fd, L[i].p, L[i].len) != 0) { bad = 1; break; }
            } else {
                if (on + L[i].len > (int)sizeof(ob)) {
                    if (write_all_checked(fd, ob, on) != 0) { bad = 1; break; }
                    on = 0;
                }
                memcpy(ob + on, L[i].p, (unsigned)L[i].len);
                on += L[i].len;
            }
        }
        if (on == (int)sizeof(ob)) {
            if (write_all_checked(fd, ob, on) != 0) { bad = 1; break; }
            on = 0;
        }
        ob[on++] = '\n';
    }
    if (!bad && on > 0 && write_all_checked(fd, ob, on) != 0) bad = 1;
    if (close(fd) != 0) bad = 1;

    if (bad) {
        set_msg(backed_up ? "Write failed — the old file is in <name>~"
                          : "Write failed");
        return 0;
    }
    dirty = 0;
    snprintf(msg, sizeof(msg), "Wrote %d lines%s", nlines,
             backed_up ? " (backup made)" : "");
    return 1;
}

static int do_exit(void) {
    if (!dirty) return 1;
    for (;;) {
        draw_plain_row(rows - 2,
                       "Save modified buffer?  (Y)es  (N)o  (C)ancel");
        int k = read_key();
        if (k == 'y' || k == 'Y') { if (save(0)) return 1; continue; }
        if (k == 'n' || k == 'N') return 1;
        if (k == 'c' || k == 'C' || k == KEY_ESC || k == KEY_INT) {
            clear_msg();
            return 0;
        }
    }
}

/* ---------------------------------------------------------------- prompts */

/* Draw the prompt row only: the wrapping character set is fixed, so the row
 * never needs the erase-to-end-of-line trick. */
static void draw_prompt(const char *label, const char *buf) {
    char out[ROWBUF];
    int o = snprintf(out, sizeof(out), "\033[%d;1H\033[K", rows - 1);
    int n = 0;
    for (; label[n] && n < cols - 1; n++) out[o++] = label[n];
    for (int i = 0; buf[i] && n < cols - 1; i++, n++) out[o++] = printable(buf[i]);
    flush_row(rows - 2, out, o);
}

static int prompt_line(const char *label, char *buf, int cap) {
    int n = slen(buf);
    for (;;) {
        draw_prompt(label, buf);
        int k = read_key();
        if (k == KEY_ESC || k == KEY_INT) return 0;
        if (k == KEY_EOF) return 0;
        if (k == '\r' || k == '\n') { buf[n] = '\0'; return 1; }
        if (k == 0x08 || k == 0x7f) { if (n > 0) buf[--n] = '\0'; continue; }
        if (k >= 0x20 && k < 0x7f && n < cap - 1) {
            buf[n++] = (char)k;
            buf[n] = '\0';
        }
    }
}

/* ----------------------------------------------------------------- input */

#define RD_EOF  (-1)
#define RD_INTR (-2)

/* One key byte.  A signal with a handler makes the kernel end the blocking read
 * with EINTR, which is reported here as RD_INTR: no byte was consumed, but the
 * handler has run and *got_int carries the news. */
static int read_byte(void) {
    static int idle;          /* consecutive reads that transferred nothing */

    if (pending >= 0) {
        int p = pending;
        pending = -1;
        return p;
    }
    unsigned char c;
    int r = (int)read(STDIN_FILENO, &c, 1);
    if (r == 1) { idle = 0; return c; }
    if (r == -EINTR) return RD_INTR;
    /* A terminal read that transfers nothing means "no data", not end of file:
     * an interrupted read surfaces like that too, and treating it as EOF would
     * quit the editor on Ctrl-C.  Only a real EOF/error ends the session; a
     * steady stream of zero reads would mean the descriptor is gone, so give up
     * after a while rather than spin. */
    if (r == 0) {
        if (++idle > 64) return RD_EOF;
        return RD_INTR;
    }
    return RD_EOF;
}

/* Same, but for the middle of an escape sequence, where dropping it would make
 * the rest of the sequence look like typed text. */
static int read_byte_seq(void) {
    for (;;) {
        int c = read_byte();
        if (c != RD_INTR) return c;
    }
}

static int read_key(void) {
    if (got_int) { got_int = 0; return KEY_INT; }

    int c = read_byte();
    if (c == RD_INTR) {
        if (got_int) { got_int = 0; return KEY_INT; }
        return KEY_NONE;
    }
    if (c < 0) return KEY_EOF;

    if (c != 0x1b) return c;

    int b = read_byte_seq();
    if (b < 0) return KEY_ESC;

    if (b == '[') {
        int param = 0;
        for (int guard = 0; guard < 8; guard++) {
            int d = read_byte_seq();
            if (d < 0) return KEY_ESC;
            if (d >= '0' && d <= '9') { param = param * 10 + (d - '0'); continue; }
            if (d == ';') continue;
            if (d == '~') {
                switch (param) {
                case 1: case 7: return KEY_HOME;
                case 2:         return KEY_INS;
                case 3:         return KEY_DEL;
                case 4: case 8: return KEY_END;
                case 5:         return KEY_PGUP;
                case 6:         return KEY_PGDN;
                default:        return KEY_NONE;   /* unhandled, ignore */
                }
            }
            switch (d) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            default:  return KEY_NONE;
            }
        }
        return KEY_NONE;
    }

    if (b == 'O') {
        int d = read_byte_seq();
        switch (d) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default:  return KEY_NONE;
        }
    }

    pending = b;      /* a lone ESC: put the next byte back and report Esc */
    return KEY_ESC;
}

/* Ctrl-C and Ctrl-\ are turned into SIGINT/SIGQUIT by the console.  The handler
 * only records that it happened: the kernel runs it on the way back from the
 * interrupted read(), and read_key() then reports KEY_INT so the editor can
 * show the cursor position (nano's Ctrl-C) instead of dying. */
static void on_signal(int sig) {
    (void)sig;
    got_int = 1;
}

/* ------------------------------------------------------------------ help */

static void help_screen(void) {
    static const char *help_lines[] = {
        "ced — the CactOS editor",
        "",
        "  ARROWS      move the cursor        Home/End   line start / end",
        "  PgUp/PgDn   one page up / down     Delete     delete under cursor",
        "  Insert      toggle overwrite       Enter      split the line",
        "  Backspace   delete before cursor   Tab        insert a tab stop",
        "",
        "  ^A ^E       line start / end       ^B ^F      one char left / right",
        "  ^P ^N       one line up / down     ^V ^Y      one page down / up",
        "  ^K          cut the current line   ^U         paste (uncut)",
        "  ^Q          undo the last change",
        "  ^W          search (Enter repeats) ^T         go to line",
        "  ^R          insert a file          ^O         save file",
        "  ^L          redraw the screen      ^G         this help",
        "  ^C          cursor position        ^X         exit",
        "",
        "  Saving keeps the previous contents as FILE~ (one backup generation).",
        "  Long lines wrap at the window width; a tab counts as 8 columns.",
        "  Ctrl-Z parks the editor — resume it with `fg`, then ^L to repaint.",
        "  Press any key to return to the editor.",
    };
    int n = (int)(sizeof(help_lines) / sizeof(help_lines[0]));

    write(STDOUT_FILENO, "\033[2J", 4);
    invalidate_rows();
    for (int i = 0; i < n && i + 1 < rows; i++)
        draw_plain_row(i + 1, help_lines[i]);
    for (int r = n + 1; r <= rows - 2; r++) draw_plain_row(r, "");
    draw_bar(rows - 1, "  ced help — press any key");

    read_key();
    write(STDOUT_FILENO, "\033[2J", 4);
    invalidate_rows();
}

/* ------------------------------------------------------------------ main */

int cact_ub_ced(char **argv, int argc) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        write(STDOUT_FILENO, usage, sizeof(usage) - 1);
        return 0;
    }
    if (argc > 2) {
        write(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGQUIT, on_signal);

    query_winsize();
    lines_reserve(1);

    if (argc == 2) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) {
            fprintf(stderr, "ced: %s: is a directory\n", argv[1]);
            return 1;
        }
        int i = 0;
        for (; argv[1][i] && i < (int)sizeof(fname) - 1; i++)
            fname[i] = printable(argv[1][i]);
        fname[i] = '\0';

        int n = 0;
        char *b = read_all(fname, &n);
        if (b) {
            int i2 = 0;
            for (;;) {
                int j = i2;
                while (j < n && b[j] != '\n') j++;
                int len = j - i2;
                if (len > 0 && b[i2 + len - 1] == '\r') len--;
                linsert(nlines, b + i2, len);
                if (j >= n) break;
                i2 = j + 1;
                if (i2 >= n) break;
            }
            free(b);
        } else {
            set_msg("New file");
        }
    }
    if (nlines == 0) linsert(0, "", 0);

    cy = cx = want_cx = 0;
    top_line = top_seg = 0;

    write(STDOUT_FILENO, "\033[2J", 4);
    invalidate_rows();
    render();

    for (;;) {
        int k = read_key();
        if (k == KEY_EOF) break;

        /* Anything that is not a plain edit ends the current undo run, so a
         * cursor move between two bursts of typing keeps them separate steps. */
        if (!((k >= 0x20 && k < 0x7f) || k == 0x09 || k == 0x0d || k == 0x0a ||
              k == 0x08 || k == 0x7f || k == KEY_DEL || k == 0x04))
            u_kind = NULL;

        switch (k) {
        case KEY_UP:    move_vert(-1); clear_msg(); break;
        case KEY_DOWN:  move_vert(1);  clear_msg(); break;
        case KEY_LEFT:  move_left();  break;
        case KEY_RIGHT: move_right(); break;
        case KEY_HOME:  cx = 0; want_cx = 0; break;
        case KEY_END:   cx = L[cy].len; want_cx = cx; break;
        case KEY_PGUP:  page(-1); break;
        case KEY_PGDN:  page(1);  break;
        case KEY_DEL:   do_delete(); break;
        case KEY_INS:   overwrite = !overwrite;
                        set_msg(overwrite ? "Overwrite mode" : "Insert mode");
                        break;
        case KEY_ESC:   clear_msg(); break;
        case KEY_INT: {                                          /* ^C */
            char b[80];
            snprintf(b, sizeof(b), "line %d/%d  col %d  char %d",
                     cy + 1, nlines, disp_col_of(cy, cx) + 1, cx);
            set_msg(b);
            break;
        }
        case 0x09: type_char('\t'); clear_msg(); break;
        case 0x0d:
        case 0x0a: do_enter(); clear_msg(); break;
        case 0x08:
        case 0x7f: do_backspace(); clear_msg(); break;
        case 0x01: cx = 0; want_cx = 0; break;                 /* ^A */
        case 0x02: move_left(); break;                          /* ^B */
        case 0x04: do_delete(); break;                          /* ^D */
        case 0x05: cx = L[cy].len; want_cx = cx; break;          /* ^E */
        case 0x06: move_right(); break;                          /* ^F */
        case 0x07: help_screen(); break;                         /* ^G */
        case 0x0b: cut_line(); break;                            /* ^K */
        case 0x0c: write(STDOUT_FILENO, "\033[2J", 4);            /* ^L */
                   query_winsize(); invalidate_rows(); clear_msg(); break;
        case 0x0e: move_vert(1);  clear_msg(); break;             /* ^N */
        case 0x0f: save(0); break;                              /* ^O */
        case 0x10: move_vert(-1); clear_msg(); break;             /* ^P */
        case 0x11: undo(); break;                               /* ^Q */
        case 0x12: insert_file(); break;                        /* ^R */
        case 0x13: save(0); break;                              /* ^S */
        case 0x14: goto_line(); break;                          /* ^T */
        case 0x15: paste(); break;                              /* ^U */
        case 0x16: page(1);  clear_msg(); break;                  /* ^V */
        case 0x17: search(); break;                             /* ^W */
        case 0x18: if (do_exit()) goto done;                    /* ^X */
                   break;
        case 0x19: page(-1); clear_msg(); break;                  /* ^Y */
        default:
            if (k >= 0x20 && k < 0x7f) { type_char((char)k); clear_msg(); }
            break;
        }

        clamp_cursor();
        render();
    }

done:
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
    return 0;
}
