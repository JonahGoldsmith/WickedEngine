# Vulkan Portability Future Plan

## Goal
Upgrade Wicked Vulkan initialization and descriptor handling so Vulkan 1.3 remains required while capability handling is portable across platforms, especially:

- Apple + MoltenVK (strict limits, portability extensions required)
- Android/Linux/BSD/Windows (often higher limits, should keep full capability paths when available)

This document captures what was already changed and what should be implemented next.

## Current Status (Implemented)

### Vulkan init and portability policy
- Vulkan API version hard-required at 1.3.
- Non-Windows instance extension discovery uses SDL Vulkan extension query.
- Apple portability requirements are enabled:
  - `VK_KHR_portability_enumeration` at instance creation
  - `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` set when enabled
  - `VK_KHR_portability_subset` required on device
- Vulkan portability fallback defines are present for older headers:
  - `VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME`
  - `VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME`
  - `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`

### Device suitability and feature policy
- Device selection rejects adapters without `dynamicRendering` (required).
- `geometryShader` moved to optional warning (not fatal assert).
- Descriptor indexing/bindless required features are validated with explicit error handling.

### VMA / memory / allocator stability
- VMA allocator now sets `vulkanApiVersion = VK_API_VERSION_1_3`.
- VMA allocator flags preserve dedicated+bind_memory2 and OR-in buffer device address.
- Null typed buffer view size fixed to avoid Metal-side zero-size texture assertions.

### Bindless robustness
- Bindless descriptor heap allocation is adaptive (backs off capacity on allocation failures).
- Global bindless budget uses `maxUpdateAfterBindDescriptorsInAllPools` and reduces capacities as needed.

### Sample backend choice
- `EngineConfig.h` includes subset backend selection:
  - `0 = Vulkan`
  - `1 = DX12`
  - `2 = Metal`
  - `-1 = Auto`

## Remaining Problem
On MoltenVK, `vkCreatePipelineLayout` validation still reports:

- Per-stage sampler count exceeded (`18 > 16`)
- Per-stage storage-buffer count exceeded (`32 > 31`)

Pipeline creation still succeeds, but this indicates descriptor layout policy is not yet device-limit-aware enough.

## Root Cause
The current Vulkan set-0 binding layout is effectively fixed-width and uses broad stage visibility:

- Samplers:
  - runtime sampler table (8)
  - immutable static samplers (10)
  - total = 18
- Storage buffers:
  - SRV table defaulted to storage buffer types (16)
  - UAV table defaulted to storage buffer types (16)
  - total = 32
- Many bindings use `VK_SHADER_STAGE_ALL`

This inflates per-stage descriptor counts on strict drivers (MoltenVK), even for shaders that do not need all bindings.

## Portability Upgrade Plan (Phased)

### Phase 1: Introduce explicit Vulkan descriptor capability policy
Create a runtime policy struct (example: `VulkanDescriptorPolicy`) populated from queried features+limits:

- `maxPerStageDescriptorSamplers`
- `maxPerStageDescriptorStorageBuffers`
- `maxPerStageDescriptorSampledImages`
- `maxPerStageDescriptorStorageImages`
- portability flags (Apple portability extension presence, etc.)
- bindless tier support flags

Policy should be capability-driven, not platform-ifdef driven.

### Phase 2: Build compact set-0 layouts from reflection
Current shader reflection only drives SRV/UAV descriptor types in set 0. Extend this to include:

- CBV usage
- sampler usage (runtime and static sampler references)
- per-binding stage usage

Then build set-0 `VkDescriptorSetLayout` with only active bindings for each pipeline layout.

This should remove unnecessary descriptor pressure and fix most MoltenVK per-stage limit warnings.

### Phase 3: Use precise stage flags (not `VK_SHADER_STAGE_ALL`)
Assign stage visibility per descriptor binding from reflected shader usage.

This is required for accurate per-stage limit compliance and better portability behavior.

### Phase 4: Update descriptor writes for compact layouts
In descriptor binder flush path:

- write only active bindings for the current layout
- do not rely on fixed-size write arrays as if all slots are always present
- keep null descriptor safety only for bindings that exist in that compact layout

### Phase 5: Extend pipeline layout cache hash
Include in layout hash:

- active binding masks
- descriptor types
- stage flags
- static sampler usage mask

This avoids cache aliasing between full and compact layout variants.

### Phase 6: Add fallback tiers for constrained devices
If compact layouts alone are not enough on strict devices:

- Tier A: compact layout only (default for portable/strict devices)
- Tier B: portable sampler mode (reduce static sampler exposure to only used entries)
- Tier C: shaderinterop reduced-budget mode (smaller SRV/UAV/SAM table counts via compile-time defines)

Full-capability devices keep existing broad behavior when limits allow.

### Phase 7: EngineConfig/runtime knobs for testing
Add policy toggles:

- `AUTO` (default): choose policy from device capabilities
- `FULL`: force legacy full descriptor profile
- `PORTABLE`: force compact/strict profile

This enables reliable A/B testing across MoltenVK, Android, and desktop Vulkan drivers.

### Phase 8: Validation and regression tests
Required checks:

- macOS + MoltenVK:
  - no sampler/storage-buffer per-stage overflow VUIDs at pipeline layout creation
- Windows/Linux/Android:
  - no regressions in existing rendering features
- negative-case portability tests:
  - deterministic failure when required Vulkan 1.3 or required portability pieces are unavailable

## Implementation Priority
Highest impact, lowest risk path:

1. Phase 1
2. Phase 2
3. Phase 3
4. Phase 4

If those are done, the current MoltenVK warnings should be resolved without forcing reduced limits on Android/desktop.

## Notes
- Keep Vulkan 1.3 hard requirement.
- Keep optional advanced features optional (RT/mesh/VRS/video capability-driven).
- Preserve SDL-first non-Windows WSI behavior.
