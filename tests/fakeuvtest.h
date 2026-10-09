/* SPDX-License-Identifier:Unlicense */

/* Helpers shared by the fake libusb based test programs (suite 'fakeuv'). To add a test program, write
 * tests/<name>.c using these helpers and add <name> to the fakeuv_tests list in tests/meson.build. Test programs are
 * built with ARAVIS_COMPILATION defined, so they can include Aravis's private headers (arvuvcpprivate.h...).
 *
 * A main () is typically:
 *
 *	fakeuv_test_init (&argc, &argv);
 *	fakeuv_test_add ("/fakeuvfoo/bar", bar_test);
 *	return fakeuv_test_run ();
 *
 * fakeuv_test_add () runs each test after fakeuv_test_reset (), then fails it if the libusb API was misused
 * (FAKE_USB_STAT_API_MISUSE, see fakeusb.h) more often than declared with fakeuv_test_expect_misuse ().
 * fakeuv_test_reset () checks the same before resetting, for tests that reset several times or were registered with
 * g_test_add_func (). fakeuv_test_run () also fails the program if a libusb context, handle or transfer is still
 * alive after arv_shutdown ().
 *
 * Set FAKEUV_DEBUG=1 to print each frame checked by fakeuv_buffer_matches_frame(), FAKE_USB_DEBUG=1 for the fake's
 * own log, and FAKEUV_TIME_SCALE=<n> to multiply every delay and timeout by n (valgrind runs use 10). */

#ifndef FAKEUV_TEST_H
#define FAKEUV_TEST_H

#include <arv.h>
#include "fakeusb.h"

G_BEGIN_DECLS

/* Frame period of the cameras created by fakeuv_add_camera(), short enough to keep the tests fast */
#define FAKEUV_FRAME_PERIOD_US		5000

/* How long a test waits at most for what it expects (frames, a buffer, a loss of control, a state of the fake), in ms,
 * to be scaled with fakeuv_timeout_ms(). Only a failing test waits that long, so it is generous: loaded CI runners and
 * sanitizer builds are slow, and valgrind runs use FAKEUV_TIME_SCALE=10. */
#define FAKEUV_TIMEOUT_MS		30000

#define FAKEUV_OTHER_DEVICE_VENDOR_ID	0x1209
#define FAKEUV_OTHER_DEVICE_PRODUCT_ID	0x0002

/* Whether arv_uv_device_check_connection () takes a request that fails otherwise than with LIBUSB_ERROR_NO_DEVICE for
 * a removal: only Windows builds of Aravis do, as libusb's Windows backend reports a removal that way. A test of the
 * Windows preset, where removals give LIBUSB_ERROR_IO, expects a loss of control with these builds only. */
#ifdef G_OS_WIN32
#define FAKEUV_IO_ERROR_IS_LOSS		TRUE
#else
#define FAKEUV_IO_ERROR_IS_LOSS		FALSE
#endif

typedef gboolean (*FakeuvCondition) (gpointer user_data);

typedef struct {
	gint n_init;
	gint n_exit;
	gint n_start_buffer;
	gint n_buffer_done;
} FakeuvStreamEvents;

/* Program setup: g_test_init(), GenICam file of the fake camera, only the USB3Vision interface enabled, time scale.
 * fakeuv_test_run() runs the tests, then shuts Aravis down and resets the fake. */

void		fakeuv_test_init		(int *argc, char ***argv);
void		fakeuv_test_add			(const char *path, GTestFunc test_func);
int		fakeuv_test_run			(void);

/* Test case setup: removes every fake device and restores the fake defaults, and Aravis's: ARV_UV_RAW_IO unset (RAW_IO
 * off), no location of a camera known from an earlier discovery or open */

void		fakeuv_test_reset		(void);
void		fakeuv_test_expect_misuse	(guint64 n_misuse);

/* A timeout, multiplied by FAKEUV_TIME_SCALE */

guint		fakeuv_timeout_ms		(guint timeout_ms);

FakeUsbDevice *	fakeuv_add_camera		(const char *serial_number);
FakeUsbDevice *	fakeuv_add_other_device		(void);
ArvCamera *	fakeuv_open_camera		(FakeUsbDevice *camera, GError **error);

gboolean	fakeuv_wait_for			(FakeuvCondition condition, gpointer user_data, guint timeout_ms);

/* Stream callback counting the ArvStreamCallbackType events into a FakeuvStreamEvents */

void		fakeuv_stream_callback		(void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer);

/* Gives a buffer popped from an async USB stream back to it, once the libusb event round of @camera that ran its
 * trailer callback is over (FAKEUV_TIMEOUT_MS at most). Not needed any more: the trailer callback used to push a
 * buffer to the output queue before detaching it from its transfer context, so that a buffer pushed back at once
 * could be lost; it detaches it first now, and both modes take buffers back at once
 * (tests/fakeuvstream.c, /fakeuvstream/trailer-race). Kept for tests that want the old pacing. */

void		fakeuv_recycle_buffer		(FakeUsbDevice *camera, ArvStream *stream, ArvBuffer *buffer);

/* Checks that a buffer is a successful frame whose payload is the one the fake camera sent */

gboolean	fakeuv_buffer_matches_frame	(FakeUsbDevice *camera, ArvBuffer *buffer);
void		fakeuv_assert_frame		(FakeUsbDevice *camera, ArvBuffer *buffer,
						 guint32 width, guint32 height, ArvPixelFormat pixel_format);

/* Acquires @n_frames frames with @n_buffers buffers in @usb_mode, checking every one of them; returns the number
 * of frames received before @timeout_ms (scaled) */

guint		fakeuv_acquire_frames		(ArvCamera *camera, FakeUsbDevice *device, ArvUvUsbMode usb_mode,
						 guint n_buffers, guint n_frames, guint timeout_ms);

G_END_DECLS

#endif
