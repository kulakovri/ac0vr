# Ace Combat Zero (SLUS-213.46): SDK symbol coverage

`config_zero/sdk/crossmatch_sdk.py` generated this file. It cross-matches the SDK functions labelled for AC5 (SLUS-208.51, in `config/sdk_symbols.json` and `config/manual_symbols.json`) against Zero's code. Sony's libraries are not needed. To re-run it, go to the repo root and run `PYTHONPATH=tools python -I config_zero/sdk/crossmatch_sdk.py`. It takes about a minute.

**The SDK versions differ.** AC5 links SDK **2.8.0** (libmc 2.8.1). Zero links **3.0.0** (libdbc, libpad2 and libmc are 3.0.2). This comes from the `PsIIlibxxxx NNNN` strings in both executables. Most library code is identical modulo relocations. libmc, libcdvd, libmpeg, libvu0 and fileio changed. SDK 3.0's libcdvd is split into more objects, so the functions Zero never calls are not linked.

Match kinds:

- **exact**: identical after masking relocatable fields (jal/j targets, the halves of absolute addresses, gp offsets, branches out of the function).
- **near**: same length, at most max(2, n/10) words differ.
- **fuzzy r**: different body; r is the similarity of the masked instruction streams.
- **callgraph r**: reached through a matched function's call site or function pointer.
- **vu0sig r**: same VU0 macro-instruction sequence. SDK 3.0 libvu0 is compiled much more compactly, so the full bodies do not compare.
- **layout r**: the best function start between matched neighbours.

Columns:

- `ratio`: body similarity of the whole function.
- `ev ok/wrap/miss/bad`: the AC5 body's calls to, and pointers at, other matched SDK functions. **ok** means the Zero body uses the counterpart. **wrap** means it reaches the counterpart through a new SDK 3.0 helper. **miss** means it does not use it (the code changed). **bad** means that at the same place it calls a *different* known SDK function.
- `entry`: whether Zero's config-less partition (`python -m ps2recomp game/SLUS_213.46`) already has a function there. ps2recomp refuses an override on an address that is not a recovered function.

Each entry in `sdk_symbols.json` keeps AC5's `lib` string and appends `; xmatch AC5 0x... <kind>`. When the cross-match is weak, `(aligned)` is added. Weak means a score below 0.75 without at least 3 agreeing SDK references (ok + wrap, and at least twice the misses), a `bad` reference, or a displacement outlier. EE syscall stubs (`li v1,N; syscall`) that AC5's table does not name are paired by syscall number and count as references too. As in AC5's table, ps2recomp then refuses to use the name for an override or hook. AC5's own `(aligned)` tags are kept.

## Symbols named in config/overrides.json

| key | AC5 symbol | AC5 addr | Zero addr | match | ratio | ev ok/wrap/miss/bad | entry | draft | notes |
|---|---|---|---|---|---|---|---|---|---|
| `sceSifInitRpc` | sceSifInitRpc | 00331320 | **002FEE90** | exact | 1.00 | 12/0/0/0 | yes | ok |  |
| `sceSifExitRpc` | sceSifExitRpc | 003314C0 | **002FF030** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceSifBindRpc` | sceSifBindRpc | 00331AB8 | **002FF638** | callgraph 0.93 | 0.93 | 6/0/0/0 | yes | ok | via 2 call site(s)/reference(s) |
| `sceSifCallRpc` | sceSifCallRpc | 00331C88 | **002FF818** | callgraph 0.96 | 0.96 | 7/0/0/0 | yes | ok | via 22 call site(s)/reference(s) |
| `sceSifCheckStatRpc` | sceSifCheckStatRpc | 00331E80 | **002FFA18** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `sceSifSetRpcQueue` | sceSifSetRpcQueue | 00331EC0 | **002FFA58** | exact | 1.00 | 2/0/0/0 | yes | ok |  |
| `sceSifGetOtherData` | sceSifGetOtherData | 00331800 | **002FF370** | fuzzy 0.94 | 0.94 | 6/0/0/0 | no | ok+seed | runner-up 0.75 |
| `sceSifResetIop` | sceSifResetIop | 00336D78 | **00304E30** | callgraph 0.98 | 0.98 | 6/0/0/0 | yes | ok | via 1 call site(s)/reference(s) |
| `sceSifSyncIop` | sceSifSyncIop | 00336EF8 | **00304FB0** | exact | 1.00 | 3/0/0/0 | yes | ok |  |
| `sceSifIsAliveIop` | sceSifIsAliveIop | 00336ED0 | **00304F88** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `sceSifRebootIop` | sceSifRebootIop | 00336F48 | **00305000** | exact | 1.00 | 4/0/0/0 | yes | ok |  |
| `sceSifInitCmd` | sceSifInitCmd | 00339970 | **003075F0** | near | 0.97 | 11/0/0/0 | yes | ok | 5/160 words differ |
| `sceSifExitCmd` | sceSifExitCmd | 00339BF0 | **00307870** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceSifInitIopHeap` | sceSifInitIopHeap | 00335AC8 | **00303B80** | exact | 1.00 | 1/0/0/0 | yes | ok | RPC sid 0x80000003 ok |
| `sceSifAllocIopHeap` | sceSifAllocIopHeap | 00335B50 | **00303C08** | exact | 1.00 | 1/0/0/0 | yes | ok | RPC fno 0x1 ok |
| `sceSifFreeIopHeap` | sceSifFreeIopHeap | 00335CB8 | **00303D70** | exact | 1.00 | 1/0/0/0 | yes | ok | 14 identical copies in Zero, picked by layout |
| `sceSifAllocSysMemory` | sceSifAllocSysMemory | 00335BC0 | **00303C78** | exact | 1.00 | 1/0/0/0 | no | ok+seed | RPC fno 0x4 ok |
| `sceSifFreeSysMemory` | sceSifFreeSysMemory | 00335C40 | **00303CF8** | exact | 1.00 | 1/0/0/0 | yes | ok | RPC fno 0x2 ok |
| `sceSifQueryMemSize` | sceSifQueryMemSize | 00335DC8 | **00303E80** | exact | 1.00 | 1/0/0/0 | no | ok+seed | RPC fno 0x5 ok |
| `sceSifQueryMaxFreeMemSize` | sceSifQueryMaxFreeMemSize | 00335E38 | **00303EF0** | exact | 1.00 | 1/0/0/0 | no | ok+seed | RPC fno 0x6 ok |
| `sceSifQueryTotalFreeMemSize` | sceSifQueryTotalFreeMemSize | 00335EA8 | **00303F60** | exact | 1.00 | 1/0/0/0 | no | ok+seed | RPC fno 0x7 ok |
| `sceSifLoadFileReset` | sceSifLoadFileReset | 003361A8 | **00304260** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `sceSifLoadModule` | sceSifLoadModule | 00336A18 | **00304AD0** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceSifLoadStartModule` | sceSifLoadStartModule | 00336A38 | **00304AF0** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `sceSifLoadModuleBuffer` | sceSifLoadModuleBuffer | 003367B0 | **00304868** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `sceSifLoadStartModuleBuffer` | sceSifLoadStartModuleBuffer | 003367D0 | **00304888** | exact | 1.00 | 1/0/0/0 | no | ok+seed | 14 identical copies in Zero, picked by layout |
| `sceSifLoadElf` | sceSifLoadElf | 00336B80 | **00304C38** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `sceSifLoadElfPart` | sceSifLoadElfPart | 00336B60 | **00304C18** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `sceSifStopModule` | sceSifStopModule | 003363E8 | **003044A0** | exact | 1.00 | 3/0/0/0 | no | ok+seed | RPC fno 0x7 ok |
| `sceSifUnloadModule` | sceSifUnloadModule | 003365F0 | **003046A8** | exact | 1.00 | 3/0/0/0 | no | ok+seed | RPC fno 0x8 ok |
| `sceSifSearchModuleByName` | sceSifSearchModuleByName | 00336680 | **00304738** | near | 0.97 | 3/0/0/0 | yes | ok | 1/40 words differ; RPC fno 0x9 ok |
| `sceSifSearchModuleByAddress` | sceSifSearchModuleByAddress | 00336720 | **003047D8** | exact | 1.00 | 3/0/0/0 | no | ok+seed | RPC fno 0xA ok |
| `sceSifWriteBackDCache` | sceSifWriteBackDCache | 0033A020 | **00307CA0** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `sceDeci2Poll` | sceDeci2Poll | 00339760 | **003073E0** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceDeci2Open` | sceDeci2Open | 003396C0 | **00307340** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceDeci2Close` | sceDeci2Close | 00339708 | **00307388** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `sceDeci2ReqSend` | sceDeci2ReqSend | 00339730 | **003073B0** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceDeci2ExRecv` | sceDeci2ExRecv | 003397E0 | **00307460** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `sceDeci2ExSend` | sceDeci2ExSend | 00339818 | **00307498** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `write` | write | 0032FF68 | **002FC8A8** | exact | 1.00 | 2/0/0/0 | yes | ok |  |
| `kputs` | kputs | 003398D0 | **00307550** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `0x0033E920` | sceCdInit | 0033E920 | **0030C8C0** | fuzzy 0.85 | 0.85 | 9/0/1/0 | yes | ok | AC5 uses a hex key; for Zero use `"0x0030C8C0"`; runner-up 0.32; RPC sid 0x80000592 ok |
| `sceCdInitEeCB` | sceCdInitEeCB | 0033DC80 | **0030BC68** | layout 0.33 | 0.33 | 3/2/1/0 | no | ok+seed | reaches syscall 34, syscall 48 through a new wrapper; the only function between its neighbours in both games; SDK 3.0 body dropped the semaphore setup; it still creates and starts the callback thread on _sceCd_cbLoop |
| `sceCdCallback` | sceCdCallback | 0033DAA8 | **0030BA90** | exact | 1.00 | 3/0/0/0 | no | ok+seed |  |
| `sceCdPOffCallback` | sceCdPOffCallback | 0033DF60 | **0030BF48** | exact | 1.00 | 3/0/0/0 | no | ok+seed |  |
| `0x0033E3C8` | sceCdSearchFile | 0033E3C8 | **0030C398** | exact | 1.00 | 1/0/0/0 | no | ok+seed | AC5 uses a hex key; for Zero use `"0x0030C398"` |
| `sceCdLayerSearchFile` | sceCdLayerSearchFile | 0033E0B0 | **0030C098** | callgraph 0.97 | 0.97 | 9/0/1/0 | yes | ok | via 1 call site(s)/reference(s); RPC sid 0x80000597 ok |
| `sceCdRead` | sceCdRead | 0033F000 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `sceCdReadIOPm` | sceCdReadIOPm | 0033F1E0 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `sceCdSync` | sceCdSync | 0033E5F0 | **0030C5A8** | exact | 1.00 | 3/0/0/0 | yes | ok |  |
| `sceCdSyncS` | sceCdSyncS | 0033E690 | **0030C648** | exact | 1.00 | 3/0/0/0 | yes | ok |  |
| `sceCdGetError` | sceCdGetError | 0033F730 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `sceCdStatus` | sceCdStatus | 0033F7C8 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `0x0033EDF8` | sceCdDiskReady | 0033EDF8 | **0030CDC0** | fuzzy 0.97 | 0.97 | 9/0/0/0 | no | ok+seed | AC5 uses a hex key; for Zero use `"0x0030CDC0"`; runner-up 0.65; RPC sid 0x8000059C ok |
| `sceCdNcmdDiskReady` | sceCdNcmdDiskReady | 0033E558 | **0030C510** | near | 0.95 | 3/0/0/0 | no | ok+seed | 2/38 words differ; RPC fno 0xE ok |
| `sceCdMmode` | sceCdMmode | 0033F938 | **0030CFE0** | near | 0.98 | 4/0/0/0 | yes | ok | 1/52 words differ |
| `sceCdSeek` | sceCdSeek | 0033F5C8 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `sceCdBreak` | sceCdBreak | 0033F880 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `sceCdGetToc` | sceCdGetToc | 0033F338 | **not in Zero** | | | | | absent | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| `scePad2Init` | scePad2Init | 0033B880 | **00309830** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `scePad2End` | scePad2End | 0033B8C0 | **00309870** | exact | 1.00 | 1/0/0/0 | no | ok+seed |  |
| `scePad2CreateSocket` | scePad2CreateSocket | 0033B928 | **003098D8** | exact | 1.00 | 2/0/0/0 | yes | ok |  |
| `scePad2DeleteSocket` | scePad2DeleteSocket | 0033BA30 | **003099E0** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `sceVibGetProfile` | sceVibGetProfile | 0033C138 | **0030A0E8** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `scePad2Read` | scePad2Read | 0033BA88 | **00309A38** | near | 0.92 | 3/0/0/0 | yes | ok | 4/53 words differ |
| `scePad2GetState` | scePad2GetState | 0033BC30 | **00309BE0** | exact | 1.00 | 4/0/0/0 | yes | ok |  |
| `scePad2GetButtonProfile` | scePad2GetButtonProfile | 0033BB60 | **00309B10** | fuzzy 0.95 | 0.95 | 4/0/0/0 | yes | ok | runner-up 0.63 |
| `scePad2InitDmaDBuff` | scePad2InitDmaDBuff | 0033BE50 | **00309E00** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `scePad2LinkDriver` | scePad2LinkDriver | 0033BEC8 | **00309E78** | exact | 1.00 | 0/0/0/0 | yes | ok |  |
| `scePad2GetSide` | scePad2GetSide | 0033BF20 | **00309ED0** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `scePad2GetSide2` | scePad2GetSide2 | 0033BF88 | **00309F38** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `scePad2CheckDma` | scePad2CheckDma | 0033BFF0 | **00309FA0** | exact | 1.00 | 1/0/0/0 | yes | ok |  |
| `scePad2SetButtonOrder` | scePad2SetButtonOrder | 0033C050 | **0030A000** | exact | 1.00 | 0/0/0/0 | yes | ok |  |

Of the 73 keys, **66 have a counterpart in Zero** and **7 do not exist in Zero**. `overrides_draft.json` is a Zero version of the list that ps2recomp can read. It has 66 active entries: 44 whose function Zero's config-less partition already recovers, and 22 (`ok+seed`) that are recovered only when `sdk_seeds.json` is passed. Also, 0 entries are commented out as weak, 0 as `needs-seed`, and 7 because the function is absent.

`ok+seed`: nothing in Zero calls the function directly (it may be unused, or reached only through a pointer). Zero's config-less partition therefore has no function at that address, and without a seed ps2recomp would stop with "not a recovered function". `sdk_seeds.json` (142 addresses, in the `--ida-seeds` format) adds every well-verified SDK function that has no recovered entry. If Zero later gets its own IDA seeds, merge the two `xref` lists, because ps2recomp accepts only one `--ida-seeds` file. The draft was checked with:

```
PYTHONPATH=tools python -m ps2recomp game/SLUS_213.46 -o <outdir> \
    --symbols config_zero/sdk/sdk_symbols.json \
    --symbols config_zero/sdk/manual_symbols.json \
    --ida-seeds config_zero/sdk/sdk_seeds.json \
    --overrides config_zero/sdk/overrides_draft.json
```

The libcdvd functions marked absent are not in Zero's executable. In SDK 3.0 they live in separate objects and are linked only when called. Zero calls only `sceCdInit`, `sceCdMmode` and `sceCdReadClock` (0x0030D0D8, a function AC5 does not have). This was confirmed independently: in all of Zero's .text, the only sceSifCallRpc callers on the libcdvd RPC clients are sceCdLayerSearchFile, sceCdNcmdDiskReady, sceCdInit, sceCdDiskReady(_old), sceCdMmode and sceCdReadClock. `_sceCd_ncmd_prechk` has one caller (sceCdNcmdDiskReady), and `_sceCd_scmd_prechk` has two (sceCdMmode and sceCdReadClock). Zero evidently reads its data some other way, for example through IOP-side file I/O.

## Overall

- AC5 SDK labels: 458. 35 are excluded as AC5 false positives (see below) and 5 are matched but not transferred because AC5's label is wrong (see below). That leaves 418 candidates.
- Located in Zero and written to `sdk_symbols.json`: **374 (89.5%)**. That is 374 distinct addresses; 10 of them are also in `manual_symbols.json`.
  - exact: 270
  - near: 20
  - fuzzy: 23
  - callgraph: 17
  - vu0sig: 29
  - layout: 15
  - tagged `(aligned)`: 59. Of these, 59 carry AC5's own tag and 0 were added because the cross-match is weak (-).
- Not located: 44 (listed in the table below).
- RPC cross-check against `config/rpc_sids.json` (server ids bound and RPC function numbers called by AC5's SDK functions): 10 server ids and 39 function numbers found in the Zero counterparts, 0 missing.
- Evidence over all matches: 705 SDK references agree, 6 go through a new wrapper, 71 are no longer used, and 0 conflict.
- 146 matches have no function in Zero's config-less partition. The matcher finds them by adding 3828 orphan starts (code nothing calls directly) to its own copy of the partition.

| library | found / in AC5 | note |
|---|---|---|
| libkernl.a | 208 / 216 | printf family rewritten in 3.0 |
| libmpeg.a | 16 / 45 | most AC5 names are (aligned); libmpeg changed a lot in 3.0 |
| libvu0.a | 39 / 39 | AC5 names (aligned); matched by VU0 instruction sequence |
| libcdvd.a | 21 / 28 | 7 functions not linked in Zero (see above) |
| libmc.a | 26 / 26 |  |
| libdma.a | 16 / 16 |  |
| libpad2.a | 13 / 13 |  |
| libgraph.a | 11 / 11 |  |
| sifrpc.o (manual) | 10 / 10 |  |
| libipu.a | 6 / 6 |  |
| libmrpc.a | 2 / 2 |  |
| libdbc.a | 2 / 2 |  |
| libvib.a | 2 / 2 |  |
| libtimer.a | 1 / 1 |  |
| libmtap.a | 1 / 1 |  |

## AC5 labels that look wrong (matched, not transferred)

- AC5 0032F4F8 `sceVu0ClipScreen3`: AC5 calls this 'sceVu0ClipScreen3' (aligned), but the body resets VIF0/VU0 (it writes VIF0 MARK/ERR/FBRST, sets FBRST through ctc2 vi28 and pushes quadwords into the VIF0 FIFO); it is probably the real sceVpu0Reset. Zero counterpart: not found.
- AC5 0032F5F0 `sceVu0ClipAll`: AC5 calls this 'sceVu0ClipAll' (aligned), but the body is the syscall 7 (_ExecPS2) stub. Zero counterpart: 002FBF30 (exact).
- AC5 0032F650 `sceVpu0Reset`: AC5 calls this 'sceVpu0Reset' (aligned), but the body is the syscall 0xD (SetVTLBRefillHandler) stub. Zero counterpart: 002FBF90 (exact).
- AC5 0032F9C0 `scePrintf`: AC5 calls this 'scePrintf', but the body is the syscall 0x42 (SignalSema) stub; libcdvd calls it to release its semaphores. Zero counterpart: 002FC300 (exact).
- AC5 003312C0 `sceSifMInitRpc`: AC5 calls this 'sceSifMInitRpc' (aligned), but the body is a varargs printf: it formats through _sceSnprintf with deci2Putchar as the sink. Zero counterpart: 002FED90 (callgraph 0.07).
- More generally, about 37 of AC5's libmpeg names, almost all of its libvu0 names, and its printf-family names are `(aligned)`. They were assigned by object order, never verified, and the libvu0 tail is visibly shifted (see above). They are transferred with the tag, but the names themselves may be wrong.

## AC5 false positives (not transferred)

AC5's 35 `netcnfif.a` / `libhttps.a` labels sit on 2-5 instruction C++ accessors in game code, and AC5 has no netcnf or SSL strings at all. They are leftovers of the original signature run, so they are not transferred. Labels: MEM_STD_free, MEM_STD_get_error, MEM_STD_malloc, MEM_STD_realloc, SSL_CTX_add_client_CA, WTLS_IDENTIFIER_free_entry, add_client_CA, sceNetcnfifAddEntry, sceNetcnfifAllocWorkarea, sceNetcnfifCheck, sceNetcnfifCheckAdditionalAT, sceNetcnfifCheckCapacity, sceNetcnfifCheckSpecialProvider, sceNetcnfifCreateCallbackThread, sceNetcnfifDeleteAll, sceNetcnfifDeleteCallbackThread, sceNetcnfifDeleteEntry, sceNetcnfifDmaCheck, sceNetcnfifEditEntry, sceNetcnfifFreeWorkarea, sceNetcnfifGetAddr, sceNetcnfifGetCount, sceNetcnfifGetList, sceNetcnfifGetResult, sceNetcnfifInit, sceNetcnfifLoadEntry, sceNetcnfifLoadEntryAuto, sceNetcnfifSendIOP, sceNetcnfifSetEnv, sceNetcnfifSetFNoDecode, sceNetcnfifSetLatestEntry, sceNetcnfifSetup, sceNetcnfifSync, sceNetcnfifTerm.

## Things to look at

- sceCdInitEeCB -> Zero 0030BC68: layout 0.33 (low body similarity); evidence ok/wrap/miss/bad 3/2/1/0
- sceFsInit (Zero 00300758) now reaches sceSifCallRpc through a new SDK 3.0 helper (fileio now wraps sceSifCallRpc in a retry loop at 0x003000C0)
- sceFsSetIopBuf (Zero 00300C58) now reaches sceSifCallRpc through a new SDK 3.0 helper (fileio now wraps sceSifCallRpc in a retry loop at 0x003000C0)
- sceFormat (Zero 00302368) now reaches sceSifCallRpc through a new SDK 3.0 helper (fileio now wraps sceSifCallRpc in a retry loop at 0x003000C0)
- InitThread (Zero 002FD0B8) now reaches syscall 34 through a new SDK 3.0 helper (StartThread is a wrapper function in SDK 3.0, not a bare syscall stub)
- sceCdInitEeCB (Zero 0030BC68) now reaches syscall 34, syscall 48 through a new SDK 3.0 helper (StartThread is a wrapper function in SDK 3.0, not a bare syscall stub)

## All AC5 SDK symbols

| AC5 addr | name | AC5 lib, class | words | Zero addr | match | ratio | ev ok/wrap/miss/bad | entry | notes |
|---|---|---|---|---|---|---|---|---|---|
| 0031CEE8 | _sceMpegAlalcReset | libmpeg.a, sig | 6 | 0030E500 | exact | 1.00 | 0/0/0/0 | no |  |
| 0032C378 | sceGsResetGraph | libgraph.a, sig | 3214 | 002F9690 | fuzzy 0.82 | 0.82 | 30/0/0/0 | yes | runner-up 0.07 |
| 0032C508 | sceGsGetGParam | libgraph.a, aligned | 3 | 002F9820 | exact | 1.00 | 0/0/0/0 | yes | 55 identical copies in Zero, picked by layout |
| 0032C518 | sceGsResetPath | libgraph.a, sig | 26 | 002F9830 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032C580 | checkModelVersion | libgraph.a, sig | 40 | 002F9898 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0032C620 | _GetGsDxDyOffset | libgraph.a, anchored | 4 | 002F9938 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032C630 | sceGsSetDefDispEnv | libgraph.a, sig | 221 | 002F9948 | near | 1.00 | 4/0/0/0 | yes | 1/221 words differ |
| 0032C9A8 | sceGsPutDispEnv | libgraph.a, sig | 47 | 002F9CC0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0032CA68 | sceGsSyncV | libgraph.a, sig | 37 | 002F9D80 | exact | 1.00 | 3/0/0/0 | yes |  |
| 0032CB00 | sceGsSyncPath | libgraph.a, sig | 197 | 002F9E18 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0032CE18 | sceGsSetDefStoreImage | libgraph.a, sig | 80 | 002FA130 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032CF58 | sceGsExecStoreImage | libgraph.a, sig | 39 | 002FA270 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032D5E8 | WaitDma | libdma.a, sig | 28 | 002FA900 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032D658 | WatchDma | libdma.a, sig | 33 | 002FA970 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032D6E0 | CheckAddress | libdma.a, sig | 10 | 002FA9F8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032D708 | memclr | libdma.a, sig | 13 | 002FAA20 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032D740 | sceDmaGetChan | libdma.a, sig | 10 | 002FAA58 | exact | 1.00 | 0/0/0/0 | no |  |
| 0032D768 | sceDmaReset | libdma.a, sig | 55 | 002FAA80 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032D848 | sceDmaPutEnv | libdma.a, sig | 118 | 002FAB60 | near | 0.99 | 0/0/0/0 | yes | 1/118 words differ |
| 0032DA70 | sceDmaSend | libdma.a, sig | 26 | 002FAD88 | exact | 1.00 | 2/0/0/0 | no |  |
| 0032DAD8 | sceDmaSendN | libdma.a, sig | 29 | 002FADF0 | exact | 1.00 | 2/0/0/0 | no |  |
| 0032DB50 | sceDmaSendI | libdma.a, sig | 29 | 002FAE68 | exact | 1.00 | 2/0/0/0 | no |  |
| 0032DBC8 | sceDmaRecv | libdma.a, sig | 18 | 002FAEE0 | exact | 1.00 | 1/0/0/0 | no |  |
| 0032DC10 | sceDmaRecvN | libdma.a, sig | 31 | 002FAF28 | exact | 1.00 | 2/0/0/0 | no |  |
| 0032DC90 | sceDmaRecvI | libdma.a, sig | 32 | 002FAFA8 | exact | 1.00 | 2/0/0/0 | no |  |
| 0032DD10 | sceDmaSync | libdma.a, sig | 14 | 002FB028 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0032DD48 | sceDmaWatch | libdma.a, sig | 13 | 002FB060 | exact | 1.00 | 1/0/0/0 | no |  |
| 0032DD80 | sceDmaPause | libdma.a, sig | 38 | 002FB098 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032DE30 | sceVu0ApplyMatrix | libvu0.a, aligned | 31 | 002FB148 | vu0sig 1.00 | 0.24 | 0/0/2/0 | yes | COP2 sequence, inside neighbour window |
| 0032DEB0 | sceVu0MulMatrix | libvu0.a, aligned | 36 | 002FB178 | vu0sig 1.00 | 0.30 | 0/0/2/0 | yes | via 1 call site(s)/reference(s) |
| 0032DF40 | sceVu0OuterProduct | libvu0.a, aligned | 27 | 002FB1C0 | vu0sig 1.00 | 0.24 | 0/0/2/0 | yes | via 2 call site(s)/reference(s) |
| 0032DFB0 | sceVu0InnerProduct | libvu0.a, aligned | 27 | 002FB1E0 | vu0sig 1.00 | 0.28 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E020 | sceVu0Normalize | libvu0.a, aligned | 31 | 002FB208 | vu0sig 1.00 | 0.57 | 0/0/2/0 | yes | via 5 call site(s)/reference(s) |
| 0032E0A0 | sceVu0TransposeMatrix | libvu0.a, aligned | 33 | 002FB248 | callgraph 0.36 | 0.36 | 0/0/2/0 | yes | via 2 call site(s)/reference(s) |
| 0032E128 | sceVu0InversMatrix | libvu0.a, aligned | 43 | 002FB290 | vu0sig 1.00 | 0.54 | 0/0/2/0 | yes | via 1 call site(s)/reference(s) |
| 0032E1D8 | sceVu0DivVector | libvu0.a, aligned | 28 | 002FB300 | vu0sig 1.00 | 0.28 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E248 | sceVu0DivVectorXYZ | libvu0.a, aligned | 28 | 002FB320 | vu0sig 1.00 | 0.28 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E2B8 | sceVu0InterVector | libvu0.a, aligned | 34 | 002FB340 | vu0sig 1.00 | 0.27 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E340 | sceVu0AddVector | libvu0.a, aligned | 25 | 002FB368 | vu0sig 1.00 | 0.13 | 0/0/2/0 | yes | COP2 sequence, inside neighbour window |
| 0032E3A8 | sceVu0SubVector | libvu0.a, aligned | 25 | 002FB380 | vu0sig 1.00 | 0.13 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E410 | sceVu0MulVector | libvu0.a, aligned | 25 | 002FB398 | vu0sig 1.00 | 0.13 | 0/0/2/0 | yes | COP2 sequence, inside neighbour window |
| 0032E478 | sceVu0ScaleVector | libvu0.a, aligned | 26 | 002FB3B0 | vu0sig 1.00 | 0.19 | 0/0/2/0 | yes | via 3 call site(s)/reference(s) |
| 0032E4E0 | sceVu0TransMatrix | libvu0.a, aligned | 31 | 002FB3C8 | vu0sig 1.00 | 0.10 | 0/0/2/0 | yes | via 1 call site(s)/reference(s) |
| 0032E560 | sceVu0CopyVector | libvu0.a, aligned | 19 | 002FB3F8 | callgraph 0.09 | 0.09 | 0/0/2/0 | yes | via 4 call site(s)/reference(s) |
| 0032E5B0 | sceVu0CopyMatrix | libvu0.a, aligned | 25 | 002FB408 | layout 0.06 | 0.06 | 0/0/2/0 | no | the only function between its neighbours in both games |
| 0032E618 | sceVu0FTOI4Vector | libvu0.a, aligned | 20 | 002FB430 | vu0sig 1.00 | 0.17 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E668 | sceVu0Normalize | libvu0.a, anchored | 20 | 002FB440 | vu0sig 1.00 | 0.17 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E6B8 | sceVu0ITOF4Vector | libvu0.a, aligned | 20 | 002FB450 | vu0sig 1.00 | 0.17 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E708 | sceVu0ITOF0Vector | libvu0.a, aligned | 20 | 002FB460 | vu0sig 1.00 | 0.17 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032E758 | sceVu0UnitMatrix | libvu0.a, aligned | 22 | 002FB470 | vu0sig 1.00 | 0.38 | 0/0/2/0 | yes | via 3 call site(s)/reference(s) |
| 0032E7B0 | _sceVu0ecossin | libvu0.a, sig | 29 | 002FB498 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0032E828 | sceVu0TransMatrix | libvu0.a, anchored | 60 | 002FB510 | vu0sig 1.00 | 0.63 | 1/0/2/0 | yes | via 1 call site(s)/reference(s) |
| 0032E918 | sceVu0RotMatrixX | libvu0.a, aligned | 61 | 002FB5B8 | vu0sig 1.00 | 0.64 | 1/0/2/0 | yes | via 1 call site(s)/reference(s) |
| 0032EA10 | sceVu0RotMatrixY | libvu0.a, aligned | 61 | 002FB660 | vu0sig 1.00 | 0.64 | 1/0/2/0 | yes | via 1 call site(s)/reference(s) |
| 0032EB08 | sceVu0RotMatrix | libvu0.a, sig | 20 | 002FB708 | exact | 1.00 | 3/0/0/0 | no |  |
| 0032EB58 | sceVu0ClampVector | libvu0.a, aligned | 33 | 002FB758 | vu0sig 1.00 | 0.24 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032EBE0 | sceVu0CameraMatrix | libvu0.a, sig | 44 | 002FB780 | exact | 1.00 | 5/0/0/0 | no |  |
| 0032EC90 | sceVu0NormalLightMatrix | libvu0.a, sig | 48 | 002FB830 | exact | 1.00 | 3/0/0/0 | no |  |
| 0032ED50 | sceVu0LightColorMatrix | libvu0.a, sig | 26 | 002FB8F0 | exact | 1.00 | 1/0/0/0 | no |  |
| 0032EDB8 | sceVu0ViewScreenMatrix | libvu0.a, sig | 65 | 002FB958 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032F050 | sceVu0RotTransPersN | libvu0.a, anchored | 49 | 002FBC48 | vu0sig 1.00 | 0.30 | 0/0/2/0 | no | found between neighbours |
| 0032F118 | sceVu0RotTransPersN | libvu0.a, aligned | 41 | 002FBC90 | layout 0.04 | 0.04 | 0/0/2/0 | no | the only function between its neighbours in both games |
| 0032F1E0 | sceVu0RotTransPers | libvu0.a, aligned | 35 | 002FBCB0 | vu0sig 1.00 | 0.30 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032F270 | sceVu0CopyVectorXYZ | libvu0.a, aligned | 26 | 002FBCE0 | vu0sig 1.00 | 0.19 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032F2D8 | sceVu0InterVectorXYZ | libvu0.a, aligned | 31 | 002FBCF8 | vu0sig 1.00 | 0.41 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032F358 | sceVu0ScaleVectorXYZ | libvu0.a, aligned | 43 | 002FBD40 | vu0sig 1.00 | 0.42 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032F408 | sceVu0ClipScreen | libvu0.a, aligned | 59 | 002FBDA0 | vu0sig 1.00 | 0.36 | 0/0/2/0 | no | COP2 sequence, inside neighbour window |
| 0032F4F8 | sceVu0ClipScreen3 | libvu0.a, aligned | 32 | - | not found | | | | layout: best start between neighbours Zero 002FBF20 ratio 0.06 (runner-up 0.06) |
| 0032F5F0 | sceVu0ClipAll | libvu0.a, aligned | 4 | 002FBF30 | exact | 1.00 | 0/0/0/0 | yes | NOT TRANSFERRED: AC5 label looks wrong |
| 0032F650 | sceVpu0Reset | libvu0.a, aligned | 4 | 002FBF90 | exact | 1.00 | 0/0/0/0 | yes | NOT TRANSFERRED: AC5 label looks wrong |
| 0032F9C0 | scePrintf | libkernl.a, verified | 4 | 002FC300 | exact | 1.00 | 0/0/0/0 | yes | NOT TRANSFERRED: AC5 label looks wrong |
| 0032FE20 | sceResetttyinit | libkernl.a, anchored | 3 | 002FC760 | exact | 1.00 | 0/0/0/0 | yes | short body, confirmed by neighbour displacement |
| 0032FE30 | VSync | libkernl.a, sig | 36 | 002FC770 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032FEC0 | VSync2 | libkernl.a, sig | 41 | 002FC800 | exact | 1.00 | 3/0/0/0 | yes |  |
| 0032FF68 | write | libkernl.a, sig | 31 | 002FC8A8 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0032FFE8 | read | libkernl.a, sig | 29 | 002FC928 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00330060 | open | libkernl.a, anchored | 10 | 002FC9A0 | exact | 1.00 | 0/0/0/0 | yes | 3 identical copies in Zero, picked by layout |
| 00330088 | close | libkernl.a, anchored | 2 | 002FC9C8 | exact | 1.00 | 0/0/0/0 | yes | 24 identical copies in Zero, picked by layout |
| 00330090 | ioctl | libkernl.a, anchored | 2 | 002FC9D0 | exact | 1.00 | 0/0/0/0 | no | 24 identical copies in Zero, picked by layout |
| 00330098 | lseek | libkernl.a, anchored | 2 | 002FC9D8 | exact | 1.00 | 0/0/0/0 | yes | 24 identical copies in Zero, picked by layout |
| 003300A0 | sbrk | libkernl.a, sig | 43 | 002FC9E0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00330150 | isatty | libkernl.a, anchored | 2 | 002FCA90 | exact | 1.00 | 0/0/0/0 | yes | 80 identical copies in Zero, picked by layout |
| 00330158 | fstat | libkernl.a, anchored | 5 | 002FCA98 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00330178 | kill | libkernl.a, sig | 10 | 002FCAB8 | exact | 1.00 | 1/0/0/0 | yes |  |
| 003301A0 | stat | libkernl.a, anchored | 10 | 002FCAE0 | exact | 1.00 | 0/0/0/0 | no | 3 identical copies in Zero, picked by layout |
| 003301C8 | unlink | libkernl.a, anchored | 10 | 002FCB08 | exact | 1.00 | 0/0/0/0 | no | 3 identical copies in Zero, picked by layout |
| 00330420 | QueueInit | libkernl.a, sig | 9 | 002FD5C0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00330448 | QueuePeekWriteDone | libkernl.a, sig | 15 | 002FD5E8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00330488 | QueuePeekReadDone | libkernl.a, sig | 15 | 002FD628 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003304C8 | sceTtyHandler | libkernl.a, sig | 101 | 002FD668 | exact | 1.00 | 3/0/0/0 | no |  |
| 00330660 | sceTtyWrite | libkernl.a, sig | 93 | 002FD800 | exact | 1.00 | 4/0/0/0 | yes |  |
| 003307D8 | sceTtyRead | libkernl.a, sig | 52 | 002FD978 | exact | 1.00 | 1/0/0/0 | yes |  |
| 003308A8 | sceTtyInit | libkernl.a, sig | 47 | 002FDA48 | exact | 1.00 | 4/0/0/0 | yes |  |
| 00330968 | kputchar | libkernl.a, aligned | 15 | 002FDB08 | callgraph 0.90 | 0.90 | 0/0/0/0 | yes | via 3 call site(s)/reference(s) |
| 003309A8 | deci2Putchar | libkernl.a, sig | 44 | 002FDB40 | near | 0.98 | 1/0/0/0 | yes | 1/44 words differ |
| 00330A58 | serialPutchar | libkernl.a, sig | 13 | 002FDBF0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00330A90 | ftoi | libkernl.a, anchored | 37 | 002FDC28 | near | 0.96 | 0/0/0/0 | yes | 3/37 words differ |
| 00330B28 | printfloat | libkernl.a, aligned | 90 | 002FDCB8 | layout 0.54 | 0.54 | 1/0/0/0 | yes | best of 34 starts between neighbours, clear margin |
| 00330C90 | _sceSnprintf | libkernl.a, aligned | 16 | - | not found | | | | layout: best start between neighbours Zero 002FEC28 ratio 0.08 (runner-up 0.08); layout: best start between neighbours Zero 002FDE90 ratio 0.05 (runner-up 0.00) |
| 00330CD0 | snprintf_char | libkernl.a, aligned | 6 | - | not found | | | | layout: best start between neighbours Zero 002FDEF0 ratio 0.14 (runner-up 0.10); layout: best start between neighbours Zero 002FDEF0 ratio 0.14 (runner-up 0.00) |
| 00330CE8 | printf_char | libkernl.a, aligned | 1 | - | not found | | | | layout: best start between neighbours Zero 002FED20 ratio 0.00 (runner-up 0.00); layout: best start between neighbours Zero 002FDEF0 ratio 0.00 (runner-up 0.00) |
| 00330CEC | kprintf_char | libkernl.a, aligned | 1 | - | not found | | | | layout: best start between neighbours Zero 002FED20 ratio 0.00 (runner-up 0.00); layout: best start between neighbours Zero 002FDEF0 ratio 0.00 (runner-up 0.00) |
| 00330CF0 | _sceSnprintf_pf | libkernl.a, aligned | 13 | 002FDF10 | layout 0.71 | 0.71 | 0/0/0/0 | no | best of 33 starts between neighbours |
| 00330D24 | sceVsnprintf | libkernl.a, aligned | 33 | - | not found | | | | layout: best start between neighbours Zero 002FE054 ratio 0.13 (runner-up 0.12) |
| 00330DA8 | sceSnprintf | libkernl.a, aligned | 1 | - | not found | | | | layout: best start between neighbours Zero 002FED20 ratio 0.00 (runner-up 0.00) |
| 00330DAC | sceVprintf | libkernl.a, aligned | 2 | - | not found | | | | layout: best start between neighbours Zero 002FE04C ratio 0.50 (runner-up 0.50) |
| 00330DBC | kprintf | libkernl.a, aligned | 47 | - | not found | | | | layout: best start between neighbours Zero 002FE054 ratio 0.13 (runner-up 0.07) |
| 003312C0 | sceSifMInitRpc | libmrpc.a, aligned | 24 | 002FED90 | callgraph 0.07 | 0.07 | 0/1/0/0 | yes | NOT TRANSFERRED: AC5 label looks wrong; via 20 call site(s)/reference(s) |
| 00331320 | sceSifInitRpc | libkernl.a, sig | 103 | 002FEE90 | exact | 1.00 | 12/0/0/0 | yes |  |
| 003314C0 | sceSifExitRpc | libkernl.a, sig | 9 | 002FF030 | exact | 1.00 | 1/0/0/0 | yes |  |
| 003314E8 | _sceRpcGetPacket | libmrpc.a, sig | 42 | 002FF058 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00331590 | _sceRpcFreePacket | libmrpc.a, sig | 7 | 002FF100 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003315B0 | _sceRpcGetFPacket | libkernl.a, sig | 12 | 002FF120 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003315E0 | _sceRpcGetFPacket2 | libkernl.a, sig | 16 | 002FF150 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00331620 | _request_end | sifrpc.o+0x300, manual | 51 | 002FF190 | exact | 1.00 | 2/0/0/0 | no |  |
| 003316F0 | _alarm_rdata | sifrpc.o+0x3D0, manual | 16 | 002FF260 | exact | 1.00 | 1/0/0/0 | no |  |
| 00331730 | _request_rdata | sifrpc.o+0x410, manual | 52 | 002FF2A0 | exact | 1.00 | 5/0/0/0 | no |  |
| 00331800 | sceSifGetOtherData | sifrpc.o+0x4E0, manual | 86 | 002FF370 | fuzzy 0.94 | 0.94 | 6/0/0/0 | no | runner-up 0.75 |
| 00331958 | _search_svdata | sifrpc.o+0x648, manual | 19 | 002FF4D8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003319A8 | _alarm_bind | sifrpc.o+0x698, manual | 16 | 002FF528 | exact | 1.00 | 1/0/0/0 | no |  |
| 003319E8 | _request_bind | sifrpc.o+0x6D8, manual | 51 | 002FF568 | exact | 1.00 | 5/0/0/0 | no |  |
| 00331AB8 | sceSifBindRpc | sifrpc.o+0x7A8, manual | 80 | 002FF638 | callgraph 0.93 | 0.93 | 6/0/0/0 | yes | via 2 call site(s)/reference(s) |
| 00331BF8 | _request_call | sifrpc.o+0x8F8, manual | 35 | 002FF788 | exact | 1.00 | 1/0/0/0 | no |  |
| 00331C88 | sceSifCallRpc | sifrpc.o+0x988, manual | 125 | 002FF818 | callgraph 0.96 | 0.96 | 7/0/0/0 | yes | via 22 call site(s)/reference(s) |
| 00331E80 | sceSifCheckStatRpc | libkernl.a, sig | 15 | 002FFA18 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00331EC0 | sceSifSetRpcQueue | libkernl.a, sig | 37 | 002FFA58 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00331F58 | sceSifRegisterRpc | libkernl.a, sig | 52 | 002FFAF0 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00332028 | sceSifRemoveRpc | libkernl.a, sig | 37 | 002FFBC0 | exact | 1.00 | 2/0/0/0 | yes |  |
| 003320C0 | sceSifRemoveRpcQueue | libkernl.a, sig | 36 | 002FFC58 | exact | 1.00 | 2/0/0/0 | no |  |
| 00332150 | sceSifGetNextRequest | libkernl.a, sig | 22 | 002FFCE8 | exact | 1.00 | 2/0/0/0 | yes |  |
| 003321A8 | sceSifExecRequest | libkernl.a, sig | 89 | 002FFD40 | exact | 0.88 | 6/0/0/0 | yes |  |
| 00332320 | new_iob | libkernl.a, anchored | 12 | 002FFEB8 | exact | 1.00 | 0/0/0/0 | no |  |
| 00332370 | sceSifRpcLoop | libkernl.a, sig | 16 | 002FFF08 | exact | 1.00 | 3/0/0/0 | no |  |
| 00332410 | new_iob | libkernl.a, sig | 34 | 00300038 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00332498 | get_iob | libkernl.a, sig | 27 | 003001B0 | near | 0.96 | 2/0/0/0 | yes | 1/27 words differ |
| 00332508 | _sceFs_Rcv_Intr | libkernl.a, sig | 50 | 00300220 | exact | 0.35 | 0/0/0/0 | no |  |
| 003328C0 | _sceFsSemInit | libkernl.a, anchored | 19 | 003005D8 | callgraph 0.75 | 0.75 | 1/0/0/0 | yes | via 1 call site(s)/reference(s) |
| 00332910 | _sceFsWaitS | libkernl.a, sig | 11 | 00300630 | exact | 1.00 | 2/0/0/0 | yes |  |
| 003329F8 | _sceFs_Poff_Intr | libkernl.a, sig | 17 | 00300710 | exact | 1.00 | 0/0/0/0 | no |  |
| 00332A40 | sceFsInit | libkernl.a, anchored | 129 | 00300758 | callgraph 0.99 | 0.99 | 9/1/0/0 | yes | via 1 call site(s)/reference(s) |
| 00332C48 | _fs_version | libkernl.a, anchored | 35 | 00300960 | exact | 1.00 | 0/0/0/0 | yes | 2 identical copies in Zero, picked by layout |
| 00332CD8 | sceFsReset | libkernl.a, sig | 13 | 003009F0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00332D10 | sceFsSetIopBuf | libkernl.a, anchored | 161 | 00300C58 | callgraph 0.89 | 0.89 | 8/1/0/0 | yes | via 1 call site(s)/reference(s) |
| 00333F70 | sceRemove | libkernl.a, sig | 7 | 00301EF0 | exact | 1.00 | 0/0/0/0 | no |  |
| 00334140 | sceRmdir | libkernl.a, sig | 7 | 003020C8 | exact | 1.00 | 0/0/0/0 | no |  |
| 003343D0 | sceFormat | libkernl.a, anchored | 72 | 00302368 | fuzzy 0.84 | 0.84 | 5/1/0/0 | no | runner-up 0.61 |
| 003344F0 | sceDelDrv | libkernl.a, sig | 7 | 00302498 | exact | 1.00 | 0/0/0/0 | no |  |
| 00334510 | sceDopen | libkernl.a, sig | 50 | 003024B8 | exact | 1.00 | 5/0/0/0 | yes |  |
| 00334E68 | sceChdir | libkernl.a, sig | 7 | 00302E50 | exact | 1.00 | 0/0/0/0 | no |  |
| 00335290 | sceUmount | libkernl.a, sig | 7 | 003032A0 | exact | 1.00 | 0/0/0/0 | no |  |
| 00335AC8 | sceSifInitIopHeap | libkernl.a, sig | 34 | 00303B80 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00335B50 | sceSifAllocIopHeap | libkernl.a, sig | 28 | 00303C08 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00335BC0 | sceSifAllocSysMemory | libkernl.a, sig | 32 | 00303C78 | exact | 1.00 | 1/0/0/0 | no |  |
| 00335C40 | sceSifFreeSysMemory | libkernl.a, sig | 30 | 00303CF8 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00335CB8 | sceSifFreeIopHeap | libkernl.a, anchored | 7 | 00303D70 | exact | 1.00 | 1/0/0/0 | yes | 14 identical copies in Zero, picked by layout |
| 00335CD8 | sceSifLoadIopHeap | libkernl.a, sig | 60 | 00303D90 | exact | 1.00 | 1/0/0/0 | no |  |
| 00335DC8 | sceSifQueryMemSize | libkernl.a, sig | 28 | 00303E80 | exact | 1.00 | 1/0/0/0 | no |  |
| 00335E38 | sceSifQueryMaxFreeMemSize | libkernl.a, sig | 28 | 00303EF0 | exact | 1.00 | 1/0/0/0 | no |  |
| 00335EA8 | sceSifQueryTotalFreeMemSize | libkernl.a, sig | 28 | 00303F60 | exact | 1.00 | 1/0/0/0 | no |  |
| 00335F18 | sceSifQueryBlockTopAddress | libkernl.a, sig | 31 | 00303FD0 | exact | 1.00 | 1/0/0/0 | no |  |
| 00335F98 | sceSifQueryBlockSize | libkernl.a, sig | 31 | 00304050 | exact | 1.00 | 1/0/0/0 | no |  |
| 00336018 | _lf_bind | libkernl.a, sig | 64 | 003040D0 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00336118 | _lf_version | libkernl.a, anchored | 35 | 003041D0 | exact | 1.00 | 0/0/0/0 | yes | 2 identical copies in Zero, picked by layout |
| 003361A8 | sceSifLoadFileReset | libkernl.a, sig | 14 | 00304260 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003361E0 | _sceSifLoadModuleBuffer | libkernl.a, sig | 130 | 00304298 | exact | 1.00 | 3/0/0/0 | yes |  |
| 003363E8 | sceSifStopModule | libkernl.a, sig | 130 | 003044A0 | exact | 1.00 | 3/0/0/0 | no |  |
| 003365F0 | sceSifUnloadModule | libkernl.a, sig | 36 | 003046A8 | exact | 1.00 | 3/0/0/0 | no |  |
| 00336680 | sceSifSearchModuleByName | libkernl.a, sig | 40 | 00304738 | near | 0.97 | 3/0/0/0 | yes | 1/40 words differ |
| 00336720 | sceSifSearchModuleByAddress | libkernl.a, sig | 36 | 003047D8 | exact | 1.00 | 3/0/0/0 | no |  |
| 003367B0 | sceSifLoadModuleBuffer | libkernl.a, sig | 7 | 00304868 | exact | 1.00 | 1/0/0/0 | no |  |
| 003367D0 | sceSifLoadStartModuleBuffer | libkernl.a, anchored | 7 | 00304888 | exact | 1.00 | 1/0/0/0 | no | 14 identical copies in Zero, picked by layout |
| 003367F0 | _sceSifLoadModule | libkernl.a, sig | 137 | 003048A8 | near | 0.99 | 3/0/0/0 | yes | 1/137 words differ |
| 00336A18 | sceSifLoadModule | libkernl.a, sig | 8 | 00304AD0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00336A38 | sceSifLoadStartModule | libkernl.a, anchored | 7 | 00304AF0 | exact | 1.00 | 1/0/0/0 | no |  |
| 00336A58 | _sceSifLoadElfPart | libkernl.a, sig | 65 | 00304B10 | near | 0.98 | 3/0/0/0 | yes | 1/65 words differ |
| 00336B60 | sceSifLoadElfPart | libkernl.a, anchored | 7 | 00304C18 | exact | 1.00 | 1/0/0/0 | no |  |
| 00336B80 | sceSifLoadElf | libkernl.a, sig | 10 | 00304C38 | exact | 1.00 | 1/0/0/0 | no |  |
| 00336BA8 | sceSifGetIopAddr | libkernl.a, sig | 59 | 00304C60 | exact | 1.00 | 2/0/0/0 | no |  |
| 00336C98 | sceSifSetIopAddr | libkernl.a, sig | 55 | 00304D50 | exact | 1.00 | 2/0/0/0 | no |  |
| 00336D78 | sceSifResetIop | libkernl.a, anchored | 85 | 00304E30 | callgraph 0.98 | 0.98 | 6/0/0/0 | yes | via 1 call site(s)/reference(s) |
| 00336ED0 | sceSifIsAliveIop | libkernl.a, sig | 10 | 00304F88 | exact | 1.00 | 1/0/0/0 | no |  |
| 00336EF8 | sceSifSyncIop | libkernl.a, sig | 20 | 00304FB0 | exact | 1.00 | 3/0/0/0 | yes |  |
| 00336F48 | sceSifRebootIop | libkernl.a, sig | 68 | 00305000 | exact | 1.00 | 4/0/0/0 | yes |  |
| 00337058 | DIntr | libkernl.a, aligned | 21 | 00305110 | callgraph 0.93 | 0.93 | 0/0/0/0 | yes | via 47 call site(s)/reference(s) |
| 003370B0 | EIntr | libkernl.a, sig | 6 | 00305160 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00337110 | __sceKernlGetEhSemaId | libkernl.a, anchored | 3 | 003051D8 | exact | 1.00 | 0/0/0/0 | no | short body, confirmed by neighbour displacement |
| 00337120 | Copy | libkernl.a, anchored | 4 | 003051E8 | exact | 1.00 | 0/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 00337130 | kCopy | libkernl.a, sig | 14 | 003051F8 | exact | 1.00 | 0/0/0/0 | no | 4 identical copies in Zero, picked by layout |
| 00337168 | kFindAddress | libkernl.a, sig | 16 | 00305230 | exact | 1.00 | 0/0/0/0 | no |  |
| 003371A8 | FindAddress | libkernl.a, anchored | 4 | 00305270 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003371B8 | GetSystemCallEntry | libkernl.a, sig | 13 | 00305280 | exact | 1.00 | 1/0/0/0 | no |  |
| 003371F0 | InitSystemCallTableAddress | libkernl.a, sig | 63 | 003052B8 | exact | 1.00 | 4/0/0/0 | yes |  |
| 003372F0 | setup | libkernl.a, anchored | 4 | 003053B8 | exact | 1.00 | 0/0/0/0 | yes | 6 identical copies in Zero, picked by layout |
| 00337300 | _InitSys | libkernl.a, sig | 1869 | 003053C8 | fuzzy 0.99 | 0.99 | 48/0/0/0 | yes | runner-up 0.73 |
| 00337358 | setup | libkernl.a, anchored | 4 | 00305420 | exact | 1.00 | 0/0/0/0 | yes | 6 identical copies in Zero, picked by layout |
| 00337368 | Copy | libkernl.a, anchored | 4 | 00305430 | exact | 1.00 | 0/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 00337378 | kCopy | libkernl.a, sig | 14 | 00305440 | exact | 1.00 | 0/0/0/0 | no | 4 identical copies in Zero, picked by layout |
| 003373B0 | GetEntryAddress | libkernl.a, anchored | 4 | 00305478 | exact | 1.00 | 0/0/0/0 | yes | 4 identical copies in Zero, picked by layout |
| 003373C0 | PatchIsNeeded | libkernl.a, sig | 26 | 00305488 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00337428 | InitExecPS2 | libkernl.a, sig | 43 | 003054F0 | exact | 1.00 | 5/0/0/0 | yes |  |
| 003374D8 | Copy | libkernl.a, anchored | 4 | 003055A0 | exact | 1.00 | 0/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 003374E8 | kCopy | libkernl.a, sig | 12 | 003055B0 | exact | 1.00 | 0/0/0/0 | no |  |
| 00337518 | setup | libkernl.a, anchored | 4 | 003055E0 | exact | 1.00 | 0/0/0/0 | yes | 6 identical copies in Zero, picked by layout |
| 00337528 | SetArg | libkernl.a, sig | 70 | 003055F0 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00337640 | TerminateLibrary | libkernl.a, anchored | 2 | 00305708 | exact | 0.00 | 1/0/0/0 | yes | 24 identical copies in Zero, picked by layout |
| 00337648 | ExecPS2 | libkernl.a, sig | 28 | 00305710 | exact | 1.00 | 3/0/0/0 | no |  |
| 003376B8 | LoadExecPS2 | libkernl.a, sig | 18 | 00305780 | exact | 1.00 | 3/0/0/0 | no |  |
| 00337700 | Exit | libkernl.a, anchored | 10 | 003057C8 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00337728 | ExecOSD | libkernl.a, sig | 19 | 003057F0 | exact | 1.00 | 3/0/0/0 | no |  |
| 00337778 | setup | libkernl.a, anchored | 4 | 00305840 | exact | 1.00 | 0/0/0/0 | yes | 6 identical copies in Zero, picked by layout |
| 00337788 | Copy | libkernl.a, anchored | 4 | 00305850 | exact | 1.00 | 0/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 00337798 | kCopy | libkernl.a, sig | 14 | 00305860 | exact | 1.00 | 0/0/0/0 | no | 4 identical copies in Zero, picked by layout |
| 003377D0 | GetEntryAddress | libkernl.a, anchored | 4 | 00305898 | exact | 1.00 | 0/0/0/0 | yes | 4 identical copies in Zero, picked by layout |
| 003377E0 | InitAlarm | libkernl.a, sig | 53 | 003058A8 | exact | 1.00 | 4/0/0/0 | yes |  |
| 003378B8 | SetT2 | libkernl.a, sig | 26 | 00305980 | exact | 0.96 | 0/0/0/0 | yes |  |
| 00337920 | SetT2_COUNT | libkernl.a, anchored | 2 | 003059E8 | exact | 1.00 | 0/0/0/0 | no | 1544 identical copies in Zero, picked by layout |
| 00337958 | InitTimer | libkernl.a, sig | 85 | 00305A20 | exact | 1.00 | 4/0/0/0 | yes |  |
| 00337AB0 | EndTimer | libkernl.a, sig | 46 | 00305B78 | exact | 1.00 | 3/0/0/0 | no |  |
| 00337B98 | StartTimerSystemTime | libkernl.a, sig | 36 | 00305C60 | exact | 1.00 | 4/0/0/0 | yes |  |
| 00337C28 | StopTimerSystemTime | libkernl.a, sig | 31 | 00305CF0 | exact | 1.00 | 2/0/0/0 | no |  |
| 00337CA8 | SetNextComp | libkernl.a, sig | 100 | 00305D70 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00337E38 | InsertAlarm | libkernl.a, sig | 33 | 00305F00 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00337EC0 | UnlinkAlarm | libkernl.a, sig | 14 | 00305F88 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00338198 | iGetTimerSystemTime | libkernl.a, sig | 19 | 00306268 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003381E8 | GetTimerSystemTime | libkernl.a, sig | 31 | 003062B8 | exact | 1.00 | 2/0/0/0 | no |  |
| 00338268 | iAllocTimerCounter | libkernl.a, sig | 28 | 00306338 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003382D8 | AllocTimerCounter | libkernl.a, sig | 18 | 003063A8 | exact | 1.00 | 3/0/0/0 | yes |  |
| 00338320 | iFreeTimerCounter | libkernl.a, sig | 40 | 003063F0 | near | 0.97 | 1/0/0/0 | yes | 1/40 words differ |
| 003383C0 | FreeTimerCounter | libkernl.a, anchored | 19 | 00306490 | exact | 1.00 | 3/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 00338410 | iGetTimerUsedUnusedCounters | libkernl.a, sig | 20 | 003064E0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00338460 | GetTimerUsedUnusedCounters | libkernl.a, sig | 23 | 00306530 | exact | 1.00 | 3/0/0/0 | no |  |
| 003384C0 | iStartTimerCounter | libkernl.a, sig | 43 | 00306590 | near | 0.98 | 3/0/0/0 | yes | 1/43 words differ |
| 00338570 | StartTimerCounter | libkernl.a, anchored | 19 | 00306640 | exact | 1.00 | 3/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 003385C0 | iStopTimerCounter | libkernl.a, sig | 49 | 00306690 | near | 0.98 | 3/0/0/0 | yes | 1/49 words differ |
| 00338688 | StopTimerCounter | libkernl.a, anchored | 19 | 00306758 | exact | 1.00 | 3/0/0/0 | no | 5 identical copies in Zero, picked by layout |
| 003386D8 | SetTimerCount | libkernl.a, sig | 51 | 003067A8 | exact | 1.00 | 3/0/0/0 | no |  |
| 003387A8 | iGetTimerBaseTime | libkernl.a, anchored | 18 | 00306878 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003387F0 | GetTimerBaseTime | libkernl.a, anchored | 19 | 003068C0 | exact | 1.00 | 3/0/0/0 | no | 5 identical copies in Zero, picked by layout |
| 00338840 | iGetTimerCount | libkernl.a, sig | 27 | 00306910 | exact | 1.00 | 1/0/0/0 | yes |  |
| 003388B0 | GetTimerCount | libkernl.a, anchored | 19 | 00306980 | exact | 1.00 | 3/0/0/0 | no | 5 identical copies in Zero, picked by layout |
| 00338900 | iSetTimerHandler | libkernl.a, sig | 61 | 003069D0 | near | 0.98 | 4/0/0/0 | yes | 1/61 words differ |
| 003389F8 | SetTimerHandler | libkernl.a, sig | 31 | 00306AC8 | exact | 1.00 | 3/0/0/0 | yes |  |
| 00338B20 | TimerUSec2BusClock | libkernl.a, anchored | 28 | 00306BF0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00338B90 | TimerBusClock2Freq | libkernl.a, anchored | 21 | 00306C60 | exact | 1.00 | 0/0/0/0 | no |  |
| 00338C20 | _InitAlarm | libkernl.a, anchored | 19 | 00306CF0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00338C70 | AlarmHandler | libtimer.a, sig | 23 | 00306D40 | exact | 1.00 | 0/0/0/0 | no |  |
| 00338CD0 | SetTimerAlarm | libkernl.a, sig | 75 | 00306DA0 | exact | 1.00 | 6/0/0/0 | yes |  |
| 00338E00 | iSetTimerAlarm | libkernl.a, sig | 60 | 00306ED0 | exact | 1.00 | 4/0/0/0 | no |  |
| 00338EF0 | ReleaseTimerAlarm | libkernl.a, sig | 42 | 00306FC0 | exact | 1.00 | 3/0/0/0 | no |  |
| 00338F98 | iReleaseTimerAlarm | libkernl.a, sig | 30 | 00307068 | exact | 1.00 | 1/0/0/0 | no |  |
| 00339038 | sceSifSetRebootNotifyBuffer | libkernl.a, sig | 31 | 00307108 | exact | 1.00 | 2/0/0/0 | no |  |
| 003390B8 | sceSifAddRebootNotifyHandler | libkernl.a, sig | 46 | 00307188 | exact | 1.00 | 2/0/0/0 | no |  |
| 003391C8 | sceSifExecNotifyHandler | libkernl.a, sig | 42 | 00307298 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00339270 | topThread | libkernl.a, sig | 54 | 002FCFE0 | exact | 1.00 | 4/0/0/0 | no |  |
| 00339348 | InitThread | libkernl.a, aligned | 53 | 002FD0B8 | callgraph 0.77 | 0.77 | 6/1/0/0 | yes | via 1 call site(s)/reference(s) |
| 00339420 | iWakeupThread | libkernl.a, sig | 37 | 002FD1A8 | exact | 1.00 | 2/0/0/0 | no |  |
| 003394B8 | iRotateThreadReadyQueue | libkernl.a, sig | 31 | 002FD240 | exact | 1.00 | 1/0/0/0 | no |  |
| 00339538 | iSuspendThread | libkernl.a, sig | 38 | 002FD2C0 | exact | 1.00 | 2/0/0/0 | no |  |
| 003395D0 | DelayThread | libkernl.a, sig | 50 | 002FD358 | near | 0.98 | 6/0/0/0 | yes | 1/50 words differ |
| 00339698 | StartThread | libkernl.a, anchored | 10 | 002FD598 | exact | 1.00 | 1/0/0/0 | no |  |
| 003396C0 | sceDeci2Open | libkernl.a, sig | 17 | 00307340 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339708 | sceDeci2Close | libkernl.a, sig | 9 | 00307388 | exact | 1.00 | 1/0/0/0 | no |  |
| 00339730 | sceDeci2ReqSend | libkernl.a, sig | 12 | 003073B0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339760 | sceDeci2Poll | libkernl.a, sig | 9 | 003073E0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339788 | sceDeci2GrpOpen | libkernl.a, sig | 12 | 00307408 | exact | 1.00 | 1/0/0/0 | no |  |
| 003397B8 | sceDeci2GrpClose | libkernl.a, sig | 9 | 00307438 | exact | 1.00 | 1/0/0/0 | no |  |
| 003397E0 | sceDeci2ExRecv | libkernl.a, sig | 13 | 00307460 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339818 | sceDeci2ExSend | libkernl.a, sig | 13 | 00307498 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339880 | sceDeci2ExLock | libkernl.a, sig | 9 | 00307500 | exact | 1.00 | 1/0/0/0 | no |  |
| 003398A8 | sceDeci2ExUnLock | libkernl.a, sig | 9 | 00307528 | exact | 1.00 | 1/0/0/0 | no |  |
| 003398D0 | kputs | libkernl.a, sig | 9 | 00307550 | exact | 1.00 | 1/0/0/0 | yes |  |
| 003398F8 | _set_sreg | libkernl.a, sig | 7 | 00307578 | exact | 1.00 | 0/0/0/0 | no |  |
| 00339918 | _change_addr | libkernl.a, anchored | 3 | 00307598 | exact | 1.00 | 0/0/0/0 | no | short body, confirmed by neighbour displacement |
| 00339928 | sceSifGetSreg | libkernl.a, anchored | 6 | 003075A8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00339960 | sceSifGetDataTable | libkernl.a, anchored | 3 | 003075E0 | exact | 1.00 | 0/0/0/0 | no | 55 identical copies in Zero, picked by layout |
| 00339970 | sceSifInitCmd | libkernl.a, sig | 160 | 003075F0 | near | 0.97 | 11/0/0/0 | yes | 5/160 words differ |
| 00339BF0 | sceSifExitCmd | libkernl.a, sig | 13 | 00307870 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339C58 | sceSifAddCmdHandler | libkernl.a, sig | 30 | 003078D8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00339CD0 | sceSifRemoveCmdHandler | libkernl.a, sig | 19 | 00307950 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00339D20 | _sceSifSendCmd | libkernl.a, sig | 77 | 003079A0 | exact | 1.00 | 3/0/0/0 | yes |  |
| 00339E58 | sceSifSendCmd | libkernl.a, sig | 15 | 00307AD8 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339E98 | isceSifSendCmd | libkernl.a, sig | 15 | 00307B18 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00339ED8 | _sceSifCmdIntrHdlr | libkernl.a, sig | 82 | 00307B58 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033A020 | sceSifWriteBackDCache | libkernl.a, sig | 43 | 00307CA0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033A130 | SetDebugHandler | libkernl.a, sig | 33 | 00307DB0 | exact | 1.00 | 3/0/0/0 | no |  |
| 0033A1B8 | Copy | libkernl.a, anchored | 4 | 00307E38 | exact | 1.00 | 0/0/0/0 | yes | 5 identical copies in Zero, picked by layout |
| 0033A1C8 | kCopy | libkernl.a, sig | 14 | 00307E48 | exact | 1.00 | 0/0/0/0 | no | 4 identical copies in Zero, picked by layout |
| 0033A200 | GetEntryAddress | libkernl.a, anchored | 4 | 00307E80 | exact | 1.00 | 0/0/0/0 | yes | 4 identical copies in Zero, picked by layout |
| 0033A210 | setup | libkernl.a, anchored | 4 | 00307E90 | exact | 1.00 | 0/0/0/0 | yes | 6 identical copies in Zero, picked by layout |
| 0033A220 | InitTLBFunctions | libkernl.a, sig | 49 | 00307EA0 | exact | 1.00 | 4/0/0/0 | yes |  |
| 0033A308 | _SetTLBEntry | libkernl.a, anchored | 4 | 00307F88 | exact | 1.00 | 0/0/0/0 | yes | 2 identical copies in Zero, picked by layout |
| 0033A318 | SetTLBEntry | libkernl.a, sig | 12 | 00307F98 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033A3A8 | InitTLB | libkernl.a, sig | 16 | 00308028 | exact | 1.00 | 3/0/0/0 | no |  |
| 0033A3E8 | InitTLB32MB | libkernl.a, sig | 125 | 00308068 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0033A600 | _kTLBException | libkernl.a, sig | 64 | 00308280 | exact | 1.00 | 0/0/0/0 | no |  |
| 0033A740 | _kExitTLBHandler | libkernl.a, sig | 61 | 003083C0 | exact | 1.00 | 0/0/0/0 | no |  |
| 0033A840 | _kDebugException | libkernl.a, sig | 63 | 003084C0 | exact | 1.00 | 0/0/0/0 | no |  |
| 0033ABF8 | sceDbcEnd | libdbc.a, sig | 18 | 003088D8 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033AF58 | sceDbcGetConnection | libdbc.a, sig | 61 | 00308D50 | exact | 1.00 | 3/0/0/0 | no |  |
| 0033B858 | DPRINT | libmtap.a, sig | 10 | 00309808 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033B880 | scePad2Init | libpad2.a, sig | 16 | 00309830 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033B8C0 | scePad2End | libpad2.a, sig | 26 | 00309870 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033B928 | scePad2CreateSocket | libpad2.a, sig | 66 | 003098D8 | exact | 1.00 | 2/0/0/0 | yes |  |
| 0033BA30 | scePad2DeleteSocket | libpad2.a, sig | 21 | 003099E0 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033BA88 | scePad2Read | libpad2.a, anchored | 53 | 00309A38 | near | 0.92 | 3/0/0/0 | yes | 4/53 words differ |
| 0033BB60 | scePad2GetButtonProfile | libpad2.a, anchored | 52 | 00309B10 | fuzzy 0.95 | 0.95 | 4/0/0/0 | yes | runner-up 0.63 |
| 0033BC30 | scePad2GetState | libpad2.a, sig | 80 | 00309BE0 | exact | 1.00 | 4/0/0/0 | yes |  |
| 0033BE50 | scePad2InitDmaDBuff | libpad2.a, sig | 30 | 00309E00 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033BEC8 | scePad2LinkDriver | libpad2.a, sig | 21 | 00309E78 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033BF20 | scePad2GetSide | libpad2.a, sig | 25 | 00309ED0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0033BF88 | scePad2GetSide2 | libpad2.a, sig | 25 | 00309F38 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0033BFF0 | scePad2CheckDma | libpad2.a, sig | 23 | 00309FA0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0033C050 | scePad2SetButtonOrder | libpad2.a, sig | 44 | 0030A000 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033C138 | sceVibGetProfile | libvib.a, sig | 32 | 0030A0E8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033C1B8 | sceVibSetActParam | libvib.a, sig | 59 | 0030A168 | exact | 1.00 | 0/0/0/0 | yes |  |
| 0033C2A8 | sceMcInit | libmc.a, aligned | 115 | 0030A258 | layout 0.67 | 0.67 | 8/0/0/0 | yes | best of 2 starts between neighbours |
| 0033C478 | sceMcEnd | libmc.a, aligned | 19 | 0030A480 | layout 0.73 | 0.73 | 1/0/0/0 | yes | best of 1 starts between neighbours |
| 0033C4F8 | sceMcChangeThreadPriority | libmc.a, anchored | 3 | 0030A4D8 | exact | 1.00 | 1/0/0/0 | no | via 1 call site(s)/reference(s) |
| 0033C508 | sceMcChangeThreadPriority | libmc.a, aligned | 47 | 0030A4E8 | fuzzy 0.87 | 0.87 | 4/0/0/0 | no | runner-up 0.81 |
| 0033C5C8 | sceMcGetSlotMax | libmc.a, aligned | 49 | 0030A5A8 | layout 0.78 | 0.78 | 3/0/0/0 | no | best of 1 starts between neighbours |
| 0033C690 | sceMcOpen | libmc.a, aligned | 74 | 0030A668 | callgraph 0.86 | 0.86 | 4/0/0/0 | yes | via 1 call site(s)/reference(s) |
| 0033C7B8 | sceMcMkdir | libmc.a, sig | 13 | 0030A790 | exact | 1.00 | 1/0/0/0 | yes |  |
| 0033C7F0 | sceMcClose | libmc.a, aligned | 47 | 0030A7C8 | layout 0.85 | 0.85 | 4/0/0/0 | yes | best of 1 starts between neighbours |
| 0033C8B0 | sceMcSeek | libmc.a, aligned | 56 | 0030A888 | fuzzy 0.88 | 0.88 | 4/0/0/0 | yes | runner-up 0.54 |
| 0033C990 | mceIntrReadFixAlign | libmc.a, aligned | 38 | 0030A968 | layout 0.46 | 0.46 | 1/0/0/0 | no | best of 1 starts between neighbours, clear margin |
| 0033CA28 | sceMcRead | libmc.a, anchored | 69 | 0030AA18 | fuzzy 0.86 | 0.86 | 5/0/0/0 | yes | runner-up 0.50 |
| 0033CB40 | sceMcWrite | libmc.a, anchored | 94 | 0030AB30 | fuzzy 0.93 | 0.93 | 5/0/0/0 | yes | runner-up 0.32 |
| 0033CCB8 | sceMcSync | libmc.a, sig | 49 | 0030ACA8 | exact | 1.00 | 3/0/0/0 | yes |  |
| 0033CD80 | mceGetInfoApdx | libmc.a, sig | 22 | 0030AD70 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033CDD8 | sceMcGetInfo | libmc.a, anchored | 95 | 0030ADC8 | fuzzy 0.93 | 0.93 | 5/0/0/0 | yes | runner-up 0.37 |
| 0033CF58 | sceMcUdCheckNewCard | libmc.a, anchored | 28 | 0030AF48 | near | 0.96 | 3/0/0/0 | no | 1/28 words differ |
| 0033CFC8 | sceMcGetDir | libmc.a, anchored | 83 | 0030AFB8 | fuzzy 0.81 | 0.81 | 5/0/0/0 | yes | runner-up 0.55 |
| 0033D118 | mceStorePwd | libmc.a, sig | 35 | 0030B108 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033D1A8 | sceMcChdir | libmc.a, anchored | 80 | 0030B198 | layout 0.75 | 0.75 | 5/0/0/0 | yes | best of 2 starts between neighbours |
| 0033D2E8 | sceMcFormat | libmc.a, anchored | 51 | 0030B2D8 | layout 0.88 | 0.88 | 4/0/0/0 | yes | best of 1 starts between neighbours |
| 0033D3B8 | sceMcDelete | libmc.a, anchored | 71 | 0030B3A8 | fuzzy 0.86 | 0.86 | 4/0/0/0 | yes | runner-up 0.70 |
| 0033D4D8 | sceMcFlush | libmc.a, anchored | 47 | 0030B4C8 | layout 0.85 | 0.85 | 4/0/0/0 | yes | best of 1 starts between neighbours |
| 0033D598 | sceMcSetFileInfo | libmc.a, anchored | 115 | 0030B588 | fuzzy 0.90 | 0.90 | 5/0/0/0 | no | runner-up 0.55 |
| 0033D768 | sceMcRename | libmc.a, anchored | 87 | 0030B758 | fuzzy 0.89 | 0.89 | 5/0/0/0 | no | runner-up 0.54 |
| 0033D8C8 | sceMcUnformat | libmc.a, aligned | 51 | 0030B8B0 | layout 0.88 | 0.88 | 4/0/0/0 | no | best of 2 starts between neighbours |
| 0033D998 | sceMcGetEntSpace | libmc.a, aligned | 67 | 0030B980 | layout 0.73 | 0.73 | 4/0/0/0 | no | best of 1 starts between neighbours |
| 0033DAA8 | sceCdCallback | libcdvd.a, sig | 26 | 0030BA90 | exact | 1.00 | 3/0/0/0 | no |  |
| 0033DB10 | _sceCd_cd_callback | libcdvd.a, sig | 40 | 0030BAF8 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033DBB0 | _sceCd_cbLoop | libcdvd.a, sig | 52 | 0030BB98 | exact | 1.00 | 3/0/0/0 | no |  |
| 0033DC80 | sceCdInitEeCB | libcdvd.a, anchored | 51 | 0030BC68 | layout 0.33 | 0.33 | 3/2/1/0 | no | the only function between its neighbours in both games |
| 0033DD50 | _sceCd_cd_read_intr | libcdvd.a, sig | 46 | 0030BD08 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033DE08 | cmd_sem_init | libcdvd.a, aligned | 46 | 0030BDC0 | callgraph 0.76 | 0.76 | 1/0/0/0 | yes | via 3 call site(s)/reference(s) |
| 0033DEC0 | cdvd_exit | libcdvd.a, sig | 40 | 0030BEA8 | exact | 1.00 | 5/0/0/0 | yes |  |
| 0033DF60 | sceCdPOffCallback | libcdvd.a, sig | 32 | 0030BF48 | exact | 1.00 | 3/0/0/0 | no |  |
| 0033DFE0 | _sceCd_Poff_Intr | libcdvd.a, sig | 21 | 0030BFC8 | exact | 1.00 | 0/0/0/0 | no |  |
| 0033E038 | PowerOffCB | libcdvd.a, sig | 30 | 0030C020 | exact | 1.00 | 4/0/0/0 | yes |  |
| 0033E0B0 | sceCdLayerSearchFile | libcdvd.a, anchored | 197 | 0030C098 | callgraph 0.97 | 0.97 | 9/0/1/0 | yes | via 1 call site(s)/reference(s) |
| 0033E3C8 | sceCdSearchFile | libcdvd.a, aligned | 7 | 0030C398 | exact | 1.00 | 1/0/0/0 | no |  |
| 0033E3E8 | _sceCd_ncmd_prechk | libcdvd.a, aligned | 92 | 0030C3B8 | callgraph 0.94 | 0.94 | 7/0/1/0 | yes | via 1 call site(s)/reference(s) |
| 0033E558 | sceCdNcmdDiskReady | libcdvd.a, sig | 38 | 0030C510 | near | 0.95 | 3/0/0/0 | no | 2/38 words differ |
| 0033E5F0 | sceCdSync | libcdvd.a, sig | 40 | 0030C5A8 | exact | 1.00 | 3/0/0/0 | yes |  |
| 0033E690 | sceCdSyncS | libcdvd.a, sig | 27 | 0030C648 | exact | 1.00 | 3/0/0/0 | yes |  |
| 0033E700 | _sceCd_scmd_prechk | libcdvd.a, aligned | 92 | 0030C6B8 | callgraph 0.94 | 0.94 | 7/0/1/0 | yes | via 1 call site(s)/reference(s) |
| 0033E920 | sceCdInit | libcdvd.a, verified | 184 | 0030C8C0 | fuzzy 0.85 | 0.85 | 9/0/1/0 | yes | runner-up 0.32 |
| 0033EC00 | sceCdDiskReady_old | libcdvd.a, verified | 126 | 0030CBA8 | fuzzy 0.97 | 0.97 | 9/0/0/0 | yes | runner-up 0.65 |
| 0033EDF8 | sceCdDiskReady | libcdvd.a, verified | 129 | 0030CDC0 | fuzzy 0.97 | 0.97 | 9/0/0/0 | no | runner-up 0.65 |
| 0033F000 | sceCdRead | libcdvd.a, sig | 120 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F1E0 | sceCdReadIOPm | libcdvd.a, sig | 85 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F338 | sceCdGetToc | libcdvd.a, sig | 164 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F5C8 | sceCdSeek | libcdvd.a, sig | 51 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F730 | sceCdGetError | libcdvd.a, sig | 38 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F7C8 | sceCdStatus | libcdvd.a, sig | 46 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F880 | sceCdBreak | libcdvd.a, sig | 46 | - | not found | | | | not linked in Zero: SDK 3.0 libcdvd keeps it in its own object and Zero never calls it |
| 0033F938 | sceCdMmode | libcdvd.a, sig | 52 | 0030CFE0 | near | 0.98 | 4/0/0/0 | yes | 1/52 words differ |
| 0033FA08 | sceMpegInit | libmpeg.a, sig | 44 | 0030DE10 | exact | 1.00 | 3/0/0/0 | yes |  |
| 0033FCD0 | sceMpegAddBs | libmpeg.a, sig | 15 | 0030DEC8 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003400C8 | _sceMpegAlalcAlloc | libmpeg.a, sig | 27 | 0030E490 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003431D8 | _dualPrimeVector | libmpeg.a, aligned | 97 | 0030FAF0 | near | 0.97 | 0/0/0/0 | yes | 3/97 words differ |
| 00343360 | _getAllRefs | libmpeg.a, aligned | 68 | - | not found | | | |  |
| 00343470 | _motionComp0 | libmpeg.a, aligned | 70 | - | not found | | | | callgraph points at Zero 00310C78 but body ratio only 0.30 |
| 00343588 | _getRef0 | libmpeg.a, aligned | 75 | - | not found | | | |  |
| 003436B8 | _doMC | libmpeg.a, aligned | 127 | - | not found | | | |  |
| 003438B8 | _sceMpegPictureData0 | libmpeg.a, aligned | 47 | 003123B8 | fuzzy 0.83 | 0.83 | 0/0/0/0 | yes | runner-up 0.23 |
| 00343978 | _slice0 | libmpeg.a, aligned | 298 | - | not found | | | |  |
| 00343E20 | _motionVector | libmpeg.a, aligned | 34 | - | not found | | | |  |
| 00343EA8 | _motionVectors | libmpeg.a, sig | 103 | 00311610 | exact | 1.00 | 0/0/0/0 | yes |  |
| 00344048 | _decMB0 | libmpeg.a, aligned | 83 | - | not found | | | | callgraph points at Zero 003111E8 but body ratio only 0.23 |
| 00344198 | _sceMpegMbAddressIncrement | libmpeg.a, aligned | 11 | - | not found | | | |  |
| 003441C8 | _sceMpegWaitIpuIdle | libmpeg.a, aligned | 90 | - | not found | | | |  |
| 00344330 | _sceMpegWaitIpuIdle64 | libmpeg.a, aligned | 97 | - | not found | | | |  |
| 003444B8 | _sceMpegNextBit | libmpeg.a, aligned | 155 | - | not found | | | |  |
| 00344728 | _sceMpegPeepBit | libmpeg.a, aligned | 71 | - | not found | | | | callgraph points at Zero 00312250 but body ratio only 0.38 |
| 00344848 | _sceMpegFlushBuf | libmpeg.a, aligned | 67 | - | not found | | | | callgraph points at Zero 003144E0 but body ratio only 0.03 |
| 00344958 | _waitIpuIdle | libmpeg.a, aligned | 91 | - | not found | | | | callgraph points at Zero 00312168 but body ratio only 0.23 |
| 00344AC8 | _flushBuf | libmpeg.a, aligned | 32 | - | not found | | | | callgraph points at Zero 00314648 but body ratio only 0.46 |
| 00344B48 | _skipMB0 | libmpeg.a, aligned | 25 | - | not found | | | |  |
| 00344BB0 | _copyAddRefImage | libmpeg.a, aligned | 71 | - | not found | | | |  |
| 00344CD0 | _copyRefImage | libmpeg.a, aligned | 295 | - | not found | | | |  |
| 00344D98 | dmaRefImage | libmpeg.a, aligned | 74 | 003142D8 | callgraph 0.60 | 0.60 | 0/0/0/0 | yes | via 1 call site(s)/reference(s) |
| 00344EC0 | _rix_000 | libmpeg.a, aligned | 124 | 003140D0 | fuzzy 0.80 | 0.80 | 0/0/0/0 | yes | runner-up 0.19 |
| 003450B0 | _ri0_000 | libmpeg.a, aligned | 18 | - | not found | | | |  |
| 00345170 | _rix_001 | libmpeg.a, aligned | 40 | 00313CA0 | fuzzy 0.85 | 0.85 | 1/0/0/0 | yes | runner-up 0.55 |
| 00345210 | _ri0_001 | libmpeg.a, aligned | 49 | - | not found | | | |  |
| 003452D8 | _rix_010 | libmpeg.a, aligned | 61 | 003147C0 | fuzzy 0.89 | 0.89 | 0/0/0/0 | yes | runner-up 0.35 |
| 003453D0 | _ri0_010 | libmpeg.a, aligned | 36 | 00314728 | fuzzy 0.93 | 0.93 | 0/0/0/0 | yes | runner-up 0.58 |
| 00345460 | _rix_011 | libmpeg.a, aligned | 51 | 0030E388 | fuzzy 0.82 | 0.82 | 0/0/0/0 | yes | runner-up 0.30 |
| 00345530 | _ri0_011 | libmpeg.a, aligned | 36 | - | not found | | | | layout: best start between neighbours Zero 0030E540 ratio 0.13 (runner-up 0.13) |
| 003455C0 | _rix_100 | libmpeg.a, aligned | 173 | - | not found | | | | layout: best start between neighbours Zero 0030E540 ratio 0.02 (runner-up 0.02) |
| 00345878 | _ri0_100 | libmpeg.a, aligned | 40 | - | not found | | | | layout: best start between neighbours Zero 0030E540 ratio 0.08 (runner-up 0.08) |
| 00345918 | _rix_101 | libmpeg.a, aligned | 164 | 0030E568 | near | 0.97 | 2/0/0/0 | yes | 5/164 words differ |
| 00345BA8 | _ri0_101 | libmpeg.a, aligned | 10 | - | not found | | | | layout: best start between neighbours Zero 0030EF98 ratio 0.09 (runner-up 0.05) |
| 00345BD0 | _rix_110 | libmpeg.a, aligned | 93 | - | not found | | | | layout: best start between neighbours Zero 0030E7F8 ratio 0.23 (runner-up 0.15) |
| 00345D48 | _ri0_110 | libmpeg.a, aligned | 68 | - | not found | | | | layout: best start between neighbours Zero 0030EED8 ratio 0.26 (runner-up 0.17) |
| 00345E58 | _rix_111 | libmpeg.a, aligned | 110 | - | not found | | | | layout: best start between neighbours Zero 0030EED8 ratio 0.11 (runner-up 0.10) |
| 00346010 | _ri0_111 | libmpeg.a, aligned | 91 | - | not found | | | | layout: best start between neighbours Zero 0030EF98 ratio 0.09 (runner-up 0.09) |
| 00346180 | receiveDataFromIPU | libmpeg.a, aligned | 33 | - | not found | | | | layout: best start between neighbours Zero 0030EED8 ratio 0.22 (runner-up 0.12) |
| 00346330 | _ch3dmaCSC | libmpeg.a, sig | 82 | 0030F6C0 | exact | 1.00 | 0/0/0/0 | no |  |
| 00346638 | _ch4dma | libmpeg.a, sig | 53 | 0030F808 | exact | 1.00 | 0/0/0/0 | no |  |
| 00346998 | setD3_CHCR | libipu.a, sig | 29 | 003148B8 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00346A10 | setD4_CHCR | libipu.a, sig | 29 | 00314930 | exact | 1.00 | 2/0/0/0 | yes | 2 identical copies in Zero, picked by layout |
| 00346A88 | sceIpuStopDMA | libipu.a, sig | 57 | 003149A8 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00346B70 | sceIpuRestartDMA | libipu.a, sig | 84 | 00314A90 | exact | 1.00 | 2/0/0/0 | yes |  |
| 00346D28 | setD4_CHCR | libipu.a, sig | 29 | 00314C48 | exact | 1.00 | 2/0/0/0 | yes | 2 identical copies in Zero, picked by layout |
| 00346DA0 | sceIpuInit | libipu.a, sig | 142 | 00314CC0 | exact | 1.00 | 1/0/0/0 | yes |  |
| 00388710 | _sceSDC | libkernl.a, sig | 41 | 002FCB30 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003887B8 | SyncDCache | libkernl.a, anchored | 31 | 002FCBD8 | exact | 1.00 | 3/0/0/0 | yes | 2 identical copies in Zero, picked by layout |
| 00388850 | _sceIDC | libkernl.a, sig | 41 | 002FCC70 | exact | 1.00 | 0/0/0/0 | yes |  |
| 003888F8 | InvalidDCache | libkernl.a, anchored | 31 | 002FCD18 | exact | 1.00 | 3/0/0/0 | no | 2 identical copies in Zero, picked by layout |
