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

The DynamoRIO text is captured on first fetch by
[fetch-dynamorio.sh](../scripts/fetch-dynamorio.sh) (the pinned tarball's own
`License.txt`); only the BSD-licensed core runtime (`libdynamorio.so`) is shipped. The
"full" hardware-trace build additionally bundles libipt / OpenCSD (both permissive) and,
when present, libbpf (conveyed under its BSD-2-Clause option) — their texts are captured
the same way when those libs are staged.

[scripts/collect-licenses.sh](../scripts/collect-licenses.sh) copies these into each
package's `THIRD-PARTY-LICENSES/` — emitting the native-trace tier entries only when the
matching lib is actually staged into the slot — (a build-time capture from
`$PREFIX/share/licenses/<dep>-<ver>/` augments them with the exact-version text when
the dep was source-built). asm-test's own license is MIT — see the top-level
[LICENSE](../LICENSE). Because the packages convey the GPL-2.0 engines, each package
as distributed is effectively GPL-2.0; see
[docs/packaging.md](../docs/reference/packaging.md) and the
[fully-featured-packages plan](../docs/internal/archive/plans/fully-featured-packages-plan.md).
