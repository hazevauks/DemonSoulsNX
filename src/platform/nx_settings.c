/* nx_settings.c -- switch/demonsouls/settings.txt: the runtime's and Soulbrandt's options.
 *
 * On a PC they are environment variables, set before the game starts (PS3_*, SPURS_*, PAD_*,
 * DES_*). The Switch has no shell to set them in, so the game reads them from this file, one
 * NAME=value a line, before the boot does: a test can turn one on without a new NRO.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx_log.h"
#include "nx_paths.h"
#include "nx_settings.h"

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static int is_name(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (!isupper((unsigned char)*s) && !isdigit((unsigned char)*s) && *s != '_')
            return 0;
    return 1;
}

int nx_settings_args(int argc, char **argv)
{
    int set = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = argv[i] ? strchr(argv[i], '=') : NULL;
        if (!eq)
            continue;
        *eq = '\0';
        if (is_name(argv[i])) {
            setenv(argv[i], eq + 1, 1);
            nx_log("settings, from the NRO's arguments: %s=%s\n", argv[i], eq + 1);
            set++;
        }
        *eq = '=';
    }
    return set;
}

int nx_settings_load(void)
{
    FILE *f = fopen(NX_SETTINGS_FILE, "r");
    if (!f) {
        nx_log("settings: none (" NX_SETTINGS_FILE ")\n");
        return 0;
    }
    char line[512];
    int set = 0, number = 0;
    while (fgets(line, sizeof line, f)) {
        number++;
        char *s = trim(line);
        if (!*s || *s == '#')
            continue;
        char *eq = strchr(s, '=');
        if (eq)
            *eq = '\0';
        char *name = trim(s);
        if (!eq || !is_name(name)) {
            nx_log("settings: line %d left out: a setting is NAME=value, the name in capitals\n", number);
            continue;
        }
        const char *value = trim(eq + 1);
        setenv(name, value, 1);
        nx_log("settings: %s=%s\n", name, value);
        set++;
    }
    fclose(f);
    return set;
}
