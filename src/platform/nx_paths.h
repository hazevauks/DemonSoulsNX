/* nx_paths.h -- where the port keeps its files on the SD card. */

#ifndef NX_PATHS_H
#define NX_PATHS_H

#define NX_APP_DIR          "sdmc:/switch/demonsouls"
#define NX_GAME_DIR         NX_APP_DIR "/game"           /* the disc's folder: PS3_DISC.SFB, PS3_GAME */
#define NX_HDD0_DIR         NX_APP_DIR "/hdd0"           /* the PS3's drive (/dev_hdd0): saves */
#define NX_HDD1_DIR         NX_APP_DIR "/hdd1"           /* the game's cache (/dev_hdd1), emptied at every start */
#define NX_HDD1_MEM_DIR     "hdd1:"                      /* the same cache in memory (nx_cache_mem.c), the default */
#define NX_HDD1_SPILL_DIR   NX_APP_DIR "/hdd1_spill"     /* its files past the memory it may use */
#define NX_FIRMWARE_DIR     NX_APP_DIR "/firmware"       /* libsre_image.bin, made from your firmware's libsre.prx */
#define NX_SHADER_CACHE_DIR NX_APP_DIR "/shader_cache"   /* the game's shaders, compiled for deko3D */
#define NX_LOG_DIR          NX_APP_DIR "/SD_LOG"
#define NX_SETTINGS_FILE    NX_APP_DIR "/settings.txt"   /* the runtime's options, read as the game starts */

#define NX_LOG_FILE         "demonsouls_log.txt"
#define NX_LOG_FILE_OLD     "demonsouls_log_previous.txt"
#define NX_CRASH_FILE       "demonsouls_crash.txt"
#define NX_FATAL_FILE       "demonsouls_fatal.txt"

#endif /* NX_PATHS_H */
