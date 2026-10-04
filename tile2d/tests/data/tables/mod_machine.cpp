// A mod that changes a class: it replaces a virtual method of the game's own class. Nothing about the
// class changes - the vtable is still the game's, and the method is still virtual - but the entry the
// vtable holds for it was a relocation, and the merge points that entry here.
#include "machine.h"

namespace shop {

int Machine::rate() const { return 9; }

int speed_bonus() { return 7; }

} // namespace shop
