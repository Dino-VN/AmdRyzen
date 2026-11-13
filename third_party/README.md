# Vendored third-party headers

These headers are needed only to compile `AMDZenSMC.kext` (the VirtualSMC plugin). They are unmodified copies of the upstream files. Both projects are BSD-3-Clause; each directory keeps its upstream `LICENSE.txt`.

| Directory | Upstream | Commit | Files |
|---|---|---|---|
| `virtualsmc/` | https://github.com/acidanthera/VirtualSMC | `b5c62c2311540c529c00996d0d9cec33f8e571c9` (master, 2026-09-27) | `VirtualSMCSDK/{AppleSmc.h, AppleSmcBridge.hpp, kern_keyvalue.hpp, kern_smcinfo.hpp, kern_value.hpp, kern_vsmcapi.hpp}`, `LICENSE.txt` |
| `lilu/` | https://github.com/acidanthera/Lilu | `0249c3a918c68a43891350f35fded615b1575776` (master) | `Lilu/Headers/{kern_util.hpp, kern_config.hpp, kern_compat.hpp}` copied to `Headers/`, `LICENSE.txt` |

Fetched with `curl` from `raw.githubusercontent.com/<repo>/<commit>/<path>` on 2026-11-09. The SHA-1 of every file matches the copies the research step compile-tested (`docs/research/smc-keys.md` §4).

## ABI notes

- The loaded VirtualSMC is a Dortania nightly labelled 1.3.9; GitHub has no 1.3.9 tag. The master headers were checked against its dSYM: `sizeof(VirtualSMCValue) == 0x90`, `sizeof(VirtualSMCKeyValue) == 0x18`, `sizeof(VirtualSMCAPI::Plugin) == 0x48`. `AMDZenSMC/src/AMDZenSMC.cpp` keeps these as `static_assert`s, so a header update that changes the layout fails the build.
- The Lilu headers are included only because the VirtualSMC SDK includes `<Headers/kern_util.hpp>`. AMDZenSMC links no Lilu symbol (`make check` verifies the import list).
- Include these directories with `-isystem` so their own warnings do not show up in the project's `-Wall` build.

To update: fetch the same paths at a new commit, update the table above, and rebuild. `make check` must still pass.
