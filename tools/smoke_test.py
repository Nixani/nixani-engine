#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Talks to the engine the way a client does and checks what comes back. No console is needed
and nothing is sent to PSN: sessions go to 127.0.0.1, where no console answers.

    python tools/smoke_test.py <path to nixani-engine>

Exit code 0 = every check passed. Runs on Windows, Linux and macOS (standard library only)."""
import queue
import struct
import subprocess
import sys
import threading
import time

PROTOCOL = 1
HELLO, DISCOVERY_START, DISCOVERY_STOP, WAKE, REGIST_START, REGIST_CANCEL = 1, 10, 11, 12, 20, 21
NEW_DEVICE_UID, SESSION_START, SESSION_STOP, PAD, LOGIN_PIN, REQUEST_KEYFRAME, PING = 31, 40, 41, 42, 43, 44, 50
HELLO_REPLY, LOG, HOSTS, REGIST_RESULT, DEVICE_UID, SESSION_STATE, SESSION_STOPPED, PONG = 101, 102, 110, 120, 131, 140, 152, 160
ENDED = 3

if sys.platform == 'win32':
    import ctypes
    _qpc = ctypes.c_int64()
    _freq = ctypes.c_int64()
    ctypes.windll.kernel32.QueryPerformanceFrequency(ctypes.byref(_freq))
    TICKS_PER_SECOND = _freq.value

    def now():  # the protocol's clock on Windows: QueryPerformanceCounter ticks
        ctypes.windll.kernel32.QueryPerformanceCounter(ctypes.byref(_qpc))
        return _qpc.value
else:
    TICKS_PER_SECOND = 1_000_000_000

    def now():  # everywhere else: CLOCK_MONOTONIC in nanoseconds
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC)

failures = []


def check(ok, what):
    print(('  ok    ' if ok else '  FAIL  ') + what, flush=True)
    if not ok:
        failures.append(what)
    return ok


def s(text):
    b = text.encode()
    return struct.pack('<I', len(b)) + b


def rstr(b, o):
    n, = struct.unpack_from('<I', b, o)
    return b[o + 4:o + 4 + n].decode(errors='replace'), o + 4 + n


exe = sys.argv[1]

out = subprocess.run([exe, '--version'], capture_output=True, text=True, timeout=20)
check(out.returncode == 0 and out.stdout.startswith('nixani-engine '), '--version prints the version: ' + out.stdout.strip())

p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
inbox = queue.Queue()


def reader():
    def read_exact(n):
        data = b''
        while len(data) < n:
            chunk = p.stdout.read(n - len(data))
            if not chunk:
                raise EOFError
            data += chunk
        return data
    try:
        while True:
            size, = struct.unpack('<I', read_exact(4))
            body = read_exact(size)
            inbox.put((struct.unpack('<H', body[:2])[0], body[2:]))
    except (EOFError, OSError, ValueError):
        inbox.put((0, b''))


threading.Thread(target=reader, daemon=True).start()


def send(msg_type, payload=b''):
    p.stdin.write(struct.pack('<IH', len(payload) + 2, msg_type) + payload)
    p.stdin.flush()


def wait_for(want, seconds, accept=lambda b: True):
    """The next message of type `want` that `accept` takes; logs are printed on the way."""
    end = time.time() + seconds
    while time.time() < end:
        try:
            t, b = inbox.get(timeout=0.2)
        except queue.Empty:
            continue
        if t == LOG:
            print('        log[%d] %s' % (b[0], rstr(b, 1)[0][:160]), flush=True)
        elif t == SESSION_STATE:
            print('        session state %d %s %r' % (b[0], 'error' if b[1] else 'ok', rstr(b, 2)[0][:160]), flush=True)
        if t == 0:
            return None  # the engine went away
        if t == want and accept(b):
            return b
    return None


def session_params(host):
    # str host, u8 ps5, bytes16 registKey, bytes16 rpKey, bytes8 accountId, u32 resolution, u32 fps, u32 bitrateKbps,
    # u8 codec, u8 dualsense, u8 internet, str psnToken, bytes32 deviceUid
    return (s(host) + bytes([1]) + b'12345678'.ljust(16, b'\0') + bytes(16) + bytes(8) + struct.pack('<III', 1080, 60, 15000)
            + bytes([1, 1, 0]) + s('') + bytes(32))


pad = (struct.pack('<IBBhhhh', 0, 0, 0, 0, 0, 0, 0) + struct.pack('<bHH', -1, 0, 0) * 2 + bytes([1])
       + struct.pack('<6f', 0, 0, 0, 0, 1, 0) + struct.pack('<Q', 1000))

try:
    print('handshake')
    send(HELLO, struct.pack('<I', PROTOCOL))
    b = wait_for(HELLO_REPLY, 10)
    if check(b is not None, 'HelloReply arrives'):
        check(struct.unpack_from('<I', b)[0] == PROTOCOL, 'protocol version %d' % PROTOCOL)
        check(rstr(b, 4)[0].startswith('nixani-engine '), 'engine names itself: ' + rstr(b, 4)[0])

    print('device uid')
    send(NEW_DEVICE_UID, struct.pack('<I', 7))
    b = wait_for(DEVICE_UID, 10)
    if check(b is not None, 'DeviceUid arrives'):
        check(struct.unpack_from('<I', b)[0] == 7 and len(rstr(b, 4)[0]) > 16, 'request id echoed, uid has %d characters' % len(rstr(b, 4)[0]))

    print('ping: echo sizes and the shared clock')
    for size in (0, 1, 100_000, 1_000_000):
        sent = now()
        send(PING, struct.pack('<QI', sent, size))
        b = wait_for(PONG, 10)
        if not check(b is not None, 'Pong arrives (%d bytes asked)' % size):
            break
        back = now()
        client_time, engine_time = struct.unpack_from('<QQ', b)
        check(client_time == sent and len(b) - 16 == size, 'client time and %d echo bytes returned' % size)
        # The engine reads the same clock: its time lies between our send and our receive.
        check(sent <= engine_time <= back, 'engine time is on the client\'s clock (%.0f us after sending)' % ((engine_time - sent) * 1e6 / TICKS_PER_SECOND))

    print('discovery')
    send(DISCOVERY_START)
    started = False
    end = time.time() + 6
    while time.time() < end and not started:
        try:
            t, b = inbox.get(timeout=0.2)
        except queue.Empty:
            continue
        if t == LOG:
            text = rstr(b, 1)[0]
            print('        log[%d] %s' % (b[0], text[:160]), flush=True)
            started = 'Discovery running' in text
        elif t == HOSTS:
            print('        hosts: %d' % struct.unpack_from('<I', b)[0], flush=True)
    check(started, 'discovery starts')
    time.sleep(1.2)  # a few search rounds on the network
    send(DISCOVERY_STOP)

    print('wake packet')
    send(WAKE, s('127.0.0.1') + b'12345678'.ljust(16, b'\0') + bytes([1]))
    b = wait_for(LOG, 10, lambda b: 'Wake' in rstr(b, 1)[0])
    check(b is not None and 'Wake packet sent' in rstr(b, 1)[0], 'wake packet goes out')

    print('messages that need a session, without one')
    send(PAD, pad)
    send(LOGIN_PIN, s('1234'))
    send(REQUEST_KEYFRAME)
    send(SESSION_STOP)
    check(wait_for(SESSION_STOPPED, 10) is not None, 'SessionStopped answers SessionStop')

    for attempt in (1, 2):
        print('session %d to an address without a console' % attempt)
        send(SESSION_START, session_params('127.0.0.1'))
        b = wait_for(SESSION_STATE, 60, lambda b: b[0] == ENDED)
        check(b is not None and b[1] == 1, 'the session ends with an error instead of hanging')
        send(PAD, pad)
        send(SESSION_STOP)
        check(wait_for(SESSION_STOPPED, 30) is not None, 'SessionStopped after the failed session')

    print('start, then stop right away')
    send(SESSION_START, session_params('127.0.0.1'))
    send(SESSION_STOP)
    check(wait_for(SESSION_STOPPED, 30) is not None, 'SessionStopped')

    print('a SessionStart that is too short')
    send(SESSION_START, b'\x01\x02')
    b = wait_for(SESSION_STATE, 10, lambda b: b[0] == ENDED)
    check(b is not None and b[1] == 1 and 'bad session request' in rstr(b, 2)[0], 'refused as a bad request')

    print('linking with an address without a console')
    send(REGIST_START, s('127.0.0.1') + struct.pack('<I', 1000100) + bytes(8) + struct.pack('<II', 12345678, 0))
    b = wait_for(REGIST_RESULT, 60)
    check(b is not None and b[0] == 0, 'RegistResult reports the failure: ' + (rstr(b, 1)[0] if b else 'nothing came'))
    send(REGIST_CANCEL)

    print('the engine still answers, then ends when its input closes')
    send(PING, struct.pack('<QI', now(), 0))
    check(wait_for(PONG, 10) is not None, 'Pong after all of the above')
    p.stdin.close()
    try:
        code = p.wait(timeout=20)
    except subprocess.TimeoutExpired:
        code = None
    check(code == 0, 'exit code 0 after the client closes the pipe (got %r)' % code)
finally:
    if p.poll() is None:
        p.kill()

print('\n%s' % ('ALL CHECKS PASSED' if not failures else '%d CHECK(S) FAILED:\n  ' % len(failures) + '\n  '.join(failures)))
sys.exit(1 if failures else 0)
