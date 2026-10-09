Title: Ethernet Devices

# Ethernet Device Performance

## Network Layout

If you face a lots of lost packets, the first thing to try is to directly connect
the device to your machine, using a wired connection. If it is possible, use a
dedicated port.

Most of the cameras are able to saturate their gigabit link, so any additional
traffic may lead to packet lost. If your machine ethernet port has a lower
bandwidth than the camera, make sure the stream you are requesting will not
exceed its bandwidth.

A device may answer the discovery through several network interfaces of the
host, for example when they share the same network, or when the device runs on
the host itself, like the fake camera. Aravis then reaches it through the
interface whose subnet contains the device address, if there is one, as the
host normally sends the device packets through it.

## Socket Buffer Size

Under heavy CPU load, the input socket buffer size may be increased in order to avoid
lost packets because of a full buffer. You can try the `--auto` parameter of
`arv-camera-test`.

In you application, this can be tweaked using `g_object_set()` on the stream
object. For example, this code, to be called before
[method@Aravis.Camera.start_acquisition], will enable the auto buffer size mode:

```c
g_object_set (stream,
	      "socket-buffer", ARV_GV_STREAM_SOCKET_BUFFER_AUTO,
	      "socket-buffer-size", 0,
	      NULL);
```

## Receiving Thread Priority

It is possible to increase the receiving thread priority. You can experiment
with thread priority using the `--realtime` or `--high-priority` options of
`arv-camera-test`.

In your code, stream thread priority can be changed in the stream buffer
callback:

```c
static void
stream_cb (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer)
{
        if (type == ARV_STREAM_CALLBACK_TYPE_INIT) {
		if (!arv_make_thread_realtime (10))
			printf ("Failed to make stream thread realtime\n");
	}
}
```
or:

```c
static void
stream_cb (void *user_data, ArvStreamCallbackType type, ArvBuffer *buffer)
{
        if (type == ARV_STREAM_CALLBACK_TYPE_INIT) {
		if (!arv_make_thread_high_priority (-10))
			printf ("Failed to make stream thread high priority\n");
	}
}
```

## Stream Packet Size

One way to increase streaming performance and lower the CPU use is to increase
the stream packet size. [method@Aravis.Camera.gv_set_packet_size] and
[method@Aravis.Camera.gv_auto_packet_size] allow you to change this parameter.
By default, the network adapter of your machine will probably not let you
receive packet bigger than 1500 bytes, which is the default Maximum Transfer
Unit (MTU). It means if you want to use big packets, you also have to increase
the network adapter MTU to a greater walue (8192 bytes being a recommended
value). The exact procedure to change the MTU depends on the distribution you
are using. Please refer to the administrator manual of your distribution.

On Fedora, MTU can be changed using the network setting panel. Edit the wired
network settings. MTU parameter is in the Identity page.

On Debian / Ubuntu you can add a line "mtu 8192" to /etc/network/interfaces
under the correct device.

```
iface ethusb0 inet static
	address 192.168.45.1
	netmask 255.255.255.0
	broadcast 192.168.45.255
	mtu 8192
```

Please note if your device is not connected directly to the machine, you may
also have to tweak the active devices on your network.

The packet size counts the whole IP datagram, so a packet larger than the MTU of
the interface the device is reached through can't be received without
fragmentation. When the operating system reports this MTU, the automatic packet
size adjustment (done at stream creation, see
[method@Aravis.Camera.gv_set_packet_size_adjustment], or by
[method@Aravis.Camera.gv_auto_packet_size]) selects a packet size not larger
than the MTU, and lowers a larger current packet size to the largest valid one
fitting in the MTU, even for devices that can't send test packets. If even the
smallest packet size of the device is larger than the MTU, no packet size fits:
the adjustment uses the smallest one, and logs a warning naming the MTU and this
size (`--debug=device:1`, or `ARV_DEBUG=device:1`, shows it). The data packets
are then likely to be dropped: increase the MTU of the interface. The `never`
adjustment mode (`--packet-size-adjustment=never` for `arv-camera-test`) keeps
the packet size unchanged.

When frames fail with only their leader and trailer packets received, which is
the usual symptom of data packets larger than the MTU of the host interface or
of the network, the stream counts them in its `n_leader_trailer_only_failures`
statistic and logs a warning naming the packet size and the interface MTU. The
last data packet of a frame may be smaller than the others, and get through:
frames with none of their full size data packets received are counted too. Use
`--debug=stream:1`, or `ARV_DEBUG=stream:1`, to see the warning.

## Packet Socket Support

Aravis can use packet sockets for the video receiving thread. But this mode
requires extended capabilities. If you want to allow your application to use
packet socket, you must set the `cap_net_raw` capability using `setcap`. For
example, the following command gives this capability to the Aravis viewer:

```
sudo setcap cap_net_raw+ep arv-viewer
```

A packet socket sees the IP packets before their reassembly. A GVSP packet
larger than the MTU of the network path arrives in several IP fragments, which
the packet socket can't put back together: such a packet is ignored, and counts
as missing in its frame. A warning of the same `stream` debug category then
suggests to lower the packet size, or to disable the packet socket, with
[method@Aravis.Camera.gv_set_stream_options] (`--no-packet-socket` for
`arv-camera-test`).

The traffic of a device running on the host itself, like the fake camera, goes
through the loopback interface. The packet socket is not used for such a device
when it is reached through the address of another interface.

# Legacy endianess mechanism

Some GigEVision devices incorrectly report a Genicam schema version greater or
equal to 1.1, while implementing the legacy behavior for register access. We
maintain a list in
[arvgcport.c](https://github.com/AravisProject/aravis/blob/6f1d65608dcecef2326ae2b3a542f5f59771ea32/src/arvgcport.c#L44)
which allows to force the use of the legacy endianness mechanism.

The documentation about the legacy endianness mechanism is in the 3.1 appendix
('Endianess of GigE Vision Cameras') of the GenICam Standard.

There is a chance this part of Aravis is due to a misunderstanding of how a
GigEVision device is supposed to behave (Remember we can not use the GigEVision
specification documentation).  But until now, there was no evidence in the issue
reports it is the case. If you think this should be implemented differently,
don't hesitate to explain your thoughts on Aravis issue report system, or even
better, to open a pull request. The related Aravis issues are available here:
[https://github.com/AravisProject/aravis/labels/5. Genicam 1.0 legacy
mode](https://github.com/AravisProject/aravis/labels/5.%20Genicam%201.0%20legacy%20mode).

Meanwhile, if you want to add a device to the exception list, please open an
issue on github, giving the vendor name and model name as found in the Genicam
data of your device, using the `genicam` command of `arv-tool`. They are stored
in the ModelName and VendorName attributes of the RegisterDescription element.
