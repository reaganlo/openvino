# OpenVINO™ Core Components

This section provides references and information about OpenVINO core components.

```
bindings/           // OpenVINO bindings
cmake/              // Common cmake scripts
common/             // Common components
core/               // OpenVINO core component provides model representation, operations and other core functionality
frontends/          // OpenVINO frontends
inference/          // Provides API for model inference
plugins/            // OpenVINO plugins
tests/              // A backed of tests binaries for core and plugins
tests_deprecated/   // Deprecated tests
```

## OpenVINO Runtime library

OpenVINO Runtime is a common OpenVINO library which provides functionality for Neural Network inference. The library includes next parts:

```mermaid
flowchart LR
    subgraph openvino [openvino library]
        core
        inference
        transformations[Common transformations]
        lp_transformations[LP transformations]
        frontend_common
        style frontend_common fill:#7f9dc0,stroke:#333,stroke-width:4px
        style transformations fill:#3d85c6,stroke:#333,stroke-width:4px
        style lp_transformations fill:#0b5394,stroke:#333,stroke-width:4px
        style core fill:#679f58,stroke:#333,stroke-width:4px
        style inference fill:#d7a203,stroke:#333,stroke-width:4px
    end
```

 * [core](./core/README.md) is responsible for model representation, contains a set of supported OpenVINO operations and base API for model modification.
 * [inference](./inference) provides the API for model inference on different accelerators.
 * Transformations:
    * [common transformations](../src/common/transformations) - a set of common transformations which are used for model optimization
    * [low precision transformations](../src/common/low_precision_transformations) - a set of transformations which are needed to optimize quantized models
 * **frontend_common** provides frontend common API which allows to support frontends for different frameworks.

## OpenVINO Frontends

OpenVINO Frontends allow to convert model from framework to OpenVINO representation.

 * [ir](./frontends/ir)
 * [onnx](./frontends/onnx)
    
## OpenVINO Plugins

Plugins provide a support of hardware device
 * [intel_cpu](./plugins/intel_cpu)

## OpenVINO Bindings

OpenVINO provides bindings for several languages:

 * [c](./bindings/c)
 * [python](./bindings/python)

## Core developer topics

 * [OpenVINO architecture](./docs/architecture.md)
 * [Plugin Development](https://docs.openvino.ai/latest/openvino_docs_ie_plugin_dg_overview.html)
 * [Thread safety](#todo)
 * [Performance](#todo)

## See also
 * [OpenVINO™ README](../README.md)
 * [Developer documentation](../docs/dev/index.md)
