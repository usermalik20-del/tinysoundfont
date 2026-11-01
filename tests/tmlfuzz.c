#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"
static unsigned long long rng = 1234567ULL;
static unsigned rnd(void){ rng ^= rng<<13; rng ^= rng>>7; rng ^= rng<<17; return (unsigned)(rng>>11); }
static unsigned long long walk(tml_message* m, int* count){ unsigned long long h = 1469598103934665603ULL; *count = 0;
 for (; m; m = m->next) { unsigned v[5] = { m->time, m->type, m->channel, m->key, m->velocity }; int i; for (i = 0; i < 5; i++) { h ^= v[i]; h *= 1099511628211ULL; } if (++*count > 5000000) break; } return h; }
int main(int argc, char** argv)
{
	FILE* fp = fopen(argv[1], "rb"); long sz; unsigned char *orig, *m; int iters = atoi(argv[2]), it, loaded = 0, cnt; tml_message* t;
	fseek(fp,0,SEEK_END); sz = ftell(fp); fseek(fp,0,SEEK_SET); orig = malloc(sz); m = malloc(sz); if (fread(orig,1,sz,fp)!=(size_t)sz) return 2; fclose(fp);
	t = tml_load_memory(orig, (int)sz); printf("valid file: messages hash %016llx", walk(t, &cnt)); printf(" count %d\n", cnt); tml_free(t);
	for (it = 0; it < iters; it++)
	{
		long len = sz; int muts = 1 + rnd()%8, j;
		memcpy(m, orig, sz);
		for (j = 0; j < muts; j++) { long pos = (rnd()%3==0) ? rnd()%64 : rnd()%sz;
			switch (rnd()%4) { case 0: m[pos] ^= 1<<(rnd()%8); break; case 1: m[pos] = (unsigned char)rnd(); break;
				case 2: if (pos+4<=sz) { unsigned v = (rnd()%2)?0xFFFFFFFFu:rnd(); memcpy(m+pos,&v,4);} break; case 3: len = pos > 4 ? pos : 4; break; } }
		t = tml_load_memory(m, (int)len); if (t) { loaded++; walk(t, &cnt); tml_free(t); }
	}
	printf("%d/%d mutated MIDI files parsed, no crash\n", loaded, iters); return 0;
}
