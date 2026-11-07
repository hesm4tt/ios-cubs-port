# iOS on Pixel: cubs port

An experimental iOS 16.5 port for the Pixel 11 (`cubs`), based on HakiOS and HackiOS. The aim is to share porting work for people exploring related Pixel devices. **This is a research source snapshot, not an installation guide or a ready-to-use ROM.** Other devices have not been verified.

The experiment runs iOS alongside Android, which owns the hardware and supplies several services through bridges. A supervised test has reached the iOS home screen and returned to Android. Internet access through Android's network has been demonstrated; native cellular, eSIM, Wi-Fi joining, fast charging, camera preview, and smooth rendering remain incomplete or unverified. Experimental code may not be enabled in the normal configuration.

## Source layout

- `cubs/current/pcwork/` contains newer Pixel bridge, GPU forwarding, launcher, sound, camera, and test sources.
- `cubs/current/hackios-cubs-tensor/xnu/files/` contains experimental XNU-side files.
- The top-level HakiOS directories preserve a source snapshot from the previous fork; see [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md).

The sources have not yet been validated as a self-contained build from a fresh checkout. Vendor firmware, device trees, boot images, device identifiers, IPSW content, generated binaries, and private test logs are excluded. Some inherited tooling refers to excluded assets or other devices and needs adaptation before use. The project does not distribute iOS.

## Acknowledgments

This work builds on and draws research from these projects:

- [HakiOS XNU kernel](https://github.com/oosexgang/hakios-xnu-kernel) and the [HackiOS XNU userland](https://github.com/oosexgang/hackios-xnu-userland), by OOSEX GANG and contributors, for the XNU port, drivers, loader, and userland tools.
- [OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm), for reverse-engineered Apple80211 interface definitions used in Wi-Fi research.
- [Bergamota](https://github.com/ry-diffusion/Bergamota), for related iOS porting research.

Preserved license texts and third-party notices are in [LICENSE](LICENSE), [LICENSE.APSL](LICENSE.APSL), [NOTICE](NOTICE), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), and the relevant source directories. Components retain their respective licenses. Some inherited notices describe upstream files excluded from this snapshot; see the migration notes in those files.

This project is independent and is not affiliated with Apple or Google. iOS and Pixel are trademarks of their respective owners.
