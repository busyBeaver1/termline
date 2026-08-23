#define _XOPEN_SOURCE 700 // for wcwidth from wchar.h
#define _POSIX_C_SOURCE 200112L // for pselect from sys/select.h

#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>

// POSIX:
#include <sys/select.h> // for pselect for waiting for SIGWINCH why watching a file
#include <termios.h> // for setting raw mode
#include <signal.h> // for receiving SIGWINCH on window resize
#include <fcntl.h> // for testing if there is data on stdin
#include <sys/ioctl.h>
#include <time.h>

bool _default_ts_set = false;
struct termios _default_ts;

// put terminal into raw mode
// returns 0 on success, -1 on error
int term_set_raw(FILE *in) {
    struct termios ts;
    int r = tcgetattr(fileno(in), &ts);
    if(r) return -1;
    if(!_default_ts_set) {
        _default_ts_set = true;
        _default_ts = ts;
    }
    ts.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    ts.c_oflag &= ~OPOST;
    ts.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    ts.c_cflag &= ~(CSIZE | PARENB);
    ts.c_cflag |= CS8;
    r = tcsetattr(fileno(in), 0, &ts);
    if(r) return -1;
}

int term_unset_raw(FILE *in) {
    if(!_default_ts_set) return -1;
    return tcsetattr(fileno(in), 0, &_default_ts);
}

#define TERM_CURSOR_SAVE       "\e7"
#define TERM_CURSOR_RESTORE    "\e8"
#define TERM_CURSOR_HIDE       "\e[?25l"
#define TERM_CURSOR_SHOW       "\e[?25h"
#define TERM_REQUEST_CURSOR    "\e[6n"
//#define TERM_REQUEST_SIZE      "\e[19t"
#define TERM_CURSOR_TO         "\e[%i;%iH"
#define TERM_CURSOR_RIGHT      "\e[%iC"
#define TERM_CURSOR_LEFT       "\e[%iD"
#define TERM_CLEAR_LINE_RIGHT  "\e[K"
#define TERM_CLEAR_SCREEN_DOWN "\e[J"
#define TERM_COLOR_RESET       "\e[0m"
#define TERM_COLOR_INVERSE     "\e[7m"
#define TERM_COLOR_UNINVERSE   "\e[27m"
#define TERM_CURSOR_NEWLINE    "\e[B\e[G" // does not push line unlike \r\n

// parse non-negative integer ([0-9]*) from s into x and return pointer to place right after the end of it
// 0 on empty
char *parse_uint(char *s, int *x) {
    *x = 0;
    for(; '0' <= *s && *s <= '9'; s ++) {
        int x_ = *x * 10 + (*s - '0');
        if((x_ >> 3) < *x) break; // overflow
        *x = x_;
    }
    return s;
}

typedef struct {
    int x, y; // 1-based, top->botom, left->right
    bool bouta_wrap; // set to true when the cursor is in right-most column and
                     // a printable character has just been typed into same column; in this case the terminal
                     // will wrap the line on next printable (given line wrap is enabled)
} pos_t;

typedef struct {
    char *s;
    int len, size;
} str_t;

void str_stretch(str_t *s, int size) {
    if(s->size >= size) return;
    if(s->size <= 0) s->size = 1;
    while(s->size < size) s->size *= 2;
    char *new_s = malloc(s->size);
    memcpy(new_s, s->s, s->len);
    free(s->s);
    s->s = new_s;
}

void str_printf(str_t *dst, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(dst->s + dst->len, dst->size - dst->len, format, args);
    if(dst->len + n >= dst->size) {
        str_stretch(dst, dst->len + n + 1);
        va_end(args); va_start(args, format);
        vsprintf(dst->s + dst->len, format, args);
    }
    dst->len += n;
    va_end(args);
}

void str_append(str_t *dst, const char *s, int len) {
    if(len == 0) return;
    str_stretch(dst, dst->len + len);
    memcpy(dst->s + dst->len, s, len);
    dst->len += len;
}

#define str_append_lit(dst, s_literal) str_append(dst, s_literal, sizeof(s_literal) - 1)

volatile bool _winch = false;

void _winch_handler(int) {
    _winch = true;
}

// necessery prep to call term_wait_resize_or_in
int setup_resize_watch() {
    //sigset_t mask;
    //sigemptyset(&mask);
    //sigaddset(&mask, SIGWINCH);
    //if(sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1; // block SIGWINCH not to lose it when not running term_wait_resize_or_in
    struct sigaction sa = {
        .sa_handler = _winch_handler,
        .sa_flags = SA_RESTART
    };
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGWINCH, &sa, NULL);
}

// returns -1 on error, 0 if no data avail., 1 if data is avail. for reading
int data_pending(FILE *in) {
    int infd = fileno(in);
    int options = fcntl(infd, F_GETFL);
    if(options < 0) return -1;
    if(fcntl(infd, F_SETFL, options | O_NONBLOCK) == -1) return -1;
    int c = fgetc(in);
    if(fcntl(infd, F_SETFL, options) == -1) return -1;
    if(c != EOF && ungetc(c, in) == EOF) return -1;
    return c != EOF;
}

int read_avail(FILE *in, str_t *dst) {
    int infd = fileno(in);
    char buf[128];
    int options = fcntl(infd, F_GETFL);
    if(options < 0) return -1;
    if(fcntl(infd, F_SETFL, options | O_NONBLOCK) == -1) return -1;
    read:
    int n = fread(buf, 1, sizeof(buf), in);
    if(n) { str_append(dst, buf, n); goto read; }
    if(fcntl(infd, F_SETFL, options) == -1) return -1;
    return 0;
}

// waits for an even of window resize or data on `in`
// returns 0 on resize and 1 on data and -1 on error
int term_wait_resize_or_in(FILE *in) {
    int db = data_pending(in);
    if(db) return db;
    //if(in->_IO_read_end > in->_IO_read_ptr) return 1; // gnu libc specific
    int infd = fileno(in);
    fpos_t fp;
    sigset_t mask;
    if(sigprocmask(SIG_BLOCK, NULL, &mask) < 0) return -1; // read current mask
    sigdelset(&mask, SIGWINCH); // not block SIGWINCH
    while(!_winch) {
        fd_set fs, fse;
        FD_ZERO(&fs);      FD_ZERO(&fse);
        FD_SET(infd, &fs); FD_SET(infd, &fse);
        int _errno = errno;
        int r = pselect(infd + 1, &fs, NULL, &fse, NULL, &mask); // wait for any change on `in` with `mask` as temporary signal mask
        if(r == 1 && FD_ISSET(infd, &fs) && !FD_ISSET(infd, &fse)) return 1;
        if(r == -1 && errno == EINTR) { errno = _errno; continue; }
        return -1;
    }
    //_winch = false;
    return 0;
}

// does not fill `bouta_wrap`
// sets errno to ENODATA on EOF from input and to ETIMEDOUT if answer is not received in 64 attempts
// return { .x = -1, .y = -1 } on any error
// if input_buf != NULL, all data from `in` except for the responce is appended to input_buf
pos_t term_read_pos(FILE *out, FILE *in, str_t *input_buf) {
//    if(fprintf(out, TERM_REQUEST_CURSOR) < 0) goto err_ret;
//    if(fflush(out) < 0) goto err_ret;
    // report format: "\e[{Y};{X}R"
    for(int i = 0; i < 64; i ++) {
        char buf[128];
        int bytes_read = 0;
        int c;
        while((c = fgetc(in)) != EOF) {
            buf[bytes_read ++] = (char)c;
            if(bytes_read >= sizeof buf) goto err;
            if(bytes_read == 1 && c != '\e') goto err;
            if(bytes_read == 2 && c != '[') goto err;
            if(c == 'R') goto ok;
        }
        if(c == EOF) goto nodata;
        ok:
        pos_t p;
        char *sep = parse_uint(buf + 2, &p.y);
        if(sep == buf + 2 || *sep != ';') goto err;
        char *end = parse_uint(sep + 1, &p.x);
        if(end - buf != bytes_read - 1) goto err;
        return p;
        err:
        if(input_buf) str_append(input_buf, buf, bytes_read);
    }
    errno = ETIMEDOUT;
    goto err_ret;
    nodata: errno = ENODATA;
    err_ret:
    return (pos_t){ .x = -1, .y = -1 };
}

pos_t term_get_pos(FILE *out, FILE *in, str_t *input_buf) {
    if(fprintf(out, TERM_REQUEST_CURSOR) < 0) return (pos_t){ .x = -1, .y = -1 };
    if(fflush(out) < 0) return (pos_t){ .x = -1, .y = -1 };
    return term_read_pos(out, in, input_buf);
}

int term_get_size(FILE *in, int *width, int *height) {
    struct winsize ws;
    if(ioctl(fileno(in), TIOCGWINSZ, &ws) < 0) return -1;
    *width = ws.ws_col;
    *height = ws.ws_row;
    return 0;
}

char *step_utf8_cp(char *s, int len, uint32_t *cp) {
    if((*s & 0x80) == 0) {
        if(cp) *cp = (uint32_t)(*s & 0x7F);
        return s + 1;
    }
    if((*s & 0xE0) == 0xC0) {
        if(2 > len) return s;
        if((s[1] & 0xC0) != 0x80) return s;
        if((uint8_t)*s < 0xC2) return s; // overlongs
        if(cp) *cp = (uint32_t)(*s & 0x7F) << 6 |
                     (uint32_t)(s[1] & 0x3F);
        return s + 2;
    }
    if((*s & 0xF0) == 0xE0) {
        if(3 > len) return s;
        if((s[1] & 0xC0) != 0x80 ||
           (s[2] & 0xC0) != 0x80) return s;
        if(*s == 0xE0 && (uint8_t)s[1] < 0xA0) return s; // overlongs
        if(*s == 0xED && (uint8_t)s[1] >= 0xA0) return s; // UTF-16 surrogates
        if(cp) *cp = (uint32_t)(*s & 0x7F) << 12 |
                     (uint32_t)(s[1] & 0x3F) << 6 |
                     (uint32_t)(s[2] & 0x3F);
        return s + 3;
    }
    if((*s & 0xF8) == 0xF0) {
        if(4 > len) return s;
        if((s[1] & 0xC0) != 0x80 ||
           (s[2] & 0xC0) != 0x80 ||
           (s[3] & 0xC0) != 0x80) return s;
        if(*s == 0xF0 && (uint8_t)s[1] < 0x90) return s; // overlongs
        if((uint8_t)*s > 0xF4 || (*s == 0xF4 && (uint8_t)s[1] > 0x8F)) return s; // U+10FFFF limit
        if(cp) *cp = (uint32_t)(*s & 0x7F) << 18 |
                     (uint32_t)(s[1] & 0x3F) << 12 |
                     (uint32_t)(s[2] & 0x3F) << 6 |
                     (uint32_t)(s[3] & 0x3F);
        return s + 4;
    }
    return s;
}

// cursor in a content, described in various ways
typedef struct {
    int byte; // byte number in the content's text, `byte` \in [0, t->len]
    int row; // logical row (i.e. not accounting for line wrap), 0-based
    int col; // logical column (i.e. not accounting for line wrap), 0-based
    int chr; // number of visible items (width > 0) or tabs (\t, counted even when invisible) that are to the left of the cursor in a logical line
} cursor_t;

// consumes one item from `s`, updating a bunch of state accordingly
// item is a sequence of bytes that gets displayed "at once", it is one of:
// - \t: moves cursor to next of 1,9,17,25,... columns
// - \n: translated into \r\n optionally preceded by clearing rest of line
// - \v, \f: both passed to terminal as is, both move cursor down 1 line
// - \e + "\[[0-9;]*m" (view ".." as regex): changes output appearence; passed as is and appended to color_stack
// - any other ASCII control (0-31), including \e when not postfixed with "\[[0-9;]*m": displayed as one of U+2400...U+241F ␀␁␂␃␄␅␆␇␈␉␊␋␌␍␎␏␐␑␒␓␔␕␖␗␘␙␚␛␜␝␞␟
// - printable ASCII (32-126): displayed as is
// - DEL (ASCII 127): displayed as U+2421 ␡
// - valid utf8 codepoint with non-negative wcwidth: displayed as is
// - valid utf8 codepoint with negative wcwidth: displayed as <HEX> where HEX is 4-digit or 8-digit hex of the cp;
//                                               in inverted colors; to restore colors afterwards, the whole color_stack is printed
// - a byte >=128, but not a part of a valid utf8 cp: displayed as U+FFFD �
// ARGS:
// - s: pointer to the beginning of an item
// - len: maximum length of the item (length of s)
// - p (NOT nullable): current position
// - width: terminal window width
// - split_wchars: whether printing wide character that does not fit on a line splits it into parts (e.g. on non-graphical TTY)
//                 or gets it wrapped to the next line as a whole (e.g. in all graphical terminals I tested)
// - out (nullable): where to append what shall go to the terminal
// - color_stack (nullable when out is NULL): where to append \e + "\[[0-9;]*m" sequences; used to restore color/appearence
// - cursor (nullable): a cursor to advanse accourding to the consumed item
// RETURNS pointer to right after the item
char *step_item(char *s, int len, pos_t *p, int width, bool split_wchars, str_t *out, str_t *color_stack, cursor_t *cursor) {
    int bytes;
    // == special control items: ==
    if(*s == '\t') {
        int x = ((p->x - 1) / 8) * 8 + 9;
        if(x > width) x = width;
        if(cursor) { cursor->col += x - p->x; cursor->chr ++; }
        if(x > p->x) {
            if(out) str_printf(out, TERM_CURSOR_RIGHT, x - p->x);
            p->x = x;
        }
        bytes = 1; goto ret;
    }
    if(*s == '\n') {
        if(out) {
            if(p->bouta_wrap) str_append_lit(out, "\r\n");
            else str_append_lit(out, TERM_CLEAR_LINE_RIGHT "\r\n");
        }
        p->y ++; p->x = 1; p->bouta_wrap = false;
        if(cursor) { cursor->row ++; cursor->col = 0; cursor->chr = 0; }
        bytes = 1; goto ret;
    }
    if(*s == '\v' || *s == '\f') {
        if(out) str_append(out, s, 1);
        p->y ++; p->bouta_wrap = false;
        if(cursor) { cursor->chr ++; }
        bytes = 1; goto ret;
    }
    if(*s == '\e' && len >= 3 && s[1] == '[') {
        int i;
        for(i = 2; i < len; i ++) if((s[i] < '0' || s[i] > '9') && s[i] != ';') break;
        if(i < len && s[i] == 'm') {
            if(out) str_append(out, s, i + 1);
            if(color_stack) str_append(color_stack, s, i + 1);
            bytes = i + 1; goto ret;
        }
    }
    // == normally displayed items: ==
    int w = 1; // char displayed width
    int n = 1; // number of chars outputted
    if((unsigned char)*s < 32 || *s == 127) {
        if(out) {
            str_append_lit(out, "\xE2\x90\x80");
            out->s[out->len - 1] += (*s == 127 ? 33 : *s); // ␀␁␂␃␄␅␆␇␈␉␊␋␌␍␎␏␐␑␒␓␔␕␖␗␘␙␚␛␜␝␞␟ ␡
        }
        bytes = 1; goto advanse;
    }
    if((unsigned char)*s < 128) {
        if(out) { str_stretch(out, out->len + 1); out->s[out->len ++] = *s; }
        bytes = 1; goto advanse;
    }
    uint32_t cp;
    char *s_ = step_utf8_cp(s, len, &cp);
    if(s_ == s) {
        if(out) str_append_lit(out, "\xEF\xBF\xBD"); // replacement char �
        bytes = 1; goto advanse;
    }
    w = (uint32_t)(wchar_t)cp == cp ? wcwidth(cp) : -1;
    if(w >= 0) {
        if(out) str_append(out, s, s_ - s);
    } else {
        if(out) str_printf(out, TERM_COLOR_INVERSE "<%.*" PRIX32 ">" TERM_COLOR_RESET "%.*s", cp <= 0xFFFF ? 4 : 8, cp, color_stack->len, color_stack->s);
        w = 1;
        n = cp <= 0xFFFF ? 6 : 10;
    }
    bytes = s_ - s;
    advanse:
    if(cursor && w > 0) { cursor->chr ++; cursor->col += w; }
    for(int i = 0; i < n; i ++) {
        if(p->bouta_wrap) {
            p->x = 1 + w;
            if(p->x > width) p->x = width;
            else p->bouta_wrap = false;
            p->y ++;
        } else if(p->x + w > width + 1) {
            if(split_wchars) p->x = p->x + w - width;
            else p->x = 1 + w;
            if(p->x > width) { p->x = width; p->bouta_wrap = true; }
            p->y ++;
        } else {
            p->x += w;
            if(p->x > width) { p->x = width; p->bouta_wrap = true; }
        }
    }
    ret:
    if(cursor) cursor->byte += bytes;
    return s + bytes;
}

// content of terminal (editable part starting from last prompt "origin")
// is editable by replacing everything starting at a byte (i.e. keeping a part)
// should handle resizing given no resize happens while the resize handler is running and the content fits in the window
// the origin should be saved via \e7
// apparently terminals that auto-recalculate line wrapping on resizes also auto adjust saved position (only on re-wrapping, not on wripping when writing the the terminal)
typedef struct {
    char *s;
    int len, size;
    // int *line_lengths; // visible character counts in logical lines (disregarding wrapping)
    // int n_lines; // number of logical lines
    int origin; // y position of origin
    int width, height; // of terminal window
    FILE *out; // file pointing to terminal (output only) (presumably just stdout)
    FILE *in; // file pointing to terminal (input only) (presumably just stdin)
    bool split_wchars; // behaviour of the terminal when printing a 2-wide character such that it does not fit on a line
                       // `false` means the whole thing is printed on the next line, `true` means it gets split between lines
    str_t input_buf; // when checking cursor location, other data than the responce gets put here; also in content_wait_in
    int cursor_byte; // where the cursor shall point to; in [0, len]; negative means hide cursor and put it anywhere
    int error; // gets set to CONT_ERR_* on an error; when non-zero, content is considered corrupted and all content_* funtions on it return immediately
} content_t;

#define CONT_ERR_GETPOS  1
#define CONT_ERR_GETSIZE 2
#define CONT_ERR_OUT     3
#define CONT_ERR_WAIT    4
#define CONT_ERR_SETUP   5
#define CONT_ERR_IN      6
#define CONT_ERR_TEST    7

content_t content_create() {
    content_t t = (content_t){
        .s = NULL, .len = 0, .size = 0,
        // .line_sizes = NULL, .n_lines = 0,
        .out = stdout, .in = stdin,
        .split_wchars = false, // todo: figure this out
        .input_buf = (str_t){ .s = NULL, .len = 0, .size = 0 },
        .error = 0,
        .cursor_byte = 0
    };
    if(term_get_size(t.in, &t.width, &t.height) < 0)
        { t.error = CONT_ERR_GETSIZE; return t; }
    if(fprintf(t.out, "\r" TERM_CURSOR_SAVE) < 0)
        { t.error = CONT_ERR_OUT; return t; }
    t.origin = term_get_pos(t.out, t.in, &t.input_buf).y;
    if(t.origin < 0) t.error = CONT_ERR_GETPOS;
    if(setup_resize_watch() < 0) t.error = CONT_ERR_SETUP;
    return t;
}

int ii = 1;

// render the content starting at byte `start`
// refuses to write to terminal if a resize is pending (_winch flag)
// in this case one should first call content_resize
void content_render_from(content_t *t, int start) {
    if(t->error) return;
    assert(0 <= start && start <= t->len);
    if(t->origin < 1) { t->origin = 1; start = 0; }
    pos_t pos = (pos_t){ .x = 1, .y = t->origin, .bouta_wrap = false };
    pos_t cur = (pos_t){ .x = 1, .y = t->origin, .bouta_wrap = false };
    bool got_cur = t->cursor_byte <= 0;
    str_t out = (str_t){ .s = NULL, .len = 0, .size = 0 };
    str_t cs  = (str_t){ .s = NULL, .len = 0, .size = 0 };
    char *s = t->s;
    if(start) {
        pos_t __pos = pos;
        int __cs_len = 0;
        for(;;) {
            pos_t _pos = pos;
            int _cs_len = cs.len;
            char *s_ = step_item(s, (t->s + start) - s, &pos, t->width, t->split_wchars, NULL, &cs, NULL);
            if(!got_cur && t->cursor_byte && s_ - t->s >= t->cursor_byte) { cur = pos; got_cur = true; }
            if(s_ > t->s + start) {
                pos = _pos;
                cs.len = _cs_len;
                break;
            }
            if(!pos.bouta_wrap) { __pos = pos; __cs_len = cs.len; } // saving pre-bouta_wrap situation 'cause restoring bouta_wrap flag with TERM_CURSOR_TO is impossible
            if(s_ == t->s + start) break;
        }
        pos = __pos;
        cs.len = __cs_len;
        str_append(&out, cs.s, cs.len);
        str_append_lit(&out, TERM_COLOR_RESET TERM_CURSOR_HIDE);
        str_printf(&out, TERM_CURSOR_TO, pos.y, pos.x);
    } else str_append_lit(&out, TERM_COLOR_RESET TERM_CURSOR_HIDE TERM_CURSOR_RESTORE "\r");
    bool overflow = false;
    while(s < t->s + t->len) {
        s = step_item(s, (t->s + t->len) - s, &pos, t->width, t->split_wchars, &out, &cs, NULL);
        if(!got_cur && t->cursor_byte && s - t->s >= t->cursor_byte) { cur = pos; got_cur = true; }
    }
    if(pos.bouta_wrap && pos.y < t->height)
        str_append_lit(&out, TERM_CURSOR_NEWLINE TERM_CLEAR_SCREEN_DOWN);
    if(!pos.bouta_wrap)
        str_append_lit(&out, TERM_CLEAR_SCREEN_DOWN);
    if(pos.y > t->height) {
        int lines_pushed = pos.y - t->height;
        t->origin -= lines_pushed;
        cur.y -= lines_pushed;
        str_printf(&out, TERM_CURSOR_TO TERM_CURSOR_SAVE, t->origin >= 1 ? t->origin : 1, 1);
    }
    //str_printf(&out, TERM_CURSOR_TO TERM_CURSOR_SAVE, t->origin >= 1 ? t->origin : 1, 1);
    //str_printf(&out, TERM_CURSOR_TO "[]", t->origin >= 1 ? t->origin : 1, 1);
    if(t->cursor_byte >= 0) str_printf(&out, TERM_CURSOR_TO TERM_CURSOR_SHOW, cur.y >= 1 ? cur.y : 1, cur.x);
    if(out.len) {
        if(_winch) goto ret;
        if(fwrite(out.s, 1, out.len, t->out) < 0) t->error = CONT_ERR_OUT;
        else if(fflush(t->out) < 0) t->error = CONT_ERR_OUT;
    }
    ret:
    free(out.s);
    free(cs.s);
}

// replace the rest of the content starting from byte `start` with `cont` of length `len`
// `start` should be in [0, t->len], `len` may be zero
// also attempts to render replaced part of ccontent (if no resize is pending)
void content_change(content_t *t, int start, char *cont, int len) {
    if(t->error) return;
    assert(0 <= start && start <= t->len);
    assert(len >= 0);
    t->len = start;
    str_append((str_t*)t, cont, len);
    t->cursor_byte = len;
    content_render_from(t, start);
}

void sleep_ms(long milliseconds) {
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// rerender the content assuming the terminal window has been resized
void content_resize(content_t *t) {
    _winch = false;
    if(t->error) return;
    if(term_get_size(t->in, &t->width, &t->height) < 0)
        { t->error = CONT_ERR_GETSIZE; return; }
    if(t->cursor_byte >= 0) {
        // workflow:
        // Save current cursor to `p`; then wait a little; if no winch happens; hide, restore origin, request pos, move cursor to `p`, show
        // Only then see the responce for origin pos. This way in time when the cursor is hidden we don't wait for stdin and avoid flickering
        pos_t p = term_get_pos(t->out, t->in, &t->input_buf);
        if(p.x < 0) { t->error = CONT_ERR_GETPOS; return; }
        sleep_ms(1); // makes resizing more stable on gnome terminal (and probably other terminals with auto-rewrapping)
        if(_winch) return;
        if(fprintf(t->out, TERM_CURSOR_HIDE TERM_CURSOR_RESTORE TERM_REQUEST_CURSOR TERM_CURSOR_TO TERM_CURSOR_SHOW, p.y, p.x) < 0 || fflush(t->out) < 0)
            { t->error = CONT_ERR_OUT; return; }
        t->origin = term_read_pos(t->out, t->in, &t->input_buf).y;
        if(t->origin < 0) { t->error = CONT_ERR_OUT; return; }
    } else {
        if(fprintf(t->out, TERM_CURSOR_RESTORE TERM_REQUEST_CURSOR) < 0 || fflush(t->out) < 0)
            { t->error = CONT_ERR_OUT; return; }
        t->origin = term_read_pos(t->out, t->in, &t->input_buf).y;
        if(t->origin < 0) { t->error = CONT_ERR_OUT; return; }
        sleep_ms(1); // makes resizing more stable on gnome terminal (and probably other terminals with auto-rewrapping)
    }
    content_render_from(t, 0);
}

// wait for data received on t->in, handling window resizes
// all received data gets read and put into t->input_buf
void content_wait_in(content_t *t) {
    //printf(TERM_CURSOR_TO "[%i]\r\n", 1, 1, ii ++);
    if(t->error) return;
    int r = 0;
    while(r != 1) {
        //printf(TERM_CURSOR_TO "[%i]\r\n", 1, 1, ii ++);
        r = term_wait_resize_or_in(t->in);
        //printf(TERM_CURSOR_TO "[%i]\r\n", 1, 1, ii ++);
        if(r < 0) { t->error = CONT_ERR_WAIT; return; }
        if(r == 0) content_resize(t);
    }
    if(read_avail(t->in, &t->input_buf) < 0) t->error = CONT_ERR_IN;
}

#define CURSOR_FROM_BYTE 1 // from `byte` field
#define CURSOR_FROM_COL  2 // from `row` and `col` fields
#define CURSOR_FROM_CHAR 3 // from `row` and `char` fields

// fill all fields of `dst` cursor from certain field, determined by `source`
// `source` shall be one of CURSOR_FROM_* constants
// source fiels may be invalid (e.g. out of bounds or `byte` pointing not to start of an item);
// corrected naturally in this case
void content_restore_cursor(content_t *t, cursor_t *dst, int source) {
    assert(1 <= source && source <= 3);
    cursor_t cur = { .byte = 0, .row = 0, .col = 0, .chr = 0 };
    cursor_t _cur = cur;
    pos_t pos = (pos_t){ .x = 1, .y = t->origin, .bouta_wrap = false };
    char *s = t->s;
    for(;;) {
        if(source == CURSOR_FROM_BYTE && cur.byte >= dst->byte) { *dst = cur; return; }
        if(source == CURSOR_FROM_CHAR) {
            if(cur.row > dst->row) { *dst = _cur; return; }
            if(cur.row == dst->row && cur.chr >= dst->chr) { *dst = cur; return; }
        }
        if(source == CURSOR_FROM_COL) {
            if(cur.row > dst->row) { *dst = _cur; return; }
            if(cur.row == dst->row && cur.col >= dst->col) { *dst = cur; return; }
        }
        if(s - t->s < t->len) break;
        _cur = cur;
        s = step_item(s, t->s + t->len - s, &pos, t->width, t->split_wchars, NULL, NULL, &cur);
    }
    *dst = cur;
}

