#pragma once
// RSXCacheIO.h — header-only reader/writer for the split shader-cache container.
//
// I4 (atomicity): files are only ever replaced via a temp file + fs::rename —
// a crash never leaves a half-written manifest or device file.
#include "RSXCacheTypes.h"
#include "Utilities/File.h"
#include "util/logs.hpp" // rsx_log
#include <vector>
#include <cstddef>
#include <functional>

namespace rsx::cache_files
{
	template <typename T>
	static bool read_file_records(const std::string& path, u32 expected_magic, std::vector<T>& out)
	{
		out.clear();
		fs::file f(path);
		if (!f) return false;
		if (f.size() < sizeof(cache_file_header)) return false;

		cache_file_header hdr{};
		if (f.read(&hdr, sizeof(hdr)) != sizeof(hdr)) return false;

		if (hdr.magic != expected_magic || hdr.format_version != CACHE_FORMAT_VERSION)
		{
			rsx_log.error("cache files: %s has an incompatible header (magic 0x%08X, version %u)", path.c_str(), hdr.magic, hdr.format_version);
			return false;
		}
		if (hdr.record_size != sizeof(T))
		{
			rsx_log.error("cache files: %s record size %u != %zu", path.c_str(), hdr.record_size, sizeof(T));
			return false;
		}
		if (f.size() != sizeof(cache_file_header) + static_cast<u64>(hdr.entry_count) * sizeof(T))
		{
			rsx_log.error("cache files: %s size mismatch (header claims %u records)", path.c_str(), hdr.entry_count);
			return false;
		}

		if (hdr.entry_count)
		{
			std::vector<u8> buf(static_cast<u64>(hdr.entry_count) * sizeof(T));
			if (f.read(buf.data(), buf.size()) != buf.size()) return false;
			out.resize(hdr.entry_count);
			for (usz i = 0; i < hdr.entry_count; ++i)
				std::memcpy(&out[i], buf.data() + i * sizeof(T), sizeof(T));
		}
		return true;
	}

	template <typename T>
	static bool write_file_records(const std::string& path, u32 magic, const std::vector<T>& records)
	{
		cache_file_header hdr{};
		hdr.magic = magic;
		hdr.format_version = CACHE_FORMAT_VERSION;
		hdr.entry_count = static_cast<u32>(records.size());
		hdr.record_size = static_cast<u32>(sizeof(T));

		// I4: write to a sibling temp file, fsync, then atomically rename over the target.
		const std::string tmp = path + ".pending";
		{
			fs::file out(tmp, fs::rewrite);
			if (!out) return false;
			if (out.write(&hdr, sizeof(hdr)) != sizeof(hdr)) return false;
			for (const auto& r : records)
			{
				if (out.write(&r, sizeof(T)) != sizeof(T)) return false;
			}
			out.sync();
		}
		return fs::rename(tmp, path, true);
	}

	static bool read_manifest(const std::string& path, std::vector<manifest_record>& out)
	{
		return read_file_records<manifest_record>(path, CACHE_MAGIC_MANIFEST, out);
	}

	static bool write_manifest(const std::string& path, const std::vector<manifest_record>& records)
	{
		return write_file_records<manifest_record>(path, CACHE_MAGIC_MANIFEST, records);
	}

	template <typename Props>
	static bool read_device(const std::string& path, std::vector<device_entry<Props>>& out)
	{
		return read_file_records<device_entry<Props>>(path, CACHE_MAGIC_DEVICE, out);
	}

	template <typename Props>
	static bool write_device(const std::string& path, const std::vector<device_entry<Props>>& entries)
	{
		return write_file_records<device_entry<Props>>(path, CACHE_MAGIC_DEVICE, entries);
	}

	// Legacy-import core (shared by the rsx::shaders_cache::import_legacy wrapper and the
	// decode-equivalence test): splits legacy records — a bare (pipeline_core + backend props)
	// pair, no container header — into manifest + device entries.
	// extract_live is the backend's inverse (GL: a no-op returning the default rsx_live_state).
	template <typename Props>
	static bool import_legacy_records(
		const std::string& directory_path,
		const std::function<rsx::rsx_live_state(const Props&)>& extract_live,
		std::vector<device_entry<Props>>& devices,
		std::vector<manifest_record>& manifest)
	{
		manifest.clear();
		devices.clear();

		fs::dir root = fs::dir(directory_path);
		if (!root) return false;

		for (auto&& tmp : root)
		{
			if (tmp.is_directory) continue;
			const auto filename = directory_path + "/" + tmp.name;
			fs::file f(filename);
			if (!f) continue;

			const u64 expected_size = sizeof(pipeline_core) + sizeof(Props);
			if (f.size() != expected_size)
			{
				// Same policy as the legacy load_shaders: log and remove
				rsx_log.error("Import: removing cached pipeline object %s (not binary compatible)", tmp.name.c_str());
				fs::remove_file(filename);
				continue;
			}

			pipeline_core core{};
			Props props{};
			if (f.read(&core, sizeof(core)) != sizeof(core)) continue;
			if (f.read(&props, sizeof(Props)) != sizeof(Props)) continue;

			// Degenerate twins (same vp/fp/state, different props/storage hash) collapse
			// into one record: the lookup key is (vp, fp, state).
			const u64 state_hash = static_cast<u64>(core_state_hash(core));
			bool duplicate = false;
			for (const auto& m : manifest)
			{
				if (m.core.vertex_program_hash == core.vertex_program_hash &&
					m.core.fragment_program_hash == core.fragment_program_hash &&
					static_cast<u64>(core_state_hash(m.core)) == state_hash)
				{
					duplicate = true;
					break;
				}
			}
			if (duplicate) continue;

			manifest_record r{};
			r.core = core;
			r.live = extract_live(props);
			manifest.push_back(std::move(r));

			device_entry<Props> e{};
			e.vp = core.vertex_program_hash;
			e.fp = core.fragment_program_hash;
			e.state_hash = state_hash;
			e.storage_hash = core.pipeline_storage_hash;
			e.props = props;
			devices.push_back(std::move(e));
		}

		return !manifest.empty();
	}
}
