// tools/shader_cache_tests/test_decode_equivalence.cpp
// I1: extract_live_state(props) -> decode_offline(...) == props  (round-trip)
// I2: decode_offline reproduces the exact field values the live path would produce.
#include "Emu/RSX/VK/VKShaderState.h"
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <execinfo.h>
#include <unistd.h>

static void segv_handler(int sig)
{
	void* frames[64];
	const int n = backtrace(frames, 64);
	write(2, "SIGSEGV backtrace:\n", 17);
	backtrace_symbols_fd(frames, n, 2);
	_exit(128 + sig);
}

// --- Test scaffolding: symbols referenced by inline/static machinery in the included
// --- headers, whose real definitions live in the full emulator build. None of these
// --- are executed by this unit test (logging registration is a no-op here, and
// --- thread::save is only referenced by the IdManager typemap, never called).
namespace logs {
	registerer::registerer(channel&) {}
	void message::broadcast(const char*, const fmt_type_info*, ...) const {} // logging is a no-op in unit tests
}
namespace rsx { void thread::save(utils::serial&) {} }
namespace utils { u64 _get_main_tid() { return 0; } }
class thread_ctrl;
void thread_ctrl::emergency_exit(std::string_view msg) // real one is [[noreturn]]; must terminate, not return
{
	::write(2, "EMERGENCY_EXIT: ", 16);
	::write(2, msg.data(), msg.size());
	::write(2, "\n", 1);
	::abort();
}

// atomic_wait_engine lives in rpcs3/util/atomic.cpp, which this host's libgcc cannot link
// (16-byte __sync atomics). Referenced only by error paths this test never executes.
void atomic_wait_engine::wait(const void*, u32, u64, atomic_wait::info*) {}
void atomic_wait_engine::notify_one(const void*) {}
void atomic_wait_engine::notify_all(const void*) {}

// g_fixed_typemap is defined in Emu/System.cpp (too heavy to link here). It is only
// consulted by fmt's raw_append engine on the formatting path, which this test never
// executes (fmt calls in the decode helpers are throw-only error paths).
#include "util/fixed_typemap.hpp"
stx::manual_typemap<void, 0x20'00000, 128> g_fixed_typemap;

static int failures = 0;
#define CHECK(cond, name) do { if (!(cond)) { std::printf("FAIL %s\n", name); ++failures; } else std::printf("PASS %s\n", name); } while (0)

static rsx::backend_configuration fixed_cfg()
{
	rsx::backend_configuration c{};
	c.supports_hw_a2c = true;
	c.supports_hw_a2one = true;
	return c;
}

int main()
{
	signal(SIGSEGV, segv_handler);
	signal(SIGABRT, segv_handler);
	std::printf("MAIN START\n"); std::fflush(stdout);
	// --- I2: known-value spot checks (mirror the live-path decode at VKGSRender.cpp:246-408) ---
	rsx::rsx_live_state s{};
	s.primitive = static_cast<u32>(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	s.primitive_restart = 1;
	s.front_face_mode = static_cast<u16>(rsx::front_face::cw);
	s.depth_test_enabled = 1; s.depth_write_enabled = 1; s.depth_func = static_cast<u16>(rsx::comparison_function::equal);
	s.cull_face_enabled = 1; s.cull_face_mode = static_cast<u16>(rsx::cull_face::back);
	s.color_mask[0] = 0b1111; s.num_draw_buffers = 1;
	s.surface_color = static_cast<u8>(rsx::surface_color_format::a8b8g8r8); // no-op host write mask
	s.blend_enabled_mask = 1;
	s.blend_func_sfactor_rgb = static_cast<u16>(rsx::blend_factor::src_alpha);
	s.blend_func_sfactor_a   = static_cast<u16>(rsx::blend_factor::one);
	s.blend_func_dfactor_rgb = static_cast<u16>(rsx::blend_factor::one_minus_src_alpha);
	s.blend_func_dfactor_a   = static_cast<u16>(rsx::blend_factor::one_minus_src_alpha);
	s.blend_equation_rgb     = static_cast<u16>(rsx::blend_equation::add);
	s.blend_equation_a       = static_cast<u16>(rsx::blend_equation::add);
	s.stencil_test_enabled = 1;
	s.stencil_op_fail = static_cast<u16>(rsx::stencil_op::zero);
	s.stencil_func = static_cast<u16>(rsx::comparison_function::equal);
	s.renderpass_key = 0x0000000000000000ULL; // 1 sample
	std::printf("STATE READY\n"); std::fflush(stdout);

	auto props = vk::decode_rsx_state_offline(s, fixed_cfg(), /*depth_bounds_support=*/true, /*force_disable_blending=*/false, /*force_depth_clamp=*/false);
	std::printf("DECODE OK\n"); std::fflush(stdout);
	CHECK(props.state.ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, "ia_topology");
	CHECK(props.state.ia.primitiveRestartEnable == VK_TRUE, "ia_restart");
	CHECK(props.state.ds.depthTestEnable == VK_TRUE, "depth_test");
	CHECK(props.state.rs.cullMode == VK_CULL_MODE_BACK_BIT, "cull_mode");
	CHECK(props.state.att_state[0].colorWriteMask == VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, "color_mask");
	CHECK(props.state.att_state[0].blendEnable == VK_TRUE, "blend_enable");
	CHECK(props.state.att_state[0].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA, "src_factor_rgb");
	CHECK(props.state.att_state[0].dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "dst_factor_a");
	CHECK(props.state.ds.stencilTestEnable == VK_TRUE, "stencil_enable");
	CHECK(props.state.ds.front.failOp == VK_STENCIL_OP_ZERO, "stencil_fail_op");
	CHECK(props.state.ds.front.compareOp == VK_COMPARE_OP_EQUAL, "stencil_func");

	// --- I1: round-trip extract->decode on 16 generated variants ---
	int rt_ok = 1;
	for (u32 variant = 0; variant < 16; ++variant)
	{
		// valid stencil op codes (enum values are raw CELL_GCM codes, not 0..7)
		static const u16 valid_stencil_ops[8] = {
			static_cast<u16>(rsx::stencil_op::zero), static_cast<u16>(rsx::stencil_op::invert),
			static_cast<u16>(rsx::stencil_op::keep), static_cast<u16>(rsx::stencil_op::replace),
			static_cast<u16>(rsx::stencil_op::incr), static_cast<u16>(rsx::stencil_op::decr),
			static_cast<u16>(rsx::stencil_op::incr_wrap), static_cast<u16>(rsx::stencil_op::decr_wrap)
		};
		rsx::rsx_live_state v = s;
		v.renderpass_key = 0x10000 | (variant << 24);   // 1 sample; vary attachment bits
		v.num_draw_buffers = 1 + (variant & 3);
		v.stencil_op_zpass = valid_stencil_ops[variant & 7];
		v.two_sided_stencil = (variant & 8) != 0;
		v.back_stencil_op_fail = valid_stencil_ops[variant & 3];
		v.back_stencil_func = static_cast<u16>(rsx::comparison_function::equal); // required when two_sided
		v.logic_op_enabled = (variant & 4) != 0;
		v.logic_operation = static_cast<u16>(rsx::logic_op::logic_xor);
		v.blend_enabled_mask = (variant & 2) ? 0 : 1;
		v.surface_color = static_cast<u8>(rsx::surface_color_format::b8);
		for (u8 i = 0; i < 4; ++i) v.color_mask[i] = 0x04; // b=1 (remapped to r by b8 host mask)

		auto p1 = vk::decode_rsx_state_offline(v, fixed_cfg(), true, false, false);
		auto back = vk::extract_live_state(p1);
		auto p2 = vk::decode_rsx_state_offline(back, fixed_cfg(), true, false, false);
		if (!(p1 == p2)) rt_ok = 0;
	}
	CHECK(rt_ok, "roundtrip_extract_decode");

	// --- depth clamp/clip: both canonical branches must round-trip ---
	{
		rsx::rsx_live_state v = s;
		v.depth_clamp_enabled = 0;
		v.depth_clip_enabled = 1;   // decode: clamp = (0 || !1) = 0  (c=0 branch)
		auto p1 = vk::decode_rsx_state_offline(v, fixed_cfg(), true, false, false);
		auto back = vk::extract_live_state(p1);
		auto p2 = vk::decode_rsx_state_offline(back, fixed_cfg(), true, false, false);
		CHECK(p1 == p2, "roundtrip_depth_clamp_off");

		v.depth_clamp_enabled = 1;
		v.depth_clip_enabled = 0;   // decode: clamp = (1 || !0) = 1  (c=1 branch)
		p1 = vk::decode_rsx_state_offline(v, fixed_cfg(), true, false, false);
		back = vk::extract_live_state(p1);
		p2 = vk::decode_rsx_state_offline(back, fixed_cfg(), true, false, false);
		CHECK(p1 == p2, "roundtrip_depth_clamp_on");
	}

	// --- MSAA (samples>1): 16-bit mask must round-trip through temp_storage ---
	{
		rsx::rsx_live_state v = s;
		v.renderpass_key = 0x20000; // 2 samples
		v.msaa_sample_mask = 0xFFFF;
		v.msaa_alpha_to_coverage = 1;
		v.msaa_alpha_to_one = 1;

		auto p1 = vk::decode_rsx_state_offline(v, fixed_cfg(), true, false, false);
		auto back = vk::extract_live_state(p1);
		auto p2 = vk::decode_rsx_state_offline(back, fixed_cfg(), true, false, false);
		CHECK(p1 == p2, "roundtrip_msaa");
		CHECK(p1.state.temp_storage.msaa_sample_mask == 0xFFFF, "msaa_mask_stored");
	}

	// --- force_disable_blending / force_depth_clamp semantics ---
	{
		rsx::rsx_live_state v = s;
		auto a = vk::decode_rsx_state_offline(v, fixed_cfg(), true, /*force_disable_blending=*/true, false);
		CHECK(a.state.att_state[0].blendEnable == VK_FALSE, "force_disable_blending");

		auto b = vk::decode_rsx_state_offline(v, fixed_cfg(), true, false, /*force_depth_clamp=*/true);
		CHECK(b.state.rs.depthClampEnable == VK_TRUE, "force_depth_clamp");
	}

	return failures ? 1 : 0;
}
