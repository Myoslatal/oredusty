// A module that asks for something nobody has: the merge has to report it rather than write a
// relocation against nothing.
extern "C" int nowhere_to_be_found();

extern "C" int call_missing() { return nowhere_to_be_found(); }
