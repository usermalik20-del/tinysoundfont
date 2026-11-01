#include <stdio.h>
#include <stdlib.h>
static long failAt = -1, counter = 0;
static void* my_malloc(size_t n){ if (failAt >= 0 && counter++ == failAt) return 0; return malloc(n); }
static void* my_realloc(void* p, size_t n){ if (failAt >= 0 && counter++ == failAt) return 0; return realloc(p, n); }
#define TML_MALLOC my_malloc
#define TML_REALLOC my_realloc
#define TML_FREE free
#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"
int main(int argc, char** argv)
{ FILE* fp = fopen(argv[1],"rb"); long sz, n; unsigned char* m; int ok = 0, bad = 0;
  fseek(fp,0,SEEK_END); sz = ftell(fp); fseek(fp,0,SEEK_SET); m = malloc(sz); if (fread(m,1,sz,fp)!=(size_t)sz) return 2; fclose(fp);
  for (n = 0; n < 60; n++) { tml_message* t; failAt = n; counter = 0; t = tml_load_memory(m, (int)sz);
    if (t) { tml_message* c = t; int k = 0; for (; c; c = c->next) k++; ok++; tml_free(t); } else bad++; }
  failAt = -1; printf("tml allocation-failure injection: %d returned NULL cleanly, %d parsed (possibly partial)\n", bad, ok); free(m); return 0; }
