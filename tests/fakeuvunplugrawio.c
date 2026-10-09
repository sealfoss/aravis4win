/* SPDX-License-Identifier:Unlicense */

/* The device loss cases of tests/fakeuvunplug.c in async USB mode with WinUSB's RAW_IO, which ARV_UV_RAW_IO turns on,
 * as a third program, so that meson runs it in parallel with the others */

#define FAKEUV_UNPLUG_RAW_IO
#include "fakeuvunplug.c"
