#!/usr/bin/env python3
"""Smoke-test an installed Aravis tree with GStreamer.

Runs 'gst-inspect-1.0 aravissrc' with GST_PLUGIN_PATH pointing into the tree,
then starts the tree's arv-fake-gv-camera on the loopback interface and runs

  gst-launch-1.0 aravissrc camera-name=Aravis-Fake-<serial> num-buffers=50 ! fakesink

which lasts two seconds at the fake camera's default 25 frames per second.

The fake camera binds the GigE Vision control port, so run one smoke test at a
time on a machine. The tree is a 'meson install --destdir' stage or an
unzipped Windows package: PREFIX/bin, PREFIX/lib/gstreamer-1.0.

Usage: smoke-test-gstreamer.py --prefix PREFIX [--gst-bin DIR] [--num-buffers N]
"""

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

API_VERSION = '0.10'


def find_program(name, directories):
    for directory in directories:
        for candidate in (name, name + '.exe'):
            path = os.path.join(directory, candidate)
            if os.path.isfile(path):
                return path
    path = shutil.which(name)
    if path is None:
        sys.exit('{} not found'.format(name))
    return path


def run(command, env, timeout):
    print('$ ' + ' '.join(command), flush=True)
    start = time.monotonic()
    try:
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired as e:
        print(e.output or '')
        sys.exit('{} did not finish within {} s'.format(command[0], timeout))
    print(result.stdout, flush=True)
    return result, time.monotonic() - start


def wait_for_camera(arv_tool, device_id, env, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = subprocess.run([arv_tool], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                universal_newlines=True, errors='replace', timeout=timeout)
        if device_id in result.stdout:
            print('Fake camera discovered: ' + result.stdout.strip(), flush=True)
            return True
        time.sleep(0.5)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--prefix', required=True, help='installed Aravis tree')
    parser.add_argument('--gst-bin', help='directory holding gst-inspect-1.0 and gst-launch-1.0 (default: PATH)')
    parser.add_argument('--serial', default='GVSMOKE', help='serial number of the fake camera')
    parser.add_argument('--num-buffers', type=int, default=50, help='frames to stream (default: 50)')
    parser.add_argument('--timeout', type=float, default=60, help='timeout of each step in seconds')
    args = parser.parse_args()

    prefix = os.path.abspath(args.prefix)
    bin_dir = os.path.join(prefix, 'bin')
    plugin_dirs = glob.glob(os.path.join(prefix, 'lib', 'gstreamer-1.0'))
    if not plugin_dirs:
        sys.exit('No lib/gstreamer-1.0 directory in ' + prefix)
    gst_dirs = [args.gst_bin] if args.gst_bin else []

    work_dir = tempfile.mkdtemp(prefix='arv-gst-smoke-')
    env = os.environ.copy()
    env['PATH'] = os.pathsep.join([bin_dir] + gst_dirs + [env.get('PATH', '')])
    env['GST_PLUGIN_PATH'] = os.pathsep.join(plugin_dirs)
    # A private registry, so that no stale entry of another aravissrc is used
    env['GST_REGISTRY'] = os.path.join(work_dir, 'registry.bin')
    env.pop('GST_PLUGIN_PATH_1_0', None)

    gst_inspect = find_program('gst-inspect-1.0', gst_dirs)
    gst_launch = find_program('gst-launch-1.0', gst_dirs)
    arv_tool = find_program('arv-tool-' + API_VERSION, [bin_dir])
    fake_camera = find_program('arv-fake-gv-camera-' + API_VERSION, [bin_dir])
    device_id = 'Aravis-Fake-' + args.serial

    camera = None
    try:
        result, _ = run([gst_inspect, 'aravissrc'], env, args.timeout)
        match = re.search(r'^\s*Filename\s+(.+?)\s*$', result.stdout, re.MULTILINE)
        if result.returncode != 0 or match is None:
            sys.exit('FAIL: gst-inspect-1.0 does not find aravissrc')
        plugin = os.path.normcase(os.path.abspath(match.group(1)))
        if not any(plugin.startswith(os.path.normcase(d) + os.sep) for d in plugin_dirs):
            sys.exit('FAIL: aravissrc comes from {}, not from {}'.format(match.group(1), prefix))
        print('PASS: gst-inspect-1.0 aravissrc', flush=True)

        print('$ {} -i 127.0.0.1 -s {}'.format(fake_camera, args.serial), flush=True)
        camera = subprocess.Popen([fake_camera, '-i', '127.0.0.1', '-s', args.serial], env=env,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not wait_for_camera(arv_tool, device_id, env, args.timeout):
            sys.exit('FAIL: {} not discovered by arv-tool'.format(device_id))

        result, duration = run([gst_launch, 'aravissrc', 'camera-name=' + device_id,
                                'num-buffers={}'.format(args.num_buffers), '!', 'fakesink'],
                               env, args.timeout)
        if result.returncode != 0 or 'Got EOS' not in result.stdout:
            sys.exit('FAIL: the aravissrc pipeline did not reach EOS (exit status {})'.format(result.returncode))
        print('PASS: {} buffers from {} in {:.1f} s'.format(args.num_buffers, device_id, duration), flush=True)
    finally:
        if camera is not None:
            camera.terminate()
            try:
                camera.wait(timeout=10)
            except subprocess.TimeoutExpired:
                camera.kill()
        shutil.rmtree(work_dir, ignore_errors=True)

    return 0


if __name__ == '__main__':
    sys.exit(main())
