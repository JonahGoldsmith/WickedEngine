#pragma once
#include "CommonInclude.h"
#include "wiGraphicsDevice.h"
#include "wiScene_Decl.h"

namespace wi
{
	struct GPUBVH
	{
		// Scene BVH intersection resources:
		wi::GPUBuffer bvhNodeBuffer;
		wi::GPUBuffer bvhParentBuffer;
		wi::GPUBuffer bvhFlagBuffer;
		wi::GPUBuffer primitiveCounterBuffer;
		wi::GPUBuffer primitiveIDBuffer;
		wi::GPUBuffer primitiveBuffer;
		wi::GPUBuffer primitiveMortonBuffer;
		uint32_t primitiveCapacity = 0;
		bool IsValid() const { return primitiveCounterBuffer.IsValid(); }

		void Update(const wi::scene::Scene& scene);
		void Build(const wi::scene::Scene& scene, wi::CommandList cmd) const;

		void Clear();

		static void Initialize();
	};
}
