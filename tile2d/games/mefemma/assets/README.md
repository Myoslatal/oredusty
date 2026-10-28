# Mefemma's assets

Everything the game reads at run time, copied beside the executable by the build and carried by the
package:

    assets/
        config/game.ecfg      the shell's settings (window, language, seed; a game adds its own keys)
        text/ui.ecfg          the interface strings, one table per language
        textures/             pictures a DrawList::sprite() names, as <name>.png
        fonts/                optional fonts shipped with the game

## The start screen

`textures/start_screen.png` is the wooden sign the start screen is drawn on (1920x1080, transparent
outside the sign). The scene places the game name and the five rows **in the picture's own coordinates** -
the paper face runs from (272, 220) to (1581, 833) - and maps them onto the window, so the menu stays on
the paper at every size and aspect ratio. Those numbers live at the top of
`src/scenes/title_scene.cpp`; replacing the picture with one of another shape means changing them.

The picture is drawn with **linear** filtering (a painted sign, not a tile); pictures are nearest by
default, and a row asks for linear with the last argument of `DrawList::sprite()`.

## Pictures

A scene asks for a picture **by name**; the shell loads `textures/<name>.png` on first use and caches it.
That is the whole convention: one picture is one texture, any size, no atlas, no manifest. A scene that
asks for a picture that is not there gets a marked box drawn in its place and a warning in the log, so a
missing file is visible rather than silent.

## Strings

Text is looked up by id (`services.text("start.new_game")`). A language is a top-level table named after its
code: `en`, `zh-Hans`, `zh-Hant`. A missing id falls back to English and then to the id itself, so a
missing string is visible as an identifier, never as an empty label. Add a language by adding a table; the
engine already knows how to draw it. The shipped settings select `zh-Hans`; `--lang en` or F2 switches.

## Fonts

The shell loads a system font (the first of a list of usual Linux locations) and a CJK collection for
Chinese; `--font` and `--cjk-font` override either. A game that ships its own font puts it under
`assets/fonts/` and points a settings or command line at it.
