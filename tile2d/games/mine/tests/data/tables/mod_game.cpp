// A mod table: it replaces a function the game's own table defined. Nothing else about the game
// changes - the game's code calls game_bonus(), and after the merge that call lands here.
extern "C" int game_bonus() { return 7; }
