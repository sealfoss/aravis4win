#!/usr/bin/env python3
# SPDX-License-Identifier:Unlicense
"""Checks the source link in the README of the Windows zips.

Usage: packagewindows.py SOURCE_DIR

Runs .github/scripts/package-windows.py on a fake 'meson install' tree and
reads the README of the zip it writes: the README must link to the tree of the
built revision in the repository given by --source-url, AravisProject/aravis
by default. The workflows that run the script must pass the repository that
they build, from GITHUB_SERVER_URL and GITHUB_REPOSITORY: otherwise the zips
that a fork builds name AravisProject/aravis as the source of the fork's
revision.
"""

import glob
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

failures = []

SHA = '8d517b7a71271c99d4a45330260e4e483f3e0f98'
UPSTREAM = 'https://github.com/AravisProject/aravis'
FORK = 'https://github.com/sealfoss/aravis4win'
# What package-windows.py requires in the tree when it ships no libusb
TREE = ['bin/aravis-0.10-0.dll', 'lib/aravis-0.10.lib', 'include/aravis-0.10/arv.h',
        'bin/arv-tool-0.10.exe', 'bin/arv-camera-test-0.10.exe', 'lib/gstreamer-1.0/gstaravis.0.10.dll']
# GITHUB_SERVER_URL/GITHUB_REPOSITORY, as a pwsh or bash variable or as an expression
SERVER_URL = r'(?:\$env:GITHUB_SERVER_URL|\$\{?GITHUB_SERVER_URL\}?|\$\{\{\s*github\.server_url\s*\}\})'
REPOSITORY = r'(?:\$env:GITHUB_REPOSITORY|\$\{?GITHUB_REPOSITORY\}?|\$\{\{\s*github\.repository\s*\}\})'
SOURCE_URL_ARGUMENT = re.compile(r'--source-url[ =]"?' + SERVER_URL + '/' + REPOSITORY + r'"?(?:\s|$)')


def check(condition, message):
    print(('ok: ' if condition else 'FAIL: ') + message)
    if not condition:
        failures.append(message)
    return condition


def fake_build(top):
    """A meson build directory and its 'meson install --destdir' tree, for the prefix C:/aravis."""
    builddir = os.path.join(top, 'build')
    info = os.path.join(builddir, 'meson-info')
    os.makedirs(info)
    intro = {
        'projectinfo': {'version': '0.9.3', 'descriptive_name': 'aravis', 'subprojects': []},
        'buildoptions': [{'name': 'prefix', 'value': 'C:/aravis'}],
        'dependencies': [{'name': 'gstreamer-base-1.0', 'version': '1.28.7'}],
        'compilers': {'host': {'c': {'id': 'msvc'}}},
    }
    for name, data in intro.items():
        with open(os.path.join(info, 'intro-{}.json'.format(name)), 'w', encoding='utf-8') as f:
            json.dump(data, f)
    destdir = os.path.join(top, 'stage')
    for path in TREE:
        path = os.path.join(destdir, 'aravis', *path.split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').close()
    return builddir, destdir


def readme(script, builddir, destdir, output_dir, arguments):
    """The README of the zip that package-windows.py writes, or None when it fails."""
    result = subprocess.run([sys.executable, script, '--builddir', builddir, '--destdir', destdir,
                             '--flavour', 'msvc-x86_64', '--libusb', 'none', '--output-dir', output_dir] + arguments,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True)
    zips = glob.glob(os.path.join(output_dir, '*.zip'))
    if result.returncode != 0 or len(zips) != 1:
        print(result.stdout.rstrip())
        return None
    with zipfile.ZipFile(zips[0]) as z:
        names = [n for n in z.namelist() if n.endswith('/README.txt')]
        return z.read(names[0]).decode('utf-8').splitlines() if names else []


def package_commands(path):
    """The package-windows.py command lines of a workflow, with their continuation lines joined."""
    with open(path, encoding='utf-8') as f:
        text = re.sub(r'[`\\][ \t]*\r?\n[ \t]*', ' ', f.read())
    return [line.strip() for line in text.splitlines()
            if 'package-windows.py' in line and not line.lstrip().startswith('#')]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    source_dir = os.path.abspath(sys.argv[1])
    script = os.path.join(source_dir, '.github', 'scripts', 'package-windows.py')

    cases = [
        ('a fork build names the fork', ['--revision', SHA, '--source-url', FORK],
         FORK + '/tree/' + SHA, True),
        ('a GitHub Enterprise Server build names its server, without a double slash',
         ['--revision', SHA, '--source-url', 'https://ghe.example.com/vision/aravis/'],
         'https://ghe.example.com/vision/aravis/tree/' + SHA, True),
        ('without --source-url, AravisProject/aravis', ['--revision', SHA], UPSTREAM + '/tree/' + SHA, True),
        ('without --revision, the repository only', ['--source-url', FORK], FORK, False),
    ]
    with tempfile.TemporaryDirectory(prefix='arv-package-test-') as top:
        builddir, destdir = fake_build(top)
        for index, (what, arguments, source, with_revision) in enumerate(cases):
            lines = readme(script, builddir, destdir, os.path.join(top, 'dist', str(index)), arguments)
            if not check(lines is not None, '{}: package-windows.py {} writes a zip with a README'.format(
                    what, ' '.join(arguments))):
                continue
            sources = [l.strip() for l in lines if l.strip().startswith('Source:')]
            check(sources[:1] == ['Source: ' + source], '{}: the README links to {} (it has {})'.format(
                what, source, sources[:1]))
            check(('Built from commit ' + SHA in lines) == with_revision,
                  '{}: the README {} the commit'.format(what, 'names' if with_revision else 'does not name'))

    workflows = sorted(glob.glob(os.path.join(source_dir, '.github', 'workflows', '*.yml')))
    commands = 0
    for workflow in workflows:
        for command in package_commands(workflow):
            commands += 1
            check(SOURCE_URL_ARGUMENT.search(command) is not None,
                  '{} passes the repository it builds to package-windows.py, '
                  'as --source-url "$GITHUB_SERVER_URL/$GITHUB_REPOSITORY"'.format(os.path.basename(workflow)))
    check(commands > 0, 'a workflow runs package-windows.py')

    if failures:
        print('{} check(s) failed'.format(len(failures)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
