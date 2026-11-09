# Firmware (BCM4306 / BCM4318)

**The firmware blobs are not committed to this repository's source.** They are proprietary Broadcom
microcode. To build the driver yourself, fetch them with the commands below (the same fetches Linux
and the BSDs use). If you download the pre-built driver from the [Releases](../../releases) page, the
firmware is already embedded and you do not need to fetch anything.

The driver carries **two firmware generations** and picks one for each card at start-up, by the card's
802.11 core revision. `embed-fw.sh` embeds both at compile time, so **both sets are needed to build**:

- **v4**, in `firmware/v4`: for 802.11 core revision 5 and later, which is the later BCM4306 and the
  BCM4318. Blobs: `ucode5.fw`, `pcm5.fw`, `b0g0initvals5.fw`, `b0g0bsinitvals5.fw`.
- **v3**, in `firmware/v3`: for 802.11 core revision 4, the older BCM4306 on the earliest A1026 cards.
  Blobs: `ucode4.fw`, `pcm4.fw`, `b0g0initvals2.fw`.

Each blob carries an 8-byte big-endian header that the driver checks at load, so a truncated or
wrong-generation blob is rejected rather than run. Run both fetches from this `firmware/` directory, so the
blobs land where `embed-fw.sh` looks for them.

## Fetch v4

```bash
curl -sSL -o v4.tar.bz2 http://mirror2.openwrt.org/sources/broadcom-wl-4.178.10.4.tar.bz2
tar xjf v4.tar.bz2
b43-fwcutter -w . broadcom-wl-4.178.10.4/linux/wl_apsta.o
mv b43 v4        # keep only this chip's blobs
```

`sha256(v4.tar.bz2) = 32f6ad98facbb9045646fdc8b54bb03086d204153253f9c65d0234a5d90ae53f` (5,986,780 bytes).
`b43-fwcutter` identifies the archive as `wl_apsta.o` version 478.104.

## Fetch v3

The v3 set is a simpler download (no `b43-fwcutter` needed):

```bash
curl -sSL -o v3.tbz https://leaf.dragonflybsd.org/~sephe/bwi/v3.tbz
tar -xjf v3.tbz            # creates v3/
```

`sha256(v3.tbz) = 9d7bfe981ad203f6b786c7bd8c7698bade947cb99b2e2472f00b61638a40b5c8` (32,222 bytes). This is
the URL NetBSD's `bwi.4` man page prints.
