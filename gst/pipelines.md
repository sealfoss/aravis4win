Title: GStreamer Plugin

# GStreamer plugin

The Aravis GStreamer plugin, built with `-Dgst-plugin=enabled`, has two features:

- `aravissrc`, a live video source;
- `aravisdeviceprovider`, a device provider that lists the cameras for
  `GstDeviceMonitor`.

The examples below use `gst-launch-1.0`. In the build tree, `gst/gst-aravis-launch`
and `gst/gst-aravis-inspect` run `gst-launch-1.0` and `gst-inspect-1.0` with the
plugin of the build.

## Simple pipeline

```
gst-launch-1.0 aravissrc ! videoconvert ! autovideosink
```

## Format filter

```
gst-launch-1.0 aravissrc ! video/x-raw,format=GRAY16_LE,depth=12 ! videoconvert ! autovideosink
```

## Camera selection

`camera-name` selects a camera by its identifier, as listed by `arv-tool-0.10`.
Setting it opens the camera. Without it, `aravissrc` opens the first camera found.

```
gst-launch-1.0 aravissrc camera-name=Aravis-Fake-GV01 ! videoconvert ! autovideosink
```

`interface` restricts the search to one interface: `USB3Vision`, `GigEVision`,
`GenTL`, `Fake`, or `V4L2` when Aravis is built with V4L2 support, the
identifiers [func@Aravis.get_interface_id] returns. A USB3Vision or GenTL camera
then opens without waiting for the GigEVision discovery, and the interfaces
disabled for device discovery, Fake and GenTL by default, can be used. Without
`camera-name`, `aravissrc` opens the first camera of the interface. An unknown
interface makes the start fail with a resource "settings" error that names it.

A GenTL camera needs `interface=GenTL`, unless the GenTL interface is enabled for
device discovery, with `ARV_GENTL_ENABLE=1` or [func@Aravis.enable_interface]:
`camera-name` alone only searches the enabled interfaces, and the camera is then
not found. The identifiers of GenTL cameras start with the vendor of the
transport layer of their GenTL producer, as listed by `arv-tool-0.10 --gentl`:

```
gst-launch-1.0 aravissrc interface=USB3Vision ! videoconvert ! autovideosink
gst-launch-1.0 aravissrc interface=GenTL camera-name=TLVendor-Vendor-Model-Serial ! videoconvert ! autovideosink
```

Write `interface` before `camera-name`. Setting `camera-name` opens the camera, so
when it comes first, it searches every enabled interface, at the cost of their
discovery (at least 1 s for GigEVision), before `interface` opens the camera
again. The order does not change the errors: when a property setter can't open
the camera, `aravissrc` only logs a warning, and opens the camera again when it
starts, which posts the error message if it still fails.

`aravissrc` opens cameras under the lock of the device list of Aravis, with or
without `interface`, so an application can list or open cameras with the Aravis
API on other threads meanwhile. Opening the first camera of an interface updates
the device list of that interface, as [func@Aravis.update_device_list] does: the
strings returned by [func@Aravis.get_device_id] and the other functions querying
the device list stay valid anyway.

## USB3Vision transfers

`usb-mode` selects the synchronous or the asynchronous libusb API, asynchronous by
default. `usb-maximum-transfer-size` sets the size of the USB bulk transfers, in
bytes: 0, the default, keeps the library default, 1 MiB, and other sizes must be
between 1024 bytes and 64 MiB (67108864 bytes). `aravissrc` refuses the others
with a warning and keeps its previous value. Both properties apply wherever they
appear relative to `camera-name` and `interface`, and only affect USB3Vision
cameras.

WinUSB RAW_IO is not a property of `aravissrc`: the `ARV_UV_RAW_IO` environment
variable controls it, see "Transfer size and RAW_IO" in the USB documentation.

```
gst-launch-1.0 aravissrc interface=USB3Vision usb-mode=async usb-maximum-transfer-size=262144 ! videoconvert ! autovideosink
```

## Latency

`aravissrc` is a live source. It reports a minimum latency of one frame period, 40
ms at 25 fps, and no maximum, as a camera may stop sending frames for a while. In
trigger mode the frame rate is unknown, and `aravissrc` leaves the latency query
unanswered.

## Frame memory

`aravissrc` passes frames downstream without copying them: a buffer owns the
memory of its frame and gives it back to the camera stream when the last element
or application holding it releases it. A frame kept after the pipeline stopped or
was renegotiated stays valid, and its memory is freed when it is released. A
frame is copied instead when lending its memory would leave the camera stream
with fewer than 2 buffers, so that a queue or an application holding many frames
costs copies instead of starving the camera: with `num-arv-buffers=2` or less,
every frame is copied.

The frames of a GenTL camera are always copied. The memory of a GenTL frame keeps
the data stream of its GenTL producer, and its device, open until it is
released, and a producer may refuse to open them again meanwhile: a lent frame
held by a sink, as GStreamer sinks keep the last frame by default, would make the
next caps change fail, and one held by the application, the next start.

```
gst-launch-1.0 aravissrc num-arv-buffers=50 ! queue max-size-buffers=20 ! videoconvert ! autovideosink
```

## Caps changes

When the caps change, `aravissrc` stops its camera stream, releases it, then
creates and starts the stream of the new caps. It only stops and starts its own
stream, not every stream of the camera.

## Stream and camera objects

The `stream` and `camera` properties give the `ArvStream` and the `ArvCamera` of
`aravissrc`, to read the stream statistics or camera features for example. They
are NULL while the element has none. `aravissrc` notifies `stream` when it
creates its stream and when it releases it: at a caps change, before it creates
the next one, and when it stops. It notifies `camera` when it opens its camera
and when it releases it, when it stops for example. The notifications of a caps
change come from the streaming thread.

An application that holds one of these objects should release it at each
notification, including those after which the property is NULL. A stream or a
camera keeps the device of the camera open, and some cameras need the stream of
the previous caps gone before the next one is created, or the device closed
before it opens again:

- A GenTL producer may refuse to open a data stream of a device while another
  one is open, and to open a device that is already open: the caps change, or
  the next start, then fails.
- On Windows, WinUSB lets a single handle open a USB3Vision device at a time: a
  stream or a camera still held keeps the next start from opening the camera.

A stream of a GigEVision, USB3Vision or Fake camera held across a caps change
does not disturb the new one: `aravissrc` does not restart it, and releasing it
later leaves the camera streaming to the new one. A GigEVision stream that is
released only disables the stream channel of the camera if no newer stream has
configured the channel since.

## Lost camera and missing frames

When frames stop, `aravissrc` posts an error that names the camera, by its
`camera-name`, or for the first camera found, as vendor-model-serial, from what
the camera tells of itself. That label is often the identifier of the camera,
but not always: a GenTL camera identifier starts with the vendor of the transport
layer, for example. GstBaseSrc's generic "Internal data stream error" follows:
act on the first error.

- The device of the camera emitted [signal@Aravis.Device::control-lost]: the
  camera was unplugged or switched off, or another application took control of
  it. The error follows within about 50 ms. A USB3Vision camera is usually found
  lost within a second, see "Device loss" in the USB documentation. A GigEVision
  camera is declared lost after its heartbeat retries, about 5 s, so a stalled
  stream usually reports the frame timeout first.
- No frame arrived within `buffer-timeout`, in ms. The default, -1, is the
  longest of 2 s and 3 frame periods. A failed frame, an incomplete one for
  example, restarts the wait: an occasional failed frame does not end a stream
  whose frames come less than `buffer-timeout` apart, like that of a camera
  triggered every 1.5 s with the default timeout of 2 s. From the 10th failed
  frame since the last complete one on, failed frames no longer restart it: a
  stream of failed frames ends `buffer-timeout` after the last complete frame,
  with an error giving their number and the last status. The details of both
  errors list the stream statistics.
- `buffer-timeout=0`, for a camera waiting for triggers, waits forever for a
  frame, but 10 failed frames in a row end the wait with an error. A camera that
  is unplugged or switched off fails its last frames before its device notices
  the loss: when the device emits control-lost within 250 ms of the 10th failed
  frame, the error says that the camera was lost.

Waiting for a frame never blocks property accesses or queries, and a state change
interrupts it within about 50 ms.

```
gst-launch-1.0 aravissrc trigger=Line1 buffer-timeout=0 ! videoconvert ! autovideosink
```

When the camera can't be opened, the error names it by its `camera-name`, or,
without one, as "a camera on interface X", or "a camera" when no interface is
set either. Without `interface`, the details of a "not found" error recall that
only the interfaces enabled for device discovery were searched.

## Device monitor

The device provider, `aravisdeviceprovider`, lists the cameras for
`GstDeviceMonitor` as `Video/Source` devices. Its rank is marginal, the lowest
named rank: `GstDeviceMonitor` uses the providers of rank 1 or above.

```
gst-device-monitor-1.0 Video/Source
```

Each listing runs [func@Aravis.update_device_list]: it finds the cameras of the
interfaces enabled for device discovery in the process. Fake and GenTL are
disabled by default, so GenTL cameras are only listed with `ARV_GENTL_ENABLE=1`,
or once the application has called [func@Aravis.enable_interface]. A listing
costs what the discovery of the enabled interfaces costs: at least 1 s for
GigEVision, which broadcasts a discovery request on every network interface and
waits for the answers, and an enumeration of the USB devices for USB3Vision. A
monitor started with `gst_device_monitor_start ()` keeps the cameras found when it
started: the provider does not watch cameras being plugged or unplugged.

As `GstDeviceMonitor` uses the provider in every application, installing the
plugin makes every application that lists video sources pay that discovery. The
environment variable `GST_ARAVIS_DEVICE_PROVIDER=0` (or `off`, `false`, `no`)
disables the provider without changing the application: its probes then run no
discovery and list no camera. The `GST_PLUGIN_FEATURE_RANK` environment variable
does not apply to device providers (GStreamer 1.28). A program can also spare the
GigEVision discovery with [func@Aravis.disable_interface], or keep
`GstDeviceMonitor` from using the provider by setting the rank of the
`aravisdeviceprovider` factory to none with `gst_plugin_feature_set_rank ()`. The
probes use the device list of Aravis as an application would, under its lock: an
application can use the Aravis API on other threads meanwhile. A probe lists the
cameras of one state of the device list, even when another thread updates the
list during the probe: it reads the list again if the update changed it, see
[func@Aravis.get_device_list_generation]. Updates that find the same cameras
don't disturb the probe, and a list that changes during each of ten reads in a
row gives no camera, until the next probe.

On Windows, the USB3Vision discovery opens each camera, while WinUSB lets a
single handle open a device at a time: a camera that the process is streaming,
through an `aravissrc` or the Aravis API, is not listed meanwhile. Its stream is
not disturbed, and the camera is listed again once it stops.

A device is named "vendor model (serial)". Its caps are `video/x-raw` and
`video/x-bayer`: the actual formats are only known once the camera is open. Its
properties are `device.api` (aravis), `device.vendor.name`,
`device.product.name` and `device.serial`, then `aravis.device-id`,
`aravis.physical-id`, `aravis.address`, `aravis.protocol` and `aravis.interface`.

The element of a device is an `aravissrc` with `interface` and `camera-name` set.
Unlike setting `camera-name`, creating it does not open the camera, which opens
when the element starts: the creation is cheap and takes no camera, and a camera
that can't be opened fails the start with an error on the bus. The `camera`
property stays empty until then. `gst-device-monitor-1.0` prints the element as a
launch line, `interface` first, so that the camera opens through its interface
even when that interface is disabled for device discovery, as GenTL is by
default:

```
gst-launch-1.0 aravissrc interface=GigEVision camera-name=Aravis-Fake-GV01 ! videoconvert ! autovideosink
gst-launch-1.0 aravissrc interface=GenTL camera-name=TLVendor-Vendor-Model-Serial ! videoconvert ! autovideosink
```
