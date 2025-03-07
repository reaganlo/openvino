// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//
#include "ov_test.hpp"

TEST(ov_model, ov_model_const_input) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* input_port = nullptr;
    OV_EXPECT_OK(ov_model_const_input(model, &input_port));
    ASSERT_NE(nullptr, input_port);

    ov_output_const_port_free(input_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_const_input_by_name) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* input_port = nullptr;
    OV_EXPECT_OK(ov_model_const_input_by_name(model, "data", &input_port));
    ASSERT_NE(nullptr, input_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_const_port_get_shape(input_port, &shape));
    ov_shape_free(&shape);

    ov_output_const_port_free(input_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_const_input_by_index) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* input_port = nullptr;
    OV_EXPECT_OK(ov_model_const_input_by_index(model, 0, &input_port));
    ASSERT_NE(nullptr, input_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_const_port_get_shape(input_port, &shape));
    ov_shape_free(&shape);

    ov_output_const_port_free(input_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_input) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_port_t* input_port = nullptr;
    OV_EXPECT_OK(ov_model_input(model, &input_port));
    ASSERT_NE(nullptr, input_port);

    ov_output_port_free(input_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_input_by_name) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_port_t* input_port = nullptr;
    OV_EXPECT_OK(ov_model_input_by_name(model, "data", &input_port));
    ASSERT_NE(nullptr, input_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_port_get_shape(input_port, &shape));
    ov_shape_free(&shape);

    ov_output_port_free(input_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_input_by_index) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_port_t* input_port = nullptr;
    OV_EXPECT_OK(ov_model_input_by_index(model, 0, &input_port));
    ASSERT_NE(nullptr, input_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_port_get_shape(input_port, &shape));
    ov_shape_free(&shape);

    ov_output_port_free(input_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_const_output) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* output_port = nullptr;
    OV_EXPECT_OK(ov_model_const_output(model, &output_port));
    ASSERT_NE(nullptr, output_port);

    ov_output_const_port_free(output_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_const_output_by_index) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* output_port = nullptr;
    OV_EXPECT_OK(ov_model_const_output_by_index(model, 0, &output_port));
    ASSERT_NE(nullptr, output_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_const_port_get_shape(output_port, &shape));
    ov_shape_free(&shape);

    ov_output_const_port_free(output_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_const_output_by_name) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* output_port = nullptr;
    OV_EXPECT_OK(ov_model_const_output_by_name(model, "fc_out", &output_port));
    ASSERT_NE(nullptr, output_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_const_port_get_shape(output_port, &shape));
    ov_shape_free(&shape);

    ov_output_const_port_free(output_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_output) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_port_t* output_port = nullptr;
    OV_EXPECT_OK(ov_model_output(model, &output_port));
    ASSERT_NE(nullptr, output_port);

    ov_output_port_free(output_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_output_by_index) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_port_t* output_port = nullptr;
    OV_EXPECT_OK(ov_model_output_by_index(model, 0, &output_port));
    ASSERT_NE(nullptr, output_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_port_get_shape(output_port, &shape));
    ov_shape_free(&shape);

    ov_output_port_free(output_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_output_by_name) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_port_t* output_port = nullptr;
    OV_EXPECT_OK(ov_model_output_by_name(model, "fc_out", &output_port));
    ASSERT_NE(nullptr, output_port);

    ov_shape_t shape;
    OV_EXPECT_OK(ov_port_get_shape(output_port, &shape));
    ov_shape_free(&shape);

    ov_output_port_free(output_port);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_inputs_size) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    size_t input_size;
    OV_EXPECT_OK(ov_model_inputs_size(model, &input_size));
    ASSERT_NE(0, input_size);

    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_outputs_size) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    size_t output_size;
    OV_EXPECT_OK(ov_model_outputs_size(model, &output_size));
    ASSERT_NE(0, output_size);

    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_is_dynamic) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ASSERT_NO_THROW(ov_model_is_dynamic(model));

    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_reshape_input_by_name) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    ov_output_const_port_t* input_port_1 = nullptr;
    OV_EXPECT_OK(ov_model_const_input(model, &input_port_1));
    ASSERT_NE(nullptr, input_port_1);

    char* tensor_name = nullptr;
    OV_EXPECT_OK(ov_port_get_any_name(input_port_1, &tensor_name));

    ov_shape_t shape = {0, nullptr};
    int64_t dims[4] = {1, 3, 896, 896};
    OV_EXPECT_OK(ov_shape_create(4, dims, &shape));

    ov_partial_shape_t partial_shape;
    OV_EXPECT_OK(ov_shape_to_partial_shape(shape, &partial_shape));
    OV_EXPECT_OK(ov_model_reshape_input_by_name(model, tensor_name, partial_shape));

    ov_output_const_port_t* input_port_2 = nullptr;
    OV_EXPECT_OK(ov_model_const_input(model, &input_port_2));
    ASSERT_NE(nullptr, input_port_2);

    EXPECT_NE(input_port_1, input_port_2);

    ov_shape_free(&shape);
    ov_partial_shape_free(&partial_shape);
    ov_free(tensor_name);
    ov_output_const_port_free(input_port_1);
    ov_output_const_port_free(input_port_2);
    ov_model_free(model);
    ov_core_free(core);
}

TEST(ov_model, ov_model_get_friendly_name) {
    ov_core_t* core = nullptr;
    OV_EXPECT_OK(ov_core_create(&core));
    ASSERT_NE(nullptr, core);

    ov_model_t* model = nullptr;
    OV_EXPECT_OK(ov_core_read_model(core, xml, bin, &model));
    ASSERT_NE(nullptr, model);

    char* friendly_name = nullptr;
    OV_EXPECT_OK(ov_model_get_friendly_name(model, &friendly_name));
    ASSERT_NE(nullptr, friendly_name);

    ov_free(friendly_name);
    ov_model_free(model);
    ov_core_free(core);
}