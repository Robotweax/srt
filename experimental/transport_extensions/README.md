# Experimental compiled congestion controllers

Private, source-tree-only C++ contracts. They are not installed plugin APIs.
See [controller integration and provider rules](../../docs/experimental-controllers.md).

`ROBOTWEAX_SRT_BUILD_EXTENSION_PROTOTYPES=ON` builds only the bounded controller
contract and its tests. `ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_CONTROLLERS=ON` also
enables the compiled provider registry and individual-socket runtime binding.
Both flags default to OFF. Packet-filter extensions are a separate feature.
