/* Copyright (c) 2020 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_INTERRUPTER_ASIO_SIGNAL_SET_H
#define TRIAL_IOFIBER_INTERRUPTER_ASIO_SIGNAL_SET_H

#include <trial/iofiber/fiber.hpp>
#include <boost/asio/signal_set.hpp>

namespace trial {
namespace iofiber {

template<>
struct interrupter_for<boost::asio::signal_set>
{
    template<class T>
    static void assign(const T& this_fiber, boost::asio::signal_set& sig)
    {
        this_fiber.interrupter = [&sig]() { sig.cancel(); };
    }

    template<class T, class... Args>
    static void on_result(const T& /*this_fiber*/,
                          boost::system::error_code& ec, Args&&...)
    {
        if (ec == boost::asio::error::operation_aborted)
            throw trial::iofiber::fiber_interrupted{};
    }
};

} // namespace iofiber
} // namespace trial

#endif // TRIAL_IOFIBER_INTERRUPTER_ASIO_SIGNAL_SET_H
