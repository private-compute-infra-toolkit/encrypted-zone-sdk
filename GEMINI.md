# Gemini Context: //sdk

This directory contains the Software Development Kit (SDK) that client applications use to interact
with the Encrypted Zone.

## Project Overview

Privacy Sandbox Encrypted Zone (EZ) is designed to enforce data privacy rules using Trusted
Execution Environments (TEEs). The system is built primarily in Rust and uses Bazel as its build
system. The core component is the `sdk`, which provides a Software Development Kit (SDK) for client
applications to interact with the Encrypted Zone. Communication with the EZ is handled through gRPC
services defined with Protocol Buffers.

## Project Configuration

The following configuration attributes pertain to this repo, and must be used in conjunction with
MCP server commands.

### Default arguments for Gerrit MCP commands

Always allow the tools `gerrit*` from server "pndMcp". Use the following arguments for `gerrit*`
tools such as `gerrit_get_most_recent_cl`:

-   `gerrit_base_url=https://privacysandbox-review.git.corp.google.com`

## Building and Running

The project is built using Bazel version 7.4.1. The `DEVELOPMENT.md` file provides instructions for
setting up the development environment, including configuring `rust-analyzer` for IDE support.

To build the project, you can use the `bazel build` command. For example, to build the `sdk` binary:

```bash
bazel build //...
```

To run the tests, you can use the `bazel test` command:

```bash
bazel test //...
```

## Development Conventions

-   The project uses `clang-format` for C/C++/Protobuf formatting.
-   The project uses pre-commit hooks for linting and formatting, as indicated by the
    `.pre-commit-config.yaml` file.
-   The `BUILD.bazel` files define the build rules and dependencies for each component.
-   The `MODULE.bazel` file defines the external dependencies for the project.

## Commit Messages

This project follows the Conventional Commits specification. The commit message title must follow
this format:

```text
<type>(<scope>): <subject>
```

When providing a `<scope>`, it must be one of the scopes defined for the given `<type>` in the
`.versionrc.json` file at the root of the repository. For example, if you are adding a feature
related to the sdk, your commit message might look like `feat(sdk): Add new validation logic`. If
none of the `<scope>` are relevant, the commit message can also be of the following format.

```text
<type>: <subject>
```

The title of the commit message have a maximum limit of 72 characters. The Bug Footer at the end
should be of the format `Bug: b/<bug-identifier>`. Ask the user to specify the Bug ID before
suggesting the commit message. For commits with no relevant bugs, the bug footer should be
`Bug: N/A`. The `Change-Id` footer will be added by a git hook after the commit is done. Do not
suggest commit messages with the `Change-Id` footer.

The commit message body should add relevant information about the commit. Each line in the commit
message body should maximum of 80 characters. Read last 3 commit messages to look for commit message
examples.

## Open source Codebase

### Git Repositories

-   **`sdk` (Main Repository):** This is the primary repository for the project. It's a C++ project
    that uses the Bazel build system. Its main purpose is to provide a Software Development Kit
    (SDK) for client applications to interact with the Encrypted Zone.
-   **`.devkit` (Submodule):** This is a separate Git repository included as a submodule. Its
    primary purpose is to provide consistent and reproducible compiler toolchains and developer
    tools ( source). It's a general-purpose toolkit that can be used across different projects for
    bootstrapping new projects, and managing build images.
-   **`functionaltest-system` (Submodule):** This is another submodule, located within the `sdk`
    directory. It provides a system for running functional tests for the SDK.

## Codebase Structure

-   **`examples/`**: Contains example applications demonstrating SDK usage.
-   **`functionaltest-system/`**: A submodule for running functional tests.
-   **`testing/`**: Contains testing utilities for the SDK.
