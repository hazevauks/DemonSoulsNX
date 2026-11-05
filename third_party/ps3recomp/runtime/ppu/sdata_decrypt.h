/*
 * SDATA/EDAT (NPD) decryption for cellFsSdataOpen.
 *
 * DemonSoulsNX: this copy of ps3recomp carries no keys and decrypts nothing (see
 * libs/filesystem/edat.c). cellFsSdataOpen reaches the player's own decrypted copy through
 * edat_resolve() first; this fallback only reports that a file is still encrypted.
 */
#ifndef SDATA_DECRYPT_H
#define SDATA_DECRYPT_H

#include <stddef.h>
#include <stdint.h>

/* Always NULL: the caller then keeps the file closed and logs that it needs decrypting. */
static inline uint8_t* sdata_decrypt(const uint8_t* d, size_t n, size_t* out_size)
{
    (void)d; (void)n;
    if (out_size) *out_size = 0;
    return NULL;
}

#endif /* SDATA_DECRYPT_H */
