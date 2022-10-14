#pragma once
#ifndef H2_INCLUDE_H2_GPU_CUDA_MEMORY_UTILS_HPP_INCLUDED
#define H2_INCLUDE_H2_GPU_CUDA_MEMORY_UTILS_HPP_INCLUDED

/** @file
 *
 *  Thin wrappers around cudaMem{cpy,set} functions. These are here so
 *  they can be inlined if possible.
 */

#include "h2/gpu/logger.hpp"
#include "h2/gpu/runtime.hpp"
#include "h2_config.hpp"

#include <cuda_runtime.h>

namespace cub
{
struct CachingDeviceAllocator;
} // namespace cub

namespace h2
{
namespace gpu
{

inline MemInfo mem_info()
{
    MemInfo info;
    H2_CHECK_CUDA(cudaMemGetInfo(&info.free, &info.total));
    return info;
}

using RawCUBAllocType = cub::CachingDeviceAllocator;

inline void mem_copy(void* dst, void const* src, size_t bytes)
{
    H2_GPU_INFO("cudaMemcpy(dst={}, src={}, bytes={}, kind=cudaMemcpyDefault)",
                dst,
                src,
                bytes);
    H2_CHECK_CUDA(cudaMemcpy(dst, src, bytes, cudaMemcpyDefault));
}

inline void
mem_copy(void* dst, void const* src, size_t bytes, DeviceStream stream)
{
    H2_GPU_INFO("cudaMemcpyAsync(dst={}, src={}, bytes={}, "
                "kind=cudaMemcpyDefault, stream={})",
                dst,
                src,
                bytes,
                (void*) stream);
    H2_CHECK_CUDA(cudaMemcpyAsync(dst, src, bytes, cudaMemcpyDefault, stream));
}

inline void mem_zero(void* mem, size_t bytes)
{
    H2_GPU_INFO("cudaMemset(mem={}, value=0x0, bytes={})", mem, bytes);
    H2_CHECK_CUDA(cudaMemset(mem, 0x0, bytes));
}

inline void mem_zero(void* mem, size_t bytes, DeviceStream stream)
{
    H2_GPU_INFO("cudaMemsetAsync(mem={}, value=0x0, bytes={}, stream={})",
                mem,
                bytes,
                (void*) stream);
    H2_CHECK_CUDA(cudaMemsetAsync(mem, 0x0, bytes, stream));
}

} // namespace gpu
} // namespace h2
#endif // H2_INCLUDE_H2_GPU_CUDA_MEMORY_UTILS_HPP_INCLUDED
