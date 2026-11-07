#pragma once
// VKShaderState.h — pure (context-free) pipeline-state decode for the portable shader cache.
//
// decode_rsx_state_offline() is a PURE replacement for vk::decode_rsx_state()
// (VKGSRender.cpp:246-408): identical body, every REGS(ctx)->X() replaced by the
// corresponding rsx::rsx_live_state field; the `ds` render_target side-effect
// (stencil_init_flags hint) stays in the live path only.
//
// extract_live_state() is the canonical inverse: given stored pipeline_props it
// recovers an rsx_live_state that decodes back to equal props (I1 invariant).
// Where decode is many-to-one (e.g. b8 color-mask remap, depth clamp/clip
// combination, two-sided stencil when front==back), extraction picks the
// canonical pre-image; the invariant is props-level equality, not state-level.

#include <vulkan/vulkan_core.h>
#include <cstring>

#include "Emu/RSX/RSXShaderState.h"       // rsx::rsx_live_state
#include "Emu/RSX/RSXThread.h"            // rsx::backend_configuration
#include "Emu/RSX/Utils/color_utils.hpp"  // get_write_output_mask, get_b8_colormask, get_g8b8_r8g8_colormask
#include "VKPipelineCompiler.h"           // vk::pipeline_props

namespace vk
{
	// ---- Enum helpers (moved here: 6 were in VKGSRender.cpp, get_compare_func in VKDraw.cpp) ----

	inline VkLogicOp get_logic_op(rsx::logic_op op)
	{
		switch (op)
		{
		case rsx::logic_op::logic_clear: return VK_LOGIC_OP_CLEAR;
		case rsx::logic_op::logic_and: return VK_LOGIC_OP_AND;
		case rsx::logic_op::logic_and_reverse: return VK_LOGIC_OP_AND_REVERSE;
		case rsx::logic_op::logic_copy: return VK_LOGIC_OP_COPY;
		case rsx::logic_op::logic_and_inverted: return VK_LOGIC_OP_AND_INVERTED;
		case rsx::logic_op::logic_noop: return VK_LOGIC_OP_NO_OP;
		case rsx::logic_op::logic_xor: return VK_LOGIC_OP_XOR;
		case rsx::logic_op::logic_or : return VK_LOGIC_OP_OR;
		case rsx::logic_op::logic_nor: return VK_LOGIC_OP_NOR;
		case rsx::logic_op::logic_equiv: return VK_LOGIC_OP_EQUIVALENT;
		case rsx::logic_op::logic_invert: return VK_LOGIC_OP_INVERT;
		case rsx::logic_op::logic_or_reverse: return VK_LOGIC_OP_OR_REVERSE;
		case rsx::logic_op::logic_copy_inverted: return VK_LOGIC_OP_COPY_INVERTED;
		case rsx::logic_op::logic_or_inverted: return VK_LOGIC_OP_OR_INVERTED;
		case rsx::logic_op::logic_nand: return VK_LOGIC_OP_NAND;
		case rsx::logic_op::logic_set: return VK_LOGIC_OP_SET;
		default:
			fmt::throw_exception("Unknown logic op 0x%x", static_cast<u32>(op));
		}
	}

	inline VkBlendFactor get_blend_factor(rsx::blend_factor factor)
	{
		switch (factor)
		{
		case rsx::blend_factor::one: return VK_BLEND_FACTOR_ONE;
		case rsx::blend_factor::zero: return VK_BLEND_FACTOR_ZERO;
		case rsx::blend_factor::src_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
		case rsx::blend_factor::dst_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
		case rsx::blend_factor::src_color: return VK_BLEND_FACTOR_SRC_COLOR;
		case rsx::blend_factor::dst_color: return VK_BLEND_FACTOR_DST_COLOR;
		case rsx::blend_factor::constant_color: return VK_BLEND_FACTOR_CONSTANT_COLOR;
		case rsx::blend_factor::constant_alpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case rsx::blend_factor::one_minus_src_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
		case rsx::blend_factor::one_minus_dst_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
		case rsx::blend_factor::one_minus_src_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case rsx::blend_factor::one_minus_dst_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
		case rsx::blend_factor::one_minus_constant_alpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		case rsx::blend_factor::one_minus_constant_color: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
		case rsx::blend_factor::src_alpha_saturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
		default:
			fmt::throw_exception("Unknown blend factor 0x%x", static_cast<u32>(factor));
		}
	}

	inline VkBlendOp get_blend_op(rsx::blend_equation op)
	{
		switch (op)
		{
		case rsx::blend_equation::add_signed:
			rsx_log.error("blend equation add_signed used. Emulating using FUNC_ADD");
			[[fallthrough]];
		case rsx::blend_equation::add: return VK_BLEND_OP_ADD;
		case rsx::blend_equation::reverse_add_signed:
			rsx_log.error("blend equation reverse_add_signed used. Emulating using FUNC_ADD");
			return VK_BLEND_OP_ADD;
		case rsx::blend_equation::subtract: return VK_BLEND_OP_SUBTRACT;
		case rsx::blend_equation::reverse_subtract_signed:
			rsx_log.error("blend equation reverse_subtract_signed used. Emulating using FUNC_REVERSE_SUBTRACT");
			[[fallthrough]];
		case rsx::blend_equation::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
		case rsx::blend_equation::min: return VK_BLEND_OP_MIN;
		case rsx::blend_equation::max: return VK_BLEND_OP_MAX;
		default:
			fmt::throw_exception("Unknown blend op: 0x%x", static_cast<u32>(op));
		}
	}

	inline VkStencilOp get_stencil_op(rsx::stencil_op op)
	{
		switch (op)
		{
		case rsx::stencil_op::keep: return VK_STENCIL_OP_KEEP;
		case rsx::stencil_op::zero: return VK_STENCIL_OP_ZERO;
		case rsx::stencil_op::replace: return VK_STENCIL_OP_REPLACE;
		case rsx::stencil_op::incr: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
		case rsx::stencil_op::decr: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
		case rsx::stencil_op::invert: return VK_STENCIL_OP_INVERT;
		case rsx::stencil_op::incr_wrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
		case rsx::stencil_op::decr_wrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
		default:
			fmt::throw_exception("Unknown stencil op: 0x%x", static_cast<u32>(op));
		}
	}

	inline VkFrontFace get_front_face(rsx::front_face ffv)
	{
		switch (ffv)
		{
		case rsx::front_face::cw: return VK_FRONT_FACE_CLOCKWISE;
		case rsx::front_face::ccw: return VK_FRONT_FACE_COUNTER_CLOCKWISE;
		default:
			fmt::throw_exception("Unknown front face value: 0x%x", static_cast<u32>(ffv));
		}
	}

	inline VkCullModeFlags get_cull_face(rsx::cull_face cfv)
	{
		switch (cfv)
		{
		case rsx::cull_face::back: return VK_CULL_MODE_BACK_BIT;
		case rsx::cull_face::front: return VK_CULL_MODE_FRONT_BIT;
		case rsx::cull_face::front_and_back: return VK_CULL_MODE_FRONT_AND_BACK;
		default:
			fmt::throw_exception("Unknown cull face value: 0x%x", static_cast<u32>(cfv));
		}
	}

	inline VkCompareOp get_compare_func(rsx::comparison_function op, bool reverse_direction = false)
	{
		switch (op)
		{
		case rsx::comparison_function::never: return VK_COMPARE_OP_NEVER;
		case rsx::comparison_function::greater: return reverse_direction ? VK_COMPARE_OP_LESS: VK_COMPARE_OP_GREATER;
		case rsx::comparison_function::less: return reverse_direction ? VK_COMPARE_OP_GREATER: VK_COMPARE_OP_LESS;
		case rsx::comparison_function::less_or_equal: return reverse_direction ? VK_COMPARE_OP_GREATER_OR_EQUAL: VK_COMPARE_OP_LESS_OR_EQUAL;
		case rsx::comparison_function::greater_or_equal: return reverse_direction ? VK_COMPARE_OP_LESS_OR_EQUAL: VK_COMPARE_OP_GREATER_OR_EQUAL;
		case rsx::comparison_function::equal: return VK_COMPARE_OP_EQUAL;
		case rsx::comparison_function::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
		case rsx::comparison_function::always: return VK_COMPARE_OP_ALWAYS;
		default:
			fmt::throw_exception("Unknown compare op: 0x%x", static_cast<u32>(op));
		}
	}

	// ---- Inverse helpers (canonical pre-images; only called with values the forward helpers produce) ----

	inline rsx::logic_op get_logic_op_inverse(VkLogicOp op)
	{
		switch (op)
		{
		case VK_LOGIC_OP_CLEAR: return rsx::logic_op::logic_clear;
		case VK_LOGIC_OP_AND: return rsx::logic_op::logic_and;
		case VK_LOGIC_OP_AND_REVERSE: return rsx::logic_op::logic_and_reverse;
		case VK_LOGIC_OP_COPY: return rsx::logic_op::logic_copy;
		case VK_LOGIC_OP_AND_INVERTED: return rsx::logic_op::logic_and_inverted;
		case VK_LOGIC_OP_NO_OP: return rsx::logic_op::logic_noop;
		case VK_LOGIC_OP_XOR: return rsx::logic_op::logic_xor;
		case VK_LOGIC_OP_OR: return rsx::logic_op::logic_or;
		case VK_LOGIC_OP_NOR: return rsx::logic_op::logic_nor;
		case VK_LOGIC_OP_EQUIVALENT: return rsx::logic_op::logic_equiv;
		case VK_LOGIC_OP_INVERT: return rsx::logic_op::logic_invert;
		case VK_LOGIC_OP_OR_REVERSE: return rsx::logic_op::logic_or_reverse;
		case VK_LOGIC_OP_COPY_INVERTED: return rsx::logic_op::logic_copy_inverted;
		case VK_LOGIC_OP_OR_INVERTED: return rsx::logic_op::logic_or_inverted;
		case VK_LOGIC_OP_NAND: return rsx::logic_op::logic_nand;
		case VK_LOGIC_OP_SET: return rsx::logic_op::logic_set;
		default:
			fmt::throw_exception("Unknown VkLogicOp 0x%x", static_cast<u32>(op));
		}
	}

	inline rsx::blend_factor get_blend_factor_inverse(VkBlendFactor factor)
	{
		switch (factor)
		{
		case VK_BLEND_FACTOR_ONE: return rsx::blend_factor::one;
		case VK_BLEND_FACTOR_ZERO: return rsx::blend_factor::zero;
		case VK_BLEND_FACTOR_SRC_ALPHA: return rsx::blend_factor::src_alpha;
		case VK_BLEND_FACTOR_DST_ALPHA: return rsx::blend_factor::dst_alpha;
		case VK_BLEND_FACTOR_SRC_COLOR: return rsx::blend_factor::src_color;
		case VK_BLEND_FACTOR_DST_COLOR: return rsx::blend_factor::dst_color;
		case VK_BLEND_FACTOR_CONSTANT_COLOR: return rsx::blend_factor::constant_color;
		case VK_BLEND_FACTOR_CONSTANT_ALPHA: return rsx::blend_factor::constant_alpha;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return rsx::blend_factor::one_minus_src_color;
		case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return rsx::blend_factor::one_minus_dst_color;
		case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return rsx::blend_factor::one_minus_src_alpha;
		case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return rsx::blend_factor::one_minus_dst_alpha;
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return rsx::blend_factor::one_minus_constant_alpha;
		case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return rsx::blend_factor::one_minus_constant_color;
		case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return rsx::blend_factor::src_alpha_saturate;
		default:
			fmt::throw_exception("Unknown VkBlendFactor 0x%x", static_cast<u32>(factor));
		}
	}

	// Canonical inverse: the emulated add_signed/reverse_add_signed both map to VK_BLEND_OP_ADD;
	// we pick `add` (subtract->SUBTRACT, reverse_subtract->REVERSE_SUBTRACT round-trip exactly).
	inline rsx::blend_equation get_blend_op_inverse(VkBlendOp op)
	{
		switch (op)
		{
		case VK_BLEND_OP_ADD: return rsx::blend_equation::add;
		case VK_BLEND_OP_SUBTRACT: return rsx::blend_equation::subtract;
		case VK_BLEND_OP_REVERSE_SUBTRACT: return rsx::blend_equation::reverse_subtract;
		case VK_BLEND_OP_MIN: return rsx::blend_equation::min;
		case VK_BLEND_OP_MAX: return rsx::blend_equation::max;
		default:
			fmt::throw_exception("Unknown VkBlendOp 0x%x", static_cast<u32>(op));
		}
	}

	inline rsx::stencil_op get_stencil_op_inverse(VkStencilOp op)
	{
		switch (op)
		{
		case VK_STENCIL_OP_KEEP: return rsx::stencil_op::keep;
		case VK_STENCIL_OP_ZERO: return rsx::stencil_op::zero;
		case VK_STENCIL_OP_REPLACE: return rsx::stencil_op::replace;
		case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return rsx::stencil_op::incr;
		case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return rsx::stencil_op::decr;
		case VK_STENCIL_OP_INVERT: return rsx::stencil_op::invert;
		case VK_STENCIL_OP_INCREMENT_AND_WRAP: return rsx::stencil_op::incr_wrap;
		case VK_STENCIL_OP_DECREMENT_AND_WRAP: return rsx::stencil_op::decr_wrap;
		default:
			fmt::throw_exception("Unknown VkStencilOp 0x%x", static_cast<u32>(op));
		}
	}

	inline rsx::front_face get_front_face_inverse(VkFrontFace face)
	{
		switch (face)
		{
		case VK_FRONT_FACE_CLOCKWISE: return rsx::front_face::cw;
		case VK_FRONT_FACE_COUNTER_CLOCKWISE: return rsx::front_face::ccw;
		default:
			fmt::throw_exception("Unknown VkFrontFace 0x%x", static_cast<u32>(face));
		}
	}

	inline rsx::cull_face get_cull_face_inverse(VkCullModeFlags mode)
	{
		switch (mode)
		{
		case VK_CULL_MODE_BACK_BIT: return rsx::cull_face::back;
		case VK_CULL_MODE_FRONT_BIT: return rsx::cull_face::front;
		case VK_CULL_MODE_FRONT_AND_BACK: return rsx::cull_face::front_and_back;
		default:
			fmt::throw_exception("Unknown VkCullModeFlags 0x%x", static_cast<u32>(mode));
		}
	}

	inline rsx::comparison_function get_compare_func_inverse(VkCompareOp op, bool reverse_direction = false)
	{
		switch (op)
		{
		case VK_COMPARE_OP_NEVER: return rsx::comparison_function::never;
		case VK_COMPARE_OP_GREATER: return reverse_direction ? rsx::comparison_function::less : rsx::comparison_function::greater;
		case VK_COMPARE_OP_LESS: return reverse_direction ? rsx::comparison_function::greater : rsx::comparison_function::less;
		case VK_COMPARE_OP_GREATER_OR_EQUAL: return reverse_direction ? rsx::comparison_function::less_or_equal : rsx::comparison_function::greater_or_equal;
		case VK_COMPARE_OP_LESS_OR_EQUAL: return reverse_direction ? rsx::comparison_function::greater_or_equal : rsx::comparison_function::less_or_equal;
		case VK_COMPARE_OP_EQUAL: return rsx::comparison_function::equal;
		case VK_COMPARE_OP_NOT_EQUAL: return rsx::comparison_function::not_equal;
		case VK_COMPARE_OP_ALWAYS: return rsx::comparison_function::always;
		default:
			fmt::throw_exception("Unknown VkCompareOp 0x%x", static_cast<u32>(op));
		}
	}

	// ---- Pure decode: same body as vk::decode_rsx_state (VKGSRender.cpp:246-408), state from rsx_live_state ----

	inline vk::pipeline_props decode_rsx_state_offline(
		const rsx::rsx_live_state& s,
		const rsx::backend_configuration& backend_config,
		bool depth_bounds_support,
		bool force_disable_blending,
		bool force_depth_clamp)
	{
		vk::pipeline_props properties{};

		// Input assembly
		properties.state.set_primitive_type(static_cast<VkPrimitiveTopology>(s.primitive));
		properties.state.enable_primitive_restart(s.primitive_restart ? VK_TRUE : VK_FALSE);

		// Rasterizer state
		properties.state.set_attachment_count(s.num_draw_buffers);
		properties.state.set_front_face(get_front_face(static_cast<rsx::front_face>(s.front_face_mode)));
		if (!force_depth_clamp) [[ likely ]]
		{
			properties.state.enable_depth_clamp(s.depth_clamp_enabled || !s.depth_clip_enabled);
		}
		else
		{
			// Depth clip and clamp are emulated in the fragment shader
			properties.state.enable_depth_clamp(true);
		}
		properties.state.enable_depth_bias(true);
		properties.state.enable_depth_bounds_test(depth_bounds_support);

		if (s.depth_test_enabled)
		{
			//NOTE: Like stencil, depth write is meaningless without depth test
			properties.state.set_depth_mask(s.depth_write_enabled);
			properties.state.enable_depth_test(get_compare_func(static_cast<rsx::comparison_function>(s.depth_func)));
		}

		if (s.cull_face_enabled)
		{
			properties.state.enable_cull_face(get_cull_face(static_cast<rsx::cull_face>(s.cull_face_mode)));
		}

		const auto host_write_mask = rsx::get_write_output_mask(static_cast<rsx::surface_color_format>(s.surface_color));
		for (uint index = 0; index < s.num_draw_buffers; ++index)
		{
			bool color_mask_r = (s.color_mask[index] & 1) != 0;
			bool color_mask_g = (s.color_mask[index] & 2) != 0;
			bool color_mask_b = (s.color_mask[index] & 4) != 0;
			bool color_mask_a = (s.color_mask[index] & 8) != 0;

			switch (static_cast<rsx::surface_color_format>(s.surface_color))
			{
			case rsx::surface_color_format::b8:
				rsx::get_b8_colormask(color_mask_r, color_mask_g, color_mask_b, color_mask_a);
				break;
			case rsx::surface_color_format::g8b8:
				rsx::get_g8b8_r8g8_colormask(color_mask_r, color_mask_g, color_mask_b, color_mask_a);
				break;
			default:
				break;
			}

			properties.state.set_color_mask(
				index,
				color_mask_r && host_write_mask[0],
				color_mask_g && host_write_mask[1],
				color_mask_b && host_write_mask[2],
				color_mask_a && host_write_mask[3]);
		}

		// LogicOp and Blend are mutually exclusive. If both are enabled, LogicOp takes precedence.
		if (s.logic_op_enabled)
		{
			properties.state.enable_logic_op(get_logic_op(static_cast<rsx::logic_op>(s.logic_operation)));
		}
		else if (!force_disable_blending)
		{
			VkBlendFactor sfactor_rgb, sfactor_a, dfactor_rgb, dfactor_a;
			VkBlendOp equation_rgb, equation_a;

			if (s.blend_enabled_mask)
			{
				sfactor_rgb = get_blend_factor(static_cast<rsx::blend_factor>(s.blend_func_sfactor_rgb));
				sfactor_a = get_blend_factor(static_cast<rsx::blend_factor>(s.blend_func_sfactor_a));
				dfactor_rgb = get_blend_factor(static_cast<rsx::blend_factor>(s.blend_func_dfactor_rgb));
				dfactor_a = get_blend_factor(static_cast<rsx::blend_factor>(s.blend_func_dfactor_a));
				equation_rgb = get_blend_op(static_cast<rsx::blend_equation>(s.blend_equation_rgb));
				equation_a = get_blend_op(static_cast<rsx::blend_equation>(s.blend_equation_a));

				for (u8 idx = 0; idx < s.num_draw_buffers; ++idx)
				{
					if (s.blend_enabled_mask & (1u << idx))
					{
						properties.state.enable_blend(idx, sfactor_rgb, sfactor_a, dfactor_rgb, dfactor_a, equation_rgb, equation_a);
					}
				}
			}
		}

		if (s.stencil_test_enabled)
		{
			if (!s.two_sided_stencil)
			{
				properties.state.enable_stencil_test(
					get_stencil_op(static_cast<rsx::stencil_op>(s.stencil_op_fail)),
					get_stencil_op(static_cast<rsx::stencil_op>(s.stencil_op_zfail)),
					get_stencil_op(static_cast<rsx::stencil_op>(s.stencil_op_zpass)),
					get_compare_func(static_cast<rsx::comparison_function>(s.stencil_func)),
					0xFF, 0xFF); //write mask, func_mask, ref are dynamic
			}
			else
			{
				properties.state.enable_stencil_test_separate(0,
					get_stencil_op(static_cast<rsx::stencil_op>(s.stencil_op_fail)),
					get_stencil_op(static_cast<rsx::stencil_op>(s.stencil_op_zfail)),
					get_stencil_op(static_cast<rsx::stencil_op>(s.stencil_op_zpass)),
					get_compare_func(static_cast<rsx::comparison_function>(s.stencil_func)),
					0xFF, 0xFF); //write mask, func_mask, ref are dynamic

				properties.state.enable_stencil_test_separate(1,
					get_stencil_op(static_cast<rsx::stencil_op>(s.back_stencil_op_fail)),
					get_stencil_op(static_cast<rsx::stencil_op>(s.back_stencil_op_zfail)),
					get_stencil_op(static_cast<rsx::stencil_op>(s.back_stencil_op_zpass)),
					get_compare_func(static_cast<rsx::comparison_function>(s.back_stencil_func)),
					0xFF, 0xFF); //write mask, func_mask, ref are dynamic
			}
			// NOTE: the `ds->stencil_init_flags` side-effect from the live version is
			// intentionally NOT here — it is a runtime hint on the render target, not part of props.
		}

		const u8 num_rasterization_samples = static_cast<u8>((s.renderpass_key >> 16) & 0xF);
		if (backend_config.supports_hw_a2c || num_rasterization_samples > 1)
		{
			const bool alpha_to_one_enable = s.msaa_alpha_to_one && backend_config.supports_hw_a2one;

			properties.state.set_multisample_state(
				num_rasterization_samples,
				s.msaa_sample_mask,
				s.msaa_enabled,
				s.msaa_alpha_to_coverage,
				alpha_to_one_enable);

			// A problem observed on multiple GPUs is that interior geometry edges can resolve 0 samples unless we force shading rate of 1.
			// For whatever reason, the way MSAA images are 'resolved' on PS3 bypasses this issue.
			// NOTE: We do not do image resolve at all, the output is merely 'exploded' and the guest application is responsible for doing the resolve in software as it is on real hardware.
			properties.state.set_multisample_shading_rate(1.f);
		}

		// The live path gets this from the renderer's current FBO (VKGSRender.cpp:1894);
		// offline it comes from the state, and extract_live_state reads it back (round-trip).
		properties.renderpass_key = s.renderpass_key;

		return properties;
	}

	// ---- Canonical inverse: recover rsx_live_state from stored props ----
	//
	// decode is many-to-one in a few places; extraction picks the canonical pre-image
	// (documented per field) so that decode(extract(props)) == props always holds.

	inline rsx::rsx_live_state extract_live_state(const vk::pipeline_props& props)
	{
		const auto& st = props.state;
		rsx::rsx_live_state s{};

		s.primitive = static_cast<u32>(st.ia.topology);
		s.primitive_restart = st.ia.primitiveRestartEnable ? 1 : 0;
		s.front_face_mode = static_cast<u16>(get_front_face_inverse(st.rs.frontFace));
		// decode: clamp = (depth_clamp_enabled || !depth_clip_enabled). Canonical pre-image of c:
		// c=1 -> (1,1); c=0 -> (0,1). Both decode back to c.
		const bool clamp = (st.rs.depthClampEnable != VK_FALSE);
		s.depth_clamp_enabled = clamp ? 1 : 0;
		s.depth_clip_enabled = 1;
		s.depth_test_enabled = st.ds.depthTestEnable ? 1 : 0;
		s.depth_write_enabled = st.ds.depthWriteEnable ? 1 : 0;
		s.depth_func = static_cast<u16>(get_compare_func_inverse(st.ds.depthCompareOp));
		s.cull_face_enabled = (st.rs.cullMode != VK_CULL_MODE_NONE) ? 1 : 0;
		s.cull_face_mode = s.cull_face_enabled ? static_cast<u16>(get_cull_face_inverse(st.rs.cullMode)) : static_cast<u16>(rsx::cull_face::back);
		// a8b8g8r8 has the no-op host write mask {T,T,T,T} and no b8/g8b8 remap, so the
		// extracted bits decode to exactly the stored colorWriteMask (canonical pre-image).
		s.surface_color = static_cast<u8>(rsx::surface_color_format::a8b8g8r8);

		s.num_draw_buffers = st.cs.attachmentCount;
		for (u8 i = 0; i < st.cs.attachmentCount && i < 4; ++i)
		{
			u8 bits = 0;
			if (st.att_state[i].colorWriteMask & VK_COLOR_COMPONENT_R_BIT) bits |= 1;
			if (st.att_state[i].colorWriteMask & VK_COLOR_COMPONENT_G_BIT) bits |= 2;
			if (st.att_state[i].colorWriteMask & VK_COLOR_COMPONENT_B_BIT) bits |= 4;
			if (st.att_state[i].colorWriteMask & VK_COLOR_COMPONENT_A_BIT) bits |= 8;
			s.color_mask[i] = bits;
		}

		s.logic_op_enabled = st.cs.logicOpEnable ? 1 : 0;
		if (s.logic_op_enabled)
			s.logic_operation = static_cast<u16>(get_logic_op_inverse(st.cs.logicOp));

		s.blend_enabled_mask = 0;
		for (u8 i = 0; i < 4; ++i) if (st.att_state[i].blendEnable) s.blend_enabled_mask |= (1u << i);
		if (s.blend_enabled_mask)
		{
			// decode writes the same factors to every enabled attachment; the first one carries them
			const u8 i0 = static_cast<u8>(__builtin_ctz(s.blend_enabled_mask));
			s.blend_func_sfactor_rgb = static_cast<u16>(get_blend_factor_inverse(st.att_state[i0].srcColorBlendFactor));
			s.blend_func_sfactor_a   = static_cast<u16>(get_blend_factor_inverse(st.att_state[i0].srcAlphaBlendFactor));
			s.blend_func_dfactor_rgb = static_cast<u16>(get_blend_factor_inverse(st.att_state[i0].dstColorBlendFactor));
			s.blend_func_dfactor_a   = static_cast<u16>(get_blend_factor_inverse(st.att_state[i0].dstAlphaBlendFactor));
			s.blend_equation_rgb     = static_cast<u16>(get_blend_op_inverse(st.att_state[i0].colorBlendOp));
			s.blend_equation_a       = static_cast<u16>(get_blend_op_inverse(st.att_state[i0].alphaBlendOp));
		}

		s.stencil_test_enabled = st.ds.stencilTestEnable ? 1 : 0;
		if (s.stencil_test_enabled)
		{
			// The non-separate path copies front onto back, so front==back is the canonical
			// (two_sided=0) case; front!=back is only producible via the separate path.
			s.two_sided_stencil = (std::memcmp(&st.ds.front, &st.ds.back, sizeof(st.ds.front)) != 0) ? 1 : 0;
			s.stencil_op_fail = static_cast<u16>(get_stencil_op_inverse(st.ds.front.failOp));
			s.stencil_op_zfail = static_cast<u16>(get_stencil_op_inverse(st.ds.front.depthFailOp));
			s.stencil_op_zpass = static_cast<u16>(get_stencil_op_inverse(st.ds.front.passOp));
			s.stencil_func = static_cast<u16>(get_compare_func_inverse(st.ds.front.compareOp));
			s.back_stencil_op_fail = static_cast<u16>(get_stencil_op_inverse(st.ds.back.failOp));
			s.back_stencil_op_zfail = static_cast<u16>(get_stencil_op_inverse(st.ds.back.depthFailOp));
			s.back_stencil_op_zpass = static_cast<u16>(get_stencil_op_inverse(st.ds.back.passOp));
			s.back_stencil_func = static_cast<u16>(get_compare_func_inverse(st.ds.back.compareOp));
		}

		// decode never sets the MSAA enable flag (constructor zero; set_multisample_state ignores it) — keep canonical 0.
		s.msaa_enabled = 0;
		s.msaa_sample_mask = static_cast<u16>(st.temp_storage.msaa_sample_mask);
		s.msaa_alpha_to_coverage = st.ms.alphaToCoverageEnable ? 1 : 0;
		s.msaa_alpha_to_one = st.ms.alphaToOneEnable ? 1 : 0;
		s.renderpass_key = props.renderpass_key;
		return s;
	}
}
