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

#define _TERM_UNIX    1
#define _TERM_WINDOWS 2

#if defined _WIN32 || defined _WIN64
#define _TERM_SYSTEM _TERM_WINDOWS
#elif defined __unix__ ||| define __APPLE__
#define _TERM_SYSTEM _TERM_UNIX
#else
#error Unknown system
#endif

#if _TERM_SYSTEM == _TERM_UNIX
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

bool _default_ts_set = false;

#if _TERM_SYSTEM == _TERM_UNIX
struct termios _default_ts;

// put terminal into raw mode
// returns 0 on success, -1 on error
int term_set_raw(FILE *in) {
    int infd = fileno(in);
    if(infd < 0) return -1;
    struct termios ts = _default_ts;
    if(!_default_ts_set) {
        int r = tcgetattr(infd, &ts);
        if(r) return -1;
        _default_ts_set = true;
        _default_ts = ts;
    }
    ts.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    ts.c_oflag &= ~OPOST;
    ts.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    ts.c_cflag &= ~(CSIZE | PARENB);
    ts.c_cflag |= CS8;
    r = tcsetattr(infd, 0, &ts);
    if(r) return -1;
    return 0;
}

int term_unset_raw(FILE *in) {
    if(!_default_ts_set) return -1;
    return tcsetattr(fileno(in), 0, &_default_ts);
}
#else
DWORD _default_ts_in;
DWORD _default_ts_out;

HANDLE _file2handle(FILE *f) {
    int fd = _fileno(f);
    if(fd == -1) return INVALID_HANDLE_VALUE;
    return (HANDLE)(intptr_t)_get_osfhandle(fd);
}

int term_set_raw(FILE *in, FILE *out) {
    HANDLE inhd = _file2handle(in);
    if(inhd == INVALID_HANDLE_VALUE) return -1;
    HANDLE outhd = _file2handle(out);
    if(outhd == INVALID_HANDLE_VALUE) return -1;
    DWORD ints = _default_ts_in;
    DWORD outts = _default_ts_out;
    if(!_default_ts_set) {
        if(!GetConsoleMode(inhd, &ints)) return -1;
        if(!GetConsoleMode(outhd, &outts)) return -1;
        _default_ts_set = true;
        _default_ts_in = ints;
        _default_ts_out = outts;
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
    if(!_default_ts_set) return -1;
    HANDLE inhd = _file2handle(in);
    if(inhd == INVALID_HANDLE_VALUE) return -1;
    HANDLE outhd = _file2handle(out);
    if(outhd == INVALID_HANDLE_VALUE) return -1;
    if(!SetConsoleMode(inhd, _default_ts_in)) return -1;
    if(!SetConsoleMode(outhd, _default_ts_out)) return -1;
    return 0;
}
#endif

#define TERM_CURSOR_SAVE       "\0337"
#define TERM_CURSOR_RESTORE    "\0338"
#define TERM_CURSOR_HIDE       "\033[?25l"
#define TERM_CURSOR_SHOW       "\033[?25h"
#define TERM_REQUEST_CURSOR    "\033[6n"
//#define TERM_REQUEST_SIZE      "\033[19t"
#define TERM_CURSOR_TO         "\033[%i;%iH"
#define TERM_CURSOR_RIGHT      "\033[%iC"
#define TERM_CURSOR_LEFT       "\033[%iD"
#define TERM_CLEAR_LINE_RIGHT  "\033[K"
#define TERM_CLEAR_SCREEN_DOWN "\033[J"
#define TERM_COLOR_RESET       "\033[0m"
#define TERM_COLOR_INVERSE     "\033[7m"
#define TERM_COLOR_UNINVERSE   "\033[27m"
#define TERM_CURSOR_NEWLINE    "\033[B\033[G" // does not push line unlike \r\n

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

#if _TERM_SYSTEM == _TERM_WINDOWS
int term_get_size(FILE *in, FILE *out, int *width, int *height) {
    int outfd = _fileno(out);
    if(outfd == -1) { printf("1\r\n"); return -1; }
    HANDLE outhd = (HANDLE)(intptr_t)_get_osfhandle(outfd);
    if(outhd == INVALID_HANDLE_VALUE) { printf("2\r\n"); return -1; }
    CONSOLE_SCREEN_BUFFER_INFO info;
    if(!GetConsoleScreenBufferInfo(outhd, &info)) { printf("3\r\n"); return -1; }
    if(width) *width = info.dwSize.X;
    if(height) *height = info.dwSize.Y;
    //printf("ok\r\n");
    return 0;
}
#else
int term_get_size(FILE *in, FILE *out, int *width, int *height) {
    struct winsize ws;
    if(ioctl(fileno(in), TIOCGWINSZ, &ws) < 0) return -1;
    if(width) *width = ws.ws_col;
    if(height) *height = ws.ws_row;
    return 0;
}
#endif

#if _TERM_SYSTEM == _TERM_UNIX

int term_check_resize(FILE *in, FILE *out, int *w, int *h) {
    if(!_winch) return 0;
    _winch = false;
    if(term_get_size(in, out, w, h) < 0) return -1;
    return 1;
}

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

int _read_avail(FILE *in, str_t *dst) {
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

struct timespec _ms2ts(unsigned long t) {
    return (struct timespec){
        .tv_sec = t / 1000,
        .tv_nsec = (t % 1000) * 1000000
    };
}

int term_wait_resize_or_in(FILE *in, str_t *input_buf, unsigned long timeout) {
    int rb = _read_avail(in, input_buf);
    if(rb) return 0;
    int infd = fileno(in);
    sigset_t mask;
    if(sigprocmask(SIG_BLOCK, NULL, &mask) < 0) return -1; // read current mask
    sigdelset(&mask, SIGWINCH); // not block SIGWINCH
    while(!_winch) {
        fd_set fs, fse;
        FD_ZERO(&fs);      FD_ZERO(&fse);
        FD_SET(infd, &fs); FD_SET(infd, &fse);
        int _errno = errno;
        struct timespec ts = _ms2ts(timeout);
        int r = pselect(infd + 1, &fs, NULL, &fse, timeout >= 0 ? &ts : NULL, &mask); // wait for any change on `in` with `mask` as temporary signal mask
        if(r == 1 && FD_ISSET(infd, &fs) && !FD_ISSET(infd, &fse)) {
            int rb = _read_avail(in, input_buf);
            return rb == 0 ? -1 : 0;
        }
        if(r == -1 && errno == EINTR) { errno = _errno; continue; }
        return -1;
    }
    _winch = false;
    return 1;
}

int _gets(FILE *in, char *dst)  {
    int c = fgetc(in);
    if(c == EOF) return 0;
    *dst = (char)c;
    return 1;
}

int _fwrite_utf8(char *s, int len, FILE *out) {
    return fwrite(s, 1, len, out);
}

#else

typedef struct {
    wchar_t *s;
    int len, size;
} wstr_t;

void wstr_stretch(wstr_t *s, int size) {
    if(s->size >= size) return;
    if(s->size <= 0) s->size = 1;
    while(s->size < size) s->size *= 2;
    wchar_t *new_s = malloc(s->size * sizeof(wchar_t));
    memcpy(new_s, s->s, s->len * sizeof(wchar_t));
    free(s->s);
    s->s = new_s;
}

void wstr_push(wstr_t *s, wchar_t c) {
    wstr_stretch(s, s->len + 1);
    s->s[s->len ++] = c;
}

int _encode_wchars(str_t *dst, wchar_t *s, int len) {
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
    HANDLE inhd = _file2handle(in);
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
        wstr_t s = { .s = NULL, .len = 0, .size = 0 };
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
                //printf("%i %i\r\n", p->y, p->x);
            } else if(irs[i].EventType == KEY_EVENT) {
                KEY_EVENT_RECORD ke = irs[i].Event.KeyEvent;
                if(!ke.bKeyDown || ke.uChar.UnicodeChar == 0) continue;
                for(int j = 0; j < ke.wRepeatCount; j ++)
                    wstr_push(&s, ke.uChar.UnicodeChar);
                //printf("%i %i %i\r\n", (int)ke.uChar.UnicodeChar, (int)ke.bKeyDown, (int)ke.wRepeatCount);
            }
        }
        if(high_surrogate) {
            if(s.len && 0xD800 <= s.s[s.len - 1] && s.s[s.len - 1] < 0xDC00)
                *high_surrogate = s.s[-- s.len];
            else *high_surrogate = 0;
        }
        if(input_buf && _encode_wchars(input_buf, s.s, s.len) < 0) goto err;
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

int _gets(FILE *in, char *dst) {
    HANDLE inhd = _file2handle(in);
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

int _fwrite_utf8(char *s, int len, FILE *out) {
    if(len == 0) return 0;
    HANDLE outhd = _file2handle(out);
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
    for(int i = 0; i < 64; i ++) {
        char buf[128];
        int bytes_read = 0;
        //printf("==\r\n");
        int k;
        while(k = _gets(in, buf + bytes_read)) {
            bytes_read += k;
            for(int j = bytes_read - k; j < bytes_read; j ++) {
                //printf("%i\r\n", buf[j]);
                if(j >= sizeof buf) goto err;
                if(j == 0 && buf[j] != '\033') goto err;
                if(j == 1 && buf[j] != '[') goto err;
                if(buf[j] == 'R') goto ok;
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
    if(fprintf(out, TERM_REQUEST_CURSOR) < 0 ||
       fflush(out) < 0) return (pos_t){ .x = -1, .y = -1 };
    return term_read_pos(out, in, input_buf);
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

#if _TERM_SYSTEM == _TERM_WINDOWS
// copying behavior of the wcwidth python package
const uint32_t _wcwidth_indices[] = {
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

const int _wcwidth_widths[] = {
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

int _wcwidth(uint32_t c) {
    int l = 0;
    int u = sizeof _wcwidth_indices / sizeof(uint32_t) - 1;
    if(_wcwidth_indices[u] <= c) return _wcwidth_widths[u];
    while(u - l > 1) {
        int m = (u + l) / 2;
        if(_wcwidth_indices[m] <= c) l = m;
        else u = m;
    }
    return _wcwidth_widths[l];
}
#endif

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
    if(*s == '\033' && len >= 3 && s[1] == '[') {
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
    #if _TERM_SYSTEM == _TERM_WINDOWS
    w = _wcwidth(cp);
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

#define _REORIGIN_RESTORE 1
#define _REORIGIN_COMPUTE 2

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
    bool cursor_unwrapped;
    bool resize_pending;
    int error; // gets set to CONT_ERR_* on an error; when non-zero, content is considered corrupted and all content_* funtions on it return immediately
    int reorigin_method;
    #if _TERM_SYSTEM == _TERM_WINDOWS
    wchar_t high_surrogate;
    #endif
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
        .cursor_byte = 0,
        .cursor_unwrapped = false,
        .resize_pending = false,
        #if _TERM_SYSTEM == _TERM_WINDOWS
        .reorigin_method = _REORIGIN_COMPUTE,
        .high_surrogate = 0
        #else
        .reorigin_method = _REORIGIN_RESTORE
        #endif
    };
    if(term_get_size(t.in, t.out, &t.width, &t.height) < 0)
        { t.error = CONT_ERR_GETSIZE; return t; }
    if(fprintf(t.out, "\r" TERM_CURSOR_SAVE) < 0)
        { t.error = CONT_ERR_OUT; return t; }
    //printf("11\r\n");
    t.origin = term_get_pos(t.out, t.in, &t.input_buf).y;
    //printf("22\r\n");
    if(t.origin < 0) t.error = CONT_ERR_GETPOS;
    #if _TERM_SYSTEM == _TERM_UNIX
    if(setup_resize_watch() < 0) t.error = CONT_ERR_SETUP;
    #endif
    return t;
}

//int ii = 1;

// todo: fix comment
// render the content starting at byte `start`
// refuses to write to terminal if a resize is pending (_winch flag)
// in this case one should first call content_resize
void content_render_from(content_t *t, int start) {
    if(t->error || t->resize_pending) return;
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
    //} else str_append_lit(&out, TERM_COLOR_RESET TERM_CURSOR_HIDE TERM_CURSOR_RESTORE "\r");
    } else str_printf(&out, TERM_COLOR_RESET TERM_CURSOR_HIDE TERM_CURSOR_TO, t->origin, 1);
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
    if(t->cursor_byte >= 0) {
        str_printf(&out, TERM_CURSOR_TO TERM_CURSOR_SHOW, cur.y >= 1 ? cur.y : 1, cur.x);
        t->cursor_unwrapped = cur.bouta_wrap;
    } else str_printf(&out, TERM_CURSOR_TO, t->origin >= 1 ? t->origin : 1, 1);
    if(out.len) {
        int r = term_check_resize(t->in, t->out, &t->width, &t->height);
        if(r < 0) { t->error = CONT_ERR_GETSIZE; goto ret; }
        if(r) { t->resize_pending = true; goto ret; }
        //if(fwrite(out.s, 1, out.len, t->out) < 0) t->error = CONT_ERR_OUT;
        if(_fwrite_utf8(out.s, out.len, t->out) < 0) t->error = CONT_ERR_OUT;
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

#if _TERM_SYSTEM == _TERM_WINDOWS
#define sleep_ms Sleep
#else
void sleep_ms(unsigned long t) {
    struct timespec ts = _ms2ts(t);
    nanosleep(&ts, NULL);
}
#endif

pos_t term_rel_pos(int width, bool split_wchars, char *s, int len, int byte) {
    pos_t _p;
    pos_t p = { .x = 1, .y = 0, .bouta_wrap = false };
    char *c = s;
    for(;;) {
        _p = p;
        c = step_item(c, len, &p, width, split_wchars, NULL, NULL, NULL);
        if(c - s > byte) break;
    }
    return _p;
}

// rerender the content assuming the terminal window has been resized
// width and height have to be set before calling this
void content_resize(content_t *t) {
    t->resize_pending = false;
    if(t->error) return;
//    if(term_get_size(t->in, &t->width, &t->height) < 0)
//        { t->error = CONT_ERR_GETSIZE; return; }
    if(t->reorigin_method == _REORIGIN_RESTORE) {
        if(0 && t->cursor_byte >= 0) {
            // workflow:
            // Save current cursor to `p`; then wait a little; if no winch happens; hide, restore origin, request pos, move cursor to `p`, show
            // Only then see the responce for origin pos. This way in time when the cursor is hidden we don't wait for stdin and avoid flickering
            pos_t p = term_get_pos(t->out, t->in, &t->input_buf);
            if(p.x < 0) { t->error = CONT_ERR_GETPOS; return; }
            sleep_ms(1); // makes resizing more stable on gnome terminal (and probably other terminals with auto-rewrapping)
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
            sleep_ms(1); // makes resizing more stable on gnome terminal (and probably other terminals with auto-rewrapping)
        }
    } else if(t->reorigin_method == _REORIGIN_COMPUTE) {
        pos_t pos = term_get_pos(t->out, t->in, &t->input_buf);
        if(t->cursor_byte < 0) t->origin = pos.y;
        else {
            pos_t p = term_rel_pos(t->width, t->split_wchars, t->s, t->len, t->cursor_byte - t->cursor_unwrapped);
            sleep_ms(1);
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
    //printf(TERM_CURSOR_TO "[%i]\r\n", 1, 1, ii ++);
    if(t->error) return;
    int l = t->input_buf.len;
    while(t->input_buf.len == l) {
        //printf(TERM_CURSOR_TO "[%i]\r\n", 1, 1, ii ++);
        int r = term_wait_resize_or_in(t->in, &t->input_buf, t->resize_pending ? 100 : -1, &t->width, &t->height
            #if _TERM_SYSTEM == _TERM_WINDOWS
            , &t->high_surrogate
            #endif
        );
        //exit(0);
        //printf(TERM_CURSOR_TO "[%i]\r\n", 1, 1, ii ++);
        if(r < 0) { t->error = CONT_ERR_WAIT; return; }
        if(r == 1) t->resize_pending = true;
        else while(t->resize_pending) content_resize(t);
    }
    while(t->resize_pending) content_resize(t);
    //if(_read_avail(t->in, &t->input_buf) < 0) t->error = CONT_ERR_IN;
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
//#endif
