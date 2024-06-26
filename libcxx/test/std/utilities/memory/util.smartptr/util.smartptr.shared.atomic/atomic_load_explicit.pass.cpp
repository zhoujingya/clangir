//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// ADDITIONAL_COMPILE_FLAGS: -D_LIBCPP_ENABLE_CXX26_REMOVED_SHARED_PTR_ATOMICS
// ADDITIONAL_COMPILE_FLAGS: -D_LIBCPP_DISABLE_DEPRECATION_WARNINGS
// UNSUPPORTED: no-threads

// <memory>

// shared_ptr

// template <class T>
// shared_ptr<T>
// atomic_load_explicit(const shared_ptr<T>* p, memory_order mo)     // Deprecated in C++20, removed in C++26

// UNSUPPORTED: c++03

#include <memory>

#include <atomic>
#include <cassert>

#include "test_macros.h"

int main(int, char**)
{
    {
        const std::shared_ptr<int> p(new int(3));
        std::shared_ptr<int> q = std::atomic_load_explicit(&p, std::memory_order_relaxed);
        assert(*q == *p);
    }

  return 0;
}
