# GenTL producer: to do

The producer exports every function of the GenTL API declared by `src/arvgentlprivate.h`, so that GenTL consumers,
Aravis included, can load it. The functions without implementation return `GC_ERR_NOT_IMPLEMENTED`, and
`GCGetLastError` names them. The `gentl` test suite checks this (`/gentl/producer/*` in `tests/gentl.c`).

What works:

* System module: information (`GCGetInfo`, `TLGetInfo`), the interface list, the system port description.
* Interfaces: every Aravis interface, the disabled ones included, except `GenTL`, whose discovery would load the
  producers of the GenTL path, this one included. Their information is available before they are opened.
* Interface module: device list, device information, device opening through its interface.
* Device module: remote device port, device id, data stream count and ids, closing.
* Ports: reads (`GCReadPort`), GenICam description URL, port name.
* The producer can be unloaded and loaded again by the same process.

To do:

* Register writes: `GCWritePort`, `GCWritePortStacked`, `GCReadPortStacked`. The remote device can only be read.
* Data stream module: `DevOpenDataStream` and every `DS*` function. No acquisition.
* Events: `GCRegisterEvent`, `GCUnregisterEvent` and every `Event*` function.
* `GCGetPortURL`, `IFGetParentTL`, `DevGetParentIF`, the `DevGetInfo` commands other than `DEVICE_INFO_ID`,
  `DEVICE_INFO_TIMESTAMP_FREQUENCY` in `IFGetDeviceInfo`, the `STRINGLIST` and `BUFFER` information types.
* The system module information is made of placeholders ("Aravis-Vendor", "Aravis-Model", ...), with a GenTL
  version 0.1. The system and interface port descriptions are minimal.
* `TLOpenInterface` doesn't return `GC_ERR_RESOURCE_IN_USE` for an interface already open: each call takes a
  reference on the Aravis interface instance used as handle, released by `IFClose`. The handles stay valid after
  `arv_shutdown()`, which only releases the instances of the process.
* 32-bit x86: the GenTL functions are expected to use the `stdcall` calling convention, which the declarations of
  `src/arvgentlprivate.h` don't specify.
* We should not use ArvCamera API but only ArvDevice
* There is no reason to expect a GigEVision device: only the stream channel count is asked to GigE Vision devices,
  the other ones have one data stream.
