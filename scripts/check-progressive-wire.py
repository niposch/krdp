#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 niposch
# SPDX-License-Identifier: BSD-2-Clause
"""Check two synthetic RDP surfaces through FreeRDP on an isolated X display."""
import argparse
import json
import os
import re
from pathlib import Path
import select
import socket
import subprocess
import tempfile
import time
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('build', type=Path)
parser.add_argument('--xvfb', type=Path, required=True)
parser.add_argument('--h264', action='store_true')
args = parser.parse_args()
processes = []
with tempfile.TemporaryDirectory(prefix='krdp-progressive-wire-') as directory:
    root = Path(directory)
    env = os.environ.copy()
    for name in ['CONFIG', 'DATA', 'STATE', 'CACHE']:
        env['XDG_' + name + '_HOME'] = str(root / name.lower())
    cert, key = root / 'test.crt', root / 'test.key'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-keyout', str(key), '-out', str(cert), '-days', '1',
                    '-subj', '/CN=localhost'], check=True, capture_output=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    encoded = []
    if args.h264:
        for index, color in enumerate([(30,215,96),(215,30,96)]):
            path = root / ('monitor' + str(index) + '.h264')
            subprocess.run(['ffmpeg', '-v', 'error', '-init_hw_device',
                            'vaapi=rdp:/dev/dri/renderD128', '-filter_hw_device', 'rdp',
                            '-f', 'rawvideo', '-pixel_format', 'rgba', '-video_size',
                            '256x256', '-i', 'pipe:0', '-vf',
                            'hwupload,scale_vaapi=format=nv12:mode=fast:out_range=full:out_color_matrix=bt709:out_color_primaries=bt709:out_color_transfer=bt709',
                            '-frames:v', '1', '-c:v', 'h264_vaapi', '-profile:v', 'main',
                            '-qp', '18', '-color_range', 'pc', '-colorspace', 'bt709',
                            '-color_primaries', 'bt709', '-color_trc', 'bt709',
                            '-f', 'h264', str(path)], input=bytes([*color,255])*256*256,
                           check=True, capture_output=True)
            encoded.append(str(path))
    try:
        read_fd, write_fd = os.pipe()
        with (root / 'xvfb.log').open('wb') as output:
            display = subprocess.Popen([str(args.xvfb), '-displayfd', str(write_fd),
                                        '-screen', '0', '512x256x24', '-nolisten', 'tcp'],
                                       pass_fds=[write_fd], stdout=output, stderr=output)
        processes.append(display)
        os.close(write_fd)
        if not select.select([read_fd], [], [], 5)[0]:
            raise RuntimeError('Isolated X display did not start')
        env['DISPLAY'] = ':' + os.read(read_fd, 64).decode().strip()
        os.close(read_fd)
        env['QT_QPA_PLATFORM'] = 'offscreen'
        env['KRDP_BANDWIDTH_PROBE'] = '1'
        env['QT_FORCE_STDERR_LOGGING'] = '1'
        env['QT_LOGGING_RULES'] = 'org.kde.krdp.debug=true;org.kde.krdp.info=true'
        with (root / 'server.log').open('wb') as output:
            server = subprocess.Popen([str(args.build / 'bin/krdp-synthetic-video-server'),
                                       str(port), str(cert), str(key)] + (['h264'] + encoded if args.h264 else []), env=env,
                                      stdout=output, stderr=output)
        processes.append(server)
        time.sleep(0.6)
        with (root / 'client.log').open('wb') as output:
            client = subprocess.Popen(['xfreerdp', '/v:127.0.0.1:' + str(port),
                                       '/u:synthetic', '/p:synthetic-test-only',
                                       '/cert:ignore', '/sec:tls',
                                       '/gfx:progressive:on,AVC420:' + ('on' if args.h264 else 'off') + ',AVC444:off',
                                       '/size:512x256', '/f', '-clipboard', '/network:auto',
                                       '/log-level:WARN'], env=env, stdout=output, stderr=output)
        processes.append(client)
        time.sleep(3)
        if client.poll() is not None or server.poll() is not None:
            raise RuntimeError('Synthetic client or server exited early')
        capture = root / 'synthetic.png'
        subprocess.run(['import', '-display', env['DISPLAY'], '-window', 'root', str(capture)],
                       check=True, capture_output=True)
        image = Image.open(capture).convert('RGB')
        samples = [image.getpixel((128,128)), image.getpixel((384,128))]
        expected = [(30,215,96),(215,30,96)]
        for actual, wanted in zip(samples, expected):
            if max(abs(a-b) for a,b in zip(actual,wanted)) > 6:
                raise RuntimeError('Decoded monitor color mismatch: ' + repr(samples))
        probe = re.search(r'Connection-time bandwidth probe:.*?-> (\d+) kbit/s', (root / 'server.log').read_text())
        fallback = 'Connection-time bandwidth probe did not produce a usable sample; retaining passive fallback' in (root / 'server.log').read_text()
        if not probe and not fallback:
            raise RuntimeError('No connection-time probe result or resolution fallback was reported')
        print(json.dumps({'connection_time_probe_kbps': int(probe.group(1)) if probe else None, 'probe_resolution_fallback': fallback, 'codec': 'avc420' if args.h264 else 'progressive', 'decoded_monitor_colors': samples,
                          'expected': expected, 'two_surface_wire_decode': 'pass', 'initial_partial_damage': not args.h264}))
    except Exception:
        for name in ['server.log','client.log','xvfb.log']:
            path = root / name
            if path.exists(): print(name + ':\n' + path.read_text(errors='replace')[-4000:])
        raise
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
