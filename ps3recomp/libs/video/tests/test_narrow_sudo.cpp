// Regression test for the RSX_RT_NARROW_READ hang (port_rbattr.h rbattr::to_sudo): with the read invalidation skipped, a
// surface sharing a page with the texture keeps that page no-access, and the texture decode on the RSX thread (holding
// the texture cache lock) faulted into a handler that needs the same lock. The decode must go through the sudo view.
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_rbattr.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

// Protection of the page holding p (a no-access read would crash the test: nonzero exit = FAIL as well)
static DWORD page_prot(const void* p)
{
	MEMORY_BASIC_INFORMATION mbi{};
	return VirtualQuery(p, &mbi, sizeof mbi) ? mbi.Protect : 0;
}

int main()
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	const size_t page = si.dwAllocationGranularity; // views must be granularity aligned; 2 pages of it
	HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(page * 2), nullptr);
	CHECK(m != nullptr);
	auto* base = static_cast<uint8_t*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, page * 2));
	auto* sudo = static_cast<uint8_t*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, page * 2));
	CHECK(base && sudo && base != sudo);
	for (size_t i = 0; i < page * 2; i++)
	{
		sudo[i] = static_cast<uint8_t>(i * 7 + 3);
	}

	// Texture = the last 256 bytes of page 0 and the first 256 of page 1; a neighbour surface locks page 1 no-access
	DWORD old = 0;
	CHECK(VirtualProtect(base + page, page, PAGE_NOACCESS, &old));
	const uint8_t* tex = base + page - 256;
	uint8_t got[512] = {};

	CHECK(page_prot(tex + 256) == PAGE_NOACCESS); // the hazard: the protected view would fault on the RSX thread

	const void* s = rbattr::to_sudo(tex, base, sudo);
	CHECK(s == sudo + page - 256);
	CHECK(page_prot(static_cast<const uint8_t*>(s) + 256) == PAGE_READWRITE);
	std::memcpy(got, s, sizeof got);
	CHECK(std::memcmp(got, sudo + page - 256, sizeof got) == 0);

	// Pass-through: null, outside the guest view, missing views
	uint8_t local = 0;
	CHECK(rbattr::to_sudo(nullptr, base, sudo) == nullptr);
	CHECK(rbattr::to_sudo(&local, base, sudo) == &local || (&local >= base && &local < base + 0x100000000ull));
	CHECK(rbattr::to_sudo(tex, nullptr, sudo) == tex);
	CHECK(rbattr::to_sudo(tex, base, nullptr) == tex);
	CHECK(rbattr::to_sudo(base - 1, base, sudo) == base - 1);

	VirtualProtect(base + page, page, PAGE_READWRITE, &old);
	UnmapViewOfFile(base);
	UnmapViewOfFile(sudo);
	CloseHandle(m);
	std::printf(g_fail ? "test_narrow_sudo: FAILED\n" : "test_narrow_sudo: all checks passed\n");
	return g_fail;
}
