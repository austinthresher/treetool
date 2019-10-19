#include <stdlib.h>
#include <string.h>

#include "readline.h"

/* Structure to represent the state of a readline in progress */
struct rlstate {
	char buf[MAXLEN];
	int len;     /* total number of chars typed so far */
	int cur;     /* horizontal cursor position */
	int scr;     /* horizontal scroll position */
	WINDOW *win; /* curses win to draw in */
	char *msg;   /* if != null, most recent error or info string */
	enum {
		INSERT,
		REPLACE
	} mode;
	char cut[MAXLEN];
	int cutlen;
};

/* static prototypes */
static void left(struct rlstate *rl);
static void right(struct rlstate *rl);
static void del(struct rlstate *rl);
static void bksp(struct rlstate *rl);
static void insert(struct rlstate *rl, int c);
static void replace(struct rlstate *rl, int c);
static void type(struct rlstate *rl, int c);
static void cutb(struct rlstate *rl);
static void cutf(struct rlstate *rl);
static void home(struct rlstate *rl);
static void end(struct rlstate *rl);
static void cls(struct rlstate *rl);
static void invalid(struct rlstate *rl);
static void wordb(struct rlstate *rl);
static void wordf(struct rlstate *rl);
static void paste(struct rlstate *rl);

/* allocate and init rlstate structure, make cur visible, enable keypad */
struct rlstate *rl_start(WINDOW *w)
{
	struct rlstate *rl = malloc(sizeof(*rl));
	if (rl == NULL)
		return NULL;

	memset(rl, 0, sizeof(*rl));
	rl->win = w;

	curs_set(1);
	keypad(rl->win, TRUE);

	return rl;
}

/* Set the line's contents (used for editing existing lines) */
void rl_set(struct rlstate *rl, const char *str)
{
	rl->len = strlen(str);
	if (rl->len > MAXLEN)
		rl->len = MAXLEN;
	memset(rl->buf, 0, sizeof(rl->buf));
	memcpy(rl->buf, str, rl->len);
	rl->cur = 0;
	rl->scr = 0;
}

/* draw the current state of the readline */
void rl_draw(struct rlstate *rl)
{
	int w, h;
	getmaxyx(rl->win, h, w);
	wmove(rl->win, 0, 0);
	wclrtoeol(rl->win);
	if (rl->msg != NULL) {
		curs_set(0);
		waddnstr(rl->win, rl->msg, w);
	} else {
		curs_set(1);
		waddnstr(rl->win, &rl->buf[rl->scr], w);
		wmove(rl->win, 0, rl->cur - rl->scr);
	}
}

/* read in one character and perform an appropriate action */
int rl_read(struct rlstate *rl)
{
	int c = '\0';
	c = wgetch(rl->win);
	if (rl->msg != NULL) {
		rl->msg = NULL;
		return '\0';
	}
	switch (c) {
	/* Silently do nothing so that the calling program can respond */
	case 0x1F: /* C-? */
	case '\t': break;
	/* Intercept these keys so they do nothing */
	case KEY_NPAGE:
	case KEY_PPAGE:
	case KEY_UP:
	case KEY_DOWN: invalid(rl); break;
	/* Newline / Carriage return (Complete entry) */
	case '\n':
	case '\r': break;
	/* C-c (Cancel Input) */
	case 0x03: cls(rl); return '\n';
	/* C-k (Cut to end of line) */
	case 0x0B: cutf(rl); break;
	/* C-u (Cut to beginning of line) */
	case 0x15: cutb(rl); break;
	/* C-w (Cut previous word) */
	case 0x17: wordb(rl); break;
	/* C-x (Cut next word) */
	case 0x18: wordf(rl); break;
	/* C-v / C-y (Paste) */
	case 0x16:
	case 0x19: paste(rl); break;
	/* C-b (Left) */
	case 0x02: 
	case KEY_LEFT: left(rl); break;
	/* C-f (Right) */
	case 0x06: 
	case KEY_RIGHT: right(rl); break;
	/* C-a / Home*/
	case 0x01: 
	case KEY_HOME: home(rl); break;
	/* C-e (End) */
	case 0x05: 
	case KEY_END: end(rl); break;
	/* C-h (Backspace) */
	case 0x08: 
	case 0x7F:
	case KEY_BACKSPACE: bksp(rl); break;
	/* C-d Delete */
	case 0x04:
	case KEY_DC: del(rl); break;
	/* insert ASCII char */
	default: type(rl, c); break;
	}
	return c;
}

/* deallocate the readline and return the entered string */
char *rl_finish(struct rlstate *rl)
{
	char *retstr;

	rl->buf[rl->len] = '\0';
	retstr = malloc(rl->len+1);
	memcpy(retstr, rl->buf, rl->len+1);
	retstr[rl->len] = '\0';

	curs_set(0);

	free(rl);
	return retstr;
}

/* move cursor left one character and scroll if necessary */
static void left(struct rlstate *rl)
{
	rl->cur--;
	if (rl->cur < 0)
		rl->cur = 0;
	if (rl->cur < rl->scr)
		rl->scr = rl->cur;
}

/* move cursor right one character and scroll if necessary */
void right(struct rlstate *rl)
{
	int w, h;
	getmaxyx(rl->win, h, w);
	rl->cur++;
	if (rl->cur > rl->len)
		rl->cur = rl->len;
	if (rl->cur - rl->scr >= w-1)
		rl->scr = rl->cur - (w-1);
}

/* delete the character under the cursor */
void del(struct rlstate *rl)
{
	if (rl->len <= 0 || rl->cur == rl->len)
		return;
	memmove(&rl->buf[rl->cur],
			&rl->buf[rl->cur+1],
			rl->len - rl->cur);
	rl->len--;
	memset(&rl->buf[rl->len], 0, MAXLEN - rl->len);
}

/* moves the cursor back and deletes that character */
void bksp(struct rlstate *rl)
{
	if (rl->cur <= 0)
		return;
	left(rl);
	del(rl);
}

/* insert character c and shift all chars after the cursor right by one */
void insert(struct rlstate *rl, int c)
{
	if (rl->cur != rl->len) {
		memmove(&rl->buf[rl->cur + 1],
				&rl->buf[rl->cur],
				rl->len - rl->cur + 1);
	}
	rl->buf[rl->cur] = c & 0x7F;
	rl->buf[++rl->len] = '\0';
}

/* replace the character under the cursor with c */
void replace(struct rlstate *rl, int c)
{
	del(rl);
	insert(rl, c);
}

/* types one character using the current insert mode */
void type(struct rlstate *rl, int c)
{
	if (c < ' ' || c > '~') {
		invalid(rl);
		return;
	}
	if (rl->mode == REPLACE) {
		replace(rl, c);
		right(rl);
	} else if (rl->len < MAXLEN) {
		insert(rl, c);
		right(rl);
	} else {
		rl->msg = "> Input limit exceeded.";
	}
}

/* cuts from the cursor to the previous whitespace */
void wordb(struct rlstate *rl)
{
	int dst = rl->cur - 1;
	/* first skip any whitespace we're on */
	while (dst > 0 && rl->buf[dst] == ' ') {
		dst--;
	}
	/* now cut until the beginning of the word we found */
	while (dst > 0 && rl->buf[dst-1] != ' ') {
		dst--;
	}
	if (dst != rl->cur - 1)
		rl->cutlen = 0;
	
	while (rl->cur > dst && rl->cur > 0) {
		rl->cut[rl->cutlen++] = rl->buf[rl->cur - 1];
		bksp(rl);
	}
	rl->cut[rl->cutlen] = '\0';
}

/* cuts from the cursor to the next whitespace */
void wordf(struct rlstate *rl)
{
	int dst = rl->cur + 1;
	/* first skip any whitespace we're on */
	while (dst < rl->len && rl->buf[dst] == ' ') {
		dst++;
	}
	/* now cut until the beginning of the word we found */
	while (dst < rl->len && rl->buf[dst+1] != ' ') {
		dst++;
	}

	if (dst != rl->cur + 1)
		rl->cutlen = 0;

	while (rl->cur <= dst) {
		rl->cut[rl->cutlen++] = rl->buf[rl->cur];
		del(rl);
		dst--;
	}
	rl->cut[rl->cutlen] = '\0';
}

/* cut (back) from the cursor to the start of the line */
void cutb(struct rlstate *rl)
{
	if (rl->cur > 0) {
		memcpy(rl->cut, rl->buf, rl->cur+1);
		rl->cutlen = rl->cur;
		rl->cut[rl->cutlen] = '\0';
	}

	memmove(rl->buf, &rl->buf[rl->cur], rl->len - rl->cur);
	rl->len -= rl->cur;
	memset(&rl->buf[rl->len], 0, MAXLEN - rl->len);
	rl->cur = 0;
	left(rl); /* scroll if needed */
}

/* cut (forward) from the cursor to the end of the line */
void cutf(struct rlstate *rl)
{
	if (rl->cur < rl->len) {
		memcpy(rl->cut, &rl->buf[rl->cur], rl->len-rl->cur+1);
		rl->cutlen = rl->len - rl->cur;
		rl->cut[rl->cutlen] = '\0';
	}
	rl->len = rl->cur;
	memset(&rl->buf[rl->cur], 0, MAXLEN - rl->cur);
	right(rl); /* scroll if needed */
}

/* move the cursor all the way left */
void home(struct rlstate *rl)
{
	rl->cur = 0;
	left(rl);
}

/* move the cursor all the way right */
void end(struct rlstate *rl)
{
	rl->cur = rl->len;
	right(rl);
}

/* clear the entered string */
void cls(struct rlstate *rl)
{
	rl->buf[0] = '\0';
	rl->len = 0;
}

/* set the error message for an illegal entry */
void invalid(struct rlstate *rl)
{
	rl->msg = "> Invalid input.";
}

void paste(struct rlstate *rl)
{
	int i = 0;
	if (rl->cutlen <= 0) {
		rl->msg = "> Clipboard empty.";
		return;
	}

	for (i = rl->cutlen-1; i >= 0; i--) {
		insert(rl, rl->cut[i]);
	}
}
