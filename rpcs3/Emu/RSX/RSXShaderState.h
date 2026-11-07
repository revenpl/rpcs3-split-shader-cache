#pragma once
#include "util/types.hpp"
#include <cstddef>
#include <cstring>
#include <type_traits>

namespace rsx
{
	// Live draw state captured at pipeline link time — the inputs vk::decode_rsx_state()
	// currently reads directly from REGS(ctx)/draw-clause (VKGSRender.cpp:246-420).
	// Persisting this explicitly is what makes the portable pipeline record sufficient
	// to regenerate vk::pipeline_props on any other GPU.
	// Layout: fixed 96 bytes, little-endian, trivially copyable (has internal padding,
	// which is part of the layout contract — pack/unpack/operator== use sizeof).
	// IMPORTANT: every field that carries a GCM constant (CELL_GCM_*) is u16 — those
	// constants are 16-bit (0x200..0xf007); storing them in u8 truncates the value.
	struct rsx_live_state
	{
		u32 primitive;            // VkPrimitiveTopology (already device-decoded: get_appropriate_topology applied)
		u8  primitive_restart;    // VkBool32
		u16 front_face_mode;      // rsx::front_face (CELL_GCM_CW=0x900, CELL_GCM_CCW=0x901)
		u8  depth_clamp_enabled;
		u8  depth_clip_enabled;
		u8  depth_test_enabled;
		u8  depth_write_enabled;
		u8  cull_face_enabled;
		u16 depth_func;           // rsx::comparison_function (CELL_GCM_NEVER=0x200 .. ALWAYS=0x207)
		u16 cull_face_mode;       // rsx::cull_face (CELL_GCM_FRONT=0x404, BACK=0x405, FRONT_AND_BACK=0x408)
		u8  surface_color;        // rsx::surface_color_format (0x1..0x10, fits u8)
		u8  color_mask[16];       // 4 attachments x 4 bits: bit0=R bit1=G bit2=B bit3=A (RAW REGS values, pre host-mask)
		u8  logic_op_enabled;
		u16 logic_operation;      // rsx::logic_op (CELL_GCM_CLEAR=0x1500 .. CELL_GCM_SET=0x150f)
		u32 blend_enabled_mask;
		u16 blend_func_sfactor_rgb; // rsx::blend_factor (CELL_GCM_ZERO=0 .. CELL_GCM_ONE_MINUS_CONSTANT_ALPHA=0x8004)
		u16 blend_func_sfactor_a;
		u16 blend_func_dfactor_rgb;
		u16 blend_func_dfactor_a;
		u16 blend_equation_rgb;   // rsx::blend_equation (CELL_GCM_FUNC_ADD=0x8006 .. 0xf007)
		u16 blend_equation_a;
		u8  stencil_test_enabled;
		u8  two_sided_stencil;
		u16 stencil_op_fail;      // rsx::stencil_op (CELL_GCM_ZERO=0, KEEP=0x1e00 .. DECR_WRAP=0x8508)
		u16 stencil_op_zfail;
		u16 stencil_op_zpass;
		u16 stencil_func;         // rsx::comparison_function (CELL_GCM_NEVER=0x200 .. ALWAYS=0x207)
		u16 back_stencil_op_fail;
		u16 back_stencil_op_zfail;
		u16 back_stencil_op_zpass;
		u16 back_stencil_func;
		u8  msaa_enabled;
		u8  msaa_alpha_to_one;
		u8  msaa_alpha_to_coverage;
		u8  num_draw_buffers;
		u16 msaa_sample_mask;     // 16-bit NV4097 field (bits 16-31 of the anti-aliasing control register)
		u16 reserved;             // keep trailing 8-byte alignment for renderpass_key
		u64 renderpass_key;       // also encodes num_rasterization_samples = (key >> 16) & 0xF

		FORCE_INLINE void pack(u8* out) const { std::memcpy(out, this, sizeof(*this)); }
		FORCE_INLINE void unpack(const u8* in) { std::memcpy(this, in, sizeof(*this)); }
		bool operator==(const rsx_live_state& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
		bool operator!=(const rsx_live_state& o) const { return !(*this == o); }
	};

	static_assert(sizeof(rsx_live_state) == 96, "rsx_live_state layout changed — bump cache format version");
	static_assert(offsetof(rsx_live_state, renderpass_key) % 8 == 0, "renderpass_key must stay 8-byte aligned");
	static_assert(std::is_trivially_copyable_v<rsx_live_state>, "rsx_live_state must stay POD");
}
