# The Godfather technical notes

This repository is an early static-recompilation port of the original Xbox game *The Godfather* (North American XBE, title ID `4541007D`, XDK 5849). The runtime and Switch graphics layer are derived from `street-nx` and its upstream projects; title-specific addresses and behavior must be confirmed against this XBE.

## Confirmed XDK mappings

The current signature scan finds 180 of 238 XDK functions. Four additional functions have been mapped manually from the game's code and data:

| Address | Function | Evidence |
|---|---|---|
| `0x4A9F70` | `D3DDevice_FlushVertexCache` | Candidate contains the reference signature's `0x41710` push-buffer method. |
| `0x4EF2B9` | `CDirectSoundStream_AddRef` | Stream vtable at `.rdata:0x55F2F8`; precedes known stream methods. |
| `0x4EF300` | `CDirectSoundStream_Release` | Same stream vtable's Release slot. |
| `0x4A9C20` | `D3DDevice_SetPalette` | Reads and writes `CDevice_Palettes[stage]` and adjusts palette references. |

The config therefore maps 184 of 238 function symbols and all 12 data symbols. The remaining unmapped entries include Bink and network functions, plus ambiguous Direct3D and DirectSound candidates. Do not assign addresses from another title's XBE without checking this image.

## Recompiler changes

- Added packed integer SSE operations used by the target's `.text` section.
- Added x86 auxiliary-carry tracking for decimal-adjust instructions.
- Implemented additional integer instructions and corrected non-faulting fallthrough for `INTO` and `BOUND`.
- Regeneration currently emits 54,130 functions, 1,517,645 decoded instructions, 704 jump tables, and 21 callback entries across 128 partitions.

## Build configuration

The Switch build now defaults to Deko3D-only, so it does not link the separate mesa-switch NVK archive set. `WITH_NVK=ON` opts into the Vulkan renderer and expects that SDK under `nvk_sdk/`. Both paths still need a real devkitPro build.

## Validation still needed

Generated C++ partitions pass syntax checking, but neither host executable nor Switch NRO has been linked. No boot, rendering, audio, save, or controller test has been run. The available environment lacks the Windows and devkitPro Switch toolchains. The XBE is a local input and game assets are not included in the repository.
