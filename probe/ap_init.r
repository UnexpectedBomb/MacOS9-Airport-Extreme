/* ap_init.r -- packs the boot vehicle into ONE extension file:
 *     'INIT' (128)  the 68K code resource (ap_init.flt), run by the System at startup
 *     'PPC ' (128)  the PowerPC CFM fragment (AirPortBoot.pef), which ap_init.c loads with
 *                   Get1Resource + GetMemFragment
 *
 * ⚠ THIS IS A SEPARATE FILE FROM THE DRIVER, AND IT HAS TO BE. The System runs 'INIT' resources
 *   from files typed 'INIT'; our driver must stay typed 'shlb' because that is what CFM scans
 *   when resolving an import library by name, and k100 proved how unforgiving that is -- typing
 *   that file 'ndrv' broke GetSharedLibrary("AirPortShim") outright with -2804. One file cannot
 *   be both, so the vehicle ships beside the driver. Two extensions, still no application and
 *   no Startup Item, so the standing extension-only rule holds.
 */
#include "Retro68.r"
#include "Types.r"         /* 'vers' -- Retro68.r does not bring the standard types in */

type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128, locked) {
    dontBreakAtEntry, $$read("ap_init.flt");
};

/* The PowerPC fragment, embedded raw so the 68K INIT can GetMemFragment it. */
data 'PPC ' (128) {
    $$read("AirPortBoot.pef")
};

resource 'vers' (1, locked) {
    0x01, 0x87, beta, 0x00, verUS,
    "1.0b187",
    "AirPort Extreme Startup 1.0b187 (k187 -- MATCH USBEnet's PORT RECORD. k186's discriminator REFUTED the port-ref theory: portRef find == create == 0x0C0A0E00, so the ref was never wrong. Verified against Apple's OTModl$radio binary that our DL_INFO_ACK (state-varying 6/0 unbound, 8/-2 bound; style/service/version) AND the 0x4f02 framing reply match byte-for-byte; mac_type = DL_ETHER = 4. k186 fully JOINED (auth+assoc AID 1 + WPA2 4-way, link up) yet OT enumerated (INFO+0x4f02) and quit without DL_BIND. Our port record was copied from Apple's WORKING USBEnet driver EXCEPT one k159 hack: fPortFlags = kOTPortIsActive ('force visible'). A fresh port claiming ACTIVE reads to OT's cold config-apply as 'already up -- nothing to open/bind', which IS our enumerate-then-quit. k187: fPortFlags = 0 like USBEnet (the port is selectable without the hack). Snapshot adds qopen/qclose counts (1 open = enumerate-only; >1 = it tried to activate). ⚠ COLD boot; use the EXISTING Ethernet slot 14 config, do NOT create a new one. Select it, Configure: Using DHCP Server, Save -> wait 60s)"
};

resource 'vers' (2, locked) {
    0x01, 0x87, beta, 0x00, verUS,
    "1.0b187",
    "AirPort Extreme Startup 1.0b187 (k187 -- MATCH USBEnet's PORT RECORD. k186's discriminator REFUTED the port-ref theory: portRef find == create == 0x0C0A0E00, so the ref was never wrong. Verified against Apple's OTModl$radio binary that our DL_INFO_ACK (state-varying 6/0 unbound, 8/-2 bound; style/service/version) AND the 0x4f02 framing reply match byte-for-byte; mac_type = DL_ETHER = 4. k186 fully JOINED (auth+assoc AID 1 + WPA2 4-way, link up) yet OT enumerated (INFO+0x4f02) and quit without DL_BIND. Our port record was copied from Apple's WORKING USBEnet driver EXCEPT one k159 hack: fPortFlags = kOTPortIsActive ('force visible'). A fresh port claiming ACTIVE reads to OT's cold config-apply as 'already up -- nothing to open/bind', which IS our enumerate-then-quit. k187: fPortFlags = 0 like USBEnet (the port is selectable without the hack). Snapshot adds qopen/qclose counts (1 open = enumerate-only; >1 = it tried to activate). ⚠ COLD boot; use the EXISTING Ethernet slot 14 config, do NOT create a new one. Select it, Configure: Using DHCP Server, Save -> wait 60s)"
};
