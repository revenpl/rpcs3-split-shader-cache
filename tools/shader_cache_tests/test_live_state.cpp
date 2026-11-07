// tools/shader_cache_tests/test_live_state.cpp
// T1: rsx_live_state — 96B persisted live draw state.
// Checks: pack/unpack round-trip, exact size, operator==.
#include "Emu/RSX/RSXShaderState.h"
#include "Utilities/Thread.h" // full thread_ctrl for the emergency_exit stub
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// --- Test scaffolding: symbols whose real definitions live in the full emulator build.
class thread_ctrl;
void thread_ctrl::emergency_exit(std::string_view msg) // real one is [[noreturn]]; must terminate, not return
{
	::write(2, "EMERGENCY_EXIT: ", 16);
	::write(2, msg.data(), msg.size());
	::write(2, "\n", 1);
	::abort();
}

namespace utils { u64 _get_main_tid() { return 0; } }
static int failures = 0;
#define CHECK(cond, name) do { if (!(cond)) { std::printf("FAIL %s\n", name); ++failures; } else std::printf("PASS %s\n", name); } while (0)

int main()
{
	rsx::rsx_live_state a{};
	a.primitive = 4;
	a.renderpass_key = 0x123456789ABCDEF0ULL;
	a.color_mask[2] = 0b0101;
	a.blend_enabled_mask = 0x5;

	u8 buf[sizeof(rsx::rsx_live_state)]{};
	a.pack(buf);

	rsx::rsx_live_state b{};
	b.unpack(buf);
	CHECK(a == b, "pack_unpack_roundtrip");

	CHECK(sizeof(rsx::rsx_live_state) == 96, "size_is_96");

	rsx::rsx_live_state c = a;
	c.stencil_func = 7;
	CHECK(!(a == c), "operator_eq_distinct");

	return failures ? 1 : 0;
}
