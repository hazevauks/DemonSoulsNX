/* nx_settings.h -- the runtime's options from settings.txt on the SD card (nx_settings.c). */

#ifndef NX_SETTINGS_H
#define NX_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Sets each NAME=value of the file as an environment variable, logs it, and returns how many. */
int nx_settings_load(void);

/* The same for each NAME=value among the NRO's arguments (nxlink --args, tools/nxlink_run.py
 * --set), after the file: they win. */
int nx_settings_args(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NX_SETTINGS_H */
