# PowerVR BXM-8-256 Vulkan driver repro cases

Standalone C++ reproductions for issues reported in the PowerVR forum topic
[PowerVR B-Series 1.15@6133110 on BXM-8-256 (Dimensity 7020): ten Vulkan driver issues with evidence](https://forums.imgtec.com/t/powervr-b-series-1-15-6133110-on-bxm-8-256-dimensity-7020-ten-vulkan-driver-issues-with-evidence/4334).

Each program is one source file plus a small shared bootstrap (`src/common.h`). There is no
window, no framework and no validation layer requirement. It creates a Vulkan 1.1 instance and device,
does the minimum needed to hit the issue, and prints `RESULT OK` when the driver behaves as the
specification requires. Shaders are GLSL in `shaders/`, compiled to SPIR-V (Vulkan 1.1 target)
at build time with the NDK's `glslc`.

## Device

| | |
| --- | --- |
| SoC / GPU | MediaTek Dimensity 7020 (MT6855), Imagination PowerVR BXM-8-256 |
| Driver | `PowerVR B-Series Vulkan Driver 1.15@6133110`, device API 1.1.170 |
| Phone | Infinix Note 40 Pro+ (X6851B) |

## Cases

| Program | Forum item | What it does | Expected (spec) |
| --- | --- | --- | --- |
| `repro_unused_attachment <control\|unused-nowrite\|unused-write> [msaa2]` | #3 | Creates a graphics pipeline whose subpass has `colorAttachmentCount = 1` with the reference set to `VK_ATTACHMENT_UNUSED` and a depth attachment; the fragment shader writes no output or writes location 0 | pipeline created |
| `repro_compute_cache <inprocess\|mixed\|save FILE\|load FILE>` | #8 | Builds two compute pipelines on an empty cache, takes `vkGetPipelineCacheData`, seeds a new cache with it (same process, or a second process via a file) and builds the same pipelines again; `mixed` puts 40 graphics pipelines in the same cache first | pipelines created |
| `repro_concurrent_compile [threads] [count] [serial]` | #7 | N threads, each with its own `VkPipelineCache`, create shader modules and graphics pipelines concurrently (unique specialization constant per pipeline); `serial` adds a global mutex as control | all pipelines created |
| `repro_view_format <control\|srgb-image-unorm-view\|unorm-image-srgb-view> [nodraw]` | #5 | Fills a mutable-format RGBA8 image with raw byte 128, runs a render pass on a view in the sibling format with `loadOp = LOAD`, a blend `src = ZERO, dst = ONE` draw and `storeOp = STORE`, copies the image back | texel reads back 128 128 128 128 |

For `repro_view_format`, if load and blend decode through the image format while the store
uses the view format, the readback is 55 for `srgb-image-unorm-view` and 188 for
`unorm-image-srgb-view`.

## Build

Needs the Android NDK (r25 or newer; it provides `glslc`), CMake 3.22 or newer and Ninja.

```sh
ANDROID_NDK=/path/to/android-ndk ./build.sh
```

Binaries land in `build/` (arm64-v8a, API 30, static libc++).

## Run

```sh
./run_on_device.sh [adb-serial]
```

This pushes the binaries to `/data/local/tmp/powervr-repros`, runs every case in its own process,
and writes `results.txt`. For crashing cases it includes the crash-buffer lines from logcat
(the full tombstone is in `/data/tombstones` or via `adb bugreport`). Any single case can be run by
hand:

```sh
adb push build/repro_view_format /data/local/tmp/
adb shell /data/local/tmp/repro_view_format srgb-image-unorm-view
```

## Results on the device above

Run on 2026-10-29, Android 15 build `AP3A.240905.015.A2`; full output in `results.txt`.

### Reproduces: item #5, attachment loaded/blended through the image format

| Case | Texel read back | Spec |
| --- | --- | --- |
| `control` (UNORM image, UNORM view, blended draw) | 128 128 128 128 | 128 |
| `srgb-image-unorm-view` (blended draw) | **55 55 55 128** | 128 |
| `unorm-image-srgb-view` (blended draw) | **188 188 188 128** | 128 |
| `srgb-image-unorm-view nodraw` (load + store only) | 128 128 128 128 | 128 |
| `unorm-image-srgb-view nodraw` (load + store only) | 128 128 128 128 | 128 |

With a draw whose blend reads the destination (`src = ZERO, dst = ONE`), the destination value
is decoded with the *image's* format and re-encoded with the *view's* format: 128 as sRGB is
0.216 linear, stored as UNORM 55; 128 as UNORM is 0.502, stored as sRGB 188. A pass that only
loads and stores keeps the bytes. Alpha is unaffected, as expected for sRGB formats. Deterministic.

### Not reproduced in isolation yet: items #3, #7, #8

These faults were observed in a large application (symbolised traces in the forum post). The
minimal programs here pass on the same device:

| Program | Variants run | Result |
| --- | --- | --- |
| `repro_unused_attachment` | no output / output to location 0, 1x and 2x MSAA | pipeline created |
| `repro_compute_cache` | same process, across two processes, cache also holding 40 graphics pipelines | pipelines created |
| `repro_concurrent_compile` | 4 threads x 60 and 8 threads x 150 pipelines, per-thread caches | all created |

So the triggering condition depends on something these programs do not have yet (shader
content, descriptor layouts or pipeline state of the original pipelines). They are kept as the
starting point for the reduction; this section will be updated when one of them faults.

## License

MIT, see `LICENSE`.

## Capture replay (work in progress)

`vkpc_replay` replays a `.vkpc` pipeline capture (device extensions and features, shader modules,
layouts, render passes, pipeline caches with seed data, every pipeline) without the application
that recorded it; `tools/reduce_shader.sh` drives `spirv-reduce` against the device with it. It
is how items #3, #7 and #8 are being reduced to application-free cases. Not yet exercised on a
real capture; treat it as experimental until a reduced case lands in the table above.
