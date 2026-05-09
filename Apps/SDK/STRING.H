/* string.h - BDS-style drop-in (K&R) */
#ifndef BDSSTR
#define BDSSTR

char *memcpy(dest, src, n);
char *memmove(dest, src, n);
char *memset(s, c, n);
int memcmp(s1, s2, n);
char *memchr(s, c, n);
unsigned strlen(s);
char *strcpy(dest, src);
char *strncpy(dest, src, n);
char *strcat(dest, src);
char *strncat(dest, src, n);
int strcmp(s1, s2);
int strncmp(s1, s2, n);
char *strchr(s, c);
char *strrchr(s, c);
char *strstr(hay, needle);

#endif