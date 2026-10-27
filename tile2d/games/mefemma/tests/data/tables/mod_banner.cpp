// A mod: one function, the same name the game's own table defines. Merged after the game's table this
// definition wins, and the call the game makes to it - from inside the game - lands here.
//
// It is the smallest proof that the launcher merges tables rather than only loading one, and it is built
// by the game's CMakeLists (target mefemma_demo_mod), not installed: a plain run stays a plain run.
extern "C" const char* mefemma_game_banner() { return "Mefemma, modded"; }
