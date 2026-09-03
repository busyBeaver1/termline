#include "term.c"
#include <locale.h>

int main(void) {
    setlocale(LC_ALL, "");
    int r = term_set_raw(stdin, stdout);
    termline_t tl = tl_create(stdin, stdout);
    tl_set_prompt(&tl, "> ", 2);
    tl_set_nl_prompt(&tl, ". ", 2);
    tl_interact(&tl);
    return 0;
//    printf("r: %i\r\n", r);
    str_t buf = { .s = NULL, .len = 0, .size = 0 };
    content_t t = content_create(stdin, stdout);
    char *s = "123456789101112131415161718192021222324252627282930\n\n1234567891011121314151617181920212223242526272829301234567891011121314151617181920212223242526272829301234567891011121314151617181920212223242526272829300\xF0\x9F\x98\x80";
    content_change(&t, 0, s, strlen(s));
    while(t.error == 0) {
        content_wait_in(&t);
        break;
    }
    term_unset_raw(stdin, stdout);
    /*
    //term_set_raw(stdin);
    int w, h;
    wchar_t high_surrogate;
    str_t s = { .s = NULL, .len = 0, .size = 0 };
    for(;;) {
        s.len = 0;
        int r = term_wait_resize_or_in(stdin, &s, &w, &h, &high_surrogate);
        printf("r: %i\r\n", r);
        if(r > 0) {
            printf("s:");
            for(int i = 0; i < s.len; i ++) printf(" %i", (int)s.s[i]);
            printf("\r\n");
        }
    }
    setlocale(LC_ALL, "");
    errno = 0;
    term_set_raw(stdin);
    content_t t = content_create();
//     FILE *dump = fopen("./dump", "w");
//     t.out = dump;
    for(;;) {
        FILE *io = fopen("./io", "r");
        if(io == NULL) { fprintf(stderr, "failed to open ./io"); return 1; }
        char buf[1024];
        int n = fread(buf, 1, sizeof(buf) - 1, io);
        buf[sizeof(buf) - 1] = 0;
        if(n && buf[0] == 'r') {
            fwrite(buf + 1, 1, n - 1, t.out);
            fflush(t.out);
            continue;
        }
        int start;
        char *s = parse_uint(buf, &start);
        //content_change(&t, start, s, n - (s - buf), true, start);
        content_change(&t, start, s, n - (s - buf));
        content_wait_in(&t);
        printf("\e[1;1H======[%i]======\r\n", t.error);
        while(data_pending(t.in)) {
            fgetc(t.in);
            data_pending(t.in);
        }
    }
    */
    return 0;
}

