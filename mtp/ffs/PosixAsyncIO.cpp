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

#include <android-base/logging.h>
#include <memory>
#include <pthread.h>
#include <queue>
#include <thread>
#include <unistd.h>

#include "PosixAsyncIO.h"

namespace {

std::thread gWorkerThread;
std::deque<struct aiocb*> gWorkQueue;
bool gSuspended = true;
int gAiocbRefcount = 0;
std::mutex gLock;
std::condition_variable gWait;

void work_func(void *) {
    pthread_setname_np(pthread_self(), "AsyncIO work");
    while (true) {
        struct aiocb *aiocbp;
        {
            std::unique_lock<std::mutex> lk(gLock);
            gWait.wait(lk, []{return gWorkQueue.size() > 0 || gSuspended;});
            if (gSuspended)
                return;
            aiocbp = gWorkQueue.back();
            gWorkQueue.pop_back();
        }
        CHECK(aiocbp->queued);
        int ret;
        if (aiocbp->read) {
            ret = TEMP_FAILURE_RETRY(pread64(aiocbp->aio_fildes,
                    aiocbp->aio_buf, aiocbp->aio_nbytes, aiocbp->aio_offset));
        } else {
            ret = TEMP_FAILURE_RETRY(pwrite64(aiocbp->aio_fildes,
               aiocbp->aio_buf, aiocbp->aio_nbytes, aiocbp->aio_offset));
        }
        {
            std::unique_lock<std::mutex> lk(aiocbp->lock);
            aiocbp->ret = ret;
            if (aiocbp->ret == -1) {
                aiocbp->error = errno;
            }
            aiocbp->queued = false;
        }
        aiocbp->cv.notify_all();
    }
}

int aio_add(struct aiocb *aiocbp) {
    CHECK(!aiocbp->queued);
    aiocbp->queued = true;
    {
        std::unique_lock<std::mutex> lk(gLock);
        gWorkQueue.push_front(aiocbp);
    }
    gWait.notify_one();
    return 0;
}

} // end anonymous namespace

aiocb::aiocb() {
    this->ret = 0;
    this->queued = false;
    {
        std::unique_lock<std::mutex> lk(gLock);
        if (gAiocbRefcount == 0) {
            CHECK(gWorkQueue.size() == 0);
            CHECK(gSuspended);
            gSuspended = false;
            gWorkerThread = std::thread(work_func, nullptr);
        }
        gAiocbRefcount++;
    }
}

aiocb::~aiocb() {
    CHECK(!this->queued);
    {
        std::unique_lock<std::mutex> lk(gLock);
        CHECK(!gSuspended);
        if (gAiocbRefcount == 1) {
            CHECK(gWorkQueue.size() == 0);
            gSuspended = true;
            lk.unlock();
            gWait.notify_one();
            gWorkerThread.join();
            lk.lock();
        }
        gAiocbRefcount--;
    }
}

int aio_read(struct aiocb *aiocbp) {
    aiocbp->read = true;
    return aio_add(aiocbp);
}

int aio_write(struct aiocb *aiocbp) {
    aiocbp->read = false;
    return aio_add(aiocbp);
}

int aio_error(const struct aiocb *aiocbp) {
    return aiocbp->error;
}

ssize_t aio_return(struct aiocb *aiocbp) {
    return aiocbp->ret;
}

int aio_suspend(struct aiocb *aiocbp[], int n,
        const struct timespec *) {
    for (int i = 0; i < n; i++) {
        {
            std::unique_lock<std::mutex> lk(aiocbp[i]->lock);
            aiocbp[i]->cv.wait(lk, [aiocbp, i]{return !aiocbp[i]->queued;});
        }
    }
    return 0;
}

void aio_prepare(struct aiocb *aiocbp, void* buf, size_t count, off64_t offset) {
    aiocbp->aio_buf = buf;
    aiocbp->aio_offset = offset;
    aiocbp->aio_nbytes = count;
}
