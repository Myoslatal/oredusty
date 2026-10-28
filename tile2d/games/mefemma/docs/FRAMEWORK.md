# The framework Mefemma is made of

This is the guide to the shell, not to a game. It explains what each piece promises, what it deliberately
does not do, and where a game's own code attaches. It assumes the reader has built the project and run it
once (`README.md`).

## 1. One frame

    poll the platform's input
        |
    InputState (edges, pointer, wheel, text)          <- mefemma/input.h
        |
    SceneStack: apply pending push/pop/replace
        |
    top scene: update(services, dt)                   <- mefemma/scene.h
        |
    every scene, bottom to top: draw(services, list)  <- mefemma/draw_list.h
        |
    Renderer2D: the list becomes batches              <- mefemma/renderer2d.h
        |
    begin_pass / end_pass                             <- Ore's Renderer

Only the **top** scene updates; **every** scene draws. That is what makes a pause menu a scene pushed on
top, and a HUD a scene that draws without updating. An opaque scene that wants to hide what is below draws a
full-screen rectangle first; the stack does not guess.

The shell's own frame counter (FPS, milliseconds, draw calls, scene count) is **off** until `F1` asks for it,
so a start screen is the screen the designer drew rather than a developer overlay. `F2` cycles the
interface language.

Stack changes are **requests**. A scene calls `services.push_scene(...)`, `pop_scene()` or
`replace_scene(...)` and keeps running to the end of its turn; the stack applies the request at the start of
the next frame, calling `on_exit`, `on_pause`, `on_enter` and `on_resume` in the order the change implies.
A scene may therefore pop itself from inside its own `update`, which is the case that makes the deferral
worth having.

## 2. Scenes

A scene is pure logic plus a drawing list: no window, no GPU, no clock of its own.

    class MyScene final : public mefemma::Scene {
    public:
        [[nodiscard]] const char* name() const override { return "my-scene"; }

        void update(mefemma::GameServices& services, mefemma::f32 delta_seconds) override {
            if (services.input().key_pressed(mefemma::Key::Escape)) services.pop_scene();
        }

        void draw(mefemma::GameServices& services, mefemma::DrawList& out) const override {
            const t2d::Vec2 view = services.viewport();
            out.rect(mefemma::Aabb2{{0.0f, 0.0f}, view}, mefemma::rgba(16, 18, 24));
            out.text(std::string(services.text("my.title")), t2d::Vec2{40.0f, 40.0f}, 28,
                     mefemma::rgba(240, 240, 240));
        }
    };

Add it to the first frame in `MefemmaApp::start_first_scene()` and to `CMakeLists.txt`
(`mefemma_app` if it draws, `mefemma_core` if it is pure logic). A test can drive it with a fake
`GameServices`: `tests/test_scene_stack.cpp` is the smallest one.

The start screen is `TitleScene` (`src/scenes/title_scene.cpp`): it draws the designer's sign
(`assets/textures/start_screen.png`), the game's name and five rows - New Game, Save List, Settings, Mods and
Quit - and answers the keyboard (Up/Down, Enter, Escape) and the mouse (hover selects, click activates). New
Game, Save List, Settings and Mods open a note that says they are not implemented yet; `TitleScene::activate()`
is the one function a game replaces to make them real. Quit leaves the game.

`PlaygroundScene` is the other worked example: a camera, the pointer, a world-space grid and a HUD. It is not
reachable from the menu while New Game is a placeholder; `--scene playground` starts the process in it. Neither
scene is a design: a game keeps the start screen's shape and replaces the rest.

### The services a scene is handed

`GameServices` (`scene.h`) is the shell's surface to a scene:

| Member | What a scene gets |
|---|---|
| `input()` | this frame's keys, pointer, wheel and text |
| `settings()` | the settings file, including the game's own fields |
| `locale()`, `text(id)` | the string table and the string for an id |
| `measure_text(text, size)` | the box a line will fill, for centring and layout |
| `random()` | the game's random stream, seeded from the settings |
| `elapsed_seconds()`, `frame()` | the shell's clock |
| `viewport()` | the framebuffer size in pixels |
| `push_scene/pop_scene/replace_scene/quit` | requests, applied at the next frame boundary |

A game with more to offer derives from `GameServices`, adds its members, and implements them in
`MefemmaApp` (which already derives from it). The framework does not need to know what a game needs; the
scenes see the richer interface because they are the game's own scenes.

## 3. The draw list

`DrawList` is the boundary between a scene and the GPU. It has four commands:

    out.rect(box, colour);                       // a filled rectangle
    out.rect_outline(box, 2.0f, colour);         // four rectangles, clamped to the box
    out.sprite("hero", box);                     // assets/textures/hero.png, whole texture
    out.sprite("atlas", box, uv, tint);          // a part of a picture, tinted
    out.sprite("start_screen", box, full_uv(), kWhite, TextureFilter::Linear); // smooth scaling
    out.text("hello", pen, 20, colour, wrap_px); // UTF-8, pen at the top left of the line box

Colours are `0xAABBGGRR`; `rgba(r, g, b, a)` builds one, and `kWhite` means "no tint". The order of the
commands is the drawing order: the renderer flushes a batch when the texture changes and never reorders.

Pictures are **nearest** by default, because a tile or a pixel sprite should keep its hard edges. A painted
picture - the start screen's sign - asks for `TextureFilter::Linear` as the last argument, and the renderer
binds a linear sampler for it; the two filters are different batches.

Text is measured with `services.measure_text()`, which asks the same text engine the renderer will use, so a
centred line and its box cannot disagree. A line box is never shorter than the font's own metrics, and the
pen is its top left corner.

A picture that cannot be found is drawn as a marked box and the shell counts it; a scene that lost its art
looks broken instead of silently drawing nothing.

## 4. The renderer

`Renderer2D` owns one `t2d::SpriteBatch` and one `t2d::TextRenderer`:

* rectangles and text share the glyph atlas (a rectangle samples its white texel);
* each picture has its own texture and its own batch;
* a batch binds one texture, so alternating pictures and rectangles issues a batch per alternation. Sorting
  would save batches and change what is drawn on top of what; the renderer draws what it was asked for.

The frame's upload ring and the batch's quad budget are shell settings (`MefemmaApp::on_configure`,
`Renderer2DOptions`). If a frame begins more batches than the sprite batch has descriptor sets, the renderer
says so once in the log: the earliest draws of that frame then sample another texture, which is a bug worth
fixing at the scene (fewer distinct pictures) or at the budget, not something to hide.

## 5. Assets and paths

`DataPaths` (`data_paths.h`) resolves the game's assets directory: **beside the executable first**, the
source tree only when a build tree has not copied the assets next to it. Names are fixed inside it:

    assets/config/game.ecfg       Settings
    assets/text/ui.ecfg           the interface strings
    assets/textures/<name>.png    one picture per name
    assets/fonts/...              optional fonts shipped with the game

`Assets` loads the strings, the fonts the language needs and the pictures a frame asks for (on first use,
cached by name). One picture is one texture: no atlas, any size, no manifest. A scene only ever sees names,
so a scene stays testable and a missing file is a shell-level report.

`assets/README.md` is the designer-facing version of this section.

## 6. Input

`InputState` separates the situation from the event:

    input.key_down(Key::W)       // held right now
    input.key_pressed(Key::F1)   // went down this frame
    input.key_released(Key::F1)  // came up this frame

    input.mouse_x()/mouse_y()    // framebuffer pixels
    input.mouse_delta_x()        // movement since the previous frame
    input.mouse_pressed(MouseButton::Left)
    input.scroll_y()             // wheel, this frame
    input.text_input()           // UTF-8 typed this frame

Key codes mirror GLFW's values (the same convention the engine uses), so a platform layer maps them without
a table. `begin_frame()` is what makes a press a one-frame event; the shell calls it at the top of every
frame, and a test calls it itself.

**A tap belongs to a frame even when it began and ended between two frames.** The shell copies the
platform's press edge as well as the key's held state, so a down and an up that both arrived before the
frame looked are still a `key_pressed()` and a `key_released()` on that frame (and the key is up). This is
what makes a tap from the debug input server, a scripted run or a very fast click behave like a tap.

## 7. Settings and the command line

`Settings` reads the six fields the shell owns - `title`, `width`, `height`, `vsync`, `language`,
`seed` - from `assets/config/game.ecfg`, refuses a field it cannot use with a message, and keeps the rest of
the document for the game (`Settings::find("difficulty")`). A game adds its own keys; the shell never
complains about a key it does not know.

A command line overrides the file:

    --settings <path>   a different settings file
    --assets <dir>      a different assets directory
    --lang <code>       en, zh-Hans, zh-Hant (F2 cycles them at run time)
    --font <path>       the Latin font
    --cjk-font <path>   the CJK collection
    --seed <n>          the random seed
    --scene <name>      title or playground
    --pointer <x,y>     where the pointer starts in a scripted run

Ore adds `--headless`, `--frames`, `--size`, `--screenshot`, `--input-server` and the rest; `--help`
lists both sets.

## 8. Adding things a game needs

**A scene** - the section above. Add the source to `mefemma_app` and a test to `tests/`.

**A game system** - a module that is not a scene: a world, an inventory, a scheduler. If it needs no GPU,
put it in `mefemma_core` and test it; if it draws, give it a scene or let a scene call it during `draw`.
The framework has no world model on purpose: a game that wants an ECS, a tile map or a scene graph adds it
here, and the framework neither knows nor cares.

**A service** - derive from `GameServices`, add the members, implement them in `MefemmaApp`. The scenes see
the game's own type because they are the game's.

**A setting** - add a key to `assets/config/game.ecfg`, read it with `Settings::find` (walk `children()` for
a nested table), and give it a default in code for a game that ships no file.

**A picture** - drop a PNG in `assets/textures/`, draw it with `out.sprite("<name>", box)`.

**A start-screen action** - `TitleScene::activate()` is where the five rows go. Today the four game rows set
`modal_open_` and draw a note; replace that branch with `services.replace_scene(make_new_game_scene())` (or the
screen the row deserves) and the start screen starts the game.

**A string or a language** - add an id to each table in `assets/text/ui.ecfg`. A new language is a new
top-level table named after its code; the engine already draws it and the shell's font loader already picks
the CJK face for it.

## 9. The code table

The executable is a **launcher**; the game is a **code table** (`mefemma.codetab`) that travels beside it
and is merged into it at start-up. That is the repository's way of shipping a game that a mod can replace a
function of, and it has two consequences a game author has to know:

* every symbol the table calls in the engine has to be in `engine.api`. The build checks it
  (`codetab build --api`), and `codetab api --surface engine.api mefemma.codetab` re-checks a table that
  already exists. Mefemma's table currently reports **0 symbols outside the surface**.
* `codetab build` compiles the table itself, with the flags in `games/mefemma/CMakeLists.txt`. A new source
  file has to be added to `MEFEMMA_TABLE_SOURCES` as well as to a library target - the table is what runs.

The launcher also merges mods: `mefemma_game --table mods/x/x.codetab` and any `.codetab` in the `packs`
directory beside the executable. A mod is data plus a table; it is not a dynamic library and it cannot link
the game's static libraries.

## 10. What is deliberately absent

| Not here | Where a game puts it |
|---|---|
| a world, entities, an ECS | its own module in `mefemma_core`, stepped from a scene's `update` |
| tile layers, plots, content packs | its own module; `DrawList` draws whatever the module says |
| save/load | its own format; a scene asks for it, the settings file is not a save |
| audio | a new module and a device of its own; the frame loop has the place for it |
| networking | Tile2D's `t2d::net` is a channel; a game's protocol rides it |
| a fixed UI toolkit | `DrawList` plus the scene's own layout; the start screen and the playground show one way |

The point of a second game project is that it can make different decisions from the first one. Mefemma
does not inherit the mine game's screens, its content model or its layer rules, and it should not grow them
back by accident.

## 11. Testing and scripted runs

Everything in `mefemma_core` is testable without a window: settings, input, the draw list, the stack and
the paths. `test_scenes` goes one step further and drives **the start screen and the playground** with a fake
`GameServices`: the five rows and their wrap-around, Enter opening the not-implemented note, Escape closing
it, Escape on the menu (and the Quit row) leaving the game, the pointer selecting a row and a click opening
it, the start screen drawing the sign with a linear filter, and the playground's grid moving when the
keyboard pans the camera. The shell's own half is exercised by running it:

    ./build/mefemma/debug/games/mefemma/mefemma_game --headless --frames 4 --scene playground \
        --pointer 900,420 --screenshot out.png

The screenshots in `README.md` were produced exactly this way. On a machine with no Vulkan device, the
renderer tests of Tile2D skip; a game's own logic tests do not need a device at all, which is the reason the
split exists.
