// tools/shader_cache_tests/test_cache_io.cpp
// T3: split-file container (portable manifest + per-device props) with atomic writes.
// Covers: read/write round-trip, header validation (magic, record size, file size),
// I4 (no pending file left behind), core_state_hash == store()'s hash formula.
#include "Emu/RSX/RSXCacheIO.h"
#include "Utilities/Thread.h" // full thread_ctrl for the emergency_exit stub
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond, name) do { if (!(cond)) { std::printf("FAIL %s\n", name); ++failures; } else std::printf("PASS %s\n", name); } while (0)

// --- Test scaffolding: symbols whose real definitions live in the full emulator build.
namespace logs {
	registerer::registerer(channel&) {}
	void message::broadcast(const char*, const fmt_type_info*, ...) const {} // logging is a no-op in unit tests
}
namespace utils {
	u64 _get_main_tid() { return 0; }
	u64 get_unique_tsc() { static u64 counter = 0; return ++counter; } // unique-name source only; counter is fine in tests
}
// atomic_wait_engine lives in rpcs3/util/atomic.cpp, which this host's libgcc cannot link
// (16-byte __sync builtin). The tests are single-threaded and never wait on atomics.
void atomic_wait_engine::wait(const void*, u32, u64, atomic_wait::info*) {}
void atomic_wait_engine::notify_one(const void*) {}
void atomic_wait_engine::notify_all(const void*) {}
class thread_ctrl;
void thread_ctrl::emergency_exit(std::string_view msg) // real one is [[noreturn]]; must terminate
{
	::abort();
}

using namespace rsx::cache_files;

int main()
{
	const std::string dir = "build/tests/tmp_io";
	fs::remove_all(dir);
	fs::create_path(dir);

	// --- manifest write/read round-trip
	{
		std::vector<manifest_record> records(3);
		records[0].core.vertex_program_hash = 0xAA; records[0].live.renderpass_key = 1;
		records[1].core.fragment_program_hash = 0xBB; records[1].live.blend_enabled_mask = 0x5;
		records[2].core.vertex_program_hash = 0xCC;
		write_manifest(dir + "/pipelines.bin", records);

		std::vector<manifest_record> m;
		CHECK(read_manifest(dir + "/pipelines.bin", m), "manifest_read");
		CHECK(m.size() == 3, "manifest_count");
		CHECK(m[0].core.vertex_program_hash == 0xAA && m[0].live.renderpass_key == 1, "manifest_r0");
		CHECK(m[1].core.fragment_program_hash == 0xBB && m[1].live.blend_enabled_mask == 0x5, "manifest_r1");
		CHECK(m[2].core.vertex_program_hash == 0xCC, "manifest_r2");
	}

	// --- device write/read round-trip (u64 as stand-in for the 448B props block)
	{
		std::vector<device_entry<u64>> d(2);
		d[0] = { 0xAA, 0, 123, 0, 42 };
		d[1] = { 0xBB, 0, 456, 0, 43 };
		write_device<u64>(dir + "/props.bin", d);

		std::vector<device_entry<u64>> r;
		CHECK(read_device<u64>(dir + "/props.bin", r), "device_read");
		CHECK(r.size() == 2, "device_count");
		CHECK(r[0].vp == 0xAA && r[0].state_hash == 123 && r[0].props == 42, "device_d0");
		CHECK(r[1].vp == 0xBB && r[1].state_hash == 456 && r[1].props == 43, "device_d1");
	}

	// --- I4: no pending/temp file left behind after successful writes
	{
		fs::dir tmp(dir);
		u32 pending = 0;
		for (auto& e : tmp) if (e.name.find(".pending") != std::string::npos) pending++;
		CHECK(pending == 0, "no_pending_left");
	}

	// --- header validation: corrupt magic → read must report failure, not garbage
	{
		fs::file f(dir + "/bad.bin", fs::rewrite);
		u32 hdr[4] = { 0xDEADBEEF, 1, 0, 320 };
		f.write(hdr, 16);
		f.close();
		std::vector<manifest_record> bad;
		CHECK(!read_manifest(dir + "/bad.bin", bad) && bad.empty(), "bad_magic_rejected");
	}

	// --- header validation: wrong record size → rejected
	{
		fs::file f(dir + "/badsize.bin", fs::rewrite);
		u32 hdr[4] = { CACHE_MAGIC_MANIFEST, CACHE_FORMAT_VERSION, 1, 123 };
		f.write(hdr, 16);
		f.close();
		std::vector<manifest_record> bad;
		CHECK(!read_manifest(dir + "/badsize.bin", bad), "bad_record_size_rejected");
	}

	// --- header validation: size mismatch (entry_count lies) → rejected
	{
		fs::file f(dir + "/badcount.bin", fs::rewrite);
		u32 hdr[4] = { CACHE_MAGIC_MANIFEST, CACHE_FORMAT_VERSION, 99, 320 };
		f.write(hdr, 16);
		f.close();
		std::vector<manifest_record> bad;
		CHECK(!read_manifest(dir + "/badcount.bin", bad), "bad_entry_count_rejected");
	}

	// --- core_state_hash must equal store()'s hash over the same 14 fields in the same order
	{
		pipeline_core c{};
		c.vp_ctrl0 = 1; c.vp_ctrl1 = 2; c.fp_ctrl = 3; c.vp_texture_dimensions = 4;
		c.fp_texture_dimensions = 5; c.fp_texcoord_control = 6; c.fp_height = 7;
		c.fp_pixel_layout = 8; c.fp_lighting_flags = 9; c.fp_shadow_textures = 10;
		c.fp_redirected_textures = 11; c.vp_multisampled_textures = 12;
		c.fp_multisampled_textures = 13; c.fp_mrt_count = 14;
		const u32 state_params[] =
		{
			c.vp_ctrl0, c.vp_ctrl1, c.fp_ctrl, c.vp_texture_dimensions, c.fp_texture_dimensions, c.fp_texcoord_control,
			c.fp_height, c.fp_pixel_layout, c.fp_lighting_flags, c.fp_shadow_textures, c.fp_redirected_textures,
			c.vp_multisampled_textures, c.fp_multisampled_textures, c.fp_mrt_count
		};
		CHECK(core_state_hash(c) == rpcs3::hash_array(state_params), "core_state_hash_matches_store_formula");
	}

	// --- layout contract (pipeline_core = first 224B of the legacy 672B Vulkan record)
	CHECK(sizeof(pipeline_core) == 224, "core_is_224");
	CHECK(sizeof(manifest_record) == 320, "manifest_is_320"); // 224B core + 96B rsx_live_state
	CHECK(sizeof(cache_file_header) == 16, "header_is_16");

	return failures ? 1 : 0;
}
