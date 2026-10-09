#!/usr/bin/env python3
"""Turn a 'meson install --destdir' tree of Aravis into a Windows binary zip.

Checks that the tree holds the Aravis DLL and import library, the headers,
arv-tool, arv-camera-test, the GStreamer plugin, a hotplug-enabled
libusb-1.0.dll built from subprojects/libusb.wrap and the WinUSB driver package
for USB3 Vision cameras, and that it holds no GLib
or GStreamer DLL: the plugin must use the GLib of the GStreamer installation
it is loaded into. Then adds the licence texts and a README, and writes
OUTPUT_DIR/aravis-VERSION[-gREVISION]-FLAVOUR.zip and its .sha256 file.

Usage: package-windows.py --builddir BUILD --destdir STAGE --flavour FLAVOUR
                          [--revision SHA] [--release] [--libusb {hotplug,any,none}]
                          [--output-dir DIR]
"""

import argparse
import fnmatch
import hashlib
import json
import ntpath
import os
import sys
import tempfile
import zipfile

SOURCE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
API_VERSION = '0.10'

REQUIRED = [
    ('Aravis DLL', ['bin/aravis-{0}-*.dll', 'bin/libaravis-{0}-*.dll']),
    ('Aravis import library', ['lib/aravis-{0}.lib', 'lib/libaravis-{0}.dll.a']),
    ('Aravis headers', ['include/aravis-{0}/arv.h']),
    ('arv-tool', ['bin/arv-tool-{0}.exe']),
    ('arv-camera-test', ['bin/arv-camera-test-{0}.exe']),
    ('GStreamer plugin', ['lib/gstreamer-1.0/gstaravis.{0}.dll', 'lib/gstreamer-1.0/libgstaravis.{0}.dll']),
]
LIBUSB = ('libusb DLL', ['bin/libusb-1.0.dll'])
# Installed by data/windows/meson.build with USB support
WINUSB = [
    ('WinUSB driver INF', ['share/aravis-{0}/winusb/aravis-u3v.inf']),
    ('WinUSB catalog CDF', ['share/aravis-{0}/winusb/aravis-u3v.cdf']),
    ('WinUSB setup script', ['share/aravis-{0}/winusb/arv-winusb-setup.ps1']),
]

# Libraries that must come from the GStreamer installation of the user
FORBIDDEN = ['*glib-2.0*.dll', '*gobject-2.0*.dll', '*gio-2.0*.dll', '*gmodule-2.0*.dll',
             '*gstreamer-1.0*.dll', '*gstbase-1.0*.dll', '*gstapp-1.0*.dll']

README = """Aravis {version} for Windows, {flavour}
{revision_line}
Aravis is a library for GigE Vision and USB3 Vision cameras, with a GStreamer
source element, aravissrc. https://github.com/AravisProject/aravis

Contents
  bin/                 {aravis_dll}, arv-tool-{api}, arv-camera-test-{api},
                       arv-fake-gv-camera-{api}, arv-test-{api}{libusb_contents}
  lib/                 import libraries and pkg-config files
  lib/gstreamer-1.0/   the GStreamer plugin, {plugin}
  include/             headers
{winusb_contents}  licenses/            licence texts

Requirements
{requirements}

Use
  set PATH=<this folder>\\bin;%PATH%
  set GST_PLUGIN_PATH=<this folder>\\lib\\gstreamer-1.0
  arv-tool-{api}
  gst-inspect-1.0 aravissrc
  gst-launch-1.0 aravissrc ! videoconvert ! autovideosink
{libusb_section}
Licences and sources
  Aravis is released under the LGPL-2.1-or-later (licenses/aravis).
  Source: https://github.com/AravisProject/aravis{source_ref}
{libusb_licence}"""

REQUIREMENTS = {
    'msvc': """  The official GStreamer {gst_series} runtime for MSVC x86_64,
  https://gstreamer.freedesktop.org/download/ (built against {gst_version}).
  It provides GLib, libxml2 and zlib, and its bin folder must be on PATH.
  GLib is deliberately not included here: Aravis and the plugin must use the
  GLib of the GStreamer installation that loads them.""",
    'gcc': """  An MSYS2 environment matching this build ({flavour}) with its glib2,
  libxml2, zlib and gstreamer packages installed, and its bin folder on PATH
  (built against GStreamer {gst_version}). These libraries are not included.""",
}

LIBUSB_SECTION = """
USB3 Vision
  libusb-1.0.dll is libusb {libusb_version}, built from subprojects/libusb.wrap{hotplug}.
  A camera can only be opened once its driver is WinUSB, the recommended one,
  or libusbK or libusb0, which libusb drives through libusbK.dll. To list the
  connected cameras with their drivers:
    powershell -ExecutionPolicy Bypass -File share\\aravis-{api}\\winusb\\arv-winusb-setup.ps1
  The same folder holds aravis-u3v.inf, a driver package that binds WinUSB to
  cameras once you have signed it. See
  https://aravisproject.github.io/aravis/aravis-main/usb.html#windows
"""

WINUSB_CONTENTS = """  share/aravis-{api}/winusb/
                       aravis-u3v.inf, a WinUSB driver package for USB3 Vision
                       cameras, and arv-winusb-setup.ps1
"""

LIBUSB_LICENCE = """  libusb is released under the LGPL-2.1-or-later (licenses/libusb).
  Source: https://github.com/libusb/libusb/releases/tag/v{libusb_version}
"""


def read_intro(builddir, name):
    with open(os.path.join(builddir, 'meson-info', 'intro-{}.json'.format(name)), encoding='utf-8') as f:
        return json.load(f)


def staged_root(destdir, prefix):
    # What meson's destdir_join does: drop the drive or root of the prefix
    parts = [p for p in ntpath.splitdrive(prefix)[1].replace('\\', '/').split('/') if p]
    return os.path.join(destdir, *parts)


def find(root, patterns):
    matches = []
    for pattern in patterns:
        directory, name = os.path.split(pattern.format(API_VERSION))
        path = os.path.join(root, *directory.split('/'))
        if os.path.isdir(path):
            matches += [directory + '/' + f for f in sorted(os.listdir(path)) if fnmatch.fnmatch(f, name)]
    return matches


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--builddir', required=True, help='meson build directory')
    parser.add_argument('--destdir', required=True, help="directory given to 'meson install --destdir'")
    parser.add_argument('--flavour', required=True, help='package flavour, for example msvc-x86_64')
    parser.add_argument('--revision', help='git commit of the build')
    parser.add_argument('--release', action='store_true', help='leave the revision out of the file name')
    parser.add_argument('--libusb', choices=['hotplug', 'any', 'none'], default='hotplug',
                        help='require libusb-1.0.dll built from subprojects/libusb.wrap with hotplug support '
                             '(default), with or without it, or do not ship libusb')
    parser.add_argument('--output-dir', default='.', help='where to write the zip')
    args = parser.parse_args()

    project = read_intro(args.builddir, 'projectinfo')
    options = {o['name']: o['value'] for o in read_intro(args.builddir, 'buildoptions')}
    dependencies = {d['name']: d.get('version') for d in read_intro(args.builddir, 'dependencies')}
    compilers = read_intro(args.builddir, 'compilers')
    compiler_id = compilers.get('host', {}).get('c', {}).get('id', 'msvc')

    version = project['version']
    libusb_version = next((s['version'] for s in project.get('subprojects', []) if s['name'] == 'libusb'), None)
    hotplug = options.get('libusb:windows-hotplug', False)
    gst_version = dependencies.get('gstreamer-base-1.0') or 'unknown'
    root = staged_root(os.path.abspath(args.destdir), options['prefix'])

    errors = []
    found = {}
    required = REQUIRED + ([LIBUSB] + WINUSB if args.libusb != 'none' else [])
    for what, patterns in required:
        matches = find(root, patterns)
        if matches:
            found[what] = matches[0]
        else:
            errors.append('missing {}: none of {}'.format(what, ', '.join(p.format(API_VERSION) for p in patterns)))
    if args.libusb != 'none' and libusb_version is None:
        errors.append('libusb was not built from subprojects/libusb.wrap, so its DLL is not installed')
    if args.libusb == 'hotplug' and not hotplug:
        errors.append('libusb was built without -Dlibusb:windows-hotplug=true')
    for directory, _, files in os.walk(root):
        for name in files:
            if any(fnmatch.fnmatch(name.lower(), pattern) for pattern in FORBIDDEN):
                errors.append('{} must not be shipped'.format(os.path.relpath(os.path.join(directory, name), root)))
    if errors:
        for error in errors:
            print('error: ' + error)
        return 1

    revision = args.revision or ''
    name = 'aravis-{}{}-{}'.format(version, '' if args.release or not revision else '-g' + revision[:8],
                                   args.flavour)
    with_libusb = args.libusb != 'none'
    readme = README.format(
        version=version, flavour=args.flavour, api=API_VERSION,
        revision_line='Built from commit {}\n'.format(revision) if revision else '',
        aravis_dll=os.path.basename(found['Aravis DLL']),
        plugin=os.path.basename(found['GStreamer plugin']),
        libusb_contents=',\n                       libusb-1.0.dll' if with_libusb else '',
        winusb_contents=WINUSB_CONTENTS.format(api=API_VERSION) if with_libusb else '',
        requirements=REQUIREMENTS['msvc' if compiler_id == 'msvc' else 'gcc'].format(
            gst_series='.'.join(gst_version.split('.')[:2]), gst_version=gst_version, flavour=args.flavour),
        libusb_section=LIBUSB_SECTION.format(
            libusb_version=libusb_version, api=API_VERSION,
            hotplug=',\n  with Windows hotplug support (-Dlibusb:windows-hotplug=true)' if hotplug else '') if with_libusb else '',
        source_ref='/tree/' + revision if revision else '',
        libusb_licence=LIBUSB_LICENCE.format(libusb_version=libusb_version) if with_libusb else '')

    os.makedirs(args.output_dir, exist_ok=True)
    zip_path = os.path.join(args.output_dir, name + '.zip')
    with tempfile.TemporaryDirectory(prefix='arv-package-') as extra:
        licences = [(os.path.join(SOURCE_DIR, 'COPYING'), 'licenses/aravis/COPYING')]
        if with_libusb:
            libusb_source = os.path.join(SOURCE_DIR, 'subprojects', 'libusb-' + libusb_version)
            licences += [(os.path.join(libusb_source, f), 'licenses/libusb/' + f) for f in ('COPYING', 'AUTHORS')]
        readme_path = os.path.join(extra, 'README.txt')
        with open(readme_path, 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(readme)

        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
            for directory, dirs, files in os.walk(root):
                dirs.sort()
                for file_name in sorted(files):
                    path = os.path.join(directory, file_name)
                    z.write(path, name + '/' + os.path.relpath(path, root).replace('\\', '/'))
            for source, target in licences:
                z.write(source, name + '/' + target)
            z.write(readme_path, name + '/README.txt')

    digest = hashlib.sha256()
    with open(zip_path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    with open(zip_path + '.sha256', 'w', encoding='utf-8', newline='\n') as f:
        f.write('{}  {}\n'.format(digest.hexdigest(), os.path.basename(zip_path)))

    with zipfile.ZipFile(zip_path) as z:
        entries = z.namelist()
    print('Wrote {} ({} files, sha256 {})'.format(zip_path, len(entries), digest.hexdigest()))
    for what in found:
        print('  {:22} {}'.format(what, found[what]))
    print('  {:22} {}'.format('libusb', '{}{}'.format(libusb_version, ', hotplug' if hotplug else '')
                              if with_libusb else 'not included'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
