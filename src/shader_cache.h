/* Disk cache of linked GL programs (src/shader_cache.c). */
#ifndef CJ_SHADER_CACHE_H
#define CJ_SHADER_CACHE_H
#include <stdint.h>

void shc_init(const char *vendor, const char *renderer, const char *version);   /* after the GL context exists */
int shc_on(void);
uint64_t shc_hash(const char *a, const char *b);   /* program key: vertex source + fragment source */
int shc_load(uint64_t key, unsigned prog);         /* 1: prog is linked from the cache */
void shc_prepare(unsigned prog);                   /* before glLinkProgram */
void shc_store(uint64_t key, unsigned prog);       /* after a successful link */
void shc_store_later(uint64_t key, unsigned prog); /* in play: stored ~2 s later by shc_tick */
void shc_forget(unsigned prog);                    /* a program is being deleted */
void shc_tick(void);                               /* once per frame */
int shc_vs_known(uint64_t vs_src_hash);            /* this vertex shader compiled before on this driver */
void shc_vs_note(uint64_t vs_src_hash);
void shc_stats(char *out, int n);

#endif
