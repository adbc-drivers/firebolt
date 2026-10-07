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

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace firebolt::adbc
{

// strdup is POSIX rather than standard C. Keep ownership compatible with the
// ADBC error ABI while remaining portable to Windows.
inline char * duplicateString(std::string_view value)
{
    auto * result = static_cast<char *>(std::malloc(value.size() + 1));
    if (!result)
        return nullptr;
    std::memcpy(result, value.data(), value.size());
    result[value.size()] = '\0';
    return result;
}

} // namespace firebolt::adbc
