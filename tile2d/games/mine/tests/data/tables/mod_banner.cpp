// A mod: one function, the same name the game's own table defines. Merged after the game's table this
// definition wins, and the call the game makes to it - from inside the game - lands here.
extern "C" const char* mine_game_banner() { return "Mine, modded"; }
