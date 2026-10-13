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
#include "IO/ReadHelpers.h"

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
static Status ExportEmptyStream(ArrowArrayStream * out)
{
    auto * state = new EmptyStreamState();
    int rc = ArrowSchemaInitFromType(&state->schema, NANOARROW_TYPE_STRUCT);
    if (rc != 0)
    {
        delete state;
        return {ADBC_STATUS_INTERNAL, "ArrowSchemaInitFromType failed"};
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

    // nanoarrow's IPC reader over a response body, with the body's errors in place
    // of its own.  It is both the ArrowArrayStream's private data and the private
    // data of the ArrowIpcInputStream the reader pulls bytes through.
    struct IpcReaderState
    {
        std::unique_ptr<IReadBuffer> body;
        // Set before each message is read: the next read starts one.
        bool at_message_start = true;
        // Whether the body had any bytes at all; DDL and zero-row results have none.
        bool body_started = false;
        // Why reading stopped for good: the body's own failure, its error text, or
        // nanoarrow's.  OK while it can go on.
        Status failure;

        nanoarrow::UniqueArrayStream ipc_reader;
        nanoarrow::UniqueSchema schema;
        std::string last_error; // failure.message, for get_last_error

        // Stop for good: keep the reason, preferring the body's own, and drop the
        // body, which gives a response's handles back to its connection at once.
        int recordFailure(int code)
        {
            if (failure.ok())
            {
                const char * reader_error = ipc_reader->get_last_error(ipc_reader.get());
                failure = {ADBC_STATUS_INTERNAL, reader_error ? reader_error : "Failed to read the query result"};
            }
            last_error = failure.message;
            body.reset();
            return code;
        }
    };

    // The error text that begins with `start`: it and the rest of the body, up to
    // MAX_ERROR_TEXT_SIZE.  The rest is read too, which ends the transfer.
    std::string readErrorText(IReadBuffer & body, const uint8_t * start, size_t size)
    {
        std::string text(reinterpret_cast<const char *>(start), size);
        if (readToEnd(body, text, MAX_ERROR_TEXT_SIZE).ok())
            (void)skipToEnd(body); // the message is complete either way
        while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();
        return text;
    }

    // ArrowIpcInputStream::read: nanoarrow asks for exactly the bytes of the next
    // part of a message.  The first read of each message must start with the
    // continuation marker; anything else is the error text Firebolt sends when a
    // query fails after its response started.
    ArrowErrorCode ipcInputRead(ArrowIpcInputStream * stream, uint8_t * buffer, int64_t size, int64_t * size_read, ArrowError * error)
    {
        auto * state = static_cast<IpcReaderState *>(stream->private_data);
        *size_read = 0;
        if (!state->body)
            return NANOARROW_OK; // read to its end already: nothing more
        try
        {
            size_t bytes_read = 0;
            Status status = readFully(*state->body, buffer, static_cast<size_t>(size), bytes_read);
            state->body_started = state->body_started || bytes_read > 0;
            if (!status.ok())
                state->failure = std::move(status);
            else if (
                state->at_message_start && bytes_read > 0
                && (bytes_read < sizeof(CONTINUATION_MARKER) || memcmp(buffer, CONTINUATION_MARKER, sizeof(CONTINUATION_MARKER)) != 0))
                state->failure
                    = {ADBC_STATUS_IO, "Query failed while streaming results: " + readErrorText(*state->body, buffer, bytes_read)};
            state->at_message_start = false;
            *size_read = static_cast<int64_t>(bytes_read);
        }
        catch (std::exception & ex)
        {
            state->failure = {ADBC_STATUS_INTERNAL, ex.what()};
        }

        if (!state->failure.ok())
        {
            ArrowErrorSet(error, "%s", state->failure.message.c_str());
            return EIO;
        }
        return NANOARROW_OK;
    }

    // The input stream does not own its private data: the IpcReaderState does.
    void ipcInputRelease(ArrowIpcInputStream * stream)
    {
        stream->private_data = nullptr;
        stream->release = nullptr;
    }

    int ipcReaderGetSchema(ArrowArrayStream * stream, ArrowSchema * out)
    {
        auto * state = static_cast<IpcReaderState *>(stream->private_data);
        return ArrowSchemaDeepCopy(state->schema.get(), out);
    }

    // Decode the next record batch.  Each call reads exactly one IPC message, so
    // the marker check in ipcInputRead sees the start of each one.  Any failure is
    // final: it frees the connection, and later calls report it again unread.
    int ipcReaderGetNext(ArrowArrayStream * stream, ArrowArray * out)
    {
        auto * state = static_cast<IpcReaderState *>(stream->private_data);
        if (!state->failure.ok())
            return state->recordFailure(EIO);
        state->at_message_start = true;
        const int rc = state->ipc_reader->get_next(state->ipc_reader.get(), out);
        if (rc != 0)
            return state->recordFailure(rc);
        // After the end-of-stream marker, read the response to its end and drop it:
        // that ends the transfer, which frees the connection for its next request.
        if (!out->release && state->body)
        {
            try
            {
                if (Status status = skipToEnd(*state->body); !status.ok())
                    state->failure = std::move(status);
            }
            catch (std::exception & ex)
            {
                state->failure = {ADBC_STATUS_INTERNAL, ex.what()};
            }
            if (!state->failure.ok())
                return state->recordFailure(EIO);
            state->body.reset();
        }
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

Status ExportIpcReadBufferAsArrowStream(std::unique_ptr<IReadBuffer> body, ArrowArrayStream * out)
{
    auto state = std::make_unique<IpcReaderState>();
    state->body = std::move(body);
    ArrowIpcInputStream input{ipcInputRead, ipcInputRelease, state.get()};
    if (ArrowIpcArrayStreamReaderInit(state->ipc_reader.get(), &input, nullptr) != 0)
        return {ADBC_STATUS_INTERNAL, "ArrowIpcArrayStreamReaderInit failed"};

    // Read now, so a body that is not an Arrow stream fails here and every later
    // get_next reads exactly one message.
    if (state->ipc_reader->get_schema(state->ipc_reader.get(), state->schema.get()) != 0)
    {
        // DDL and zero-row results have no bytes at all, not even a schema.
        if (!state->body_started && state->failure.ok())
            return ExportEmptyStream(out);
        state->recordFailure(EIO);
        return state->failure;
    }

    out->get_schema = ipcReaderGetSchema;
    out->get_next = ipcReaderGetNext;
    out->get_last_error = ipcReaderGetLastError;
    out->release = ipcReaderRelease;
    out->private_data = state.release();
    return {};
}

Status ExportIpcBytesAsArrowStream(std::vector<uint8_t> ipc_bytes, ArrowArrayStream * out)
{
    return ExportIpcReadBufferAsArrowStream(std::make_unique<ReadBufferFromMemory>(std::move(ipc_bytes)), out);
}

Status ExportIpcResponsesAsArrowStream(std::vector<std::vector<uint8_t>> ipc_responses, ArrowArrayStream * out)
{
    if (ipc_responses.empty())
        return ExportIpcBytesAsArrowStream({}, out);

    auto state = std::make_unique<ConcatenatedStreamState>();
    state->streams.resize(ipc_responses.size());
    for (size_t i = 0; i < ipc_responses.size(); ++i)
    {
        if (Status status = ExportIpcBytesAsArrowStream(std::move(ipc_responses[i]), &state->streams[i]); !status.ok())
            return status;

        nanoarrow::UniqueSchema response_schema;
        if (state->streams[i].get_schema(&state->streams[i], response_schema.get()) != 0)
            return {ADBC_STATUS_INTERNAL, "Failed to read a parameter result schema"};
        if (i == 0)
        {
            ArrowSchemaMove(response_schema.get(), &state->schema);
        }
        else if (!schemasEqual(&state->schema, response_schema.get()))
        {
            return {ADBC_STATUS_INTERNAL, "Parameter executions returned incompatible schemas"};
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
