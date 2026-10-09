#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 niposch
# SPDX-License-Identifier: BSD-2-Clause
"""Bounded, loopback-only TLS/peer teardown check with isolated KRDP state."""
from pathlib import Path
import argparse
import os
import socket
import signal
import ssl
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('binary', type=Path)
args = parser.parse_args()
request = bytes.fromhex('030000130ee000000000000100080001000000')
with tempfile.TemporaryDirectory(prefix='krdp-peer-check-') as name:
    root=Path(name)
    for d in ['config','data','state','cache']: (root/d).mkdir()
    # KRDP requires an authentication method to start; no credentials are sent.
    (root/'config'/'krdpserverrc').write_text('[General]\nSystemUserEnabled=true\nAutostart=false\nMonitorMode=workspace\nVirtualMonitorPolicy=extend\n')
    subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(root/'test.key'),'-out',str(root/'test.crt'),'-days','1','-subj','/CN=krdp-loopback-test'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1',0)); port=listener.getsockname()[1]
    env=os.environ.copy()
    env.update(QT_QPA_PLATFORM='offscreen',QT_QUICK_BACKEND='software',
        XDG_CONFIG_HOME=str(root/'config'),XDG_DATA_HOME=str(root/'data'),
        XDG_STATE_HOME=str(root/'state'),XDG_CACHE_HOME=str(root/'cache'))
    env.pop('LD_LIBRARY_PATH',None)
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname=False; context.verify_mode=ssl.CERT_NONE
    def connection():
        with socket.create_connection(('127.0.0.1',port),timeout=3) as sock:
            sock.sendall(request)
            reply=sock.recv(256)
            assert reply.startswith(b'\x03\x00'), 'No RDP negotiation response'
            with context.wrap_socket(sock,server_hostname='krdp-loopback-test') as tls:
                assert tls.version() in ['TLSv1.2','TLSv1.3']
    def counts():
        process=Path('/proc')/str(server.pid)
        return len(list((process/'fd').iterdir())),len(list((process/'task').iterdir()))
    with (root/'server.log').open('w') as log:
        server=subprocess.Popen([str(args.binary),'--address','127.0.0.1','--port',str(port),'--certificate',str(root/'test.crt'),'--certificate-key',str(root/'test.key')],env=env,stdout=log,stderr=subprocess.STDOUT)
        try:
            deadline=time.monotonic()+8
            while True:
                assert server.poll() is None, 'Test server exited at startup'
                try: connection(); break
                except (ConnectionRefusedError,ConnectionResetError):
                    if time.monotonic()>deadline: raise
                    time.sleep(.1)
            for _ in range(4): connection()
            time.sleep(1)
            before=counts()
            for _ in range(25): connection()
            time.sleep(1)
            after=counts()
            assert after[0] <= before[0]+2, (before,after,'file descriptor leak')
            assert after[1] <= before[1]+2, (before,after,'thread leak')
            server.terminate()
            server.wait(timeout=8)
            assert server.returncode in (0,-signal.SIGTERM), ('Shutdown status',server.returncode)
            print(f'PASS: 25 TLS connections, FD/thread counts {before} -> {after}; stop within 8 seconds, status {server.returncode}')
        except Exception:
            log.flush()
            print((root/'server.log').read_text()[-2000:])
            raise
        finally:
            if server.poll() is None:
                server.kill(); server.wait()
