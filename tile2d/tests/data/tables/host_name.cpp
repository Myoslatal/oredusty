// A definition of a name the *test program* also exports. Every table's call to it goes to this module
// now; the program's own call was bound when it was linked and does not follow (docs/ABI.md H6).
extern "C" int host_owned() { return 7; }
