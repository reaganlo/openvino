// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "quantize_inst.h"
#include "binary_convolution_inst.h"
#include "primitive_type_base.h"
#include "intel_gpu/runtime/memory.hpp"
#include "intel_gpu/runtime/error_handler.hpp"
#include "json_object.h"
#include "data_inst.h"
#include <string>

namespace cldnn {
primitive_type_id quantize::type_id() {
    static primitive_type_base<quantize> instance;
    return &instance;
}

layout quantize_inst::calc_output_layout(quantize_node const& node, kernel_impl_params const& impl_param) {
    auto desc = impl_param.typed_desc<quantize>();

    auto input_layout = impl_param.get_input_layout();
    auto output_format = input_layout.format;
    auto out_dt = input_layout.data_type;
    if (desc->output_data_type)
        out_dt = *desc->output_data_type;

    if (out_dt == data_types::bin) {
        output_format = format::b_fs_yx_32fp;
    }

    return layout{out_dt, output_format, input_layout.get_tensor()};
}

template<typename ShapeType>
std::vector<layout> quantize_inst::calc_output_layouts(quantize_node const&, kernel_impl_params const& impl_param) {
    auto desc = impl_param.typed_desc<quantize>();

    auto input_layout = impl_param.get_input_layout();
    auto output_format = input_layout.format;
    auto out_dt = desc->output_data_type.value_or(input_layout.data_type);

    if (out_dt == data_types::bin) {
        output_format = format::b_fs_yx_32fp;
    }

    return { layout{input_layout.get<ShapeType>(), out_dt, output_format} };
}

std::string quantize_inst::to_string(quantize_node const& node) {
    auto desc = node.get_primitive();
    auto node_info = node.desc_to_json();
    auto& input = node.input(0);
    auto& input_low = node.input(1);
    auto& input_high = node.input(2);
    auto& output_low = node.input(3);
    auto& output_high = node.input(4);
    auto scale_shift_opt = node.get_scale_shift_opt() ? "true" : "false";

    std::stringstream primitive_description;

    json_composite quantize_info;
    quantize_info.add("input id", input.id());
    quantize_info.add("input low id", input_low.id());
    quantize_info.add("input high id", input_high.id());
    quantize_info.add("output low id", output_low.id());
    quantize_info.add("output high id", output_high.id());
    quantize_info.add("scale_shift_opt", scale_shift_opt);
    quantize_info.add("levels", desc->levels);

    node_info->add("quantize info", quantize_info);
    node_info->dump(primitive_description);

    return primitive_description.str();
}

quantize_inst::typed_primitive_inst(network& network, quantize_node const& node) : parent(network, node) {}

}  // namespace cldnn
