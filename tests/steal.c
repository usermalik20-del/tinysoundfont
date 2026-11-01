#include <stdio.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
int main(int c, char** v){ tsf* f = tsf_load_filename(v[1]); int i, k, found = 0; float b[2*256];
 tsf_set_max_voices(f, 4);
 for (i = 0; i < 10; i++){ tsf_note_on(f, 0, 50+i, 1.0f); tsf_render_float(f, b, 256, 0); }
 printf("active voices=%d (limit 4)\n", tsf_active_voice_count(f)); 
 tsf_note_on(f, 0, 80, 1.0f); { int has = 0; for (i = 0; i < f->voiceNum; i++) if (f->voices[i].playingPreset != -1 && f->voices[i].playingKey == 80) has = 1; printf("newest note (key 80) got a voice: %s\n", has ? "yes" : "NO (dropped)"); } tsf_render_float(f, b, 256, 0);
 for (k = 0; k < 512; k++) if (b[k] != 0) found = 1;
 printf("audio from newest note: %s\n", found ? "yes" : "NO");
 tsf_close(f); return 0; }
