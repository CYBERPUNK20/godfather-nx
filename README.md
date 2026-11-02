# The Godfather for Nintendo Switch — work in progress

This project adapts the static recompilation/runtime approach used by [street-nx](https://github.com/xsaja/street-nx) for *The Godfather* (original Xbox). It is not a playable port yet. No `.nro` or tested build is available.

The target is the North American original Xbox release, XBE title ID `4541007D`. The project does not contain the game or its assets. Use your own disc dump and keep its files in `game/assets/`; `default.xbe` and game data are ignored by Git.

## Current progress

- Parsed the provided XBE and generated 128 C++ recompilation partitions.
- Added support for packed integer SSE and additional x86 flag, decimal-adjust, bounds, and segment-load instructions.
- Checked the generated C++ partitions for syntax with GCC.
- The recompiler no longer reports unimplemented instructions for this XBE. One explicit trap remains for BNDSTX, an MPX instruction unsupported by the Xbox CPU.
- Resolved an ambiguous Direct3D flush helper to the candidate matching the XDK reference signature.
- Updated the project identity and title ID checks for the PC and Switch hosts.
- No game boot, audio, graphics, or Switch hardware testing has been completed.

## Build outline

The source tree retains the upstream street-nx build structure:

1. On Windows, build the PC host and generate the recompilation output with `tools/build_pc.cmd`.
2. In the devkitPro MSYS2 shell, build the Switch program with `switch/build.sh`. This defaults to the Deko3D renderer. The optional NVK/Vulkan renderer can be included with `WITH_NVK=ON`, but it requires a separate mesa-switch NVK SDK in `nvk_sdk/`.

The build needs the Windows C++/Clang toolchain for the PC host and devkitPro packages for the Switch. The default Deko3D build avoids the separate mesa-switch NVK archives. This workspace currently has neither complete toolchain, so it cannot produce or test the final `.nro` here.

The game files stay local and must be copied to the Switch by the owner of the game. Do not distribute the XBE, disc image, or extracted game data.

The `installer/` directory is still the upstream FIFA Street installer and its release manifest does not target this game. Do not use it to package or install this Godfather port. A Godfather-specific package flow can only be prepared after a valid build exists.

## Credits and licensing

The runtime and Switch graphics layer are derived from street-nx and its upstream projects. See `LICENSE`, `LICENSES.txt`, `SDK-LICENSE.txt`, and `THIRD_PARTY.txt` for the applicable licenses and notices.
"# godfather-nx" 
