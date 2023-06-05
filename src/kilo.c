/*** includes ***/

// Feature test macro for getline()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

// write() with error checking by its return value
#define WRITE_WITH_CHECK(...) { \
    if (write(__VA_ARGS__) == -1) {\
        perror("write"); \
        exit(1); \
    } \
}

// Internal representations of control keys
enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

// Editor row
typedef struct ERow {
    int size;
    char* chars;
} ERow;

// Editor configuration
struct EditorConfig {
    int cx, cy; // Cursor position
    int screen_rows;
    int screen_cols;
    int num_rows;
    ERow row;
    struct termios orig_termios; // Original configuration
};

static struct EditorConfig E;

/*** terminal ***/

void die(const char* s) {
    // "<ESC>[2J": clear the entire screen
    WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[2J", 4);
    // "<ESC>[H" ("[1;1H"): move the cursor to top-left of the screen
    WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // Turn off input flags:
    // - Break condition (SIGINT)
    // - Translating CR into NL
    // - Parity checking
    // - Stripping 8th bit of each input byte
    // - Software flow control (Ctrl-S, Ctrl-Q)
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

    // Turn off output flags:
    // - Translating NL to CR+NL
    raw.c_oflag &= ~(OPOST);

    // Set control flags:
    // - Set the character size to 8bits per byte
    raw.c_cflag |= (CS8);

    // Turn off local flags:
    // - Echoing
    // - Canonical mode
    // - Input of special characters
    // - Interrupts (SIGINT, SIGTSTP)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // Set timeout condition of read()
    raw.c_cc[VMIN] = 0;  // There's any input to be read
    raw.c_cc[VTIME] = 1; // Wait 1/10[s]=100[ms]

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if ((nread == -1) && (errno != EAGAIN)) {
            die("read");
        }
    }

    // Parse escape sequences
    if (c == '\x1b') {
        char seq[3];
        // Return when preceding bytes can't be read
        if (read(STDIN_FILENO, &seq[0], 1) != 1) {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) {
            return '\x1b';
        }

        if (seq[0] == '[') {
            if ((seq[1] >= '0') && (seq[1] <= '9')) {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) {
                    return '\x1b';
                }
                // "<ESC>[0" - "<ESC>[9"
                if (seq[2] == '~') { // '~' is the terminator
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                        default: break;
                    }
                }
            } else {
                // "<ESC>[" + non-digit characters
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                    default: break;
                }
            }
        } else if (seq[0] == 'O') {
            // "<ESC>OH" and "<ESC>OF" also represent the Home and End keys
            // (depend on the environment)
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                default: break;
            }
        }

        return '\x1b';
    } else {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) {
    // "<ESC>[6n": ask for the cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
        return -1;
    }

    // Get the cursor position from stdin
    char buf[32];
    unsigned int i = 0;
    while (i < (sizeof(buf) - 1)) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) {
            break;
        }
        if (buf[i] == 'R') {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if ((buf[0] != '\x1b') || (buf[1] != '[')) {
        return -1;
    }

    // Set the cursor position to the arguments
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
        return -1;
    }

    return -1;
}

int getWindowSize(int* rows, int* cols) {
    struct winsize ws;

    // Get window size by ioctl()
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) || (ws.ws_col == 0)) {
        // If it failed, move the cursor to the bottom-right and get its position
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
            return -1;
        }
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** file I/O ***/

void editorOpen(const char* file_name) {
    FILE* fp = fopen(file_name, "r");
    if (!fp) {
        die("fopen");
    }

    char* line = NULL;
    size_t line_cap = 0; // Line capacity
    ssize_t line_len; // Num of characters being read
    line_len = getline(&line, &line_cap, fp);
    if (line_len != -1) {
        while ((line_len > 0) &&
               ((line[line_len - 1] == '\n') || (line[line_len - 1] == '\r'))) {
            line_len--;
        }
        // Copy a line text to the one of the editor row
        E.row.size = line_len;
        E.row.chars = malloc(line_len + 1);
        memcpy(E.row.chars, line, line_len);
        E.row.chars[line_len] = '\0';
        E.num_rows = 1;
    }

    free(line);
    fclose(fp);
}

/*** append buffer ***/

// Append buffer
struct ABuf {
    char* b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void ABAppend(struct ABuf* ab, const char* s, int len) {
    // Re-allocate and update the append buffer with appending new characters
    char* new = realloc(ab->b, ab->len + len);
    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void ABFree(struct ABuf* ab) {
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct ABuf* ab) {
    for (int y = 0; y < E.screen_rows; y++) {
        if (y >= E.num_rows) {
            // If there are no editor rows:
            // Draw editor titles at the center of the screen
            // And draw '~' at the end of each line
            if ((E.num_rows == 0) && (y == E.screen_rows / 3)) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                    "kilo editor -- version %s", KILO_VERSION);
                if (welcome_len > E.screen_cols) {
                    welcome_len = E.screen_cols;
                }
                // Do centering of the titles
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding) {
                    ABAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    ABAppend(ab, " ", 1);
                }
                ABAppend(ab, welcome, welcome_len);
            } else {
                ABAppend(ab, "~", 1);
            }
        } else {
            // Draw the editor rows
            int len = E.row.size;
            if (len > E.screen_rows) {
                len = E.screen_cols;
            }
            ABAppend(ab, E.row.chars, len);
        }

        // "<ESC>[K" ("[K1"): clear the current line
        ABAppend(ab, "\x1b[K", 3);
        if (y < (E.screen_rows - 1)) {
            ABAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen(void) {
    struct ABuf ab = ABUF_INIT;

    // "<ESC>[?25l": make the cursor invisible (in VT-510 terminal)
    ABAppend(&ab, "\x1b[?25l", 6);
    ABAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    // Refer the cursor position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy + 1), (E.cx + 1));
    ABAppend(&ab, buf, strlen(buf));

    // "<ESC>[?25h": make the cursor visible (same with the above)
    ABAppend(&ab, "\x1b[?25h", 6);

    // Draw append buffer
    WRITE_WITH_CHECK(STDOUT_FILENO, ab.b, ab.len);

    ABFree(&ab);
}

/*** input ***/

void editorMoveCursor(const int key) {
    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            }
            break;
        case ARROW_RIGHT:
            if (E.cx != (E.screen_cols - 1)) {
                E.cx++;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != (E.screen_rows - 1)) {
                E.cy++;
            }
            break;
        default:
            break;
    }
}

void editorProcessKeypress(void) {
    int c = editorReadKey();

    switch (c) {
        // Quit the editor
        case CTRL_KEY('q'):
            WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[2J", 4);
            WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        // Move cursor
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = E.screen_cols - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = E.screen_rows;
                while (times--) {
                    editorMoveCursor((c == PAGE_UP) ? ARROW_UP : ARROW_DOWN);
                }
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        default:
            break;
    }
}

/*** init ***/

void initEditor(void) {
    // Initialize
    // Cursor position
    E.cx = 0;
    E.cy = 0;
    // Editor row
    E.num_rows = 0;

    // Save the current window size
    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1) {
        die("getWindowSize");
    }
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
