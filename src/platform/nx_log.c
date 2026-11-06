/* nx_log.c -- the log file in SD_LOG, and nxlink when it is connected. */

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <switch.h>

#include "nx_log.h"
#include "nx_paths.h"

static FILE *log_file;
static int nxlink_fd = -1;

void nx_log_init(const char *version)
{
    mkdir(NX_APP_DIR, 0777);
    mkdir(NX_LOG_DIR, 0777);

    /* keep the previous run: it is the one that went wrong */
    remove(NX_LOG_DIR "/" NX_LOG_FILE_OLD);
    rename(NX_LOG_DIR "/" NX_LOG_FILE, NX_LOG_DIR "/" NX_LOG_FILE_OLD);

    log_file = fopen(NX_LOG_DIR "/" NX_LOG_FILE, "w");
    if (log_file)
        setvbuf(log_file, NULL, _IOLBF, 4096);

    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault()))
        nxlink_fd = nxlinkStdioForDebug();

    /* Without nxlink, what the libraries print to stderr goes to the log file too, through its
     * descriptor: the SD card's file system lets a file be open for writing once. stdout stays
     * on the screen: a descriptor moved onto the file no longer reaches libnx's console, which
     * left the first build's screen black. The game, with no console, will move stdout too. */
    if (nxlink_fd < 0 && log_file) {
        dup2(fileno(log_file), STDERR_FILENO);
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    time_t now = time(NULL);
    const struct tm *t = localtime(&now);
    nx_log("DemonSoulsNX %s log, %04d-%02d-%02d %02d:%02d:%02d, nxlink %s\n", version,
           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec,
           nxlink_fd >= 0 ? "on" : "off");
}

/* One stream for all of it, so that its lock keeps lines from different threads apart. The
 * first game run had stdout and stderr as streams of their own on the log's descriptor: each
 * write goes where the descriptor's offset was when it began, so two threads writing at once
 * wrote over each other and moved the offset past a hole, and the SD card filled the holes with
 * whatever its sectors held before. Threads created from now on take these streams with them. */
void nx_log_capture_stdout(void)
{
    fflush(stdout);
    fflush(stderr);
    if (nxlink_fd < 0 && log_file) {
        stdout = log_file;
        stderr = log_file;
    } else {
        stdout = stderr;    /* nxlink's socket: the log file is nx_log's alone */
    }
}

void nx_log_shutdown(void)
{
    fflush(stdout);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    if (nxlink_fd >= 0) {
        close(nxlink_fd);
        nxlink_fd = -1;
        socketExit();
    }
}

/* Always to the file; with nxlink also to stderr, which goes to the socket while stdout
 * stays on the screen. */
static void write_line(const char *format, va_list args)
{
    va_list copy;
    if (log_file) {
        va_copy(copy, args);
        vfprintf(log_file, format, copy);
        va_end(copy);
    }
    if (nxlink_fd >= 0) {
        va_copy(copy, args);
        vfprintf(stderr, format, copy);
        va_end(copy);
    }
}

void nx_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    write_line(format, args);
    va_end(args);
}

void nx_log_fatal(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    write_line(format, args);
    va_end(args);

    FILE *fatal = fopen(NX_LOG_DIR "/" NX_FATAL_FILE, "w");
    if (fatal) {
        va_start(args, format);
        vfprintf(fatal, format, args);
        va_end(args);
        fclose(fatal);
    }
}
