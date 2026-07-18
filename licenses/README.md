# Third-party license texts

Verbatim license texts for the native dependencies this repo's build/test scripts
fetch from **pinned upstream releases**. Most are bundled into the published
packages; Pin is the one test-lane-only exception (see its row and the paragraph
below).

| File | Component | Version | SPDX |
|---|---|---|---|
| `Unicorn-COPYING` | Unicorn Engine (emulator) | 2.1.x | GPL-2.0-only |
| `Keystone-COPYING` | Keystone Engine (in-line assembler) | 0.9.2 ([build-keystone.sh](../scripts/build-keystone.sh)) | GPL-2.0-only |
| `Capstone-LICENSE.TXT` | Capstone (disassembler) | 5.0.1 ([build-capstone.sh](../scripts/build-capstone.sh)) | BSD-3-Clause |
| `Capstone-LICENSE_LLVM.TXT` | Capstone (LLVM-derived tables) | 5.0.1 | LLVM (Apache-2.0-with-LLVM-exception) |
| `DynamoRIO-<ver>.txt` | DynamoRIO (native-trace tier runtime) | 11.91.20630 ([fetch-dynamorio.sh](../scripts/fetch-dynamorio.sh)) | BSD-3-Clause |
| `LGPL-2.1.txt` | drwrap (DynamoRIO ext/) and umbra (Dr. Memory Framework) — verbatim GNU LGPL v2.1 | 11.91.20630 | LGPL-2.1-only |
| `Pin-<ver>.txt` / `Pin-<ver>-third-party.txt` | Intel Pin (DBI pintool test lane) — **test-lane only, never bundled**: fetched and digest-verified at build/test time for `docker-pintool`/`pintool-test`, never linked into a shipped package | 4.2-99776-g21d818fa2 ([fetch-pin.sh](../scripts/fetch-pin.sh)) | LicenseRef-Intel-Simplified-Software-License |

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

[scripts/collect-licenses.sh](../scripts/collect-licenses.sh) copies the shipped-package
rows above (all but Pin) into each package's `THIRD-PARTY-LICENSES/` — emitting the
native-trace tier entries only when the
matching lib is actually staged into the slot — (a build-time capture from
`$PREFIX/share/licenses/<dep>-<ver>/` augments them with the exact-version text when
the dep was source-built). asm-test's own license is MIT — see the top-level
[LICENSE](../LICENSE). Because the packages convey the GPL-2.0 engines, each package
as distributed is effectively GPL-2.0; see
[docs/packaging.md](../docs/reference/packaging.md) and the
[fully-featured-packages plan](../docs/internal/archive/plans/fully-featured-packages-plan.md).
