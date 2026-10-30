# Licensing

This repository is split by component. The root is the **Ore** engine; `tile2d/` is the Tile2D game framework; the games live under `tile2d/games/`.

| Component | Path | License |
|---|---|---|
| Ore | repository root | **MIT** ([LICENSE](LICENSE)) |
| Tile2D | `tile2d/` | **MIT** ([LICENSE](tile2d/LICENSE)) |
| codetab | `tile2d/tools/codetab/` | **MIT** ([LICENSE](tile2d/tools/codetab/LICENSE)) |
| Mine - code | `tile2d/games/mine/` (sources, headers, tests, shaders, build scripts) | **Reciprocal MIT** ([LICENSE](tile2d/games/mine/LICENSE)) |
| Mine - assets | `tile2d/games/mine/` (content, art, docs/images and other game resources) | **CC BY-SA 4.0** ([LICENSE-ASSETS](tile2d/games/mine/LICENSE-ASSETS)) |
| Mefemma - code | `tile2d/games/mefemma/` (sources, headers, tests, shaders, build scripts) | **Reciprocal MIT** ([LICENSE](tile2d/games/mefemma/LICENSE)) |
| Mefemma - assets | `tile2d/games/mefemma/` (assets, docs/images and other game resources) | **CC BY-SA 4.0** ([LICENSE-ASSETS](tile2d/games/mefemma/LICENSE-ASSETS)) |

## Reciprocal MIT

The game code uses a **Reciprocal MIT License**, which is the MIT License plus a share-alike condition: if you modify, adapt or create a derivative work of the game code and distribute it, that derivative must stay under the same license. The condition applies to derivative works of the game code itself; it does not reach separate works that merely use, link to or are distributed alongside the Ore/Tile2D engines. The full text is in each game's `LICENSE`.

## Game assets

The games' art, audio, fonts, documentation images and content data are licensed under **Creative Commons Attribution-ShareAlike 4.0 International (CC BY-SA 4.0)**. Attribution: the respective game contributors; if you remix or build on them, your contributions must be shared under the same license. Code, headers, tests, shaders and build scripts are not assets; they use the Reciprocal MIT License.

Third-party files keep their own notices. The Ore and Tile2D engines and codetab are MIT, so a game or mod may use them without turning its own code into a derivative work of the engine.

## Packaged builds

Paths above are repository-relative. A packaged Mine/Mefemma build flattens the engine and toolchain files beside the game as `LICENSE-Ore`, `LICENSE-Tile2D` and (in the developer package) `LICENSE-codetab`, and keeps the game's own `LICENSE`, `LICENSE-ASSETS` and this `LICENSING.md` at the package root.
