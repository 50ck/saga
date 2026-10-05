#pragma once
// Public ncurses ABI subset, used only when system development headers are absent.
// WINDOW/SCREEN remain opaque; do not depend on their internal layout.
#include <stdio.h>
#include <wchar.h>
extern "C" {
typedef struct _win_st WINDOW;
typedef struct screen SCREEN;
typedef unsigned mmask_t;
typedef struct { short id; int x,y,z; mmask_t bstate; } MEVENT;
extern WINDOW* stdscr;
SCREEN* newterm(const char*,FILE*,FILE*);
void use_env(bool);
void use_tioctl(bool);
int clearok(WINDOW*,bool);
int resizeterm(int,int);
void delscreen(SCREEN*);
int endwin(void);
int raw(void);
int noecho(void);
int keypad(WINDOW*,bool);
void wtimeout(WINDOW*,int);
int wget_wch(WINDOW*,wint_t*);
mmask_t mousemask(mmask_t,mmask_t*);
int getmouse(MEVENT*);
int werase(WINDOW*);
int wmove(WINDOW*,int,int);
int waddnstr(WINDOW*,const char*,int);
int wnoutrefresh(WINDOW*);
int doupdate(void);
int curs_set(int);
int getmaxx(const WINDOW*);
int getmaxy(const WINDOW*);
extern int COLORS;
bool has_colors(void);
int start_color(void);
int use_default_colors(void);
int init_pair(short,short,short);
int wcolor_set(WINDOW*,short,void*);
int wattr_set(WINDOW*,unsigned int,short,void*);
}
#define A_BOLD (1U << 21)
#define A_UNDERLINE (1U << 17)
#define A_DIM (1U << 20)
#define A_ITALIC (1U << 31)
#define ERR (-1)
#define KEY_CODE_YES 0x100
#define KEY_DOWN 0x102
#define KEY_UP 0x103
#define KEY_LEFT 0x104
#define KEY_RIGHT 0x105
#define KEY_HOME 0x106
#define KEY_BACKSPACE 0x107
#define KEY_F(n) (0x108+(n))
#define KEY_DC 0x14a
#define KEY_NPAGE 0x152
#define KEY_PPAGE 0x153
#define KEY_ENTER 0x157
#define KEY_END 0x168
#define KEY_MOUSE 0x199
#define KEY_RESIZE 0x19a
#define BUTTON1_CLICKED (1U << 2)
#define BUTTON4_PRESSED (1U << 16)
#define BUTTON5_PRESSED (1U << 21)
