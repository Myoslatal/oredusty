# Mefemma - a game project on Tile2D

Mefemma is the **second game project** in this repository. It is not a copy of the mine game and it shares
none of its screens, its content model or its world: it is a separate shell on the same engine, with the
pieces a game needs and nothing that a game's designer has not asked for.

What ships today is the **framework**:

* a window and a frame loop (Ore's application), with a clock and a headless path;
* **scenes** - the places a game is in - and a stack that pauses, resumes, overlays and replaces them
  without a scene ever seeing the window;
* a **DrawList**: a scene asks for rectangles, pictures and text, and never touches the GPU;
* a 2D renderer that turns that list into batches, preserving the order it was asked in;
* **assets**: pictures by name, interface strings by id, fonts per language, all found beside the
  executable (with the source tree as a build-tree fallback);
* **input** with the distinction between a key held and a key pressed this frame, pointer deltas that
  survive a frame boundary, the wheel and typed text;
* a **settings** file and a command line, plus the code-table launcher the repository's games ship behind.

It ships **no gameplay and no game content** on purpose: what a game is, what its screens are and what
art it has are the designer's to decide. The two scenes here are examples of the shape, not a design.

![The title scene](docs/images/title.png)

![The playground: a camera, a grid, a pointer and the shell's own chrome](docs/images/playground.png)

![The same title in Simplified Chinese](docs/images/title_zh_hans.png)

## Build and run

    cd tile2d
    cmake --preset debug
    cmake --build build/debug -j
    ./build/debug/games/mefemma/mefemma_game

The other presets build it too (`release`, `asan`, `tsan`); `no-renderer` builds Tile2D without any game,
because there is no window and no GPU in that configuration.

    ctest --test-dir build/debug -R 'test_draw_list|test_scene_stack|test_settings|test_input|test_data_paths|test_scenes'

A scripted run needs no keyboard, a window or a display:

    ./build/debug/games/mefemma/mefemma_game --headless --frames 4 --size 1280x720 \
        --scene playground --pointer 900,420 --screenshot out.png

`--scene playground` skips the title; `--pointer x,y` puts the pointer somewhere, because a headless run
has no mouse (a real pointer takes over as soon as it moves).

## The pieces

| Module | What it does | Tested by |
|---|---|---|
| `mefemma_core` | settings, input, the draw list, the scene stack, where the files are | `test_settings`, `test_input`, `test_draw_list`, `test_scene_stack`, `test_data_paths` |
| `mefemma_app` | assets, the 2D renderer, the application shell | `test_scenes` for the scenes; the rest is compiled and run through the launcher and needs a GPU |
| `src/scenes` | `TitleScene` and `PlaygroundScene`: two worked examples of a scene | `test_scenes`, and the headless screenshots above |

`mefemma_core` has no graphics dependency at all: it links `t2d::core` and `t2d::text` and nothing else, so
a test drives a scene with a fake `GameServices` and no window.

## Layout

    include/mefemma/
        app.h            the shell: window, clock, scene stack, the services a scene asks for
        scene.h          Scene, GameServices, SceneStack
        draw_list.h      rectangles, pictures and text, in submission order
        renderer2d.h     a DrawList in, batches out
        assets.h         fonts, strings, pictures
        input.h          keys, mouse, wheel, typed text
        settings.h       the settings file and the fields the shell reads
        data_paths.h     where the files that travel with the game are
        scenes/          the two example scenes
    src/                 the same, plus game_main.cpp (the game's entry) and main.cpp (the launcher)
    assets/              settings, strings, pictures: what the game reads at run time
    docs/                this project's framework guide
    tests/               the core's unit tests

## What is deliberately absent

Mefemma has no world model, no entities, no tile layers, no content packs, no save format, no audio and no
networking. Those are not omissions to be filled in by the framework: they are **design decisions a game
makes**, and the framework's job is to leave the room for them. `docs/FRAMEWORK.md` names the place each
one would attach, with the extension points and the reasons.

## Where a game starts

1. Read [docs/FRAMEWORK.md](docs/FRAMEWORK.md) - the frame, the scenes, the draw list, the services.
2. Replace `src/scenes/title_scene.cpp` and `src/scenes/playground_scene.cpp` with your screens.
3. Put your own modules next to them, add them to `CMakeLists.txt` (`mefemma_core` if they need no GPU,
   `mefemma_app` if they do) and write a test in `tests/`.
4. Put the game's own fields in `assets/config/game.ecfg` and read them with `Settings::find`; put its
   strings in `assets/text/ui.ecfg` and its pictures in `assets/textures/`.

## Verification

Run in this checkout, on this machine (Intel Arc via Mesa when a display is available; the screenshots above
were rendered through SwiftShader's Vulkan ICD because the session has no display-attached device):

| Check | Result |
|---|---|
| `cmake --build build/debug` | 6/6 mefemma suites green (41/41 with the mine suites beside them) |
| `codetab api --surface engine.api mefemma.codetab` | `accept (0 symbol(s) outside the surface)` |
| headless title, playground and `zh-Hans` runs | screenshots written, no error |
| `mefemma.codetab` loaded by the launcher | `1 module(s), 0 error(s)` |
| title → playground → title over `--input-server` | Enter pushed the playground (2 scenes), Escape popped back to the title (1 scene) |
