/* Thread serialization check: one thread renders while another hammers notes
 * and controllers through the tsf_playback wrapper. The wrapper must keep all
 * tsf_* calls serialized (no crash, no torn state) and the audio thread must
 * never block (it skips a buffer when the lock is busy). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

#define TSF_IMPLEMENTATION
#include "../tsf.h"
#include "../tsf_playback.h"

static tsf_playback* pb;
static volatile int stopRequested = 0;
static volatile long renderOk = 0, renderSkipped = 0, notes = 0, nonFinite = 0;

static void* renderThread(void* arg)
{
	float buf[512 * 2];
	(void)arg;
	while (!stopRequested)
	{
		int i, r = tsf_playback_render_float(pb, buf, 512, 0);
		if (!r) { renderSkipped++; continue; }
		renderOk++;
		for (i = 0; i < 512 * 2; i++)
			if (!(buf[i] == buf[i]) || fabsf(buf[i]) > 1e6f) { nonFinite++; break; }
	}
	return NULL;
}

static void* controlThread(void* arg)
{
	int i = 0;
	(void)arg;
	while (!stopRequested)
	{
		int key = 36 + (i % 40), ch = i % 4;
		tsf_playback_channel_note_on(pb, ch, key, 0.4f + 0.005f * (i % 100));
		tsf_playback_midi_control(pb, ch, 1, (i * 13) & 127);   /* mod wheel */
		tsf_playback_midi_control(pb, ch, 91, (i * 7) & 127);   /* reverb send */
		tsf_playback_midi_control(pb, ch, 93, (i * 5) & 127);   /* chorus send */
		tsf_playback_channel_note_off(pb, ch, key);
		notes++;
		i++;
	}
	return NULL;
}

int main(int argc, char** argv)
{
	tsf* f;
	pthread_t tr, tc;
	struct timespec ts;
	long prevOk = 0;
	int stalled = 0, elapsedMs = 0, failed = 0;
	if (argc < 2) { puts("usage: thread <soundfont.sf2>"); return 2; }
	f = tsf_load_filename(argv[1]);
	if (!f) { puts("FAIL: could not load soundfont"); return 1; }
	/* everything that allocates happens before the instance is shared */
	if (!tsf_set_max_voices(f, 128)) { puts("FAIL: could not pre-allocate voices"); return 1; }
	if (!tsf_set_effects(f, 0.5f, 0.5f)) { puts("FAIL: could not allocate effect buffers"); return 1; }
	pb = tsf_playback_create(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
	if (!pb) { puts("FAIL: could not create the playback wrapper"); return 1; }

	if (pthread_create(&tr, NULL, renderThread, NULL) != 0) { puts("FAIL: render thread"); return 1; }
	if (pthread_create(&tc, NULL, controlThread, NULL) != 0) { puts("FAIL: control thread"); return 1; }

	while (elapsedMs < 1500)
	{
		ts.tv_sec = 0; ts.tv_nsec = 50 * 1000 * 1000;
		nanosleep(&ts, NULL);
		elapsedMs += 50;
		if (renderOk == prevOk) stalled++; else stalled = 0;
		prevOk = renderOk;
		if (stalled > 20) { puts("FAIL: the audio thread stopped making progress"); failed = 1; break; }
	}
	stopRequested = 1;
	pthread_join(tr, NULL);
	pthread_join(tc, NULL);

	printf("rendered blocks: %ld  skipped (lock busy): %ld  notes: %ld\n", renderOk, renderSkipped, notes);
	if (nonFinite) { printf("FAIL: %ld non-finite output blocks\n", nonFinite); failed = 1; }
	if (renderOk <= 0) { puts("FAIL: nothing was rendered"); failed = 1; }
	if (notes <= 0) { puts("FAIL: no notes were played"); failed = 1; }
	tsf_playback_destroy(pb);
	tsf_close(f);
	if (failed) return 1;
	puts("THREAD SERIALIZATION TESTS PASSED");
	return 0;
}
