// Where the machine lives. It is a namespace scope object, so the compiler emits a static
// constructor for it - the kind of constructor the runtime has to run after every relocation is in,
// because what it stores in the object is the address of a vtable.
#include "machine.h"

namespace shop {
namespace {

Machine instance;

} // namespace

Machine* the_machine() { return &instance; }

int start_count() { return 41; }

} // namespace shop
