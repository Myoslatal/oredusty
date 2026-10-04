// The mod half: it defines a name the game already defined. Merged after the game's table this
// definition wins, and every call site - including the calls inside the game's own code - lands here.
extern "C" int base_value() { return 100; }
