# Metal True MultiDrawIndirect (ICB) Notes

This document captures how `DrawInstancedIndirectCount` / `DrawIndexedInstancedIndirectCount`
were implemented on Wicked Engine Metal backend using GPU-encoded ICBs (not CPU draw loops),
plus the demo-side setup used to validate behavior across Vulkan, DX12, and Metal.

## Goals and constraints

Implemented goals:
- Real Metal indirect-count multidraw path using `MTL::IndirectCommandBuffer`.
- Preserve existing Wicked indirect argument/count ABI from `ShaderInterop.h`.
- Keep public graphics API unchanged.

Intentional constraints:
- No CPU fallback path for this feature path.
- Phase 1 scope: non-mesh draw and draw-indexed count paths.
- Mesh-indirect-count is deferred.

## ABI compatibility (preserved)

No ABI changes were made for:
- `IndirectDrawArgsInstanced`
- `IndirectDrawArgsIndexedInstanced`
- count buffer layout (`uint32_t` draw count)

These are still consumed as defined in:
- `WickedEngine/shaders/ShaderInterop.h`

## Metal backend architecture

Primary implementation file:
- `WickedEngine/wiGraphicsDevice_Metal.cpp`

Main internal pieces added:
- Runtime-compiled MSL kernels for ICB command encoding.
- Per-command-list ICB resource cache for:
  - non-indexed count draws
  - indexed count draws
- Render-pass split/resume helpers around the compute-encode step.

Key backend entry points:
- `EnsureDrawCountICBEncoder()`
- `EnsureDrawCountICBResources()`
- `EndRenderPassForIndirectEncoding()`
- `ResumeRenderPassAfterIndirectEncoding()`
- `DrawInstancedIndirectCount()`
- `DrawIndexedInstancedIndirectCount()`

## GPU encoding path (how true MDI is produced)

Internal compute kernels:
- `wicked_encode_draw_indirect_count_icb`
- `wicked_encode_draw_indexed_indirect_count_icb`

For each command index `i`:
1. Read `countBuffer[0]`.
2. Clamp to `drawCount = min(countBuffer[0], maxCount)`.
3. If `i < drawCount`, encode draw command from args buffer entry `i`.
4. If `i >= drawCount`, call `cmd.reset()` so stale prior-frame commands do not execute.

Execution:
- After encode, render encoder executes ICB via `executeCommandsInBuffer(...)`.
- Execution is chunked at 16,384 commands per call to stay within Metal range limits.

## Render pass handling

Why split pass:
- ICB build is done in compute.
- Active render pass must be ended before switching to compute encoder in this path.

What happens:
1. Save active PSO state.
2. End active render encoder safely.
3. Run compute encode kernel.
4. Resume the same render pass with `LOAD` semantics.
5. Restore/validate state and execute ICB commands.

Guardrails:
- Fail-fast on missing bound pipeline.
- Fail-fast if geometry-shader emulation path is active (unsupported here).
- Fail-fast if active pass cannot be safely resumed.

## Critical fixes that made it stable

These were the most important fixes after initial crashes / one-cube / zero-cube regressions:

1. Correct ICB container argument binding in MSL:
- Changed from device pointer style to:
  - `constant ICBContainer& icbContainer [[buffer(26)]]`
- This matches command-buffer handle usage expectations for this argument-buffer form.

2. Always encode per-command draw params in ICB count path:
- `params.needsDrawParams = 1` for both indexed and non-indexed count kernels.
- Reason: reflection-derived `needs_draw_params` could under-report for converted variants,
  causing command-local start/base semantics to collapse.

3. Always rebind draw-type uniform after render-pass resume:
- Non-indexed path rebinds `kIRNonIndexedDraw`.
- Indexed path rebinds `IRMetalIndexToIRIndex(index_type)`.
- Prevents stale/missing draw-type state after pass split/resume.

4. Explicit command reset beyond active draw count:
- Required so previously encoded commands are not accidentally re-executed when count drops.

5. Deterministic chunked execute:
- `METAL_ICB_EXECUTION_CHUNK = 16384` and execute loop over full `maxCount`.

## Demo-side validation setup

Validation demo files:
- `Samples/WickedGraphicsSubset/src/wicked_subset_cube_indirect_compare_demo.cpp`
- `Samples/WickedGraphicsSubset/src/shaders/wicked_subset_cube_indirect_compare_demo.hlsl`

Current demo model:
- Cubes are pre-baked into one large vertex buffer (`36` vertices per cube).
- Per-command args for each cube:
  - `VertexCountPerInstance = 36`
  - `InstanceCount = 1`
  - `StartVertexLocation = cubeIndex * 36`
  - `StartInstanceLocation = 0`

Modes:
- `Draw`: CPU loop `Draw(36, i * 36)`.
- `DrawIndirect`: CPU loop `DrawInstancedIndirect(args, offset_i)`.
- `DrawIndirectCount`: single `DrawInstancedIndirectCount(args, count, max_count=visibleCubeCount_)`.

Shader path:
- Simple vertex input (`POSITION`, `COLOR`), no push-constant cube indexing dependency.
- This keeps validation centered on indirect argument correctness.

## Known limitations

- Mesh-indirect-count is not implemented in this phase.
- Geometry-shader emulation + indirect-count ICB path is explicitly unsupported.
- This note describes subset-demo validation path, not full renderer feature matrix.

## Troubleshooting checklist

If Metal path appears incorrect:
- Verify backend selection is Metal (`WICKED_ENGINECONFIG_SUBSET_BACKEND=2`).
- Rebuild the sample target after backend changes.
- Run with Metal validation enabled:
  - `MTL_DEBUG_LAYER=1`
  - `MTL_SHADER_VALIDATION=1`
  - `METAL_DEVICE_WRAPPER_TYPE=1`
- Confirm the mode summary logs cycle through:
  - Draw
  - DrawIndirect
  - DrawIndirectCount

If backend appears stuck on Vulkan, reconfigure:

```bash
cmake -S /Volumes/Dec\ 2025/WickedEngineClone \
      -B /Volumes/Dec\ 2025/WickedEngineClone/cmake-build-debug \
      -DWICKED_ENGINECONFIG_SUBSET_BACKEND=2
```

Then rebuild:

```bash
cmake --build /Volumes/Dec\ 2025/WickedEngineClone/cmake-build-debug \
      --target wicked_subset_cube_indirect_compare_demo -j8
```
