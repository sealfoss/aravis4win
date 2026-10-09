Title: Thread Safety

# Thread Safety

Aravis is not thread safe, which means one can not use the same object
simultaneously from different threads, without using mutexes. But it is
perfectly fine to use different aravis objects in different threads, with the
exception of the [class@Aravis.Interface] instances.

The [class@Aravis.Interface] instances are shared by the whole process, and the
device list functions use them under a lock of their own: different threads
can call [func@Aravis.update_device_list], [func@Aravis.get_n_devices], the
functions querying the device list like [func@Aravis.get_device_id] and
[func@Aravis.get_device_interface_id], [func@Aravis.open_device],
[func@Aravis.open_device_on_interface], and the functions selecting the
interfaces of the list, like [func@Aravis.enable_interface], at the same time.
The strings the device list queries return remain valid after the following
updates of the list: they are kept until the process exits, as
[func@Aravis.update_device_list] explains. Each query takes the lock on its own,
so another thread may change the list between two queries:
[func@Aravis.get_device_list_generation] tells whether it did, to read several
devices, or several fields of a device, from one state of the list.
The methods of the interfaces returned by [func@Aravis.get_interface] and
[func@Aravis.get_interface_by_id], like
[method@Aravis.Interface.update_device_list] and
[method@Aravis.Interface.open_device], don't take this lock: they must not be
used while another thread, for example the Aravis GStreamer plugin, may use the
device list. [func@Aravis.open_device_on_interface] opens a device through a
single interface safely, enabled or not.

The GenICam descriptions of devices that different threads create at the same
time, for example several cameras opened directly with [ctor@Aravis.GvDevice.new]
or [ctor@Aravis.UvDevice.new], are parsed independently: the parser keeps no
state between documents.

A possible trap is that glib signal callbacks are called from the thread that
emitted the corresponding signal. For example, the
[signal@Aravis.Stream::new-buffer] callback is emitted from the stream packet
receiving thread, which is an internal thread created by
[class@Aravis.GvStream]. It is not safe to use [class@Aravis.Device] and some
[class@Aravis.Stream] functions from this callback without using mutexes. The
exceptions are [method@Aravis.Stream.push_buffer],
[method@Aravis.Stream.pop_buffer], [method@Aravis.Stream.try_pop_buffer] and
[method@Aravis.Stream.timeout_pop_buffer].
