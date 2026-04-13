# Visibility Pipeline Benchmark: Render Path Design

This document describes the benchmark renderer architecture in:
- `Samples/WickedGraphicsSubset/src/wicked_subset_visibility_pipeline_benchmark.cpp`
- `Samples/WickedGraphicsSubset/src/shaders/wicked_subset_visibility_pipeline_benchmark.hlsl`

Goal: benchmark three modern GPU-driven visibility paths with the same scene generator and camera tooling, while keeping path-specific logic intentionally different.

## Shared GPU Culling Backbone (Step 2)

All non-TVB paths now share a common culling core inspired by GeometryFX-style GPU workflows:

1. Instance culling (`cs_instance_filter`)
- Frustum sphere cull per instance.
- Writes per-instance visibility bits.

2. Cluster culling (`cs_cluster_filter`)
- Reads instance visibility.
- Sphere/frustum cull per cluster.
- Optional Hi-Z occlusion rejection.
- Appends surviving cluster command indices into a compact visible list.

3. Visible args compaction (`cs_compact_visible_args`)
- Builds tightly packed indirect draw args from the visible command list.
- Produces a draw-ready argument stream and visible command count.

4. Hi-Z depth pyramid build
- `cs_hiz_init` copies current frame depth into mip 0.
- `cs_hiz_downsample` builds a min-depth pyramid for conservative occlusion.
- Pyramid is consumed by cluster culling on subsequent frames.

5. Async compute execution
- Culling can run on `QUEUE_COMPUTE` while draw/present stay on graphics.
- Graphics queue waits on the compute cull command list before draw submission.
- Runtime toggle: `J` key.

## Render Path Definitions (Step 3)

The benchmark now uses intentionally distinct behavior for each path:

### 1) Wicked path
Pipeline intent:
- Clustered indexed raster path without TVB triangle stream generation.

Flow:
- Instance cull -> cluster cull (sphere + Hi-Z) -> compact visible args.
- Draw only compacted visible clusters via indexed indirect count.
- In mesh mode, uses visible cluster list to skip hidden work.

### 2) TVB path
Pipeline intent:
- Classic triangle visibility buffer path over active command set.

Flow:
- Runs TVB triangle filtering over active commands (no cluster compaction stage).
- Raster uses TVB-filtered index stream and primitive-id stream.

### 3) Esoterica path
Pipeline intent:
- Cluster-first visibility reduction, then TVB refinement.

Flow:
- Instance cull -> cluster cull (sphere + Hi-Z) -> visible list compaction.
- Portable mode: TVB triangle filtering runs only on visible clusters.
- Mesh mode: dispatches from visible cluster list.

## Why this architecture is strong for engine growth

- Shared infrastructure, distinct policies:
  - One reusable GPU culling backbone lowers maintenance cost.
  - Each path still has unique algorithmic behavior for fair benchmarking.

- Future-friendly:
  - Works in portable (compute+indexed) and mesh paths.
  - Scales from low-end compatibility to high-end mesh-shader hardware.

- Data-oriented pipeline:
  - Compact visible lists and indirect args are ideal building blocks for later material/lighting passes.

## Queue Sync and Submission Strategy (Future Work)

Current benchmark already supports async-compute culling with a graphics wait point before draw. To reduce CPU bottlenecks further and prepare for full frame-buffered renderer architecture:

1. Move to frame-buffered submission timeline
- Keep N frames in flight.
- Decouple CPU scene/cull prep from GPU frame completion.
- Avoid per-submit queue-to-queue hard sync unless resource hazard requires it.

2. Use explicit async compute overlap
- Run culling/compaction/Hi-Z on async compute queue where backend allows.
- Signal/wait only at pass boundaries that feed graphics draws.

3. Replace global waits with per-resource fences/barriers
- Prefer narrow synchronization scopes over whole-queue synchronization.
- Keep indirect args/count buffers in ring allocations per frame-in-flight.

4. Threaded command recording
- Record cull, draw, and debug/present command lists on worker threads.
- Merge and submit batched lists once per frame.

5. Benchmark both correctness and overlap
- Track CPU submission time, GPU busy time, async overlap %, and queue idle gaps.

This staged approach gives you a practical path to a highly competitive renderer architecture while preserving benchmark clarity.
