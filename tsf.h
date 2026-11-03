/* TinySoundFont - v0.9 - SoundFont2 synthesizer - https://github.com/schellingb/TinySoundFont
                                     no warranty implied; use at your own risk
   Do this:
      #define TSF_IMPLEMENTATION
   before you include this file in *one* C or C++ file to create the implementation.
   // i.e. it should look like this:
   #include ...
   #include ...
   #define TSF_IMPLEMENTATION
   #include "tsf.h"

   [OPTIONAL] #define TSF_NO_STDIO to remove stdio dependency
   [OPTIONAL] #define TSF_MALLOC, TSF_REALLOC, and TSF_FREE to avoid stdlib.h
   [OPTIONAL] #define TSF_MEMCPY, TSF_MEMSET to avoid string.h
   [OPTIONAL] #define TSF_POW, TSF_POWF, TSF_EXPF, TSF_LOG, TSF_TAN, TSF_LOG10, TSF_SQRT to avoid math.h

   [OPTIONAL] #define TSF_NO_VOICE_STEALING to drop new notes (instead of stealing the oldest
              voice) when a tsf_set_max_voices limit is reached and no voice is releasing

   HARDENING NOTES (this copy)
     - SoundFont loading validates all table indices and sizes and fails cleanly on corrupt input
     - Sample buffer has one padding sample so interpolation can never read past the end
     - Invalid channel / controller / velocity input is clamped or rejected instead of misbehaving

   QUALITY DEFAULTS (this fork)
     - Catmull-Rom spline interpolation instead of linear (TSF_HIGH_QUALITY_INTERPOLATION,
       define it to 0 for the old linear path)
     - RBJ low-pass biquad with clamped coefficients that ramps between blocks
       instead of jumping, so cutoff modulation no longer zippers
     - SF2 modulators (per zone and per preset, plus the SF2 default set) with
       note-on sources (velocity, key) and continuously updated sources
       (MIDI CC, channel pressure, pitch wheel) driving filter cutoff,
       attenuation, vibrato / mod LFO depths, pitch and effect sends
     - ChorusEffectsSend and ReverbEffectsSend generators plus a chorus and
       reverb stage (tsf_set_effects, CC91 / CC93)

   NOT IMPLEMENTED (and not claimed)
     - The SF2.01 8.4.2 "velocity to filter cutoff" default modulator is
       installed but neutralised, exactly like FluidSynth does, because the
       specification's own definition is inconsistent; set
       TSF_DEFAULT_VELOCITY_TO_FILTER to 1 to enable it
     - Modulator destinations other than the ones listed above are ignored
     - Effect sends are applied to the mix as a whole, not per voice
     - Poly (key) pressure is not tracked, so modulators using it evaluate to 0

   LICENSE (MIT)

   Copyright (C) 2017-2025 Bernhard Schelling
   Based on SFZero, Copyright (C) 2012 Steve Folta (https://github.com/stevefolta/SFZero)

   Permission is hereby granted, free of charge, to any person obtaining a copy of this
   software and associated documentation files (the "Software"), to deal in the Software
   without restriction, including without limitation the rights to use, copy, modify, merge,
   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
   to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
   INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
   PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
   LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
   USE OR OTHER DEALINGS IN THE SOFTWARE.

*/

#ifndef TSF_INCLUDE_TSF_INL
#define TSF_INCLUDE_TSF_INL

#ifdef __cplusplus
extern "C" {
#  define CPP_DEFAULT0 = 0
#else
#  define CPP_DEFAULT0
#endif

//define this if you want the API functions to be static
#ifdef TSF_STATIC
#define TSFDEF static
#else
#define TSFDEF extern
#endif

// The load functions will return a pointer to a struct tsf which all functions
// thereafter take as the first parameter.
// On error the tsf_load* functions will return NULL most likely due to invalid
// data (or if the file did not exist in tsf_load_filename).
typedef struct tsf tsf;

#ifndef TSF_NO_STDIO
// Directly load a SoundFont from a .sf2 file path
TSFDEF tsf* tsf_load_filename(const char* filename);
#endif

// Load a SoundFont from a block of memory
TSFDEF tsf* tsf_load_memory(const void* buffer, int size);

// Stream structure for the generic loading
struct tsf_stream
{
	// Custom data given to the functions as the first parameter
	void* data;

	// Function pointer will be called to read 'size' bytes into ptr (returns number of read bytes)
	int (*read)(void* data, void* ptr, unsigned int size);

	// Function pointer will be called to skip ahead over 'count' bytes (returns 1 on success, 0 on error)
	int (*skip)(void* data, unsigned int count);
};

// Generic SoundFont loading method using the stream structure above
TSFDEF tsf* tsf_load(struct tsf_stream* stream);

// Copy a tsf instance from an existing one, use tsf_close to close it as well.
// All copied tsf instances and their original instance are linked, and share the underlying soundfont.
// This allows loading a soundfont only once, but using it for multiple independent playbacks.
// (This function isn't thread-safe without locking.)
TSFDEF tsf* tsf_copy(tsf* f);

// Free the memory related to this tsf instance
TSFDEF void tsf_close(tsf* f);

// Stop all playing notes immediately and reset all channel parameters
TSFDEF void tsf_reset(tsf* f);

// Returns the preset index from a bank and preset number, or -1 if it does not exist in the loaded SoundFont
TSFDEF int tsf_get_presetindex(const tsf* f, int bank, int preset_number);

// Returns the number of presets in the loaded SoundFont
TSFDEF int tsf_get_presetcount(const tsf* f);

// Returns the name of a preset index >= 0 and < tsf_get_presetcount()
TSFDEF const char* tsf_get_presetname(const tsf* f, int preset_index);

// Returns the name of a preset by bank and preset number
TSFDEF const char* tsf_bank_get_presetname(const tsf* f, int bank, int preset_number);

// Supported output modes by the render methods
enum TSFOutputMode
{
	// Two channels with single left/right samples one after another
	TSF_STEREO_INTERLEAVED,
	// Two channels with all samples for the left channel first then right
	TSF_STEREO_UNWEAVED,
	// A single channel (stereo instruments are mixed into center)
	TSF_MONO
};

// Thread safety:
//
// 1. Rendering / voices:
//
// Your audio output which calls the tsf_render* functions will most likely
// run on a different thread than where the playback tsf_note* functions
// are called. In which case some sort of concurrency control like a
// mutex needs to be used so they are not called at the same time.
// Alternatively, you can pre-allocate a maximum number of voices that can
// play simultaneously by calling tsf_set_max_voices after loading.
// That way memory re-allocation will not happen during tsf_note_on and
// TSF should become mostly thread safe.
// There is a theoretical chance that ending notes would negatively influence
// a voice that is rendering at the time but it is hard to say.
// Also be aware, this has not been tested much.
//
// 2. Channels:
//
// Calls to tsf_channel_set_... functions may allocate new channels
// if no channel with that number was previously used. Make sure to
// create all channels at the beginning as required if you call tsf_render*
// from a different thread.

// Setup the parameters for the voice render methods
//   outputmode: if mono or stereo and how stereo channel data is ordered
//   samplerate: the number of samples per second (output frequency)
//   global_gain_db: volume gain in decibels (>0 means higher, <0 means lower)
TSFDEF void tsf_set_output(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db CPP_DEFAULT0);

// Set the global gain as a volume factor
//   global_gain: the desired volume where 1.0 is 100%
TSFDEF void tsf_set_volume(tsf* f, float global_gain);

// Enable the built-in chorus and reverb effect stage, with each level from 0.0
// (off) to 1.0 (full). Buffers are allocated here so rendering stays
// allocation free. The amount of effect that is actually applied is scaled by
// the sends of the currently playing voices (the region ChorusEffectsSend and
// ReverbEffectsSend generators, their modulators and the per channel sends).
//   (tsf_set_effects returns 0 if the buffers could not be allocated)
TSFDEF int tsf_set_effects(tsf* f, float chorus_level, float reverb_level);

// Set the maximum number of voices to play simultaneously
// Depending on the soundfond, one note can cause many new voices to be started,
// so don't keep this number too low or otherwise sounds may not play.
//   max_voices: maximum number to pre-allocate and set the limit to
//   (tsf_set_max_voices returns 0 if allocation failed, otherwise 1)
TSFDEF int tsf_set_max_voices(tsf* f, int max_voices);

// Start playing a note
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   bank: instrument bank number (alternative to preset_index)
//   preset_number: preset number (alternative to preset_index)
//   (tsf_note_on returns 0 if the allocation of a new voice failed, otherwise 1)
//   (tsf_bank_note_on returns 0 if preset does not exist or allocation failed, otherwise 1)
TSFDEF int tsf_note_on(tsf* f, int preset_index, int key, float vel);
TSFDEF int tsf_bank_note_on(tsf* f, int bank, int preset_number, int key, float vel);

// Stop playing a note
//   (bank_note_off returns 0 if preset does not exist, otherwise 1)
TSFDEF void tsf_note_off(tsf* f, int preset_index, int key);
TSFDEF int  tsf_bank_note_off(tsf* f, int bank, int preset_number, int key);

// Stop playing all notes (end with sustain and release)
TSFDEF void tsf_note_off_all(tsf* f);

// Returns the number of active voices
TSFDEF int tsf_active_voice_count(tsf* f);

// Render output samples into a buffer
// You can either render as signed 16-bit values (tsf_render_short) or
// as 32-bit float values (tsf_render_float)
//   buffer: target buffer of size samples * output_channels * sizeof(type)
//   samples: number of samples to render
//   flag_mixing: if 0 clear the buffer first, otherwise mix into existing data
TSFDEF void tsf_render_short(tsf* f, short* buffer, int samples, int flag_mixing CPP_DEFAULT0);
TSFDEF void tsf_render_float(tsf* f, float* buffer, int samples, int flag_mixing CPP_DEFAULT0);

// Higher level channel based functions, set up channel parameters
//   channel: channel number
//   preset_index: preset index >= 0 and < tsf_get_presetcount()
//   preset_number: preset number (alternative to preset_index)
//   flag_mididrums: 0 for normal channels, otherwise apply MIDI drum channel rules
//   bank: instrument bank number (alternative to preset_index)
//   pan: stereo panning value from 0.0 (left) to 1.0 (right) (default 0.5 center)
//   volume: linear volume scale factor (default 1.0 full)
//   pitch_wheel: pitch wheel position 0 to 16383 (default 8192 unpitched)
//   pitch_range: range of the pitch wheel in semitones (default 2.0, total +/- 2 semitones)
//   tuning: tuning of all playing voices in semitones (default 0.0, standard (A440) tuning)
//   flag_sustain: 0 to end notes that were held sustained and disable holding sustain otherwise enable it
//   (tsf_set_preset_number and set_bank_preset return 0 if preset does not exist, otherwise 1)
//   (tsf_channel_set_... return 0 if a new channel needed allocation and that failed, otherwise 1)
TSFDEF int tsf_channel_set_presetindex(tsf* f, int channel, int preset_index);
TSFDEF int tsf_channel_set_presetnumber(tsf* f, int channel, int preset_number, int flag_mididrums CPP_DEFAULT0);
TSFDEF int tsf_channel_set_bank(tsf* f, int channel, int bank);
TSFDEF int tsf_channel_set_bank_preset(tsf* f, int channel, int bank, int preset_number);
TSFDEF int tsf_channel_set_pan(tsf* f, int channel, float pan);
TSFDEF int tsf_channel_set_volume(tsf* f, int channel, float volume);
TSFDEF int tsf_channel_set_pitchwheel(tsf* f, int channel, int pitch_wheel);
TSFDEF int tsf_channel_set_pitchrange(tsf* f, int channel, float pitch_range);
TSFDEF int tsf_channel_set_tuning(tsf* f, int channel, float tuning);
TSFDEF int tsf_channel_set_sustain(tsf* f, int channel, int flag_sustain);

// Start or stop playing notes on a channel (needs channel preset to be set)
//   channel: channel number
//   key: note value between 0 and 127 (60 being middle C)
//   vel: velocity as a float between 0.0 (equal to note off) and 1.0 (full)
//   (tsf_channel_note_on returns 0 on allocation failure of new voice, otherwise 1)
TSFDEF int tsf_channel_note_on(tsf* f, int channel, int key, float vel);
TSFDEF void tsf_channel_note_off(tsf* f, int channel, int key);
TSFDEF void tsf_channel_note_off_all(tsf* f, int channel); //end with sustain and release
TSFDEF void tsf_channel_sounds_off_all(tsf* f, int channel); //end immediately

// Apply a MIDI control change to the channel (not all controllers are supported!)
//    (tsf_channel_midi_control returns 0 on allocation failure of new channel, otherwise 1)
TSFDEF int tsf_channel_midi_control(tsf* f, int channel, int controller, int control_value);

// Get current values set on the channels
TSFDEF int tsf_channel_get_preset_index(tsf* f, int channel);
TSFDEF int tsf_channel_get_preset_bank(tsf* f, int channel);
TSFDEF int tsf_channel_get_preset_number(tsf* f, int channel);
TSFDEF float tsf_channel_get_pan(tsf* f, int channel);
TSFDEF float tsf_channel_get_volume(tsf* f, int channel);
TSFDEF int tsf_channel_get_pitchwheel(tsf* f, int channel);
TSFDEF float tsf_channel_get_pitchrange(tsf* f, int channel);
TSFDEF float tsf_channel_get_tuning(tsf* f, int channel);
TSFDEF int tsf_channel_set_reverb(tsf* f, int channel, float send);
TSFDEF float tsf_channel_get_reverb(tsf* f, int channel);
TSFDEF int tsf_channel_set_chorus(tsf* f, int channel, float send);
TSFDEF float tsf_channel_get_chorus(tsf* f, int channel);
TSFDEF int tsf_channel_set_pressure(tsf* f, int channel, int pressure);
TSFDEF int tsf_channel_get_pressure(tsf* f, int channel);

#ifdef __cplusplus
#  undef CPP_DEFAULT0
}
#endif

// end header
// ---------------------------------------------------------------------------------------------------------
#endif //TSF_INCLUDE_TSF_INL

#ifdef TSF_IMPLEMENTATION
#undef TSF_IMPLEMENTATION

// The lower this block size is the more accurate the effects are.
// Increasing the value significantly lowers the CPU usage of the voice rendering.
// If LFO affects the low-pass filter it can be hearable even as low as 8.
#ifndef TSF_RENDER_EFFECTSAMPLEBLOCK
#define TSF_RENDER_EFFECTSAMPLEBLOCK 64
#endif

// Enable high-quality Catmull-Rom interpolation (default on for quality)
#ifndef TSF_HIGH_QUALITY_INTERPOLATION
#define TSF_HIGH_QUALITY_INTERPOLATION 1
#endif

// Maximum number of modulators stored per region (SF2 files rarely use more)
#ifndef TSF_MAX_MODULATORS
#define TSF_MAX_MODULATORS 8
#endif

// The SF2.01 default "velocity to filter cutoff" modulator (section 8.4.2) is
// broken in the specification itself and is disabled by most synths (FluidSynth
// returns zero for it). Set this to 1 to enable it.
#ifndef TSF_DEFAULT_VELOCITY_TO_FILTER
#define TSF_DEFAULT_VELOCITY_TO_FILTER 0
#endif

// When using tsf_render_short, to do the conversion a buffer of a fixed size is
// allocated on the stack. On low memory platforms this could be made smaller.
// Increasing this above 512 should not have a significant impact on performance.
// The value should be a multiple of TSF_RENDER_EFFECTSAMPLEBLOCK.
#ifndef TSF_RENDER_SHORTBUFFERBLOCK
#define TSF_RENDER_SHORTBUFFERBLOCK 512
#endif

// Grace release time for quick voice off (avoid clicking noise)
#define TSF_FASTRELEASETIME 0.01f

/* Resource limits - adjust or override before including tsf.h */
#ifndef TSF_MAX_SAMPLES
#  define TSF_MAX_SAMPLES (128u * 1024u * 1024u)      /* 128 M samples max */
#endif
#ifndef TSF_MAX_PRESETS
#  define TSF_MAX_PRESETS 4096
#endif
#ifndef TSF_MAX_REGIONS_PER_PRESET
#  define TSF_MAX_REGIONS_PER_PRESET 512
#endif
#ifndef TSF_MAX_HYDRA_RECORDS
#  define TSF_MAX_HYDRA_RECORDS 65536
#endif
#ifndef TSF_MAX_ALLOCATION
#  define TSF_MAX_ALLOCATION (256u * 1024u * 1024u)   /* 256 MiB per single alloc */
#endif
#ifndef TSF_MAX_MIDI_MESSAGES
#  define TSF_MAX_MIDI_MESSAGES 100000
#endif
#ifndef TSF_MAX_MIDI_TRACK_LENGTH
#  define TSF_MAX_MIDI_TRACK_LENGTH (16u * 1024u * 1024u) /* 16 MiB */
#endif

/* Helper: size_t-safe allocation guard */
#define TSF_ALLOC_GUARD(nmemb, size) \
  ((size_t)(nmemb) <= (size_t)-1 / (size) && (size_t)(nmemb) * (size) <= TSF_MAX_ALLOCATION ? (nmemb) : 0)

#if !defined(TSF_MALLOC) || !defined(TSF_FREE) || !defined(TSF_REALLOC)
#  include <stdlib.h>
#  define TSF_MALLOC  malloc
#  define TSF_FREE    free
#  define TSF_REALLOC realloc
#endif

#if !defined(TSF_MEMCPY) || !defined(TSF_MEMSET)
#  include <string.h>
#  define TSF_MEMCPY  memcpy
#  define TSF_MEMSET  memset
#endif

#if !defined(TSF_POW) || !defined(TSF_POWF) || !defined(TSF_EXPF) || !defined(TSF_LOG) || !defined(TSF_TAN) || !defined(TSF_LOG10) || !defined(TSF_SQRT)
#  include <math.h>
#  if !defined(__cplusplus) && !defined(NAN) && !defined(powf) && !defined(expf) && !defined(sqrtf)
#    define powf (float)pow // deal with old math.h
#    define expf (float)exp // files that come without
#    define sqrtf (float)sqrt // powf, expf and sqrtf
#  endif
#  define TSF_POW     pow
#  define TSF_POWF    powf
#  define TSF_EXPF    expf
#  define TSF_LOG     log
#  define TSF_TAN     tan
#  define TSF_LOG10   log10
#  define TSF_SQRTF   sqrtf
#  define TSF_SIN     sin
#endif

#ifndef TSF_NO_STDIO
#  include <stdio.h>
#endif

#define TSF_TRUE 1
#define TSF_FALSE 0
#define TSF_BOOL unsigned char
#define TSF_PI 3.14159265358979323846264338327950288
#define TSF_NULL 0

#ifdef __cplusplus
extern "C" {
#endif

typedef char tsf_fourcc[4];
typedef signed char tsf_s8;
typedef unsigned char tsf_u8;
typedef unsigned short tsf_u16;
typedef signed short tsf_s16;
typedef unsigned int tsf_u32;
typedef char tsf_char20[20];

#define TSF_FourCCEquals(value1, value2) (value1[0] == value2[0] && value1[1] == value2[1] && value1[2] == value2[2] && value1[3] == value2[3])

struct tsf
{
	struct tsf_preset* presets;
	float* fontSamples;
	struct tsf_voice* voices;
	struct tsf_channels* channels;

	int presetNum;
	int voiceNum;
	int maxVoiceNum;
	unsigned int voicePlayIndex;

	enum TSFOutputMode outputmode;
	float outSampleRate;
	float globalGainDB;
	int* refCount;
	struct tsf_effects* effects;
};

#ifndef TSF_NO_STDIO
static int tsf_stream_stdio_read(void* data, void* ptr, unsigned int size) { return (int)fread(ptr, 1, size, (FILE*)data); }
static int tsf_stream_stdio_skip(void* data, unsigned int count)
{
	FILE* f = (FILE*)data;
	// fseek takes a signed long which can be only 32 bits wide, so skip huge counts in smaller steps
	while (count > 0x40000000u) { if (fseek(f, 0x40000000L, SEEK_CUR)) return 0; count -= 0x40000000u; }
	return !fseek(f, (long)count, SEEK_CUR);
}
TSFDEF tsf* tsf_load_filename(const char* filename)
{
	tsf* res;
	struct tsf_stream stream = { TSF_NULL, tsf_stream_stdio_read, tsf_stream_stdio_skip };
	#if __STDC_WANT_SECURE_LIB__
	FILE* f = TSF_NULL; fopen_s(&f, filename, "rb");
	#else
	FILE* f = fopen(filename, "rb");
	#endif
	if (!f)
	{
		//if (e) *e = TSF_FILENOTFOUND;
		return TSF_NULL;
	}
	stream.data = f;
	res = tsf_load(&stream);
	fclose(f);
	return res;
}
#endif

struct tsf_stream_memory { const char* buffer; unsigned int total, pos; };
static int tsf_stream_memory_read(void* data, void* ptr, unsigned int size) { struct tsf_stream_memory* m = (struct tsf_stream_memory*)data; if (size > m->total - m->pos) size = m->total - m->pos; TSF_MEMCPY(ptr, m->buffer+m->pos, size); m->pos += size; return size; }
static int tsf_stream_memory_skip(void* data, unsigned int count) { struct tsf_stream_memory* m = (struct tsf_stream_memory*)data; if (count > m->total - m->pos) return 0; m->pos += count; return 1; }
TSFDEF tsf* tsf_load_memory(const void* buffer, int size)
{
	struct tsf_stream stream = { TSF_NULL, tsf_stream_memory_read, tsf_stream_memory_skip };
	struct tsf_stream_memory f = { 0, 0, 0 };
	if (!buffer || size <= 0) return TSF_NULL;
	f.buffer = (const char*)buffer;
	f.total = (unsigned int)size;
	stream.data = &f;
	return tsf_load(&stream);
}

enum { TSF_LOOPMODE_NONE, TSF_LOOPMODE_CONTINUOUS, TSF_LOOPMODE_SUSTAIN };

enum { TSF_SEGMENT_NONE, TSF_SEGMENT_DELAY, TSF_SEGMENT_ATTACK, TSF_SEGMENT_HOLD, TSF_SEGMENT_DECAY, TSF_SEGMENT_SUSTAIN, TSF_SEGMENT_RELEASE, TSF_SEGMENT_DONE };

struct tsf_hydra
{
	struct tsf_hydra_phdr *phdrs; struct tsf_hydra_pbag *pbags; struct tsf_hydra_pmod *pmods;
	struct tsf_hydra_pgen *pgens; struct tsf_hydra_inst *insts; struct tsf_hydra_ibag *ibags;
	struct tsf_hydra_imod *imods; struct tsf_hydra_igen *igens; struct tsf_hydra_shdr *shdrs;
	int phdrNum, pbagNum, pmodNum, pgenNum, instNum, ibagNum, imodNum, igenNum, shdrNum;
};

union tsf_hydra_genamount { struct { tsf_u8 lo, hi; } range; tsf_s16 shortAmount; tsf_u16 wordAmount; };
struct tsf_hydra_phdr { tsf_char20 presetName; tsf_u16 preset, bank, presetBagNdx; tsf_u32 library, genre, morphology; };
struct tsf_hydra_pbag { tsf_u16 genNdx, modNdx; };
struct tsf_hydra_pmod { tsf_u16 modSrcOper, modDestOper; tsf_s16 modAmount; tsf_u16 modAmtSrcOper, modTransOper; };
struct tsf_hydra_pgen { tsf_u16 genOper; union tsf_hydra_genamount genAmount; };
struct tsf_hydra_inst { tsf_char20 instName; tsf_u16 instBagNdx; };
struct tsf_hydra_ibag { tsf_u16 instGenNdx, instModNdx; };
struct tsf_hydra_imod { tsf_u16 modSrcOper, modDestOper; tsf_s16 modAmount; tsf_u16 modAmtSrcOper, modTransOper; };
struct tsf_hydra_igen { tsf_u16 genOper; union tsf_hydra_genamount genAmount; };
struct tsf_hydra_shdr { tsf_char20 sampleName; tsf_u32 start, end, startLoop, endLoop, sampleRate; tsf_u8 originalPitch; tsf_s8 pitchCorrection; tsf_u16 sampleLink, sampleType; };

#define TSFR(FIELD) if (stream->read(stream->data, &i->FIELD, sizeof(i->FIELD)) != (int)sizeof(i->FIELD)) return 0;
static int tsf_hydra_read_phdr(struct tsf_hydra_phdr* i, struct tsf_stream* stream) { TSFR(presetName) TSFR(preset) TSFR(bank) TSFR(presetBagNdx) TSFR(library) TSFR(genre) TSFR(morphology) return 1; }
static int tsf_hydra_read_pbag(struct tsf_hydra_pbag* i, struct tsf_stream* stream) { TSFR(genNdx) TSFR(modNdx) return 1; }
static int tsf_hydra_read_pmod(struct tsf_hydra_pmod* i, struct tsf_stream* stream) { TSFR(modSrcOper) TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper) return 1; }
static int tsf_hydra_read_pgen(struct tsf_hydra_pgen* i, struct tsf_stream* stream) { TSFR(genOper) TSFR(genAmount) return 1; }
static int tsf_hydra_read_inst(struct tsf_hydra_inst* i, struct tsf_stream* stream) { TSFR(instName) TSFR(instBagNdx) return 1; }
static int tsf_hydra_read_ibag(struct tsf_hydra_ibag* i, struct tsf_stream* stream) { TSFR(instGenNdx) TSFR(instModNdx) return 1; }
static int tsf_hydra_read_imod(struct tsf_hydra_imod* i, struct tsf_stream* stream) { TSFR(modSrcOper) TSFR(modDestOper) TSFR(modAmount) TSFR(modAmtSrcOper) TSFR(modTransOper) return 1; }
static int tsf_hydra_read_igen(struct tsf_hydra_igen* i, struct tsf_stream* stream) { TSFR(genOper) TSFR(genAmount) return 1; }
static int tsf_hydra_read_shdr(struct tsf_hydra_shdr* i, struct tsf_stream* stream) { TSFR(sampleName) TSFR(start) TSFR(end) TSFR(startLoop) TSFR(endLoop) TSFR(sampleRate) TSFR(originalPitch) TSFR(pitchCorrection) TSFR(sampleLink) TSFR(sampleType) return 1; }
#undef TSFR

struct tsf_riffchunk { tsf_fourcc id; tsf_u32 size; };
struct tsf_envelope { float delay, attack, hold, decay, sustain, release, keynumToHold, keynumToDecay; };
struct tsf_voice_envelope { unsigned char segment, segmentIsExponential : 1, isAmpEnv : 1; short midiVelocity; float level, slope; int samplesUntilNextSegment; struct tsf_envelope parameters; };
struct tsf_voice_lowpass { double QInv, a0, a1, b1, b2, z1, z2; double ta0, ta1, tb1, tb2; double currentFc; int rampRemaining, rampTotal; TSF_BOOL active; };
struct tsf_voice_lfo { int samplesUntil; float level, delta; };

/* SF2.01 section 8.2 modulator record */
struct tsf_modulator
{
	tsf_u16 srcOper, destOper;
	tsf_s16 amount;
	tsf_u16 amtSrcOper, transOper;
};

struct tsf_region
{
	struct tsf_modulator modulators[TSF_MAX_MODULATORS];
	unsigned char modulatorNum;
	int loop_mode;
	unsigned int sample_rate;
	unsigned char lokey, hikey, lovel, hivel;
	unsigned int group, offset, end, loop_start, loop_end;
	int transpose, tune, pitch_keycenter, pitch_keytrack;
	float attenuation, pan;
	struct tsf_envelope ampenv, modenv;
	float chorusSend, reverbSend;
	int initialFilterQ, initialFilterFc;
	int modEnvToPitch, modEnvToFilterFc, modLfoToFilterFc, modLfoToVolume;
	float delayModLFO;
	int freqModLFO, modLfoToPitch;
	float delayVibLFO;
	int freqVibLFO, vibLfoToPitch;
};

struct tsf_preset
{
	tsf_char20 presetName;
	tsf_u16 preset, bank;
	struct tsf_region* regions;
	int regionNum;
};

struct tsf_voice
{
	int playingPreset, playingKey, playingChannel, heldSustain;
	struct tsf_region* region;
	double pitchInputTimecents, pitchOutputFactor;
	double sourceSamplePosition;
	float  noteGainDB, panFactorLeft, panFactorRight;
	unsigned int playIndex, loopStart, loopEnd;
	struct tsf_voice_envelope ampenv, modenv;
	struct tsf_voice_lowpass lowpass;
	struct tsf_voice_lfo modlfo, viblfo;
	// Modulator contributions. The *Dyn fields are refreshed per render block
	// from continuously changing sources (CC, channel pressure, pitch wheel).
	float modPitchCents;
	float modFilterFc, modFilterFcDyn;
	float modAttenuationDB, modAttenuationDBDyn;
	float modVibLfoToPitch, modVibLfoToPitchDyn;
	float modModLfoToPitch, modModLfoToPitchDyn;
	float modModLfoToFilterFc, modModLfoToFilterFcDyn;
	float modModLfoToVolume, modModLfoToVolumeDyn;
	float modChorusSend, modReverbSend;
	unsigned char modVelocity;
	/* Bit flags: 1 = dynamic filter modulation, 2 = dynamic pitch modulation,
	 * 4 = dynamic gain modulation, 8 = has any dynamic modulator at all. */
	unsigned char hasDynamicMod;
};

/* Optional post-processing stage: a stereo chorus and a small Schroeder style
 * reverb. All buffers are allocated by tsf_set_effects, never while rendering. */
#define TSF_FX_COMBS 4
#define TSF_FX_ALLPASS 2
struct tsf_effects
{
	float chorusLevel, reverbLevel;
	float* chorus[2];
	int chorusSize, chorusPos;
	float chorusPhase;
	float* reverb[2];
	int reverbSize;
	int combOffset[TSF_FX_COMBS], combLen[TSF_FX_COMBS], combPos[TSF_FX_COMBS];
	float combStore[2][TSF_FX_COMBS];
	int apOffset[TSF_FX_ALLPASS], apLen[TSF_FX_ALLPASS], apPos[TSF_FX_ALLPASS];
};
static void tsf_effects_free(struct tsf_effects* e);

struct tsf_channel
{
	unsigned short presetIndex, bank, pitchWheel, midiPan, midiVolume, midiExpression, midiRPN, midiData : 14, sustain : 1;
	float panOffset, gainDB, pitchRange, tuning, reverbSend, chorusSend;
	unsigned char cc[128];      /* last value of every MIDI continuous controller */
	unsigned char chanPressure; /* MIDI channel pressure (0..127) */
};

struct tsf_channels
{
	void (*setupVoice)(tsf* f, struct tsf_voice* voice);
	int channelNum, activeChannel;
	struct tsf_channel channels[1];
};

static double tsf_timecents2Secsd(double timecents) { return TSF_POW(2.0, timecents / 1200.0); }
static float tsf_timecents2Secsf(float timecents) { return TSF_POWF(2.0f, timecents / 1200.0f); }
static float tsf_cents2Hertz(float cents) { return 8.176f * TSF_POWF(2.0f, cents / 1200.0f); }
static float tsf_decibelsToGain(float db) { return (db > -100.f ? TSF_POWF(10.0f, db * 0.05f) : 0); }
static float tsf_gainToDecibels(float gain) { return (gain <= .00001f ? -100.f : (float)(20.0 * TSF_LOG10(gain))); }

static TSF_BOOL tsf_riffchunk_read(struct tsf_riffchunk* parent, struct tsf_riffchunk* chunk, struct tsf_stream* stream)
{
	TSF_BOOL IsRiff, IsList;
	if (parent && parent->size < sizeof(tsf_fourcc) + sizeof(tsf_u32)) return TSF_FALSE;
	if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) || *chunk->id <= ' ' || *chunk->id >= 'z') return TSF_FALSE;
	if (!stream->read(stream->data, &chunk->size, sizeof(tsf_u32))) return TSF_FALSE;
	if (parent && chunk->size > parent->size - (tsf_u32)(sizeof(tsf_fourcc) + sizeof(tsf_u32))) return TSF_FALSE;
	if (parent) parent->size -= (tsf_u32)(sizeof(tsf_fourcc) + sizeof(tsf_u32)) + chunk->size;
	IsRiff = TSF_FourCCEquals(chunk->id, "RIFF"), IsList = TSF_FourCCEquals(chunk->id, "LIST");
	if (IsRiff && parent) return TSF_FALSE; //not allowed
	if (!IsRiff && !IsList) return TSF_TRUE; //custom type without sub type
	if (chunk->size < sizeof(tsf_fourcc)) return TSF_FALSE; //a list needs room for its type id
	if (!stream->read(stream->data, &chunk->id, sizeof(tsf_fourcc)) || *chunk->id <= ' ' || *chunk->id >= 'z') return TSF_FALSE;
	chunk->size -= (tsf_u32)sizeof(tsf_fourcc);
	return TSF_TRUE;
}

static void tsf_region_add_mod(struct tsf_region* region, tsf_u16 src, tsf_u16 dest, tsf_s16 amount, tsf_u16 amtSrc, tsf_u16 trans, int override);
static void tsf_region_add_default_mods(struct tsf_region* region);

static void tsf_region_clear(struct tsf_region* i, TSF_BOOL for_relative)
{
	TSF_MEMSET(i, 0, sizeof(struct tsf_region));
	i->hikey = i->hivel = 127;
	i->pitch_keycenter = 60; // C4
	if (for_relative) return;

	i->pitch_keytrack = 100;

	i->pitch_keycenter = -1;

	// SF2 defaults in timecents.
	i->ampenv.delay = i->ampenv.attack = i->ampenv.hold = i->ampenv.decay = i->ampenv.release = -12000.0f;
	i->modenv.delay = i->modenv.attack = i->modenv.hold = i->modenv.decay = i->modenv.release = -12000.0f;

	i->initialFilterFc = 13500;

	i->delayModLFO = -12000.0f;
	i->delayVibLFO = -12000.0f;

	tsf_region_add_default_mods(i);
}

static void tsf_region_operator(struct tsf_region* region, tsf_u16 genOper, union tsf_hydra_genamount* amount, struct tsf_region* merge_region)
{
	enum
	{
		_GEN_TYPE_MASK       = 0x0F,
		GEN_FLOAT            = 0x01,
		GEN_INT              = 0x02,
		GEN_UINT_ADD         = 0x03,
		GEN_UINT_ADD15       = 0x04,
		GEN_KEYRANGE         = 0x05,
		GEN_VELRANGE         = 0x06,
		GEN_LOOPMODE         = 0x07,
		GEN_GROUP            = 0x08,
		GEN_KEYCENTER        = 0x09,

		_GEN_LIMIT_MASK      = 0xF0,
		GEN_INT_LIMIT12K     = 0x10, //min -12000, max 12000
		GEN_INT_LIMITFC      = 0x20, //min 1500, max 13500
		GEN_INT_LIMITQ       = 0x30, //min 0, max 960
		GEN_INT_LIMIT960     = 0x40, //min -960, max 960
		GEN_INT_LIMIT16K4500 = 0x50, //min -16000, max 4500
		GEN_FLOAT_LIMIT12K5K = 0x60, //min -12000, max 5000
		GEN_FLOAT_LIMIT12K8K = 0x70, //min -12000, max 8000
		GEN_FLOAT_LIMIT1200  = 0x80, //min -1200, max 1200
		GEN_FLOAT_LIMITPAN   = 0x90, //* .001f, min -.5f, max .5f,
		GEN_FLOAT_LIMITATTN  = 0xA0, //* .1f, min 0, max 144.0
		GEN_FLOAT_MAX1000    = 0xB0, //min 0, max 1000
		GEN_FLOAT_MAX1440    = 0xC0, //min 0, max 1440

		_GEN_MAX = 59
	};
	#define _TSFREGIONOFFSET(TYPE, FIELD) (unsigned char)(((TYPE*)&((struct tsf_region*)0)->FIELD) - (TYPE*)0)
	#define _TSFREGIONENVOFFSET(TYPE, ENV, FIELD) (unsigned char)(((TYPE*)&((&(((struct tsf_region*)0)->ENV))->FIELD)) - (TYPE*)0)
	static const struct { unsigned char mode, offset; } genMetas[_GEN_MAX] =
	{
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, offset               ) }, // 0 StartAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, end                  ) }, // 1 EndAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, loop_start           ) }, // 2 StartloopAddrsOffset
		{ GEN_UINT_ADD                     , _TSFREGIONOFFSET(unsigned int, loop_end             ) }, // 3 EndloopAddrsOffset
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, offset               ) }, // 4 StartAddrsCoarseOffset
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modLfoToPitch        ) }, // 5 ModLfoToPitch
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, vibLfoToPitch        ) }, // 6 VibLfoToPitch
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modEnvToPitch        ) }, // 7 ModEnvToPitch
		{ GEN_INT   | GEN_INT_LIMITFC      , _TSFREGIONOFFSET(         int, initialFilterFc      ) }, // 8 InitialFilterFc
		{ GEN_INT   | GEN_INT_LIMITQ       , _TSFREGIONOFFSET(         int, initialFilterQ       ) }, // 9 InitialFilterQ
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modLfoToFilterFc     ) }, //10 ModLfoToFilterFc
		{ GEN_INT   | GEN_INT_LIMIT12K     , _TSFREGIONOFFSET(         int, modEnvToFilterFc     ) }, //11 ModEnvToFilterFc
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, end                  ) }, //12 EndAddrsCoarseOffset
		{ GEN_INT   | GEN_INT_LIMIT960     , _TSFREGIONOFFSET(         int, modLfoToVolume       ) }, //13 ModLfoToVolume
		{ 0                                , (0                                                  ) }, //   Unused
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONOFFSET(       float, chorusSend           ) }, //15 ChorusEffectsSend
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONOFFSET(       float, reverbSend           ) }, //16 ReverbEffectsSend
		{ GEN_FLOAT | GEN_FLOAT_LIMITPAN   , _TSFREGIONOFFSET(       float, pan                  ) }, //17 Pan
		{ 0                                , (0                                                  ) }, //   Unused
		{ 0                                , (0                                                  ) }, //   Unused
		{ 0                                , (0                                                  ) }, //   Unused
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONOFFSET(       float, delayModLFO          ) }, //21 DelayModLFO
		{ GEN_INT   | GEN_INT_LIMIT16K4500 , _TSFREGIONOFFSET(         int, freqModLFO           ) }, //22 FreqModLFO
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONOFFSET(       float, delayVibLFO          ) }, //23 DelayVibLFO
		{ GEN_INT   | GEN_INT_LIMIT16K4500 , _TSFREGIONOFFSET(         int, freqVibLFO           ) }, //24 FreqVibLFO
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, modenv, delay        ) }, //25 DelayModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, attack       ) }, //26 AttackModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, modenv, hold         ) }, //27 HoldModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, decay        ) }, //28 DecayModEnv
		{ GEN_FLOAT | GEN_FLOAT_MAX1000    , _TSFREGIONENVOFFSET(    float, modenv, sustain      ) }, //29 SustainModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, modenv, release      ) }, //30 ReleaseModEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, modenv, keynumToHold ) }, //31 KeynumToModEnvHold
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, modenv, keynumToDecay) }, //32 KeynumToModEnvDecay
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, ampenv, delay        ) }, //33 DelayVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, attack       ) }, //34 AttackVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K5K , _TSFREGIONENVOFFSET(    float, ampenv, hold         ) }, //35 HoldVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, decay        ) }, //36 DecayVolEnv
		{ GEN_FLOAT | GEN_FLOAT_MAX1440    , _TSFREGIONENVOFFSET(    float, ampenv, sustain      ) }, //37 SustainVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT12K8K , _TSFREGIONENVOFFSET(    float, ampenv, release      ) }, //38 ReleaseVolEnv
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, ampenv, keynumToHold ) }, //39 KeynumToVolEnvHold
		{ GEN_FLOAT | GEN_FLOAT_LIMIT1200  , _TSFREGIONENVOFFSET(    float, ampenv, keynumToDecay) }, //40 KeynumToVolEnvDecay
		{ 0                                , (0                                                  ) }, //   Instrument (special)
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_KEYRANGE                     , (0                                                  ) }, //43 KeyRange
		{ GEN_VELRANGE                     , (0                                                  ) }, //44 VelRange
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, loop_start           ) }, //45 StartloopAddrsCoarseOffset
		{ 0                                , (0                                                  ) }, //46 Keynum (special)
		{ 0                                , (0                                                  ) }, //47 Velocity (special)
		{ GEN_FLOAT | GEN_FLOAT_LIMITATTN  , _TSFREGIONOFFSET(       float, attenuation          ) }, //48 InitialAttenuation
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_UINT_ADD15                   , _TSFREGIONOFFSET(unsigned int, loop_end             ) }, //50 EndloopAddrsCoarseOffset
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, transpose            ) }, //51 CoarseTune
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, tune                 ) }, //52 FineTune
		{ 0                                , (0                                                  ) }, //   SampleID (special)
		{ GEN_LOOPMODE                     , _TSFREGIONOFFSET(         int, loop_mode            ) }, //54 SampleModes
		{ 0                                , (0                                                  ) }, //   Reserved
		{ GEN_INT                          , _TSFREGIONOFFSET(         int, pitch_keytrack       ) }, //56 ScaleTuning
		{ GEN_GROUP                        , _TSFREGIONOFFSET(unsigned int, group                ) }, //57 ExclusiveClass
		{ GEN_KEYCENTER                    , _TSFREGIONOFFSET(         int, pitch_keycenter      ) }, //58 OverridingRootKey
	};
	#undef _TSFREGIONOFFSET
	#undef _TSFREGIONENVOFFSET
	if (amount)
	{
		int offset;
		if (genOper >= _GEN_MAX) return;
		offset = genMetas[genOper].offset;
		switch (genMetas[genOper].mode & _GEN_TYPE_MASK)
		{
			case GEN_FLOAT:      ((       float*)region)[offset]  = amount->shortAmount;     return;
			case GEN_INT:        ((         int*)region)[offset]  = amount->shortAmount;     return;
			case GEN_UINT_ADD:   ((unsigned int*)region)[offset] += amount->shortAmount;     return;
			case GEN_UINT_ADD15: ((unsigned int*)region)[offset] += (unsigned int)((int)amount->shortAmount * 32768); return; //(multiply instead of shifting a possibly negative value)
			case GEN_KEYRANGE:   region->lokey = amount->range.lo; region->hikey = amount->range.hi; return;
			case GEN_VELRANGE:   region->lovel = amount->range.lo; region->hivel = amount->range.hi; return;
			case GEN_LOOPMODE:   region->loop_mode       = ((amount->wordAmount&3) == 3 ? TSF_LOOPMODE_SUSTAIN : ((amount->wordAmount&3) == 1 ? TSF_LOOPMODE_CONTINUOUS : TSF_LOOPMODE_NONE)); return;
			case GEN_GROUP:      region->group           = amount->wordAmount;  return;
			case GEN_KEYCENTER:  region->pitch_keycenter = amount->shortAmount; return;
		}
	}
	else //merge regions and clamp values
	{
		for (genOper = 0; genOper != _GEN_MAX; genOper++)
		{
			int offset = genMetas[genOper].offset;
			switch (genMetas[genOper].mode & _GEN_TYPE_MASK)
			{
				case GEN_FLOAT:
				{
					float *val = &((float*)region)[offset], vfactor, vmin, vmax;
					*val += ((float*)merge_region)[offset];
					switch (genMetas[genOper].mode & _GEN_LIMIT_MASK)
					{
						case GEN_FLOAT_LIMIT12K5K: vfactor =   1.0f; vmin = -12000.0f; vmax = 5000.0f; break;
						case GEN_FLOAT_LIMIT12K8K: vfactor =   1.0f; vmin = -12000.0f; vmax = 8000.0f; break;
						case GEN_FLOAT_LIMIT1200:  vfactor =   1.0f; vmin =  -1200.0f; vmax = 1200.0f; break;
						case GEN_FLOAT_LIMITPAN:   vfactor = 0.001f; vmin =     -0.5f; vmax =    0.5f; break;
						case GEN_FLOAT_LIMITATTN:  vfactor =  0.01f; vmin =      0.0f; vmax =   14.4f; break;
						case GEN_FLOAT_MAX1000:    vfactor =   1.0f; vmin =      0.0f; vmax = 1000.0f; break;
						case GEN_FLOAT_MAX1440:    vfactor =   1.0f; vmin =      0.0f; vmax = 1440.0f; break;
						default: continue;
					}
					*val *= vfactor;
					if      (*val < vmin) *val = vmin;
					else if (*val > vmax) *val = vmax;
					continue;
				}
				case GEN_INT:
				{
					int *val = &((int*)region)[offset], vmin, vmax;
					*val += ((int*)merge_region)[offset];
					switch (genMetas[genOper].mode & _GEN_LIMIT_MASK)
					{
						case GEN_INT_LIMIT12K:     vmin = -12000; vmax = 12000; break;
						case GEN_INT_LIMITFC:      vmin =   1500; vmax = 13500; break;
						case GEN_INT_LIMITQ:       vmin =      0; vmax =   960; break;
						case GEN_INT_LIMIT960:     vmin =   -960; vmax =   960; break;
						case GEN_INT_LIMIT16K4500: vmin = -16000; vmax =  4500; break;
						default: continue;
					}
					if      (*val < vmin) *val = vmin;
					else if (*val > vmax) *val = vmax;
					continue;
				}
				case GEN_UINT_ADD:
				{
					((unsigned int*)region)[offset] += ((unsigned int*)merge_region)[offset];
					continue;
				}
			}
		}
	}
}

static void tsf_region_envtosecs(struct tsf_envelope* p, TSF_BOOL sustainIsGain)
{
	// EG times need to be converted from timecents to seconds.
	// Pin very short EG segments.  Timecents don't get to zero, and our EG is
	// happier with zero values.
	p->delay   = (p->delay   < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->delay));
	p->attack  = (p->attack  < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->attack));
	p->release = (p->release < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->release));

	// If we have dynamic hold or decay times depending on key number we need
	// to keep the values in timecents so we can calculate it during startNote
	if (!p->keynumToHold)  p->hold  = (p->hold  < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->hold));
	if (!p->keynumToDecay) p->decay = (p->decay < -11950.0f ? 0.0f : tsf_timecents2Secsf(p->decay));

	if (p->sustain < 0.0f) p->sustain = 0.0f;
	else if (sustainIsGain) p->sustain = tsf_decibelsToGain(-p->sustain / 10.0f);
	else p->sustain = 1.0f - (p->sustain / 1000.0f);
}

// Make the index chains of the hydra tables safe to walk: every index is clamped to the terminal record
// of the table it points into and must never decrease. Valid SoundFonts are not changed by this.
static int tsf_hydra_sanitize(struct tsf_hydra *hydra)
{
	int i, prev;
	if (hydra->phdrNum < 2 || hydra->pbagNum < 1 || hydra->pgenNum < 1 || hydra->instNum < 1 || hydra->ibagNum < 1 || hydra->igenNum < 1 || hydra->shdrNum < 1) return 0;
	for (i = 0, prev = 0; i < hydra->phdrNum; i++) { int v = hydra->phdrs[i].presetBagNdx; if (v > hydra->pbagNum - 1) v = hydra->pbagNum - 1; if (v < prev) v = prev; hydra->phdrs[i].presetBagNdx = (tsf_u16)v; prev = v; }
	for (i = 0, prev = 0; i < hydra->pbagNum; i++) { int v = hydra->pbags[i].genNdx;       if (v > hydra->pgenNum - 1) v = hydra->pgenNum - 1; if (v < prev) v = prev; hydra->pbags[i].genNdx = (tsf_u16)v;       prev = v; }
	for (i = 0, prev = 0; i < hydra->instNum; i++) { int v = hydra->insts[i].instBagNdx;   if (v > hydra->ibagNum - 1) v = hydra->ibagNum - 1; if (v < prev) v = prev; hydra->insts[i].instBagNdx = (tsf_u16)v;   prev = v; }
	for (i = 0, prev = 0; i < hydra->ibagNum; i++) { int v = hydra->ibags[i].instGenNdx;   if (v > hydra->igenNum - 1) v = hydra->igenNum - 1; if (v < prev) v = prev; hydra->ibags[i].instGenNdx = (tsf_u16)v;   prev = v; }
	if (hydra->pmodNum > 0) for (i = 0, prev = 0; i < hydra->pbagNum; i++) { int v = hydra->pbags[i].modNdx; if (v > hydra->pmodNum) v = hydra->pmodNum; if (v < prev) v = prev; hydra->pbags[i].modNdx = (tsf_u16)v; prev = v; }
	else for (i = 0; i < hydra->pbagNum; i++) hydra->pbags[i].modNdx = 0;
	if (hydra->imodNum > 0) for (i = 0, prev = 0; i < hydra->ibagNum; i++) { int v = hydra->ibags[i].instModNdx; if (v > hydra->imodNum) v = hydra->imodNum; if (v < prev) v = prev; hydra->ibags[i].instModNdx = (tsf_u16)v; prev = v; }
	else for (i = 0; i < hydra->ibagNum; i++) hydra->ibags[i].instModNdx = 0;
	return 1;
}

/* SF2.01 section 9.5.3 curve tables, expressed as closed form math.
 * concave maps 0..127 to 1..0 and convex is its mirror image. */
static float tsf_mod_concave(float x)
{
	if (!(x > 0.0f)) return 1.0f;
	if (x >= 127.0f) return 0.0f;
	return (float)(TSF_LOG10(127.0*127.0 / ((127.0 - x) * (127.0 - x) + 127.0 * 127.0)) / TSF_LOG10(0.5));
}
static float tsf_mod_convex(float x) { return tsf_mod_concave(127.0f - x); }

/* Returns non-zero when the modulator has a source that can change while a
 * note is playing, which means it has to be re-evaluated every render block. */
static int tsf_mod_is_dynamic(const struct tsf_modulator* m)
{
	unsigned int ops[2]; int k;
	ops[0] = m->srcOper; ops[1] = m->amtSrcOper;
	for (k = 0; k < 2; k++)
	{
		unsigned int s = ops[k];
		/* index 0 without the CC flag means "no source at all" */
		if (!(s & 0x0080) && (s & 0x7F) == 0) continue;
		if (s & 0x0080) return 1; /* any MIDI continuous controller */
		switch (s & 0x7F)
		{
			case 10: /* poly (key) pressure */
			case 13: /* channel pressure */
			case 14: /* pitch wheel */
			case 16: /* pitch wheel sensitivity */
				return 1;
		}
	}
	return 0;
}

/* Add a modulator to a region. When override is set an existing modulator with
 * the same identity (sources, destination and transform) gets its amount
 * replaced instead of adding a duplicate, which matches how preset level
 * modulators override instrument/default ones in SF2. */
static void tsf_region_add_mod(struct tsf_region* region, tsf_u16 src, tsf_u16 dest, tsf_s16 amount, tsf_u16 amtSrc, tsf_u16 trans, int override)
{
	int i;
	for (i = 0; i < region->modulatorNum; i++)
	{
		struct tsf_modulator* e = &region->modulators[i];
		if (e->srcOper == src && e->destOper == dest && e->amtSrcOper == amtSrc && e->transOper == trans)
		{
			if (override) e->amount = amount;
			return;
		}
	}
	if (region->modulatorNum >= TSF_MAX_MODULATORS) return; /* keep the first ones, drop the rest */
	region->modulators[region->modulatorNum].srcOper = src;
	region->modulators[region->modulatorNum].destOper = dest;
	region->modulators[region->modulatorNum].amount = amount;
	region->modulators[region->modulatorNum].amtSrcOper = amtSrc;
	region->modulators[region->modulatorNum].transOper = trans;
	region->modulatorNum++;
}

/* The SF2.01 default modulators that this engine does not already implement
 * elsewhere (velocity/volume/expression/pan/pitch wheel are handled in the
 * note-on and channel code, adding them again here would apply them twice).
 * See SF2.01 section 8.4. */
#define TSF_MOD_GC_POS_UNI 0x0000u /* general controller, positive, unipolar */
#define TSF_MOD_LINEAR     0x0000u
#define TSF_MOD_DEST_VIBLFOTOPITCH 6
#define TSF_MOD_DEST_FILTERFC      8
static void tsf_region_add_default_mods(struct tsf_region* region)
{
	/* 8.4.3: channel pressure to vibrato LFO pitch depth, amount 50 cents */
	tsf_region_add_mod(region, (tsf_u16)(TSF_MOD_GC_POS_UNI | 13), TSF_MOD_DEST_VIBLFOTOPITCH, 50, 0, TSF_MOD_LINEAR, 0);
	/* 8.4.4: mod wheel (CC1) to vibrato LFO pitch depth, amount 50 cents */
	tsf_region_add_mod(region, (tsf_u16)(0x0080 | 1), TSF_MOD_DEST_VIBLFOTOPITCH, 50, 0, TSF_MOD_LINEAR, 0);
#if TSF_DEFAULT_VELOCITY_TO_FILTER
	/* 8.4.2: velocity to initial filter cutoff. Disabled by default because the
	 * specification's version is inconsistent and most synths (including
	 * FluidSynth) neutralize it. */
	tsf_region_add_mod(region, (tsf_u16)(TSF_MOD_GC_POS_UNI | 2), TSF_MOD_DEST_FILTERFC, -2400, 0, TSF_MOD_LINEAR, 0);
#endif
}

static int tsf_load_presets(tsf* res, struct tsf_hydra *hydra, unsigned int fontSampleCount)
{
	enum { GenInstrument = 41, GenKeyRange = 43, GenVelRange = 44, GenSampleID = 53 };
	// Read each preset.
	struct tsf_hydra_phdr *pphdr, *pphdrMax;
	res->presetNum = hydra->phdrNum - 1;
	res->presetNum = TSF_ALLOC_GUARD(res->presetNum, sizeof(struct tsf_preset));
	if (!res->presetNum) return 0;
	res->presets = (struct tsf_preset*)TSF_MALLOC(res->presetNum * sizeof(struct tsf_preset));
	if (!res->presets) return 0;
	else { int i; for (i = 0; i != res->presetNum; i++) res->presets[i].regions = TSF_NULL; }
	for (pphdr = hydra->phdrs, pphdrMax = pphdr + hydra->phdrNum - 1; pphdr != pphdrMax; pphdr++)
	{
		int sortedIndex = 0, region_index = 0;
		struct tsf_hydra_phdr *otherphdr;
		struct tsf_preset* preset;
		struct tsf_hydra_pbag *ppbag, *ppbagEnd;
		struct tsf_region globalRegion;
		for (otherphdr = hydra->phdrs; otherphdr != pphdrMax; otherphdr++)
		{
			if (otherphdr == pphdr || otherphdr->bank > pphdr->bank) continue;
			else if (otherphdr->bank < pphdr->bank) sortedIndex++;
			else if (otherphdr->preset > pphdr->preset) continue;
			else if (otherphdr->preset < pphdr->preset) sortedIndex++;
			else if (otherphdr < pphdr) sortedIndex++;
		}

		preset = &res->presets[sortedIndex];
		TSF_MEMCPY(preset->presetName, pphdr->presetName, sizeof(preset->presetName));
		preset->presetName[sizeof(preset->presetName)-1] = '\0'; //should be zero terminated in source file but make sure
		preset->bank = pphdr->bank;
		preset->preset = pphdr->preset;
		preset->regionNum = 0;

		//count regions covered by this preset
		for (ppbag = hydra->pbags + pphdr->presetBagNdx, ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx; ppbag != ppbagEnd; ppbag++)
		{
			unsigned char plokey = 0, phikey = 127, plovel = 0, phivel = 127;
			struct tsf_hydra_pgen *ppgen, *ppgenEnd; struct tsf_hydra_inst *pinst; struct tsf_hydra_ibag *pibag, *pibagEnd; struct tsf_hydra_igen *pigen, *pigenEnd;
			for (ppgen = hydra->pgens + ppbag->genNdx, ppgenEnd = hydra->pgens + ppbag[1].genNdx; ppgen != ppgenEnd; ppgen++)
			{
				if (ppgen->genOper == GenKeyRange) { plokey = ppgen->genAmount.range.lo; phikey = ppgen->genAmount.range.hi; continue; }
				if (ppgen->genOper == GenVelRange) { plovel = ppgen->genAmount.range.lo; phivel = ppgen->genAmount.range.hi; continue; }
				if (ppgen->genOper != GenInstrument) continue;
				if (ppgen->genAmount.wordAmount >= hydra->instNum - 1) continue;
				pinst = hydra->insts + ppgen->genAmount.wordAmount;
				for (pibag = hydra->ibags + pinst->instBagNdx, pibagEnd = hydra->ibags + pinst[1].instBagNdx; pibag != pibagEnd; pibag++)
				{
					unsigned char ilokey = 0, ihikey = 127, ilovel = 0, ihivel = 127;
					for (pigen = hydra->igens + pibag->instGenNdx, pigenEnd = hydra->igens + pibag[1].instGenNdx; pigen != pigenEnd; pigen++)
					{
						if (pigen->genOper == GenKeyRange) { ilokey = pigen->genAmount.range.lo; ihikey = pigen->genAmount.range.hi; continue; }
						if (pigen->genOper == GenVelRange) { ilovel = pigen->genAmount.range.lo; ihivel = pigen->genAmount.range.hi; continue; }
						if (pigen->genOper == GenSampleID && ihikey >= plokey && ilokey <= phikey && ihivel >= plovel && ilovel <= phivel) preset->regionNum++;
					}
				}
			}
		}

		preset->regionNum = TSF_ALLOC_GUARD(preset->regionNum ? preset->regionNum : 1, sizeof(struct tsf_region));
		if (!preset->regionNum) return 0;
		preset->regions = (struct tsf_region*)TSF_MALLOC(preset->regionNum * sizeof(struct tsf_region));
		if (!preset->regions)
		{
			int i; for (i = 0; i != res->presetNum; i++) TSF_FREE(res->presets[i].regions);
			TSF_FREE(res->presets);
			return 0;
		}
		tsf_region_clear(&globalRegion, TSF_TRUE);

		// Zones.
		for (ppbag = hydra->pbags + pphdr->presetBagNdx, ppbagEnd = hydra->pbags + pphdr[1].presetBagNdx; ppbag != ppbagEnd; ppbag++)
		{
			struct tsf_hydra_pgen *ppgen, *ppgenEnd; struct tsf_hydra_inst *pinst; struct tsf_hydra_ibag *pibag, *pibagEnd; struct tsf_hydra_igen *pigen, *pigenEnd;
			struct tsf_region presetRegion = globalRegion;
			int hadGenInstrument = 0;

			// Generators.
			for (ppgen = hydra->pgens + ppbag->genNdx, ppgenEnd = hydra->pgens + ppbag[1].genNdx; ppgen != ppgenEnd; ppgen++)
			{
				// Instrument.
				if (ppgen->genOper == GenInstrument)
				{
					struct tsf_region instRegion;
					tsf_u16 whichInst = ppgen->genAmount.wordAmount;
					if (whichInst >= hydra->instNum - 1) continue;

					tsf_region_clear(&instRegion, TSF_FALSE);
					pinst = &hydra->insts[whichInst];
					for (pibag = hydra->ibags + pinst->instBagNdx, pibagEnd = hydra->ibags + pinst[1].instBagNdx; pibag != pibagEnd; pibag++)
					{
						// Generators.
						struct tsf_region zoneRegion = instRegion;
						int hadSampleID = 0;
						for (pigen = hydra->igens + pibag->instGenNdx, pigenEnd = hydra->igens + pibag[1].instGenNdx; pigen != pigenEnd; pigen++)
						{
							if (pigen->genOper == GenSampleID)
							{
								struct tsf_hydra_shdr* pshdr;
								if (pigen->genAmount.wordAmount >= hydra->shdrNum || region_index >= preset->regionNum) { hadSampleID = 1; continue; } //unusable zone, but still not a global zone

								//preset region key and vel ranges are a filter for the zone regions
								if (zoneRegion.hikey < presetRegion.lokey || zoneRegion.lokey > presetRegion.hikey) continue;
								if (zoneRegion.hivel < presetRegion.lovel || zoneRegion.lovel > presetRegion.hivel) continue;
								if (presetRegion.lokey > zoneRegion.lokey) zoneRegion.lokey = presetRegion.lokey;
								if (presetRegion.hikey < zoneRegion.hikey) zoneRegion.hikey = presetRegion.hikey;
								if (presetRegion.lovel > zoneRegion.lovel) zoneRegion.lovel = presetRegion.lovel;
								if (presetRegion.hivel < zoneRegion.hivel) zoneRegion.hivel = presetRegion.hivel;

								//sum regions
								tsf_region_operator(&zoneRegion, 0, TSF_NULL, &presetRegion);

								// EG times need to be converted from timecents to seconds.
								tsf_region_envtosecs(&zoneRegion.ampenv, TSF_TRUE);
								tsf_region_envtosecs(&zoneRegion.modenv, TSF_FALSE);

								// LFO times need to be converted from timecents to seconds.
								zoneRegion.delayModLFO = (zoneRegion.delayModLFO < -11950.0f ? 0.0f : tsf_timecents2Secsf(zoneRegion.delayModLFO));
								zoneRegion.delayVibLFO = (zoneRegion.delayVibLFO < -11950.0f ? 0.0f : tsf_timecents2Secsf(zoneRegion.delayVibLFO));

								// Fixup sample positions
								pshdr = &hydra->shdrs[pigen->genAmount.wordAmount];
								zoneRegion.offset += pshdr->start;
								zoneRegion.end += pshdr->end;
								zoneRegion.loop_start += pshdr->startLoop;
								zoneRegion.loop_end += pshdr->endLoop;
								if (pshdr->endLoop > 0) zoneRegion.loop_end -= 1;
								if (zoneRegion.loop_end > fontSampleCount) zoneRegion.loop_end = fontSampleCount;
								if (zoneRegion.pitch_keycenter == -1) zoneRegion.pitch_keycenter = pshdr->originalPitch;
								zoneRegion.tune += pshdr->pitchCorrection;
								zoneRegion.sample_rate = pshdr->sampleRate;
								if (zoneRegion.end && zoneRegion.end < fontSampleCount) zoneRegion.end++;
								else zoneRegion.end = fontSampleCount;
								if (zoneRegion.loop_start > zoneRegion.end) zoneRegion.loop_start = zoneRegion.end;
								if (zoneRegion.loop_end   > zoneRegion.end) zoneRegion.loop_end   = zoneRegion.end;
								if (zoneRegion.offset >= zoneRegion.end) { hadSampleID = 1; continue; } //no (valid) sample data, skip this region (but it is not a global zone)

								// Modulators: instrument zone first, then the preset
								// zone which may override an identical modulator.
								{
									int mi;
									for (mi = pibag->instModNdx; mi < pibag[1].instModNdx && mi < hydra->imodNum; mi++)
										tsf_region_add_mod(&zoneRegion, hydra->imods[mi].modSrcOper, hydra->imods[mi].modDestOper, hydra->imods[mi].modAmount, hydra->imods[mi].modAmtSrcOper, hydra->imods[mi].modTransOper, 0);
									for (mi = ppbag->modNdx; mi < ppbag[1].modNdx && mi < hydra->pmodNum; mi++)
										tsf_region_add_mod(&zoneRegion, hydra->pmods[mi].modSrcOper, hydra->pmods[mi].modDestOper, hydra->pmods[mi].modAmount, hydra->pmods[mi].modAmtSrcOper, hydra->pmods[mi].modTransOper, 1);
								}

								preset->regions[region_index] = zoneRegion;
								region_index++;
								hadSampleID = 1;
							}
							else tsf_region_operator(&zoneRegion, pigen->genOper, &pigen->genAmount, TSF_NULL);
						}

						// Handle instrument's global zone.
						if (pibag == hydra->ibags + pinst->instBagNdx && !hadSampleID)
							instRegion = zoneRegion;

						// Modulators (TODO)
						//if (ibag->instModNdx < ibag[1].instModNdx) addUnsupportedOpcode("any modulator");
					}
					hadGenInstrument = 1;
				}
				else tsf_region_operator(&presetRegion, ppgen->genOper, &ppgen->genAmount, TSF_NULL);
			}

			// Modulators (TODO)
			//if (pbag->modNdx < pbag[1].modNdx) addUnsupportedOpcode("any modulator");

			// Handle preset's global zone.
			if (ppbag == hydra->pbags + pphdr->presetBagNdx && !hadGenInstrument)
				globalRegion = presetRegion;
		}
		preset->regionNum = region_index;
	}
	return 1;
}

#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
static int tsf_decode_ogg(const tsf_u8 *pSmpl, const tsf_u8 *pSmplEnd, float** pRes, tsf_u32* pResNum, tsf_u32* pResMax, tsf_u32 resInitial)
{
	float *res = *pRes, *oldres; tsf_u32 resNum = *pResNum; tsf_u32 resMax = *pResMax; stb_vorbis *v;

	// Use whatever stb_vorbis API that is available (either pull or push)
	#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
	v = stb_vorbis_open_memory(pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, TSF_NULL);
	#else
	{ int use, err; v = stb_vorbis_open_pushdata(pSmpl, (int)(pSmplEnd - pSmpl), &use, &err, TSF_NULL); pSmpl += use; }
	#endif
	if (v == TSF_NULL) return 0;

	for (;;)
	{
		float** outputs; int n_samples;

		// Decode one frame of vorbis samples with whatever stb_vorbis API that is available
		#if !defined(STB_VORBIS_NO_PULLDATA_API) && !defined(STB_VORBIS_NO_FROMMEMORY)
		n_samples = stb_vorbis_get_frame_float(v, TSF_NULL, &outputs);
		if (!n_samples) break;
		#else
		if (pSmpl >= pSmplEnd) break;
		{ int use = stb_vorbis_decode_frame_pushdata(v, pSmpl, (int)(pSmplEnd - pSmpl), TSF_NULL, &outputs, &n_samples); pSmpl += use; }
		if (!n_samples) continue;
		#endif

		// Expand our output buffer if necessary then copy over the decoded frame samples
		resNum += n_samples;
		if (resNum > resMax)
		{
			do { resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial); } while (resNum > resMax);
			oldres = res;
			res = (float*)TSF_REALLOC(res, resMax * sizeof(float));
			if (!res) { TSF_FREE(oldres); stb_vorbis_close(v); return 0; }
		}
		TSF_MEMCPY(res + resNum - n_samples, outputs[0], n_samples * sizeof(float));
	}
	stb_vorbis_close(v);
	*pRes = res; *pResNum = resNum; *pResMax = resMax;
	return 1;
}

static int tsf_decode_sf3_samples(const void* rawBuffer, float** pFloatBuffer, unsigned int* pSmplCount, struct tsf_hydra *hydra)
{
	const tsf_u8* smplBuffer = (const tsf_u8*)rawBuffer;
	tsf_u32 smplLength = *pSmplCount, resNum = 0, resMax = 0, resInitial = (smplLength > 0x100000 ? (smplLength & ~0xFFFFF) : 65536);
	float *res = TSF_NULL, *oldres;
	int i, shdrLast = hydra->shdrNum - 1, is_sf3 = 0;
	for (i = 0; i <= shdrLast; i++)
	{
		struct tsf_hydra_shdr *shdr = &hydra->shdrs[i];
		if (shdr->sampleType & 0x30) // compression flags (sometimes Vorbis flag)
		{
			const tsf_u8 *pSmpl = smplBuffer + shdr->start, *pSmplEnd = smplBuffer + shdr->end;
			if (pSmpl + 4 > pSmplEnd || !TSF_FourCCEquals(pSmpl, "OggS"))
			{
				shdr->start = shdr->end = shdr->startLoop = shdr->endLoop = 0;
				continue;
			}

			// Fix up sample indices in shdr (end index is set after decoding)
			shdr->start = resNum;
			shdr->startLoop += resNum;
			shdr->endLoop += resNum;
			if (!tsf_decode_ogg(pSmpl, pSmplEnd, &res, &resNum, &resMax, resInitial)) { TSF_FREE(res); return 0; }
			shdr->end = resNum;
			is_sf3 = 1;
		}
		else // raw PCM sample
		{
			float *out; short *in = (short*)smplBuffer + resNum, *inEnd; tsf_u32 oldResNum = resNum;
			if (is_sf3) // Fix up sample indices in shdr
			{
				tsf_u32 fix_offset = resNum - shdr->start;
				in -= fix_offset;
				shdr->start = resNum;
				shdr->end += fix_offset;
				shdr->startLoop += fix_offset;
				shdr->endLoop += fix_offset;
			}
			inEnd = in + ((shdr->end >= shdr->endLoop ? shdr->end : shdr->endLoop) - resNum);
			if (i == shdrLast || (tsf_u8*)inEnd > (smplBuffer + smplLength)) inEnd = (short*)(smplBuffer + smplLength);
			if (inEnd <= in) continue;

			// expand our output buffer if necessary then convert the PCM data from short to float
			resNum += (tsf_u32)(inEnd - in);
			if (resNum > resMax)
			{
				do { resMax += (resMax ? (resMax < 1048576 ? resMax : 1048576) : resInitial); } while (resNum > resMax);
				resMax = TSF_ALLOC_GUARD(resMax, sizeof(float));
	if (!resMax || resMax < resNum) { TSF_FREE(res); return 0; }
				oldres = res;
				res = (float*)TSF_REALLOC(res, (size_t)resMax * sizeof(float));
				if (!res) { TSF_FREE(oldres); return 0; }
			}

			// Convert the samples from short to float
			for (out = res + oldResNum; in < inEnd;)
				*(out++) = (float)(*(in++) / 32767.0);
		}
	}

	// Trim the sample buffer down then return success (unless out of memory)
	if (!res) { TSF_FREE(res); return 0; }
	resNum = TSF_ALLOC_GUARD(resNum, sizeof(float));
	if (!resNum) { TSF_FREE(res); return 0; }
	if (!(*pFloatBuffer = (float*)TSF_REALLOC(res, ((size_t)resNum + 1) * sizeof(float)))) { TSF_FREE(res); return 0; }
	(*pFloatBuffer)[resNum] = 0.0f; // padding sample for the interpolation
	*pSmplCount = resNum;
	return 1;
}
#endif

static int tsf_load_samples(void** pRawBuffer, float** pFloatBuffer, unsigned int* pSmplCount, struct tsf_riffchunk *chunkSmpl, struct tsf_stream* stream)
{
	#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
	// With OGG Vorbis support we cannot pre-allocate the memory for tsf_decode_sf3_samples
	tsf_u32 resNum, resMax; float* oldres;
	*pSmplCount = chunkSmpl->size;
	*pRawBuffer = (void*)TSF_MALLOC(*pSmplCount);
	if (!*pRawBuffer || (tsf_u32)stream->read(stream->data, *pRawBuffer, chunkSmpl->size) != chunkSmpl->size) return 0;
	if (chunkSmpl->size & 1) { if (!stream->skip(stream->data, 1)) return 0; } //odd sized chunks have a trailing byte we did not read
	if (chunkSmpl->id[3] != 'o') return 1;

	// Decode custom .sfo 'smpo' format where all samples are in a single ogg stream
	resNum = resMax = 0;
	if (!tsf_decode_ogg((tsf_u8*)*pRawBuffer, (tsf_u8*)*pRawBuffer + chunkSmpl->size, pFloatBuffer, &resNum, &resMax, 65536)) return 0;
	oldres = *pFloatBuffer;
	if (!oldres || (size_t)resNum > (size_t)-1 / sizeof(float) - 1) return 0; //size computation would overflow
	// one extra zero sample at the end so linear interpolation never reads past the buffer
	if (!(*pFloatBuffer = (float*)TSF_REALLOC(*pFloatBuffer, ((size_t)resNum + 1) * sizeof(float)))) { *pFloatBuffer = oldres; return 0; }
	(*pFloatBuffer)[resNum] = 0.0f;
	*pSmplCount = resNum;
	return 1;
	#else
	// Inline convert the samples from short to float
	float *res, *out; const short *in;
	(void)pRawBuffer;
	*pSmplCount = chunkSmpl->size / (unsigned int)sizeof(short);
	#if defined(__SIZEOF_SIZE_T__) && __SIZEOF_SIZE_T__ == 4
	if ((size_t)*pSmplCount > (size_t)-1 / sizeof(float) - 1) return 0; //size computation would overflow (32-bit hosts)
	#endif
	// one extra zero sample at the end so linear interpolation never reads past the buffer
	*pFloatBuffer = (float*)TSF_MALLOC(((size_t)*pSmplCount + 1) * sizeof(float));
	if (!*pFloatBuffer) return 0;
	// the raw 16 bit data is read into the front of the (larger) float buffer and widened in place, back to front
	if ((tsf_u32)stream->read(stream->data, *pFloatBuffer, *pSmplCount * (unsigned int)sizeof(short)) != *pSmplCount * (unsigned int)sizeof(short)) return 0;
	if (chunkSmpl->size & 1) { if (!stream->skip(stream->data, 1)) return 0; } //odd sized chunks have a trailing byte we did not read
	for (res = *pFloatBuffer, out = res + *pSmplCount, in = (short*)res + *pSmplCount; out != res;)
		*(--out) = (float)(*(--in) / 32767.0);
	(*pFloatBuffer)[*pSmplCount] = 0.0f;
	return 1;
	#endif
}

static int tsf_voice_envelope_release_samples(struct tsf_voice_envelope* e, float outSampleRate)
{
	return (int)((e->parameters.release <= 0 ? TSF_FASTRELEASETIME : e->parameters.release) * outSampleRate);
}

static void tsf_voice_envelope_nextsegment(struct tsf_voice_envelope* e, short active_segment, float outSampleRate)
{
	switch (active_segment)
	{
		case TSF_SEGMENT_NONE:
			e->samplesUntilNextSegment = (int)(e->parameters.delay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DELAY;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0;
				e->slope = 0.0;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DELAY:
			e->samplesUntilNextSegment = (int)(e->parameters.attack * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				if (!e->isAmpEnv)
				{
					//mod env attack duration scales with velocity (velocity of 1 is full duration, max velocity is 0.125 times duration)
					e->samplesUntilNextSegment = (int)(e->parameters.attack * ((145 - e->midiVelocity) / 144.0f) * outSampleRate);
				}
				e->segment = TSF_SEGMENT_ATTACK;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 0.0f;
				e->slope = 1.0f / e->samplesUntilNextSegment;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_ATTACK:
			e->samplesUntilNextSegment = (int)(e->parameters.hold * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_HOLD;
				e->segmentIsExponential = TSF_FALSE;
				e->level = 1.0f;
				e->slope = 0.0f;
				return;
			}
			/* fall through */
		case TSF_SEGMENT_HOLD:
			e->samplesUntilNextSegment = (int)(e->parameters.decay * outSampleRate);
			if (e->samplesUntilNextSegment > 0)
			{
				e->segment = TSF_SEGMENT_DECAY;
				e->level = 1.0f;
				if (e->isAmpEnv)
				{
					// I don't truly understand this; just following what LinuxSampler does.
					float mysterySlope = -9.226f / e->samplesUntilNextSegment;
					e->slope = TSF_EXPF(mysterySlope);
					e->segmentIsExponential = TSF_TRUE;
					if (e->parameters.sustain > 0.0f)
					{
						// Again, this is following LinuxSampler's example, which is similar to
						// SF2-style decay, where "decay" specifies the time it would take to
						// get to zero, not to the sustain level.  The SFZ spec is not that
						// specific about what "decay" means, so perhaps it's really supposed
						// to specify the time to reach the sustain level.
						e->samplesUntilNextSegment = (int)(TSF_LOG(e->parameters.sustain) / mysterySlope);
					}
				}
				else
				{
					e->slope = -1.0f / e->samplesUntilNextSegment;
					e->samplesUntilNextSegment = (int)(e->parameters.decay * (1.0f - e->parameters.sustain) * outSampleRate);
					e->segmentIsExponential = TSF_FALSE;
				}
				return;
			}
			/* fall through */
		case TSF_SEGMENT_DECAY:
			e->segment = TSF_SEGMENT_SUSTAIN;
			e->level = e->parameters.sustain;
			e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFFF;
			e->segmentIsExponential = TSF_FALSE;
			return;
		case TSF_SEGMENT_SUSTAIN:
			e->segment = TSF_SEGMENT_RELEASE;
			e->samplesUntilNextSegment = tsf_voice_envelope_release_samples(e, outSampleRate);
			if (e->isAmpEnv)
			{
				// I don't truly understand this; just following what LinuxSampler does.
				float mysterySlope = -9.226f / e->samplesUntilNextSegment;
				e->slope = TSF_EXPF(mysterySlope);
				e->segmentIsExponential = TSF_TRUE;
			}
			else
			{
				e->slope = -e->level / e->samplesUntilNextSegment;
				e->segmentIsExponential = TSF_FALSE;
			}
			return;
		case TSF_SEGMENT_RELEASE:
		default:
			e->segment = TSF_SEGMENT_DONE;
			e->segmentIsExponential = TSF_FALSE;
			e->level = e->slope = 0.0f;
			e->samplesUntilNextSegment = 0x7FFFFFF;
	}
}

static void tsf_voice_envelope_setup(struct tsf_voice_envelope* e, struct tsf_envelope* new_parameters, int midiNoteNumber, short midiVelocity, TSF_BOOL isAmpEnv, float outSampleRate)
{
	e->parameters = *new_parameters;
	if (e->parameters.keynumToHold)
	{
		e->parameters.hold += e->parameters.keynumToHold * (60.0f - midiNoteNumber);
		e->parameters.hold = (e->parameters.hold < -10000.0f ? 0.0f : tsf_timecents2Secsf(e->parameters.hold));
	}
	if (e->parameters.keynumToDecay)
	{
		e->parameters.decay += e->parameters.keynumToDecay * (60.0f - midiNoteNumber);
		e->parameters.decay = (e->parameters.decay < -10000.0f ? 0.0f : tsf_timecents2Secsf(e->parameters.decay));
	}
	e->midiVelocity = midiVelocity;
	e->isAmpEnv = isAmpEnv;
	tsf_voice_envelope_nextsegment(e, TSF_SEGMENT_NONE, outSampleRate);
}

static void tsf_voice_envelope_process(struct tsf_voice_envelope* e, int numSamples, float outSampleRate)
{
	if (e->slope)
	{
		if (e->segmentIsExponential) e->level *= TSF_POWF(e->slope, (float)numSamples);
		else e->level += (e->slope * numSamples);
	}
	if ((e->samplesUntilNextSegment -= numSamples) <= 0)
		tsf_voice_envelope_nextsegment(e, e->segment, outSampleRate);
}

static void tsf_voice_lowpass_setup(struct tsf_voice_lowpass* e, float Fc)
{
	// Improved lowpass filter: RBJ biquad topology with a pre-warped tangent
	// and explicit clamping, which keeps the coefficients finite and the
	// response stable for the full range of cutoff frequencies and Q values.
	// The previous implementation used an unguarded tan() that could produce
	// denormals/inf for cutoffs very close to Nyquist.
	if (!(Fc > 0.0f)) Fc = 0.0f;
	if (Fc > 0.49f) Fc = 0.49f; //stay below Nyquist with a safety margin
	double QInv = e->QInv;
	if (!(QInv > 0.0)) QInv = 1.0;
	if (QInv > 1.0) QInv = 1.0; //QInv is 1/Q; clamp to a stable range
	double K = TSF_TAN(TSF_PI * Fc), KK = K * K;
	double norm = 1 / (1 + K * QInv + KK);
	e->ta0 = KK * norm;
	e->ta1 = 2 * e->ta0;
	e->tb1 = 2 * (KK - 1) * norm;
	e->tb2 = (1 - K * QInv + KK) * norm;
	e->currentFc = (double)Fc;
	// When the filter is already running, ramp towards the new coefficients
	// instead of jumping, which removes the zipper noise on cutoff changes.
	if (e->active && e->rampRemaining <= 0)
	{
		e->rampTotal = TSF_RENDER_EFFECTSAMPLEBLOCK;
		e->rampRemaining = TSF_RENDER_EFFECTSAMPLEBLOCK;
	}
	else if (!e->active)
	{
		e->a0 = e->ta0; e->a1 = e->ta1; e->b1 = e->tb1; e->b2 = e->tb2;
		e->rampTotal = e->rampRemaining = 0;
	}
}

/* Apply the per-sample coefficient ramp. Called once for every rendered sample. */
static void tsf_voice_lowpass_ramp(struct tsf_voice_lowpass* e)
{
	if (e->rampRemaining <= 0) return;
	double step = 1.0 / (double)e->rampTotal;
	e->a0 += (e->ta0 - e->a0) * step;
	e->a1 += (e->ta1 - e->a1) * step;
	e->b1 += (e->tb1 - e->b1) * step;
	e->b2 += (e->tb2 - e->b2) * step;
	e->rampRemaining--;
	if (e->rampRemaining <= 0)
	{
		e->a0 = e->ta0; e->a1 = e->ta1; e->b1 = e->tb1; e->b2 = e->tb2;
	}
}

static float tsf_voice_lowpass_process(struct tsf_voice_lowpass* e, double In)
{
	double Out = In * e->a0 + e->z1; e->z1 = In * e->a1 + e->z2 - e->b1 * Out; e->z2 = In * e->a0 - e->b2 * Out; return (float)Out;
}



/* Catmull-Rom spline interpolation for high-quality pitch shifting.
 * The render loop keeps position bounded; we only need local boundary handling. */
static float tsf_voice_interpolate(const float* input, double position,
	unsigned int sampleStart, unsigned int sampleEnd,
	unsigned int loopStart, unsigned int loopEnd, TSF_BOOL isLooping)
{
	unsigned int loopLen = (loopEnd >= loopStart) ? (loopEnd - loopStart + 1) : 0;
	if (isLooping && loopLen > 0)
	{
		// Position is kept near [loopStart, loopEnd+1) by render loop.
		// Handle the case where position slightly exceeds loopEnd (by < pitchRatio).
		if (position >= (double)loopEnd + 1.0)
			position -= (double)loopLen;
		else if (position < (double)loopStart)
			position += (double)loopLen;
	}
	else
	{
		if (position < (double)sampleStart) position = (double)sampleStart;
		if (position > (double)sampleEnd) position = (double)sampleEnd;
	}
	unsigned int pos = (unsigned int)position;
	float x = (float)(position - pos);
	float a = input[(pos >= 1) ? pos - 1 : 0];
	float b = input[pos];
	float c = input[pos + 1];
	float d = input[pos + 2];
	return b + 0.5f * x * (c - a + x * (2.0f*a - 5.0f*b + 4.0f*c - d + x * (3.0f*(b-c) + d-a)));
}

static void tsf_voice_lfo_setup(struct tsf_voice_lfo* e, float delay, int freqCents, float outSampleRate)
{
	e->samplesUntil = (int)(delay * outSampleRate);
	e->delta = (4.0f * tsf_cents2Hertz((float)freqCents) / outSampleRate);
	e->level = 0;
}

static void tsf_voice_lfo_process(struct tsf_voice_lfo* e, int blockSamples)
{
	if (e->samplesUntil > blockSamples) { e->samplesUntil -= blockSamples; return; }
	e->level += e->delta * blockSamples;
	if      (e->level >  1.0f) { e->delta = -e->delta; e->level =  2.0f - e->level; }
	else if (e->level < -1.0f) { e->delta = -e->delta; e->level = -2.0f - e->level; }
}

static void tsf_voice_kill(struct tsf_voice* v)
{
	v->playingPreset = -1;
}

static void tsf_voice_end(tsf* f, struct tsf_voice* v)
{
	// if maxVoiceNum is set, assume that voice rendering and note queuing are on separate threads
	// so to minimize the chance that voice rendering would advance the segment at the same time
	// we just do it twice here and hope that it sticks
	int repeats = (f->maxVoiceNum ? 2 : 1);
	while (repeats--)
	{
		tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		if (v->region->loop_mode == TSF_LOOPMODE_SUSTAIN)
		{
			// Continue playing, but stop looping.
			v->loopEnd = v->loopStart;
		}
	}
}

static void tsf_voice_endquick(tsf* f, struct tsf_voice* v)
{
	// if maxVoiceNum is set, assume that voice rendering and note queuing are on separate threads
	// so to minimize the chance that voice rendering would advance the segment at the same time
	// we just do it twice here and hope that it sticks
	int repeats = (f->maxVoiceNum ? 2 : 1);
	while (repeats--)
	{
		v->ampenv.parameters.release = 0.0f; tsf_voice_envelope_nextsegment(&v->ampenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
		v->modenv.parameters.release = 0.0f; tsf_voice_envelope_nextsegment(&v->modenv, TSF_SEGMENT_SUSTAIN, f->outSampleRate);
	}
}

static void tsf_voice_calcpitchratio(struct tsf_voice* v, float pitchShift, float outSampleRate)
{
	double note = v->playingKey + v->region->transpose + v->region->tune / 100.0 + v->modPitchCents / 100.0;
	double adjustedPitch = v->region->pitch_keycenter + (note - v->region->pitch_keycenter) * (v->region->pitch_keytrack / 100.0);
	if (pitchShift) adjustedPitch += pitchShift;
	v->pitchInputTimecents = adjustedPitch * 100.0;
	v->pitchOutputFactor = v->region->sample_rate / (tsf_timecents2Secsd(v->region->pitch_keycenter * 100.0) * outSampleRate);
}

static struct tsf_channel* tsf_voice_mod_channel(tsf* f, struct tsf_voice* v)
{
	if (!f->channels || v->playingChannel < 0 || v->playingChannel >= f->channels->channelNum) return TSF_NULL;
	return &f->channels->channels[v->playingChannel];
}

/* Raw value of one modulator source operator (SF2.01 section 8.2.1). */
static float tsf_mod_source_raw(tsf* f, struct tsf_voice* v, struct tsf_channel* c, unsigned int src, float* range)
{
	(void)f;
	*range = 128.0f;
	if (src & 0x0080) /* a MIDI continuous controller */
	{
		unsigned int cc = src & 0x7F;
		return (c ? (float)c->cc[cc] : 0.0f);
	}
	switch (src & 0x7F)
	{
		case 0:  return *range;  /* "no controller" resolves to the full range */
		case 2:  return (float)v->modVelocity;
		case 3:  return (float)v->playingKey;
		case 10: return 0.0f;    /* poly/key pressure is not tracked */
		case 13: return (c ? (float)c->chanPressure : 0.0f);
		case 14: *range = 16384.0f; return (c ? (float)c->pitchWheel : 8192.0f);
		case 16: return (c ? c->pitchRange * 100.0f : 200.0f); /* pitch wheel sensitivity in cents */
	}
	return 0.0f;
}

/* Apply the curve/polarity/direction bits of a source operator to a value. */
static float tsf_mod_transform_value(float val, unsigned int flags, float range)
{
	float n = (range > 0.0f ? val / range : 0.0f);
	unsigned int type = (flags >> 10) & 0x3F;
	int negative = (flags & 0x0100) != 0;
	int bipolar = (flags & 0x0200) != 0;
	if (type == 3) /* switch */
	{
		if (bipolar) return (n >= 0.5f ? (negative ? -1.0f : 1.0f) : 0.0f);
		return (n >= 0.5f ? 1.0f : (negative ? 1.0f : 0.0f));
	}
	if (type == 1) /* concave */
	{
		if (bipolar) return (n > 0.5f ?  tsf_mod_concave(254.0f * (n - 0.5f)) : -tsf_mod_concave(254.0f * (0.5f - n)));
		return tsf_mod_concave(127.0f * (negative ? (1.0f - n) : n));
	}
	if (type == 2) /* convex */
	{
		if (bipolar) return (n > 0.5f ?  tsf_mod_convex(254.0f * (n - 0.5f)) : -tsf_mod_convex(254.0f * (0.5f - n)));
		return tsf_mod_convex(127.0f * (negative ? (1.0f - n) : n));
	}
	if (bipolar) return (negative ? (1.0f - 2.0f * n) : (-1.0f + 2.0f * n));
	return (negative ? (1.0f - n) : n);
}

/* Output of one modulator: amount * primary * secondary (SF2.01 section 9.5). */
static float tsf_mod_value(const struct tsf_modulator* m, tsf* f, struct tsf_voice* v, struct tsf_channel* c)
{
	float r1 = 128.0f, r2 = 128.0f, out;
	float v1 = tsf_mod_transform_value(tsf_mod_source_raw(f, v, c, m->srcOper, &r1), m->srcOper, r1);
	float v2 = tsf_mod_transform_value(tsf_mod_source_raw(f, v, c, m->amtSrcOper, &r2), m->amtSrcOper, r2);
	out = (float)m->amount * v1 * v2;
	if (m->transOper & 0x0001) out = (out < 0.0f ? -out : out); /* absolute value transform */
	return out;
}

/* Evaluate the region modulators of a voice. dynamicOnly == 0 collects the
 * note-on time contributions, dynamicOnly == 1 refreshes the ones driven by
 * continuously changing sources (CC, channel pressure, pitch wheel). */
static void tsf_voice_apply_modulators(tsf* f, struct tsf_voice* v, int dynamicOnly)
{
	struct tsf_region* region = v->region;
	struct tsf_channel* c = (dynamicOnly || 1) ? tsf_voice_mod_channel(f, v) : TSF_NULL;
	int i;
	if (dynamicOnly)
	{
		v->modFilterFcDyn = v->modAttenuationDBDyn = v->modVibLfoToPitchDyn = 0.0f;
		v->modModLfoToPitchDyn = v->modModLfoToFilterFcDyn = v->modModLfoToVolumeDyn = 0.0f;
	}
	else
	{
		v->modPitchCents = v->modFilterFc = v->modAttenuationDB = 0.0f;
		v->modVibLfoToPitch = v->modModLfoToPitch = v->modModLfoToFilterFc = v->modModLfoToVolume = 0.0f;
		v->modChorusSend = region->chorusSend * 0.001f;
		v->modReverbSend = region->reverbSend * 0.001f;
		v->hasDynamicMod = 0;
	}
	for (i = 0; i < region->modulatorNum; i++)
	{
		const struct tsf_modulator* m = &region->modulators[i];
		int dyn = tsf_mod_is_dynamic(m);
		float value;
		if (!dynamicOnly && dyn)
		{
			v->hasDynamicMod |= 8;
			switch (m->destOper)
			{
				case  5: case  6: case 51: case 52: v->hasDynamicMod |= 2; break; /* pitch */
				case  8: case 10: case 11:            v->hasDynamicMod |= 1; break; /* filter */
				case 13: case 48:                     v->hasDynamicMod |= 4; break; /* gain */
				default: break;
			}
			continue;
		}
		if (dynamicOnly != dyn) continue;
		value = tsf_mod_value(m, f, v, c);
		switch (m->destOper)
		{
			case  5: if (dyn) v->modModLfoToPitchDyn += value; else v->modModLfoToPitch += value; break;
			case  6: if (dyn) v->modVibLfoToPitchDyn += value; else v->modVibLfoToPitch += value; break;
			case  8: if (dyn) v->modFilterFcDyn += value; else v->modFilterFc += value; break;
			case 10: if (dyn) v->modModLfoToFilterFcDyn += value; else v->modModLfoToFilterFc += value; break;
			case 11: if (!dyn) v->modFilterFc += value; break; /* mod envelope to filter, applied as an offset */
			case 13: if (dyn) v->modModLfoToVolumeDyn += value * 0.1f; else v->modModLfoToVolume += value * 0.1f; break;
			case 15: v->modChorusSend += value * 0.001f; if (v->modChorusSend < 0.0f) v->modChorusSend = 0.0f; else if (v->modChorusSend > 1.0f) v->modChorusSend = 1.0f; break;
			case 16: v->modReverbSend += value * 0.001f; if (v->modReverbSend < 0.0f) v->modReverbSend = 0.0f; else if (v->modReverbSend > 1.0f) v->modReverbSend = 1.0f; break;
			case 48: if (dyn) v->modAttenuationDBDyn += value * 0.01f; else v->modAttenuationDB += value * 0.01f; break;
			case 51: if (!dyn) v->modPitchCents += value * 100.0f; break; /* coarse tune */
			case 52: if (!dyn) v->modPitchCents += value; break;          /* fine tune */
			default: break; /* unsupported destination */
		}
	}
}

static void tsf_voice_render(tsf* f, struct tsf_voice* v, float* outputBuffer, int numSamples)
{
	struct tsf_region* region = v->region;
	float* input = f->fontSamples;
	float* outL = outputBuffer;
	float* outR = (f->outputmode == TSF_STEREO_UNWEAVED ? outL + numSamples : TSF_NULL);

	// Cache some values, to give them at least some chance of ending up in registers.
	TSF_BOOL dynMod = (v->hasDynamicMod != 0);
	TSF_BOOL dynModFilter = ((v->hasDynamicMod & 1) != 0);
	TSF_BOOL dynModPitch = ((v->hasDynamicMod & 2) != 0);
	TSF_BOOL dynModGain = ((v->hasDynamicMod & 4) != 0);
	TSF_BOOL updateModEnv = (region->modEnvToPitch || region->modEnvToFilterFc);
	TSF_BOOL updateModLFO = (v->modlfo.delta && (region->modLfoToPitch || region->modLfoToFilterFc || region->modLfoToVolume || v->modModLfoToPitch || v->modModLfoToFilterFc || v->modModLfoToVolume || dynModPitch || dynModFilter || dynModGain));
	TSF_BOOL updateVibLFO = (v->viblfo.delta && (region->vibLfoToPitch || v->modVibLfoToPitch || dynModPitch));
	TSF_BOOL isLooping    = (v->loopStart < v->loopEnd);
	unsigned int tmpLoopStart = v->loopStart, tmpLoopEnd = v->loopEnd;
	double tmpSampleEndDbl = (double)region->end, tmpLoopEndDbl = (double)tmpLoopEnd + 1.0;
	double tmpSourceSamplePosition = v->sourceSamplePosition;
	struct tsf_voice_lowpass tmpLowpass = v->lowpass;

	TSF_BOOL dynamicLowpass = (region->modLfoToFilterFc || region->modEnvToFilterFc || dynModFilter);
	float tmpSampleRate = f->outSampleRate, tmpInitialFilterFc, tmpModLfoToFilterFc, tmpModEnvToFilterFc;

	TSF_BOOL dynamicPitchRatio = (region->modLfoToPitch || region->modEnvToPitch || region->vibLfoToPitch || dynModPitch);
	double pitchRatio;
	float tmpModLfoToPitch, tmpVibLfoToPitch, tmpModEnvToPitch;

	TSF_BOOL dynamicGain = (region->modLfoToVolume != 0 || dynModGain);
	float noteGain = 0, tmpModLfoToVolume;

	if (dynamicLowpass) tmpInitialFilterFc = (float)region->initialFilterFc, tmpModLfoToFilterFc = (float)region->modLfoToFilterFc, tmpModEnvToFilterFc = (float)region->modEnvToFilterFc;
	else tmpInitialFilterFc = 0, tmpModLfoToFilterFc = 0, tmpModEnvToFilterFc = 0;

	if (dynamicPitchRatio) pitchRatio = 0, tmpModLfoToPitch = (float)region->modLfoToPitch, tmpVibLfoToPitch = (float)region->vibLfoToPitch, tmpModEnvToPitch = (float)region->modEnvToPitch;
	else pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents) * v->pitchOutputFactor, tmpModLfoToPitch = 0, tmpVibLfoToPitch = 0, tmpModEnvToPitch = 0;

	if (dynamicGain) tmpModLfoToVolume = (float)region->modLfoToVolume * 0.1f;
	else noteGain = tsf_decibelsToGain(v->noteGainDB), tmpModLfoToVolume = 0;

	while (numSamples)
	{
		float gainMono, gainLeft, gainRight;
		int blockSamples = (numSamples > TSF_RENDER_EFFECTSAMPLEBLOCK ? TSF_RENDER_EFFECTSAMPLEBLOCK : numSamples);
		numSamples -= blockSamples;

		if (dynMod) tsf_voice_apply_modulators(f, v, 1);

		if (dynamicLowpass)
		{
			float fres = tmpInitialFilterFc + v->modFilterFc + v->modFilterFcDyn + v->modlfo.level * (tmpModLfoToFilterFc + v->modModLfoToFilterFc + v->modModLfoToFilterFcDyn) + v->modenv.level * tmpModEnvToFilterFc;
			float lowpassFc = (fres <= 13500 ? tsf_cents2Hertz(fres) / tmpSampleRate : 1.0f);
			{
				TSF_BOOL wasActive = tmpLowpass.active;
				tmpLowpass.active = (lowpassFc < 0.499f);
				if (tmpLowpass.active) tsf_voice_lowpass_setup(&tmpLowpass, lowpassFc);
				else if (wasActive) { tmpLowpass.rampRemaining = 0; tmpLowpass.rampTotal = 0; }
			}
		}

		if (dynamicPitchRatio)
			pitchRatio = tsf_timecents2Secsd(v->pitchInputTimecents + (v->modlfo.level * (tmpModLfoToPitch + v->modModLfoToPitch + v->modModLfoToPitchDyn) + v->viblfo.level * (tmpVibLfoToPitch + v->modVibLfoToPitch + v->modVibLfoToPitchDyn) + v->modenv.level * tmpModEnvToPitch)) * v->pitchOutputFactor;

		if (dynamicGain)
			noteGain = tsf_decibelsToGain(v->noteGainDB + v->modAttenuationDB + v->modAttenuationDBDyn + (v->modlfo.level * (tmpModLfoToVolume + v->modModLfoToVolume + v->modModLfoToVolumeDyn)));

		gainMono = noteGain * v->ampenv.level;

		// Update EG.
		tsf_voice_envelope_process(&v->ampenv, blockSamples, tmpSampleRate);
		if (updateModEnv) tsf_voice_envelope_process(&v->modenv, blockSamples, tmpSampleRate);

		// Update LFOs.
		if (updateModLFO) tsf_voice_lfo_process(&v->modlfo, blockSamples);
		if (updateVibLFO) tsf_voice_lfo_process(&v->viblfo, blockSamples);

		switch (f->outputmode)
		{
			case TSF_STEREO_INTERLEAVED:
				gainLeft = gainMono * v->panFactorLeft, gainRight = gainMono * v->panFactorRight;
				while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
				{
										// High-quality Catmull-Rom interpolation (default) or linear fallback.
					float val;
					#if TSF_HIGH_QUALITY_INTERPOLATION
					val = tsf_voice_interpolate(input, tmpSourceSamplePosition, region->offset, region->end, tmpLoopStart, tmpLoopEnd, isLooping);
					#else
					{
						unsigned int pos = (unsigned int)tmpSourceSamplePosition;
						unsigned int nextPos = (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);
						float alpha = (float)(tmpSourceSamplePosition - (double)pos);
						val = input[pos] * (1.0f - alpha) + input[nextPos] * alpha;
					}
					#endif

					// Low-pass filter.
					if (tmpLowpass.active) { tsf_voice_lowpass_ramp(&tmpLowpass); val = tsf_voice_lowpass_process(&tmpLowpass, val); }

					*outL++ += val * gainLeft;
					*outL++ += val * gainRight;

					// Next sample.
					tmpSourceSamplePosition += pitchRatio;
					if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
				}
				break;

			case TSF_STEREO_UNWEAVED:
				gainLeft = gainMono * v->panFactorLeft, gainRight = gainMono * v->panFactorRight;
				while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
				{
										// High-quality Catmull-Rom interpolation (default) or linear fallback.
					float val;
					#if TSF_HIGH_QUALITY_INTERPOLATION
					val = tsf_voice_interpolate(input, tmpSourceSamplePosition, region->offset, region->end, tmpLoopStart, tmpLoopEnd, isLooping);
					#else
					{
						unsigned int pos = (unsigned int)tmpSourceSamplePosition;
						unsigned int nextPos = (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);
						float alpha = (float)(tmpSourceSamplePosition - (double)pos);
						val = input[pos] * (1.0f - alpha) + input[nextPos] * alpha;
					}
					#endif

					// Low-pass filter.
					if (tmpLowpass.active) { tsf_voice_lowpass_ramp(&tmpLowpass); val = tsf_voice_lowpass_process(&tmpLowpass, val); }

					*outL++ += val * gainLeft;
					*outR++ += val * gainRight;

					// Next sample.
					tmpSourceSamplePosition += pitchRatio;
					if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
				}
				break;

			case TSF_MONO:
				while (blockSamples-- && tmpSourceSamplePosition < tmpSampleEndDbl)
				{
										// High-quality Catmull-Rom interpolation (default) or linear fallback.
					float val;
					#if TSF_HIGH_QUALITY_INTERPOLATION
					val = tsf_voice_interpolate(input, tmpSourceSamplePosition, region->offset, region->end, tmpLoopStart, tmpLoopEnd, isLooping);
					#else
					{
						unsigned int pos = (unsigned int)tmpSourceSamplePosition;
						unsigned int nextPos = (pos >= tmpLoopEnd && isLooping ? tmpLoopStart : pos + 1);
						float alpha = (float)(tmpSourceSamplePosition - (double)pos);
						val = input[pos] * (1.0f - alpha) + input[nextPos] * alpha;
					}
					#endif

					// Low-pass filter.
					if (tmpLowpass.active) { tsf_voice_lowpass_ramp(&tmpLowpass); val = tsf_voice_lowpass_process(&tmpLowpass, val); }

					*outL++ += val * gainMono;

					// Next sample.
					tmpSourceSamplePosition += pitchRatio;
					if (tmpSourceSamplePosition >= tmpLoopEndDbl && isLooping) tmpSourceSamplePosition -= (tmpLoopEnd - tmpLoopStart + 1.0);
				}
				break;
		}

		if (tmpSourceSamplePosition >= tmpSampleEndDbl || v->ampenv.segment == TSF_SEGMENT_DONE)
		{
			tsf_voice_kill(v);
			return;
		}
	}

	v->sourceSamplePosition = tmpSourceSamplePosition;
	if (tmpLowpass.active || dynamicLowpass) v->lowpass = tmpLowpass;
}

TSFDEF tsf* tsf_load(struct tsf_stream* stream)
{
	tsf* res = TSF_NULL;
	struct tsf_riffchunk chunkHead;
	struct tsf_riffchunk chunkList;
	struct tsf_hydra hydra;
	void* rawBuffer = TSF_NULL;
	float* floatBuffer = TSF_NULL;
	tsf_u32 smplCount = 0;

	if (!tsf_riffchunk_read(TSF_NULL, &chunkHead, stream) || !TSF_FourCCEquals(chunkHead.id, "sfbk"))
	{
		//if (e) *e = TSF_INVALID_NOSF2HEADER;
		return res;
	}

	// Read hydra and locate sample data.
	TSF_MEMSET(&hydra, 0, sizeof(hydra));
	while (tsf_riffchunk_read(&chunkHead, &chunkList, stream))
	{
		struct tsf_riffchunk chunk;
		if (TSF_FourCCEquals(chunkList.id, "pdta"))
		{
			while (tsf_riffchunk_read(&chunkList, &chunk, stream))
			{
				#define HandleChunk(chunkName) (TSF_FourCCEquals(chunk.id, #chunkName) && !(chunk.size % chunkName##SizeInFile)) \
					{ \
						int num = (int)(chunk.size / chunkName##SizeInFile), i; \
						TSF_FREE(hydra.chunkName##s); hydra.chunkName##s = TSF_NULL; hydra.chunkName##Num = 0; /* a repeated chunk replaces the earlier one */ \
						if (num > 0) \
						{ \
							num = TSF_ALLOC_GUARD(num, sizeof(struct tsf_hydra_##chunkName)); \
							if (!num) goto out_of_memory; \
							hydra.chunkName##s = (struct tsf_hydra_##chunkName*)TSF_MALLOC(num * sizeof(struct tsf_hydra_##chunkName)); \
							if (!hydra.chunkName##s) goto out_of_memory; \
							for (i = 0; i < num; ++i) if (!tsf_hydra_read_##chunkName(&hydra.chunkName##s[i], stream)) goto out_of_memory; \
							hydra.chunkName##Num = num; \
						} \
					}
				enum
				{
					phdrSizeInFile = 38, pbagSizeInFile =  4, pmodSizeInFile = 10,
					pgenSizeInFile =  4, instSizeInFile = 22, ibagSizeInFile =  4,
					imodSizeInFile = 10, igenSizeInFile =  4, shdrSizeInFile = 46
				};
				if      HandleChunk(phdr) else if HandleChunk(pbag) else if HandleChunk(pmod)
				else if HandleChunk(pgen) else if HandleChunk(inst) else if HandleChunk(ibag)
				else if HandleChunk(imod) else if HandleChunk(igen) else if HandleChunk(shdr)
				else stream->skip(stream->data, chunk.size);
				#undef HandleChunk
			}
		}
		else if (TSF_FourCCEquals(chunkList.id, "sdta"))
		{
			while (tsf_riffchunk_read(&chunkList, &chunk, stream))
			{
				if ((TSF_FourCCEquals(chunk.id, "smpl")
						#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
						|| TSF_FourCCEquals(chunk.id, "smpo")
						#endif
					) && !rawBuffer && !floatBuffer && chunk.size >= sizeof(short))
				{
					if (!tsf_load_samples(&rawBuffer, &floatBuffer, &smplCount, &chunk, stream)) goto out_of_memory;
				}
				else stream->skip(stream->data, chunk.size);
			}
		}
		else stream->skip(stream->data, chunkList.size);
	}
	if (!hydra.phdrs || !hydra.pbags || !hydra.pmods || !hydra.pgens || !hydra.insts || !hydra.ibags || !hydra.imods || !hydra.igens || !hydra.shdrs)
	{
		//if (e) *e = TSF_INVALID_INCOMPLETE;
	}
	else if (!rawBuffer && !floatBuffer)
	{
		//if (e) *e = TSF_INVALID_NOSAMPLEDATA;
	}
	else if (!tsf_hydra_sanitize(&hydra))
	{
		//if (e) *e = TSF_INVALID_INCOMPLETE;
	}
	else
	{
		#ifdef STB_VORBIS_INCLUDE_STB_VORBIS_H
		if (!floatBuffer && !tsf_decode_sf3_samples(rawBuffer, &floatBuffer, &smplCount, &hydra)) goto out_of_memory;
		#endif
		res = (tsf*)TSF_MALLOC(sizeof(tsf));
		if (res) TSF_MEMSET(res, 0, sizeof(tsf));
		if (!res || !tsf_load_presets(res, &hydra, smplCount)) goto out_of_memory;
		res->outSampleRate = 44100.0f;
		res->fontSamples = floatBuffer;
		floatBuffer = TSF_NULL; // don't free below
	}
	if (0)
	{
		out_of_memory:
		TSF_FREE(res);
		res = TSF_NULL;
		//if (e) *e = TSF_OUT_OF_MEMORY;
	}
	TSF_FREE(hydra.phdrs); TSF_FREE(hydra.pbags); TSF_FREE(hydra.pmods);
	TSF_FREE(hydra.pgens); TSF_FREE(hydra.insts); TSF_FREE(hydra.ibags);
	TSF_FREE(hydra.imods); TSF_FREE(hydra.igens); TSF_FREE(hydra.shdrs);
	TSF_FREE(rawBuffer);   TSF_FREE(floatBuffer);
	return res;
}

TSFDEF tsf* tsf_copy(tsf* f)
{
	tsf* res;
	if (!f) return TSF_NULL;
	if (!f->refCount)
	{
		f->refCount = (int*)TSF_MALLOC(sizeof(int));
		if (!f->refCount) return TSF_NULL;
		*f->refCount = 1;
	}
	res = (tsf*)TSF_MALLOC(sizeof(tsf));
	if (!res) return TSF_NULL;
	TSF_MEMCPY(res, f, sizeof(tsf));
	res->voices = TSF_NULL;
	res->voiceNum = 0;
	res->maxVoiceNum = 0; // the copy has no pre-allocated voices (call tsf_set_max_voices again if wanted)
	res->channels = TSF_NULL;
	res->effects = TSF_NULL; /* the effect buffers belong to the original instance */
	(*res->refCount)++;
	return res;
}

TSFDEF void tsf_close(tsf* f)
{
	if (!f) return;
	if (!f->refCount || !--(*f->refCount))
	{
		struct tsf_preset *preset = f->presets, *presetEnd = preset + f->presetNum;
		for (; preset != presetEnd; preset++) TSF_FREE(preset->regions);
		TSF_FREE(f->presets);
		TSF_FREE(f->fontSamples);
		TSF_FREE(f->refCount);
	}
	TSF_FREE(f->channels);
	TSF_FREE(f->voices);
	tsf_effects_free(f->effects);
	TSF_FREE(f);
}

TSFDEF void tsf_reset(tsf* f)
{
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && (v->ampenv.segment < TSF_SEGMENT_RELEASE || v->ampenv.parameters.release))
			tsf_voice_endquick(f, v);
	if (f->channels) { TSF_FREE(f->channels); f->channels = TSF_NULL; }
}

TSFDEF int tsf_get_presetindex(const tsf* f, int bank, int preset_number)
{
	const struct tsf_preset *presets;
	int i, iMax;
	for (presets = f->presets, i = 0, iMax = f->presetNum; i < iMax; i++)
		if (presets[i].preset == preset_number && presets[i].bank == bank)
			return i;
	return -1;
}

TSFDEF int tsf_get_presetcount(const tsf* f)
{
	return f->presetNum;
}

TSFDEF const char* tsf_get_presetname(const tsf* f, int preset)
{
	return (preset < 0 || preset >= f->presetNum ? TSF_NULL : f->presets[preset].presetName);
}

TSFDEF const char* tsf_bank_get_presetname(const tsf* f, int bank, int preset_number)
{
	return tsf_get_presetname(f, tsf_get_presetindex(f, bank, preset_number));
}

TSFDEF void tsf_set_output(tsf* f, enum TSFOutputMode outputmode, int samplerate, float global_gain_db)
{
	f->outputmode = outputmode;
	f->outSampleRate = (float)(samplerate >= 1 ? samplerate : 44100.0f);
	f->globalGainDB = global_gain_db;
}

TSFDEF void tsf_set_volume(tsf* f, float global_volume)
{
	f->globalGainDB = (global_volume == 1.0f ? 0 : -tsf_gainToDecibels(1.0f / global_volume));
}

TSFDEF int tsf_set_max_voices(tsf* f, int max_voices)
{
	int i = f->voiceNum;
	int newVoiceNum = (f->voiceNum > max_voices ? f->voiceNum : max_voices);
	struct tsf_voice *newVoices = (struct tsf_voice*)TSF_REALLOC(f->voices, newVoiceNum * sizeof(struct tsf_voice));
	if (!newVoices) return 0;
	f->voices = newVoices;
	f->voiceNum = f->maxVoiceNum = newVoiceNum;
	for (; i < max_voices; i++)
		f->voices[i].playingPreset = -1;
	return 1;
}

TSFDEF int tsf_note_on(tsf* f, int preset_index, int key, float vel)
{
	short midiVelocity;
	unsigned int voicePlayIndex;
	struct tsf_region *region, *regionEnd;

	if (preset_index < 0 || preset_index >= f->presetNum) return 1;
	if (!(vel > 0.0f)) { tsf_note_off(f, preset_index, key); return 1; } // also catches NaN
	if (vel >= 1.0f) midiVelocity = 127;
	else { int mv = (int)(vel * 127.0f); midiVelocity = (short)mv; }

	// Play all matching regions.
	voicePlayIndex = f->voicePlayIndex++;
	for (region = f->presets[preset_index].regions, regionEnd = region + f->presets[preset_index].regionNum; region != regionEnd; region++)
	{
		struct tsf_voice *voice, *v, *vEnd; TSF_BOOL doLoop; float lowpassFilterQDB, lowpassFc;
		if (key < region->lokey || key > region->hikey || midiVelocity < region->lovel || midiVelocity > region->hivel) continue;

	voice = TSF_NULL, v = f->voices, vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
		if (region->group)
		{
			for (; v != vEnd; v++)
				if (v->playingPreset == preset_index && v->region->group == region->group) tsf_voice_endquick(f, v);
				else if (v->playingPreset == -1 && !voice) voice = v;
		}
		else for (; v != vEnd; v++) if (v->playingPreset == -1) { voice = v; break; }

		if (!voice)
		{
			if (f->maxVoiceNum)
			{
				// Voices have been pre-allocated and limited to a maximum, try to kill a voice off in its release envelope
				int bestKillReleaseSamplePos = -999999999;
				for (v = f->voices; v != vEnd; v++)
				{
					if (v->ampenv.segment == TSF_SEGMENT_RELEASE)
					{
						// We're looking for the voice furthest into its release
						int releaseSamplesDone = tsf_voice_envelope_release_samples(&v->ampenv, f->outSampleRate) - v->ampenv.samplesUntilNextSegment;
						if (releaseSamplesDone > bestKillReleaseSamplePos)
						{
							bestKillReleaseSamplePos = releaseSamplesDone;
							voice = v;
						}
					}
				}
				#ifndef TSF_NO_VOICE_STEALING
				if (!voice)
				{
					// Nothing is releasing, steal the voice that has been playing the longest instead of dropping the new note
					unsigned int bestAge = 0;
					for (v = f->voices; v != vEnd; v++)
					{
						unsigned int age = voicePlayIndex - v->playIndex;
						if (v->playingPreset != -1 && (!voice || age > bestAge)) { bestAge = age; voice = v; }
					}
				}
				#endif
				if (!voice)
					continue;
				tsf_voice_kill(voice);
			}
			else
			{
				// Allocate more voices so we don't need to kill one off.
				struct tsf_voice* newVoices;
				newVoices = (struct tsf_voice*)TSF_REALLOC(f->voices, (f->voiceNum + 4) * sizeof(struct tsf_voice));
				if (!newVoices) return 0; // voiceNum is untouched so the existing voices stay valid
				f->voices = newVoices;
				f->voiceNum += 4;
				voice = &f->voices[f->voiceNum - 4];
				voice[1].playingPreset = voice[2].playingPreset = voice[3].playingPreset = -1;
			}
		}

		voice->region = region;
		voice->playingPreset = preset_index;
		voice->playingKey = key;
		voice->playIndex = voicePlayIndex;
		voice->heldSustain = 0;
		voice->modVelocity = (unsigned char)midiVelocity;
		voice->modPitchCents = 0.0f;
		voice->modChorusSend = voice->modReverbSend = 0.0f;
		voice->noteGainDB = f->globalGainDB - region->attenuation - tsf_gainToDecibels(1.0f / vel);

		// Modulators: evaluated here so the channel setup below already sees the
		// pitch offset, and the static attenuation is folded into the gain.
		tsf_voice_apply_modulators(f, voice, 0);
		voice->noteGainDB += voice->modAttenuationDB;
		if (f->channels)
		{
			struct tsf_channel* vc = tsf_voice_mod_channel(f, voice);
			if (vc)
			{
				voice->modReverbSend += vc->reverbSend;
				voice->modChorusSend += vc->chorusSend;
			}
			f->channels->setupVoice(f, voice);
		}
		else
		{
			tsf_voice_calcpitchratio(voice, 0, f->outSampleRate);
			// The SFZ spec is silent about the pan curve, but a 3dB pan law seems common. This sqrt() curve matches what Dimension LE does; Alchemy Free seems closer to sin(adjustedPan * pi/2).
			voice->panFactorLeft  = TSF_SQRTF(0.5f - region->pan);
			voice->panFactorRight = TSF_SQRTF(0.5f + region->pan);
		}

		// Offset/end.
		voice->sourceSamplePosition = region->offset;

		// Loop.
		doLoop = (region->loop_mode != TSF_LOOPMODE_NONE && region->loop_start < region->loop_end);
		voice->loopStart = (doLoop ? region->loop_start : 0);
		voice->loopEnd = (doLoop ? region->loop_end : 0);

		// Setup envelopes.
		tsf_voice_envelope_setup(&voice->ampenv, &region->ampenv, key, midiVelocity, TSF_TRUE, f->outSampleRate);
		tsf_voice_envelope_setup(&voice->modenv, &region->modenv, key, midiVelocity, TSF_FALSE, f->outSampleRate);

		// Setup lowpass filter.
		lowpassFc = (region->initialFilterFc + voice->modFilterFc <= 13500 ? tsf_cents2Hertz((float)(region->initialFilterFc + voice->modFilterFc)) : 1.0f);
		if (lowpassFc != 1.0f) lowpassFc /= f->outSampleRate;
		lowpassFilterQDB = region->initialFilterQ / 10.0f;
		voice->lowpass.QInv = 1.0 / TSF_POW(10.0, (lowpassFilterQDB / 20.0));
		voice->lowpass.z1 = voice->lowpass.z2 = 0;
		voice->lowpass.active = TSF_FALSE;
		voice->lowpass.rampRemaining = voice->lowpass.rampTotal = 0;
		if (lowpassFc < 0.499f)
		{
			voice->lowpass.active = TSF_TRUE;
			tsf_voice_lowpass_setup(&voice->lowpass, lowpassFc);
		}

		// Setup LFO filters.
		tsf_voice_lfo_setup(&voice->modlfo, region->delayModLFO, region->freqModLFO, f->outSampleRate);
		tsf_voice_lfo_setup(&voice->viblfo, region->delayVibLFO, region->freqVibLFO, f->outSampleRate);
	}
	return 1;
}

TSFDEF int tsf_bank_note_on(tsf* f, int bank, int preset_number, int key, float vel)
{
	int preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	return tsf_note_on(f, preset_index, key, vel);
}

TSFDEF void tsf_note_off(tsf* f, int preset_index, int key)
{
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL, *vMatchFirst = TSF_NULL, *vMatchLast = TSF_NULL;
	for (; v != vEnd; v++)
	{
		//Find the first and last entry in the voices list with matching preset, key and look up the smallest play index
		if (v->playingPreset != preset_index || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE) continue;
		else if (!vMatchFirst || v->playIndex < vMatchFirst->playIndex) vMatchFirst = vMatchLast = v;
		else if (v->playIndex == vMatchFirst->playIndex) vMatchLast = v;
	}
	if (!vMatchFirst) return;
	for (v = vMatchFirst; v <= vMatchLast; v++)
	{
		//Stop all voices with matching preset, key and the smallest play index which was enumerated above
		if (v != vMatchFirst && v != vMatchLast &&
			(v->playIndex != vMatchFirst->playIndex || v->playingPreset != preset_index || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE)) continue;
		tsf_voice_end(f, v);
	}
}

TSFDEF int tsf_bank_note_off(tsf* f, int bank, int preset_number, int key)
{
	int preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	tsf_note_off(f, preset_index, key);
	return 1;
}

TSFDEF void tsf_note_off_all(tsf* f)
{
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (; v != vEnd; v++) if (v->playingPreset != -1 && v->ampenv.segment < TSF_SEGMENT_RELEASE)
		tsf_voice_end(f, v);
}

TSFDEF int tsf_active_voice_count(tsf* f)
{
	int count = 0;
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (; v != vEnd; v++) if (v->playingPreset != -1) count++;
	return count;
}

TSFDEF void tsf_render_short(tsf* f, short* buffer, int samples, int flag_mixing)
{
	float outputSamples[TSF_RENDER_SHORTBUFFERBLOCK];
	int channels = (f->outputmode == TSF_MONO ? 1 : 2), maxChannelSamples = TSF_RENDER_SHORTBUFFERBLOCK / channels;
	while (samples > 0)
	{
		int channelSamples = (samples > maxChannelSamples ? maxChannelSamples : samples);
		short* bufferEnd = buffer + channelSamples * channels;
		float *floatSamples = outputSamples;
		tsf_render_float(f, floatSamples, channelSamples, TSF_FALSE);
		samples -= channelSamples;

		if (flag_mixing)
			while (buffer != bufferEnd)
			{
				float v = *floatSamples++;
				int vi = *buffer + (v < -1.00004566f ? (int)-32768 : (v > 1.00001514f ? (int)32767 : (int)(v * 32767.5f)));
				*buffer++ = (vi < -32768 ? (short)-32768 : (vi > 32767 ? (short)32767 : (short)vi));
			}
		else
			while (buffer != bufferEnd)
			{
				float v = *floatSamples++;
				*buffer++ = (v < -1.00004566f ? (short)-32768 : (v > 1.00001514f ? (short)32767 : (short)(v * 32767.5f)));
			}
	}
}

static void tsf_effects_free(struct tsf_effects* e)
{
	if (!e) return;
	TSF_FREE(e->chorus[0]); TSF_FREE(e->chorus[1]);
	TSF_FREE(e->reverb[0]); TSF_FREE(e->reverb[1]);
	TSF_FREE(e);
}

TSFDEF int tsf_set_effects(tsf* f, float chorus_level, float reverb_level)
{
	static const int combTune[TSF_FX_COMBS] = { 1116, 1188, 1277, 1356 };
	static const int apTune[TSF_FX_ALLPASS] = { 556, 441 };
	struct tsf_effects* e;
	int ch, i, chorusSize, reverbSize;
	if (!f) return 0;
	if (!(chorus_level > 0.0f)) chorus_level = 0.0f; else if (chorus_level > 1.0f) chorus_level = 1.0f;
	if (!(reverb_level > 0.0f)) reverb_level = 0.0f; else if (reverb_level > 1.0f) reverb_level = 1.0f;
	if (chorus_level == 0.0f && reverb_level == 0.0f)
	{
		tsf_effects_free(f->effects);
		f->effects = TSF_NULL;
		return 1;
	}
	e = f->effects;
	if (!e)
	{
		e = (struct tsf_effects*)TSF_MALLOC(sizeof(struct tsf_effects));
		if (!e) return 0;
		TSF_MEMSET(e, 0, sizeof(struct tsf_effects));
		f->effects = e;
	}
	e->chorusLevel = chorus_level;
	e->reverbLevel = reverb_level;

	/* Sizes scale with the output sample rate, with a sane floor. */
	chorusSize = (int)(0.05f * f->outSampleRate);   /* 50 ms delay line */
	if (chorusSize < 256) chorusSize = 256;
	reverbSize = 0;
	for (i = 0; i < TSF_FX_COMBS; i++)
	{
		e->combOffset[i] = reverbSize;
		e->combLen[i] = (int)(combTune[i] * f->outSampleRate / 44100.0f);
		if (e->combLen[i] < 32) e->combLen[i] = 32;
		reverbSize += e->combLen[i];
	}
	for (i = 0; i < TSF_FX_ALLPASS; i++)
	{
		e->apOffset[i] = reverbSize;
		e->apLen[i] = (int)(apTune[i] * f->outSampleRate / 44100.0f);
		if (e->apLen[i] < 16) e->apLen[i] = 16;
		reverbSize += e->apLen[i];
	}
	if (e->chorusSize != chorusSize)
	{
		for (ch = 0; ch < 2; ch++)
		{
			TSF_FREE(e->chorus[ch]);
			e->chorus[ch] = (float*)TSF_MALLOC(sizeof(float) * (size_t)chorusSize);
			if (!e->chorus[ch]) { e->chorusSize = 0; return 0; }
			TSF_MEMSET(e->chorus[ch], 0, sizeof(float) * (size_t)chorusSize);
		}
		e->chorusSize = chorusSize;
	}
	if (e->reverbSize != reverbSize)
	{
		for (ch = 0; ch < 2; ch++)
		{
			TSF_FREE(e->reverb[ch]);
			e->reverb[ch] = (float*)TSF_MALLOC(sizeof(float) * (size_t)reverbSize);
			if (!e->reverb[ch]) { e->reverbSize = 0; return 0; }
			TSF_MEMSET(e->reverb[ch], 0, sizeof(float) * (size_t)reverbSize);
		}
		e->reverbSize = reverbSize;
	}
	e->chorusPos = 0;
	e->chorusPhase = 0.0f;
	for (i = 0; i < TSF_FX_COMBS; i++) e->combPos[i] = 0;
	for (i = 0; i < TSF_FX_ALLPASS; i++) e->apPos[i] = 0;
	return 1;
}

/* Apply chorus and reverb to an already mixed buffer. Called from
 * tsf_render_float, so it must not allocate or block. */
static void tsf_effects_process(tsf* f, float* buffer, int samples)
{
	struct tsf_effects* e = f->effects;
	struct tsf_voice *v, *vEnd;
	float sendC = 0.0f, sendR = 0.0f, lfoStep, chorusBase, chorusDepth;
	int i, ch, comb, ap;
	const float reverbFeedback = 0.78f, reverbDamp = 0.35f, allpassFeedback = 0.5f;
	if (!e || samples <= 0) return;
	vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (v = f->voices; v != vEnd; v++)
	{
		if (v->playingPreset == -1) continue;
		if (v->modChorusSend > sendC) sendC = v->modChorusSend;
		if (v->modReverbSend > sendR) sendR = v->modReverbSend;
	}
	sendC = (sendC > 1.0f ? 1.0f : sendC) * e->chorusLevel;
	sendR = (sendR > 1.0f ? 1.0f : sendR) * e->reverbLevel;
	if (sendC <= 0.0f && sendR <= 0.0f) return;

	lfoStep = (float)(2.0 * TSF_PI * 0.6 / f->outSampleRate); /* 0.6 Hz */
	chorusBase = 0.012f * f->outSampleRate;                   /* 12 ms */
	chorusDepth = 0.004f * f->outSampleRate;                  /* +/- 4 ms */

	for (i = 0; i < samples; i++)
	{
		float inL, inR, outL, outR;
		float dryL, dryR;
		if (f->outputmode == TSF_STEREO_UNWEAVED) { dryL = buffer[i]; dryR = buffer[samples + i]; }
		else if (f->outputmode == TSF_STEREO_INTERLEAVED) { dryL = buffer[i * 2]; dryR = buffer[i * 2 + 1]; }
		else { dryL = dryR = buffer[i]; }
		inL = dryL; inR = dryR;
		outL = outR = 0.0f;

		if (sendC > 0.0f && e->chorusSize > 0)
		{
			for (ch = 0; ch < 2; ch++)
			{
				float* line = e->chorus[ch];
				float phase = e->chorusPhase + (ch ? 0.25f : 0.0f);
				float input = (ch ? inR : inL);
				float delay = chorusBase + chorusDepth * (float)TSF_SIN(2.0 * TSF_PI * phase);
				int dpos = e->chorusPos - (int)delay;
				float frac = delay - (float)(int)delay;
				float s0, s1, wet;
				while (dpos < 0) dpos += e->chorusSize;
				if (dpos >= e->chorusSize) dpos -= e->chorusSize;
				s0 = line[dpos];
				s1 = line[(dpos + 1 >= e->chorusSize ? 0 : dpos + 1)];
				wet = s0 + (s1 - s0) * frac;
				line[e->chorusPos] = input + wet * 0.2f; /* a little feedback for warmth */
				if (ch) outR += wet; else outL += wet;
			}
			e->chorusPos++;
			if (e->chorusPos >= e->chorusSize) e->chorusPos = 0;
			e->chorusPhase += lfoStep;
			if (e->chorusPhase >= 1.0f) e->chorusPhase -= 1.0f;
		}

		if (sendR > 0.0f && e->reverbSize > 0)
		{
			for (ch = 0; ch < 2; ch++)
			{
				float* rvb = e->reverb[ch];
				float input = (ch ? inR : inL) * 0.5f;
				float acc = 0.0f;
				for (comb = 0; comb < TSF_FX_COMBS; comb++)
				{
					float* line = rvb + e->combOffset[comb];
					int len = e->combLen[comb], pos = e->combPos[comb];
					float y = line[pos];
					acc += y;
					e->combStore[ch][comb] = y * (1.0f - reverbDamp) + e->combStore[ch][comb] * reverbDamp;
					line[pos] = input + e->combStore[ch][comb] * reverbFeedback;
					if (++pos >= len) pos = 0;
					e->combPos[comb] = pos;
				}
				acc *= 0.25f;
				for (ap = 0; ap < TSF_FX_ALLPASS; ap++)
				{
					float* line = rvb + e->apOffset[ap];
					int len = e->apLen[ap], pos = e->apPos[ap];
					float y = line[pos];
					float out = -acc + y;
					line[pos] = acc + y * allpassFeedback;
					if (++pos >= len) pos = 0;
					e->apPos[ap] = pos;
					acc = out;
				}
				if (ch) outR += acc; else outL += acc;
			}
		}

		if (f->outputmode == TSF_STEREO_UNWEAVED) { buffer[i] = dryL + outL * sendC + outL * sendR; buffer[samples + i] = dryR + outR * sendC + outR * sendR; }
		else if (f->outputmode == TSF_STEREO_INTERLEAVED) { buffer[i * 2] = dryL + outL * sendC + outL * sendR; buffer[i * 2 + 1] = dryR + outR * sendC + outR * sendR; }
		else buffer[i] = dryL + (outL + outR) * 0.5f * (sendC + sendR);
	}
}

TSFDEF void tsf_render_float(tsf* f, float* buffer, int samples, int flag_mixing)
{
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	if (samples <= 0) return;
	if (!flag_mixing) TSF_MEMSET(buffer, 0, (f->outputmode == TSF_MONO ? 1 : 2) * sizeof(float) * (size_t)samples);
	for (; v != vEnd; v++)
		if (v->playingPreset != -1)
			tsf_voice_render(f, v, buffer, samples);
	if (f->effects) tsf_effects_process(f, buffer, samples);
}

static void tsf_channel_setup_voice(tsf* f, struct tsf_voice* v)
{
	struct tsf_channel* c = &f->channels->channels[f->channels->activeChannel];
	float newpan = v->region->pan + c->panOffset;
	v->playingChannel = f->channels->activeChannel;
	v->noteGainDB += c->gainDB;
	tsf_voice_calcpitchratio(v, (c->pitchWheel == 8192 ? c->tuning : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) - c->pitchRange + c->tuning)), f->outSampleRate);
	if      (newpan <= -0.5f) { v->panFactorLeft = 1.0f; v->panFactorRight = 0.0f; }
	else if (newpan >=  0.5f) { v->panFactorLeft = 0.0f; v->panFactorRight = 1.0f; }
	else { v->panFactorLeft = TSF_SQRTF(0.5f - newpan); v->panFactorRight = TSF_SQRTF(0.5f + newpan); }
}

static struct tsf_channel* tsf_channel_init(tsf* f, int channel)
{
	int i;
	if (channel < 0 || channel > 0xFFFF) return TSF_NULL;
	if (f->channels && channel < f->channels->channelNum) return &f->channels->channels[channel];
	if (!f->channels)
	{
		f->channels = (struct tsf_channels*)TSF_MALLOC(sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
		if (!f->channels) return TSF_NULL;
		f->channels->setupVoice = &tsf_channel_setup_voice;
		f->channels->channelNum = 0;
		f->channels->activeChannel = 0;
	}
	else
	{
		struct tsf_channels *newChannels = (struct tsf_channels*)TSF_REALLOC(f->channels, sizeof(struct tsf_channels) + sizeof(struct tsf_channel) * channel);
		if (!newChannels) return TSF_NULL;
		f->channels = newChannels;
	}
	i = f->channels->channelNum;
	f->channels->channelNum = channel + 1;
	for (; i <= channel; i++)
	{
		struct tsf_channel* c = &f->channels->channels[i];
		c->presetIndex = c->bank = 0;
		c->pitchWheel = c->midiPan = 8192;
		c->midiVolume = c->midiExpression = 16383;
		c->midiRPN = 0xFFFF;
		c->midiData = c->sustain = 0;
		c->panOffset = 0.0f;
		c->gainDB = 0.0f;
		c->pitchRange = 2.0f;
		c->tuning = 0.0f;
		c->reverbSend = 0.0f;
		c->chorusSend = 0.0f;
		c->chanPressure = 0;
		{ int j; for (j = 0; j < 128; j++) c->cc[j] = 0; }
	}
	return &f->channels->channels[channel];
}

static void tsf_channel_applypitch(tsf* f, int channel, struct tsf_channel* c)
{
	struct tsf_voice *v, *vEnd;
	float pitchShift = (c->pitchWheel == 8192 ? c->tuning : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) - c->pitchRange + c->tuning));
	for (v = f->voices, vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
			tsf_voice_calcpitchratio(v, pitchShift, f->outSampleRate);
}

TSFDEF int tsf_channel_set_presetindex(tsf* f, int channel, int preset_index)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->presetIndex = (unsigned short)preset_index;
	return 1;
}

TSFDEF int tsf_channel_set_presetnumber(tsf* f, int channel, int preset_number, int flag_mididrums)
{
	int preset_index;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (flag_mididrums)
	{
		preset_index = tsf_get_presetindex(f, 128 | (c->bank & 0x7FFF), preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 128, preset_number);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, 128, 0);
		if (preset_index == -1) preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
	}
	else preset_index = tsf_get_presetindex(f, (c->bank & 0x7FFF), preset_number);
	if (preset_index == -1) preset_index = tsf_get_presetindex(f, 0, preset_number);
	if (preset_index != -1)
	{
		c->presetIndex = (unsigned short)preset_index;
		return 1;
	}
	return 0;
}

TSFDEF int tsf_channel_set_bank(tsf* f, int channel, int bank)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	c->bank = (unsigned short)bank;
	return 1;
}

TSFDEF int tsf_channel_set_bank_preset(tsf* f, int channel, int bank, int preset_number)
{
	int preset_index;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	preset_index = tsf_get_presetindex(f, bank, preset_number);
	if (preset_index == -1) return 0;
	c->presetIndex = (unsigned short)preset_index;
	c->bank = (unsigned short)bank;
	return 1;
}

TSFDEF int tsf_channel_set_pan(tsf* f, int channel, float pan)
{
	struct tsf_voice *v, *vEnd;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	for (v = f->voices, vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
		{
			float newpan = v->region->pan + pan - 0.5f;
			if      (newpan <= -0.5f) { v->panFactorLeft = 1.0f; v->panFactorRight = 0.0f; }
			else if (newpan >=  0.5f) { v->panFactorLeft = 0.0f; v->panFactorRight = 1.0f; }
			else { v->panFactorLeft = TSF_SQRTF(0.5f - newpan); v->panFactorRight = TSF_SQRTF(0.5f + newpan); }
		}
	c->panOffset = pan - 0.5f;
	return 1;
}

TSFDEF int tsf_channel_set_volume(tsf* f, int channel, float volume)
{
	float gainDB = tsf_gainToDecibels(volume), gainDBChange;
	struct tsf_voice *v, *vEnd;
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (gainDB == c->gainDB) return 1;
	for (v = f->voices, vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL, gainDBChange = gainDB - c->gainDB; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel)
			v->noteGainDB += gainDBChange;
	c->gainDB = gainDB;
	return 1;
}

TSFDEF int tsf_channel_set_pitchwheel(tsf* f, int channel, int pitch_wheel)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pitchWheel == pitch_wheel) return 1;
	c->pitchWheel = (unsigned short)pitch_wheel;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_pitchrange(tsf* f, int channel, float pitch_range)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->pitchRange == pitch_range) return 1;
	c->pitchRange = pitch_range;
	if (c->pitchWheel != 8192) tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_tuning(tsf* f, int channel, float tuning)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (c->tuning == tuning) return 1;
	c->tuning = tuning;
	tsf_channel_applypitch(f, channel, c);
	return 1;
}

TSFDEF int tsf_channel_set_sustain(tsf* f, int channel, int flag_sustain)
{
	struct tsf_channel *c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (!c->sustain == !flag_sustain) return 1;
	c->sustain = (unsigned short)(flag_sustain != 0);
	//Turning on sustain does no action now, just starts note_off behaving differently
	if (flag_sustain) return 1;
	//Turning off sustain, actually end voices that got a note_off and were set to heldSustain status
	{
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE && v->heldSustain)
			tsf_voice_end(f, v);
	}
	return 1;
}

TSFDEF int tsf_channel_note_on(tsf* f, int channel, int key, float vel)
{
	if (!f->channels || channel < 0 || channel >= f->channels->channelNum) return 1;
	f->channels->activeChannel = channel;
	if (!vel)
	{
		tsf_channel_note_off(f, channel, key);
		return 1;
	}
	return tsf_note_on(f, f->channels->channels[channel].presetIndex, key, vel);
}

TSFDEF void tsf_channel_note_off(tsf* f, int channel, int key)
{
	unsigned sustain;
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL, *vMatchFirst = TSF_NULL, *vMatchLast = TSF_NULL;
	for (; v != vEnd; v++)
	{
		//Find the first and last entry in the voices list with matching channel, key and look up the smallest play index
		if (v->playingPreset == -1 || v->playingChannel != channel || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE || v->heldSustain) continue;
		else if (!vMatchFirst || v->playIndex < vMatchFirst->playIndex) vMatchFirst = vMatchLast = v;
		else if (v->playIndex == vMatchFirst->playIndex) vMatchLast = v;
	}
	if (!vMatchFirst || !f->channels || channel < 0 || channel >= f->channels->channelNum) return;
	for (sustain = f->channels->channels[channel].sustain, v = vMatchFirst; v <= vMatchLast; v++)
	{
		//Stop all voices with matching channel, key and the smallest play index which was enumerated above
		if (v != vMatchFirst && v != vMatchLast &&
			(v->playIndex != vMatchFirst->playIndex || v->playingPreset == -1 || v->playingChannel != channel || v->playingKey != key || v->ampenv.segment >= TSF_SEGMENT_RELEASE)) continue;
		//Don't turn off if sustain is active, just mark as held by sustain so we don't forget it
		if (sustain)
			v->heldSustain = 1;
		else
			tsf_voice_end(f, v);
	}
}

TSFDEF void tsf_channel_note_off_all(tsf* f, int channel)
{
	//Ignore sustain channel settings, note_off_all overrides
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && v->ampenv.segment < TSF_SEGMENT_RELEASE)
			tsf_voice_end(f, v);
}

TSFDEF void tsf_channel_sounds_off_all(tsf* f, int channel)
{
	struct tsf_voice *v = f->voices, *vEnd = f->voices ? f->voices + f->voiceNum : TSF_NULL;
	for (; v != vEnd; v++)
		if (v->playingPreset != -1 && v->playingChannel == channel && (v->ampenv.segment < TSF_SEGMENT_RELEASE || v->ampenv.parameters.release))
			tsf_voice_endquick(f, v);
}

TSFDEF int tsf_channel_midi_control(tsf* f, int channel, int controller, int control_value)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (control_value < 0) control_value = 0; else if (control_value > 127) control_value = 127; // MIDI data bytes are 7 bit
	c->cc[controller & 0x7F] = (unsigned char)control_value;
	switch (controller)
	{
		case   7 /*VOLUME_MSB*/      : c->midiVolume     = (unsigned short)((c->midiVolume     & 0x7F  ) | (control_value << 7)); goto TCMC_SET_VOLUME;
		case  39 /*VOLUME_LSB*/      : c->midiVolume     = (unsigned short)((c->midiVolume     & 0x3F80) |  control_value);       goto TCMC_SET_VOLUME;
		case  11 /*EXPRESSION_MSB*/  : c->midiExpression = (unsigned short)((c->midiExpression & 0x7F  ) | (control_value << 7)); goto TCMC_SET_VOLUME;
		case  43 /*EXPRESSION_LSB*/  : c->midiExpression = (unsigned short)((c->midiExpression & 0x3F80) |  control_value);       goto TCMC_SET_VOLUME;
		case  10 /*PAN_MSB*/         : c->midiPan        = (unsigned short)((c->midiPan        & 0x7F  ) | (control_value << 7)); goto TCMC_SET_PAN;
		case  42 /*PAN_LSB*/         : c->midiPan        = (unsigned short)((c->midiPan        & 0x3F80) |  control_value);       goto TCMC_SET_PAN;
		case   6 /*DATA_ENTRY_MSB*/  : c->midiData       = (unsigned short)((c->midiData       & 0x7F)   | (control_value << 7)); goto TCMC_SET_DATA;
		case  38 /*DATA_ENTRY_LSB*/  : c->midiData       = (unsigned short)((c->midiData       & 0x3F80) |  control_value);       goto TCMC_SET_DATA;
		case   0 /*BANK_SELECT_MSB*/ : c->bank = (unsigned short)(0x8000 | control_value); return 1; //bank select MSB alone acts like LSB
		case  32 /*BANK_SELECT_LSB*/ : c->bank = (unsigned short)((c->bank & 0x8000 ? ((c->bank & 0x7F) << 7) : 0) | control_value); return 1;
		case 101 /*RPN_MSB*/         : c->midiRPN = (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x7F  ) | (control_value << 7)); return 1;
		case 100 /*RPN_LSB*/         : c->midiRPN = (unsigned short)(((c->midiRPN == 0xFFFF ? 0 : c->midiRPN) & 0x3F80) |  control_value); return 1;
		case  98 /*NRPN_LSB*/        : c->midiRPN = 0xFFFF; return 1;
		case  99 /*NRPN_MSB*/        : c->midiRPN = 0xFFFF; return 1;
		case  64 /*SUSTAIN*/         : tsf_channel_set_sustain(f, channel, (int)(control_value >= 64)); return 1;
		case  91 /*REVERB_SEND*/     : tsf_channel_set_reverb(f, channel, control_value / 127.0f); return 1;
		case  93 /*CHORUS_SEND*/     : tsf_channel_set_chorus(f, channel, control_value / 127.0f); return 1;
		case 120 /*ALL_SOUND_OFF*/   : tsf_channel_sounds_off_all(f, channel); return 1;
		case 123 /*ALL_NOTES_OFF*/   : tsf_channel_note_off_all(f, channel);   return 1;
		case 121 /*ALL_CTRL_OFF*/    :
			c->midiVolume = c->midiExpression = 16383;
			c->midiPan = 8192;
			c->bank = 0;
			c->midiRPN = 0xFFFF;
			c->midiData = 0;
			tsf_channel_set_volume(f, channel, 1.0f);
			tsf_channel_set_pan(f, channel, 0.5f);
			tsf_channel_set_pitchrange(f, channel, 2.0f);
			tsf_channel_set_tuning(f, channel, 0);
			return 1;
	}
	return 1;
TCMC_SET_VOLUME:
	//Raising to the power of 3 seems to result in a decent sounding volume curve for MIDI
	tsf_channel_set_volume(f, channel, TSF_POWF((c->midiVolume / 16383.0f) * (c->midiExpression / 16383.0f), 3.0f));
	return 1;
TCMC_SET_PAN:
	tsf_channel_set_pan(f, channel, c->midiPan / 16383.0f);
	return 1;
TCMC_SET_DATA:
	if      (c->midiRPN == 0) tsf_channel_set_pitchrange(f, channel, (c->midiData >> 7) + 0.01f * (c->midiData & 0x7F));
	else if (c->midiRPN == 1) tsf_channel_set_tuning(f, channel, (int)c->tuning + ((float)c->midiData - 8192.0f) / 8192.0f); //fine tune
	else if (c->midiRPN == 2 && controller == 6) tsf_channel_set_tuning(f, channel, ((float)control_value - 64.0f) + (c->tuning - (int)c->tuning)); //coarse tune
	return 1;
}

TSFDEF int tsf_channel_get_preset_index(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].presetIndex : 0);
}

TSFDEF int tsf_channel_get_preset_bank(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? (f->channels->channels[channel].bank & 0x7FFF) : 0);
}

TSFDEF int tsf_channel_get_preset_number(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->presets[f->channels->channels[channel].presetIndex].preset : 0);
}

TSFDEF float tsf_channel_get_pan(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].panOffset - 0.5f : 0.5f);
}

TSFDEF float tsf_channel_get_volume(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? tsf_decibelsToGain(f->channels->channels[channel].gainDB) : 1.0f);
}

TSFDEF int tsf_channel_get_pitchwheel(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].pitchWheel : 8192);
}

TSFDEF float tsf_channel_get_pitchrange(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].pitchRange : 2.0f);
}

TSFDEF float tsf_channel_get_tuning(tsf* f, int channel)
{
	return (f->channels && channel < f->channels->channelNum ? f->channels->channels[channel].tuning : 0.0f);
}

TSFDEF int tsf_channel_set_reverb(tsf* f, int channel, float send)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (send < 0.0f) send = 0.0f; else if (send > 1.0f) send = 1.0f;
	c->reverbSend = send;
	return 1;
}

TSFDEF float tsf_channel_get_reverb(tsf* f, int channel)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	return c ? c->reverbSend : 0.0f;
}

TSFDEF int tsf_channel_set_chorus(tsf* f, int channel, float send)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (send < 0.0f) send = 0.0f; else if (send > 1.0f) send = 1.0f;
	c->chorusSend = send;
	return 1;
}

TSFDEF float tsf_channel_get_chorus(tsf* f, int channel)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	return c ? c->chorusSend : 0.0f;
}

TSFDEF int tsf_channel_set_pressure(tsf* f, int channel, int pressure)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	if (!c) return 0;
	if (pressure < 0) pressure = 0; else if (pressure > 127) pressure = 127;
	c->chanPressure = (unsigned char)pressure;
	return 1;
}

TSFDEF int tsf_channel_get_pressure(tsf* f, int channel)
{
	struct tsf_channel* c = tsf_channel_init(f, channel);
	return (c ? (int)c->chanPressure : 0);
}

#ifdef __cplusplus
}
#endif

#endif //TSF_IMPLEMENTATION
