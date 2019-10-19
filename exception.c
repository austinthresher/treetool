#include "exception.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_ERROR_LEN 256

const char *errstrings[] = {
#define X(a, b) b,
#include "errors.h"
};

/* TODO: die should be a function pointer set at program start */

struct stackframe {
	jmp_buf            jmp;
	struct stackframe *prev;
	int                line;
	char              *file;
	struct {
		enum errcode val;
		char         msg[MAX_ERROR_LEN];
		int          line;
		char        *file;
	} error;
};

struct stackframe *context;

bool except_try(const char *file, const int line)
{
	struct stackframe *frame = malloc(sizeof(*frame));
	memset(frame, 0, sizeof(*frame));
	memcpy(&frame->file, (const void*)&file, sizeof(file));
	memcpy(&frame->line, (const void*)&line, sizeof(line));
	frame->prev = context;
	context = frame;
	return true;
	/* setjmp is called in the try() macro that calls this function */
}

void except_raise(enum errcode type, const char *msg, const char *file, const int line)
{
	const char *errstr = errstrings[type];
	size_t errlen = strlen(errstr);
	size_t msglen = strlen(msg);
	if (msglen > MAX_ERROR_LEN - 1 - errlen)
		msglen = MAX_ERROR_LEN - 1 - errlen;
	if (context != NULL) {
		memcpy(&context->error.file, &file, sizeof(file));
		memcpy(&context->error.line, &line, sizeof(line));
		memcpy(&context->error.msg, errstr, errlen);
		memcpy(&context->error.msg[errlen], msg, msglen);
		context->error.msg[errlen+msglen] = '\0';
		context->error.val = type;
	} else {	
		char err[MAX_ERROR_LEN];
		sprintf(err, "Exception raised outside of try block: "
				"%s%s\n", errstrings[type], msg);
		die(err);
	}

	longjmp(context->jmp, type);
}

bool catch(enum errcode type)
{
	if (context != NULL && context->error.val == type) {
		context->error.val = 0;
		return true;
	}
	return false;
}

void finally()
{
	struct stackframe *leaving = context;
	if (context == NULL) {
		die("finally() called without matching try()");
	} else if (context->error.val != 0) {
		char err[MAX_ERROR_LEN];
		sprintf(err, "<UNCAUGHT EXCEPTION at %s:%d>\n"
				"<try() at %s:%d>\n"
				"%s",
				context->error.file,
				context->error.line,
				context->file,
				context->line,
				context->error.msg);
		die(err);
	}
	context = context->prev;
	free(leaving);
}

jmp_buf *get_jmp()
{
	return &context->jmp;
}

const char *get_error()
{
	if (context == NULL)
		return "";
	return &context->error.msg[0];
}
