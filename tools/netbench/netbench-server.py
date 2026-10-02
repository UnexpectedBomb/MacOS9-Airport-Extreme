#!/usr/bin/env python3
"""netbench-server.py -- the Pi end of NetBench, a TCP throughput benchmark for Mac OS 9.

The network equivalent of QuickBench: QuickBench cannot test a network volume (it dropped the AFP share
over AirPort AND Ethernet), and an AFP Finder copy mixes AppleShare, the Pi's disk and the network into
one number. NetBench measures plain TCP between the OS 9 Mac and this Pi, in both directions, timed at
BOTH ends, and VERIFIES EVERY BYTE against a known pattern -- a data-path oracle, not just a stopwatch
(it would have caught k192's 512-byte receive truncation directly).

Run by scripts/pi-netbench.sh as the unprivileged `claude` account (no sudo; a high port).

PROTOCOL v1 -- one TCP connection per session; each test starts with a 16-byte big-endian header
    'NBv1'  cmd(u32)  n(u32)  arg(u32)
  cmd 1 PING   n round trips: client sends 1 byte, we echo it.
  cmd 2 UP     client sends n pattern bytes; we time first->last byte, verify, and reply
               'NBok' received(u32) server_usec(u32) mismatches(u32).
  cmd 3 DOWN   we send n pattern bytes; the client times + verifies.
  cmd 4 NOTE   client sends n bytes of text (its own results); we append them to the log, reply 'NBok'.
  cmd 5 BYE    end of session.
PATTERN: byte at absolute stream offset i = (i*7 + (i >> 9)) & 0xFF. Period 131072, so generating and
verifying are slices of one precomputed block (fast even in CPython on a Pi 3).
"""
import socket, struct, sys, time, datetime

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5201
LOG = sys.argv[2] if len(sys.argv) > 2 else "NetBench Server Log"
MAGIC, OK = b"NBv1", b"NBok"
PERIOD = 131072
BLOCK = bytes(((j * 7 + (j >> 9)) & 0xFF) for j in range(PERIOD))
CHUNK = 32768


def pattern(off, n):
    """n pattern bytes starting at absolute stream offset off."""
    out = bytearray()
    while n > 0:
        j = off % PERIOD
        take = min(n, PERIOD - j)
        out += BLOCK[j:j + take]
        off += take
        n -= take
    return bytes(out)


def log(line):
    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with open(LOG, "a") as f:
        f.write("%s  %s\n" % (stamp, line))
    print("%s  %s" % (stamp, line), flush=True)


def recv_exact(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(min(CHUNK, n - len(buf)))
        if not chunk:
            raise ConnectionError("peer closed after %d of %d bytes" % (len(buf), n))
        buf += chunk
    return bytes(buf)


def rate(nbytes, usec):
    return (nbytes / 1024.0) / (usec / 1e6) if usec > 0 else 0.0


def session(s, peer):
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # pings must not wait on Nagle/delayed ACK
    log("connect from %s" % peer)
    while True:
        hdr = recv_exact(s, 16)
        magic, cmd, n, arg = struct.unpack(">4sIII", hdr)
        if magic != MAGIC:
            log("bad magic %r from %s -- closing" % (magic, peer))
            return
        if cmd == 1:                                            # PING
            for _ in range(n):
                s.sendall(recv_exact(s, 1))
            log("PING  %d round trips echoed" % n)
        elif cmd == 2:                                          # UP (Mac -> Pi)
            got, bad, t0 = 0, 0, None
            while got < n:
                chunk = s.recv(min(CHUNK, n - got))
                if not chunk:
                    raise ConnectionError("peer closed during UP after %d of %d" % (got, n))
                if t0 is None:
                    t0 = time.perf_counter()
                exp = pattern(got, len(chunk))
                if chunk != exp:
                    bad += sum(1 for a, b in zip(chunk, exp) if a != b)
                got += len(chunk)
            usec = int((time.perf_counter() - t0) * 1e6) if t0 is not None else 0
            s.sendall(OK + struct.pack(">III", got, usec, bad))
            log("UP    %8d bytes  %8.1f ms  %8.1f KB/s  (Pi-timed)  mismatched bytes %d" %
                (got, usec / 1000.0, rate(got, usec), bad))
        elif cmd == 3:                                          # DOWN (Pi -> Mac)
            t0, off = time.perf_counter(), 0
            while off < n:
                take = min(CHUNK, n - off)
                s.sendall(pattern(off, take))
                off += take
            usec = int((time.perf_counter() - t0) * 1e6)
            log("DOWN  %8d bytes  %8.1f ms  send side (the Mac-timed figure is authoritative)" %
                (n, usec / 1000.0))
        elif cmd == 4:                                          # NOTE
            text = recv_exact(s, n).decode("mac-roman", "replace")
            for line in text.replace("\r", "\n").split("\n"):
                if line.strip():
                    log("MAC   " + line)
            s.sendall(OK + struct.pack(">III", n, 0, 0))
        elif cmd == 5:                                          # BYE
            log("BYE   session from %s complete" % peer)
            return
        else:
            log("unknown cmd %d from %s -- closing" % (cmd, peer))
            return


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)
    log("netbench-server listening on port %d" % PORT)
    while True:
        s, addr = srv.accept()
        try:
            session(s, addr[0])
        except Exception as e:                                  # one bad session must not stop the server
            log("session from %s ended: %s" % (addr[0], e))
        finally:
            s.close()


if __name__ == "__main__":
    main()
