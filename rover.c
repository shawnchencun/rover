#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <sys/types.h>  /* pid_t, ... */
#include <stdio.h>      /* FILENAME_MAX */
#include <locale.h>     /* setlocale(), LC_ALL */
#include <unistd.h>     /* chdir(), getcwd() */
#include <dirent.h>     /* DIR, struct dirent, opendir(), ... */
#include <sys/stat.h>
#include <sys/wait.h>   /* waitpid() */
#include <signal.h>     /* struct sigaction, sigaction() */
#include <curses.h>

#include "config.h"

/* String buffers. */
#define ROWSZ 256
static char ROW[ROWSZ];
#define STATUSSZ 256
static char STATUS[STATUSSZ];
#define SEARCHSZ 256
static char SEARCH[SEARCHSZ];

/* Argument buffers for execvp(). */
#define MAXARGS 256
static char *ARGS[MAXARGS];

typedef enum {DEFAULT, RED, GREEN, YELLOW, BLUE, CYAN, MAGENTA, WHITE} color_t;

/* Height of listing view. */
#define HEIGHT (LINES-4)

/* Listing view flags. */
#define SHOW_FILES      0x01u
#define SHOW_DIRS       0x02u
#define SHOW_HIDDEN     0x04u

/* Information associated to each entry in listing. */
typedef struct {
    char *name;
    off_t size;
} row_t;

/* Global state. Some basic info is allocated for ten tabs. */
struct rover_t {
    int tab;
    int nfiles;
    int scroll[10];
    int fsel[10];
    uint8_t flags[10];
    row_t *rows;
    WINDOW *window;
    char cwd[10][FILENAME_MAX];
} rover;

/* Macros for accessing global state. */
#define FNAME(I)    rover.rows[I].name
#define FSIZE(I)    rover.rows[I].size
#define SCROLL      rover.scroll[rover.tab]
#define FSEL        rover.fsel[rover.tab]
#define FLAGS       rover.flags[rover.tab]
#define CWD         rover.cwd[rover.tab]

/* Curses clean up. Must be called before exiting browser. */
static void
clean_term()
{
    endwin();
}

static void handle_segv(int sig);
static void handle_winch(int sig);

/* Curses setup. */
static void
init_term()
{
    struct sigaction sa;

    setlocale(LC_ALL, "");
    initscr();
    cbreak(); /* Get one character at a time. */
    noecho();
    nonl(); /* No NL->CR/NL on output. */
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);
    curs_set(FALSE); /* Hide blinking cursor. */
    memset(&sa, 0, sizeof(struct sigaction));
    /* Setup SIGSEGV handler. */
    sa.sa_handler = handle_segv;
    sigaction(SIGSEGV, &sa, NULL);
    /* Setup SIGWINCH handler. */
    sa.sa_handler = handle_winch;
    sigaction(SIGWINCH, &sa, NULL);
    if (has_colors()) {
        start_color();
        init_pair(RED, COLOR_RED, COLOR_BLACK);
        init_pair(GREEN, COLOR_GREEN, COLOR_BLACK);
        init_pair(YELLOW, COLOR_YELLOW,COLOR_BLACK);
        init_pair(BLUE, COLOR_BLUE, COLOR_BLACK);
        init_pair(CYAN, COLOR_CYAN, COLOR_BLACK);
        init_pair(MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(WHITE, COLOR_WHITE, COLOR_BLACK);
    }
    atexit(clean_term);
}

/* Update the listing view. */
static void
update_browser()
{
    int i, j;
    int ishidden, isdir;

    for (i = 0, j = SCROLL; i < HEIGHT && j < rover.nfiles; i++, j++) {
        ishidden = FNAME(j)[0] == '.';
        isdir = strchr(FNAME(j), '/') != NULL;
        if (j == FSEL)
            wattr_on(rover.window, A_REVERSE, NULL);
        if (ishidden)
            wcolor_set(rover.window, RVC_HIDDEN, NULL);
        else if (isdir)
            wcolor_set(rover.window, RVC_DIR, NULL);
        else
            wcolor_set(rover.window, RVC_FILE, NULL);
        if (!isdir)
            sprintf(ROW, "%s%*d", FNAME(j),
                    COLS - strlen(FNAME(j)) - 2, (int) FSIZE(j));
        else
            strcpy(ROW, FNAME(j));
        mvwhline(rover.window, i + 1, 1, ' ', COLS - 2);
        mvwaddnstr(rover.window, i + 1, 1, ROW, COLS - 2);
        wcolor_set(rover.window, DEFAULT, NULL);
        if (j == FSEL)
            wattr_off(rover.window, A_REVERSE, NULL);
    }
    if (rover.nfiles > HEIGHT) {
        int center, height;
        center = (SCROLL + (HEIGHT >> 1)) * HEIGHT / rover.nfiles;
        height = (HEIGHT-1) * HEIGHT / rover.nfiles;
        if (!height) height = 1;
        wcolor_set(rover.window, RVC_BORDER, NULL);
        wborder(rover.window, 0, 0, 0, 0, 0, 0, 0, 0);
        wcolor_set(rover.window, RVC_SCROLLBAR, NULL);
        mvwvline(rover.window, center-(height>>1)+1, COLS-1, ACS_CKBOARD, height);
        wcolor_set(rover.window, DEFAULT, NULL);
    }
    wrefresh(rover.window);
    STATUS[0] = FLAGS & SHOW_FILES  ? 'F' : ' ';
    STATUS[1] = FLAGS & SHOW_DIRS   ? 'D' : ' ';
    STATUS[2] = FLAGS & SHOW_HIDDEN ? 'H' : ' ';
    if (!rover.nfiles)
        strcpy(ROW, "0/0");
    else
        sprintf(ROW, "%d/%d", FSEL + 1, rover.nfiles);
    sprintf(STATUS+3, "%*s", 12, ROW);
    color_set(RVC_STATUS, NULL);
    mvaddstr(LINES - 1, COLS - 15, STATUS);
    color_set(DEFAULT, NULL);
    refresh();
}

/* Comparison used to sort listing entries. */
static int
rowcmp(const void *a, const void *b)
{
    int isdir1, isdir2, cmpdir;
    const row_t *r1 = a;
    const row_t *r2 = b;
    isdir1 = strchr(r1->name, '/') != NULL;
    isdir2 = strchr(r2->name, '/') != NULL;
    cmpdir = isdir2 - isdir1;
    return cmpdir ? cmpdir : strcoll(r1->name, r2->name);
}

/* Get all entries for a given path (usually cwd). */
static int
ls(char *path, row_t **rowsp, uint8_t flags)
{
    DIR *dp;
    struct dirent *ep;
    struct stat statbuf;
    row_t *rows;
    int i, n;

    if((dp = opendir(path)) == NULL)
        return -1;
    n = -2; /* We don't want the entries "." and "..". */
    while (readdir(dp)) n++;
    rewinddir(dp);
    rows = (row_t *) malloc(n * sizeof(row_t));
    i = 0;
    while ((ep = readdir(dp))) {
        if (!strcmp(ep->d_name, ".") || !strcmp(ep->d_name, ".."))
            continue;
        if (!(flags & SHOW_HIDDEN) && ep->d_name[0] == '.')
            continue;
        /* FIXME: ANSI C doesn't have lstat(). How do we handle symlinks? */
        stat(ep->d_name, &statbuf);
        if (S_ISDIR(statbuf.st_mode)) {
            if (flags & SHOW_DIRS) {
                rows[i].name = (char *) malloc(strlen(ep->d_name) + 2);
                strcpy(rows[i].name, ep->d_name);
                strcat(rows[i].name, "/");
                i++;
            }
        }
        else if (flags & SHOW_FILES) {
            rows[i].name = (char *) malloc(strlen(ep->d_name) + 1);
            strcpy(rows[i].name, ep->d_name);
            rows[i].size = statbuf.st_size;
            i++;
        }
    }
    n = i; /* Ignore unused space in array caused by filters. */
    qsort(rows, n, sizeof(row_t), rowcmp);
    closedir(dp);
    *rowsp = rows;
    return n;
}

/* Deallocate entries. */
static void
free_rows(row_t **rowsp, int nfiles)
{
    int i;

    for (i = 0; i < nfiles; i++)
        free((*rowsp)[i].name);
    free(*rowsp);
    *rowsp = NULL;
}

/* Change working directory. */
/* NOTE: The caller needs to write the new path to CWD
 *  *before* calling this function. */
static void
cd(int reset)
{
    if (reset)
        FSEL = SCROLL = 0;
    chdir(CWD);
    mvhline(0, 0, ' ', COLS);
    color_set(RVC_CWD, NULL);
    mvaddnstr(0, 0, CWD, COLS);
    color_set(DEFAULT, NULL);
    attr_on(A_BOLD, NULL);
    color_set(RVC_TABNUM, NULL);
    mvaddch(0, COLS-4, rover.tab + '0');
    color_set(DEFAULT, NULL);
    attr_off(A_BOLD, NULL);
    if (rover.nfiles)
        free_rows(&rover.rows, rover.nfiles);
    rover.nfiles = ls(CWD, &rover.rows, FLAGS);
    wclear(rover.window);
    wcolor_set(rover.window, RVC_BORDER, NULL);
    wborder(rover.window, 0, 0, 0, 0, 0, 0, 0, 0);
    wcolor_set(rover.window, DEFAULT, NULL);
    update_browser();
}

/* SIGSEGV handler: clean up curses before exiting. */
static void
handle_segv(int sig)
{
    (void) sig;
    clean_term();
    puts("Received SIGSEGV (segmentation fault).");
    exit(1);
}

/* SIGWINCH handler: resize application according to new terminal settings. */
static void
handle_winch(int sig)
{
    (void) sig;
    delwin(rover.window);
    endwin();
    refresh();
    clear();
    rover.window = subwin(stdscr, LINES - 2, COLS, 1, 0);
    cd(0);
}

/* Do a fork-exec to external program (e.g. $EDITOR). */
static void
spawn()
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid > 0) {
        /* fork() succeeded. */
        clean_term();
        waitpid(pid, &status, 0);
        init_term();
        doupdate();
    }
    else if (pid == 0) {
        /* Child process. */
        execvp(ARGS[0], ARGS);
    }
}

/* Interactive getstr(). */
static int
igetstr(char *buffer, int maxlen)
{
    int ch, length;

    length = strlen(buffer);
    ch = getch();
    if (ch == '\r' || ch == '\n' || ch == KEY_DOWN || ch == KEY_ENTER)
        return 0;
    else if (ch == erasechar() || ch == KEY_LEFT || ch == KEY_BACKSPACE) {
        if (length)
            buffer[--length] = '\0';
    }
    else if (ch == killchar()) {
        length = 0;
        buffer[0] = '\0';
    }
    else if (length < maxlen - 1 && isprint(ch)) {
        buffer[length++] = ch;
        buffer[length] = '\0';
    }
    return 1;
}

int
main(int argc, char *argv[])
{
    int i, ch;
    char *program, *key;
    DIR *d;

    init_term();
    /* Avoid invalid free() calls in cd() by zeroing the tally. */
    rover.nfiles = 0;
    for (i = 0; i < 10; i++) {
        rover.fsel[i] = rover.scroll[i] = 0;
        rover.flags[i] = SHOW_FILES | SHOW_DIRS;
    }
    strcpy(rover.cwd[0], getenv("HOME"));
    for (i = 1; i < argc && i < 10; i++) {
        d = opendir(argv[i]);
        if (d) {
            strcpy(rover.cwd[i], argv[i]);
            closedir(d);
        }
        else strcpy(rover.cwd[i], rover.cwd[0]);
    }
    getcwd(rover.cwd[i], FILENAME_MAX);
    for (i++; i < 10; i++)
        strcpy(rover.cwd[i], rover.cwd[i-1]);
    for (i = 0; i < 10; i++)
        if (rover.cwd[i][strlen(rover.cwd[i]) - 1] != '/')
            strcat(rover.cwd[i], "/");
    rover.tab = 1;
    rover.window = subwin(stdscr, LINES - 2, COLS, 1, 0);
    cd(1);
    while (1) {
        ch = getch();
        key = keyname(ch);
        if (!strcmp(key, RVK_QUIT))
            break;
        else if (ch >= '0' && ch <= '9') {
            rover.tab = ch - '0';
            cd(0);
        }
        else if (!strcmp(key, RVK_DOWN)) {
            if (!rover.nfiles) continue;
            if (FSEL == rover.nfiles - 1)
                SCROLL = FSEL = 0;
            else {
                FSEL++;
                if ((FSEL - SCROLL) == HEIGHT)
                    SCROLL++;
            }
            update_browser();
        }
        else if (!strcmp(key, RVK_UP)) {
            if (!rover.nfiles) continue;
            if (FSEL == 0) {
                FSEL = rover.nfiles - 1;
                SCROLL = rover.nfiles - HEIGHT;
                if (SCROLL < 0)
                    SCROLL = 0;
            }
            else {
                FSEL--;
                if (FSEL < SCROLL)
                    SCROLL--;
            }
            update_browser();
        }
        else if (!strcmp(key, RVK_JUMP_DOWN)) {
            if (!rover.nfiles) continue;
            FSEL += RV_JUMP;
            if (FSEL >= rover.nfiles)
                FSEL = rover.nfiles - 1;
            if (rover.nfiles > HEIGHT) {
                SCROLL += RV_JUMP;
                if (SCROLL > rover.nfiles - HEIGHT)
                    SCROLL = rover.nfiles - HEIGHT;
            }
            update_browser();
        }
        else if (!strcmp(key, RVK_JUMP_UP)) {
            if (!rover.nfiles) continue;
            FSEL -= RV_JUMP;
            if (FSEL < 0)
                FSEL = 0;
            SCROLL -= RV_JUMP;
            if (SCROLL < 0)
                SCROLL = 0;
            update_browser();
        }
        else if (!strcmp(key, RVK_CD_DOWN)) {
            if (!rover.nfiles) continue;
            if (strchr(FNAME(FSEL), '/') == NULL)
                continue;
            strcat(CWD, FNAME(FSEL));
            cd(1);
        }
        else if (!strcmp(key, RVK_CD_UP)) {
            char *dirname, first;
            if (strlen(CWD) == 1)
                continue;
            CWD[strlen(CWD) - 1] = '\0';
            dirname = strrchr(CWD, '/') + 1;
            first = dirname[0];
            dirname[0] = '\0';
            cd(1);
            if ((FLAGS & SHOW_DIRS) &&
                ((FLAGS & SHOW_HIDDEN) || (first != '.'))
               ) {
                dirname[0] = first;
                dirname[strlen(dirname)] = '/';
                while (strcmp(FNAME(FSEL), dirname))
                    FSEL++;
                if (rover.nfiles > HEIGHT) {
                    SCROLL = FSEL - (HEIGHT >> 1);
                    if (SCROLL < 0)
                        SCROLL = 0;
                    if (SCROLL > rover.nfiles - HEIGHT)
                        SCROLL = rover.nfiles - HEIGHT;
                }
                dirname[0] = '\0';
                update_browser();
            }
        }
        else if (!strcmp(key, RVK_HOME)) {
            strcpy(CWD, getenv("HOME"));
            if (CWD[strlen(CWD) - 1] != '/')
                strcat(CWD, "/");
            cd(1);
        }
        else if (!strcmp(key, RVK_SHELL)) {
            program = getenv("SHELL");
            if (program) {
                ARGS[0] = program;
                ARGS[1] = NULL;
                spawn();
            }
        }
        else if (!strcmp(key, RVK_VIEW)) {
            if (!rover.nfiles) continue;
            if (strchr(FNAME(FSEL), '/') != NULL)
                continue;
            program = getenv("PAGER");
            if (program) {
                ARGS[0] = program;
                ARGS[1] = FNAME(FSEL);
                ARGS[2] = NULL;
                spawn();
            }
        }
        else if (!strcmp(key, RVK_EDIT)) {
            if (!rover.nfiles) continue;
            if (strchr(FNAME(FSEL), '/') != NULL)
                continue;
            program = getenv("EDITOR");
            if (program) {
                ARGS[0] = program;
                ARGS[1] = FNAME(FSEL);
                ARGS[2] = NULL;
                spawn();
            }
        }
        else if (!strcmp(key, RVK_SEARCH)) {
            int oldsel, oldscroll;
            if (!rover.nfiles) continue;
            oldsel = FSEL;
            oldscroll = SCROLL;
            *SEARCH = '\0';
            color_set(RVC_PROMPT, NULL);
            mvaddstr(LINES - 1, 0, "search: ");
            curs_set(TRUE);
            color_set(DEFAULT, NULL);
            while (igetstr(SEARCH, SEARCHSZ)) {
                int length, sel;
                color_t color;
                length = strlen(SEARCH);
                if (length) {
                    for (sel = 0; sel < rover.nfiles; sel++)
                        if (!strncmp(FNAME(sel), SEARCH, length))
                            break;
                    if (sel < rover.nfiles) {
                        color = GREEN;
                        FSEL = sel;
                        if (rover.nfiles > HEIGHT) {
                            if (sel < 3)
                                SCROLL = 0;
                            else if (sel - 3 > rover.nfiles - HEIGHT)
                                SCROLL = rover.nfiles - HEIGHT;
                            else
                                SCROLL = sel - 3;
                        }
                    }
                    else
                        color = RED;
                }
                else {
                    FSEL = oldsel;
                    SCROLL = oldscroll;
                }
                update_browser();
                color_set(color, NULL);
                mvaddstr(LINES - 1, 8, SEARCH);
                mvaddch(LINES - 1, length + 8, ' ');
                move(LINES - 1, length + 8);
                color_set(DEFAULT, NULL);
            }
            curs_set(FALSE);
            move(LINES - 1, 0);
            clrtoeol();
            update_browser();
        }
        else if (!strcmp(key, RVK_TG_FILES)) {
            FLAGS ^= SHOW_FILES;
            cd(1);
        }
        else if (!strcmp(key, RVK_TG_DIRS)) {
            FLAGS ^= SHOW_DIRS;
            cd(1);
        }
        else if (!strcmp(key, RVK_TG_HIDDEN)) {
            FLAGS ^= SHOW_HIDDEN;
            cd(1);
        }
    }
    if (rover.nfiles) {
        free_rows(&rover.rows, rover.nfiles);
    }
    delwin(rover.window);
    return 0;
}
