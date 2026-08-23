#include "term.c"
#include <locale.h>

int main(void) {
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
    return 0;
}

