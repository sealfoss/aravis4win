Title: USB Devices

# USB

## Permissions

By default, USB devices permissions may not be sufficient to allow any user to
access the USB3 cameras. These permissions can be changed by using an udev rule
file. There is a file example in [Aravis
sources](https://github.com/AravisProject/aravis/blob/main/src/aravis.rules).
This file must be placed in `/etc/udev/rules.d` directory (The exact location
may depend on the distribution you are using). This file only contains
declarations for a couple of vendors. If you want to add an entry with the
vendor of your camera, the output of `lsusb` command will give you the vendor
id, which is the first 4 digits of the ID field.

## Windows

On Windows, Aravis reaches USB3 Vision cameras through libusb, which can only
open a device whose driver is one of these:

- WinUSB (`winusb.sys`), part of Windows, the recommended driver;
- libusbK or libusb0 (libusb-win32), two drivers that Zadig can install, which
  libusb drives through `libusbK.dll`. libusb loads that DLL from the `System32`
  folder of Windows only, where Zadig installs it with these drivers.

Windows installs no driver for a USB3 Vision camera by itself, and the software
of a camera vendor installs the vendor's own driver: until one of these drivers
drives a camera, Aravis does not list it. Windows gives each device a single
driver, so a camera bound to WinUSB is out of reach of the vendor's software,
and the reverse.

Prefer WinUSB: it needs no other DLL, and the USB code of Aravis follows its
documentation, for [RAW_IO](#winusb-raw_io) in particular. The routes below
bind WinUSB.

There are three ways to bind WinUSB to a camera: the driver package of Aravis
(route 1), Zadig (route 2) and Device Manager (route 3). Whichever you use, check
the result as described below.

### Where to bind WinUSB

A USB3 Vision camera is a composite USB device, for which Windows creates two
device nodes:

| Node | Instance ID | Compatible ID | Default driver |
| --- | --- | --- | --- |
| Composite device | `USB\VID_vvvv&PID_pppp\<serial>` | `USB\COMPOSITE` | `usbccgp`, which creates a node for each function |
| USB3 Vision function | `USB\VID_vvvv&PID_pppp&MI_00\<…>` | `USB\Class_EF&SubClass_05&Prot_00` | none, or the vendor's driver |

Bind WinUSB to the USB3 Vision function. libusb opens the camera through the
composite device and claims the interfaces of the function, which needs libusb
1.0.27 or later, the version that every Windows build of Aravis requires (see
[libusb version](building.html#libusb-version)). Binding WinUSB to the
composite device instead, as older guides do, works with any libusb version: do
it only with a libusb older than 1.0.27.

The `MI_00` suffix and the compatible ID assume that the USB3 Vision function
of the camera starts at its interface 0, and that the function has the protocol
00. Both are unverified on real cameras: check them on yours.

### Checking a camera

On Windows 11, `pnputil` lists the device nodes with their IDs and drivers:

```
pnputil /enum-devices /connected /deviceids /services /drivers
```

Look for the node whose compatible IDs include `USB\Class_EF&SubClass_05`
(from Windows 11 22H2, add `/deviceid "USB\Class_EF&SubClass_05"` to list only
those nodes). Aravis can open the camera when the `Service` of that node is
WinUSB, which `pnputil` may print as `WINUSB`, or libusbK or libusb0 with
`libusbK.dll` in the `System32` folder of Windows.

`arv-winusb-setup.ps1` does this check for every connected camera, with that
of `libusbK.dll` for libusbK and libusb0, and says whether Aravis can open it
and why not. Aravis installs it on Windows, with the driver package of route 1,
in `share\aravis-0.10\winusb` (`data/windows` in the sources). Windows does not
run PowerShell scripts by default, hence the `-ExecutionPolicy` option:

```
powershell -ExecutionPolicy Bypass -File arv-winusb-setup.ps1
```

Some camera vendors install WinUSB themselves. When the vendor's driver package
of a camera makes WinUSB its driver, Aravis can open the camera as it is,
whenever the vendor's software does not have it open: there is nothing to
change. The helper reports this case too.

### Route 1: the driver package of Aravis

`aravis-u3v.inf` makes WinUSB the driver of the USB3 Vision function of every
camera that has no better-ranked driver. It matches the compatible ID
`USB\Class_EF&SubClass_05&Prot_00`, so it covers the cameras of every vendor, and
it registers the device interface GUID that libusb needs. This route suits
developers and controlled deployments.

Windows installs only signed driver packages, and Microsoft does not sign this
one: Inf2Cat, the catalog tool of the Windows Driver Kit, rejects a third-party
USB driver that matches a compatible ID (its signability test B2.6.4.9). Sign
the package with your own certificate, with MakeCat and SignTool from the
Windows SDK, which `aravis-u3v.cdf` makes a two-command step.

Once, create a code-signing certificate and export its public part. Keep its
private key safe, or delete it after signing: every machine that trusts the
certificate trusts any code signed with it.

```
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Aravis WinUSB (Example Ltd)" -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5)
Export-Certificate -Cert $cert -FilePath aravis-winusb.cer
```

Copy `aravis-u3v.inf` and `aravis-u3v.cdf` to an empty folder. In that folder,
from a Developer PowerShell for Visual Studio or with the Windows SDK tools in
`PATH`, create the catalog and sign it with the thumbprint of the certificate
(`$cert.Thumbprint`):

```
makecat aravis-u3v.cdf
signtool sign /fd SHA256 /sha1 <thumbprint> aravis-u3v.cat
```

The folder, with `aravis-u3v.inf` and `aravis-u3v.cat`, is the driver package.
On each machine, from an elevated PowerShell, trust the certificate, then
install the package:

```
Import-Certificate -FilePath aravis-winusb.cer -CertStoreLocation Cert:\LocalMachine\Root
Import-Certificate -FilePath aravis-winusb.cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher
pnputil /add-driver aravis-u3v.inf /install
```

`arv-winusb-setup.ps1 install -InfPath <folder>\aravis-u3v.inf` runs the same
`pnputil` command, after checking the catalog, its signature and the trust of
its certificate, and lists the cameras afterwards. With `-WhatIf`, it prints the
command instead of running it. Then `arv-tool-0.10` should list the camera.

### Route 2: Zadig

[Zadig](https://zadig.akeo.ie), built on [libwdi](https://github.com/pbatard/libwdi),
writes a driver package for one device, signs it with a certificate that it
creates and installs, and forces its installation. It is the simplest route for
an end user, and it replaces a vendor driver, which route 1 never does.

In Zadig, select List All Devices in the Options menu, then the camera's
interface 0, listed as the camera name followed by "(Interface 0)". Choose
WinUSB as the driver, then Replace Driver, or Install Driver when the camera
has none.

The `wdi-simple` sample of libwdi does the same from a command line, with the
vendor and product IDs of the camera:

```
wdi-simple --vid 0x1234 --pid 0x5678 --iid 0 --type 0
```

`--iid 0` targets the USB3 Vision function, and `--type 0` selects WinUSB.
Without `--iid`, `wdi-simple` binds the composite device instead.

### Route 3: Device Manager

This route needs no download. In Device Manager, right-click the USB3 Vision
function of the camera (under Other devices when it has no driver), select
Update driver, Browse my computer for drivers, then Let me pick from a list of
available drivers on my computer, Universal Serial Bus devices and WinUsb Device.

This binding registers no device interface GUID, which libusb needs. From an
elevated command prompt, add one to the `Device Parameters` registry key of the
node, where `<instance ID>` is its instance ID as `pnputil` prints it, then
unplug the camera and plug it back into the same port:

```
reg add "HKLM\SYSTEM\CurrentControlSet\Enum\<instance ID>\Device Parameters" /v DeviceInterfaceGUIDs /t REG_MULTI_SZ /d "{CF9A8954-37DB-49E9-8C34-346799A3FB0C}"
```

Any GUID works: this one is that of `aravis-u3v.inf`. The registry key belongs
to one device node, so repeat both steps for another camera, or for another
port. This route was not tested with a camera.

### Undoing a binding

Routes 1 and 2 add a driver package to the driver store. `pnputil /enum-drivers`
lists the packages: find the published name, `oemNN.inf`, of the one whose
original name is `aravis-u3v.inf` (route 1), or whose provider is libwdi (route
2), then remove it from the cameras that use it and from the store:

```
pnputil /enum-drivers
pnputil /delete-driver oemNN.inf /uninstall
```

The cameras go back to their next best driver, if any. `arv-winusb-setup.ps1
uninstall` does this for every copy of `aravis-u3v.inf`, and also takes
`-WhatIf`. The certificates stay trusted: remove them from the Trusted Root
Certification Authorities and Trusted Publishers stores of the local machine
(`certlm.msc`) once no package uses them. For route 3, delete the
`DeviceInterfaceGUIDs` value, then use Roll Back Driver, or Uninstall device, in
the Device Manager properties of the node.

### Limits

- Route 1 is for self-signed installation only, as Microsoft does not sign an
  INF that matches a compatible ID.
- `aravis-u3v.inf` never replaces a vendor driver: Windows ranks the
  compatible-ID match of the INF below the hardware-ID match of a vendor driver,
  and `pnputil /add-driver /install` does not force a lower-ranked driver onto a
  device. This lets Aravis and the vendor's software share a machine, each with
  its own cameras. To move a camera from its vendor driver to WinUSB, use route 2
  or 3.
- USB3 Vision cameras need libusb 1.0.27 or later on Windows.
- x64 only. Windows in S mode installs only driver packages signed by
  Microsoft, and Windows on ARM64 is reported to do the same (unverified).
- Unverified: the compatible ID of real cameras, and the installation of the
  self-signed package on Windows 11 24H2 and 25H2. InfVerif and MakeCat check
  the package, but it was never installed during its development.
- UsbDk is not recommended: its last release dates from 2020, and an open
  report says that it breaks Windows 11 24H2.

### libusb with hotplug support

libusb 1.0.30 has a hotplug backend for Windows, but it is a build option of
libusb, off by default: a `libusb-1.0.dll` built with the default options lacks
it. With hotplug support, libusb tells Aravis as soon as Windows reports that a
camera which Aravis has open was unplugged, and Aravis emits the `control-lost`
signal of the device at once. Without it, Aravis can only infer the removal
from failing transfers, which takes longer. Prefer a libusb with hotplug
support on Windows. These `libusb-1.0.dll` files have it:

- the one of the Windows zip packages of Aravis (see [Building on
  Windows](building.html#building-on-windows));
- in `libusb-1.0.30.7z`, the official binary archive of libusb 1.0.30 on its
  [release page](https://github.com/libusb/libusb/releases/tag/v1.0.30), the
  ones of the `VS2025\MS64-hotplug` (x64), `VS2025\MS32-hotplug` (x86) and
  `VS2025\ARM64-hotplug` (ARM64) folders, in their `dll` folder. The other
  folders of the archive, `MinGW64` and `VS2022\MS64` among them, hold builds
  without hotplug support. These DLLs need the Visual C++ runtime
  (`VCRUNTIME140.dll`), as the official GStreamer package for MSVC does;
- a libusb built from source with its Windows hotplug option:
  `-Dlibusb:windows-hotplug=true` with the libusb fallback of Aravis (see
  [libusb version](building.html#libusb-version)), `--enable-windows-hotplug`
  with the `configure` script of libusb, or a `Hotplug` configuration, such as
  `Release-Hotplug`, of its Visual Studio solution, `msvc\libusb.sln`.

Windows loads the first `libusb-1.0.dll` of its DLL search order, which starts
with the folder of the program, goes on with the system folders of Windows, and
ends with the folders of `PATH`: put the DLL with hotplug support next to the
program, or check that no other copy comes first. In a build tree of Aravis,
`meson test -C build libusb-version -v` prints the version of the libusb that
the programs of the build load, and whether it has hotplug support.

## Performance

Since version 0.8.28, Aravis uses the asynchronous libusb API by default:
`ARV_UV_USB_MODE_DEFAULT` is `ARV_UV_USB_MODE_ASYNC`. It gives better
performances than the synchronous API, especially on embedded platforms like
Raspberry Pi or Nvidia Jetson boards. [method@Aravis.Camera.uv_set_usb_mode]
selects the synchronous API instead.

`arv-viewer` and `arv-camera-test` use the synchronous API if their `usb-mode`
option is set to `sync`. Similarly, the GStreamer plugin uses the synchronous
API if its `usb-mode` property is set to `sync`.

## Transfer size and RAW_IO

### Maximum transfer size

A stream reads each image payload in USB bulk transfers of at most the maximum
transfer size, 1 MiB by default. In asynchronous mode, Aravis keeps reads of up
to 8 times the maximum transfer size in flight (8 MiB by default), so that the
camera never waits for a read. A smaller size means more transfers per frame; a
larger one more memory in flight, which on Linux counts against the usbfs memory
limit (`usbfs_memory_mb`, 16 MB by default).

The size can be set from 1 KiB (1024 bytes) to 64 MiB (67108864 bytes), and 0
selects the default. A size out of this range is refused with a warning, and
the previous size is kept. The setting applies to the streams created
afterwards:

- in code, [method@Aravis.Camera.uv_set_maximum_transfer_size];
- with `arv-camera-test`, the `--usb-maximum-transfer-size=<bytes>` option;
- with the GStreamer plugin, the `usb-maximum-transfer-size` property of
  `aravissrc` (0 keeps the library default).

At each acquisition start, the stream computes the transfer size it uses: the
largest size not above the maximum that is a multiple of both the payload
alignment the camera requires and the packet size of the stream endpoint
(1024 bytes at SuperSpeed, 512 bytes at high speed), capped at the RAW_IO
maximum when RAW_IO is on. The camera is told to send its payload in transfers
of at most that size. The RAW_IO cap makes the reads smaller, not the bytes in
flight: there are more of them.

### WinUSB RAW_IO

On Windows, the WinUSB driver queues the reads of an endpoint and passes them to
the USB stack one at a time. Its RAW_IO pipe policy passes them on directly
instead, which Microsoft documents as a significant gain for multiple
outstanding reads. libusb gives access to it from version 1.0.30.

RAW_IO is off by default. What it brings to Aravis has not been measured yet
(see [Throughput benchmark](#throughput-benchmark)), and the way Aravis handles
the errors of reads with RAW_IO, described below, was only checked against a
model of WinUSB, never with a camera. To turn it on, set the `ARV_UV_RAW_IO`
environment variable to `1`, or `on`, `true`, `yes` or `auto`, whatever their
case. Unset, empty, `0`, `off`, `false`, `no`, or any other value, leave it off.
Aravis reads the variable at each acquisition start, in any program, the
GStreamer plugin included. Comparing a run with `ARV_UV_RAW_IO=1` against one
without it shows what RAW_IO brings with a given camera and host controller.

With `ARV_UV_RAW_IO=1`, Aravis turns RAW_IO on for the stream endpoint at each
acquisition start, where it is available, in asynchronous mode only
(synchronous mode has a single read in flight), and turns it off again at the
stop, once every transfer is back. A stream that stops after another stream of
the same camera has started, as when an application keeps a stream while it
creates a new one, leaves RAW_IO, and the streaming of the camera, to that one.

RAW_IO is not available, and nothing changes, outside Windows, with another
driver than WinUSB, or with a libusb older than 1.0.30, at build time or at run
time. On Windows, an Aravis built with libusb 1.0.30 still runs with an older
`libusb-1.0.dll` (Windows loads the first one of its DLL search order, see
[libusb with hotplug support](#libusb-with-hotplug-support)): Aravis looks the
functions of libusb 1.0.30 up in the DLL that the program loaded, the first time
it needs them, and goes without RAW_IO when they are not there. With
`ARV_DEBUG=device` (info level), the log names that DLL, and says whether it has
them:

```
[UvDevice::libusb] C:\aravis\bin\libusb-1.0.dll: RAW_IO functions found, device string function found
```

A libusb linked statically, into the program or into Aravis, is not a DLL and
exports nothing: Aravis calls these functions directly then. The build tells
the two apart: Aravis is configured for a static libusb when the libusb of
`subprojects/libusb.wrap` is built as a static library (as with
`-Ddefault_library=static`), or when the installed libusb that meson finds is a
static library. If the build takes a static libusb for a DLL, the log says
`not exported (libusb linked statically, Aravis built for a libusb DLL)`
instead of `found`, and neither RAW_IO nor the device strings of the open
errors are available: configure Aravis with `-Dc_args=-DARV_UV_LIBUSB_STATIC`.

With RAW_IO on, WinUSB rejects any read that is not a whole number of packets,
or is larger than the RAW_IO maximum transfer size of the endpoint. Aravis reads
accordingly:

- the payload transfers are a whole number of packets, no larger than the
  RAW_IO maximum;
- the leader and trailer reads are rounded up to whole packets, while the sizes
  given to the camera do not change;
- the end of the payload, when it is not a whole number of packets (for
  example the last 704 bytes of a 720x540 Mono8 frame), is read into a small
  buffer, then copied into the image buffer: one copy of less than one transfer
  per frame.

With reads of whole packets, the payload transfers no longer end with a short
packet. If the reads get out of step with the transfers of the camera, after a
transfer lost in the middle of a frame for example, the stream drops the frames
in flight (they come back as failed buffers) and reads up to the trailer that
ends a frame, then goes on with the next one. The trailer is a short packet: it
ends the read that gets it, after the end of the payload when the frame is a
whole number of packets (640x480 or 1920x1080 in Mono8, for example). If no
frame end shows up within two frames' worth of data, the stream goes on anyway,
and resynchronizes again when needed: buffers keep coming back, failed.

With RAW_IO on, WinUSB bypasses its error handling: after a failed read, it no
longer resets the pipe by itself (its `AUTO_CLEAR_STALL` policy, which libusb
turns on, applies only without RAW_IO), and every later read would fail. So
when a read fails other than by a cancellation or a removal, the stream takes
its reads back, checks that the camera is still there, resets the pipe
(`libusb_clear_halt()`) and resynchronizes. While reads keep failing, the
stream retries at most ten times a second, and the buffers wait in the input
queue.

If RAW_IO cannot be turned on, the acquisition goes on without it. If WinUSB
rejects a read anyway, which points at a packet size or a RAW_IO maximum that
changed, the stream checks that the camera is still there, gives that buffer
back as aborted, and goes on without RAW_IO, with a warning, keeping the same
reads; RAW_IO is not used again for that camera until it is opened again. A read
that follows the RAW_IO rules but fails anyway, the camera being there, is
retried as without RAW_IO, as the failure may be transient; if it keeps failing
for a second, the acquisition goes on without RAW_IO, and the next one tries
again. A camera that ends its transfers of whole packets with a zero-length
packet, which reads of whole packets cannot follow, fails its frames until the
acquisition stops, with a warning, and the next acquisitions of that device go
without RAW_IO. A program that opens the camera again, as `aravissrc` does at
each start, meets the failure again: leave `ARV_UV_RAW_IO` unset for such a
camera.

### Checking the result

With `ARV_DEBUG=stream` (info level), each acquisition start logs the outcome.
For a 720x540 Mono8 camera on Windows, by default:

```
Transfer size 1048576 bytes (maximum 1048576), packet size 1024 bytes, RAW_IO off (default, ARV_UV_RAW_IO=1 turns it on)
Reads: leader 56, payload 388800 x 1 + 0, trailer 32 bytes
Reads in flight: up to 8388608 bytes
```

And with `ARV_UV_RAW_IO=1`:

```
Transfer size 1048576 bytes (maximum 1048576), packet size 1024 bytes, RAW_IO on (maximum 2097152 bytes)
Reads: leader 1024, payload 388096 x 1 + 704, trailer 1024 bytes
Reads in flight: up to 8388608 bytes
```

The first line gives the transfer size in use, the maximum it was computed
from, the packet size of the stream endpoint and the RAW_IO state: `on`, with
the RAW_IO maximum transfer size of the endpoint, or `off` with the reason:

- `default, ARV_UV_RAW_IO=1 turns it on`: RAW_IO is available, but
  `ARV_UV_RAW_IO` does not turn it on;
- `ARV_UV_RAW_IO`: the variable turns it off;
- `sync mode`;
- `not available`: outside Windows, another driver than WinUSB, a libusb older
  than 1.0.30, a static libusb that the build took for a DLL (see above), or
  RAW_IO turned off for the camera by a failure since it was opened;
- `unknown packet size`, `maximum transfer size too small` or `failed to turn
  it on`.

The second gives the sizes of the leader and trailer reads, and how the payload
is split: the size of its transfers, their count, and the size of the final
transfer, if any. The third, in asynchronous mode only, gives the bytes of
reads kept in flight at most.

## Thread priority

An application raises the priority of the stream thread from the stream
callback, at `ARV_STREAM_CALLBACK_TYPE_INIT`, as the `--realtime` and
`--high-priority` options of `arv-camera-test` do (see the Ethernet page for an
example). With a USB3 Vision camera, the work that completes the buffers in
async mode runs on another thread: the transfer callbacks run on the libusb
event thread of the device, `arv_libusb`, which the application cannot reach.

On Windows, the event thread takes the priority of the stream thread as it is
when the INIT callback returns, if it is higher than its own: it is never
lowered. Its own priority is the one of the thread that opened the device, which
Windows threads created by GLib inherit. It changes at its next turn, and keeps
that priority until the stream thread ends. Then it goes back to its own
priority within 100 ms. Each device has its own event thread. With
`ARV_DEBUG=device`, each change is logged. Nothing changes on other platforms.

On Windows, [func@Aravis.make_thread_realtime] moves the whole process to
`REALTIME_PRIORITY_CLASS`. That needs the "Increase scheduling priority"
privilege (`SeIncreaseBasePriorityPrivilege`), which administrators have.
Without it, Windows grants `HIGH_PRIORITY_CLASS`, and the function returns
`FALSE`. The thread still gets `THREAD_PRIORITY_TIME_CRITICAL`, the highest
priority outside the realtime class. `ARV_DEBUG=misc` logs what was granted.
[func@Aravis.make_thread_high_priority] gives the thread
`THREAD_PRIORITY_HIGHEST`.

On Windows, libusb also has a thread of its own for each device. It takes the
completed transfers from the system and wakes the event thread up, and it keeps
the normal priority.

## Device loss

When a camera goes away while streaming (cable pulled, power lost), its device
emits [signal@Aravis.Device::control-lost], the stream thread exits, and every
buffer comes back: through the output queue, failed or aborted, or still in the
input queue. Register accesses then fail at once with
`ARV_DEVICE_ERROR_NOT_CONNECTED`. Once the camera is plugged back, open it
again.

libusb reports the loss through hotplug where it is available: Linux, macOS,
and the Windows libusb builds with hotplug support. A default Windows libusb
build has none, and reports a removed device as an I/O error rather than as a
missing device. So Aravis also checks the device, with a read of its device
descriptor, when a transfer fails, when a submission keeps failing, and, without
hotplug (or when the device could not register for it), when no transfer came
back for a second while some are pending. Expect the signal within about a
second; a triggered camera waiting for its trigger without hotplug gets one such
check per second. Outside Windows, libusb reports a removed device as missing,
and the check counts only that as a loss: on Linux, a check that the kernel
refuses for lack of usbfs memory fails with an I/O error. A submission refused
for lack of memory is retried without any check.

A camera that goes away while it does not stream shows it at the next access to
its registers: a register access whose transfers fail otherwise than by a
timeout makes Aravis check the device the same way, at most every 100 ms. When
the check finds the camera gone, the device emits the signal, and the access
fails with `ARV_DEVICE_ERROR_NOT_CONNECTED`, as the next ones do. An acquisition
start fails with the error of the first stream register it cannot read.

The signal may be emitted from the stream thread, from libusb's event thread, or
from a thread accessing the camera registers. Its handler must not stop or
release the stream itself: set a flag, or hand the work over to another thread,
with `g_main_context_invoke()` for example.


## Throughput benchmark

**No measurements yet — needs a USB3 Vision camera on Windows.**

Nobody has published what Aravis achieves over WinUSB, so the USB settings of
Aravis on Windows, its transfer size, RAW_IO and thread priority, are not chosen
from measurements yet. In particular, RAW_IO stays off by default until the
results table below shows that it brings a gain. This section gives a protocol
that anyone can repeat, and the table where its results go.

### Protocol

**Tool.** `scripts/arv-usb-benchmark.py`, in the Aravis sources, needs Python 3
and nothing else. It runs `arv-camera-test-0.10` once for each configuration of
the matrix below, for one minute:

```
arv-camera-test-0.10 -n <camera> --usb-mode=async --usb-maximum-transfer-size=<size> --duration=60
```

It records the frame rate, the throughput and the error count that
`arv-camera-test` prints every second, the stream statistics that it prints at
the end (completed buffers, failures, underruns, aborted buffers, transferred
bytes), and the CPU time of `arv-camera-test`.

**Matrix.** Twelve runs per camera:

| Setting | Values |
| --- | --- |
| Transfer size | 64 KiB, 256 KiB, 1 MiB (`--usb-maximum-transfer-size`) |
| RAW_IO | off, on (`ARV_UV_RAW_IO=0`, `ARV_UV_RAW_IO=1`) |
| Priority | default, high (`--high-priority`, which raises the priority of the stream thread) |

RAW_IO is a pipe policy of WinUSB that lets several reads reach the USB stack
at once. It needs libusb 1.0.30, and an endpoint that supports it. The script
sets the `ARV_UV_RAW_IO` environment variable for every run, to `0` or `1`, so
that no run depends on the default of Aravis (RAW_IO off) or on the environment
of the script.

With `ARV_UV_RAW_IO=1`, Aravis turns RAW_IO on only where libusb and the
endpoint support it, and the stream goes on without RAW_IO when reads fail with
it, as [WinUSB RAW_IO](#winusb-raw_io) explains: a run with RAW_IO on may
measure RAW_IO off, throughout or in part. So the script also sets
`ARV_DEBUG=stream:2,stream-thread:1` for every run, unless `ARV_DEBUG` is set:
the output of the run then gives the RAW_IO state at the acquisition start, as
[Checking the result](#checking-the-result) shows, and a warning of the stream
thread when it goes on without RAW_IO. The script reads them, and records the
RAW_IO state of each run. When that state is not the setting of the run, the
RAW_IO column of `results.md` gives both, such as `on, but off (not available)`
or `on, then off`, and the run gets a warning. With an `ARV_DEBUG` of your own,
include `stream:2,stream-thread:1` in it.

**Controls.**

- The camera runs at its maximum frame rate and full resolution, in an 8-bit
  pixel format. `arv-camera-test` keeps the settings of the camera, except
  those that its options change: set the camera up beforehand, or give options
  to `arv-camera-test` after `--` on the command line of the script, such as
  `--frequency`, `--exposure` or `--features`.
- USB selective suspend is off: in the advanced settings of the power plan, USB
  settings, USB selective suspend setting, Disabled. The `powercfg` commands
  below do the same for the current power plan, on mains power.
- Note the host controller of the camera (Intel, AMD, Renesas, ASMedia...), and
  whether a hub sits between them: Device Manager, View, Devices by connection
  shows the path from the controller to the camera.
- Nothing else streams through the same controller during the runs.

```
powercfg /setacvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0
powercfg /setactive SCHEME_CURRENT
```

**References.** On the same machine, with the same camera, also measure:

- under Linux, with the same script. RAW_IO is a WinUSB setting, so outside
  Windows the script runs the RAW_IO off half of the matrix only;
- with the camera vendor's own viewer on Windows, which needs the driver of the
  vendor (see [Windows](#windows)): note the frame rate that it reaches.

**What to record.** The script records, for each run, the camera, the frame
rate (the mean and the minimum over the seconds after the first one), the
throughput, the CPU use, the errors, the stream statistics and the RAW_IO
state, and, from the output of the runs with RAW_IO on, the largest RAW_IO
transfer size of the data endpoint of the camera
(`libusb_get_max_raw_io_transfer_size ()`, which Microsoft does not document).
Give it, with `--controller` and `--notes`, what it cannot find by itself: the
host controller and the hub, and the version of libusb and whether it has
hotplug support.

**Through GStreamer.** Then repeat the transfer sizes and the RAW_IO settings
through `aravissrc`, whose `usb-maximum-transfer-size` property matches the
option of `arv-camera-test`. `ARV_UV_RAW_IO` applies to any program that uses
Aravis: set it to `0` or `1` for each run, as the script does, with
`ARV_DEBUG=stream:2,stream-thread:1` to see the RAW_IO state of the run in its
output. The script does not run GStreamer: run each configuration by hand, with
`num-buffers` set to the number of frames of one minute, and note the `average`
frame rate of the last `last-message` that `fpsdisplaysink` prints. `aravissrc`
has no priority setting.

```
gst-launch-1.0 -v aravissrc camera-name=<camera> usb-mode=async usb-maximum-transfer-size=<size> num-buffers=<frames> ! fpsdisplaysink video-sink=fakesink text-overlay=false sync=false
```

### Running the benchmark

`--dry-run` prints the commands of the matrix without running them. Each run
lasts the duration of `arv-camera-test` and a few seconds more, so the twelve
runs of one camera take about 13 minutes. Without `-n`, `arv-camera-test` opens
the first camera that it finds.

```
python arv-usb-benchmark.py --dry-run -n <camera>
python arv-usb-benchmark.py -n <camera> --controller "Intel xHCI, no hub" --notes "libusb 1.0.30, hotplug"
```

The script writes its results in a new directory, `usb-benchmark-<date>-<time>`
unless `-o` gives another one, after each run: `results.md`, whose table has
the columns of the table below, `results.csv` and `results.json`, which hold
the values of each second, all the stream statistics and the RAW_IO state of
each run, and the output of each run in `logs`. `--transfer-sizes`, `--raw-io`, `--priorities` and
`--duration` change the matrix, and `--help` lists the other options.

### Results

Frames/s is the mean frame rate over the seconds after the first one, with the
minimum in parentheses. MiB/s is the mean throughput over the same seconds, in
2^20 bytes per second: `arv-camera-test` labels its throughput MiB/s, but counts
it in 10^6 bytes, which the script converts. CPU % is the CPU time of
`arv-camera-test` while it streams, in percent of one logical processor.
Completed, Failures and Underruns are the stream statistics of the run, and
Errors the buffers that `arv-camera-test` received with an error status.

| Camera | OS | Host controller | Transfer size | RAW_IO | Priority | Frames/s | MiB/s | CPU % | Completed | Errors | Failures | Underruns |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| *No measurements yet — needs a USB3 Vision camera on Windows* | | | | | | | | | | | | |

Add the rows of `results.md` here, with a note on the libusb in use, and the
references in the same form. Keep their RAW_IO column as the script writes it:
a run with RAW_IO on that went without it, throughout or in part, does not
measure what RAW_IO brings. The default transfer size of Aravis on Windows is
to be chosen from this table, with at least one camera that delivers more than
300 MiB/s. RAW_IO stays off by default until this table shows that it brings a
gain.

## Cameras that are found but cannot be opened

A camera whose driver libusb cannot use is still found by the device discovery,
but libusb fails to open it. When no device is found, the error of
[func@Aravis.open_device], [ctor@Aravis.Camera.new] and
[func@Aravis.open_device_on_interface] with the `USB3Vision` interface, which
`aravissrc` uses with `interface=USB3Vision`, lists these USB3 Vision devices,
one per line, and `arv-tool` prints the same lines after "No device found". No
debug output is needed:

```
No supported device found
Failed to open USB3 Vision device 1209:0001 (Aravis Fake S/N UV1) on bus 2, address 7: LIBUSB_ERROR_NOT_FOUND (libusb finds no usable WinUSB driver on its USB3 Vision function: no driver, the vendor's driver, WinUSB without a DeviceInterfaceGUIDs registry value (as Device Manager binds it), or an unfinished driver installation; bind WinUSB to the function, see Windows in the USB documentation of Aravis)
```

Each line gives the vendor and product ids, the bus and address, the libusb
error and a hint at its usual cause. With libusb 1.0.30 and later, it also gives
the manufacturer, product and serial number strings of a device that answers:
libusb reads them without opening the device, but on Windows each one is a
request to the device, which fails while Windows keeps the device suspended.
These strings are read to build the error message only, not at each discovery.
On Windows, an Aravis built with libusb 1.0.30 that runs with an older
`libusb-1.0.dll`, or with a static libusb that the build took for a DLL, leaves
them out (see [WinUSB RAW_IO](#winusb-raw_io)).

One error code has several causes, so the hint is a guess:

| libusb error | Usual cause on Windows | What to do |
| --- | --- | --- |
| `LIBUSB_ERROR_NOT_FOUND` | libusb finds no usable WinUSB driver on the USB3 Vision function of the camera: no driver, the vendor's driver, WinUSB bound without a `DeviceInterfaceGUIDs` registry value (as Device Manager does), or a driver installation that is not finished | Bind WinUSB to the USB3 Vision function as [Windows](#windows) explains, or add the missing registry value, then replug the camera |
| `LIBUSB_ERROR_NOT_SUPPORTED` | A driver libusb does not support, such as a vendor driver, bound to the whole device | Bind WinUSB instead |
| `LIBUSB_ERROR_ACCESS` | The camera is already open, in this or another process, or the WinUSB DLL is missing | Close the other program, or retry |
| `LIBUSB_ERROR_NO_DEVICE` | The camera was unplugged | |
| Any other | | Set `LIBUSB_DEBUG=4` to see the log of libusb |

On Linux, `LIBUSB_ERROR_ACCESS` means that the user may not open the device
node: see the Permissions section above. With `ARV_DEBUG=interface:2`, the
discovery logs each failed open as a warning, without the device strings, and
counts them in its summary.

Opening a camera, by name or with the `arv_uv_device_new*()` functions, only
opens the USB devices that the discovery would list: the USB3 Vision device
class (EF/02/01) with a USB3 Vision control and data interface. No device of
another kind is opened or sent a request. A device that has the USB3 Vision
interfaces but another device class, which an exact name could open before, is
no longer found.

The discovery opens each USB3 Vision device for a moment, to read its strings,
and remembers where it found each camera: its bus, address and port numbers, for
the whole process, until [func@Aravis.shutdown]. Opening a camera that the
discovery listed, by name, or by GUID with `arv_uv_device_new_from_guid()`, then
opens the device at that place first, and no other USB3 Vision device when it is
the camera. Otherwise the open looks at the USB3 Vision devices in turn: it
opens those that libusb lists before the camera, to compare their strings, then
remembers where it found the camera. That happens without a discovery before (a
name that the discovery has not listed yet makes [func@Aravis.open_device] run
one first), when the camera was plugged in again, which gives it another
address, or when another device took its place. `arv_uv_device_new()`, by
vendor, model and serial number, always looks at the devices in turn.

On Windows, WinUSB lets one handle at a time open a device. A device that the
discovery, or an open looking at the devices in turn, has open for a moment
cannot be opened at that moment by another program, or with the
`arv_uv_device_new*()` functions: that open fails with `LIBUSB_ERROR_ACCESS`,
and a retry works.
