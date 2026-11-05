/* nx_paths.h -- where the port keeps its files on the SD card. */

#ifndef NX_PATHS_H
#define NX_PATHS_H

#define NX_APP_DIR          "sdmc:/switch/demonsouls"
#define NX_GAME_DIR         NX_APP_DIR "/game"           /* the disc's folder: PS3_DISC.SFB, PS3_GAME */
#define NX_HDD0_DIR         NX_APP_DIR "/hdd0"           /* the PS3's drive (/dev_hdd0): saves */
#define NX_HDD1_DIR         NX_APP_DIR "/hdd1"           /* the game's cache (/dev_hdd1), emptied at every start */
#define NX_FIRMWARE_DIR     NX_APP_DIR "/firmware"       /* libsre.prx, decrypted from your PS3 firmware */
#define NX_LOG_DIR          NX_APP_DIR "/SD_LOG"

#define NX_LOG_FILE         "demonsouls_log.txt"
#define NX_LOG_FILE_OLD     "demonsouls_log_previous.txt"
#define NX_CRASH_FILE       "demonsouls_crash.txt"
#define NX_FATAL_FILE       "demonsouls_fatal.txt"

#endif /* NX_PATHS_H */
