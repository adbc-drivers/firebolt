// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//         http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <utility>

namespace firebolt::adbc
{

template <typename Fn>
class ScopeGuard
{
public:
    explicit ScopeGuard(Fn && fn) : fn_(std::move(fn)) { }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard & operator=(const ScopeGuard &) = delete;
    ScopeGuard(ScopeGuard &&) = delete;
    ScopeGuard & operator=(ScopeGuard &&) = delete;

    ~ScopeGuard() { fn_(); }

private:
    Fn fn_;
};

} // namespace firebolt::adbc

#define FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT_INNER(x, y) x##y
#define FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT(x, y) FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT_INNER(x, y)
#define FIREBOLT_SCOPE_GUARD(...) \
    ::firebolt::adbc::ScopeGuard FIREBOLT_INTERNAL_SCOPE_GUARD_CONCAT(_firebolt_scope_guard_, __COUNTER__)([&]() { __VA_ARGS__; })
