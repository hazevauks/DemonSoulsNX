/* nx_osk.c -- the PS3's on-screen keyboard (cellOskDialog) as the Switch's own keyboard applet.
 *
 * The game asks for a name at character creation through cellOskDialogLoadAsync, which returns
 * at once and later tells the game the text through a sysutil event. ps3recomp's
 * libs/misc/cellOskDialog.c takes a host for the asking (cellOskDialog_set_host): open() with
 * the prompt, the initial text and the length limit, all UTF-16, and the answer back through
 * cellOskDialog_host_finish from any thread. Without a host it answered at once with the
 * initial text, empty, so the game had no name to go on with.
 *
 * Here open() starts a thread that shows libnx's software keyboard (swkbdShow waits until the
 * player is done) and hands the text back; Cancel is the PS3's Cancel.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "nx_log.h"
#include "nx_osk.h"

/* ps3recomp's libs/misc/cellOskDialog.h, the host's part, in plain C types: that header's
 * ps3types.h and libnx's switch.h both name a u128 and an s128, differently, so the two cannot
 * meet in one file. CellOskDialogChar is a u16, CELL_OSK_MAX_TEXT_LENGTH 512. */
typedef struct CellOskDialogHost {
    void (*open)(const uint16_t* prompt, const uint16_t* text, uint32_t limit);
    void (*close)(void);
} CellOskDialogHost;
void cellOskDialog_set_host(const CellOskDialogHost* host);
void cellOskDialog_host_finish(int ok, const uint16_t* text, uint32_t n);
typedef uint16_t CellOskDialogChar;

#define TEXT_MAX 512

static struct {
    uint16_t prompt[TEXT_MAX + 1];
    uint16_t text[TEXT_MAX + 1];
    uint32_t limit;
} s_ask;

static size_t u16len(const uint16_t* s)
{
    size_t n = 0;
    while (n < TEXT_MAX && s[n]) n++;
    return n;
}

static void* keyboard(void* arg)
{
    (void)arg;
    char prompt[TEXT_MAX * 3 + 1] = "", initial[TEXT_MAX * 3 + 1] = "", out[TEXT_MAX * 4 + 1] = "";
    ssize_t n = utf16_to_utf8((uint8_t*)prompt, s_ask.prompt, sizeof prompt - 1);
    prompt[n > 0 ? n : 0] = 0;
    n = utf16_to_utf8((uint8_t*)initial, s_ask.text, sizeof initial - 1);
    initial[n > 0 ? n : 0] = 0;

    SwkbdConfig kbd;
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) {
        nx_log("keyboard: the software keyboard could not be made (%#x); the game gets its initial text\n", rc);
        cellOskDialog_host_finish(1, s_ask.text, (uint32_t)u16len(s_ask.text));
        return NULL;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, prompt);
    swkbdConfigSetGuideText(&kbd, prompt);
    swkbdConfigSetInitialText(&kbd, initial);
    if (s_ask.limit) swkbdConfigSetStringLenMax(&kbd, s_ask.limit);
    rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);

    if (R_FAILED(rc)) {   /* the player pressed Cancel, or the applet did not run */
        nx_log("keyboard: closed without a text (%#x)\n", rc);
        cellOskDialog_host_finish(0, NULL, 0);
        return NULL;
    }
    uint16_t text[TEXT_MAX + 1];
    n = utf8_to_utf16(text, (const uint8_t*)out, TEXT_MAX);
    if (n < 0) n = 0;
    text[n] = 0;
    nx_log("keyboard: %zd characters entered\n", n);
    cellOskDialog_host_finish(1, text, (uint32_t)n);
    return NULL;
}

static void osk_open(const CellOskDialogChar* prompt, const CellOskDialogChar* text, uint32_t limit)
{
    memset(&s_ask, 0, sizeof s_ask);
    for (size_t i = 0; prompt && i < TEXT_MAX && prompt[i]; i++) s_ask.prompt[i] = prompt[i];
    for (size_t i = 0; text && i < TEXT_MAX && text[i]; i++) s_ask.text[i] = text[i];
    s_ask.limit = limit > TEXT_MAX ? TEXT_MAX : limit;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, keyboard, NULL) != 0) {
        nx_log("keyboard: no thread for it; the game gets its initial text\n");
        cellOskDialog_host_finish(1, s_ask.text, (uint32_t)u16len(s_ask.text));
    }
    pthread_attr_destroy(&attr);
}

static void osk_close(void)
{
    /* (an applet on the screen is the player's to close) */
}

void nx_osk_init(void)
{
    static const CellOskDialogHost host = {osk_open, osk_close};
    cellOskDialog_set_host(&host);
}
