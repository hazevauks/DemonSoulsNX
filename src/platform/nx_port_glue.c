/* nx_port_glue.c -- what Soulbrandt's game code takes from parts of the build the Switch does not have.
 *
 * Soulbrandt's PC build has a D3D12 renderer and a launcher UI (port/ui) that the Switch replaces
 * with its own. Its game code calls into both in a few places; these are the Switch's answers.
 */

#include "nx_paths.h"

/* des_prework.cpp, the shader cache's work at startup, asks the D3D12 renderer where its cache
 * is. The Switch's shader cache is a folder on the SD card, which the deko3D backend fills
 * (docs/engine.md). */
const char *rsx_d3d12_shader_cache_path(void)
{
    return NX_SHADER_CACHE_DIR;
}

/* des_options.cpp calls the UI's problem-report hook at every frame flip (ui/src/report.cpp).
 * The Switch writes its crash reports to SD_LOG instead, so there is nothing to do here. */
void des_report_flip_hook(void)
{
}

/* "des_boot --icon <game> <out>" draws the launcher's icon from the game's files on a PC
 * (ui/src/souls_icon.cpp). The NRO's icon is made on the PC (assets/icon.jpg): no such mode here. */
int des_icon_render(const char *game, const char *out)
{
    (void)game;
    (void)out;
    return 1;
}
