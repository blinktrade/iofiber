/* Copyright (c) 2019 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_CONDITION_VARIABLE_H
#define TRIAL_IOFIBER_CONDITION_VARIABLE_H

#include <trial/iofiber/mutex.hpp>
#include <algorithm>

namespace trial {
namespace iofiber {

template<class Strand>
class basic_condition_variable
{
public:
    using executor_type = Strand;

    basic_condition_variable(executor_type executor)
        : executor(executor)
        , pending(std::make_shared<std::deque<std::shared_ptr<
            typename basic_fiber<Strand>::this_fiber::impl
        >>>())
    {}

    executor_type get_executor() const
    {
        return executor;
    }

    basic_condition_variable(const basic_condition_variable&) = delete;
    basic_condition_variable& operator=(const basic_condition_variable&)
        = delete;

    void wait(basic_unique_lock<Strand>& lk,
              typename basic_fiber<Strand>::this_fiber this_fiber)
    {
#ifndef NDEBUG
        assert(this_fiber.suspension_disallowed == 0);
#endif // NDEBUG

        assert(lk.mutex());
        assert(lk.owns_lock());
        assert(lk.mutex()->get_executor() == executor);

        auto& pimpl = this_fiber.pimpl_;
        if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
            throw fiber_interrupted();
        }

        auto& executor = this->executor;
        auto& pending = this->pending;
        this_fiber.interrupter = [executor,pending,pimpl]() {
            executor.dispatch([pending,pimpl]() {
                auto it = std::find(pending->begin(), pending->end(), pimpl);
                if (it == pending->end())
                    return;

                pending->erase(it);
                pimpl->executor.defer([pimpl]() {
                    // Fiber can be parked on wait() while an interruption
                    // request is sent. While the interruption delivery is
                    // scheduled, the wait() result may arrive just so the user
                    // could disable interruptions and do another wait(). This
                    // time, this task here is executed and would be trying to
                    // interrupt the wrong wait() call. What we do: a spurious
                    // wakeup because we already removed the fiber from the
                    // pending list and possibly lost a signalization.
                    if (!pimpl->interruption_enabled) {
                        pimpl->coro = std::move(pimpl->coro).resume();
                        return;
                    }

                    pimpl->coro = std::move(pimpl->coro).resume_with(
                        [pimpl](boost::context::fiber&& sink)
                            -> boost::context::fiber {
                            pimpl->coro.swap(sink);
                            throw fiber_interrupted();
                            return {};
                        }
                    );
                }, std::allocator<void>{});
            }, std::allocator<void>{});
        };

        auto mutex = lk.mutex();
        executor.dispatch([pimpl,mutex,pending] {
            pending->emplace_back(pimpl);
            mutex->unlock();
        }, std::allocator<void>{});

        try {
            auto ex_work_guard = boost::asio::make_work_guard(pimpl->executor);
            pimpl->coro = std::move(pimpl->coro).resume();
            pimpl->interrupter = nullptr;
            ex_work_guard.reset();
            mutex->lock(this_fiber);
        } catch (const fiber_interrupted&) {
            pimpl->interrupter = nullptr;
            mutex->lock(this_fiber);
            throw;
        }
    }

    void notify_one()
    {
        auto& pending = this->pending;
        executor.dispatch([pending]() {
            if (pending->size() == 0)
                return;

            auto next{pending->front()};
            pending->pop_front();
            next->executor.post([next]() {
                next->coro = std::move(next->coro).resume();
            }, std::allocator<void>{});
        }, std::allocator<void>{});
    }

    void notify_all()
    {
        auto& pending = this->pending;
        executor.dispatch([pending]() {
            for (auto& p: *pending) {
                p->executor.post([p]() {
                    p->coro = std::move(p->coro).resume();
                }, std::allocator<void>{});
            }
            pending->clear();
        }, std::allocator<void>{});
    }

private:
    executor_type executor;
    std::shared_ptr<std::deque<std::shared_ptr<
        typename basic_fiber<Strand>::this_fiber::impl
    >>> pending;
};

using condition_variable
    = basic_condition_variable<boost::asio::io_context::strand>;

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_CONDITION_VARIABLE_H
