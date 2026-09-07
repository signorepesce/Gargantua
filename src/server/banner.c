#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void server_banner(void)
{
    const char *term = getenv("TERM");
    int color = isatty(STDOUT_FILENO) && term != NULL &&
                strcmp(term, "dumb") != 0 && getenv("NO_COLOR") == NULL;
    if (color) { (void)fputs("\033[38;5;100m", stdout); }
    (void)fputs(
        "\n"
        "                          _...._\n"
        "                       .-'      `-.\n"
        "                     .'       /\\   \\\n"
        "                   .'        /  \\   |\n"
        "                  /       .-'    `._/\n"
        "                 /    _.-'  |\n"
        "                /  .-'      |\n"
        "               /            |\n"
        "              /    __..--   \\\n"
        "             /_.--'          \\\n"
        "            /       ____..---'\\\n"
        "           /__..---'           \\\n"
        "      _..-'     __....----..__  `-.._\n"
        "  _.-'   __..--'             `--.._  `-._\n"
        " (___..-'      ___....----....___   `-.___)\n"
        "     `---..___/                 `---..-'\n"
        "\n"
        "                 G A R G A N T U A\n"
        "\n", stdout);
    if (color) { (void)fputs("\033[0m", stdout); }
}
