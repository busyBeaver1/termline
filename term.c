/*
 * Pefixes: term_*    - terminal-related utilities
 *          tu_*      - general utilities
 *          content_* - functions on content_t
 *          tl_*      - functions on termline_t
 *          KEY_*     - non-printable key representations
 *          MOD_*     - key modifier bits (for CTRL/ALT/SHIFT/nompad)
 *          LHREC_*   - inline history (for undo/redo) record types
 */

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

FILE *debug;
#define DEBUG(...) do { fprintf(debug, __VA_ARGS__); fflush(debug); } while(0)

#define TU_UNIX    1
#define TU_WINDOWS 2

#if defined _WIN32 || defined _WIN64
#define TU_SYSTEM TU_WINDOWS
#elif defined __unix__ || defined __APPLE__
#define TU_SYSTEM TU_UNIX
#else
#error Unknown system
#endif

#if TU_SYSTEM == TU_UNIX
// POSIX:
#include <sys/select.h> // for pselect for waiting for SIGWINCH why watching a file
#include <termios.h> // for setting raw mode
#include <signal.h> // for receiving SIGWINCH on window resize
#include <fcntl.h> // for testing if there is data on stdin
#include <sys/ioctl.h>
#include <time.h>
#else
#include <windows.h>
#endif

bool tu_default_ts_set = false;

#if TU_SYSTEM == TU_UNIX
struct termios tu_default_ts;

// put terminal into raw mode
// returns 0 on success, -1 on error
// unly `in` is used, `out` kept for compat. with windows version
int term_set_raw(FILE *in, FILE *out) {
    int infd = fileno(in);
    if(infd < 0) return -1;
    struct termios ts = tu_default_ts;
    if(!tu_default_ts_set) {
        if(tcgetattr(infd, &ts)) return -1;
        tu_default_ts_set = true;
        tu_default_ts = ts;
    }
    ts.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    ts.c_oflag &= ~OPOST;
    ts.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    ts.c_cflag &= ~(CSIZE | PARENB);
    ts.c_cflag |= CS8;
    if(tcsetattr(infd, 0, &ts)) return -1;
    return 0;
}

// unly `in` is used, `out` kept for compat. with windows version
int term_unset_raw(FILE *in, FILE *out) {
    if(!tu_default_ts_set) return -1;
    return tcsetattr(fileno(in), 0, &tu_default_ts);
}
#else
DWORD tu_default_ts_in;
DWORD tu_default_ts_out;

HANDLE tu_file2handle(FILE *f) {
    int fd = _fileno(f);
    if(fd == -1) return INVALID_HANDLE_VALUE;
    return (HANDLE)(intptr_t)_get_osfhandle(fd);
}

int term_set_raw(FILE *in, FILE *out) {
    HANDLE inhd = tu_file2handle(in);
    if(inhd == INVALID_HANDLE_VALUE) return -1;
    HANDLE outhd = tu_file2handle(out);
    if(outhd == INVALID_HANDLE_VALUE) return -1;
    DWORD ints = tu_default_ts_in;
    DWORD outts = tu_default_ts_out;
    if(!tu_default_ts_set) {
        if(!GetConsoleMode(inhd, &ints)) return -1;
        if(!GetConsoleMode(outhd, &outts)) return -1;
        tu_default_ts_set = true;
        tu_default_ts_in = ints;
        tu_default_ts_out = outts;
    }
    ints &= ~(ENABLE_ECHO_INPUT           | ENABLE_INSERT_MODE        | ENABLE_MOUSE_INPUT                 |
              ENABLE_PROCESSED_INPUT      | ENABLE_QUICK_EDIT_MODE);
    ints |=   ENABLE_EXTENDED_FLAGS       | ENABLE_WINDOW_INPUT       | ENABLE_VIRTUAL_TERMINAL_INPUT;
    outts |=  ENABLE_PROCESSED_OUTPUT     | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
              DISABLE_NEWLINE_AUTO_RETURN;
    if(!SetConsoleMode(inhd, ints)) return -1;
    if(!SetConsoleMode(outhd, outts)) return -1;
    return 0;
}

int term_unset_raw(FILE *in, FILE *out) {
    if(!tu_default_ts_set) return -1;
    HANDLE inhd = tu_file2handle(in);
    if(inhd == INVALID_HANDLE_VALUE) return -1;
    HANDLE outhd = tu_file2handle(out);
    if(outhd == INVALID_HANDLE_VALUE) return -1;
    if(!SetConsoleMode(inhd, tu_default_ts_in)) return -1;
    if(!SetConsoleMode(outhd, tu_default_ts_out)) return -1;
    return 0;
}
#endif

#define TERM_CURSOR_SAVE          "\0337"
#define TERM_CURSOR_RESTORE       "\0338"
#define TERM_CURSOR_HIDE          "\33[?25l"
#define TERM_CURSOR_SHOW          "\33[?25h"
#define TERM_REQUEST_CURSOR       "\33[6n"
//#define TERM_REQUEST_SIZE         "\33[19t"
#define TERM_CURSOR_TO            "\33[%i;%iH"
#define TERM_CURSOR_RIGHT         "\33[%iC"
#define TERM_CURSOR_LEFT          "\33[%iD"
#define TERM_CLEAR_LINE_RIGHT     "\33[K"
#define TERM_CLEAR_LINE_LEFT      "\33[1K"
#define TERM_CLEAR_SCREEN_DOWN    "\33[J"
#define TERM_COLOR_RESET          "\33[0m"
#define TERM_COLOR_UNDERLINE      "\33[4m"
#define TERM_COLOR_INVERSE        "\33[7m"
#define TERM_COLOR_UNINVERSE      "\33[27m"
#define TERM_COLOR_RED_BACKGROUND "\33[41m"
#define TERM_CURSOR_NEWLINE       "\33[B\33[G" // does not push line unlike \r\n

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

#define tu_implement_arr(prefix, type)                       \
typedef struct { type *p; int len, cap; } prefix##_t;       \
                                                             \
void prefix##_stretch(prefix##_t *arr, int cap) {           \
    if(arr->cap >= cap) return;                            \
    if(arr->cap <= 0) arr->cap = 64;                       \
    while(arr->cap < cap) arr->cap *= 2;                  \
    type *new_p = malloc(arr->cap * sizeof(type));          \
    if(arr->p) {                                             \
        memcpy(new_p, arr->p, arr->len * sizeof(type));      \
        free(arr->p);                                        \
    }                                                        \
    arr->p = new_p;                                          \
}                                                            \
                                                             \
void prefix##_append(prefix##_t *arr, type *cont, int len) { \
    prefix##_stretch(arr, arr->len + len);                   \
    memcpy(arr->p + arr->len, cont, len * sizeof(type));     \
    arr->len += len;                                         \
}

typedef struct {
    int x, y; // 1-based, top->botom, left->right
    bool bouta_wrap; // set to true when the cursor is in right-most column and
                     // a printable character has just been typed into same column; in this case the terminal
                     // will wrap the line on next printable (given line wrap is enabled)
} pos_t;

typedef struct {
    char *s;
    int len, cap;
} str_t;

tu_implement_arr(str_arr, str_t)

void str_stretch(str_t *s, int cap) {
    if(s->cap >= cap) return;
    if(s->cap <= 0) s->cap = 64;
    while(s->cap < cap) s->cap *= 2;
    char *new_s = malloc(s->cap);
    if(s->s) {
        memcpy(new_s, s->s, s->len);
        free(s->s);
    }
    s->s = new_s;
}

void str_printf(str_t *dst, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = vsnprintf(dst->s + dst->len, dst->cap - dst->len, format, args);
    if(dst->len + n >= dst->cap) {
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

// allows out of bounds, returns a view that can be turned into a self-allocated string with str_sovereign
str_t str_substr(str_t *s, int at, int len) {
    if(at + len < 0 || at > s->len) return (str_t){ NULL };
    if(at < 0) { len += at; at = 0; }
    if(at + len >= s->len) len = s->len - at;
    return (str_t){ .s = s->s + at, .len = len, .cap = 0 };
}

void str_sovereign(str_t *s) {
    if(s->len == 0) { s->cap = 0; s->s = NULL; return; }
    s->cap = 64;
    while(s->cap < s->len) s->cap *= 2;
    char *new_s = malloc(s->cap);
    memcpy(new_s, s->s, s->len);
    s->s = new_s;
}

#define str_append_lit(dst, s_literal) str_append(dst, s_literal, sizeof(s_literal) - 1)

volatile bool tu_winch = false;

#if TU_SYSTEM == TU_WINDOWS
// only `out` is used, `in` kept for compat. with unix version
int term_get_size(FILE *in, FILE *out, int *width, int *height) {
    int outfd = _fileno(out);
    if(outfd == -1) return -1;
    HANDLE outhd = (HANDLE)(intptr_t)_get_osfhandle(outfd);
    if(outhd == INVALID_HANDLE_VALUE) return -1;
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(!GetConsoleScreenBufferInfo(outhd, &info)) return -1;
    if(width) *width = info.dwSize.X;
    if(height) *height = info.dwSize.Y;
    return 0;
}
#else
// only `in` is used, `out` kept for compat. with windows version
int term_get_size(FILE *in, FILE *out, int *width, int *height) {
    struct winsize ws;
    if(ioctl(fileno(in), TIOCGWINSZ, &ws) < 0) return -1;
    if(width) *width = ws.ws_col;
    if(height) *height = ws.ws_row;
    return 0;
}
#endif

#if TU_SYSTEM == TU_UNIX

int term_check_resize(FILE *in, FILE *out, int *w, int *h) {
    if(!tu_winch) return 0;
    tu_winch = false;
    if(term_get_size(in, out, w, h) < 0) return -1;
    return 1;
}

void tu_winch_handler(int) { tu_winch = true; }

// necessery prep to call term_wait_resize_or_in
int setup_resize_watch() {
    //sigset_t mask;
    //sigemptyset(&mask);
    //sigaddset(&mask, SIGWINCH);
    //if(sigprocmask(SIG_BLOCK, &mask, NULL) < 0) return -1; // block SIGWINCH not to lose it when not running term_wait_resize_or_in
    struct sigaction sa = {
        .sa_handler = tu_winch_handler,
        .sa_flags = SA_RESTART
    };
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGWINCH, &sa, NULL);
}

/*
// returns -1 on error, 0 if no data avail., 1 if data is avail. for reading
int _data_pending(FILE *in) {
    int infd = fileno(in);
    int options = fcntl(infd, F_GETFL);
    if(options < 0) return -1;
    if(fcntl(infd, F_SETFL, options | O_NONBLOCK) == -1) return -1;
    int c = fgetc(in);
    if(fcntl(infd, F_SETFL, options) == -1) return -1;
    if(c != EOF && ungetc(c, in) == EOF) return -1;
    return c != EOF;
}
*/

int tu_read_avail(FILE *in, str_t *dst) {
    int infd = fileno(in);
    int options = fcntl(infd, F_GETFL);
    if(options < 0) return -1;
    if(fcntl(infd, F_SETFL, options | O_NONBLOCK) == -1) return -1;
    int N = 0;
    read:
    char buf[128];
    int n = fread(buf, 1, sizeof(buf), in);
    if(n) { N += n; str_append(dst, buf, n); goto read; }
    if(fcntl(infd, F_SETFL, options) == -1) return -1;
    return N;
}

struct timespec tu_ms2ts(unsigned long t) {
    return (struct timespec){
        .tv_sec = t / 1000,
        .tv_nsec = (t % 1000) * 1000000
    };
}

int term_wait_resize_or_in(FILE *in, str_t *input_buf, unsigned long timeout, int *width, int *height) {
    int rb = tu_read_avail(in, input_buf);
    if(rb) return 0;
    int infd = fileno(in);
    sigset_t mask;
    if(sigprocmask(SIG_BLOCK, NULL, &mask) < 0) return -1; // read current mask
    sigdelset(&mask, SIGWINCH); // not block SIGWINCH
    while(!tu_winch) {
        fd_set fs, fse;
        FD_ZERO(&fs);      FD_ZERO(&fse);
        FD_SET(infd, &fs); FD_SET(infd, &fse);
        struct timespec ts = tu_ms2ts(timeout);
        int _errno = errno; errno = 0;
        int r = pselect(infd + 1, &fs, NULL, &fse, timeout >= 0 ? &ts : NULL, &mask); // wait for any change on `in` with `mask` as temporary signal mask
        int e = errno; errno = _errno;
        if(r == 0) return 0;
        if(r == 1 && FD_ISSET(infd, &fs) && !FD_ISSET(infd, &fse)) {
            int rb = tu_read_avail(in, input_buf);
            return rb == 0 ? -1 : 0;
        }
        if(r == -1 && e == EINTR) continue;
        return -1;
    }
    tu_winch = false;
    term_get_size(in, NULL, width, height);
    return 1;
}

int tu_gets(FILE *in, char *dst)  {
    int c = fgetc(in);
    if(c == EOF) return 0;
    *dst = (char)c;
    return 1;
}

int tu_fwrite_utf8(char *s, int len, FILE *out) { return fwrite(s, 1, len, out); }

#else

typedef struct {
    wchar_t *s;
    int len, cap;
} wstr_t;

void wstr_stretch(wstr_t *s, int cap) {
    if(s->cap >= cap) return;
    if(s->cap <= 0) s->cap = 1;
    while(s->cap < cap) s->cap *= 2;
    wchar_t *new_s = malloc(s->cap * sizeof(wchar_t));
    memcpy(new_s, s->s, s->len * sizeof(wchar_t));
    free(s->s);
    s->s = new_s;
}

void wstr_push(wstr_t *s, wchar_t c) {
    wstr_stretch(s, s->len + 1);
    s->s[s->len ++] = c;
}

int tu_encode_wchars(str_t *dst, wchar_t *s, int len) {
    if(len == 0) return 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    if(n == 0) return -1;
    str_stretch(dst, dst->len + n);
    if(WideCharToMultiByte(CP_UTF8, 0, s, len, dst->s + dst->len, n, NULL, NULL) != n)
        return -1;
    dst->len += n;
    return 0;
}

int term_wait_resize_or_in(FILE *in, str_t *input_buf, unsigned long timeout, int *w, int *h, wchar_t *high_surrogate) {
    HANDLE inhd = tu_file2handle(in);
    if(inhd == INVALID_HANDLE_VALUE) return -1;
    bool got_resize = false;
    for(;;) {
        int r = WaitForSingleObject(inhd, timeout >= 0 ? timeout : INFINITE);
        if(r == WAIT_TIMEOUT) return 0;
        if(r != WAIT_OBJECT_0) return -1;
        DWORD n;
        if(!GetNumberOfConsoleInputEvents(inhd, &n)) return -1;
        if(n <= 0) return -1;
        INPUT_RECORD *irs = malloc(sizeof(INPUT_RECORD) * n);
        //wchar_t *s = malloc(sizeof(wchar_t) * (n + (*high_surrogate != 0)));
        wstr_t s = { .s = NULL, .len = 0, .cap = 0 };
        if(high_surrogate && *high_surrogate) wstr_push(&s, *high_surrogate);
        DWORD n_;
        if(!ReadConsoleInputW(inhd, irs, n, &n_)) goto err;
        if(n_ != n) goto err;
        for(int i = 0; i < n; i ++) {
            if(irs[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                got_resize = true;
                COORD cd = irs[i].Event.WindowBufferSizeEvent.dwSize;
                if(w) *w = cd.X;
                if(h) *h = cd.Y;
            } else if(irs[i].EventType == KEY_EVENT) {
                KEY_EVENT_RECORD ke = irs[i].Event.KeyEvent;
                if(!ke.bKeyDown || ke.uChar.UnicodeChar == 0) continue;
                for(int j = 0; j < ke.wRepeatCount; j ++)
                    wstr_push(&s, ke.uChar.UnicodeChar);
            }
        }
        if(high_surrogate) {
            if(s.len && 0xD800 <= s.s[s.len - 1] && s.s[s.len - 1] < 0xDC00)
                *high_surrogate = s.s[-- s.len];
            else *high_surrogate = 0;
        }
        if(input_buf && tu_encode_wchars(input_buf, s.s, s.len) < 0) goto err;
        bool err = false;
        if(0) { err: err = true; }
        free(s.s); free(irs);
        if(err) return -1;
        if(s.len || got_resize) return got_resize;
    }
}

int term_check_resize(FILE *in, FILE *out, int *w, int *h) {
    int x, y;
    if(term_get_size(in, out, &x, &y) < 0) return -1;
    bool r = (w && *w != x) || (h && *h != y);
    if(w) *w = x;
    if(h) *h = y;
    return r;
}

int tu_gets(FILE *in, char *dst) {
    HANDLE inhd = tu_file2handle(in);
    if(inhd == INVALID_HANDLE_VALUE) return 0;
    wchar_t ws[2];
    INPUT_RECORD ir1, ir2;
    DWORD n;
    read1:
    if(!ReadConsoleInputW(inhd, &ir1, 1, &n) || n == 0) return 0;
    if(ir1.EventType != KEY_EVENT) goto read1;
    KEY_EVENT_RECORD ke = ir1.Event.KeyEvent;
    if(!ke.bKeyDown || ke.uChar.UnicodeChar == 0) goto read1;
    ws[0] = ke.uChar.UnicodeChar;
    int N = 1;
    if(0xD800 <= ws[0] && ws[0] < 0xDC00) { // got high surogate, wsaiting for lows counterpart
        read2:
        WaitForSingleObject(inhd, INFINITE);
        if(!PeekConsoleInputW(inhd, &ir2, 1, &n) || n == 0) return 0;
        if(ir2.EventType != KEY_EVENT) goto read2;
        KEY_EVENT_RECORD ke = ir2.Event.KeyEvent;
        if(!ke.bKeyDown || ke.uChar.UnicodeChar == 0) goto read2;
        ws[1] = ke.uChar.UnicodeChar;
        if(!(0xD800 <= ws[1] && ws[1] < 0xDC00)) { // if got not a high surrogate, read it, otherwsise leave as is
            if(!ReadConsoleInputW(inhd, &ir2, 1, &n) || n == 0) return 0;
            N = 2;
        }
    }
    return WideCharToMultiByte(CP_UTF8, 0, ws, N, dst, 8, NULL, NULL);
}

int tu_fwrite_utf8(char *s, int len, FILE *out) {
    if(len == 0) return 0;
    HANDLE outhd = tu_file2handle(out);
    if(outhd == INVALID_HANDLE_VALUE) return -1;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    if(n == 0) return -1;
    wchar_t *S = malloc(n * sizeof(wchar_t));
    if(MultiByteToWideChar(CP_UTF8, 0, s, len, S, n) != n) goto err;
    if(!WriteConsoleW(outhd, S, n, NULL, NULL)) goto err;
    int r = 0; if(0) { err: r = -1; }
    free(S);
    return r;
}

#endif
//#if 0
// does not fill `bouta_wrap`
// sets errno to ENODATA on EOF from input and to ETIMEDOUT if answer is not received in 64 attempts
// return { .x = -1, .y = -1 } on any error
// if input_buf != NULL, all data from `in` except for the responce is appended to input_buf
pos_t term_read_pos(FILE *out, FILE *in, str_t *input_buf) {
//    if(fprintf(out, TERM_REQUEST_CURSOR) < 0) goto err_ret;
//    if(fflush(out) < 0) goto err_ret;
    // report format: "\e[{Y};{X}R"
    char buf[128];
    int bytes_read = 0;
    for(int i = 0; i < 64; i ++) {
        int k;
        while(k = tu_gets(in, buf + bytes_read)) {
            bytes_read += k;
            for(int j = bytes_read - k; j < bytes_read; j ++) {
                if(j >= 2 && buf[j] == 'R') goto ok;
                if(j >= sizeof buf - 8 ||
                   j == 0 && buf[j] != '\33' ||
                   j == 1 && buf[j] != '[' ||
                   j >= 2 && (buf[j] < '0' || buf[j] > '9') && buf[j] != ';') {
                    if(input_buf) str_append(input_buf, buf, j);
                    memmove(buf, buf + j, bytes_read - j);
                    bytes_read -= j;
                    goto err;
                }
            }
        }
        if(k == 0) goto nodata;
        ok:
        pos_t p;
        char *sep = parse_uint(buf + 2, &p.y);
        if(sep == buf + 2 || *sep != ';') goto err;
        char *end = parse_uint(sep + 1, &p.x);
        if(end - buf != bytes_read - 1) goto err;
        return p;
        err:;
    }
    if(input_buf) str_append(input_buf, buf, bytes_read);
    errno = ETIMEDOUT;
    goto err_ret;
    nodata: errno = ENODATA;
    err_ret:
    return (pos_t){ .x = -1, .y = -1 };
}

pos_t term_get_pos(FILE *out, FILE *in, str_t *input_buf) {
    if(fprintf(out, TERM_REQUEST_CURSOR) < 0 ||
       fflush(out) < 0) return (pos_t){ .x = -1, .y = -1 };
    return term_read_pos(out, in, input_buf);
}

char *step_utf8_cp(char *_s, int len, uint32_t *cp) {
    unsigned char *s = (unsigned char*)_s;
    if((*s & 0x80) == 0) {
        if(cp) *cp = (uint32_t)(*s & 0x7F);
        return _s + 1;
    }
    if((*s & 0xE0) == 0xC0) {
        if(2 > len) return _s;
        if((s[1] & 0xC0) != 0x80) return _s;
        if(*s < 0xC2) return _s; // overlongs
        if(cp) *cp = (uint32_t)(*s & 0x3F) << 6 |
                     (uint32_t)(s[1] & 0x3F);
        return _s + 2;
    }
    if((*s & 0xF0) == 0xE0) {
        if(3 > len) return _s;
        if((s[1] & 0xC0) != 0x80 ||
           (s[2] & 0xC0) != 0x80) return _s;
        if(*s == 0xE0 && s[1] < 0xA0) return _s; // overlongs
        if(*s == 0xED && s[1] >= 0xA0) return _s; // UTF-16 surrogates
        if(cp) *cp = (uint32_t)(*s & 0x1F) << 12 |
                     (uint32_t)(s[1] & 0x3F) << 6 |
                     (uint32_t)(s[2] & 0x3F);
        return _s + 3;
    }
    if((*s & 0xF8) == 0xF0) {
        if(4 > len) return _s;
        if((s[1] & 0xC0) != 0x80 ||
           (s[2] & 0xC0) != 0x80 ||
           (s[3] & 0xC0) != 0x80) return _s;
        if(*s == 0xF0 && s[1] < 0x90) return _s; // overlongs
        if(*s > 0xF4 || (*s == 0xF4 && s[1] > 0x8F)) return _s; // U+10FFFF limit
        if(cp) *cp = (uint32_t)(*s & 0x0F) << 18 |
                     (uint32_t)(s[1] & 0x3F) << 12 |
                     (uint32_t)(s[2] & 0x3F) << 6 |
                     (uint32_t)(s[3] & 0x3F);
        return _s + 4;
    }
    return _s;
}

char *step_utf8_cp_partial(char *_s, int len, uint32_t *cp) {
    unsigned char *s = (unsigned char*)_s;
    char template[4] = "\x00\x80\x80\x80";
    if(*s == 0xE0) template[1] = 0xA0;
    if(*s == 0xF0) template[1] = 0x90;
    memcpy(template, _s, len > 4 ? 4 : len);
//    printf("\r\n%i {%s} %i %i %i\r\n", len, template, template[0], template[1], step_utf8_cp(template, len, cp) - template);
    return step_utf8_cp(template, 4, cp) - template + _s;
}

char *write_utf8_cp(char *dst, uint32_t c) {
    assert(c < 0x110000);
    if(c < 0x80) {
        *dst = (char)c;
        return dst + 1;
    }
    if(c < 0x800) {
        *dst = 0xC0 | c >> 6; dst[1] = 0x80 | c & 0x3F;
        return dst + 2;
    }
    if(c < 0x10000) {
        *dst = 0xE0 | c >> 12; dst[1] = 0x80 | c >> 6 & 0x3F; dst[2] = 0x80 | c & 0x3F;
        return dst + 3;
    }
    *dst = 0xF0 | c >> 24; dst[1] = 0x80 | c >> 12 & 0x3F; dst[2] = 0x80 | c >> 6 & 0x3F; dst[3] = 0x80 | c & 0x3F;
    return dst + 4;
}

// cursor in a content, described in various ways
typedef struct {
    int byte; // byte number in the content's text, `byte` \in [0, t->len]
    int row; // logical row (i.e. not accounting for line wrap), 0-based
    int col; // logical column (i.e. not accounting for line wrap), 0-based
    int cp; // number of visible items (width > 0) or tabs (\t, counted even when invisible) that are to the left of the cursor in a logical line
    int row_byte;
} cursor_t;

int tu_binary_search(const uint32_t *indices, int len, uint32_t c) {
    int l = 0;
    int u = len - 1;
    if(indices[u] <= c) return u;
    while(u - l > 1) {
        int m = (u + l) / 2;
        if(indices[m] <= c) l = m;
        else u = m;
    }
    return l;
}

// copying python's builtin .upper and .lower
const uint32_t tu_lower_indices_even[] = {
0,66,92,192,224,256,304,306,312,330,376,378,386,390,392,394,396,398,400,402,404,406,408,410,412,414,416,422,424,428,430,
432,434,436,440,442,444,446,452,454,456,458,460,478,496,498,502,504,544,546,564,570,572,574,576,580,582,592,880,884,886,
888,902,904,908,910,912,914,930,932,940,984,1008,1012,1014,1018,1020,1022,1024,1040,1072,1120,1154,1162,1216,1218,1232,
1328,1330,1368,4256,4294,5024,5104,5110,7312,7356,7358,7360,7680,7830,7838,7840,7936,7944,7952,7960,7966,7976,7984,7992,
8000,8008,8014,8040,8048,8072,8080,8088,8096,8104,8112,8120,8122,8124,8126,8136,8140,8142,8152,8154,8156,8168,8170,8172,
8174,8184,8186,8188,8190,8486,8488,8490,8492,8498,8500,8544,8560,9398,9424,11264,11312,11360,11362,11364,11366,11374,
11376,11378,11380,11390,11392,11492,11506,11508,42560,42606,42624,42652,42786,42800,42802,42864,42878,42888,42896,42900,
42902,42922,42924,42926,42928,42930,42932,42948,42950,42952,42956,42958,42960,42962,42966,42972,42974,65314,65340,66560,
66600,66736,66772,66928,66966,68736,68788,68944,68966,71840,71872,93760,93792,125184,125218
};
const int tu_lower_shifts_even[] = {
0,32,0,32,0,1,0,1,0,1,-121,0,1,206,0,205,0,79,203,0,207,211,1,0,211,0,1,218,0,1,218,0,217,0,1,0,1,0,2,0,1,2,0,1,0,1,-97,
1,-130,1,0,10795,0,10792,0,69,1,0,1,0,1,0,38,37,64,63,0,32,0,32,0,1,0,-60,0,1,0,-130,80,32,0,1,0,1,15,0,1,0,48,0,7264,0,
38864,8,0,-3008,0,-3008,0,1,0,-7615,1,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,-74,-9,0,-86,-9,0,-8,-100,0,-8,
-112,-7,0,-128,-126,-9,0,-7517,0,-8383,0,28,0,16,0,26,0,48,0,1,-10743,-10727,0,-10749,-10782,1,0,-10815,1,0,1,0,1,0,1,0,
1,0,1,0,1,0,1,0,1,-42308,-42315,-42308,-42258,-42261,1,-48,-35384,0,1,0,1,0,1,-42561,0,32,0,40,0,40,0,39,0,64,0,32,0,32,
0,32,0,34,0
};
const uint32_t tu_lower_indices_odd[] = {
1,65,91,193,215,217,223,313,329,377,383,385,387,391,393,395,397,399,401,403,405,407,409,413,415,417,423,425,427,431,433,
435,439,441,453,455,457,459,477,497,499,503,505,571,573,575,577,579,581,583,895,897,905,907,911,913,941,975,977,1015,
1017,1019,1021,1025,1041,1073,1217,1231,1329,1367,4257,4297,4301,4303,5025,5105,5111,7305,7307,7313,7355,7357,7361,7945,
7953,7961,7967,7977,7985,7993,8001,8009,8015,8025,8033,8041,8049,8073,8081,8089,8097,8105,8113,8121,8123,8125,8137,8141,
8153,8155,8157,8169,8171,8173,8185,8187,8189,8491,8493,8545,8561,8579,8581,9399,9425,11265,11313,11363,11365,11367,
11373,11375,11377,11381,11383,11391,11393,11499,11503,42873,42877,42879,42891,42893,42895,42923,42925,42927,42929,42931,
42933,42949,42951,42955,42957,42997,42999,65313,65339,66561,66601,66737,66773,66929,66939,66941,66955,66957,66963,66965,
66967,68737,68787,68945,68967,71841,71873,93761,93793,125185,125219
};
const int tu_lower_shifts_odd[] = {
0,32,0,32,0,32,0,1,0,1,0,210,0,1,205,1,0,202,1,205,0,209,0,213,214,0,1,218,0,1,217,1,219,0,1,2,0,1,0,2,0,-56,0,1,-163,0,
1,-195,71,0,116,0,37,0,63,32,0,8,0,1,-7,0,-130,80,32,0,1,0,48,0,7264,0,7264,0,38864,8,0,1,0,-3008,0,-3008,0,-8,0,-8,0,
-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,0,-8,-74,0,-86,0,-8,-100,0,-8,-112,0,-128,-126,0,-8262,0,16,0,1,0,26,0,48,0,-3814,
0,1,-10780,-10783,0,1,0,-10815,0,1,0,1,-35332,0,1,-42280,0,-42319,-42305,0,-42282,928,0,-42307,1,-42343,0,1,0,32,0,40,0,
40,0,39,0,39,0,39,0,39,0,64,0,32,0,32,0,32,0,34,0
};
const uint32_t tu_upper_indices_even[] = {
0,98,124,224,256,314,330,378,384,386,392,394,396,398,402,404,410,412,414,416,424,426,432,434,436,440,454,456,458,460,
462,478,498,500,572,574,576,578,580,592,594,596,598,600,604,606,608,610,612,614,616,618,620,622,626,628,640,642,644,648,
650,652,654,658,660,670,672,892,894,940,942,944,946,962,964,972,974,976,978,982,984,1008,1010,1012,1016,1018,1072,1104,
1120,1218,1232,1378,1416,4304,4348,4350,4352,5112,5118,7296,7298,7300,7302,7304,7306,7308,7566,7568,7936,7944,7952,7958,
7968,7976,7984,7992,8000,8006,8032,8040,8048,8050,8054,8056,8058,8060,8062,8112,8114,8126,8128,8144,8146,8160,8162,8526,
8528,8560,8576,8580,8582,9424,9450,11312,11360,11366,11368,11374,11382,11384,11500,11504,11520,11558,42874,42878,42892,
42894,42900,42902,42952,42956,42998,43000,43888,43968,65346,65372,66600,66640,66776,66812,66968,66978,66980,66994,66996,
67002,67004,67006,68800,68852,68976,68998,71872,71904,93792,93824,125218,125252
};
const int tu_upper_shifts_even[] = {
0,-32,0,-32,0,-1,0,-1,195,0,-1,0,-1,0,-1,0,163,0,130,0,-1,0,-1,0,-1,0,-2,-1,0,-2,-1,0,-1,0,-1,0,10815,-1,0,10783,10782,
-206,-205,0,42319,0,-205,0,42343,42308,-209,42308,42305,0,-213,0,-218,42307,0,-218,-217,-71,0,-219,0,42258,0,130,0,-38,
-37,0,-32,-31,-32,-64,-63,-62,0,-54,0,-86,7,0,-1,0,-32,-80,0,-1,0,-48,0,3008,0,3008,0,-8,0,-6254,-6244,-6242,-6236,
35266,-1,0,35384,0,8,0,8,0,8,0,8,0,8,0,8,0,74,86,100,128,112,126,0,8,0,-7205,0,8,0,8,0,-28,0,-16,0,-1,0,-26,0,-48,0,
-10792,-1,0,-1,0,-1,0,-7264,0,-1,0,-1,0,48,0,-1,0,-1,0,-38864,0,-32,0,-40,0,-40,0,-39,0,-39,0,-39,0,-39,0,-64,0,-32,0,
-32,0,-32,0,-34,0
};
const uint32_t tu_upper_indices_odd[] = {
1,97,123,181,183,225,247,249,255,257,305,307,313,331,377,383,385,387,391,405,407,409,411,413,417,423,429,431,441,443,
445,447,449,453,455,457,459,461,477,479,497,499,501,503,505,545,547,565,575,577,583,593,595,597,599,601,603,605,609,611,
613,615,617,619,621,623,625,627,629,631,637,639,643,645,647,649,651,653,669,671,837,839,881,885,887,889,891,895,941,945,
973,975,977,979,981,983,985,1009,1011,1013,1015,1019,1021,1073,1105,1121,1155,1163,1217,1231,1233,1329,1377,1415,4305,
4347,4349,4353,5113,5119,7297,7299,7301,7303,7305,7545,7547,7549,7551,7681,7831,7835,7837,7841,7937,7945,7953,7959,7969,
7977,7985,7993,8001,8007,8017,8025,8033,8041,8049,8051,8055,8057,8059,8061,8063,8113,8115,8145,8147,8161,8163,8165,8167,
8561,8577,9425,9451,11313,11361,11363,11365,11367,11379,11381,11393,11493,11507,11509,11521,11561,11565,11567,42561,
42607,42625,42653,42787,42801,42803,42865,42879,42889,42897,42901,42903,42923,42933,42949,42957,42959,42961,42963,42967,
42973,43859,43861,43889,43969,65345,65371,66601,66641,66777,66813,66967,67005,68801,68851,68977,68999,71873,71905,93793,
93825,125219,125253
};
const int tu_upper_shifts_odd[] = {
0,-32,0,743,0,-32,0,-32,121,-1,-232,-1,0,-1,0,-300,0,-1,0,97,0,-1,42561,0,-1,0,-1,0,-1,0,-1,56,0,-1,0,-2,-1,0,-79,-1,0,
-2,-1,0,-1,0,-1,0,10815,0,-1,10780,-210,0,-205,-202,-203,0,42315,-207,42280,0,-211,10743,0,-211,10749,0,-214,0,10727,0,
-218,0,42282,-69,-217,0,42261,0,84,0,-1,0,-1,0,130,0,-37,-32,-63,0,-57,0,-47,-8,-1,-80,-116,-96,0,-1,0,-32,-80,-1,0,-1,
0,-15,-1,0,-48,0,3008,0,3008,0,-8,0,-6253,-6242,-6243,-6181,0,35332,0,3814,0,-1,0,-59,0,-1,8,0,8,0,8,0,8,0,8,0,8,0,8,0,
74,86,100,128,112,126,0,8,0,8,0,8,0,7,0,-16,0,-26,0,-48,-1,0,-10795,0,-1,0,-1,0,-1,0,-7264,0,-7264,0,-1,0,-1,0,-1,0,-1,
0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-1,0,-928,0,-38864,0,-32,0,-40,0,-40,0,-39,0,-64,0,-32,0,-32,0,-32,0,-34,0
};

const int tu_case_lens[] = {
    sizeof tu_lower_indices_even / sizeof(uint32_t),
    sizeof tu_lower_indices_odd / sizeof(uint32_t),
    sizeof tu_upper_indices_even / sizeof(uint32_t),
    sizeof tu_upper_indices_odd / sizeof(uint32_t)
};
const uint32_t *tu_case_indices[] =
    { tu_lower_indices_even, tu_lower_indices_odd, tu_upper_indices_even, tu_upper_indices_odd };
const int *tu_case_shifts[] =
    { tu_lower_shifts_even,  tu_lower_shifts_odd,  tu_upper_shifts_even, tu_upper_shifts_odd };

#define CASE_LOWER 0
#define CASE_UPPER 2

typedef struct {
    uint32_t s[3];
    int len;
} uint32_x3_t;

uint32_x3_t tu_case_cp(uint32_t c, int _case) {
    assert(c < 0x110000);
    uint32_t *s;
    #define TU_C(i, ...) case i: s = (uint32_t[3]){ __VA_ARGS__ }; goto multichar;
    if(_case == CASE_UPPER) {
        switch(c) {
TU_C(223,83,83)TU_C(329,700,78)TU_C(496,74,780)TU_C(912,921,776,769)TU_C(944,933,776,769)TU_C(1415,1333,1362)
TU_C(7830,72,817)TU_C(7831,84,776)TU_C(7832,87,778)TU_C(7833,89,778)TU_C(7834,65,702)TU_C(8016,933,787)
TU_C(8018,933,787,768)TU_C(8020,933,787,769)TU_C(8022,933,787,834)TU_C(8064,7944,921)TU_C(8065,7945,921)
TU_C(8066,7946,921)TU_C(8067,7947,921)TU_C(8068,7948,921)TU_C(8069,7949,921)TU_C(8070,7950,921)TU_C(8071,7951,921)
TU_C(8072,7944,921)TU_C(8073,7945,921)TU_C(8074,7946,921)TU_C(8075,7947,921)TU_C(8076,7948,921)TU_C(8077,7949,921)
TU_C(8078,7950,921)TU_C(8079,7951,921)TU_C(8080,7976,921)TU_C(8081,7977,921)TU_C(8082,7978,921)TU_C(8083,7979,921)
TU_C(8084,7980,921)TU_C(8085,7981,921)TU_C(8086,7982,921)TU_C(8087,7983,921)TU_C(8088,7976,921)TU_C(8089,7977,921)
TU_C(8090,7978,921)TU_C(8091,7979,921)TU_C(8092,7980,921)TU_C(8093,7981,921)TU_C(8094,7982,921)TU_C(8095,7983,921)
TU_C(8096,8040,921)TU_C(8097,8041,921)TU_C(8098,8042,921)TU_C(8099,8043,921)TU_C(8100,8044,921)TU_C(8101,8045,921)
TU_C(8102,8046,921)TU_C(8103,8047,921)TU_C(8104,8040,921)TU_C(8105,8041,921)TU_C(8106,8042,921)TU_C(8107,8043,921)
TU_C(8108,8044,921)TU_C(8109,8045,921)TU_C(8110,8046,921)TU_C(8111,8047,921)TU_C(8114,8122,921)TU_C(8115,913,921)
TU_C(8116,902,921)TU_C(8118,913,834)TU_C(8119,913,834,921)TU_C(8124,913,921)TU_C(8130,8138,921)TU_C(8131,919,921)
TU_C(8132,905,921)TU_C(8134,919,834)TU_C(8135,919,834,921)TU_C(8140,919,921)TU_C(8146,921,776,768)TU_C(8147,921,776,769)
TU_C(8150,921,834)TU_C(8151,921,776,834)TU_C(8162,933,776,768)TU_C(8163,933,776,769)TU_C(8164,929,787)TU_C(8166,933,834)
TU_C(8167,933,776,834)TU_C(8178,8186,921)TU_C(8179,937,921)TU_C(8180,911,921)TU_C(8182,937,834)TU_C(8183,937,834,921)
TU_C(8188,937,921)TU_C(64256,70,70)TU_C(64257,70,73)TU_C(64258,70,76)TU_C(64259,70,70,73)TU_C(64260,70,70,76)
TU_C(64261,83,84)TU_C(64262,83,84)TU_C(64275,1348,1350)TU_C(64276,1348,1333)TU_C(64277,1348,1339)TU_C(64278,1358,1350)
TU_C(64279,1348,1341)
            default: break;
            multichar:
            return (uint32_x3_t){ .s = { s[0], s[1], s[2] }, .len = 2 + (s[2] != 0) };
        }
    } else if(c == 304) return (uint32_x3_t){ .s = { 105, 775 }, .len = 2 };
    #undef TU_C
    const uint32_t *indices = tu_case_indices[c % 2 + _case];
    const int *shifts = tu_case_shifts[c % 2 + _case];
    int len = tu_case_lens[c % 2 + _case];
    int i = tu_binary_search(indices, len, c);
    return (uint32_x3_t){ .s = { c + shifts[i] }, .len = 1 };
}

// to_uppercase or to_lowercase result length
// _case - CASE_UPPER or CASE_LOWER
int tu_case_len(const char *src, int len, int _case) {
    int l = 0;
    for(const char *s = src; s < src + len;) {
        uint32_t c;
        char *s_ = step_utf8_cp((char*)s, src + len - s, &c);
        if(s_ == s) { s ++; l ++; continue; }
        uint32_x3_t r = tu_case_cp(c, _case);
        for(int i = 0; i < r.len; i ++) {
            uint32_t c = r.s[i];
            l += 1 + (c >= 0x80) + (c >= 0x800) + (c >= 0x10000);
        }
        s = s_;
    }
    return l;
}

// to_uppercase or to_lowercase
// _case - CASE_UPPER or CASE_LOWER
// movables - indices into src that get modified to point to respective places in dst
// nm - count of movables
int tu_case(char *dst, const char *src, int len, int _case, int *movables, int nm) {
    bool *moved_flags = malloc(sizeof(bool) * nm);
    char *_dst = dst;
    for(const char *s = src; s < src + len;) {
        char *d = dst;
        uint32_t c;
        char *s_ = step_utf8_cp((char*)s, src + len - s, &c);
        if(s_ == s) { *(dst ++) = *s; s_ ++; goto cnt; }
        uint32_x3_t r = tu_case_cp(c, _case);
        for(int i = 0; i < r.len; i ++) dst = write_utf8_cp(dst, r.s[i]);
        cnt:
        for(int i = 0; i < nm; i ++) {
            if(s - src <= movables[i] && movables[i] < s_ - src && !moved_flags[i]) {
                movables[i] = d - _dst;
                moved_flags[i] = true;
            }
        }
        s = s_;
    }
    for(int i = 0; i < nm; i ++) {
        if(!moved_flags[i] && movables[i] >= len) movables[i] += dst - _dst - len;
    }
    free(moved_flags);
    return dst - _dst;
}

#if TU_SYSTEM == TU_WINDOWS
// copying behavior of the wcwidth python package
const uint32_t tu_wcwidth_indices[] = {
0,1,32,127,160,768,880,1155,1162,1425,1470,1471,1472,1473,1475,1476,1478,1479,1480,1552,1563,1564,1565,1611,1632,1648,
1649,1750,1757,1759,1765,1767,1769,1770,1774,1809,1810,1840,1867,1958,1969,2027,2036,2045,2046,2070,2074,2075,2084,2085,
2088,2089,2094,2137,2140,2199,2208,2250,2274,2275,2308,2362,2365,2366,2384,2385,2392,2402,2404,2433,2436,2492,2493,2494,
2501,2503,2505,2507,2510,2519,2520,2530,2532,2558,2559,2561,2564,2620,2621,2622,2627,2631,2633,2635,2638,2641,2642,2672,
2674,2677,2678,2689,2692,2748,2749,2750,2758,2759,2762,2763,2766,2786,2788,2810,2816,2817,2820,2876,2877,2878,2885,2887,
2889,2891,2894,2901,2904,2914,2916,2946,2947,3006,3011,3014,3017,3018,3022,3031,3032,3072,3077,3132,3133,3134,3141,3142,
3145,3146,3150,3157,3159,3170,3172,3201,3204,3260,3261,3262,3269,3270,3273,3274,3278,3285,3287,3298,3300,3315,3316,3328,
3332,3387,3389,3390,3397,3398,3401,3402,3406,3415,3416,3426,3428,3457,3460,3530,3531,3535,3541,3542,3543,3544,3552,3570,
3572,3633,3634,3636,3643,3655,3663,3761,3762,3764,3773,3784,3791,3864,3866,3893,3894,3895,3896,3897,3898,3902,3904,3953,
3973,3974,3976,3981,3992,3993,4029,4038,4039,4139,4159,4182,4186,4190,4193,4194,4197,4199,4206,4209,4213,4226,4238,4239,
4240,4250,4254,4352,4448,4608,4957,4960,5906,5910,5938,5941,5970,5972,6002,6004,6068,6100,6109,6110,6155,6160,6277,6279,
6313,6314,6432,6444,6448,6460,6679,6684,6741,6751,6752,6781,6783,6784,6832,6878,6880,6892,6912,6917,6964,6981,7019,7028,
7040,7043,7073,7086,7142,7156,7204,7224,7376,7379,7380,7401,7405,7406,7412,7413,7415,7418,7616,7680,8203,8208,8232,8239,
8288,8304,8400,8433,8986,8988,9001,9003,9193,9197,9200,9201,9203,9204,9725,9727,9748,9750,9776,9784,9800,9812,9855,9856,
9866,9872,9875,9876,9889,9890,9898,9900,9917,9919,9924,9926,9934,9935,9940,9941,9962,9963,9970,9972,9973,9974,9978,9979,
9981,9982,9989,9990,9994,9996,10024,10025,10060,10061,10062,10063,10067,10070,10071,10072,10133,10136,10160,10161,10175,
10176,11035,11037,11088,11089,11093,11094,11503,11506,11647,11648,11744,11776,11904,11930,11931,12020,12032,12246,12272,
12330,12336,12351,12353,12439,12441,12443,12544,12549,12592,12593,12644,12645,12687,12688,12774,12783,12831,12832,12872,
12880,42125,42128,42183,42607,42611,42612,42622,42654,42656,42736,42738,43010,43011,43014,43015,43019,43020,43043,43048,
43052,43053,43136,43138,43188,43206,43232,43250,43263,43264,43302,43310,43335,43348,43360,43389,43392,43396,43443,43457,
43493,43494,43561,43575,43587,43588,43596,43598,43643,43646,43696,43697,43698,43701,43703,43705,43710,43712,43713,43714,
43755,43760,43765,43767,44003,44011,44012,44014,44032,55204,55216,55296,63744,64256,64286,64287,65024,65040,65050,65056,
65072,65107,65108,65127,65128,65132,65279,65280,65281,65377,65440,65441,65504,65511,65520,65532,66045,66046,66272,66273,
66422,66427,68097,68100,68101,68103,68108,68112,68152,68155,68159,68160,68325,68327,68900,68904,68969,68974,69291,69293,
69370,69376,69446,69457,69506,69510,69632,69635,69688,69703,69744,69745,69747,69749,69759,69763,69808,69819,69826,69827,
69888,69891,69927,69941,69957,69959,70003,70004,70016,70019,70067,70081,70089,70093,70094,70096,70188,70200,70206,70207,
70209,70210,70367,70379,70400,70404,70459,70461,70462,70469,70471,70473,70475,70478,70487,70488,70498,70500,70502,70509,
70512,70517,70584,70593,70594,70595,70597,70598,70599,70603,70604,70609,70610,70611,70625,70627,70709,70727,70750,70751,
70832,70852,71087,71094,71096,71105,71132,71134,71216,71233,71339,71352,71453,71468,71724,71739,71984,71990,71991,71993,
71995,71999,72000,72001,72002,72004,72145,72152,72154,72161,72164,72165,72193,72203,72243,72250,72251,72255,72263,72264,
72273,72284,72330,72346,72544,72552,72751,72759,72760,72768,72850,72872,72873,72887,73009,73015,73018,73019,73020,73022,
73023,73030,73031,73032,73098,73103,73104,73106,73107,73112,73459,73463,73472,73474,73475,73476,73524,73531,73534,73539,
73562,73563,78896,78913,78919,78934,90398,90416,92912,92917,92976,92983,94031,94032,94033,94088,94095,94099,94176,94180,
94181,94192,94194,94199,94208,101590,101631,101663,101760,101875,110576,110580,110581,110588,110589,110591,110592,
110883,110898,110899,110928,110931,110933,110934,110948,110952,110960,111356,113821,113823,113824,113828,118528,118574,
118576,118599,119141,119146,119149,119171,119173,119180,119210,119214,119362,119365,119552,119639,119648,119671,121344,
121399,121403,121453,121461,121462,121476,121477,121499,121504,121505,121520,122880,122887,122888,122905,122907,122914,
122915,122917,122918,122923,123023,123024,123184,123191,123566,123567,123628,123632,124140,124144,124398,124400,124643,
124644,124646,124647,124654,124656,124661,124662,125136,125143,125252,125259,126980,126981,127183,127184,127374,127375,
127377,127387,127462,127491,127504,127548,127552,127561,127568,127570,127584,127590,127744,127777,127789,127798,127799,
127869,127870,127892,127904,127947,127951,127956,127968,127985,127988,127989,127992,128063,128064,128065,128066,128253,
128255,128318,128331,128335,128336,128360,128378,128379,128405,128407,128420,128421,128507,128592,128640,128710,128716,
128717,128720,128723,128725,128729,128732,128736,128747,128749,128756,128765,128992,129004,129008,129009,129292,129339,
129340,129350,129351,129536,129648,129661,129664,129675,129678,129735,129736,129737,129741,129757,129759,129771,129775,
129785,131072,196606,196608,262142,917504,921600,1114112
};
const int tu_wcwidth_widths[] = {
0,-1,1,-1,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,2,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,
1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,
1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,1,0,1,0,1,2,1,2,1,2,1,2,0,2,1,2,1,0,2,1,2,1,2,0,2,1,2,1,
2,1,2,1,2,1,2,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,2,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,2,1,0,1,2,1,0,1,0,2,1,0,2,1,2,1,2,1,0,1,2,1,0,1,2,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,
0,1,2,0,1,0,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,2,1,2,1,0,1,0,1,0,1,
0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,
2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,
2,1,2,1,2,1,2,1,2,1,2,1,2,1,2,1,0,1,-1
};

int tu_wcwidth(uint32_t c) {
    return tu_wcwidth_widths[tu_binary_search(tu_wcwidth_indices, sizeof tu_wcwidth_indices, c)];
}
#endif

// consumes one item from `s`, updating a bunch of state accordingly
// item is a sequence of bytes that gets displayed "at once", it is one of:
// - \t: moves cursor to next of 1,9,17,25,... columns
// - \n: translated into \r\n optionally preceded by clearing rest of line
// - \v, \f: both passed to terminal as is, both move cursor down 1 line
// - (\e\[|\x9B|\xC2\x9B)[0-9;]*m (view as regex): changes output appearence; passed as is but using CSI \e[ and appended to color_stack
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
// - p (nullable when out is NULL): current position
// - width: terminal window width, not used when p is NULL
// - split_wchars: whether printing wide character that does not fit on a line splits it into parts (e.g. on non-graphical TTY)
//                 or gets it wrapped to the next line as a whole (e.g. in all graphical terminals I tested); not used when p is NULL
// - out (nullable): where to append what shall go to the terminal
// - color_stack (nullable when out is NULL): where to append (\e\[|\x9B|\xC2\x9B)[0-9;]*m sequences; used to restore color/appearence
// - cursor (nullable): a cursor to advanse accourding to the consumed item
// RETURNS pointer to right after the item
char *tu_step_item(char *s, int len, pos_t *p, int width, bool split_wchars, str_t *out, str_t *color_stack, cursor_t *cursor) {
    int bytes;
    // == special control items: ==
    if(*s == '\t') {
        if(p) {
            int x = ((p->x - 1) / 8) * 8 + 9;
            if(x > width) x = width;
            if(cursor) { cursor->col += x - p->x; cursor->cp ++; }
            if(x > p->x) {
                if(out) for(int i = 0; i < x - p->x; i ++) str_append_lit(out, " ");
                //if(out) str_append_lit(out, "\t");
                p->x = x;
            }
        }
        bytes = 1; goto ret;
    }
    if(*s == '\n') {
        if(out) {
            if(p->bouta_wrap) str_append_lit(out, "\r\n");
            else str_append_lit(out, TERM_CLEAR_LINE_RIGHT "\r\n");
        }
        if(p) { p->y ++; p->x = 1; p->bouta_wrap = false; }
        if(cursor) { cursor->row ++; cursor->col = 0; cursor->cp = 0; cursor->row_byte = -1; }
        bytes = 1; goto ret;
    }
    if(*s == '\v' || *s == '\f') {
        if(out) {
            str_append_lit(out, TERM_CLEAR_LINE_RIGHT);
            str_append(out, s, 1);
            str_append_lit(out, TERM_CLEAR_LINE_LEFT);
        }
        if(p) { p->y ++; p->bouta_wrap = false; }
        if(cursor) { cursor->cp ++; }
        bytes = 1; goto ret;
    }
    if((*s == '\33' && len >= 3 && s[1] == '[') ||    // regular CSI (\e[)
       (*s == '\233' && len >= 2)                ||    // CSI char (\x9B)
       (*s == '\302' && len >= 3 && s[1] == '\233')) { // CSI char in utf8
        char *s_ = s + 1 + (*s != '\233');
        int i;
        for(i = s_ - s; i < len; i ++)
            if((s[i] < '0' || s[i] > '9') && s[i] != ';') break;
        if(i < len && s[i] == 'm') {
            if(out) { str_append_lit(out, "\33["); str_append(out, s_, s + i + 1 - s_); }
            if(color_stack) {
                int l = s + i - s_;
                if(l == 1 && *s_ == '0' || l == 0) color_stack->len = 0; // \e[0m of \e[m erases color stack
                else { str_append_lit(color_stack, "\33["); str_append(color_stack, s_, s + i + 1 - s_); }
            }
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
    #if TU_SYSTEM == TU_WINDOWS
    w = tu_wcwidth(cp);
    #else
    w = (uint32_t)(wchar_t)cp == cp ? wcwidth(cp) : -1;
    #endif
    if(w >= 0) {
        if(out) str_append(out, s, s_ - s);
    } else {
        if(out) str_printf(out, TERM_COLOR_INVERSE "<%.*" PRIX32 ">" TERM_COLOR_RESET "%.*s", cp <= 0xFFFF ? 4 : 8, cp, color_stack->len, color_stack->s);
        w = 1;
        n = cp <= 0xFFFF ? 6 : 10;
    }
    bytes = s_ - s;
    advanse:
    if(cursor && w > 0) { cursor->cp ++; cursor->col += w; }
    if(p) for(int i = 0; i < n; i ++) {
        if(p->bouta_wrap && w > 0) {
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
    if(cursor) { cursor->byte += bytes; cursor->row_byte += bytes; }
    return s + bytes;
}

// get boundaries of a utf8 cp; like tu_item_boundary
// return as if single char when not a part of a valid cp
void utf8_cp_boundary(char *s, int len, int at, int *begin, int *end, int *width) {
    if((s[at] & 0x80) == 0) goto single;
    char *b, *e;
    uint32_t c;
    if(at >= 0)           { b = s + at    ; e = step_utf8_cp(b, len - at    , &c); }
    if(at >= 1 && b == e) { b = s + at - 1; e = step_utf8_cp(b, len - at + 1, &c); }
    if(at >= 2 && b == e) { b = s + at - 2; e = step_utf8_cp(b, len - at + 2, &c); }
    if(at >= 3 && b == e) { b = s + at - 3; e = step_utf8_cp(b, len - at + 3, &c); }
    if(e > s + at) {
        if(begin) *begin = b - s; if(end) *end = e - s;
        #if TU_SYSTEM == TU_WINDOWS
        if(width) *width = tu_wcwidth(c);
        #else
        if(width) *width = (uint32_t)(wchar_t)c == c ? wcwidth(c) : -1;
        #endif
        return;
    }
    single:
    if(begin) *begin = at; if(end) *end = at + 1;
    if(width) *width = 1;
}

//int ii = 0;

// get boundaries of the item in `s` that contains `at` byte
// at should be in [0, len)
// begin, end - nullable output params
void tu_item_boundary(char *s, int len, int at, int *begin, int *end, int *width) {
    // testing for (\e\[|\x9B|\xC2\x9B)[0-9;]*m
    int csi_test = -1; // after or right on CSI end, strictly before 'm'
    if(s[at] == ']' || '0' <= s[at] && s[at] <= '9' || s[at] == ';') csi_test = at;
    if(s[at] == 'm') csi_test = at - 1;
    if(s[at] == '\33' || s[at] == '\233') csi_test = at + 1;
    if(s[at] == '\302') csi_test = at + 2;
    if(csi_test >= 0 && csi_test < len - 1) {
        // forward check
        int i;
        for(i = csi_test + 1; i < len; i ++) if((s[i] < '0' || s[i] > '9') && s[i] != ';') break;
        if(i >= len || s[i] != 'm') goto no_csi;
        if(end) *end = i + 1;
        // backwards check
        for(i = csi_test; i >= 0; i --) if((s[i] < '0' || s[i] > '9') && s[i] != ';') break;
        int b;
        if(i >= 1 && s[i] == '[' && s[i - 1] == '\33') { b = i - 1; goto csi; } // \e[ SCI
        if(i >= 1 && s[i] == '\233' && s[i - 1] == '\302') { b = i - 1; goto csi; } // csi char \x9B in utf8
        if(i >= 0 && s[i] == '\233') b = i; // csi char \x9B raw
        if(b > 0) { // testing that raw \x9B is not a part of a cp
            int _e;
            utf8_cp_boundary(s, len, b - 1, NULL, &_e, NULL);
            if(_e != b) goto no_csi;
        } else goto no_csi;
        csi:
        if(begin) *begin = b;
        if(width) *width = 0;
        return;
    }
    no_csi:
    utf8_cp_boundary(s, len, at, begin, end, width);
//    if(ii) { printf("\r\n{%i %i %i %i}\r\n", *begin, *end, at, len); exit(0); }
}

// how to find the origin after a window resize
#define REORIGIN_RESTORE 1 // using save (\e7) + restore (\e8), tested and works in gnu terminal & xfce terminal
                           // where the origin is adjusted after rewrapping lines, and in xterm where lines are not rewrapped
#define REORIGIN_COMPUTE 2 // compute assuming lines rawrapping and that the cursor remains on the same char
                           // used on windows because windows console does not auto-agjust saved pos on rewrap

// content of terminal (editable part starting from "origin")
// is editable by replacing everything starting at a byte (i.e. keeping a part)
// should handle resizing given no resize happens while the resize handler is running and the content fits in the window
// if REORIGIN_RESTORE is used, the origin should be saved via \e7
// apparently terminals other than windows console that auto-recalculate line wrapping on resizes
// also auto adjust saved position (only on re-wrapping, not on wripping when writing to the terminal)
typedef struct {
    char *s;
    int len, cap;
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
    bool cursor_bw; // indicates that on the last render cursor ended up with bouta_wrap=true, thus pointing to previous char to cursor_byte
    bool resize_pending; // need to process resize before rendering
    long resize_timeout; // if non-negative, only rerender after this number of ms of no resizes, in content_wait_in, to avoid race conditions
                         // used on windows, though the necessity of this could've been dictated by testing on a super-laggy vm
    int error; // gets set to CONT_ERR_* on an error; when non-zero, content is considered corrupted and all content_* funtions on it return immediately
    int reorigin_method; // REORIGIN_*
    #if TU_SYSTEM == TU_WINDOWS
    wchar_t high_surrogate; // temp storage for high utf16 surrogate read from console, to then join with lower surrogate
                            // 0 when no high surrogate is pending
    #endif
} content_t;

// values for content_t.error
#define CONT_ERR_GETPOS  1
#define CONT_ERR_GETSIZE 2
#define CONT_ERR_OUT     3
#define CONT_ERR_WAIT    4
#define CONT_ERR_SETUP   5
#define CONT_ERR_IN      6
#define CONT_ERR_TEST    7

content_t content_create(FILE *in, FILE *out) {
    content_t t = (content_t){
        .s = NULL, .len = 0, .cap = 0,
        // .line_sizes = NULL, .n_lines = 0,
        .out = out, .in = in,
        .split_wchars = false, // todo: figure this out
        .input_buf = (str_t){ .s = NULL, .len = 0, .cap = 0 },
        .error = 0,
        .cursor_byte = 0,
        .cursor_bw = false,
        .resize_pending = false,
        #if TU_SYSTEM == TU_WINDOWS
        .resize_timeout = 100,
        .reorigin_method = REORIGIN_COMPUTE,
        .high_surrogate = 0
        #else
        .resize_timeout = -1,
        .reorigin_method = REORIGIN_RESTORE
        #endif
    };
    if(term_get_size(t.in, t.out, &t.width, &t.height) < 0)
        { t.error = CONT_ERR_GETSIZE; return t; }
    if(fprintf(t.out, "\r" TERM_CURSOR_SAVE) < 0)
        { t.error = CONT_ERR_OUT; return t; }
    t.origin = term_get_pos(t.out, t.in, &t.input_buf).y;
    if(t.origin < 0) t.error = CONT_ERR_GETPOS;
    #if TU_SYSTEM == TU_UNIX
    if(setup_resize_watch() < 0) t.error = CONT_ERR_SETUP;
    #endif
    return t;
}

// render the content starting at byte `start`
// refuses to write to terminal if the window has been resized (tu_winch on unix or cap change on windows)
// in this case t->resize_pending is set and one should first call content_resize
void content_render_from(content_t *t, int start) {
    if(t->error || t->resize_pending) return;
    assert(0 <= start && start <= t->len);
    if(t->origin < 1) { t->origin = 1; start = 0; }
    pos_t pos = (pos_t){ .x = 1, .y = t->origin, .bouta_wrap = false };
//    printf("\n\r[%i]\n\r", pos.x);
    pos_t cur = (pos_t){ .x = 1, .y = t->origin, .bouta_wrap = false };
    bool got_cur = t->cursor_byte <= 0;
    bool nl_after_cur = false;
    bool wrap_after_cur = false; // wrapped after cursor but before \n, \f or \v; in this case cur.bouta_wrap is translated into newline
    str_t out = (str_t){ .s = NULL, .len = 0, .cap = 0 };
    str_t cs  = (str_t){ .s = NULL, .len = 0, .cap = 0 };
    char *s = t->s;
    if(start) {
        pos_t _pos = pos, __pos = pos;
        int _cs_len = 0, __cs_len = 0;
        char *_s = s, *__s = s;
        for(;;) {
            int _x = pos.x, _y = pos.y, _b = s - t->s;
            bool nl = *s == '\n' || *s == '\v' || *s == '\f';
            s = tu_step_item(s, (t->s + start) - s, &pos, t->width, t->split_wchars, NULL, &cs, NULL);
            if(!got_cur && s - t->s >= t->cursor_byte) { cur = pos; got_cur = true; }
            nl_after_cur |= (got_cur && nl);
            wrap_after_cur |= (got_cur && !nl_after_cur && _y != pos.y);
            if(s > t->s + start) break;
            if(_x != pos.x || _y != pos.y) { __pos = _pos; __cs_len = _cs_len; __s = _s; } // saving situation before a non-zero width char to potentially overwrite modifiers like accents
            if(!pos.bouta_wrap) { _pos = pos; _cs_len = cs.len; _s = s; } // saving pre-bouta_wrap situation 'cause restoring bouta_wrap flag with TERM_CURSOR_TO is impossible
            if(s >= t->s + start) break;
        }
        pos = __pos; cs.len = __cs_len; s = __s;
        str_append_lit(&out, TERM_COLOR_RESET TERM_CURSOR_HIDE);
        str_printf(&out, TERM_CURSOR_TO, pos.y, pos.x);
        str_append(&out, cs.s, cs.len);
    } else str_printf(&out, TERM_COLOR_RESET TERM_CURSOR_HIDE TERM_CURSOR_TO, t->origin, 1);
    bool overflow = false;
    while(s < t->s + t->len) {
        int _y = pos.y;
        bool nl = *s == '\n' || *s == '\v' || *s == '\f';
        s = tu_step_item(s, (t->s + t->len) - s, &pos, t->width, t->split_wchars, &out, &cs, NULL);
        if(!got_cur && t->cursor_byte && s - t->s >= t->cursor_byte) { cur = pos; got_cur = true; }
        nl_after_cur |= (got_cur && nl);
        wrap_after_cur |= (got_cur && !nl_after_cur && _y != pos.y);
    }
//    printf("\n\r{%i}\n\r", pos.x);
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
    if(t->cursor_byte >= 0) {
        if(wrap_after_cur && cur.bouta_wrap)
            { cur.x = 0; cur.y ++; cur.bouta_wrap = false; }
        str_printf(&out, TERM_CURSOR_TO TERM_CURSOR_SHOW, cur.y >= 1 ? cur.y : 1, cur.x);
        t->cursor_bw = cur.bouta_wrap;
    } else str_printf(&out, TERM_CURSOR_TO, t->origin >= 1 ? t->origin : 1, 1);
    if(out.len) {
        int r = term_check_resize(t->in, t->out, &t->width, &t->height);
        if(r < 0) { t->error = CONT_ERR_GETSIZE; goto ret; }
        if(r) { t->resize_pending = true; goto ret; }
        //if(fwrite(out.s, 1, out.len, t->out) < 0) t->error = CONT_ERR_OUT;
        if(tu_fwrite_utf8(out.s, out.len, t->out) < 0) t->error = CONT_ERR_OUT;
        else if(fflush(t->out) < 0) t->error = CONT_ERR_OUT;
    }
    ret:
    free(out.s);
    free(cs.s);
}

// replace the rest of the content starting from byte `start` with `cont` of length `len`
// `start` should be in [0, t->len], `len` may be zero
// also attempts to render replaced part of ccontent (if no resize is pending)
void content_change(content_t *t, int start, const char *cont, int len) {
    assert(0 <= start && start <= t->len);
    assert(len >= 0);
    if(t->error) return;
    bool item_cut = false; // if we are _possibly_ cutting an item
    int b = start;
    if(start > 0) {
        int e;
        tu_item_boundary(t->s, t->len, start - 1, &b, &e, NULL);
//        printf("\r\n[%i %i %i]\r\n", b, e, start);
        if(e == start) b = start;
    }
    t->len = start;
    str_append((str_t*)t, cont, len);
    if(start > 0 && b == start) {
        int e;
        tu_item_boundary(t->s, t->len, start - 1, &b, &e, NULL);
        if(e == start) b = start;
    }
//    if(item_cut) printf("\r\n=========================\r\n");
    content_render_from(t, b);
}

// todo: watch over content error from tl

#if TU_SYSTEM == TU_WINDOWS
#define tu_sleep_ms Sleep
#else
void tu_sleep_ms(unsigned long t) {
    struct timespec ts = tu_ms2ts(t);
    nanosleep(&ts, NULL);
}
#endif

// relative position after writing `s` up to `byte` starting at x=1, i.e. with y starting at 0
pos_t term_rel_pos(int width, bool split_wchars, char *s, int len, int byte) {
    pos_t _p;
    pos_t p = { .x = 1, .y = 0, .bouta_wrap = false };
    char *c = s;
    for(;;) {
        _p = p;
        c = tu_step_item(c, len, &p, width, split_wchars, NULL, NULL, NULL);
        if(c - s > byte) break;
    }
    return _p;
}

#ifndef TU_SAFETY_DELAY
#define TU_SAFETY_DELAY 4 // before rerendering we shall get no resize for this number of ms to avoid race conditions
#endif

// rerender the content assuming the terminal window has been resized
// width and height have to be set before calling this
void content_resize(content_t *t) {
    t->resize_pending = false;
    if(t->error) return;
    if(t->reorigin_method == REORIGIN_RESTORE) {
        if(t->cursor_byte >= 0) {
            // workflow:
            // Save current cursor to `p`; then wait a little; if no winch happens; hide, restore origin, request pos, move cursor to `p`, show
            // Only then see the responce for origin pos. This way in time when the cursor is hidden we don't wait for stdin and avoid flickering
            pos_t p = term_get_pos(t->out, t->in, &t->input_buf);
            if(p.x < 0) { t->error = CONT_ERR_GETPOS; return; }
            tu_sleep_ms(TU_SAFETY_DELAY); // makes resizing more stable on gnome terminal (and probably other terminals with auto-rewrapping)
            int r = term_check_resize(t->in, t->out, &t->width, &t->height);
            if(r < 0) { t->error = CONT_ERR_GETSIZE; return; }
            if(r) { t->resize_pending = true; return; }
            if(fprintf(t->out, TERM_CURSOR_HIDE TERM_CURSOR_RESTORE
                               TERM_REQUEST_CURSOR TERM_CURSOR_TO
                               TERM_CURSOR_SHOW, p.y, p.x) < 0 || fflush(t->out) < 0)
                { t->error = CONT_ERR_OUT; return; }
            t->origin = term_read_pos(t->out, t->in, &t->input_buf).y;
            if(t->origin < 0) { t->error = CONT_ERR_OUT; return; }
        } else {
            if(fprintf(t->out, TERM_CURSOR_RESTORE TERM_REQUEST_CURSOR) < 0 ||
               fflush(t->out) < 0) { t->error = CONT_ERR_OUT; return; }
            t->origin = term_read_pos(t->out, t->in, &t->input_buf).y;
            if(t->origin < 0) { t->error = CONT_ERR_OUT; return; }
            tu_sleep_ms(1); // makes resizing more stable on gnome terminal (and probably other terminals with auto-rewrapping)
        }
    } else if(t->reorigin_method == REORIGIN_COMPUTE) {
        pos_t pos = term_get_pos(t->out, t->in, &t->input_buf);
        if(t->cursor_byte < 0) t->origin = pos.y;
        else {
            pos_t p = term_rel_pos(t->width, t->split_wchars, t->s, t->len, t->cursor_byte - t->cursor_bw);
            tu_sleep_ms(TU_SAFETY_DELAY);
            int r = term_check_resize(t->in, t->out, &t->width, &t->height);
            if(r < 0) { t->error = CONT_ERR_GETSIZE; return; }
            if(r) { t->resize_pending = true; return; }
            t->origin = pos.y - p.y - p.bouta_wrap;
        }
    }
    content_render_from(t, 0);
}

// wait for data received on t->in, handling window resizes
// all received data gets read and put into t->input_buf
void content_wait_in(content_t *t) {
    if(t->error) return;
    int l = t->input_buf.len;
    if(t->resize_timeout < 0) while(t->resize_pending) content_resize(t);
    while(t->input_buf.len == l || t->resize_pending) {
        int r = term_wait_resize_or_in(t->in, &t->input_buf,
            t->resize_pending ? t->resize_timeout : -1, &t->width, &t->height
            #if TU_SYSTEM == TU_WINDOWS
            , &t->high_surrogate
            #endif
        );
        if(r < 0) { t->error = CONT_ERR_WAIT; return; }
        if(r == 1) t->resize_pending = true;
        int _r = t->resize_timeout;
        if(r == 0 || t->resize_timeout < 0)
            while(t->resize_pending) content_resize(t);
    }
    while(t->resize_pending) content_resize(t);
}

#define CURSOR_FROM_BYTE 1 // from `byte` field
#define CURSOR_FROM_COL  2 // from `row` and `col` fields
#define CURSOR_FROM_CP 3 // from `row` and `char` fields

// fill all fields of `dst` cursor from certain field, determined by `source`
// `source` shall be one of CURSOR_FROM_* constants
// source fiels may be invalid (e.g. out of bounds or `byte` pointing not to start of an item);
// corrected naturally in this case
void cursor_restore(const content_t *t, cursor_t *dst, int source) {
    assert(1 <= source && source <= 3);
//    DEBUG("+\n");
    cursor_t cur = { 0 };
    cursor_t _cur = { 0 };
    pos_t pos = (pos_t){ .x = 1, .y = t->origin, .bouta_wrap = false };
    char *s = t->s;
    for(;;) {
        if(source == CURSOR_FROM_BYTE && cur.byte >= dst->byte) { *dst = cur; return; }
        if(source == CURSOR_FROM_CP) {
            if(cur.row > dst->row) { *dst = _cur; return; }
            if(cur.row == dst->row && cur.cp > dst->cp) { *dst = _cur.row == dst->row ? _cur : cur; return; }
        }
        if(source == CURSOR_FROM_COL) {
            if(cur.row > dst->row) { *dst = _cur; return; }
            if(cur.row == dst->row && cur.col > dst->col) { *dst = _cur.row == dst->row ? _cur : cur; return; }
        }
        if(s - t->s >= t->len) break;
        _cur = cur;
        s = tu_step_item(s, t->s + t->len - s, &pos, t->width, t->split_wchars, NULL, NULL, &cur);
    }
//    DEBUG("%i\n", t->len);
//    for(int i = 0; i < t->len; i ++) {
//        DEBUG("%X %c\n", (unsigned char)t->s[i], t->s[i]);
//    }
//    DEBUG("-\n");
    *dst = cur;
}

typedef struct {
    char *s;
    int len, cap;
    int cursor, mark;
    bool mark_weak;
} lhrec_t; // line history record

tu_implement_arr(lhrec_arr, lhrec_t)

// text formating sequences \e\[[0-9;]*m
typedef struct {
    char *s;
    int len, cap;
    int at; // before which byte to put it
    int prio; // smaller go first; non-negative only
} color_t;

#define TU_INT_HIGH_BIT (~((unsigned int)(-1) >> 1) >> 1)

void color_sort_(color_t *begin, color_t *end, int bit, int level) {
    color_t *_begin = begin;
    color_t *_end = end;
    while(begin < end) {
        while(begin < end && ((level ? begin->prio : begin->at) & bit) == 0) begin ++;
        while(begin < end &&  (level ? end->prio   : end->at  ) & bit) end --;
        color_t temp = *begin; *begin = *end; *end = temp;
    }
    color_t *mid = begin + (((level ? begin->prio : begin->at) & bit) == 0);
    int bit_ = bit >> 1;
    int level_ = level;
    if(bit_ == 0) {
        if(level) return;
        bit_ = TU_INT_HIGH_BIT;
        level_ ++;
    }
    if(mid - _begin >= 2) color_sort_(_begin, mid - 1, bit_, level_);
    if(_end - mid >= 1) color_sort_(mid, _end, bit_, level_);
}

void color_sort(color_t *f, int len) { if(len >= 2) color_sort_(f, f + len - 1, TU_INT_HIGH_BIT, 0); }

tu_implement_arr(color_arr, color_t)

typedef struct {
    char *s;
    int len;
    int type;
    uint32_t key;
    uint32_t c;
} input_t;

typedef struct termline_s termline_t;

// input handlers; called sequentially each consumes/handles some number of keystrokes/inputs; when no handler consumes an input, it is discarded
// when a number of inputs is consumed by a different handler or an input is discarded and unhandle is defined, it is called on that
typedef struct {
    // - line: the termline to make changes upon
    // - t: for reference only; the content currently displayed
    // - lowest_change: write the lowest byte you've altered in line->s here, if it's lower than the current value of *lowest_change
    // - inputs: the keystrokes to handle; the handler should consume a numer of keystrokes that are of kind this handler cares about
    // - len: the numer of inputs
    // returns: the number of inputs handled
    int (*handle)(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len);
    void (*unhandle)(termline_t *line, input_t *inputs, int len);
} handler_t;

tu_implement_arr(handler_arr, handler_t)

typedef struct {
    str_t s, e; // original, edited
    bool edited;
} histrec_t;

tu_implement_arr(hist, histrec_t)

struct termline_s {
    char *s; // the entered text
    int len, cap;
    int cursor, mark; // the selection is from mark to cursor; no selection when mark < 0
    bool mark_weak; // like just after yanking when moving around removes selection; meaningless when mark < 0

    int magnet; // the column where the cursor "magnets" towards when navigating up/down; -1 for current cursor culumn

    int char_search; // usually 0; after CTRL+]: +1 after ALT+CTRL+]: -1

    lhrec_arr_t lh; // inline history (undo/redo)
    int lh_idx;
    int lhrec_type; // one of LHREC_*; lhrec is saved when we get an lhrec of a different type or LHREC_INDEP
                    // to prevent saving like after each small edit

    hist_t hist; // command history
    int hist_idx;

    int hist_search; // 0 when not in search, 1 for forward, -1 for backwards
    str_t search; // search string
    bool search_success;

    FILE *in, *out;
    str_t prompt;
    str_t nl_prompt;

    str_arr_t killring;
    int kr_idx;

    handler_arr_t handlers;

    void (*user_callback)(struct termline_s* line, const content_t *t, int *lowest_change, bool text_changed, bool cursor_changed);
    color_arr_t highlights;
    str_t hint; // inline hint shown at the cursor; is set to len=0 when in search mode
    str_t preview; // lone shown at the bottom; just not displayed when in search mode
};

// go to history entry i and save the current one
void tl_hist(termline_t *line, int i) {
    if(i == line->hist_idx) return;
    if(line->hist.len <= line->hist_idx) {
        hist_stretch(&line->hist, line->hist_idx + 1);
        memset(line->hist.p + line->hist.len, 0, (line->hist_idx + 1 - line->hist.len) * sizeof(histrec_t));
        line->hist.len = line->hist_idx + 1;
    }
    line->hist.p[line->hist_idx].e.len = 0;
    str_append(&line->hist.p[line->hist_idx].e, line->s, line->len);
    line->hist.p[line->hist_idx].edited = true;
    line->len = 0;
    str_t h = line->hist.p[i].edited ? line->hist.p[i].e : line->hist.p[i].s;
    str_append((str_t*)line, h.s, h.len);
    line->mark = -1;
    line->cursor = line->len;
    line->hist_idx = i;
}

void tl_set_prompt(termline_t *line, const char *s, int len) {
    line->prompt.len = 0;
    str_append(&line->prompt, s, len);
}

void tl_set_nl_prompt(termline_t *line, const char *s, int len) {
    line->nl_prompt.len = 0;
    str_append(&line->nl_prompt, s, len);
}

// types of modifications an lhrec could be saved after
#define LHREC_INIT           0
#define LHREC_INSERT_WORD    1
#define LHREC_INSERT_NONWORD 2
#define LHREC_KILL_WORD      3
#define LHREC_KILL_NONWORD   4
#define LHREC_YANK           5
#define LHREC_HIST           6
#define LHREC_INDEP          7

void tl_lhrec(termline_t *line, int type, bool advanse) {
    if(line->lhrec_type != type || type == LHREC_INDEP) {
        for(int i = line->lh_idx; i < line->lh.len; i ++) {
            free(line->lh.p[i].s);
//            printf("\r\nfreeing #%i: %p\r\n", i, line->lh.p[i].s);
        }
        line->lh.len = line->lh_idx;
        lhrec_t rec = *(lhrec_t*)line;
        str_sovereign((str_t*)&rec);
        lhrec_arr_append(&line->lh, &rec, 1);
        if(advanse) line->lh_idx ++;
    }
    ret:
    line->lhrec_type = type;
}

#define TERM_SEQ_INVALID -1
#define TERM_SEQ_UNKNOWN  0

#define TERM_SEQ_CSI      1
#define TERM_SEQ_SS2      2
#define TERM_SEQ_SS3      3
#define TERM_SEQ_UTF8     4

#define KEY_UNKNOWN    (uint32_t)-2
#define KEY_INVALID_CP (uint32_t)-1
#define KEY_HOME           0x200001
#define KEY_INSERT         0x200002
#define KEY_DELETE         0x200003
#define KEY_END            0x200004
#define KEY_PAGEUP         0x200005
#define KEY_PAGEDOWN       0x200006
#define KEY_F1             0x20000B
#define KEY_F2             0x20000C
#define KEY_F3             0x20000D
#define KEY_F4             0x20000E
#define KEY_F5             0x20000F
#define KEY_F6             0x200011
#define KEY_F7             0x200012
#define KEY_F8             0x200013
#define KEY_F9             0x200014
#define KEY_F10            0x200015
#define KEY_F11            0x200017
#define KEY_F12            0x200018
#define KEY_F13            0x200019
#define KEY_F14            0x20001A
#define KEY_F15            0x20001C
#define KEY_F16            0x20001D
#define KEY_F17            0x20001F
#define KEY_F18            0x200020
#define KEY_F19            0x200021
#define KEY_F20            0x200022
#define KEY_ENTER          0x200100
#define KEY_BACKSPACE      0x200101
#define KEY_ESCAPE         0x200102
#define KEY_UP             0x200201
#define KEY_DOWN           0x200202
#define KEY_RIGHT          0x200203
#define KEY_LEFT           0x200204

#define NON_CHAR           0x3E00000
#define NON_MOD            0x03FFFFF

#define MOD_CTRL           0x0400000
#define MOD_ALT            0x0800000
#define MOD_SHIFT          0x1000000
#define MOD_NUMPAD         0x2000000

input_t parse_CSI(char *_s, int len, bool *finished) {
    *finished = true;
    unsigned char *s = (unsigned char*)_s + (*_s != '\233') + 1;
    len -= (*_s != '\233') + 1;
    if(len == 1 && *s == '[')
        { *finished = false; return (input_t){ .type = TERM_SEQ_CSI, .s = s, .len = len }; }
    if(len >= 2 && *s == '[' && 'A' <= s[2] && s[2] <= 'E')
        { *finished = true; return (input_t){ .type = TERM_SEQ_CSI, .s = s, .len = 2 }; }
    // ^ linux TTY sequences for F1-F5
    unsigned char *c = s;
    for(; c - s < len && 0x30 <= *c && *c < 0x40; c ++);
    if(c - s < len && !(0x40 <= *c && *c <= 0x7E)) // invalid termination
        return (input_t){ .type = TERM_SEQ_INVALID };
    if(c - s >= len) *finished = false;
    return (input_t){ .type = TERM_SEQ_CSI, .s = s, .len = c - s + 1 };
}

input_t parse_esc_seq(char *s, int len, bool *finished) {
    *finished = true;
    bool is_CSI = (*s == '\33' && len >= 2 && s[1] == '[') ||  // regular CSI
                  (*s == '\233' && len >= 1)                ||  // CSI char (\x9B)
                  (*s == '\302' && len >= 2 && s[1] == '\233'); // CSI char in utf8
    bool is_SS2 = (*s == '\33' && len >= 2 && s[1] == 'N') ||
                  (*s == '\216' && len >= 1)                ||
                  (*s == '\302' && len >= 2 && s[1] == '\216');
    bool is_SS3 = (*s == '\33' && len >= 2 && s[1] == 'O') ||
                  (*s == '\217' && len >= 1)                ||
                  (*s == '\302' && len >= 2 && s[1] == '\217');
    if(*s != '\33' && *s != '\233' && *s != '\216' && *s != '\217' && *s != '\302') goto err;
    if(len == 1) { *finished = false; return (input_t){ .type = TERM_SEQ_UNKNOWN }; }
    if(!is_CSI && !is_SS2 && !is_SS3) goto err;
    if(is_CSI) return parse_CSI(s, len, finished);
    if(is_SS2 || is_SS3) {
        unsigned char *s_ = (unsigned char*)s + (*s != (is_SS2 ? '\216' : '\217')) + 1;
        int l = len - (*s != (is_SS2 ? '\216' : '\217')) - 1;
        if(l <= 0) {
            *finished = false;
            return (input_t){ .type = is_SS2 ? TERM_SEQ_SS2 : TERM_SEQ_SS3, .s = s_, .len = 0 };
        }
        if(*s_ < 0x20 || *s_ >= 0x80) return (input_t){ .type = TERM_SEQ_INVALID };
        return (input_t){ .type = is_SS2 ? TERM_SEQ_SS2 : TERM_SEQ_SS3, .s = s_, .len = 1 };
    }
    err:
    return (input_t){ .type = TERM_SEQ_INVALID };
}

char *tu_step_input(char *s, int len, input_t *in) {
    bool finished;
    *in = parse_esc_seq(s, len, &finished);
    if(!finished) return NULL;
    if(in->type == TERM_SEQ_CSI || in->type == TERM_SEQ_SS3) {
        if(in->len == 2 && in->s[0] == '[' && 'A' <= in->s[1] && in->s[1] <= 'E')
            { in->key = KEY_F1 + in->s[1] - 'A'; goto ret; }
        if(in->len > 5)
            { in->key = KEY_UNKNOWN; goto ret; }
        int k, n, m, modifiers;
        if(in->len == 1) { k = 1; modifiers = 0; goto parse; }
        char buf[5] = {0};
        memcpy(buf, in->s, in->len - 1);
        int r = sscanf(buf, "%i%n", &k, &n);
        if(r == 1 && n == in->len - 1) { modifiers = 0; goto parse; }
        r = sscanf(buf, "%i;%i%n", &k, &m, &n);
        if(r == 2 && n == in->len - 1) {
            if(m == 5) modifiers = MOD_CTRL;
            else if(m == 2) modifiers = MOD_SHIFT;
            else if(m == 6) modifiers = MOD_CTRL | MOD_SHIFT;
            else if(m == 3) modifiers = MOD_ALT;
            else { in->key = KEY_UNKNOWN; goto ret; }
        }
        parse:
        char c = in->s[in->len - 1];
        if(c == '~') {
            if(1 <= k && k <= 8 || 11 <= k && k <= 34 && k != 16 && k != 22 && k != 27 && k != 30)
                in->key = (k == 7 ? KEY_HOME : k == 8 ? KEY_END : KEY_HOME + k - 1) | modifiers;
            else in->key = KEY_UNKNOWN;
        }
        else if(('P' <= c && c <= 'S') && k == 1) in->key = (KEY_F1 + (c - 'P')) | modifiers;
        else if(c == 'H' && k == 1) in->key = KEY_HOME  | modifiers;
        else if(c == 'F' && k == 1) in->key = KEY_END   | modifiers;
        else if(c == 'M' && k == 1) in->key = KEY_ENTER | modifiers | (in->type == TERM_SEQ_SS3 ? MOD_NUMPAD : 0);
        else if('A' <= c && c <= 'D' && k == 1) in->key = (KEY_UP + c - 'A') | modifiers | (in->type == TERM_SEQ_SS3 ? MOD_NUMPAD : 0);
        else if('j' <= c && c <= 'y' && k == 1) in->key = (uint32_t)(c - 'j' + '*') | modifiers | (in->type == TERM_SEQ_SS3 ? MOD_NUMPAD : 0);
        else in->key = KEY_UNKNOWN;
        ret:
        return in->s + in->len;
    } else if(in->type != TERM_SEQ_INVALID) {
        in->key = KEY_UNKNOWN;
        return in->s + in->len;
    }
    bool esc = *s == '\33';
    s += esc; len -= esc;
    if(len == 0) return NULL;
    char *s_ = step_utf8_cp_partial(s, len, &in->c);
//    printf("\r\n[%i]", s_ - s);
    if(s_ > s + len) return NULL;
    in->s = s;
    in->type = TERM_SEQ_UTF8;
    in->len = s_ - s;
    if(s_ == s) {
        if(esc) goto esc;
        in->key = KEY_INVALID_CP;
        in->len = 1;
        return s + 1;
    }
//    if((in->c < 0x20 || in->c == 127) && esc) goto esc;
    if((in->c == '\33') && esc) goto esc;
    if(in->c == '\r') in->key = KEY_ENTER;
    else if(in->c == '\33') in->key = KEY_ESCAPE;
    else if(in->c == '\010') in->key = KEY_BACKSPACE | MOD_CTRL;
    else if(in->c < 0x20) in->key = (in->c + '@') | MOD_CTRL;
    else if(in->c == '\177') in->key = KEY_BACKSPACE;
    else in->key = in->c;
    if(esc) in->key |= MOD_ALT;
    return s_;
    esc:
    in->s = s - 1;
    in->len = 1;
    in->key = KEY_ESCAPE;
    return s;
}

int tl_cont2text(termline_t *line, int cursor_byte) {
    int b = cursor_byte - line->prompt.len;
    if(b == 0) return 0;
    int i;
    for(i = 0; i < line->len; i ++) {
        b --;
        if(line->s[i] == '\n') b -= line->nl_prompt.len;
        if(b <= 0) break;
    }
    assert(b == 0);
    return i + 1;
}

bool is_color_reset(char *csi, int len) {
    len -= 1 + (*csi != '\233');
    csi += 1 + (*csi != '\233');
    return len == 1 && *csi == 'm' ||
           len == 2 && *csi == '0' && csi[1] == 'm';
}

// text byte to content byte mapping; does not account for hint
int tl_text2cont(termline_t *line, int text_byte, color_arr_t *colors) {
    int newlines = 0;
    int color_bytes = 0;
    int color_bytes_skipped = 0;
    int color_bytes_total = 0;
    int fi = 0;
    for(int i = 0; i < text_byte; i ++) {
        if(line->s[i] == '\n') {
            newlines ++;
            if(colors) {
                for(; fi < colors->len && colors->p[fi].at <= i; fi ++) {
                    color_bytes += colors->p[fi].len;
                    if(is_color_reset(colors->p[fi].s, colors->p[fi].len)) color_bytes_skipped = color_bytes;
                }
                color_bytes_total += (sizeof TERM_COLOR_RESET - 1) * (color_bytes != color_bytes_skipped);
                color_bytes_total += color_bytes - color_bytes_skipped;
            }
        }
    }
    if(colors)
        for(; fi < colors->len && colors->p[fi].at < text_byte; fi ++) color_bytes += colors->p[fi].len;
    color_bytes_total += color_bytes;
    return line->prompt.len + text_byte + newlines * line->nl_prompt.len + color_bytes_total;
}

void tl_insert(termline_t *line, bool before_cursor, int at, char *s, int len) {
    str_stretch((str_t*)line, line->len + len);
    memmove(line->s + at + len, line->s + at, line->len - at);
    line->len += len;
    memcpy(line->s + at, s, len);
    if(line->cursor > at || line->cursor == at && before_cursor) {
        line->cursor += len;
        line->magnet = -1;
    }
    if(line->mark > at) {
        line->mark += len;
        line->magnet = -1;
    }
}

void tl_kill(termline_t *line, int at, int len, bool to_killring) {
    str_t s = str_substr((str_t*)line, at, len);
    if(s.len == 0) return;
    if(to_killring) {
        str_t s_ = s;
        str_sovereign(&s_);
        str_arr_append(&line->killring, &s_, 1);
        line->kr_idx = -1;
    }
    at = s.s - line->s;
    if(line->cursor > at) line->magnet = -1;
    if(line->cursor > at && line->cursor <= at + s.len) line->cursor = at;
    else if(line->cursor > at + s.len) line->cursor -= s.len;
    if(line->mark > at && line->mark <= at + s.len) line->mark = at;
    else if(line->mark > at + s.len) line->mark -= s.len;
    memmove(s.s, s.s + s.len, line->len - at - s.len);
    line->len -= s.len;
}

// returns length increment (neg. for dec.)
int tl_case(termline_t *line, int at, int len, int _case) {
    str_t s = str_substr((str_t*)line, at, len);
    int len_ = tu_case_len(s.s, s.len, _case);
    str_stretch((str_t*)line, line->len + len_ - len);
    char *_s = s.s;
    str_sovereign(&s);
    memmove(_s + len_, _s + s.len, line->s + line->len - _s - s.len);
    line->len += len_ - len;
    int ptrs[] = { line->s + line->cursor - _s, line->s + line->mark - _s };
    tu_case(_s, s.s, s.len, _case, ptrs, 2);
    line->cursor = _s + ptrs[0] - line->s;
    line->mark = _s + ptrs[1] - line->s;
    free(s.s);
    return len_ - len;
}

// colors have to be sorted
void tl_compose(termline_t *line, int start, str_t *out, color_arr_t *colors, bool include_prompt, bool include_decorations) {
    str_stretch(out, line->len - start + include_prompt * line->prompt.len);
    out->len = include_prompt * line->prompt.len;
    if(include_prompt) memcpy(out->s, line->prompt.s, line->prompt.len);
    int newlines = 0;
    char *s = line->s;
    int len = line->len;
    int fi = 0;
    int ri = 0;
    if(colors) {
        while(fi < colors->len && colors->p[fi].at < start) {
            if(is_color_reset(colors->p[fi].s, colors->p[fi].len)) ri = fi + 1;
            fi ++;
        }
    }
    for(int i = start;; i ++) {
        if(colors) {
            while(fi < colors->len && colors->p[fi].at == i) {
                str_append(out, colors->p[fi].s, colors->p[fi].len);
                if(is_color_reset(colors->p[fi].s, colors->p[fi].len)) ri = fi + 1;
                fi ++;
            }
        }
        if(i >= len) break;
        if(i == line->cursor && include_decorations)
            str_append(out, line->hint.s, line->hint.len);
        str_append(out, s + i, 1);
        if(s[i] == '\n') {
            if(colors && ri < fi) str_append_lit(out, TERM_COLOR_RESET);
            str_append(out, line->nl_prompt.s, line->nl_prompt.len);
            if(colors)
                for(int i = ri; i < fi; i ++) str_append(out, colors->p[i].s, colors->p[i].len);
        }
    }
    if(line->cursor == line->len && include_decorations) str_append(out, line->hint.s, line->hint.len);
    if(!include_decorations) return;
    str_append_lit(out, TERM_COLOR_RESET);
    if(line->hist_search) {
        if(line->hist_search > 0) str_append_lit(out, "\nforward search: ");
        else                      str_append_lit(out, "\nbackward search: ");
        if(!line->search_success) str_append_lit(out, TERM_COLOR_RED_BACKGROUND);
        str_append(out, line->search.s, line->search.len);
        if(!line->search_success) str_append_lit(out, TERM_COLOR_RESET);
        str_append_lit(out, "_");
    } else str_append(out, line->preview.s, line->preview.len);
}

bool tu_is_wordy(char c, int big) {
    return ('0' <= c && c <= '9') ||
           ('A' <= c && c <= 'Z') ||
           ('a' <= c && c <= 'z') ||
           c == '_' ||
           (unsigned char)c >= 0x80 ||
           (unsigned char)c > 0x20 && c != '\377' && big;
}

int tl_handle_text(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    str_t temp = { NULL };
    for(; len > 0; len --, inputs ++) {
        if(inputs->key & NON_CHAR && inputs->key != KEY_ENTER && inputs->key != KEY_INVALID_CP) break;
        char c = inputs->key != KEY_ENTER ? *inputs->s : '\n';
        tl_lhrec(line, tu_is_wordy(c, false) ? LHREC_INSERT_WORD : LHREC_INSERT_NONWORD, true);
        str_append(&temp, inputs->key != KEY_ENTER ? inputs->s : "\n", inputs->len);
    }
    if(len != _len) {
        if(line->cursor < *lowest_change) *lowest_change = line->cursor;
        tl_insert(line, true, line->cursor, temp.s, temp.len);
    }
    free(temp.s);
    return _len - len;
}

int search_back_word(char *s, int byte, bool big) {
    if(byte == 0) return 0;
    int got_word = false;
    for(int i = byte - 1; i >= 0; i --) {
        if(tu_is_wordy(s[i], big)) got_word = true;
        else if(got_word) return i + 1;
    }
    return 0;
}

int search_forward_word(char *s, int len, int byte, bool big) {
    int got_word = false;
    for(int i = byte; i < len; i ++) {
        if(tu_is_wordy(s[i], big)) got_word = true;
        else if(got_word) return i;
    }
    return len;
}

void tl_move_h(termline_t *line, int move) {
    int _cb = line->cursor;
    if(move >= 0) {
        if(line->cursor >= line->len) return;
        int e;
        tu_item_boundary(line->s, line->len, line->cursor, NULL, &e, NULL);
        int b = e, w = 0;
        while(e < line->len && w == 0) tu_item_boundary(line->s, line->len, e, &b, &e, &w);
        line->cursor = w == 0 ? e : b;
    } else {
        int b = line->cursor, e = line->cursor, w = 0;
        while(b > 0 && w == 0) tu_item_boundary(line->s, line->len, b - 1, &b, &e, &w);
        line->cursor = b;
    }
    if(line->cursor != _cb) line->magnet = -1;
}

void tl_move_v(termline_t *line, const content_t *t, int move) {
    cursor_t cursor = { .byte = tl_text2cont(line, line->cursor, NULL) };
    cursor_restore(t, &cursor, CURSOR_FROM_BYTE);
    if(line->magnet < 0) line->magnet = cursor.col;
    int _row = cursor.row;
    cursor.row += move;
    cursor.col = line->magnet;
    cursor_restore(t, &cursor, CURSOR_FROM_COL);
//    printf("\r\n%i\r\n", cursor.byte);
    int prompt_len = (cursor.row == 0 ? line->prompt.len : line->nl_prompt.len);
    if(cursor.row_byte < prompt_len) {
        cursor.byte += prompt_len - cursor.row_byte;
    }
    if(cursor.row == _row) {
//        cursor_restore(t, &cursor, CURSOR_FROM_BYTE);
//    printf("\r\n%i\r\n", cursor.byte);
//        printf("\r\n[%i] {%.*s}\r\n", t->len, t->len, t->s);
        line->magnet = cursor.col;
    }
    line->cursor = tl_cont2text(line, cursor.byte);
}

void tl_move_H(termline_t *line, int move) {
    if(move >= 0 && (line->cursor >= line->len || line->s[line->cursor] == '\n')) return;
    if(move < 0 && (line->cursor <= 0 || line->s[line->cursor - 1] == '\n')) return;
    for(; line->cursor >= 0 && (line->cursor < line->len || move < 0); line->cursor += move)
        if(line->s[line->cursor] == '\n') break;
    if(move < 0) line->cursor ++;
    line->magnet = -1;
}

//#include <time.h>
//
//long _time() {
//    struct timespec ts;
//    timespec_get(&ts, TIME_UTC);
//    return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
//}

int tl_handle_arrows(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    content_t _t = *t;
    _t.s = NULL; _t.len = 0; _t.cap = 0;
    int _len = len;
    int _cb = line->cursor;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if((k == KEY_UP || k == KEY_DOWN) && _t.s == NULL) tl_compose(line, 0, (str_t*)&_t, NULL, true, false);
        if((key & NON_CHAR) == 0 && line->char_search) {
            if(line->char_search > 0) {
                if(line->cursor >= line->len) goto no_found;
                int b, e;
                tu_item_boundary(line->s, line->len, line->cursor, &b, &e, NULL);
                while(e < line->len) {
                    tu_item_boundary(line->s, line->len, e, &b, &e, NULL);
                    uint32_t cp;
                    char *cp_e = step_utf8_cp(line->s + b, line->len - b, &cp);
                    if(cp_e == line->s + e && cp == key) { line->cursor = b; break; }
                }
            }
            else {
                if(line->cursor == 0) goto no_found;
                int b = line->cursor, e;
                while(b > 0) {
                    tu_item_boundary(line->s, line->len, b - 1, &b, &e, NULL);
                    uint32_t cp;
                    char *cp_e = step_utf8_cp(line->s + b, line->len - b, &cp);
                    if(cp_e == line->s + e && cp == key) { line->cursor = b; break; }
                }
            }
            no_found:
            line->char_search = 0;
        }
        else if(k == KEY_UP    && (key & MOD_CTRL) == 0) tl_move_v(line, &_t, -1);
        else if(k == KEY_DOWN  && (key & MOD_CTRL) == 0) tl_move_v(line, &_t, 1);
        else if(k == KEY_RIGHT && (key & MOD_CTRL) || (k == 'f' || k == 'F') && (key & MOD_ALT)) line->cursor = search_forward_word(line->s, line->len, line->cursor, false);
        else if(k == KEY_LEFT  && (key & MOD_CTRL) || (k == 'b' || k == 'B') && (key & MOD_ALT)) line->cursor = search_back_word(line->s, line->cursor, false);
        else if(k == KEY_LEFT  || k == 'B' && (key & MOD_CTRL)) tl_move_h(line, -1);
        else if(k == KEY_RIGHT || k == 'F' && (key & MOD_CTRL)) tl_move_h(line, 1);
        else if(k == KEY_HOME  || k == 'A' && (key & MOD_CTRL)) tl_move_H(line, -1);
        else if(k == KEY_END   || k == 'E' && (key & MOD_CTRL)) tl_move_H(line, 1);
        else if(k == ']' && (key & MOD_CTRL)) line->char_search = (key & MOD_ALT) ? -1 : +1;
        else break;
        if(_cb != line->cursor) line->lhrec_type = LHREC_INIT;
    }
    free(_t.s);
    return _len - len;
}

// todo: process Ctrl+J (\n)

int tl_handle_kills(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    int at = line->cursor, l = 0;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
//        printf("\r\n[%x]\r\n", key);
        if((k == 'w' || k == 'W') && (key & MOD_ALT)) {
            if(line->mark >= 0 & line->mark != line->cursor) {
                tl_lhrec(line, LHREC_INDEP, true);
                if(line->mark < at) { tl_kill(line, line->mark, at - line->mark, true); at = line->mark; }
                if(at + l < line->mark) { tl_kill(line, at + l, line->mark - at - l, true); }
                line->mark = -1;
            }
        } else if(k == 'W' && (key & MOD_CTRL) || k == KEY_BACKSPACE && (key & MOD_ALT)) {
            if(at > 0) {
                int i = search_back_word(line->s, at, key == ('W' | MOD_CTRL));
                tl_lhrec(line, LHREC_INDEP, true);
                tl_kill(line, i, at - i, true);
                at = i;
            }
        } else if((k == 'd' || k == 'D') && (key & MOD_ALT)) {
            if(at + l < line->len) {
                int i = search_forward_word(line->s, line->len, at + l, false);
                tl_lhrec(line, LHREC_INDEP, true);
                tl_kill(line, at + l, i - at - l, true);
            }
        } else if(k == 'U' && (key & MOD_CTRL)) {
            if(at > 0) {
                int i;
                for(i = at; i --> 0;) if(line->s[i] == '\n') break;
                i ++;
                tl_lhrec(line, LHREC_INDEP, true);
                tl_kill(line, i, at - i, true);
                at = i;
            }
        } else if(k == 'K' && (key & MOD_CTRL)) {
            if(at + l < line->len && line->s[at + l] != '\n') {
                int i;
                for(i = at; i < line->len; i ++) if(line->s[i] == '\n') break;
                tl_lhrec(line, LHREC_INDEP, true);
                tl_kill(line, at + l, i - at - l, true);
            }
        } else if(k == KEY_DELETE || k == 'D' && (key & MOD_CTRL)) {
            if(at + l < line->len) {
                tl_lhrec(line, tu_is_wordy(line->s[at + l], false) ? LHREC_KILL_WORD : LHREC_KILL_NONWORD, true);
                int e;
                tu_item_boundary(line->s, line->len, at + l, NULL, &e, NULL);
                int b = e, w = 0;
                while(e < line->len && w == 0) tu_item_boundary(line->s, line->len, e, &b, &e, &w);
                l = (w == 0 ? e : b) - at;
            }
        } else if(k == KEY_BACKSPACE || k == 'H' && (key & MOD_CTRL)) {
            if(at > 0) tl_lhrec(line, tu_is_wordy(line->s[at - 1], false) ? LHREC_KILL_WORD : LHREC_KILL_NONWORD, true);
            int b = at, e = at, w = 0;
            while(b > 0 && w == 0) tu_item_boundary(line->s, line->len, b - 1, &b, &e, &w);
            l += at - b;
            at = b;
        } else break;
    }
    tl_kill(line, at, l, false);
    if(len != _len && line->cursor < *lowest_change) *lowest_change = line->cursor;
    return _len - len;
}

int tl_handle_yanks(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if(k == '@' && (key & MOD_CTRL)) {
            line->lhrec_type = LHREC_INIT;
            if(line->mark >= 0 && line->mark < *lowest_change) *lowest_change = line->mark;
            line->mark = line->cursor >= 0 ? line->cursor : 0;
            line->mark_weak = false;
        } else if(k == 'G' && (key & MOD_CTRL)) {
            line->lhrec_type = LHREC_INIT;
            line->mark = -1;
        } else if((k == 'y' || k == 'Y') && (key & MOD_CTRL || key & MOD_ALT)) {
            if(line->killring.len == 0) goto no_yank;
            if(line->mark_weak && (key & MOD_ALT) == 0) { line->mark = -1; line->kr_idx = -1; }
            tl_lhrec(line, LHREC_YANK, true);
            if(line->kr_idx < 0) line->kr_idx = line->killring.len - 1;
            str_t y = line->killring.p[line->kr_idx];
            if(line->mark >= 0) {
                int b = line->mark > line->cursor ? line->cursor : line->mark;
                int e = line->mark > line->cursor ? line->mark : line->cursor;
                str_stretch((str_t*)line, line->len + y.len - (e - b));
                if(e < line->len) memmove(line->s + b + y.len, line->s + e, line->len - e);
                line->len += y.len - (e - b);
                memcpy(line->s + b, y.s, y.len);
                if(b < *lowest_change) *lowest_change = b;
                line->mark = b;
                line->cursor = b + y.len;
            } else {
                str_stretch((str_t*)line, line->len + y.len);
                if(line->cursor < line->len) memmove(line->s + line->cursor + y.len, line->s + line->cursor, line->len - line->cursor);
                line->len += y.len;
                memcpy(line->s + line->cursor, y.s, y.len);
                if(line->cursor < *lowest_change) *lowest_change = line->cursor;
                line->mark = line->cursor;
                line->cursor += y.len;
            }
            line->kr_idx --;
            if(line->kr_idx < 0) line->kr_idx = line->killring.len - 1;
            line->mark_weak = true;
            no_yank:;
        } else if((k == 'x' || k == 'X') && (key & MOD_CTRL || key & MOD_ALT)) {
            line->lhrec_type = LHREC_INIT;
            if(line->mark < 0) line->mark = (key & MOD_ALT) ? line->len : 0;
            if(line->mark >= 0) {
                line->cursor ^= line->mark;
                line->mark ^= line->cursor;
                line->cursor ^= line->mark;
            }
            line->mark_weak = false;
        } else if(k == KEY_ESCAPE) {
            line->lhrec_type = LHREC_INIT;
            line->mark = -1;
        } else break;
    }
    return _len - len;
}

void tl_unhandle_yanks(termline_t *line, input_t *inputs, int len) { if(line->mark_weak) line->mark = -1; }

int tl_handle_swaps(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if(k == 'T' && (key & MOD_CTRL)) {
            if(line->len >= 2) {
                tl_lhrec(line, LHREC_INDEP, true);
                int cb = line->cursor;
                if(cb <= 0) cb = 1;
                if(cb >= line->len) cb = line->len - 1;
                line->s[cb] ^= line->s[cb - 1];
                line->s[cb - 1] ^= line->s[cb];
                line->s[cb] ^= line->s[cb - 1];
                line->cursor = cb + 1;
                if(cb <= *lowest_change) *lowest_change = cb - 1;
            }
        } else if((k == 't' || k == 'T') && (key & MOD_ALT)) {
            int i = search_back_word(line->s, line->cursor, k == 'T');
            i = search_back_word(line->s, i, k == 'T');
            int e1 = search_forward_word(line->s, line->len, i, k == 'T');
            int e2 = search_forward_word(line->s, line->len, e1, k == 'T');
            int s1 = search_back_word(line->s, e1, k == 'T');
            int s2 = search_back_word(line->s, e2, k == 'T');
            if(s2 >= e1) {
                tl_lhrec(line, LHREC_INDEP, true);
                char *s = malloc(e2 - s1);
                memcpy(s, line->s + s2, e2 - s2);
                memcpy(s + e2 - s2, line->s + e1, s2 - e1);
                memcpy(s + e2 - e1, line->s + s1, e1 - s1);
                memcpy(line->s + s1, s, e2 - s1);
                free(s);
                if(s1 < *lowest_change) *lowest_change = s1;
                line->cursor = e2;
            }
        } else break;
    }
    return _len - len;
}

int tl_handle_controls(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if(k == 'L' && (key & MOD_CTRL)) {
            int r = term_check_resize(t->in, t->out, &t->width, &t->height);
            if(r < 0) t->error = CONT_ERR_GETSIZE;
            t->resize_pending = true;
        } else if(k == 'C' && (key & MOD_CTRL)) {
            term_unset_raw(line->in, line->out);
            exit(0); // todo: proper handling
        } else break;
    }
    return _len - len;
}

int tl_handle_case(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if((k == 'c' || k == 'C' || k == 'u' || k == 'U' || k == 'l' || k == 'L') && (key & MOD_ALT)) {
            tl_lhrec(line, LHREC_INDEP, true);
            int b = -1, E;
            if(line->mark >= 0 && line->mark != line->cursor) {
                b = line->mark > line->cursor ? line->cursor : line->mark;
                E = line->mark > line->cursor ? line->mark : line->cursor;
            } else if(line->cursor < line->len) {
                b = line->cursor;
                E = search_forward_word(line->s, line->len, line->cursor, false);
            }
            if(b >= 0) {
                int e;
                utf8_cp_boundary(line->s, line->len, b, &b, &e, NULL);
                if(k == 'c' || k == 'C') {
                    int c = tl_case(line, b, e - b, CASE_UPPER);
                    tl_case(line, e + c, E - e, CASE_LOWER);
                } else {
                    tl_case(line, b, E - b, k == 'u' || k == 'U' ? CASE_UPPER : CASE_LOWER);
                }
                if(*lowest_change > b) *lowest_change = b;
            }
        } else break;
    }
    return _len - len;
}

int tl_handle_lh(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    int _lh_idx = line->lh_idx;
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if(k == '_' && (key & MOD_CTRL)) {
//            printf("\r\n[%i]", line->lh_idx);
            if(line->lh_idx > 0) {
                tl_lhrec(line, LHREC_INIT, false);
                line->lh_idx --;
            }
        } else if(k == '^' && (key & MOD_CTRL)) {
            if(line->lh_idx < line->lh.len - 1) {
//                printf("\r\n[%i %i %i]\r\n", line->lh_idx, line->lh.len, line->lhrec_type);
                tl_lhrec(line, LHREC_INIT, false);
                line->lh_idx ++;
            }
        } else break;
    }
    if(_lh_idx != line->lh_idx) {
//        if(line->mark >= 0) tl_kill(line, line->mark > line->cursor ? line->cursor : line->mark, line->mark > line->cursor ? line->mark - line->cursor : line->cursor - line->mark);
//        line->mark = line->cursor;
//        tl_insert(line, true, line->cursor,
        free(line->s);
        memcpy(line, line->lh.p + line->lh_idx, sizeof(lhrec_t));
        str_sovereign((str_t*)line);
        *lowest_change = 0;
    }
    return _len - len;
}

int str_search(char *s, int len, char *sub, int sub_len) {
    if(sub_len == 0) return 0;
    for(int i = 0; i <= len - sub_len; i ++) {
        if(memcmp(s + i, sub, sub_len) == 0) return i;
    }
    return -1;
}

int tl_handle_hist(termline_t *line, content_t *t, int *lowest_change, input_t *inputs, int len) {
    int _len = len;
    bool multiline = false;
    for(int i = 0; i < line->len; i ++) multiline |= (line->s[i] == '\n');
    for(; len > 0; len --, inputs ++) {
        uint32_t key = inputs->key;
        uint32_t k = key & NON_MOD;
        if((k == 'N' || k == 'P' || k == KEY_UP || k == KEY_DOWN) && (key & MOD_CTRL) ||
           (k == KEY_UP || k == KEY_DOWN) && !multiline) {
            int move = k == 'N' || k == KEY_DOWN ? +1 : -1;
            if(line->hist_idx + move < line->hist.len && line->hist_idx + move >= 0) {
                tl_lhrec(line, LHREC_HIST, true);
                tl_hist(line, line->hist_idx + move);
//                printf("\r\n%i\r\n", multiline);
                *lowest_change = 0;
            }
        } else if((k == 'R' || k == 'S') && (key & MOD_CTRL)) {
            if(line->hist_search == 0) line->search.len = 0;
            if(!line->hist_search) tl_lhrec(line, LHREC_INDEP, true);
            line->hist_search = k == 'R' ? -1 : +1;
            line->hint.len = 0;
            goto do_search;
        } else if(line->hist_search && (key & NON_CHAR) == 0) {
            char c = (char)key;
            str_append(&line->search, &c, 1);
            goto do_search;
        } else if(line->hist_search && line->search.len > 0 && key == KEY_BACKSPACE) {
            line->search.len --;
            goto do_search;
        } else if(line->hist_search && k == 'G' && (key & MOD_CTRL)) {
            line->hist_search = 0;
        } else break;
        continue;
        do_search:
        line->search_success = false;
        for(int i = line->hist_idx; 0 <= i && i < line->hist.len; i += line->hist_search) {
            str_t h = line->hist_idx == i ? *(str_t*)line : line->hist.p[i].edited ? line->hist.p[i].e : line->hist.p[i].s;
            int r = str_search(h.s, h.len, line->search.s, line->search.len);
            if(r >= 0) {
                if(i != line->hist_idx) {
                    *lowest_change = 0;
                    tl_hist(line, i);
                }
                line->cursor = r;
                line->search_success = true;
                break;
            }
        }
    }
    return _len - len;
}

void tl_unhandle_hist(termline_t *line, input_t *inputs, int len) { line->hist_search = 0; }

int tl_process_input(termline_t *line, content_t *t, input_t *inputs, int len) {
    int lowest_change = line->len;
    int _len = line->len;
    int _hint_len = line->hint.len;
    int _mark = line->mark;
    int _cb = line->cursor;
    while(len > 0) {
        int consumed = 0;
        int by = -1;
        for(int i = 0; i < line->handlers.len; i ++) {
            consumed = line->handlers.p[i].handle(line, t, &lowest_change, inputs, len);
            if(consumed) { by = i; break; }
        }
        if(consumed == 0) consumed = 1;
        for(int i = 0; i < line->handlers.len; i ++) {
            if(line->handlers.p[i].unhandle && i != by) line->handlers.p[i].unhandle(line, inputs, consumed);
        }
        inputs += consumed;
        len -= consumed;
    }
    bool text_changed = lowest_change != line->len || lowest_change != _len;
    if(line->user_callback) {
        if(!line->hist_search) line->user_callback(line, t, &lowest_change, text_changed, line->cursor != _cb);
        if(line->hint.len && line->cursor < lowest_change) lowest_change = line->cursor;
        if(_hint_len && _cb < lowest_change) lowest_change = _cb;
    }
    if(line->cursor != _cb || line->mark != _mark) {
        if(_cb          < lowest_change) lowest_change = _cb;
        if(line->cursor < lowest_change) lowest_change = line->cursor;
        if(_mark      >= 0 && _mark      < lowest_change) lowest_change = _mark;
        if(line->mark >= 0 && line->mark < lowest_change) lowest_change = line->mark;
    }
    return lowest_change;
}

termline_t tl_create(FILE *in, FILE *out) {
    termline_t line = {
        .in = in,
        .out = out,
        .mark = -1,
        .kr_idx = -1
    };
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_hist, .unhandle = tl_unhandle_hist }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_lh }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_case }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_controls }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_swaps }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_yanks, .unhandle = tl_unhandle_yanks }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_kills }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_arrows }, 1);
    handler_arr_append(&line.handlers, &(handler_t){ .handle = tl_handle_text }, 1);
    return line;
}

void tl_interact(termline_t *line) {
    content_t t = content_create(line->in, line->out);
    color_arr_t colors = { NULL };
    cursor_t cursor = { .byte = line->prompt.len };
    t.cursor_byte = cursor.byte;
    content_change(&t, 0, line->prompt.s, line->prompt.len);
    str_t temp = { .s = NULL, .len = 0, .cap = 0 };
    for(;;) {
        int x = term_get_pos(stdout, stdin, &t.input_buf).x;
////        printf("\r\n%i\r\n", x);
//        printf("\33[9999D\33[20C");
////        for(int i = 0; i < line->len; i ++) printf("%X ", (unsigned char)line->s[i]);
//        for(int i = 0; i < t.len; i ++) printf("%X ", (unsigned char)t.s[i]);
//        printf("\33[9999D\33[%iC", x - 1);
//        printf("\r\n");
//        fflush(stdout);
        content_wait_in(&t);
        char *s = t.input_buf.s;
        int len = t.input_buf.len;
        int i;
        input_t *inputs = malloc(len * sizeof(input_t));
        int n = 0;
        for(i = 0; i < len;) {
            char *s_ = tu_step_input(s + i, len - i, inputs + n);
            if(s_ == NULL) break;
            n ++;
            i = s_ - s;
        }
        int _cursor = line->cursor;
        int lowest_change = tl_process_input(line, &t, inputs, n);
//        if(lowest_change > _cursor) lowest_change = _cursor;
        free(inputs);
        memmove(s, s + i, len - i);
        t.input_buf.len -= i;
        colors.len = 0;
        if(line->mark >= 0 && line->mark != line->cursor) {
            color_arr_append(&colors, (color_t[]){
                    { .s = TERM_COLOR_UNDERLINE, .len = sizeof TERM_COLOR_UNDERLINE - 1, .at = line->mark > line->cursor ? line->cursor : line->mark },
                    { .s = TERM_COLOR_RESET    , .len = sizeof TERM_COLOR_RESET     - 1, .at = line->mark > line->cursor ? line->mark : line->cursor }
            }, 2);
        }
        color_arr_append(&colors, line->highlights.p, line->highlights.len);
        color_sort(colors.p, colors.len);
        temp.len = 0;
        tl_compose(line, lowest_change, &temp, &colors, false, true);
        t.cursor_byte = tl_text2cont(line, line->cursor, &colors);
        int at = tl_text2cont(line, lowest_change, &colors);
        if(lowest_change > line->cursor) at += line->hint.len;
        content_change(&t, at, temp.s, temp.len);
    }
    ret:
    free(temp.s);
}

