#!/usr/bin/env python3
# SPDX-License-Identifier:Unlicense
"""Checks subprojects/libusb.wrap and the libusb build files it uses.

Usage: libusbwrap.py SOURCE_DIR

The wrap must let meson set up libusb without any external program, as in an
MSYS2 environment without 'patch' or 'git': its build files come from a
patch_directory under subprojects/packagefiles, not from diff_files, which
also need meson 0.63. Every source file that these build files name, such as
libusb/os/windows_hotplug.c, must exist in the libusb sources, read from the
extracted subproject or from the tarball in subprojects/packagecache; that part
is skipped when meson has fetched neither.
"""

import configparser
import os
import posixpath
import re
import sys
import tarfile

failures = []

# A meson string literal, with the keyword argument that it is the value of
STRING = re.compile(r"(?:(\w+)\s*:\s*)?'([^'\n]*)'")
# Not headers: the build files also name system headers, to look for them
SOURCE_EXTENSIONS = ('.c', '.cc', '.cpp', '.def')


def check(condition, message):
    print(('ok: ' if condition else 'FAIL: ') + message)
    if not condition:
        failures.append(message)
    return condition


def named_files(patch_dir):
    """The source files that the build files name, relative to the top of libusb."""
    names = {}
    for directory, _, files in os.walk(patch_dir):
        if 'meson.build' not in files:
            continue
        relative = os.path.relpath(directory, patch_dir).replace(os.sep, '/')
        if relative == '.':
            relative = ''
        with open(os.path.join(directory, 'meson.build'), encoding='utf-8') as f:
            for line in f:
                code = line.split('#', 1)[0]
                for keyword, value in STRING.findall(code):
                    # 'output:' names a generated file, '@0@' a computed one
                    if keyword == 'output' or '@' in value or not value.endswith(SOURCE_EXTENSIONS):
                        continue
                    path = posixpath.normpath(posixpath.join(relative, value))
                    names[path] = posixpath.join(relative, 'meson.build')
    return names


def source_files(subprojects, wrap):
    """The files of the libusb sources, or None when meson has fetched none."""
    extracted = os.path.join(subprojects, wrap.get('directory', ''))
    if wrap.get('directory') and os.path.isfile(os.path.join(extracted, 'libusb', 'core.c')):
        print('libusb sources: ' + extracted)
        files = set()
        for directory, _, names in os.walk(extracted):
            relative = os.path.relpath(directory, extracted).replace(os.sep, '/')
            files |= {posixpath.normpath(posixpath.join(relative, n)) for n in names}
        return files
    tarball = os.path.join(subprojects, 'packagecache', wrap.get('source_filename', ''))
    if wrap.get('source_filename') and os.path.isfile(tarball):
        print('libusb sources: ' + tarball)
        with tarfile.open(tarball) as archive:
            return {m.name.split('/', 1)[1] for m in archive.getmembers() if m.isfile() and '/' in m.name}
    return None


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    subprojects = os.path.join(os.path.abspath(sys.argv[1]), 'subprojects')
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(os.path.join(subprojects, 'libusb.wrap'), encoding='utf-8')
    if not check(parser.has_section('wrap-file'), 'libusb.wrap is a wrap-file'):
        return 1
    wrap = parser['wrap-file']

    check('diff_files' not in wrap,
          'libusb.wrap has no diff_files, which need a patch or git program and meson 0.63')
    check(not ('patch_directory' in wrap and ('patch_filename' in wrap or 'patch_url' in wrap)),
          'libusb.wrap does not mix a patch_directory with a patch archive, which meson refuses')
    patch_dir = os.path.join(subprojects, 'packagefiles', wrap.get('patch_directory', ''))
    if not check('patch_directory' in wrap and os.path.isfile(os.path.join(patch_dir, 'meson.build')),
                 'the build files of libusb are in subprojects/packagefiles/{}'.format(
                     wrap.get('patch_directory', '<patch_directory>'))):
        return 1
    check(os.path.isfile(os.path.join(patch_dir, 'LICENSE.build')), 'the licence of the build files comes with them')

    names = named_files(patch_dir)
    check('libusb/os/windows_hotplug.c' in names, 'the build files name the Windows hotplug backend')
    sources = source_files(subprojects, wrap)
    if sources is None:
        print('skipped: no libusb sources in subprojects to check the build files against')
    else:
        for path, build_file in sorted(names.items()):
            check(path in sources, '{} names {}, which libusb has'.format(build_file, path))

    if failures:
        print('{} check(s) failed'.format(len(failures)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
