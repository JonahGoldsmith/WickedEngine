#ifndef WICKED_SUBSET_SHADER_COMPAT_HLSLI
#define WICKED_SUBSET_SHADER_COMPAT_HLSLI

#include "ShaderInterop.h"

#if defined(__hlsl_dx_compiler) && !defined(__spirv__) && !defined(WICKED_ENGINE_DEFAULT_ROOTSIGNATURE)
#define WICKED_ENGINE_DEFAULT_ROOTSIGNATURE \
    "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED), " \
    "RootConstants(num32BitConstants=12, b999), " \
    "CBV(b0), " \
    "CBV(b1), " \
    "CBV(b2), " \
    "DescriptorTable( " \
        "CBV(b3, numDescriptors = 11, flags = DATA_STATIC_WHILE_SET_AT_EXECUTE)," \
        "SRV(t0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE | DATA_STATIC_WHILE_SET_AT_EXECUTE)," \
        "UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE | DATA_STATIC_WHILE_SET_AT_EXECUTE)" \
    ")," \
    "DescriptorTable( " \
        "Sampler(s0, offset = 0, numDescriptors = 8, flags = DESCRIPTORS_VOLATILE)" \
    ")," \
    "StaticSampler(s100, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP, filter = FILTER_MIN_MAG_MIP_LINEAR)," \
    "StaticSampler(s101, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP, filter = FILTER_MIN_MAG_MIP_LINEAR)," \
    "StaticSampler(s102, addressU = TEXTURE_ADDRESS_MIRROR, addressV = TEXTURE_ADDRESS_MIRROR, addressW = TEXTURE_ADDRESS_MIRROR, filter = FILTER_MIN_MAG_MIP_LINEAR)," \
    "StaticSampler(s103, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP, filter = FILTER_MIN_MAG_MIP_POINT)," \
    "StaticSampler(s104, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP, filter = FILTER_MIN_MAG_MIP_POINT)," \
    "StaticSampler(s105, addressU = TEXTURE_ADDRESS_MIRROR, addressV = TEXTURE_ADDRESS_MIRROR, addressW = TEXTURE_ADDRESS_MIRROR, filter = FILTER_MIN_MAG_MIP_POINT)," \
    "StaticSampler(s106, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP, filter = FILTER_ANISOTROPIC, maxAnisotropy = 16)," \
    "StaticSampler(s107, addressU = TEXTURE_ADDRESS_WRAP, addressV = TEXTURE_ADDRESS_WRAP, addressW = TEXTURE_ADDRESS_WRAP, filter = FILTER_ANISOTROPIC, maxAnisotropy = 16)," \
    "StaticSampler(s108, addressU = TEXTURE_ADDRESS_MIRROR, addressV = TEXTURE_ADDRESS_MIRROR, addressW = TEXTURE_ADDRESS_MIRROR, filter = FILTER_ANISOTROPIC, maxAnisotropy = 16)," \
    "StaticSampler(s109, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP, filter = FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, comparisonFunc = COMPARISON_GREATER_EQUAL),"
#endif

#define WICKED_SUBSET_PUSHCONSTANT(name, type) PUSHCONSTANT(name, type)

#if !defined(__spirv__) && !defined(__PSSL__)
#define WICKED_SUBSET_ROOT_SIGNATURE_DEFAULT [RootSignature(WICKED_ENGINE_DEFAULT_ROOTSIGNATURE)]
#else
#define WICKED_SUBSET_ROOT_SIGNATURE_DEFAULT
#endif

#endif
