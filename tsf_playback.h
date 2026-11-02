/* tsf_playback.h - single owner thread serialization for TinySoundFont
 *
 * TinySoundFont is not internally synchronized: tsf_render_* on the audio
 * thread and tsf_note_* / tsf_channel_* on the control thread must never run
 * at the same time. This header wraps one tsf instance behind a lock so every
 * call is serialized, and by default lets the audio thread skip a buffer
 * instead of blocking on a busy control thread.
 *
 * Usage:
 *     #include "tsf_playback.h"
 *     tsf_playback* pb = tsf_playback_create(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
 *     // audio thread:      tsf_playback_render_float(pb, out, frames, 0);
 *     // control thread:    tsf_playback_note_on(pb, 0, 60, 0.8f);
 *     tsf_playback_destroy(pb);
 *
 * Requirements:
 *   - Call tsf_set_max_voices() (and tsf_set_effects()) before handing the
 *     instance to another thread, so recording notes never allocates.
 *   - Failures are reported through the return value: 0 means "nothing was
 *     rendered" (the lock was busy, or the arguments were invalid). The buffer
 *     is filled with silence by this header before it returns 0.
 *
 * Embedding without C11 threads:
 *   Define all of TSF_PLAYBACK_MUTEX_TYPE, TSF_PLAYBACK_MUTEX_INIT,
 *   TSF_PLAYBACK_MUTEX_LOCK, TSF_PLAYBACK_MUTEX_TRY_LOCK,
 *   TSF_PLAYBACK_MUTEX_UNLOCK and TSF_PLAYBACK_MUTEX_DESTROY before including
 *   this header (for example for pthreads, Win32 or a bare metal spin lock).
 *
 * License: MIT, same as TinySoundFont.
 */
#ifndef TSF_PLAYBACK_H
#define TSF_PLAYBACK_H

#include "tsf.h"

/* Render calls try the lock instead of waiting for the control thread. */
#ifndef TSF_PLAYBACK_RENDER_TRY_LOCK
#define TSF_PLAYBACK_RENDER_TRY_LOCK 1
#endif

#ifndef TSF_PLAYBACK_MUTEX_TYPE
#  include <threads.h>
#  define TSF_PLAYBACK_MUTEX_TYPE            mtx_t
#  define TSF_PLAYBACK_MUTEX_INIT(m)         (mtx_init((m), mtx_plain) == thrd_success)
#  define TSF_PLAYBACK_MUTEX_LOCK(m)         (mtx_lock(m) == thrd_success)
#  define TSF_PLAYBACK_MUTEX_TRY_LOCK(m)     (mtx_trylock(m) == thrd_success)
#  define TSF_PLAYBACK_MUTEX_UNLOCK(m)       (mtx_unlock(m) == thrd_success)
#  define TSF_PLAYBACK_MUTEX_DESTROY(m)      mtx_destroy(m)
#endif

#ifndef TSF_PLAYBACK_MALLOC
#  include <stdlib.h>
#  define TSF_PLAYBACK_MALLOC malloc
#  define TSF_PLAYBACK_FREE   free
#endif

#include <string.h>

struct tsf_playback
{
	tsf* f;
	TSF_PLAYBACK_MUTEX_TYPE mutex;
	int channels; /* channels per frame of the configured output mode */
};

typedef struct tsf_playback tsf_playback;

static tsf_playback* tsf_playback_create(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db)
{
	tsf_playback* p;
	if (!f) return NULL;
	if (!(samplerate > 0)) return NULL;
	p = (tsf_playback*)TSF_PLAYBACK_MALLOC(sizeof(tsf_playback));
	if (!p) return NULL;
	if (!TSF_PLAYBACK_MUTEX_INIT(&p->mutex)) { TSF_PLAYBACK_FREE(p); return NULL; }
	p->f = f;
	/* tsf_set_output takes the same lock the wrapper uses, so guard it too */
	TSF_PLAYBACK_MUTEX_LOCK(&p->mutex);
	tsf_set_output(f, outputmode, samplerate, global_gain_db);
	p->channels = (outputmode == TSF_MONO ? 1 : 2);
	TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex);
	return p;
}

static void tsf_playback_destroy(tsf_playback* p)
{
	if (!p) return;
	TSF_PLAYBACK_MUTEX_DESTROY(&p->mutex);
	TSF_PLAYBACK_FREE(p);
}

static int tsf_playback_render_float(tsf_playback* p, float* buffer, int samples, int flag_mixing)
{
	int ok;
	if (!p || !buffer || samples <= 0) return 0;
#if TSF_PLAYBACK_RENDER_TRY_LOCK
	if (!TSF_PLAYBACK_MUTEX_TRY_LOCK(&p->mutex))
	{
		/* An audio callback must never block: hand back silence instead. */
		if (!flag_mixing) memset(buffer, 0, sizeof(float) * (size_t)samples * (size_t)p->channels);
		return 0;
	}
	ok = 1;
#else
	ok = TSF_PLAYBACK_MUTEX_LOCK(&p->mutex);
	if (!ok) { if (!flag_mixing) memset(buffer, 0, sizeof(float) * (size_t)samples * (size_t)p->channels); return 0; }
#endif
	tsf_render_float(p->f, buffer, samples, flag_mixing);
	TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex);
	return 1;
}

static int tsf_playback_render_short(tsf_playback* p, short* buffer, int samples, int flag_mixing)
{
	int ok;
	if (!p || !buffer || samples <= 0) return 0;
#if TSF_PLAYBACK_RENDER_TRY_LOCK
	if (!TSF_PLAYBACK_MUTEX_TRY_LOCK(&p->mutex))
	{
		if (!flag_mixing) memset(buffer, 0, sizeof(short) * (size_t)samples * (size_t)p->channels);
		return 0;
	}
	ok = 1;
#else
	ok = TSF_PLAYBACK_MUTEX_LOCK(&p->mutex);
	if (!ok) { if (!flag_mixing) memset(buffer, 0, sizeof(short) * (size_t)samples * (size_t)p->channels); return 0; }
#endif
	tsf_render_short(p->f, buffer, samples, flag_mixing);
	TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex);
	return 1;
}

static int tsf_playback_note_on(tsf_playback* p, int preset_index, int key, float vel)
{
	int r = 0;
	if (!p) return 0;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { r = tsf_note_on(p->f, preset_index, key, vel); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
	return r;
}

static void tsf_playback_note_off(tsf_playback* p, int preset_index, int key)
{
	if (!p) return;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { tsf_note_off(p->f, preset_index, key); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
}

static void tsf_playback_note_off_all(tsf_playback* p)
{
	if (!p) return;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { tsf_note_off_all(p->f); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
}

static int tsf_playback_channel_note_on(tsf_playback* p, int channel, int key, float vel)
{
	int r = 0;
	if (!p) return 0;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { r = tsf_channel_note_on(p->f, channel, key, vel); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
	return r;
}

static void tsf_playback_channel_note_off(tsf_playback* p, int channel, int key)
{
	if (!p) return;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { tsf_channel_note_off(p->f, channel, key); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
}

static int tsf_playback_midi_control(tsf_playback* p, int channel, int controller, int value)
{
	int r = 0;
	if (!p) return 0;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { r = tsf_channel_midi_control(p->f, channel, controller, value); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
	return r;
}

static int tsf_playback_set_effects(tsf_playback* p, float chorus_level, float reverb_level)
{
	int r = 0;
	if (!p) return 0;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { r = tsf_set_effects(p->f, chorus_level, reverb_level); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
	return r;
}

static void tsf_playback_reset(tsf_playback* p)
{
	if (!p) return;
	if (TSF_PLAYBACK_MUTEX_LOCK(&p->mutex)) { tsf_reset(p->f); TSF_PLAYBACK_MUTEX_UNLOCK(&p->mutex); }
}

#endif /* TSF_PLAYBACK_H */
