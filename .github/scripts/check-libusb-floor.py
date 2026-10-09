#!/usr/bin/env python3
"""Check the libusb version floor that meson.build applies on Windows.

Configures throw-away build directories against fake libusb-1.0.pc files and
checks meson's verdict:

  libusb 1.0.26, -Dusb=enabled, no fallback   configure fails, with the explanation
  libusb 1.0.26, -Dusb=auto, no fallback      configure succeeds without USB, with the explanation
  libusb 1.0.29, -Dusb=enabled, no fallback   configure succeeds, with a warning about 1.0.30
  libusb 1.0.26, -Dusb=enabled                configure uses subprojects/libusb.wrap (1.0.30)
  hotplug fallback, fresh, no patch or git    configure succeeds

The last case configures a copy of the sources, without build directories and
extracted subprojects, with --force-fallback-for=libusb-1.0 and
-Dlibusb:windows-hotplug=true, and with no 'patch' or 'git' program on PATH:
meson extracts libusb afresh, and must need no external tool to set up its
build files, as in an MSYS2 environment without these packages.

Other arguments, after an optional '--', go to 'meson setup', for example
--vsenv or a native file. The last two cases need the libusb tarball, from
subprojects/packagecache or from the network; the fourth one can also use an
extracted subprojects/libusb-1.0.30. The floor only applies to Windows hosts,
so the script does nothing elsewhere.

Usage: check-libusb-floor.py [--meson MESON] [--keep] [--] [MESON_SETUP_ARGS...]
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

SOURCE_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

# The not_found_message of meson.build
EXPLANATION = re.escape('USB support on Windows needs libusb-1.0 >= 1.0.27')
# meson's own verdict: "found: NO. Found 1.0.26 but need: '>=1.0.27'" in recent
# versions, "found: NO found 1.0.26 but need: '>=1.0.27'" in older ones
TOO_OLD = r"(?i)found:? NO\.? found 1\.0\.26 but need: '>=1\.0\.27'"

CASES = [
    {'version': '1.0.26', 'usb': 'enabled', 'fallback': False, 'succeeds': False,
     'patterns': [TOO_OLD, EXPLANATION, r"Dependency '?libusb-1\.0'? is required but not found"]},
    {'version': '1.0.26', 'usb': 'auto', 'fallback': False, 'succeeds': True,
     'patterns': [TOO_OLD, EXPLANATION, r'USB support\s*:\s*(false|NO)']},
    {'version': '1.0.29', 'usb': 'enabled', 'fallback': False, 'succeeds': True,
     'patterns': [r'WARNING: libusb-1\.0 1\.0\.29 lacks the WinUSB RAW_IO functions',
                  r'USB support\s*:\s*(true|YES)']},
    {'version': '1.0.26', 'usb': 'enabled', 'fallback': True, 'succeeds': True,
     'patterns': [TOO_OLD, r'Looking for a fallback subproject for the dependency libusb-1\.0',
                  r'(?i)libusb-1\.0 .*found: YES 1\.0\.30', r'USB support\s*:\s*(true|YES)']},
    {'version': None, 'usb': 'enabled', 'fallback': True, 'fresh': True, 'succeeds': True,
     'arguments': ['--force-fallback-for=libusb-1.0', '-Dlibusb:windows-hotplug=true'],
     'patterns': [r'(?i)libusb-1\.0 .*found: YES 1\.0\.30', r'USB support\s*:\s*(true|YES)',
                  r'libusb:windows-hotplug\s*:\s*true'],
     'absent': [r'Missing "patch" or "git"', r"uses feature introduced in '[\d.]+': Wrap files"]},
]

# The programs with which meson applies the diff_files of a wrap
PATCH_PROGRAMS = ['patch', 'git']

PC_TEMPLATE = """prefix={prefix}
libdir=${{prefix}}/lib
includedir=${{prefix}}/include

Name: libusb-1.0
Description: Fake libusb for check-libusb-floor.py
Version: {version}
Libs:
Cflags:
"""

# Lines of the configure output worth showing
INTERESTING = re.compile(r'libusb-1\.0|fallback|USB support|windows-hotplug|ERROR|WARNING|Message:')


def holds_program(directory, name):
    extensions = [''] + os.environ.get('PATHEXT', '.EXE;.CMD;.BAT').split(os.pathsep)
    return any(os.path.isfile(os.path.join(directory, name + extension)) for extension in extensions)


def copy_sources(destination):
    """Copies the sources without .git, build directories and extracted subprojects."""
    subprojects = os.path.normcase(os.path.join(SOURCE_DIR, 'subprojects'))

    def ignore(directory, names):
        ignored = [n for n in names if n == '.git' or os.path.isdir(os.path.join(directory, n, 'meson-private'))]
        if os.path.normcase(os.path.abspath(directory)) == subprojects:
            ignored += [n for n in names
                        if n not in ('packagecache', 'packagefiles') and os.path.isdir(os.path.join(directory, n))]
        return ignored

    shutil.copytree(SOURCE_DIR, destination, ignore=ignore)


def run_case(meson, workdir, case, extra_args):
    version = case['version']
    index = CASES.index(case)
    env = os.environ.copy()
    source_dir = SOURCE_DIR
    if version is not None:
        pc_dir = os.path.join(workdir, 'pkgconfig-' + version)
        os.makedirs(pc_dir, exist_ok=True)
        with open(os.path.join(pc_dir, 'libusb-1.0.pc'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(PC_TEMPLATE.format(prefix=pc_dir.replace('\\', '/'), version=version))
        env['PKG_CONFIG_PATH'] = os.pathsep.join(p for p in [pc_dir, env.get('PKG_CONFIG_PATH')] if p)
        title = 'libusb {}, -Dusb={}{}'.format(version, case['usb'], '' if case['fallback'] else ', no fallback')
    else:
        title = 'hotplug fallback, fresh, no {}'.format(' or '.join(PATCH_PROGRAMS))
    if case.get('fresh'):
        source_dir = os.path.join(workdir, 'source-{}'.format(index))
        copy_sources(source_dir)
        env['PATH'] = os.pathsep.join(d for d in env.get('PATH', '').split(os.pathsep)
                                      if d and not any(holds_program(d, p) for p in PATCH_PROGRAMS))

    build_dir = os.path.join(workdir, 'build-{}'.format(index))
    command = [meson, 'setup', build_dir, source_dir,
               '-Dusb=' + case['usb'],
               '-Dgst-plugin=disabled', '-Dviewer=disabled', '-Dintrospection=disabled',
               '-Ddocumentation=disabled', '-Dtests=false']
    if not case['fallback']:
        command.append('--wrap-mode=nofallback')
    command += case.get('arguments', []) + extra_args

    print('\n=== {}\n{}'.format(title, ' '.join(command)), flush=True)
    # From the work directory: on Windows, Python, and so meson, looks for
    # programs in the current directory first
    result = subprocess.run(command, env=env, cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, errors='replace')
    output = result.stdout
    for line in output.splitlines():
        if INTERESTING.search(line) and not line.startswith('libusb|'):
            print('    ' + line.strip())

    failures = []
    if case['succeeds'] and result.returncode != 0:
        failures.append('configure failed (exit status {})'.format(result.returncode))
    if not case['succeeds'] and result.returncode == 0:
        failures.append('configure succeeded, it should have failed')
    for pattern in case['patterns']:
        if not re.search(pattern, output):
            failures.append('output does not match {!r}'.format(pattern))
    for pattern in case.get('absent', []):
        if re.search(pattern, output):
            failures.append('output matches {!r}'.format(pattern))

    for failure in failures:
        print('FAIL: {}: {}'.format(title, failure))
    if failures:
        print('--- full configure output ---\n' + output)
    else:
        print('PASS: ' + title)
    return not failures


def parse_arguments(argv):
    # Not argparse: every unknown argument goes to meson, '--' or not, as
    # Windows PowerShell drops '--' from the arguments of native commands.
    meson = shutil.which('meson') or 'meson'
    keep = False
    meson_args = []
    arguments = iter(argv)
    for argument in arguments:
        if argument == '--':
            meson_args += list(arguments)
        elif argument == '--keep':
            keep = True
        elif argument == '--meson':
            meson = next(arguments, meson)
        elif argument.startswith('--meson='):
            meson = argument.split('=', 1)[1]
        elif argument in ('-h', '--help'):
            print(__doc__)
            sys.exit(0)
        else:
            meson_args.append(argument)
    return meson, keep, meson_args


def main():
    meson, keep, meson_args = parse_arguments(sys.argv[1:])

    if sys.platform != 'win32':
        print('The libusb version floor only applies to Windows hosts: nothing to check.')
        return 0

    workdir = tempfile.mkdtemp(prefix='arv-libusb-floor-')
    try:
        results = [run_case(meson, workdir, case, meson_args) for case in CASES]
    finally:
        if keep:
            print('\nKept ' + workdir)
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    print('\n{} of {} libusb floor checks passed'.format(sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
