#!/usr/bin/env python3
# SPDX-License-Identifier:Unlicense
"""Tests scripts/arv-usb-benchmark.py, the USB3 Vision throughput benchmark.

Usage: usbbenchmark.py SOURCE_ROOT [ARV_CAMERA_TEST]

Tests the parser of the script on outputs of arv-camera-test saved in
tests/data, on lines written with the formats of src/arvcameratest.c and on its
messages, and checks that src/arvcameratest.c still prints those formats and
takes the options that the script passes. Tests the reading of the RAW_IO state
of a run on debug lines written with the formats of src/arvuvstream.c, and
checks that the stream logs a warning wherever it turns RAW_IO off during an
acquisition. Tests the matrix, the commands and the dry run of the script, the
CPU time and the timeout of its process runner, and its reports. With
ARV_CAMERA_TEST, the arv-camera-test of the build, runs the script against the
Fake camera of Aravis, which lives in the process of arv-camera-test: no camera
and no network. Also checks the results table, the RAW_IO settings, the debug
output and the default USB mode on the USB page of the documentation.

The saved outputs come from an MSVC build of arv-camera-test-0.10 on Windows 11,
which prints CRLF line endings (a checkout may have LF ones; the parser takes
both):
- arv-camera-test-fakegv.txt: with arv-fake-gv-camera-0.10 -i 127.0.0.1,
  arv-camera-test-0.10 -n Aravis-Fake-GV01 --duration=3
  --gv-discovery-interface "Loopback Pseudo-Interface 1"
- arv-camera-test-fakegv-lossy.txt: the same with --gvsp-lost-ratio=2 for the
  fake camera, and --no-packet-resend for arv-camera-test
- arv-camera-test-fake.txt: arv-camera-test-0.10 -n Fake_1 --duration=3
"""

import argparse
import csv
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
import unittest.mock

SOURCE_ROOT = None
SCRIPT = None
CAMERA_TEST = None
benchmark = None

FAKEGV = 'arv-camera-test-fakegv.txt'
FAKEGV_LOSSY = 'arv-camera-test-fakegv-lossy.txt'
FAKE = 'arv-camera-test-fake.txt'

# A name that matches no program, so that the dry run prints it as it is
MISSING_CAMERA_TEST = 'missing-arv-camera-test'


def load_script(path):
    # No __pycache__ in the source tree
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('arv_usb_benchmark', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_source(*path):
    with open(os.path.join(SOURCE_ROOT, *path), 'rb') as f:
        return f.read().decode('utf-8')


def read_data(name):
    return read_source('tests', 'data', name)


def per_second_line(frames, mb, errors=0):
    """A per-second line of arv-camera-test, from the formats of
    periodic_task_cb () in src/arvcameratest.c. Python's % operator formats
    these conversions as C's printf does."""
    line = '%3d frame%s - %7.3g MiB/s' % (frames, 's/s' if frames > 1 else '/s ', mb)
    if errors > 0:
        line += ' - %d error%s' % (errors, 's' if errors > 1 else '')
    return line


def statistic_line(name, value):
    # g_print ("%-22s = %" G_GUINT64_FORMAT "\n", ...) in src/arvcameratest.c
    return '%-22s = %d' % (name, value)


# A C string literal, or a format macro of GLib between two of them
C_FORMAT_PART = r'"(?:[^"\\]|\\.)*"|G_\w+_(?:FORMAT|MODIFIER)'


def log_formats(source):
    """The calls of the debug output in SOURCE, the text of a C file: the macro,
    such as arv_info_stream, its position, and its format, with the adjacent
    string literals joined, as a format for Python's % operator, which formats
    these conversions as C's printf does."""
    formats = []
    for match in re.finditer(r'\b(arv_(?:warning|info|debug|trace)_\w+) \(\s*((?:{0})(?:\s*(?:{0}))*)'.format(
            C_FORMAT_PART), source):
        text = ''
        for part in re.findall(C_FORMAT_PART, match.group(2)):
            if part.startswith('"'):
                text += part[1:-1].replace('\\"', '"').replace('\\n', '\n')
            elif part.endswith('_FORMAT'):
                # An integer conversion, such as G_GUINT64_FORMAT
                text += 'd'
        # Without length modifiers: %zu is %u
        text = re.sub(r'%([-+ #0]*\d*(?:\.\d+)?)(?:hh|h|ll|l|z|j|t)?(?=[diuxXsgfc])', r'%\1', text)
        formats.append((match.group(1), match.start(), text))
    return formats


def format_args(text, string):
    """Values for the conversions of the Python format TEXT: STRING for %s, and
    distinct numbers for the others."""
    conversions = re.findall(r'%[-+ #0]*\d*(?:\.\d+)?([diuxXsgfc])', text.replace('%%', ''))
    return tuple(string if c == 's' else 1000 * (i + 1) for i, c in enumerate(conversions))


# The starts of the debug lines of Aravis: none, with the glyph of the level,
# which Aravis writes to a terminal, or its ASCII symbol and a timestamp
DEBUG_PREFIXES = ['', '[22:10:04.413] \U0001f178 stream> ', '[22:10:04.413] I stream> ',
                  '\U0001f186 stream-thread> ', '[22:10:04.413] W stream-thread> ']


def usb_output(*debug_lines):
    """An output of arv-camera-test for a USB3 Vision camera, with the debug lines
    DEBUG_LINES of its stream, as stderr interleaves them, before its frames."""
    lines = ["Looking for camera 'Example-Model 1-1234'",
             'vendor name            = Example',
             'model name             = Model 1',
             'uv bandwidth limit     = 380000000 [10000000..400000000]']
    lines += list(debug_lines)
    lines += [per_second_line(160, 248.8), per_second_line(161, 250.4)]
    lines += [statistic_line(name, value) for name, value in (('n_completed_buffers', 321), ('n_failures', 0),
                                                             ('n_underruns', 0), ('n_aborted', 0))]
    return '\r\n'.join(lines) + '\r\n'


def fake_output(text, returncode=0, timed_out=False):
    """A ProcessOutput for TEXT, as if a run had printed it."""
    output = benchmark.ProcessOutput()
    output.lines = [(float(i), line) for i, line in enumerate(text.splitlines())]
    output.data = [(line + '\n').encode('utf-8') for line in text.splitlines()]
    output.returncode = returncode
    output.timed_out = timed_out
    output.wall_time = 3.0
    return output


def interpreter():
    """The Python interpreter itself. On Windows, the python.exe of a virtual
    environment is a launcher that runs the interpreter in a child process, so
    that the CPU time of the launcher is not that of the interpreter."""
    if sys.platform == 'win32' and sys.prefix != sys.base_prefix:
        path = os.path.join(sys.base_exec_prefix, 'python.exe')
        if os.path.isfile(path):
            return path
    return sys.executable


def script_environment(**variables):
    """The environment of this test without ARV_DEBUG, with VARIABLES."""
    environment = {name: value for name, value in os.environ.items() if name.upper() != 'ARV_DEBUG'}
    environment.update(variables)
    return environment


def run_script(*arguments, environment=None):
    return subprocess.run([sys.executable, SCRIPT] + list(arguments), stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True,
                          errors='replace', timeout=60,
                          env=script_environment() if environment is None else environment)


def commands_of(stdout):
    return [line for line in stdout.splitlines() if line and not line.startswith('#')]


def table_cells(row):
    return [cell.strip() for cell in re.split(r'(?<!\\)\|', row.strip())[1:-1]]


class CaptureTest(unittest.TestCase):
    """Saved outputs of arv-camera-test."""

    def test_fake_gv(self):
        parsed = benchmark.parse_output(read_data(FAKEGV))
        self.assertEqual(parsed['camera_name'], 'Aravis-Fake-GV01')
        camera = parsed['camera']
        self.assertEqual((camera['vendor'], camera['model'], camera['serial']), ('Aravis', 'Fake', 'GV01'))
        self.assertEqual((camera['width'], camera['height'], camera['payload']), (512, 512, 262144))
        self.assertEqual((camera['horizontal_binning'], camera['vertical_binning']), (1, 1))
        self.assertEqual((camera['exposure_us'], camera['gain_db']), (10000, 0))
        self.assertEqual((camera['gv_n_stream_channels'], camera['gv_current_channel'],
                          camera['gv_packet_delay_ns'], camera['gv_packet_size']), (1, 0, 0, 1400))
        self.assertEqual([(s['frames'], s['mb'], s['errors']) for s in parsed['seconds']],
                         [(25, 6.55, 0), (26, 6.82, 0), (25, 6.55, 0)])
        statistics = parsed['statistics']
        self.assertEqual(len(statistics), 18)
        self.assertEqual(list(statistics)[:5],
                         ['n_completed_buffers', 'n_failures', 'n_underruns', 'n_timeouts', 'n_aborted'])
        self.assertEqual((statistics['n_completed_buffers'], statistics['n_failures'], statistics['n_underruns']),
                         (76, 0, 0))
        self.assertEqual(statistics['n_size_mismatch_errors'], 0)
        self.assertEqual(statistics['n_received_packets'], 14820)
        self.assertEqual(statistics['n_transferred_bytes'], 20044848)
        self.assertEqual(parsed['stream_type'], 'GigEVision')
        self.assertEqual((parsed['failures'], parsed['warnings'], parsed['control_lost']), ([], [], False))

        summary = benchmark.summarize(parsed, warmup=1)
        self.assertEqual((summary['seconds'], summary['measured_seconds']), (3, 2))
        self.assertEqual((summary['fps_mean'], summary['fps_min'], summary['fps_max']), (25.5, 25, 26))
        self.assertAlmostEqual(summary['mibps_mean'], (6.82 + 6.55) / 2 * 1e6 / 2 ** 20)
        self.assertAlmostEqual(summary['mibps_min'], 6.55 * 1e6 / 2 ** 20)
        self.assertEqual((summary['errors'], summary['completed'], summary['failures'], summary['underruns']),
                         (0, 76, 0, 0))

        everything = benchmark.summarize(parsed, warmup=0)
        self.assertEqual(everything['measured_seconds'], 3)
        self.assertAlmostEqual(everything['fps_mean'], 76 / 3)
        # A warm-up longer than the run leaves all the seconds in
        self.assertEqual(benchmark.summarize(parsed, warmup=5)['measured_seconds'], 3)

    def test_fake_gv_lossy(self):
        parsed = benchmark.parse_output(read_data(FAKEGV_LOSSY))
        self.assertEqual([(s['frames'], s['mb'], s['errors']) for s in parsed['seconds']],
                         [(15, 3.93, 9), (15, 3.93, 11), (21, 5.51, 5)])
        statistics = parsed['statistics']
        self.assertEqual((statistics['n_completed_buffers'], statistics['n_failures'],
                          statistics['n_missing_packets']), (51, 25, 2430))
        summary = benchmark.summarize(parsed)
        # arv-camera-test counts the failed buffers that it pops as errors
        self.assertEqual(summary['errors'], 25)
        self.assertEqual(summary['failures'], 25)
        self.assertEqual((summary['fps_mean'], summary['fps_min']), (18.0, 15))
        self.assertEqual(parsed['failures'], [])

    def test_fake(self):
        parsed = benchmark.parse_output(read_data(FAKE))
        self.assertEqual(parsed['camera_name'], 'Fake_1')
        self.assertEqual(parsed['camera']['serial'], '1')
        self.assertEqual(list(parsed['statistics']),
                         ['n_completed_buffers', 'n_failures', 'n_underruns', 'n_transferred_bytes', 'n_ignored_bytes'])
        self.assertEqual(sum(s['frames'] for s in parsed['seconds']), parsed['statistics']['n_completed_buffers'])
        self.assertIsNone(parsed['stream_type'])

    def test_line_endings(self):
        for name in (FAKEGV, FAKEGV_LOSSY, FAKE):
            with self.subTest(name=name):
                text = read_data(name)
                lf = text.replace('\r\n', '\n')
                crlf = lf.replace('\n', '\r\n')
                self.assertEqual(benchmark.parse_output(lf), benchmark.parse_output(crlf))
                self.assertEqual(benchmark.parse_output(text), benchmark.parse_output(lf))


class FormatTest(unittest.TestCase):
    """Lines in the formats of src/arvcameratest.c."""

    def test_per_second_lines(self):
        for frames in (0, 1, 2, 25, 999, 1000, 123456):
            for mb in (0.0, 0.000123, 0.262144, 6.5536, 99.95, 380.4, 999.5, 1234.5, 98765.4):
                for errors in (0, 1, 7):
                    line = per_second_line(frames, mb, errors)
                    seconds = benchmark.parse_output(line)['seconds']
                    self.assertEqual(len(seconds), 1, line)
                    self.assertEqual(seconds[0], {'frames': frames, 'mb': float('%.3g' % mb), 'errors': errors},
                                     line)

    def test_usb3vision_output(self):
        statistics = [('n_completed_buffers', 321), ('n_failures', 1), ('n_underruns', 2), ('n_aborted', 0),
                      ('n_transferred_bytes', 499219200), ('n_ignored_bytes', 1555200)]
        lines = ["Looking for camera 'Example-Model 1-1234'",
                 'vendor name            = Example',
                 'model name             = Model 1',
                 'device serial number   = 1234',
                 'image width            = 1440',
                 'image height           = 1080',
                 'exposure               = 1000 µs',
                 'gain                   = 0 dB',
                 'payload                = 1555200 bytes',
                 'uv bandwidth limit     = 380000000 [10000000..400000000]',
                 per_second_line(158, 245.7),
                 per_second_line(160, 248.8),
                 per_second_line(161, 250.4, 1)]
        lines += [statistic_line(name, value) for name, value in statistics]
        parsed = benchmark.parse_output('\n'.join(lines))
        self.assertEqual(parsed['camera_name'], 'Example-Model 1-1234')
        self.assertEqual(parsed['camera']['model'], 'Model 1')
        self.assertEqual(parsed['camera']['payload'], 1555200)
        self.assertEqual(parsed['camera']['uv_bandwidth_limit'], '380000000 [10000000..400000000]')
        self.assertEqual(list(parsed['statistics'].items()), statistics)
        self.assertEqual(parsed['stream_type'], 'USB3Vision')
        summary = benchmark.summarize(parsed)
        self.assertEqual((summary['fps_mean'], summary['fps_min'], summary['errors']), (160.5, 160, 1))
        self.assertAlmostEqual(summary['mibps_mean'], (249 + 250) / 2 * 1e6 / 2 ** 20)

    def test_messages(self):
        cases = [
            ("No camera found: Device 'Missing' not found", ["no camera found: Device 'Missing' not found"]),
            ('No camera found', ['no camera found']),
            ('Failed to configure the device: Feature not found',
             ['failed to configure the camera: Feature not found']),
            ('Failed to read the current device configuration: Timeout',
             ['failed to read the camera configuration: Timeout']),
            ('Failed to start acquisition (Stream not available)',
             ['failed to start the acquisition: Stream not available']),
            ("Can't create stream thread: Out of memory", ['failed to create the stream: Out of memory']),
            ("Can't create stream thread", ['failed to create the stream']),
            ('Option parsing failed: Unknown option --foo',
             ['arv-camera-test rejected an option: Unknown option --foo']),
            ('Invalid USB device I/O mode', ['arv-camera-test rejected an option: Invalid USB device I/O mode']),
        ]
        for line, failures in cases:
            with self.subTest(line=line):
                self.assertEqual(benchmark.parse_output(line + '\r\n')['failures'], failures)

        parsed = benchmark.parse_output('Control lost\nFailed to make stream thread high priority\n')
        self.assertTrue(parsed['control_lost'])
        self.assertEqual(parsed['failures'], [])
        self.assertEqual(len(parsed['warnings']), 1)
        self.assertTrue(parsed['warnings'][0].startswith('high priority not applied'))
        self.assertTrue(benchmark.parse_output('Failed to make stream thread realtime')['warnings'][0]
                        .startswith('realtime not applied'))

    def test_ignored_lines(self):
        # Debug output, chunk values (--chunks), and anything else
        lines = ['[12:00:00.000] \U0001f178 stream> [UvStream::start] Start USB3Vision stream',
                 'ChunkExposureTime = 1000', 'something = 12', 'frames/s', '25 frames per second', '',
                 ' 25 frames/s -    6.55 MB/s']
        parsed = benchmark.parse_output('\n'.join(lines))
        self.assertEqual((parsed['seconds'], parsed['statistics'], parsed['camera']), ([], {}, {}))
        self.assertEqual((parsed['failures'], parsed['warnings'], parsed['camera_name']), ([], [], None))


class SourceTest(unittest.TestCase):
    """The output formats and options of arv-camera-test that the script relies on."""

    def test_output_formats(self):
        source = read_source('src', 'arvcameratest.c')
        fragments = [
            '"%3d frame%s - %7.3g MiB/s"',
            '" - %d error%s\\n"',
            # The per-second throughput counts 10^6 bytes, which the script converts to MiB
            '(double) data->transferred / 1e6',
            '"%-22s = %" G_GUINT64_FORMAT "\\n"',
            '"Looking for camera \'%s\'\\n"',
            '"vendor name            = %s\\n"',
            '"model name             = %s\\n"',
            '"device serial number   = %s\\n"',
            '"payload                = %d bytes\\n"',
            '"Control lost\\n"',
            '"Failed to make stream thread high priority\\n"',
            '"No camera found%s%s\\n"',
            '"Failed to start acquisition (%s)\\n"',
        ]
        for fragment in fragments:
            self.assertIn(fragment, source, 'src/arvcameratest.c changed: update the parser of '
                          'scripts/arv-usb-benchmark.py')

    def test_options(self):
        source = read_source('src', 'arvcameratest.c')
        for option in benchmark.CAMERA_TEST_OPTIONS:
            self.assertRegex(source, r'"{}",\s*\''.format(re.escape(option)))
        self.assertRegex(source, r'"name",\s*\'n\'')
        self.assertRegex(source, r'g_strcmp0 \(arv_option_uv_usb_mode, "async"\)')

    def test_statistics(self):
        # The statistics of the CSV columns are those of the USB3 Vision stream, and
        # parse_output () tells the USB3 Vision and GigE Vision streams apart by theirs
        uv = re.findall(r'arv_stream_declare_info \(ARV_STREAM \(uv_stream\), "(\w+)"',
                        read_source('src', 'arvuvstream.c'))
        gv = re.findall(r'arv_stream_declare_info \(ARV_STREAM \(gv_stream\), "(\w+)"',
                        read_source('src', 'arvgvstream.c'))
        self.assertEqual(set(benchmark.STATISTICS_COLUMNS) - set(uv), set())
        self.assertIn('n_aborted', uv)
        self.assertNotIn('n_received_packets', uv)
        self.assertIn('n_received_packets', gv)

    def test_raw_io_off_warnings(self):
        # Wherever the stream turns RAW_IO off during an acquisition, it sets
        # raw_io_off_needed after a warning of the stream thread, which
        # ARV_DEBUG=stream-thread:1 shows: the script must read these warnings,
        # and only these, as RAW_IO going off
        source = read_source('src', 'arvuvstream.c')
        warnings = [(position, text) for macro, position, text in log_formats(source)
                    if macro == 'arv_warning_stream_thread']
        assignments = [match.start() for match in re.finditer(r'\braw_io_off_needed = TRUE;', source)]
        message = 'src/arvuvstream.c changed: update the RAW_IO parser of scripts/arv-usb-benchmark.py'
        self.assertTrue(assignments, message)
        turning_off = set()
        for assignment in assignments:
            before = [(position, text) for position, text in warnings if position < assignment]
            self.assertTrue(before, message)
            turning_off.add(before[-1][0])
        for position, text in warnings:
            if position in turning_off:
                self.assertRegex(text, benchmark.RAW_IO_DROPPED_RE, message)
            else:
                self.assertNotRegex(text, benchmark.RAW_IO_DROPPED_RE, message)


class RawIoTest(unittest.TestCase):
    """The RAW_IO state of a run, from debug lines in the formats of src/arvuvstream.c."""

    def setUp(self):
        self.source = read_source('src', 'arvuvstream.c')
        self.formats = log_formats(self.source)

    def start_lines(self):
        """The info lines of the acquisition start, in each format and with each
        RAW_IO state of src/arvuvstream.c, with that state and the RAW_IO maximum
        that they give."""
        message = 'src/arvuvstream.c changed: update the RAW_IO parser of scripts/arv-usb-benchmark.py'
        starts = [text for macro, position, text in self.formats
                  if macro == 'arv_info_stream' and text.startswith('Transfer size ')]
        on = [text for text in starts if '%s' not in text]
        off = [text for text in starts if '%s' in text]
        states = re.findall(r'raw_io_state = "(off\b[^"]*)"', self.source)
        self.assertTrue(on and off and states, message)
        lines = []
        for text in on:
            # The RAW_IO maximum is the last value of the line
            values = format_args(text, None)
            lines.append((text % values, 'on', values[-1]))
        for text in off:
            for state in states:
                lines.append((text % format_args(text, state), state, None))
        return lines

    def test_start(self):
        for line, state, maximum in self.start_lines():
            for prefix in DEBUG_PREFIXES:
                with self.subTest(line=prefix + line):
                    parsed = benchmark.parse_output(usb_output(prefix + line))
                    self.assertEqual(benchmark.raw_io_state(parsed), state)
                    self.assertEqual(benchmark.raw_io_maximum(parsed), maximum)
                    # The other lines are read as without debug output
                    self.assertEqual(parsed['stream_type'], 'USB3Vision')
                    self.assertEqual(len(parsed['seconds']), 2)
                    self.assertEqual(parsed['statistics']['n_completed_buffers'], 321)
        parsed = benchmark.parse_output(usb_output())
        self.assertEqual((benchmark.raw_io_state(parsed), benchmark.raw_io_maximum(parsed)), (None, None))

    def test_turned_off(self):
        # The messages of the stream thread about RAW_IO: those that say that
        # the stream goes on without it, which SourceTest checks, and the others
        on, maximum = next((line, maximum) for line, state, maximum in self.start_lines() if state == 'on')
        thread = [text for macro, position, text in self.formats
                  if macro.endswith('_stream_thread') and 'RAW_IO' in text]
        self.assertTrue(any(benchmark.RAW_IO_DROPPED_RE.search(text) for text in thread))
        for text in thread:
            line = text % format_args(text, 'LIBUSB_ERROR_IO')
            for prefix in DEBUG_PREFIXES:
                with self.subTest(line=prefix + line):
                    parsed = benchmark.parse_output(usb_output(on, prefix + line))
                    self.assertEqual(benchmark.raw_io_state(parsed),
                                     'on, then off' if benchmark.RAW_IO_DROPPED_RE.search(text) else 'on')
                    self.assertEqual(benchmark.raw_io_maximum(parsed), maximum)


class MatrixTest(unittest.TestCase):

    def test_parse_size(self):
        cases = {'65536': 65536, '64KiB': 65536, '64kib': 65536, '64K': 65536, '256k': 262144,
                 '1MiB': 1048576, '1M': 1048576, ' 4 MiB ': 4194304, 'default': None, 'DEFAULT': None, '0': None,
                 '2147483647': 2147483647}
        for text, size in cases.items():
            self.assertEqual(benchmark.parse_size(text), size, text)
        for text in ('64KB', '1.5MiB', '-1', 'abc', '', '2048MiB', '64 KiB,'):
            with self.assertRaises(ValueError, msg=text):
                benchmark.parse_size(text)

    def test_format_size(self):
        cases = [(65536, '64 KiB'), (262144, '256 KiB'), (1048576, '1 MiB'), (1536 * 1024, '1536 KiB'),
                 (100000, '100000 B'), (None, 'default')]
        for size, text in cases:
            self.assertEqual(benchmark.format_size(size), text)

    def test_matrix(self):
        runs = benchmark.build_matrix(['A', 'B'], [65536, 262144, 1048576], ['off', 'on'], ['default', 'high'])
        self.assertEqual(len(runs), 24)
        self.assertEqual([run['index'] for run in runs], list(range(1, 25)))
        self.assertEqual(runs[0], {'index': 1, 'camera': 'A', 'transfer_size': 65536, 'raw_io': 'off',
                                   'priority': 'default'})
        self.assertEqual(runs[11], {'index': 12, 'camera': 'A', 'transfer_size': 1048576, 'raw_io': 'on',
                                    'priority': 'high'})
        self.assertEqual({(run['transfer_size'], run['raw_io'], run['priority']) for run in runs[12:]},
                         {(size, raw_io, priority) for size in (65536, 262144, 1048576)
                          for raw_io in ('off', 'on') for priority in ('default', 'high')})
        self.assertEqual([run['camera'] for run in benchmark.build_matrix([], [None], ['off'], ['default'])],
                         [None])

    def test_command(self):
        run = {'index': 1, 'camera': 'Vendor-Model-1', 'transfer_size': 262144, 'raw_io': 'on', 'priority': 'high'}
        self.assertEqual(benchmark.build_command('arv-camera-test-0.10', run),
                         ['arv-camera-test-0.10', '-n', 'Vendor-Model-1', '--usb-mode=async',
                          '--usb-maximum-transfer-size=262144', '--duration=60', '--high-priority'])
        run = dict(run, camera=None, transfer_size=None, priority='realtime')
        self.assertEqual(benchmark.build_command('t', run, 'sync', 5, ['--features=Width=640']),
                         ['t', '--usb-mode=sync', '--duration=5', '--realtime', '--features=Width=640'])
        self.assertEqual(benchmark.build_command('t', dict(run, priority='default')),
                         ['t', '--usb-mode=async', '--duration=60'])

    def test_environment(self):
        # Every run sets ARV_UV_RAW_IO: none depends on the default of Aravis,
        # which leaves RAW_IO off when it is unset, or on the environment
        self.assertEqual(sorted(benchmark.RAW_IO_SETTINGS), ['off', 'on'])
        debug = 'stream:2,stream-thread:1'
        self.assertEqual(benchmark.RAW_IO_DEBUG, debug)
        base = {'PATH': 'x', 'ARV_UV_RAW_IO': 'previous'}
        self.assertEqual(benchmark.build_environment(base, 'off'),
                         {'PATH': 'x', 'ARV_UV_RAW_IO': '0', 'ARV_DEBUG': debug})
        self.assertEqual(benchmark.build_environment(base, 'on'),
                         {'PATH': 'x', 'ARV_UV_RAW_IO': '1', 'ARV_DEBUG': debug})
        self.assertEqual(base, {'PATH': 'x', 'ARV_UV_RAW_IO': 'previous'})
        self.assertEqual(list(benchmark.run_variables({}, 'off').items()),
                         [('ARV_UV_RAW_IO', '0'), ('ARV_DEBUG', debug)])
        self.assertEqual(benchmark.run_variables({'ARV_DEBUG': ''}, 'on'), {'ARV_UV_RAW_IO': '1', 'ARV_DEBUG': debug})
        # The ARV_DEBUG of the environment of the script is kept
        self.assertEqual(benchmark.run_variables({'ARV_DEBUG': 'all:1'}, 'on'), {'ARV_UV_RAW_IO': '1'})
        self.assertEqual(benchmark.build_environment({'PATH': 'x', 'ARV_DEBUG': 'all:1'}, 'on'),
                         {'PATH': 'x', 'ARV_DEBUG': 'all:1', 'ARV_UV_RAW_IO': '1'})

    def test_format_command(self):
        command = ['arv-camera-test-0.10', '-n', 'Vendor Model', '--duration=60']
        quoted = '"Vendor Model"' if sys.platform == 'win32' else "'Vendor Model'"
        self.assertEqual(benchmark.format_command(command, {'ARV_UV_RAW_IO': '0'}),
                         'ARV_UV_RAW_IO=0 arv-camera-test-0.10 -n {} --duration=60'.format(quoted))
        self.assertEqual(benchmark.format_command(command, benchmark.run_variables({}, 'on')),
                         'ARV_UV_RAW_IO=1 ARV_DEBUG=stream:2,stream-thread:1 arv-camera-test-0.10 -n {} '
                         '--duration=60'.format(quoted))


class DryRunTest(unittest.TestCase):

    def check_protocol_commands(self, commands, camera='"Cam A"' if sys.platform == 'win32' else "'Cam A'"):
        self.assertEqual(len(commands), 12)
        for command in commands:
            self.assertRegex(command, r'^ARV_UV_RAW_IO=[01] ARV_DEBUG=stream:2,stream-thread:1 {} -n {} '
                             r'--usb-mode=async --usb-maximum-transfer-size=(65536|262144|1048576) --duration=60'
                             r'( --high-priority)?$'.format(MISSING_CAMERA_TEST, re.escape(camera)))
        for size in ('65536', '262144', '1048576'):
            self.assertEqual(sum('--usb-maximum-transfer-size={} '.format(size) in c for c in commands), 4)
        self.assertEqual(sum(c.startswith('ARV_UV_RAW_IO=0 ') for c in commands), 6)
        self.assertEqual(sum(c.startswith('ARV_UV_RAW_IO=1 ') for c in commands), 6)
        self.assertEqual(sum(c.endswith(' --high-priority') for c in commands), 6)
        self.assertEqual(len(set(commands)), 12)

    def test_protocol(self):
        result = run_script('--dry-run', '-n', 'Cam A', '--raw-io', 'off,on', '--camera-test', MISSING_CAMERA_TEST)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_protocol_commands(commands_of(result.stdout))
        self.assertIn('# 12 runs of 60 s', result.stdout)
        self.assertIn('and ARV_DEBUG, for the RAW_IO state in the output', result.stdout)

    def test_debug_variable(self):
        # An ARV_DEBUG of the environment of the script is kept, with a note
        result = run_script('--dry-run', '-n', 'Cam A', '--raw-io', 'off,on', '--camera-test', MISSING_CAMERA_TEST,
                            environment=script_environment(ARV_DEBUG='all:1'))
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = commands_of(result.stdout)
        self.assertEqual(len(commands), 12)
        self.assertTrue(all(re.match(r'^ARV_UV_RAW_IO=[01] {} '.format(MISSING_CAMERA_TEST), c) for c in commands))
        self.assertIn('# ARV_DEBUG is set: the output of a run gives its RAW_IO state only if it includes '
                      'stream:2,stream-thread:1.', result.stdout)

    def test_default_raw_io(self):
        result = run_script('--dry-run', '-n', 'Cam A', '--camera-test', MISSING_CAMERA_TEST)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = commands_of(result.stdout)
        if sys.platform == 'win32':
            self.check_protocol_commands(commands)
        else:
            # RAW_IO is a WinUSB setting
            self.assertEqual(len(commands), 6)
            self.assertTrue(all(c.startswith('ARV_UV_RAW_IO=0 ARV_DEBUG=stream:2,stream-thread:1 ') for c in commands))

    def test_several_cameras(self):
        result = run_script('--dry-run', '-n', 'A', '-n', 'B', '--raw-io', 'off,on', '--camera-test',
                            MISSING_CAMERA_TEST)
        self.assertEqual(result.returncode, 0, result.stderr)
        commands = commands_of(result.stdout)
        self.assertEqual(len(commands), 24)
        self.assertTrue(all(' -n A ' in c for c in commands[:12]))
        self.assertTrue(all(' -n B ' in c for c in commands[12:]))

    def test_custom_matrix(self):
        result = run_script('--dry-run', '--transfer-sizes', 'default,128KiB', '--raw-io', 'on',
                            '--priorities', 'realtime', '--duration', '5', '--usb-mode', 'sync',
                            '--camera-test', MISSING_CAMERA_TEST, '--', '--features=Width=640')
        self.assertEqual(result.returncode, 0, result.stderr)
        variables = 'ARV_UV_RAW_IO=1 ARV_DEBUG=stream:2,stream-thread:1 '
        self.assertEqual(commands_of(result.stdout), [
            variables + MISSING_CAMERA_TEST + ' --usb-mode=sync --duration=5 --realtime --features=Width=640',
            variables + MISSING_CAMERA_TEST + ' --usb-mode=sync --usb-maximum-transfer-size=131072 '
            '--duration=5 --realtime --features=Width=640'])
        self.assertIn('# No camera given', result.stdout)

    def test_invalid_arguments(self):
        # No setting leaves ARV_UV_RAW_IO unset, as 'auto' would, for the default of Aravis to decide
        for arguments in (['--transfer-sizes', '64KB'], ['--transfer-sizes', '4096MiB'],
                          ['--transfer-sizes', '64KiB,'], ['--raw-io', 'maybe'], ['--raw-io', 'auto'],
                          ['--priorities', 'low'], ['--usb-mode', 'bulk'], ['--duration', '0'], ['--warmup', '-1']):
            with self.subTest(arguments=arguments):
                result = run_script('--dry-run', *arguments)
                self.assertEqual(result.returncode, 2, result.stdout)
                self.assertIn('error:', result.stderr)
                self.assertEqual(commands_of(result.stdout), [])

    def test_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, 'results')
            result = run_script('--dry-run', '-o', output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(os.path.exists(output))

    def test_missing_camera_test(self):
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, 'results')
            result = run_script('--camera-test', MISSING_CAMERA_TEST, '-o', output)
            self.assertEqual(result.returncode, 2)
            self.assertIn(MISSING_CAMERA_TEST + ' not found', result.stderr)
            self.assertFalse(os.path.exists(output))


class RunnerTest(unittest.TestCase):

    # Uses 0.8 s of CPU time, and prints three per-second lines
    BUSY_CHILD = textwrap.dedent('''
        import time

        def burn(seconds):
            end = time.process_time() + seconds
            while time.process_time() < end:
                pass

        burn(0.2)
        for i in range(3):
            print("%3d frame%s - %7.3g MiB/s" % (25, "s/s", 6.55), flush=True)
            if i < 2:
                burn(0.3)
        ''')

    def test_cpu_time(self):
        output = benchmark.run_process([interpreter(), '-c', self.BUSY_CHILD], timeout=60)
        self.assertEqual(output.returncode, 0, output.text)
        self.assertFalse(output.timed_out)
        self.assertEqual(len(output.per_second_times), 3)
        self.assertEqual(len(benchmark.parse_output(output.text)['seconds']), 3)
        self.assertIsNotNone(output.cpu_time)
        self.assertGreaterEqual(output.cpu_time, 0.7)
        self.assertLessEqual(output.cpu_time, output.wall_time * (os.cpu_count() or 1) + 0.1)
        usage = benchmark.cpu_usage(output, warmup=1)
        live = sys.platform == 'win32' or sys.platform.startswith('linux')
        self.assertEqual(usage['method'], 'streaming' if live else 'process')
        self.assertGreater(usage['percent'], 0)
        self.assertLess(usage['percent'], 150)
        self.assertEqual(usage['seconds'], output.cpu_time)

        output = benchmark.run_process([interpreter(), '-c', self.BUSY_CHILD], timeout=60, cpu=False)
        self.assertIsNone(output.cpu_time)
        self.assertEqual(output.cpu_samples, [])

    def test_buffered_output(self):
        # Lines that arrive at once say nothing about the streaming time
        output = fake_output('\n'.join(per_second_line(25, 6.55) for i in range(3)))
        output.cpu_samples = [(2.5, 1.0), (2.5, 1.0), (2.5, 1.0)]
        output.cpu_time = 0.3
        usage = benchmark.cpu_usage(output)
        self.assertEqual((usage['method'], usage['seconds'], usage['wall_seconds']), ('process', 0.3, 3.0))
        self.assertAlmostEqual(usage['percent'], 10.0)
        output.cpu_samples = [(1.0, 0.1), (2.0, 0.15), (3.0, 0.2)]
        usage = benchmark.cpu_usage(output)
        self.assertEqual(usage['method'], 'streaming')
        self.assertAlmostEqual(usage['percent'], 5.0)
        self.assertAlmostEqual(benchmark.cpu_usage(output, warmup=2)['percent'], 5.0)

    def test_timeout(self):
        output = benchmark.run_process([interpreter(), '-c', 'import time; time.sleep(60)'], timeout=2)
        self.assertTrue(output.timed_out)
        self.assertNotEqual(output.returncode, 0)
        self.assertLess(output.wall_time, 30)

    def test_evaluate(self):
        good = read_data(FAKE)
        cases = [
            (fake_output(good), ('ok', None)),
            (fake_output(good, timed_out=True, returncode=1), ('failed', 'killed after 5 s')),
            (fake_output(good + 'Control lost\n'), ('failed', 'control lost')),
            (fake_output(good, returncode=3), ('failed', 'exit code 3')),
            (fake_output("Looking for camera 'X'\nNo camera found: Device 'X' not found\n"),
             ('failed', "no camera found: Device 'X' not found")),
            (fake_output('vendor name            = Aravis\n'), ('failed', 'no per-second line in the output')),
            (fake_output(per_second_line(25, 6.55)), ('failed', 'no stream statistics in the output')),
            (fake_output(per_second_line(0, 0) + '\n' + statistic_line('n_completed_buffers', 0)),
             ('failed', 'no frame received')),
        ]
        for output, expected in cases:
            parsed = benchmark.parse_output(output.text)
            summary = benchmark.summarize(parsed)
            self.assertEqual(benchmark.evaluate(parsed, summary, output, 5), expected)


class VersionTest(unittest.TestCase):

    def check(self, outputs, version, calls):
        results = [subprocess.CompletedProcess(['t', '--version'], code, stdout) for code, stdout in outputs]
        with unittest.mock.patch.object(benchmark.subprocess, 'run', side_effect=results) as run:
            self.assertEqual(benchmark.camera_test_version('t'), version)
        self.assertEqual(run.call_count, calls)

    def test_version(self):
        self.check([(0, b'0.9.3\r\n')], '0.9.3', 1)
        # Empty output, as GIO sometimes loses it on Windows: try again
        self.check([(0, b''), (0, b'0.9.3\n')], '0.9.3', 2)
        self.check([(0, b'')] * 3, None, 3)
        self.check([(1, b'')], None, 1)
        self.check([(0, b'Option parsing failed\n')], None, 1)


class ReportTest(unittest.TestCase):

    def make_results(self):
        runs = benchmark.build_matrix(['Aravis-Fake-GV01'], [65536], ['off', 'on'], ['default', 'high'])
        texts = [read_data(FAKEGV), read_data(FAKEGV_LOSSY),
                 "Looking for camera 'Aravis-Fake-GV01'\nNo camera found: Device not found\n",
                 read_data(FAKE).replace('Fake', 'Fake | Test')]
        results = []
        for run, text in zip(runs, texts):
            command = benchmark.build_command('arv-camera-test-0.10', run)
            output = fake_output(text)
            output.cpu_time = 0.6
            results.append(benchmark.make_result(run, command, output, benchmark.run_variables({}, run['raw_io']),
                                                 timeout=120, log='logs/run-{:02d}.log'.format(run['index'])))
        return results

    def metadata(self, environment=None):
        args = argparse.Namespace(controller='Example | xHCI', notes=['a note'], name=['Aravis-Fake-GV01'],
                                  usb_mode='async', duration=60, warmup=1, transfer_sizes=[65536],
                                  raw_io=['off', 'on'], priorities=['default', 'high'], no_cpu=False)
        return benchmark.collect_metadata(args, None, ['--features=Width=640'], {} if environment is None else
                                          environment)

    def test_results(self):
        results = self.make_results()
        self.assertEqual([r['status'] for r in results], ['ok', 'ok', 'failed', 'ok'])
        self.assertEqual(results[2]['reason'], 'no camera found: Device not found')
        debug = 'stream:2,stream-thread:1'
        self.assertEqual(results[0]['environment'], {'ARV_UV_RAW_IO': '0', 'ARV_DEBUG': debug})
        self.assertEqual(results[1]['environment'], {'ARV_UV_RAW_IO': '0', 'ARV_DEBUG': debug})
        self.assertEqual(results[2]['environment'], {'ARV_UV_RAW_IO': '1', 'ARV_DEBUG': debug})
        # No RAW_IO state in these outputs, and no USB3 Vision stream to warn about
        self.assertEqual([(r['raw_io_state'], r['raw_io_maximum']) for r in results], [(None, None)] * 4)
        self.assertEqual(results[0]['seconds'][0], {'frames': 25, 'mb': 6.55, 'errors': 0, 'time': None})
        # Not USB3 Vision streams: the USB settings did nothing
        self.assertEqual(len(results[0]['warnings']), 1)
        self.assertIn('GigEVision', results[0]['warnings'][0])
        self.assertEqual(results[2]['warnings'], [])
        self.assertEqual(len(results[3]['warnings']), 1)
        cpu = results[0]['cpu']
        self.assertEqual((cpu['method'], cpu['seconds'], cpu['wall_seconds']), ('process', 0.6, 3.0))
        self.assertAlmostEqual(cpu['percent'], 20.0)

    def test_reports(self):
        results = self.make_results()
        metadata = self.metadata()
        with tempfile.TemporaryDirectory() as directory:
            benchmark.write_reports(directory, metadata, results)

            with open(os.path.join(directory, 'results.json'), encoding='utf-8') as f:
                data = json.load(f)
            self.assertEqual(data['format'], 1)
            self.assertEqual(data['metadata']['controller'], 'Example | xHCI')
            self.assertEqual(len(data['runs']), 4)
            self.assertEqual(data['runs'][1]['statistics']['n_failures'], 25)

            with open(os.path.join(directory, 'results.csv'), newline='', encoding='utf-8') as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 4)
            self.assertEqual([row['status'] for row in rows], ['ok', 'ok', 'failed', 'ok'])
            self.assertEqual((rows[0]['transfer_size'], rows[0]['raw_io'], rows[0]['priority']),
                             ('65536', 'off', 'default'))
            self.assertEqual((rows[0]['fps_mean'], rows[0]['n_completed_buffers'], rows[0]['n_received_packets']),
                             ('25.5', '76', '14820'))
            self.assertEqual((rows[1]['errors'], rows[1]['n_failures']), ('25', '25'))
            self.assertEqual(rows[3]['n_received_packets'], '')
            self.assertTrue(rows[2]['command'].startswith('ARV_UV_RAW_IO=1 ARV_DEBUG=stream:2,stream-thread:1 '
                                                          'arv-camera-test-0.10 -n'))
            self.assertEqual((rows[2]['raw_io_state'], rows[2]['raw_io_maximum']), ('', ''))

            with open(os.path.join(directory, 'results.md'), encoding='utf-8') as f:
                markdown = f.read()
            lines = markdown.splitlines()
            header = '| ' + ' | '.join(benchmark.MARKDOWN_COLUMNS) + ' |'
            start = lines.index(header)
            self.assertEqual(table_cells(lines[start + 1]), ['---'] * len(benchmark.MARKDOWN_COLUMNS))
            rows = [table_cells(line) for line in lines[start + 2:start + 6]]
            for row in rows:
                self.assertEqual(len(row), len(benchmark.MARKDOWN_COLUMNS))
            self.assertEqual(lines[start + 6], '')
            # (6.82 + 6.55) / 2 MB/s is 6.375 MiB/s
            self.assertEqual(rows[0][:9], ['Aravis Fake', metadata['os'], 'Example \\| xHCI', '64 KiB', 'off',
                                           'default', '25.5 (25)', '6.4', '20.0'])
            self.assertEqual(rows[0][9:], ['76', '0', '0', '0'])
            self.assertEqual(rows[1][6:], ['18.0 (15)', '4.5', '20.0', '51', '25', '25', '0'])
            self.assertEqual(rows[2], ['Aravis-Fake-GV01', metadata['os'], 'Example \\| xHCI', '64 KiB', 'on',
                                       'default', 'failed', 'n/a', 'n/a', 'n/a', 'n/a', 'n/a', 'n/a'])
            self.assertEqual(rows[3][0], 'Aravis Fake \\| Test')
            self.assertIn('- Run 3 (Aravis-Fake-GV01, 64 KiB, RAW_IO on, default priority): '
                          'no camera found: Device not found', lines)
            self.assertIn('- Note: a note', lines)
            self.assertIn('--duration=60 --features=Width=640`, with ARV_UV_RAW_IO=0 for RAW_IO off, 1 for on, '
                          'and ARV_DEBUG=stream:2,stream-thread:1', markdown)
            self.assertNotIn('RAW_IO maximum', markdown)

        # The ARV_DEBUG of the environment of the script
        metadata = self.metadata({'ARV_DEBUG': 'all:1'})
        self.assertEqual(metadata['arv_debug'], 'all:1')
        self.assertIn('for on, and ARV_DEBUG=all:1\n', benchmark.format_markdown(metadata, results))

    def test_raw_io(self):
        # The RAW_IO state that the output of a run gives, and its setting
        on = ('[22:10:04.413] I stream> Transfer size 1048576 bytes (maximum 1048576), packet size 1024 bytes, '
              'RAW_IO on (maximum 2097152 bytes)')
        dropped = ('[22:10:05.000] W stream-thread> Reads keep failing with RAW_IO on (LIBUSB_ERROR_IO) while the '
                   'camera is there: going on without RAW_IO for this acquisition')
        off = ('[22:10:04.413] I stream> Transfer size 1048576 bytes (maximum 1048576), packet size 1024 bytes, '
               'RAW_IO off ({})')
        cases = [
            ('on', [on], 'on', 'on', 2097152, None),
            ('on', [on, dropped], 'on, then off', 'on, then off', 2097152,
             'RAW_IO went off during the run, which measured RAW_IO off in part'),
            ('on', [off.format('not available')], 'off (not available)', 'on, but off (not available)', None,
             'RAW_IO was off (not available): the run measured RAW_IO off'),
            ('on', [], None, 'on', None, 'the output does not give the RAW_IO state: leave ARV_DEBUG unset, or '
             'include stream:2,stream-thread:1 in it'),
            ('off', [off.format('ARV_UV_RAW_IO')], 'off (ARV_UV_RAW_IO)', 'off', None, None),
            ('off', [on], 'on', 'off, but on', 2097152, 'RAW_IO was on, though ARV_UV_RAW_IO=0'),
            ('off', [], None, 'off', None, None),
        ]
        results = []
        for index, (raw_io, debug, state, label, maximum, warning) in enumerate(cases):
            run = {'index': index + 1, 'camera': 'Example-Model 1-1234', 'transfer_size': 1048576, 'raw_io': raw_io,
                   'priority': 'default'}
            result = benchmark.make_result(run, benchmark.build_command('arv-camera-test-0.10', run),
                                           fake_output(usb_output(*debug)), benchmark.run_variables({}, raw_io))
            with self.subTest(case=index + 1):
                self.assertEqual(result['status'], 'ok')
                self.assertEqual((result['raw_io_state'], result['raw_io_maximum']), (state, maximum))
                self.assertEqual(benchmark.raw_io_label(result), label)
                self.assertEqual(result['warnings'], [warning] if warning else [])
            results.append(result)

        metadata = self.metadata()
        with tempfile.TemporaryDirectory() as directory:
            benchmark.write_reports(directory, metadata, results)
            with open(os.path.join(directory, 'results.csv'), newline='', encoding='utf-8') as f:
                rows = list(csv.DictReader(f))
            self.assertEqual([(row['raw_io'], row['raw_io_state'], row['raw_io_maximum']) for row in rows],
                             [(case[0], case[2] or '', str(case[4] or '')) for case in cases])
            with open(os.path.join(directory, 'results.md'), encoding='utf-8') as f:
                lines = f.read().splitlines()
            start = lines.index('| ' + ' | '.join(benchmark.MARKDOWN_COLUMNS) + ' |')
            column = benchmark.MARKDOWN_COLUMNS.index('RAW_IO')
            self.assertEqual([table_cells(line)[column] for line in lines[start + 2:start + 2 + len(cases)]],
                             [label for raw_io, debug, state, label, maximum, warning in cases])
            self.assertIn('- RAW_IO maximum transfer size of the stream endpoint: Example Model 1: 2097152 bytes',
                          lines)
            self.assertIn('- Run 2 (Example-Model 1-1234, 1 MiB, RAW_IO on, default priority): RAW_IO went off during '
                          'the run, which measured RAW_IO off in part', lines)


class FakeCameraTest(unittest.TestCase):
    """The script with the arv-camera-test of the build, on the Fake camera."""

    def test_fake_camera(self):
        if CAMERA_TEST is None:
            self.skipTest('no arv-camera-test given')
        with tempfile.TemporaryDirectory() as directory:
            output = os.path.join(directory, 'results')
            environment = script_environment(ARV_UV_RAW_IO='1')
            result = subprocess.run([sys.executable, SCRIPT, '--camera-test', CAMERA_TEST, '-n', 'Fake_1',
                                     '--duration', '1', '--warmup', '0', '--transfer-sizes', '64KiB',
                                     '--raw-io', 'off', '--priorities', 'default,high', '--pause', '0',
                                     '--timeout', '30', '-o', output, '--controller', 'none',
                                     '--notes', 'Fake camera'],
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    universal_newlines=True, errors='replace', env=environment, timeout=100)
            print(result.stdout)
            self.assertEqual(result.returncode, 0)

            with open(os.path.join(output, 'results.json'), encoding='utf-8') as f:
                data = json.load(f)
            # None when arv-camera-test --version printed nothing, which happens on
            # Windows: see camera_test_version ()
            if data['metadata']['aravis_version'] is not None:
                self.assertRegex(data['metadata']['aravis_version'], r'^\d+\.\d+\.\d+$')
            runs = data['runs']
            self.assertEqual(len(runs), 2)
            for run in runs:
                self.assertEqual(run['status'], 'ok', run['reason'])
                self.assertEqual((run['camera_info']['vendor'], run['camera_info']['model']), ('Aravis', 'Fake'))
                self.assertGreaterEqual(len(run['seconds']), 1)
                self.assertIsNotNone(run['seconds'][0]['time'])
                self.assertGreater(run['statistics']['n_completed_buffers'], 0)
                self.assertEqual(run['command'][1:6], ['-n', 'Fake_1', '--usb-mode=async',
                                                       '--usb-maximum-transfer-size=65536', '--duration=1'])
                self.assertEqual(run['environment'], {'ARV_UV_RAW_IO': '0', 'ARV_DEBUG': 'stream:2,stream-thread:1'})
                self.assertIsNotNone(run['cpu']['seconds'])
                # Not a USB3 Vision stream: no RAW_IO state
                self.assertEqual((run['raw_io_state'], run['raw_io_maximum']), (None, None))
                self.assertEqual([w for w in run['warnings'] if 'not the stream of a USB3 Vision camera' not in w], [])
                self.assertEqual(len(run['warnings']), 1)
                with open(os.path.join(output, run['log']), 'rb') as f:
                    log = f.read().decode('utf-8', errors='replace')
                self.assertIn('ARV_UV_RAW_IO=0 ARV_DEBUG=stream:2,stream-thread:1 ', log.splitlines()[0])
                self.assertIn('n_completed_buffers', log)
            self.assertEqual(runs[1]['command'][-1], '--high-priority')

            with open(os.path.join(output, 'results.md'), encoding='utf-8') as f:
                markdown = f.read()
            self.assertEqual(sum(line.startswith('| Aravis Fake | ') for line in markdown.splitlines()), 2)
            with open(os.path.join(output, 'results.csv'), newline='', encoding='utf-8') as f:
                self.assertEqual(len(list(csv.DictReader(f))), 2)


class DocumentationTest(unittest.TestCase):
    """docs/reference/aravis/usb.md"""

    def test_results_table(self):
        text = read_source('docs', 'reference', 'aravis', 'usb.md')
        header = [line for line in text.splitlines() if line.startswith('| Camera |')]
        self.assertEqual(len(header), 1)
        self.assertEqual(table_cells(header[0]), benchmark.MARKDOWN_COLUMNS)
        self.assertIn('No measurements yet', text)

    def test_raw_io_settings(self):
        # The matrix of the protocol sets ARV_UV_RAW_IO for every run, as the script does
        text = read_source('docs', 'reference', 'aravis', 'usb.md')
        section = text.split('\n## Throughput benchmark', 1)[1].split('\n## ', 1)[0]
        rows = [line for line in section.splitlines() if line.startswith('| RAW_IO |')]
        self.assertEqual(len(rows), 1)
        self.assertEqual(table_cells(rows[0])[1].split(' (')[0], ', '.join(benchmark.RAW_IO_SETTINGS))
        self.assertEqual(re.findall(r'`ARV_UV_RAW_IO=([^`]*)`', rows[0]), list(benchmark.RAW_IO_SETTINGS.values()))

    def test_raw_io_state(self):
        # The protocol gives the ARV_DEBUG that the script sets, which shows the
        # RAW_IO state of a run, and the RAW_IO cells of results.md
        text = read_source('docs', 'reference', 'aravis', 'usb.md')
        section = text.split('\n## Throughput benchmark', 1)[1].split('\n## ', 1)[0]
        self.assertIn('`ARV_DEBUG={}`'.format(benchmark.RAW_IO_DEBUG), section)
        self.assertIn('](#checking-the-result)', section)
        for state in ('off (not available)', 'on, then off'):
            label = benchmark.raw_io_label({'raw_io': 'on', 'raw_io_state': state})
            self.assertIn('`{}`'.format(label), section)

    def test_default_usb_mode(self):
        default = re.search(r'ARV_UV_USB_MODE_DEFAULT\s*=\s*ARV_UV_USB_MODE_(\w+)',
                            read_source('src', 'arvenums.h')).group(1)
        text = read_source('docs', 'reference', 'aravis', 'usb.md')
        performance = text.split('\n## Performance', 1)[1].split('\n## ', 1)[0]
        sentence = next(s for s in re.split(r'(?<=[.:])\s+', performance) if 'default' in s)
        words = {'ASYNC': r'\basynchronous\b', 'SYNC': r'\bsynchronous\b'}
        self.assertRegex(sentence, words[default])
        self.assertNotRegex(sentence, words['SYNC' if default == 'ASYNC' else 'ASYNC'])


def main():
    global SOURCE_ROOT, SCRIPT, CAMERA_TEST, benchmark
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    SOURCE_ROOT = sys.argv[1]
    CAMERA_TEST = sys.argv[2] if len(sys.argv) > 2 else None
    SCRIPT = os.path.join(SOURCE_ROOT, 'scripts', 'arv-usb-benchmark.py')
    benchmark = load_script(SCRIPT)
    unittest.main(argv=[sys.argv[0], '-v'])


if __name__ == '__main__':
    main()
