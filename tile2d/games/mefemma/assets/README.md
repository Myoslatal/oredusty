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

The Settings row opens an **internal settings menu** on the same picture: a panel in the paper's
coordinates with a LANGUAGE row (the active language's endonym in angle brackets) and a BACK row. Its
geometry is beside the start screen's at the top of `src/scenes/title_scene.cpp`; it needs no second
texture.

## The settings file vs the save directory's config.ecfg

`config/game.ecfg` is the **shipped default** the shell reads at startup (window, language and seed, plus any
fields the game adds); a package carries it and the game never rewrites it, because the assets directory may
be read-only. The Settings menu's own choices are written to `<save directory>/config.ecfg` instead - the
save directory chosen on first run - so a player's language travels with their saves:

    # Mefemma's settings for this save directory. Written by the Settings menu.
    language:"en"

That file is not an asset and the build does not copy it. Startup precedence is `--lang` >
`<save directory>/config.ecfg` > `config/game.ecfg`; deleting the save-directory file returns to the shipped
default. F2 and the Settings menu both write it, and a failed write is logged without changing the live
language.

## redirect.ecfg is not an asset

The save directory the player picks on first run is stored in `redirect.ecfg` **beside the executable**, not
under `assets/` and not under this directory: it is runtime state, not shipped content. The build does not
copy it and the package does not carry it. The format is one quoted field:

    save_path:"/absolute/path/to/saves"

The first-run prompt writes it; deleting it makes the next run ask again. `--redirect <path>` chooses a
different file and `--save-path <dir>` answers for one run without writing anything. Because `assets/` may
be read-only (a package, a system install), nothing about saving is stored there.

## The open-world test atlas

`textures/test_tiles.png` is the 32x16 atlas the open-world example scene draws: two 16x16 cells side
by side, tile A (id 1, left, warm cross-hatch) and tile B (id 2, right, blue checker). The scene asks for
the picture by name (`"test_tiles"`) and selects a cell with a uv rectangle; it keeps the default
**nearest** filter, because a tile is not a photograph. Replacing the picture with another 32x16 PNG
changes what the two test chunks look like; the left-half / right-half split belongs to the scene
(`src/scenes/open_world_scene.cpp`).

## Pictures

A scene asks for a picture **by name**; the shell loads `textures/<name>.png` on first use and caches it.
That is the whole convention: one picture is one texture, any size, no atlas, no manifest. A scene that
asks for a picture that is not there gets a marked box drawn in its place and a warning in the log, so a
missing file is visible rather than silent.

## Strings

Text is looked up by id (`services.text("start.new_game")`). A language is a top-level table named after its
code: `en`, `zh-Hans`, `zh-Hant`. A missing id falls back to English and then to the id itself, so a
missing string is visible as an identifier, never as an empty label. Add a language by adding a table; the
engine already knows how to draw it. The shipped settings select `zh-Hans`; `--lang en`, F2 or the
settings menu's LANGUAGE row switches. The settings menu's own ids are `settings.title`,
`settings.language`, `settings.back` and `settings.hint`, and every table has all four. The first-run
prompt's ids are `savepath.title`, `savepath.prompt`, `savepath.hint`, `savepath.error.empty` and
`savepath.error.failed`, and every table has all five.

## Fonts

The shell loads a system font (the first of a list of usual Linux locations) and a CJK collection for
Chinese; `--font` and `--cjk-font` override either. A game that ships its own font puts it under
`assets/fonts/` and points a settings or command line at it.
