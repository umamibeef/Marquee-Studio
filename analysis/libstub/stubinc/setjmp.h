#ifndef STUB_SETJMP_H
#define STUB_SETJMP_H
typedef long jmp_buf[64];
int setjmp(); void longjmp();
#endif
