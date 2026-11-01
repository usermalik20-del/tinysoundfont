#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
static unsigned long long h = 1469598103934665603ULL;
static void hash(const void* p, size_t n){ const unsigned char* b=p; while(n--){ h ^= *b++; h *= 1099511628211ULL; } }
int main(int argc, char** argv)
{
	tsf* f = tsf_load_filename(argv[1]); static float buf[2*441]; int p, i, blk, np;
	if (!f) { puts("load failed"); return 1; }
	np = tsf_get_presetcount(f);
	tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, -6.0f);
	printf("presets: %d\n", np);
	for (p = 0; p < np; p++)
	{
		tsf_reset(f);
		for (i = 0; i < 5; i++) tsf_note_on(f, p, 36 + i*7, 0.3f + 0.15f*i);
		for (blk = 0; blk < 30; blk++) { tsf_render_float(f, buf, 441, 0); hash(buf, sizeof(buf)); if (blk == 12) tsf_note_off_all(f); }
		tsf_render_short(f, (short*)buf, 441, 0); hash(buf, 441*2*sizeof(short));
	}
	// channel API: pitch bend, sustain, pan, volume, program
	tsf_reset(f);
	for (i = 0; i < 4; i++) { tsf_channel_set_presetnumber(f, i, i*3, i==9); tsf_channel_set_pan(f, i, 0.2f*i); tsf_channel_midi_control(f, i, 7, 90+i); }
	for (blk = 0; blk < 60; blk++)
	{
		if (blk % 10 == 0) { for (i = 0; i < 4; i++) tsf_channel_note_on(f, i, 48 + blk/10*2 + i, 0.7f); }
		if (blk == 20) tsf_channel_set_pitchwheel(f, 1, 12000);
		if (blk == 25) tsf_channel_midi_control(f, 0, 64, 127);
		if (blk == 30) { tsf_channel_note_off(f, 0, 48); }
		if (blk == 40) tsf_channel_midi_control(f, 0, 64, 0);
		tsf_render_float(f, buf, 441, 0); hash(buf, sizeof(buf));
	}
	printf("hash: %016llx  active voices at end: %d\n", h, tsf_active_voice_count(f));
	tsf_close(f); return 0;
}
