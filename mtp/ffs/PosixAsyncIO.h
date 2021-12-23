/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _POSIXASYNCIO_H
#define _POSIXASYNCIO_H

#include <condition_variable>
#include <mutex>
#include <sys/cdefs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/**
 * Provides a subset of POSIX aio operations.
 */

struct aiocb {
    int aio_fildes;
    void *aio_buf;

    off64_t aio_offset;
    size_t aio_nbytes;

    // Used internally
    bool read;
    bool queued;
    ssize_t ret;
    int error;

    std::mutex lock;
    std::condition_variable cv;

    aiocb();
    ~aiocb();
};

// Submit a request for IO to be completed
int aio_read(struct aiocb *);
int aio_write(struct aiocb *);

// Suspend current thread until given IO is complete, at which point
// its return value and any errors can be accessed
// All submitted requests must have a corresponding suspend.
// aiocb->aio_buf must refer to valid memory until after the suspend call
int aio_suspend(struct aiocb *[], int, const struct timespec *);
int aio_error(const struct aiocb *);
ssize_t aio_return(struct aiocb *);

// Helper method for setting aiocb members
void aio_prepare(struct aiocb *, void*, size_t, off64_t);

#endif // POSIXASYNCIO_H

