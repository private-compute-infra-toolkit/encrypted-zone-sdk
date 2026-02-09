# syntax=docker/dockerfile:1.4
# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

#
# stage: golang-base
#
FROM golang:1.23.4-bookworm AS golang-base

#
# stage: bazelisk
#
FROM golang-base AS bazelisk
ARG TARGETARCH
ARG BAZELISK_VERSION=1.25.0
ENV \
  GOPATH=/opt/go \
  BAZELISK_HOME=/usr/local/share/.bazelisk
# install bazelisk
RUN --mount=type=cache,target=/root/.cache/go-build \
  go install github.com/bazelbuild/bazelisk@v${BAZELISK_VERSION} \
  && ln -s ${GOPATH}/bin/bazelisk /usr/local/bin/bazel

#
# stage: build-tools
#
FROM bazelisk AS clang
ARG CLANG_VER=18
ADD https://apt.llvm.org/llvm.sh /tmp/llvm.sh
RUN --mount=target=/var/lib/apt/lists,type=cache,sharing=locked \
  --mount=target=/var/cache/apt,type=cache,sharing=locked \
  rm -f /etc/apt/apt.conf.d/docker-clean \
  && apt-get update \
  && apt-get --quiet install -y --no-install-recommends \
  apt-transport-https ca-certificates libssl-dev locales \
  lsb-release openssh-client software-properties-common patch \
  && chmod +x /tmp/llvm.sh \
  && /tmp/llvm.sh ${CLANG_VER} \
  && apt-get --quiet install -y --no-install-recommends \
  clang-tidy libc++-${CLANG_VER}-dev libc++abi-${CLANG_VER}-dev libclang-${CLANG_VER}-dev \
  && for p in clang lldb lld ld.lld; do update-alternatives --install /usr/bin/${p} ${p} /usr/bin/${p}-${CLANG_VER} 100; done \
  && rm -f /tmp/llvm.sh \
  && clang --version
ENV \
  CC=clang \
  CXX=clang++ \
  TZ=Etc/UTC

#
# stage: bazel
#
FROM clang AS bazel
COPY .bazelversion tools/generate_system_bazelrc /tmp/
ENV BAZEL_ROOT=/bazel_root
# install specified bazel version
COPY tools/generate_system_bazelrc /tmp
RUN cd /tmp \
  && touch WORKSPACE \
  && bazel version \
  && /tmp/generate_system_bazelrc \
  && rm -f WORKSPACE .bazelversion

#
# stage: bazel-workspace
#
FROM bazel AS bazel-workspace
ENV \
  BAZEL_CXXOPTS=-stdlib=libc++ \
  BAZEL_LINKOPTS=-lc++:-lm \
  WORKSPACE=/workspace
ADD . /workspace
WORKDIR /workspace
RUN bazel version

#
# stage: generate docs
#
FROM bazel-workspace AS generate-docs
RUN bazel run //docs/bazel:update

#
# stage: docs artifacts
#
FROM scratch AS docs-artifacts
COPY --from=generate-docs /workspace/docs/bazel /

#
# stage: build workspace artifacts
#
FROM bazel-workspace AS generate-workspace-artifacts
ARG BAZEL_STARTUP_ARGS
ARG BAZEL_DIRECT_ARGS
ARG GOOGLE_APPLICATION_CREDENTIALS

RUN --mount=type=cache,target=/root/.cache/bazel \
  --mount=type=cache,target=/bazel_root/functest-system \
  --mount=type=secret,id=gcloud_creds \
  bazel \
  ${BAZEL_STARTUP_ARGS} \
  test \
  --config=run_all_tests \
  ${BAZEL_DIRECT_ARGS} \
  -- \
  //sut/cmd/deploy_and_test:lib_test

RUN --mount=type=cache,target=/root/.cache/bazel \
  --mount=type=cache,target=/bazel_root/functest-system \
  --mount=type=secret,id=gcloud_creds \
  bazel \
  ${BAZEL_STARTUP_ARGS} \
  run \
  ${BAZEL_DIRECT_ARGS} \
  -- \
  //sut:copy_to_dist

#
# stage: build artifacts
#
FROM scratch AS dist-artifacts
COPY --from=generate-workspace-artifacts /workspace/dist/ /

#
# stage: ghz
#
FROM golang-base AS ghz
RUN --mount=type=cache,target=/root/.cache/go-build \
  go install github.com/bojand/ghz/cmd/ghz@v0.120.0

#
# stage: grpcurl
#
FROM golang-base AS grpcurl
RUN --mount=type=cache,target=/root/.cache/go-build \
  go install github.com/fullstorydev/grpcurl/cmd/grpcurl@v1.9.2

#
# stage: runtime-base
#
FROM bazelisk AS runtime-base
ARG VERSION="0.0.0"
LABEL org.opencontainers.image.version="${VERSION}"
LABEL org.opencontainers.image.authors="Google Privacy Sandbox"
LABEL org.opencontainers.image.source="https://github.com/privacysandbox/data-plane-shared-libraries"
WORKDIR /
RUN --mount=target=/var/lib/apt/lists,type=cache,sharing=locked \
  --mount=target=/var/cache/apt,type=cache,sharing=locked \
  rm -f /etc/apt/apt.conf.d/docker-clean \
  && apt-get update \
  && apt-get --quiet install -y --no-install-recommends \
  apt-transport-https ca-certificates file zip

#
# stage: promql-cli-sources
#
FROM alpine:latest AS promql-cli-sources
ADD https://github.com/nalbury/promql-cli/archive/refs/tags/v0.3.0.tar.gz /tmp/src.tar.gz
RUN tar --extract --directory=/tmp --file=/tmp/src.tar.gz \
  && mv /tmp/promql-cli-0.3.0 /tmp/promql-cli

#
# stage: promql-cli-build
#
FROM golang-base AS promql-cli-build
ARG TARGETARCH
COPY --from=promql-cli-sources /tmp/promql-cli /promql-cli
WORKDIR /promql-cli
RUN go mod download
RUN OS=linux ARCH=${TARGETARCH} INSTALL_PATH=/promql-cli/build/bin/ make install

#
# stage: gojq
#
FROM golang-base AS jq
RUN --mount=type=cache,target=/root/.cache/go-build \
  go install github.com/itchyny/gojq/cmd/gojq@v0.12.17

#
# stage: protoc
#
FROM runtime-base AS protoc
ARG TARGETARCH
ARG PROTOC_VERSION=28.3
RUN --mount=target=/var/lib/apt/lists,type=cache,sharing=locked \
  --mount=target=/var/cache/apt,type=cache,sharing=locked \
  rm -f /etc/apt/apt.conf.d/docker-clean \
  && apt-get update \
  && apt-get --quiet install -y --no-install-recommends unzip \
  && case ${TARGETARCH} in \
       amd64) PROTOC_ARCH=x86_64 ;; \
       arm64) PROTOC_ARCH=aarch_64 ;; \
       *) echo "Unsupported architecture: ${TARGETARCH}" && exit 1 ;; \
     esac \
  && curl -L "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-${PROTOC_ARCH}.zip" -o /tmp/protoc.zip \
  && unzip /tmp/protoc.zip -d /tmp/protoc \
  && rm /tmp/protoc.zip \
  && /tmp/protoc/bin/protoc --version

#
# stage: buf
#
FROM bufbuild/buf:1.55 AS buf

#
# stage: Azure CLI
#
FROM runtime-base AS azure-cli
ADD https://aka.ms/InstallAzureCLIDeb /tmp/InstallAzureCLIDeb
# install python3-venv
RUN --mount=target=/var/lib/apt/lists,type=cache,sharing=locked \
  --mount=target=/var/cache/apt,type=cache,sharing=locked \
  rm -f /etc/apt/apt.conf.d/docker-clean \
  && apt-get update \
  && apt-get --quiet install -y --no-install-recommends python3.11-venv
ARG AZ_VENV_DIR=/opt/az-venv
RUN python3 -m venv "${AZ_VENV_DIR}" \
  && "${AZ_VENV_DIR}"/bin/pip install azure-cli==2.74.0
ENV AZ="${AZ_VENV_DIR}/bin/az"

#
# stage: functionaltest CLI
#
FROM azure-cli AS functest-cli
LABEL org.opencontainers.image.title="functionaltest CLI"
LABEL org.opencontainers.image.description="functionaltest CLI tool for Privacy Sandbox"
COPY --from=dist-artifacts --chmod=0755 /functionaltest /usr/local/bin/functionaltest
COPY --from=ghz --chmod=0755 /go/bin/ghz /usr/local/bin/
COPY --from=grpcurl --chmod=0755 /go/bin/grpcurl /usr/local/bin/
COPY --from=ghcr.io/jqlang/jq:1.7.1 --chmod=0755 /jq /usr/local/bin
COPY --from=jq --chmod=0755 /go/bin/gojq /usr/local/bin/
COPY --from=promql-cli-build --chmod=0755 /promql-cli/build/bin/promql /usr/bin/promql
COPY --from=protoc --chmod=0755 /tmp/protoc/bin/protoc /usr/local/bin/protoc
COPY --from=buf --chmod=0755 /usr/local/bin/buf /usr/local/bin/buf
ENV \
  GHZ=/usr/local/bin/ghz \
  GRPCURL=/usr/local/bin/grpcurl \
  JQ=/usr/local/bin/jq \
  GOJQ=/usr/local/bin/gojq \
  PROMQL=/usr/bin/promql \
  PROTOC=/usr/local/bin/protoc \
  BUF=/usr/local/bin/buf
RUN /usr/local/bin/functionaltest dockersut deploy-and-test --init-bazel
ENTRYPOINT ["/usr/local/bin/functionaltest"]
