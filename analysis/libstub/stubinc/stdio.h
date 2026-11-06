/* Parse-only stub libc: just enough declarations for clang to build an AST of libpng/zlib. */
#ifndef STUB_STDIO_H
#define STUB_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct _stub_file FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
FILE *fopen(); FILE *fdopen(); int fclose(); size_t fread(); size_t fwrite(); int fflush();
int fprintf(); int printf(); int sprintf(); int vsprintf(); int fputs(); int fputc(); int fgetc(); int getc();
int putc(); int ungetc(); long ftell(); int fseek(); int ferror(); int feof(); void perror(); int puts();
int fileno(); void rewind(); char *fgets(); int sscanf(); int getchar(); int putchar();
#endif
