/* Realtime allocation check: after the instance is fully set up, rendering
 * must not call malloc/realloc at all (this is what makes tsf usable on an
 * audio thread). The custom allocators below count every call. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long allocCount = 0, reallocCount = 0, freeCount = 0;

static void* counting_malloc(size_t n) { allocCount++; return malloc(n); }
static void* counting_realloc(void* p, size_t n) { reallocCount++; return realloc(p, n); }
static void counting_free(void* p) { freeCount++; free(p); }

#define TSF_MALLOC   counting_malloc
#define TSF_REALLOC  counting_realloc
#define TSF_FREE     counting_free
#define TSF_IMPLEMENTATION
#include "../tsf.h"

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else printf("ok: %s\n", msg); } while (0)

static int renderBlocks(tsf* f, int blocks)
{
	static float buf[2048];
	int i, blk, finite = 1;
	for (blk = 0; blk < blocks; blk++)
	{
		tsf_render_float(f, buf, 1024, 0);
		for (i = 0; i < 2048; i++) if (!(buf[i] == buf[i])) finite = 0;
	}
	return finite;
}

int main(int argc, char** argv)
{
	tsf* f;
	long a, r, fr;
	int i;
	if (argc < 2) { puts("usage: realtime <soundfont.sf2>"); return 2; }
	f = tsf_load_filename(argv[1]);
	if (!f) { puts("FAIL: could not load soundfont"); return 1; }

	/* Note: tsf_set_output does not allocate; tsf_set_max_voices and
	 * tsf_set_effects do, which is why they are called here, before rendering. */
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
	CHECK(tsf_set_max_voices(f, 64) == 1, "tsf_set_max_voices pre-allocates the voice pool");
	CHECK(tsf_set_effects(f, 0.5f, 0.5f) == 1, "tsf_set_effects allocates its buffers up front");

	for (i = 0; i < 16; i++) tsf_note_on(f, i % tsf_get_presetcount(f), 40 + i * 3, 0.7f);

	a = allocCount; r = reallocCount; fr = freeCount;
	CHECK(renderBlocks(f, 50) == 1, "rendering produces finite output");
	CHECK(allocCount == a, "no malloc during rendering");
	CHECK(reallocCount == r, "no realloc during rendering");
	CHECK(freeCount == fr, "no free during rendering");

	/* notes on/off and channel changes on a live instance must not allocate either */
	for (i = 0; i < 32; i++)
	{
		tsf_channel_note_on(f, 0, 50 + i % 20, 0.6f);
		tsf_channel_midi_control(f, 0, 1, (i * 7) & 127);
		tsf_channel_set_pressure(f, 0, (i * 11) & 127);
		tsf_channel_note_off(f, 0, 50 + i % 20);
	}
	a = allocCount; r = reallocCount;
	CHECK(renderBlocks(f, 20) == 1, "rendering with modulators and effects produces finite output");
	CHECK(allocCount == a && reallocCount == r, "notes, controllers and rendering stay allocation free after warm up");

	/* turning the effects off frees them (an allocation event, but not on the render path) */
	CHECK(tsf_set_effects(f, 0.0f, 0.0f) == 1, "tsf_set_effects(0, 0) disables the effect stage");
	a = allocCount;
	CHECK(renderBlocks(f, 10) == 1, "rendering without effects produces finite output");
	CHECK(allocCount == a, "no malloc during rendering without effects");

	tsf_close(f);
	printf("allocation totals: malloc %ld realloc %ld free %ld\n", allocCount, reallocCount, freeCount);
	if (failures) { printf("%d CHECK(S) FAILED\n", failures); return 1; }
	puts("REALTIME ALLOCATION TESTS PASSED");
	return 0;
}
