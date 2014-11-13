/* CTRL+X: "^X"
 * ALT+X: "M-X"
 */
#define RVK_QUIT        "q"
#define RVK_DOWN        "j"
#define RVK_UP          "k"
#define RVK_JUMP_DOWN   "J"
#define RVK_JUMP_UP     "K"
#define RVK_CD_DOWN     "l"
#define RVK_CD_UP       "h"
#define RVK_HOME        "H"
#define RVK_SHELL       "^M"
#define RVK_VIEW        " "
#define RVK_EDIT        "e"
#define RVK_SEARCH      "/"
#define RVK_TG_FILES    "f"
#define RVK_TG_DIRS     "d"
#define RVK_TG_HIDDEN   "s"
#define RVK_TG_MARK     "m"
#define RVK_INVMARK     "M"
#define RVK_MARKALL     "a"
#define RVK_DELETE      "X"
#define RVK_COPY        "C"

/* Colors available: DEFAULT, RED, GREEN, YELLOW, BLUE, CYAN, MAGENTA, WHITE. */
#define RVC_CWD         GREEN
#define RVC_STATUS      BLUE
#define RVC_BORDER      CYAN
#define RVC_SCROLLBAR   BLUE
#define RVC_FILE        DEFAULT
#define RVC_DIR         DEFAULT
#define RVC_HIDDEN      YELLOW
#define RVC_PROMPT      DEFAULT
#define RVC_TABNUM      DEFAULT

/* Special symbols used by the TUI. See <curses.h> for available constants. */
#define RVS_SCROLLBAR   ACS_CKBOARD

#define RV_JUMP         10
