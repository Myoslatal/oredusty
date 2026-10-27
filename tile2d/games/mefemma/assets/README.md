# Mefemma's assets

Everything the game reads at run time, copied beside the executable by the build and carried by the
package:

    assets/
        config/game.ecfg      the shell's settings (window, language, seed; a game adds its own keys)
        text/ui.ecfg          the interface strings, one table per language
        textures/             pictures a DrawList::sprite() names, as <name>.png
        fonts/                optional fonts shipped with the game

## Pictures

A scene asks for a picture **by name**; the shell loads `textures/<name>.png` on first use and caches it.
That is the whole convention: one picture is one texture, any size, no atlas, no manifest. A scene that
asks for a picture that is not there gets a marked box drawn in its place and a warning in the log, so a
missing file is visible rather than silent.

`textures/placeholder.png` is the title scene's picture and is a **placeholder**: replace it with the
game's own art and change the name in `src/scenes/title_scene.cpp` (or keep the name and change the file).

## Strings

Text is looked up by id (`services.text("title.start")`). A language is a top-level table named after its
code: `en`, `zh-Hans`, `zh-Hant`. A missing id falls back to English and then to the id itself, so a
missing string is visible as an identifier, never as an empty label. Add a language by adding a table; the
engine already knows how to draw it.

## Fonts

The shell loads a system font (the first of a list of usual Linux locations) and a CJK collection for
Chinese; `--font` and `--cjk-font` override either. A game that ships its own font puts it under
`assets/fonts/` and points a settings or command line at it.
