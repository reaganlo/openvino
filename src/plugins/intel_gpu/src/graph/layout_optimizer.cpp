// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "layout_optimizer.h"
#include "primitive_inst.h"
#include "program_helpers.h"
#include "intel_gpu/runtime/error_handler.hpp"
#include "intel_gpu/runtime/debug_configuration.hpp"
#include "data_inst.h"
#include "reorder_inst.h"
#include "resample_inst.h"
#include "reshape_inst.h"
#include "generic_layer.hpp"
#include <sstream>

#include "gemm_inst.h"
#include "eltwise_inst.h"
#include "pooling_inst.h"
#include "reduce_inst.h"
#include "one_hot_inst.h"
#include "permute_inst.h"
#include "gemm_inst.h"
#include "quantize_inst.h"
#include "mvn_inst.h"
#include "depth_to_space_inst.h"
#include "region_yolo_inst.h"
#include <vector>
#include <memory>
#include <utility>

using namespace cldnn;

static size_t get_post_ops_count(const program_node& node) {
    size_t onednn_post_ops_count = 0;
    for (auto& fo : node.get_fused_primitives()) {
        if (fo.is_type<activation>() || fo.is_type<eltwise>()) {
            onednn_post_ops_count++;
        } else if (fo.is_type<quantize>()) {
            // pre-scale, pre-shift
            const auto& q_param = fo.get_typed_fuse_params<kernel_selector::quantize_fuse_params>();
            if (q_param->per_tensor_input_scale && q_param->per_tensor_input_shift) {
                onednn_post_ops_count++;
            } else {
                onednn_post_ops_count += 2;
            }

            // post-scale, post-shift
            if (q_param->has_post_scale && q_param->has_post_shift && q_param->per_tensor_output_scale && q_param->per_tensor_output_shift) {
                onednn_post_ops_count++;
            } else {
                onednn_post_ops_count += 2;
            }

            auto out_dt = fo.output_layout.data_type;
            auto output_type_is_int8 = out_dt == data_types::u8 || out_dt == data_types::i8;
            auto out_range_usage = q_param->per_tensor_output_range && (q_param->out_lo < q_param->out_hi);

            if (out_range_usage) {
                // round
                if (!output_type_is_int8) {
                    onednn_post_ops_count++;
                }

                // clamp
                if (q_param->has_clamp) {
                    onednn_post_ops_count++;
                }
            } else {
                // clamp
                if (q_param->has_clamp) {
                    onednn_post_ops_count += 2;
                }
                // round
                {
                    onednn_post_ops_count++;
                }
            }
        }
    }

    return onednn_post_ops_count;
}

static bool is_reduce_blocked_axes(reduce_node const& node) {
    auto prim = node.get_primitive();
    auto reduce_axes = prim->axes;
    auto input_layout = node.input().get_output_layout();

    if (prim->keep_dims == false &&
        (count(reduce_axes.begin(), reduce_axes.end(), 1) > 0 ||
        (count(reduce_axes.begin(), reduce_axes.end(), 0) > 0 && input_layout.batch() > 1))) {
        return true;
    }

    return false;
}

std::pair<std::shared_ptr<reorder>, bool> reorder_factory::get_reorder(primitive_id src_id,
                                                                       const layout& in_layout,
                                                                       const layout& out_layout) {
    if (in_layout == out_layout)
        return std::make_pair(nullptr, true);

    cache_key ckey{ src_id, out_layout };
    auto itr = _cached_reorders.find(ckey);
    if (itr != _cached_reorders.end())
        return std::make_pair(itr->second, true);

    auto count = _cached_reorders.size();
    std::stringstream ss;
    ss << src_id << "_reorder_" << count;

    auto reorder = std::make_shared<cldnn::reorder>(ss.str(), src_id, out_layout);
    _cached_reorders[ckey] = reorder;

    return std::make_pair(reorder, false);
}

std::vector<std::pair<std::shared_ptr<primitive>, bool>> reorder_factory::get_weights_reorder(
    primitive_id input_id,
    const layout& old_layout,
    const kernel_selector::weights_reorder_params& reorder_params) {

    if (reorder_params.engine == kernel_selector::weights_reorder_params::Engine::NONE)
        return {};

    std::vector<std::pair<std::shared_ptr<primitive>, bool>> ret;

    if (reorder_params.engine == kernel_selector::weights_reorder_params::Engine::CPU &&
        reorder_params.cpuKernel != nullptr) {
        const auto intermediate_format = from_weights_layout(reorder_params.cpuKernel->GetExpectedInputLayout());
        const auto intermediate_type = from_weights_type(reorder_params.cpuKernel->GetExpectedInputType());
        if (intermediate_format != old_layout.format || intermediate_type != old_layout.data_type) {
            const layout intermediate_layout = { intermediate_type,
                                                intermediate_format,
                                                old_layout.get_tensor().transform(intermediate_format, 1) };

            auto reorder = get_reorder(input_id, old_layout, intermediate_layout);
            if (reorder.first) {
                ret.push_back(reorder);
                input_id = reorder.first->id;
            }
        }
    }

    layout expected_layout = from_weights_tensor(reorder_params.dest);

    cache_key ckey{ input_id, expected_layout, false };
    auto itr = _cached_generic_reorders.find(ckey);
    if (itr != _cached_generic_reorders.end()) {
        ret.push_back(std::make_pair(itr->second, true));
    } else {
        auto count = _cached_generic_reorders.size();
        std::stringstream ss;
        ss << input_id << "_generic_layer_" << count;

        auto reorder = std::make_shared<cldnn::generic_layer>(ss.str(), input_id, expected_layout, reorder_params);
        _cached_generic_reorders[ckey] = reorder;
        ret.push_back(std::make_pair(reorder, false));
    }

    return ret;
}

bool layout_optimizer::is_format_supported(program_node& node, format::type fmt) {
    if (node.is_type<fully_connected>() && fmt == format::byxf)
        return false;

    if (node.is_type<mvn>() && fmt == format::b_fs_yx_fsv16 &&
        node.get_dependency(0).get_output_layout().data_type != data_types::i8 &&
        node.get_dependency(0).get_output_layout().data_type != data_types::u8)
        return false;

    if (node.is_type<input_layout>())
        return node.get_output_layout().format == fmt;

    if (!_forcing_map.empty() && _forcing_map.count(node.id()))
        return _forcing_map.at(node.id()).first == fmt;

    auto prev_layout = node.get_output_layout();
    auto new_layout = prev_layout;
    new_layout.format = fmt;
    node.set_output_layout(new_layout, false);

    auto supported = node.type()->does_possible_implementation_exist(node);

    node.set_output_layout(prev_layout, false);

    return supported;
}

bool layout_optimizer::can_fuse_reorder(program_node& prev, program_node& next, format fmt_prev, format fmt_next) {
    auto prev_simple = fmt_prev == format::bfyx || fmt_prev == format::byxf || fmt_prev == format::yxfb;
    auto next_simple = fmt_next == format::bfyx || fmt_next == format::byxf || fmt_next == format::yxfb;
    auto prev_output_layout = prev.get_output_layout();
    auto next_output_layout = next.get_output_layout();
    auto prev_dt = prev.get_output_layout().data_type;
    auto next_dt = next.get_output_layout().data_type;
    auto use_onednn_impls = _optimization_attributes.use_onednn_impls;

    if (prev.is_dynamic() || next.is_dynamic())
        return false;

    auto is_input_idx = [&](size_t idx) -> bool {
        if (&next.get_dependency(idx) == &prev)
            return true;
        if (next.get_dependency(idx).is_type<reorder>() && &next.get_dependency(idx).get_dependency(0) == &prev)
            return true;
        return false;
    };

    // Not to fuse reorder if this removal changes input format of its next node which has reuse in fused_op
    if (next.get_preferred_impl_type() == impl_types::onednn) {
        for (auto& fused_op : next.get_fused_primitives()) {
            if (fused_op.is_type<eltwise>()) {
                auto out_layout = next.get_output_layout();
                auto add_type = onednn_add_fusing_helpers::get_add_fusing_type(next, fused_op);
                if (add_type == add_fusing_type::sum && prev.get_output_layout().format != out_layout.format)
                    return false;
            }
        }
    }

    // Ref kernels are the main for depth_to_space and region_yolo. It can do anything.
    if (next.is_type<depth_to_space>() || next.is_type<region_yolo>())
        return true;

    if (next.is_type<reorder>())
        return true;

    // Do not remove reorder if it is necessary to fulfill required_input
    auto reorder_layout = next.get_dependency(0).get_output_layout();
    if (reorder_layout.format == next.get_required_input0()
            && !reorder_layout.data_padding)
        return false;

    // resample_opt kernel can work cross-layout between fsv16 and fsv32
    if (next.is_type<resample>() &&
        (fmt_prev == format::b_fs_yx_fsv16 || fmt_prev == format::b_fs_yx_fsv32
            || fmt_prev == format::bs_fs_yx_bsv32_fsv16 || fmt_prev == format::bs_fs_yx_bsv32_fsv32) &&
        (fmt_next == format::b_fs_yx_fsv16 || fmt_next == format::b_fs_yx_fsv32
            || fmt_next == format::bs_fs_yx_bsv32_fsv16 || fmt_next == format::bs_fs_yx_bsv32_fsv32))
        return true;

    if (next.is_type<pooling>() &&
        (((prev_simple && next_simple) && (prev_dt == next_dt)) ||
        ((fmt_prev == format::b_fs_yx_fsv4 && fmt_next == format::bfyx) && (prev_dt == data_types::u8 || prev_dt == data_types::i8))))
        return true;

    if (next.is_type<eltwise>() && prev_simple && next_simple)
        return true;

    if (next.is_type<permute>() && (fmt_prev == format::b_fs_zyx_fsv16 &&
        next_output_layout.batch() > 1 &&
        next_output_layout.feature() % 16 != 0)) {
        return true;
    }

    if (next.is_type<fully_connected>() &&
        (fmt_prev == format::bfyx || fmt_prev == format::yxfb ||
         fmt_prev == format::b_fs_yx_fsv16 || fmt_prev == format::fs_b_yx_fsv32 ||
         fmt_prev == format::b_fs_yx_fsv32 ||
         (fmt_prev == format::b_fs_yx_fsv4 &&
          prev_output_layout.feature() % 32 == 0 &&
          prev_output_layout.spatial(0) == 1 &&
          prev_output_layout.spatial(1) == 1)))
        return true;

    if (next.is_type<convolution>() && fmt_prev == format::b_fs_yx_fsv16 && fmt_next == format::b_fs_yx_fsv4 && is_input_idx(0))
        return true;

    if (next.is_type<quantize>() && (fmt_prev == format::bfyx || fmt_prev == format::bfzyx) &&
        prev.is_input() && (prev_dt == data_types::u8 || prev_dt == data_types::i8))
        return true;

    if (!use_onednn_impls) {
        if (next.is_type<convolution>() &&
            (fmt_prev == format::bfyx || fmt_prev == format::bs_fs_yx_bsv4_fsv2) &&
            ((fmt_next == format::fs_b_yx_fsv32 && next.as<convolution>().get_primitive()->groups == 1) ||
            (fmt_next == format::b_fs_yx_fsv32 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::bs_fs_yx_bsv32_fsv32 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::bs_fs_yx_bsv32_fsv16 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::bs_fs_yx_bsv16_fsv16 && next_output_layout.feature() % 16 == 0 && prev_output_layout.feature() == 3) ||
            (fmt_next == format::bs_fs_yx_bsv16_fsv16 && next_output_layout.feature() >= 16 && prev_output_layout.feature() == 3 &&
            (next_output_layout.data_type != data_types::i8 && next_output_layout.data_type != data_types::u8))))
            return true;

        if (next.is_type<convolution>() &&
            fmt_prev == format::bfyx &&
            (fmt_next == format::b_fs_yx_fsv16 || fmt_next == format::bs_fs_yx_bsv32_fsv16) &&
            next_output_layout.feature() >= 16 && prev_output_layout.feature() <= 4 &&
            next.as<convolution>().get_primitive()->activations_zero_points.empty() &&
            next.as<convolution>().get_primitive()->weights_zero_points.empty())
            return true;

        if (next.is_type<convolution>() &&
            (fmt_prev == format::b_fs_yx_fsv4 || fmt_prev == format::bs_fs_yx_bsv4_fsv4 || fmt_prev == format::bs_fs_yx_bsv8_fsv4) &&
            ((fmt_next == format::b_fs_yx_fsv32 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::bs_fs_yx_bsv32_fsv32 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::bs_fs_yx_bsv4_fsv4 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::bs_fs_yx_bsv8_fsv4 && (prev_output_layout.feature() == 3 || prev_output_layout.feature() == 4)) ||
            (fmt_next == format::b_fs_yx_fsv16 && next_output_layout.feature() >= 16 &&
            (prev_output_layout.feature() == 3 || (prev_output_layout.feature() == 4 && (prev_dt == data_types::u8 || prev_dt == data_types::i8))))))
            return true;
    }

    if (next.is_type<quantize>() && (fmt_prev == format::bfyx || fmt_prev == format::bfzyx) &&
        (fmt_next == format::b_fs_yx_fsv16 || fmt_next == format::b_fs_zyx_fsv16 ||
         fmt_next == format::bs_fs_yx_bsv16_fsv16 || fmt_next == format::b_fs_yx_fsv4))
        return true;

    if (next.is_type<convolution>() &&
        !(prev.is_type<quantize>() && (prev_dt == data_types::i8 || prev_dt == data_types::u8)) &&
        (fmt_prev == format::b_fs_yx_fsv4 || fmt_prev == format::bfyx)  && prev_output_layout.feature() == 3 &&
        (fmt_next == format::b_fs_yx_fsv4 ||
         fmt_next == format::bs_fs_yx_bsv16_fsv16))
        return true;

    if (next.is_type<convolution>() &&
        fmt_prev == format::bfzyx &&
        ((fmt_next == format::b_fs_zyx_fsv16 || fmt_next == format::bs_fs_zyx_bsv16_fsv16) &&
            next_output_layout.feature() >= 16 && prev_output_layout.feature() == 3))
        return true;

    if (use_onednn_impls) {
        if (next.is_type<eltwise>() && (fmt_prev == format::bfyx) && (fmt_next == format::bs_fs_yx_bsv4_fsv2) &&
            prev.is_input() && (prev_dt == data_types::u8 || prev_dt == data_types::i8))
            return true;

        // Remove Reorder for Convolution if mixed layout.
        auto& node = prev.get_users().front();
        if (prev.get_output_layout().format == next.get_required_input0() &&
                node->get_output_layout().data_padding == prev.get_output_layout().data_padding)
            return true;

        if (next.is_type<convolution>() &&
            (fmt_prev == format::bfyx && fmt_next == format::bs_fs_yx_bsv32_fsv32) &&
            // Condition to avoid execution in reorder_inputs.
            prev.get_users().size() == 1 && prev.get_users().front()->is_type<reorder>()) {
            const auto& cur = prev.get_users().front();
            std::set<size_t> dep_idx_set;
            for (auto& p : next.get_fused_primitives()) {
                // find eltwise sum primitive which has dependency nodes, and gather dependency indices of it.
                if (p.is_type<eltwise>() && p.typed_desc<eltwise>()->mode == eltwise_mode::sum) {
                    for (size_t i = p.dep_start_idx; i < p.dep_start_idx + p.total_num_deps; i++) {
                        dep_idx_set.insert(i);
                    }
                }
            }
            // The current reorder can be fused if it is a dependency of eltwise sum primitive fused.
            for (size_t i = 0; i < next.get_dependencies().size(); i++) {
                auto& d_node = next.get_dependency(i);
                if (cur->id() == d_node.id() && dep_idx_set.find(i) != dep_idx_set.end()) {
                    return true;
                }
            }
        }

        if (next.is_type<quantize>() && prev.get_users().size() == 1)
            return true;

        if (next.is_type<permute>()) {
            auto& permute_order = next.as<permute>().get_primitive()->permute_order;
            if ((fmt_prev == format::b_fs_yx_fsv4 || fmt_prev == format::b_fs_yx_fsv32 || fmt_prev == format::b_fs_zyx_fsv32 ||
                fmt_prev == format::b_fs_yx_fsv16 || fmt_prev == format::b_fs_zyx_fsv16 || fmt_prev == format::bs_fs_yx_bsv16_fsv16)
                && permute_order.back() != 1
                && (!next.as<permute>().is_rotating_except_batch())) {
                    return false;
            }
            return true;
        }
    }

    return false;
}

bool layout_optimizer::can_fuse_reorder_to_prev(program_node& prev, program_node* next, format fmt_prev, format fmt_next) {
    if (prev.is_dynamic() || (next && next->is_dynamic()))
        return false;

    // Ref kernels are the main for depth_to_space, region_yolo and detection_output. It can do anything. Should not see next.
    if (prev.is_type<depth_to_space>() || prev.is_type<region_yolo>() || prev.is_type<detection_output>())
        return true;

    if (next == nullptr)
        return false;

    auto dt_prev = prev.get_output_layout().data_type;
    auto dt_next = next->get_output_layout().data_type;
    auto use_onednn_impls = _optimization_attributes.use_onednn_impls;

    if (prev.is_type<reorder>())
        return true;

    // resample_opt kernel can work cross-layout between fsv16 and fsv32
    if (prev.is_type<resample>() &&
        (fmt_prev == format::b_fs_yx_fsv16 || fmt_prev == format::b_fs_yx_fsv32
            || fmt_prev == format::bs_fs_yx_bsv32_fsv16 || fmt_prev == format::bs_fs_yx_bsv32_fsv32) &&
        (fmt_next == format::b_fs_yx_fsv16 || fmt_next == format::b_fs_yx_fsv32
            || fmt_next == format::bs_fs_yx_bsv32_fsv16 || fmt_next == format::bs_fs_yx_bsv32_fsv32))
        return true;

    if (prev.is_type<binary_convolution>() && fmt_next == format::b_fs_yx_fsv16)
        return true;

    if (prev.is_type<one_hot>() &&
        !data_type_traits::is_floating_point(dt_prev) &&
        data_type_traits::is_floating_point(dt_next) &&
        fmt_prev == fmt_next)
        return true;

    if (prev.is_type<quantize>() &&
        (fmt_next == format::b_fs_yx_fsv4 || fmt_next == format::b_fs_zyx_fsv32 || (fmt_next == format::b_fs_yx_fsv32 && !use_onednn_impls) ||
         fmt_next == format::b_fs_yx_fsv16 || fmt_next == format::b_fs_zyx_fsv16 ||fmt_next == format::bs_fs_yx_bsv16_fsv16))
        return true;

    if (prev.is_type<permute>()) {
        auto& permute_order = prev.as<permute>().get_primitive()->permute_order;
        if ((fmt_prev == format::b_fs_yx_fsv4 || fmt_prev == format::b_fs_yx_fsv32 || fmt_prev == format::b_fs_zyx_fsv32 ||
         fmt_prev == format::b_fs_yx_fsv16 || fmt_prev == format::b_fs_zyx_fsv16 || fmt_prev == format::bs_fs_yx_bsv16_fsv16)
         && permute_order.back() != 1
         && (!prev.as<permute>().is_rotating_except_batch())) {
            return false;
        }
        return true;
    }


    // Remove Reorder after convolution if possible.
    if (use_onednn_impls) {
        auto reorder_layout = next->get_dependency(0).get_output_layout();
        if (reorder_layout.format == prev.get_required_output() &&
                reorder_layout.data_padding == prev.get_output_layout().data_padding)
            return true;

        if (prev.is_type<eltwise>() &&
            is_mixed_layout(prev, *next, false, {{ format::bs_fs_zyx_bsv32_fsv32, format::bs_fs_zyx_bsv32_fsv16 }}))
            return true;
    }

    return false;
}

namespace {
bool should_use_winograd_2x3_s1(std::shared_ptr<const convolution> const& prim,
                                layout const& input_layout,
                                layout const& weights_layout,
                                bool output_size_handling_enabled) {
    // cases when NOT to use winograd
    if (input_layout.data_type != data_types::f16
        || input_layout.feature() % 64 != 0  // current algorithm is effective for ifm to be multiply of 64
        || weights_layout.spatial(0) != 3     // weights have to be 3x3 by definiton
        || weights_layout.spatial(1) != 3     // weights have to be 3x3 by definition
        || weights_layout.batch() % 64 != 0  // current algorithm is effective for ofm to be multiply of 64
        || any_not_one(prim->stride)               // stride has to be 1x1 by definition
        || any_not_one(prim->dilation)             // no support for dilation
        || prim->split() != 1                      // no support for splitted convolutions
        || (output_size_handling_enabled &&
            prim->with_output_size)                // no support for convolutions with user-specified output size
        || (input_layout.count() > 3000000)        // limit max input size as winograd consumes more memory
        || (input_layout.count() < 50000)          // limit min input size as winograd is not effective for small input
        || (input_layout.spatial(0) < 8 &&
            input_layout.spatial(1) < 8)      // disable winograd for small spatials as perf is poor
        || prim->groups != 1) {                    // disable winograd for groups
        return false;
    }
    return true;
}
}  // namespace

layout_optimizer::layout_optimizer(bool output_size_handling_enabled)
    : _optimization_attributes(), _output_size_handling_enabled(output_size_handling_enabled), _total_conv(0) {
    for (auto& format : optimized_formats) {
        _optimized_conv_count.insert({format, 0});
    }
}

bool layout_optimizer::is_depthwise(const convolution_node& node) const {
        const int32_t output_channels = node.get_output_layout().feature();
        const int32_t input_channels = node.get_dependency(0).get_output_layout().feature();

        return (node.get_groups() == static_cast<uint32_t>(input_channels) && input_channels == output_channels && node.get_split() == 1)
                 || (node.get_split() == input_channels && node.get_groups() == 1);
}

bool layout_optimizer::convolution_bfyx_opt(layout const& output_layout,
                                            const layout& weights_layout,
                                            std::shared_ptr<const convolution> conv) {
    // A set of rules that define when bfyx mem format has better performance than yxfb
    if (output_layout.batch() == 16 || output_layout.batch() % 16 != 0 ||
        output_layout.data_type != data_types::f16 || weights_layout.batch() % 16 != 0 ||
        !((weights_layout.spatial(0) == 1 && weights_layout.spatial(1) == 1) ||
          (weights_layout.spatial(0) >= 5 && weights_layout.spatial(1) >= 5) ||
          (conv->stride[0] > 1 && conv->stride[1] > 1) ||
          (weights_layout.feature() <= 32 && output_layout.spatial(0) < 224 &&
           output_layout.spatial(1) < 224) ||
          (weights_layout.feature() <= 64 && output_layout.spatial(0) < 112 &&
           output_layout.spatial(1) < 112) ||
          (weights_layout.feature() <= 128 && output_layout.spatial(0) < 56 &&
           output_layout.spatial(1) < 56) ||
          (weights_layout.feature() <= 256 && output_layout.spatial(0) < 28 &&
           output_layout.spatial(1) < 28) ||
          (weights_layout.feature() <= 512 && output_layout.spatial(0) < 14 &&
           output_layout.spatial(1) < 14) ||
          (weights_layout.feature() <= 1024 && output_layout.spatial(0) <= 7 &&
           output_layout.spatial(1) <= 7)) ||
        // WA for AgeGender, which has one convolution that is better on yxfb, but due to additonal reorder overall
        // performance is worse than bfyx
        (output_layout.spatial(0) == 82 && output_layout.spatial(1) == 82) ||
        (_optimization_attributes.splitted_convolution && output_layout.batch() == 16) ||
        (!_optimization_attributes.splitted_convolution && output_layout.batch() >= 128) ||
        _optimization_attributes.bfyx_only_layer)
        return true;

    return false;
}

bool layout_optimizer::convolution_byxf_opt(const layout& input_layout,
                                            layout const& output_layout,
                                            const layout& weights_layout,
                                            const convolution_node& node) {
    auto conv = node.get_primitive();

    if (node.get_dependency(0).is_type<convolution>()) {
        auto& dep = node.get_dependency(0).as<convolution>();
        if (is_depthwise(dep))
            return false;
    }

    // A set of rules that define when byxf mem format has better performance
    if ((output_layout.data_type == data_types::f16 && weights_layout.spatial(0) == 1 &&
        all_ones(conv->dilation) &&
        !node.get_transposed() &&
         node.get_groups() == 1 &&
         input_layout.feature() % 32 == 0 &&
         weights_layout.spatial(1) == 1 && output_layout.feature() % 64 == 0 &&
         weights_layout.batch() % 64 == 0 &&
         all_ones(conv->stride) &&
         all_zeroes(conv->pad)) ||
        // Winograd
        should_use_winograd_2x3_s1(conv, input_layout, weights_layout, _output_size_handling_enabled))
        return true;

    return false;
}

bool layout_optimizer::convolution_b_fs_yx_fsv16_opt(const layout& input_layout,
                                                     const layout& output_layout,
                                                     const layout& weights_layout,
                                                     std::shared_ptr<const convolution> conv,
                                                     bool weak_restrictions) {
    // A set of rules that define when b_fs_yx_fsv16 mem format can be used for int8 case
    bool i8_dt_case = (input_layout.data_type == data_types::u8 || input_layout.data_type == data_types::i8) &&
                       weights_layout.data_type == data_types::i8;

    if (i8_dt_case) {
        auto ks_x = weights_layout.spatial(0);
        auto ks_y = weights_layout.spatial(1);

        size_t in_features_per_group = input_layout.feature() / conv->groups;
        size_t out_features_per_group = output_layout.feature() / conv->groups;

        // Check for non-grouped or depthwise convolution
        if (input_layout.format.dimension() == 4 &&
            ((ks_x == 7 && ks_y == 7) || (ks_x == 3 && ks_y == 3) || (ks_x == 1 && ks_y == 1) || (ks_x == 5 && ks_y == 5)) &&
            output_layout.feature() >= 16 &&
            ((conv->groups == 1 && conv->split() == 1) ||
             conv->groups == static_cast<uint32_t>(input_layout.feature()) ||
             conv->split() == static_cast<int32_t>(input_layout.feature())))
            return true;
        // Check for grouped convolution
        else if (input_layout.format.dimension() == 4 && input_layout.batch() < 16 &&
                 out_features_per_group >= 16 &&
                 // Need to extend imad fsv4 kernel to handle e.g. 3 input features per group
                 (in_features_per_group % 4 == 0) &&
                 ((conv->dilation[conv->dilation.size() - 1] + 1) * (ks_x - 1)) <= 16)
                return true;
        // Check for fsv16 imad kernel
        else if ((input_layout.format.dimension() == 4) &&
                 ((in_features_per_group > 8) || (out_features_per_group >= 4)))
                return true;
        return false;
    }
    // A set of rules that define when b_fs_yx_fsv16 mem format can be used for fp16/fp32 case
    int32_t feature_block_size = 16;
    bool correct_data_type = (input_layout.data_type == data_types::f16 || input_layout.data_type == data_types::f32) &&
                             (weights_layout.data_type == input_layout.data_type);
    bool correct_batch = (input_layout.batch() == 1) || (input_layout.batch() > 1 && input_layout.data_type == data_types::f32);
    bool correct_spatial_dims = input_layout.spatial(2) == 1 && input_layout.spatial(3) == 1;
    int32_t required_feature_num = weak_restrictions ? feature_block_size / 2 : feature_block_size;
    bool correct_in_feature = (input_layout.feature() >= required_feature_num &&
                                  output_layout.feature() >= required_feature_num);
    int32_t in_features_per_group = input_layout.feature() / conv->groups;
    int32_t out_features_per_group = output_layout.feature() / conv->groups;
    if (!correct_in_feature && input_layout.feature() <= 4 && out_features_per_group >= feature_block_size)
        correct_in_feature = true;
    bool depthwise = conv->groups == static_cast<uint32_t>(input_layout.feature());  // depthwise conv
    bool grouped = ((feature_block_size % out_features_per_group == 0) &&
                       (feature_block_size % in_features_per_group == 0) &&
                       (feature_block_size / out_features_per_group > 1) &&
                       (feature_block_size / in_features_per_group > 1) &&
                       (out_features_per_group != 1) &&
                       (in_features_per_group != 1)) ||
                      ((out_features_per_group % feature_block_size == 0 || feature_block_size % out_features_per_group == 0) &&
                       (in_features_per_group % feature_block_size == 0));
    if (correct_data_type &&
        correct_batch &&
        correct_spatial_dims &&
        correct_in_feature &&
        (conv->groups == 1 || depthwise || grouped))
        return true;
    return false;
}

bool layout_optimizer::should_select_b_fs_yx_fsv16_layout(convolution_node const& node, layout const& weights_layout) {
    auto prim = node.get_primitive();
    auto input_layout = node.get_dependency(0).get_output_layout();
    auto const cond_denom = _total_conv > 0 ? 1.0f / static_cast<float>(_total_conv) : 1.0f;
    auto fully_support_conv_num = _optimized_conv_count.at({format::b_fs_yx_fsv16, false});
    auto partially_support_conv_num = _optimized_conv_count.at({format::b_fs_yx_fsv16, true});

    auto output_layout = node.calc_output_layout();

    auto current_conv_supports_layout = convolution_b_fs_yx_fsv16_opt(input_layout, output_layout, weights_layout,  prim);
    auto is_prev_conv_node_supports_layout = node.get_dependency(0).is_type<convolution>() &&
                                             is_format_optimized(node.get_dependency(0).as<convolution>(), format::b_fs_yx_fsv16);
    auto weak_restriction_cond = (partially_support_conv_num - fully_support_conv_num) * cond_denom < 0.15f;
    auto current_conv_partially_supports_layout = convolution_b_fs_yx_fsv16_opt(input_layout, output_layout, weights_layout, prim, true);
    auto may_use_weak_restrictions = is_prev_conv_node_supports_layout || weak_restriction_cond;

    return ((_optimization_attributes.b_fs_yx_fsv16_network) &&
            (current_conv_supports_layout || (may_use_weak_restrictions && current_conv_partially_supports_layout))) ||
           input_layout.format == format::b_fs_yx_fsv16;
}

bool layout_optimizer::convolution_b_fs_zyx_fsv16_opt(const layout& input_layout,
                                                      const layout& output_layout,
                                                      const layout& weights_layout,
                                                      std::shared_ptr<const convolution> conv) {
    // A set of rules that define when b_fs_zyx_fsv16 mem format can be used
    size_t in_features_per_group = input_layout.feature() / conv->groups;
    size_t out_features_per_group = output_layout.feature() / conv->groups;

    // Check for fsv16 imad kernel
    if ((input_layout.format.dimension() == 5) &&
        (input_layout.data_type == data_types::i8 || input_layout.data_type == data_types::u8) &&
        (weights_layout.data_type == data_types::i8 || weights_layout.data_type == data_types::u8) &&
        ((in_features_per_group > 8) || (out_features_per_group >= 4)))
        return true;

    bool format_ver = (input_layout.format == format::bfzyx || input_layout.format == format::b_fs_zyx_fsv16 ||
                      input_layout.format == format::bs_fs_zyx_bsv16_fsv16);
    bool data_type_ver = input_layout.data_type == data_types::f16 || input_layout.data_type == data_types::f32;
    bool w_layout = weights_layout.data_type == input_layout.data_type;
    bool single_dilation = all_ones(conv->dilation);
    bool groups_ver = conv->groups == 1 || out_features_per_group % 16 == 0
        || (conv->groups > 1 && out_features_per_group == 8);

    return format_ver && data_type_ver && w_layout && single_dilation && groups_ver;
}
bool layout_optimizer::convolution_bs_fs_yx_bsv16_fsv16_opt(const layout& input_layout,
                                                            const layout& output_layout,
                                                            const layout& weights_layout,
                                                            std::shared_ptr<const convolution> conv) {
    // A set of rules that define when bs_fs_yx_bsv16_fsv16 mem format can be used
    bool correct_batch = input_layout.batch() > 16;
    bool correct_feature = (input_layout.feature() % 16 == 0 || input_layout.feature() == 3) && conv->output_size.feature[0] % 16 == 0;
    bool fp16_ver = input_layout.data_type == data_types::f16 && input_layout.batch() % 32 == 0;
    bool fp32_ver = input_layout.data_type == data_types::f32 && input_layout.batch() % 16 == 0;
    bool single_group = conv->groups == 1;

    bool int8_sup = (input_layout.data_type == data_types::i8 || input_layout.data_type == data_types::u8);
    if (int8_sup)
        correct_batch = input_layout.batch() >= 16;
    int8_sup &= (input_layout.batch() % 16 == 0 && weights_layout.data_type == data_types::i8 &&
                 conv->activations_zero_points.empty() && conv->weights_zero_points.empty());
    auto ks_x = weights_layout.spatial(0);
    auto ks_y = weights_layout.spatial(1);
    int8_sup &= (input_layout.spatial(2) == 1 && ((ks_x == 1 && ks_y == 1) || (ks_x == 3 && ks_y == 3) || (ks_x == 7 && ks_y == 7)) &&
                 output_layout.feature() % 32 == 0 && conv->split() == 1 && all_ones(conv->dilation));

    return (int8_sup || fp16_ver || fp32_ver) && correct_feature && correct_batch && single_group;
}

bool layout_optimizer::convolution_fs_b_yx_fsv32_opt(const layout& input_layout,
                                                     const layout& output_layout,
                                                     const layout& weights_layout,
                                                     std::shared_ptr<const convolution> conv,
                                                     bool weak_restrictions) {
    auto ofm = output_layout.feature();
    // A set of rules that define when fs_b_yx_fsv32 mem format can be used
    bool correct_batch = input_layout.batch() > 1;
    bool correct_in_feature = input_layout.feature() >= 16;
    bool correct_out_feature = weak_restrictions ? ofm >= 16 : ofm > 16;
    bool dw_conv = static_cast<int>(conv->groups) == input_layout.feature();
    if (!correct_in_feature && input_layout.feature() == 3 && conv->groups == 1) {   // bfyx with 3 feature -> fs_b_yx_fsv32 case
        correct_in_feature = true;
    }

    if (input_layout.data_type != data_types::f16 || weights_layout.data_type != data_types::f16) {
        return false;
    }

    if ((input_layout.format == format::fs_b_yx_fsv32) || (correct_out_feature && correct_in_feature && correct_batch &&
        conv->split() == 1 && (dw_conv || conv->groups == 1) )) {
        return true;
    }
    return false;
}

bool layout_optimizer::deconvolution_b_fs_zyx_fsv16_opt(layout const &input_layout,
                                                        const layout &/*weights_layout*/,
                                                        std::shared_ptr<const deconvolution> deconv) {
    // A set of rules that define when b_fs_zyx_fsv16 mem format can be used
    if ((input_layout.format == format::bfzyx ||
         input_layout.format == format::b_fs_zyx_fsv16 ||
         input_layout.format == format::bs_fs_zyx_bsv16_fsv16) &&
        (input_layout.data_type == data_types::f32 || input_layout.data_type == data_types::f16) &&
        deconv->split() == 1)
        return true;

    if (input_layout.format.dimension() == 5 &&
        (input_layout.data_type == data_types::i8 || input_layout.data_type == data_types::u8) &&
        deconv->split() == 1)
        return true;

    return false;
}

bool layout_optimizer::deconvolution_b_fs_yx_fsv16_opt(layout const &input_layout,
                                                       const layout &weights_layout,
                                                       std::shared_ptr<const deconvolution> deconv) {
    // A set of rules that define when b_fs_yx_fsv16 mem format can be used
    if ((input_layout.format == format::bfyx || input_layout.format == format::b_fs_yx_fsv16) &&
        (input_layout.data_type == data_types::f32 || input_layout.data_type == data_types::f16) &&
        deconv->split() == 1 &&
        (deconv->groups == 1 || (static_cast<int>(deconv->groups) == weights_layout.group())))
        return true;

    if (input_layout.format.dimension() == 4 &&
        (input_layout.data_type == data_types::i8 || input_layout.data_type == data_types::u8) &&
        deconv->split() == 1)
        return true;

    return false;
}

static bool is_node_for_onednn(reduce_node const& node, format preferred_format) {
    auto& input = node.input();
    auto reduce_prim = node.get_primitive();
    // oneDNN reduction currently does not support logical_and, logical_or, log_sum and log_sum_exp.
    switch (reduce_prim->mode) {
        case reduce_mode::mean:
        case reduce_mode::max:
        case reduce_mode::min:
        case reduce_mode::sum:
        case reduce_mode::prod:
            break;
        case reduce_mode::sum_square:
        case reduce_mode::l1:
        case reduce_mode::l2:
            // modes have a limitation of data type
            if (input.get_output_layout().data_type == data_types::f16 ||
                input.get_output_layout().data_type == data_types::f32)
                break;
        default:
            return false;
    }

    auto input_layout = input.get_output_layout();
    if (input_layout.get_dims().size() > 4) {
        return false;
    }

    // redundant reduce is not acceptable on oneDNN reduction
    if (node.get_output_layout() == input_layout) {
        return false;
    }

    // oneDNN reduction selects ref kernel for simple formats(bfyx..) which has perf regression with a decent tensor size.
    if (format::is_simple_data_format(preferred_format)) {
        return false;
    } else {
        // Onednn reduction does NOT support reordering of unreduced-axes.
        // Currently, an Onednn reduce layer which contains reduction of blocked axes(b-f) is expected to select planar format.
        if (is_reduce_blocked_axes(node)) {
            return false;
        }
    }

    return true;
}

static bool is_node_for_onednn(deconvolution_node const& node) {
    auto prim = node.get_primitive();
    auto input_layout = node.get_dependency(0).get_output_layout();
    auto output_layout = node.get_output_layout();

    // Onednn deconv does not support cross-precision
    bool onednn_valid_dt = (data_type_traits::is_i8_u8(input_layout.data_type) && data_type_traits::is_i8_u8(output_layout.data_type)) ||
                            (input_layout.data_type == data_types::f16 && output_layout.data_type == data_types::f16);

    bool onednn_valid_params = onednn_valid_dt &&
                               input_layout.feature() >= 16 &&
                               prim->groups == 1 &&
                               get_post_ops_count(node) <= 32 &&
                               input_layout.data_type == output_layout.data_type;

    auto spatial_dims_num = input_layout.get_spatial_rank();

    return onednn_valid_dt && onednn_valid_params && spatial_dims_num <= 3;
}


static bool is_node_for_onednn(program_node& node, fully_connected_node const& fc_node) {
    bool is_suitable_for_onednn = true;
    auto out_layout = node.get_output_layout();
    for (auto& fo : node.get_fused_primitives()) {
        if (fo.is_type<eltwise>()) {
            // FC checkings
            auto in_layout = node.get_dependency(fo.dep_start_idx).get_output_layout();
            auto in_dt = in_layout.data_type;
            auto out_dt = out_layout.data_type;
            // if it is not eltwise sum and input is full tensor
            if ((out_layout.count() == in_layout.count()) && in_dt != out_dt
                && (data_type_traits::is_floating_point(in_dt) || data_type_traits::is_floating_point(out_dt))
                && onednn_add_fusing_helpers::is_full_tensor(in_layout)) {
                return false;
            }

            // WA: onednn sum/binary_add post-op are not supported due to perf drop.
            auto add_type = onednn_add_fusing_helpers::get_add_fusing_type(node, fo);
            if (add_type == add_fusing_type::sum || add_type == add_fusing_type::binary_per_tensor || add_type == add_fusing_type::binary_per_oc) {
                return false;
            }
        }
    }

    auto fc_prim = fc_node.get_primitive();
    int32_t rank = cldnn::format::dimension(out_layout.format);
    auto size = out_layout.get_tensor();
    // OneDnn doesn't support spatial dimensions for output
    for (int i = 0; i < rank - 2 - (fc_prim->input_size == 3 ? 1 : 0); i++) {
        if (size.spatial[i] != 1) {
            return false;
        }
    }

    return is_suitable_for_onednn;
}

// This function is needed to avoid performance regressions for the convolutions with byxf layout
// Previously some topologies had scale operations which prevented byxf usage
// Now instead of scale we have eltwise + fused_ops which might enable byxf convolution in unexpected cases
// So here we check that given eltwise node is replacement of the old scale primitive
// TODO: Adjust byxf convolution selection logic
static bool is_scale_shift(const eltwise_node& node) {
    if (node.get_dependencies().size() != 2)
        return false;

    if (node.get_primitive()->mode != eltwise_mode::prod)
        return false;

    if (node.get_fused_primitives().empty())
        return false;

    auto fused_op0 = node.get_fused_primitives().front();

    if (!fused_op0.is_type<eltwise>())
        return false;

    if (fused_op0.typed_desc<eltwise>()->mode != eltwise_mode::sum)
        return false;

    return true;
}

bool layout_optimizer::users_for_convolution_byxf_opt(program_node const& node, uint32_t depth) {
    // This function checks if byxf optimization can be applied to the required depth of node's users.
    // Setting depth to 1 will check only node's users, depth = 2 are user's users etc.
    if (depth == 0)
        return true;

    for (auto& user : node.get_users()) {
        // primitives that support transitions byxf->other format and other format->byxf are valid for byxf opt
        if ((user->type() == cldnn::eltwise::type_id() && !is_scale_shift(user->as<eltwise>())) || user->type() == cldnn::pooling::type_id()) {
            if (!users_for_convolution_byxf_opt(*user, depth - 1))
                return false;
        // convolution that is capable to use byxf and is performant is also valid for byxf opt
        } else if (user->type() == cldnn::convolution::type_id()) {
            if (convolution_byxf_opt(node.get_output_layout(),
                                     user->calc_output_layout(),
                                     user->get_dependency(1).get_output_layout(),
                                     user->as<convolution>())) {
                if (!users_for_convolution_byxf_opt(*user, depth - 1))
                    return false;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

bool layout_optimizer::deps_for_convolution_byxf_opt(program_node const& node, uint32_t depth) {
    // This function checks if requested format is the same for node's users in the required depth.
    // Setting depth to 1 will check only node's dependencies, depth = 2 are dep's dependencies etc.
    if (depth == 0)
        return true;

    for (auto& dep : node.get_dependencies()) {
        // skip data and generic_layers
        if (dep->is_type<data>() || dep->is_type<generic_layer>())
            continue;

        if (dep->is_type<convolution>()) {
            auto& conv_dep = dep->as<convolution>();
            if (!convolution_byxf_opt(conv_dep.input().get_output_layout(),
                                      conv_dep.get_output_layout(),
                                      conv_dep.weights().get_output_layout(),
                                      conv_dep)) {
                return false;
            }
        } else if ((!dep->is_type<pooling>() && !dep->is_type<eltwise>()) ||
                   (dep->is_type<eltwise>() && is_scale_shift(dep->as<eltwise>()))) {
            return false;
        }

        if (!deps_for_convolution_byxf_opt(*dep, depth - 1))
            return false;
    }
    return true;
}

format layout_optimizer::imad_case(convolution_node const& node) const {
    auto dims_count = format::dimension(node.input().get_output_layout().format);

    bool is_grouped = node.get_split() > 1 || node.get_groups() > 1;
    bool is_dw = is_depthwise(node);

    if (dims_count == 5 && is_grouped) {
        return format::bfzyx;
    } else if (dims_count == 4 && is_grouped && !is_dw) {
        return format::b_fs_yx_fsv4;
    }

    bool asymmetric_quantization = node.activations_zero_points_term() || node.weights_zero_points_term();

    if (asymmetric_quantization && _optimization_attributes.b_fs_zyx_fsv32_network) {
        if (dims_count == 5) {
            return format::b_fs_zyx_fsv32;
        } else {
            return format::b_fs_yx_fsv32;
        }
    }

    if (dims_count == 5) {
        return format::bfzyx;
    }

    return format::b_fs_yx_fsv4;
}

bool layout_optimizer::is_mixed_layout(program_node& prev, program_node& next, bool check_data_type,
                                       std::vector<std::pair<format, format>> custom_list) const {
    auto prev_layout = prev.get_output_layout();
    auto prev_fmt = prev_layout.format;
    auto prev_dt = prev_layout.data_type;
    auto next_layout = next.get_output_layout();
    auto next_fmt = next_layout.format;
    auto next_dt = next_layout.data_type;

    // std::pair<i8_u8_format, float_format>
    std::vector<std::pair<format, format>> supported_list = {
        { format::b_fs_yx_fsv32, format::b_fs_yx_fsv16 },
        { format::bs_fs_yx_bsv32_fsv32, format::bs_fs_yx_bsv32_fsv16 },
        { format::bs_fs_yx_bsv32_fsv32, format::bs_fs_yx_bsv16_fsv16 },
        { format::b_fs_zyx_fsv32, format::b_fs_zyx_fsv16 },
        { format::bs_fs_zyx_bsv32_fsv32, format::bs_fs_zyx_bsv32_fsv16 },
        { format::bs_fs_zyx_bsv32_fsv32, format::bs_fs_zyx_bsv16_fsv16 },
    };

    auto& check_list = custom_list.size() > 0 ? custom_list : supported_list;

    for (auto& pair : check_list) {
        if ((prev_fmt == pair.first && next_fmt == pair.second) &&
            (!check_data_type || (data_type_traits::is_i8_u8(prev_dt) && data_type_traits::is_floating_point(next_dt)))) {
            if ((next_fmt == format::bs_fs_yx_bsv32_fsv16 || next_fmt == format::bs_fs_zyx_bsv32_fsv16) && (next_dt == data_types::f32)) return false;
            if ((next_fmt == format::bs_fs_yx_bsv16_fsv16 || next_fmt == format::bs_fs_zyx_bsv16_fsv16) && (next_dt == data_types::f16)) return false;
            return true;
        }
        if ((next_fmt == pair.first && prev_fmt == pair.second) &&
            (!check_data_type || (data_type_traits::is_i8_u8(next_dt) && data_type_traits::is_floating_point(prev_dt)))) {
            if ((prev_fmt == format::bs_fs_yx_bsv32_fsv16 || prev_fmt == format::bs_fs_zyx_bsv32_fsv16) && (prev_dt == data_types::f32)) return false;
            if ((prev_fmt == format::bs_fs_yx_bsv16_fsv16 || prev_fmt == format::bs_fs_zyx_bsv16_fsv16) && (prev_dt == data_types::f16)) return false;
            return true;
        }
    }

    return false;
}

layout layout_optimizer::get_expected_layout(layout const& current_layout,
                                             convolution_node const& node,
                                             layout const& weights_layout) {
    auto prim = node.get_primitive();
    auto expected_tensor = current_layout.get_tensor();
    auto expected_data_type = current_layout.data_type;
    auto expected_format = current_layout.format;
    auto input_layout = node.get_dependency(0).get_output_layout();
    auto output_layout = node.calc_output_layout();

    const float cond_denom = _total_conv > 0 ? 1.0f / static_cast<float>(_total_conv) : 1.0f;

    bool onednn_valid_post_ops = get_post_ops_count(node) <= 32;
    bool use_onednn_impls = _optimization_attributes.use_onednn_impls && input_layout.data_type != data_types::f32;
    bool i8_u8_input = input_layout.data_type == data_types::u8 || input_layout.data_type == data_types::i8;

    if (use_onednn_impls && onednn_valid_post_ops) {
        expected_format = node.get_required_output();
    } else {
        /* *************************** Native impls format selection part ************************** */
        if (use_onednn_impls && i8_u8_input) {
            // It is here because of post operation condition for onednn.
            // Use fsv32 for onednn friendliness.
            expected_format = cldnn::format::b_fs_yx_fsv32;
        } else if (i8_u8_input) {
            if ((_optimization_attributes.b_fs_yx_fsv16_network &&
                convolution_b_fs_yx_fsv16_opt(input_layout, output_layout, weights_layout, prim))) {
                expected_format = cldnn::format::b_fs_yx_fsv16;
            } else if ((_optimization_attributes.b_fs_zyx_fsv16_network &&
                convolution_b_fs_zyx_fsv16_opt(input_layout, output_layout, weights_layout, prim))) {
                expected_format = cldnn::format::b_fs_zyx_fsv16;
            } else {
                expected_format = imad_case(node);
            }
            expected_tensor = current_layout.get_tensor();
        } else if (_optimization_attributes.b_fs_zyx_fsv16_network &&
                convolution_b_fs_zyx_fsv16_opt(input_layout, output_layout, weights_layout, prim)) {
            expected_tensor = current_layout.get_tensor();
            if ((current_layout.data_type == data_types::f32 && current_layout.batch() % 16 == 0) ||
                (current_layout.data_type == data_types::f16 && current_layout.batch() % 32 == 0))
                expected_format = cldnn::format::bs_fs_zyx_bsv16_fsv16;
            else
                expected_format = cldnn::format::b_fs_zyx_fsv16;

        } else if (current_layout.format == format::bfzyx) {
            expected_tensor = current_layout.get_tensor();
            expected_format = cldnn::format::bfzyx;
        } else if (_optimization_attributes.bs_fs_yx_bsv16_fsv16_network &&
                convolution_bs_fs_yx_bsv16_fsv16_opt(node.input().get_output_layout(), output_layout, weights_layout, prim)) {
            expected_tensor = current_layout.get_tensor();
            expected_format = cldnn::format::bs_fs_yx_bsv16_fsv16;
        } else if (_optimization_attributes.fs_b_yx_fsv32_network && !node.get_transposed() &&
                ((convolution_fs_b_yx_fsv32_opt(input_layout,
                                                output_layout,
                                                weights_layout, prim) ||
                (((node.get_dependency(0).is_type<convolution>() && is_format_optimized(node.get_dependency(0).as<convolution>(), format::fs_b_yx_fsv32))
                    || (_optimized_conv_count.at({format::fs_b_yx_fsv32, false}) * cond_denom > 0.8f)) &&
                    convolution_fs_b_yx_fsv32_opt(input_layout,
                                                output_layout,
                                                weights_layout, prim, true))))) {
            // Chose fs_b_yx_fsv32 layout in two cases: 1-st: the current conv primitive totally supports fs_b_yx_fsv32 layout
            //                                          2-nd: the previous conv primitive supports fs_b_yx_fsv32 layout and
            //                                                current conv primitives supports this one with weak restrictions -
            //                                                that should be cheaper than reordering data to another layout
            expected_tensor = current_layout.get_tensor();
            expected_format = format::fs_b_yx_fsv32;
        } else if (should_select_b_fs_yx_fsv16_layout(node, weights_layout)) {
            expected_tensor = current_layout.get_tensor();
            expected_format = cldnn::format::b_fs_yx_fsv16;
        } else if (current_layout.data_type == data_types::f16 &&
                    layout_optimizer::convolution_byxf_opt(input_layout, current_layout, weights_layout, node) &&
                    (users_for_convolution_byxf_opt(node, 2) || deps_for_convolution_byxf_opt(node, 2)) &&
                    // todo: remove this condition when yxfb optimizations will be disabled
                    current_layout.format != cldnn::format::yxfb && current_layout.batch() == 1) {
            expected_tensor = current_layout.get_tensor();
            expected_format = cldnn::format::byxf;
        } else if (current_layout.format == format::b_fs_yx_fsv4 ||
                    current_layout.format == format::os_is_yx_osv16_isv4) {
            // imad case
            // nothing to do, just go out from here.
        } else if (layout_optimizer::convolution_bfyx_opt(current_layout, weights_layout, prim) ||
                    (_output_size_handling_enabled && prim->with_output_size) || node.get_transposed()) {
            {
                expected_tensor = current_layout.get_tensor();
                if (current_layout.format == format::b_fs_zyx_fsv16 || current_layout.format == format::bs_fs_zyx_bsv16_fsv16)
                    expected_format = cldnn::format::bfzyx;
                else
                    expected_format = cldnn::format::bfyx;
            }

        } else {
            expected_tensor = current_layout.get_tensor();
            expected_format = cldnn::format::yxfb;
        }
    }

    return layout(expected_data_type, expected_format, expected_tensor);
}

layout layout_optimizer::get_expected_layout(layout const& current_layout,
                                             deconvolution_node const& node,
                                             layout const& output_or_weights_layout) {
    auto prim = node.get_primitive();
    auto expected_tensor = current_layout.get_tensor();
    auto expected_data_type = current_layout.data_type;
    auto expected_format = current_layout.format;
    auto input_layout = node.get_dependency(0).get_output_layout();
    auto is_2d = input_layout.format.spatial_num() == 2;
    bool use_onednn_impls = _optimization_attributes.use_onednn_impls;

    if (use_onednn_impls && is_node_for_onednn(node)) {
        if (input_layout.data_type == data_types::f16) {
            if (input_layout.batch() < 16) {
                expected_format = is_2d ? cldnn::format::b_fs_yx_fsv16 : cldnn::format::b_fs_zyx_fsv16;
            } else {
                expected_format = is_2d ? cldnn::format::bs_fs_yx_bsv32_fsv16 : cldnn::format::bs_fs_zyx_bsv32_fsv16;
            }
        } else {
            if (input_layout.batch() < 16) {
                expected_format = is_2d ? cldnn::format::b_fs_yx_fsv32 : cldnn::format::b_fs_zyx_fsv32;
            } else {
                expected_format = is_2d ? cldnn::format::bs_fs_yx_bsv32_fsv32 : cldnn::format::bs_fs_zyx_bsv32_fsv32;
            }
        }
    } else if (_optimization_attributes.b_fs_zyx_fsv16_network &&
        deconvolution_b_fs_zyx_fsv16_opt(current_layout, output_or_weights_layout, prim)) {
        if ((current_layout.data_type == data_types::f32 && expected_tensor.batch[0] % 16 == 0) ||
            (current_layout.data_type == data_types::f16 && expected_tensor.batch[0] % 32 == 0))
            expected_format = cldnn::format::bs_fs_zyx_bsv16_fsv16;
        else
            expected_format = cldnn::format::b_fs_zyx_fsv16;
    } else if (_optimization_attributes.b_fs_yx_fsv16_network &&
               deconvolution_b_fs_yx_fsv16_opt(current_layout, output_or_weights_layout, prim)) {
        auto input_tensor = node.get_dependency(0).get_output_layout().get_tensor();
        int input_features = input_tensor.feature[0];
        int output_features = expected_tensor.feature[0];
        float f_cost = static_cast<float>(input_features * output_features) / (align_to(input_features, 16) * align_to(output_features, 16));
        float stride_cost = 1 / static_cast<float>(prim->stride[prim->stride.size() - 1]);
        if (f_cost * stride_cost > 0.1f)
            expected_format = cldnn::format::b_fs_yx_fsv16;
        else
            expected_format = cldnn::format::bfyx;
    }
    return layout(expected_data_type, expected_format, expected_tensor);
}

layout layout_optimizer::get_expected_layout(layout const& current_layout,
                                             detection_output_node const& node,
                                             layout const& output_or_weights_layout) {
    auto prim = node.get_primitive();
    auto expected_tensor = current_layout.get_tensor();
    auto expected_data_type = data_types::f32;
    auto expected_format = output_or_weights_layout.format;

    return layout(expected_data_type, expected_format, expected_tensor);
}

layout layout_optimizer::get_expected_layout(layout const& current_layout,
                                             binary_convolution_node const& node,
                                             layout const& /*output_or_weights_layout*/) {
    auto prim = node.get_primitive();
    auto expected_tensor = current_layout.get_tensor();
    auto expected_data_type = data_types::bin;
    auto expected_format = cldnn::format::b_fs_yx_32fp;

    return layout(expected_data_type, expected_format, expected_tensor);
}

bool layout_optimizer::are_data_types_suitable_for_onednn(program_node& node) {
    auto in_dt = node.get_dependency(0).get_output_layout().data_type;
    auto out_dt = node.get_output_layout().data_type;

    if (in_dt == data_types::f32 && (!node.is_type<fully_connected>() && !node.is_type<convolution>()))
        return false;

    if (node.is_type<pooling>()) {
        if (!data_type_traits::is_floating_point(in_dt) && in_dt != out_dt)
            return false;
        if ((in_dt == data_types::i8 || in_dt == data_types::u8) && out_dt != data_types::f32)
            return true;
        if (in_dt == data_types::f16 || out_dt == data_types::f16)
            return true;
        if (out_dt == data_types::f32)
            return true;
        if (in_dt == data_types::i32 || out_dt == data_types::i32)
            return true;
        if ((in_dt == data_types::i8 || out_dt == data_types::i8) || (in_dt == data_types::u8 || out_dt == data_types::u8))
            return true;
    } else if (node.is_type<convolution>() || node.is_type<deconvolution>()) {
        bool is_conv = node.is_type<convolution>();
        auto wei_dt = is_conv ? node.as<convolution>().weights().get_output_layout().data_type :
                                node.as<deconvolution>().weights().get_output_layout().data_type;

        if ((in_dt == data_types::f16 && wei_dt == data_types::f16) &&
            (out_dt == data_types::f16 || out_dt == data_types::f32 || out_dt == data_types::i8 || out_dt == data_types::u8))
            return true;
        if ((in_dt == data_types::i8 || in_dt == data_types::u8) && wei_dt == data_types::i8 &&
            (out_dt == data_types::f32 || out_dt == data_types::i32 || out_dt == data_types::f16 || out_dt == data_types::i8 || out_dt == data_types::u8))
            return true;
        if ((in_dt == data_types::f32 && wei_dt == data_types::f32) &&
            (out_dt == data_types::i8 || out_dt == data_types::u8))
            return true;
    } else if (node.is_type<fully_connected>() || node.is_type<gemm>()) {
        bool is_fc = node.is_type<fully_connected>();
        auto wei_dt = is_fc ? node.as<fully_connected>().weights().get_output_layout().data_type :
                              node.as<gemm>().get_dependency(1).get_output_layout().data_type;

        if ((in_dt == data_types::f16 && wei_dt == data_types::f16) &&
            (out_dt == data_types::f16 || out_dt == data_types::f32 || out_dt == data_types::i8))
            return true;
        if (in_dt == data_types::f32 && wei_dt == data_types::f32)
            return true;
        if ((in_dt == data_types::i8 || in_dt == data_types::u8) && (wei_dt == data_types::i8) &&
            (out_dt == data_types::i8 || out_dt == data_types::u8 || out_dt == data_types::i32 || out_dt == data_types::f16 || out_dt == data_types::f32))
            return true;
    }

    return false;
}

bool layout_optimizer::are_layouts_suitable_for_onednn(program_node& node) {
    auto in_padding = node.get_dependencies().front()->get_output_layout().data_padding;
    auto out_padding = node.get_output_layout().data_padding;
    // Check if padding exists
    if (node.get_preferred_impl_type() == impl_types::onednn && (in_padding || out_padding)) {
        bool no_spatial_padding = true;
        for (size_t i = 0; i < in_padding.lower_size().spatial.size(); ++i) {
            no_spatial_padding &= (in_padding.lower_size().spatial[i] == 0);
        }
        for (size_t i = 0; i < in_padding.upper_size().spatial.size(); ++i) {
            no_spatial_padding &= (in_padding.upper_size().spatial[i] == 0);
        }
        for (size_t i = 0; i < out_padding.lower_size().spatial.size(); ++i) {
            no_spatial_padding &= (out_padding.lower_size().spatial[i] == 0);
        }
        for (size_t i = 0; i < out_padding.upper_size().spatial.size(); ++i) {
            no_spatial_padding &= (out_padding.upper_size().spatial[i] == 0);
        }
        bool no_batch_padding = true;
        for (size_t i = 0; i < in_padding.lower_size().batch.size(); ++i) {
            no_batch_padding &= (in_padding.lower_size().batch[i] == 0);
        }
        for (size_t i = 0; i < in_padding.upper_size().batch.size(); ++i) {
            no_batch_padding &= (in_padding.upper_size().batch[i] == 0);
        }
        for (size_t i = 0; i < out_padding.lower_size().batch.size(); ++i) {
            no_batch_padding &= (out_padding.lower_size().batch[i] == 0);
        }
        for (size_t i = 0; i < out_padding.upper_size().batch.size(); ++i) {
            no_batch_padding &= (out_padding.upper_size().batch[i] == 0);
        }
        return (no_spatial_padding && no_batch_padding);
    }
    return true;
}

impl_types layout_optimizer::get_forced_impl_type_by_config(program_node& node) {
#ifdef GPU_DEBUG_CONFIG
    GPU_DEBUG_GET_INSTANCE(debug_config);
    GPU_DEBUG_IF(!debug_config->forced_impl_type.empty()) {
        // Forcing impl type of one primitive
        std::string forced_impl_type = debug_config->forced_impl_type;
        if (node.is_type<fully_connected>()) {
            if (forced_impl_type == "fc:ocl")
                return impl_types::ocl;
            else if (forced_impl_type == "fc:onednn")
                return impl_types::onednn;
        } else if (node.is_type<detection_output>()) {
            if (forced_impl_type == "do:cpu")
                return impl_types::cpu;
            else if (forced_impl_type == "do:ocl")
                return impl_types::ocl;
        } else if (node.is_type<reduce>()) {
            if (forced_impl_type == "reduce:ocl")
                return impl_types::ocl;
            else if (forced_impl_type == "reduce:onednn")
                return impl_types::onednn;
        }

        // Forcing one layer
        size_t found_type = forced_impl_type.rfind(":");
        if (found_type != std::string::npos) {
            impl_types preferred_type = impl_types::any;
            auto impl_type = forced_impl_type.substr(found_type + 1);
            if (impl_type == "ocl")
                preferred_type = impl_types::ocl;
            else if (impl_type == "onednn")
                preferred_type = impl_types::onednn;
            else if (impl_type == "cpu")
                preferred_type = impl_types::cpu;

            if (node.id() == forced_impl_type.substr(0, found_type)) {
                GPU_DEBUG_COUT << " Forced implementation type : " << forced_impl_type.substr(0, found_type) << " : "
                    << forced_impl_type.substr(found_type + 1) << std::endl;
                return preferred_type;
            }
        }
    }
#endif

    return impl_types::any;
}

impl_types layout_optimizer::get_preferred_impl_type(program_node& node, format preferred_format) {
    impl_types preferred_impl = impl_types::any;
    auto forced_impl = get_forced_impl_type_by_config(node);
    if (forced_impl != impl_types::any)
        return forced_impl;

    if (!_forcing_map.empty() && _forcing_map.count(node.id()) != 0) {
        preferred_impl = _forcing_map.at(node.id()).second;
    } else if (node.is_type<detection_output>()) {
        const auto& program = node.get_program();
        const auto& device_info = program.get_engine().get_device_info();
        const int64_t lws_max = device_info.max_work_group_size;
        auto& detection_output_node = node.as<detection_output>();
        auto confidence_layout = detection_output_node.confidence().get_output_layout();
        auto prim = detection_output_node.get_primitive();
        auto batch_size_limitations = (device_info.supports_immad && device_info.execution_units_count >= 256) ? true : confidence_layout.batch() >= 4;
        if (confidence_layout.batch() <= lws_max && batch_size_limitations &&
            prim->confidence_threshold >= 0.1 &&
            prim->top_k <= 400 && prim->num_classes >= 16 && confidence_layout.feature() > 10000)
            preferred_impl = impl_types::ocl;
        else
            preferred_impl = impl_types::cpu;
    } else if (node.is_type<non_max_suppression>()) {
        const std::set<format> blocked_formats = {
            format::b_fs_yx_fsv16,
            format::b_fs_yx_fsv32,
            format::bs_fs_yx_bsv16_fsv16,
            format::bs_fs_yx_bsv32_fsv16,
            format::bs_fs_yx_bsv32_fsv32,
        };
        if (blocked_formats.find(node.get_dependency(0).get_output_layout().format) != blocked_formats.end()) {
            preferred_impl = impl_types::ocl;
        } else {
            auto& nms_node = node.as<non_max_suppression>();
            auto scoresTensor = convert_data_tensor(nms_node.input_scores().get_output_layout());
            const size_t kBatchNum = scoresTensor.Batch().v;
            const size_t kClassNum = scoresTensor.Feature().v;
            const size_t kNStreams =
                static_cast<size_t>(node.get_program().get_engine().configuration().throughput_streams);
            const size_t kKeyValue = kBatchNum * std::min(kClassNum, static_cast<size_t>(8)) * kNStreams;
            preferred_impl = (kKeyValue > 64) ? impl_types::ocl : impl_types::cpu;
        }
    } else if (node.is_type<reorder>()) {
        if (!_optimization_attributes.use_onednn_impls)
            return impl_types::ocl;

        std::vector<format> onednn_optimized_fmt = {
            format::bfyx,
            format::b_fs_zyx_fsv16,
            format::b_fs_yx_fsv16,
            format::b_fs_yx_fsv32,
            format::bs_fs_zyx_bsv8_fsv4,
            format::bs_fs_yx_bsv8_fsv4,
            format::bs_fs_yx_bsv16_fsv4,
            format::bs_fs_zyx_bsv16_fsv4,
            format::bs_fs_yx_bsv16_fsv2,
            format::bs_fs_zyx_bsv16_fsv2,
            format::bs_fs_zyx_bsv8_fsv2,
            format::bs_fs_yx_bsv8_fsv2,
            format::bs_fs_zyx_bsv16_fsv16,
            format::bs_fs_yx_bsv16_fsv16,
            format::bs_fs_zyx_bsv32_fsv16,
            format::bs_fs_yx_bsv32_fsv16,
            format::bs_fs_zyx_bsv32_fsv32,
            format::bs_fs_yx_bsv32_fsv32,
        };

        auto input_layout = node.get_dependency(0).get_output_layout();
        auto output_layout = node.get_output_layout();

        auto input_fmt = input_layout.format;
        auto output_fmt = output_layout.format;
        auto input_dt = input_layout.data_type;
        auto output_dt = output_layout.data_type;

        preferred_impl = impl_types::onednn;

        if (std::find(onednn_optimized_fmt.begin(), onednn_optimized_fmt.end(), input_fmt) == onednn_optimized_fmt.end() ||
            std::find(onednn_optimized_fmt.begin(), onednn_optimized_fmt.end(), output_fmt) == onednn_optimized_fmt.end()) {
            preferred_impl = impl_types::ocl;
        }

        // onednn doesn't support paddings
        if (input_layout.data_padding || output_layout.data_padding) {
            preferred_impl = impl_types::ocl;
        }

        // Native impl works faster for this type of reorder
        if (input_fmt == format::bfyx && output_fmt == format::bfyx) {
            preferred_impl = impl_types::ocl;
        }

        // onednn reorder doesn't support different number of dimensions in input and output layouts
        if (input_fmt.dimension() != output_fmt.dimension()) {
            preferred_impl = impl_types::ocl;
        }

        // For mixed precision case, onednn is slower than cldnn
        if (input_fmt == format::b_fs_yx_fsv16 && data_type_traits::is_i8_u8(input_dt))
            preferred_impl = impl_types::ocl;
        if (output_fmt == format::b_fs_yx_fsv16 && data_type_traits::is_i8_u8(output_dt))
            preferred_impl = impl_types::ocl;
        if (output_fmt == format::bfyx && output_dt == data_types::f32)
            preferred_impl = impl_types::ocl;
    } else if (node.is_type<reduce>()) {
        if (!_optimization_attributes.use_onednn_impls)
            return impl_types::ocl;

        if (is_node_for_onednn(node.as<reduce>(), preferred_format))
            return impl_types::onednn;
        else
            return impl_types::ocl;
    } else if (node.is_type<pooling>() || node.is_type<convolution>() || node.is_type<deconvolution>()) {
        if (!_optimization_attributes.use_onednn_impls)
            return impl_types::ocl;

        std::vector<format> onednn_optimized_formats = {
            format::byxf,
            format::bzyxf,
            format::b_fs_zyx_fsv32,
            format::b_fs_yx_fsv32,
            format::b_fs_zyx_fsv16,
            format::b_fs_yx_fsv16,
            format::bs_fs_zyx_bsv16_fsv16,
            format::bs_fs_yx_bsv16_fsv16,
            format::bs_fs_zyx_bsv32_fsv16,
            format::bs_fs_yx_bsv32_fsv16,
            format::bs_fs_zyx_bsv32_fsv32,
            format::bs_fs_yx_bsv32_fsv32,
            format::bs_fs_zyx_bsv8_fsv4,
            format::bs_fs_yx_bsv8_fsv4,
            format::bs_fs_yx_bsv16_fsv4,
            format::bs_fs_zyx_bsv16_fsv4,
            format::bs_fs_yx_bsv16_fsv2,
            format::bs_fs_zyx_bsv16_fsv2,
            format::bs_fs_zyx_bsv8_fsv2,
            format::bs_fs_yx_bsv8_fsv2,
            format::bs_fs_yx_bsv4_fsv4,
            format::bs_fs_yx_bsv4_fsv2,
        };

        impl_types impl_candidate = impl_types::onednn;

        // Unexpected layout
        if (std::find(onednn_optimized_formats.begin(), onednn_optimized_formats.end(), preferred_format) == onednn_optimized_formats.end()) {
            impl_candidate = impl_types::ocl;
        }

        // WA: fallback cldnn if onednn pooling's layout is b_fs_zyx_fsv32 and i8.
        if (node.is_type<pooling>()) {
            auto pool_layout = node.get_output_layout();
            if (pool_layout.data_type == data_types::i8) {
                if (pool_layout.format == format::b_fs_zyx_fsv32 || pool_layout.format == format::bs_fs_zyx_bsv32_fsv32)
                    impl_candidate = impl_types::ocl;
            }
        }

        if (node.is_type<deconvolution>()) {
            if (!is_node_for_onednn(node.as<deconvolution>()))
                impl_candidate = impl_types::ocl;
        }

        // [WA] oneDNN doesn't support > 32 post-ops. Remove once oneDNN improve post-ops for GPU.
        if (get_post_ops_count(node) > 32) {
           impl_candidate = impl_types::ocl;
        }

        if (!are_data_types_suitable_for_onednn(node)) {
            impl_candidate = impl_types::ocl;
        }

        for (auto& fo : node.get_fused_primitives()) {
            if (fo.is_type<activation>()) {
                // Some activations aren't implemented in oneDNN
                auto activation_prim = fo.typed_desc<activation>();
                if (activation_prim->activation_function == activation_func::negative ||
                    activation_prim->activation_function == activation_func::negation ||
                    activation_prim->activation_function == activation_func::sign)
                    impl_candidate = impl_types::ocl;
            }
        }

        // oneDNN doesn't support asymmetric weights quantization
        if (node.is_type<convolution>() && node.as<convolution>().weights_zero_points_term())
            impl_candidate = impl_types::ocl;

        preferred_impl = impl_candidate;
    } else if (node.is_type<concatenation>()) {
        if (!_optimization_attributes.use_onednn_impls)
            return impl_types::ocl;

        for (auto& dep : node.get_dependencies()) {
            if (dep->is_in_data_flow() && dep->get_preferred_impl_type() == impl_types::onednn) {
                return impl_types::onednn;
            }
        }
        if (format::is_blocked(node.get_output_layout().format)) {
            return impl_types::onednn;
        }
    // TODO: uncomment this code when onednn gemm implementations will have real perf improvements vs cldnn
    } else if (node.is_type<fully_connected>() || node.is_type<gemm>()) {
        if (!_optimization_attributes.use_onednn_impls)
            return impl_types::ocl;

        impl_types impl_candidate = impl_types::onednn;

        if (!are_data_types_suitable_for_onednn(node)) {
            impl_candidate = impl_types::ocl;
        }

        if (node.is_type<fully_connected>()) {
            if (!is_node_for_onednn(node, node.as<fully_connected>()))
                impl_candidate = impl_types::ocl;

            // WA : Use cldnn FC due to perf drop of small batch size until onednn FC improve perf
            if (node.get_output_layout().batch() < 32)
                impl_candidate = impl_types::ocl;
        } else {
            for (auto& fo : node.get_fused_primitives()) {
                if (fo.is_type<eltwise>()) {
                    // Gemm checkings
                    // TODO: investigate why currently onednn gemm has some "sum" post-op restrictions
                    // which don't correlate with fc checkings in the code above
                    // Temprorary WA: disable onednn gemm with sum post-op inside
                    if (fo.typed_desc<eltwise>()->mode == eltwise_mode::sum) {
                        impl_candidate = impl_types::ocl;
                        break;
                    }
                }
            }

            auto gemm_prim = node.as<gemm>().get_primitive();
            auto in0_l = node.get_dependency(0).get_output_layout();
            auto in1_l = node.get_dependency(1).get_output_layout();
            auto out_l = node.get_output_layout();
            auto has_input2 = gemm_prim->dependencies().size() == 3;
            size_t in2_batched_size = 0;
            if (has_input2) {
                auto in2_l = node.get_dependency(2).get_output_layout();
                in2_batched_size = in2_l.count() / (in2_l.spatial(0) * in2_l.spatial(1));
            }
            size_t size_k = gemm_prim->transpose_input0 ? in0_l.spatial(1) : in0_l.spatial(0);

            size_t in0_batched_size = in0_l.count() / (in0_l.spatial(0) * in0_l.spatial(1));
            size_t in1_batched_size = in1_l.count() / (in1_l.spatial(0) * in1_l.spatial(1));
            size_t out_batched_size = out_l.count() / (out_l.spatial(0) * out_l.spatial(1));

            auto valid_input_batch = in0_batched_size != 1 && (in1_batched_size == in0_batched_size || in1_batched_size == 1);
            auto valid_output_batch = in0_batched_size > in1_batched_size ? out_batched_size == in0_batched_size :
                                                                        out_batched_size == in1_batched_size;
            auto valid_extra_input_batch = has_input2 ? in2_batched_size == 1 || in2_batched_size == out_batched_size : true;
            auto valid_scale_factor = gemm_prim->alpha == 1.f && (has_input2 ? gemm_prim->beta == 1.f : true);
            auto unsupported_onednn_gemm = !valid_input_batch ||
                                           !valid_output_batch ||
                                           !valid_extra_input_batch ||
                                           !valid_scale_factor;

            bool is_u8_i8 = data_type_traits::is_i8_u8(in0_l.data_type) && data_type_traits::is_i8_u8(in1_l.data_type);
            bool use_ops_cldnn_kernel = is_u8_i8 || (in0_l.spatial(0) % 16 == 0 && in0_l.spatial(1) % 16 == 0 &&
                                                     in1_l.spatial(0) % 16 == 0 && in1_l.spatial(1) % 16 == 0);

            // Gemm with k < 64 may be faster in cldnn unless ref impl is used
            if ((size_k < 64 && use_ops_cldnn_kernel) || unsupported_onednn_gemm) {
                impl_candidate = impl_types::ocl;
            }
        }

        preferred_impl = impl_candidate;
    }

    return preferred_impl;
}

format layout_optimizer::get_preferred_format(program_node& node) {
    format expected = format::any;
    auto output_layout = node.get_output_layout();
    bool use_onednn_impls = _optimization_attributes.use_onednn_impls;

    if (!_forcing_map.empty() && _forcing_map.count(node.id()) != 0) {
        expected = _forcing_map.at(node.id()).first;
    } else if (node.is_type<convolution>()) {
        auto& conv_node = node.as<convolution>();
        auto weights_layout = conv_node.weights(0).get_output_layout().convert_to_weights_layout(conv_node.get_primitive()->grouped_weights_shape);
        expected = get_expected_layout(output_layout, conv_node, weights_layout).format;
    } else if (node.is_type<binary_convolution>()) {
        auto& bconv_node = node.as<binary_convolution>();
        auto weights_layout = bconv_node.weights(0).get_output_layout().convert_to_weights_layout(false);
        expected = get_expected_layout(output_layout, bconv_node, weights_layout).format;
    } else if (node.is_type<detection_output>()) {
        expected = get_expected_layout(
            output_layout,
            node.as<detection_output>(),
            layout{ data_types::f32, format::bfyx, tensor{} }).format;
    } else if (node.is_type<quantize>()) {
        auto layout = node.get_output_layout();

        std::function<bool(const program_node& node)> only_gemm_users = [&](const program_node& node) {
            bool all_users_gemm = true;

            for (auto user : node.get_users()) {
                if (user->is_type<reorder>() || user->is_type<reshape>())
                    all_users_gemm &= only_gemm_users(*user);
                else if (user->is_type<gemm>())
                    all_users_gemm &= true;
                else
                    return false;
            }

            return all_users_gemm;
        };

        if (use_onednn_impls) {
            if (node.get_users().front()->get_required_input0() != format::any) {
                expected = node.get_users().front()->get_required_input0();
            } else {
                expected = format::any;
            }
        } else if (only_gemm_users(node)) {
            // TODO: Gemm is not supporting fsv layouts
            if (node.get_output_layout().format.dimension() == 6) {
                expected = format::bfwzyx;
            } else if (node.get_output_layout().format.dimension() == 5) {
                expected = format::bfzyx;
            } else if (node.get_output_layout().format.dimension() == 4) {
                expected = format::bfyx;
            }
            // TODO: check other types for first conv
        } else if (layout.is_static() && layout.format.spatial_num() == 2 &&
                  (layout.data_type == data_types::i8 || layout.data_type == data_types::u8) &&
                  layout.batch() % 16 == 0) {
            if (use_onednn_impls && layout.batch() % 32 == 0) {
                if (node.get_users().size() == 1 && node.get_users().front()->is_type<convolution>()) {
                    auto& conv = node.get_users().front()->as<convolution>();
                    auto ws = conv.get_dependency(1).get_output_layout().get_tensor();
                    if (ws.spatial[0] != 7 || conv.get_primitive()->groups > 1 || layout.feature() == 1)
                        expected = format::bfyx;
                    else
                        expected = format::bs_fs_yx_bsv16_fsv4;

                    auto conv_output_layout = conv.get_output_layout();
                    auto weights_layout = conv.weights(0).get_output_layout().convert_to_weights_layout(conv.get_primitive()->grouped_weights_shape);
                    format expected_conv_fmt = get_expected_layout(conv_output_layout, conv, weights_layout).format;
                    if (expected == format::bfyx && expected_conv_fmt == format::bs_fs_yx_bsv32_fsv32 && layout.feature() % 32 == 0)
                        expected = expected_conv_fmt;
                }
            } else if (layout.feature() > 8) {
                expected = format::b_fs_yx_fsv16;
            } else {
                expected = format::b_fs_yx_fsv4;
            }
        } else if (layout.format.spatial_num() == 3 && (layout.data_type == data_types::i8 || layout.data_type == data_types::u8)) {
            expected = format::b_fs_zyx_fsv16;
        }

        // In case of input -> ... -> quantize -> concat
        if (expected == format::any
            && (node.get_users().size() == 1 && node.get_users().front()->is_type<concatenation>())
            && (layout.batch() < 4 && layout.feature() < 4)) {
                expected = format::get_default_format(layout.get_rank(), false, false);
        }
    } else if (node.is_type<reorder>() || node.is_type<input_layout>()) {
        expected = node.get_output_layout().format;
    } else if (node.is_type<reshape>()) {
        if (node.get_output_layout().format.dimension() == 6) {
            expected = format::bfwzyx;
        } else if (node.get_output_layout().format.dimension() == 5) {
            expected = format::bfzyx;
        } else if (node.get_output_layout().format.dimension() == 4) {
            expected = format::bfyx;
        }
    } else if (node.is_type<deconvolution>()) {
        auto& deconv_node = node.as<deconvolution>();
        auto weights_layout = deconv_node.weights(0).get_output_layout().convert_to_weights_layout(deconv_node.get_primitive()->grouped_weights_shape);
        expected = get_expected_layout(output_layout, deconv_node, weights_layout).format;
    } else if (node.is_type<mvn>()) {
        auto input_layout = node.get_dependency(0).get_output_layout();
        if (input_layout.format.dimension() == 5 &&
            (input_layout.data_type == data_types::f32 || input_layout.data_type == data_types::f16))
            expected = format::bfzyx;
    } else if (node.is_type<resample>()) {
        // if the resample is in the last part of the network and there are no users using blocked format,
        // it is better to reorder to bfyx before resample is done.
        if (all_users_simple_format_until_output(node, node, 0, 10)) {
            const auto& dim = format::dimension(node.get_output_layout().format);
            expected = format::get_default_format(dim, false, false);
        } else {
            expected = format::any;
        }
    } else if (node.is_type<permute>()) {
        if (node.get_dependencies().size() == 1 && node.get_dependencies().front()->is_type<convolution>()) {
            auto& conv_node = node.get_dependencies().front()->as<convolution>();
            const auto& fmt = get_preferred_format(conv_node);
            // if the preferred format of the previous conv of permute is fs_b_yx_fsv32,
            // it is better to set to b_fs_yx_fsv32 that supports tiled permute (permute_tile_8x8_4x4_fsv)
            // because fs_b_yx_fsv32 is only supported by permute_ref.
            if (node.as<permute>().is_rotating_except_batch() && fmt == format::fs_b_yx_fsv32) {
                expected = format::b_fs_yx_fsv32;
            }
        }
    } else if (node.is_type<reduce>()) {
        auto& reduce_node = node.as<reduce>();
        // if blocked axes are reduced, it will have huge memory overhead. A clDNN reduce reorders un-reduced axes to b-f and w-x axis for this.
        // But oneDNN does not allow this. So planar format is used for this case.
        if (is_reduce_blocked_axes(reduce_node) && use_onednn_impls) {
            auto input_layout = reduce_node.input().get_output_layout();
            if (input_layout.format.dimension() == 6)
                expected = format::bfwzyx;
            else if (input_layout.format.dimension() == 5)
                expected = format::bfzyx;
            else if (input_layout.format.dimension() == 4)
                expected = format::bfyx;
        }
    }

    return expected;
}

bool layout_optimizer::all_users_simple_format_until_output(program_node& origin_node, program_node& cur_node, int32_t cur_depth, int32_t max_depth) {
    if (cur_node.is_output()) return true;
    if (cur_depth > max_depth) return false;

    if (cur_node.is_type<permute>()) {
        if (!cur_node.as<permute>().is_rotating_except_batch())
            return false;
    }

    if (cur_node.is_in_data_flow() && (cur_node.type() != origin_node.type())) {
        const auto& fmt = get_preferred_format(cur_node);
        if (fmt != format::any && !format::is_simple_data_format(fmt)) {
            return false;
        }
    }

    bool res = true;
    for (const auto& usr : cur_node.get_users()) {
        res &= all_users_simple_format_until_output(origin_node, *usr, cur_depth + 1, max_depth);
    }
    return res;
}

void layout_optimizer::set_optimization_attribute(optimization_attributes_type attribute, int32_t val) {
    switch (attribute) {
        case optimization_attributes_type::splitted_convolution:
            _optimization_attributes.splitted_convolution = val;
            break;
        case optimization_attributes_type::group_convolution:
            _optimization_attributes.group_convolution = val;
            break;
        case optimization_attributes_type::deformable_convolution:
            _optimization_attributes.deformable_convolution = val;
            break;
        case optimization_attributes_type::bfyx_only_layer:
            _optimization_attributes.bfyx_only_layer = val;
            break;
        case optimization_attributes_type::fs_b_yx_fsv32_network:
            _optimization_attributes.fs_b_yx_fsv32_network = val;
            break;
        case optimization_attributes_type::b_fs_zyx_fsv32_network:
            _optimization_attributes.b_fs_zyx_fsv32_network = val;
            break;
        case optimization_attributes_type::b_fs_yx_fsv16_network:
            _optimization_attributes.b_fs_yx_fsv16_network = val;
            break;
        case optimization_attributes_type::b_fs_zyx_fsv16_network:
            _optimization_attributes.b_fs_zyx_fsv16_network = val;
            break;
        case optimization_attributes_type::bs_fs_yx_bsv16_fsv16_network:
            _optimization_attributes.bs_fs_yx_bsv16_fsv16_network = val;
            break;
        case optimization_attributes_type::use_onednn_impls:
            _optimization_attributes.use_onednn_impls = val;
            break;
        default:
            throw std::out_of_range("unsupported layout optimization attribute");
    }
}

bool layout_optimizer::is_format_optimized(const convolution_node& node, const format& format, bool use_weak_restrictions) {
    auto input_layout = node.input().get_output_layout();
    auto weights_layout = node.weights().get_output_layout();
    auto output_layout = node.calc_output_layout();
    auto prim = node.get_primitive();

    switch (format) {
        case format::b_fs_yx_fsv16:
            return convolution_b_fs_yx_fsv16_opt(input_layout, output_layout, weights_layout, prim, use_weak_restrictions) &&
                   // Work-around for inability to use b_fs_yx_fsv16 and winograd together
                   !should_use_winograd_2x3_s1(prim, input_layout, weights_layout, _output_size_handling_enabled);
        case format::b_fs_zyx_fsv16:
        case format::bs_fs_zyx_bsv16_fsv16:
            return convolution_b_fs_zyx_fsv16_opt(input_layout, output_layout, weights_layout, prim);
        case format::fs_b_yx_fsv32:
            return convolution_fs_b_yx_fsv32_opt(input_layout, output_layout, weights_layout, prim);
        case format::bs_fs_yx_bsv16_fsv16:
            return convolution_bs_fs_yx_bsv16_fsv16_opt(input_layout, output_layout, weights_layout, prim);
        case format::bs_fs_yx_bsv32_fsv32:
        case format::bs_fs_yx_bsv32_fsv16:
            return false;
        default:
            throw std::invalid_argument(
                "[Layout optimizer] Other formats in is_format_optimized(...) method are not implemented!");
    }
}

void layout_optimizer::update_formats_map(const convolution_node &node) {
    for (auto& format : optimized_formats) {
        if (is_format_optimized(node, format.first, format.second)) {
            _optimized_conv_count.at(format)++;
        }
    }
    _total_conv++;
}

size_t layout_optimizer::get_total_conv_count() {
    return _total_conv;
}

size_t layout_optimizer::get_optimized_conv_count(const std::pair<format::type, bool>& format) {
    if (_optimized_conv_count.count(format) > 0) {
        return _optimized_conv_count.at(format);
    }
    return 0;
}

bool layout_optimizer::is_format_optimized(const deconvolution_node& node, const format& format) {
    auto input_layout = node.input().get_output_layout();
    auto weights_layout = node.weights().get_output_layout();
    auto prim = node.get_primitive();

    switch (format) {
    case format::b_fs_zyx_fsv16:
    case format::bs_fs_zyx_bsv16_fsv16:
        return deconvolution_b_fs_zyx_fsv16_opt(input_layout, weights_layout, prim);
    case format::b_fs_yx_fsv16:
        return deconvolution_b_fs_yx_fsv16_opt(input_layout, weights_layout, prim);
    default:
        throw std::invalid_argument(
            "[Layout optimizer] Other formats in is_format_optimized(...) method are not implemented!");
    }
}

void layout_optimizer::set_implementation_forcing(const implementation_forcing_map& map) {
    for (const auto& kv : map) {
        _forcing_map.emplace(kv.first, std::make_pair(kv.second.output_format, kv.second.impl_type));
    }
}

const std::vector<std::pair<format::type, bool>> layout_optimizer::optimized_formats = {
        {format::b_fs_yx_fsv16, true},
        {format::b_fs_yx_fsv16, false},
        {format::b_fs_zyx_fsv16, false},
        {format::bs_fs_zyx_bsv16_fsv16, false},
        {format::bs_fs_yx_bsv16_fsv16, false},
        {format::fs_b_yx_fsv32, false}
};
