#!/bin/bash

# Copyright 2020 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Run all ctest tests in a build directory, adding some automatic test filtering
# based on environment variables and defaulting to running as many test jobs as
# there are processes. The build directory to test is passed as the first
# argument. Designed for CI, but can be run manually.

set -euo pipefail

BUILD_DIR="$1"

# Respect the user setting, but default to as many jobs as we have cores.
export CTEST_PARALLEL_LEVEL=${CTEST_PARALLEL_LEVEL:-$(nproc)}

# Respect the user setting, but default to turning on Vulkan.
export IREE_VULKAN_DISABLE=${IREE_VULKAN_DISABLE:-0}
# Respect the user setting, but default to turning off CUDA.
export IREE_CUDA_DISABLE=${IREE_CUDA_DISABLE:-1}
# The VK_KHR_shader_float16_int8 extension is optional prior to Vulkan 1.2.
# We test on SwiftShader as a baseline, which does not support this extension.
export IREE_VULKAN_F16_DISABLE=${IREE_VULKAN_F16_DISABLE:-1}
# Respect the user setting, default to no --repeat-until-fail.
export IREE_CTEST_REPEAT_UNTIL_FAIL_COUNT=${IREE_CTEST_REPEAT_UNTIL_FAIL_COUNT:-}
# Respect the user setting, default to no --tests-regex.
export IREE_CTEST_TESTS_REGEX=${IREE_CTEST_TESTS_REGEX:-}

# Tests to exclude by label. In addition to any custom labels (which are carried
# over from Bazel tags), every test should be labeled with its directory.
declare -a label_exclude_args=(
  # Exclude specific labels.
  # Put the whole label with anchors for exact matches.
  # For example:
  #   ^nokokoro$
  # TODO: update label name as part of dropping Kokoro
  ^nokokoro$

  # Exclude all tests in a directory.
  # Put the whole directory with anchors for exact matches.
  # For example:
  #   ^bindings/python/iree/runtime$

  # Exclude all tests in some subdirectories.
  # Put the whole parent directory with only a starting anchor.
  # Use a trailing slash to avoid prefix collisions.
  # For example:
  #   ^bindings/
)

if [[ "${IREE_VULKAN_DISABLE}" == 1 ]]; then
  label_exclude_args+=("^driver=vulkan$")
fi
if [[ "${IREE_CUDA_DISABLE}" == 1 ]]; then
  label_exclude_args+=("^driver=cuda$")
fi
if [[ "${IREE_VULKAN_F16_DISABLE}" == 1 ]]; then
  label_exclude_args+=("^vulkan_uses_vk_khr_shader_float16_int8$")
fi

IFS=',' read -ra extra_label_exclude_args <<< "${IREE_EXTRA_COMMA_SEPARATED_CTEST_LABELS_TO_EXCLUDE:-}"
label_exclude_args+=(${extra_label_exclude_args[@]})

# Some tests are just failing on some platforms and this filtering lets us
# exclude any type of test. Ideally each test would be tagged with the
# platforms it doesn't support, but that would require editing through layers
# of CMake functions. Hopefully this list stays very short.
declare -a excluded_tests=()
if [[ "$OSTYPE" =~ ^msys ]]; then
  # These tests are failing on Windows.
  excluded_tests+=(
    # TODO(#11077): Fix assert on task->pending_dependency_count atomic
    "iree/tests/e2e/matmul/e2e_matmul_direct_i8_small_ukernel_vmvx_local-task"
    "iree/tests/e2e/matmul/e2e_matmul_direct_f32_small_ukernel_vmvx_local-task"
    # TODO(#11068): Fix compilation segfault
    "iree/tests/e2e/regression/check_regression_llvm-cpu_lowering_config.mlir"
    # TODO(#11070): Fix argument/result signature mismatch
    "iree/tests/e2e/tosa_ops/check_vmvx_local-sync_microkernels_fully_connected.mlir"
    # TODO(#11080): Fix arrays not matching in test_variant_list_buffers
    "iree/runtime/bindings/python/vm_types_test"
  )
fi

ctest_args=(
  "--test-dir ${BUILD_DIR}"
  "--timeout 900"
  "--output-on-failure"
  "--no-tests=error"
)

if [[ -n "${IREE_CTEST_TESTS_REGEX}" ]]; then
  ctest_args+=("--tests-regex ${IREE_CTEST_TESTS_REGEX}")
fi

if [[ -n "${IREE_CTEST_REPEAT_UNTIL_FAIL_COUNT}" ]]; then
  ctest_args+=("--repeat-until-fail ${IREE_CTEST_REPEAT_UNTIL_FAIL_COUNT}")
fi

if (( ${#label_exclude_args[@]} )); then
  # Join on "|"
  label_exclude_regex="($(IFS="|" ; echo "${label_exclude_args[*]}"))"
  ctest_args+=("--label-exclude ${label_exclude_regex}")
fi

if (( ${#excluded_tests[@]} )); then
  # Prefix with `^` anchor
  excluded_tests=( "${excluded_tests[@]/#/^}" )
  # Suffix with `$` anchor
  excluded_tests=( "${excluded_tests[@]/%/$}" )
  # Join on `|` and wrap in parens
  excluded_tests_regex="($(IFS="|" ; echo "${excluded_tests[*]?}"))"
  ctest_args+=("--exclude-regex ${excluded_tests_regex}")
fi

echo "*************** Running CTest ***************"

set -x
ctest ${ctest_args[@]}
