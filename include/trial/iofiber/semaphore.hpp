/* Copyright (c) 2018 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_SEMAPHORE_H
#define TRIAL_IOFIBER_SEMAPHORE_H

#include <list>
#include <chrono>
#include <functional>

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <trial/iofiber/fiber.hpp>

namespace trial {
namespace iofiber {

template<class Strand>
class basic_semaphore
{
public:
    using executor_type = Strand;
    using duration = std::chrono::steady_clock::duration;

    basic_semaphore(executor_type executor)
        : executor(std::move(executor))
        , free(0)
    {}

    executor_type get_executor() const
    {
        return executor;
    }

    void post()
    {
        // TODO: inter-strand communication
        assert(executor.running_in_this_thread());

        if (pending.size() == 0) {
            ++free;
            return;
        }

        for (auto it = pending.rbegin() ; it != pending.rend() ; ++it) {
            if ((*it)())
                return;
        }
        ++free;
    }

    void wait_for(duration timeout_duration, fiber::this_fiber this_fiber)
    {
        // TODO: inter-strand communication
        assert(this_fiber.get_executor() == executor);

        if (free > 0) {
            --free;
            return;
        }

        // suspend
        boost::asio::async_completion<
            fiber::this_fiber, void(boost::system::error_code)
        > init{this_fiber};

        // TODO: improve interruption support
        fiber::this_fiber::disable_interruption di(this_fiber);

        boost::asio::steady_timer timer{executor.context()};
        timer.expires_from_now(timeout_duration);
        pending.emplace_front([&timer]() { return timer.cancel() > 0; });
        auto it = pending.begin();

        auto handler = std::move(init.completion_handler);
        timer.async_wait(
            boost::asio::bind_executor(
                executor,
                [handler,this,it](boost::system::error_code ec) mutable {
                    pending.erase(it);
                    // wake up one fiber
                    if (ec == boost::asio::error::operation_aborted) {
                        handler({});
                    } else {
                        handler(make_error_code(
                            boost::system::errc::timed_out));
                    }
                }
            )
        );

        init.result.get();

        // dump interruption point
        fiber::this_fiber::restore_interruption ri{di};
        boost::ignore_unused(ri);
        this_fiber.yield();
    }

    int get_value() const
    {
        // TODO: inter-strand communication
        assert(executor.running_in_this_thread());

        return static_cast<int>(free) - static_cast<int>(pending.size());
    }

    void reset()
    {
        // TODO: inter-strand communication
        assert(executor.running_in_this_thread());

        free = 0;
    }

private:
    executor_type executor;
    unsigned free;
    std::list<std::function<bool()>> pending;
};

using semaphore = basic_semaphore<boost::asio::io_context::strand>;

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_SEMAPHORE_H
