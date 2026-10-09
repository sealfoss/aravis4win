#!/usr/bin/env python3
"""Measures the USB3 Vision throughput of Aravis with arv-camera-test.

Runs arv-camera-test-0.10 once per configuration of a matrix and writes the
results. The default matrix is the protocol of the USB page of the Aravis
documentation (docs/reference/aravis/usb.md): transfer size 64 KiB, 256 KiB and
1 MiB, RAW_IO off and on, default and high priority, twelve runs of
  arv-camera-test-0.10 -n CAMERA --usb-mode=async
                       --usb-maximum-transfer-size=SIZE --duration=60
per camera. Every run sets the ARV_UV_RAW_IO environment variable, to 0 for
RAW_IO off and to 1 for on, so that no run depends on the default of Aravis
(RAW_IO off when the variable is unset) or on the environment of the script.
RAW_IO is a WinUSB setting: outside Windows, the default matrix has RAW_IO off
only.

With ARV_UV_RAW_IO=1, Aravis turns RAW_IO on only where libusb and the endpoint
support it, and the stream goes on without RAW_IO when reads keep failing with
it. So every run also sets ARV_DEBUG=stream:2,stream-thread:1, unless ARV_DEBUG
is set: the output of the run then gives the RAW_IO state at the acquisition
start, and a warning when the stream goes on without RAW_IO. The script records
that state, and warns about the runs whose RAW_IO state is not their setting.

For each run, the script records the frame rate, the throughput and the error
count that arv-camera-test prints every second, the stream statistics that it
prints at the end, the RAW_IO state and, unless --no-cpu is given, the CPU time
of arv-camera-test. It writes results.md (a Markdown table with the columns of
the table of the USB page), results.csv, results.json and the output of each
run (logs/) into a new directory, after each run.

Arguments after -- go to arv-camera-test, for example to set up the camera:
  arv-usb-benchmark.py -n CAMERA -- --features="PixelFormat=Mono8"

Examples:
  arv-usb-benchmark.py --dry-run -n CAMERA
  arv-usb-benchmark.py -n CAMERA --controller "Intel xHCI, no hub"
  arv-usb-benchmark.py -n CAMERA --transfer-sizes 128KiB,512KiB --raw-io on

Needs Python 3.7 or later, and nothing outside its standard library.
"""

import argparse
import csv
import datetime
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time

try:
    import resource
except ImportError:
    resource = None

CAMERA_TEST = 'arv-camera-test-0.10'
RAW_IO_VARIABLE = 'ARV_UV_RAW_IO'
DEBUG_VARIABLE = 'ARV_DEBUG'
# The debug output that gives the RAW_IO state of a run: the info lines of the
# stream, and the warnings of the stream thread
RAW_IO_DEBUG = 'stream:2,stream-thread:1'

KIB = 1024
MIB = 1024 * 1024
# arv-camera-test reads --usb-maximum-transfer-size as a 32-bit signed integer
MAX_TRANSFER_SIZE = 2 ** 31 - 1
# The per-second throughput of arv-camera-test is labelled MiB/s, but counts
# 10^6 bytes: this converts it to MiB (2^20 bytes)
MB_TO_MIB = 1e6 / MIB

DEFAULT_TRANSFER_SIZES = '64KiB,256KiB,1MiB'
DEFAULT_RAW_IO = 'off,on' if sys.platform == 'win32' else 'off'
DEFAULT_PRIORITIES = 'default,high'
DEFAULT_DURATION = 60

# Value of ARV_UV_RAW_IO for each RAW_IO setting, which every run sets: no run
# depends on the default of Aravis. With 1, Aravis turns RAW_IO on where libusb
# and the endpoint support it.
RAW_IO_SETTINGS = {'off': '0', 'on': '1'}
PRIORITY_OPTIONS = {'default': [], 'high': ['--high-priority'], 'realtime': ['--realtime']}
USB_MODES = ['async', 'sync']

# The long options of arv-camera-test that this script uses
CAMERA_TEST_OPTIONS = ['name', 'usb-mode', 'usb-maximum-transfer-size', 'duration', 'high-priority', 'realtime']

# Columns of results.md, and of the results table of docs/reference/aravis/usb.md
MARKDOWN_COLUMNS = ['Camera', 'OS', 'Host controller', 'Transfer size', 'RAW_IO', 'Priority',
                    'Frames/s', 'MiB/s', 'CPU %', 'Completed', 'Errors', 'Failures', 'Underruns']

STATISTICS_COLUMNS = ['n_completed_buffers', 'n_failures', 'n_underruns', 'n_aborted',
                      'n_transferred_bytes', 'n_ignored_bytes']

# Output of arv-camera-test (src/arvcameratest.c)

# printf ("%3d frame%s - %7.3g MiB/s", ...), then " - %d error%s" when there are errors
PER_SECOND_RE = re.compile(r'^\s*(?P<frames>\d+) frames?/s\s+-\s+'
                           r'(?P<mb>\d+(?:\.\d*)?(?:[eE][-+]?\d+)?) MiB/s'
                           r'(?:\s+-\s+(?P<errors>\d+) errors?)?\s*$')
# g_print ("%-22s = %" G_GUINT64_FORMAT "\n", ...) for each statistic of the stream; their
# names start with n_, unlike the chunk values that --chunks prints for each frame
STATISTIC_RE = re.compile(r'^(?P<name>n_[a-z0-9_]+)\s*= (?P<value>\d+)\s*$')
# printf ("vendor name            = %s\n", ...) and the like, before the acquisition
HEADER_RE = re.compile(r'^(?P<key>[a-z][a-z_ ]*[a-z])\s+= (?P<value>.*?)\s*$')
LOOKING_FOR_RE = re.compile(r"^Looking for camera '(?P<name>.*)'\s*$")
CONTROL_LOST_RE = re.compile(r'^Control lost\s*$')
PRIORITY_FAILED_RE = re.compile(r'^Failed to make stream thread (?P<what>high priority|realtime)\s*$')
FAILURE_RES = [
    (re.compile(r'^No camera found(?:: (?P<detail>.*?))?\s*$'), 'no camera found'),
    (re.compile(r'^Failed to configure the device: (?P<detail>.*?)\s*$'), 'failed to configure the camera'),
    (re.compile(r'^Failed to read the current device configuration: (?P<detail>.*?)\s*$'),
     'failed to read the camera configuration'),
    (re.compile(r'^Failed to start acquisition \((?P<detail>.*)\)\s*$'), 'failed to start the acquisition'),
    (re.compile(r"^Can't create stream thread(?:: (?P<detail>.*?))?\s*$"), 'failed to create the stream'),
    (re.compile(r'^Option parsing failed: (?P<detail>.*?)\s*$'), 'arv-camera-test rejected an option'),
    (re.compile(r'^(?P<detail>Invalid .*?)\s*$'), 'arv-camera-test rejected an option'),
]
NUMBER_RE = re.compile(r'^[-+]?\d+(?:\.\d*)?(?:[eE][-+]?\d+)?')

# Debug output of the USB3 Vision stream (src/arvuvstream.c), whatever the start
# of its lines. At the acquisition start, the stream logs "Transfer size N bytes
# (maximum N), packet size N bytes, RAW_IO on (maximum N bytes)", or "RAW_IO off
# (reason)" instead. When it goes on without RAW_IO during the acquisition, its
# warnings say "going on without RAW_IO", and its info line "RAW_IO off for the
# rest of the acquisition".
RAW_IO_START_RE = re.compile(r'\bTransfer size \d+ bytes\b.*\bRAW_IO (?P<state>on|off)\b(?: \((?P<detail>[^()]*)\))?')
RAW_IO_MAXIMUM_RE = re.compile(r'^maximum (?P<size>\d+) bytes$')
RAW_IO_DROPPED_RE = re.compile(r'\bwithout RAW_IO\b|\bRAW_IO off for the rest of the acquisition\b')


def _number(value):
    """The number at the start of VALUE, such as 262144 in '262144 bytes'."""
    match = NUMBER_RE.match(value)
    if match is None:
        return None
    token = match.group(0)
    return int(token) if re.match(r'^[-+]?\d+$', token) else float(token)


# Header lines of arv-camera-test: key, and name in the parsed camera dictionary
HEADER_KEYS = {
    'vendor name': 'vendor',
    'model name': 'model',
    'device serial number': 'serial',
    'image width': 'width',
    'image height': 'height',
    'horizontal binning': 'horizontal_binning',
    'vertical binning': 'vertical_binning',
    'exposure': 'exposure_us',
    'gain': 'gain_db',
    'payload': 'payload',
    'uv bandwidth limit': 'uv_bandwidth_limit',
    'gv n_stream channels': 'gv_n_stream_channels',
    'gv current channel': 'gv_current_channel',
    'gv packet delay': 'gv_packet_delay_ns',
    'gv packet size': 'gv_packet_size',
}
HEADER_STRINGS = {'vendor', 'model', 'serial', 'uv_bandwidth_limit'}


def parse_output(text):
    """Parses the output of arv-camera-test.

    Returns a dictionary: 'camera_name' (from "Looking for camera"), 'camera'
    (the header lines: vendor, model, serial, width, height, payload...),
    'seconds' (one dictionary per per-second line: frames, mb, the throughput
    as printed, in 10^6 bytes, and errors), 'statistics' (the final stream
    statistics, in printed order), 'failures' and 'warnings' (messages of
    arv-camera-test), 'control_lost', 'stream_type' (USB3Vision, GigEVision,
    or None when the statistics do not tell), and, from the debug output of a
    USB3 Vision stream, 'raw_io_starts' (the RAW_IO state, 'on' or 'off', and
    its detail in parentheses, at each acquisition start) and 'raw_io_dropped'
    (whether the stream went on without RAW_IO). Other lines, such as other
    debug output, are ignored."""
    parsed = {
        'camera_name': None,
        'camera': {},
        'seconds': [],
        'statistics': {},
        'failures': [],
        'warnings': [],
        'control_lost': False,
        'stream_type': None,
        'raw_io_starts': [],
        'raw_io_dropped': False,
    }
    for line in text.splitlines():
        line = line.rstrip('\r\n')
        match = PER_SECOND_RE.match(line)
        if match:
            parsed['seconds'].append({'frames': int(match.group('frames')),
                                      'mb': float(match.group('mb')),
                                      'errors': int(match.group('errors') or 0)})
            continue
        match = HEADER_RE.match(line)
        if match and match.group('key') in HEADER_KEYS:
            name = HEADER_KEYS[match.group('key')]
            value = match.group('value')
            parsed['camera'][name] = value if name in HEADER_STRINGS else _number(value)
            continue
        match = STATISTIC_RE.match(line)
        if match:
            parsed['statistics'][match.group('name')] = int(match.group('value'))
            continue
        match = LOOKING_FOR_RE.match(line)
        if match:
            parsed['camera_name'] = match.group('name')
            continue
        if CONTROL_LOST_RE.match(line):
            parsed['control_lost'] = True
            continue
        match = PRIORITY_FAILED_RE.match(line)
        if match:
            parsed['warnings'].append('{} not applied: arv-camera-test printed "{}"'.format(
                match.group('what'), line.strip()))
            continue
        match = RAW_IO_START_RE.search(line)
        if match:
            parsed['raw_io_starts'].append({'state': match.group('state'), 'detail': match.group('detail')})
            continue
        if RAW_IO_DROPPED_RE.search(line):
            parsed['raw_io_dropped'] = True
            continue
        for regex, reason in FAILURE_RES:
            match = regex.match(line)
            if match:
                detail = match.group('detail')
                parsed['failures'].append(reason + (': ' + detail if detail else ''))
                break

    statistics = parsed['statistics']
    if 'n_received_packets' in statistics or 'gv_packet_size' in parsed['camera']:
        parsed['stream_type'] = 'GigEVision'
    elif 'n_aborted' in statistics or 'uv_bandwidth_limit' in parsed['camera']:
        parsed['stream_type'] = 'USB3Vision'
    return parsed


def _mean(values):
    return sum(values) / len(values) if values else None


def summarize(parsed, warmup=1):
    """Summarizes a parsed output. Frame rates and throughputs are taken over
    the per-second lines after the first WARMUP ones (over all of them when
    there are no more), errors over all of them. Throughputs are in MiB/s."""
    seconds = parsed['seconds']
    measured = seconds[warmup:] or seconds
    frames = [s['frames'] for s in measured]
    mibps = [s['mb'] * MB_TO_MIB for s in measured]
    statistics = parsed['statistics']
    return {
        'seconds': len(seconds),
        'measured_seconds': len(measured),
        'fps_mean': _mean(frames),
        'fps_min': min(frames) if frames else None,
        'fps_max': max(frames) if frames else None,
        'mibps_mean': _mean(mibps),
        'mibps_min': min(mibps) if mibps else None,
        'mibps_max': max(mibps) if mibps else None,
        'errors': sum(s['errors'] for s in seconds),
        'completed': statistics.get('n_completed_buffers'),
        'failures': statistics.get('n_failures'),
        'underruns': statistics.get('n_underruns'),
    }


def raw_io_state(parsed):
    """The RAW_IO state of a run, from the debug output of its stream: 'on';
    'on, then off' when the stream went on without RAW_IO during the
    acquisition; 'off', or 'off (reason)'; or None when the output does not
    give it."""
    if not parsed['raw_io_starts']:
        return None
    start = parsed['raw_io_starts'][0]
    if start['state'] == 'on':
        return 'on, then off' if parsed['raw_io_dropped'] else 'on'
    return 'off ({})'.format(start['detail']) if start['detail'] else 'off'


def raw_io_maximum(parsed):
    """The RAW_IO maximum transfer size of the stream endpoint, in bytes, from
    the debug output of a stream with RAW_IO on, or None."""
    for start in parsed['raw_io_starts']:
        match = RAW_IO_MAXIMUM_RE.match(start['detail'] or '') if start['state'] == 'on' else None
        if match:
            return int(match.group('size'))
    return None


def raw_io_warning(run, parsed, state):
    """A warning when the RAW_IO state of the run is not its setting, or is not
    known for a USB3 Vision stream with RAW_IO on, else None."""
    if run['raw_io'] == 'on':
        if state == 'on, then off':
            return 'RAW_IO went off during the run, which measured RAW_IO off in part'
        if state is not None and state != 'on':
            return 'RAW_IO was {}: the run measured RAW_IO off'.format(state)
        if state is None and parsed['stream_type'] == 'USB3Vision':
            return 'the output does not give the RAW_IO state: leave {} unset, or include {} in it'.format(
                DEBUG_VARIABLE, RAW_IO_DEBUG)
    elif state is not None and state.startswith('on'):
        return 'RAW_IO was {}, though {}={}'.format(state, RAW_IO_VARIABLE, RAW_IO_SETTINGS[run['raw_io']])
    return None


def raw_io_label(result):
    """The RAW_IO cell of results.md: the setting of the run, with its RAW_IO
    state when the output gives another."""
    setting = result['raw_io']
    state = result.get('raw_io_state')
    if state is None or state == setting or (setting == 'off' and state.startswith('off')):
        return setting
    if state.startswith(setting):
        return state
    return '{}, but {}'.format(setting, state)


# Matrix and commands

SIZE_RE = re.compile(r'^(?P<value>\d+)\s*(?P<unit>k|kib|m|mib)?$', re.IGNORECASE)
SIZE_UNITS = {'': 1, 'k': KIB, 'kib': KIB, 'm': MIB, 'mib': MIB}


def parse_size(text):
    """Parses a transfer size: a number of bytes, with an optional binary unit
    (K or KiB, M or MiB). 'default' or 0 selects the default of Aravis, and
    returns None."""
    text = text.strip()
    if text.lower() == 'default':
        return None
    match = SIZE_RE.match(text)
    if match is None:
        raise ValueError('invalid transfer size "{}": use a number of bytes, KiB or MiB, '
                         'for example 65536, 64KiB or 1MiB, or "default"'.format(text))
    size = int(match.group('value')) * SIZE_UNITS[(match.group('unit') or '').lower()]
    if size > MAX_TRANSFER_SIZE:
        raise ValueError('transfer size "{}" is larger than {} bytes'.format(text, MAX_TRANSFER_SIZE))
    return size or None


def format_size(size):
    if size is None:
        return 'default'
    if size % MIB == 0:
        return '{} MiB'.format(size // MIB)
    if size % KIB == 0:
        return '{} KiB'.format(size // KIB)
    return '{} B'.format(size)


def _list_parser(item_parser):
    def parse(text):
        items = [item.strip() for item in text.split(',')]
        if not all(items):
            raise argparse.ArgumentTypeError('empty item in "{}"'.format(text))
        try:
            return [item_parser(item) for item in items]
        except ValueError as e:
            raise argparse.ArgumentTypeError(str(e))
    return parse


def _choice_parser(choices, what):
    def parse(text):
        if text.lower() not in choices:
            raise ValueError('invalid {} "{}": choose among {}'.format(what, text, ', '.join(choices)))
        return text.lower()
    return parse


def build_matrix(cameras, transfer_sizes, raw_io, priorities):
    """Returns the runs, in order: camera, then transfer size, RAW_IO and
    priority. A camera of None lets arv-camera-test open the first camera that
    it finds."""
    runs = []
    for camera in cameras or [None]:
        for size in transfer_sizes:
            for setting in raw_io:
                for priority in priorities:
                    runs.append({'index': len(runs) + 1, 'camera': camera, 'transfer_size': size,
                                 'raw_io': setting, 'priority': priority})
    return runs


def build_command(camera_test, run, usb_mode='async', duration=DEFAULT_DURATION, extra=()):
    command = [camera_test]
    if run['camera'] is not None:
        command += ['-n', run['camera']]
    command.append('--usb-mode=' + usb_mode)
    if run['transfer_size'] is not None:
        command.append('--usb-maximum-transfer-size={}'.format(run['transfer_size']))
    command.append('--duration={}'.format(duration))
    command += PRIORITY_OPTIONS[run['priority']]
    command += list(extra)
    return command


def run_variables(base, raw_io):
    """The environment variables that a run sets, in order: ARV_UV_RAW_IO for its
    RAW_IO setting, and ARV_DEBUG for the RAW_IO state in its output, unless
    BASE, the environment of the script, sets it."""
    variables = {RAW_IO_VARIABLE: RAW_IO_SETTINGS[raw_io]}
    if not base.get(DEBUG_VARIABLE):
        variables[DEBUG_VARIABLE] = RAW_IO_DEBUG
    return variables


def build_environment(base, raw_io):
    environment = dict(base)
    environment.update(run_variables(base, raw_io))
    return environment


def quote_command(command):
    if sys.platform == 'win32':
        return subprocess.list2cmdline(command)
    return ' '.join(shlex.quote(argument) for argument in command)


def format_command(command, variables):
    """The command line of a run, after the environment variables that it sets."""
    return ' '.join(['{}={}'.format(name, value) for name, value in variables.items()] + [quote_command(command)])


def describe_run(run):
    return '{}, {}, RAW_IO {}, {} priority'.format(run['camera'] or 'first camera found',
                                                   format_size(run['transfer_size']),
                                                   run['raw_io'], run['priority'])


# Running arv-camera-test

if sys.platform == 'win32':
    import ctypes
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    _kernel32.OpenProcess.restype = wintypes.HANDLE
    _kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    _kernel32.GetProcessTimes.restype = wintypes.BOOL
    _kernel32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    _kernel32.CloseHandle.restype = wintypes.BOOL
    _kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    _PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class CpuProbe:
    """Reads the CPU time, user and system, that a child process has used so
    far: GetProcessTimes on Windows, /proc on Linux. read () returns None
    elsewhere, or when the time cannot be read."""

    def __init__(self, pid):
        self.pid = pid
        self.handle = None
        if sys.platform == 'win32':
            # The handle keeps the times readable after the process has exited
            self.handle = _kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)

    def read(self):
        if sys.platform == 'win32':
            if not self.handle:
                return None
            times = [wintypes.FILETIME() for i in range(4)]
            if not _kernel32.GetProcessTimes(self.handle, *[ctypes.byref(t) for t in times]):
                return None
            kernel, user = times[2], times[3]
            return sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in (kernel, user)) / 1e7
        if sys.platform.startswith('linux'):
            try:
                with open('/proc/{}/stat'.format(self.pid)) as f:
                    fields = f.read().rsplit(')', 1)[1].split()
                return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
            except (OSError, IndexError, ValueError):
                return None
        return None

    def close(self):
        if self.handle:
            _kernel32.CloseHandle(self.handle)
            self.handle = None


def _children_cpu_time():
    if resource is None:
        return None
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


class ProcessOutput:
    def __init__(self):
        self.data = []
        self.lines = []
        self.per_second_times = []
        self.cpu_samples = []
        self.returncode = None
        self.timed_out = False
        self.wall_time = None
        self.cpu_time = None

    @property
    def text(self):
        return '\n'.join(text for t, text in self.lines)


def run_process(command, environment=None, timeout=None, on_line=None, cpu=True):
    """Runs COMMAND, with stderr merged into stdout, and returns a
    ProcessOutput: the raw output (data), its lines with their arrival times
    in seconds since the start (lines), the arrival time of each per-second
    line (per_second_times), the CPU time of the process at each per-second
    line (cpu_samples) and in total (cpu_time), its exit code, its run time,
    and whether it was killed after TIMEOUT seconds. ON_LINE is called with
    the text of each line as it arrives."""
    output = ProcessOutput()
    children_cpu = _children_cpu_time() if cpu else None
    start = time.monotonic()
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, env=environment)
    probe = CpuProbe(process.pid) if cpu else None

    def read():
        try:
            for data in iter(process.stdout.readline, b''):
                now = time.monotonic() - start
                text = data.decode('utf-8', errors='replace').rstrip('\r\n')
                # Split as parse_output () does, so that the times match its per-second lines
                for part in text.splitlines():
                    if PER_SECOND_RE.match(part):
                        output.per_second_times.append(now)
                        cpu_time = probe.read() if probe is not None else None
                        if cpu_time is not None:
                            output.cpu_samples.append((now, cpu_time))
                output.data.append(data)
                output.lines.append((now, text))
                if on_line is not None:
                    on_line(text)
        except (OSError, ValueError):
            # The pipe was closed under the reader, after a process that did not exit
            pass

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        output.timed_out = True
        process.kill()
        process.wait()
    except BaseException:
        process.kill()
        process.wait()
        raise
    finally:
        output.wall_time = time.monotonic() - start
        # The pipe closes when the process exits, which ends the reader
        reader.join(10)
        process.stdout.close()
        output.returncode = process.returncode
        if probe is not None:
            # A Windows handle keeps the times of the exited process; elsewhere,
            # the process is gone, and its times are among those of the reaped children
            output.cpu_time = probe.read() if probe.handle else None
            probe.close()
            if output.cpu_time is None and children_cpu is not None:
                output.cpu_time = _children_cpu_time() - children_cpu
    return output


def cpu_usage(output, warmup=1):
    """CPU use of a run, in percent of one logical processor: over the
    streaming phase, from the CPU time read at the per-second lines after the
    warm-up ones, when they arrived as they were printed ('streaming'), else
    over the whole run ('process')."""
    usage = {'percent': None, 'method': None, 'seconds': output.cpu_time,
             'wall_seconds': output.wall_time}
    samples = output.cpu_samples[max(warmup, 1) - 1:]
    if len(samples) >= 2:
        elapsed = samples[-1][0] - samples[0][0]
        # Lines that arrive in one burst were buffered, and tell nothing about time
        if elapsed >= 0.1 * (len(samples) - 1):
            usage['percent'] = 100.0 * (samples[-1][1] - samples[0][1]) / elapsed
            usage['method'] = 'streaming'
            return usage
    if output.cpu_time is not None and output.wall_time:
        usage['percent'] = 100.0 * output.cpu_time / output.wall_time
        usage['method'] = 'process'
    return usage


def _format_exit_code(code):
    if code is None:
        return 'unknown'
    if sys.platform == 'win32' and (code < 0 or code > 0x7fffffff):
        return '0x{:08X}'.format(code & 0xffffffff)
    return str(code)


def evaluate(parsed, summary, output, timeout):
    """Returns the status of a run, 'ok' or 'failed', and the reason of a
    failure."""
    if parsed['failures']:
        return 'failed', '; '.join(parsed['failures'])
    if output.timed_out:
        return 'failed', 'killed after {:g} s'.format(timeout) if timeout else 'killed'
    if parsed['control_lost']:
        return 'failed', 'control lost'
    if output.returncode != 0:
        return 'failed', 'exit code ' + _format_exit_code(output.returncode)
    if not parsed['seconds']:
        return 'failed', 'no per-second line in the output'
    if summary['completed'] is None:
        return 'failed', 'no stream statistics in the output'
    if summary['completed'] == 0:
        return 'failed', 'no frame received'
    return 'ok', None


def find_camera_test(path):
    if path is not None:
        found = shutil.which(path)
        return os.path.abspath(found) if found else None
    found = shutil.which(CAMERA_TEST)
    return os.path.abspath(found) if found else None


def camera_test_version(camera_test):
    """The Aravis version that arv-camera-test --version prints, or None."""
    # On Windows, a program that loads GIO sometimes loses the output that it
    # has not flushed when it exits, which --version does not: try again then
    for attempt in range(3):
        try:
            result = subprocess.run([camera_test, '--version'], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        version = result.stdout.decode('utf-8', errors='replace').strip()
        if result.returncode != 0 or version:
            break
    return version if result.returncode == 0 and re.match(r'^\d+\.\d+\.\d+$', version) else None


def os_description():
    if sys.platform == 'win32':
        return 'Windows {}'.format(platform.version())
    if sys.platform == 'darwin':
        return 'macOS {}'.format(platform.mac_ver()[0])
    return '{} {}'.format(platform.system(), platform.release())


def collect_metadata(args, camera_test, extra, environment=None):
    """What results.json records about the benchmark. ENVIRONMENT is that of the
    script, os.environ by default."""
    if environment is None:
        environment = os.environ
    return {
        'date': datetime.datetime.now().astimezone().isoformat(timespec='seconds'),
        'os': os_description(),
        'platform': platform.platform(),
        'machine': platform.machine(),
        'processor': platform.processor(),
        'logical_processors': os.cpu_count(),
        'python': platform.python_version(),
        'camera_test': camera_test,
        'aravis_version': camera_test_version(camera_test) if camera_test else None,
        'controller': args.controller,
        'notes': args.notes,
        'cameras': args.name,
        'usb_mode': args.usb_mode,
        'duration': args.duration,
        'warmup': args.warmup,
        'transfer_sizes': args.transfer_sizes,
        'raw_io': args.raw_io,
        'priorities': args.priorities,
        'extra_arguments': list(extra),
        'cpu': not args.no_cpu,
        # The ARV_DEBUG of the runs
        'arv_debug': environment.get(DEBUG_VARIABLE) or RAW_IO_DEBUG,
    }


def make_result(run, command, output, variables, warmup=1, timeout=None, log=None, cpu=True):
    """Parses and evaluates the ProcessOutput of a run of COMMAND, which set the
    environment variables VARIABLES: returns what results.json holds for the
    run."""
    parsed = parse_output(output.text)
    summary = summarize(parsed, warmup)
    status, reason = evaluate(parsed, summary, output, timeout)
    warnings = list(parsed['warnings'])
    if parsed['statistics'] and parsed['stream_type'] != 'USB3Vision':
        warnings.append('not the stream of a USB3 Vision camera ({}): the USB settings of the run had no '
                        'effect'.format(parsed['stream_type'] or 'statistics of another kind'))
    state = raw_io_state(parsed)
    warning = raw_io_warning(run, parsed, state)
    if warning is not None:
        warnings.append(warning)
    times = output.per_second_times
    seconds = [dict(second, time=round(times[i], 3) if i < len(times) else None)
               for i, second in enumerate(parsed['seconds'])]
    return dict(run, **{
        'command': command,
        'environment': dict(variables),
        'status': status,
        'reason': reason,
        'warnings': warnings,
        'exit_code': output.returncode,
        'raw_io_state': state,
        'raw_io_maximum': raw_io_maximum(parsed),
        'camera_info': parsed['camera'],
        'stream_type': parsed['stream_type'],
        'seconds': seconds,
        'statistics': parsed['statistics'],
        'summary': summary,
        'cpu': cpu_usage(output, warmup) if cpu else None,
        'log': log,
    })


def execute_run(run, camera_test, args, extra, logs_dir, echo):
    command = build_command(camera_test, run, args.usb_mode, args.duration, extra)
    variables = run_variables(os.environ, run['raw_io'])
    environment = build_environment(os.environ, run['raw_io'])
    # Without a terminal, glibc buffers the output of arv-camera-test, which
    # then arrives at its end: stdbuf makes it arrive line by line
    capture = command
    if sys.platform.startswith('linux') and shutil.which('stdbuf'):
        capture = ['stdbuf', '-oL'] + command

    def on_line(text):
        if echo:
            print('    ' + text, flush=True)

    output = run_process(capture, environment, args.timeout, on_line, cpu=not args.no_cpu)

    log_name = 'run-{:02d}.log'.format(run['index'])
    with open(os.path.join(logs_dir, log_name), 'wb') as f:
        f.write('# {}\n# {}exit code {}, {:.1f} s\n'.format(format_command(command, variables),
                                                          'killed, ' if output.timed_out else '',
                                                          _format_exit_code(output.returncode),
                                                          output.wall_time).encode('utf-8'))
        f.write(b''.join(output.data))

    return make_result(run, command, output, variables, args.warmup, args.timeout, 'logs/' + log_name,
                       cpu=not args.no_cpu)


# Reports

def _camera_label(result):
    info = result.get('camera_info') or {}
    label = ' '.join(info[key] for key in ('vendor', 'model') if info.get(key))
    return label or result['camera'] or '?'


def _format_number(value, digits=1):
    return 'n/a' if value is None else '{:.{}f}'.format(value, digits)


def _format_integer(value):
    return 'n/a' if value is None else str(value)


def _escape(cell):
    return cell.replace('|', '\\|')


def markdown_row(result, metadata):
    summary = result['summary']
    cpu = result.get('cpu') or {}
    ok = result['status'] == 'ok'
    cells = [
        _camera_label(result),
        metadata['os'],
        metadata['controller'] or '?',
        format_size(result['transfer_size']),
        raw_io_label(result),
        result['priority'],
        '{} ({})'.format(_format_number(summary['fps_mean']), summary['fps_min']) if ok else 'failed',
        _format_number(summary['mibps_mean']) if ok else 'n/a',
        _format_number(cpu.get('percent')) if ok else 'n/a',
        _format_integer(summary['completed']),
        _format_integer(summary['errors'] if result['seconds'] else None),
        _format_integer(summary['failures']),
        _format_integer(summary['underruns']),
    ]
    return '| ' + ' | '.join(_escape(cell) for cell in cells) + ' |'


def format_markdown(metadata, results):
    lines = ['# USB3 Vision throughput benchmark', '']
    lines.append('- Date: {}'.format(metadata['date']))
    lines.append('- Machine: {}, {}, {} logical processors'.format(metadata['platform'], metadata['machine'],
                                                                   metadata['logical_processors']))
    lines.append('- Host controller: {}'.format(metadata['controller'] or 'not given (--controller)'))
    lines.append('- arv-camera-test: `{}`{}'.format(metadata['camera_test'],
                                                   ', Aravis ' + metadata['aravis_version']
                                                   if metadata['aravis_version'] else ''))
    command = '{}{} --usb-mode={} --usb-maximum-transfer-size=<size> --duration={}'.format(
        CAMERA_TEST, ' -n <camera>' if metadata['cameras'] else '', metadata['usb_mode'], metadata['duration'])
    if metadata['extra_arguments']:
        command += ' ' + quote_command(metadata['extra_arguments'])
    lines.append('- Command: `{}`, with {}=0 for RAW_IO off, 1 for on, and {}={}'.format(
        command, RAW_IO_VARIABLE, DEBUG_VARIABLE, metadata['arv_debug']))
    maxima = []
    for result in results:
        if result['raw_io_maximum'] is not None:
            maximum = '{}: {} bytes'.format(_camera_label(result), result['raw_io_maximum'])
            if maximum not in maxima:
                maxima.append(maximum)
    if maxima:
        lines.append('- RAW_IO maximum transfer size of the stream endpoint: {}'.format(', '.join(maxima)))
    for note in metadata['notes']:
        lines.append('- Note: {}'.format(note))
    lines.append('')
    warmup = metadata['warmup']
    lines.append('Frames/s is the mean over {}, with the minimum in parentheses. MiB/s is the mean '
                 'throughput over the same seconds, in 2^20 bytes per second (arv-camera-test prints it in '
                 '10^6 bytes per second). CPU % is the CPU time of arv-camera-test while it streams, in '
                 'percent of one logical processor. Completed, Failures and Underruns are the stream '
                 'statistics of the run, Errors the buffers that arv-camera-test received with an error '
                 'status.'.format('every second' if warmup == 0 else
                                  'the seconds after the first' if warmup == 1 else
                                  'the seconds after the first {}'.format(warmup)))
    lines.append('')
    lines.append('| ' + ' | '.join(MARKDOWN_COLUMNS) + ' |')
    lines.append('|' + ' --- |' * len(MARKDOWN_COLUMNS))
    for result in results:
        lines.append(markdown_row(result, metadata))
    notes = []
    for result in results:
        messages = ([result['reason']] if result['reason'] else []) + result['warnings']
        if messages:
            notes.append('- Run {} ({}): {}'.format(result['index'], describe_run(result), '; '.join(messages)))
    if notes:
        lines += ['', '## Failures and warnings', ''] + notes
    return '\n'.join(lines) + '\n'


def write_csv(path, metadata, results):
    extra_statistics = []
    for result in results:
        for name in result['statistics']:
            if name not in STATISTICS_COLUMNS and name not in extra_statistics:
                extra_statistics.append(name)
    fields = ['run', 'camera_name', 'vendor', 'model', 'serial', 'stream_type', 'width', 'height', 'payload',
              'os', 'controller', 'usb_mode', 'transfer_size', 'raw_io', 'raw_io_state', 'raw_io_maximum',
              'priority', 'duration',
              'status', 'reason', 'warnings', 'seconds', 'measured_seconds',
              'fps_mean', 'fps_min', 'fps_max', 'mibps_mean', 'mibps_min', 'mibps_max', 'errors',
              'cpu_percent', 'cpu_method', 'cpu_seconds', 'wall_seconds', 'exit_code'] + \
        STATISTICS_COLUMNS + extra_statistics + ['command', 'log']
    with open(path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fields)
        writer.writeheader()
        for result in results:
            info = result['camera_info']
            summary = result['summary']
            cpu = result['cpu'] or {}
            row = {
                'run': result['index'], 'camera_name': result['camera'],
                'vendor': info.get('vendor'), 'model': info.get('model'), 'serial': info.get('serial'),
                'stream_type': result['stream_type'],
                'width': info.get('width'), 'height': info.get('height'), 'payload': info.get('payload'),
                'os': metadata['os'], 'controller': metadata['controller'], 'usb_mode': metadata['usb_mode'],
                'transfer_size': result['transfer_size'], 'raw_io': result['raw_io'],
                'raw_io_state': result['raw_io_state'], 'raw_io_maximum': result['raw_io_maximum'],
                'priority': result['priority'], 'duration': metadata['duration'],
                'status': result['status'], 'reason': result['reason'], 'warnings': '; '.join(result['warnings']),
                'cpu_percent': cpu.get('percent'), 'cpu_method': cpu.get('method'),
                'cpu_seconds': cpu.get('seconds'), 'wall_seconds': cpu.get('wall_seconds'),
                'exit_code': result['exit_code'],
                'command': format_command(result['command'], result['environment']), 'log': result['log'],
            }
            for key in ('seconds', 'measured_seconds', 'fps_mean', 'fps_min', 'fps_max',
                        'mibps_mean', 'mibps_min', 'mibps_max', 'errors'):
                row[key] = summary[key]
            for name in STATISTICS_COLUMNS + extra_statistics:
                row[name] = result['statistics'].get(name)
            writer.writerow(row)


def write_reports(directory, metadata, results):
    with open(os.path.join(directory, 'results.json'), 'w', encoding='utf-8') as f:
        json.dump({'format': 1, 'metadata': metadata, 'runs': results}, f, indent=2, ensure_ascii=False)
        f.write('\n')
    write_csv(os.path.join(directory, 'results.csv'), metadata, results)
    with open(os.path.join(directory, 'results.md'), 'w', encoding='utf-8') as f:
        f.write(format_markdown(metadata, results))


# Command line

def split_arguments(argv):
    """Splits the arguments at the first --: the ones after it go to
    arv-camera-test."""
    if '--' in argv:
        index = argv.index('--')
        return argv[:index], argv[index + 1:]
    return argv, []


def build_parser():
    parser = argparse.ArgumentParser(
        prog='arv-usb-benchmark.py',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        usage='%(prog)s [options] [-- arv-camera-test options]',
        description=__doc__.split('\n\n')[0],
        epilog='The protocol, and the published results: '
        'https://aravisproject.github.io/aravis/aravis-main/usb.html#throughput-benchmark')
    parser.add_argument('-n', '--name', action='append', default=[], metavar='CAMERA',
                        help='camera to measure, as arv-camera-test -n takes it; repeat for several cameras '
                        '(default: the first camera that arv-camera-test finds)')
    parser.add_argument('--transfer-sizes', type=_list_parser(parse_size), default=DEFAULT_TRANSFER_SIZES,
                        metavar='LIST', help='USB maximum transfer sizes, in bytes, KiB or MiB, or "default" '
                        '(default: %(default)s)')
    parser.add_argument('--raw-io', type=_list_parser(_choice_parser(list(RAW_IO_SETTINGS), 'RAW_IO setting')),
                        default=DEFAULT_RAW_IO, metavar='LIST',
                        help='RAW_IO settings among off and on (ARV_UV_RAW_IO set to 0, or 1) '
                        '(default: off,on on Windows, off elsewhere)')
    parser.add_argument('--priorities', type=_list_parser(_choice_parser(list(PRIORITY_OPTIONS), 'priority')),
                        default=DEFAULT_PRIORITIES, metavar='LIST',
                        help='stream thread priorities among default, high (--high-priority) and realtime '
                        '(--realtime) (default: %(default)s)')
    parser.add_argument('--usb-mode', choices=USB_MODES, default='async', help='USB mode (default: %(default)s)')
    parser.add_argument('--duration', type=int, default=DEFAULT_DURATION, metavar='SECONDS',
                        help='duration of each run (default: %(default)s)')
    parser.add_argument('--warmup', type=int, default=1, metavar='SECONDS',
                        help='first seconds of each run left out of the frame rate, throughput and CPU '
                        '(default: %(default)s)')
    parser.add_argument('--pause', type=float, default=2, metavar='SECONDS',
                        help='pause between runs (default: %(default)s)')
    parser.add_argument('--timeout', type=float, metavar='SECONDS',
                        help='time after which a run is killed (default: duration + 60)')
    parser.add_argument('--camera-test', metavar='PATH',
                        help='arv-camera-test program (default: {} from PATH)'.format(CAMERA_TEST))
    parser.add_argument('-o', '--output-dir', metavar='DIR',
                        help='new directory for the results (default: usb-benchmark-DATE-TIME)')
    parser.add_argument('--controller', default='', metavar='TEXT',
                        help='USB host controller, and hub if any, to record, for example "Intel xHCI, no hub"')
    parser.add_argument('--notes', action='append', default=[], metavar='TEXT',
                        help='note to record, such as the libusb version; repeat for several notes')
    parser.add_argument('--no-cpu', action='store_true', help='do not record CPU use')
    parser.add_argument('-q', '--quiet', action='store_true', help='do not show the output of arv-camera-test')
    parser.add_argument('--dry-run', action='store_true', help='print the commands, and run nothing')
    return parser


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='replace')

    arguments, extra = split_arguments(sys.argv[1:] if argv is None else list(argv))
    parser = build_parser()
    args = parser.parse_args(arguments)
    if args.duration < 1:
        parser.error('--duration must be 1 or more')
    if args.warmup < 0:
        parser.error('--warmup must be 0 or more')
    if args.timeout is None:
        args.timeout = args.duration + 60

    runs = build_matrix(args.name, args.transfer_sizes, args.raw_io, args.priorities)
    camera_test = find_camera_test(args.camera_test)

    if args.dry_run:
        # About 5 s per run to open the camera and stop
        total = len(runs) * (args.duration + 5) + (len(runs) - 1) * args.pause
        debug = DEBUG_VARIABLE in run_variables(os.environ, 'off')
        print('# {} runs of {} s, about {}. Each line sets {} (0: RAW_IO off, 1: on){}, '
              'then runs arv-camera-test.'.format(len(runs), args.duration,
                                                  '{:.0f} s'.format(total) if total < 90 else
                                                  '{:.0f} min'.format(total / 60), RAW_IO_VARIABLE,
                                                  ' and {}, for the RAW_IO state in the output'.format(
                                                      DEBUG_VARIABLE) if debug else ''))
        if not debug:
            print('# {} is set: the output of a run gives its RAW_IO state only if it includes {}.'.format(
                DEBUG_VARIABLE, RAW_IO_DEBUG))
        if not args.name:
            print('# No camera given (-n): arv-camera-test opens the first camera that it finds.')
        for run in runs:
            command = build_command(camera_test or args.camera_test or CAMERA_TEST, run, args.usb_mode,
                                    args.duration, extra)
            print(format_command(command, run_variables(os.environ, run['raw_io'])))
        return 0

    if camera_test is None:
        parser.error('{} not found: put it in PATH, or give its path with --camera-test'.format(
            args.camera_test or CAMERA_TEST))

    directory = args.output_dir or 'usb-benchmark-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    if os.path.exists(directory) and (not os.path.isdir(directory) or os.listdir(directory)):
        parser.error('{} already exists, and is not an empty directory'.format(directory))
    logs_dir = os.path.join(directory, 'logs')
    os.makedirs(logs_dir, exist_ok=True)

    metadata = collect_metadata(args, camera_test, extra)
    print('{} runs of {} s with {}{}, results in {}'.format(
        len(runs), args.duration, camera_test,
        ' (Aravis ' + metadata['aravis_version'] + ')' if metadata['aravis_version'] else '', directory),
        flush=True)

    results = []
    interrupted = False
    try:
        for run in runs:
            if results and args.pause > 0:
                time.sleep(args.pause)
            print('Run {}/{}: {}'.format(run['index'], len(runs), describe_run(run)), flush=True)
            result = execute_run(run, camera_test, args, extra, logs_dir, echo=not args.quiet)
            results.append(result)
            write_reports(directory, metadata, results)
            summary = result['summary']
            if result['status'] == 'ok':
                cpu = (result['cpu'] or {}).get('percent')
                print('  {:.1f} frames/s, {:.1f} MiB/s, {} errors{}'.format(
                    summary['fps_mean'], summary['mibps_mean'], summary['errors'],
                    ', CPU {:.1f} %'.format(cpu) if cpu is not None else ''), flush=True)
            else:
                print('  failed: {}'.format(result['reason']), flush=True)
            for warning in result['warnings']:
                print('  warning: {}'.format(warning), flush=True)
    except KeyboardInterrupt:
        interrupted = True
        print('Interrupted', flush=True)

    write_reports(directory, metadata, results)
    print()
    print(format_markdown(metadata, results), end='')
    if interrupted:
        return 130
    return 0 if results and all(result['status'] == 'ok' for result in results) else 1


if __name__ == '__main__':
    sys.exit(main())
