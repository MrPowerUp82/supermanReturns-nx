#include "generated/default/superman_returns_init.h"
#include "superman_returns_app.h"

#if defined(__SWITCH__)
// Emulators may interpret libnx's TLS+0x1F8 (the ELF thread pointer) as an
// nn::os::ThreadType. Keep its version and name-pointer offsets in zero-filled
// padding, ahead of the runtime's TLS objects. This preserves libnx's thread
// pointer and prevents the emulator from following unrelated TLS data as a name.
thread_local volatile unsigned char sr_emulator_tls_guard[0x200]
    __attribute__((section(".tdata"))) = {};
extern "C" void SrTouchEmulatorTlsGuard() { sr_emulator_tls_guard[0] = 0; }
#endif

REX_DEFINE_APP(superman_returns, SupermanReturnsApp::Create)
