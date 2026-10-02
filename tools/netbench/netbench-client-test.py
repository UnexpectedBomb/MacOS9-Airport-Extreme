#!/usr/bin/env python3
"""netbench-client-test.py -- exercise netbench-server.py end to end from any host, before the OS 9 app.

Speaks protocol v1 exactly as the NetBench app does, and FALSIFIES the byte oracle: one upload carries
three deliberately corrupted bytes and the server must report exactly three mismatches.

    netbench-client-test.py [host] [port]       (default localhost 5201; exit 0 = every check passed)
"""
import socket, struct, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5201
PERIOD = 131072
BLOCK = bytes(((j * 7 + (j >> 9)) & 0xFF) for j in range(PERIOD))
fails = 0


def check(what, ok):
    global fails
    fails += 0 if ok else 1
    print("  %-60s %s" % (what, "[ok]" if ok else "[FAIL]"))


def pattern(off, n):
    out = bytearray()
    while n > 0:
        j = off % PERIOD; take = min(n, PERIOD - j)
        out += BLOCK[j:j + take]; off += take; n -= take
    return bytes(out)


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        c = s.recv(n - len(buf))
        if not c: raise ConnectionError("closed")
        buf += c
    return bytes(buf)


def hdr(cmd, n, arg=0):
    return struct.pack(">4sIII", b"NBv1", cmd, n, arg)


s = socket.create_connection((HOST, PORT), timeout=30)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

s.sendall(hdr(1, 10)); rtts = []
for k in range(10):
    t = time.perf_counter(); s.sendall(bytes([k])); b = recv_exact(s, 1); rtts.append(time.perf_counter() - t)
    if b != bytes([k]): break
check("PING x10: every byte echoed back unchanged", len(rtts) == 10)

n = 1048576; data = pattern(0, n)
s.sendall(hdr(2, n)); t = time.perf_counter(); s.sendall(data); rep = recv_exact(s, 16); dt = time.perf_counter() - t
magic, got, usec, bad = struct.unpack(">4sIII", rep)
check("UP 1 MB clean: reply NBok, all bytes received", magic == b"NBok" and got == n)
check("UP 1 MB clean: server found 0 mismatched bytes", bad == 0)
print("      (this host -> Pi: %.0f KB/s client-timed, %.0f KB/s Pi-timed)" % (n / 1024 / dt, n / 1024 / (usec / 1e6)))

n = 65536; data = bytearray(pattern(0, n)); data[100] ^= 0xFF; data[40000] ^= 0x01; data[65535] ^= 0x80
s.sendall(hdr(2, n)); s.sendall(bytes(data)); magic, got, usec, bad = struct.unpack(">4sIII", recv_exact(s, 16))
check("FALSIFY: UP with 3 corrupted bytes -> server reports exactly 3", magic == b"NBok" and bad == 3)

n = 1048576; s.sendall(hdr(3, n)); t = time.perf_counter(); got = recv_exact(s, n); dt = time.perf_counter() - t
check("DOWN 1 MB: all bytes arrive and match the pattern", got == pattern(0, n))
print("      (Pi -> this host: %.0f KB/s)" % (n / 1024 / dt))

note = b"netbench-client-test: protocol check from the dev Mac\r"
s.sendall(hdr(4, len(note))); s.sendall(note); magic = recv_exact(s, 16)[:4]
check("NOTE: text accepted (NBok)", magic == b"NBok")
s.sendall(hdr(5, 0)); s.close()

print("\n%s  %d failure(s)" % ("[ok]" if fails == 0 else "[FAIL]", fails))
sys.exit(1 if fails else 0)
