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

#include "ArrowIpcStream.h"

#include <cstring>
#include <memory>

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

namespace firebolt::adbc
{

// Empty-stream helpers: a trivial stream that immediately signals end-of-stream
// with a zero-column schema.
struct EmptyStreamState
{
    ArrowSchema schema{};
};

static int emptyGetSchema(ArrowArrayStream * stream, ArrowSchema * out)
{
    auto * state = static_cast<EmptyStreamState *>(stream->private_data);
    ArrowSchemaMove(&state->schema, out);
    return 0;
}

static int emptyGetNext(ArrowArrayStream * /*stream*/, ArrowArray * out)
{
    out->release = nullptr;
    return 0;
}

static const char * emptyGetLastError(ArrowArrayStream * /*stream*/)
{
    return nullptr;
}

static void emptyRelease(ArrowArrayStream * stream)
{
    if (!stream || !stream->private_data)
        return;
    auto * state = static_cast<EmptyStreamState *>(stream->private_data);
    if (state->schema.release)
        state->schema.release(&state->schema);
    delete state;
    stream->private_data = nullptr;
    stream->release = nullptr;
}

std::string ExportIpcBytesAsArrowStream(std::vector<uint8_t> ipc_bytes, ArrowArrayStream * out)
{
    if (ipc_bytes.empty())
    {
        // DDL or empty result — produce a zero-column empty stream
        auto * state = new EmptyStreamState();
        int rc = ArrowSchemaInitFromType(&state->schema, NANOARROW_TYPE_STRUCT);
        if (rc != 0)
        {
            delete state;
            return "ArrowSchemaInitFromType failed";
        }
        out->get_schema = emptyGetSchema;
        out->get_next = emptyGetNext;
        out->get_last_error = emptyGetLastError;
        out->release = emptyRelease;
        out->private_data = state;
        return {};
    }

    // Copy bytes into an ArrowBuffer — ArrowIpcInputStreamInitBuffer takes ownership
    // via ArrowBufferMove, so buf will be zeroed after a successful init call.
    ArrowBuffer buf{};
    ArrowBufferInit(&buf);
    int rc = ArrowBufferAppend(&buf, ipc_bytes.data(), static_cast<int64_t>(ipc_bytes.size()));
    if (rc != 0)
    {
        ArrowBufferReset(&buf);
        return "ArrowBufferAppend failed (OOM)";
    }

    // ArrowIpcInputStreamInitBuffer moves buf into the stream's private state.
    ArrowIpcInputStream input{};
    rc = ArrowIpcInputStreamInitBuffer(&input, &buf);
    if (rc != 0)
    {
        ArrowBufferReset(&buf); // buf was NOT moved on error
        return "ArrowIpcInputStreamInitBuffer failed";
    }

    // ArrowIpcArrayStreamReaderInit takes ownership of input.
    rc = ArrowIpcArrayStreamReaderInit(out, &input, nullptr);
    if (rc != 0)
    {
        if (input.release)
            input.release(&input);
        return "ArrowIpcArrayStreamReaderInit failed";
    }

    return {};
}

} // namespace firebolt::adbc
