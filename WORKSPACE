workspace(name = "privacy-sandbox-ez-sdk")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# Oak repository for OCI oci_runtime_bundle.
# Sadly Oak is not Bzlmod ready yet.
http_archive(
    name = "oak",
    sha256 = "c980e70cb535eac5185aee397fa577874ee37a38955161938cd89784ab5f33de",
    # commit 439c15a 2024-10-18
    strip_prefix = "oak-439c15a855fdb42067d4544535f836a19ee9f168",
    url = "https://github.com/project-oak/oak/archive/439c15a855fdb42067d4544535f836a19ee9f168.tar.gz",
)

load("@oak//bazel:repositories.bzl", "oak_toolchain_repositories")

oak_toolchain_repositories()

http_archive(
    name = "bazel_clang_tidy",
    sha256 = "352aeb57ad7ed53ff6e02344885de426421fb6fd7a3890b04d14768d759eb598",
    strip_prefix = "bazel_clang_tidy-4884c32e09c1ea9ac96b3f08c3004f3ac4c3fe39",
    urls = ["https://github.com/erenon/bazel_clang_tidy/archive/4884c32e09c1ea9ac96b3f08c3004f3ac4c3fe39.zip"],
)
