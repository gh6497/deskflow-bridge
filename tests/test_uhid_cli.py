"""Real CLI + fake ADB/scrcpy and Deskflow: no phones, USB or system services."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time

FAKE_ADB = r'''
import json, os, pathlib, signal, socket, struct, sys, time
root = pathlib.Path(os.environ['OTG_UHID_TEST_ROOT'])
mode = os.environ['OTG_UHID_TEST_MODE']
args = sys.argv[1:]
assert args[:2] == ['-s', 'UHID_PHONE'], args
args = args[2:]
with (root / 'calls').open('a') as f:
    f.write(json.dumps(args) + '\n')
if args == ['get-state']:
    print('device'); sys.exit(0)
if args[0] == 'shell' and '/dev/uhid' in args[-1]:
    (root / 'probe').touch()
    if mode == 'cancel-probe': time.sleep(30)
    sys.exit(1 if mode == 'denied' else 0)
if args[0] == 'push': sys.exit(0)
if args[0] == 'forward':
    if args[1] == '--remove': (root / 'port').unlink(missing_ok=True)
    else: (root / 'port').write_text(args[1].split(':')[1])
    sys.exit(0)
if args == ['shell', 'cmd', 'media_session', 'dispatch', 'pause']: sys.exit(0)
if args[0] == 'shell' and 'wm size;' in args[-1]:
    print('Physical size: 1080x1920\nSurfaceOrientation: 0')
    if mode != 'unlock-unknown':
        print('Broadcast completed: result=0, data="%s"' %
              ('unlocked' if mode == 'unlock-unlocked' else 'locked'))
    if (root / 'awake').exists(): (root / 'entry-lock-query').touch()
    sys.exit(0)
assert args[0] == 'shell' and 'com.genymobile.scrcpy.Server' in args
assert 'video=false' in args and 'control=true' in args and 'power_on=false' in args
assert ('clipboard_autosync=true' in args) == (mode in ('clipboard', 'both'))
signal.signal(signal.SIGTERM, signal.SIG_IGN)
print('fake scrcpy diagnostic', flush=True)
deadline = time.monotonic() + 5
while not (root / 'port').exists():
    assert time.monotonic() < deadline
    time.sleep(.005)
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(('127.0.0.1', int((root / 'port').read_text())))
listener.listen(2)
first, _ = listener.accept(); first.sendall(b'\0')
if 'audio=true' in args:
    control, _ = listener.accept()
    first.sendall(b'\0' * 4)  # No hardware audio required.
else: control = first
listener.close()
def read(n):
    out = b''
    while len(out) < n:
        data = control.recv(n - len(out))
        if not data: return None
        out += data
    return out
created = set()
pin_keys = set()
with (root / 'reports').open('w', buffering=1) as log:
    while True:
        kind = read(1)
        if kind is None: break
        kind = kind[0]
        if kind == 12:
            header = read(7); ident, vendor, product, length = struct.unpack('>HHHB', header)
            name = read(length).decode()
            desc = read(struct.unpack('>H', read(2))[0])
            assert ident in (1, 2, 3) and ident not in created
            expected = {1: b'\x05\x01\x09\x06', 2: b'\x05\x0d\x09\x01', 3: b'\x05\x01\x09\x80'}
            assert desc[:4] == expected[ident]
            if ident == 3: assert b'\x09\x83' in desc
            created.add(ident)
            log.write(json.dumps(['create', ident, name]) + '\n')
            if ident == 1:
                control.sendall(b'\x02\x00')  # Fragmented keyboard LED output.
                control.sendall(b'\x01\x00\x01\x00')
        elif kind == 13:
            ident, size = struct.unpack('>HH', read(4)); data = read(size)
            assert ident in created and size == (1 if ident == 3 else 8)
            log.write(json.dumps(['input', ident, data.hex()]) + '\n')
            if ident == 3:
                assert data in (b'\0', b'\x01')
                if data == b'\x01': (root / 'awake').touch()
            else:
                # Starting/rejecting/cancelling a handshake may send neutral
                # release reports, but actual input must follow the wake pulse.
                if any(data): assert (root / 'awake').exists()
            if ident == 1 and mode.startswith('unlock'):
                pin_keys.update(k for k in data[2:] if k in (0x1e, 0x1f, 0x20, 0x21))
                if data[2] == 0x28:
                    if len(pin_keys) == 4: (root / 'pin-submitted').touch()
                    else: (root / 'unlock-scheduled').touch()
            if ident == 1 and data[2] == 4:
                (root / 'key-down').touch()
                if mode in ('clipboard', 'both'):
                    control.sendall(b'\0' + struct.pack('>I', 10) + b'uhid-phone')
                    control.sendall(b'\x02\x00\x01\x00\x01\x02')
                if mode == 'drop': break
        elif kind == 14:
            ident = struct.unpack('>H', read(2))[0]
            assert ident in created; created.remove(ident)
            log.write(json.dumps(['destroy', ident]) + '\n')
        elif kind == 9:
            header = read(13); text = read(struct.unpack('>I', header[9:13])[0])
            control.sendall(b'\x01' + header[:8])
        else: raise AssertionError(kind)
control.close()
if first is not control: first.close()
(root / 'done').touch()
'''


def wait_file(path, proc):
    deadline = time.monotonic() + 5
    while not path.exists():
        assert proc.poll() is None, ('early exit', proc.communicate())
        assert time.monotonic() < deadline, str(path)
        time.sleep(.005)


def packet(sock, data):
    sock.sendall(struct.pack('>I', len(data)) + data)


def receive(sock):
    def read(n):
        data = b''
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            assert chunk, 'unexpected Deskflow EOF'
            data += chunk
        return data
    return read(struct.unpack('>I', read(4))[0])


cli, audio, clipboard = sys.argv[1:]
cases = ['plain', 'drop', 'reject', 'cancel-handshake', 'denied', 'cancel-probe',
         'unlock-space', 'unlock-swipe', 'unlock-unlocked', 'unlock-unknown', 'unlock-cancel']
if clipboard == 'ON': cases.append('clipboard')
if audio == 'ON' and clipboard == 'ON': cases.append('both')
for mode in cases:
    parent = '/tmp/opencode' if Path('/tmp/opencode').is_dir() else None
    with tempfile.TemporaryDirectory(prefix='uhid-', dir=parent) as directory:
        root = Path(directory)
        adb = root / 'adb'
        adb.write_text(f'#!{sys.executable}\n' + FAKE_ADB)
        adb.chmod(0o700)
        env = dict(os.environ, OTG_UHID_TEST_ROOT=directory, OTG_UHID_TEST_MODE=mode)
        env.pop('OTG_UNLOCK_PIN', None); env.pop('OTG_UNLOCK_ADB_GATE', None)
        if mode.startswith('unlock'):
            env['OTG_UNLOCK_PIN'] = '1234'  # Synthetic test credential; never a real phone.
            env['OTG_UNLOCK_ADB_GATE'] = '1'
        listener = socket.socket()
        listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(8)
        args = [cli, '--gui', '--transport', 'uhid', '--adb', str(adb), '--adb-serial', 'UHID_PHONE',
                '--scrcpy-server', 'fake-server', '--scrcpy-version', '5.0.1',
                '--port', str(listener.getsockname()[1])]
        if mode == 'unlock-swipe': args.append('--unlock-with-swipe')
        if mode in ('clipboard', 'both'): args.append('--clipboard')
        if mode == 'both': args.append('--audio')
        proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        connection = None
        try:
            if mode == 'cancel-probe':
                wait_file(root / 'probe', proc)
            elif mode != 'denied':
                connection, _ = listener.accept(); connection.settimeout(5)
                if mode == 'cancel-handshake':
                    connection.sendall(b'\0\0\0\x0bBar')  # Incomplete frame must remain cancellable.
                else:
                    packet(connection, b'Barrier' + struct.pack('>HH', 1, 6))
                    assert receive(connection).startswith(b'Barrier')
                    if mode == 'reject':
                        packet(connection, b'EUNK')
                    else:
                        packet(connection, b'QINF')
                        assert receive(connection).startswith(b'DINF')
                        packet(connection, b'CIAK')
                        packet(connection, b'DSOP' + struct.pack('>I', 0))
                        packet(connection, b'CINN' + struct.pack('>HHIH', 0, 0, 1, 0))
                        packet(connection, b'DMMV' + struct.pack('>HH', 1079, 1919))
                        if mode.startswith('unlock'):
                            wait_file(root / 'awake', proc)
                            if mode in ('unlock-space', 'unlock-swipe'):
                                wait_file(root / 'pin-submitted', proc)
                            elif mode == 'unlock-cancel':
                                wait_file(root / 'unlock-scheduled', proc)
                                packet(connection, b'COUT')
                                time.sleep(1.8)
                                assert not (root / 'pin-submitted').exists()
                            else:
                                wait_file(root / 'entry-lock-query', proc)
                                time.sleep(1.8)
                                assert not (root / 'pin-submitted').exists()
                        else:
                            packet(connection, b'DKDN' + struct.pack('>HHH', 97, 0, 1))
                            wait_file(root / 'key-down', proc)
                            if mode == 'plain':
                                (root / 'key-down').unlink()
                                packet(connection, b'DKUP' + struct.pack('>HHH', 97, 0, 1))
                                packet(connection, b'COUT')
                                packet(connection, b'CINN' + struct.pack('>HHIH', 1079, 1919, 2, 0))
                                packet(connection, b'DKDN' + struct.pack('>HHH', 97, 0, 1))
                                wait_file(root / 'key-down', proc)
                        if mode in ('clipboard', 'both'):
                            for _ in range(32):
                                if receive(connection).startswith(b'CCLP'): break
                            else: raise AssertionError('phone clipboard ownership not forwarded')
                            packet(connection, b'COUT')
                            for _ in range(32):
                                if b'uhid-phone' in receive(connection): break
                            else: raise AssertionError('phone clipboard body not forwarded')
            if mode in ('denied', 'drop', 'reject'):
                proc.wait(timeout=6)  # EOF/error must end the session without GUI cancellation.
                out, err = proc.communicate()
            else:
                out, err = proc.communicate(b'stop\n', timeout=6)
            events = [json.loads(line)['event'] for line in out.splitlines()]
            connected = mode in ('plain', 'clipboard', 'both', 'drop') or mode.startswith('unlock')
            assert ('connected' in events) == connected, (mode, events, err.decode())
            assert 'usb-ready' not in events and b'fake scrcpy diagnostic' not in out
            calls = [json.loads(line) for line in (root / 'calls').read_text().splitlines()]
            started = mode not in ('denied', 'cancel-probe')
            assert sum('com.genymobile.scrcpy.Server' in call for call in calls) == started, calls
            assert sum(call[0] == 'push' for call in calls) == started, calls
            assert not (root / 'port').exists()
            if started:
                assert (root / 'done').exists(), (mode, err.decode())
                assert b'fake scrcpy diagnostic' in err
                reports = [json.loads(line) for line in (root / 'reports').read_text().splitlines()]
                assert [r[1] for r in reports if r[0] == 'create'] == [1, 2, 3]
                wake = [r[2] for r in reports if r[:2] == ['input', 3]]
                assert wake.count('01') == (2 if mode == 'plain' else int(connected)), wake
                if connected:
                    inputs = [r for r in reports if r[0] == 'input']
                    assert inputs[:2] == [['input', 3, '01'], ['input', 3, '00']], inputs[:2]
                if mode != 'drop':
                    assert reports[-3:] == [['destroy', 1], ['destroy', 2], ['destroy', 3]], reports
                if connected and mode != 'drop':
                    keys = [bytes.fromhex(r[2]) for r in reports if r[:2] == ['input', 1]]
                    assert keys[-1] == bytes(8), keys
                    if not mode.startswith('unlock'):
                        assert any(k[2] == 4 for k in keys), keys
                    else:
                        if mode == 'unlock-space':
                            assert [k[2] for k in keys if k[2]] == [0x28, 0x2c, 0x1e, 0x1f, 0x20, 0x21, 0x28], keys
                        else:
                            digits = [k[2] for k in keys if k[2] in (0x1e, 0x1f, 0x20, 0x21)]
                            assert digits == ([0x1e, 0x1f, 0x20, 0x21] if mode == 'unlock-swipe' else []), digits
                    pointers = [bytes.fromhex(r[2]) for r in reports if r[:2] == ['input', 2]]
                    if mode == 'unlock-space':
                        assert not any(p[0] & 1 for p in pointers), pointers  # No synthetic touch.
                    if mode == 'unlock-swipe':
                        assert any(p[0] & 1 for p in pointers), pointers
                    corner_x = 1079 * 32767 // 1919
                    corner = bytes((corner_x & 0xff, corner_x >> 8, 0xff, 0x7f))
                    assert any(p[2:6] == corner for p in pointers), pointers
                    assert pointers[-1][:2] == b'\0\0', pointers
            else:
                assert 'uhid-ready' not in events
        finally:
            if connection: connection.close()
            listener.close()
            if proc.poll() is None:
                proc.kill(); proc.communicate(timeout=5)
print('PASS: real UHID CLI lifecycle:', ', '.join(cases))
