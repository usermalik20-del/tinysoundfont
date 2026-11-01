#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"

static unsigned long long rng = 88172645463325252ULL;
static unsigned rnd(void){ rng ^= rng<<13; rng ^= rng>>7; rng ^= rng<<17; return (unsigned)(rng>>11); }

static void exercise(tsf* f)
{
	static float buf[2*512];
	int n = tsf_get_presetcount(f), i, k;
	tsf_set_output(f, (enum TSFOutputMode)(rnd()%3), 44100, 0);
	if (n > 0) {
		tsf_set_max_voices(f, 8);
		for (i = 0; i < 12; i++) {
			tsf_note_on(f, rnd() % (n+2) - 1, (int)(rnd()%140) - 6, (rnd()%100)/50.0f);
			tsf_channel_set_presetindex(f, rnd()%16, rnd()%n);
			tsf_channel_note_on(f, rnd()%16, rnd()%128, 0.8f);
			tsf_channel_midi_control(f, rnd()%16, rnd()%130, (int)(rnd()%300)-20);
			for (k = 0; k < 4; k++) tsf_render_float(f, buf, 256, k&1);
		}
		tsf_channel_note_off_all(f, 0);
		for (k = 0; k < 40; k++) tsf_render_float(f, buf, 256, 0);
	}
	tsf_note_off_all(f);
}

int main(int argc, char** argv)
{
	FILE* fp = fopen(argv[1], "rb"); long sz; unsigned char *orig, *m; int iters = atoi(argv[2]), it, loaded = 0;
	fseek(fp,0,SEEK_END); sz = ftell(fp); fseek(fp,0,SEEK_SET);
	orig = malloc(sz); m = malloc(sz); fread(orig,1,sz,fp); fclose(fp);
	for (it = 0; it < iters; it++)
	{
		long len = sz; int muts = 1 + rnd()%6, j; tsf* f;
		memcpy(m, orig, sz);
		for (j = 0; j < muts; j++) {
			long pos = (rnd()%3==0) ? rnd()%2000 : rnd()%sz; // bias towards the header / hydra tables
			switch (rnd()%4) {
				case 0: m[pos] ^= 1 << (rnd()%8); break;
				case 1: m[pos] = (unsigned char)rnd(); break;
				case 2: if (pos+4 <= sz) { unsigned v = (rnd()%2) ? 0xFFFFFFFFu : rnd(); memcpy(m+pos,&v,4);} break;
				case 3: len = pos > 12 ? pos : 12; break;
			}
		}
		f = tsf_load_memory(m, (int)len);
		if (f) { loaded++; exercise(f); tsf_close(f); }
	}
	printf("%d/%d mutated fonts still loaded, no crash\n", loaded, iters);
	return 0;
}
