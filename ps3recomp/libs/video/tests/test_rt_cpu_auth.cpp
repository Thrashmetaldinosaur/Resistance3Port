// Unit test for RSX_RT_CPU_AUTH tag rules (third_party/rpcs3/rpcs3/Emu/RSX/Common/r3_rt_cpu_auth.h): the env parser and
// the surface_store::on_write re-tag after a flush, including a depth-only draw that consumes the flush epoch.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/Common/r3_rt_cpu_auth.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

struct surf
{
	std::uint64_t last_use_tag = 0;
	std::uint64_t r3_cpu_flush_tag = 0;
	bool is_depth = false;
};

// Mirrors surface_store::on_write: colour target + depth target bound, per-draw write masks
struct store
{
	surf color, depth;
	std::uint64_t shared_tag = 1, write_tag = 0, cache_tag = 1, seen_epoch = 0;

	void on_write(bool color_writes, bool depth_writes)
	{
		const std::uint64_t epoch = rsx::rt_flush_epoch().load();
		if (rsx::r3_rt_on_write_skip(write_tag >= cache_tag, epoch, seen_epoch, [&]
			{
				return (color_writes && rsx::r3_rt_needs_retag(&color)) || (depth_writes && rsx::r3_rt_needs_retag(&depth));
			}))
		{
			return;
		}
		seen_epoch = epoch;
		write_tag = ++shared_tag;
		if (color_writes) color.last_use_tag = write_tag;
		if (depth_writes) depth.last_use_tag = write_tag;
	}

	// texture_cache_utils.h on_flush
	void flush(surf& s)
	{
		s.r3_cpu_flush_tag = s.last_use_tag;
		rsx::rt_flush_epoch().fetch_add(1);
	}

	bool cpu_owned(const surf& s) const { return rsx::r3_rt_cpu_owned_tags(s.is_depth, s.last_use_tag, s.r3_cpu_flush_tag); }
};

static void test_parse()
{
	for (const char* off : { "0", "off", "OFF", "False", "no", "NO", " 0 ", "\tfalse\r\n" })
		CHECK(!rsx::rt_cpu_auth_parse(off));
	for (const char* on : { (const char*)nullptr, "", "1", "on", "true", "yes", "2", "garbage", "00", "offf" })
		CHECK(rsx::rt_cpu_auth_parse(on));
}

static void test_flush_then_color_draw()
{
	store s;
	s.depth.is_depth = true;
	s.on_write(true, true);
	s.flush(s.color);
	CHECK(s.cpu_owned(s.color)); // SPU may now overwrite guest memory -> decode from memory
	s.on_write(true, false);
	CHECK(!s.cpu_owned(s.color));
}

static void test_flush_depth_only_then_color()
{
	store s;
	s.depth.is_depth = true;
	s.on_write(true, true);
	s.flush(s.color);
	s.on_write(false, true); // z-prepass / stencil pass: consumes the epoch, colour untouched
	CHECK(s.cpu_owned(s.color));
	s.on_write(true, false); // colour pass: must re-tag even though the epoch is unchanged
	CHECK(!s.cpu_owned(s.color)); // a different-format sample now reads the GPU surface
	const std::uint64_t t = s.shared_tag;
	s.on_write(true, false); // steady state: fast early-out again
	CHECK(s.shared_tag == t);
}

static void test_no_flush_fast_path()
{
	store s;
	s.on_write(true, true);
	const std::uint64_t t = s.shared_tag;
	for (int i = 0; i < 4; i++) s.on_write(i & 1, true);
	CHECK(s.shared_tag == t);
	CHECK(!s.cpu_owned(s.color));
}

static void test_cpu_written_after_gpu_redraw()
{
	static std::uint8_t mem[0x20000] = {};
	const std::uint32_t a = 0x1000, alen = 0x8000;   // FP16 light buffer
	const std::uint32_t b = 0x5000, blen = 0x8000;   // overlapping surface, other format
	std::uint64_t wt = 0;
	rsx::rt_written_note_flush(mem, a, alen, 5);
	CHECK(!rsx::rt_written_check(mem, a, alen, 5, a, a + alen)); // GPU redraw only: guest unchanged
	// (1) another surface's flush changes guest bytes: not a CPU write
	for (std::uint32_t i = b; i < b + blen; i++) mem[i] = 0x3C;
	rsx::rt_written_note_flush(mem, b, blen, 9);
	CHECK(!rsx::rt_written_check(mem, a, alen, 5, a, a + alen));
	// (3) SPU output after the flushes: seen only under the sampled range
	mem[a + 0x18] = 0xAA;
	CHECK(rsx::rt_written_check(mem, a, alen, 5, a, a + 0x800));
	CHECK(!rsx::rt_written_check(mem, a, alen, 5, a + 0x4000, a + alen));
	CHECK(!rsx::rt_written_check(mem, a, alen, 6, a, a + alen)); // another flush's surface (tag) at that range
	// (2) seen at write tag 7 -> CPU-owned until the GPU draws again, then not until the next flush
	CHECK(rsx::r3_rt_cpu_written_state(wt, 7, true) && wt == 7);
	CHECK(rsx::r3_rt_cpu_written_state(wt, 7, true));
	CHECK(!rsx::r3_rt_cpu_written_state(wt, 8, true) && wt == rsx::rt_written_consumed);
	CHECK(!rsx::r3_rt_cpu_written_state(wt, 8, true));
	wt = 0; // next flush
	CHECK(!rsx::r3_rt_cpu_written_state(wt, 8, false) && wt == 0);
}

// pitch-interleaved surfaces (960x704 FP16 at 0x30030000, 480x704 at 0x30030f00, pitch 8192) share every block: a
// flush of the second must not hide an SPU write to the first made before that flush
static void test_cpu_written_interleaved()
{
	static std::uint8_t mem[0x20000] = {};
	const std::uint32_t pitch = 0x200, rows = 32;
	const std::uint32_t a = 0x2000, alen = pitch * rows;                  // rows of 0x1E0 bytes
	const std::uint32_t b = a + 0xF0, blen = pitch * (rows - 1) + 0x80;   // rows of 0x80 bytes inside a's pitch
	const auto flush_b = [&](std::uint8_t v)
	{
		rsx::rt_written_pre_flush(mem, b, blen);
		for (std::uint32_t r = 0; r < rows; r++) std::memset(mem + b + r * pitch, v, 0x80);
		rsx::rt_written_note_flush(mem, b, blen, 11);
	};
	rsx::rt_written_note_flush(mem, a, alen, 21);
	flush_b(0x11);   // no CPU write: b's flush alone does not make a CPU-written
	CHECK(!rsx::rt_written_check(mem, a, alen, 21, a, a + alen));
	mem[a + 0x10] = 0xAA;   // SPU output into a's first row
	flush_b(0x22);
	CHECK(rsx::rt_written_check(mem, a, alen, 21, a, a + alen));
	rsx::rt_written_note_flush(mem, a, alen, 22);   // a flushed again: new baseline, sticky bits cleared
	CHECK(!rsx::rt_written_check(mem, a, alen, 22, a, a + alen));
}

int main()
{
	_putenv_s("RSX_RT_CPU_AUTH", "");
	test_parse();
	test_flush_then_color_draw();
	test_flush_depth_only_then_color();
	test_no_flush_fast_path();
	test_cpu_written_after_gpu_redraw();
	test_cpu_written_interleaved();
	std::printf(g_fail ? "FAILED\n" : "all rt_cpu_auth tests passed\n");
	return g_fail;
}
