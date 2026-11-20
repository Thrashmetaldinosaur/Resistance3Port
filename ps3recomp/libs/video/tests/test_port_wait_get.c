/* R3_FIFO_WAIT_GET_MS (RSX\port_wait_get.h): a blocking NV406E acquire must not publish GET past words the puller has
 * not executed until it has waited the delay, so the game cannot recycle a segment's exit slot before the exit JUMP
 * there was fetched (2026-11-17 desyncs: an EA pointer executed at io 0x80a138). */
#include "../../../third_party/rpcs3/rpcs3/Emu/RSX/port_wait_get.h"

#include <stdio.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while (0)

/* Puller model: segment [0x80a000, E) linked, PUT = E = 0x80a138 holds the exit JUMP the game writes later.
 * The acquire's arg word sits at 0x80a100; the fetch cache ends at PUT. */
enum { POS = 0x80a100, EXIT = 0x80a138 };

/* The game recycles the segment (overwriting the exit slot) once GET reached E; the slot is safe while GET < E. */
static int game_recycles(unsigned get) { return get >= EXIT; }

int main(void)
{
	const uint64_t delay = 20000;
	unsigned get = 0x80a0f0;

	/* acquire begins: publishes its own position only */
	unsigned g = port_wait_get(get, POS, EXIT, delay, 0);
	CHECK(g == POS);
	get = g ? g : get;
	CHECK(!game_recycles(get));

	/* the SPU store arrives within the delay: GET never went past the acquire, the exit slot is still owned */
	g = port_wait_get(get, POS, EXIT, delay, 19999);
	CHECK(g == 0); /* same value: leave GET */
	CHECK(!game_recycles(get));

	/* the wait lasts: the cache end is published so an application waiting for GET == PUT is not deadlocked */
	g = port_wait_get(get, POS, EXIT, delay, 20000);
	CHECK(g == EXIT);

	/* delay 0 = upstream PR #20055 behaviour: cache end at once (the leak) */
	CHECK(port_wait_get(0x80a0f0, POS, EXIT, 0, 0) == EXIT);
	CHECK(game_recycles(port_wait_get(0x80a0f0, POS, EXIT, 0, 0)));

	/* never backwards: a GET already farther (e.g. a label publication) is left alone */
	CHECK(port_wait_get(0x80a120, POS, EXIT, delay, 0) == 0);
	CHECK(port_wait_get(EXIT, POS, EXIT, delay, 50000) == 0);
	CHECK(port_wait_get(0x80a120, POS, EXIT, delay, 50000) == EXIT);

	/* GET still in another buffer (far above): the early publication leaves it, the late one still escapes */
	CHECK(port_wait_get(0x39ffd20, POS, EXIT, delay, 0) == 0);
	CHECK(port_wait_get(0x39ffd20, POS, EXIT, delay, 20000) == EXIT);
	CHECK(port_wait_get(0x7ff000, POS, EXIT, delay, 0) == POS);

	if (!g_fail) printf("test_port_wait_get: all checks passed\n");
	return g_fail;
}
