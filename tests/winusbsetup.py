#!/usr/bin/env python3
# SPDX-License-Identifier:Unlicense
"""Checks arv-winusb-setup.ps1, the WinUSB helper for USB3 Vision cameras.

Usage: winusbsetup.py HELPER

With Windows PowerShell and, when it is on PATH, PowerShell 7 (pwsh): parses
the script and its help, runs its list action on saved pnputil outputs, in XML
and in text, and its install and uninstall actions with -WhatIf, which must
print the expected pnputil commands, and runs its list action on this machine,
which only reads, as does its reading of every connected device with pnputil.
Its verdicts on this machine are also checked with the results of its registry
and libusbK.dll checks replaced. Nothing is installed or removed. Skipped
outside Windows.

The saved outputs follow the layout of pnputil 10.0.26200. Their devices have
made-up IDs and drivers: a camera with WinUSB from aravis-u3v.inf, one without
a driver, one with a vendor's own driver, one with a vendor's WinUSB package,
one whose USB3 Vision function has another protocol, one with the WinUsb Device
driver of Windows, two with the libusbK and libusb0 drivers that Zadig
installs, which libusb also drives, and a composite device that is not a
camera. They come in XML, in XML without the start tag of its root element, as
pnputil 10.0.26200 writes some of its /output-file files, in English text, and
in German text, which the helper cannot read.
"""

import base64
import os
import re
import shutil
import subprocess
import sys
import tempfile
from xml.sax.saxutils import escape, quoteattr

SKIP = 77
# The first build of Windows 11
WINDOWS_11 = 22000
INF_NAME = 'aravis-u3v.inf'
USBDEVICE_GUID = '{88bae032-5a81-49f0-bc3d-a4ff138216d6}'
U3V_IDS = [r'USB\Class_EF&SubClass_05&Prot_00', r'USB\Class_EF&SubClass_05', r'USB\Class_EF']

ARAVIS = ('oem9001.inf', INF_NAME, 'Aravis Project')
VENDOR = ('oem9003.inf', 'examplevision-u3v.inf', 'Example Vision')
VENDOR_WINUSB = ('oem9004.inf', 'examplevision-winusb.inf', 'Example Vision')
INBOX = ('winusb.inf', None, 'Microsoft')
# Zadig writes usb_device.inf by default, and libwdi is its provider
LIBWDI_K = ('oem9005.inf', 'usb_device.inf', 'libwdi')
LIBWDI_0 = ('oem9006.inf', 'usb_device.inf', 'libwdi')


def camera(pid, description, service=None, installed=None, outranked=(), compatible_ids=U3V_IDS, problem=None):
    instance = r'USB\VID_ABCD&PID_{}&MI_00\7&1A2B3C4D&0&000{}'.format(pid, pid[-1])
    drivers = []
    if installed:
        drivers.append(installed + ('Best Ranked / Installed', 'BestRanked/Installed'))
    for driver in outranked:
        drivers.append(driver + ('Outranked', 'Outranked'))
    return {
        'instance': instance,
        'description': description,
        'status': 'Problem' if problem else 'Started',
        'problem': problem,
        'driver': installed[0] if installed else None,
        'service': service,
        'hardware_ids': [r'USB\VID_ABCD&PID_{}&REV_0100&MI_00'.format(pid), r'USB\VID_ABCD&PID_{}&MI_00'.format(pid)],
        'compatible_ids': list(compatible_ids),
        'drivers': drivers,
    }


DEVICES = [
    camera('0001', 'USB3 Vision camera (WinUSB for Aravis)', 'WINUSB', ARAVIS),
    camera('0002', 'USB3 Vision Camera', problem=('28', '(0x1C) [CM_PROB_FAILED_INSTALL]')),
    camera('0003', 'Example Vision camera', 'ExampleU3V', VENDOR, [ARAVIS]),
    camera('0004', 'Example Vision camera (WinUSB)', 'WINUSB', VENDOR_WINUSB, [ARAVIS]),
    camera('0005', 'Camera with another protocol',
           compatible_ids=[r'USB\Class_EF&SubClass_05&Prot_01', r'USB\Class_EF&SubClass_05', r'USB\Class_EF'],
           problem=('28', '(0x1C) [CM_PROB_FAILED_INSTALL]')),
    camera('0007', 'WinUsb Device', 'WINUSB', INBOX),
    camera('0008', 'Camera with libusbK', 'libusbK', LIBWDI_K),
    camera('0009', 'Camera with libusb-win32', 'libusb0', LIBWDI_0),
    {
        'instance': r'USB\VID_ABCD&PID_0006\SERIAL0006',
        'description': 'USB Composite Device',
        'status': 'Started',
        'problem': None,
        'driver': 'usb.inf',
        'service': 'usbccgp',
        'hardware_ids': [r'USB\VID_ABCD&PID_0006&REV_0100', r'USB\VID_ABCD&PID_0006'],
        'compatible_ids': [r'USB\DevClass_EF&SubClass_02&Prot_01', r'USB\DevClass_EF&SubClass_02', r'USB\DevClass_EF',
                           r'USB\COMPOSITE'],
        'drivers': [('usb.inf', None, 'Microsoft', 'Best Ranked / Installed', 'BestRanked/Installed')],
    },
]

PACKAGES = [('oem9001.inf', INF_NAME, 'Aravis Project'), ('oem9002.inf', INF_NAME, 'Aravis Project'),
            ('oem9003.inf', 'examplevision-u3v.inf', 'Example Vision')]

failures = []


def check(condition, message, output=None):
    if condition:
        print('ok: ' + message)
    else:
        print('FAIL: ' + message)
        if output is not None:
            print('--- output\n{}\n---'.format(output))
        failures.append(message)
    return condition


def device_xml(devices):
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<PnpUtil Version="10.0.26200" '
             'Command="/enum-devices /connected /deviceids /services /drivers /format xml">']
    if not devices:
        lines.append('No devices were found on the system.')
    for d in devices:
        lines.append('    <Device InstanceId={}>'.format(quoteattr(d['instance'])))
        lines.append('        <DeviceDescription>{}</DeviceDescription>'.format(escape(d['description'])))
        lines.append('        <ClassName>USBDevice</ClassName>')
        lines.append('        <ClassGuid>{}</ClassGuid>'.format(USBDEVICE_GUID))
        lines.append('        <Status>{}</Status>'.format(d['status']))
        if d['problem']:
            # The element name is a guess: the helper reports every Problem* child
            lines.append('        <ProblemCode>{}</ProblemCode>'.format(d['problem'][0]))
        if d['driver']:
            lines.append('        <DriverName>{}</DriverName>'.format(d['driver']))
        lines.append('        <HardwareIds>')
        lines += ['            <HardwareId>{}</HardwareId>'.format(escape(i)) for i in d['hardware_ids']]
        lines.append('        </HardwareIds>')
        lines.append('        <CompatibleIds>')
        lines += ['            <CompatibleId>{}</CompatibleId>'.format(escape(i)) for i in d['compatible_ids']]
        lines.append('        </CompatibleIds>')
        if d['service']:
            lines.append('        <Service>{}</Service>'.format(d['service']))
        if d['drivers']:
            lines.append('        <MatchingDrivers>')
            for name, original, provider, text_status, xml_status in d['drivers']:
                lines.append('            <DriverName DriverName={}>'.format(quoteattr(name)))
                if original:
                    lines.append('                <OriginalName>{}</OriginalName>'.format(original))
                lines.append('                <ProviderName>{}</ProviderName>'.format(escape(provider)))
                lines.append('                <Status>{}</Status>'.format(xml_status))
                lines.append('            </DriverName>')
            lines.append('        </MatchingDrivers>')
        lines.append('    </Device>')
    lines.append('</PnpUtil>')
    return '\r\n'.join(lines) + '\r\n'


def field(label, values, indent=0):
    """A pnputil text field: values start at column 28, further values on their own lines."""
    if isinstance(values, str):
        values = [values]
    head = ' ' * indent + label + ':'
    lines = [head + ' ' * (28 - len(head)) + values[0]]
    lines += [' ' * 28 + value for value in values[1:]]
    return lines


def device_text(devices):
    lines = ['Microsoft PnP Utility', '']
    if not devices:
        lines += ['No devices were found on the system.', '']
    for d in devices:
        lines += field('Instance ID', d['instance'])
        lines += field('Device Description', d['description'])
        lines += field('Class Name', 'USBDevice')
        lines += field('Class GUID', USBDEVICE_GUID)
        lines += field('Status', d['status'])
        if d['problem']:
            lines += field('Problem Code', ' '.join(d['problem']))
        if d['driver']:
            lines += field('Driver Name', d['driver'])
        lines += field('Hardware IDs', d['hardware_ids'])
        lines += field('Compatible IDs', d['compatible_ids'])
        if d['service']:
            lines += field('Service', d['service'])
        if d['drivers']:
            lines.append('Matching Drivers:')
            for i, (name, original, provider, text_status, xml_status) in enumerate(d['drivers']):
                if i:
                    lines.append(' ' * 28)
                lines += field('Driver Name', name, 4)
                if original:
                    lines += field('Original Name', original, 4)
                lines += field('Provider Name', provider, 4)
                lines += field('Driver Status', text_status, 4)
        lines.append('')
    return '\r\n'.join(lines) + '\r\n'


def driver_xml(packages):
    lines = ['<?xml version="1.0" encoding="utf-8"?>',
             '<PnpUtil Version="10.0.26200" Command="/enum-drivers /format xml">']
    for published, original, provider in packages:
        lines += ['    <Driver DriverName={}>'.format(quoteattr(published)),
                  '        <OriginalName>{}</OriginalName>'.format(original),
                  '        <ProviderName>{}</ProviderName>'.format(escape(provider)),
                  '        <ClassName>USBDevice</ClassName>',
                  '    </Driver>']
    lines.append('</PnpUtil>')
    return '\r\n'.join(lines) + '\r\n'


def driver_text(packages):
    lines = ['Microsoft PnP Utility', '']
    for published, original, provider in packages:
        lines += ['Published Name:     ' + published, 'Original Name:      ' + original,
                  'Provider Name:      ' + provider, 'Class Name:         USBDevice', '']
    return '\r\n'.join(lines) + '\r\n'


def without_root_start_tag(xml):
    """The XML as pnputil 10.0.26200 writes it to an /output-file when the start tag
    of its root element, which repeats the command line, would be 255 to 289
    characters long: an empty line instead of the tag, and still the end tag."""
    lines = xml.split('\r\n')
    assert lines[1].startswith('<PnpUtil ') and lines[-2] == '</PnpUtil>'
    lines[1] = ''
    return '\r\n'.join(lines)


# Labels of the text output of a German Windows
GERMAN = [('Microsoft PnP Utility', 'Microsoft-PnP-Dienstprogramm'), ('Instance ID', 'Instanz-ID'),
          ('Device Description', 'Ger\u00e4tebeschreibung'), ('Class Name', 'Klassenname'),
          ('Class GUID', 'Klassen-GUID'), ('Problem Code', 'Problemcode'), ('Hardware IDs', 'Hardware-IDs'),
          ('Compatible IDs', 'Kompatible IDs'), ('Service:', 'Dienst:'), ('Matching Drivers', 'Passende Treiber'),
          ('Driver Status', 'Treiberstatus'), ('Driver Name', 'Treibername'),
          ('Published Name', 'Ver\u00f6ffentlichter Name'), ('Original Name', 'Urspr\u00fcnglicher Name'),
          ('Provider Name', 'Anbietername')]


def german(text):
    for english, translation in GERMAN:
        text = text.replace(english, translation)
    return text


def write_fixture(directory, name, text):
    path = os.path.join(directory, name)
    with open(path, 'w', newline='', encoding='utf-8') as f:
        f.write(text)
    return path


def format_command(arguments):
    """What the helper prints: arguments with other characters than these are quoted."""
    return 'pnputil ' + ' '.join(a if re.match(r'^[A-Za-z0-9_.:\\/-]+$', a) else '"{}"'.format(a) for a in arguments)


def find_shells():
    shells = []
    windows = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32', 'WindowsPowerShell', 'v1.0',
                           'powershell.exe')
    if os.path.isfile(windows):
        shells.append(windows)
    pwsh = shutil.which('pwsh')
    if pwsh:
        shells.append(pwsh)
    return shells


def shell_environment():
    # Each PowerShell computes its own module path: one inherited from the other
    # makes Windows PowerShell load PowerShell 7 modules, which fail
    env = {k: v for k, v in os.environ.items() if k.upper() != 'PSMODULEPATH'}
    env['POWERSHELL_TELEMETRY_OPTOUT'] = '1'
    env['POWERSHELL_UPDATECHECK'] = 'Off'
    # No ANSI colours in the warnings of PowerShell 7
    env['NO_COLOR'] = '1'
    return env


class Runner:
    def __init__(self, shell, script):
        self.shell = shell
        self.script = script
        self.env = shell_environment()

    def command(self, *arguments):
        return [self.shell, '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass'] + list(arguments)

    def execute(self, command):
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.env, timeout=120)
        out = result.stdout.decode('utf-8', errors='replace').replace('\r\n', '\n')
        err = result.stderr.decode('utf-8', errors='replace').replace('\r\n', '\n')
        return result.returncode, out, err

    def run(self, *arguments):
        return self.execute(self.command('-File', self.script, *arguments))

    def run_code(self, code):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode('ascii')
        return self.execute(self.command('-EncodedCommand', encoded))


def blocks(output):
    """Maps the instance ID that starts each paragraph of the list output to the paragraph."""
    result = {}
    for paragraph in output.split('\n\n'):
        lines = paragraph.strip('\n').split('\n')
        if lines and lines[0].startswith('USB\\'):
            result[lines[0]] = '\n'.join(lines)
    return result


def what_if_commands(output):
    # The pnputil command that ends a line, whatever precedes it: a localized
    # Windows PowerShell translates the 'What if:' prefix, and no other line of
    # the outputs of install and uninstall holds a pnputil command
    return [m.group(1) for m in re.finditer(r'^.*?(pnputil /.*)$', output, re.MULTILINE)]


def check_parse(runner):
    path = runner.script.replace("'", "''")
    code = ("$tokens = $null; $errors = $null; "
            "[void][System.Management.Automation.Language.Parser]::ParseFile('{0}', [ref]$tokens, [ref]$errors); "
            "foreach ($e in $errors) {{ [Console]::Out.WriteLine($e.ToString()) }}; "
            "[Console]::Out.WriteLine('synopsis: ' + (Get-Help -Name '{0}').Synopsis); "
            "exit $errors.Count").format(path)
    code, out, err = runner.run_code(code)
    check(code == 0, 'the script parses without error', out + err)
    synopsis = re.search(r'synopsis: (.*)', out)
    check(synopsis is not None and synopsis.group(1).startswith('Lists the USB3 Vision cameras'),
          'its comment-based help parses', out + err)


def check_registry(runner):
    # The functions behind the DeviceInterfaceGUIDs check of the list action, on
    # registry keys that every Windows has: the test writes nothing
    path = runner.script.replace("'", "''")
    code = ("$ast = [System.Management.Automation.Language.Parser]::ParseFile('{0}', [ref]$null, [ref]$null); "
            "foreach ($name in 'Test-RegistryValue', 'Test-InterfaceGuid') {{ "
            "  $function = $ast.Find({{ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] "
            "    -and $n.Name -eq $name }}, $true); "
            "  Invoke-Expression $function.Extent.Text }}; "
            "$key = 'HKLM:\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion'; "
            "$results = @((Test-RegistryValue $key @('ArvNoSuchValue', 'CurrentBuild')), "
            "  (Test-RegistryValue $key @('ArvNoSuchValue')), "
            "  (Test-InterfaceGuid 'USB\\VID_0000&PID_0000\\ARAVIS-NO-SUCH-DEVICE')); "
            "[Console]::Out.WriteLine('results: ' + (($results | ForEach-Object {{ if ($null -eq $_) {{ 'null' }} "
            "  else {{ $_ }} }}) -join ' '))").format(path)
    code, out, err = runner.run_code(code)
    check(code == 0 and 'results: True False null' in out,
          'the registry check finds a value, misses an absent one, and reports an unknown device', out + err)


# PowerShell code that defines the functions and the constants (its top-level
# variable assignments) of the helper at @HELPER@
LOAD_HELPER = (
    "Set-StrictMode -Version 2.0; $ErrorActionPreference = 'Stop'; "
    "$ast = [System.Management.Automation.Language.Parser]::ParseFile('@HELPER@', [ref]$null, [ref]$null); "
    "foreach ($f in $ast.FindAll({ param($n) "
    "  $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true)) { "
    "  Invoke-Expression $f.Extent.Text }; "
    "foreach ($s in $ast.EndBlock.Statements) { "
    "  if ($s -is [System.Management.Automation.Language.AssignmentStatementAst] -and "
    "      $s.Left -is [System.Management.Automation.Language.VariableExpressionAst]) { "
    "    Invoke-Expression $s.Extent.Text } }; ")

# Verdicts of the list action on this machine, for a driver, the result of the
# libusbK.dll check and that of the DeviceInterfaceGUIDs check ($null: the
# registry key cannot be read): the start of the verdict, and whether the
# libusbK.dll check applies
LIVE_VERDICTS = [
    ('libusbK', True, True, 'can open it: libusb drives libusbK through libusbK.dll, which is installed. '
     'WinUSB, which needs no other DLL, is the recommended driver', True),
    ('libusbK', False, True, 'cannot open it: libusb drives libusbK through libusbK.dll, which is missing from the '
     'System32 folder of Windows.', True),
    ('libusbK', True, False, 'cannot open it yet: libusbK is its driver, but the DeviceInterfaceGUIDs registry value',
     True),
    # The DLL comes first, and pnputil may print a service name in capitals
    ('LIBUSB0', False, False, 'cannot open it: libusb drives LIBUSB0 through libusbK.dll, which is missing', True),
    ('libusb0', True, None, 'can open it: libusb drives libusb0 through libusbK.dll, which is installed.', True),
    ('WINUSB', False, True, 'can open it while no other program', False),
    ('WINUSB', False, False, 'cannot open it yet: WinUSB is its driver, but the DeviceInterfaceGUIDs registry value',
     False),
    ('ExampleU3V', False, True, 'cannot open it: its driver is ExampleU3V, which libusb does not support. '
     'aravis-u3v.inf never replaces another driver', False),
]


def check_libusbk(runner, directory):
    """libusb drives the libusbK and libusb0 drivers through libusbK.dll, which
    it loads from the System32 folder only. Checks, in one PowerShell, that the
    helper looks for that file there, by default in the folder that
    Get-SystemDirectory returns, then its verdicts on this machine with the
    results of its libusbK.dll and DeviceInterfaceGUIDs checks replaced."""
    def literal(value):
        if value is None:
            return '$null'
        if isinstance(value, bool):
            return '$true' if value else '$false'
        return "'" + value.replace("'", "''") + "'"

    folder = tempfile.mkdtemp(prefix='system ', dir=directory)
    cases = ', '.join('@({}, {}, {})'.format(literal(service), literal(dll), literal(guid))
                      for service, dll, guid, _, _ in LIVE_VERDICTS)
    code = (LOAD_HELPER.replace('@HELPER@', runner.script.replace("'", "''")) +
            "$folder = " + literal(folder) + "; $dll = " + literal(os.path.join(folder, 'libusbK.dll')) + "; "
            "$results = @((Test-LibusbKDll $folder)); "
            "$null = New-Item -ItemType Directory -Path $dll; "
            "$results += (Test-LibusbKDll $folder); "
            "Remove-Item -LiteralPath $dll; "
            "[IO.File]::WriteAllBytes($dll, [byte[]] @(77, 90)); "
            "$results += (Test-LibusbKDll $folder); "
            "$results += (Test-Path -LiteralPath (Join-Path (Get-SystemDirectory) 'kernel32.dll') -PathType Leaf); "
            "$results += (Test-LibusbKDll).GetType().Name; "
            # Without an argument, the check looks in the folder of Get-SystemDirectory,
            # which its default value calls at each check
            "function Get-SystemDirectory { return $folder }; "
            "$results += (Test-LibusbKDll); "
            "Remove-Item -LiteralPath $dll; "
            "$results += (Test-LibusbKDll); "
            "[Console]::Out.WriteLine('results: ' + ($results -join ' ')); "
            "$InfPath = 'aravis-u3v.inf'; "
            "function Test-InterfaceGuid ([string] $InstanceId) { return $global:Guid }; "
            "function Test-LibusbKDll { $global:DllChecks++; return $global:Dll }; "
            "foreach ($case in @(" + cases + ")) { "
            "  $global:Dll = $case[1]; $global:Guid = $case[2]; $global:DllChecks = 0; "
            "  $device = New-Device -InstanceId 'USB\\VID_ABCD&PID_0001&MI_00\\1' -Description 'Camera' "
            "    -Status 'Started' -Problems @() -DriverName 'oem9001.inf' -Service $case[0] -HardwareIds @() "
            "    -CompatibleIds @($InfMatchId, $UsbVisionId) -Drivers @(); "
            "  $verdict = @(Get-Verdict $device $true) -join ' '; "
            "  [Console]::Out.WriteLine(('verdict {0} {1}: {2}' -f $case[0], $global:DllChecks, $verdict)) }")
    code, out, err = runner.run_code(code)
    check(code == 0 and 'results: False False True True Boolean True False\n' in out,
          'the libusbK.dll check looks for that file, by default in the System32 folder of Windows, which '
          'Get-SystemDirectory returns', out + err)
    verdicts = re.findall(r'^verdict (\S+) (\d+): (.*)$', out, re.MULTILINE)
    if not check(code == 0 and len(verdicts) == len(LIVE_VERDICTS), 'the helper gives a verdict in each case',
                 out + err):
        return
    for (service, dll, guid, expected, dll_checked), (printed, checks, verdict) in zip(LIVE_VERDICTS, verdicts):
        check(printed == service and verdict.startswith(expected) and checks == ('1' if dll_checked else '0'),
              'on this machine, a camera whose driver is {} gets "{}" when libusbK.dll is {} and its '
              'DeviceInterfaceGUIDs value {}'.format(service, expected,
                                                     'present' if dll else 'missing',
                                                     'unknown' if guid is None else 'present' if guid else 'missing'),
              'printed: {} ({} libusbK.dll checks): {}'.format(printed, checks, verdict))


def check_list(runner, path, form):
    code, out, err = runner.run('list', '-DeviceListFile', path)
    if not check(code == 0 and not err, 'list reads a saved {} device list'.format(form), out + err):
        return
    found = blocks(out)
    expected = {d['instance'] for d in DEVICES if r'USB\Class_EF&SubClass_05' in d['compatible_ids']}
    check(set(found) == expected, 'list shows the USB3 Vision functions only ({} of {} devices)'.format(
        len(expected), len(DEVICES)), out)
    expectations = {
        '0001': ['Driver: WINUSB, from oem9001.inf (aravis-u3v.inf, Aravis Project)', 'Aravis: can open it.',
                 'IDs:    vendor ID 0xABCD, product ID 0x0001, interface 0'],
        '0002': ['Driver: none', 'Aravis: cannot open it: it has no driver. Install aravis-u3v.inf',
                 'Status: Problem, ', '28'],
        '0003': ['Driver: ExampleU3V, from oem9003.inf (examplevision-u3v.inf, Example Vision)',
                 'Aravis: cannot open it: its driver is ExampleU3V, which libusb does not support.',
                 'aravis-u3v.inf never replaces another driver',
                 'aravis-u3v.inf (oem9001.inf) matches it too, but ranks below its driver.'],
        '0004': ['Driver: WINUSB, from oem9004.inf (examplevision-winusb.inf, Example Vision)',
                 'Aravis: can open it while no other program, such as the software of the camera vendor, has it open.',
                 'aravis-u3v.inf (oem9001.inf) matches it too, but ranks below its driver.'],
        '0005': ['Aravis: cannot open it: it has no driver.',
                 r'aravis-u3v.inf does not match it: its compatible IDs lack USB\Class_EF&SubClass_05&Prot_00.'],
        # Read from a file, the DeviceInterfaceGUIDs value of the camera is unknown
        '0007': ['Driver: WINUSB, from winusb.inf (Microsoft)',
                 'Aravis: can open it once its DeviceInterfaceGUIDs registry value is set'],
        # libusb drives libusbK and libusb0 through libusbK.dll, which a file does not tell about
        '0008': ['Driver: libusbK, from oem9005.inf (usb_device.inf, libwdi)',
                 'Aravis: can open it if libusbK.dll, through which libusb drives libusbK, is in the System32',
                 'WinUSB, which needs no other DLL, is the recommended driver'],
        '0009': ['Driver: libusb0, from oem9006.inf (usb_device.inf, libwdi)',
                 'Aravis: can open it if libusbK.dll, through which libusb drives libusb0, is in the System32',
                 'WinUSB, which needs no other DLL, is the recommended driver'],
    }
    for pid, texts in sorted(expectations.items()):
        block = next((b for instance, b in found.items() if '&PID_{}&'.format(pid) in instance), '')
        missing = [t for t in texts if t not in block]
        check(not missing, 'list describes the camera with product ID {} ({} output)'.format(pid, form),
              'missing: {}\n{}'.format(missing, block))
    check('ranks below' not in found.get(DEVICES[0]['instance'], 'ranks below'),
          'list reports no lower-ranked package for a camera that uses aravis-u3v.inf')


def check_empty_list(runner, directory):
    path = os.path.join(directory, 'no-devices.xml')
    with open(path, 'w', newline='') as f:
        f.write(device_xml([]))
    code, out, err = runner.run('list', '-DeviceListFile', path)
    check(code == 0 and 'No USB3 Vision camera found' in out, 'list reports that no camera is connected', out + err)


def check_live_list(runner):
    build = sys.getwindowsversion().build
    if build < WINDOWS_11:
        # The helper says so, and fails: the pnputil options that it needs came with Windows 11
        print('skipped: list on this machine, which runs Windows build {}, not Windows 11'.format(build))
        return
    code, out, err = runner.run('list')
    check(code == 0 and 'USB3 Vision cameras, from pnputil /enum-devices /connected' in out,
          'list runs pnputil on this machine', out + err)


def check_xml_fallback(runner, directory):
    # When the XML output of pnputil cannot be read, even repaired, list and
    # uninstall read its text output instead: the functions of the helper, with
    # pnputil replaced by saved outputs, unreadable XML and English text
    devices = write_fixture(directory, 'fallback-devices.text', device_text(DEVICES))
    drivers = write_fixture(directory, 'fallback-drivers.text', driver_text(PACKAGES))
    code = ("Set-StrictMode -Version 2.0; $ErrorActionPreference = 'Stop'; "
            "$ast = [System.Management.Automation.Language.Parser]::ParseFile('{0}', [ref]$null, [ref]$null); "
            "foreach ($f in $ast.FindAll({{ param($n) "
            "  $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }}, $true)) {{ "
            "  Invoke-Expression $f.Extent.Text }}; "
            "function Get-PnputilXml {{ return '<?xml version=\"1.0\"?><PnpUtil><Device InstanceId=\"' }}; "
            "function Get-PnputilText ([string[]] $Arguments, [string] $Hint) {{ "
            "  if ($Arguments[0] -eq '/enum-drivers') {{ return [IO.File]::ReadAllLines('{2}') }}; "
            "  return [IO.File]::ReadAllLines('{1}') }}; "
            "$DeviceListFile = $null; $DriverListFile = $null; $InfPath = 'aravis-u3v.inf'; "
            "$UsbVisionId = 'USB\\Class_EF&SubClass_05'; "
            "$result = Get-UsbVisionDevices; "
            "[Console]::Out.WriteLine(('devices: {{0}} from {{1}}' -f $result.Devices.Count, $result.Source)); "
            "$packages = Get-DriverPackages; "
            "[Console]::Out.WriteLine(('driver packages: {{0}}' -f @($packages).Count))").format(
                runner.script.replace("'", "''"), devices.replace("'", "''"), drivers.replace("'", "''"))
    code, out, err = runner.run_code(code)
    expected = sum(1 for d in DEVICES if r'USB\Class_EF&SubClass_05' in d['compatible_ids'])
    text = re.sub(r'\s+', '', out + err)
    check(code == 0
          and 'devices: {} from pnputil /enum-devices /connected /deviceids /services /drivers\n'.format(expected) in out
          and 'driver packages: {}\n'.format(len(PACKAGES)) in out
          and text.count('CouldnotreadtheXMLoutputof') == 1
          and text.count('Couldnotreadtheoutputofpnputil/enum-drivers/formatxml') == 1,
          'list and uninstall read the text output of pnputil when its XML output cannot be read', out + err)


def check_live_xml(runner):
    # pnputil 10.0.26200 leaves out the start tag of the root element of an
    # /output-file when that tag, <PnpUtil Version="10.0.26200" Command="...">,
    # which repeats the command line with the path of the file, would be 255 to
    # 289 characters long. The helper writes the file to the temporary folder:
    # read every connected device, as the list action reads cameras, with
    # temporary folders whose length puts that tag below, inside and above
    # this range. A pnputil without /format makes the helper read text instead.
    base = tempfile.mkdtemp(prefix='arv-winusb-xml-')
    try:
        prefix = len('<PnpUtil Version="10.0.26200" Command="/enum-devices /connected /format xml /output-file ">')
        folders = []
        for tag_length in (248, 258, 266, 274, 282, 286, 296):
            # The file is BASE\PADDING\tmpXXXX.tmp with Windows PowerShell, which
            # writes up to 4 hexadecimal digits, and BASE\PADDING\tmpXXXXXX.tmp
            # with PowerShell 7: the lengths leave margins for both
            padding = tag_length - prefix - len(base + '\\') - len('\\tmpXXXX.tmp')
            if padding > 0:
                folders.append(os.path.join(base, 'p' * padding))
        for folder in folders:
            os.makedirs(folder)
        script = runner.script.replace("'", "''")
        code = ("Set-StrictMode -Version 2.0; $ErrorActionPreference = 'Stop'; "
                "$ast = [System.Management.Automation.Language.Parser]::ParseFile('{0}', [ref]$null, [ref]$null); "
                "foreach ($f in $ast.FindAll({{ param($n) "
                "  $n -is [System.Management.Automation.Language.FunctionDefinitionAst] }}, $true)) {{ "
                "  Invoke-Expression $f.Extent.Text }}; "
                "foreach ($folder in @({1})) {{ "
                "  $env:TMP = $folder; $env:TEMP = $folder; "
                "  $text = Get-PnputilXml @('/enum-devices', '/connected'); "
                "  if ($null -eq $text) {{ [Console]::Out.WriteLine('no xml'); break }}; "
                "  $rootless = $text -notmatch '<PnpUtil[\\s>]'; "
                "  try {{ $devices = ConvertFrom-DeviceXml $text; "
                "    [Console]::Out.WriteLine(('read {{0}} {{1}} {{2}}' -f $folder.Length, $rootless, $devices.Count)) }} "
                "  catch {{ [Console]::Out.WriteLine(('failed {{0}} {{1}} {{2}}' -f $folder.Length, $rootless, "
                "    $_.Exception.Message)) }} }}").format(
                    script, ', '.join("'{}'".format(f.replace("'", "''")) for f in folders))
        code, out, err = runner.run_code(code)
    finally:
        shutil.rmtree(base, ignore_errors=True)
    if 'no xml' in out:
        print('skipped: reading devices in XML, as pnputil has no /format option on this machine')
        return
    results = re.findall(r'^(read|failed) (\d+) (True|False) (.*)$', out, re.MULTILINE)
    rootless = sum(1 for r in results if r[2] == 'True')
    print('{} of {} pnputil XML outputs lacked the start tag of their root element'.format(rootless, len(results)))
    check(code == 0 and len(results) == len(folders) and all(r[0] == 'read' and int(r[3]) > 0 for r in results),
          'the helper reads the connected devices from pnputil whatever the length of the temporary folder',
          out + err)


def same_file(a, b):
    # realpath expands the 8.3 names of a temporary directory, such as RUNNER~1
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


def printed_inf(output):
    """The INF of the only 'pnputil /add-driver INF /install' command printed by -WhatIf, or None."""
    commands = what_if_commands(output)
    match = re.match(r'^pnputil /add-driver ("[^"]+"|[^"\s]+) /install$', commands[0]) if len(commands) == 1 else None
    if match is None:
        return None
    path = match.group(1)
    # A path is quoted when it holds a character that cmd.exe or PowerShell would split on
    if path.startswith('"') != (re.match(r'^"?[A-Za-z0-9_.:\\/-]+"?$', path) is None):
        return None
    return path.strip('"')


def check_install(runner, default_inf, directory):
    code, out, err = runner.run('install', '-WhatIf')
    inf = printed_inf(out)
    check(code == 0 and inf is not None and same_file(inf, default_inf),
          'install -WhatIf prints "pnputil /add-driver <INF next to the script> /install"', out + err)

    package = tempfile.mkdtemp(prefix='driver package ', dir=directory)
    copy = os.path.join(package, INF_NAME)
    shutil.copy(default_inf, copy)
    code, out, err = runner.run('install', '-InfPath', copy, '-WhatIf')
    inf = printed_inf(out)
    check(code == 0 and inf is not None and same_file(inf, copy) and '"{}"'.format(inf) in out,
          'install -WhatIf quotes an INF path with a space', out + err)
    # Warnings are wrapped at the console width: compare without white space. A
    # localized Windows PowerShell translates their 'WARNING:' prefix.
    check('aravis-u3v.catdoesnotexist' in re.sub(r'\s+', '', out),
          'install -WhatIf warns that the catalog is missing', out + err)

    with open(os.path.join(package, 'aravis-u3v.cat'), 'wb') as f:
        f.write(b'not a catalog')
    code, out, err = runner.run('install', '-InfPath', copy, '-WhatIf')
    check(code == 0 and printed_inf(out) is not None and 'aravis-u3v.catisnotsigned' in re.sub(r'\s+', '', out),
          'install -WhatIf warns that the catalog is not signed', out + err)


def check_uninstall(runner, directory):
    for form, name, text in (('XML', 'drivers.xml', driver_xml(PACKAGES)),
                             ('XML without its root start tag', 'drivers-rootless.xml',
                              without_root_start_tag(driver_xml(PACKAGES))),
                             ('text', 'drivers.text', driver_text(PACKAGES))):
        path = write_fixture(directory, name, text)
        code, out, err = runner.run('uninstall', '-DriverListFile', path, '-WhatIf')
        expected = [format_command(['/delete-driver', name, '/uninstall']) for name, original, _ in PACKAGES
                    if original == INF_NAME]
        check(code == 0 and what_if_commands(out) == expected,
              'uninstall -WhatIf prints "pnputil /delete-driver oemNN.inf /uninstall" for each copy of {} only '
              '({} driver list)'.format(INF_NAME, form), out + err)
        if form == 'XML':
            # Safe even if the guard were missing: these packages do not exist
            code, out, err = runner.run('uninstall', '-DriverListFile', path)
            check(code == 1 and '-DriverListFile only works with -WhatIf' in err and 'delete-driver' not in out,
                  'uninstall refuses a saved driver list without -WhatIf', out + err)
    path = os.path.join(directory, 'no-drivers.xml')
    with open(path, 'w', newline='') as f:
        f.write(driver_xml([p for p in PACKAGES if p[1] != INF_NAME]))
    code, out, err = runner.run('uninstall', '-DriverListFile', path, '-WhatIf')
    check(code == 0 and not what_if_commands(out) and 'aravis-u3v.inf is not in the driver store' in out,
          'uninstall -WhatIf does nothing when the package is not installed', out + err)


def check_unread_text(runner, directory):
    # The helper only understands the English text output of pnputil
    path = write_fixture(directory, 'devices-de.txt', german(device_text(DEVICES)))
    code, out, err = runner.run('list', '-DeviceListFile', path)
    check(code == 0 and 'probablynotinEnglish' in re.sub(r'\s+', '', out + err),
          'list warns that it cannot read a device list in German', out + err)
    path = write_fixture(directory, 'drivers-de.txt', german(driver_text(PACKAGES)))
    code, out, err = runner.run('uninstall', '-DriverListFile', path, '-WhatIf')
    check(code == 1 and 'probably not in English' in err and 'not in the driver store' not in out
          and not what_if_commands(out),
          'uninstall fails, rather than saying that aravis-u3v.inf is not in the driver store, '
          'when it cannot read a driver list in German', out + err)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    if sys.platform != 'win32':
        print('skipped: Windows only')
        return SKIP
    shells = find_shells()
    if not shells:
        print('skipped: no PowerShell found')
        return SKIP
    script = os.path.normpath(os.path.abspath(sys.argv[1]))
    default_inf = os.path.join(os.path.dirname(script), INF_NAME)
    directory = tempfile.mkdtemp(prefix='arv winusb ')
    try:
        fixtures = [(write_fixture(directory, name, text), form) for form, name, text in (
            ('XML', 'devices.xml', device_xml(DEVICES)),
            ('XML without its root start tag', 'devices-rootless.xml', without_root_start_tag(device_xml(DEVICES))),
            ('text', 'devices.text', device_text(DEVICES)))]
        for shell in shells:
            print('=== ' + shell)
            runner = Runner(shell, script)
            check_parse(runner)
            check_registry(runner)
            check_libusbk(runner, directory)
            for path, form in fixtures:
                check_list(runner, path, form)
            check_empty_list(runner, directory)
            check_unread_text(runner, directory)
            check_xml_fallback(runner, directory)
            check_live_list(runner)
            check_live_xml(runner)
            check_install(runner, default_inf, directory)
            check_uninstall(runner, directory)
    finally:
        shutil.rmtree(directory, ignore_errors=True)
    if failures:
        print('{} check(s) failed'.format(len(failures)))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
