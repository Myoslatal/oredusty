// The game's own half of the class test: a virtual method defined out of line (so it is a strong
// symbol, not the weak copy an inline one would be), a plain function, and the call that goes through
// the vtable.
#include "machine.h"

namespace shop {

int Machine::rate() const { return 2; }

int speed_bonus() { return 5; }

int machines_started = start_count();

int machines_started_value() { return machines_started; }

int produce() { return the_machine()->rate() * 10 + speed_bonus(); }

} // namespace shop

extern "C" int machine_output() { return shop::produce(); }
