#!/usr/bin/env python3
"""Write the library plan (data/GM4E01/plan.tsv) from the ported symbols.

Every function of the retail DOL belongs to an object file of the debug map
(port_symbols.py), and the plan is decided per object here: lift (recompiled
with the game), replace (native, at the library's API) or drop (never runs).
docs/code-analysis.md gives the reasons. Consecutive objects with the same
decision become one range; a range runs from its first function to the next
range's first function (or the end of the code in its section), so the ranges
tile the code exactly.

  make_plan.py --symbols work/symbols.txt --dol main.dol [-o data/GM4E01/plan.tsv]

Without -o it prints the plan and checks it against data/GM4E01/plan.tsv.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcdol  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Dolphin.a, object by object: (plan, label). The SDK's libraries, the C library, the runtime
# and the debugger stub are all in this one archive.
SDK_GROUPS = [
    ("replace", "dolphin/base (CPU registers: HID, L2CR, MMCR ...)", ["PPCArch.c"]),
    ("lift", "dolphin/mtx", ["mtx.c", "mtxvec.c", "mtx44.c", "mtx44vec.c", "vec.c", "quat.c"]),
    ("replace", "dolphin/dvd", ["dvdlow.c", "dvdfs.c", "dvd.c", "dvdqueue.c", "dvderror.c", "dvdidutils.c",
                                "dvdFatal.c", "fstload.c"]),
    ("replace", "dolphin/vi", ["vi.c"]),
    ("lift", "dolphin/ai (its registers and DMA are emulated)", ["ai.c"]),
    ("replace", "dolphin/ar (ARAM)", ["ar.c"]),
    ("lift", "dolphin/arq (request queue over AR)", ["arq.c"]),
    ("lift", "dolphin/dsp (task manager; the DSP's mailboxes are emulated)", ["dsp.c", "dsp_debug.c", "dsp_task.c"]),
    ("replace", "dolphin/card", ["CARDBios.c", "CARDUnlock.c", "CARDRdwr.c", "CARDBlock.c", "CARDDir.c",
                                 "CARDCheck.c", "CARDMount.c", "CARDFormat.c", "CARDOpen.c", "CARDCreate.c",
                                 "CARDRead.c", "CARDWrite.c", "CARDDelete.c", "CARDStat.c", "CARDRename.c"]),
    ("replace", "dolphin/hio (host I/O, development kit)", ["hio.c"]),
    ("lift", "dolphin/gx (its FIFO and registers are emulated)",
     ["GXInit.c", "GXFifo.c", "GXAttr.c", "GXMisc.c", "GXGeometry.c", "GXFrameBuf.c", "GXLight.c", "GXTexture.c",
      "GXBump.c", "GXTev.c", "GXPixel.c", "GXDraw.c", "GXDisplayList.c", "GXTransform.c", "GXPerf.c"]),
    ("drop", "debugger link stubs (amcstubs, odenotstub)", ["AmcExi2Stubs.c", "odenotstub.c"]),
    ("drop", "OdemuExi2 (debugger link over EXI2)", ["DebuggerDriver.c"]),
    ("replace", "dolphin/exi", ["EXIBios.c", "EXIUart.c"]),
    ("lift", "dolphin/gd (display-list writer)", ["GDBase.c", "GDGeometry.c"]),
    ("lift", "dolphin/ip (TCP/IP stack, over eth)",
     ["IP.c", "IPArp.c", "IPIcmp.c", "IPRoute.c", "IPUdp.c", "IPFrag.c", "IPEther.c", "IFFifo.c", "IFRing.c",
      "IPTcp.c", "IPTcpOutput.c", "IPTcpTimer.c", "IPTcpUser.c", "IPTcpTimeWait.c", "IPDns.c", "IPDhcp.c",
      "IPZero.c", "IPOpt.c", "IPSocket.c", "IPPPP.c", "IPPPPoE.c", "IPLcp.c", "IPIpcp.c", "IPPap.c", "IPChap.c",
      "IPIgmp.c", "IPUuid.c"]),
    ("replace", "dolphin/eth (Broadband Adapter driver)", ["eth.c", "ethsec.c"]),
    ("lift", "dolphin/ip: md5, base64", ["md5.c", "base64.c"]),
    ("replace", "dolphin/os",
     ["OS.c", "OSAlarm.c", "OSAudioSystem.c", "OSCache.c", "OSContext.c", "OSError.c", "OSFont.c",
      "OSInterrupt.c", "OSLink.c", "OSMessage.c", "OSMemory.c", "OSMutex.c", "OSReboot.c", "OSReset.c",
      "OSResetSW.c", "OSRtc.c", "OSSync.c", "OSThread.c", "OSTime.c"]),
    ("lift", "dolphin/os: OSAlloc (heaps) and OSArena (arena bounds), no hardware access",
     ["OSAlloc.c", "OSArena.c"]),
    ("lift", "dolphin/os: __init_user, __init_cpp, _ExitProcess (the EABI's constructors)", ["__ppc_eabi_init.cpp"]),
    ("lift", "dolphin/pad: PADClamp (stick clamping, no hardware access)", ["Padclamp.c"]),
    ("replace", "dolphin/pad", ["Pad.c"]),
    ("replace", "dolphin/si", ["SIBios.c", "SISamplingRate.c", "SISteering.c", "SISteeringXfer.c",
                               "SISteeringAuto.c"]),
    ("replace", "dolphin/db (debugger hooks)", ["db.c"]),
    ("lift", "dolphin/thp (movie decoder)", ["THPDec.c", "THPAudio.c"]),
    ("lift", "Logitech steering-wheel force feedback (over SISteering)", ["allsrc.c"]),
    ("lift", "dolphin/upnp (over ip)", ["UPnP.c", "UPnPHttp.c", "UPnPSsdp.c", "UPnPUuid.c", "UPnPUri.c",
                                        "UPnPHttpd.c", "UPnPHttpdResponse.c"]),
    ("lift", "EABI runtime",
     ["__va_arg.c", "global_destructor_chain.c", "CPlusLibPPC.cp", "NMWException.cp", "ptmf.c", "runtime.c",
      "__init_cpp_exceptions.cpp", "Gecko_ExceptionPPC.cp", "GCN_mem_alloc.c"]),
    ("lift", "EABI MSL (C library)",
     ["abort_exit.c", "alloc.c", "ansi_files.c", "ansi_fp.c", "arith.c", "bsearch.c", "buffer_io.c",
      "critical_regions.gamecube.c", "ctype.c", "direct_io.c", "file_io.c", "FILE_POS.C", "mbstring.c", "mem.c",
      "mem_funcs.c", "misc_io.c", "printf.c", "scanf.c", "signal.c", "string.c", "strtold.c", "strtoul.c",
      "wchar_io.c", "uart_console_io_gcn.c", "e_asin.c", "e_atan2.c", "e_exp.c", "e_pow.c", "e_rem_pio2.c",
      "k_cos.c", "k_rem_pio2.c", "k_sin.c", "k_tan.c", "s_atan.c", "s_ceil.c", "s_copysign.c", "s_cos.c",
      "s_floor.c", "s_frexp.c", "s_ldexp.c", "s_modf.c", "s_sin.c", "s_tan.c", "w_asin.c", "w_atan2.c", "w_exp.c",
      "w_pow.c", "e_sqrt.c", "math_ppc.c", "w_sqrt.c", "extras.c"]),
    ("drop", "MetroTRK (debugger stub)",
     ["mainloop.c", "nubevent.c", "nubinit.c", "msg.c", "msgbuf.c", "serpoll.c", "usr_put.c", "dispatch.c",
      "msghndlr.c", "support.c", "mutex_TRK.c", "notify.c", "flush_cache.c", "mem_TRK.c", "targimpl.c",
      "mpc_7xx_603e.c", "dolphin_trk.c", "main_TRK.c", "dolphin_trk_glue.c", "targcont.c", "target_options.c",
      "mslsupp.c", "UDP_Stubs.c", "main.c", "CircleBuffer.c", "MWTrace.c", "MWCriticalSection_gc.cpp",
      "__exception.s"]),
]
# .init is laid out by the linker, object by object, before the text.
INIT = {
    "__start.c": ("replace", ".init: OS start-up (__start, __init_registers, __init_data)"),
    "__ppc_eabi_init.cpp": ("replace", ".init: OS start-up (__init_hardware, __flush_cache)"),
    "__mem.c": ("lift", ".init: EABI runtime (memset, __fill_mem, memcpy)"),
    "mem_TRK.c": ("drop", ".init: debugger stub and exception vectors"),
    "dolphin_trk.c": ("drop", ".init: debugger stub and exception vectors"),
}
# Single functions decided apart from their object.
FUNCTIONS = {
    "OSReport": ("lift", "dolphin/os: OSReport (formats through MSL's vprintf)"),
    "__PADDisableRecalibration": ("lift", "dolphin/pad: __PADDisableRecalibration (a flag in low memory)"),
    "EXIGetType": ("lift", "dolphin/exi: EXIGetType (a table over EXIGetID)"),
    # MSL's FILE procedures on the debugger's console: stdin/stdout/stderr point at them, and
    # exit's __close_all calls through them. With no debugger they report an I/O error.
    "__TRK_write_console": ("replace", "MetroTRK: console procedures behind MSL's FILE (no debugger)"),
    "__read_console": ("replace", "MetroTRK: console procedures behind MSL's FILE (no debugger)"),
}
# JSystemV.a: its sub-libraries, by object name. The retail link order is theirs.
JSYSTEM = [
    ("J3DU", "JSystem/J3DU"), ("JKR", "JSystem/JKernel"), ("JSU", "JSystem/JSupport"),
    ("hashcode", "JSystem/JGadget"), ("linklist", "JSystem/JGadget"), ("JUT", "JSystem/JUtility"),
    ("Wheels", "JSystem/JUtility (force feedback)"), ("Constant", "JSystem/JUtility (force feedback)"),
    ("Force", "JSystem/JUtility (force feedback)"), ("LGWheels", "JSystem/JUtility (force feedback)"),
    ("Periodic", "JSystem/JUtility (force feedback)"), ("Ramp", "JSystem/JUtility (force feedback)"),
    ("Conditon", "JSystem/JUtility (force feedback)"), ("JMath", "JSystem/JMath"), ("random", "JSystem/JMath"),
    ("JMA", "JSystem/JMath"), ("JHI", "JSystem/JHostIO"), ("JOR", "JSystem/JHostIO"), ("J2D", "JSystem/J2DGraph"),
    ("J3D", "JSystem/J3D"), ("JFW", "JSystem/JFramework"), ("JPA", "JSystem/JParticle"),
    ("JAI", "JSystem/JAudio (JAInterface)"), ("JAS", "JSystem/JAudio (JASystem)"),
    ("dspproc", "JSystem/JAudio (JASystem: DSP driver)"), ("dsptask", "JSystem/JAudio (JASystem: DSP driver)"),
    ("osdsp", "JSystem/JAudio (JASystem: DSP driver)"), ("JAU", "JSystem/JAudio (JAUtility)"),
]
GAME = {"BandoS.a": "game: Bando (engine sound)", "InagakiS.a": "game: Inagaki (audio)",
        "KamedaS.a": "game: Kameda (2D screens, scenes)", "KaneshigeS.a": "game: Kaneshige (race, course, movies)",
        "KawanoS.a": "game: Kawano (drivers, animation)", "OsakoS.a": "game: Osako (system, applications, network)",
        "SatoS.a": "game: Sato (items, objects, effects)", "ShiraiwaS.a": "game: Shiraiwa (map objects, wipes)",
        "YamamotoS.a": "game: Yamamoto (kart physics, camera)", "-": "game: kartLocale"}


def decide(address, name, lib, obj, init_end):
    if name in FUNCTIONS:
        return FUNCTIONS[name]
    if lib == "Dolphin.a":
        if address < init_end:
            return INIT[obj]
        for plan, label, objects in SDK_GROUPS:
            if obj in objects:
                return plan, label
        raise SystemExit(f"no decision for Dolphin.a object {obj} ({name} at {address:08X})")
    if lib == "JSystemV.a":
        for prefix, label in JSYSTEM:
            if obj.startswith(prefix):
                return "lift", label
        raise SystemExit(f"no JSystem sub-library for object {obj}")
    return "lift", GAME[lib]


def build(symbols, dol):
    text = sorted((a, a + s) for a, s, _, t, _ in dol.sections if t)
    init_end = text[0][1]
    rows = []
    alone_end = None
    for address, size, name, lib, obj, _ in sorted(symbols):
        if lib == "-" and alone_end is None:
            # kartLocale.o is linked alone before JSystem and has one function: what follows
            # it, named or not, is JSystem's (J3DU's weak template functions, mostly unnamed)
            alone_end = address + size
        elif alone_end:
            address, alone_end = alone_end, 0
        if lib == "-" and alone_end == 0:
            lib, obj = "JSystemV.a", "J3DUMotion.cpp"
        plan, label = decide(address, name, lib, obj, init_end)
        section = next(i for i, (lo, hi) in enumerate(text) if lo <= address < hi)
        if rows and rows[-1][2:] == [plan, label, section]:
            continue
        rows.append([address, None, plan, label, section])
    # a range ends where the next begins; the last of a section at the end of its code
    for i, row in enumerate(rows):
        nxt = rows[i + 1] if i + 1 < len(rows) else None
        row[1] = nxt[0] if nxt and nxt[4] == row[4] else code_end(dol, text[row[4]])
    return [(a, b, plan, label) for a, b, plan, label, _ in rows]


def code_end(dol, section):
    """End of the code in a text section: .init ends with __init_data's tables and padding."""
    lo, hi = section
    if lo == min(a for a, _, _, t, _ in dol.sections if t):
        # _rom_copy_info follows the last function: its first entry is the section itself
        for a in range(lo, hi, 4):
            if dol.u32(a) == lo and dol.u32(a + 4) == lo:
                return a
    while hi > lo and dol.u32(hi - 4) == 0:
        hi -= 4
    return hi


def text_of(rows):
    out = ["# The library plan: which code is lifted, replaced or dropped (docs/code-analysis.md).\n",
           "# Written by tools/analysis/make_plan.py from the ported symbols; read by qilin lift --plan.\n",
           "# start\tend\tplan\tlibrary\n"]
    out += [f"{a:08X}\t{b:08X}\t{plan}\t{label}\n" for a, b, plan, label in rows]
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--symbols", required=True, help="work/symbols.txt (port_symbols.py)")
    ap.add_argument("--dol", required=True)
    ap.add_argument("-o", "--output")
    args = ap.parse_args()
    rows = build(gcdol.read_symbols(args.symbols), gcdol.Dol(args.dol))
    text = text_of(rows)
    if args.output:
        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print(f"wrote {len(rows)} ranges to {args.output}")
        return 0
    sys.stdout.write(text)
    committed = os.path.join(ROOT, "data", "GM4E01", "plan.tsv")
    if os.path.exists(committed):
        same = open(committed, encoding="utf-8").read() == text
        print(f"# {'same as' if same else 'DIFFERS from'} {committed}", file=sys.stderr)
        return 0 if same else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
