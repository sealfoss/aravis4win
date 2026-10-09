/* SPDX-License-Identifier:Unlicense */

/*
 * Mock GenTL producer, see arvgentlmock.h for its description and its options.
 *
 * It implements the GenTL C API as declared by src/arvgentlprivate.h. Every handle is validated before use, and the
 * misuses of the API by the consumer are counted as violations instead of crashing the producer. All the state is
 * protected by a single mutex, released only while a thread waits in EventGetData. Frames are produced on demand, in
 * EventGetData, once the frame period has elapsed and a buffer is queued. The image of a frame is filled without the
 * mutex, which the other threads would otherwise wait for during most of a short frame period: the functions flushing
 * or freeing a data stream wait for the end of its fill.
 */

#define G_LOG_DOMAIN "ArvGenTLMock"

#include <arv.h>
#include <arvgentlprivate.h>
#include <stdarg.h>
#include <string.h>

#include "arvgentlmock.h"

#if defined (ARV_GENTL_MOCK_DEP)
#define MOCK_VARIANT "Dep"
#elif defined (ARV_GENTL_MOCK_BROKEN)
#define MOCK_VARIANT "Broken"
#elif defined (ARV_GENTL_MOCK_MINIMAL)
#define MOCK_VARIANT "Minimal"
#else
#define MOCK_VARIANT ""
#endif

/* The minimal variant omits the functions the consumer treats as optional. EventGetInfo and EventGetDataInfo are
 * optional without event support only. */
#ifdef ARV_GENTL_MOCK_MINIMAL
#define MOCK_HAS_OPTIONAL_FUNCTIONS 0
#else
#define MOCK_HAS_OPTIONAL_FUNCTIONS 1
#endif
#define MOCK_HAS_EVENT_INFO_FUNCTIONS (MOCK_HAS_OPTIONAL_FUNCTIONS || ARAVIS_HAS_EVENT)

#define MOCK_TL_ID			"ArvGenTLMock" MOCK_VARIANT
#define MOCK_TL_VENDOR			ARV_GENTL_MOCK_TL_VENDOR MOCK_VARIANT
#define MOCK_FILENAME			"ArvGenTLMock" MOCK_VARIANT ".cti"
#define MOCK_TIMESTAMP_FREQUENCY	G_GUINT64_CONSTANT (1000000000)
#define MOCK_DEFAULT_FRAME_PERIOD_US	40000
#define MOCK_EVENT_DATA_SIZE_MAX	64
/* Hexadecimal digits of the id of the remote device events */
#define MOCK_DEVICE_EVENT_ID_LENGTH_MAX	8

#ifdef ARV_GENTL_MOCK_DEP
__declspec(dllimport) int arv_gentl_mock_helper_get_answer (void);
#endif

ARV_API int		arv_gentl_mock_get_counter		(const char *name);
ARV_API const char *	arv_gentl_mock_get_last_violation	(void);
ARV_API gboolean	arv_gentl_mock_set_option		(const char *name, const char *value);
ARV_API void		arv_gentl_mock_reset_counters		(void);

/* GenTL functions, in the order of src/arvgentlprivate.h */

#define MOCK_FUNCTIONS(F) \
	F (GCCloseLib) F (GCGetInfo) F (GCGetLastError) F (GCInitLib) \
	F (TLClose) F (TLGetInfo) F (TLGetInterfaceID) F (TLGetInterfaceInfo) F (TLGetNumInterfaces) F (TLOpen) \
	F (TLOpenInterface) F (TLUpdateInterfaceList) \
	F (IFClose) F (IFGetInfo) F (IFGetDeviceID) F (IFGetDeviceInfo) F (IFGetNumDevices) F (IFOpenDevice) \
	F (IFUpdateDeviceList) F (IFGetParentTL) \
	F (DevClose) F (DevGetInfo) F (DevGetDataStreamID) F (DevGetNumDataStreams) F (DevGetPort) \
	F (DevOpenDataStream) F (DevGetParentIF) \
	F (DSAllocAndAnnounceBuffer) F (DSAnnounceBuffer) F (DSAnnounceCompositeBuffer) F (DSClose) \
	F (DSFlushQueue) F (DSGetBufferID) F (DSGetBufferInfo) F (DSGetInfo) F (DSQueueBuffer) F (DSRevokeBuffer) \
	F (DSStartAcquisition) F (DSStopAcquisition) F (DSGetBufferChunkData) F (DSGetParentDev) \
	F (DSGetNumBufferParts) F (DSGetBufferPartInfo) F (DSGetBufferInfoStacked) F (DSGetBufferPartInfoStacked) \
	F (DSGetNumFlows) F (DSGetFlowInfo) F (DSGetNumBufferSegments) F (DSGetBufferSegmentInfo) \
	F (GCGetPortInfo) F (GCGetPortURL) F (GCGetNumPortURLs) F (GCGetPortURLInfo) F (GCReadPort) \
	F (GCWritePort) F (GCWritePortStacked) F (GCReadPortStacked) \
	F (EventFlush) F (EventGetData) F (EventGetDataInfo) F (EventGetInfo) F (EventKill) F (GCRegisterEvent) \
	F (GCUnregisterEvent)

#define MOCK_FUNCTION_ID(function) MOCK_FUNCTION_##function,
#define MOCK_FUNCTION_NAME(function) #function,

typedef enum {
	MOCK_FUNCTIONS (MOCK_FUNCTION_ID)
	MOCK_N_FUNCTIONS
} MockFunction;

static const char *mock_function_names[MOCK_N_FUNCTIONS] = {
	MOCK_FUNCTIONS (MOCK_FUNCTION_NAME)
};

/* String informations, which can be scripted to be missing or empty. The order of each group follows the GenTL
 * command values. */

#define MOCK_INFOS(I) \
	I (TL_INFO_ID) I (TL_INFO_VENDOR) I (TL_INFO_MODEL) I (TL_INFO_VERSION) I (TL_INFO_TLTYPE) I (TL_INFO_NAME) \
	I (TL_INFO_PATHNAME) I (TL_INFO_DISPLAYNAME) \
	I (INTERFACE_INFO_ID) I (INTERFACE_INFO_DISPLAYNAME) I (INTERFACE_INFO_TLTYPE) \
	I (DEVICE_INFO_ID) I (DEVICE_INFO_VENDOR) I (DEVICE_INFO_MODEL) I (DEVICE_INFO_TLTYPE) \
	I (DEVICE_INFO_DISPLAYNAME) I (DEVICE_INFO_ACCESS_STATUS) I (DEVICE_INFO_USER_DEFINED_NAME) \
	I (DEVICE_INFO_SERIAL_NUMBER) I (DEVICE_INFO_VERSION) I (DEVICE_INFO_TIMESTAMP_FREQUENCY)

#define MOCK_INFO_ID(info) MOCK_##info,
#define MOCK_INFO_NAME(info) #info,

typedef enum {
	MOCK_INFOS (MOCK_INFO_ID)
	MOCK_N_INFOS
} MockInfo;

static const char *mock_info_names[MOCK_N_INFOS] = {
	MOCK_INFOS (MOCK_INFO_NAME)
};

typedef enum {
	MOCK_EVENT_KILL_LATCH,
	MOCK_EVENT_KILL_WAITERS,
	MOCK_EVENT_KILL_IGNORE
} MockEventKillMode;

typedef struct {
	guint n_devices;
	guint n_interfaces;
	guint32 missing_infos;
	guint32 empty_infos;
	guint fail_from[MOCK_N_FUNCTIONS];
	GC_ERROR fail_error[MOCK_N_FUNCTIONS];
	MockEventKillMode event_kill;
	guint incomplete_every;
	guint64 frame_period_us;
	guint buf_announce_min;
	gint n_parts;
	guint64 close_delay_us;
	char device_event_id[MOCK_DEVICE_EVENT_ID_LENGTH_MAX + 1];	/* Empty: no remote device event */
	guint64 size_excess;
	guint64 image_offset;
} MockConfig;

static const char *mock_option_names[] = {
	"ARV_GENTL_MOCK_N_DEVICES",
	"ARV_GENTL_MOCK_N_INTERFACES",
	"ARV_GENTL_MOCK_MISSING_INFO",
	"ARV_GENTL_MOCK_EMPTY_INFO",
	"ARV_GENTL_MOCK_FAIL",
	"ARV_GENTL_MOCK_EVENT_KILL",
	"ARV_GENTL_MOCK_INCOMPLETE_EVERY",
	"ARV_GENTL_MOCK_FRAME_PERIOD_US",
	"ARV_GENTL_MOCK_BUF_ANNOUNCE_MIN",
	"ARV_GENTL_MOCK_N_PARTS",
	"ARV_GENTL_MOCK_CLOSE_DELAY_US",
	"ARV_GENTL_MOCK_DEVICE_EVENT_ID",
	"ARV_GENTL_MOCK_SIZE_EXCESS",
	"ARV_GENTL_MOCK_IMAGE_OFFSET"
};

typedef enum {
	MOCK_HANDLE_NONE = 0,
	MOCK_HANDLE_SYSTEM,
	MOCK_HANDLE_INTERFACE,
	MOCK_HANDLE_DEVICE,
	MOCK_HANDLE_PORT,
	MOCK_HANDLE_STREAM,
	MOCK_HANDLE_BUFFER,
	MOCK_HANDLE_EVENT
} MockHandleType;

typedef struct _MockDevice MockDevice;
typedef struct _MockStream MockStream;

typedef struct {
	MockDevice *device;
} MockPort;

/* An interface module. Its address is its handle. */
typedef struct {
	guint index;
	char id[32];
	gboolean is_open;
} MockInterface;

struct _MockDevice {
	guint index;			/* In its interface */
	MockInterface *interface;
	char *id;
	char *serial_nbr;
	char *display_name;
	gboolean is_open;
	MockPort port;
	ArvFakeCamera *camera;
	MockStream *stream;
};

typedef enum {
	MOCK_BUFFER_IDLE,		/* Announced, owned by the consumer */
	MOCK_BUFFER_QUEUED,		/* In the input queue */
	MOCK_BUFFER_FILLING,		/* Out of the input queue, being filled */
	MOCK_BUFFER_DELIVERED		/* In the output queue */
} MockBufferState;

typedef struct {
	MockStream *stream;
	void *data;
	size_t size;
	gboolean is_owned;
	void *user_pointer;
	MockBufferState state;

	gboolean has_new_data;
	gboolean is_incomplete;
	gboolean is_data_larger;
	size_t size_filled;
	size_t data_size;
	size_t image_offset;
	guint64 frame_id;
	guint64 timestamp_ns;
	guint32 width;
	guint32 height;
	guint32 x_offset;
	guint32 y_offset;
	guint32 pixel_format;
} MockBuffer;

typedef struct {
	EVENT_TYPE type;
	gpointer module;		/* NULL once the module is closed */
	gboolean is_registered;
	gboolean is_kill_pending;
	gboolean is_zombie;		/* Freed by the last waiting thread */
	guint n_waiters;
	guint64 n_fired;
	gint64 next_event_time;		/* Remote device events of a device: time of the next one */
	GCond cond;
} MockEvent;

struct _MockStream {
	MockDevice *device;
	GList *buffers;
	GQueue input;
	GQueue output;
	gboolean is_acquiring;
	guint64 num_to_acquire;
	guint64 n_acquired;
	gint64 next_frame_time;
	guint64 frame_counter;
	guint64 n_delivered;
	guint64 n_underruns;
	guint64 n_started;
	gboolean is_filling;		/* A buffer is filled without the mock mutex */
	MockEvent *new_buffer_event;
};

static GMutex mock_mutex;
/* Signaled at the end of each fill */
static GCond mock_fill_cond;

/* Counters, kept until arv_gentl_mock_reset_counters () */
static guint mock_n_calls[MOCK_N_FUNCTIONS];
static guint mock_n_violations = 0;
static guint mock_n_kills_unregistered = 0;
static guint mock_n_frames = 0;
static guint mock_n_underruns = 0;
static guint mock_n_new_buffer_waits = 0;
static guint mock_n_device_events = 0;
static char mock_last_violation[512] = "";

/* Session state, between GCInitLib and GCCloseLib */
static gboolean mock_is_initialized = FALSE;
static MockConfig mock_config;
static guint mock_session_calls[MOCK_N_FUNCTIONS];
static GHashTable *mock_handles = NULL;
static int mock_system_tag;
static gboolean mock_system_is_open = FALSE;
static MockInterface mock_interfaces[ARV_GENTL_MOCK_MAX_INTERFACES];
static guint mock_n_interfaces = 0;
/* The devices of all the interfaces, interface by interface */
static MockDevice *mock_devices[ARV_GENTL_MOCK_MAX_INTERFACES * ARV_GENTL_MOCK_MAX_DEVICES];
static guint mock_n_devices = 0;
static GList *mock_events = NULL;
static GC_ERROR mock_last_error = GC_ERR_SUCCESS;
static char mock_last_error_text[256] = "";

static void mock_violation (const char *format, ...) G_GNUC_PRINTF (1, 2);

static void
mock_violation (const char *format, ...)
{
	va_list args;

	va_start (args, format);
	g_vsnprintf (mock_last_violation, sizeof (mock_last_violation), format, args);
	va_end (args);

	mock_n_violations++;

	g_debug ("Violation: %s", mock_last_violation);
}

/* Configuration */

static void
mock_config_reset (MockConfig *config)
{
	memset (config, 0, sizeof (MockConfig));

	config->n_devices = 1;
	config->n_interfaces = 1;
	config->event_kill = MOCK_EVENT_KILL_LATCH;
	config->buf_announce_min = 1;
	config->n_parts = -1;
}

static gboolean
mock_parse_infos (const char *value, guint32 *infos)
{
	char **entries;
	gboolean success = TRUE;
	guint i, j;

	*infos = 0;

	if (value == NULL)
		return TRUE;

	entries = g_strsplit (value, ",", -1);
	for (i = 0; entries[i] != NULL; i++) {
		char *entry = g_strstrip (entries[i]);
		gboolean found = FALSE;

		if (entry[0] == '\0')
			continue;

		for (j = 0; j < MOCK_N_INFOS && !found; j++) {
			if (g_strcmp0 (entry, mock_info_names[j]) == 0) {
				*infos |= 1u << j;
				found = TRUE;
			}
		}

		if (!found) {
			g_debug ("Unknown info '%s'", entry);
			success = FALSE;
		}
	}
	g_strfreev (entries);

	return success;
}

static gboolean
mock_parse_failures (MockConfig *config, const char *value)
{
	char **entries;
	gboolean success = TRUE;
	guint i, j;

	memset (config->fail_from, 0, sizeof (config->fail_from));
	memset (config->fail_error, 0, sizeof (config->fail_error));

	if (value == NULL)
		return TRUE;

	entries = g_strsplit (value, ",", -1);
	for (i = 0; entries[i] != NULL; i++) {
		char *entry = g_strstrip (entries[i]);
		char *separator;
		guint64 from = 1;
		gint64 error = GC_ERR_ERROR;
		gboolean found = FALSE;

		if (entry[0] == '\0')
			continue;

		separator = strchr (entry, ':');
		if (separator != NULL) {
			*separator = '\0';
			error = g_ascii_strtoll (separator + 1, NULL, 10);
		}
		separator = strchr (entry, '@');
		if (separator != NULL) {
			*separator = '\0';
			from = g_ascii_strtoull (separator + 1, NULL, 10);
		}

		for (j = 0; j < MOCK_N_FUNCTIONS && !found; j++) {
			if (g_strcmp0 (entry, mock_function_names[j]) == 0) {
				config->fail_from[j] = (guint) MAX (from, 1);
				config->fail_error[j] = (GC_ERROR) error;
				/* The call count starts with the option change */
				mock_session_calls[j] = 0;
				found = TRUE;
			}
		}

		if (!found) {
			g_debug ("Unknown function '%s'", entry);
			success = FALSE;
		}
	}
	g_strfreev (entries);

	return success;
}

static gboolean
mock_config_set (MockConfig *config, const char *name, const char *value)
{
	if (g_strcmp0 (name, "ARV_GENTL_MOCK_N_DEVICES") == 0) {
		guint64 n_devices = value != NULL ? g_ascii_strtoull (value, NULL, 10) : 1;

		config->n_devices = (guint) MIN (n_devices, ARV_GENTL_MOCK_MAX_DEVICES);
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_N_INTERFACES") == 0) {
		guint64 n_interfaces = value != NULL ? g_ascii_strtoull (value, NULL, 10) : 1;

		config->n_interfaces = (guint) CLAMP (n_interfaces, 1, ARV_GENTL_MOCK_MAX_INTERFACES);
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_MISSING_INFO") == 0) {
		return mock_parse_infos (value, &config->missing_infos);
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_EMPTY_INFO") == 0) {
		return mock_parse_infos (value, &config->empty_infos);
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_FAIL") == 0) {
		return mock_parse_failures (config, value);
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_EVENT_KILL") == 0) {
		if (value == NULL || g_strcmp0 (value, "latch") == 0)
			config->event_kill = MOCK_EVENT_KILL_LATCH;
		else if (g_strcmp0 (value, "waiters") == 0)
			config->event_kill = MOCK_EVENT_KILL_WAITERS;
		else if (g_strcmp0 (value, "ignore") == 0)
			config->event_kill = MOCK_EVENT_KILL_IGNORE;
		else
			return FALSE;
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_INCOMPLETE_EVERY") == 0) {
		config->incomplete_every = value != NULL ? (guint) g_ascii_strtoull (value, NULL, 10) : 0;
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_FRAME_PERIOD_US") == 0) {
		config->frame_period_us = value != NULL ? g_ascii_strtoull (value, NULL, 10) : 0;
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_BUF_ANNOUNCE_MIN") == 0) {
		config->buf_announce_min = value != NULL ? (guint) g_ascii_strtoull (value, NULL, 10) : 1;
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_N_PARTS") == 0) {
		if (value == NULL || g_strcmp0 (value, "none") == 0)
			config->n_parts = -1;
		else
			config->n_parts = (gint) MIN (g_ascii_strtoull (value, NULL, 10), 1);
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_CLOSE_DELAY_US") == 0) {
		config->close_delay_us = value != NULL ? g_ascii_strtoull (value, NULL, 10) : 0;
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_DEVICE_EVENT_ID") == 0) {
		size_t length = value != NULL ? strlen (value) : 0;
		size_t i;

		if (length > MOCK_DEVICE_EVENT_ID_LENGTH_MAX)
			return FALSE;
		for (i = 0; i < length; i++)
			if (!g_ascii_isxdigit (value[i]))
				return FALSE;

		memset (config->device_event_id, 0, sizeof (config->device_event_id));
		if (length > 0)
			memcpy (config->device_event_id, value, length);
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_SIZE_EXCESS") == 0) {
		config->size_excess = value != NULL ? g_ascii_strtoull (value, NULL, 10) : 0;
		return TRUE;
	} else if (g_strcmp0 (name, "ARV_GENTL_MOCK_IMAGE_OFFSET") == 0) {
		guint64 image_offset = value != NULL ? g_ascii_strtoull (value, NULL, 10) : 0;

		if (image_offset > ARV_GENTL_MOCK_IMAGE_OFFSET_MAX)
			return FALSE;
		config->image_offset = image_offset;
		return TRUE;
	}

	return FALSE;
}

static void
mock_config_apply_environment (MockConfig *config)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS (mock_option_names); i++) {
		const char *value = g_getenv (mock_option_names[i]);

		if (value != NULL && !mock_config_set (config, mock_option_names[i], value))
			g_debug ("Invalid value '%s' for %s", value, mock_option_names[i]);
	}
}

/* Handles */

static void
mock_handle_add (gconstpointer handle, MockHandleType type)
{
	g_hash_table_insert (mock_handles, (gpointer) handle, GINT_TO_POINTER (type));
}

static void
mock_handle_remove (gconstpointer handle)
{
	if (mock_handles != NULL)
		g_hash_table_remove (mock_handles, handle);
}

static MockHandleType
mock_handle_get_type (gconstpointer handle)
{
	if (handle == NULL || mock_handles == NULL)
		return MOCK_HANDLE_NONE;

	return GPOINTER_TO_INT (g_hash_table_lookup (mock_handles, handle));
}

static gboolean
mock_check_handle (gconstpointer handle, MockHandleType type, MockFunction function)
{
	if (mock_handle_get_type (handle) == type)
		return TRUE;

	mock_violation ("%s: invalid handle %p", mock_function_names[function], handle);

	return FALSE;
}

/* Entry and exit of every GenTL function: lock, count the call, check initialization, apply scripted failures. */

static gboolean
mock_enter (MockFunction function, GC_ERROR *error)
{
	g_mutex_lock (&mock_mutex);

	mock_n_calls[function]++;
	mock_session_calls[function]++;

	if (!mock_is_initialized &&
	    function != MOCK_FUNCTION_GCGetInfo &&
	    function != MOCK_FUNCTION_GCGetLastError) {
		mock_violation ("%s called while the producer is not initialized", mock_function_names[function]);
		*error = GC_ERR_NOT_INITIALIZED;
		g_mutex_unlock (&mock_mutex);
		return FALSE;
	}

	if (mock_config.fail_from[function] > 0 &&
	    mock_session_calls[function] >= mock_config.fail_from[function]) {
		*error = mock_config.fail_error[function];
		mock_last_error = *error;
		g_snprintf (mock_last_error_text, sizeof (mock_last_error_text), "%s: scripted failure",
			    mock_function_names[function]);
		g_mutex_unlock (&mock_mutex);
		return FALSE;
	}

	return TRUE;
}

static void
mock_leave (MockFunction function, GC_ERROR error)
{
	if (error != GC_ERR_SUCCESS) {
		mock_last_error = error;
		g_snprintf (mock_last_error_text, sizeof (mock_last_error_text), "%s failed (%d)",
			    mock_function_names[function], error);
	}

	g_mutex_unlock (&mock_mutex);
}

#define MOCK_ENTER(function) \
	const MockFunction mock_function = MOCK_FUNCTION_##function; \
	GC_ERROR mock_error; \
	if (!mock_enter (mock_function, &mock_error)) \
		return mock_error

#define MOCK_RETURN(expression) \
	G_STMT_START { \
		mock_error = (expression); \
		mock_leave (mock_function, mock_error); \
		return mock_error; \
	} G_STMT_END

#define MOCK_CHECK(handle,type) \
	G_STMT_START { \
		if (!mock_check_handle ((handle), (type), mock_function)) \
			MOCK_RETURN (GC_ERR_INVALID_HANDLE); \
	} G_STMT_END

#define MOCK_CHECK_PARAMETER(condition) \
	G_STMT_START { \
		if (!(condition)) \
			MOCK_RETURN (GC_ERR_INVALID_PARAMETER); \
	} G_STMT_END

/* Scripted delay of the close functions, before their handle check: the other threads run meanwhile */

static void
mock_close_delay (void)
{
	guint64 delay_us = mock_config.close_delay_us;

	if (delay_us > 0) {
		g_mutex_unlock (&mock_mutex);
		g_usleep ((gulong) MIN (delay_us, G_USEC_PER_SEC));
		g_mutex_lock (&mock_mutex);
	}
}

/* Info output, following the GenTL convention: a NULL buffer only returns the size */

static GC_ERROR
mock_output (INFO_DATATYPE type, const void *value, size_t size,
	     INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	if (piSize == NULL)
		return GC_ERR_INVALID_PARAMETER;

	if (piType != NULL)
		*piType = type;

	if (pBuffer == NULL) {
		*piSize = size;
		return GC_ERR_SUCCESS;
	}

	if (*piSize < size) {
		*piSize = size;
		return GC_ERR_BUFFER_TOO_SMALL;
	}

	memcpy (pBuffer, value, size);
	*piSize = size;

	return GC_ERR_SUCCESS;
}

static GC_ERROR
mock_output_string (const char *value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	return mock_output (INFO_DATATYPE_STRING, value, strlen (value) + 1, piType, pBuffer, piSize);
}

static GC_ERROR
mock_output_bool8 (gboolean value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	bool8_t data = value ? 1 : 0;

	return mock_output (INFO_DATATYPE_BOOL8, &data, sizeof (data), piType, pBuffer, piSize);
}

static GC_ERROR
mock_output_int32 (gint32 value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	int32_t data = value;

	return mock_output (INFO_DATATYPE_INT32, &data, sizeof (data), piType, pBuffer, piSize);
}

static GC_ERROR
mock_output_uint32 (guint32 value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	uint32_t data = value;

	return mock_output (INFO_DATATYPE_UINT32, &data, sizeof (data), piType, pBuffer, piSize);
}

static GC_ERROR
mock_output_uint64 (guint64 value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	uint64_t data = value;

	return mock_output (INFO_DATATYPE_UINT64, &data, sizeof (data), piType, pBuffer, piSize);
}

static GC_ERROR
mock_output_sizet (size_t value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	return mock_output (INFO_DATATYPE_SIZET, &value, sizeof (value), piType, pBuffer, piSize);
}

static GC_ERROR
mock_output_ptr (void *value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	return mock_output (INFO_DATATYPE_PTR, &value, sizeof (value), piType, pBuffer, piSize);
}

static gboolean
mock_is_info_missing (MockInfo info)
{
	return (mock_config.missing_infos & (1u << info)) != 0;
}

static GC_ERROR
mock_info_text (MockInfo info, const char *value, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	if (mock_is_info_missing (info))
		return GC_ERR_NOT_AVAILABLE;

	if ((mock_config.empty_infos & (1u << info)) != 0)
		value = "";

	return mock_output_string (value, piType, pBuffer, piSize);
}

static GC_ERROR
mock_tl_info (TL_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	switch (info_cmd) {
		case TL_INFO_ID:
			return mock_info_text (MOCK_TL_INFO_ID, MOCK_TL_ID, piType, pBuffer, piSize);
		case TL_INFO_VENDOR:
			return mock_info_text (MOCK_TL_INFO_VENDOR, MOCK_TL_VENDOR, piType, pBuffer, piSize);
		case TL_INFO_MODEL:
			return mock_info_text (MOCK_TL_INFO_MODEL, "ArvGenTLMock", piType, pBuffer, piSize);
		case TL_INFO_VERSION:
			return mock_info_text (MOCK_TL_INFO_VERSION, "1.0", piType, pBuffer, piSize);
		case TL_INFO_TLTYPE:
			return mock_info_text (MOCK_TL_INFO_TLTYPE, ARV_GENTL_MOCK_TL_TYPE, piType, pBuffer, piSize);
		case TL_INFO_NAME:
			return mock_info_text (MOCK_TL_INFO_NAME, MOCK_FILENAME, piType, pBuffer, piSize);
		case TL_INFO_PATHNAME:
			return mock_info_text (MOCK_TL_INFO_PATHNAME, MOCK_FILENAME, piType, pBuffer, piSize);
		case TL_INFO_DISPLAYNAME:
			return mock_info_text (MOCK_TL_INFO_DISPLAYNAME, "Aravis GenTL mock producer",
					       piType, pBuffer, piSize);
		case TL_INFO_CHAR_ENCODING:
			return mock_output_int32 (TL_CHAR_ENCODING_ASCII, piType, pBuffer, piSize);
		case TL_INFO_GENTL_VER_MAJOR:
			return mock_output_uint32 (1, piType, pBuffer, piSize);
		case TL_INFO_GENTL_VER_MINOR:
			return mock_output_uint32 (5, piType, pBuffer, piSize);
		default:
			return GC_ERR_INVALID_PARAMETER;
	}
}

static GC_ERROR
mock_interface_info (MockInterface *interface, INTERFACE_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer,
		     size_t *piSize)
{
	switch (info_cmd) {
		case INTERFACE_INFO_ID:
			return mock_info_text (MOCK_INTERFACE_INFO_ID, interface->id,
					       piType, pBuffer, piSize);
		case INTERFACE_INFO_DISPLAYNAME:
			return mock_info_text (MOCK_INTERFACE_INFO_DISPLAYNAME, "Mock interface",
					       piType, pBuffer, piSize);
		case INTERFACE_INFO_TLTYPE:
			return mock_info_text (MOCK_INTERFACE_INFO_TLTYPE, ARV_GENTL_MOCK_TL_TYPE,
					       piType, pBuffer, piSize);
		default:
			return GC_ERR_INVALID_PARAMETER;
	}
}

static GC_ERROR
mock_device_info (MockDevice *device, DEVICE_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer,
		  size_t *piSize)
{
	switch (info_cmd) {
		case DEVICE_INFO_ID:
			return mock_info_text (MOCK_DEVICE_INFO_ID, device->id, piType, pBuffer, piSize);
		case DEVICE_INFO_VENDOR:
			return mock_info_text (MOCK_DEVICE_INFO_VENDOR, ARV_GENTL_MOCK_VENDOR,
					       piType, pBuffer, piSize);
		case DEVICE_INFO_MODEL:
			return mock_info_text (MOCK_DEVICE_INFO_MODEL, ARV_GENTL_MOCK_MODEL,
					       piType, pBuffer, piSize);
		case DEVICE_INFO_TLTYPE:
			return mock_info_text (MOCK_DEVICE_INFO_TLTYPE, ARV_GENTL_MOCK_TL_TYPE,
					       piType, pBuffer, piSize);
		case DEVICE_INFO_DISPLAYNAME:
			return mock_info_text (MOCK_DEVICE_INFO_DISPLAYNAME, device->display_name,
					       piType, pBuffer, piSize);
		case DEVICE_INFO_ACCESS_STATUS:
			if (mock_is_info_missing (MOCK_DEVICE_INFO_ACCESS_STATUS))
				return GC_ERR_NOT_AVAILABLE;
			return mock_output_int32 (device->is_open ?
						  DEVICE_ACCESS_STATUS_OPEN_READWRITE :
						  DEVICE_ACCESS_STATUS_READWRITE,
						  piType, pBuffer, piSize);
		case DEVICE_INFO_USER_DEFINED_NAME:
			return GC_ERR_NOT_AVAILABLE;
		case DEVICE_INFO_SERIAL_NUMBER:
			return mock_info_text (MOCK_DEVICE_INFO_SERIAL_NUMBER, device->serial_nbr,
					       piType, pBuffer, piSize);
		case DEVICE_INFO_VERSION:
			return mock_info_text (MOCK_DEVICE_INFO_VERSION, "1.0", piType, pBuffer, piSize);
		case DEVICE_INFO_TIMESTAMP_FREQUENCY:
			if (mock_is_info_missing (MOCK_DEVICE_INFO_TIMESTAMP_FREQUENCY))
				return GC_ERR_NOT_AVAILABLE;
			return mock_output_uint64 (MOCK_TIMESTAMP_FREQUENCY, piType, pBuffer, piSize);
		default:
			return GC_ERR_INVALID_PARAMETER;
	}
}

static MockInterface *
mock_find_interface (const char *id)
{
	guint i;

	for (i = 0; i < mock_n_interfaces; i++)
		if (g_strcmp0 (mock_interfaces[i].id, id) == 0)
			return &mock_interfaces[i];

	return NULL;
}

/* Devices are found by their id in their interface: the interfaces have the same device ids */

static MockDevice *
mock_find_device (MockInterface *interface, const char *id)
{
	guint i;

	for (i = 0; i < mock_n_devices; i++)
		if (mock_devices[i]->interface == interface && g_strcmp0 (mock_devices[i]->id, id) == 0)
			return mock_devices[i];

	return NULL;
}

static MockDevice *
mock_get_interface_device (MockInterface *interface, guint index)
{
	guint i;

	for (i = 0; i < mock_n_devices; i++)
		if (mock_devices[i]->interface == interface && mock_devices[i]->index == index)
			return mock_devices[i];

	return NULL;
}

static guint
mock_get_interface_n_devices (MockInterface *interface)
{
	guint n_devices = 0;
	guint i;

	for (i = 0; i < mock_n_devices; i++)
		if (mock_devices[i]->interface == interface)
			n_devices++;

	return n_devices;
}

/* Object lifecycle. Closing a module before its children is a violation, the children are then closed. */

static void
mock_detach_events (gconstpointer module, const char *module_name)
{
	GList *iter;

	for (iter = mock_events; iter != NULL; iter = iter->next) {
		MockEvent *event = iter->data;

		if (event->module == module) {
			if (event->is_registered)
				mock_violation ("%s closed with a registered event %d", module_name, event->type);
			event->module = NULL;
			event->is_registered = FALSE;
			g_cond_broadcast (&event->cond);
		}
	}
}

static void
mock_event_free (MockEvent *event)
{
	g_cond_clear (&event->cond);
	g_free (event);
}

static void
mock_buffer_free (MockBuffer *buffer)
{
	mock_handle_remove (buffer);
	if (buffer->is_owned)
		g_free (buffer->data);
	g_free (buffer);
}

/* Waits for the end of the fill of a buffer of the stream, done by a thread waiting in EventGetData */

static void
mock_stream_wait_fill (MockStream *stream)
{
	while (stream->is_filling)
		g_cond_wait (&mock_fill_cond, &mock_mutex);
}

static void
mock_stream_free (MockStream *stream)
{
	mock_stream_wait_fill (stream);

	mock_detach_events (stream, "Data stream");

	g_queue_clear (&stream->input);
	g_queue_clear (&stream->output);
	g_list_free_full (stream->buffers, (GDestroyNotify) mock_buffer_free);
	stream->buffers = NULL;

	mock_handle_remove (stream);
	stream->device->stream = NULL;

	g_free (stream);
}

static void
mock_device_close (MockDevice *device)
{
	if (device->stream != NULL) {
		mock_violation ("Device %s closed with an open data stream", device->id);
		mock_stream_free (device->stream);
	}

	mock_detach_events (device, "Device");
	mock_handle_remove (&device->port);
	mock_handle_remove (device);
	g_clear_object (&device->camera);
	device->is_open = FALSE;
}

static void
mock_interface_close (MockInterface *interface)
{
	guint i;

	for (i = 0; i < mock_n_devices; i++) {
		if (mock_devices[i]->interface == interface && mock_devices[i]->is_open) {
			mock_violation ("Interface closed with the open device %s", mock_devices[i]->id);
			mock_device_close (mock_devices[i]);
		}
	}

	mock_detach_events (interface, "Interface");
	mock_handle_remove (interface);
	interface->is_open = FALSE;
}

static void
mock_system_close (void)
{
	guint i;

	for (i = 0; i < mock_n_interfaces; i++) {
		if (mock_interfaces[i].is_open) {
			mock_violation ("System closed with an open interface");
			mock_interface_close (&mock_interfaces[i]);
		}
	}

	mock_detach_events (&mock_system_tag, "System");
	mock_handle_remove (&mock_system_tag);
	mock_system_is_open = FALSE;
}

static void
mock_init (void)
{
	guint i;

	mock_handles = g_hash_table_new (g_direct_hash, g_direct_equal);

	mock_n_interfaces = mock_config.n_interfaces;
	for (i = 0; i < mock_n_interfaces; i++) {
		mock_interfaces[i].index = i;
		g_snprintf (mock_interfaces[i].id, sizeof (mock_interfaces[i].id),
			    ARV_GENTL_MOCK_INTERFACE_ID_PREFIX "%u", i);
		mock_interfaces[i].is_open = FALSE;
	}

	/* The same device ids on each interface. The fake cameras of the interfaces other than the first one have
	 * other serial numbers, which tell them apart. */
	mock_n_devices = mock_n_interfaces * mock_config.n_devices;
	for (i = 0; i < mock_n_devices; i++) {
		MockDevice *device = g_new0 (MockDevice, 1);
		guint interface_index = i / mock_config.n_devices;
		guint index = i % mock_config.n_devices;

		device->index = index;
		device->interface = &mock_interfaces[interface_index];
		device->id = g_strdup_printf (ARV_GENTL_MOCK_GENTL_DEVICE_ID "%u", index);
		if (interface_index == 0) {
			device->serial_nbr = g_strdup_printf (ARV_GENTL_MOCK_SERIAL "%u", index);
			device->display_name = g_strdup_printf ("Mock camera %u", index);
		} else {
			device->serial_nbr = g_strdup_printf (ARV_GENTL_MOCK_SERIAL "%u-IF%u", index, interface_index);
			device->display_name = g_strdup_printf ("Mock camera %u of interface %u", index,
								interface_index);
		}
		device->port.device = device;

		mock_devices[i] = device;
	}

	mock_is_initialized = TRUE;
}

static GC_ERROR
mock_close_lib (void)
{
	GList *iter;
	guint i;

	if (mock_system_is_open) {
		mock_violation ("GCCloseLib with an open system");
		mock_system_close ();
	}

	for (i = 0; i < mock_n_devices; i++) {
		MockDevice *device = mock_devices[i];

		g_free (device->id);
		g_free (device->serial_nbr);
		g_free (device->display_name);
		g_free (device);
		mock_devices[i] = NULL;
	}
	mock_n_devices = 0;
	mock_n_interfaces = 0;

	/* An event still waited for is freed by the last waiting thread */
	for (iter = mock_events; iter != NULL; iter = iter->next) {
		MockEvent *event = iter->data;

		if (event->n_waiters > 0) {
			mock_violation ("GCCloseLib while a thread waits for an event");
			event->module = NULL;
			event->is_registered = FALSE;
			event->is_zombie = TRUE;
			g_cond_broadcast (&event->cond);
		} else {
			mock_event_free (event);
		}
	}
	g_clear_pointer (&mock_events, g_list_free);
	g_clear_pointer (&mock_handles, g_hash_table_unref);

	mock_is_initialized = FALSE;

	return GC_ERR_SUCCESS;
}

/* Data stream */

static void
mock_stream_notify (MockStream *stream)
{
	if (stream->new_buffer_event != NULL)
		g_cond_broadcast (&stream->new_buffer_event->cond);
}

static gint64
mock_device_get_frame_period_us (MockDevice *device)
{
	guint32 period_us = 0;

	if (mock_config.frame_period_us > 0)
		return mock_config.frame_period_us;

	arv_fake_camera_read_register (device->camera, ARV_FAKE_CAMERA_REGISTER_ACQUISITION_FRAME_PERIOD_US,
				       &period_us);

	return period_us > 0 ? period_us : MOCK_DEFAULT_FRAME_PERIOD_US;
}

static gint64
mock_stream_get_frame_period_us (MockStream *stream)
{
	return mock_device_get_frame_period_us (stream->device);
}

static gboolean
mock_stream_can_produce (MockStream *stream)
{
	return stream->is_acquiring &&
		!stream->is_filling &&
		stream->n_acquired < stream->num_to_acquire &&
		!g_queue_is_empty (&stream->input);
}

/* Called with the mock mutex held, which is released during the fill of the image */

static void
mock_buffer_fill (MockStream *stream, MockBuffer *buffer)
{
	ArvFakeCamera *camera = stream->device->camera;
	guint32 value;
	size_t payload;
	size_t image_offset;

	payload = arv_fake_camera_get_payload (camera);
	/* Data before the image, like chunk data, counted by the data size */
	image_offset = (size_t) mock_config.image_offset;

	stream->frame_counter++;

	buffer->has_new_data = TRUE;
	buffer->frame_id = stream->frame_counter;
	buffer->timestamp_ns = g_get_real_time () * 1000;
	buffer->image_offset = image_offset;
	buffer->data_size = image_offset + payload;
	buffer->is_incomplete = FALSE;
	buffer->is_data_larger = FALSE;

	value = 0;
	arv_fake_camera_read_register (camera, ARV_FAKE_CAMERA_REGISTER_WIDTH, &value);
	buffer->width = value;
	value = 0;
	arv_fake_camera_read_register (camera, ARV_FAKE_CAMERA_REGISTER_HEIGHT, &value);
	buffer->height = value;
	value = 0;
	arv_fake_camera_read_register (camera, ARV_FAKE_CAMERA_REGISTER_X_OFFSET, &value);
	buffer->x_offset = value;
	value = 0;
	arv_fake_camera_read_register (camera, ARV_FAKE_CAMERA_REGISTER_Y_OFFSET, &value);
	buffer->y_offset = value;
	value = 0;
	arv_fake_camera_read_register (camera, ARV_FAKE_CAMERA_REGISTER_PIXEL_FORMAT, &value);
	buffer->pixel_format = value;

	if (image_offset + payload > buffer->size) {
		buffer->is_incomplete = TRUE;
		buffer->is_data_larger = TRUE;
		buffer->size_filled = 0;
	} else {
		ArvBuffer *arv_buffer;

		/* The fake camera fills the buffer memory after the image offset, wrapped in an ArvBuffer. The stream,
		 * its device and the buffer stay valid without the mutex: the buffer is in no queue, and can't be
		 * revoked, and the stream can't be flushed or freed during the fill. */
		arv_buffer = arv_buffer_new (buffer->size - image_offset, (guint8 *) buffer->data + image_offset);
		stream->is_filling = TRUE;
		g_mutex_unlock (&mock_mutex);

		memset (buffer->data, ARV_GENTL_MOCK_IMAGE_OFFSET_FILL, image_offset);
		arv_fake_camera_fill_buffer (camera, arv_buffer, NULL);

		g_mutex_lock (&mock_mutex);
		stream->is_filling = FALSE;
		g_cond_broadcast (&mock_fill_cond);
		g_object_unref (arv_buffer);

		buffer->size_filled = image_offset + payload;

		if (mock_config.incomplete_every > 0 &&
		    stream->frame_counter % mock_config.incomplete_every == 0) {
			buffer->is_incomplete = TRUE;
			buffer->size_filled = image_offset + payload / 2;
		}

		/* Sizes larger than the buffer, without the flag of the data larger than the buffer */
		if (mock_config.size_excess > 0 && !buffer->is_incomplete) {
			buffer->size_filled = buffer->size + mock_config.size_excess;
			buffer->data_size = buffer->size + mock_config.size_excess;
		}
	}
}

/* Frames are produced on demand, when a thread waits for one */

static void
mock_stream_produce (MockStream *stream, gint64 now)
{
	/* One fill at a time, by one of the waiting threads */
	if (!stream->is_acquiring ||
	    stream->is_filling ||
	    stream->n_acquired >= stream->num_to_acquire ||
	    now < stream->next_frame_time)
		return;

	if (g_queue_is_empty (&stream->input)) {
		stream->n_underruns++;
		mock_n_underruns++;
	} else {
		MockBuffer *buffer = g_queue_pop_head (&stream->input);

		buffer->state = MOCK_BUFFER_FILLING;
		mock_buffer_fill (stream, buffer);
		buffer->state = MOCK_BUFFER_DELIVERED;
		g_queue_push_tail (&stream->output, buffer);

		stream->n_acquired++;
		mock_n_frames++;

		/* For the other threads waiting for a frame */
		mock_stream_notify (stream);
	}

	stream->next_frame_time = now + mock_stream_get_frame_period_us (stream);
}

static void
mock_buffer_set_idle (gpointer data, gpointer user_data)
{
	MockBuffer *buffer = data;

	buffer->state = MOCK_BUFFER_IDLE;
}

static GC_ERROR
mock_stream_flush (MockStream *stream, ACQ_QUEUE_TYPE operation)
{
	GList *iter;

	/* The buffer being filled is delivered first */
	mock_stream_wait_fill (stream);

	switch (operation) {
		case ACQ_QUEUE_INPUT_TO_OUTPUT:
			while (!g_queue_is_empty (&stream->input)) {
				MockBuffer *buffer = g_queue_pop_head (&stream->input);

				buffer->has_new_data = FALSE;
				buffer->is_incomplete = TRUE;
				buffer->size_filled = 0;
				buffer->state = MOCK_BUFFER_DELIVERED;
				g_queue_push_tail (&stream->output, buffer);
			}
			break;
		case ACQ_QUEUE_OUTPUT_DISCARD:
			g_queue_foreach (&stream->output, mock_buffer_set_idle, NULL);
			g_queue_clear (&stream->output);
			break;
		case ACQ_QUEUE_ALL_TO_INPUT:
			g_queue_clear (&stream->input);
			g_queue_clear (&stream->output);
			for (iter = stream->buffers; iter != NULL; iter = iter->next) {
				MockBuffer *buffer = iter->data;

				buffer->state = MOCK_BUFFER_QUEUED;
				buffer->has_new_data = FALSE;
				g_queue_push_tail (&stream->input, buffer);
			}
			break;
		case ACQ_QUEUE_UNQUEUED_TO_INPUT:
			for (iter = stream->buffers; iter != NULL; iter = iter->next) {
				MockBuffer *buffer = iter->data;

				if (buffer->state == MOCK_BUFFER_IDLE) {
					buffer->state = MOCK_BUFFER_QUEUED;
					buffer->has_new_data = FALSE;
					g_queue_push_tail (&stream->input, buffer);
				}
			}
			break;
		case ACQ_QUEUE_ALL_DISCARD:
			g_queue_foreach (&stream->input, mock_buffer_set_idle, NULL);
			g_queue_foreach (&stream->output, mock_buffer_set_idle, NULL);
			g_queue_clear (&stream->input);
			g_queue_clear (&stream->output);
			break;
		default:
			return GC_ERR_INVALID_PARAMETER;
	}

	mock_stream_notify (stream);

	return GC_ERR_SUCCESS;
}

static GC_ERROR
mock_buffer_info (MockBuffer *buffer, BUFFER_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	switch (info_cmd) {
		case BUFFER_INFO_BASE:
			return mock_output_ptr (buffer->data, piType, pBuffer, piSize);
		case BUFFER_INFO_SIZE:
			return mock_output_sizet (buffer->size, piType, pBuffer, piSize);
		case BUFFER_INFO_USER_PTR:
			return mock_output_ptr (buffer->user_pointer, piType, pBuffer, piSize);
		case BUFFER_INFO_TIMESTAMP:
		case BUFFER_INFO_TIMESTAMP_NS:
			/* The timestamp frequency is 1 GHz */
			return mock_output_uint64 (buffer->timestamp_ns, piType, pBuffer, piSize);
		case BUFFER_INFO_NEW_DATA:
			return mock_output_bool8 (buffer->has_new_data, piType, pBuffer, piSize);
		case BUFFER_INFO_IS_QUEUED:
			return mock_output_bool8 (buffer->state != MOCK_BUFFER_IDLE, piType, pBuffer, piSize);
		case BUFFER_INFO_IS_ACQUIRING:
			return mock_output_bool8 (FALSE, piType, pBuffer, piSize);
		case BUFFER_INFO_IS_INCOMPLETE:
			return mock_output_bool8 (buffer->is_incomplete, piType, pBuffer, piSize);
		case BUFFER_INFO_TLTYPE:
			return mock_output_string (ARV_GENTL_MOCK_TL_TYPE, piType, pBuffer, piSize);
		case BUFFER_INFO_SIZE_FILLED:
			return mock_output_sizet (buffer->size_filled, piType, pBuffer, piSize);
		case BUFFER_INFO_WIDTH:
			return mock_output_sizet (buffer->width, piType, pBuffer, piSize);
		case BUFFER_INFO_HEIGHT:
			return mock_output_sizet (buffer->height, piType, pBuffer, piSize);
		case BUFFER_INFO_XOFFSET:
			return mock_output_sizet (buffer->x_offset, piType, pBuffer, piSize);
		case BUFFER_INFO_YOFFSET:
			return mock_output_sizet (buffer->y_offset, piType, pBuffer, piSize);
		case BUFFER_INFO_XPADDING:
		case BUFFER_INFO_YPADDING:
			return mock_output_sizet (0, piType, pBuffer, piSize);
		case BUFFER_INFO_IMAGEOFFSET:
			return mock_output_sizet (buffer->image_offset, piType, pBuffer, piSize);
		case BUFFER_INFO_FRAMEID:
			return mock_output_uint64 (buffer->frame_id, piType, pBuffer, piSize);
		case BUFFER_INFO_IMAGEPRESENT:
			return mock_output_bool8 (TRUE, piType, pBuffer, piSize);
		case BUFFER_INFO_PAYLOADTYPE:
			return mock_output_sizet (PAYLOAD_TYPE_IMAGE, piType, pBuffer, piSize);
		case BUFFER_INFO_PIXELFORMAT:
			return mock_output_uint64 (buffer->pixel_format, piType, pBuffer, piSize);
		case BUFFER_INFO_DELIVERED_IMAGEHEIGHT:
			return mock_output_sizet (buffer->data_size > 0 ?
						  buffer->height * buffer->size_filled / buffer->data_size : 0,
						  piType, pBuffer, piSize);
		case BUFFER_INFO_DATA_SIZE:
			return mock_output_sizet (buffer->data_size, piType, pBuffer, piSize);
		case BUFFER_INFO_DATA_LARGER_THAN_BUFFER:
			return mock_output_bool8 (buffer->is_data_larger, piType, pBuffer, piSize);
		case BUFFER_INFO_CONTAINS_CHUNKDATA:
		case BUFFER_INFO_IS_COMPOSITE:
			return mock_output_bool8 (FALSE, piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

static gboolean
mock_check_buffer (MockStream *stream, BUFFER_HANDLE handle, MockFunction function)
{
	if (!mock_check_handle (handle, MOCK_HANDLE_BUFFER, function))
		return FALSE;

	if (((MockBuffer *) handle)->stream != stream) {
		mock_violation ("%s: buffer %p of another data stream", mock_function_names[function], handle);
		return FALSE;
	}

	return TRUE;
}

static GC_ERROR
mock_stream_start (MockStream *stream, uint64_t num_to_acquire)
{
	if (stream->is_acquiring)
		return GC_ERR_RESOURCE_IN_USE;

	if (g_list_length (stream->buffers) < mock_config.buf_announce_min)
		return GC_ERR_INVALID_BUFFER;

	stream->is_acquiring = TRUE;
	stream->num_to_acquire = num_to_acquire;
	stream->n_acquired = 0;
	stream->n_started++;
	stream->next_frame_time = g_get_monotonic_time () + mock_stream_get_frame_period_us (stream);

	mock_stream_notify (stream);

	return GC_ERR_SUCCESS;
}

static GC_ERROR
mock_stream_stop (MockStream *stream)
{
	if (!stream->is_acquiring)
		return GC_ERR_RESOURCE_IN_USE;

	stream->is_acquiring = FALSE;

	mock_stream_notify (stream);

	return GC_ERR_SUCCESS;
}

static GC_ERROR
mock_stream_close (MockStream *stream)
{
	if (stream->is_acquiring)
		mock_violation ("DSClose while acquiring");
	if (stream->buffers != NULL)
		mock_violation ("DSClose with %u announced buffers", g_list_length (stream->buffers));

	mock_stream_free (stream);

	return GC_ERR_SUCCESS;
}

/* Events */

static MockEvent *
mock_find_event (gconstpointer module, EVENT_TYPE type)
{
	GList *iter;

	for (iter = mock_events; iter != NULL; iter = iter->next) {
		MockEvent *event = iter->data;

		if (event->is_registered && event->module == module && event->type == type)
			return event;
	}

	return NULL;
}

static GC_ERROR
mock_register_event (gpointer module, EVENT_TYPE type, EVENT_HANDLE *phEvent)
{
	MockHandleType module_type = mock_handle_get_type (module);
	MockEvent *event;

	if (module_type != MOCK_HANDLE_SYSTEM &&
	    module_type != MOCK_HANDLE_INTERFACE &&
	    module_type != MOCK_HANDLE_DEVICE &&
	    module_type != MOCK_HANDLE_STREAM) {
		mock_violation ("GCRegisterEvent: invalid module handle %p", module);
		return GC_ERR_INVALID_HANDLE;
	}

	if (phEvent == NULL)
		return GC_ERR_INVALID_PARAMETER;

	if (type == EVENT_NEW_BUFFER && module_type != MOCK_HANDLE_STREAM)
		return GC_ERR_NOT_AVAILABLE;

	if (mock_find_event (module, type) != NULL)
		return GC_ERR_RESOURCE_IN_USE;

	event = g_new0 (MockEvent, 1);
	event->type = type;
	event->module = module;
	event->is_registered = TRUE;
	g_cond_init (&event->cond);

	mock_events = g_list_prepend (mock_events, event);
	mock_handle_add (event, MOCK_HANDLE_EVENT);

	if (type == EVENT_NEW_BUFFER)
		((MockStream *) module)->new_buffer_event = event;
	else if (type == EVENT_REMOTE_DEVICE && module_type == MOCK_HANDLE_DEVICE)
		event->next_event_time = g_get_monotonic_time () + mock_device_get_frame_period_us (module);

	*phEvent = event;

	return GC_ERR_SUCCESS;
}

static GC_ERROR
mock_unregister_event (gpointer module, EVENT_TYPE type)
{
	MockHandleType module_type = mock_handle_get_type (module);
	MockEvent *event;

	if (module_type == MOCK_HANDLE_NONE) {
		mock_violation ("GCUnregisterEvent: invalid module handle %p", module);
		return GC_ERR_INVALID_HANDLE;
	}

	event = mock_find_event (module, type);
	if (event == NULL)
		return GC_ERR_INVALID_PARAMETER;

	if (event->n_waiters > 0)
		mock_violation ("GCUnregisterEvent while a thread waits for event %d", type);

	/* The event handle stays known until GCCloseLib, to detect its use after unregistration */
	event->is_registered = FALSE;
	g_cond_broadcast (&event->cond);

	if (type == EVENT_NEW_BUFFER && module_type == MOCK_HANDLE_STREAM)
		((MockStream *) module)->new_buffer_event = NULL;

	return GC_ERR_SUCCESS;
}

static gboolean
mock_check_event (EVENT_HANDLE handle, MockFunction function)
{
	if (!mock_check_handle (handle, MOCK_HANDLE_EVENT, function))
		return FALSE;

	if (!((MockEvent *) handle)->is_registered) {
		mock_violation ("%s: event %p is not registered", mock_function_names[function], handle);
		return FALSE;
	}

	return TRUE;
}

/* With ARV_GENTL_MOCK_DEVICE_EVENT_ID, the device event of an open device fires at the frame period. The data of a
 * remote device event is its number, the value of the event. */

typedef guint64 MockDeviceEventData;

static gboolean
mock_event_has_device_events (MockEvent *event)
{
	return event->type == EVENT_REMOTE_DEVICE &&
		mock_handle_get_type (event->module) == MOCK_HANDLE_DEVICE &&
		mock_config.device_event_id[0] != '\0';
}

static GC_ERROR
mock_event_get_data (MockEvent *event, void *pBuffer, size_t *piSize, uint64_t iTimeout)
{
	gint64 deadline;
	GC_ERROR error = GC_ERR_ERROR;

	if (piSize == NULL || pBuffer == NULL)
		return GC_ERR_INVALID_PARAMETER;

	if (event->type == EVENT_NEW_BUFFER && *piSize < sizeof (EVENT_NEW_BUFFER_DATA)) {
		*piSize = sizeof (EVENT_NEW_BUFFER_DATA);
		return GC_ERR_BUFFER_TOO_SMALL;
	}

	if (iTimeout == GENTL_INFINITE)
		deadline = G_MAXINT64;
	else
		deadline = g_get_monotonic_time () + (gint64) MIN (iTimeout, G_GUINT64_CONSTANT (1) << 40) * 1000;

	event->n_waiters++;

	for (;;) {
		MockStream *stream;
		gint64 now;
		gint64 wake_time;

		if (event->is_kill_pending) {
			event->is_kill_pending = FALSE;
			error = GC_ERR_ABORT;
			break;
		}

		if (!event->is_registered) {
			error = GC_ERR_ABORT;
			break;
		}

		stream = event->type == EVENT_NEW_BUFFER ? event->module : NULL;
		now = g_get_monotonic_time ();

		if (stream != NULL) {
			MockBuffer *buffer;

			mock_stream_produce (stream, now);

			buffer = g_queue_pop_head (&stream->output);
			if (buffer != NULL) {
				EVENT_NEW_BUFFER_DATA data;

				buffer->state = MOCK_BUFFER_IDLE;
				stream->n_delivered++;
				event->n_fired++;

				data.BufferHandle = buffer;
				data.UserPointer = buffer->user_pointer;
				memcpy (pBuffer, &data, sizeof (data));
				*piSize = sizeof (data);

				error = GC_ERR_SUCCESS;
				break;
			}
		}

		if (mock_event_has_device_events (event) && now >= event->next_event_time) {
			MockDeviceEventData data;

			if (*piSize < sizeof (data)) {
				*piSize = sizeof (data);
				error = GC_ERR_BUFFER_TOO_SMALL;
				break;
			}

			data = ++mock_n_device_events;
			event->n_fired++;
			event->next_event_time = now + mock_device_get_frame_period_us (event->module);

			memcpy (pBuffer, &data, sizeof (data));
			*piSize = sizeof (data);

			error = GC_ERR_SUCCESS;
			break;
		}

		if (now >= deadline) {
			error = GC_ERR_TIMEOUT;
			break;
		}

		wake_time = deadline;
		if (stream != NULL && mock_stream_can_produce (stream))
			wake_time = MIN (wake_time, stream->next_frame_time);
		if (mock_event_has_device_events (event))
			wake_time = MIN (wake_time, event->next_event_time);

		if (wake_time == G_MAXINT64)
			g_cond_wait (&event->cond, &mock_mutex);
		else
			g_cond_wait_until (&event->cond, &mock_mutex, wake_time);
	}

	event->n_waiters--;

	if (event->is_zombie && event->n_waiters == 0)
		mock_event_free (event);

	return error;
}

static GC_ERROR
mock_event_kill (MockEvent *event)
{
	switch (mock_config.event_kill) {
		case MOCK_EVENT_KILL_LATCH:
			event->is_kill_pending = TRUE;
			break;
		case MOCK_EVENT_KILL_WAITERS:
			if (event->n_waiters > 0)
				event->is_kill_pending = TRUE;
			break;
		case MOCK_EVENT_KILL_IGNORE:
		default:
			break;
	}

	g_cond_broadcast (&event->cond);

	return GC_ERR_SUCCESS;
}

/* Remote device port, backed by the fake camera memory */

static const char *
mock_port_get_url (MockPort *port)
{
	return arv_fake_camera_get_genicam_xml_url (port->device->camera);
}

static GC_ERROR
mock_port_read (MockPort *port, uint64_t address, void *buffer, size_t *size)
{
	size_t xml_size = 0;

	if (buffer == NULL || size == NULL || *size == 0)
		return GC_ERR_INVALID_PARAMETER;

	arv_fake_camera_get_genicam_xml (port->device->camera, &xml_size);
	if (address > ARV_FAKE_CAMERA_MEMORY_SIZE + xml_size ||
	    *size > ARV_FAKE_CAMERA_MEMORY_SIZE + xml_size - address)
		return GC_ERR_INVALID_ADDRESS;

	if (!arv_fake_camera_read_memory (port->device->camera, (guint32) address, (guint32) *size, buffer))
		return GC_ERR_IO;

	return GC_ERR_SUCCESS;
}

static GC_ERROR
mock_port_write (MockPort *port, uint64_t address, const void *buffer, size_t *size)
{
	if (buffer == NULL || size == NULL || *size == 0)
		return GC_ERR_INVALID_PARAMETER;

	/* The GenICam data is read only */
	if (address > ARV_FAKE_CAMERA_MEMORY_SIZE || *size > ARV_FAKE_CAMERA_MEMORY_SIZE - address)
		return GC_ERR_INVALID_ADDRESS;

	if (!arv_fake_camera_write_memory (port->device->camera, (guint32) address, (guint32) *size, buffer))
		return GC_ERR_IO;

	return GC_ERR_SUCCESS;
}

/* Global functions */

GC_ERROR
GCInitLib (void)
{
	GC_ERROR error = GC_ERR_SUCCESS;

	g_mutex_lock (&mock_mutex);

	mock_n_calls[MOCK_FUNCTION_GCInitLib]++;

	if (mock_is_initialized) {
		mock_violation ("GCInitLib called twice");
		error = GC_ERR_RESOURCE_IN_USE;
	} else {
		memset (mock_session_calls, 0, sizeof (mock_session_calls));

		mock_config_reset (&mock_config);
		mock_config_apply_environment (&mock_config);

		mock_session_calls[MOCK_FUNCTION_GCInitLib] = 1;

		if (mock_config.fail_from[MOCK_FUNCTION_GCInitLib] == 1)
			error = mock_config.fail_error[MOCK_FUNCTION_GCInitLib];
#ifdef ARV_GENTL_MOCK_DEP
		else if (arv_gentl_mock_helper_get_answer () != 42)
			error = GC_ERR_ERROR;
#endif
		else
			mock_init ();
	}

	g_mutex_unlock (&mock_mutex);

	return error;
}

GC_ERROR
GCCloseLib (void)
{
	MOCK_ENTER (GCCloseLib);
	MOCK_RETURN (mock_close_lib ());
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

GC_ERROR
GCGetInfo (TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (GCGetInfo);
	MOCK_RETURN (mock_tl_info (iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
GCGetLastError (GC_ERROR *piErrorCode, char *sErrorText, size_t *piSize)
{
	MOCK_ENTER (GCGetLastError);
	if (piErrorCode != NULL)
		*piErrorCode = mock_last_error;
	MOCK_RETURN (mock_output_string (mock_last_error_text, NULL, sErrorText, piSize));
}

#endif

/* System module */

#ifndef ARV_GENTL_MOCK_BROKEN

GC_ERROR
TLOpen (TL_HANDLE *phSystem)
{
	MOCK_ENTER (TLOpen);
	MOCK_CHECK_PARAMETER (phSystem != NULL);

	if (mock_system_is_open)
		MOCK_RETURN (GC_ERR_RESOURCE_IN_USE);

	mock_system_is_open = TRUE;
	mock_handle_add (&mock_system_tag, MOCK_HANDLE_SYSTEM);
	*phSystem = &mock_system_tag;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#endif

GC_ERROR
TLClose (TL_HANDLE hSystem)
{
	MOCK_ENTER (TLClose);
	mock_close_delay ();
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);

	mock_system_close ();

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
TLGetInfo (TL_HANDLE hSystem, TL_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (TLGetInfo);
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);
	MOCK_RETURN (mock_tl_info (iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
TLGetInterfaceID (TL_HANDLE hSystem, uint32_t iIndex, char *sIfaceID, size_t *piSize)
{
	MOCK_ENTER (TLGetInterfaceID);
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);

	if (iIndex >= mock_n_interfaces)
		MOCK_RETURN (GC_ERR_INVALID_INDEX);

	MOCK_RETURN (mock_output_string (mock_interfaces[iIndex].id, NULL, sIfaceID, piSize));
}

GC_ERROR
TLGetInterfaceInfo (TL_HANDLE hSystem, const char *sIfaceID, INTERFACE_INFO_CMD iInfoCmd,
		    INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MockInterface *interface;

	MOCK_ENTER (TLGetInterfaceInfo);
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);

	interface = mock_find_interface (sIfaceID);
	if (interface == NULL)
		MOCK_RETURN (GC_ERR_INVALID_ID);

	MOCK_RETURN (mock_interface_info (interface, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
TLGetNumInterfaces (TL_HANDLE hSystem, uint32_t *piNumIfaces)
{
	MOCK_ENTER (TLGetNumInterfaces);
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);
	MOCK_CHECK_PARAMETER (piNumIfaces != NULL);

	*piNumIfaces = mock_n_interfaces;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
TLOpenInterface (TL_HANDLE hSystem, const char *sIfaceID, IF_HANDLE *phIface)
{
	MockInterface *interface;

	MOCK_ENTER (TLOpenInterface);
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);
	MOCK_CHECK_PARAMETER (phIface != NULL);

	interface = mock_find_interface (sIfaceID);
	if (interface == NULL)
		MOCK_RETURN (GC_ERR_INVALID_ID);

	if (interface->is_open)
		MOCK_RETURN (GC_ERR_RESOURCE_IN_USE);

	interface->is_open = TRUE;
	mock_handle_add (interface, MOCK_HANDLE_INTERFACE);
	*phIface = interface;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
TLUpdateInterfaceList (TL_HANDLE hSystem, bool8_t *pbChanged, uint64_t iTimeout)
{
	MOCK_ENTER (TLUpdateInterfaceList);
	MOCK_CHECK (hSystem, MOCK_HANDLE_SYSTEM);

	if (pbChanged != NULL)
		*pbChanged = 0;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

/* Interface module */

GC_ERROR
IFClose (IF_HANDLE hIface)
{
	MOCK_ENTER (IFClose);
	mock_close_delay ();
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);

	mock_interface_close (hIface);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

GC_ERROR
IFGetInfo (IF_HANDLE hIface, INTERFACE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (IFGetInfo);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);
	MOCK_RETURN (mock_interface_info (hIface, iInfoCmd, piType, pBuffer, piSize));
}

#endif

GC_ERROR
IFGetDeviceID (IF_HANDLE hIface, uint32_t iIndex, char *sDeviceID, size_t *piSize)
{
	MockDevice *device;

	MOCK_ENTER (IFGetDeviceID);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);

	device = mock_get_interface_device (hIface, iIndex);
	if (device == NULL)
		MOCK_RETURN (GC_ERR_INVALID_INDEX);

	MOCK_RETURN (mock_output_string (device->id, NULL, sDeviceID, piSize));
}

GC_ERROR
IFGetDeviceInfo (IF_HANDLE hIface, const char *sDeviceID, DEVICE_INFO_CMD iInfoCmd,
		 INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MockDevice *device;

	MOCK_ENTER (IFGetDeviceInfo);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);

	device = mock_find_device (hIface, sDeviceID);
	if (device == NULL)
		MOCK_RETURN (GC_ERR_INVALID_ID);

	MOCK_RETURN (mock_device_info (device, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
IFGetNumDevices (IF_HANDLE hIface, uint32_t *piNumDevices)
{
	MOCK_ENTER (IFGetNumDevices);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);
	MOCK_CHECK_PARAMETER (piNumDevices != NULL);

	*piNumDevices = mock_get_interface_n_devices (hIface);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
IFOpenDevice (IF_HANDLE hIface, const char *sDeviceID, DEVICE_ACCESS_FLAGS iOpenFlag, DEV_HANDLE *phDevice)
{
	MockDevice *device;

	MOCK_ENTER (IFOpenDevice);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);
	MOCK_CHECK_PARAMETER (phDevice != NULL);

	device = mock_find_device (hIface, sDeviceID);
	if (device == NULL)
		MOCK_RETURN (GC_ERR_INVALID_ID);

	if (device->is_open)
		MOCK_RETURN (GC_ERR_RESOURCE_IN_USE);

	device->camera = arv_fake_camera_new (device->serial_nbr);
	if (device->camera == NULL)
		MOCK_RETURN (GC_ERR_IO);

	device->is_open = TRUE;
	mock_handle_add (device, MOCK_HANDLE_DEVICE);
	mock_handle_add (&device->port, MOCK_HANDLE_PORT);
	*phDevice = device;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
IFUpdateDeviceList (IF_HANDLE hIface, bool8_t *pbChanged, uint64_t iTimeout)
{
	MOCK_ENTER (IFUpdateDeviceList);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);

	if (pbChanged != NULL)
		*pbChanged = 0;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

GC_ERROR
IFGetParentTL (IF_HANDLE hIface, TL_HANDLE *phSystem)
{
	MOCK_ENTER (IFGetParentTL);
	MOCK_CHECK (hIface, MOCK_HANDLE_INTERFACE);
	MOCK_CHECK_PARAMETER (phSystem != NULL);

	*phSystem = &mock_system_tag;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#endif

/* Device module */

GC_ERROR
DevClose (DEV_HANDLE hDevice)
{
	MOCK_ENTER (DevClose);
	mock_close_delay ();
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);

	mock_device_close (hDevice);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DevGetInfo (DEV_HANDLE hDevice, DEVICE_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (DevGetInfo);
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);
	MOCK_RETURN (mock_device_info (hDevice, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
DevGetDataStreamID (DEV_HANDLE hDevice, uint32_t iIndex, char *sDataStreamID, size_t *piSize)
{
	MOCK_ENTER (DevGetDataStreamID);
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);

	if (iIndex != 0)
		MOCK_RETURN (GC_ERR_INVALID_INDEX);

	MOCK_RETURN (mock_output_string (ARV_GENTL_MOCK_STREAM_ID, NULL, sDataStreamID, piSize));
}

GC_ERROR
DevGetNumDataStreams (DEV_HANDLE hDevice, uint32_t *piNumDataStreams)
{
	MOCK_ENTER (DevGetNumDataStreams);
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);
	MOCK_CHECK_PARAMETER (piNumDataStreams != NULL);

	*piNumDataStreams = 1;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DevGetPort (DEV_HANDLE hDevice, PORT_HANDLE *phRemoteDev)
{
	MOCK_ENTER (DevGetPort);
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);
	MOCK_CHECK_PARAMETER (phRemoteDev != NULL);

	*phRemoteDev = &((MockDevice *) hDevice)->port;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DevOpenDataStream (DEV_HANDLE hDevice, const char *sDataStreamID, DS_HANDLE *phDataStream)
{
	MockDevice *device = hDevice;
	MockStream *stream;

	MOCK_ENTER (DevOpenDataStream);
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);
	MOCK_CHECK_PARAMETER (phDataStream != NULL);

	if (g_strcmp0 (sDataStreamID, ARV_GENTL_MOCK_STREAM_ID) != 0)
		MOCK_RETURN (GC_ERR_INVALID_ID);

	if (device->stream != NULL)
		MOCK_RETURN (GC_ERR_RESOURCE_IN_USE);

	stream = g_new0 (MockStream, 1);
	stream->device = device;
	g_queue_init (&stream->input);
	g_queue_init (&stream->output);
	device->stream = stream;

	mock_handle_add (stream, MOCK_HANDLE_STREAM);
	*phDataStream = stream;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

GC_ERROR
DevGetParentIF (DEV_HANDLE hDevice, IF_HANDLE *phIface)
{
	MOCK_ENTER (DevGetParentIF);
	MOCK_CHECK (hDevice, MOCK_HANDLE_DEVICE);
	MOCK_CHECK_PARAMETER (phIface != NULL);

	*phIface = ((MockDevice *) hDevice)->interface;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#endif

/* Data stream module */

GC_ERROR
DSAllocAndAnnounceBuffer (DS_HANDLE hDataStream, size_t iBufferSize, void *pPrivate, BUFFER_HANDLE *phBuffer)
{
	MockBuffer *buffer;

	MOCK_ENTER (DSAllocAndAnnounceBuffer);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (iBufferSize > 0 && phBuffer != NULL);

	buffer = g_new0 (MockBuffer, 1);
	buffer->stream = hDataStream;
	buffer->data = g_malloc0 (iBufferSize);
	buffer->size = iBufferSize;
	buffer->is_owned = TRUE;
	buffer->user_pointer = pPrivate;
	buffer->state = MOCK_BUFFER_IDLE;

	buffer->stream->buffers = g_list_append (buffer->stream->buffers, buffer);
	mock_handle_add (buffer, MOCK_HANDLE_BUFFER);
	*phBuffer = buffer;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSAnnounceBuffer (DS_HANDLE hDataStream, void *pBuffer, size_t iSize, void *pPrivate, BUFFER_HANDLE *phBuffer)
{
	MockBuffer *buffer;

	MOCK_ENTER (DSAnnounceBuffer);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (pBuffer != NULL && iSize > 0 && phBuffer != NULL);

	buffer = g_new0 (MockBuffer, 1);
	buffer->stream = hDataStream;
	buffer->data = pBuffer;
	buffer->size = iSize;
	buffer->is_owned = FALSE;
	buffer->user_pointer = pPrivate;
	buffer->state = MOCK_BUFFER_IDLE;

	buffer->stream->buffers = g_list_append (buffer->stream->buffers, buffer);
	mock_handle_add (buffer, MOCK_HANDLE_BUFFER);
	*phBuffer = buffer;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSClose (DS_HANDLE hDataStream)
{
	MOCK_ENTER (DSClose);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (mock_stream_close (hDataStream));
}

GC_ERROR
DSFlushQueue (DS_HANDLE hDataStream, ACQ_QUEUE_TYPE iOperation)
{
	MOCK_ENTER (DSFlushQueue);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (mock_stream_flush (hDataStream, iOperation));
}

GC_ERROR
DSGetBufferInfo (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, BUFFER_INFO_CMD iInfoCmd,
		 INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (DSGetBufferInfo);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	MOCK_RETURN (mock_buffer_info (hBuffer, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
DSQueueBuffer (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer)
{
	MockBuffer *buffer = hBuffer;

	MOCK_ENTER (DSQueueBuffer);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	if (buffer->state != MOCK_BUFFER_IDLE)
		MOCK_RETURN (GC_ERR_RESOURCE_IN_USE);

	buffer->state = MOCK_BUFFER_QUEUED;
	buffer->has_new_data = FALSE;
	g_queue_push_tail (&buffer->stream->input, buffer);

	mock_stream_notify (buffer->stream);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSRevokeBuffer (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, void **ppBuffer, void **ppPrivate)
{
	MockBuffer *buffer = hBuffer;

	MOCK_ENTER (DSRevokeBuffer);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	if (buffer->state != MOCK_BUFFER_IDLE) {
		mock_violation ("DSRevokeBuffer: buffer %p is still queued", hBuffer);
		MOCK_RETURN (GC_ERR_BUSY);
	}

	if (ppBuffer != NULL)
		*ppBuffer = buffer->is_owned ? NULL : buffer->data;
	if (ppPrivate != NULL)
		*ppPrivate = buffer->user_pointer;

	buffer->stream->buffers = g_list_remove (buffer->stream->buffers, buffer);
	mock_buffer_free (buffer);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSStartAcquisition (DS_HANDLE hDataStream, ACQ_START_FLAGS iStartFlags, uint64_t iNumToAcquire)
{
	MOCK_ENTER (DSStartAcquisition);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (mock_stream_start (hDataStream, iNumToAcquire));
}

GC_ERROR
DSStopAcquisition (DS_HANDLE hDataStream, ACQ_STOP_FLAGS iStopFlags)
{
	MOCK_ENTER (DSStopAcquisition);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (mock_stream_stop (hDataStream));
}

GC_ERROR
DSGetBufferInfoStacked (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, DS_BUFFER_INFO_STACKED *pInfoStacked,
			size_t iNumInfos)
{
	size_t i;

	MOCK_ENTER (DSGetBufferInfoStacked);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (pInfoStacked != NULL);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	for (i = 0; i < iNumInfos; i++)
		pInfoStacked[i].iResult = mock_buffer_info (hBuffer, pInfoStacked[i].iInfoCmd,
							    &pInfoStacked[i].iType, pInfoStacked[i].puffer,
							    &pInfoStacked[i].iSize);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

static GC_ERROR
mock_stream_info (MockStream *stream, STREAM_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	switch (info_cmd) {
		case STREAM_INFO_ID:
			return mock_output_string (ARV_GENTL_MOCK_STREAM_ID, piType, pBuffer, piSize);
		case STREAM_INFO_NUM_DELIVERED:
			return mock_output_uint64 (stream->n_delivered, piType, pBuffer, piSize);
		case STREAM_INFO_NUM_UNDERRUN:
			return mock_output_uint64 (stream->n_underruns, piType, pBuffer, piSize);
		case STREAM_INFO_NUM_ANNOUNCED:
			return mock_output_sizet (g_list_length (stream->buffers), piType, pBuffer, piSize);
		case STREAM_INFO_NUM_QUEUED:
			return mock_output_sizet (g_queue_get_length (&stream->input), piType, pBuffer, piSize);
		case STREAM_INFO_NUM_AWAIT_DELIVERY:
			return mock_output_sizet (g_queue_get_length (&stream->output), piType, pBuffer, piSize);
		case STREAM_INFO_NUM_STARTED:
			return mock_output_uint64 (stream->n_started, piType, pBuffer, piSize);
		case STREAM_INFO_PAYLOAD_SIZE:
			return mock_output_sizet (arv_fake_camera_get_payload (stream->device->camera),
						  piType, pBuffer, piSize);
		case STREAM_INFO_IS_GRABBING:
			return mock_output_bool8 (stream->is_acquiring, piType, pBuffer, piSize);
		case STREAM_INFO_DEFINES_PAYLOADSIZE:
			return mock_output_bool8 (TRUE, piType, pBuffer, piSize);
		case STREAM_INFO_TLTYPE:
			return mock_output_string (ARV_GENTL_MOCK_TL_TYPE, piType, pBuffer, piSize);
		case STREAM_INFO_NUM_CHUNKS_MAX:
			return mock_output_sizet (0, piType, pBuffer, piSize);
		case STREAM_INFO_BUF_ANNOUNCE_MIN:
			return mock_output_sizet (mock_config.buf_announce_min, piType, pBuffer, piSize);
		case STREAM_INFO_BUF_ALIGNMENT:
			return mock_output_sizet (1, piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

static GC_ERROR
mock_buffer_part_info (MockBuffer *buffer, uint32_t part_index, BUFFER_PART_INFO_CMD info_cmd,
		       INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	if (mock_config.n_parts < 1 || part_index >= (uint32_t) mock_config.n_parts)
		return GC_ERR_INVALID_INDEX;

	/* The image part, without the data before the image */
	switch (info_cmd) {
		case BUFFER_PART_INFO_BASE:
			return mock_output_ptr ((guint8 *) buffer->data + buffer->image_offset, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_DATA_SIZE:
			return mock_output_sizet (buffer->size_filled > buffer->image_offset ?
						  buffer->size_filled - buffer->image_offset : 0,
						  piType, pBuffer, piSize);
		case BUFFER_PART_INFO_DATA_TYPE:
			/* 2D image */
			return mock_output_sizet (1, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_DATA_FORMAT:
			return mock_output_uint64 (buffer->pixel_format, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_WIDTH:
			return mock_output_sizet (buffer->width, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_HEIGHT:
			return mock_output_sizet (buffer->height, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_XOFFSET:
			return mock_output_sizet (buffer->x_offset, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_YOFFSET:
			return mock_output_sizet (buffer->y_offset, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_XPADDING:
			return mock_output_sizet (0, piType, pBuffer, piSize);
		case BUFFER_PART_INFO_SOURCE_ID:
			return mock_output_uint64 (0, piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

GC_ERROR
DSAnnounceCompositeBuffer (DS_HANDLE hDataStream, size_t iNumSegments, void **ppSegments, size_t *piSizes,
			   void *pPrivate, BUFFER_HANDLE *phBuffer)
{
	MOCK_ENTER (DSAnnounceCompositeBuffer);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (GC_ERR_NOT_IMPLEMENTED);
}

GC_ERROR
DSGetBufferID (DS_HANDLE hDataStream, uint32_t iIndex, BUFFER_HANDLE *phBuffer)
{
	MockStream *stream = hDataStream;

	MOCK_ENTER (DSGetBufferID);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (phBuffer != NULL);

	if (iIndex >= g_list_length (stream->buffers))
		MOCK_RETURN (GC_ERR_INVALID_INDEX);

	*phBuffer = g_list_nth_data (stream->buffers, iIndex);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSGetInfo (DS_HANDLE hDataStream, STREAM_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (DSGetInfo);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (mock_stream_info (hDataStream, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
DSGetBufferChunkData (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, SINGLE_CHUNK_DATA *pChunkData,
		      size_t *piNumChunks)
{
	MOCK_ENTER (DSGetBufferChunkData);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (piNumChunks != NULL);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	/* No chunk data */
	*piNumChunks = 0;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSGetParentDev (DS_HANDLE hDataStream, DEV_HANDLE *phDevice)
{
	MOCK_ENTER (DSGetParentDev);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (phDevice != NULL);

	*phDevice = ((MockStream *) hDataStream)->device;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSGetNumBufferParts (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t *piNumParts)
{
	MOCK_ENTER (DSGetNumBufferParts);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (piNumParts != NULL);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	if (mock_config.n_parts < 0)
		MOCK_RETURN (GC_ERR_NOT_AVAILABLE);

	*piNumParts = mock_config.n_parts;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSGetBufferPartInfo (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t iPartIndex,
		     BUFFER_PART_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (DSGetBufferPartInfo);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	MOCK_RETURN (mock_buffer_part_info (hBuffer, iPartIndex, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
DSGetBufferPartInfoStacked (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, DS_BUFFER_PART_INFO_STACKED *pInfoStacked,
			    size_t iNumInfos)
{
	size_t i;

	MOCK_ENTER (DSGetBufferPartInfoStacked);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_CHECK_PARAMETER (pInfoStacked != NULL);

	if (!mock_check_buffer (hDataStream, hBuffer, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	for (i = 0; i < iNumInfos; i++)
		pInfoStacked[i].iResult = mock_buffer_part_info (hBuffer, pInfoStacked[i].iPartIndex,
								 pInfoStacked[i].iInfoCmd, &pInfoStacked[i].iType,
								 pInfoStacked[i].pBuffer, &pInfoStacked[i].iSize);

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
DSGetNumFlows (DS_HANDLE hDataStream, uint32_t *piNumFlows)
{
	MOCK_ENTER (DSGetNumFlows);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (GC_ERR_NOT_IMPLEMENTED);
}

GC_ERROR
DSGetFlowInfo (DS_HANDLE hDataStream, uint32_t iFlowIndex, FLOW_INFO_CMD iInfoCmd,
	       INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (DSGetFlowInfo);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (GC_ERR_NOT_IMPLEMENTED);
}

GC_ERROR
DSGetNumBufferSegments (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t *piNumSegments)
{
	MOCK_ENTER (DSGetNumBufferSegments);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (GC_ERR_NOT_IMPLEMENTED);
}

GC_ERROR
DSGetBufferSegmentInfo (DS_HANDLE hDataStream, BUFFER_HANDLE hBuffer, uint32_t iSegmentIndex,
			SEGMENT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (DSGetBufferSegmentInfo);
	MOCK_CHECK (hDataStream, MOCK_HANDLE_STREAM);
	MOCK_RETURN (GC_ERR_NOT_IMPLEMENTED);
}

#endif

/* Port functions */

GC_ERROR
GCGetPortURLInfo (PORT_HANDLE hPort, uint32_t iURLIndex, URL_INFO_CMD iInfoCmd,
		  INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (GCGetPortURLInfo);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);

	if (iURLIndex != 0)
		MOCK_RETURN (GC_ERR_INVALID_INDEX);

	if (iInfoCmd != URL_INFO_URL)
		MOCK_RETURN (GC_ERR_NOT_AVAILABLE);

	MOCK_RETURN (mock_output_string (mock_port_get_url (hPort), piType, pBuffer, piSize));
}

GC_ERROR
GCReadPort (PORT_HANDLE hPort, uint64_t iAddress, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (GCReadPort);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_RETURN (mock_port_read (hPort, iAddress, pBuffer, piSize));
}

GC_ERROR
GCWritePort (PORT_HANDLE hPort, uint64_t iAddress, const void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (GCWritePort);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_RETURN (mock_port_write (hPort, iAddress, pBuffer, piSize));
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

static GC_ERROR
mock_port_info (MockPort *port, PORT_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	switch (info_cmd) {
		case PORT_INFO_ID:
		case PORT_INFO_PORTNAME:
			return mock_output_string (port->device->id, piType, pBuffer, piSize);
		case PORT_INFO_VENDOR:
			return mock_output_string (ARV_GENTL_MOCK_VENDOR, piType, pBuffer, piSize);
		case PORT_INFO_MODEL:
			return mock_output_string (ARV_GENTL_MOCK_MODEL, piType, pBuffer, piSize);
		case PORT_INFO_TLTYPE:
			return mock_output_string (ARV_GENTL_MOCK_TL_TYPE, piType, pBuffer, piSize);
		case PORT_INFO_MODULE:
			return mock_output_string ("Device", piType, pBuffer, piSize);
		case PORT_INFO_VERSION:
			return mock_output_string ("1.0", piType, pBuffer, piSize);
		case PORT_INFO_LITTLE_ENDIAN:
		case PORT_INFO_ACCESS_NA:
		case PORT_INFO_ACCESS_NI:
			return mock_output_bool8 (FALSE, piType, pBuffer, piSize);
		case PORT_INFO_BIG_ENDIAN:
		case PORT_INFO_ACCESS_READ:
		case PORT_INFO_ACCESS_WRITE:
			return mock_output_bool8 (TRUE, piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

GC_ERROR
GCGetPortInfo (PORT_HANDLE hPort, PORT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (GCGetPortInfo);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_RETURN (mock_port_info (hPort, iInfoCmd, piType, pBuffer, piSize));
}

GC_ERROR
GCGetPortURL (PORT_HANDLE hPort, char *sURL, size_t *piSize)
{
	MOCK_ENTER (GCGetPortURL);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_RETURN (mock_output_string (mock_port_get_url (hPort), NULL, sURL, piSize));
}

GC_ERROR
GCGetNumPortURLs (PORT_HANDLE hPort, uint32_t *piNumURLs)
{
	MOCK_ENTER (GCGetNumPortURLs);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_CHECK_PARAMETER (piNumURLs != NULL);

	*piNumURLs = 1;

	MOCK_RETURN (GC_ERR_SUCCESS);
}

GC_ERROR
GCWritePortStacked (PORT_HANDLE hPort, PORT_REGISTER_STACK_ENTRY *pEntries, size_t *piNumEntries)
{
	GC_ERROR error = GC_ERR_SUCCESS;
	size_t i;

	MOCK_ENTER (GCWritePortStacked);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_CHECK_PARAMETER (pEntries != NULL && piNumEntries != NULL);

	for (i = 0; i < *piNumEntries && error == GC_ERR_SUCCESS; i++)
		error = mock_port_write (hPort, pEntries[i].Address, pEntries[i].Buffer, &pEntries[i].Size);

	if (error != GC_ERR_SUCCESS)
		*piNumEntries = i - 1;

	MOCK_RETURN (error);
}

GC_ERROR
GCReadPortStacked (PORT_HANDLE hPort, PORT_REGISTER_STACK_ENTRY *pEntries, size_t *piNumEntries)
{
	GC_ERROR error = GC_ERR_SUCCESS;
	size_t i;

	MOCK_ENTER (GCReadPortStacked);
	MOCK_CHECK (hPort, MOCK_HANDLE_PORT);
	MOCK_CHECK_PARAMETER (pEntries != NULL && piNumEntries != NULL);

	for (i = 0; i < *piNumEntries && error == GC_ERR_SUCCESS; i++)
		error = mock_port_read (hPort, pEntries[i].Address, pEntries[i].Buffer, &pEntries[i].Size);

	if (error != GC_ERR_SUCCESS)
		*piNumEntries = i - 1;

	MOCK_RETURN (error);
}

#endif

/* Event functions */

GC_ERROR
GCRegisterEvent (EVENTSRC_HANDLE hModule, EVENT_TYPE iEventID, EVENT_HANDLE *phEvent)
{
	MOCK_ENTER (GCRegisterEvent);
	MOCK_RETURN (mock_register_event (hModule, iEventID, phEvent));
}

GC_ERROR
GCUnregisterEvent (EVENTSRC_HANDLE hModule, EVENT_TYPE iEventID)
{
	MOCK_ENTER (GCUnregisterEvent);
	MOCK_RETURN (mock_unregister_event (hModule, iEventID));
}

/* Waits for new buffers are counted apart from the waits of the device event thread, scripted failures included */

static void
mock_count_event_wait (EVENT_HANDLE hEvent)
{
	g_mutex_lock (&mock_mutex);

	if (mock_handle_get_type (hEvent) == MOCK_HANDLE_EVENT &&
	    ((MockEvent *) hEvent)->type == EVENT_NEW_BUFFER)
		mock_n_new_buffer_waits++;

	g_mutex_unlock (&mock_mutex);
}

static GC_ERROR
mock_event_get_data_call (EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout)
{
	MOCK_ENTER (EventGetData);

	if (!mock_check_event (hEvent, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	MOCK_RETURN (mock_event_get_data (hEvent, pBuffer, piSize, iTimeout));
}

GC_ERROR
EventGetData (EVENT_HANDLE hEvent, void *pBuffer, size_t *piSize, uint64_t iTimeout)
{
	mock_count_event_wait (hEvent);

	return mock_event_get_data_call (hEvent, pBuffer, piSize, iTimeout);
}

GC_ERROR
EventKill (EVENT_HANDLE hEvent)
{
	MOCK_ENTER (EventKill);
	MOCK_CHECK (hEvent, MOCK_HANDLE_EVENT);

	/* A kill racing with the unregistration of the event is counted apart */
	if (!((MockEvent *) hEvent)->is_registered) {
		mock_n_kills_unregistered++;
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);
	}

	MOCK_RETURN (mock_event_kill (hEvent));
}

#if MOCK_HAS_OPTIONAL_FUNCTIONS

static GC_ERROR
mock_event_flush (MockEvent *event)
{
	MockStream *stream = event->type == EVENT_NEW_BUFFER ? event->module : NULL;

	if (stream != NULL) {
		g_queue_foreach (&stream->output, mock_buffer_set_idle, NULL);
		g_queue_clear (&stream->output);
	}

	return GC_ERR_SUCCESS;
}

GC_ERROR
EventFlush (EVENT_HANDLE hEvent)
{
	MOCK_ENTER (EventFlush);

	if (!mock_check_event (hEvent, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	MOCK_RETURN (mock_event_flush (hEvent));
}

#endif

#if MOCK_HAS_EVENT_INFO_FUNCTIONS

static GC_ERROR
mock_device_event_data_info (const void *in_buffer, size_t in_size, EVENT_DATA_INFO_CMD info_cmd,
			     INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MockDeviceEventData data;

	if (in_buffer == NULL || in_size < sizeof (data))
		return GC_ERR_INVALID_PARAMETER;

	memcpy (&data, in_buffer, sizeof (data));

	switch (info_cmd) {
		case EVENT_DATA_ID:
			/* Hexadecimal digits, without the zero ending a string */
			return mock_output (INFO_DATATYPE_STRING, mock_config.device_event_id,
					    strlen (mock_config.device_event_id), piType, pBuffer, piSize);
		case EVENT_DATA_VALUE:
			return mock_output (INFO_DATATYPE_BUFFER, &data, sizeof (data), piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

static GC_ERROR
mock_event_data_info (MockEvent *event, const void *in_buffer, size_t in_size, EVENT_DATA_INFO_CMD info_cmd,
		      INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	EVENT_NEW_BUFFER_DATA data;

	if (mock_event_has_device_events (event))
		return mock_device_event_data_info (in_buffer, in_size, info_cmd, piType, pBuffer, piSize);

	if (event->type != EVENT_NEW_BUFFER)
		return GC_ERR_NOT_AVAILABLE;

	if (in_buffer == NULL || in_size < sizeof (EVENT_NEW_BUFFER_DATA))
		return GC_ERR_INVALID_PARAMETER;

	memcpy (&data, in_buffer, sizeof (data));

	switch (info_cmd) {
		case EVENT_DATA_ID:
			return mock_output_ptr (data.BufferHandle, piType, pBuffer, piSize);
		case EVENT_DATA_VALUE:
			return mock_output_ptr (data.UserPointer, piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

static GC_ERROR
mock_event_info (MockEvent *event, EVENT_INFO_CMD info_cmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MockStream *stream = event->type == EVENT_NEW_BUFFER ? event->module : NULL;
	size_t size_max = event->type == EVENT_NEW_BUFFER ? sizeof (EVENT_NEW_BUFFER_DATA) : MOCK_EVENT_DATA_SIZE_MAX;

	switch (info_cmd) {
		case EVENT_EVENT_TYPE:
			return mock_output_int32 (event->type, piType, pBuffer, piSize);
		case EVENT_NUM_IN_QUEUE:
			return mock_output_sizet (stream != NULL ? g_queue_get_length (&stream->output) : 0,
						  piType, pBuffer, piSize);
		case EVENT_NUM_FIRED:
			return mock_output_uint64 (event->n_fired, piType, pBuffer, piSize);
		case EVENT_SIZE_MAX:
		case EVENT_INFO_DATA_SIZE_MAX:
			return mock_output_sizet (size_max, piType, pBuffer, piSize);
		default:
			return GC_ERR_NOT_AVAILABLE;
	}
}

GC_ERROR
EventGetDataInfo (EVENT_HANDLE hEvent, const void *pInBuffer, size_t iInSize, EVENT_DATA_INFO_CMD iInfoCmd,
		  INFO_DATATYPE *piType, void *pOutBuffer, size_t *piOutSize)
{
	MOCK_ENTER (EventGetDataInfo);

	if (!mock_check_event (hEvent, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	MOCK_RETURN (mock_event_data_info (hEvent, pInBuffer, iInSize, iInfoCmd, piType, pOutBuffer, piOutSize));
}

GC_ERROR
EventGetInfo (EVENT_HANDLE hEvent, EVENT_INFO_CMD iInfoCmd, INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
{
	MOCK_ENTER (EventGetInfo);

	if (!mock_check_event (hEvent, mock_function))
		MOCK_RETURN (GC_ERR_INVALID_HANDLE);

	MOCK_RETURN (mock_event_info (hEvent, iInfoCmd, piType, pBuffer, piSize));
}

#endif

/* Control functions, reached through GModule */

static int
mock_get_counter (const char *name)
{
	GList *iter;
	int value = 0;
	guint i;

	for (i = 0; i < MOCK_N_FUNCTIONS; i++)
		if (g_strcmp0 (name, mock_function_names[i]) == 0)
			return mock_n_calls[i];

	if (g_strcmp0 (name, "violations") == 0)
		return mock_n_violations;
	if (g_strcmp0 (name, "kills-unregistered") == 0)
		return mock_n_kills_unregistered;
	if (g_strcmp0 (name, "frames") == 0)
		return mock_n_frames;
	if (g_strcmp0 (name, "underruns") == 0)
		return mock_n_underruns;
	if (g_strcmp0 (name, "new-buffer-waits") == 0)
		return mock_n_new_buffer_waits;
	if (g_strcmp0 (name, "device-events") == 0)
		return mock_n_device_events;
	if (g_strcmp0 (name, "new-buffer-waiters") == 0) {
		for (iter = mock_events; iter != NULL; iter = iter->next) {
			MockEvent *event = iter->data;

			if (event->type == EVENT_NEW_BUFFER)
				value += event->n_waiters;
		}
		return value;
	}
	if (g_strcmp0 (name, "new-buffer-events") == 0) {
		for (iter = mock_events; iter != NULL; iter = iter->next) {
			MockEvent *event = iter->data;

			if (event->type == EVENT_NEW_BUFFER && event->is_registered)
				value++;
		}
		return value;
	}
	if (g_strcmp0 (name, "initialized") == 0)
		return mock_is_initialized ? 1 : 0;
	if (g_strcmp0 (name, "open-tl") == 0)
		return mock_system_is_open ? 1 : 0;
	if (g_strcmp0 (name, "open-interfaces") == 0) {
		for (i = 0; i < mock_n_interfaces; i++)
			value += mock_interfaces[i].is_open ? 1 : 0;
		return value;
	}

	if (g_strcmp0 (name, "open-devices") == 0) {
		for (i = 0; i < mock_n_devices; i++)
			value += mock_devices[i]->is_open ? 1 : 0;
		return value;
	}
	if (g_strcmp0 (name, "open-streams") == 0) {
		for (i = 0; i < mock_n_devices; i++)
			value += mock_devices[i]->stream != NULL ? 1 : 0;
		return value;
	}
	if (g_strcmp0 (name, "announced-buffers") == 0) {
		for (i = 0; i < mock_n_devices; i++)
			if (mock_devices[i]->stream != NULL)
				value += g_list_length (mock_devices[i]->stream->buffers);
		return value;
	}
	if (g_strcmp0 (name, "registered-events") == 0) {
		for (iter = mock_events; iter != NULL; iter = iter->next)
			value += ((MockEvent *) iter->data)->is_registered ? 1 : 0;
		return value;
	}

	g_debug ("Unknown counter '%s'", name);

	return -1;
}

int
arv_gentl_mock_get_counter (const char *name)
{
	int value;

	g_return_val_if_fail (name != NULL, -1);

	g_mutex_lock (&mock_mutex);
	value = mock_get_counter (name);
	g_mutex_unlock (&mock_mutex);

	return value;
}

const char *
arv_gentl_mock_get_last_violation (void)
{
	return mock_last_violation;
}

gboolean
arv_gentl_mock_set_option (const char *name, const char *value)
{
	gboolean success = FALSE;

	g_return_val_if_fail (name != NULL, FALSE);

	g_mutex_lock (&mock_mutex);

	/* The number of devices and interfaces is fixed by GCInitLib */
	if (mock_is_initialized && g_strcmp0 (name, "ARV_GENTL_MOCK_N_DEVICES") != 0 &&
	    g_strcmp0 (name, "ARV_GENTL_MOCK_N_INTERFACES") != 0)
		success = mock_config_set (&mock_config, name, value);

	g_mutex_unlock (&mock_mutex);

	return success;
}

void
arv_gentl_mock_reset_counters (void)
{
	g_mutex_lock (&mock_mutex);

	memset (mock_n_calls, 0, sizeof (mock_n_calls));
	mock_n_violations = 0;
	mock_n_kills_unregistered = 0;
	mock_n_frames = 0;
	mock_n_underruns = 0;
	mock_n_new_buffer_waits = 0;
	mock_n_device_events = 0;
	mock_last_violation[0] = '\0';

	g_mutex_unlock (&mock_mutex);
}
