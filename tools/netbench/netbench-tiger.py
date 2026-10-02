#!/usr/bin/python
# netbench-tiger.py -- NetBench's client for Mac OS X 10.4 Tiger: the KNOWN-GOOD BASELINE.
#
# The OS 9 driver is measured by NetBench (netbench.c). This runs the identical tests against the
# same Pi server from Tiger on the SAME machine, card, access point and spot, so the two SUMMARY
# lines compare directly: Tiger's number is what this radio can do in that room, and the gap to it
# is ours to close.
#
#   Tiger:  python netbench-tiger.py [server[:port]]        (default 192.168.1.207:5201)
#           Unplug Ethernet first, so the test runs over AirPort; the transcript names the interface.
#   Pi:     scripts/pi-netbench.sh start                     (the same server NetBench uses)
#
# Tests, exactly as NetBench 1.0: 20 one-byte round trips; UP 128 KB warm-up + 3 x 1 MB (timed here
# AND on the Pi, every byte verified by the Pi); DOWN 128 KB warm-up + 3 x 1 MB (timed and verified
# here). The transcript goes to the Pi's "NetBench Server Log" like NetBench's.
#
# Written for Tiger's Python 2.3 AND for Python 3 (so it is tested on the dev Mac against the real
# server): no 'with', no conditional expressions, no sorted()/set(), no b'' literals, no print
# statement, and 'except X:' + sys.exc_info(). Pure ASCII -- Python 2.3 warns on anything else.
# The SSID and BSSID are deliberately NOT reported: these logs describe the user's home network.
import os, socket, struct, sys, time

PY3 = sys.version_info[0] >= 3
VERSION = "NetBench-Tiger 1.1"
PING_COUNT, WARM, TEST, REPEATS = 20, 128 * 1024, 1024 * 1024, 3
PERIOD, CHUNK = 131072, 32768
AIRPORT_TOOL = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/A/Resources/airport"


def wire(s):
    """An ASCII string as the bytes a socket carries."""
    if PY3:
        return s.encode("latin-1")
    return s


def mkbytes(vals):
    if PY3:
        return bytes(vals)
    return "".join([chr(v) for v in vals])


def byteval(buf, i):
    if PY3:
        return buf[i]
    return ord(buf[i])


# byte at absolute stream offset i = (i*7 + (i >> 9)) & 0xFF, period 131072 -- netbench-server.py's
BLOCK = mkbytes([((j * 7 + (j >> 9)) & 0xFF) for j in range(PERIOD)])
PAT = BLOCK * (TEST // PERIOD)                    # 1 MB, starts at offset 0 like every test

lines = []


def say(s):
    lines.append(s)
    sys.stdout.write(s + "\n")
    sys.stdout.flush()


def recv_exact(s, n):
    parts, got = [], 0
    while got < n:
        c = s.recv(min(CHUNK, n - got))
        if not c:
            raise IOError("server closed after %d of %d bytes" % (got, n))
        parts.append(c)
        got = got + len(c)
    return wire("").join(parts)


def send_hdr(s, cmd, n):
    s.sendall(struct.pack(">4sIII", wire("NBv1"), cmd, n, 0))


def mismatches(a, b):
    if a == b:
        return 0
    bad, i = 0, 0
    n = min(len(a), len(b))
    while i < n:
        if byteval(a, i) != byteval(b, i):
            bad = bad + 1
        i = i + 1
    return bad + abs(len(a) - len(b))


def kbps(nbytes, secs):
    if secs <= 0:
        return 0.0
    return (nbytes / 1024.0) / secs


def test_ping(s):
    send_hdr(s, 1, PING_COUNT)
    times = []
    k = 0
    while k < PING_COUNT:
        out = mkbytes([k & 0xFF])
        t0 = time.time()
        s.sendall(out)
        back = recv_exact(s, 1)
        times.append(time.time() - t0)
        if back != out:
            raise IOError("PING: the echoed byte differs from the one sent")
        k = k + 1
    avg = sum(times) / len(times)
    say("PING   %d round trips   min %.2f ms   avg %.2f ms   max %.2f ms" %
        (PING_COUNT, min(times) * 1000.0, avg * 1000.0, max(times) * 1000.0))
    return avg


def test_up(s, n, label):
    t0 = time.time()
    send_hdr(s, 2, n)
    s.sendall(PAT[:n])
    rep = recv_exact(s, 16)
    secs = time.time() - t0
    magic, got, srv_us, bad = struct.unpack(">4sIII", rep)
    if magic != wire("NBok"):
        raise IOError("UP: bad reply from the server")
    short = ""
    if got != n:
        short = "  [!!] short"
        bad = bad + 1
    mine = kbps(n, secs)
    say("UP     %s %7d bytes  %8.1f KB/s (Mac)  %8.1f KB/s (Pi)  bad bytes %d%s" %
        (label, n, mine, kbps(got, srv_us / 1e6), bad, short))
    return mine, bad


def test_down(s, n, label):
    t0 = time.time()
    send_hdr(s, 3, n)
    data = recv_exact(s, n)
    secs = time.time() - t0
    bad = mismatches(data, PAT[:n])                # verified OUTSIDE the timed window, as NetBench does
    mine = kbps(n, secs)
    say("DOWN   %s %7d bytes  %8.1f KB/s (Mac)  bad bytes %d" % (label, n, mine, bad))
    return mine, bad


def run(cmd):
    try:
        p = os.popen(cmd + " 2>/dev/null")
        out = p.read()
        p.close()
        return out
    except Exception:
        return ""


def describe_link(host, port):
    """Local IP, the interface the route to the Pi uses, and (Tiger) the AirPort link numbers."""
    ip = "?"
    try:
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.connect((host, port))
        ip = u.getsockname()[0]
        u.close()
    except Exception:
        pass
    iface = "?"
    for ln in run("/sbin/route -n get %s" % host).split("\n"):
        ln = ln.strip()
        if ln.startswith("interface:"):
            iface = ln.split(":", 1)[1].strip()
    say("local IP %s   interface %s   (Tiger: en0 = Ethernet, en1 = AirPort)" % (ip, iface))
    if iface == "en0":                             # 1.1: the first baseline run went over Ethernet unnoticed
        say("[!!] this run is going over ETHERNET (en0), not AirPort. For the wireless baseline, unplug")
        say("     the Ethernet cable (or make it inactive in System Preferences > Network) and run again.")
    # Every field EXCEPT the network's identifiers. 1.0 kept an allowlist of 10.5-era key names, and
    # Tiger 10.4's airport tool names its signal fields differently, so only lastTxRate/maxRate got
    # through on the first run. A denylist keeps whatever 10.4 calls them.
    got = []
    for ln in run(AIRPORT_TOOL + " -I").split("\n"):
        if ":" not in ln:
            continue
        k, v = ln.split(":", 1)
        k = k.strip()
        if not k or k.lower().find("ssid") >= 0:   # never SSID / BSSID (they locate the user's home)
            continue
        got.append("%s %s" % (k, v.strip()))
    if got:
        say("airport -I: " + "   ".join(got))
    else:
        say("airport -I: not available (Ethernet run, or no airport tool on this system)")


def main():
    host, port = "192.168.1.207", 5201
    if len(sys.argv) > 1:
        a = sys.argv[1]
        if ":" in a:
            host, p = a.split(":", 1)
            port = int(p)
        else:
            host = a
    say("==== %s  %s  (Python %d.%d on %s) ====" %
        (VERSION, time.strftime("%Y-%m-%d %H:%M:%S"), sys.version_info[0], sys.version_info[1], sys.platform))
    describe_link(host, port)
    say("connecting to %s:%d ..." % (host, port))
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(120.0)
    s.connect((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)   # pings must not wait on Nagle
    say("connected.")
    ping = test_ping(s)
    bad_sum = test_up(s, WARM, "warm")[1]
    up_sum = 0.0
    r = 1
    while r <= REPEATS:
        k, bad = test_up(s, TEST, "#%d  " % r)
        up_sum, bad_sum = up_sum + k, bad_sum + bad
        r = r + 1
    bad_sum = bad_sum + test_down(s, WARM, "warm")[1]
    down_sum = 0.0
    r = 1
    while r <= REPEATS:
        k, bad = test_down(s, TEST, "#%d  " % r)
        down_sum, bad_sum = down_sum + k, bad_sum + bad
        r = r + 1
    say("SUMMARY  up %.1f KB/s   down %.1f KB/s   ping %.2f ms   bad bytes %d" %
        (up_sum / REPEATS, down_sum / REPEATS, ping * 1000.0, bad_sum))
    note = wire("\r".join(lines))
    send_hdr(s, 4, len(note))
    s.sendall(note)
    recv_exact(s, 16)
    send_hdr(s, 5, 0)
    s.close()
    say("done. The transcript is also in the Pi's 'NetBench Server Log'.")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        say("[!!] stopped: %s -- is pi-netbench.sh running on the Pi?" % (sys.exc_info()[1],))
        sys.exit(1)
