# Third-party license texts

Verbatim license texts for the native dependencies the published packages bundle,
fetched from the **pinned upstream releases** the build scripts use:

| File | Component | Version | SPDX |
|---|---|---|---|
| `Unicorn-COPYING` | Unicorn Engine (emulator) | 2.1.x | GPL-2.0-only |
| `Keystone-COPYING` | Keystone Engine (in-line assembler) | 0.9.2 ([build-keystone.sh](../scripts/build-keystone.sh)) | GPL-2.0-only |
| `Capstone-LICENSE.TXT` | Capstone (disassembler) | 5.0.1 ([build-capstone.sh](../scripts/build-capstone.sh)) | BSD-3-Clause |
| `Capstone-LICENSE_LLVM.TXT` | Capstone (LLVM-derived tables) | 5.0.1 | LLVM (Apache-2.0-with-LLVM-exception) |

[scripts/collect-licenses.sh](../scripts/collect-licenses.sh) copies these into each
package's `THIRD-PARTY-LICENSES/` (a build-time capture from
`$PREFIX/share/licenses/<dep>-<ver>/` augments them with the exact-version text when
the dep was source-built). asm-test's own license is MIT — see the top-level
[LICENSE](../LICENSE). Because the packages convey the GPL-2.0 engines, each package
as distributed is effectively GPL-2.0; see
[docs/packaging.md](../docs/packaging.md) and the
[fully-featured-packages plan](../docs/plans/fully-featured-packages-plan.md).
