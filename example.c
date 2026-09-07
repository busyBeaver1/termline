#include "term.c"

char *hinted_words[] = { "Lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "Ut", "enim", "ad", "minim", "veniam", "quis", "nostrud", "exercitation", "ullamco", "laboris", "nisi", "aliquip", "ex", "ea", "commodo", "consequat", "Duis", "aute", "irure", "in", "reprehenderit", "voluptate", "velit", "esse", "cillum", "eu", "fugiat", "nulla", "pariatur", "Excepteur", "sint", "occaecat", "cupidatat", "non", "proident", "sunt", "culpa", "qui", "officia", "deserunt", "mollit", "anim", "id", "est", "laborum" };

// find possible completions at the cursor out of hinted_words, write indices into word_idxs,
// write length of the already inputed part into *match_len, return the count of completions found
int get_completions(termline_t *line, int *word_idxs, int *match_len) {
    if(line->cursor < line->len && line->s[line->cursor] != ' ' && line->s[line->cursor] != '\n') return 0; // no completions mid-word
    for(int i = line->cursor - 2; i >= -1; i --) {
        if(i < 0 || line->s[i] == ' ' || line->s[i] == '\n') { *match_len = line->cursor - i - 1; goto got_word; }
    }
    return 0;
    got_word:
    int r = 0;
    for(int i = 0; i < sizeof hinted_words / sizeof(char*); i ++) {
        char *word = hinted_words[i];
        int len = strlen(word);
        if(len <= *match_len) continue;
        if(memcmp(line->s + line->cursor - *match_len, word, *match_len) == 0)
            word_idxs[r ++] = i;
    }
//    printf("\r\n%i\r\n", r);
    return r;
}

char counter_text[128];
char hint_text[128];

int callback(termline_t *line, const content_t *t, int *lowest_change, bool text_changed, bool cursor_changed) {
    static bool selection = false;
    bool selection_ = line->mark >= 0;
    int r = text_changed || cursor_changed && (selection_ || selection) || selection != selection_;
    if(text_changed || cursor_changed) { if(line->hint.len) r = 2; line->hint.len = 0; }
    int word_idxs[sizeof hinted_words / sizeof(char*)];
    int match_len;
    int n = get_completions(line, word_idxs, &match_len);
    if(n == 1) {
        char *word = hinted_words[word_idxs[0]];
        int len = strlen(word);
        line->hint.len = sprintf(hint_text, "\33[2m%.*s\33[22m", len - match_len, word + match_len);
        line->hint.s = hint_text;
    }
    if(r) {
        if(line->len == 0) {
            line->preview.len = 0;
        } else {
            if(selection_) {
                int len = line->mark > line->cursor ? line->mark - line->cursor : line->cursor - line->mark;
                line->preview.len = sprintf(counter_text, "\nbytes inputed: %i; bytes selected: %i", line->len, len);
            } else line->preview.len = sprintf(counter_text, "\nbytes inputed: %i", line->len);
            line->preview.s = counter_text;
        }
    }
    selection = selection_;
    return r;
}

void tab_callback(termline_t *line, const input_t *inp) {
    int word_idxs[sizeof hinted_words / sizeof(char*)];
    int match_len;
    int n = get_completions(line, word_idxs, &match_len);
    for(int i = 0; i < n; i ++) {
        char *word = hinted_words[word_idxs[i]];
        int len = strlen(word);
        tl_add_tab_compl(line, word, len, match_len);
    }
}

int main(void) {
    termline_t line = tl_create(stdin, stdout);
    line.callback = callback;
    line.tab_callback = tab_callback;
    line.prompt = tu_str("> ");
    line.nl_prompt = tu_str(". ");
    tl_interact(&line);
}

