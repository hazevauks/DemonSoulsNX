/* nx_log.h -- the log file in SD_LOG, and nxlink when it is connected. */

#ifndef NX_LOG_H
#define NX_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opens SD_LOG/demonsouls_log.txt (the last run's log becomes demonsouls_log_previous.txt)
 * and sends stderr there, or to nxlink when the NRO was started with nxlink -s. */
void nx_log_init(const char *version);
void nx_log_shutdown(void);

/* The game, which has no text screen: stdout goes where stderr goes, the log or nxlink. */
void nx_log_capture_stdout(void);

void nx_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Writes SD_LOG/demonsouls_fatal.txt with the message, and logs it. */
void nx_log_fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif /* NX_LOG_H */
