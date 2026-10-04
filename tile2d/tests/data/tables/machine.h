// The class the game and the mod are both compiled against: the game's own header, in other words.
// What a mod may replace is decided by how the game defines things - a virtual function defined out
// of line is a strong symbol, and the vtable's entry for it is a relocation the merge fills in.
#pragma once

namespace shop {

class Machine {
public:
    virtual ~Machine() = default;
    virtual int rate() const;
};

/// The one machine the game hands out. It lives in another translation unit, so a call through it
/// cannot be devirtualised away: the call really does go through the vtable.
Machine* the_machine();

int speed_bonus();

int produce();

/// A global that cannot be worked out at compile time, so this translation unit needs a static
/// constructor - and the runtime has to run it, after every relocation is in.
int start_count();
extern int machines_started;
int machines_started_value();

} // namespace shop
