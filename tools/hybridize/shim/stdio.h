#pragma once
typedef unsigned int size_t;
typedef struct _iobuf FILE;
extern "C" { int sprintf(char *, const char *, ...); int printf(const char *, ...); int _snprintf(char *, size_t, const char *, ...);
int vsprintf(char *, const char *, char *); int sscanf(const char *, const char *, ...); }
