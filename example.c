// This is example usage of Termline library
// All callbacks are optional, jump straight to `int main` to see the main code

#define TERMLINE_IMPLEMENTATION 1
#include "term.c"

#include <locale.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

char *help_message =
"Most keybinds are bash/zsh -like, with some differences\n"
"Keybinds:\n"
"Any Unicode or non-control ASCII: Insert in place of cursor and move\n"
"                                  to the right\n"
"Ctrl+V then KEY: Insert KEY as raw bytes\n"
"Enter: Insert a newline or stop editing, depending on the return of a\n"
"       user-defined callback; In this example it's to stop on\n"
"       Alt+Enter; On regular Enter insert a newline when in multiline\n"
"       mode or after a \\ at the end of the line, otherwise stop\n"
"Backspace or Ctrl+H: Delete character to the left of cursor\n"
"Delete or Ctrl+D (on non-empty line): Delete character under cursor\n"
"Ctrl+U: Kill from cursor to beginning of line\n"
"Ctrl+K: Kill from cursor to end of line\n"
"Alt+w: Kill selected text\n"
"Ctrl+W: Kill from cusor to Word (space-delimited) beginning\n"
"Alt+Backspace: Kill from cusor to word ([0-9a-zA-Z_] and any\n"
"               non-ASCII Unicode) beginning\n"
"Alt+D: Kill from cusor to word ([0-9a-zA-Z_] and any non-ASCII\n"
"       Unicode) end\n"
"Ctrl+2 or Ctrl+@ or Ctrl+Space: Set mark, denoting a boundary of\n"
"                                selection\n"
"Ctrl+X, Alt+x: Swap cursor with mark; if one does not exist, place\n"
"               it at the beginning or at the end respectively\n"
"Ctrl+Y: Paste and select text from last entry of killring, ignoring\n"
"        existing selection\n"
"Alt+y: Paste and select text, scrolling through killring, replacing\n"
"       existing selection\n"
"Ctrl+T: Swap character under cursor with one to the left and move\n"
"        right\n"
"Alt+t, Alt+T: Swap word ([0-9a-zA-Z_] and any non-ASCII Unicode) or\n"
"              Word (space-delimited) under cursor with one to the\n"
"              left and move right, respectively\n"
"Ctrl+C: Stop editing, leaving the appearence as is (hints,\n"
"        selection etc)\n"
"Ctrl+L: Rerender all content\n"
"Ctrl+D: On empty line: exit; On non-empty line: delete character\n"
"        under cursor (like Delete)\n"
"Alt+c, Alt+u, Alt+l: Capitalize, Uppercase or Lowercase a word\n"
"                     (starting at cursor) or selection (if one exists)\n"
"Ctrl+7 or Ctrl+_ or Ctrl+/: Undo last edit\n"
"Ctrl+6 or Ctrl+^: Redo last Undo\n"
"Left or Ctrl+B: Move cursor left 1 character\n"
"Right or Ctrl+F: Move cursor right 1 character\n"
"Home or Ctrl+A: Move cursor to line beginning\n"
"End or Ctrl+E: Move cursor to line end\n"
"Ctrl+] then CHAR: Move cursor to first appearence of CHAR to the right\n"
"Alt+Ctrl+] then CHAR: Move cursor to first appearence of CHAR to the\n"
"                      left\n"
"Up, Down: In multiline mode: Navigate through lines; In single-line\n"
"          mode: Scroll history\n"
"Ctrl+P or Ctrl+Up: Scroll history up\n"
"Ctrl+P or Ctrl+Down: Scroll history down\n"
"Ctrl+R, Ctrl+S: Search history backwards or forwards respectively\n"
"Tab, Shift+Tab: Insert the completion if just one is available, or\n"
"                scroll through completions forwards or backwards\n"
"                respectively\n"
"Clrl+G: Remove selection, exit tab menu or search mode\n";

char *hinted_words[] = { "Lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "Ut", "enim", "ad", "minim", "veniam", "quis", "nostrud", "exercitation", "ullamco", "laboris", "nisi", "aliquip", "ex", "ea", "commodo", "consequat", "Duis", "aute", "irure", "in", "reprehenderit", "voluptate", "velit", "esse", "cillum", "eu", "fugiat", "nulla", "pariatur", "Excepteur", "sint", "occaecat", "cupidatat", "non", "proident", "sunt", "culpa", "qui", "officia", "deserunt", "mollit", "anim", "id", "est", "laborum" };

// find possible completions at the cursor out of hinted_words, write indices into word_idxs,
// write length of the already inputted part into *match_len, return the count of completions found
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
    return r;
}

char counter_text[128];
char hint_text[128];

// this callback sets hint, preview and colors
// returns 0 when nothing changed, 1 when just preview or text or colors changed, 2 when hint changed
int callback(termline_t *line, int *lowest_change, bool text_changed, bool cursor_changed) {
    static bool selection = false; // tracks whether we had selection (mark) on previous run
    bool selection_ = line->mark >= 0;
    bool any_changed = text_changed || // change in "bytes inputted"
                       cursor_changed && (selection_ || selection) || // change in "bytes selected"
                       selection != selection_; // change in presence of "bytes selected"

    // === hint ===
    bool hint_changed = (text_changed || cursor_changed) && line->hint.len;
    if(hint_changed) line->hint.len = 0;
    int word_idxs[sizeof hinted_words / sizeof(char*)];
    int match_len;
    int n = get_completions(line, word_idxs, &match_len);
    if(n == 1) {
        if(line->hint.len == 0) hint_changed = true;
        char *word = hinted_words[word_idxs[0]];
        int len = strlen(word);
        line->hint.len = sprintf(hint_text, "\33[2m%.*s\33[22m", len - match_len, word + match_len);
        line->hint.s = hint_text;
    }

    if(!any_changed) goto ret;
    // === preview ===
    if(line->len == 0) {
        line->preview.len = 0;
    } else {
        if(selection_) {
            int len = line->mark > line->cursor ? line->mark - line->cursor : line->cursor - line->mark;
            line->preview.len = sprintf(counter_text, "\n\33[2mbytes inputted: %i; bytes selected: %i", line->len, len);
        } else line->preview.len = sprintf(counter_text, "\n\33[2mbytes inputted: %i", line->len);
        line->preview.s = counter_text;
    }

    // === highlights ==
    char *highlighted_words[] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white" };
    char *highlights[] = { "\33[40m", "\33[41m", "\33[42m", "\33[43m", "\33[44m", "\33[45m", "\33[46m", "\33[47m" };
    int max_len = 7; // magenta
    int rehighlight_from = *lowest_change - max_len + 1; // rebuilding highlights after this byte
    if(rehighlight_from < 0) rehighlight_from = 0;
    for(int i = 0; i < line->highlights.len; i ++) {
        tu_color_t *h = line->highlights.p + i;
        // removing all highlight after a color introduction (nonzero prio) after rebuild point
        if(h->at >= rehighlight_from && h->prio) {
            line->highlights.len = i;
            if(i < *lowest_change) *lowest_change = i;
            break;
        }
    }
    int n_words = sizeof highlighted_words / sizeof(char*);
    for(int i = rehighlight_from; i <= line->len; i ++) {
        // outer loop through positions to keep ordering for easy trimming
        for(int j = 0; j < n_words; j ++) {
            char *word = highlighted_words[j];
            int len = strlen(word);
            if(i + len > line->len) continue;
            if(memcmp(line->s + i, word, len) == 0) {
                if(i < *lowest_change) *lowest_change = i;
                tu_color_arr_append(&line->highlights, &(tu_color_t){
                        .s = highlights[j],
                        .len = strlen(highlights[j]),
                        .at = i,
                        .prio = 2500 // > 2000 to introduce color after hint which is at 2000 (see hint_prio in termline_t)
                                     // though practically hint won't even appear at the same byte as color introduction
                }, 1);
                tu_color_arr_append(&line->highlights, &tu_color("\33[0m", i + len, 0), 1); // resetting color at 0 priority (asap)
            }
        }
    }
    ret:
    selection = selection_;
    return hint_changed ? 2 : any_changed;
}

// tab completion options
void tab_callback(termline_t *line, const tu_input_t *inp) {
    // makeing Tab insert "    " indent when at line beginning
    bool at_line_beginning = true;
    for(int i = line->cursor - 1; i >= 0 && line->s[i] != '\n'; i --)
        at_line_beginning &= line->s[i] == ' ';
    if(at_line_beginning) {
        tl_add_tab_compl(line, "    ", 4, 0);
        return;
    }
    // otherwise searching for known words
    int word_idxs[sizeof hinted_words / sizeof(char*)];
    int match_len;
    int n = get_completions(line, word_idxs, &match_len);
    for(int i = 0; i < n; i ++) {
        char *word = hinted_words[word_idxs[i]];
        int len = strlen(word);
        tl_add_tab_compl(line, word, len, match_len);
    }
}

bool enter_callback(termline_t *line, const tu_input_t *enter) {
    if(enter->key & TU_MOD_ALT) return true;
    // repeating indent
    int line_begin = line->cursor;
    while(line_begin > 0 && line->s[line_begin - 1] != '\n') line_begin --;
    int space_count = 0; // indent depth
    for(int i = line_begin; i < line->len && line->s[i] == ' '; i ++) space_count ++;
    char *indent = malloc(space_count + 1);
    indent[0] = '\n';
    memset(indent + 1, ' ', space_count);
    tl_set_newline(line, indent, space_count + 1); // setting `indent` to be inserted insted of '\n'
    free(indent);

    if(line->cursor == line->len && line->len > 0 && line->s[line->len - 1] == '\\') return false;
    bool multiline = false;
    for(int i = 0; i < line->len; i ++) multiline |= line->s[i] == '\n';
    return !multiline;
}

int backspace_callback(termline_t *line, const tu_input_t *backspace) {
    int line_begin = line->cursor;
    while(line_begin > 0 && line->s[line_begin - 1] != '\n') line_begin --;
    bool is_indent = true;
    for(int i = line_begin; i < line->cursor; i ++) is_indent &= line->s[i] == ' ';
    if(is_indent && line_begin < line->cursor)
        return (line->cursor - line_begin) > 4 ? 4 : (line->cursor - line_begin);
    return -1;
}

int main(void) {
    setlocale(LC_ALL, ""); // necessery for the function wcwidth to operate properly
    termline_t line = tl_create(stdin, stdout);
    line.callback = callback;
    line.tab_callback = tab_callback;
    line.enter_callback = enter_callback;
    line.backspace_callback = backspace_callback;
    line.prompt = tu_str("> ");
    line.nl_prompt = tu_str(". ");
    printf("Termline by BusyBeaver\nTry entering Lorem ipsum or colors (red, green, ...)\nType `help` to see keybinds\nType `exit` or use Ctrl+D to exit\n");
    for(;;) {
        tl_interact(&line);
        if(line.exit_reason == TL_EXIT_ERROR) {
            fprintf(stderr, "%s\n", term_error_names[line.error]);
            break;
        } else if(line.exit_reason == TL_EXIT_INTERRUPT) {
            continue;
        } else if(line.exit_reason == TL_EXIT_EOF) {
            printf("exit\n");
            break;
        } else if(line.exit_reason == TL_EXIT_ENTER && line.len > 0) {
            printf("bytes inputted: %i\n", line.len);
            tl_hist_add(&line, line.s, line.len);
        }
        if(line.len == 4 && memcmp(line.s, "help", 4) == 0) printf("%s", help_message);
        if(line.len == 4 && memcmp(line.s, "exit", 4) == 0) break;
    }
    tl_free(&line);
//    printf("malloc count: %i\n", mm);
    return 0;
}

