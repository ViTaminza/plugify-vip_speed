# vip_speed (Plugify)

[VIP] Speed: writes the 'speed' feature value of a VIP's group (a decimal in [0.1, 2.0]) into the pawn's m_flVelocityModifier when the player spawns. Ported from the Pisex VIP core module VIP_Speed (vip_speed). Requires the vip core.

Plugify port (fork) of **VIP_Speed ([VIP] Speed)** by ABKAM (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_speed-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_speed`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

