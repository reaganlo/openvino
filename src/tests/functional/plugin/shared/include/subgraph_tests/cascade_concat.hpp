// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include "shared_test_classes/subgraph/cascade_concat.hpp"

namespace SubgraphTestsDefinitions {

TEST_P(CascadeConcat, CompareWithRefs) {
    Run();
}

TEST_P(CascadeConcatWithMultiConnReshape, CompareWithRefs) {
    Run();
}

} // namespace SubgraphTestsDefinitions
