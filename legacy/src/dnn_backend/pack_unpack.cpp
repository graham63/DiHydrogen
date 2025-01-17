#include "distconv/dnn_backend/backend.hpp"
#include "h2/gpu/logger.hpp"

#include <numeric>
#include <stdexcept>
#include <variant>
#include <vector>

#if H2_HAS_CUDA
#define H2_DNN_BACKEND_NS cudnn
#elif H2_HAS_ROCM
#define H2_DNN_BACKEND_NS miopen
namespace distconv
{
namespace miopen
{
void do_gpu_tensor_repack(
    float const& alpha,
    float const& beta,
    size_t const ndims,
    int const* dims,
    int const* src_strides,
    int const* tgt_strides,
    float const* src_data,
    float* tgt_data,
    hipStream_t stream);
} // namespace miopen
} // namespace distconv
#endif

namespace distconv
{
namespace H2_DNN_BACKEND_NS
{
namespace
{

// The behavior we should have is to just be able to shove whatever
// (valid) tensor we want through these interfaces. HOWEVER, doing
// this on ROCm platfmorms means accepting incorrect results, and this
// is not acceptible. The default behavior, therefore, is to "opt-in"
// on CUDA platforms and "opt-out" on ROCm platforms. In the code
// below, explicitly setting the environment variable will use the
// truthiness of the variable's value to determine whether to
// pack/unpack or just pass tensors through. Leaving the variable
// unset will pass tensors through on non-ROCm platforms and will
// pack/unpack on ROCm platforms.
bool do_pack_unpack() noexcept
{
    static bool const val = []() {
#if H2_HAS_ROCM
        bool tf = true;
#else
        bool tf = false;
#endif
        char const* env = std::getenv("H2_DISTCONV_FORCE_PACKED");
        if (env)
            tf = (env && std::strlen(env) && env[0] != '0');
        // Any nonempty string matching "[^0].*" is truthy.
        H2_GPU_DEBUG("Doing pack/unpack: {}", tf);
        return tf;
    } ();

    return val;
}

// This is just a quick wrapper around the "alpha"/"beta" scaling
// parameters needed for the cuDNN/MIOpen interface.
struct host_scalar
{
    std::variant<float, double> val;
    explicit host_scalar(float const v)
        : val{v}
    {}
    explicit host_scalar(double const v)
        : val{v}
    {}
    void const* get() const
    {
        return std::visit([](auto&& x)
        {
            return static_cast<void const*>(&x);
        },
            val);
    }
    operator void const* () const { return get(); }
};// host_scalar

host_scalar make_host_scalar(DataType_t const dt, double const v)
{
#if H2_HAS_CUDA
    switch (dt)
    {
    case CUDNN_DATA_FLOAT: [[fallthrough]];
    case CUDNN_DATA_HALF:
        return host_scalar{static_cast<float>(v)};
    case CUDNN_DATA_DOUBLE:
        return host_scalar{v};
    default:
        throw std::runtime_error(
            "Only float, double, and half are supported.");
    }
#elif H2_HAS_ROCM
    switch (dt)
    {
    case miopenFloat: [[fallthrough]];
    case miopenHalf:
        return host_scalar{static_cast<float>(v)};
    default:
        throw std::runtime_error("Only float and half are supported.");
    }
#endif
}

size_t datatype_size(DataType_t dt)
{
#if H2_HAS_CUDA
    switch (dt) {
    case CUDNN_DATA_FLOAT: return sizeof(float);
    case CUDNN_DATA_DOUBLE: return sizeof(double);
    case CUDNN_DATA_HALF: return sizeof(short);
    default:
        throw std::runtime_error(
            "Only float, double, and half are supported.");
    }
#elif H2_HAS_ROCM
    switch (dt)
    {
    case miopenHalf: return sizeof(short);
    case miopenFloat: return sizeof(float);
    default:
        throw std::runtime_error("Only float and half are supported.");
    }
#endif
    return 1UL;
}

bool is_fully_packed(std::vector<int> const& dims,
                     std::vector<int> const& strides)
{
    // As far as I know, LBANN doesn't do any overlapping striding
    // (this is exceptionally poorly supported in the real world and
    // it has semantic issues). Thus, a tensor is fully packed if and
    // only if strides[0] == prod(dims[1:]).
    return strides.front() == std::accumulate(std::next(dims.cbegin()),
                                              dims.cend(),
                                              1,
                                              std::multiplies<int>{});
}

std::vector<int> get_fully_packed_strides(std::vector<int> const& dims)
{
    size_t const ndims = dims.size();
    std::vector<int> strides(ndims, 1);
    std::partial_sum(dims.rbegin(),
                     std::prev(dims.rend()),
                     std::next(strides.rbegin()),
                     std::multiplies<int>{});
    return strides;
}

// std::tuple<cudnnDataType_t, std::vector<int>, std::vector<int>>
// but with nice names.
struct MyTensorDesc
{
    DataType_t dt;
    std::vector<int> dims;
    std::vector<int> strides;
    void set_ndims(size_t ndims)
    {
        dims.resize(ndims);
        strides.resize(ndims);
    }
    size_t memory_size() const
    {
        assert_eq(dims.size(), strides.size());
        assert_always(dims.size() > 0);
        return dims[0] * strides[0] * datatype_size(dt);
    }
};

MyTensorDesc get_details(TensorDescriptor_t desc)
{
    int ndims = get_tensor_num_dimensions(desc);
    DataType_t dt;
    std::vector<int> dims(ndims), strides(ndims);
#if H2_HAS_CUDA
    DISTCONV_CHECK_CUDNN(
        cudnnGetTensorNdDescriptor(
            desc, ndims, &dt, &ndims, dims.data(), strides.data()));
#elif H2_HAS_ROCM
    DISTCONV_CHECK_MIOPEN(
        miopenGetTensorDescriptor(
            desc, &dt, dims.data(), strides.data()));
#endif
    return {dt, std::move(dims), std::move(strides)};
};

DataType_t get_data_type(TensorDescriptor_t desc)
{
#if H2_HAS_CUDA
    DataType_t dt;
    int dim=-1, stride=-1, ndims=1;
    DISTCONV_CHECK_CUDNN(
        cudnnGetTensorNdDescriptor(
            desc, ndims, &dt, &ndims, &dim, &stride));
    return dt;
#elif H2_HAS_ROCM
    return get_details(desc).dt; // ugh
#endif
}

TensorDescriptor_t make_backend_desc(MyTensorDesc my_desc)
{
    auto desc = make_tensor_descriptor();
#if H2_HAS_CUDA
    auto const& [dt, dims, strides] = my_desc;
    DISTCONV_CHECK_CUDNN(
        cudnnSetTensorNdDescriptor(
            desc, dt, dims.size(), dims.data(), strides.data()));
#elif H2_HAS_ROCM
    auto& [dt, dims, strides] = my_desc;
    DISTCONV_CHECK_MIOPEN(
        miopenSetTensorDescriptor(
            desc, dt, dims.size(), dims.data(), strides.data()));
#endif
    return desc;
}

// If the input tensor descriptor is already packed, then return it
// directly. Otherwise, create a new handle and set it up with the
// same dimensions but fully packed strides.
TensorDescriptor_t get_packed_desc(TensorDescriptor_t desc)
{
    auto const [dt, dims, strides] = get_details(desc);
    if (is_fully_packed(dims, strides))
        return desc;
    else
        return make_backend_desc({dt, dims, get_fully_packed_strides(dims)});
}

struct MyTypeErasedPtr
{
    void* data;
    DataType_t dt;
    template <typename T, typename U>
    operator std::tuple<T,U>() { return {data, dt}; }
};

MyTypeErasedPtr allocate(Handle_t handle, TensorDescriptor_t desc)
{
    auto const [dt, dims, strides] = get_details(desc);
    auto const mem_size = dims[0] * strides[0] * datatype_size(dt);

    // Stream-aware allocation
    void* data;
    DISTCONV_CHECK_GPU(
        h2::gpu::default_cub_allocator().DeviceAllocate(
            &data,
            mem_size,
            get_stream(handle)));
    return {data, dt};
}

void copy_tensor(
    Handle_t handle,
    host_scalar const& alpha,
    TensorDescriptor_t src_desc,
    void const* src_data,
    host_scalar const& beta,
    TensorDescriptor_t tgt_desc,
    void* tgt_data)
{
#if H2_HAS_CUDA
    DISTCONV_CHECK_CUDNN(
        cudnnTransformTensor(handle,
                             alpha,
                             src_desc,
                             src_data,
                             beta,
                             tgt_desc,
                             tgt_data));
#elif H2_HAS_ROCM
    hipStream_t const stream = get_stream(handle);
    auto const [src_dt, src_dims, src_strides] = get_details(src_desc);
    auto const [tgt_dt, tgt_dims, tgt_strides] = get_details(tgt_desc);
    assert_always(src_dt == tgt_dt);
    assert_always(src_dims == tgt_dims);
    switch (src_dt)
    {
    case miopenFloat:
        do_gpu_tensor_repack(
            *reinterpret_cast<float const*>(alpha.get()),
            *reinterpret_cast<float const*>(beta.get()),
            src_dims.size(),
            src_dims.data(),
            src_strides.data(),
            tgt_strides.data(),
            reinterpret_cast<float const*>(src_data),
            reinterpret_cast<float*>(tgt_data),
            stream);
        break;
    default:
        throw std::runtime_error("Only float.");
    }
#endif
}

}// namespace

// Read proxy impl

PackedTensorReadProxy::PackedTensorReadProxy(TensorDescriptor_t unpacked_desc,
                                             bool const force)
    : m_unpacked_desc{unpacked_desc},
      m_packed_desc{unpacked_desc},
      m_unpacked_data{nullptr},
      m_packed_data{nullptr}
{
    if (force || do_pack_unpack())
        m_packed_desc = get_packed_desc(m_unpacked_desc);
}

PackedTensorReadProxy::PackedTensorReadProxy(Handle_t handle,
                                             TensorDescriptor_t unpacked_desc,
                                             void const* unpacked_data,
                                             bool const force)
    : m_unpacked_desc{unpacked_desc},
      m_packed_desc{unpacked_desc},
      m_unpacked_data{unpacked_data},
      m_packed_data{nullptr}
{
    if (force || do_pack_unpack())
        m_packed_desc = get_packed_desc(m_unpacked_desc);

    if (m_unpacked_desc == m_packed_desc)
        m_packed_data = const_cast<void*>(m_unpacked_data);
    else
    {
        DataType_t dt;
        std::tie(m_packed_data, dt) = allocate(handle, m_packed_desc);
        copy_tensor(handle,
                    make_host_scalar(dt, 1.0),
                    m_unpacked_desc,
                    m_unpacked_data,
                    make_host_scalar(dt, 0.0),
                    m_packed_desc,
                    m_packed_data);
    }
}

PackedTensorReadProxy::~PackedTensorReadProxy()
{
    if ((m_packed_data != m_unpacked_data)
        && m_packed_data)
    {
        DISTCONV_CHECK_GPU(
            h2::gpu::default_cub_allocator().DeviceFree(m_packed_data));
        m_packed_data = nullptr;
        m_unpacked_data = nullptr;
    }
    if (m_unpacked_desc != m_packed_desc)
        destroy_tensor_descriptor(m_packed_desc);
    m_packed_desc = 0;
    m_unpacked_desc = 0;
}

// Write proxy -- possibly copy in/copy out

PackedTensorWriteProxy::PackedTensorWriteProxy(TensorDescriptor_t unpacked_desc,
                                               bool const force)
    : m_unpacked_desc{unpacked_desc},
      m_packed_desc{unpacked_desc},
      m_unpacked_data{nullptr},
      m_packed_data{nullptr}
{
    if (force || do_pack_unpack())
        m_packed_desc = get_packed_desc(unpacked_desc);
}

PackedTensorWriteProxy::PackedTensorWriteProxy(Handle_t handle,
                                               TensorDescriptor_t unpacked_desc,
                                               void* unpacked_data,
                                               double beta,
                                               bool const force)
    : m_unpacked_desc{unpacked_desc},
      m_packed_desc{unpacked_desc},
      m_unpacked_data{unpacked_data},
      m_packed_data{nullptr},
      m_handle{handle}
{
    if (force || do_pack_unpack())
        m_packed_desc = get_packed_desc(unpacked_desc);

    // When "unpacked" == "packed", we don't actually need dt, so we
    // leave it as the default.
    if (m_unpacked_desc == m_packed_desc)
        m_packed_data = m_unpacked_data;
    else
    {
        std::tie(m_packed_data, m_dt) = allocate(m_handle, m_packed_desc);

        if (beta != 0.)
        {
            copy_tensor(m_handle,
                        make_host_scalar(m_dt, 1.0),
                        m_unpacked_desc,
                        m_unpacked_data,
                        make_host_scalar(m_dt, 0.0),
                        m_packed_desc,
                        m_packed_data);
        }
    }
}

// This is a "special" dtor because it can throw (and more
// importantly, semantically, it should be able to throw!). If we
// "unrolled" the code, this class replaces a pattern something like:
//
// x = make_writeable_proxy(unpacked_tensor);
// do_write_stuff(x);
// copy(x, unpacked_tensor)
//
// and we wouldn't terminate just because "copy(x, unpacked_tensor)"
// threw... It'd just be a normal exception that someone else could
// catch.
//
// However, the C++ rule is that a dtor cannot throw an exception
// while unwinding the stack to handle another exception -- such
// behavior guarantees an std::terminate. We can check for this,
// though, with std::uncaught_exceptions().
PackedTensorWriteProxy::~PackedTensorWriteProxy()
{
    if ((m_unpacked_data != m_packed_data)
        && m_packed_data)
    {
        if (!std::uncaught_exceptions())
        {
            copy_tensor(m_handle,
                        make_host_scalar(m_dt, 1.0),
                        m_packed_desc,
                        m_packed_data,
                        make_host_scalar(m_dt, 0.0),
                        m_unpacked_desc,
                        m_unpacked_data);
        }
        DISTCONV_CHECK_GPU(h2::gpu::default_cub_allocator().DeviceFree(m_packed_data));
        m_packed_data = nullptr;
        m_unpacked_data = nullptr;
    }
    if (m_unpacked_desc != m_packed_desc)
        destroy_tensor_descriptor(m_packed_desc);
    m_packed_desc = 0;
    m_unpacked_desc = 0;
}
}// namespace details
}// namespace distconv
