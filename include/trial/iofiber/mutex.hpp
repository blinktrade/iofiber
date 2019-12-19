/* Copyright (c) 2018, 2019 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_MUTEX_H
#define TRIAL_IOFIBER_MUTEX_H

#include <deque>
#include <functional>

#include <trial/iofiber/fiber.hpp>

namespace trial {
namespace iofiber {

template<class Strand>
class basic_mutex
{
public:
    using executor_type = Strand;

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
        auto ex_work_guard = boost::asio::make_work_guard(pimpl->executor);
        pimpl->coro = std::move(pimpl->coro).resume();
    }

    void unlock()
    {
        executor.dispatch([this]() {
            assert(locked);
            locked = false;

            if (pending.size() == 0)
                return;

            auto next{pending.front()};
            pending.pop_front();
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
            auto ex_work_guard = boost::asio::make_work_guard(pimpl->executor);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        locked = true;
    }

    executor_type executor;
    bool locked;
    std::deque<std::shared_ptr<
        typename basic_fiber<Strand>::this_fiber::impl
    >> pending;
};

using mutex = basic_mutex<boost::asio::io_context::strand>;

template<class Strand>
class basic_unique_lock
{
public:
    basic_unique_lock(basic_mutex<Strand>& mutex,
          typename basic_fiber<Strand>::this_fiber this_fiber)
        : mutex_(&mutex)
        , owns_lock_(true)
    {
        mutex_->lock(this_fiber);
    }

    basic_unique_lock(basic_unique_lock&& o)
        : mutex_(o.mutex_)
        , owns_lock_(o.owns_lock_)
    {
        o.mutex_ = nullptr;
    }

    basic_unique_lock& operator=(basic_unique_lock&& o)
    {
        if (mutex_ && owns_lock_)
            mutex_->unlock();

        mutex_ = o.mutex_;
        owns_lock_ = o.owns_lock_;
        o.mutex_ = nullptr;
        return *this;
    }

    ~basic_unique_lock()
    {
        if (!mutex_) // moved
            return;

        if (owns_lock_)
            mutex_->unlock();
    }

    void lock(typename basic_fiber<Strand>::this_fiber this_fiber)
    {
        if (!mutex_) {
            throw std::system_error{
                make_error_code(std::errc::operation_not_permitted)};
        }
        if (owns_lock_) {
            throw std::system_error{
                make_error_code(std::errc::resource_deadlock_would_occur)};
        }

        mutex_->lock(this_fiber);
        owns_lock_ = true;
    }

    void unlock()
    {
        if (!mutex_ || !owns_lock_) {
            throw std::system_error{
                make_error_code(std::errc::operation_not_permitted)};
        }

        mutex_->unlock();
        owns_lock_ = false;
    }

    basic_mutex<Strand>* release() noexcept
    {
        auto m = mutex_;
        mutex_ = nullptr;
        return m;
    }

    basic_mutex<Strand>* mutex() const noexcept
    {
        return mutex_;
    }

    bool owns_lock() const noexcept
    {
        return owns_lock_;
    }

private:
    basic_mutex<Strand>* mutex_;
    bool owns_lock_;
};

using unique_lock = basic_unique_lock<boost::asio::io_context::strand>;

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_MUTEX_H
