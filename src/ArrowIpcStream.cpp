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
#include "IO/ReadBufferFromMemory.h"

#include <algorithm>
#include <cctype>
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

// A zero-column stream with no batches: the result of a body with no bytes.
static std::string ExportEmptyStream(ArrowArrayStream * out)
{
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

namespace
{

    // Every message of an Arrow IPC stream written by Arrow C++, Firebolt's writer,
    // starts with it; Firebolt's error text after a failed query cannot.
    constexpr uint8_t CONTINUATION_MARKER[] = {0xFF, 0xFF, 0xFF, 0xFF};
    // How much of that error text is kept for the message.
    constexpr size_t MAX_ERROR_TEXT_SIZE = 64 * 1024;

    // The body behind nanoarrow's IPC reader, owned by its ArrowIpcInputStream.
    struct IpcInput
    {
        std::unique_ptr<IReadBuffer> body;
        // Read ahead to tell an empty body from one that starts with a schema.
        std::vector<uint8_t> lookahead;
        // Set by the stream before each message: the next read starts one.
        bool at_message_start = true;
        // Why the body could not be read; empty while it can.
        std::string error;

        // Fill `buffer` with `size` bytes, fewer only at the end of the body.
        Status readFully(uint8_t * buffer, size_t size, size_t & bytes_read)
        {
            bytes_read = std::min(size, lookahead.size());
            memcpy(buffer, lookahead.data(), bytes_read);
            lookahead.erase(lookahead.begin(), lookahead.begin() + static_cast<std::ptrdiff_t>(bytes_read));
            while (bytes_read < size)
            {
                size_t received = 0;
                Status status = body->read(buffer + bytes_read, size - bytes_read, received);
                if (!status.ok())
                    return status;
                if (received == 0)
                    break;
                bytes_read += received;
            }
            return {};
        }
    };

    // Read and drop whatever is left of the body.
    void skipToEnd(IReadBuffer & body)
    {
        uint8_t chunk[4096];
        size_t received = 0;
        while (body.read(chunk, sizeof(chunk), received).ok() && received > 0)
        {
        }
    }

    // The error text that begins with `start`: it and the rest of the body.
    std::string readErrorText(IReadBuffer & body, const uint8_t * start, size_t size)
    {
        std::string text(reinterpret_cast<const char *>(start), size);
        uint8_t chunk[4096];
        size_t received = 0;
        while (text.size() < MAX_ERROR_TEXT_SIZE && body.read(chunk, sizeof(chunk), received).ok() && received > 0)
            text.append(reinterpret_cast<const char *>(chunk), received);
        while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();
        return text;
    }

    ArrowErrorCode ipcInputRead(ArrowIpcInputStream * stream, uint8_t * buffer, int64_t size, int64_t * size_read, ArrowError * error)
    {
        auto * reader = static_cast<IpcInput *>(stream->private_data);
        size_t bytes_read = 0;
        Status status = reader->readFully(buffer, static_cast<size_t>(size), bytes_read);
        if (!status.ok())
            reader->error = status.message;
        else if (
            reader->at_message_start && bytes_read > 0
            && (bytes_read < sizeof(CONTINUATION_MARKER) || memcmp(buffer, CONTINUATION_MARKER, sizeof(CONTINUATION_MARKER)) != 0))
            reader->error = "Query failed while streaming results: " + readErrorText(*reader->body, buffer, bytes_read);
        reader->at_message_start = false;

        if (!reader->error.empty())
        {
            ArrowErrorSet(error, "%s", reader->error.c_str());
            return EIO;
        }
        *size_read = static_cast<int64_t>(bytes_read);
        return NANOARROW_OK;
    }

    void ipcInputRelease(ArrowIpcInputStream * stream)
    {
        delete static_cast<IpcInput *>(stream->private_data);
        stream->private_data = nullptr;
        stream->release = nullptr;
    }

    // nanoarrow's IPC reader, with the body's errors in place of its own.
    struct IpcReaderState
    {
        nanoarrow::UniqueArrayStream ipc_reader; // owns the input stream, and so `body`
        IpcInput * body = nullptr;
        nanoarrow::UniqueSchema schema;
        std::string last_error;

        // The reason the last read failed, preferring the body's own.
        int fail(int code)
        {
            const char * reader_error = ipc_reader->get_last_error(ipc_reader.get());
            last_error = !body->error.empty() ? body->error : reader_error ? reader_error : "Failed to read the query result";
            return code;
        }
    };

    int ipcReaderGetSchema(ArrowArrayStream * stream, ArrowSchema * out)
    {
        auto * state = static_cast<IpcReaderState *>(stream->private_data);
        return ArrowSchemaDeepCopy(state->schema.get(), out);
    }

    int ipcReaderGetNext(ArrowArrayStream * stream, ArrowArray * out)
    {
        auto * state = static_cast<IpcReaderState *>(stream->private_data);
        if (!state->body->error.empty())
            return state->fail(EIO);
        state->body->at_message_start = true;
        const int rc = state->ipc_reader->get_next(state->ipc_reader.get(), out);
        if (rc != 0)
            return state->fail(rc);
        // After the end-of-stream marker, read the response to its end: that ends
        // the transfer, which frees the connection for its next request.
        if (!out->release)
            skipToEnd(*state->body->body);
        return 0;
    }

    const char * ipcReaderGetLastError(ArrowArrayStream * stream)
    {
        auto * state = static_cast<IpcReaderState *>(stream->private_data);
        return state->last_error.empty() ? nullptr : state->last_error.c_str();
    }

    void ipcReaderRelease(ArrowArrayStream * stream)
    {
        if (!stream || !stream->private_data)
            return;
        delete static_cast<IpcReaderState *>(stream->private_data);
        memset(stream, 0, sizeof(*stream));
    }

} // namespace

std::string ExportIpcReadBufferAsArrowStream(std::unique_ptr<IReadBuffer> body, ArrowArrayStream * out)
{
    auto reader = std::make_unique<IpcInput>();
    reader->body = std::move(body);

    // DDL and zero-row results have no bytes at all, not even a schema.
    reader->lookahead.resize(1);
    size_t received = 0;
    Status status = reader->body->read(reader->lookahead.data(), 1, received);
    if (!status.ok())
        return status.message;
    if (received == 0)
        return ExportEmptyStream(out);

    auto state = std::make_unique<IpcReaderState>();
    state->body = reader.get();
    ArrowIpcInputStream input{ipcInputRead, ipcInputRelease, reader.release()};
    if (ArrowIpcArrayStreamReaderInit(state->ipc_reader.get(), &input, nullptr) != 0)
    {
        if (input.release)
            input.release(&input);
        return "ArrowIpcArrayStreamReaderInit failed";
    }

    // Read now, so a body that is not an Arrow stream fails here and every later
    // get_next reads exactly one message.
    if (state->ipc_reader->get_schema(state->ipc_reader.get(), state->schema.get()) != 0)
    {
        state->fail(EIO);
        return state->last_error;
    }

    out->get_schema = ipcReaderGetSchema;
    out->get_next = ipcReaderGetNext;
    out->get_last_error = ipcReaderGetLastError;
    out->release = ipcReaderRelease;
    out->private_data = state.release();
    return {};
}

std::string ExportIpcBytesAsArrowStream(std::vector<uint8_t> ipc_bytes, ArrowArrayStream * out)
{
    return ExportIpcReadBufferAsArrowStream(std::make_unique<ReadBufferFromMemory>(std::move(ipc_bytes)), out);
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
