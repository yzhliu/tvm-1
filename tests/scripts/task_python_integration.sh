#!/bin/bash
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

set -e
set -u

source tests/scripts/setup-pytest-env.sh
export PYTHONPATH=${PYTHONPATH}:${TVM_PATH}/apps/extension/python
export LD_LIBRARY_PATH="build:${LD_LIBRARY_PATH:-}"

# to avoid CI CPU thread throttling.
export TVM_BIND_THREADS=0
export TVM_NUM_THREADS=2

# cleanup pycache
find . -type f -path "*.pyc" | xargs rm -f

# Test TVM
make cython3

# Test MISRA-C runtime
cd apps/bundle_deploy
rm -rf build
make test_dynamic test_static
cd ../..

# Test extern package
cd apps/extension
rm -rf lib
make
cd ../..

TVM_FFI=cython python3 -m pytest apps/extension/tests
TVM_FFI=ctypes python3 -m pytest apps/extension/tests

# Test dso plugin
cd apps/dso_plugin_module
rm -rf lib
make
cd ../..
TVM_FFI=cython python3 -m pytest apps/dso_plugin_module
TVM_FFI=ctypes python3 -m pytest apps/dso_plugin_module

# Do not enable TensorFlow op
# TVM_FFI=cython sh prepare_and_test_tfop_module.sh
# TVM_FFI=ctypes sh prepare_and_test_tfop_module.sh

TVM_FFI=ctypes python3 -m pytest tests/python/integration
TVM_FFI=ctypes python3 -m pytest tests/python/contrib

TVM_TEST_TARGETS="${TVM_RELAY_TEST_TARGETS:-llvm;cuda}" TVM_FFI=ctypes python3 -m pytest tests/python/relay

# Do not enable OpenGL
# TVM_FFI=cython python -m pytest tests/webgl
# TVM_FFI=ctypes python3 -m pytest tests/webgl
