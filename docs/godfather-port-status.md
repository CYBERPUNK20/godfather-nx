# The Godfather port status

## Target image

- Original Xbox XBE, North American release, title ID `4541007D`.
- XDK build `5849`; entry point `0x0025E1C6`.
- The XBE supplied by the owner is ignored by Git under `game/assets/default.xbe`.

## Recompilation

- Current codegen output: 54,130 functions, 1,517,645 decoded instructions, 704 jump tables, and 21 callback entries across 128 partitions.
- All 128 generated C++ partitions passed `g++ -std=c++20 -fsyntax-only` after the current recompiler changes.
- The recompiler now emits no `unimplemented:` instruction diagnostics for this image. BNDSTX remains an explicit invalid-instruction trap; the Xbox Pentium III CPU does not implement Intel MPX.
- Added packed integer SSE handling and x86 AF flag tracking, including decimal-adjust instructions. Conditional INTO and BOUND paths now continue through the function when they do not fault.

## XDK symbols

- The signature scan finds 180 of 238 functions and all 12 of 12 data symbols. Four additional function addresses have been manually verified and added, bringing the current mapping to 184 of 238 functions.
- `D3DDevice_FlushVertexCache` is mapped to `0x4A9F70`; among the scanner candidates, this one contains the XDK reference signature's `0x41710` push-buffer method. `CDirectSoundStream_AddRef` and `Release` were confirmed from the stream vtable at `.rdata:0x55F2F8`. `D3DDevice_SetPalette` was mapped to `0x4A9C20` after tracing its updates to `CDevice_Palettes[stage]` and palette reference counts.
- Remaining unmapped symbols include network and Bink APIs, plus several ambiguous or missing Direct3D and DirectSound entries. Their use through indirect calls still needs runtime evidence.

## Not validated yet

- No PC host executable or Switch NRO has been linked.
- No game boot, rendering, audio, save, or controller behavior has been tested.
- The build environment lacks the Windows host toolchain and the devkitPro Switch toolchain and graphics libraries. The Switch CMake project now defaults to Deko3D-only; NVK is an explicit optional build requiring mesa-switch archives.
- The generated code and supplied XBE are local build inputs; game assets are not included in this repository.
- The `installer/` directory is still configured for the upstream FIFA Street project and must not be used for this target.

## Next engineering step

Trace indirect calls that reach the still-unmapped graphics and audio entry points, then build the host runtime in an environment with the required platform toolchain. Only a boot trace can confirm which unresolved APIs need replacements.
