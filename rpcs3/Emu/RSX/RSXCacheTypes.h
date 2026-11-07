#pragma once
// RSXCacheTypes.h — on-disk container types for the split shader cache.
//
// Layout of today's legacy per-pipeline file (672B, Vulkan) is:
//   [ 224B pipeline_core (GPU-portable) ] [ 448B pipeline_props (GPU-specific) ]
// The split cache stores the two halves in separate files:
//   pipelines.bin          — portable manifest: N x (pipeline_core + rsx_live_state)
//   device/<key>/props.bin — this GPU's records: N x (keys + pipeline_props)
//
// pipeline_core is copied VERBATIM (field order preserved) from
// shaders_cache::pipeline_data in rsx_cache.h — the last field of that struct
// (pipeline_storage_type pipeline_properties) is removed and everything else
// is the first 224 bytes of the legacy record. Do not reorder fields.
#include "RSXShaderState.h"
#include "util/fnv_hash.hpp"
#include <cstddef>

namespace rsx::cache_files
{
	static constexpr u32 CACHE_MAGIC_MANIFEST = 0x4C505352; // "RSPL" (little-endian on disk: 52 53 50 4C)
	static constexpr u32 CACHE_MAGIC_DEVICE   = 0x54505352; // "RSPT"
	static constexpr u32 CACHE_FORMAT_VERSION = 1;

	struct pipeline_core
	{
		u64 vertex_program_hash;
		u64 fragment_program_hash;
		u64 pipeline_storage_hash;

		u32 vp_ctrl0;
		u32 vp_ctrl1;
		u32 vp_texture_dimensions;
		u32 vp_reserved_0;
		u64 vp_instruction_mask[9];

		u32 vp_base_address;
		u32 vp_entry;
		u16 vp_jump_table[32];

		u16 vp_multisampled_textures;
		u16 vp_reserved_1;
		u32 vp_reserved_2;

		u32 fp_ctrl;
		u32 fp_texture_dimensions;
		u32 fp_texcoord_control;
		u16 fp_height;
		u16 fp_pixel_layout;
		u16 fp_lighting_flags;
		u16 fp_shadow_textures;
		u16 fp_redirected_textures;
		u16 fp_multisampled_textures;
		u8  fp_mrt_count;
		u8  fp_reserved0;
		u16 fp_reserved1;
		u32 fp_reserved2;
	};
	static_assert(sizeof(pipeline_core) == 224, "pipeline_core must be the first 224B of the legacy pipeline_data");

	struct cache_file_header
	{
		u32 magic;
		u32 format_version;
		u32 entry_count;
		u32 record_size;
	};
	static_assert(sizeof(cache_file_header) == 16, "header must stay 16B");

	// Portable manifest record: the GPU-independent half of the legacy record,
	// plus the live draw state that decode_rsx_state_offline() needs to regenerate
	// pipeline_props on any other GPU.
	struct manifest_record
	{
		pipeline_core core;
		rsx::rsx_live_state live;
	};
	static_assert(sizeof(manifest_record) == 224 + sizeof(rsx::rsx_live_state), "manifest record = 224B core + rsx_live_state");

	// Per-device record: lookup keys + this GPU's pipeline properties.
	// Vulkan: sizeof = 32 + 448 = 480B; OpenGL: 32 + 8 = 40B (props = void*).
	template <typename Props>
	struct device_entry
	{
		u64 vp;            // vertex_program_hash
		u64 fp;            // fragment_program_hash
		u64 state_hash;    // hash of the 14 state params (identical formula to shaders_cache::store())
		u64 storage_hash;  // hash of props (identity/debug; NOT part of the lookup key)
		Props props;
	};
	static_assert(sizeof(device_entry<u8[448]>) == 480, "Vulkan device entry must be 32B keys + 448B props");

	// The 14-param state hash, computed with the IDENTICAL field list and order as
	// shaders_cache::store() (rsx_cache.h state_params[]). This is the single source
	// of truth for the (vp, fp, state_hash) dedup key shared by store(), load() and
	// legacy import — do not duplicate the array elsewhere.
	inline usz core_state_hash(const pipeline_core& c)
	{
		const u32 state_params[] =
		{
			c.vp_ctrl0,
			c.vp_ctrl1,
			c.fp_ctrl,
			c.vp_texture_dimensions,
			c.fp_texture_dimensions,
			c.fp_texcoord_control,
			c.fp_height,
			c.fp_pixel_layout,
			c.fp_lighting_flags,
			c.fp_shadow_textures,
			c.fp_redirected_textures,
			c.vp_multisampled_textures,
			c.fp_multisampled_textures,
			c.fp_mrt_count
		};
		return rpcs3::hash_array(state_params);
		}

		// (vp, fp, state_hash) — the pipeline identity within the split cache
		struct entry_key
		{
			u64 vp;
			u64 fp;
			u64 state_hash;

			bool operator==(const entry_key&) const = default;
		};

		inline entry_key make_entry_key(u64 vp, u64 fp, u64 state_hash)
		{
			return entry_key{ vp, fp, state_hash };
		}

		struct entry_key_hash
		{
			usz operator()(const entry_key& k) const
			{
				u64 h = k.vp;
				h = h * 0x9E3779B97F4A7C15ull + k.fp;
				h = h * 0x9E3779B97F4A7C15ull + k.state_hash;
				return static_cast<usz>(h ^ (h >> 32));
			}
		};
		}
