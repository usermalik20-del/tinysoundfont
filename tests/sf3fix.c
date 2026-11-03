/* SF3 (Ogg Vorbis sample) checks. Build with stb_vorbis included:
 *   gcc -I.. -include /path/stb_vorbis.c -o sf3fix sf3fix.c -lm
 * Without stb_vorbis the SF3 code path does not exist, so the test reports
 * that it was skipped instead of failing.
 */
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

int main(int argc, char** argv)
{
	tsf* f;
	const char* path = (argc > 1 ? argv[1] : "data/tone.sf3");
	static float buf[1024];
	int i, blk, np, finite = 1, peak = 0;
	unsigned long long h;
#ifndef STB_VORBIS_INCLUDE_STB_VORBIS_H
	printf("SKIPPED: built without stb_vorbis, the SF3 path is compiled out\n");
	(void)path; (void)f; (void)finite; (void)peak; (void)h; (void)buf; (void)i; (void)blk; (void)np;
	return 0;
#else
	f = tsf_load_filename(path);
	if (!f) { printf("FAIL: could not load %s\n", path); return 1; }
	np = tsf_get_presetcount(f);
	printf("presets: %d\n", np);
	if (np < 1) { puts("FAIL: no presets"); return 1; }
	if (f->fontSamples == TSF_NULL) { puts("FAIL: no decoded samples"); return 1; }
	printf("region sample range: %d..%d\n", (int)f->presets[0].regions[0].offset, (int)f->presets[0].regions[0].end);
	if ((int)(f->presets[0].regions[0].end - f->presets[0].regions[0].offset) < 100) { puts("FAIL: decoded sample data looks too short"); return 1; }

	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
	tsf_set_max_voices(f, 32);
	tsf_note_on(f, 0, 60, 0.9f);
	if (tsf_active_voice_count(f) < 1) { puts("FAIL: note on produced no voice"); return 1; }
	memset(buf, 0, sizeof(buf));
	for (blk = 0; blk < 30; blk++)
	{
		tsf_render_float(f, buf, 512, 0);
		for (i = 0; i < 1024; i++)
		{
			if (!(buf[i] == buf[i]) || buf[i] > 1e6f || buf[i] < -1e6f) finite = 0;
			if (fabsf(buf[i]) > peak) peak = (int)(fabsf(buf[i]) * 1e6f);
		}
	}
	h = h64(buf, sizeof(buf));
	printf("hash: %016llx\n", h);
	if (!finite) { puts("FAIL: rendered audio is not finite"); return 1; }
	if (peak <= 0) { puts("FAIL: rendered audio is silent"); return 1; }
	tsf_close(f);
	puts("SF3 FIXTURE TESTS PASSED");
	return 0;
#endif
}
