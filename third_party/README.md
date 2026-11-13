# third_party

Vendored build dependencies that were previously referenced by `build.sh`
from sibling trees outside this repository (`../integrated-mesa-next/...`,
`../async-draw-next/...`, `../broker-draw-next/...`, `../spirv-cross-build/...`).
A clean clone could not link `shim_cli` / `shim_gui` until these were brought
into the repo.

## rdgpu host objects (sources + headers)

The three rdgpu host-side objects that `build.sh` links are compiled from
source here rather than linked as pre-built `.o` files:

| file | provenance |
|------|------------|
| `rdgpu_broker.m`, `rdgpu_broker.h` | `metal-wayland-next/mesa-metal-driver-candidate/host-broker-candidate/` |
| `rdmetal_resource.m`, `rdmetal_resource.h` | `metal-wayland-next/mesa-metal-driver-candidate/host-resource-core/` |
| `rdgpu_draw_host.mm`, `rdgpu_draw_host.h` | `metal-wayland-next/broker-draw-next/` |
| `rdmetal_draw_wire.h` | `metal-wayland-next/shim-metal-clear/src/` (v1+v2+v3 wire) |
| `rdgpu_wire.h` | this repo's `src/` (stable broker wire protocol) |

`rdgpu_broker.h` / `rdmetal_resource.h` / `rdgpu_wire.h` are byte-identical
copies of this repo's own `src/` headers so the vendored code stays
ABI-identical to what `shim.c` compiles against.

`rdmetal_resource.m` and `rdgpu_broker.m` use manual Objective-C reference
counting and are compiled **without** `-fobjc-arc`; `rdgpu_draw_host.mm` uses
`@autoreleasepool` / `__bridge` and is compiled **with** `-fobjc-arc`.

## spirv-cross

* `spirv-cross/*.h,*.hpp` — public headers from `SPIRV-Cross`
  (`metal-wayland-next/spirv-cross-source/`), needed to compile
  `rdgpu_draw_host.mm` against `spirv_msl.hpp`.
* `lib/libspirv-cross-{msl,glsl,core}.a` — pre-built arm64 static libraries
  from `metal-wayland-next/spirv-cross-build/` (each < 50 MB, so they are
  committed directly rather than fetched or submoduled).

SPIRV-Cross is Apache-2.0; see `spirv-cross/LICENSE` for the upstream text if
the header tree is replaced by a full checkout.
