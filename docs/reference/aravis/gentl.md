Title: GenTL Devices

# GenTL

Aravis can load camera vendor's GenTL producers to connect GenICam cameras.
It searches GenTL producers, dynamic libraries with *.cti* extension,
in the paths defined by environment variable `GENICAM_GENTL{32/64}_PATH`
(`GENICAM_GENTL64_PATH` for 64-bit builds, `GENICAM_GENTL32_PATH` for 32-bit ones).
Each path can be either a directory, whose files with a name ending in `.cti`
(lower case) are loaded, or a file, loaded whatever its name. Directories are not
searched recursively. A producer found twice, by the same path or by another path
to the same library (a symbolic link, or another spelling of its folder on
Windows), is loaded once.

The GenTL support is experimental.

## Enabling the GenTL interface

The GenTL interface is disabled by default, like the Fake interface. Vendor SDKs
usually define `GENICAM_GENTL64_PATH` for the whole system, and every producer
found in these paths is loaded by the device discovery. The interface is enabled
by any of:

- [func@Aravis.enable_interface] with `"GenTL"`, or [func@Aravis.select_interface]
  to use it alone, which also avoids the GigEVision discovery delay;
- the `ARV_GENTL_ENABLE` environment variable set to `1` (or `on`, `true`, `yes`),
  for applications without such a switch, like `gst-launch-1.0`. It is read once,
  by the first call to the device discovery or to the interface selection
  functions, and the selection made by the application afterwards prevails;
- the `--gentl` option of `arv-tool`, `arv-camera-test`, `arv-test` and
  `arv-viewer`.

A device can still be opened through the disabled interface by
[func@Aravis.open_device_on_interface], which only runs the discovery of the
GenTL producers, and can be called from any thread:

```c
ArvDevice *device = arv_open_device_on_interface ("GenTL", device_id, &error);
```

A `NULL` device id opens the first device the producers list. To list the
GenTL devices with [func@Aravis.update_device_list] and the functions querying
the device list, like [func@Aravis.get_device_id], enable the interface:
[func@Aravis.get_device_interface_id] then tells which devices it lists. The
methods of the [class@Aravis.Interface] returned by
[func@Aravis.get_interface_by_id] don't take the lock of these functions, and
must not be used while another thread may use the device list, as the Aravis
GStreamer plugin does (see [Thread Safety](thread-safety.html)).

## Producer selection

The `ARV_GENTL_PRODUCERS` environment variable restricts the loaded producers to
a list of file names, separated like the paths of `GENICAM_GENTL64_PATH` (`;` on
Windows, `:` elsewhere). The match ignores the case and the directory part of the
list entries, and accepts the `*` and `?` wildcards. On Linux and macOS, a
symbolic link matches by its own name or by the name of its target. The variable
is read each time the producers are loaded:

```sh
ARV_GENTL_ENABLE=1 ARV_GENTL_PRODUCERS="VendorU3V*.cti" arv-tool-0.10
```

## Device identifiers

The id of a GenTL device is `<TL vendor>-<vendor>-<model>-<serial number>`, where
the TL vendor is the vendor of the producer. The device can also be opened with
`<TL vendor>-<vendor>-<serial number>`, the same with the vendor alias, or the
device id given by the producer. Control characters are removed from the ids.

Producers don't always tell every string. A missing transport layer vendor is
replaced by the producer file name without extension, and the other missing
strings by empty strings, except the serial number: in the Aravis device ids, a
missing serial number is replaced by `<producer>-<interface id>-<device id>`, the
producer file name without extension and the GenTL ids of the interface and of
the device. Within each of these three components, only ASCII letters, digits,
`_`, `.` and `~` are kept unchanged; every other byte is encoded as `%HH`, with
uppercase hexadecimal digits. In particular, a hyphen becomes `%2D` and a literal
percent sign becomes `%25`. This keeps component boundaries distinct and
preserves control bytes through device-name sanitization. For example,
`Arv-Q.cti` / `A0` / `Device0` gives `Arv%2DQ-A0-Device0`, while
`Arv.cti` / `Q-A0` / `Device0` gives `Arv-Q%2DA0-Device0`.

This encoding changes fallback ids containing these special bytes. Fallback
ids whose components contain only the unchanged characters, and ids with a
supplied serial number, keep their previous spelling. A GenTL device id is only
unique in its interface, and a vendor may ship several producers. The device id
given by the producer, which can also open the device, is not made unique: when
several interfaces or producers have a device with this id, it opens one of
them, which one is not defined. Copies of one producer with the same file name,
in different folders, still share these ids.

## Producer loading

Producers are loaded by the first discovery, and stay loaded until
[func@Aravis.shutdown]. A producer still used by a device, a stream or a buffer
is only closed with the last of them, even after [func@Aravis.shutdown]. The
next discovery loads the producers again, except the ones still in use, which
it shares with their devices: a producer is never initialized twice.

The producer modules opened on demand, the transport layer and its interfaces,
are shared by the discovery and the devices. A device can be destroyed by any
thread, during a discovery.

On Windows, a producer is loaded by its absolute path, a relative path of
`GENICAM_GENTL64_PATH` being relative to the current directory, with
`LoadLibraryExW` and `LOAD_WITH_ALTERED_SEARCH_PATH`: vendors usually install a
producer with the libraries it depends on, in a folder which is not in the
library search path. The dependencies of the producer are then searched in the
folder of the producer, the system folders, the current folder, and the folders
of `PATH`, but not in the folder of the application. A dependency whose name is
the name of a library already loaded by the process is not searched at all:
Windows uses the loaded one, whatever its folder and its version. No error
dialog box is shown when a dependency is missing.

On Linux and macOS, producers are loaded with `dlopen`, which searches their
dependencies in the usual library paths and in the run paths of the producer.

A producer which can't be loaded, misses a required function, or fails its
initialization is skipped, with a warning naming the file and the error (on
Windows, the system error message and code, for example 126 for a missing
library). A producer which fails to open its transport layer (`TLOpen`) is
skipped by each discovery, with a warning the first time. More details are
given by the `interface` debug category (`ARV_DEBUG=interface:3`).

Aravis only requires the functions it calls. A producer may omit the following
ones: `GCGetInfo`, `GCGetLastError`, `GCReadPortStacked`, `GCWritePortStacked`,
`GCGetPortURL`, `GCGetPortInfo`, `GCGetNumPortURLs`, `IFGetInfo`,
`IFGetParentTL`, `DevGetParentIF`, `DSGetInfo`, `DSGetBufferID`,
`DSGetBufferChunkData`, `DSGetParentDev`, `EventFlush`, the multi-part functions
`DSGetNumBufferParts` and `DSGetBufferPartInfo` (buffers are then single part),
and, unless Aravis is built with the experimental event support,
`EventGetInfo` and `EventGetDataInfo`. The functions added by GenTL 1.6
(composite buffers, flows, segments, stacked buffer information) are not used.

## Streaming

The buffers created by [method@Aravis.Stream.create_buffers] are allocated by the
producer. The other ones are announced to the producer by the acquisition start,
or by the stream thread when they are pushed during the acquisition.

When the data stream can't be opened, [method@Aravis.Camera.create_stream]
returns `NULL` and an error. When a buffer can't be announced or queued, or when
the producer fails to register the new buffer event or to start the acquisition,
[method@Aravis.Stream.start_acquisition] and
[method@Aravis.Camera.start_acquisition] return `FALSE` and an
`ARV_DEVICE_ERROR_STREAM_ERROR` error, which names the failed GenTL function, its
error, and the producer description of the error when `GCGetLastError` gives
one. The stream is then left as it was, with its buffers in the input queue, and
a stop does nothing. A buffer pushed during the acquisition and refused by the
producer is not retried: it comes back in the output queue with the
`ARV_BUFFER_STATUS_ABORTED` status, and counts as a failure. The refused buffers
come back once the stream thread has emptied the input queue, and it then waits
for a frame, or 50 ms when the producer holds no buffer, before queueing the
buffers pushed again: an application pushing them back at once, even from its
stream callback or its `new-buffer` signal handler, doesn't make it loop, and
can stop the acquisition.

The buffer status follows the producer flags: `ARV_BUFFER_STATUS_SIZE_MISMATCH`
when the data was larger than the buffer, `ARV_BUFFER_STATUS_MISSING_PACKETS`
for an incomplete frame, and `ARV_BUFFER_STATUS_UNKNOWN` when the producer gives
neither the payload type nor the filled size of the buffer, for example after an
error. A buffer whose filled size, or the size or position of one of its parts,
exceeds the buffer memory by the producer's account also gets the
`ARV_BUFFER_STATUS_SIZE_MISMATCH` status, whatever its flags: the data returned
by [method@Aravis.Buffer.get_data] and the part functions is then cut to the
buffer memory. The buffers still queued when the acquisition stops
come back with the `ARV_BUFFER_STATUS_ABORTED` status, and are counted by the
`n_aborted` stream information. The buffers left in the output queue are used
again by the next acquisition start.

The stream thread waits for the new buffers with a 50 ms timeout, and checks for
a stop request between two waits. The stop ends a wait in progress with
`EventKill`, and a producer losing or ignoring it only delays the stop by this
timeout.

## Aravis GenTL producer

Aravis can also be a GenTL producer, for the GenTL consumers of other software.
It is built with the experimental `-Dgentl-producer=true` meson option, and
installed as `gentlproducer/ProducerAravis-0.10.cti` in the library folder. Its
folder, or the file itself, must be added to `GENICAM_GENTL64_PATH`.

Its system module exposes one interface for each Aravis interface, enabled or
not, except GenTL: Fake and GigEVision, and USB3Vision and V4L2 when Aravis is
built with them. Their GenTL transport layer types are `GEV` for GigEVision,
`U3V` for USB3Vision, and `Custom` for the other ones.
The producer exports every function of the GenTL API, but only implements the
device discovery, the device information, the opening of a device, and the
reading of its registers and of its GenICam description. The register writes,
the data streams and the events return `GC_ERR_NOT_IMPLEMENTED`: the cameras can
be listed and their features read, but not changed, and no image can be
acquired. `src/gentl/TODO.md` lists the missing parts.

The producer doesn't change the interface selection of the process. An Aravis
application with the GenTL interface enabled can load it, which lists the
devices of Aravis a second time, through the producer: its fake camera is
`Aravis-Vendor-Aravis-Fake-1`. When it is in the same folder as vendor
producers, listing these ones in `ARV_GENTL_PRODUCERS` keeps it out. Its
interface handles, the Aravis interfaces of the process, stay valid until
`IFClose`, even after [func@Aravis.shutdown].

## Limits

- On 32-bit x86, GenTL producers use the `stdcall` calling convention, which
  Aravis doesn't declare for the GenTL functions, neither as a consumer nor as
  a producer. Only 64-bit builds can be used with GenTL producers.
- Only the first data stream of a device is used. The `n_underruns` stream
  information is not counted.
- The device events are only received with the experimental event support
  (`-Devent=true`), from producers which implement them: with the other ones,
  like the Aravis producer, the device has no event.
- Producers run in the process of the application: a producer which crashes or
  blocks takes the application with it.

## Tests

The `gentl` test suite runs the GenTL consumer against mock producers, built in
`tests/gentlmock`. Their behaviour can be scripted to reproduce producer
failures: see `tests/gentlmock/arvgentlmock.h`. With `-Dgentl-producer=true`, it
also loads the Aravis producer, directly and through the consumer.
