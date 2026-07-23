# Third-party license texts

Verbatim license texts for the native dependencies this repo's build/test scripts
fetch from **pinned upstream releases**. Most are bundled into the published
packages; the test-lane-only exceptions — never bundled, never linked into a
shipped package — are the Pin kits (both versions), Intel SDE, and libdft64
(see their rows and the paragraph below).

| File | Component | Version | SPDX |
|---|---|---|---|
| `Unicorn-COPYING` | Unicorn Engine (emulator) | 2.1.x | GPL-2.0-only |
| `Keystone-COPYING` | Keystone Engine (in-line assembler) | 0.9.2 ([build-keystone.sh](../scripts/build-keystone.sh)) | GPL-2.0-only |
| `Capstone-LICENSE.TXT` | Capstone (disassembler) | 5.0.1 ([build-capstone.sh](../scripts/build-capstone.sh)) | BSD-3-Clause |
| `Capstone-LICENSE_LLVM.TXT` | Capstone (LLVM-derived tables) | 5.0.1 | LLVM (Apache-2.0-with-LLVM-exception) |
| `DynamoRIO-<ver>.txt` | DynamoRIO (native-trace tier runtime) | 11.91.20630 ([fetch-dynamorio.sh](../scripts/fetch-dynamorio.sh)) | BSD-3-Clause |
| `DynamoRIO-fork.txt` | DynamoRIO built from the wilvk/dynamorio source fork (macOS drtrace tier runtime — upstream publishes no macOS release asset) | git-commit pin in [third-party-digests.txt](../scripts/third-party-digests.txt) ([build-dynamorio-macos.sh](../scripts/build-dynamorio-macos.sh)) | BSD-3-Clause |
| `LGPL-2.1.txt` | drwrap (DynamoRIO ext/) and umbra (Dr. Memory Framework) — verbatim GNU LGPL v2.1 | 11.91.20630 | LGPL-2.1-only |
| `Pin-<ver>.txt` / `Pin-<ver>-third-party.txt` | Intel Pin (DBI pintool test lane) — **test-lane only, never bundled**: fetched and digest-verified at build/test time for `docker-pintool`/`pintool-test`, never linked into a shipped package | 4.2-99776-g21d818fa2 ([fetch-pin.sh](../scripts/fetch-pin.sh)) | LicenseRef-Intel-Simplified-Software-License |
| `intel-sde-10.8.0/` | Intel SDE (test-lane oracle) — **test-lane only, never bundled**: the future/absent-ISA emulator, fetched and digest-verified at build/test time for `docker-sde`/`sde-test`, never linked into a shipped package | 10.8.0 ([fetch-sde.sh](../scripts/fetch-sde.sh)) | LicenseRef-Intel-Simplified-Software-License |
| `Pin-3.20-98437-gf02b61307.txt` / `-third-party.txt` | Intel Pin (libdft64 differential-oracle test lane) — **test/oracle-only, never bundled**: fetched + digest-verified at build/test time for `docker-taint-oracle`, never linked into a shipped package. The 3.20 kit is libdft64's only tested pin | 3.20-98437-gf02b61307 ([fetch-pin.sh](../scripts/fetch-pin.sh)) | LicenseRef-Intel-EULA-SDP-2018 |
| `libdft64.txt` | libdft64 (AngoraFuzzer fork) — the independently-implemented byte-level taint engine the DR taint client is cross-validated against — **test/oracle-only, never bundled**: fetched + git-commit-pinned for `docker-taint-oracle`, never linked into `libasmtest`/any binding | 20804d5 ([fetch-libdft.sh](../scripts/fetch-libdft.sh)) | BSD-3-Clause (Columbia libdft) |

The DynamoRIO text is captured on first fetch by
[fetch-dynamorio.sh](../scripts/fetch-dynamorio.sh) (the pinned tarball's own
`License.txt`); only the BSD-licensed core runtime (`libdynamorio.so`) is shipped by
default. The "full" hardware-trace build additionally bundles libipt / OpenCSD (both
permissive) and, when present, libbpf (conveyed under its BSD-2-Clause option) — their
texts are captured the same way when those libs are staged.

`LGPL-2.1.txt` is the §6 "copy of this License" that ships **only** when a drtrace build
opts into linking the LGPL-2.1 extensions `drwrap` (`-DPROBE_DRWRAP` / a future LGPL
client) or `umbra` (`-DPROBE_UMBRA`) and their `.so` is vendored into the slot. Default
drtrace builds are BSD-clean and bundle neither, so it is not emitted. When they are
bundled, [collect-licenses.sh](../scripts/collect-licenses.sh) records them as
`LGPL-2.1-only` and adds an LGPL-2.1 §6 written offer backed by
[fetch-corresponding-source.sh](../scripts/fetch-corresponding-source.sh)
(`INCLUDE_LGPL_SOURCE=1`). They are conveyed as unmodified, dynamically-linked shared
objects (§6 relink is satisfiable); GPL-2.0 already dominates the bundle, so they add no
stronger copyleft.

`Pin-<ver>.txt` and `Pin-<ver>-third-party.txt` are captured on first fetch by
[fetch-pin.sh](../scripts/fetch-pin.sh) from the pinned kit's
`licensing/intel-simplified-software-license.txt` and
`licensing/third-party-programs.txt` (the second records that the kit's bundled XED
decoder is Apache-2.0). Unlike every other row here, the Pin kit is fetched for
**test-lane use only** (`docker-pintool` / `pintool-test`): it is never linked into
`libasmtest`/`libasmtest_emu`, ships in no package, gets no public header, and is
**not** collected by `collect-licenses.sh` — the capture exists purely so the
fetched kit's proprietary license terms are recorded in the tree. See
[pin-xed-trace-tier.md](../docs/internal/implementations/pin-xed-trace-tier.md#constraints--gates)
for the full rationale.

`intel-sde-10.8.0/` is a **directory** (not a single `.txt`) captured verbatim on
first fetch by [fetch-sde.sh](../scripts/fetch-sde.sh) from the pinned kit's
`Licenses/` — the Intel Simplified Software License PDF (`LICENSE.pdf`), the
`third-party-programs.txt` notice, and the bundled Pin/XED `pin licensing/`
subdirectory. The Intel Simplified Software License permits redistribution
**without modification** with the notice reproduced, so the copy must stay
verbatim; the license is **not on the SPDX license list**, hence the
`LicenseRef-Intel-Simplified-Software-License` id above rather than a standard
SPDX identifier. Like Pin, SDE is a **test/oracle-only** dependency
(`docker-sde` / `sde-test`): it emulates future/absent ISA extensions (APX,
AVX10.2, AMX, AVX-512-on-AVX2) for the whole process so an unmodified suite runs
under `sde64 -future`, but it is never linked into `libasmtest`/`libasmtest_emu`,
ships in no package, gets no public header, and — unlike DynamoRIO, which *can*
be a native-trace-tier payload — never reaches a package slot, so
`collect-licenses.sh` carries **no** entry for it at all. See
[pin-sde-future-isa-lane.md](../docs/internal/implementations/pin-sde-future-isa-lane.md#constraints--gates)
for the full rationale.

The **Pin 3.20** kit (`docker-taint-oracle`, libdft64's only tested pin) is the same
`fetch-pin.sh` invoked with a 3.20 `PIN_VERSION`/`PIN_URL` override. Two kit-shape
differences the script now handles (and the older 4.2 path is unaffected by): the 3.20
tarball's top dir is `pin-3.20-…-gcc-linux` (no `external` segment), and its license
text is `licensing/EULA.txt` (the Intel EULA for Software Development Products, October
2018 version) rather than the 4.2 kit's `intel-simplified-software-license.txt` — so the
captured `Pin-3.20-98437-gf02b61307.txt` is that EULA. Like the 4.2 kit it is
**test/oracle-only**, never bundled. `libdft64.txt` is captured on first fetch by
[fetch-libdft.sh](../scripts/fetch-libdft.sh) from the pinned checkout's `LICENSE` (the
permissive Columbia libdft BSD-3-Clause text libdft64 inherits); the engine is likewise
oracle-only — linked only into the `docker-taint-oracle` Pintool, never into a shipped
package — so it too is recorded here for provenance and not collected by
`collect-licenses.sh`. See
[pin-libdft-taint-oracle.md](../docs/internal/implementations/pin-libdft-taint-oracle.md#constraints--gates).

[scripts/collect-licenses.sh](../scripts/collect-licenses.sh) copies the shipped-package
rows above (not the test-lane-only rows: Pin, SDE, libdft64) into each package's `THIRD-PARTY-LICENSES/` — emitting the
native-trace tier entries only when the
matching lib is actually staged into the slot — (a build-time capture from
`$PREFIX/share/licenses/<dep>-<ver>/` augments them with the exact-version text when
the dep was source-built). asm-test's own license is MIT — see the top-level
[LICENSE](../LICENSE). Because the packages convey the GPL-2.0 engines, each package
as distributed is effectively GPL-2.0; see
[docs/packaging.md](../docs/reference/packaging.md) and the
[fully-featured-packages plan](../docs/internal/archive/plans/fully-featured-packages-plan.md).
