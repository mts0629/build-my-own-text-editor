/*** includes ***/

// Feature test macro for getline()
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

// write() with error checking by its return value
#define WRITE_WITH_CHECK(...) { \
    if (write(__VA_ARGS__) == -1) {\
        perror("write"); \
        exit(1); \
    } \
}

// Internal representations of control keys
enum editorKey {
    BACKSPACE = 127,
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

// Highlighting values
enum editorHighlight {
    HL_NORMAL = 0,
    HL_NUMBER,
    HL_MATCH
};

/*** data ***/

// Editor row
typedef struct erow {
    int size; // Row size
    int rsize; // Rendering size
    char* chars; // Characters in the row
    char* render; // Rendering characters
    unsigned char* hl; // Highlighting
} erow;

// Editor configuration
struct editorConfig {
    int cx, cy; // Cursor position
    int rx; // Rendering index
    int rowoff; // Row offset
    int coloff; // Column offset
    int screenrows; // The number of rows of the screen
    int screencols; // The number of columns of the screen
    int numrows; // The number of rows
    erow* row; // Editor rows
    int dirty; // Dirty flag
    char* filename; // File name
    char statusmsg[80]; // Status message
    time_t statusmsg_time; // Timestamp when status message is updated
    struct termios orig_termios; // Original configuration
};

static struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen(void);
char* editorPrompt(char* prompt, void (*callback)(char*, int));

/*** terminal ***/

// Clean up when abnormal termination
void die(const char* s) {
    // "<ESC>[2J": clear the entire screen
    WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[2J", 4);
    // "<ESC>[H" ("[1;1H"): move the cursor to top-left of the screen
    WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

// Disable raw mode
void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
        die("tcsetattr");
    }
}

// Enable raw mode
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

// Read a pressed key and return the key value
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

// Get cursor position
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

// Get window size
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

/*** syntax highlighting ***/

// Update syntax values of the row
void editorUpdateSyntax(erow* row) {
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    for (int i = 0; i < row->rsize; i++) {
        if (isdigit(row->render[i])) {
            row->hl[i] = HL_NUMBER;
        }
    }
}

// Return corresponding ANSI color code for each syntax value
int editorSyntaxToColor(int hl) {
    switch (hl) {
        case HL_NUMBER:
            return 31; // Foreground red
        case HL_MATCH:
            return 34; // Foreground blue
        default:
            return 37; // Foreground white
    }
}

/*** row operations ***/

// Convert character position X to rendering position
int editorRowCxToRx(erow* row, const int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t') {
            rx += ((KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP));
        }
        rx++;
    }
    return rx;
}

// Convert rendering position X to character position
int editorRowRxToCx(erow *row, const int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t') {
            cur_rx += (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
        }
        cur_rx++;

        if (cur_rx > rx) {
            return cx;
        }
    }
    return cx;
}

// Update the editor row
void editorUpdateRow(erow* row) {
    // Count tab
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    // Copy display characters to the render
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        // Expand tab
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while ((idx % KILO_TAB_STOP) != 0) {
                row->render[idx++] = ' ';
            }
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

// Append characters to the editor row
void editorInsertRow(const int at, char* s, const size_t len) {
    if ((at < 0) || (at > E.numrows)) {
        return;
    }

    // Reallocate character row
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow)* (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    // Update rendering row
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].hl = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

// Free the editor row
void editorFreeRow(erow* row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

// Delete the editor row
void editorDelRow(const int at) {
    if ((at < 0) || (at >= E.numrows)) {
        return;
    }

    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
    E.numrows--;
    E.dirty++;
}

// Insert a character to the editor row
void editorRowInsertChar(erow* row, int at, const int c) {
    if ((at < 0) || (at > row->size)) {
        at = row->size;
    }
    row->chars = realloc(row->chars, (row->size + 2));
    // Copy string, safe for overwrapping of src/dest buffers
    memmove(&row->chars[at + 1], &row->chars[at], (row->size - at + 1));
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

// Append a string to the editor row
void editorRowAppendString(erow* row, char* s, const size_t len) {
    row->chars = realloc(row->chars, (row->size + len + 1));
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

// Delete a character from the editor row
void editorRowDelChar(erow* row, const int at) {
    if ((at < 0) || (at >= row->size)) {
        return;
    }

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

// Insert a character
void editorInsertChar(const int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

// Insert a newline
void editorInsertNewline(void) {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
    } else {
        erow* row = &E.row[E.cy];
        editorInsertRow((E.cy + 1), &row->chars[E.cx], (row->size - E.cx));
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cy++;
    E.cx = 0;
}

// Delete a character
void editorDelChar(void) {
    if (E.cy == E.numrows) {
        return;
    }
    if ((E.cx == 0) && (E.cy == 0)) {
        return;
    }

    erow* row = &E.row[E.cy];
    if (E.cx > 0) {
        editorRowDelChar(row, (E.cx - 1));
        E.cx--;
    } else {
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}

/*** file I/O ***/

// Copy string of editor rows to the buffer
char* editorRowsToString(int* buflen) {
    // Get total length
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++) {
        totlen += E.row[j].size + 1;
    }
    *buflen = totlen;

    // Copy editor rows to the buffer
    char* buf = malloc(totlen);
    char* p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

// Open the file
void editorOpen(const char* filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE* fp = fopen(filename, "r");
    if (!fp) {
        die("fopen");
    }

    char* line = NULL;
    size_t linecap = 0; // Capacity of line
    ssize_t linelen; // Length of reading line
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while ((linelen > 0) &&
               ((line[linelen - 1] == '\n') || (line[linelen - 1] == '\r'))) {
            linelen--;
        }
        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);

    E.dirty = 0;
}

// Save the file
void editorSave(void) {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char* buf = editorRowsToString(&len);

    // Open a file descriptor
    // `0644` is the standrd permissions for text file (read/write)
    int fd = open(E.filename, (O_RDWR | O_CREAT), 0644);
    if (fd != -1) {
        // Truncate the file size
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error %s", strerror(errno));
}

/*** find ***/
// Searching process for editorFind
void editorFindCallback(char* query, int key) {
    static int last_match = -1; // -1: no match, otherwise: num of the row
    static int direction = 1; // 1: forward, -1: backward

    static int saved_hl_line; // Line which highlighting is saved
    static char* saved_hl = NULL; // Saved highlighting

    if (saved_hl) {
        memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
        free(saved_hl); // saved_hl is guaranteed to be deallocated here
        saved_hl = NULL;
    }

    // If the ENTER or ESC are pressed, quit search mode immediately
    if ((key == '\r') || (key == '\x1b')) {
        last_match = -1;
        direction = 1;
        return;
    } else if ((key == ARROW_RIGHT) || (key == ARROW_DOWN)) {
        direction = 1;
    } else if ((key == ARROW_LEFT) || (key == ARROW_UP)) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    // If there's no match, search forward
    if (last_match == -1) {
        direction = 1;
    }

    // Search the query
    // and move the cursor to a head of founded query
    int current = last_match;
    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        // Search from the next line
        if (current == -1) {
            current = E.numrows - 1;
        } else if (current == E.numrows) {
            current = 0;
        }

        erow *row  = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, (match - row->render));
            E.rowoff = E.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

// Find an input string from the editor row
void editorFind(void) {
    int saved_cx = E.cx;
    int saved_cy = E.cy;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    // Search the input string by any key-press event
    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

    if (query == NULL) {
        free(query);
    } else {
        E.cx = saved_cx;
        E.cy = saved_cy;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** append buffer ***/

// Append buffer
struct abuf {
    char* b; // Character buffer
    int len; // Length
};

// Initial buffer
#define ABUF_INIT {NULL, 0}

// Append new characters to the append buffer
void abAppend(struct abuf* ab, const char* s, int len) {
    // Reallocate and update the append buffer by additional characters
    char* new = realloc(ab->b, ab->len + len);
    if (new == NULL) {
        return;
    }

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

// Free the append buffer
void abFree(struct abuf* ab) {
    free(ab->b);
}

/*** output ***/

// Scroll the screen
void editorScroll(void) {
    // Set rendering index
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    // Set rendering position
    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screenrows + 1;
    }
}

// Draw rows
void editorDrawRows(struct abuf* ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            // If there are no editor rows:
            // Draw editor titles at the center of the screen
            // And draw '~' at the end of each line
            if ((E.numrows == 0) && (y == E.screenrows / 3)) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > E.screencols) {
                    welcomelen = E.screencols;
                }
                // Do centering of the titles
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) {
                    abAppend(ab, " ", 1);
                }
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            // Draw the rendering rows
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screencols) {
                len = E.screencols;
            }
            char* c = &E.row[filerow].render[E.coloff];
            unsigned char* hl = &E.row[filerow].hl[E.coloff];
            int current_color = -1;
            for (int j = 0; j < len; j++) {
                if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5); // Set the text color back to normal
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    // Apply a color by the highlighting value when it is chahged
                    int color = editorSyntaxToColor(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }

        // "<ESC>[K" ("[K1"): clear the current line
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

// Draw status bar
void editorDrawStatusBar(struct abuf* ab) {
    abAppend(ab, "\x1b[7m", 4); // Invert color
    // Copy the file name
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        (E.filename ? E.filename : "[No Name]"), E.numrows,
        (E.dirty ? "(modified)" : ""));
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        (E.cy + 1), E.numrows);
    if (len > E.screencols) {
        len = E.screencols;
    }
    abAppend(ab, status, len);
    // Draw the status
    while (len < E.screencols) {
        if ((E.screencols - len) == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3); // Restore default formatting
    abAppend(ab, "\r\n", 2);
}

// Draw the message bar
void editorDrawMessageBar(struct abuf* ab) {
    // Clear the message bar
    abAppend(ab, "\x1b[K", 3);
    int msg_len = strlen(E.statusmsg);
    if (msg_len > E.screencols) {
        msg_len = E.screencols;
    }
    // Disappear when any key is pressed after 5 seconds from the start
    if (msg_len && (time(NULL) - E.statusmsg_time < 5)) {
        abAppend(ab, E.statusmsg, msg_len);
    }
}

// Refresh screen
void editorRefreshScreen(void) {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    // "<ESC>[?25l": make the cursor invisible (in VT-510 terminal)
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // Refer the cursor position
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
        (E.cy - E.rowoff + 1), (E.rx - E.coloff + 1));
    abAppend(&ab, buf, strlen(buf));

    // "<ESC>[?25h": make the cursor visible (same with the above)
    abAppend(&ab, "\x1b[?25h", 6);

    // Draw append buffer
    WRITE_WITH_CHECK(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

// Set string to the status bar
void editorSetStatusMessage(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** input ***/

// Show a prompt and execute a callback set by the user input
char* editorPrompt(char* prompt, void (*callback)(char*, int)) {
    size_t bufsize = 128;
    char* buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if ((c == DEL_KEY) || (c == CTRL_KEY('h')) || (c == BACKSPACE)) {
            if (buflen != 0) {
                buf[--buflen] = '\0';
            }
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                if (callback) {
                    callback(buf, c);
                }
                return buf;
            }
        } else if (!iscntrl(c) && (c < 128)) {
            if (buflen == (bufsize - 1)) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }

        if (callback) {
            callback(buf, c);
        }
    }
}

// Move the cursor by a key code
void editorMoveCursor(const int key) {
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && (E.cx  < row->size)) {
                E.cx++;
            } else if (row && (E.cx == row->size)) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy != E.numrows) {
                E.cy++;
            }
            break;
        default:
            break;
    }

    // Snap cursor to the end of line
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int row_len = row ? row->size : 0;
    if (E.cx > row_len) {
        E.cx = row_len;
    }
}

// Do process corresponding with the key value
void editorProcessKeypress(void) {
    static int quit_times = KILO_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;

        // Quit the editor
        case CTRL_KEY('q'):
            if (E.dirty && (quit_times > 0)) {
                editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[2J", 4);
            WRITE_WITH_CHECK(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        // Save the editor contents
        case CTRL_KEY('s'):
            editorSave();
            break;

        // Move cursor
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows) {
                E.cx = E.row[E.cy].size;
            }
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) {
                editorMoveCursor(ARROW_RIGHT);
            }
            editorDelChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) {
                        E.cy = E.numrows;
                    }
                }

                int times = E.screenrows;
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

        case CTRL_KEY('l'):
        case '\x1b': // ESC
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

// Initialize the editor
void initEditor(void) {
    // Initialize parameters
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    // Save the current window size
    if (getWindowSize(&E.screenrows, &E.screencols) == -1) {
        die("getWindowSize");
    }

    E.screenrows -= 2;
}

int main(int argc, char* argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage(
        "HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find"
    );

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
