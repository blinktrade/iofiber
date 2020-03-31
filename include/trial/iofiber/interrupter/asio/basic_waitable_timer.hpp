/* Copyright (c) 2020 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_INTERRUPTER_ASIO_BASIC_WAITABLE_TIMER_H
#define TRIAL_IOFIBER_INTERRUPTER_ASIO_BASIC_WAITABLE_TIMER_H

#include <trial/iofiber/fiber.hpp>
#include <boost/asio/basic_waitable_timer.hpp>

namespace trial {
namespace iofiber {

template<class Clock, class WaitTraits, class Executor>
struct interrupter_for<
    boost::asio::basic_waitable_timer<Clock, WaitTraits, Executor>>
{
    template<class T>
    static void assign(
        const T& this_fiber,
        boost::asio::basic_waitable_timer<Clock, WaitTraits, Executor>& timer)
    {
        this_fiber.interrupter = [&timer]() { timer.cancel(); };
    }

    template<class T, class... Args>
    static void on_result(const T& /*this_fiber*/,
                          boost::system::error_code& ec, Args&...)
    {
        if (ec == boost::asio::error::operation_aborted)
            throw trial::iofiber::fiber_interrupted();
    }
};

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_INTERRUPTER_ASIO_BASIC_WAITABLE_TIMER_H
