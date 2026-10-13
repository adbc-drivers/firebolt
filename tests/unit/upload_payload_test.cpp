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

#include "HttpClient.h"

#include <cstdio>
#include <numeric>
#include <vector>

#include <curl/curl.h>

using firebolt::adbc::UploadPayload;

namespace
{

std::vector<uint8_t> SampleBytes(size_t size)
{
    std::vector<uint8_t> bytes(size);
    std::iota(bytes.begin(), bytes.end(), static_cast<uint8_t>(7));
    return bytes;
}

// Read the whole payload the way libcurl does, `chunk_size` bytes at a time.
std::vector<uint8_t> ReadAll(UploadPayload & payload, size_t chunk_size)
{
    std::vector<uint8_t> read;
    std::vector<char> chunk(chunk_size);
    for (;;)
    {
        size_t got = UploadPayload::read(chunk.data(), 1, chunk.size(), &payload);
        if (got == 0)
            return read;
        read.insert(read.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
}

} // namespace

TEST(UploadPayloadTest, ReadsTheBytesInPieces)
{
    const std::vector<uint8_t> bytes = SampleBytes(1000);
    UploadPayload payload{bytes};
    EXPECT_EQ(ReadAll(payload, 7), bytes);
}

TEST(UploadPayloadTest, SeekToTheStartReadsItAgain)
{
    // What libcurl does to send the request again (the FB2 401 retry).
    const std::vector<uint8_t> bytes = SampleBytes(300);
    UploadPayload payload{bytes};
    EXPECT_EQ(ReadAll(payload, 64), bytes);
    EXPECT_EQ(UploadPayload::seek(&payload, 0, SEEK_SET), CURL_SEEKFUNC_OK);
    EXPECT_EQ(ReadAll(payload, 13), bytes);
}

TEST(UploadPayloadTest, SeekOutsideThePayloadFails)
{
    const std::vector<uint8_t> bytes = SampleBytes(10);
    UploadPayload payload{bytes};
    EXPECT_EQ(UploadPayload::seek(&payload, 11, SEEK_SET), CURL_SEEKFUNC_FAIL);
    EXPECT_EQ(UploadPayload::seek(&payload, 4, SEEK_SET), CURL_SEEKFUNC_OK);
    EXPECT_EQ(ReadAll(payload, 3), std::vector<uint8_t>(bytes.begin() + 4, bytes.end()));
}

TEST(UploadPayloadTest, EmptyPayloadReadsNothing)
{
    UploadPayload payload{};
    EXPECT_TRUE(ReadAll(payload, 8).empty());
}
