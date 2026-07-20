// Display.h — selects the panel driver at compile time. The driver headers
// share one API (display_init/set_contrast/power/task + DISP_W/DISP_H), so
// the rest of the firmware includes this header and stays panel-agnostic.
// Pick the panel in platformio.ini (DISPLAY SELECT).
#pragma once

#if defined(DISPLAY_ST7789)
#include "DisplayST7789.h" // v1 prototype: ST7789, 284x76 landscape
#else
#include "DisplayNV3007.h" // current: NV3007, 428x142 landscape
#endif
