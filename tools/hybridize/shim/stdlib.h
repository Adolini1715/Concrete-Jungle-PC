#pragma once
typedef unsigned int size_t;
extern "C" { int abs(int); int atoi(const char *); double atof(const char *); int rand(void); void srand(unsigned); void *malloc(size_t); void free(void *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *)); long strtol(const char *, char **, int); }
