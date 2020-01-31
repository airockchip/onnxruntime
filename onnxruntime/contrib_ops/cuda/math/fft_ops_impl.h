// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once
#include "core/providers/cuda/cuda_common.h"
#include "fft_ops.h"

namespace onnxruntime {
namespace contrib {
namespace cuda {

template <typename T>
void PostProcess(const std::vector<int64_t>& signal_dims, Tensor* Y, T* output_data);

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
