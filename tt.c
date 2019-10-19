#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <stdbool.h>
#include <setjmp.h>
#include <errno.h>

#include "exception.h"
#include "readline.h"

/******************************************************************************
TODO:
	Add means to view entries that don't fit onscreen
	Handle horizontal scrolling
	Show filename in "say" messages
	Listen for resize signal
	Handle Ctrl+Z signal
	Alphabetize functions
	Check Delete edge cases for crashes
	Check for memory leaks with valgrind
	Tab complete for filenames
*/

/* Program info displayed in status bar */
#define PROGRAM "tree tool"
#define VERSION "v0.2"

/* Size in rows of footer sections */
#define HELP_SIZE 2
#define STATUS_SIZE 1

/* Set to 0 to hide help on startup*/
#define SHOW_HELP_DEFAULT 0

#define MAX_ENTRY_LEN 256

/* Duration of blink in ms */
#define SAY_DURATION 96 
#define SAY_BLINKS 2
#define MAX_SAY_CHARS 40

enum fold_state {
	EMPTY,
	EXPANDED,
	COLLAPSED
};

struct tree {
	int nchild;
	int nalloc;
	struct tree *parent;
	struct tree **child;
	struct tree *sibling; /* TODO: use a linked list instead of array for child nodes */
	enum fold_state state;
	char* text;
};

/* function prototypes */
bool confirm(const char *question);
bool modified_warning();
char *prompt(const char *msgstr, const char *defstr);
int main(int argc, char *argv[]);
struct tree *add_child(struct tree *parent, char* text);
struct tree *add_leaf(struct tree *parent, struct tree *child);
struct tree *del_child(struct tree *child);
struct tree *find_root(struct tree *leaf);
struct tree *read_tree(FILE *f, char delim, int depth);
void delete();
void demote();
void die(const char *error);
void draw_info(int y, int x, const char *key, const char *label);
void edit_entry();
void free_tree(struct tree *t);
void help_normal();
void help_edit();
void init_curses();
void insert_entry();
bool load(const char *fname);
void menu();
void print_tree(struct tree *tree, int depth);
void promote();
void redraw();
void resize();
void save();
void saveas(const char *fname);
void say(const char *str);
void select_down();
void select_up();
void set_fold(enum fold_state f);
WINDOW *set_window(WINDOW *win, int h, int w, int y, int x);
void shove_down();
void shove_up();
void status();
void write_tree(struct tree *t, FILE *f, int depth);

/******************************************************************************
   Globals
*/

/* curses windows for each UI section */
WINDOW *tree_window;
WINDOW *status_window;
WINDOW *help_window;

int tree_win_height;
int status_win_height;
int help_win_height;
int input_win_height;

int screenh, screenw;
int vscroll;
int printed_lines;
int selected_index;

enum {
	H_HIDE,
	H_NORMAL,
	H_EDIT
} help_mode;

struct tree **onscreen_entries;
struct tree *selected_entry;
struct tree *root;

unsigned char int_size = sizeof(int);

char filename[MAX_ENTRY_LEN];
bool modified;

char saymsg[MAX_SAY_CHARS];
int sayblink;

/******************************************************************************
	Allocate a new node, add it to the parent's list of children,
	and set its contents to "text"
*/
struct tree *add_child(struct tree *parent, char* text)
{
	struct tree *tree = NULL;
	struct tree *child = malloc(sizeof(*child));
	int len = strlen(text)+1;
 	child->nchild = child->nalloc = 0;
	child->text = malloc(len);
	memcpy(child->text, text, len);
	child->text[len-1] = '\0';
	child->state = EMPTY;
	return add_leaf(parent, child);
}

/******************************************************************************
	Places the given node into the parent node's list of
	children, allocating space in the parent's list if needed
*/
struct tree *add_leaf(struct tree *parent, struct tree *child)
{
	child->parent = NULL;
	if (parent == NULL) {
		return child;
	}
	if (parent->nalloc == 0) {
		parent->child = malloc(sizeof(parent->child));
		parent->nalloc = parent->nchild = 1;
		parent->child[0] = child;
		parent->state = EXPANDED;
		child->parent = parent;
		return child;
	}
	if (parent->nchild < parent->nalloc) {
		parent->child[parent->nchild++] = child;
		parent->state = EXPANDED;
		child->parent = parent;
		return child;
	}
	parent->nalloc *= 2;
	parent->child = realloc(parent->child,
			sizeof(parent->child) * parent->nalloc);
	parent->child[parent->nchild++] = child;
	parent->state = EXPANDED;
	child->parent = parent;
	return child;
}

/******************************************************************************
	Remove a child from its parent and return a pointer
	to the removed node. Returns NULL if node was not found.
*/
struct tree *del_child(struct tree *child)
{
	int i, j;
	struct tree *tree = NULL;
	if (child == NULL)
		return NULL;
	tree = child->parent;
	if (tree == NULL)
		return NULL;
	for (i = 0; i < tree->nchild; i++) {
		if (tree->child[i] == child) {
			for (j = i; j < tree->nchild-1; j++) {
				tree->child[j] = tree->child[j+1];
			}
			tree->nchild--;
			child->parent = NULL;
			return child;
		}
	}
	return NULL;
}

/******************************************************************************
	Release the memory used by a tree and its children
*/
void free_tree(struct tree *t)
{
	int i;
	for (i = 0; i < t->nchild; i++) {
		free_tree(t->child[i]);
	}
	free(t->text);
	free(t);
}

/******************************************************************************
	Print an error and quit
*/
void die(const char *error)
{
	endwin();
	fprintf(stderr, "=====================================\n");
	fprintf(stderr, "                ERROR                \n");
	fprintf(stderr, "-------------------------------------\n");
	fprintf(stderr, "%s\n", error);
	fprintf(stderr, "=====================================\n");
	exit(1);
}

/******************************************************************************
	Return the topmost node connected to leaf
*/
struct tree *find_root(struct tree *leaf)
{
	if (leaf->parent == NULL)
		return leaf;
	return find_root(leaf->parent);
}

/******************************************************************************
	Set the window's position and size, reallocating if necessary
*/
WINDOW *set_window(WINDOW *win, int h, int w, int y, int x)
{
	int wy, wx, ww, wh;
	if (win != NULL) {
		getbegyx(win, wy, wx);
		getmaxyx(win, wh, ww);
		if (ww == w && wh == h && wx == x && wy == y)
			return win;
		if (ww == w && wh == h && (wx != x || wy != y)) {
			mvwin(win, y, x);
			return win;
		}
		delwin(win);		
		win = NULL;
	}
	return newwin(h, w, y, x);
}

/******************************************************************************
	Query terminal for size and update windows accordingly
*/
void resize()
{
	getmaxyx(stdscr, screenh, screenw);
	status_win_height = STATUS_SIZE;
	help_win_height = help_mode == H_HIDE ? 0 : HELP_SIZE;
	tree_win_height = screenh
		- (status_win_height + help_win_height + input_win_height);
	/* keep a table of onscreen entries to map cursor row to struct ptr */
	onscreen_entries = realloc(onscreen_entries,
			sizeof(*onscreen_entries) * tree_win_height);
	/* create new tree window of correct size */
	tree_window = set_window(tree_window, tree_win_height, screenw, 0, 0);
	/* create new status window of correct size */
	status_window = set_window(status_window, status_win_height, screenw,
			screenh - help_win_height - status_win_height, 0);
	/* if visible, create new help window of correct size */
	if (help_mode != H_HIDE) {
		help_window = set_window(help_window,
				help_win_height,
				screenw,
				screenh - help_win_height, 0);
	} else {
		if (help_window != NULL) {
			delwin(help_window);
			help_window = NULL;
		}
	}
}

/******************************************************************************
	Enter curses mode
*/
void init_curses()
{
	setlocale(LC_ALL, "");
	initscr();
	noecho();
	raw();
	curs_set(0);
	resize();
}

/******************************************************************************
	Move the selected entry up
*/
void shove_up()
{
	if (selected_entry != NULL) {
		struct tree *sel = selected_entry;
		struct tree *parent = sel->parent;
		int i, a, b;

		if (parent == NULL || parent->nchild < 2)
			return;
		a = b = -1;
		for (i = 0; i < parent->nchild; i++) {
			if (parent->child[i] == sel) {
				b = (a = i) - 1;
				break;
			}
		}
		if (a >= 0 && b >= 0) {
			struct tree *swap = parent->child[b];
			parent->child[b] = sel;
			parent->child[a] = swap;
			modified = true;
		}
	}
}

/******************************************************************************
	Move the selected entry down
*/
void shove_down()
{
	if (selected_entry != NULL) {
		struct tree *sel = selected_entry;
		struct tree *parent = sel->parent;
		int i, a, b;

		if (parent == NULL || parent->nchild < 2)
			return;
		a = b = -1;
		for (i = 0; i < parent->nchild; i++) {
			if (parent->child[i] == sel) {
				b = (a = i) + 1;
				break;
			}
		}
		if (a >= 0 && b >= 0 && b < parent->nchild) {
			struct tree *swap = parent->child[b];
			parent->child[b] = sel;
			parent->child[a] = swap;
			modified = true;
		}
	}
}

/******************************************************************************
	Move the selected entry to a higher tier
*/
void promote()
{
	if (selected_entry != NULL && selected_entry->parent != NULL) {
		struct tree *sel = selected_entry;
		struct tree *parent = sel->parent;
		struct tree *new_parent = parent->parent;
		int i, shoves = 0;

		if (new_parent == NULL)
			return;
		sel = del_child(sel);
		if (sel != NULL) {
			/* Find the index of the old parent so we can position the
			   promoted entry directly above it */
			for (i = 0; i < new_parent->nchild; i++) {
				if (new_parent->child[i] == parent) {
				    shoves = new_parent->nchild - i;
				    break;
				}
			}
			new_parent = add_leaf(new_parent, sel);
			for (i = 0; i < shoves; i++) {
				shove_up(); 
			}
			modified = true;
		} else {
			die("Lost child while promoting");
		}
	}
}

/******************************************************************************
	Move the selected entry to a lower tier
*/
void demote()
{
	if (selected_entry != NULL) {
		struct tree *sel = selected_entry;
		struct tree *parent = sel->parent;
		struct tree *new_parent = NULL;
		int i;
		
		if (parent == NULL || parent->nchild < 2) { return; }
		for (i = 0; i < parent->nchild-1; i++) {
			if (parent->child[i] == sel) {
				new_parent = parent->child[i+1];
				break;
			}
		}
		/* If we're trying to demote the last child, the new parent is the
		   one before, not after */
		if (new_parent == NULL)
			new_parent=parent->child[parent->nchild-2];
		sel = del_child(sel);
		if (sel != NULL) {
			new_parent = add_leaf(new_parent, sel);
			/* Position the entry at the top */
			for (i = 0; i < new_parent->nchild-1; i++) {
				shove_up();
			}
			modified = true;
		}
	}
}

/******************************************************************************
	Move the current selection up
*/
void select_up()
{
	if (selected_entry == NULL || selected_index <= 0) {
		selected_entry = onscreen_entries[0];
		if (vscroll > 0)
			vscroll--;
	}
	else 
		selected_entry = onscreen_entries[selected_index-1];
}

/******************************************************************************
	Move the current selection down
*/
void select_down()
{
	if (selected_entry == NULL || selected_index >= printed_lines-vscroll-1) {
		selected_entry = onscreen_entries[printed_lines-vscroll-1];
		if (vscroll+tree_win_height <= printed_lines)
			vscroll++;
	}
	else
		selected_entry = onscreen_entries[selected_index+1];
}

/******************************************************************************
	Expand or collapse selected entry
*/
void set_fold(enum fold_state f)
{
	if (selected_entry != NULL) {
		selected_entry->state = f;
	}

}

/******************************************************************************
	Prompt the user to input a string
*/
char *prompt(const char *msgstr, const char *defstr)
{
	WINDOW *prompt_win;
	WINDOW *input_win;
	char *str;
	int len;
	int c;
	struct rlstate *rl;

	help_mode = help_mode == H_NORMAL ? H_EDIT : H_HIDE;
	input_win_height = 2;
	resize();
	redraw();

	prompt_win = newwin(1, screenw, tree_win_height, 0);
	wbkgdset(prompt_win, A_BOLD | A_UNDERLINE);
	whline(prompt_win, ' ', screenw);
	waddstr(prompt_win, msgstr);
	wrefresh(prompt_win);

	input_win = newwin(1, screenw, tree_win_height + 1, 0);
	wmove(input_win, 0, 0);
	wrefresh(input_win);

	rl = rl_start(input_win);
	if (defstr != NULL)
		rl_set(rl, defstr);
	do  {
		if (c == 0x1F) { /* C-? */
			help_mode = help_mode == H_HIDE ? H_EDIT : H_HIDE;
			resize();
			mvwin(input_win, tree_win_height + 1, 0);
			mvwin(prompt_win, tree_win_height, 0);
			redraw();
			wnoutrefresh(tree_window);
			wnoutrefresh(status_window);
			wnoutrefresh(help_window);
			wnoutrefresh(prompt_win);
		}
		rl_draw(rl);
		wnoutrefresh(input_win);
		doupdate();
		c = rl_read(rl);
	} while (c != '\n');

	str = rl_finish(rl);
	delwin(input_win);
	delwin(prompt_win);

	help_mode = help_mode == H_EDIT ? H_NORMAL : H_HIDE;
	input_win_height = 0;
	resize();

	if (str[0] == '\0') {
		free(str);
		say("Input cancelled.");
		return NULL;
	}
	return str;
}

/******************************************************************************
	Create a new entry, prompt for its contents, add it to tree
*/
void insert_entry()
{
	char *str = prompt("New entry", NULL);
	if (str == NULL)
		return;
	if (strlen(str) > 0) {
		if (selected_entry == NULL)
			selected_entry = root;
		selected_entry = add_child(selected_entry, str);
		modified = true;
	} else {
		say("Entry cancelled.");
	}
	free(str);
}

/******************************************************************************
	Edit an existing entry
*/
void edit_entry()
{
	char *str;
	
	if (selected_entry == NULL) {
		return;
	} else if (selected_entry == root) {
		say("Cannot modify root entry.");
		return;
	}

 	str = prompt("Edit entry", selected_entry->text);
	if (str == NULL)
		return;
	if (strlen(str) > 0) {
		free(selected_entry->text);
		selected_entry->text = str;
		say("Editing complete.");
		modified = true;
	} else {
		free(str);
		say("Edit cancelled.");
	}
}


/******************************************************************************
	Recursively traverse the tree and print its contents.
	Also updates the values of onscreen_entries to simplify
	selection and cursor movement
*/
void print_tree(struct tree *tree, int depth)
{
	int i = 0;
	int col = 0;
	if (tree == NULL)
		return;
	if (tree == root) {
		printed_lines = 0;
		wmove(tree_window, 0, 0);
	}
	if (printed_lines - vscroll >= tree_win_height)
		return;
	if (tree_window == NULL)
		return;
	if (tree->nchild == 0)
		tree->state = EMPTY;

	/* only actually print if its onscreen */
	if (printed_lines - vscroll >= 0
			&& printed_lines - vscroll < tree_win_height) {
		/* indent */
		for (i = 0; i < depth; i++) {
			wprintw(tree_window, "  ");
		}
	
		switch(tree->state) {
			case EMPTY:     wprintw(tree_window, "[ ] "); break;
			case EXPANDED:  wprintw(tree_window, "[-] "); break;
			case COLLAPSED: wprintw(tree_window, "[+] "); break;
		}

		/*    indent     [ ] */
		col = depth * 2 + 4;

		/* highlight selection */
		if (selected_entry == tree)
			wattron(tree_window, A_STANDOUT);
		waddnstr(tree_window, tree->text, screenw - 3 - col);
		if (strlen(tree->text) > screenw - 3 - col) 
			waddstr(tree_window, "...");
		waddch(tree_window, '\n');
		if (selected_entry == tree)
			wattroff(tree_window, A_STANDOUT);

		onscreen_entries[printed_lines - vscroll] = tree;
	}
	printed_lines++;

	/* print child recursively */
	if (tree->state == EXPANDED) {
		for (i = 0; i  < tree->nchild; i++) {
			if (tree->child[i] != NULL)
				print_tree(tree->child[i], depth+1);
		}
	}

	/* stuff to run after the original call has finished */
	if (tree == root) {
		/* find the onscreen index of selected entry */
		for (i = 0; i < printed_lines - vscroll; i++) {
			if (onscreen_entries[i] == selected_entry) {
				selected_index = i;
				break;
			}
		}
		/* clear any empty lines below the last entry */
		for (i = printed_lines - vscroll; i < tree_win_height; i++) {
			onscreen_entries[i] = NULL;
			wclrtoeol(tree_window);
			wprintw(tree_window, "\n");
		}
	}
}

/******************************************************************************
	Write a tree recursively to file
*/
void write_tree(struct tree *t, FILE *f, int depth)
{
	int i;
	for (i = 0; i < depth; i++) {
		fprintf(f, "\t");
	}
	fprintf(f, "%s\n", t->text);
	for (i = 0; i < t->nchild; i++) {
		write_tree(t->child[i], f, depth+1);
	}
}

/******************************************************************************
	Read a tree recursively from file
*/
struct tree *read_tree(FILE *f, char delim, int indent) 
{
	char buf[MAX_ENTRY_LEN];
	int i;
	int len;
	struct tree *t;
	fpos_t fp;
	int c;
	int dcount;

	t = malloc(sizeof(*t));
	t->nchild = t->nalloc = 0;

	/* save beginnning of line */
	if (fgetpos(f, &fp) < 0 && errno != 0)
		raise(ERR_IO, strerror(errno));

	/*
		If delim is '\0', value will be set to the first whitespace
		character encountered at the beginning of a line
	*/
	if (delim == '\0') {
		c = fgetc(f);
		if (c == ' ' || c == '\t')
			delim = c;
		if (ungetc(c, f) == EOF && errno != 0)
			raise(ERR_IO, strerror(errno));
	}

	/* ensure consistent indentation */ 
	dcount = 0;
	c = fgetc(f);
	while (c == delim) {
		dcount++;
		c = fgetc(f);
	}
	if (ungetc(c, f) == EOF && errno != 0) 
		raise(ERR_IO, strerror(errno));

	if (dcount != indent)
		raise(ERR_FORMAT, "invalid indentation");
	if (fgets(buf, MAX_ENTRY_LEN, f) == NULL && errno != 0)
		raise(ERR_IO, strerror(errno));

	len = strlen(buf);
	if (buf[len-1] == '\n') {
		/* remove trailing newline */
		buf[--len] = '\0';
	} else {
		/* we didn't finish reading the line. discard the rest */
		int c = fgetc(f);
		while (c != '\n' && c != EOF) {
			c = fgetc(f);
		}
	}
	t->text = malloc(len+1);
	memcpy(t->text, buf, len+1);
	t->parent = NULL;
	t->state = COLLAPSED;

	for (;;) {
		if (fgetpos(f, &fp) < 0 && errno != 0)
			raise(ERR_IO, strerror(errno));
		dcount = 0;
		c = fgetc(f);
		while (c == delim) {
			dcount++;
			c = fgetc(f);
		}
		if (c == EOF)
			return t;
		if (fsetpos(f, &fp) < 0 && errno != 0)
			raise(ERR_IO, strerror(errno));
		if (dcount <= indent)
			return t;
		if (dcount > indent+1)
			raise(ERR_FORMAT, "invalid indentation");

		t->nalloc++;
		t->nchild++;
		t->child = realloc(t->child, sizeof(*t->child) * t->nalloc);
		if (t->child == NULL)
			raise(ERR_ALLOC, "realloc failed");
		t->child[t->nchild-1] = read_tree(f, delim, indent+1);
		t->child[t->nchild-1]->parent = t;
	}	
	return NULL; /* unreachable */
}

/******************************************************************************
	Save the current tree to the specified file
*/
void saveas(const char *fname)
{
	FILE *f;
	int i;

	if (fname == NULL || strlen(fname) == 0) {
		say("No filename given.");
		return;
	}

	if (strcmp(fname, filename) != 0) {
		/* Warn if overwriting */
		f = fopen(fname, "r");
		if (f) {
			fclose(f);
			if (!confirm("File exists, overwrite? (y/n)")) {
				say("Save cancelled.");
				return;
			}
		}
	}
	f = fopen(fname, "w");

	if (!f) {
		say("Error opening file.");
		return;
	}
	
	for (i = 0; i < root->nchild; i++) {
		write_tree(root->child[i], f, 0);
	}
	fclose(f);

	strcpy(filename, fname);
	modified = false;

	say("Saved.");
}

/******************************************************************************
	If a working file exists, save to it. Otherwise prompt for a filename
*/
void save()
{
	if (strlen(filename) == 0) {
		saveas(prompt("Save as...", NULL));
		return;
	}
	saveas(filename);	
}

/******************************************************************************
	Discard current tree and load a new one from file
*/
bool load(const char *fname)
{
	bool success = false;
	FILE *f = NULL;

	if (strlen(fname) == 0) {
		say("No filename given.");
		return false;
	}

	if (modified_warning()) {
		if (try()) {
			f = fopen(fname, "r");

			if (!f) {
				raise(ERR_FILENOTFOUND, fname); 
			}
			if (root != NULL) {
				free_tree(root);
			}
			root = add_child(NULL, "Entries");
			selected_entry = root;

			while (!feof(f)) {
				add_leaf(root, read_tree(f, '\0', 0));
			}
			fclose(f);
			strcpy(filename, fname);
			modified = false;
			success = true;
		} else { /* error handling */
			if (catch(ERR_FILENOTFOUND)
	/*			|| catch(ERR_IO) */
				|| catch(ERR_FORMAT)) {
				say(get_error());
				if (f != NULL) {
					fclose(f);
				}
				/* discard the partial load */
				if (root != NULL) {
					free_tree(root);
					root = add_child(NULL, "Entries");
				}
			}
		}
		finally();
	} else if (sayblink == 0) {
		say("Cancelled.");
		return false;
	}
	return success;
}

/******************************************************************************
	Prints a key / description pair for the help screen
*/
void draw_info(int y, int x, const char *key, const char *label)
{
	wmove(help_window, y, x);
	wattron(help_window, A_REVERSE | A_BOLD);
	waddstr(help_window, key);
	wattroff(help_window, A_REVERSE | A_BOLD);
	waddstr(help_window, " ");
	waddstr(help_window, label);
}

/******************************************************************************
	Draw the status bar
*/
void status()
{
	int plen = strlen(PROGRAM " " VERSION);
	int i;
	int flen;
	wmove(status_window, 0, 0);
	wattron(status_window, A_REVERSE);
	for (i = 0; i < screenw; i++) {
		waddstr(status_window, " ");
	}
	wmove(status_window, 0, 0);
	wattron(status_window, A_BOLD);
	if (strlen(filename) == 0) {
		waddstr(status_window, "[Untitled]");
		flen = 10;         
	} else {
		waddstr(status_window, filename);
		flen = strlen(filename);
	}
	if (modified) {
		wmove(status_window, 0, flen);
		waddstr(status_window, "*");
		flen++;
	}
	wmove(status_window, 0, screenw-plen);
	waddstr(status_window, PROGRAM " " VERSION);
	wattroff(status_window, A_BOLD);

	if (strlen(saymsg) > 0) {
		wmove(status_window, 0, screenw - plen - strlen(saymsg) - 2 - 4);
		if (sayblink > 1 && sayblink % 2 == 0) {
			wattron(status_window, A_BOLD);
		}
		waddstr(status_window, "> ");
		waddstr(status_window, saymsg);
		wattroff(status_window, A_BOLD);
		waddstr(status_window, "    ");
	}
	wattroff(status_window, A_REVERSE);
}

/******************************************************************************
	Draws the normal mode help info
*/
void help_normal()
{
	int col = screenw / 6;
	wmove(help_window, 1, 0);
	wclrtoeol(help_window);
	wmove(help_window, 0, 0);
	wclrtoeol(help_window);
	draw_info(0, 0 * col, " i ", "New");
	draw_info(1, 0 * col, " e ", "Edit");
	draw_info(0, 1 * col, " H ", "Promote");
	draw_info(1, 1 * col, " L ", "Demote");
	draw_info(0, 2 * col, " K ", "Move Up");
	draw_info(1, 2 * col, " J ", "Move Dn");
	draw_info(0, 3 * col, " D ", "Delete");
	draw_info(0, 4 * col, " S ", "Save");
	draw_info(1, 4 * col, " O ", "Open");
	draw_info(0, 5 * col, " A ", "Save as");
	draw_info(1, 5 * col, " Q ", "Quit");
}

/******************************************************************************
	Draws the insert / edit mode help info
*/
void help_edit()
{
	int col = screenw / 6;
	wmove(help_window, 1, 0);
	wclrtoeol(help_window);
	wmove(help_window, 0, 0);
	wclrtoeol(help_window);
	draw_info(0, 0 * col, "C-a", "Home");
	draw_info(1, 0 * col, "C-e", "End");
	draw_info(0, 1 * col, "C-h", "Backsp");
	draw_info(1, 1 * col, "C-d", "Delete");
	draw_info(0, 2 * col, "C-c", "Cancel");
	draw_info(1, 2 * col, "Ret", "Done");
	draw_info(0, 3 * col, "C-k", "CutLineR");
	draw_info(1, 3 * col, "C-u", "CutLineL");
	draw_info(0, 4 * col, "C-w", "CutWordL");
	draw_info(1, 4 * col, "C-x", "CutWordR");
	draw_info(0, 5 * col, "C-v", "Paste");
	draw_info(1, 5 * col, "C-?", "Hide Help");
}

/******************************************************************************
	Print a saymsg that briefly blinks in the status bar
*/
void say(const char *str)
{
	strcpy(saymsg, str);
	if (strlen(str) > 0)
		sayblink = 2 * SAY_BLINKS;
}

/******************************************************************************
	Suppress a previous call to say
*/
void squelch()
{
	sayblink = 0;
	memset(saymsg, 0, MAX_SAY_CHARS);
}

/******************************************************************************
	Redraw but do not refresh the screen
*/
void redraw()
{
	print_tree(root, 0);
	status();
	switch(help_mode) {
	case H_NORMAL: help_normal(); break;
	case H_EDIT: help_edit(); break;
	default: break;
	}
}

/******************************************************************************
	Ask the user a yes or no question
*/
bool confirm(const char *question)
{
	char *str, ans;
	str = prompt(question, NULL);
	ans = str[0];
	free(str);
	if (ans == 'y')
		return true;
	if (ans != 'n')
		say("Please type 'y' or 'n'.");
	return false;
}

/******************************************************************************
	Returns true if the user doesn't want to save changes
*/
bool modified_warning()
{
	if (!modified)
		return true;
	return confirm("Discard unsaved changes? (y/n)");
}

/******************************************************************************
	Confirm the user wants to delete the selected entry, and then does so
*/
void delete()
{
	if (confirm("Delete entry? (y/n)")) {
		struct tree *t = del_child(selected_entry);
		if (t != NULL && t != root) {
			free_tree(t);
			say("Entry deleted.");
			modified = true;
		} else if (t == root) {
			say("Cannot delete root entry.");
		} else {
			say("No entry found.");
		}
	} else {
		say("Deletion cancelled.");
	}
}

/******************************************************************************
	Enter the main runtime loop and wait for commands
*/
void menu()
{
	char *tmpstr;
	int c = 0;
	keypad(tree_window, TRUE);
	while (c != 'Q') {
		redraw();
		wrefresh(tree_window);
		do {
			status();
			wrefresh(status_window);
			if (sayblink) {
				sayblink--;
				napms(SAY_DURATION);
			}
		} while (sayblink > 0);
		if (help_window)
			wrefresh(help_window);
		say("");
		c = wgetch(tree_window);
		switch(c) {
		case 0x03: /* Ctrl+C */
		case 'q':
			say("Shift+Q to quit");
			break;
		case 'Q':
			if (!modified_warning())
				c = '\0';
			break;
		case 'K':
			shove_up();
			break;
		case 'k':
		case KEY_UP:
			select_up();
			break;
		case 'J':
			shove_down();
			break;
		case 'j':
		case KEY_DOWN:
			select_down();
			break;
		case 'L':
			demote();
			break;
		case 'l': 
		case KEY_RIGHT:
			set_fold(EXPANDED);
			break;
		case 'H':
			promote();
			break;
		case 'h':
		case KEY_LEFT:
			set_fold(COLLAPSED);
			break;
		case 'i':
			insert_entry();
			break;
		case 'e':
			edit_entry();
			break;
		case 'D':
			delete();
			break;
		case 'A':
			tmpstr = prompt("Save as...", filename); 
			if (tmpstr != NULL) {
				saveas(tmpstr);
				free(tmpstr);
			}
			break;
		case 'S':
			save();
			break;
		case 'O':
			tmpstr = prompt("Open...", filename);
			if (tmpstr != NULL) {
	 			load(tmpstr);
	 			free(tmpstr);
			}
			break;
		case 0x1F: /* C-? */
		case '?':
			help_mode = help_mode == H_HIDE ? H_NORMAL : H_HIDE;
			resize();
			break;
		default:
			break;
		}
	}
}

/******************************************************************************
	Entry point
*/
int main(int argc, char *argv[])
{
	(void)(argc);
	(void)(argv);

	modified = false;
	help_mode = SHOW_HELP_DEFAULT ? H_NORMAL : H_HIDE;
	memset(filename, 0, MAX_ENTRY_LEN);

	if (argc > 1) {
		if(!load(argv[1])) {
			FILE *f = fopen(argv[1], "w");		
			if (f) {
				char temp[MAX_SAY_CHARS];
				int len = strlen(argv[1]);
				if (len > MAX_ENTRY_LEN-1)
					len = MAX_ENTRY_LEN-1;
				sprintf(temp, "Created '%s'", argv[1]);
				fclose(f);
				load(argv[1]);
				squelch();
				say(temp);
				memcpy(filename, argv[1], len);
				filename[len] = '\0';
			} else {
				say("Failed to create file.");
			}
		}
	} else {
		root = add_child(NULL, "Entries");
	}
	selected_entry = root;

	init_curses();
	menu();
	endwin();

	return 0;
}
