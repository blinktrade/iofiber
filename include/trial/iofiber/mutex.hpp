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
        guard(basic_mutex<Strand> &mutex,
              typename basic_fiber<Strand>::this_fiber this_fiber)
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

    basic_mutex(const basic_mutex&) = delete;
    basic_mutex& operator=(const basic_mutex&) = delete;

    executor_type get_executor() const
    {
        return executor;
    }

    void lock(typename basic_fiber<Strand>::this_fiber this_fiber)
    {
        typename basic_fiber<Strand>::this_fiber::disable_interruption di(
            this_fiber);
        boost::ignore_unused(di);

        assert(this_fiber.get_executor().running_in_this_thread());
        if (this_fiber.get_executor() == executor)
            return same_strand_lock(this_fiber);

        auto& pimpl = this_fiber.pimpl_;
        pimpl->executor.defer([pimpl,this]() {
            executor.dispatch([pimpl,this] {
                if (locked) {
                    pending.emplace_back(pimpl);
                    return;
                }

                locked = true;
                pimpl->executor.post([pimpl]() {
                    pimpl->coro = std::move(pimpl->coro).resume();
                }, std::allocator<void>{});
            }, std::allocator<void>{});
        }, std::allocator<void>{});
        pimpl->coro = std::move(pimpl->coro).resume();
    }

    void unlock()
    {
        executor.dispatch([this]() {
            assert(locked);
            locked = false;

            if (pending.size() == 0)
                return;

            auto next{pending.back()};
            pending.pop_back();
            next->executor.post([next]() {
                next->coro = std::move(next->coro).resume();
            }, std::allocator<void>{});
        }, std::allocator<void>{});
    }

private:
    void same_strand_lock(typename basic_fiber<Strand>::this_fiber this_fiber)
    {
        if (locked) {
            auto& pimpl = this_fiber.pimpl_;
            pending.emplace_back(pimpl);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        locked = true;
    }

    executor_type executor;
    bool locked;
    std::vector<std::shared_ptr<
        typename basic_fiber<Strand>::this_fiber::impl
    >> pending;
};

using mutex = basic_mutex<boost::asio::io_context::strand>;

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_MUTEX_H
