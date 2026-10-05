// A static object inside a table. Its destructor is registered with the C++ runtime against the
// module's own base address - the value the runtime answered __dso_handle with - so it has to run when
// the image holding it is destroyed, and not one moment later: after that, the code it lives in is
// unmapped (docs/ABI.md).
extern "C" int dtor_host_flag;

namespace {

struct Watched {
    ~Watched() { dtor_host_flag = 1; }
};

Watched watched;

} // namespace

extern "C" int dtor_probe() { return 0; }
