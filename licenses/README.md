# Third-party license texts

Verbatim license texts for the native dependencies the published packages bundle,
fetched from the **pinned upstream releases** the build scripts use:

| File | Component | Version | SPDX |
|---|---|---|---|
| `Unicorn-COPYING` | Unicorn Engine (emulator) | 2.1.x | GPL-2.0-only |
| `Keystone-COPYING` | Keystone Engine (in-line assembler) | 0.9.2 ([build-keystone.sh](../scripts/build-keystone.sh)) | GPL-2.0-only |
| `Capstone-LICENSE.TXT` | Capstone (disassembler) | 5.0.1 ([build-capstone.sh](../scripts/build-capstone.sh)) | BSD-3-Clause |
| `Capstone-LICENSE_LLVM.TXT` | Capstone (LLVM-derived tables) | 5.0.1 | LLVM (Apache-2.0-with-LLVM-exception) |
| `DynamoRIO-<ver>.txt` | DynamoRIO (native-trace tier runtime) | 11.91.20630 ([fetch-dynamorio.sh](../scripts/fetch-dynamorio.sh)) | BSD-3-Clause |
| `LGPL-2.1.txt` | drwrap (DynamoRIO ext/) and umbra (Dr. Memory Framework) — verbatim GNU LGPL v2.1 | 11.91.20630 | LGPL-2.1-only |

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

[scripts/collect-licenses.sh](../scripts/collect-licenses.sh) copies these into each
package's `THIRD-PARTY-LICENSES/` — emitting the native-trace tier entries only when the
matching lib is actually staged into the slot — (a build-time capture from
`$PREFIX/share/licenses/<dep>-<ver>/` augments them with the exact-version text when
the dep was source-built). asm-test's own license is MIT — see the top-level
[LICENSE](../LICENSE). Because the packages convey the GPL-2.0 engines, each package
as distributed is effectively GPL-2.0; see
[docs/packaging.md](../docs/reference/packaging.md) and the
[fully-featured-packages plan](../docs/internal/archive/plans/fully-featured-packages-plan.md).
