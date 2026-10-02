# MacOS9-Airport-Extreme

A **Mac OS 9 driver for the Apple AirPort Extreme card (Broadcom BCM4306)**: 802.11g with
**WPA2-Personal and open networks**, on a Power Mac G4 running Mac OS 9.2.2.

Apple shipped no OS 9 driver for the AirPort Extreme card, and the original 802.11b AirPort card tops out
at WEP, which no modern network accepts. This driver puts a real, modern Wi-Fi connection on an OS 9 Mac
with its own hardware: it scans, joins WPA2-secured or open networks, remembers them, and rejoins on its
own at start-up, all from a native control panel and Control Strip module, with no Mac OS X involved.

> **Status: working on real hardware.** WPA2-Personal and open-network joins are tested on a Power Mac G4
> MDD FW800 with an A1026 card. It is a hobby project, offered as-is; see [Limitations](#limitations).

## What you get

Three native OS 9 pieces:

- **AirPort Extreme** (system extension): the driver. BCM4306 bring-up, an 802.11 station stack, a
  WPA2-Personal supplicant (PBKDF2 / 4-way handshake / CCMP), and the Open Transport DLPI glue so TCP/IP
  runs over it like any Ethernet port.
- **AirPort Extreme** (control panel): signal and status, on/off, scan and choose a network, enter a
  password once, forget a network, and an About box.
- **AirPort Extreme Strip** (Control Strip module): signal, on/off, and choosing a network from the strip.

## Requirements

- A Mac that boots **Mac OS 9.2.2**. The **Power Mac G4 MDD FW800** (`PowerMac3,6`) is the tested machine;
  see [Compatibility](#compatibility) for other machines.
- A **Broadcom BCM4306-based AirPort Extreme card**. The common one is Apple's **A1026** (BCM4306, PCI ID
  `14e4:4320`). Not every card sold as "AirPort Extreme" is a BCM4306: the later Wi-Fi/Bluetooth combo cards
  (A1126 / A1127) use the **BCM4318** (`14e4:4318`), which this driver does not support. The driver checks
  the card at start-up and steps aside cleanly if it is not a BCM4306, so an unsupported card does no harm.
- **Broadcom firmware**: proprietary, not included here. See [firmware/README.md](firmware/README.md) for
  the one-line fetch (the same blobs Linux and the BSDs use).

## Install

The three pieces are available ready to install on the [Releases](../../releases) page: download and decode
them (they are BinHex `.hqx` files; StuffIt Expander on OS 9 handles them). Or build them yourself (see
[Building](#building)). Either way:

1. Put the driver (**AirPort Extreme Driver**) in the **Extensions** folder. If you installed an earlier
   build that was named `AirPortExtreme.shlb`, remove it first; two copies of the same driver fragment
   must not be present at once.
2. Put the **AirPort Extreme** control panel in **Control Panels**.
3. Put **AirPort Extreme Strip** in **Control Strip Modules**.
4. Restart. In the **TCP/IP** control panel choose *Connect via: AirPort Extreme* and close it to save.

## Compatibility

Only the **Power Mac G4 MDD FW800** has been tested on real hardware. The driver no longer assumes that
machine's antenna wiring (it lets the card pick its antenna automatically), so it should work on any OS
9.2.2 Mac fitted with a **BCM4306** AirPort Extreme card. The table lists the OS 9-capable G4s that took the
discrete **A1026** card: all are expected to work, but only the MDD FW800 is confirmed. Every other row
needs a community report to move from No to Yes.

| Model | Model ID | Confirmed working |
|---|---|---|
| Power Mac G4 MDD FW800 | PowerMac3,6 | Yes |
| eMac G4 1.25GHz (USB 2.0) | PowerMac6,4 | No |
| eMac G4 1.42GHz (2005) | PowerMac6,4 | No |
| iMac G4 15-inch 1.0GHz | PowerMac6,3 | No |
| iMac G4 17/20-inch 1.25GHz | PowerMac6,3 | No |
| Mac mini G4 (Early 2005) | PowerMac10,1 | No |
| iBook G4 (2003-2004, Radeon 9200) | PowerBook6,3 / 6,5 | No |
| Al PowerBook G4 12-inch 867MHz | PowerBook6,1 | No |
| Al PowerBook G4 12-inch 1.0-1.5GHz | PowerBook6,2 / 6,4 / 6,8 | No |
| Al PowerBook G4 17-inch 1.0GHz | PowerBook5,1 | No |
| Al PowerBook G4 15/17-inch (Radeon) | PowerBook5,2-5,7 | No |

**Check your card, not just your Mac.** The A1026 card shipped in two chip revisions: early cards are the
**BCM4306** (supported); later ones are the **BCM4318** (not supported). In Apple System Profiler, a card
reporting PCI ID `14e4:4320` or `14e4:4325` is a BCM4306 and will be driven; `14e4:4318` is a BCM4318 and is
declined cleanly. These machines use only the BCM4318 combo card (A1126/A1127) and are **not supported**:
Mac mini G4 (Late 2005, PowerMac10,2), iBook G4 Mid 2005 (PowerBook6,7), Al PowerBook G4 DLSD
(PowerBook5,8 / 5,9). The Xserve G4 had no AirPort option.

Reports of success or failure are very welcome: please say which Mac, which card and its PCI ID, and what
happened.

## Coexistence with Apple's AirPort software

This driver is independent and runs alongside Apple's original AirPort software, so you do not need to
remove Apple's AirPort extensions. It claims the AirPort Extreme (BCM4306) card as its own Open Transport
port (`OTModl$AirPortBCM`), separate from Apple's driver for the original 802.11b card.

One caveat, and it is an Apple bug rather than this driver's: Apple's **AirPort AP** extension crashes at
startup if the **TCP/IP Preferences** file is missing or corrupt *and* no original 802.11b AirPort card is
present. If you do not use an original AirPort card you can safely remove **AirPort AP** and **AirPort AP
Support**; **AirPort Driver** can stay (it just finds no card and idles). And do not delete TCP/IP
Preferences while **AirPort AP** is installed.

## Use

Open the **AirPort Extreme** control panel (or the Control Strip), pick a network from the list, and for a
WPA2 network type the password once. The Mac stores it in **AirPort Extreme Known Networks** (in the
Preferences folder) and rejoins on its own at every start-up. Open networks join with no password.

## Security

**WPA2-Personal** (PSK, CCMP) and **open** networks. A WPA3-transition network joins through its WPA2 half.
Not supported: WEP, the original WPA/TKIP, WPA3-only (SAE), or 802.1X / Enterprise.

## Limitations

- Tested only on the MDD FW800; other machines rely on community reports (see [Compatibility](#compatibility)).
- No hidden-network ("closed SSID" / manual "Other...") entry yet.
- With a base station that presents several BSSIDs for one SSID, a join may occasionally come back from a
  different BSSID of the same network; this is being hardened.
- It is a hobby driver: expect rough edges, and do not rely on it where failure matters.

## Building

Each of `probe/` (the driver), `cpanel/` (the control panel) and `csm/` (the Control Strip module) is a
CMake project built with the [Retro68](https://github.com/autc04/Retro68) PowerPC toolchain. The exact
`cmake` configure line is in the comment at the top of each `CMakeLists.txt`; the driver's final target is
`AirPortDriver`. Host-side unit tests (the pure logic: scan parsing, the known-networks store, the panel
view) build with a normal `cc` and run on the development machine.

## Credits and license

Built by reverse-engineering and adapting Linux **b43** / **mac80211**, BSD **bwi(4)**, and the **hostap**
project (the WPA2 crypto); see [CREDITS](CREDITS). Released under the **GNU General Public License v2**; see
[LICENSE](LICENSE).

AirPort and AirPort Extreme are trademarks of Apple Inc. This is an independent, unofficial driver, not
affiliated with or endorsed by Apple.

## Related

- [MacOS9-USB2-EHCI](https://github.com/UnexpectedBomb/MacOS9-USB2-EHCI): USB 2.0 for Mac OS 9
- [MacOS9-FW800-FireWire](https://github.com/UnexpectedBomb/MacOS9-FW800-FireWire): FireWire 800 fix
