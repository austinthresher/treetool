#ifndef NYX_EXCEPTION_H
#define NYX_EXCEPTION_H

#include <setjmp.h>
#include <stdbool.h>

struct stackframe;

enum errcode {
#define X(a, b) ERR_##a,
#include "errors.h"
};

#define try() (except_try(__FILE__, __LINE__) && setjmp(*get_jmp()) == 0) 
#define raise(type, msg) except_raise((type), (msg), __FILE__, __LINE__)
#define catch(type) except_catch(type)
#define finally() except_finally()

bool except_try(const char *file, const int line);
void except_raise(enum errcode type, const char *msg, const char *file, const int line);
bool except_catch(enum errcode type);
void except_finally();
jmp_buf *get_jmp();
const char *get_error();


#endif /* NYX_EXCEPTION_H */
