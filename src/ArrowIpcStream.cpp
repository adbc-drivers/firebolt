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

namespace
{

    bool nullableStringsEqual(const char * left, const char * right)
    {
        if (!left || !right)
            return left == right;
        return strcmp(left, right) == 0;
    }

    bool metadataEqual(const char * left, const char * right)
    {
        if (!left || !right)
            return left == right;
        const int64_t left_size = ArrowMetadataSizeOf(left);
        const int64_t right_size = ArrowMetadataSizeOf(right);
        return left_size == right_size && memcmp(left, right, static_cast<size_t>(left_size)) == 0;
    }

    bool schemasEqual(const ArrowSchema * left, const ArrowSchema * right)
    {
        if (!left || !right || !nullableStringsEqual(left->format, right->format) || !nullableStringsEqual(left->name, right->name)
            || !metadataEqual(left->metadata, right->metadata) || left->flags != right->flags || left->n_children != right->n_children
            || (left->dictionary == nullptr) != (right->dictionary == nullptr))
            return false;

        for (int64_t i = 0; i < left->n_children; ++i)
            if (!schemasEqual(left->children[i], right->children[i]))
                return false;
        return !left->dictionary || schemasEqual(left->dictionary, right->dictionary);
    }

    struct ConcatenatedStreamState
    {
        ArrowSchema schema{};
        std::vector<ArrowArrayStream> streams;
        size_t current_stream = 0;
        std::string last_error;

        ~ConcatenatedStreamState()
        {
            if (schema.release)
                schema.release(&schema);
            for (auto & stream : streams)
                if (stream.release)
                    stream.release(&stream);
        }
    };

    int concatenatedGetSchema(ArrowArrayStream * stream, ArrowSchema * out)
    {
        auto * state = static_cast<ConcatenatedStreamState *>(stream->private_data);
        return ArrowSchemaDeepCopy(&state->schema, out);
    }

    int concatenatedGetNext(ArrowArrayStream * stream, ArrowArray * out)
    {
        auto * state = static_cast<ConcatenatedStreamState *>(stream->private_data);
        while (state->current_stream < state->streams.size())
        {
            ArrowArrayStream & current = state->streams[state->current_stream];
            const int result = current.get_next(&current, out);
            if (result != 0)
            {
                const char * child_error = current.get_last_error ? current.get_last_error(&current) : nullptr;
                state->last_error = child_error ? child_error : "Failed to read a parameter result stream";
                return result;
            }
            if (out->release)
                return 0;
            ++state->current_stream;
        }
        out->release = nullptr;
        return 0;
    }

    const char * concatenatedGetLastError(ArrowArrayStream * stream)
    {
        auto * state = static_cast<ConcatenatedStreamState *>(stream->private_data);
        return state->last_error.empty() ? nullptr : state->last_error.c_str();
    }

    void concatenatedRelease(ArrowArrayStream * stream)
    {
        if (!stream || !stream->private_data)
            return;
        delete static_cast<ConcatenatedStreamState *>(stream->private_data);
        memset(stream, 0, sizeof(*stream));
    }

} // namespace

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

std::string ExportIpcResponsesAsArrowStream(std::vector<std::vector<uint8_t>> ipc_responses, ArrowArrayStream * out)
{
    if (ipc_responses.empty())
        return ExportIpcBytesAsArrowStream({}, out);

    auto state = std::make_unique<ConcatenatedStreamState>();
    state->streams.resize(ipc_responses.size());
    for (size_t i = 0; i < ipc_responses.size(); ++i)
    {
        std::string error = ExportIpcBytesAsArrowStream(std::move(ipc_responses[i]), &state->streams[i]);
        if (!error.empty())
            return error;

        nanoarrow::UniqueSchema response_schema;
        if (state->streams[i].get_schema(&state->streams[i], response_schema.get()) != 0)
            return "Failed to read a parameter result schema";
        if (i == 0)
        {
            ArrowSchemaMove(response_schema.get(), &state->schema);
        }
        else if (!schemasEqual(&state->schema, response_schema.get()))
        {
            return "Parameter executions returned incompatible schemas";
        }
    }

    out->get_schema = concatenatedGetSchema;
    out->get_next = concatenatedGetNext;
    out->get_last_error = concatenatedGetLastError;
    out->release = concatenatedRelease;
    out->private_data = state.release();
    return {};
}

} // namespace firebolt::adbc
