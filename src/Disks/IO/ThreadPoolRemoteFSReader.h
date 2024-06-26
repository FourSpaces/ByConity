#pragma once

#include <IO/AsynchronousReader.h>
#include <IO/ReadBuffer.h>
#include <Common/ThreadPool.h>
#include <Interpreters/threadPoolCallbackRunner.h>

namespace DB
{

struct AsyncReadCounters;

class ThreadPoolRemoteFSReader : public IAsynchronousReader
{
public:
    ThreadPoolRemoteFSReader(size_t pool_size, size_t queue_size_);

    std::future<IAsynchronousReader::Result> submit(Request request) override;

    void wait() override;

private:
    std::unique_ptr<ThreadPool> pool;
};

class RemoteFSFileDescriptor : public IAsynchronousReader::IFileDescriptor
{
public:
    explicit RemoteFSFileDescriptor(
        ReadBuffer & read_buffer_,
        std::shared_ptr<AsyncReadCounters> async_read_counters_)
        : read_buffer(read_buffer_)
        , async_read_counters(async_read_counters_) {}

    IAsynchronousReader::Result readInto(char * data, size_t size, size_t offset, size_t ignore = 0);

    std::shared_ptr<AsyncReadCounters> getReadCounters() const { return async_read_counters; }

private:
    ReadBuffer & read_buffer;
    std::shared_ptr<AsyncReadCounters> async_read_counters;
};

}
