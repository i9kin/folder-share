#pragma once

// inspired but not finalized asynchronous waiting https://github.com/BenBrock/async_io

#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

namespace async_io {

struct AsyncRead {
    AsyncRead(const char* fname, off_t offset, size_t size)
    {
        int fd = open(fname, O_RDONLY);
        if (fd == -1) {
            throw std::runtime_error("AsyncRead could not read file \"" + std::string(fname) + "\"");
        }
        buffer_.resize(size);
        aio_request_.aio_fildes = fd;
        aio_request_.aio_buf = buffer_.data();
        aio_request_.aio_offset = offset;
        aio_request_.aio_nbytes = buffer_.size();
        aio_request_.aio_sigevent.sigev_notify = SIGEV_NONE;
        aio_request_.aio_reqprio = 0;

        int status = aio_read(&aio_request_);
        if (status != 0) {
            throw std::runtime_error("Could not read from fd " + std::to_string(fd) + std::to_string(status));
        }
    }

    bool is_ready() const
    {
        if (ready_) {
            return true;
        }
        int return_value = aio_error(&aio_request_);
        return (return_value != EINPROGRESS);
    }

    std::vector<char> get()
    {
        if (ready_) {
            return buffer_;
        }

        while (!is_ready()) {
          // TODO async waiting
        }

        ready_ = true;

        int return_value = aio_return(&aio_request_);
        if (return_value == -1) {
            throw std::runtime_error("Asynchronous read error'd out.");
        }
        return buffer_;
    }

    aiocb aio_request_;
    std::vector<char> buffer_;
    bool ready_ = false;
};

using read = AsyncRead;

} // end async_io
