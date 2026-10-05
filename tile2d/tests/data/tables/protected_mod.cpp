// A definition with protected visibility, replacing one of the game's. The compiler is allowed to bind
// calls to a protected symbol directly - measured: a call to one carries no relocation at all, so a
// merge cannot redirect it - and that is worth saying when such a definition wins (docs/ABI.md H8).
//
// The pragma rather than the attribute: __attribute__((visibility("protected"))) in front of a
// definition is ignored by GCC here ("attributes are not permitted in this position"), and the symbol
// comes out with default visibility.
#pragma GCC visibility push(protected)
extern "C" int base_value() { return 200; }
#pragma GCC visibility pop
