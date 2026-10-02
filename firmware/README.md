# Firmware (BCM4306)

**The firmware blobs are not committed to this repository's source.** They are proprietary Broadcom
microcode. To build the driver yourself, fetch them with the one-line command below (the same fetch Linux
and the BSDs use). If you download the pre-built driver from the [Releases](../../releases) page, the
firmware is already embedded and you do not need to fetch anything.

The card is a BCM4306 (802.11 core revision 5), which uses the **v4** firmware generation. The build
embeds the blobs at compile time with `embed-fw.sh`, which defaults to `../firmware/v4`. The four blobs it
needs are `ucode5.fw`, `pcm5.fw`, `b0g0initvals5.fw`, and `b0g0bsinitvals5.fw`. Each carries an 8-byte
big-endian header that the driver checks at load, so a truncated or wrong-generation blob is rejected
rather than run.

## Fetch (v4, the default)

```bash
curl -sSL -o v4.tar.bz2 http://mirror2.openwrt.org/sources/broadcom-wl-4.178.10.4.tar.bz2
tar xjf v4.tar.bz2
b43-fwcutter -w . broadcom-wl-4.178.10.4/linux/wl_apsta.o
mv b43 v4        # keep only this chip's blobs
```

`sha256(v4.tar.bz2) = 32f6ad98facbb9045646fdc8b54bb03086d204153253f9c65d0234a5d90ae53f` (5,986,780 bytes).
`b43-fwcutter` identifies the archive as `wl_apsta.o` version 478.104.

## Fetch (v3, alternative)

The older v3 generation also works and is a simpler download (no `b43-fwcutter` needed):

```bash
curl -sSL -o v3.tbz https://leaf.dragonflybsd.org/~sephe/bwi/v3.tbz
tar -xjf v3.tbz            # creates v3/
```

`sha256(v3.tbz) = 9d7bfe981ad203f6b786c7bd8c7698bade947cb99b2e2472f00b61638a40b5c8` (32,222 bytes). This is
the URL NetBSD's `bwi.4` man page prints. To build against it, point `embed-fw.sh` at `../firmware/v3`.
