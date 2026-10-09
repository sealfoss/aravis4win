Title: Installation and Debug

# Installing Aravis

Aravis uses the meson build system ( http://mesonbuild.com/ ). After you have
downloaded the latest release from
[https://github.com/AravisProject/aravis/releases](https://github.com/AravisProject/aravis/releases),
you can build and install Aravis like [any other meson
project](http://mesonbuild.com/Quick-guide.html#compiling-a-meson-project):

```sh
meson setup build
cd build
ninja
ninja install
```

The build can be configured at any time using `meson configure` in the build
directory. `meson configure` invoked without any other argument will show the
configuration options.

On some platforms (like Ubuntu), you may have to configure the dynamic linker
(ld) to let it know where the aravis libraries are installed, and run ldconfig
as root in order to update ld cache.

```sh
sudo ldconfig
```

## Release versions

Please use the released versions of Aravis and not a git checkout, unless you
want to work on Aravis itself or test a commit.

The current version naming follows a major.minor.micro scheme. Odd minor values
indicates development releases, and even minor values stable releases. Prior to
1.0.0, stable releases with different minor are not compatible, and are parallel
installable, with executables suffixed with `-major.minor`.

## Install dependencies on Ubuntu 20.04

Prior to running `meson` and `ninja`, dependencies can be installed using the
following(tested on Ubuntu 20.04):

```sh
sudo apt install libxml2-dev libglib2.0-dev cmake libusb-1.0-0-dev gobject-introspection \
                 libgtk-3-dev gtk-doc-tools  xsltproc libgstreamer1.0-dev \
                 libgstreamer-plugins-base1.0-dev libgstreamer-plugins-good1.0-dev \
                 libgirepository1.0-dev gettext
```

## Install dependencies on Fedora 34/35
Due to differences in the deb and rpm package ecosystems dependencies can be
installed on  Fedora (tested on 34 and 35) with:

```sh
sudo dnf install libxml2-devel glib2-devel cmake libusb1-devel gobject-introspection \
                 gobject-introspection-devel gstreamer1-plugins-base-devel gtk3-devel \
                 gtk-doc libxslt gstreamer1-devel gstreamer1-plugins-good python3-gobject \
                 g++ meson gettext
```


## NixOS development shell

Use the following command to enter a development shell with
all required dependencies:

```sh
nix-shell -p aravis.buildInputs aravis.nativeBuildInputs
```

## Building on macOS

Using the GNU build system on macOS is not directly supported, but can be
mimicked by augmenting the install procedure above with some environment
settings (tested on macOS Catalina):

```sh
brew install gettext intltool gtk-doc libxml2 meson libusb
meson setup build
ninja -C build
```

If you want to be able to build the viewer, you have to install some additional
packages:

```sh
brew install gtk+3 gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad libnotify gnome-icon-theme
meson configure -Dviewer=enabled build
```

## Building on Windows

[MSYS2](https://msys2.org) provides native [Aravis
packages](https://packages.msys2.org/base/mingw-w64-aravis).  The package
includes the DLL, headers and utilities (including the viewer).

The
[Aravis-MSVC-GStreamer](https://github.com/AravisProject/aravis/blob/main/.github/workflows/aravis-msvc-gstreamer.yml)
and [Aravis-MinGW](https://github.com/AravisProject/aravis/blob/main/.github/workflows/aravis-mingw.yml)
CI workflows also build zip packages, available as artifacts of each run and
attached to each GitHub release when it is published.
`aravis-<version>-msvc-x86_64.zip` holds the Aravis DLL, import library and
headers, `arv-tool`, `arv-camera-test`, the GStreamer plugin and a
`libusb-1.0.dll` with hotplug support, for use with the official GStreamer
runtime for MSVC x86_64, which provides GLib. `aravis-<version>-msys2-ucrt64.zip`
is the same for an MSYS2 UCRT64 environment. In the artifacts of the runs that
are not releases, `-g` and the commit of the build follow the version in these
names. Each zip comes with a `.sha256` file.

Aravis opens a USB3 Vision camera only once its driver is one that libusb can
use: WinUSB, the recommended one, or libusbK or libusb0 with `libusbK.dll`.
Builds for Windows install `share/aravis-0.10/winusb`, which holds a driver
package that binds WinUSB to cameras and a PowerShell script that lists the
cameras with their drivers: see [USB on Windows](usb.html#windows).

### libusb version

On Windows, USB3 Vision cameras need libusb 1.0.27 or later: older versions can
only claim the first interface of a WinUSB device, so the data interface of a
camera cannot be claimed. libusb 1.0.30 adds the WinUSB RAW_IO functions and a
Windows hotplug backend, which is a build option of libusb.

On Windows, meson ignores an installed libusb older than 1.0.27, and builds
libusb 1.0.30 from `subprojects/libusb.wrap` instead, as it does when no libusb
is installed. An installed libusb 1.0.27 to 1.0.29 is used, with a warning. If
meson cannot use the wrap, for example with `--wrap-mode=nofallback`, or with
`--wrap-mode=nodownload` or no network access while the libusb tarball is not
in `subprojects/packagecache`, then `-Dusb=auto`, the default, disables USB
support with a message, and `-Dusb=enabled` makes the configuration fail.

`--force-fallback-for=libusb-1.0` makes meson build libusb from the wrap even
when a suitable libusb is installed, and `-Dlibusb:windows-hotplug=true`
selects its hotplug flavour:

```sh
meson setup --force-fallback-for=libusb-1.0 -Dlibusb:windows-hotplug=true build
```

The wrap takes the sources of libusb from its release page, and its meson build
files from `subprojects/packagefiles/libusb`: those of
[WrapDB](https://mesonbuild.com/Wrapdb-projects.html), with a fix that makes the
hotplug flavour build. meson copies them over the sources when it extracts
libusb into `subprojects/libusb-1.0.30`, which needs no other program. If that
directory was extracted by an older checkout of Aravis, delete it so that meson
extracts it again with the fix.

[libusb with hotplug support](usb.html#libusb-with-hotplug-support) says why
hotplug support matters, and where to find a `libusb-1.0.dll` that has it.

### MinGW (MSYS2)

To build Aravis by yourself, install MSYS2 and enter the mingw64 shell. Refer to
the [mingw CI configuration
file](https://github.com/AravisProject/aravis/blob/main/.github/workflows/aravis-mingw.yml)
for list of dependencies (such as `mingw-w64-x86_64-libxml2` and so on) which
must be installed prior to building via `pacman -S ...`. The build process
itself is the same as on other platforms (meson/ninja).

### MSVC with the official GStreamer package

Aravis and its GStreamer plugin can be built with Microsoft Visual C++ (MSVC)
against the [official GStreamer package](https://gstreamer.freedesktop.org/download/)
for MSVC x86_64, which provides GLib, libxml2, zlib and pkg-config. This is what
the [Aravis-MSVC-GStreamer CI
workflow](https://github.com/AravisProject/aravis/blob/main/.github/workflows/aravis-msvc-gstreamer.yml)
does. The plugin must use the GLib of the GStreamer installation that loads it,
so do not mix in a GLib from another package manager.

Install the GStreamer package with its development files (the "Runtime and
development headers" installation type) in a path without spaces: its
pkg-config files compute their prefix from their location, and pkg-config
escapes the spaces of `C:\Program Files`, which breaks the build. From a command
line:

```sh
gstreamer-1.0-msvc-x86_64-1.28.7.exe /VERYSILENT /ALLUSERS /TYPE=devel /DIR=C:\gstreamer\1.0\msvc_x86_64
```

Install Python 3, then meson and ninja with `pip install meson ninja`, and write
a meson native file for the package. Its `glib-mkenums` and `glib-genmarshal`
scripts name the Python interpreter of the machine that built the package, so
meson cannot run them as they are:

```sh
python .github/scripts/gstreamer-native-file.py C:\gstreamer\1.0\msvc_x86_64 gstreamer-msvc.ini
```

Then build and test. `--vsenv` sets up the MSVC environment, unless meson runs
in a Visual Studio developer shell:

```sh
meson setup build --vsenv --native-file gstreamer-msvc.ini --buildtype=release -Dgst-plugin=enabled -Dusb=enabled -Dviewer=disabled -Dintrospection=disabled
meson compile -C build
meson test -C build
```

To run the plugin from the build directory, put `build\src`,
`build\subprojects\libusb-1.0.30` and `C:\gstreamer\1.0\msvc_x86_64\bin` in
`PATH`, set `GST_PLUGIN_PATH` to `build\gst`, and check that
`gst-inspect-1.0 aravissrc` finds the element.

### MSVC with Conan

The [Conan 2 CI configuration
file](https://github.com/AravisProject/aravis/blob/main/.github/workflows/aravis-msvc-conan2.yml)
builds the library and the utilities with MSVC and dependencies from
ConanCenter, without the GStreamer plugin, as ConanCenter has no
gst-plugins-base recipe for Conan 2. ConanCenter stops at libusb 1.0.29, so
libusb comes from `subprojects/libusb.wrap` instead.

### Cross-compilation for Windows

Aravis for Windows can be also cross-compiled on Linux (and used in Wine) using
[crossroad](https://pypi.org/project/crossroad/), provided that cross-compiler
and native build tools (`sudo apt install gcc-mingw-w64-x86-64 meson
ninja=build` on Debian/Ubuntu) are installed:

```sh
# note: use the git version, not the one from pypi
pip3 install --user git+git://git.tuxfamily.org/gitroot/crossroad/crossroad.git
# create cross-compilation environment with architecture "w64" called "aravis"
crossroad w64 aravis
# install packages required for compilation; crossroad adds the mingw-w64_x86_64- prefix automatically
crossroad install libnotify gstreamer gst-plugins-good gst-plugins-bad gst-plugins-bad gobject-introspection libusb gtk3 libxml2 zlib
# clone aravis sources
git clone https://github.com/AravisProject/aravis
cd aravis
# configure, crossroad adjusts meson for cross-compilation; build directory is created
crossroad meson setup build
# compile and install
ninja -C build install
```

# Debugging Aravis

The `ARV_DEBUG` environment variable can be set to a comma separated list of
debugging categories, which will make Aravis print out different types of
debugging informations to the console. A debug level can also be specified,
using a number from 0 (none) to 4 (trace) separated from the category name by a
colon. For example, the following command before running an Aravis based
application will make Aravis print out all stream and device related
informations:

```
export ARV_DEBUG=stream:3,device:3
```

Available debug level and categories are:

```
Debug categories:
interface      : Device lookup for each supported protocol
device         : Device control
stream         : Video stream management
stream-thread  : Video stream thread (likely high volume output)
cp             : Control protocol packets
sp             : Stream protocol packets (likely high volume output)
genicam        : Genicam specialized DOM elements
policies       : Genicam runtime configurable policies
chunk          : Chunk data code
dom            : Genicam DOM document
evaluator      : Expression evaluator
viewer         : Simple viewer application
misc           : Miscellaneous code
gentl          : GenTL interface
all            : Everything

Debug levels:
0: none
1: warning
2: info
3: debug
4: trace
```

The messages go to the standard error output. Each one starts with a symbol for
its level: 🆆, 🅸, 🅳 or 🆃 for warning, info, debug and trace. On Windows,
where these symbols often cannot be displayed, the letters `W`, `I`, `D` and
`T` replace them as described below. Aravis chooses once, on its first message,
and never changes the console code page. Elsewhere, the symbols are always
used.

When the standard error output is a console, the symbols are used only if all
of these hold:

- The console uses UTF-8 (`chcp 65001`). A legacy code page, such as 437 or
  850, would garble them.
- The locale of the C runtime used by Aravis is the default "C" locale, or uses
  UTF-8. Programs that call `setlocale (LC_ALL, "")` with a legacy code page,
  such as `gst-launch-1.0` and GTK applications, get the letters: the C runtime
  converts what it writes to a console from that code page, which would garble
  the symbols.
- A terminal emulator displays the console through a pseudo console (ConPTY),
  as Windows Terminal and the terminal of Visual Studio Code do. The window of
  the classic console host cannot show the symbols, which lie outside the
  Basic Multilingual Plane, even with UTF-8: it holds a replacement character
  for each. Aravis recognizes a pseudo console by its hidden window, and the
  classic console host by its window. When the console has neither, because it
  has no window or one of another kind, the `WT_SESSION` variable, which
  Windows Terminal sets, or the `TERM_PROGRAM` variable of other terminal
  emulators stands for a terminal. In a window of the classic console host,
  these variables do not count: a program started from a terminal in a new
  console window inherits them.

Output redirected to a file or a pipe keeps the symbols when there is no
console or when the console uses UTF-8. Otherwise, it follows the character set
GLib reports for the console: the one named after the dot in `LANG`, such as
`UTF-8` in `en_US.UTF-8` as MSYS2 sets it, else the console code page.

Aravis formats each message with GLib, then writes it in one piece with the C
runtime it is linked with, so that the lines of messages from different threads
do not mix. With MSVC, this C runtime may not be the one of GLib: a debug build
of Aravis, meson's default build type, uses the debug C runtime, while the GLib
of the official GStreamer package uses the release one. Debug output works in
that combination too, as in `gst-launch-1.0` with a debug build of the plugin.
