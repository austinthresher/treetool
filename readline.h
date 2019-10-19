#ifndef TT_READLINE_H
#define TT_READLINE_H

#include <ncurses.h>

/* maximum input size */
#define MAXLEN 256

/* opaque struct representing a line being read */
struct rlstate;

/* allocate and init rlstate structure, make cur visible, enable keypad */
struct rlstate *rl_start(WINDOW *w);

/* set the line's contents to the given string */
void rl_set(struct rlstate *rl, const char *str);

/* draw the current state of the readline */
void rl_draw(struct rlstate *rl);

/* read in one character and perform an appropriate action */
int rl_read(struct rlstate *rl);

/* deallocate the readline and return the entered string */
char *rl_finish(struct rlstate *rl);

#endif /* TT_READLINE_H */
