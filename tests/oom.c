#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static long failAt = -1, counter = 0;
static void* my_malloc(size_t n){ if (failAt >= 0 && counter++ == failAt) return 0; return malloc(n); }
static void* my_realloc(void* p, size_t n){ if (failAt >= 0 && counter++ == failAt) return 0; return realloc(p, n); }
#define TSF_MALLOC my_malloc
#define TSF_REALLOC my_realloc
#define TSF_FREE free
#define TSF_IMPLEMENTATION
#include "tsf.h"
int main(int argc, char** argv)
{
	FILE* fp = fopen(argv[1],"rb"); long sz; unsigned char* m; long n; int fails = 0, oks = 0;
	fseek(fp,0,SEEK_END); sz = ftell(fp); fseek(fp,0,SEEK_SET); m = malloc(sz); if (fread(m,1,sz,fp)!=(size_t)sz) return 2; fclose(fp);
	for (n = 0; n < 200; n++)
	{
		tsf* f; failAt = n; counter = 0;
		f = tsf_load_memory(m, (int)sz);
		if (!f) fails++; else {
			float b[64*2]; oks++;
			tsf_set_max_voices(f, 4);
			tsf_channel_note_on(f, 3, 60, 1.0f);
			tsf_note_on(f, 0, 60, 1.0f); tsf_note_on(f, 0, 62, 1.0f); tsf_note_on(f, 0, 64, 1.0f);
			tsf_render_float(f, b, 64, 0); tsf_close(f);
		}
	}
	failAt = -1;
	printf("allocation-failure injection: %d loads failed cleanly, %d loads succeeded (alloc index beyond load)\n", fails, oks);
	free(m); return 0;
}
