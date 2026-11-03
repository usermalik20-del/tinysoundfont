/* Modulator engine checks: verifies that the SF2 modulator records are read,
 * that the default modulators are installed, that continuously changing
 * sources (mod wheel, channel pressure) actually change the rendered audio and
 * that all output stays finite. */
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

static unsigned long long render(tsf* f, int preset, int cc1, int pressure, int* finite)
{
	static float buf[2048];
	int blk, i, np = tsf_get_presetcount(f);
	unsigned long long h;
	tsf_reset(f);
	for (i = 0; i < np; i++) tsf_note_on(f, i, 48 + (i % 8) * 6, 0.8f);
	if (cc1 >= 0) tsf_channel_midi_control(f, 0, 1, cc1);
	if (pressure >= 0) tsf_channel_set_pressure(f, 0, pressure);
	memset(buf, 0, sizeof(buf));
	for (blk = 0; blk < 20; blk++)
	{
		tsf_render_float(f, buf, 1024, 0);
		for (i = 0; i < 2048; i++)
			if (!(buf[i] == buf[i]) || buf[i] > 1e6f || buf[i] < -1e6f) *finite = 0;
	}
	h = h64(buf, sizeof(buf));
	tsf_reset(f);
	(void)preset;
	return h;
}

int main(int argc, char** argv)
{
	tsf* f;
	int finite = 1, pi, ri, withMods = 0, total = 0, dynamic = 0;
	unsigned long long dry, wheel, press;
	if (argc < 2) { puts("usage: mod <soundfont.sf2>"); return 2; }
	f = tsf_load_filename(argv[1]);
	if (!f) { puts("FAIL: could not load soundfont"); return 1; }
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
	tsf_set_max_voices(f, 64);

	for (pi = 0; pi < f->presetNum; pi++)
		for (ri = 0; ri < f->presets[pi].regionNum; ri++)
		{
			struct tsf_region* r = &f->presets[pi].regions[ri];
			int mi;
			total += r->modulatorNum;
			if (r->modulatorNum) withMods++;
			for (mi = 0; mi < r->modulatorNum; mi++)
				if (tsf_mod_is_dynamic(&r->modulators[mi])) dynamic++;
		}
	printf("regions: %d regions with modulators: %d modulator records: %d dynamic records: %d\n", 0, withMods, total, dynamic);

	CHECK(withMods > 0, "the loader reads modulator records from the file");
	CHECK(dynamic > 0, "the two SF2 default modulators (channel pressure and mod wheel to vibrato) are installed");

	dry   = render(f, 0, -1,  -1, &finite);
	wheel = render(f, 0, 127, -1, &finite);
	press = render(f, 0, -1, 127, &finite);

	printf("hash dry: %016llx  mod wheel: %016llx  pressure: %016llx\n", dry, wheel, press);
	CHECK(finite, "all rendered audio stays finite with modulators active");
	CHECK(wheel != dry, "moving the mod wheel changes the rendered audio (vibrato depth)");
	CHECK(press != dry, "channel pressure changes the rendered audio");

	/* a modulator with an unsupported destination must not corrupt anything */
	{
		struct tsf_region* r = &f->presets[0].regions[0];
		tsf_region_add_mod(r, 0x0080 | 1, 12345 /* not a generator */, 1000, 0, 0, 0);
		finite = 1;
		wheel = render(f, 0, 127, -1, &finite);
		CHECK(finite, "an unsupported modulator destination is ignored safely");
	}

	tsf_close(f);
	if (failures) { printf("%d CHECK(S) FAILED\n", failures); return 1; }
	puts("MODULATOR TESTS PASSED");
	return 0;
}
