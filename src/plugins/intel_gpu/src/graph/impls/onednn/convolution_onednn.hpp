// Copyright (C) 2022-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace cldnn {
namespace onednn {

static std::shared_ptr<dnnl::convolution_forward::desc> get_convolution_descriptor(const convolution_node& arg,
                                            dnnl::memory::format_tag tag_in_out = dnnl::memory::format_tag::undef) {
    auto prim = arg.get_primitive();

    auto& input = arg.get_dependency(0);
    auto& weights = arg.get_dependency(1);

    dnnl::memory::dims stride(prim->stride.begin(), prim->stride.end());
    dnnl::memory::dims dilation(prim->dilation.begin(), prim->dilation.end());
    dnnl::memory::dims pad_l(prim->pad.begin(), prim->pad.end());
    dnnl::memory::dims pad_r(prim->pad.begin(), prim->pad.end());

    auto input_md = onednn::layout_to_memory_desc(input.get_output_layout(), tag_in_out);
    auto weights_md = onednn::layout_to_memory_desc(weights.get_output_layout(), dnnl::memory::format_tag::any);
    auto output_md = onednn::layout_to_memory_desc(arg.get_output_layout(), tag_in_out);
    auto grouped_weights = format::is_grouped(weights.get_output_layout().format) || prim->grouped_weights_shape;

    // adjust_conv_dilation_pad(dilation, stride, pad_l, pad_r, input_md, output_md, weights_md, grouped_weights);
    for (size_t i = 0; i < dilation.size(); i++) {
        dilation[i]--;
        int weights_offset = (grouped_weights ? 3 : 2) + static_cast<int>(i);
        auto os = output_md.dims()[2 + i];
        auto is = input_md.dims()[2 + i];
        auto ks = weights_md.dims()[weights_offset];
        auto kernel_range = 1 + (ks - 1) * (dilation[i] + 1);
        pad_r[i] = (os - 1) * stride[i] - is + kernel_range - pad_l[i];
    }

    if (arg.bias_term()) {
        auto bias_md = onednn::layout_to_memory_desc(arg.get_dependency(2).get_output_layout(), dnnl::memory::format_tag::any, true);
        return std::make_shared<dnnl::convolution_forward::desc>(
            dnnl::prop_kind::forward_inference,
            dnnl::algorithm::convolution_direct,
            input_md,
            weights_md,
            bias_md,
            output_md,
            stride,
            dilation,
            pad_l,
            pad_r);
    } else {
        return std::make_shared<dnnl::convolution_forward::desc>(
            dnnl::prop_kind::forward_inference,
            dnnl::algorithm::convolution_direct,
            input_md,
            weights_md,
            output_md,
            stride,
            dilation,
            pad_l,
            pad_r);
    }
}
} // namespace onednn
} // namespace cldnn
