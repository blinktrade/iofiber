/* Copyright (c) 2018, 2019 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_MUTEX_H
#define TRIAL_IOFIBER_MUTEX_H

#include <vector>
#include <functional>

#include <trial/iofiber/fiber.hpp>

namespace trial {
namespace iofiber {

template<class Strand>
class basic_mutex
{
public:
    using executor_type = Strand;

    class guard
    {
    public:
        guard(basic_mutex<Strand> &mutex, fiber::this_fiber this_fiber)
            : mutex(&mutex)
        {
            mutex.lock(this_fiber);
        }

        guard(guard &&o)
            : mutex(o.mutex)
        {
            o.mutex = nullptr;
        }

        guard(const guard &) = delete;

        ~guard()
        {
            if (!mutex) // moved
                return;

            mutex->unlock();
        }

    private:
        basic_mutex<Strand> *mutex;
    };

    basic_mutex(executor_type executor)
        : executor(std::move(executor))
        , locked(false)
    {}

    executor_type get_executor() const
    {
        return executor;
    }

    void lock(fiber::this_fiber this_fiber)
    {
        // TODO: inter-strand communication
        assert(this_fiber.get_executor() == executor);

        this_fiber.yield();

        if (locked) {
            fiber::this_fiber::disable_interruption di(this_fiber);
            boost::ignore_unused(di);
            auto& pimpl = this_fiber.pimpl_;
            pending.emplace_back(pimpl);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        locked = true;
    }

    void unlock()
    {
        assert(locked);

        // TODO: inter-strand communication
        assert(executor.running_in_this_thread());

        locked = false;

        if (pending.size() == 0)
            return;

        auto next{pending.back()};
        pending.pop_back();
        next->executor.post([next]() {
            next->coro = std::move(next->coro).resume();
        }, std::allocator<void>{});
    }

private:
    executor_type executor;
    bool locked;
    std::vector<std::shared_ptr<fiber::this_fiber::impl>> pending;
};

using mutex = basic_mutex<boost::asio::io_context::strand>;

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_MUTEX_H
