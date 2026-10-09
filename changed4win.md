# Aravis on Windows: changes on branch win-fixes

This branch implements the 21-item Windows fix plan for Aravis, three items of it in part. It also fixes the 13 rows of the plan's "Also found" table and the defects found during the work. Its base is upstream `main` at `de17cdde` (3 Oct 2026, version 0.9.3) plus the fork's commits up to `ef8ac13d`. It was built and tested against fakes on one Windows 11 machine, with MSVC and MinGW, and cross-built for Linux. Nothing ran against a real camera, real WinUSB or GigE traffic, on Linux or macOS, or in CI: GitHub Actions has never run on the fork.

The work is two commits on top of `ef8ac13d`: the implementation, `8d517b7a` (140 files, +48,719 and −1,435 lines), and the fixes of an external audit of that commit (see [the audit](#from-the-audit-of-9-october-2026)).

## Protocol-code rule

Aravis's README asks that contributions to `src/arvuv*.[ch]` and `src/arvgv*.[ch]` not be based on the specification documents published by A3. It also asks pull requests to say so. The changes to these files, and the USB and GigE test fakes, were written without the A3 documents. Their only sources were Aravis, libusb 1.0.30, GLib, GStreamer and public Microsoft documentation. The fakes are `tests/fakeusb/`, `tests/fakeuvtest.c`, the `tests/fakeuv*.c` programs, `tests/fakegvmtu.c` and `tests/fakegv.c`. The Linux packet-socket change also drew on the Linux kernel sources. No USB3 Vision, GenCP or GigE Vision specification document was used. The fake USB3 Vision camera builds its packets from Aravis's own protocol headers and parsing code. The GigE packet-size clamp (issue #1047) comes from IP networking: a datagram larger than the interface MTU is fragmented or dropped.

## At a glance

"Done" means implemented and tested against fakes on Windows. The checks that still need a camera or another platform are listed in [What still needs real hardware or other platforms](#what-still-needs-real-hardware-or-other-platforms).

| # | Item | Status | Main changes | Tests |
| --- | --- | --- | --- | --- |
| 1 | WinUSB RAW_IO | Done; off by default | Opt-in RAW_IO (`ARV_UV_RAW_IO`), whole-packet reads, bounce buffer, recovery; `src/arvuvstream.c`, `src/arvuvdevice.c` | `fakeuvrawio`, `fakeuvunplugrawio`, `fakeuvdevice`, `fakeuvstream`, `uvlibusb` |
| 2 | libusb version floor | Done; an old libusb builds the wrap | libusb 1.0.27 or later on Windows, 1.0.30 wrap fallback, hotplug only when available; `meson.build`, `subprojects/libusb.wrap`, `src/arvuvdevice.c` | `libusb-version`, `libusb-wrap`, `check-libusb-floor.py`, `fakeuvdevice` |
| 3 | Unplug not detected | Done | Connection check, failure flags, idle watchdog, every buffer back once; `src/arvuvdevice.c`, `src/arvuvstream.c` | `fakeuvunplug`, `fakeuvunplugsync`, `fakeuvunplugrawio`, `fakeuv`, `fakeuvgst` |
| 4 | Transfer sizing | Partly done: no Windows default yet | Setter range 1 KiB to 64 MiB, start log, `usb-maximum-transfer-size` property; `src/arvuvdevice.c`, `gst/gstaravis.c` | `fakeuvrawio`, `gstaravis` (usb), `fakeuvgst` |
| 5 | Supported way to bind WinUSB | Done; never installed | INF, catalog definition, PowerShell helper, docs; `data/windows/` | `winusb-inf`, `winusb-setup` |
| 6 | Discovery fails silently | Done | Unopenable cameras listed in not-found errors and by `arv-tool`; `src/arvuvinterface.c`, `src/arvsystem.c` | `fakeuvdiscovery`, `fakeuvsystem` |
| 7 | Open probes every USB device | Done; first open in a process still discovers | Descriptor filter before any open, remembered camera location; `src/arvuvdevice.c`, `src/arvuvinterface.c` | `fakeuvdiscovery` |
| 8 | Coexistence with vendor drivers | Done; mock producers only | GenTL opt-in, loading, lifetime and stream fixes; `src/arvgentl*.c`, `src/gentl/` | `gentl`, `gstaravis` (gentl) |
| 9 | Plugin under MSVC in CI | Done; never run on GitHub | `aravis-msvc-gstreamer.yml` on the official GStreamer MSVC kit | the workflow, `smoke-test-gstreamer.py` |
| 10 | Retire Conan 1 | Done; never run on GitHub | `aravis-msvc.yml` deleted, Conan 2 job builds libusb 1.0.30 | `libusb-version` in each Windows job |
| 11 | Windows binaries | Partly done: external steps open | Zips from the MSVC and MinGW jobs, release upload; `.github/scripts/package-windows.py` | `smoke-test-gstreamer.py` |
| 12 | USB3 Vision test coverage | Done; never run on Linux or macOS | Link-time fake libusb, 12 test programs; `tests/fakeusb/`, `tests/fakeuv*.c` | suite `fakeuv`, `uvlibusb` |
| 13 | aravissrc frames alias pool memory | Done; valgrind not run | Buffers own their frames, copy fallback, own-stream restarts; `gst/gstaravis.c` | `gstaravis` (ownership, gentl), `gstaravis-network`, `fakeuvgst` |
| 14 | Latency query unit | Done | Minimum latency of one frame period; `gst/gstaravis.c` | `gstaravis` (latency) |
| 15 | Device loss late and generic in aravissrc | Done | Unlocked sliced wait, `unlock`, named errors, `buffer-timeout`; `gst/gstaravis.c` | `gstaravis` (unlock), `fakeuvgst` |
| 16 | Device provider | Done; no hotplug | `aravisdeviceprovider` for `GstDeviceMonitor`; `gst/gstaravisdeviceprovider.c` | `gstaravis` (device-provider), `fakeuvgst` |
| 17 | Auto-detach warning | Done | Capability check before the call; `src/arvuvdevice.c` | `fakeuvdevice` |
| 18 | USB thread priority | Done; MMCSS not done | Event thread follows the stream thread; checked priority class; `src/arvuvdevice.c`, `src/arvrealtime.c` | `fakeuvpriority`, `fakeuvlibusb` |
| 19 | Debug glyphs | Done | Letters `W`, `I`, `D`, `T` where glyphs would be garbled; `src/arvdebug.c` | `debug` |
| 20 | Throughput benchmark | Partly done: no camera numbers | `scripts/arv-usb-benchmark.py`, protocol in `usb.md` | `usb-benchmark` |
| 21 | GigE issue #1047 | Done; reporter not asked | MTU clamp, leader+trailer-only statistic; `src/arvnetwork.c`, `src/arvgvdevice.c`, `src/arvgvstream.c` | `gvpacketsize`, `fakegvmtu`, `gvsp`, `gvnetwork`, `fakegv` |

## Changes users will notice

### Interfaces, discovery and opening

- **GenTL is opt-in.** The GenTL interface is now disabled for device discovery by default, like Fake. It is enabled by any of:
  - `arv_enable_interface ("GenTL")`, or `arv_select_interface ("GenTL")` to use it alone;
  - `ARV_GENTL_ENABLE=1` (or `on`, `true`, `yes`), read once, before the first change of the interface selection;
  - the new `--gentl` option of `arv-tool`, `arv-camera-test`, `arv-test` and `arv-viewer`.
- `arv_open_device_on_interface ("GenTL", ...)` and `aravissrc interface=GenTL` reach the GenTL interface while it is disabled.
- `ARV_GENTL_PRODUCERS` restricts the loaded producers to a list of file names, separated like the paths of `GENICAM_GENTL64_PATH`. The match ignores case and accepts `*` and `?`.
- **Not-found errors explain USB failures.** When no device is found, the errors of `arv_open_device ()`, `arv_camera_new ()` and `arv_open_device_on_interface ()` list the USB3 Vision devices that libusb could not open. Each line gives the IDs, the device strings (libusb 1.0.30), bus, address, libusb error and a driver hint. These errors can now span several lines.
- `arv-tool` prints the same lines after "No device found". The discovery summary in the `interface` log counts these devices.
- **Opening a USB3 Vision camera** opens only devices that carry the USB3 Vision descriptors, and tries first the place where discovery found the camera. A device with USB3 Vision interfaces but another device class can no longer be opened by exact name.
- **Device-list strings live until exit.** `arv_get_device_id ()` and the other device-list queries return interned strings, still valid after later list updates. Their memory is bounded by the number of distinct strings.
- Before, these queries already took the device-list lock, but returned the interface's own string after releasing it: a concurrent update could free it. `thread-safety.md` now says which calls may run in parallel: the device-list functions, `arv_open_device ()`, `arv_open_device_on_interface ()` and the interface selection functions.
- **Consistent reads of the device list.** `arv_enable_interface ()`, `arv_disable_interface ()` and `arv_select_interface ()` now take the device-list lock. The new `arv_get_device_list_generation ()` changes whenever the list does: read it before and after several queries to know they describe one state of the list.
- `arv_device_start_acquisition ()` now puts the first stream error in its own error message.

### USB3 Vision

- **libusb version on Windows.** Configuring needs libusb 1.0.27 or later, and warns below 1.0.30. Without a suitable libusb, meson builds libusb 1.0.30 from `subprojects/libusb.wrap`.
- With `--wrap-mode=nofallback`, `-Dusb=auto` disables USB and `-Dusb=enabled` fails, both with an explanation.
- **Older libusb DLL at run time.** Aravis looks the libusb 1.0.30 functions up at run time. So `aravis-0.10-0.dll` still loads when an older `libusb-1.0.dll` comes first in the DLL search order. RAW_IO and the device strings are then unavailable, and the `device` info log names the DLL.
- **RAW_IO is off by default.** `ARV_UV_RAW_IO=1` (or `on`, `true`, `yes`, `auto`) turns WinUSB's RAW_IO policy on where it is available, in async mode. `0`, `off`, `false` or `no` keep it off without asking libusb. Unset or empty means off; an unknown value logs a warning and means off. The variable is read at each acquisition start.
- **Transfer size validation.** `arv_camera_uv_set_maximum_transfer_size ()` and `arv_uv_device_set_maximum_transfer_size ()` accept 0 (the default, 1 MiB) or 1024 to 67108864 bytes. Other sizes give a warning, and the previous size is kept.
- Each start rounds the transfer size down to a multiple of both the camera alignment and the endpoint packet size. This applies on every platform, with RAW_IO on or off. Up to 8 times the maximum transfer size of reads stay in flight, as before.
- **Large memory writes.** Write blocks now fit the camera's maximum command packet. Before, with 1024-byte command packets, memory writes of 1005 bytes or more failed without a GError.
- **RAW_IO left on.** If turning RAW_IO off fails at a stop, the device remembers that it is still on, and the next acquisition without RAW_IO turns it off before reading, or fails to start with `ARV_DEVICE_ERROR_TRANSFER_ERROR`.
- **Unplug.** On Windows without hotplug, a removal now emits `control-lost`: during streaming, or at the first failed register access when idle. Later register accesses fail at once with `ARV_DEVICE_ERROR_NOT_CONNECTED`. Each buffer comes back exactly once, failed or aborted, or is still in the input queue. No thread spins.
- **Logs.** At the `device` info level, the libusb version and whether hotplug is available. At the `stream` info level, at each start: transfer size, packet size, RAW_IO state and read layout, plus the bytes in flight in async mode.
- The "Failed to set auto kernel detach feature" warning no longer appears on Windows and the BSDs.
- **Thread priority (Windows).** During acquisition, the `arv_libusb` event thread takes the priority of the stream thread when it is higher. `arv_make_thread_realtime ()` now returns FALSE, with a warning, when Windows grants only `HIGH_PRIORITY_CLASS`.
- In async mode, the `n_transferred_bytes` stream statistic counts the bytes received instead of the bytes requested.

### GigE Vision

- **Packet size and MTU.** When the system reports the MTU of the host interface, the automatic packet size does not exceed it, unless even the camera's smallest packet size is larger. A larger stored size is lowered to the largest valid packet size not above the MTU, even on cameras without `GevSCPSFireTestPacket`. If no valid packet size fits, the smallest one is used, and a `device` warning names the MTU and that size.
- A setup that relied on packets above the MTU, through a lenient network adapter, now gets MTU-sized packets. The `never` adjustment mode (`--packet-size-adjustment=never` of `arv-camera-test`) keeps the old size.
- **Diagnosis.** A new stream statistic, `n_leader_trailer_only_failures`, counts frames that failed with only their leader and trailer. A rate-limited warning names the packet size and the MTU.
- The last frame of an acquisition whose first data packet was lost now times out once its trailer has arrived, instead of staying pending.
- A device that answers through several host interfaces is reached through the interface whose subnet contains its address.
- An application that keeps an old stream across a new stream's creation no longer disables the new stream's channel when it releases the old one.
- On Linux, the packet socket ignores IP fragments and truncated captures, counts them in `n_ignored_packets`, and warns once.
- `arv-fake-gv-camera --gvsp-mtu` drops GVSP packets larger than an emulated path MTU.
- The documentation of `arv_camera_gv_auto_packet_size ()` now matches the code: without a test packet, the current size is kept, then capped at the MTU, or at the camera's smallest packet size if that is larger.

### GenTL

- On Windows, producers load with `LoadLibraryExW` and `LOAD_WITH_ALTERED_SEARCH_PATH`: libraries installed next to a `.cti` file are found. A producer that fails to load gives a warning naming the file and the error.
- Producers that lack functions Aravis never calls are accepted.
- A device whose producer gives no serial number gets the Aravis id `<TL vendor>-<vendor>-<model>-<producer>-<interface id>-<device id>`: GenTL device ids are only unique within an interface, and a vendor may ship several producers. Devices with a serial number keep `<TL vendor>-<vendor>-<model>-<serial>`.
- Single-part frames whose image starts at an offset (`BUFFER_INFO_IMAGEOFFSET`, after chunk data or a gap) are complete frames; the image part ends with the delivered data, and `aravissrc` sends the image alone.
- Stream setup failures return `ARV_DEVICE_ERROR_STREAM_ERROR`, naming the GenTL function and the producer's error text, instead of being ignored.
- Buffer status follows the producer: `ARV_BUFFER_STATUS_SIZE_MISMATCH`, `ARV_BUFFER_STATUS_MISSING_PACKETS`. Buffers flushed by a stop come back as ABORTED and are counted in the new `n_aborted` stream statistic.

### aravissrc

- New properties `usb-maximum-transfer-size`, `interface` and `buffer-timeout`, described in the API table below.
- `usb-mode` and `usb-maximum-transfer-size` now apply whatever their position relative to `camera-name` and `interface`.
- **Frame memory.** A pushed GstBuffer owns its frame. The ArvBuffer goes back to its stream when the last reference is dropped, on any thread. Frames held after a stop or a caps change stay valid.
- A frame is copied when lending it would leave the stream fewer than 2 buffers: with `num-arv-buffers` of 2 or less, every frame is copied. GenTL frames are always copied.
- **Latency.** Minimum one frame period (40 ms at 25 fps), maximum none. No answer in trigger mode or while the frame rate is unknown.
- **Stalls and device loss.** State changes and property reads no longer wait for the frame timeout. A lost camera gives `Lost control of camera "<name>"`. A timeout gives `No frame received from camera "<name>" within N ms`, with the stream statistics in the debug text. Both come before GstBaseSrc's generic error.
- A failed frame restarts the frame timeout, unless 10 failed frames came since the last complete one.
- **Caps changes.** The element releases its old stream before creating the next one, and stops and starts only its own stream.
- The `stream` and `camera` properties are now also notified, with NULL, when the element releases them: `stream` at each caps change and at stop, `camera` at stop.
- A property setter that cannot open the camera only logs a warning; the start then posts the error.
- Errors name the camera by its label once it is open: `camera-name`, or vendor-model-serial read from the camera. Before that, they say `camera-name`, "a camera on interface X" or "a camera". An unknown `interface` fails the start with a resource "settings" error.

### Device provider

- The plugin now registers `aravisdeviceprovider` at rank marginal. `GstDeviceMonitor` users, such as `gst-device-monitor-1.0 Video/Source`, list Aravis cameras.
- Each probe runs a full Aravis discovery: at least 1 s with GigE Vision, which broadcasts on every network interface. `GST_ARAVIS_DEVICE_PROVIDER=0` (or `off`, `false`, `no`) disables the probes without code changes.
- On Windows, a camera that this process is streaming is not listed meanwhile, as WinUSB lets one handle open a device.
- Each probe reads the device list from one state of it (`arv_get_device_list_generation ()`), so a listed device never mixes two cameras' properties.

### Debug output

- On Windows, the level glyphs become `W`, `I`, `D` and `T`, padded to the same width, wherever a console would garble them. That covers classic console windows, legacy code pages, and programs with a legacy C runtime locale such as `gst-launch-1.0`.
- Glyphs stay in a terminal emulator on code page 65001 (Windows Terminal, the Visual Studio Code terminal), for programs whose C runtime locale is "C" or UTF-8. Redirected output keeps them when there is no console, or when the console or `LANG` uses UTF-8. Other platforms are unchanged.
- Each message is written in one piece by Aravis's own C runtime, so lines of different threads no longer mix. A debug (`/MDd`) Aravis no longer hangs or asserts at exit inside a release GStreamer process.
- `aravis-0.10-0.dll` now imports `USER32.dll`, for `GetClassNameW`. GLib loads it anyway.

### New public API

Checked against `src/arvsystem.h`, `gst/gstaravis.c`, `gst/gstaravisdeviceprovider.c` and the tools.

| Kind | Name | What it is |
| --- | --- | --- |
| Function | `arv_open_device_on_interface (interface_id, device_id, error)` | Opens a device through one interface, enabled or not, without the discovery of the others. A NULL `device_id` opens the first device. Same not-found lines as `arv_open_device ()`. Since 0.10.0. |
| Function | `arv_get_device_list_generation ()` | A number that increases whenever the device list changes: other devices, identifiers, names or addresses, or another interface selection. Queries made between two equal readings describe one state of the list. Since 0.10.0. |
| Contract | `arv_enable_interface ()`, `arv_disable_interface ()`, `arv_select_interface ()` | Now take the device-list lock, so they may run while other threads use the list; they wait for an update in progress. |
| Function | `arv_get_device_interface_id (index)` | The interface that lists the device at `index`, as `arv_open_device_on_interface ()` takes it. Since 0.10.0. |
| Contract | `arv_get_device_id ()` and the other device-list queries | The returned strings stay valid until the process exits. |
| Element property | `aravissrc` `usb-maximum-transfer-size` (uint, 0 to 67108864, default 0) | USB bulk transfer size in bytes. 0 keeps the library default; 1 to 1023 are refused with a warning. |
| Element property | `aravissrc` `interface` (string, default NULL) | Opens the camera through this interface only (`USB3Vision`, `GigEVision`, `GenTL`, `Fake`), even one disabled for discovery. |
| Element property | `aravissrc` `buffer-timeout` (int, ms, default -1) | Frame timeout. -1 is the longest of 2 s and 3 frame periods; 0 waits forever, but stops after 10 failed frames in a row. |
| Plugin feature | `aravisdeviceprovider` | `GstDeviceProvider` at rank marginal, device class `Video/Source`. |
| Object property | `ArvGvFakeCamera` `gvsp-mtu` | Drops GVSP packets larger than this emulated path MTU; 0 for no limit. |
| Stream statistic | `n_leader_trailer_only_failures` (GigE Vision streams) | Frames that failed with their leader and trailer but no full-size data packet. |
| Stream statistic | `n_aborted` (GenTL streams) | Buffers flushed by a stop. |
| Tool option | `--gentl` of `arv-tool`, `arv-camera-test`, `arv-test` and `arv-viewer` | Enables the GenTL interface. |
| Tool option | `--gvsp-mtu` (`-m`) of `arv-fake-gv-camera` | Sets the `gvsp-mtu` property of the fake camera. |
| Environment | `ARV_UV_RAW_IO` | WinUSB RAW_IO on (`1`, `on`, `true`, `yes`, `auto`) or off (`0`, `off`, `false`, `no`, unset). |
| Environment | `ARV_GENTL_ENABLE` | Enables the GenTL interface for discovery (`1`, `on`, `true`, `yes`). |
| Environment | `ARV_GENTL_PRODUCERS` | Restricts the loaded GenTL producers to these file names. |
| Environment | `GST_ARAVIS_DEVICE_PROVIDER` | `0`, `off`, `false` or `no` disables the probes of the device provider. |
| Build | `subprojects/libusb.wrap` | libusb 1.0.30, the Windows fallback when no libusb 1.0.27 or later is installed. |
| Build | `-Dlibusb:windows-hotplug=true` | Builds the hotplug flavour of the wrap. Add meson's `--force-fallback-for=libusb-1.0` to use the wrap even when libusb is installed. |
| Build | `-Dc_args=-DARV_UV_LIBUSB_STATIC` | Windows: calls the libusb 1.0.30 functions directly, for a static libusb that the build did not detect. |
| Installed files | `share/aravis-0.10/winusb/` | `aravis-u3v.inf`, `aravis-u3v.cdf` and `arv-winusb-setup.ps1`, on Windows builds with USB. |
| CI script option | `--source-url` of `.github/scripts/package-windows.py` | The repository the zip README links to, at the built revision; the workflows pass their own (`GITHUB_SERVER_URL/GITHUB_REPOSITORY`), the default is upstream. |
| Script (not installed) | `scripts/arv-usb-benchmark.py` | The USB3 Vision throughput benchmark of item 20. |
| Test environment | `ARV_TEST_LIBUSB_MIN_VERSION`, `ARV_TEST_LIBUSB_HOTPLUG` | Expectations of the `libusb-version` test, set by the Windows CI jobs. |
| Test environment | `FAKEUV_TIME_SCALE`, `FAKE_USB_DEBUG`, `FAKEUV_DEBUG` | Time scale and logs of the fake libusb tests. |

No option was added to `meson_options.txt`.

Private symbols newly exported with `ARV_API`, for the tools and the tests only: `arv_dup_device_open_errors` (`src/arvsystemprivate.h`, used by `arv-tool`), `arv_debug_use_glyphs`, `arv_debug_is_terminal`, `arv_gv_device_search_packet_size`, `arv_network_interface_get_mtu`, `arv_network_get_interface_mtu_by_address`, `arv_network_set_loopback_mtu_override`, `arv_network_is_host_address`, `arv_network_address_is_on_subnet` and `arv_network_ethernet_frame_get_udp_payload`.

## Plan items

Each item starts with the plan's title. Function names are the stable reference; line numbers drift.

### 1. WinUSB RAW_IO is not enabled

**Problem.** Aravis never turned on WinUSB's RAW_IO pipe policy, so WinUSB passed its queued stream reads to the USB stack one at a time.

**What changed.**

- `src/arvuvstream.c`, `arv_uv_stream_start_acquisition ()`: RAW_IO is considered in async mode only, with libusb 1.0.30 headers (`LIBUSB_API_VERSION >= 0x0100010C`) and endpoint support. It stays off unless `ARV_UV_RAW_IO` turns it on (`_get_raw_io_setting ()`).
- The payload transfer size is the largest multiple of both the camera alignment and the packet size (`_lcm ()`) not larger than the maximum transfer size. With RAW_IO on, it is not larger than the RAW_IO maximum either. It is written to `SIRM_PAYLOAD_SIZE`.
- Leader and trailer reads are rounded up to whole packets on the host side. The sizes written to the camera do not change.
- A payload read that is not a whole number of packets goes through a bounce buffer. `arv_uv_stream_payload_cb ()` copies it into the frame before any GenDC parsing.
- RAW_IO goes on after the endpoint reset, before the stream thread starts, and off after the thread has joined.
- Only the stream that set up the stream interface last may undo RAW_IO and SIRM (`arv_uv_device_take_stream_interface ()`, `arv_uv_device_release_stream_interface ()`).
- Recovery with RAW_IO on:
  - reads out of step: a synchronous resync to the next trailer (`arv_uv_stream_thread_resync ()`), given up after two frames' worth of data;
  - a failed read: the stream checks the device and resets the pipe (`arv_uv_device_clear_stream_halt ()`, which calls `libusb_clear_halt ()`);
  - a confirmed rejection: one buffer aborted, then RAW_IO off for that device;
  - one second of IO errors: RAW_IO off for the rest of the acquisition;
  - a camera that ends whole-packet transfers with a zero-length packet is detected.
- `src/arvuvdevice.c`: with a libusb DLL on Windows, the four libusb 1.0.30 functions are looked up once with `GetProcAddress ()` (`ARV_UV_LIBUSB_RUN_TIME_LOOKUP`). With a static libusb, `src/meson.build` defines `ARV_UV_LIBUSB_STATIC` and they are called directly.
- The stream info log gives the transfer size, packet size and RAW_IO state at each start, for example `RAW_IO off (default, ARV_UV_RAW_IO=1 turns it on)`.
- `docs/reference/aravis/usb.md`: "WinUSB RAW_IO" and "Checking the result".

**Tests.**

- `fakeuvrawio` (37 cases). Frames that are not packet multiples (720×540, 1440×1080), read sizes, sync mode, fallback, `environment`, `older-libusb-dll` and the `transfer-size/*` rules. Also GenDC, resync and its limits, zero-length packets, rejected reads, IO failures, halted and failing pipes, unplug and pending transfers.
- `fakeuvunplugrawio`: the unplug cases with RAW_IO on.
- `/fakeuvdevice/raw-io-log` with the subprocess cases `raw-io-default`, `raw-io-on`, `raw-io-older-libusb` and `raw-io-static-libusb`: the start log and the lookup log.
- `/fakeuvstream/replaced-buffers-raw-io`, `wrapped-buffers-raw-io`, `resized-buffer-raw-io`.
- `uvlibusb` (suite `usb`, one case, `/uvlibusb/device-string`): Aravis reaches `libusb_get_device_string ()` in the build's real libusb, DLL or static. It calls it with a NULL device and expects `LIBUSB_ERROR_INVALID_PARAM`. This shows that the shared lookup works; the three RAW_IO functions are not called.
- Restoring RAW_IO on by default made `/fakeuvrawio/environment` and `/fakeuvdevice/raw-io-log` fail. `dumpbin /imports aravis-0.10-0.dll` lists none of the 1.0.30-only libusb functions.

**Deviations from the plan.**

- RAW_IO is off by default, and a new environment variable, `ARV_UV_RAW_IO`, turns it on. The plan had no switch: it turned RAW_IO on wherever available, and left the default to item 20's numbers, which do not exist yet.
- The recovery mechanisms above go beyond the plan.
- Also beyond the plan, the run-time lookup of the 1.0.30 functions. The first version imported them, which stopped `aravis-0.10-0.dll` from loading when an older `libusb-1.0.dll` came first.

**Not verified here.**

- Real WinUSB behaviour is modelled in the fake from Microsoft's pipe-policy page: the rejection rules, the bypass of AUTO_CLEAR_STALL, recovery by ResetPipe, and policy changes with transfers pending.
- The real RAW_IO maximum transfer size, and the gain itself (item 20).
- Whether real cameras end whole-packet transfers with zero-length packets. Such a camera fails every frame of its first RAW_IO acquisition per device object; `usb.md` documents it.
- The plan's Done-when: a 10-minute stream of a 1440×1080 Mono8 camera with no failed buffer, and byte-identical frames on Linux.
- `ARV_UV_LIBUSB_STATIC` detection for an installed static libusb (vcpkg, Conan) was only simulated, with hand-written `.pc` files pointing at the wrap's libraries.
- With a libusbK or libusb0 driver, libusb 1.0.30 applies RAW_IO too, through `libusbK.dll`. This was neither considered nor tested.
- Pre-existing limits, unchanged:
  - in sync mode, a final transfer padded to whole packets fails its frame;
  - on Linux in sync mode, a padded final transfer overflows;
  - in async mode, a buffer larger than the payload by one transfer loses the trailer.

**Audit follow-up.** A stop whose RAW_IO disable failed (for example `LIBUSB_ERROR_NO_MEM`) used to leave the policy on silently: the next acquisition without RAW_IO then had every read rejected, with no frame and no error. The device now tracks the actual policy (`arv_uv_device_get_raw_io ()`), and a start without RAW_IO turns a policy left on back off, or fails. Tests: `/fakeuvrawio/failed-disable`, `failed-disable-sync`, `failed-disable-lasting`, `failed-disable-lasting-sync`.

### 2. The libusb version floor is too low for Windows

**Problem.** `meson.build` accepted any libusb, although Windows needs 1.0.27 to claim a camera's data interface, and hotplug was registered unchecked.

**What changed.**

- `meson.build`, Windows only: `dependency ('libusb-1.0', version: '>=1.0.27', fallback: ['libusb', 'libusb_dep'], ...)`, a `not_found_message`, and a warning below 1.0.30.
- `subprojects/libusb.wrap`: libusb 1.0.30, with the meson build files of `subprojects/packagefiles/libusb`. `-Dlibusb:windows-hotplug=true` selects the hotplug flavour. See [The libusb wrap](#the-libusb-wrap).
- `src/arvuvdevice.c`, `arv_uv_device_constructed ()`:
  - logs `libusb_get_version ()` at the `device` info level;
  - registers the hotplug callback only when `libusb_has_capability (LIBUSB_CAP_HAS_HOTPLUG)` is true;
  - checks the result in `hotplug_cb_registered`, and warns on failure.
- `arv_uv_device_finalize ()` deregisters only a registered callback.
- Every Windows CI job uses libusb 1.0.30 and checks it at test time (items 9 to 11).
- Docs: `building.md` "libusb version", `usb.md` "libusb with hotplug support" (where to get a hotplug DLL, DLL search order).

**Tests.**

- `libusb-version` (suite `usb`): the run-time libusb version and hotplug support, against `ARV_TEST_LIBUSB_MIN_VERSION` and `ARV_TEST_LIBUSB_HOTPLUG`.
- `libusb-wrap` (suite `packaging`): the wrap needs no patch or git program, and its build files name only files of the libusb sources.
- `.github/scripts/check-libusb-floor.py`, run by the MSVC GStreamer job: five configure cases, from "1.0.26 with `-Dusb=enabled` fails" to a fresh hotplug fallback without `patch` or `git`.
- `/fakeuvdevice/hotplug`, `hotplug-log` and `subprocess/hotplug-failure`.

**Deviations from the plan.**

- "Configuring against 1.0.26 fails" holds only when meson cannot use the wrap: `--wrap-mode=nofallback`, or offline without the tarball in `subprojects/packagecache`. By default meson builds 1.0.30 from the wrap instead.
- The zips ship a hotplug `libusb-1.0.dll` built from the wrap, not the DLL of libusb's official archive.

**Not verified here.**

- The version and hotplug log lines appear only when a device opens: never seen with a real camera.
- Hotplug behaviour of a real hotplug DLL on a real removal.

### 3. Unplugging a camera while streaming is not detected

**Problem.** On Windows the async stream thread never learnt that the camera was gone: failed submissions were retried forever and the application just stopped receiving frames.

**What changed.**

- `src/arvuvdevice.c`, new private `arv_uv_device_check_connection ()`: a device descriptor read with a 100 ms timeout (`ARV_UV_DEVICE_CHECK_CONNECTION_TIMEOUT_MS`).
  - On Windows, any result but success, timeout, `LIBUSB_ERROR_BUSY` or `LIBUSB_ERROR_NO_MEM` means the device is lost.
  - Elsewhere only `LIBUSB_ERROR_NO_DEVICE` counts.
  - `_set_disconnected ()` emits `control-lost` once, whichever path notices first.
- `src/arvuvstream.c`:
  - transfer callbacks set `transfer_failed`;
  - `arv_uv_stream_thread_check_device ()` checks the device, at most every 100 ms, after failed transfers, failed submissions, IO retries and failed sync transfers;
  - an idle watchdog checks after one second with transfers pending and none back, when no hotplug callback is registered.
- `_submit_transfer ()` now returns a result, and IO and NO_MEM errors are retried. When a buffer's reads cannot all be submitted, `arv_uv_stream_buffer_context_submit ()` cancels the submitted ones and waits for them. It then returns the buffer once, as `ARV_BUFFER_STATUS_ABORTED`, counted in `n_aborted`.
- Buffers whose reads fail during the removal come back as `ARV_BUFFER_STATUS_MISSING_PACKETS`; buffers in flight at the stop come back aborted.
- The async loop waits 10 ms after failures, so it never spins. The sync thread exits when the device is gone.
- Register access to a lost device fails at once with `ARV_DEVICE_ERROR_NOT_CONNECTED`.
- `_send_cmd_and_receive_ack ()` checks the connection after a control-transfer failure other than a timeout or BUSY, never on the event thread. An idle unplug on Windows without hotplug therefore emits `control-lost` too.
- The SBRM and SIRM reads at acquisition start are checked: "Failed to read the stream interface registers: ...".
- `usb.md`: "Device loss".

**Tests.**

- `fakeuvunplug`, `fakeuvunplugsync` and `fakeuvunplugrawio` (async, sync and RAW_IO builds of `tests/fakeuvunplug.c`). Cases: `no-device`, `error`, `stall`, `cancelled`, `pending-error`, `hang`, `hang-streaming`, `submit-io`, `submit-no-device`, `hotplug-windows`, `hotplug-linux`, `hotplug-hang`, `hotplug-held`, `hotplug-unregistered`, the `transient-*` cases, `control-channel`, `start-after-unplug`, `no-memory`, `resync-loss`.
- They check `control-lost` timing, that every buffer comes back once, that nothing spins (libusb call counts), that the thread exits, and replug plus restart.
- Also `/fakeuv/unplug-hotplug-linux`, `/fakeuv/unplug-hotplug-windows`, `/fakeuvrawio/unplug`, `/fakeuvstream/start-read-error` and `/fakeuvgst/unplug`.
- Removing the check after a failed register access made `/fakeuvunplug/control-channel` fail.
- Outside Windows, the cases where a removal shows as `LIBUSB_ERROR_IO` are skipped, or expect a transfer error (`FAKEUV_IO_ERROR_IS_LOSS`): libusb reports `NO_DEVICE` there.

**Deviations from the plan.**

- Any transfer status other than completed or a requested cancel triggers a check, not only NO_DEVICE, ERROR and STALL.
- Failed sync transfers trigger a check on every platform, not only Windows; outside Windows only NO_DEVICE concludes a loss.
- NO_MEM retries never trigger a check. The idle watchdog and the 10 ms back-off are additions.

**Not verified here.**

- Which libusb status a real WinUSB removal gives, and whether control requests then fail at once.
- False positives. On Windows, a failed descriptor read other than a timeout, BUSY or NO_MEM marks the device lost for good: `control-lost`, then `ARV_DEVICE_ERROR_NOT_CONNECTED` on every access. So a single IO or PIPE error from a real camera or hub would end the session.
- The plan's Done-when: a real cable pull with a default and a hotplug libusb 1.0.30.
- The non-Windows branch (only NO_DEVICE counts) never ran on Linux or macOS.
- Follow-up: `arv_camera_stop_acquisition ()` skips the device stop when `AcquisitionStop` fails after a loss.

### 4. Transfer sizing is tuned for Linux and barely exposed

**Problem.** The 1 MiB × 8 defaults were chosen on Linux, the setter stored any value, `aravissrc` had no matching property, and `usb.md` called sync the default mode.

**What changed.**

- `arv_uv_device_set_maximum_transfer_size ()`: 0 selects the default (1 MiB). Sizes outside 1 KiB to 64 MiB give a `g_warning` and keep the previous size (`ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MIN` and `ARV_UV_STREAM_MAXIMUM_TRANSFER_SIZE_MAX` in `src/arvuvstreamprivate.h`).
- Each acquisition start computes one transfer size (see item 1). It logs that size with the read layout and, in async mode, the bytes in flight: up to 8 times the maximum.
- `aravissrc` gains `usb-maximum-transfer-size`: 0 to 67108864 (`GST_ARAVIS_MAX_USB_TRANSFER_SIZE`), 1 to 1023 refused with a warning. It applies at open and to an already open camera.
- `usb.md`: "Maximum transfer size", and "Performance" now says async is the default. `gst/pipelines.md`: "USB3Vision transfers".

**Tests.**

- `/fakeuvrawio/setter`, `read-sizes` and the `transfer-size/*` cases.
- `gstaravis` usb tcase: `test_usb_maximum_transfer_size_property`, `test_usb_maximum_transfer_size_invalid`, `test_usb_settings_non_usb_camera`.
- `/fakeuvgst/transfer-size`: before and after `camera-name`, 64 MiB and 0.
- `gst-inspect-1.0 aravissrc` shows `Range: 0 - 67108864`. Widening the property maximum again made the usb tcase fail.

**Deviations from the plan.**

- The accepted range of 1 KiB to 64 MiB is the implementer's choice.
- Step 3, a Windows default chosen from item 20's data, is deferred: the default stays 1 MiB.

**Not verified here.**

- Which size works best on Windows: that needs item 20's numbers.

### 5. There is no supported way to bind WinUSB to a camera

**Problem.** Aravis shipped no INF, installer or Windows driver documentation, so users had to discover Zadig on their own.

**What changed.**

- `data/windows/aravis-u3v.inf`: class `USBDevice`, `CatalogFile = aravis-u3v.cat`, `PnpLockdown = 1`, x64 only.
  - It matches the compatible ID `USB\Class_EF&SubClass_05&Prot_00`, so it covers every vendor's camera and never displaces a vendor driver.
  - It includes `winusb.inf` (`WINUSB.NT`, `.Services`, `.HW`) and sets its own `DeviceInterfaceGUIDs`.
- `data/windows/aravis-u3v.cdf`: MakeCat definition of the catalog, since Inf2Cat refuses compatible-ID USB INFs.
- `data/windows/arv-winusb-setup.ps1`:
  - `list` shows the USB3 Vision functions, their driver, and whether Aravis can open them;
  - `install` and `uninstall` run `pnputil`; `-WhatIf` prints the commands instead;
  - it repairs the missing XML root of pnputil 10.0.26200, and falls back to English text output;
  - libusbK and libusb0 cameras count as openable when `libusbK.dll` is in System32 (`Test-LibusbKDll`).
- The three files are installed to `share/aravis-0.10/winusb` (`data/windows/meson.build`) and required in the zips.
- `.gitattributes` and `.editorconfig` keep `*.inf` and `*.cdf` at CRLF.
- `usb.md` "Windows": where to bind, checking a camera, routes 1 to 3, undoing a binding, limits. `building.md` points to it.

**Tests.**

- `winusb-inf` (suite `packaging`): INF, CDF and helper consistency; InfVerif and MakeCat when installed.
- `winusb-setup` (suite `packaging`, Windows only): the helper in Windows PowerShell and PowerShell 7, on saved and malformed pnputil outputs, libusbK and libusb0 verdicts, `-WhatIf`, and live read-only checks.

**Deviations from the plan.**

- None for the routes. Route 2 is documented through Zadig and `wdi-simple`, not shipped as a tool, which the plan allows. The plan's tool name, `arv-winusb-setup`, went to the `pnputil` helper of route 1.
- Addition: libusbK and libusb0 are documented as alternatives, with WinUSB recommended.

**Not verified here.**

- The INF was never installed. Unverified: the compatible ID of real cameras, self-signed installs on Windows 11 24H2 and 25H2, route 3, non-English pnputil output.
- The helper's repair of pnputil's missing XML root was measured on pnputil 10.0.26200 only. The windows-2025 runner has 10.0.26100, which matters for the live checks of `winusb-setup` in CI.
- The plan's Done-when: a clean Windows 11 machine, a camera, the WINUSB service on its function node, and a clean removal.

### 6. Discovery fails silently

**Problem.** A camera present but not openable was dropped from the device list, and the application got "device not found" with no cause unless debug output was on.

**What changed.**

- `src/arvuvinterface.c`: discovery keeps an `ArvUvInterfaceOpenError` for each USB3 Vision device that `libusb_open ()` rejects.
- `arv_uv_interface_describe_open_error ()` builds one line per device: IDs, the manufacturer, product and serial strings (`libusb_get_device_string`, looked up at run time), bus, address, libusb error and a hint.
- `arv_uv_interface_get_open_error_hint ()` maps the error to its usual cause: no usable WinUSB driver, an unsupported driver, already open, or Linux permissions.
- The discovery summary line counts these devices.
- `src/arvsystem.c`: `arv_open_device ()` and `arv_open_device_on_interface ()` append the lines to their NOT_FOUND error (`_set_device_not_found_error ()`), also for a disabled interface. `arv_camera_new ()` inherits them.
- The USB3Vision interface itself still reports no error, so GigE Vision and GenTL devices stay reachable.
- `src/arvtool.c` prints the lines after "No device found", through the new private `arv_dup_device_open_errors ()` (`src/arvsystemprivate.h`).
- `usb.md`: "Cameras that are found but cannot be opened".

**Tests.**

- `fakeuvdiscovery`: `not-found-error`, `two-cameras`, `device-strings`, `string-requests`, `device-list-error`, `other-devices`, `known-device`, `arv-tool`, `hints`, `summary-log`, `older-libusb-dll`.
- `fakeuvsystem`: `not-found-error`, `disabled-interface`, `found-camera`. Dropping the lines from `arv_open_device_on_interface ()` made `/fakeuvsystem/not-found-error` fail.

**Deviations from the plan.**

- The device strings are read only when an error message is built, not at each discovery: on Windows each one is a request to the device.
- The hint for `LIBUSB_ERROR_NOT_FOUND` also names WinUSB bound without `DeviceInterfaceGUIDs` (as Device Manager binds it) and an unfinished driver installation.

**Not verified here.**

- A real camera without WinUSB; the error-to-hint mapping comes from the libusb 1.0.30 sources.
- The latency of `libusb_get_device_string` while a camera streams in another process.
- `usb.md` does not yet mention `arv_open_device_on_interface ()` in that section.

### 7. Opening a camera probes every USB device on the bus

**Problem.** `_open_usb_device ()` opened every USB device on the bus and read its strings until one matched, including unrelated WinUSB devices.

**What changed.**

- `arv_uv_interface_match_descriptors ()`, shared by discovery and open: device class EF/02/01, a control interface EF/05/00 and a data interface EF/05/02. `_try_usb_device ()` applies it before `libusb_open ()`.
- The open's not-found error lists the `libusb_open ()` errors of the USB3 Vision candidates, without device strings.
- Optional step 3, done: a process-wide table maps each camera GUID to its bus, address and port numbers. Discovery and successful opens fill it (`arv_uv_interface_remember_device_location ()`), and `arv_shutdown ()` clears it.
- `_open_usb_device ()` tries that location first (`arv_uv_interface_lookup_device_location ()`), and opens no other device when the camera is there. Otherwise it falls back to the scan.
- Accepted change: a device with USB3 Vision interfaces but another device class is no longer found by exact name, matching discovery.

**Tests.**

- `/fakeuvdiscovery/open-probes`: no open of a non-USB3 Vision device.
- `/fakeuvdiscovery/open-other-camera`: after a discovery, the other camera gets no open, control or string request, even while open.
- `/fakeuvdiscovery/open-without-discovery`, `open-replugged-camera`, `open-moved-camera`.

**Deviations from the plan.**

- The first open by name in a process, through `arv_open_device ()`, still runs a discovery first. Discovery opens every USB3 Vision camera to read its GUID, by design. So for the other USB3 Vision cameras, the Done-when holds only from the second open on. `usb.md` documents this.

**Not verified here.**

- `arv_uv_device_new ()` by vendor, model and serial always scans.
- The plan's Done-when with `LIBUSB_DEBUG=4` or a USB analyser.

### 8. No coexistence with vendor drivers

**Problem.** A camera works either with Aravis over WinUSB or with the vendor SDK, and the GenTL consumer that could bridge the two was not dependable.

**What changed.**

- **Opt-in.** GenTL is disabled for discovery by default (`arv_enable_interface ()`, `ARV_GENTL_ENABLE`, `--gentl`). `ARV_GENTL_PRODUCERS` selects producers by file name. `aravissrc` gains an `interface` property (`gst/gstaravis.c`), so a GenTL camera opens without the GigE Vision discovery.
- **Loading** (`src/arvgentlsystem.c`):
  - Windows producers load with `LoadLibraryExW` and `LOAD_WITH_ALTERED_SEARCH_PATH`, with no error dialog;
  - load failures, missing required functions and `GCInitLib` failures give a warning naming the file;
  - only the functions the consumer calls are required;
  - a producer is initialised once per process and shared, identified by its loaded module;
  - a producer whose `TLOpen` fails is skipped.
- **Discovery** tolerates missing or empty producer strings, with fallbacks; the per-device TL-vendor leak is fixed.
- **Lifetime and threads:** each device references its producer, so `arv_shutdown ()` with a live GenTL camera no longer crashes. Transport-layer and interface handles are reference-counted under a per-producer mutex.
- **Streaming** (`src/arvgentlstream.c`):
  - the thread waits for buffers in 50 ms steps (`ARV_GENTL_STREAM_EVENT_TIMEOUT_MS`) and checks for a stop between them;
  - the new-buffer event is registered, and buffers announced and queued, before `DSStartAcquisition`;
  - buffers pushed during acquisition are announced; a refused buffer comes back ABORTED without a busy loop;
  - open, announce, register and start failures return `ARV_DEVICE_ERROR_STREAM_ERROR` with the GenTL function, code and producer text;
  - statuses follow the producer; flushed buffers are ABORTED and counted in `n_aborted`;
  - the received size and every part are clamped to the buffer memory, with `ARV_BUFFER_STATUS_SIZE_MISMATCH`.
- **`-Devent=true`** (`src/arvgentldevice.c`): the event thread frees event ids and values, zero-terminates copies, keeps values as `GBytes` under a mutex with range checks, and has an atomic stop flag.
- **Aravis's own producer** (`src/gentl/`, `-Dgentl-producer=true`): see the "Also found" table below.
- `aravissrc` copies GenTL frames instead of lending them (item 13).
- `docs/reference/aravis/gentl.md` is rewritten: enabling, producer selection, device ids, loading, streaming, limits, tests.

The failures that the plan reproduced against a mock producer, and their tests:

| Plan's trigger | Was | Test |
| --- | --- | --- |
| Producer fails the serial-number query | Crash | `/gentl/discovery/missing-serial-number` |
| Stop while waiting in `EventGetData` | Hang | `/gentl/stream/stop-blocked` |
| Buffer pushed after acquisition start | Busy loop, stop never returns | `/gentl/stream/late-buffer`, `refused-buffers` |
| Stream open, announce or start fails | Error ignored | `/gentl/stream/open-failure`, `announce-failure`, `start-failure` |
| Incomplete frame | Reported as success | `/gentl/stream/incomplete-frame` |
| `arv_shutdown ()` with a live camera | Segfault | `/gentl/shutdown/live-camera` |

**Tests.**

- `gentl` (suite `gentl`, 52 cases on Windows, 51 elsewhere) against the mock producers of `tests/gentlmock`: `ArvGenTLMock`, `ArvGenTLMockMinimal`, `ArvGenTLMockBroken`, and `ArvGenTLMockDep` on Windows.
- Groups: `/gentl/opt-in/*`, `/gentl/load/*` (including `dependency`), `/gentl/discovery/*`, `/gentl/open/*`, `/gentl/acquisition/basic`, `/gentl/shutdown/*`, `/gentl/stream/*`, `/gentl/device/*` (`-Devent=true`), `/gentl/producer/*` (`-Dgentl-producer=true`), `/gentl/utilities/*`.
- `gstaravis` gentl and interface tcases. Unclamping the received size made `/gentl/stream/oversized-frame` fail.

**Deviations from the plan.**

- A refused buffer waits 50 ms only while the producer holds no buffer.
- GenTL frames in `aravissrc` cost one copy each: lent frames kept the data stream open.
- The opt-in is global, which the plan allows. The producer filter, `ARV_GENTL_PRODUCERS`, is an addition.

**Not verified here.**

- The plan's Done-when needs a vendor SDK and producer: an hour of streaming, 100 start/stop cycles, an unplug reported as an error, the vendor's viewer still working.
- 32-bit (stdcall) producers are not handled; `n_underruns` is not counted for GenTL streams.
- The cost of the per-producer mutex with real producers; `-Devent=true` was tested with the mock only.
- The MSVC GStreamer CI job does not set `-Dgentl-producer=true`, so `/gentl/producer/*` skip there; no CI job builds `-Devent=true`.

**Audit follow-up.**

- The single-part fallback of `src/arvgentlstream.c` gave the image part the whole `BUFFER_INFO_DATA_SIZE` at offset `BUFFER_INFO_IMAGEOFFSET`, so a frame with a nonzero image offset that filled its buffer became `SIZE_MISMATCH`. The part now ends with the data: `DATA_SIZE - IMAGEOFFSET`. `aravissrc` sends that image part rather than the whole buffer, so the bytes before the image do not shift the frame. Tests: `/gentl/stream/image-offset` (mock option `ARV_GENTL_MOCK_IMAGE_OFFSET`, fallback and multi-part paths) and `test_gentl_image_offset`, which checks the content of the frames `aravissrc` sends.
- The missing-serial fallback of `src/arvgentlinterface.c` used the GenTL device id alone, so devices of two interfaces, or of two producers of one vendor, could share an Aravis id: one camera could not be opened, and its entry opened the other. The fallback now adds the producer and the interface (`gentl.md`). Each component is percent-encoded before joining with hyphens, so embedded hyphens, percent signs and control bytes cannot merge different tuples. ASCII letters, digits, `_`, `.` and `~` remain unchanged; other bytes use uppercase `%HH`. Simple fallback ids and ids with supplied serial numbers keep their spelling. Tests: `/gentl/discovery/missing-serial-number-interfaces`, `missing-serial-number-producers`, `interfaces-same-device-id` (mock option `ARV_GENTL_MOCK_N_INTERFACES`); the re-audit follow-up below covers encoded components.

### 9. The GStreamer plugin is never built with MSVC upstream

**Problem.** Both MSVC CI jobs disabled the plugin, so nothing checked that `aravissrc` builds and loads against the official GStreamer for Windows.

**What changed.**

- New workflow `.github/workflows/aravis-msvc-gstreamer.yml` (Aravis-MSVC-GStreamer, `windows-2025`):
  - installs the GStreamer 1.28.7 MSVC x86_64 development package, pinned by SHA-256 and cached;
  - writes a native file with `.github/scripts/gstreamer-native-file.py`;
  - configures with `--vsenv`, `-Dgst-plugin=enabled`, `-Dusb=enabled` and the hotplug libusb wrap;
  - packages and smoke-tests a zip, runs `meson test --no-suite libusb`, then `check-libusb-floor.py`.
- `building.md`: "MSVC with the official GStreamer package".
- Details in [The MSVC job and the GStreamer kit](#the-msvc-job-and-the-gstreamer-kit).

**Tests.**

- The workflow itself. Its package and smoke-test steps were replayed here before the last small fix round: `package-windows.py` wrote a 111-file zip, and `smoke-test-gstreamer.py` passed `gst-inspect-1.0 aravissrc` and 50 buffers from `arv-fake-gv-camera` to EOS.

**Deviations from the plan.**

- The kit is installed in `C:\gstreamer\1.0\msvc_x86_64`, not in the plan's `/DIR=C:\gstreamer`, with more installer flags. The pinned 1.28.7 meets the plan's "1.28.5 or later".
- A generated native file runs the kit's `glib-mkenums` and `glib-genmarshal`.
- The libusb wrap carries its own build files in `subprojects/packagefiles/libusb`, instead of WrapDB's patch (see [The libusb wrap](#the-libusb-wrap)).

**Not verified here.**

- The plan's Done-when: a GitHub Actions run. The download, checksum and installer steps never ran; the local replay used GStreamer 1.28.3.

### 10. CI still depends on Conan 1

**Problem.** The original MSVC job installed `conan<2.0.0`, frozen since November 2024, with libusb 1.0.26.

**What changed.**

- `.github/workflows/aravis-msvc.yml` is deleted.
- `aravis-msvc-conan2.yml` no longer takes libusb from ConanCenter (1.0.29). With no `libusb-1.0.pc` from Conan, meson builds 1.0.30 from the wrap.
- Every Windows job runs `libusb-version` with `ARV_TEST_LIBUSB_MIN_VERSION=1.0.30`, and with `ARV_TEST_LIBUSB_HOTPLUG=1` where the hotplug DLL ships.
- The README badge points to Aravis-MSVC-GStreamer; `building.md` "MSVC with Conan" is updated.

**Tests.** No workflow installs Conan 1: only `conan>=2.0.0,<3.0.0` remains.

**Deviations from the plan.** The Conan 2 job is kept for Conan consumers.

**Not verified here.** The Conan 2 job never ran. The MinGW `mingw64` entry relies on MSYS2's libusb staying at 1.0.30 or later.

### 11. No current Windows binaries

**Problem.** Upstream releases carried source tarballs only, and every downstream Windows package was behind or built without the plugin.

**What changed.**

- `.github/scripts/package-windows.py` turns a `meson install --destdir` tree into `aravis-<version>[-g<commit>]-<flavour>.zip` plus a `.sha256` file.
  - Required: the Aravis DLL, import library and headers, `arv-tool`, `arv-camera-test`, the plugin, a hotplug `libusb-1.0.dll` and the WinUSB INF, CDF and helper.
  - Refused: any GLib or GStreamer DLL, since the plugin must use the GLib of the user's GStreamer.
  - Added: the licence texts and a README listing the three usable drivers.
- `.github/scripts/smoke-test-gstreamer.py` runs `gst-inspect-1.0 aravissrc` and a 50-buffer pipeline from the tree's `arv-fake-gv-camera`.
- The MSVC GStreamer job and the MinGW `ucrt64` entry package, smoke-test and upload the zips (`aravis-msvc-x86_64`, `aravis-msys2-ucrt64`).
- `release-assets` jobs (`contents: write`) attach them to published releases. `RELEASING.md` mentions them.
- Zips are built with the default `gv-n-buffers`; the tests then run with `-Dgv-n-buffers=1`, as in every Windows job.

**Tests.** Re-run here before the last small fix round: a 111-file `aravis-0.9.3-g<commit>-msvc-x86_64.zip` with `libusb-1.0.dll` 1.0.30 and the WinUSB files; the smoke test passed (50 buffers to EOS in 3.4 s).

**Deviations from the plan.** The plan's Done-when is a zip that runs on a machine with only the official GStreamer runtime. A smoke test of the unzipped package approximates it.

**Not done.** Steps 2 to 4 are external: the MSYS2 package bump, a `gst-plugin` feature for the vcpkg port, a conda-forge feedstock.

**Not verified here.**

- The `release-assets` jobs never ran, and no zip was tried on a clean machine.
- The `msys2-ucrt64` zip was never produced: `package-windows.py` and the smoke test ran on an MSVC tree only.

**Audit follow-up.** The zip README linked to `github.com/AravisProject/aravis` with the revision of whatever repository built it. `package-windows.py` takes `--source-url`, which both workflows set to their own repository. Test: `package-windows` (suite `packaging`), which builds zips for a fork, an Enterprise Server and upstream.

### 12. USB3 Vision code has no automated test

**Problem.** The fake camera speaks GigE Vision only, so no CI job on any platform ran `arvuvstream.c` or `arvuvdevice.c`.

**What changed.**

- `tests/fakeusb/` (`fakeusb.c`, `fakeusbcamera.c`, `fakeusb.h`, `fakeusbprivate.h`): a link-time fake of libusb-1.0 with libusb 1.0.30 semantics.
  - It implements every libusb entry point Aravis uses, with libusb's per-context event lock, synchronous calls that handle events, close with transfers in flight, and hotplug.
  - USB packet rules: short packets, zero-length packets, overflow; on Windows, ALLOW_PARTIAL_READS, AUTO_CLEAR_STALL, auto-claim and the RAW_IO length rules.
  - Windows, Linux and macOS presets (`fake_usb_set_platform ()`).
  - Scripted failures: open, claim and transfer errors, unplug and replug, hotplug timing, transfer failures without unplug.
  - Call counters and statistics; a test fails on unexpected libusb API misuse.
  - It exports the 1.0.30 functions only when Aravis looks them up, and has its own error strings.
- Its USB3 Vision camera runs on `ArvFakeCamera`'s register map and GenICam XML: frames split as the SIRM registers ask, 64-bit frame ids, GenDC, pending and dropped acknowledges.
- `tests/fakeuvtest.c` and `tests/fakeuvtest.h`: shared helpers. The test programs link the library's own objects, compiled once.
- Twelve programs in suite `fakeuv`: `fakeuv`, `fakeuvlibusb`, `fakeuvdevice`, `fakeuvdiscovery`, `fakeuvsystem`, `fakeuvstream`, `fakeuvunplug`, `fakeuvunplugsync`, `fakeuvunplugrawio`, `fakeuvrawio`, `fakeuvpriority`, `fakeuvgst`. Plus `uvlibusb` in suite `usb`.
- The valgrind setup runs them with `FAKEUV_TIME_SCALE=10`; `fakeuvgst` is excluded from it.
- The fake builds against libusb headers 1.0.21 to 1.0.30.

**Tests.** The suite itself. On MSVC all twelve programs pass, also at `FAKEUV_TIME_SCALE=10`. They also all pass in a non-Windows emulation: `G_OS_WIN32` compiled out of the USB code and the fake, with its Linux or its macOS preset. See the [verification matrix](#verification-matrix).

**Platforms.** Windows builds take a failed request other than a timeout for a removal; other builds take only `LIBUSB_ERROR_NO_DEVICE`. `FAKEUV_IO_ERROR_IS_LOSS` (`tests/fakeuvtest.h`) gives each test the expectation of the platform it was built for. The Windows-preset unplug cases without hotplug expect a lost camera on Windows, and a frame timeout or failed frames elsewhere.

**Failure injection.** `fake_usb_device_inject_transfer_status ()` first fails the transfers already pending on the endpoint, in submission order and with the bytes they received, as a failing pipe does (`/fakeuvlibusb/injected-status-order`). Before, they completed later, out of order, and `/fakeuvrawio/failing-pipe-without-raw-io` failed in 1 to 3% of runs under heavy load.

**Time budgets.** `FAKEUV_TIMEOUT_MS` (30 s, scaled by `FAKEUV_TIME_SCALE`) is the longest a test waits for frames, buffers, a loss of control or a state of the fake; only a failing test waits that long. The fakeuv programs have a 180 s meson timeout. The loss-of-control checks still bound the latency: 0.7 s for a loss reported at once, 1.5 s for one found by the one-second check.

**Deviations from the plan.** `fakeuvgst` is excluded from valgrind, as GStreamer's own allocations are not suppressed.

**Not verified here.**

- The plan's Done-when on the Linux, macOS and MinGW CI jobs. The suite never ran on a real Linux or macOS host; macOS was never built.
- Valgrind never ran. The fake models WinUSB from documentation and the libusb sources, not from the real driver stack.

### 13. Frames alias pool memory

**Problem.** `aravissrc` handed downstream a pointer into an ArvBuffer and gave the ArvBuffer back to the stream at once, so held frames were overwritten or freed.

**What changed.**

- `gst/gstaravis.c`: `gst_buffer_new_wrapped_full ()` now gets a `GstAravisBufferContext` and a destroy notify. The context holds the ArvBuffer, a `GWeakRef` to its stream and the reference-counted `GstAravisStreamState`.
- The notify runs on whichever thread drops the last reference. It pushes the buffer back to its own stream if alive, never to a later one, and frees it otherwise.
- A frame is copied when lending would leave the stream fewer than `GST_ARAVIS_MIN_STREAM_BUFFERS` (2) buffers. The element counts its lent buffers itself (`n_lent`).
- GenTL frames are always copied (`can_lend` is false for GenTL streams). Lent and copied sizes are clamped to the allocated buffer size.
- `gst_aravis_set_caps ()` releases the old stream before creating the next one. The element starts and stops only its own stream, with `AcquisitionStart`/`AcquisitionStop` and `arv_stream_start_acquisition ()`/`arv_stream_stop_acquisition ()`.
- `stream` is notified with NULL when the element releases it; `camera` too, at stop.
- `gst/pipelines.md`: "Frame memory", "Caps changes", "Stream and camera objects".

**Tests.**

- `gstaravis` ownership tcase: `test_ownership_hold_after_stop`, `test_ownership_caps_change`, `test_ownership_hold_many_frames` (the plan's three scenarios), `test_ownership_queue_threshold`, `test_ownership_start_stop_cycles` (100 cycles, live ArvBuffer count), `test_ownership_lending_limit`, `test_ownership_renegotiation_stream`, `test_ownership_renegotiation_held_stream`.
- `gstaravis` gentl tcase; `/fakeuvgst/frames-held-after-stop`, `frames-held-across-renegotiation`, `renegotiation-held-stream`; `gstaravis-network` (`test_network_gv_renegotiation`, `test_network_gv_renegotiation_followed_stream`, and `test_network_gv_renegotiation_held_stream`: the first GigE stream is kept across a caps change, then released; the stream-channel port register 0xd00 must still hold the new stream's port).
- `/fakeuvgst/renegotiation-held-stream` counts the acquisition starts of each USB3 Vision stream: an element that restarts a held stream fails it.
- All of them ran clean under AddressSanitizer (MSVC).
- Two 10-minute soaks with the Fake camera: through `queue ! queue min-threshold-buffers=10`, 60001 frames, 0 failures, 0 underruns. With `num-arv-buffers=4` and `fakesink sync=true`, 60004 frames, 0 failures, 1 underrun (also 1 before the review fixes), all 4 buffers back.

**Deviations from the plan.**

- Releasing the old stream first, starting only the element's own stream, the NULL notifications, and copying GenTL frames all go beyond the plan. They fix the defects found during the work (see the tables below).
- AddressSanitizer on Windows and live ArvBuffer counting stand in for valgrind.

**Not verified here.**

- The plan's Done-when "Valgrind shows no leak across 100 start/stop cycles": no Linux run, and the valgrind setup excludes the `gst` and `fakeuvgst` suites. Its comment records what stands in for valgrind there: the live ArvBuffer counts of the ownership cases, and local AddressSanitizer runs (no CI job builds with a sanitizer).
- The last frame release may finalize the stream on a downstream thread; this is documented.
- `aravissrc` ran with GStreamer 1.28 only (see item 15).

### 14. The latency query reports the frame rate as nanoseconds

**Problem.** `gst_aravis_query ()` stored the frame rate in the nanosecond minimum-latency field, so 25 fps became 25 ns.

**What changed.**

- The minimum is `GST_SECOND / frame_rate`; the maximum stays `GST_CLOCK_TIME_NONE`.
- No answer without a stream, without a frame rate, or in trigger mode. While `set_caps` replaces the stream, the query answers with the previous frame period.
- `gst/pipelines.md`: "Latency".

**Tests.** `test_latency_frame_period` (25/1 gives 40 ms, 50/1 gives 20 ms) and `test_latency_trigger_mode`. The plan's Done-when (40 ms at 25 fps) is met with the fake camera.

**Deviations from the plan.** None.

**Not verified here.** Nothing hardware-specific.

### 15. Device loss surfaces late and generically

**Problem.** `create()` blocked for the whole timeout while holding the object lock, then failed with GStreamer's generic "Internal data stream error", and never heard of `control-lost`.

**What changed.**

- `gst_aravis_create ()` waits without the object lock, in 50 ms slices (`GST_ARAVIS_WAIT_SLICE_US`).
- `gst_aravis_unlock ()` and `gst_aravis_unlock_stop ()` interrupt the wait. A live source paused on GStreamer 1.24 or later waits with `gst_base_src_wait_playing ()` instead of returning FLUSHING.
- The `control-lost` handler sets a flag in a reference-counted `GstAravisDeviceState`, shared with the element. That avoids a use-after-free when a signal arrives during finalize.
- Errors (`GST_ELEMENT_ERROR`, RESOURCE READ):
  - `Lost control of camera "<name>"`;
  - `No frame received from camera "<name>" within N ms`, with the stream statistics in the debug text;
  - `No complete frame received from camera "<name>" within N ms: K failed frames ...`;
  - `Camera "<name>" sent K failed frames in a row ...`.
- New `buffer-timeout` property. A failed frame restarts the wait, unless 10 failed frames came since the last complete one (`GST_ARAVIS_MAX_FAILED_BUFFERS`).
- With `buffer-timeout=0`, after 10 failed frames in a row, the element waits up to 250 ms for `control-lost` (`GST_ARAVIS_CONTROL_LOST_DELAY_US`). An unplug is then reported as a lost camera.
- The stall and loss errors name the camera by its label: `camera-name`, or vendor-model-serial read from the camera (`gst_aravis_build_camera_label ()`).
- Start, caps and not-found errors use that label once the camera is open (`gst_aravis_describe_camera_locked ()`). Before that, they say `camera-name`, "a camera on interface X" or "a camera".
- `gst/pipelines.md`: "Lost camera and missing frames".

**Tests.**

- `gstaravis` unlock tcase (11 cases): `test_unlock_buffer_timeout_property`, `test_unlock_stop_while_waiting`, `test_unlock_pause_resume`, `test_unlock_access_while_waiting`, `test_unlock_control_lost`, `test_unlock_timeout`, `test_unlock_wait_forever`, `test_unlock_failed_frames`, `test_unlock_failed_frames_control_lost`, `test_unlock_failed_frame_restarts_wait`, `test_unlock_control_lost_dispose`.
- `test_interface_error_camera_names`, `test_gentl_error_camera_names`.
- `/fakeuvgst/unplug` (10 cases): a USB unplug through `aravissrc`, with and without hotplug, sync and async, hanging transfers, RAW_IO on, off and default, and `buffer-timeout=0`.
- With the fakes, `set_state (NULL)` returns within 200 ms in each situation of the plan's table:
  - waiting for a frame, with the default or a 6 s timeout;
  - a property read while waiting;
  - every frame failing;
  - a timeout;
  - `control-lost` while idle.

**Deviations from the plan.** The pause handling for GStreamer 1.24, the failed-frame rule and the 250 ms grace are additions. The 50 ms slices meet the plan's "at most 100 ms".

**Not verified here.**

- A real unplug through `aravissrc`.
- On GigE Vision, `control-lost` after the heartbeat retries was not re-run after the review fixes.
- GStreamer 1.28 only: the 1.28.3 MSVC kit, and MSYS2's 1.28 in one native MinGW run of `fakeuvgst`. The pause handling depends on GStreamer 1.24 behaviour, and the Ubuntu runner of Aravis-Linux has 1.24.
- Outside Windows and without hotplug, a removal that libusb reports as `LIBUSB_ERROR_IO` is not taken for a loss: `aravissrc` then reports a frame timeout or failed frames, as `/fakeuvgst/unplug` expects there.

### 16. No device provider

**Problem.** Applications that list cameras through `GstDeviceMonitor` never saw Aravis cameras.

**What changed.**

- New `gst/gstaravisdeviceprovider.c` and `.h`: `aravisdeviceprovider`, registered at `GST_RANK_MARGINAL` in `plugin_init`.
- `gst_aravis_device_provider_probe ()` runs `arv_update_device_list ()` and returns one device per camera:
  - display name "vendor model (serial)", class `Video/Source`, caps `video/x-raw; video/x-bayer`;
  - properties `device.api` (aravis), `device.vendor.name`, `device.product.name`, `device.serial`, `aravis.device-id`, `aravis.physical-id`, `aravis.address`, `aravis.protocol`, `aravis.interface`.
- `gst_aravis_device_create_element ()` returns an `aravissrc` with `interface` and `camera-name` set through `gst_aravis_select_camera ()`, which does not open the camera.
- `GST_ARAVIS_DEVICE_PROVIDER=0` (or `off`, `false`, `no`), read at each probe, disables it.
- Library support: `arv_get_device_interface_id ()`, and interned device-list strings that a concurrent list update cannot free.
- A probe reads the whole device list between two equal readings of `arv_get_device_list_generation ()`, up to 10 times. So a `GstDevice` never mixes two cameras, and no camera is listed twice or left out for moving during the probe. A list that changes during all 10 reads gives no camera until the next probe.
- Cameras that share an identifier (same vendor, model and serial) on one interface are listed once; the per-probe dedupe no longer keeps a freed key. The dedupe key is the pair (interface, identifier) that `create_element` selects by: the same identifier on two interfaces, such as a USB3Vision and a GigEVision camera with the same vendor, model and serial, gives two devices.
- `gst/pipelines.md` is now published as the "GStreamer Plugin" page; "Device monitor" covers the cost, the opt-out, the GenTL opt-in and Windows exclusivity.

**Tests.**

- `gstaravis` device-provider tcase (9 cases): `test_device_provider_factory`, `test_device_provider_probe`, `test_device_provider_enabled_interfaces`, `test_device_provider_create_element`, `test_device_provider_launch_line`, `test_device_provider_monitor`, `test_device_provider_concurrent_open`, `test_device_provider_concurrent_listing`, `test_device_provider_disabled`. Also `test_gentl_device_provider`.
- `/fakeuvgst/provider-concurrent-update` (another thread changes the list at scripted device reads of a probe), `provider-while-streaming`, `provider-list-changes-twice`, `provider-duplicate-ids`. `/fakeuvgst/provider-same-id-two-interfaces` (a fake USB and a fake GigE camera both named `Aravis-Fake-1`) runs only in the `fakeuvgst-network` test of the `network` suite, as the fake GigE camera binds the fixed GigE Vision ports.
- `/fake/device-list-generation` and `/fakeuvsystem/device-list-generation`: when the generation changes, and when it does not.
- Before the review fixes, `gst-device-monitor-1.0 Video/Source` listed an `arv-fake-gv-camera` on 127.0.0.1, and its launch line played 20 buffers. This was not re-run after the review fixes.

**Deviations from the plan.**

- `create_element` sets `interface` as well as `camera-name`, and does not open the camera.
- The rank stays marginal with an environment opt-out; the plan's fallback was `GST_RANK_NONE`.
- A new library function, `arv_get_device_list_generation ()`, makes each probe a consistent snapshot of the device list.

**Not done.** Step 3, hotplug monitoring with `start()` and `stop()`, and `reconfigure_element`.

**Not verified here.**

- A real USB or GigE camera in `gst-device-monitor-1.0`.
- Each probe costs a full discovery. On Windows a camera streaming in this process is not listed; this is documented.

### 17. Kernel-driver auto-detach warning on every open

**Problem.** Every USB open on Windows logged "Failed to set auto kernel detach feature", because the call is not supported there.

**What changed.** `_try_usb_device ()` calls `libusb_set_auto_detach_kernel_driver ()` only when `libusb_has_capability (LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER)` is true. A real failure is still a warning.

**Tests.** `/fakeuvdevice/auto-detach`, `auto-detach-log`, and the subprocess cases `windows` and `auto-detach-failure`.

**Deviations from the plan.** None.

**Not verified here.** The plan's Done-when with real Windows libusb (`ARV_DEBUG=device:1`).

### 18. The USB completion thread runs at default priority and the application cannot reach it

**Problem.** In async mode the transfer callbacks complete frames on the `arv_libusb` event thread, which no application hook could raise, and `arv_make_thread_realtime ()` reported success it did not get.

**What changed.**

- `src/arvuvdevice.c`: `arv_uv_device_request_event_thread_priority ()` and `arv_uv_device_release_event_thread_priority ()`. Windows only.
- Both stream threads request after the `ARV_STREAM_CALLBACK_TYPE_INIT` callback and release before the EXIT callback.
- `_event_thread_update_priority ()` applies the higher of the request and the thread's own priority at the top of each turn of `event_thread_func ()`, and goes back afterwards.
- `src/arvrealtime.c`: `arv_make_thread_realtime ()` and `arv_make_thread_high_priority ()` read the priority class and thread priority back (`arv_win32_check_priority_request ()`), report what Windows granted, and correct the messages.
- `usb.md`: "Thread priority".

**Tests.** `fakeuvpriority`: 9 cases on Windows (`async`, `sync`, `unchanged`, `own-priority`, `two-cameras`, `request`, `never-lower`, `priority-request`, `realtime`), and only `other-platforms` elsewhere. Also `/fakeuvlibusb/thread-priority`. They also pass natively under MinGW.

**Deviations from the plan.** The event thread never goes below its own priority, and the request is withdrawn before the EXIT callback.

**Not done.** Step 3, MMCSS (`AvSetMmThreadCharacteristicsW`), which the plan ties to item 20 showing dropped frames.

**Not verified here.** The plan's Done-when in Process Explorer with `arv-camera-test-0.10 --high-priority`. The fake completes transfers at once, which overstates what the priority change achieves.

### 19. Debug glyphs render as mojibake in Windows consoles

**Problem.** Each log line starts with a four-byte UTF-8 glyph, which a console on a legacy code page prints as garbage.

**What changed.**

- `src/arvdebug.c`: `arv_debug_use_glyphs ()` (private, exported for the tests) decides once, on the first message. It uses:
  - the console output code page (`GetConsoleOutputCP ()`);
  - whether stderr is the console (`GetConsoleMode ()`, or a zero-length `WriteConsoleW ()` for write-only handles);
  - the code page of the C runtime locale (`___lc_codepage_func ()`);
  - `g_get_console_charset ()` for redirected output;
  - a terminal query, `arv_debug_is_terminal ()`.
- `arv_debug_is_terminal ()`: `PseudoConsoleWindow` (ConPTY) means a terminal; `ConsoleWindowClass` means a classic console; otherwise `WT_SESSION` or `TERM_PROGRAM`.
- The letters `W`, `I`, `D` and `T` are padded to the glyphs' width. Outside Windows the glyphs stay.
- `arv_debug_with_level ()` formats each message with GLib and writes it once with `fwrite ()` and `fflush ()` on Aravis's own stderr (see the debug-CRT row below).
- `building.md`: "Debugging Aravis".

**Tests.** `debug` (suites `main` and `debug`): 22 cases on Windows, 4 elsewhere.

- Everywhere: `/debug/use-glyphs` (a decision table), `/debug/use-glyphs-without-queries` and `/debug/is-terminal`.
- On Windows with GLib 2.62 or later, 19 `/debug/output/*` cases run child processes that write to a pipe, a hidden console read back, and a pseudo console. The children cover code pages 437 and 65001, `LANG` values, C runtime locales, and write-only console handles.
- Elsewhere, `/debug/output/glyphs` checks that the glyphs stay.

**Deviations from the plan.**

- A classic console window on code page 65001 gets letters: it stores U+FFFD for the glyphs. The plan kept glyphs on 65001.
- Programs that set a legacy C runtime locale, such as `gst-launch-1.0` and GTK applications, get letters even on 65001.

**Not verified here.**

- Nobody looked at a real `cmd.exe` or Windows Terminal window; the tests read the console buffer back. Windows 10 and Server 2022 were not tried.
- Known gap: the choice is made at Aravis's first message. A program that sets its C runtime locale later keeps the earlier choice.

### 20. No published Windows throughput numbers

**Problem.** Nobody had published what Aravis achieves over WinUSB, so items 1, 4 and 18 had no baseline.

**What changed.**

- `scripts/arv-usb-benchmark.py` runs `arv-camera-test-0.10 --usb-mode=async --usb-maximum-transfer-size=N --duration=60` over a matrix. The default is 64 KiB, 256 KiB and 1 MiB, RAW_IO off and on, default and `--high-priority`: twelve runs per camera on Windows. Elsewhere, RAW_IO defaults to off only: six runs.
- Each run sets `ARV_UV_RAW_IO` to 0 or 1, and `ARV_DEBUG=stream:2,stream-thread:1` unless `ARV_DEBUG` is set.
- The script records the frame rate, throughput, errors, stream statistics and CPU time. It also records the actual RAW_IO state and the RAW_IO maximum, and flags runs whose state differs from their setting.
- It writes `results.md`, `results.csv`, `results.json` and per-run logs; `--dry-run` prints the commands.
- `usb.md`: "Throughput benchmark", with the protocol and an empty results table.

**Tests.** `usb-benchmark` (suite `benchmark`): the parser on saved outputs (`tests/data/arv-camera-test-*.txt`), the matrix, dry run and reports, and a run against the Fake camera. It also checks `usb.md`'s async default and the RAW_IO log formats of `src/arvuvstream.c`.

**Not done.** No numbers: the plan's Done-when needs a camera above 300 MiB/s, and items 1 and 4 then choose their defaults from it.

**Static builds.** `usb-benchmark` used to fail with `-Ddefault_library=static`: nothing referenced the library's embedded GResource, so a static link dropped it, and the Fake camera found no GenICam. `ArvFakeCamera` and the V4L2 device now read their XML through `arvresources_get_resource ()`, which keeps it. The static build passes 29/29.

### 21. GigE issue #1047: cameras are discovered but no frame completes

**Problem.** A camera stored at a packet size above the host interface's MTU, such as 8228 on a 1500-byte NIC, sends packets the host drops. Nothing in Aravis prevented it.

**What changed.**

- `src/arvnetwork.c`: interface MTU from `SIOCGIFMTU` (Linux, macOS, BSD) or `IP_ADAPTER_ADDRESSES.Mtu` (Windows). Values outside 68 to 65535, such as loopback's, count as unknown (`arv_network_interface_get_mtu ()`).
- `src/arvgvdevice.c`, `auto_packet_size ()`: the search is capped at the MTU and starts from it. Without a test packet, a larger stored size is lowered to the largest valid packet size not above the MTU. When the MTU is below the smallest valid packet size, that size is used, with a warning. The search is `arv_gv_device_search_packet_size ()` (private, unit-tested).
- `src/arvgvstream.c`: the `n_leader_trailer_only_failures` statistic and its rate-limited warning. The last frame of an acquisition times out once its trailer arrives.
- The fake GigE camera gains `gvsp-mtu` (`--gvsp-mtu`). The private hook `arv_network_set_loopback_mtu_override ()` lets tests give loopback an MTU.
- `src/arvgvinterface.c`: discovery reaches a device through the interface whose subnet contains its address.
- Linux packet socket:
  - IP fragments and truncated captures are ignored;
  - capture starts once the filter is attached;
  - it is not used for a device on the host reached through another interface's address.
- `ethernet.md`: the MTU clamp, the new statistic, packet-socket fragments, devices on the host.
- Related fix: stream-channel ownership under a lock (see "Renegotiation with a held stream" below).

**Tests.**

- `gvpacketsize` (9 cases): the search, the MTU rules, including an MTU below the smallest packet size (`mtu-below-minimum`), interface MTU enumeration on this machine's real adapters.
- `fakegvmtu` (suite `network`, 9 cases): `auto-packet-size`, `known-mtu`, `issue-1047`, `never-adjust`, `mtu-below-minimum`, the `leader-trailer-only` cases, `no-false-positive`. `issue-1047` streams complete frames from a camera stored at 8228 through a 1500-byte MTU.
- `gvsp`, `gvnetwork` and `fakegv` (7 cases, including `stream-channel` and `stream-channel-concurrent`).

**Deviations from the plan.**

- The end-to-end test uses a loopback MTU hook instead of reaching the fake camera through a LAN interface.
- The detector counts frames with none of their full-size data packets, since a smaller last packet may get through.

**Not done.** Step 3: ask the reporter of issue #1047 for the `-i 1500` result.

**Not verified here.**

- The plan's Done-when: a real camera stored at 8228 on a Windows NIC with MTU 1500.
- The Linux packet-socket changes were compiled only; macOS and BSD were never built.
- The detector's handling of multipart payloads: the fake camera cannot send them.
- The subnet-based choice of interface ran only with the fake camera on 127.0.0.1, never with a real camera on a host with several adapters.
- Jumbo frames: whether Windows reports an MTU of about 9000 for such an adapter, so that the packet size stays above 1500.
- `auto_packet_size ()` still points the channel at its test socket without the new lock (pre-existing).

## Defects fixed beyond the plan items

### From the plan's "Also found" table

| Where | Defect | Fix | Test |
| --- | --- | --- | --- |
| `src/arvuvstream.c`, `arv_uv_stream_buffer_context_new ()` | Each transfer context allocated twice; one leaked | Allocated once; unused contexts dropped once the table doubles (`arv_uv_stream_thread_drop_unused_contexts ()`) | `/fakeuvstream/replaced-buffers` bounds live transfers; the leak itself needs valgrind |
| `src/arvuvstream.c`, `_gendc_payload ()`, `_gendc_packet ()` | Y dimension stored in the width; height never set | Width from `dimension_x`, height from `dimension_y` | `/fakeuv/gendc`, `/fakeuvrawio/gendc*` |
| `src/arvuvstream.c`, trailer callback | Buffer pushed to the output queue before being detached; a buffer recycled at once, as `aravissrc` does, could be lost | `arv_uv_stream_buffer_context_complete ()` detaches, pushes, then calls BUFFER_DONE; callbacks touch the context last | `/fakeuvstream/trailer-race`, `recycle-in-callback` |
| `src/arvuvdevice.c`, `_send_cmd_and_receive_ack ()` | Missing `else` after the pending-ack branch: a bogus "unexpected answer" per pending ack | `else if` | `/fakeuvdevice/pending-ack-log`, `/fakeuv/pending-ack` |
| `src/arvuvdevice.c`, `src/arvuvinterface.c` | `libusb_get_config_descriptor ()` result unchecked: crash in discovery or open | Checked in `arv_uv_interface_match_descriptors ()` and `_find_endpoints ()` | `/fakeuvdevice/discovery-config-descriptor`, `open-config-descriptor` |
| `src/arvuvdevice.c`, open | `endpoint[0]` read on a data interface without endpoints: crash | `_find_endpoints ()` fails with `ARV_DEVICE_ERROR_PROTOCOL_ERROR` | `/fakeuvdevice/no-data-endpoint` |
| `src/arvuvinterface.c`, `arv_uv_interface_open_device ()` | `error` passed where `&local_error` was meant: retried open, leaked GError | `&local_error`; a known device's error is returned at once | `/fakeuvdevice/open-error` |
| `gst/gstaravis.c`, property setters | `usb-mode` ignored when set after `camera-name` | Both USB properties apply to an open camera and at open | `/fakeuvgst/usb-mode`, `/fakeuvgst/transfer-size` |
| `src/gentl/` | Aravis's own GenTL producer exported 37 of the 57 functions its consumer needs | All 64 GenTL functions exported (DS and Event stubs); TLGetInfo and interface infos; IFOpenDevice through the interface; DevClose; reload; no GenTL interface recursion. Also fixed: the last error, `*pbChanged`, the size in local URLs, and interface handles after `arv_shutdown ()` | `/gentl/producer/api`, `discovery`, `reload`, `shutdown` (need `-Dgentl-producer=true`) |
| `src/arvnetwork.c`, `src/gentl/*.c` | printf formats wrong on Windows (`%d` for `DWORD`, `%ld` for 64-bit values) | Formats fixed, also in an `#if 0` block of `src/arvgvstream.c` | MinGW cross-build: the 11 old warnings are gone, 0 new |
| `src/arvgvspprivate.h` | `G_LIKELY (leader) != NULL` | `G_LIKELY (leader != NULL)` | `gvsp`, built with `G_LIKELY` redefined so the misuse fails to compile |
| `docs/reference/aravis/usb.md`, and the doc comment of `arv_uv_device_set_usb_mode ()` | Said sync is the default USB mode | "Performance" and the doc comment say async (`ARV_UV_USB_MODE_DEFAULT`) | `usb-benchmark` checks the `usb.md` text |
| `tests/fakegv` | Failed when the fake camera was discovered through a non-loopback interface | Device reached through the interface whose subnet contains its address; `fakegv` never runs in parallel | `fakegv`, `/gvnetwork/subnet` |

### Found during the work

"In upstream?" says whether upstream `main` at `de17cdde` has the defect. The "No" rows came with this branch's own changes and were fixed on it.

| Where | Defect | Fix | Test | In upstream? |
| --- | --- | --- | --- | --- |
| `src/arvuvdevice.c`, `_get_data_size_max ()` | `write_memory` blocks sized from the acknowledge maximum: writes above 1004 bytes failed, with no GError, on 1024-byte transfers | Each block fits its packet and the 16-bit size fields, in whole 32-bit words; a maximum with room for under 4 bytes is a protocol error | `/fakeuvdevice/memory-chunks`, `memory-large`, `memory-small-max` | Yes |
| `src/arvstream.c` | `n_input` read -1 while the stream thread waited (`g_async_queue_length ()` counts waiters) | Queue lengths counted under each queue's lock; `n_buffer_filling` atomic | `/fakeuvstream/owned-buffers-async`, `owned-buffers-sync`; `/gentl/stream/late-buffer` | Yes |
| `src/arvuvstream.c` | Trailer-callback race (the "Also found" row above) confirmed: it caused fakeuv flakes under load | See above | `/fakeuvstream/trailer-race` | Yes |
| `src/arvuvstream.c` | GenDC image-data size taken from the container | Part size from the part header (`arv_uvsp_packet_get_gendc_partdatasize ()`); `_gendc_clamp_part_size ()` on every completion path | `/fakeuv/gendc`, `/fakeuvrawio/gendc`, `gendc-small`, `gendc-failed` | Yes |
| `src/arvdebug.c`, `arv_debug_with_level ()` | Debug-CRT hang: a debug (`/MDd`) Aravis passed its stderr to the release GLib's `g_fprintf ()`; exit then hung on an assertion dialog or corrupted the heap | Format with GLib, write once with `fwrite ()` and `fflush ()` on Aravis's own stderr; `arv-test` uses `printf ()` | `debug` fails at once on revert; `gst-launch-1.0` with `ARV_DEBUG` on the `/MDd` build exits 0 | Yes |
| `src/arvuvstream.c`, async context lookup | A new buffer allocated at a freed buffer's address reused its context: libusb wrote frames into freed memory, then a crash | Contexts keyed by payload memory with a size check, renewed once idle; memory already being filled comes back ABORTED | `/fakeuvstream/replaced-buffers`, `wrapped-buffers`, `resized-buffer`, their `*-raw-io` variants, `shared-memory` | Yes |
| `src/arvuvstream.c`, `arv_uv_stream_thread_sync ()` | After a lost trailer, the next leader was parsed from a buffer already handed to the application (use-after-free) | Leaders and trailers parsed from a zero-padded copy (`_copy_packet ()`); short reads not parsed | `/fakeuvstream/sync-lost-trailer-replace`, `sync-lost-trailer-reuse`, `sync-lost-trailer-only-*` | Yes |
| `gst/gstaravis.c` | GenTL frames lent downstream kept the data stream and device open: caps changes and restarts failed with "Requested resource is already in use" | GenTL frames are copied | `test_gentl_renegotiation`, `test_gentl_restart_holding_frame`, `test_gentl_renegotiation_followed_stream`, `test_gentl_restart_followed_objects` | No: lending is new here |
| `gst/gstaravis.c`, `src/arvuvstream.c`, `src/arvgvdevice.c`, `src/arvgvstream.c` | Renegotiation with a held stream: an old `stream` object kept by the application was restarted, then its release stopped the new stream | The element starts and stops only its own stream; USB: only the stream owning the stream interface undoes SIRM and RAW_IO; GigE: a stream disables its channel only while it owns it, under a lock | `test_ownership_renegotiation_held_stream`, `test_network_gv_renegotiation_held_stream`, `/fakeuvgst/renegotiation-held-stream`, `/fakeuvstream/stale-stream*`, `/fakegv/stream-channel`, `stream-channel-concurrent` | Yes |
| `src/arvuvstream.c`, transfer callbacks | During a RAW_IO resynchronization, a trailer callback read its context's `is_aborting` flag twice: a lost frame could come back ABORTED instead of failed | Atomic flags, `is_resyncing` set first, each callback reads them once on entry | `/fakeuvrawio/resync*` and `rejected-read` check `n_aborted == 0`; with a delay injected between the two reads, the old code failed 6 of 8 runs | No: RAW_IO recovery is new here |
| `src/arvsystem.c`, `src/arvinterface.c` | A reader of several device-list fields could mix two states of the list | `arv_get_device_list_generation ()`; the interface selection functions take the device-list lock | `/fake/device-list-generation`, `/fakeuvsystem/device-list-generation`, `/fakeuvgst/provider-*` | Yes |
| `src/arvfakecamera.c`, `src/arvv4l2device.c` | In a static link, the embedded GResource was dropped: the Fake camera found no GenICam (`arv-camera-test`, `arv-tool`) | XML read through `arvresources_get_resource ()` | `usb-benchmark` in a static build | Yes |
| `src/arvdomparser.c`, `src/arvdomimplementation.c` | Thread-unsafe GenICam parsing: two devices constructed at once crashed | SAX state local to each parse; `xmlInitParser ()` once; `document_types` under a mutex | `/dom/parallel-parse` | Yes |
| `meson.build` | `-Ddefault_library=both` with MSVC produced no import library | `build_shared` covers `both` | build with `both`: DLL, import and static libraries, 31/31 tests | Yes |
| `src/arvuvdevice.c`, `src/meson.build` | The libusb 1.0.30 functions were hard imports: Aravis failed to load with an older `libusb-1.0.dll` first | Run-time lookup, or direct calls with a static libusb | `/fakeuvdevice/raw-io-log` subprocesses, `/fakeuvrawio/older-libusb-dll`, `uvlibusb` | No: introduced and fixed here |
| `src/arvuvdevice.c`, `_send_cmd_and_receive_ack ()` | A pending ack answering a read of 1 to 3 bytes overran the acknowledge buffer; acks shorter than a header were parsed | Room for a pending ack; acks shorter than a header not parsed; read acks must carry the data | `/fakeuvdevice/pending-ack-small-read`, `short-ack`, `ack-zero-length-packet-log` | Yes |
| `src/arvuvstream.c`, sync mode | Reads shorter than a packet header parsed (overread on Windows); buffers 1 to 27 bytes larger than the payload failed every frame; a frame cut short livelocked Linux or came back SUCCESS | Short reads not parsed; payload read to its expected size; a cut frame fails, then the stream reads up to the next trailer or leader | `/fakeuvstream/sync-larger-buffers-*`, `sync-cut-frames-*`, `sync-padded-final-transfer` | Yes |
| `src/arvuvstream.c`, acquisition start | SBRM and SIRM register reads unchecked: the layout was computed from undefined values | Checked, with an error | `/fakeuvstream/start-read-error` | Yes |
| `src/arvuvstream.c`, RAW_IO resync | The resync limit never triggered when reads timed out | Timed-out reads with data count | `/fakeuvrawio/resync-limit`, `resync-limit-slow` | No: RAW_IO is new here |
| `src/arvgvdevice.c`, `src/arvgvstream.c` | A former stream destroyed in another thread during the next stream's creation could disable the new channel; a failed stream wrote through a NULL device | Stream-channel lock; NULL check | `/fakegv/stream-channel-concurrent` | Yes: upstream always disabled the channel |
| `src/arvgvstream.c` | The last frame of an acquisition stayed pending when its first data packet was lost | Times out (TIMEOUT) once the trailer arrived | `/fakegvmtu/leader-trailer-only-single-frame` | Yes |
| `src/arvgvstream.c`, `src/arvnetwork.c` (Linux packet socket) | IP fragments and truncated captures were parsed as whole GVSP packets, completing frames with missing data; traffic captured before the filter was attached was parsed too | Ignored and counted, with a warning; capture starts once the filter is attached | `/gvnetwork/ethernet-frame`; Linux path compiled only | Yes |
| `src/arvgentldevice.c` (`-Devent=true`) | Event thread leaked the event id per event and parsed unterminated strings | Freed, zero-terminated, `GBytes` under a mutex, bounds checks | `/gentl/device/events` | Yes |
| `src/arvgentlstream.c` | Received size from the producer unclamped, and `aravissrc` lent that many bytes | Clamped, with `ARV_BUFFER_STATUS_SIZE_MISMATCH` | `/gentl/stream/oversized-frame` | Yes, exposed by lending |
| `gst/gstaravis.c` | GigE Vision and USB3 Vision cameras sent no frame after a caps change: the old stream was released after the new one was created | Old stream released first | `test_network_gv_renegotiation`, `test_ownership_renegotiation_stream` | Yes |
| `gst/gstaravisdeviceprovider.c` | Per-probe dedupe kept a freed key: a camera seen three times read freed memory | Fixed | `/fakeuvgst/provider-list-changes-twice` | No: the provider is new here |
| `src/arvdevice.c`, `arv_device_start_acquisition ()` | The stream's start error was dropped | The first one is put in the message | `/gentl/stream/start-failure` | Yes |
| `src/arvsystem.c`, `arv_get_info ()` | The device-list queries returned the interface's string after releasing the lock: a concurrent update could free it | Strings interned (`g_intern_string ()`) | `/fake/device-list-strings`, `/fake/device-list-threads` | Yes |

### From the audit of 9 October 2026

An external audit of `8d517b7a` reported six findings, read from the source only (four at P2, two at P3 on its scale). Each was reproduced here with a compiled test that fails on `8d517b7a`, then fixed; the six fixes were reviewed together. The "Verified" column is the rating after reproduction.

| Audit | Where | Defect | Fix | Test | Verified |
| --- | --- | --- | --- | --- | --- |
| 1 (P2) | `src/arvuvstream.c`, `src/arvuvdevice.c` | A failed RAW_IO disable at stop left the policy on; the next acquisition without RAW_IO got every read rejected, silently | The device tracks the actual policy; a start without RAW_IO turns it off first, or fails | `/fakeuvrawio/failed-disable*` | P3: needs a policy change to fail on a plugged camera, on the opt-in RAW_IO path |
| 2 (P2) | `src/arvgentlstream.c` | A nonzero `BUFFER_INFO_IMAGEOFFSET` made complete single-part frames `SIZE_MISMATCH` | Image part size `DATA_SIZE - IMAGEOFFSET`; `aravissrc` sends the image part | `/gentl/stream/image-offset`, `test_gentl_image_offset` (frame content) | P2: every frame of such a producer is lost |
| 3 (P2) | `src/arvgentlinterface.c` | Without serial numbers, devices of two interfaces or two producers shared an Aravis id: one could not be opened, and its entry opened the other | Producer and interface in the fallback id; components escaped after the re-audit below | `/gentl/discovery/missing-serial-number-interfaces`, `-producers`, `interfaces-same-device-id`, and the encoding cases below | P2 |
| 4 (P2) | `gst/gstaravisdeviceprovider.c` | The provider deduplicated by device id only, hiding a camera with the same id on another interface | Key on (interface, device id) | `/fakeuvgst/provider-same-id-two-interfaces` (`fakeuvgst-network`) | P3: needs the same vendor, model and serial on two transports |
| 5 (P3) | `.github/scripts/package-windows.py`, workflows | Fork zips named upstream as their source | `--source-url` from the workflow | `package-windows` | P3 |
| 6 (P3) | `src/arvgvdevice.c`, `src/arvcamera.c`, docs | "Never exceeds the MTU" was false when the camera's smallest packet size is larger, and the log said the size fit | Corrected wording in every place, truthful info line, a warning when no size fits | `/gvpacketsize/mtu-below-minimum`, `/fakegvmtu/mtu-below-minimum` | P3 |

### From the v030 re-audit of 9 October 2026

The re-audit of `573b2271` found that finding 3 was only partly fixed: joining producer, interface and device ids with unescaped hyphens still merged different tuples. `Arv-Q.cti` / `A0` / `MockDevice0` and `Arv.cti` / `Q-A0` / `MockDevice0` listed the same id, and both entries opened one camera.

`src/arvgentlinterface.c` now escapes each fallback component before joining it. Only ASCII letters, digits, `_`, `.` and `~` remain literal; other bytes use uppercase `%HH`, including hyphens, percent signs, control bytes and UTF-8 bytes. The example now yields `Arv%2DQ-A0-MockDevice0` and `Arv-Q%2DA0-MockDevice0`, and each opens its own camera. The raw producer identifiers used to open interfaces/devices are preserved. The compatibility change for special-character fallback ids is documented in `gentl.md`; supplied serial numbers and simple fallback components retain their spelling.

Five regressions were added under `/gentl/discovery/missing-serial-number-`: `producer-separators`, `interface-device-separators`, `percent-identifiers`, `control-identifiers` and `utf8-identifiers`. They check unique/exact ids and the actual cameras reached, independently of device-list sorting. A mock-only `arv_gentl_mock_set_ids()` control changes opaque ids between discoveries without changing the cameras' serial registers. The tests use ordinary compiled mock producers, without binary patching.

Validation on the MSVC debug shared build: compilation passed; the full `gentl` program passed 46 cases with six configuration skips (four Aravis-producer cases and two event cases); `gstaravis` passed all 50 checks; `doc-pages` passed. All five new regressions were also run against the preserved, unmodified `573b2271` Aravis DLL and failed on duplicate ids, missing encoded ids or the wrong opened camera. With the rebuilt DLL, all five pass. Local evidence is in `build-msvc/meson-logs/gentl-id-fix-final.*`, `gentl-id-fix.*` (GStreamer/docs), and `build-msvc/audit-v030/id-regressions-before.json`.

The documented same-basename producer-copy and raw device-id alias limitations remain. No hardware or other-platform validation was added in this follow-up.

## Build, CI and packaging

### meson

- `meson.build`:
  - Windows: the libusb floor and wrap fallback of item 2, with `examples=false` and `tests=false` for the subproject.
  - `build_shared` is true for `default_library` `shared` and `both`, so MSVC exports the API in both cases.
  - `aravis_dependencies_without_usb`, for the fake libusb tests.
  - `subdir ('data' / 'windows')`, and `subdir ('tests')` moved after `subdir ('gst')`: the plugin tests need the plugin.
- `src/meson.build`: `arvsystemprivate.h`; on Windows, `-DARV_UV_LIBUSB_STATIC` when libusb is linked statically; `aravis_programs`, the tools by name, for the tests.
- `ARV_UV_LIBUSB_STATIC` is detected for the wrap built as a static library, or for an installed libusb whose internal `usbi_backend` symbol links.
- `gst/meson.build`: the device provider sources; `gst_source_files` and `gst_inc` for `fakeuvgst`.
- `data/windows/meson.build`: installs the WinUSB files on Windows builds with USB.
- `tests/meson.build`: the new tests. `network` tests never run in parallel. The valgrind setup sets `FAKEUV_TIME_SCALE=10`. Besides `python`, already excluded, it now also excludes the `packaging`, `benchmark`, `gst` and `fakeuvgst` suites; its comment says why, and what stands in for valgrind for the last two. The fakeuv programs have a 180 s timeout.
- Documentation: `gentl.md` and `gst/pipelines.md` rebuild the docs when edited, and `pipelines.md` is published (`docs/reference/aravis/meson.build`, `aravis.toml.in`).

### The libusb wrap

- `subprojects/libusb.wrap` takes the libusb 1.0.30 release tarball (GitHub, with WrapDB as fallback URL), and `patch_directory = libusb`.
- `subprojects/packagefiles/libusb` holds WrapDB's `libusb_1.0.30-1` meson build files, with WrapDB's MIT `LICENSE.build`. The one change: the path of `libusb/os/windows_hotplug.c`, so that `-Dlibusb:windows-hotplug=true` builds.
- Why packagefiles:
  - WrapDB's build files cannot build the hotplug flavour, and WrapDB master still has the bug;
  - `diff_files` needs a `patch` or `git` program, absent from the MSYS2 CI environment, and meson 0.63, above Aravis's 0.57 floor;
  - `patch_directory` needs no external tool.
- The wrap has no `wrapdb_version`, so `meson wrap update libusb` refuses without `--force`.
- `subprojects/.gitignore` ignores `libusb-*/`, `packagecache/` and `.wraplock`.
- A `subprojects/libusb-1.0.30` extracted with WrapDB's original files must be deleted, or purged with `meson subprojects purge --confirm libusb`, before the hotplug flavour builds.

### Workflows

| Workflow | Change | What it does now |
| --- | --- | --- |
| `aravis-msvc-gstreamer.yml` (Aravis-MSVC-GStreamer) | Added | Official GStreamer 1.28.7 MSVC kit; plugin and hotplug libusb; package, smoke test, `meson test --no-suite libusb`, libusb floor check; artifact `aravis-msvc-x86_64`; `release-assets` job |
| `aravis-msvc.yml` (Conan 1) | Removed | Was: `conan<2`, libusb 1.0.26, no plugin |
| `aravis-msvc-conan2.yml` | Changed | libusb 1.0.30 from the wrap instead of ConanCenter's 1.0.29; `ARV_TEST_LIBUSB_MIN_VERSION=1.0.30`; newer actions |
| `aravis-mingw.yml` | Changed | `mingw64`: MSYS2's libusb, plain build. `ucrt64`: release build, hotplug libusb from the wrap, packaged and smoke-tested, artifact `aravis-msys2-ucrt64`. Release trigger, `release-assets` job, libusb version checks |
| `aravis-linux.yml` | Changed | Installs `gstreamer-check` (`libgstreamer1.0-dev`), base plugins and tools, so the GStreamer tests build; newer actions |
| `aravis-macos.yml` | Changed | Newer actions only |

- `README.md`: the Aravis-MSVC badge now points to Aravis-MSVC-GStreamer. `RELEASING.md`: check that the release got its zips.
- New scripts in `.github/scripts/`: `gstreamer-native-file.py`, `package-windows.py`, `smoke-test-gstreamer.py`, `check-libusb-floor.py`.
- In every Windows job, tests run with `-Dgv-n-buffers=1`, the upstream workaround of commit `b4211e5e`. The packaging jobs set it after making the zips, which keep the default of 16.
- None of these workflows has run.
- Risk for `winusb-setup` in the Windows jobs: the windows-2025 runner has pnputil 10.0.26100. The helper's repair of pnputil's missing XML root was measured on 10.0.26200 only.
- The Linux job builds the documentation, the introspection data and the viewer, and the MinGW job the last two. No build here had them (see the [verification matrix](#verification-matrix)).
- The fakeuv tests wait long (30 s, scaled) only when they fail, to suit slower hosted runners; the workflows' own time limits were not tuned.

### Artifacts and Windows packaging

- `package-windows.py` checks the `meson install --destdir` tree, then writes `aravis-<version>[-g<commit>]-<flavour>.zip` and its `.sha256` file.
- Required contents: the Aravis DLL, import library and headers, `arv-tool`, `arv-camera-test`, the plugin, a hotplug `libusb-1.0.dll` from the wrap, and `aravis-u3v.inf`, `aravis-u3v.cdf` and `arv-winusb-setup.ps1`.
- No GLib or GStreamer DLL may be in the zip: the plugin must use the GLib of the user's GStreamer. The README lists WinUSB, libusbK and libusb0 as usable drivers.
- `-g<commit>` appears in the names of non-release builds. For a published release, the jobs pass `--release`, which leaves it out.
- `release-assets` jobs (`contents: write`) attach the zips to a published release with `gh release upload --clobber`.

### The MSVC job and the GStreamer kit

The official GStreamer MSVC package has quirks; the job handles each:

| Quirk | Handling |
| --- | --- |
| The `.pc` files compute their prefix from their location, and pkg-config escapes the space of `C:\Program Files` | Install to `C:\gstreamer\1.0\msvc_x86_64` with `/DIR=` |
| `glib-mkenums` and `glib-genmarshal` start with the packager's Python path | `gstreamer-native-file.py` writes a native file that runs them with the job's Python and uses the kit's `pkg-config` |
| The kit has no libusb | `subprojects/libusb.wrap` with `-Dlibusb:windows-hotplug=true` |
| Silent install of the Inno Setup installer | `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /ALLUSERS /TYPE=devel /MERGETASKS=!install_vcredist`, checked by exit code and `gstreamer-1.0.pc`, log printed on failure |
| Download size and integrity | Installer pinned by SHA-256 and cached with `actions/cache` |
| MSVC environment | `meson setup --vsenv`; `GSTREAMER_1_0_ROOT_MSVC_X86_64` exported and the kit's `bin` added to `PATH` |
| PowerShell splits unquoted `-Dfoo.bar=baz` at the dot | `"-Dpkgconfig.relocatable=true"` and `"-Dlibusb:windows-hotplug=true"` are quoted |

The job does not set `-Dgentl-producer=true`, so `/gentl/producer/*` skip there.

## Testing

### New and changed test programs

| Suite | Program | What it covers | Runs where |
| --- | --- | --- | --- |
| `fakeuv` | `fakeuv` | Discovery, open, features, async and sync acquisition, single frame, restart, software trigger, hotplug unplug, pending acks, 64-bit frame ids, GenDC | Every build with USB |
| `fakeuv` | `fakeuvlibusb` | The fake's own libusb semantics | Every build with USB |
| `fakeuv` | `fakeuvdevice` | Open, control channel, memory chunking, hotplug registration, auto-detach, RAW_IO and lookup logs | Every build with USB |
| `fakeuv` | `fakeuvdiscovery` | Open errors and hints, device strings, open filtering and locations, `arv-tool` output | Every build with USB |
| `fakeuv` | `fakeuvsystem` | Not-found errors of `arv_open_device_on_interface ()` | Every build with USB |
| `fakeuv` | `fakeuvstream` | Buffer ownership, trailer race, context reuse, stale streams, sync-mode framing | Every build with USB |
| `fakeuv` | `fakeuvunplug`, `fakeuvunplugsync`, `fakeuvunplugrawio` | Unplug and transient failures in async, sync and RAW_IO modes | Every build with USB; Windows-only cases skip elsewhere |
| `fakeuv` | `fakeuvrawio` | RAW_IO sizes, bounce buffer, recovery, environment variable, transfer-size rules | Every build with USB |
| `fakeuv` | `fakeuvpriority` | Event-thread priority | Every build with USB; the effect is Windows-only |
| `fakeuv`, `fakeuvgst` | `fakeuvgst` | `aravissrc` and the provider against the fake libusb | With the plugin and `gstreamer-check-1.0` |
| `usb` | `libusb-version` | Run-time libusb version and hotplug, against the CI expectations | Every build with USB |
| `usb` | `uvlibusb` | Aravis reaches `libusb_get_device_string ()` in the build's real libusb, DLL or static, through the shared lookup; the RAW_IO functions are not called | Every build with USB |
| `packaging` | `libusb-wrap` | The wrap needs no patch or git program; its files name existing sources | Everywhere |
| `packaging` | `doc-pages` | Every page published; links name existing pages and sections | Everywhere |
| `packaging` | `winusb-inf` | INF, CDF and helper; InfVerif and MakeCat when present | Everywhere |
| `packaging` | `winusb-setup` | `arv-winusb-setup.ps1` on saved outputs, `-WhatIf`, live read-only checks | Windows only |
| `benchmark` | `usb-benchmark` | The benchmark script, and a run against the Fake camera | Everywhere |
| `main`, `debug` | `debug` | Glyph or letter choice; console, redirected and pseudo-console children | Everywhere; children on Windows only |
| `main` | `dom`, `fake` (changed) | `/dom/parallel-parse`; device-list threads and strings, `/fake/open-device-on-interface`, `/fake/device-interface-id` | Everywhere |
| `gige` | `gvpacketsize`, `gvsp`, `gvnetwork` | Packet size search and MTU; GVSP accessors; subnets and the packet-socket frame parser | Everywhere |
| `network` | `fakegv` (changed), `fakegvmtu` | Stream-channel ownership; MTU clamp, issue #1047, leader+trailer-only detection | Everywhere; binds the GVCP port |
| `gentl` | `gentl` | The GenTL consumer against the mock producers | Shared and `both` builds |
| `gst` | `gstaravis` | `aravissrc` and the provider: harness, latency, usb, interface, ownership, unlock, device-provider and gentl tcases | Shared and `both` builds with `gstreamer-check-1.0` |
| `network`, `gst` | `gstaravis-network` | GigE renegotiation, including a held old stream, with a fake camera on 127.0.0.1 | Shared and `both` builds with `gstreamer-check-1.0`; binds the GVCP port |

`.github/scripts/check-libusb-floor.py` is a CI script, not a meson test. The fake's scripting API is documented in `tests/fakeusb/fakeusb.h`.

### Running the tests on Windows

This is the setup used for the verification here: MSVC, the official GStreamer MSVC kit (1.28.3 here), meson and ninja from pip, Git Bash.

1. Put the kit's `bin` folder on `PATH`: it provides `pkg-config.exe`.
2. If the kit is under `C:\Program Files`, its `.pc` files break on the space. Point `PKG_CONFIG_LIBDIR` at the 8.3 short path of its `lib\pkgconfig` (`dir /x` shows the short names):

   ```sh
   export PKG_CONFIG_LIBDIR="C:/PROGRA~1/GSTREA~1/1.0/MSVC_X~1/lib/pkgconfig"
   ```

   In PowerShell: `$env:PKG_CONFIG_LIBDIR = "C:/PROGRA~1/GSTREA~1/1.0/MSVC_X~1/lib/pkgconfig"`. A kit installed in a path without spaces needs none of this.
3. Give meson a native file that runs the kit's `glib-mkenums` and `glib-genmarshal` with your Python:

   ```ini
   [binaries]
   glib-mkenums = ['C:/Python314/python.exe', 'C:/PROGRA~1/GSTREA~1/1.0/MSVC_X~1/bin/glib-mkenums']
   glib-genmarshal = ['C:/Python314/python.exe', 'C:/PROGRA~1/GSTREA~1/1.0/MSVC_X~1/bin/glib-genmarshal']
   ```

   `python .github/scripts/gstreamer-native-file.py <kit root> msvc-native.ini` writes the same, plus the kit's `pkg-config` and `.pc` folder, as the CI job does. Give it the 8.3 path of a kit under `C:\Program Files`.
4. Configure and build. `--vsenv` sets up MSVC outside a developer shell. Meson builds libusb 1.0.30 from the wrap, since the kit has none; the first configure downloads the tarball into `subprojects/packagecache`. If `subprojects/libusb-1.0.30` exists from an older checkout, run `meson subprojects purge --confirm libusb` first (see [The libusb wrap](#the-libusb-wrap)).

   ```sh
   meson setup build-msvc --native-file msvc-native.ini --vsenv -Dgst-plugin=enabled -Dusb=enabled -Dviewer=disabled -Dintrospection=disabled -Ddocumentation=disabled
   meson compile -C build-msvc
   ```

   Add `-Dlibusb:windows-hotplug=true` for the hotplug flavour, and `-Dgentl-producer=true -Devent=true` to run every GenTL case.
5. Run the tests. `--no-suite libusb` mirrors the CI command. It excludes nothing unless `-Dlibusb:tests=true`, as `meson.build` builds the wrap with `tests=false`:

   ```sh
   meson test -C build-msvc --no-suite libusb --no-suite network --print-errorlogs
   meson test -C build-msvc --suite network --print-errorlogs
   ```

Tips:

- The `network` suite (`fakegv`, `fakegvmtu`, `gstaravis-network`) runs a fake GigE camera on 127.0.0.1, which binds the fixed GVCP port 3956. Never run two network suites at once on one machine; meson already runs these tests one at a time.
- From Git Bash, set `MSYS_NO_PATHCONV=1` before `<test>.exe -p /suite/case`. Otherwise Git Bash rewrites the path, no test runs, and the run still reports success.
- `FAKEUV_TIME_SCALE=10` stretches the fake's timings, as the valgrind setup does; `FAKE_USB_DEBUG=1` and `FAKEUV_DEBUG=1` log the fake's events.
- Under AddressSanitizer (`-Db_sanitize=address -Db_vscrt=md`), give `gstaravis` a `--timeout-multiplier` of 2 or more.
- Keep build directory paths short: very deep paths exceed MAX_PATH for `cl.exe` on the libusb sources.
- `winusb-setup` also runs PowerShell 7 when `pwsh` is on `PATH`.
- To try the plugin from the build tree, put `build-msvc\src`, `build-msvc\subprojects\libusb-1.0.30` and the kit's `bin` on `PATH`, set `GST_PLUGIN_PATH` to `build-msvc\gst`, then run `gst-inspect-1.0 aravissrc`.

### Verification matrix

Machine: Windows 11 Pro 10.0.26200, MSVC 19.51 (Visual Studio 18.9.1), meson 1.12.1, GStreamer MSVC kit 1.28.3 (GLib 2.82.4). Results on the final tree, unless a row says otherwise.

| Configuration | Result |
| --- | --- |
| MSVC debug (`/MDd`), shared, release GLib of the kit | 32/32 tests, network 4/4 with the audit fixes (31/31 and 3/3 before) |
| MSVC release | 31/31, network 3/3 |
| `-Ddefault_library=both` | DLL, import library and static library built; 31/31, network 3/3 |
| `-Ddefault_library=static` | 29/29 (static builds define no `gst` or `gentl` tests) |
| `-Dusb=disabled` (`/MDd`) | 17/17, network 3/3 |
| `-Dgentl-producer=true -Devent=true` | 31/31 with `gentl` 43/43 and no skips, network 3/3 |
| AddressSanitizer, with and without `gentl-producer` and `event` | Before the last small fix round: 31/31 and network 3/3 each; 267 program runs, also with `ARV_DEBUG=all:3`, 0 ASan reports. After it: `gstaravis`, `fake`, `fakeuvsystem`, `fakeuvgst` and `gentl` 5/5 under ASan, and the provider's concurrent-listing case 10/10 (200 probes each), 0 ASan reports |
| Mirror of the MSVC CI job (release, relocatable `.pc`, hotplug libusb, `gv-n-buffers=1`, libusb checks) | 34/34; install, package and smoke test pass |
| Conan job options (release, no plugin, `gentl-producer`, `gv-n-buffers=1`) | 31/31 in 3 of 4 runs; one `debug` timeout under machine load, not reproduced |
| `FAKEUV_TIME_SCALE=10` (valgrind timings, without valgrind) | All 12 fakeuv programs pass, `fakeuvgst` included |
| Non-Windows emulation: `G_OS_WIN32` compiled out of the USB code and fakes, Linux and macOS presets | All 12 fakeuv programs pass, with the Linux and with the macOS preset |
| Linux cross-build (Debian 12, zig clang 21, libusb 1.0.26 headers) | 231/231 targets, 0 warnings; not run |
| MinGW cross-build (zig, MSYS2 UCRT64 packages, libusb 1.0.30) | 234/234 targets, 0 warnings: the 11 old `-Wformat` and `-Wstrict-prototypes` warnings are gone |
| MinGW programs run natively on Windows | 11 fakeuv programs, `uvlibusb`, `debug` and `dom` pass; `fakeuvgst` passed in one run and could not start in another (MSYS2 runtime DLLs missing) |
| Packaging | Before the last small fix round: a 111-file `aravis-0.9.3-g<commit>-msvc-x86_64.zip`; smoke test passes |
| 10-minute `aravissrc` soaks (Fake camera, 100 fps) | 60001 frames, 0 failures, 0 underruns; with 4 buffers and a synchronised sink, 60004 frames, 1 underrun, all buffers back |
| Stress: 1,315 `meson test` runs, up to `--repeat 20` and 32 processes | Two rare failures, both fixed since: `/fakeuvrawio/resync-1024x1024` (the trailer-callback flag race) and `/fakeuvrawio/failing-pipe-without-raw-io` (the fake's out-of-order failure injection, pre-existing). After the fixes, under 48 CPU burners with 32 runs in parallel: `resync-1024x1024` 300/300, `fakeuvrawio` and `fakeuvstream` 64/64 each, the unplug programs 192/192 |
| 13 throwaway reverts of fixes | Each made its test fail; `/fakeuvgst/renegotiation-held-stream` now also catches the element-side held-stream revert |
| `dumpbin /imports aravis-0.10-0.dll` | None of the four libusb 1.0.30-only functions imported |
| `gst-launch-1.0` with `ARV_DEBUG` on the `/MDd` build | Exits 0, no C runtime assertion |
| `gst-inspect-1.0 aravissrc` | `usb-maximum-transfer-size` shows `Range: 0 - 67108864` |

Not run anywhere:

- a real camera, real WinUSB or GigE traffic;
- Linux or macOS at run time, or an MSYS2 gcc build;
- valgrind, or any leak detector: MSVC's AddressSanitizer has no LeakSanitizer;
- a GStreamer other than 1.28, or a GLib older than 2.82, at run time;
- a build with introspection, documentation or the viewer. So the GIR of the two new functions, the `python` suite and the gi-docgen build never ran. That build includes the new `--content-dir` for `gst/` and the `[func@…]` links; the `gst/pipelines.md` rendering was only checked with gi-docgen and a minimal GIR. `arv-viewer --gentl` was only syntax-checked against a stub GTK header. Aravis-Linux will be the first build of all three, and Aravis-MinGW of the GIR and the viewer;
- GitHub Actions.

## What still needs real hardware or other platforms

The setup comes first. Each other entry is a Done-when of the plan that could not be met here.

- [ ] **Setup.**
  - Use a release build configured with `-Dlibusb:windows-hotplug=true`, or a zip that `package-windows.py` makes (the CI artifacts, once Actions run).
  - Also get a libusb 1.0.30 DLL without hotplug: a wrap build without that option, or a DLL from a non-hotplug folder of libusb's official `libusb-1.0.30.7z`.
  - Optionally, make an older `libusb-1.0.dll` (1.0.27 to 1.0.29, which can still claim the camera) the one Windows loads: next to the program, or first on `PATH` in a build tree. Aravis must still load. At the first async acquisition start, `ARV_DEBUG=device:2` must show `RAW_IO functions missing (libusb older than 1.0.30)`.
- [ ] **Item 5, WinUSB binding.** On a clean Windows 11 machine with a USB3 Vision camera, follow `usb.md` "Route 1":
  - read the function's compatible ID: `pnputil /enum-devices /connected /deviceids`;
  - copy `aravis-u3v.inf` and `aravis-u3v.cdf` from `share/aravis-0.10/winusb` to an empty folder;
  - create a certificate with `New-SelfSignedCertificate` and export it with `Export-Certificate`;
  - in the folder, run `makecat aravis-u3v.cdf`, then `signtool sign /fd SHA256 /sha1 <thumbprint> aravis-u3v.cat`;
  - from an elevated PowerShell, `Import-Certificate` the certificate into `Cert:\LocalMachine\Root` and `Cert:\LocalMachine\TrustedPublisher`;
  - run `powershell -ExecutionPolicy Bypass -File arv-winusb-setup.ps1 install -InfPath <folder>\aravis-u3v.inf`, with `-WhatIf` first;
  - check that the function node shows service `WINUSB`, and that `arv-tool-0.10` lists the camera;
  - run `arv-winusb-setup.ps1 uninstall` and check that the previous driver is back. Repeat on 24H2 and 25H2; try route 3 once.
- [ ] **Item 2, libusb log.** Open a camera with `ARV_DEBUG=device:2`, once with the hotplug DLL and once with the DLL without hotplug. Check the libusb version line and "Hotplug enabled" or "Hotplug unavailable".
- [ ] **Item 17.** With `ARV_DEBUG=device:1`, no "Failed to set auto kernel detach feature".
- [ ] **Item 6.** Put the camera on no driver, or on its vendor driver, and leave `ARV_DEBUG` unset.
  - `arv-tool-0.10` and `arv-camera-test-0.10` must show its IDs, strings, libusb error and hint. `arv-camera-test-0.10` prints them after "No camera found: ", from the error of `arv_camera_new ()`.
  - Time it while another process streams a camera.
- [ ] **Item 7.** Attach a second USB3 Vision camera and a WinUSB device that is not a camera. Open one camera with `LIBUSB_DEBUG=4`, or watch with a USB analyser.
  - The discovery before the first open still opens every USB3 Vision camera, by design.
  - Check two things in the log: the open itself touches only the target camera, and the other WinUSB device is never opened.
- [ ] **Item 3, unplug.** Pull the cable mid-stream with `arv-camera-test-0.10 --usb-mode=async`, then `--usb-mode=sync`, once with each libusb DLL. Expect `control-lost` within about 1 s, no CPU spin, and the stream thread gone.
  - `arv-camera-test` prints "Control lost", then its stream statistics, and exits. With `ARV_DEBUG=stream:2`, its two `[Stream::delete_buffers]` lines at exit must add up to its 5 buffers: every buffer came back.
  - It cannot show the plan's "replug and restart without restarting the process". Use `arv-viewer` (MinGW build), which goes back to its camera list, or a short program that opens the camera again.
  - Also unplug while idle, then read a feature from a program that keeps the camera open: `control-lost` must fire.
  - Note which libusb status WinUSB gives. In these runs and the item 1 run, check that no `control-lost` appears while the camera is plugged in.
- [ ] **Item 15.** The same through `gst-launch-1.0 aravissrc ...`: a bus error "Lost control of camera ..." naming the camera, and a `NULL` state change within 200 ms. On GigE, check `control-lost` after the heartbeat retries.
- [ ] **Item 1, RAW_IO.** With `ARV_UV_RAW_IO=1` and libusb 1.0.30, stream a 1440×1080 Mono8 camera for 10 minutes. Expect no failed buffer, and "RAW_IO on (maximum N bytes)" in the stream log; note N.
  - Also stream a frame of whole packets, such as 1920×1080 Mono8, with RAW_IO on. Watch for the warning "The camera ends its transfers of whole packets with a zero length packet ...".
  - On Linux, compare the frames with an unpatched build.
- [ ] **Item 20, benchmark.** Run `scripts/arv-usb-benchmark.py -n <camera> --controller "<host controller, hub>"` on a camera above 300 MiB/s, plus the Linux and vendor-viewer references. Fill the table in `usb.md`.
  - A run reported as "no stream statistics in the output" may be the stdout loss of `arv-camera-test` (see Known problems): run it again. The script retries only `--version`, and `usb.md` does not mention this yet.
- [ ] **Items 1 and 4, defaults.** From item 20's table, decide whether RAW_IO goes on by default. Then choose a Windows transfer size: 256 KiB or 1 MiB.
- [ ] **Item 18.** Run `arv-camera-test-0.10 --high-priority` and watch Process Explorer: `arv_uv_stream` and `arv_libusb` raised during acquisition, `arv_libusb` back to normal afterwards. Try `--realtime` without administrator rights: a warning, and HIGH class.
- [ ] **Item 19.** `set ARV_DEBUG=all:2` in `cmd.exe` on code page 437: letters. In Windows Terminal after `chcp 65001`: glyphs. In a classic console window on 65001: letters. Repeat on Windows 10 or Server 2022.
- [ ] **Item 8, GenTL.** Install a vendor SDK and its producer. Run `gst-launch-1.0 aravissrc interface=GenTL ...` for an hour, then 100 start/stop cycles. An unplug must give an error, and the vendor's viewer must still work afterwards. Also check with `pnputil /enum-devices /services` whether the vendor driver is already WinUSB.
- [ ] **Item 16.** `gst-device-monitor-1.0 Video/Source` lists a real USB and a real GigE camera, and each printed launch line plays. With `GST_ARAVIS_DEVICE_PROVIDER=0`, it lists no Aravis camera and has no discovery delay.
- [ ] **Item 21.** Store a GigE camera at packet size 8228, on a Windows NIC with MTU 1500 (jumbo frames off). It must stream complete frames with default settings. The `device` info log shows "Packet size lowered from 8228 to ... bytes, to fit in the interface MTU (1500 bytes)".
  - Jumbo frames: with an adapter at an MTU of about 9000, the log must show that MTU ("Interface MTU = ... bytes"). The packet size must stay above 1500, or existing jumbo-frame setups regress.
  - On Linux, try the packet socket (`CAP_NET_RAW`) with fragmented packets.
- [ ] **Item 13.** On Linux, run the `gstaravis` ownership checks under valgrind across 100 start/stop cycles; this needs GStreamer suppressions.
- [ ] **Items 9 to 11, CI.** Enable GitHub Actions on the fork.
  - GitHub offers `workflow_dispatch` only for workflows on the default branch, and `aravis-msvc-gstreamer.yml` exists only on `win-fixes`. So open a pull request from `win-fixes` into the fork's `main`: all five workflows run on pull requests to `main`.
  - A push to the fork's `main` also runs the gh-pages publishing step of Aravis-Linux.
  - Watch the documentation, GIR and viewer steps of the Linux and MinGW jobs: no build here had them.
  - Then publish a test release to exercise the `release-assets` jobs.
- [ ] **Item 11.** On a machine with only the official GStreamer runtime, unzip the release zip and run `gst-launch-1.0 aravissrc ! fakesink` with a camera.
- [ ] **Item 12, Linux and macOS CI.** Once Actions are enabled, run Aravis-Linux (with its valgrind step) and Aravis-macOS. The fakeuv tests passed here only in a non-Windows emulation.

## Not done and follow-ups

**External work.**

- MSYS2: bump `mingw-w64-aravis` (set `pkgver`, run `updpkgsums`, open a pull request against `msys2/MINGW-packages`).
- vcpkg: add a `gst-plugin` feature to the `aravis` port.
- conda-forge: create a feedstock.
- WrapDB: send the libusb hotplug path fix, then return the wrap to `patch_url`, `patch_hash` and `wrapdb_version`, and delete `subprojects/packagefiles/libusb`.
- Issue #1047: ask the reporter for the result with `-i 1500`, a 1500-byte packet size (`--gv-packet-size` of `arv-camera-test`). This is item 21 step 3.

**Decisions waiting for data.**

- Whether RAW_IO goes on by default (items 1 and 20).
- A Windows default transfer size (item 4 step 3).
- MMCSS for the stream and event threads, if item 20 shows dropped frames (item 18 step 3).
- Whether `gv-n-buffers=16` works on GitHub runners, so that the Windows jobs can drop `-Dgv-n-buffers=1`.

**Features left out.**

- Device provider hotplug monitoring (item 16 step 3) and `reconfigure_element`.
- GenTL: 32-bit producers, `n_underruns`, and register writes, data streams and events in Aravis's own producer.

**Follow-ups.**

- `arv-tool` prints the open errors only when no matching device is found. It could print them whenever there are some.
- The hint for `LIBUSB_ERROR_ACCESS` could mention `libusbK.dll`, for cameras on libusbK or libusb0.
- Give the other test programs the C runtime dialog settings of `tests/debug.c` (see Known problems).

**Main deviations from the plan.**

- Item 1: RAW_IO is opt-in (`ARV_UV_RAW_IO=1`) until item 20 has numbers.
- Item 2: by default, configuring against an old libusb builds the wrap instead of failing.
- Item 6: device strings are read when an error message is built, not at each discovery.
- Item 7: the first open by name in a process still discovers, and so opens, every USB3 Vision camera.
- Item 8: `aravissrc` copies GenTL frames instead of lending them.
- Item 9: the GStreamer kit installs in `C:\gstreamer\1.0\msvc_x86_64`, and the libusb wrap carries its own build files.
- Item 12: `fakeuvgst` is left out of the valgrind setup.
- Item 13: AddressSanitizer and live ArvBuffer counts stand in for valgrind.
- Item 16: rank marginal plus an environment opt-out, rather than the plan's fallback to `GST_RANK_NONE`; `create_element` does not open the camera.
- Item 19: a classic console window on code page 65001 gets letters, not glyphs.

**Known problems.**

- The workflows' time limits have not been checked on hosted runners.
- GigE: the MTU check runs before the camera reads back the packet size it accepted. A camera that rounds a written size up can end above the MTU without the warning.
- USB: the RAW_IO record starts as "off" when a device opens. libusb claims the stream interface through `WinUsb_GetAssociatedInterface`, and Microsoft documents the policy reset only for `WinUsb_Initialize`. If WinUSB keeps RAW_IO on across a close, the first acquisition without RAW_IO after a reopen would get no frames. This needs a camera to settle.
- The device provider now lists a camera id found on two interfaces twice, with identical display names; only `aravis.interface` and `aravis.protocol` differ.
- The `/gentl/discovery/missing-serial-number-producers` and `missing-serial-number-producer-separators` tests load producers copied into the system temp folder, so they fail where that folder is mounted `noexec`.
- With GLib 2.82 on Windows, a process that loads GIO sometimes loses its unflushed stdout at exit, in about 5% of runs. `arv-camera-test` can lose its final stream statistics that way.
- In MSVC debug (`/MDd`) builds, only `tests/debug.c` turns off the C runtime's dialogs. Another test that fails inside the debug C runtime may wait on a dialog until meson's timeout. Seen once: a deliberately failing `uvlibusb` with `ARV_DEBUG` in the static `/MDd` build; the cause was not investigated.

**Pre-existing limits found, not fixed.**

- USB sync mode: a final transfer padded to whole packets fails its frame; on Linux, a padded final transfer overflows.
- USB async mode: a buffer larger than the payload by one transfer loses the trailer.
- USB async mode: the BUFFER_DONE stream callback and the `new-buffer` signal run on the libusb event thread. A register access from them can deadlock against another thread's register access, as with real libusb.
- USB: two streams of one camera cannot acquire at the same time.
- Linux and macOS do not reset a halted stream endpoint without RAW_IO.
- `arv_camera_stop_acquisition ()` skips the device stop when `AcquisitionStop` fails after a loss.
- GenDC descriptor offsets are not bounds-checked.
- GigE: `auto_packet_size ()` redirects the channel without the stream-channel lock; `_loop ()` ignores `MSG_TRUNC`; the packet-socket ring setup does not fall back; the GVSP leader accessors don't check the data size.
- GigE: `arv_socket_set_recv_buffer_size ()` reports `strerror (errno)` on Windows; `arv_gv_interface_camera_locate ()` can dereference a NULL netmask.
- GigE: `n_missing_packets` counts the received trailer as missing when only the leader is contiguous.
- GenTL: in a folder, only files whose names end in lower-case `.cti` are loaded. `src/gentl/meson.build` lists a nonexistent `GenTL_v1_6.h` in an unused variable.
- `aravissrc`: `trigger` set after `camera-name` is not applied to an open camera; `gamma` set before the camera opens is never applied.
- `fakegv`'s `not-an-iface` step costs about 20 s in a DNS lookup.
- Docs: `introduction.md` has a broken GenICam link; `docs_url` in `aravis.toml.in` points to aravis-0.8.

**Before upstreaming.**

- This branch holds the work as two commits, made for this fork: the implementation and the audit fixes. For upstream, split it into pull requests by area (see the last point), with messages that describe each change rather than the plan's item numbers.
- Leave out the fork-only commits `6409832b` (`design/` in `.gitignore`) and `ef8ac13d` (an empty `changed4win.md`), and this file.
- Put the protocol-code statement above in the pull requests that touch `src/arvuv*.[ch]` or `src/arvgv*.[ch]`.
- Several files link to upstream pages that lack the new sections until upstream merges and republishes its documentation:
  - `aravis-main/usb.html#windows` in the zip README (`.github/scripts/package-windows.py`);
  - `aravis-main/usb.html` in the comment of `data/windows/aravis-u3v.inf` and in `data/windows/arv-winusb-setup.ps1`;
  - `aravis-main/usb.html#throughput-benchmark` in `scripts/arv-usb-benchmark.py`;
  - in `building.md`, the links to `blob/main/.github/workflows/aravis-msvc-gstreamer.yml`.
- Licences: the new test sources carry the Unlicense SPDX line. The device provider and `src/arvsystemprivate.h` are LGPL-2.1-or-later. `subprojects/packagefiles/libusb` holds WrapDB's MIT build files (`LICENSE.build`). The fake libusb copies no libusb code: its error strings and device-string copy are its own.
- Consider one pull request per area: USB3 Vision with the fake libusb, GigE Vision, GenTL, `aravissrc` with the provider, debug output, and build, CI and packaging.
