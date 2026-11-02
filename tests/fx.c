/* Chorus / reverb effect stage checks: the stage is off by default, enabling it
 * changes the output, disabling it restores the exact dry signal, the channel
 * send controls (CC91 / CC93) feed it and everything stays finite. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#define TSF_IMPLEMENTATION
#include "../tsf.h"

static unsigned long long h64(const void* p, size_t n)
{
	unsigned long long h = 1469598103934665603ULL;
	const unsigned char* b = (const unsigned char*)p; size_t i;
	for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } else printf("ok: %s\n", msg); } while (0)

/* Re-triggers the same notes every time (tsf_reset also clears the channel
 * state) and renders a fixed number of blocks. */
static unsigned long long render(tsf* f, float chorusSend, float reverbSend, int* finite)
{
	static float buf[2048];
	int blk, i;
	unsigned long long h;
	float sendC = chorusSend, sendR = reverbSend;
	tsf_reset(f);
	tsf_channel_set_chorus(f, 0, sendC);
	tsf_channel_set_reverb(f, 0, sendR);
	for (i = 0; i < 4; i++) tsf_note_on(f, i, 55 + i * 5, 0.85f);
	memset(buf, 0, sizeof(buf));
	for (blk = 0; blk < 30; blk++)
	{
		tsf_render_float(f, buf, 1024, 0);
		for (i = 0; i < 2048; i++)
			if (!(buf[i] == buf[i]) || buf[i] > 1e6f || buf[i] < -1e6f) *finite = 0;
	}
	h = h64(buf, sizeof(buf));
	return h;
}

int main(int argc, char** argv)
{
	tsf* f;
	int finite = 1;
	unsigned long long dry, wet, off, sparse;
	if (argc < 2) { puts("usage: fx <soundfont.sf2>"); return 2; }
	f = tsf_load_filename(argv[1]);
	if (!f) { puts("FAIL: could not load soundfont"); return 1; }
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
	tsf_set_max_voices(f, 64);

	CHECK(f->effects == TSF_NULL, "the effect stage is off by default");
	dry = render(f, 1.0f, 1.0f, &finite);
	CHECK(finite, "dry rendering is finite");

	CHECK(tsf_set_effects(f, 0.6f, 0.7f) == 1, "tsf_set_effects enables the stage");
	wet = render(f, 1.0f, 1.0f, &finite);
	CHECK(finite, "rendering with chorus and reverb stays finite");
	CHECK(wet != dry, "enabling the effect stage changes the output");

	CHECK(tsf_set_effects(f, 0.0f, 0.0f) == 1, "tsf_set_effects(0, 0) disables the stage");
	off = render(f, 1.0f, 1.0f, &finite);
	CHECK(off == dry, "disabling the stage restores the exact dry output");
	CHECK(f->effects == TSF_NULL, "the effect buffers are released");

	/* the sends themselves gate the effect: a fully enabled stage with every
	 * send at zero must leave the mix untouched */
	tsf_set_effects(f, 1.0f, 1.0f);
	sparse = render(f, 0.0f, 0.0f, &finite);
	CHECK(finite, "rendering with zero sends stays finite");
	CHECK(sparse == dry, "a fully enabled stage with zero sends leaves the mix untouched");

	/* CC91 / CC93 are the MIDI way to set those sends */
	tsf_channel_midi_control(f, 0, 91, 127);
	tsf_channel_midi_control(f, 0, 93, 127);
	CHECK(tsf_channel_get_reverb(f, 0) > 0.9f, "CC91 sets the reverb send");
	CHECK(tsf_channel_get_chorus(f, 0) > 0.9f, "CC93 sets the chorus send");

	tsf_set_effects(f, 0.0f, 0.0f);
	tsf_close(f);
	if (failures) { printf("%d CHECK(S) FAILED\n", failures); return 1; }
	puts("EFFECT TESTS PASSED");
	return 0;
}
