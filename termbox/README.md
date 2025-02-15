
## README

This is a fork of https://github.com/nsf/termbox
as of commit a04f34c11494a1dbfb6286fc95470dea4086c520
"Add a note: project is no longer maintained"

This README has been trimmed down to just the introduction and changelog
(below) which remain unchanged from the upstream version:

## Termbox

Termbox is a library that provides minimalistic API which allows the
programmer to write text-based user interfaces.

It is based on a very simple abstraction. The main idea is viewing terminals as
a table of fixed-size cells and input being a stream of structured
messages. Would be fair to say that the model is inspired by windows console
API. The abstraction itself is not perfect and it may create problems in certain
areas. The most sensitive ones are copy & pasting and wide characters (mostly
Chinese, Japanese, Korean (CJK) characters). When it comes to copy & pasting,
the notion of cells is not really compatible with the idea of text. And CJK
runes often require more than one cell to display them nicely. Despite the
mentioned flaws, using such a simple model brings benefits in a form of
simplicity. And KISS principle is important.

At this point one should realize, that CLI (command-line interfaces) aren't
really a thing termbox is aimed at. But rather pseudo-graphical user interfaces.

### Getting started

Termbox's interface only consists of 12 functions::

```
tb_init() // initialization
tb_shutdown() // shutdown

tb_width() // width of the terminal screen
tb_height() // height of the terminal screen

tb_clear() // clear buffer
tb_present() // sync internal buffer with terminal

tb_put_cell()
tb_change_cell()
tb_blit() // drawing functions

tb_select_input_mode() // change input mode
tb_peek_event() // peek a keyboard event
tb_poll_event() // wait for a keyboard event
```

See src/termbox.h header file for full detail.

### Changes

v1.1.2:

- Properly include changelog into the tagged version commit. I.e. I messed up
  by tagging v1.1.1 and describing it in changelog after tagged commit. This
  commit marked as v1.1.2 includes changelog for both v1.1.1 and v1.1.2. There
  are no code changes in this minor release.

v1.1.1:

- Ncurses 6.1 compatibility fix. See https://github.com/nsf/termbox-go/issues/185.

v1.1.0:

- API: tb_width() and tb_height() are guaranteed to be negative if the termbox
  wasn't initialized.
- API: Output mode switching is now possible, adds 256-color and grayscale color
  modes.
- API: Better tb_blit() function. Thanks, Gunnar Zötl <gz@tset.de>.
- API: New tb_cell_buffer() function for direct back buffer access.
- API: Add new init function variants which allow using arbitrary file
  descriptor as a terminal.
- Improvements in input handling code.
- Calling tb_shutdown() twice is detected and results in abort() to discourage
  doing so.
- Mouse event handling is ported from termbox-go.
- Paint demo port from termbox-go to demonstrate mouse handling capabilities.
- Bug fixes in code and documentation.

v1.0.0:

- Remove the Go directory. People generally know about termbox-go and where
  to look for it.
- Remove old terminfo-related python scripts and backport the new one from
  termbox-go.
- Remove cmake/make-based build scripts, use waf.
- Add a simple terminfo database parser. Now termbox prefers using the
  terminfo database if it can be found. Otherwise it still has a fallback
  built-in database for most popular terminals.
- Some internal code cleanups and refactorings. The most important change is
  that termbox doesn't leak meaningless exported symbols like 'keys' and
  'funcs' now. Only the ones that have 'tb_' as a prefix are being exported.
- API: Remove unsigned ints, use plain ints instead.
- API: Rename UTF-8 functions 'utf8_*' -> 'tb_utf8_*'.
- API: TB_DEFAULT equals 0 now, it means you can use attributes alones
  assuming the default color.
- API: Add TB_REVERSE.
- API: Add TB_INPUT_CURRENT.
- Move python module to its own directory and update it due to changes in the
  termbox library.
