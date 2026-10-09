/* SPDX-License-Identifier:Unlicense */

/* The device loss cases of tests/fakeuvunplug.c in sync USB mode, as a second program, so that meson runs both
 * modes in parallel */

#define FAKEUV_UNPLUG_SYNC
#include "fakeuvunplug.c"
