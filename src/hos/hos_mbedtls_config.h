/* hos_mbedtls_config.h -- mbedTLS on Horizon (MBEDTLS_USER_CONFIG_FILE for the runtime).
 *
 * Horizon has none of the platform pieces mbedTLS looks for: no /dev/urandom, no POSIX clock it
 * recognises, no socket layer of its own. Entropy comes from the kernel's random source and
 * milliseconds from the system tick (hos_compat.c); the runtime brings its own sockets
 * (libs/network/ps3_tls.c uses neither mbedTLS's network nor its timing module).
 */
#pragma once

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
