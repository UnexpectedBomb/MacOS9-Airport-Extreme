/* ap_shim.r -- the CFM fragment declaration for the AirPort Ethernet Shim driver.
 *
 * A SHARED LIBRARY, not an ndrv. EnetShimLib binds to us through a CFM connection whose ID the
 * registration call carries, so CFM must be able to find us BY FRAGMENT NAME -- which is what the
 * 'cfrg' below declares. That is also why there is no patch-pef-main.py step here: `main` matters
 * for VerifyFragmentAsDriver on a Device Manager ndrv, and we are not one.
 *
 * kImportLibraryCFrag   = a shared library others connect to (not an application, not a driver)
 * kDataForkCFragLocator = the PEF lives in the data fork, which is where MakePEF puts it
 */
#include "CodeFragmentTypes.r"

/* The extension's icon family, at ID -16455 ("Item Icon") -- the custom-file-icon
 * convention. Generated from the supplied icon file by icons-to-rez.py and committed, so
 * every build carries it and nothing has to be grafted on afterwards. ⚠ The matching
 * kHasCustomIcon Finder flag is set by package-probe.sh; resources without the flag show
 * nothing, and the flag without resources shows a blank. */
#include "ap_shim_icons.r"

/* ★★★ THE 'cfrg' IS GENERATED, AND THAT IS NOT A STYLE CHOICE.
 *
 * It declares TWO members over the SAME PEF:
 *
 *     'pwpc'  "AirPortShim"   the import library. CFM registers this BY NAME and EnetShimLib
 *                             and the test application both find us through it. k100 proved how
 *                             load-bearing that is: typing the FILE 'ndrv' stopped CFM scanning
 *                             it and GetSharedLibrary returned -2804 cfragNoLibraryErr.
 *     'ndrv'  "AirPortShim"   the same bytes, described again for the Device Manager. This is
 *                             exactly how Apple's AirPort Driver declares OTModl$radio -- once
 *                             'pwpc', once 'ndrv', in a file that also carries WirelessAPI as
 *                             an import library. One file, two roles, architecture separates.
 *
 * ⛔ RETRO68'S REZ CANNOT EXPRESS A MULTI-MEMBER cfrg. CodeFragments.r's template rejects the
 *   second array member with "extra value specified" -- verified with a minimal test in which
 *   BOTH members were identical PowerPC entries, so it is the template and not the content. The
 *   blob is emitted by make-cfrg.py instead, whose header documents the layout and which
 *   round-trips through the same parser used to read Apple's driver. */
#include "ap_shim_cfrg.r"
