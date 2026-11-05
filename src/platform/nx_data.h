/* nx_data.h -- checks the game's folder on the SD card. */

#ifndef NX_DATA_H
#define NX_DATA_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool found;                     /* PS3_GAME/PARAM.SFO could be read */
    bool right_version;             /* BLUS30443, version 01.00 */
    char title_id[16];
    char version[8];
    int folders_found;              /* of the USRDIR folders the game needs */
    int folders_expected;
    const char *first_missing;      /* the first of them that is missing, or NULL */
} NxData;

void nx_data_check(NxData *out);
void nx_data_log(const NxData *data);

#ifdef __cplusplus
}
#endif

#endif /* NX_DATA_H */
