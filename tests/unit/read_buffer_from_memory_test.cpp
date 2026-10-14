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

#include <gtest/gtest.h>

#include "IO/ReadBufferFromMemory.h"
#include "IO/ReadHelpers.h"

#include <numeric>
#include <vector>

using firebolt::adbc::ReadBufferFromMemory;
using firebolt::adbc::readToEnd;

namespace
{

std::vector<uint8_t> SampleBytes(size_t size)
{
    std::vector<uint8_t> bytes(size);
    std::iota(bytes.begin(), bytes.end(), static_cast<uint8_t>(7));
    return bytes;
}

} // namespace

TEST(ReadBufferFromMemoryTest, ReadsBorrowedBytesInPieces)
{
    const std::vector<uint8_t> bytes = SampleBytes(1000);
    ReadBufferFromMemory in{std::span<const uint8_t>(bytes)};

    std::vector<uint8_t> read(7);
    size_t bytes_read = 0;
    ASSERT_TRUE(in.read(read.data(), read.size(), bytes_read).ok());
    EXPECT_EQ(bytes_read, 7u);
    ASSERT_TRUE(readToEnd(in, read).ok());
    EXPECT_EQ(read, bytes);
}

TEST(ReadBufferFromMemoryTest, SeekToTheStartReadsItAgain)
{
    // What libcurl does to send an upload again (the FB2 401 retry).
    const std::vector<uint8_t> bytes = SampleBytes(300);
    ReadBufferFromMemory in{std::span<const uint8_t>(bytes)};
    std::vector<uint8_t> first;
    ASSERT_TRUE(readToEnd(in, first).ok());
    EXPECT_TRUE(in.seek(0));
    std::vector<uint8_t> second;
    ASSERT_TRUE(readToEnd(in, second).ok());
    EXPECT_EQ(first, bytes);
    EXPECT_EQ(second, bytes);
}

TEST(ReadBufferFromMemoryTest, SeekPastTheEndFails)
{
    const std::vector<uint8_t> bytes = SampleBytes(10);
    ReadBufferFromMemory in{std::span<const uint8_t>(bytes)};
    EXPECT_FALSE(in.seek(11));
    EXPECT_TRUE(in.seek(4));
    std::vector<uint8_t> rest;
    ASSERT_TRUE(readToEnd(in, rest).ok());
    EXPECT_EQ(rest, std::vector<uint8_t>(bytes.begin() + 4, bytes.end()));
}

TEST(ReadBufferFromMemoryTest, EmptyReadsNothing)
{
    ReadBufferFromMemory in{std::vector<uint8_t>{}};
    std::vector<uint8_t> read;
    ASSERT_TRUE(readToEnd(in, read).ok());
    EXPECT_TRUE(read.empty());
}
