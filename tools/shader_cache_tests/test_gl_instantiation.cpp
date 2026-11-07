// T4: rsx::shaders_cache<void*, Backend> template instantiation — mirrors the GL backend
// (rsx::shaders_cache<void*, GLProgramBuffer>) without needing GL headers.
// FakeGLProgramBuffer has EXACTLY the signatures of the three GLProgramBuffer members the
// template calls (verified against GLProgramBuffer.h:127/143/148).
#include "Emu/RSX/rsx_cache.h"
#include "Emu/RSX/Program/ProgramStateCache.h" // rsx::program_cache_hint_t

class FakeGLProgramBuffer
{
public:
	u64 get_hash(void* const&)
	{
		return 0;
	}
	u64 get_hash(const RSXVertexProgram& prog)
	{
		return 0;
	}
	u64 get_hash(const RSXFragmentProgram& prog)
	{
		return 0;
	}
	template <typename... Args>
	void add_pipeline_entry(const RSXVertexProgram& vp, const RSXFragmentProgram& fp, void*& props, Args&&... args)
	{
		(void)vp; (void)fp; (void)props;
	}
	void preload_programs(rsx::program_cache_hint_t* cache_hint, const RSXVertexProgram& vp, const RSXFragmentProgram& fp)
	{
		(void)cache_hint; (void)vp; (void)fp;
	}
};

// Force full template instantiation (all members)
template class rsx::shaders_cache<void*, FakeGLProgramBuffer>;

// GL-style make_props/extract_live (same shape as GLGSRender.cpp's lambdas)
static void exercise_api()
{
	FakeGLProgramBuffer storage;
	rsx::shaders_cache<void*, FakeGLProgramBuffer> cache(
		storage, "opengl", "v1.95",
		[](const rsx::rsx_live_state&, u32) -> void*
		{
			return nullptr;
		},
		[](void*) -> rsx::rsx_live_state
		{
			return {};
		},
		"opengl");
	(void)cache;
}
