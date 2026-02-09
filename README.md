# EZ SDK

## Overview

The Encrypted Zone Software Development Kit (EZ SDK) is a versioned, public artifact that simplifies
building private data processing Isolates by providing an abstraction layer for "lift-and-shift"
migrations with minimal code changes. The SDK includes the EZ Isolate Protobuf Interface for
defining RPC interactions, language-specific integration code for C++ and Rust, and the Isolate
Bundle Interface for binary packaging. Additionally, it provides the EzManifest (EZ Deployment
Manifest Spec), which allows operators to configure Isolates and their runtime environments.

For a deeper look into the EZ and EZ SDK, we recommend reading the
[EZ Node readme.](https://github.com/private-compute-infra-toolkit/encrypted-zone-node/blob/main/README.md)

## Development Workflow

To begin your integration, visit the [Get Started with EZ SDK](docs/get-started.md) guide. This
guide serves as an adoption resource for EZ SDK users to transform an existing gRPC implementation
of a service defined using protobuf Interface Definition Language (IDL) into a secure EZ isolate.

## License

Apache 2.0 - See [LICENSE](LICENSE) for more information.
