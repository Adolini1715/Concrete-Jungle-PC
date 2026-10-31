/* hybridize parse shim (MSVC x86 declarations only; never compiled for the game) */
#pragma once
typedef unsigned int size_t;
extern "C" {
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t); int memcmp(const void *, const void *, size_t);
void *memmove(void *, const void *, size_t); size_t strlen(const char *); char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t); char *strcat(char *, const char *); int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t); int _stricmp(const char *, const char *); int _strnicmp(const char *, const char *, size_t);
char *strchr(const char *, int); char *strrchr(const char *, int); char *strstr(const char *, const char *);
char *_strupr(char *); char *_strlwr(char *); int stricmp(const char *, const char *);
}
