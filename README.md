This is a single-header cross-platform C library for creating prompt-based TUIs (a GNU readline analogue). Bash/Zsh -like keystrokes are supported. Most important features are:

 - Inline suggestion right at the cursor (called "hint") and preview under the prompt (like in Node)
 - Tab-completions with scrollable options when multiple are available
 - Coloring/formatting of the text with ANSI codes ("highlights")
 - Backward/foward history search
 - Text selection
 - Undo/redo (note, GNU readline only supports undo)
 - Killring (buffer of "killed" chunks of text that one can paste back in)
 - High resistance against falling apart on window resize

To test these features, compile and run example.c. Typing `help` will show all supported keystrokes.

Tested on:
- (on GNU/linux): gnu terminal, xfce terminal, xterm, non-graphical linux TTY
- (on Windows 11): builtin command line (cmd.exe), mintty (resizes do inevitably break in my understanding because mintty creates a virtual native console and missinchronizes resizes with it, mintty is to blame)

## API

Both header and implementation are in "term.c" file. To get the implementation, define TERMLINE_IMPLEMENTATION to 1 before including it.

On POSIX, you should place "term.c" include before any standard includes because it uses _XOPEN_SOURCE and _POSIX_C_SOURCE feature set definitions which must go before standard includes, or just copy those out.

The header includes almost all internal functions, most of which the user does not need. All capabilities user needs for basic use case are shown in example.c.

Central user-facing object is the `termline_t` structure. It represents the editable line with prompt and everything. It operates on `content_t` underneeth, which represents the dynamic (changable) part of text, but only knows text as byte array with no structue.

Basic workflow is as follows:
Create `termline_t` object with `tl_create(stdin, stdout)`. Set `.prompt` and `.nl_prompt` fields on it, and callbacks as needed. Use `tl_interact` to run the interactive prompt. Then check `.exit_reason`; check `.s` and `.len` to get the inputted text. Add the entered text to history with `tl_hist_add(line, line->s, line->len)` (where `line` points to the `termline_t` structure), or clear history with `tl_hist_clear`. When done, use `tl_free` to deallocate all allocations in the `termline_t` instance.

For detailed descriptions see comments in term.c and the example.

On POSIX you should also `setlocale(LC_ALL, "")` so that the wcwidth function (used internally) operates correctly according to the locale. On Window, custom locale-invariant implementation of wcwidth is used

### Ai disclosure

All code is 100% hand-written; LLMs were used for consulting only

### Lisense

Use this code for any intends and purpuses; Attribution to the author is highly desirable but not mandatory

