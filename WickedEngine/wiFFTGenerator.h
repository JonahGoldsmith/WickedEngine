#pragma once
#include "CommonInclude.h"
#include "wiGraphicsDevice.h"

namespace wi::fftgenerator
{
	void fft_512x512_c2c(
		const wi::GPUResource& pUAV_Dst,
		const wi::GPUResource& pSRV_Dst,
		const wi::GPUResource& pSRV_Src,
		wi::CommandList cmd);

	void LoadShaders();
}
