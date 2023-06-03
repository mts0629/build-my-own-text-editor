/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** data ***/

// Original termios settings
static struct termios orig_termios;

/*** terminal ***/

void die(const char* s) {
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = orig_termios;

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

/*** init ***/

int main(void) {
    enableRawMode();

    while (1) {
        char c = '\0';
        if ((read(STDIN_FILENO, &c, 1) == -1) && (errno != EAGAIN)) {
            die("read");
        }

        // Display ASCII code of pressed key
        if (iscntrl(c)) {
            printf("%d\r\n", c);
        } else {
            // Also display a character of pressed key when it is printable
            printf("%d ('%c')\r\n", c, c);
        }

        if (c == 'q') {
            break;
        }
    }

    return 0;
}
