#!/usr/bin/env python3
# SPDX-License-Identifier:Unlicense
"""Checks aravis-u3v.inf, the WinUSB driver package for USB3 Vision cameras.

Usage: winusbinf.py INF CDF [HELPER]

Checks the encoding and the line endings that Windows expects of an INF, its
sections and entries against Microsoft's WinUSB installation documentation
(https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation),
the USB3 Vision model line, and the device interface GUID. Checks that the
MakeCat definition file CDF catalogs the INF, and that the PowerShell helper
HELPER looks for the compatible ID of the model line.

On Windows, also runs InfVerif from the Windows Driver Kit, and MakeCat from the
Windows SDK, when they are installed or named by the ARV_TEST_INFVERIF and
ARV_TEST_MAKECAT environment variables. MakeCat writes the catalog into a
temporary directory: nothing is signed or installed.
"""

import datetime
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import uuid

MODEL_ID = r'USB\Class_EF&SubClass_05&Prot_00'
USB_DEVICE_CLASS_GUID = '{88BAE032-5A81-49f0-BC3D-A4FF138216D6}'

# GUIDs that must not be the device interface GUID: Microsoft's samples, the USB
# device interface class, the USBDevice setup class, and the null GUID
FORBIDDEN_GUIDS = [
    '{9f543223-cede-4fa3-b376-a25ce9a30e74}',
    '{D696BFEB-1734-417d-8A04-86D01071C512}',
    '{A5DCBF10-6530-11D2-901F-00C04FB951ED}',
    USB_DEVICE_CLASS_GUID,
    '{00000000-0000-0000-0000-000000000000}',
]

failures = []


def check(condition, message):
    if condition:
        print('ok: ' + message)
    else:
        print('FAIL: ' + message)
        failures.append(message)
    return condition


def read_text(path, what, allow_utf16):
    """Decodes a Windows text file: ASCII, or UTF-16 LE with a byte order mark,
    the encodings that Windows reads in an INF. A UTF-8 byte order mark makes
    InfVerif report error 1003."""
    with open(path, 'rb') as f:
        data = f.read()
    if allow_utf16 and data.startswith(b'\xff\xfe'):
        text = data[2:].decode('utf-16-le')
    else:
        check(not data.startswith(b'\xef\xbb\xbf'), '{} has no UTF-8 byte order mark'.format(what))
        check(all(b < 0x80 for b in data), '{} is ASCII'.format(what))
        text = data.decode('ascii', errors='replace')
    check('\0' not in text, '{} has no NUL character'.format(what))
    lone_lf = len(re.findall(r'(?<!\r)\n', text))
    lone_cr = len(re.findall(r'\r(?!\n)', text))
    check(lone_lf == 0 and lone_cr == 0 and text.endswith('\r\n'),
          '{} has CRLF line endings ({} lone LF, {} lone CR); .gitattributes keeps them, '
          'a file written with LF needs "git add --renormalize" and a fresh checkout'.format(what, lone_lf, lone_cr))
    return text


def strip_comment(line):
    """Removes a ; comment, outside of double-quoted strings."""
    quoted = False
    for i, c in enumerate(line):
        if c == '"':
            quoted = not quoted
        elif c == ';' and not quoted:
            return line[:i]
    return line


def split_list(value):
    """Splits a comma-separated INF value list, outside of double-quoted strings."""
    items = []
    current = ''
    quoted = False
    for c in value:
        if c == '"':
            quoted = not quoted
        if c == ',' and not quoted:
            items.append(current.strip())
            current = ''
        else:
            current += c
    items.append(current.strip())
    return items


def parse_sections(text, what):
    """Returns {lower-case section name: (name, [(line number, key or None, value)])}."""
    sections = {}
    current = None
    continued = []
    duplicates = []
    outside = []
    for number, line in enumerate(text.split('\r\n'), 1):
        stripped = strip_comment(line).strip()
        if not stripped:
            continue
        if stripped.endswith('\\'):
            continued.append(number)
        match = re.match(r'^\[([^\]]+)\]$', stripped)
        if match:
            name = match.group(1).strip()
            if name.lower() in sections:
                duplicates.append(name)
            current = sections.setdefault(name.lower(), (name, []))
            continue
        if current is None:
            outside.append(number)
            continue
        key, separator, value = stripped.partition('=')
        if separator and '"' not in key:
            current[1].append((number, key.strip(), value.strip()))
        else:
            current[1].append((number, None, stripped))
    check(not continued, '{} has no line continued with a backslash (found lines {})'.format(what, continued))
    check(not duplicates, '{} defines each section once (found {})'.format(what, duplicates))
    check(not outside, '{} has no entry outside a section (found lines {})'.format(what, outside))
    return sections


def entries(sections, name):
    return sections.get(name.lower(), (name, []))[1]


def single_values(sections, name):
    """Maps the lower-case keys of a section to their values, when each key appears once."""
    values = {}
    for number, key, value in entries(sections, name):
        if key is None:
            check(False, '[{}] line {} is a key = value entry'.format(name, number))
            continue
        check(key.lower() not in values, '[{}] sets {} once'.format(name, key))
        values[key.lower()] = value
    return values


def unquote(value):
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        return value[1:-1].replace('""', '"')
    return value


def check_inf(path):
    text = read_text(path, 'the INF', allow_utf16=True)
    sections = parse_sections(text, 'the INF')
    referenced = {'version', 'manufacturer', 'strings'}
    for name in sorted(referenced):
        check(name in sections, 'the INF has a [{}] section'.format(name.capitalize()))

    strings = {key: unquote(value) for key, value in single_values(sections, 'Strings').items()}
    tokens = set(re.findall(r'%([^%\s]+)%', strip_comment_lines(text)))
    for token in sorted(tokens):
        check(token.lower() in strings and strings[token.lower()], '%{}% is defined in [Strings]'.format(token))

    def resolve(value):
        return re.sub(r'%([^%\s]+)%', lambda m: strings.get(m.group(1).lower(), m.group(0)), value)

    # [Version]
    version = single_values(sections, 'Version')
    expected = {'signature': '"$Windows NT$"', 'class': 'USBDevice', 'classguid': USB_DEVICE_CLASS_GUID,
                'pnplockdown': '1'}
    for key, value in expected.items():
        check(version.get(key, '').lower() == value.lower(),
              '[Version] {} is {} (found {!r})'.format(key, value, version.get(key)))
    check(resolve(version.get('provider', '')).strip() not in ('', version.get('provider')),
          '[Version] Provider names a string of [Strings]')
    catalog = version.get('catalogfile', '')
    stem = os.path.splitext(os.path.basename(path))[0]
    check(catalog.lower() == stem.lower() + '.cat',
          '[Version] CatalogFile is {}.cat (found {!r})'.format(stem, catalog))
    driver_ver = re.match(r'^(\d{2})/(\d{2})/(\d{4}),(\d+)\.(\d+)\.(\d+)\.(\d+)$', version.get('driverver', ''))
    if check(driver_ver is not None, '[Version] DriverVer is MM/DD/YYYY,w.x.y.z (found {!r})'.format(
            version.get('driverver'))):
        month, day, year = (int(driver_ver.group(i)) for i in (1, 2, 3))
        try:
            date = datetime.date(year, month, day)
        except ValueError:
            date = None
        check(date is not None and date.year >= 2026, 'the date of DriverVer is valid')
        check(all(int(driver_ver.group(i)) <= 65535 for i in range(4, 8)), 'the version of DriverVer is valid')
    allowed = {'signature', 'class', 'classguid', 'provider', 'catalogfile', 'driverver', 'pnplockdown'}
    check(set(version) <= allowed, '[Version] has no other entry than {}'.format(', '.join(sorted(allowed))))

    # [Manufacturer] and the x64 Models section
    manufacturers = entries(sections, 'Manufacturer')
    models = None
    if check(len(manufacturers) == 1 and manufacturers[0][1] is not None, '[Manufacturer] has one entry'):
        number, key, value = manufacturers[0]
        check(resolve(key) != key, 'the manufacturer name is a string of [Strings]')
        decorations = split_list(value)
        check([d.lower() for d in decorations[1:]] == ['ntamd64'],
              'the models are for x64 only (NTamd64): ARM64 and S mode need Microsoft signatures (found {!r})'.format(
                  value))
        models = decorations[0] + '.NTamd64'
        others = [s for s in sections if s.split('.')[0] == decorations[0].lower() and s != models.lower()]
        check(not others, 'no other Models section than [{}] (found {})'.format(models, others))
    install = None
    if models is not None and check(models.lower() in sections, 'the INF has a [{}] section'.format(models)):
        referenced.add(models.lower())
        lines = entries(sections, models)
        if check(len(lines) == 1 and lines[0][1] is not None, '[{}] has one model line'.format(models)):
            number, key, value = lines[0]
            check(resolve(key) != key, 'the device name is a string of [Strings]')
            fields = split_list(value)
            install = fields[0]
            check(install == 'USB_Install', 'the model installs the USB_Install section (found {!r})'.format(install))
            check([f.lower() for f in fields[1:]] == [MODEL_ID.lower()],
                  'the model matches the compatible ID {} only (found {!r})'.format(MODEL_ID, fields[1:]))

    # Install sections, as in Microsoft's WinUSB sample. As the inbox winusb.inf
    # asks, each one includes its WinUSB section, the .HW one too.
    if install is not None:
        expected_sections = [(install, {'include': 'winusb.inf', 'needs': 'WINUSB.NT'}),
                             (install + '.Services', {'include': 'winusb.inf', 'needs': 'WINUSB.NT.Services'}),
                             (install + '.HW', {'include': 'winusb.inf', 'needs': 'WINUSB.NT.HW'})]
        for name, expected in expected_sections:
            if check(name.lower() in sections, 'the INF has a [{}] section'.format(name)):
                referenced.add(name.lower())
                values = single_values(sections, name)
                if name.lower().endswith('.hw'):
                    addreg = values.pop('addreg', '')
                    if check(addreg.lower() in sections, '[{}] adds the registry values of [{}]'.format(name, addreg)):
                        referenced.add(addreg.lower())
                        check_interface_guid(entries(sections, addreg))
                check({k: v.lower() for k, v in values.items()} == {k: v.lower() for k, v in expected.items()},
                      '[{}] includes {} (found {!r})'.format(name, expected, values))

    names = [sections[name][0] for name in sorted(sections)]
    check(not [n for n in names if n.lower().startswith('classinstall32')],
          'no [ClassInstall32] section: USBDevice is a system-supplied setup class')
    check(not [n for n in names if n.lower().endswith(('.coinstallers', '.wdf'))],
          'no co-installer or KMDF section: Windows 10 and later need none for WinUSB')
    check(not [n for n in names if n.lower() not in referenced],
          'every section is used (unused: {})'.format([n for n in names if n.lower() not in referenced]))
    copies = [number for name in sections for number, key, value in sections[name][1]
              if key is not None and key.lower() in ('copyfiles', 'copyinf', 'defaultdestdir')]
    check(not copies, 'the INF copies no file: the package is the INF alone (found lines {})'.format(copies))
    return sections, version


def strip_comment_lines(text):
    return '\r\n'.join(strip_comment(line) for line in text.split('\r\n'))


def check_interface_guid(lines):
    if not check(len(lines) == 1 and lines[0][1] is None, 'the AddReg section has one line'):
        return
    fields = split_list(lines[0][2])
    check(len(fields) == 5 and fields[0].upper() == 'HKR' and fields[1] == '' and fields[2] == 'DeviceInterfaceGUIDs',
          'the AddReg line sets HKR,,DeviceInterfaceGUIDs (found {!r})'.format(lines[0][2]))
    if len(fields) != 5:
        return
    try:
        flags = int(fields[3], 0)
    except ValueError:
        flags = None
    check(flags == 0x10000,
          'DeviceInterfaceGUIDs is a REG_MULTI_SZ value, flags 0x10000 (found {!r})'.format(fields[3]))
    guid = unquote(fields[4])
    if check(re.match(r'^\{[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\}$', guid)
             and fields[4].startswith('"'),
             'the device interface GUID is a quoted {{8-4-4-4-12}} GUID: {}'.format(guid)):
        check(guid.lower() not in [g.lower() for g in FORBIDDEN_GUIDS],
              'the device interface GUID is not one of a sample or of a Windows class')
        value = uuid.UUID(guid)
        check(value.version == 4 and value.variant == uuid.RFC_4122, 'the device interface GUID is a random one')


def check_cdf(path, inf_path, catalog):
    text = read_text(path, 'the CDF', allow_utf16=False)
    sections = parse_sections(text, 'the CDF')
    header = single_values(sections, 'CatalogHeader')
    check(header.get('name', '').lower() == catalog.lower(),
          '[CatalogHeader] Name is the CatalogFile of the INF, {} (found {!r})'.format(catalog, header.get('name')))
    check(header.get('catalogversion') == '2' and header.get('hashalgorithms', '').upper() == 'SHA256',
          'the catalog hashes with SHA-256 (CatalogVersion 2)')
    check(re.match(r'^0x10010001:OSAttr:2:10\.0$', header.get('catattr1', '')) is not None,
          'the catalog targets Windows 10 and later (OSAttr 2:10.0)')
    files = entries(sections, 'CatalogFiles')
    name = os.path.basename(inf_path)
    check([(key, value) for number, key, value in files] == [('<hash>' + name, name)],
          '[CatalogFiles] lists the INF only, by hash (found {!r})'.format(files))


def check_helper(path, inf_path, model_id):
    with open(path, 'rb') as f:
        data = f.read()
    # Windows PowerShell reads a script without byte order mark in the ANSI code page
    check(all(b < 0x80 for b in data), 'the helper is ASCII')
    text = data.decode('ascii', errors='replace')
    match = re.search(r"^\$InfMatchId = '([^']*)'", text, re.MULTILINE)
    check(match is not None and match.group(1).lower() == model_id.lower(),
          'the helper reports the compatible ID of the model line ({})'.format(match and match.group(1)))
    match = re.search(r"^\$UsbVisionId = '([^']*)'", text, re.MULTILINE)
    check(match is not None and model_id.lower().startswith(match.group(1).lower() + '&'),
          'the helper lists the nodes of every USB3 Vision protocol ({})'.format(match and match.group(1)))
    check("Join-Path $PSScriptRoot '{}'".format(os.path.basename(inf_path)) in text,
          'the helper finds the INF next to itself')


def newest(patterns):
    candidates = []
    for pattern in patterns:
        for path in glob.glob(pattern):
            version = re.search(r'\\(\d+(?:\.\d+)+)\\', path)
            candidates.append((tuple(int(p) for p in version.group(1).split('.')) if version else (), path))
    return max(candidates)[1] if candidates else None


def windows_tool(variable, patterns):
    path = os.environ.get(variable)
    if path:
        return path
    kits = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'), 'Windows Kits', '10')
    return newest([os.path.join(kits, *pattern) for pattern in patterns])


def run_infverif(inf_path):
    infverif = windows_tool('ARV_TEST_INFVERIF',
                            [('Tools', '*', 'x64', 'infverif.exe'), ('Tools', 'x64', 'infverif.exe')])
    if infverif is None:
        print('skipped: InfVerif, from the Windows Driver Kit, is not installed')
        return
    # /w: the requirements of Windows Drivers. Not /h, the WHQL requirements: this
    # INF cannot get a WHQL signature, as it matches a compatible ID.
    result = subprocess.run([infverif, '/v', '/w', inf_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            universal_newlines=True, errors='replace', timeout=120)
    print(result.stdout)
    # InfVerif prints some errors, such as 1003, and still exits with 0
    check(result.returncode == 0 and 'INF is VALID' in result.stdout and 'ERROR(' not in result.stdout,
          '{} /w accepts the INF'.format(infverif))


def run_makecat(inf_path, cdf_path, catalog):
    makecat = windows_tool('ARV_TEST_MAKECAT', [('bin', '*', 'x64', 'makecat.exe'), ('bin', '*', 'x86', 'makecat.exe')])
    if makecat is None:
        print('skipped: MakeCat, from the Windows SDK, is not installed')
        return
    directory = tempfile.mkdtemp(prefix='arv-winusb-catalog-')
    try:
        for path in (inf_path, cdf_path):
            shutil.copy(path, directory)
        result = subprocess.run([makecat, '-v', os.path.basename(cdf_path)], cwd=directory, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, universal_newlines=True, errors='replace', timeout=120)
        print(result.stdout)
        catalog_path = os.path.join(directory, catalog)
        if not check(result.returncode == 0 and os.path.isfile(catalog_path), '{} creates {}'.format(makecat, catalog)):
            return
        certutil = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'certutil.exe')
        dump = subprocess.run([certutil, '-dump', catalog_path], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              universal_newlines=True, errors='replace', timeout=120).stdout
        with open(inf_path, 'rb') as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        # The labels of certutil are in the language of Windows, but not the
        # hash and the attribute names, which come from the catalog
        check(digest in dump.lower(), 'the catalog holds the SHA-256 hash of the INF, {}'.format(digest))
        check('OSAttr' in dump, 'the catalog has the OSAttr attribute')
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__)
        return 2
    inf_path, cdf_path = (os.path.abspath(p) for p in sys.argv[1:3])
    sections, version = check_inf(inf_path)
    catalog = version.get('catalogfile', '')
    check_cdf(cdf_path, inf_path, catalog)
    if len(sys.argv) == 4:
        check_helper(os.path.abspath(sys.argv[3]), inf_path, MODEL_ID)
    if sys.platform == 'win32':
        run_infverif(inf_path)
        run_makecat(inf_path, cdf_path, catalog)
    else:
        print('skipped: InfVerif and MakeCat, which run on Windows only')
    if failures:
        print('{} check(s) failed'.format(len(failures)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
