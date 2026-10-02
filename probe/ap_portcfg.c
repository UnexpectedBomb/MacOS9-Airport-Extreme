/* ap_portcfg.c -- OTPortCfg$AirPortCfgHelper: the Open Transport CONFIGURATION HELPER fragment.
 *
 * ★ WHY (k188, fragment 2 of 3). The port record our scanner registers names this helper in its
 * fResourceInfo field ("AirPortCfgHelper"); OT builds "OTPortCfg$" + that = OTPortCfg$AirPortCfgHelper
 * and loads it to get the port's user-visible name and icon for the TCP/IP control panel. Both of
 * Apple's working extension Ethernet drivers ship one (OTPortCfg$USBEnetCfgHelper, OTPortCfg$radio);
 * we were the only one without, so we add it to match the shape exactly.
 *
 * ★ THE CONTRACT (kPortConfigLibPrefix, OpenTransportProtocol.h), two exports:
 *     OTGetUserPortName(OTPortRecord*, includeSlot, includePort, Str255 name)  -- the friendly name
 *     OTGetPortIcon(OTPortRecord*, OTResourceLocator*) -> Boolean               -- false = no icon
 *
 * Apple's USBEnet helper (disassembled) loads its EnetShimLib to read the connected adapter's real
 * device name, falling back to a constant "External USB Ethernet". OUR card is fixed and driven by
 * our own OTModl -- there is no shim to consult -- so we return the constant "AirPort Extreme" and no
 * icon (OT draws its default network-port icon). No imports, no hardware, no allocation: this fragment is
 * pure and re-entrant, exactly what a config helper is meant to be.
 *
 * ★ k219: the name is "AirPort Extreme" (was "AirPort"), so the TCP/IP "Connect via" menu makes it
 *   obvious which driver is in use, and it matches the name the install docs tell the user to choose.
 */
#include <MacTypes.h>

#define AP_PORTCFG_BUILD 2

/* OTGetUserPortName -- OT calls this to fill the "Connect via" name. `userVisibleName` is a Str255
 * (Pascal string: [0]=length, [1..]=chars). includeSlot/includePort ask for disambiguating suffixes
 * for multiport / multi-card setups; we are a single fixed card, so a constant name is unambiguous
 * (it will not collide with the built-in "Ethernet built-in"). Signature arg widths match the OT
 * register layout r3..r6. */
void OTGetUserPortName(void *port, unsigned long includeSlot, unsigned long includePort,
                       unsigned char *userVisibleName);
void OTGetUserPortName(void *port, unsigned long includeSlot, unsigned long includePort,
                       unsigned char *userVisibleName)
{
    static const char *kName = "AirPort Extreme";
    int i = 0;
    (void)port; (void)includeSlot; (void)includePort;
    if (!userVisibleName) return;
    while (kName[i] && i < 63) { userVisibleName[i + 1] = (unsigned char)kName[i]; i++; }
    userVisibleName[0] = (unsigned char)i;
}

/* OTGetPortIcon -- return false: no custom icon family, OT uses its default network-port icon. */
Boolean OTGetPortIcon(void *port, void *iconLocation);
Boolean OTGetPortIcon(void *port, void *iconLocation)
{
    (void)port; (void)iconLocation;
    return false;
}
