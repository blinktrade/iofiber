/* Copyright (c) 2018 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_BARRIER_H
#define TRIAL_IOFIBER_BARRIER_H

#include <vector>
#include <functional>

#include <trial/iofiber/fiber.hpp>

namespace trial {
namespace iofiber {

// TODO: executor is now unused. Remove it or update barrier to be useful in
// multi-threaded environment.

class fiber_barrier
{
public:
    fiber_barrier(boost::asio::io_context &ctx, std::size_t initial)
        : executor(ctx.get_executor())
        , target(initial)
    {
        assert(initial > 0);
        pending.reserve(target - 1);
    }

    fiber_barrier(const fiber_barrier&) = delete;
    fiber_barrier& operator=(const fiber_barrier&) = delete;

    bool wait(fiber::this_fiber this_fiber)
    {
        if (pending.size() < target - 1) { // locked
            boost::asio::async_completion<fiber::this_fiber, void()>
                init{this_fiber};
            auto handler = std::move(init.completion_handler);
            pending.emplace_back([handler]() mutable {
                auto ex = handler.get_executor();
                ex.post(std::move(handler), std::allocator<void>{});
            });
            init.result.get();
            return false;
        }

        for (auto &p: pending)
            p();
        pending.clear();

        return true;
    }

private:
    boost::asio::io_context::executor_type executor;
    std::size_t target;
    std::vector<std::function<void()>> pending;
};

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_BARRIER_H
