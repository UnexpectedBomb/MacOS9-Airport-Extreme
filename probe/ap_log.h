/* ap_log.h -- the HOST logging sink, split out of ap_bringup.h by Stage 8-2d-0.
 *
 * ★ WHY THIS FILE EXISTS. The driver needs ap_bringup.h's hardware and must NOT have the File
 * Manager reachable -- FSWrite below task level hangs Mac OS 9 with no NMI and no debugger, which
 * is this project's oldest hard rule. But ap_bringup.h's hardware code calls Say(), and so do the
 * 736 log statements inside ap_phy_initg.h, whose own header refuses to have them stripped:
 * "Stripping the ~680 Say() lines would have meant editing the body rather than copying it, which
 * is precisely the risk this file exists to remove."
 *
 * It is right. So the call sites are left alone and the SINK is made replaceable instead.
 * Everything funnels through Out(), which is one function.
 *
 *   the application includes ap_log.h  -> Out() writes to a file on the Desktop
 *   the driver defines its own Out()   -> the interrupt-safe ring in ap_ilog.h, no File Manager
 *
 * ⚠ INCLUDE THIS BEFORE ap_bringup.h (or before ap_phy_initg.h, which pulls ap_bringup.h in).
 *   ap_bringup.h deliberately does NOT include it: a driver that wants the hardware must be able
 *   to get it without dragging the File Manager along, and an #include here would take that
 *   choice away.
 *
 * ⚠⚠ THE BODY BELOW IS A VERBATIM MOVE from ap_bringup.h lines 135-173, not a rewrite. That is
 *   checkable and was checked: reconstructing the original file from this block plus the new
 *   ap_bringup.h reproduces the previous contents exactly. A logging refactor that silently
 *   changed a format string would corrupt every comparison this project makes against its banked
 *   logs, so "it still compiles" was not good enough.
 */
#ifndef AP_LOG_H
#define AP_LOG_H

/* The Toolbox surface the logging needs, and the window/event headers the probe applications
 * relied on reaching THROUGH ap_bringup.h. Kept together here so no caller loses an include it
 * used to get transitively -- that would turn a pure refactor into a debugging session. */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <Files.h>
#include <Folders.h>
#include <Script.h>

/* ⚠ THE ONE INTENTIONAL DEVIATION from the verbatim extraction, recorded so it is not mistaken
 * for drift. In airport_accessors.c, LogOpen() hardcodes "\pAirPort Accessors Log". Here that
 * literal is replaced by AP_LOG_NAME so two probes do not overwrite each other's log -- which
 * would resurrect exactly the stale-log confusion that cost a cycle between g2 and g3. Nothing
 * else in the extracted body differs; `diff` against airport_accessors.c:238-512 shows this line
 * and no other. A probe defines AP_LOG_NAME before including this header. */
#ifndef AP_LOG_NAME
#define AP_LOG_NAME "\pAirPort Probe Log"
#endif
/* ⚠ THE FALLBACK IS A HAZARD, AND IT IS KEPT ONLY TO PRESERVE THIS REFACTOR'S PURITY.
 * A probe that forgets to define AP_LOG_NAME silently writes "AirPort Probe Log" -- the same
 * name as every other forgetful probe -- which is exactly the stale-log confusion the comment
 * above says cost a cycle between g2 and g3, and which very nearly ended the CardBus work.
 * Replacing this #ifndef with a hard #error would be a real improvement and is a DIFFERENT
 * change: 8-2d-0 is a verbatim move and is proven by reconstruction, so it does not get to
 * smuggle a behaviour change in alongside. Worth doing next. */

#define kFontIDMonaco 4

static short gLogRef=0,gLogVol=0; static long gLogDir=0;
static Str255 gLines[220]; static short gN=0;
/* ★ THE LOG GOES ON THE DESKTOP, not into the System Folder.
 *
 * It used to land in the System Folder, which meant every run buried a file the tester then had
 * to dig out before copying it to the share. The Desktop is where it is actually wanted.
 *
 * ⚠ FALL BACK TO THE SYSTEM FOLDER RATHER THAN TO NOTHING. kDesktopFolderType is resolved per
 *   volume, and a failure here would silently produce a run with NO LOG AT ALL -- which on this
 *   project is a wasted reboot, and worse, one that looks like a crash. The old destination is
 *   known to work, so it stays as the fallback and the banner says which was used. */
static int gLogOnDesktop = 0;
static void LogOpen(void){FSSpec sp;
  if(FindFolder(kOnSystemDisk,kDesktopFolderType,kDontCreateFolder,&gLogVol,&gLogDir)==noErr)
    gLogOnDesktop = 1;
  else if(FindFolder(kOnSystemDisk,kSystemFolderType,kDontCreateFolder,&gLogVol,&gLogDir)!=noErr)
    return;
  if(FSMakeFSSpec(gLogVol,gLogDir,AP_LOG_NAME,&sp)==noErr)FSpDelete(&sp);
  if(FSpCreate(&sp,'ttxt','TEXT',smSystemScript)!=noErr)return;
  if(FSpOpenDF(&sp,fsRdWrPerm,&gLogRef)!=noErr)gLogRef=0;}
static void Out(Str255 s){
  if(gLogRef){long len=s[0];char cr='\r';FSWrite(gLogRef,&len,&s[1]);len=1;FSWrite(gLogRef,&len,&cr);FlushVol(NULL,gLogVol);}
  if(gN<220){BlockMoveData(s,gLines[gN],(long)s[0]+1);gN++;}}

/* ★ 8-2d-1: PCat, PCatHex, PCatDec and Say/SayH/Say1 moved to ap_fmt.h, and it is included
 * BELOW rather than at the top -- they call Out(), so they have to come after it. The driver
 * defines a DIFFERENT Out() and includes this same ap_fmt.h, so both sinks share one copy of the
 * formatters and a change to a format string cannot drift between the app's log and the driver's
 * narration of the same bring-up. */
#include "ap_fmt.h"

#endif /* AP_LOG_H */
