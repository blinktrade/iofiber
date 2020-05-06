/* Copyright (c) 2018 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE fiber
#include <boost/test/unit_test.hpp>

#include <type_traits>
#include <thread>
#include <vector>

#include <boost/asio/steady_timer.hpp>

#include <trial/iofiber/interrupter/asio/basic_waitable_timer.hpp>
#include <trial/iofiber/fiber.hpp>

namespace asio = boost::asio;
using namespace trial::iofiber;

struct async_identity_initiation
{
    template<class Handler, class... Args>
    void operator()(Handler&& handler, Args&&... args)
    {
        auto ex = asio::get_associated_executor(handler);
        ex.post(std::bind(std::forward<Handler>(handler),
                          std::forward<Args>(args)...),
                std::allocator<void>{});
    }
};

template<class CompletionToken, class... Args>
BOOST_ASIO_INITFN_RESULT_TYPE(
    CompletionToken, void(typename std::decay<Args>::type...))
async_identity(CompletionToken&& token, Args&&... args)
{
    return asio::async_initiate<
        CompletionToken, void(typename std::decay<Args>::type...)
    >(async_identity_initiation{}, token, std::forward<Args>(args)...);
}

struct async_never_finishes_initiation
{
    async_never_finishes_initiation(
        bool& op_started, const asio::io_context::strand& strand)
        : op_started(op_started)
        , ex(strand)
    {}

    template<class Handler>
    void operator()(Handler&&)
    {
        // Yeah, we're not calling handler. It is "never finishes" after all.
        //
        // Technically we should only initialize work guard here, when operation
        // starts, and maintain it while the operation is in progress, but then
        // the io_context would stay busy forever. The asio contract is broken
        // for the purposes of IOFiber testability.
        op_started = true;
    }

    bool& op_started;
    asio::executor_work_guard<asio::io_context::strand> ex;
};

template<class CompletionToken>
BOOST_ASIO_INITFN_RESULT_TYPE(CompletionToken, void())
async_never_finishes(bool& op_started, CompletionToken&& token)
{
    return asio::async_initiate<CompletionToken, void()>(
        async_never_finishes_initiation{op_started, token.get_executor()},
        token);
}

struct dummy_io_object
{
    template<class CompletionToken>
    void async_foo(int /*x*/, double /*y*/, CompletionToken&& token)
    {
        async_identity(std::forward<CompletionToken>(token));
    }

    template<class CompletionToken>
    int async_bar(CompletionToken&& token)
    {
        async_identity(std::forward<CompletionToken>(token),
                       make_error_code(asio::error::operation_aborted));
        return 3;
    }

    template<class CompletionToken>
    int async_baz(int x, CompletionToken&& token)
    {
        return async_identity(std::forward<CompletionToken>(token),
                              boost::system::error_code{}, x);
    }

    void cancel()
    {}
};

namespace trial {
namespace iofiber {

template<>
struct interrupter_for<dummy_io_object>
{
    template<class T>
    static void assign(T this_fiber, dummy_io_object& o)
    {
        this_fiber.interrupter = [&o]() { o.cancel(); };
    }

    template<class T, class... Args>
    static void on_result(T /*this_fiber*/, boost::system::error_code& ec,
                          Args&...)
    {
        if (ec == boost::asio::error::operation_aborted)
            throw fiber_interrupted();
    }

    template<class T, class... Args>
    static void on_result(T /*this_fiber*/, Args&...)
    {}
};

} // namespace iofiber
} // namespace trial

BOOST_AUTO_TEST_CASE(yield)
{
    asio::io_context ctx;
    std::vector<int> events;

    spawn(ctx, [&events](fiber::this_fiber this_fiber) {
        spawn(
            this_fiber,
            [&events](fiber::this_fiber /*this_fiber*/) {
                events.push_back(0);
            }
        ).detach();
        events.push_back(1);
        this_fiber.yield();
        events.push_back(2);
    }).detach();

    spawn(ctx, [&events](fiber::this_fiber /*this_fiber*/) {
        events.push_back(3);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1, 3, 0, 2}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(strand_inheritance)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    bool end = false;

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        BOOST_REQUIRE(this_fiber.get_executor() == strand);
        spawn(this_fiber, [&](fiber::this_fiber this_fiber) {
            BOOST_REQUIRE(this_fiber.get_executor() == strand);
            end = true;
        }).detach();
    }).detach();

    ctx.run();
    BOOST_REQUIRE(end);
}

BOOST_AUTO_TEST_CASE(same_strand_join)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [](fiber::this_fiber /*this_fiber*/) {
    });

    auto f2 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(0);
        this_fiber.yield();
        events.push_back(1);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        events.push_back(2);
        f2.join(this_fiber);
        BOOST_REQUIRE(!f2.interruption_caught());
        events.push_back(3);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{0, 2, 1, 3}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(inter_strand_join1)
{
    asio::io_context ctx1;
    asio::io_context::strand strand1{ctx1};
    std::thread::id tid1 = std::this_thread::get_id();

    asio::io_context ctx2;
    asio::io_context::strand strand2{ctx2};
    std::thread::id tid2;

    std::vector<int> events;

    auto f1 = spawn(
        strand1,
        [&](fiber::this_fiber /*this_fiber*/) {
            BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid1);
            events.push_back(0);
        }
    );

    spawn(strand2, [&](fiber::this_fiber this_fiber) {
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid2);
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid2);
    }).detach();

    auto t = std::thread([&]() {
        tid2 = std::this_thread::get_id();
        assert(tid1 != tid2);
        ctx2.run();
    });

    ctx1.run();
    t.join();

    BOOST_TEST(events == (std::vector<int>{0, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(inter_strand_join2)
{
    asio::io_context ctx1;
    asio::io_context::strand strand1{ctx1};
    std::thread::id tid1 = std::this_thread::get_id();

    asio::io_context ctx2;
    asio::io_context::strand strand2{ctx2};
    std::thread::id tid2;

    std::vector<int> events;

    auto f1 = spawn(strand1, [&](fiber::this_fiber this_fiber) {
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid1);
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    spawn(strand2, [&](fiber::this_fiber this_fiber) {
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid2);
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid2);
    }).detach();

    auto t = std::thread([&]() {
        tid2 = std::this_thread::get_id();
        assert(tid1 != tid2);
        ctx2.run();
    });

    ctx1.run();
    t.join();

    BOOST_TEST(events == (std::vector<int>{0, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(inter_strand_join3)
{
    asio::io_context ctx1;
    asio::io_context::strand strand1{ctx1};
    std::thread::id tid1 = std::this_thread::get_id();

    asio::io_context ctx2;
    asio::io_context::strand strand2{ctx2};
    std::thread::id tid2;

    std::vector<int> events;

    auto f1 = spawn(
        strand1,
        [&](fiber::this_fiber /*this_fiber*/) {
            BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid1);
            events.push_back(0);
        }
    );

    spawn(strand2, [&](fiber::this_fiber this_fiber) {
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid2);
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
        BOOST_REQUIRE_EQUAL(std::this_thread::get_id(), tid2);
    }).detach();

    auto t = std::thread([&]() {
        tid2 = std::this_thread::get_id();
        assert(tid1 != tid2);
        ctx2.run();
    });

    ctx1.run();
    t.join();

    BOOST_TEST(events == (std::vector<int>{0, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(token)
{
    asio::io_context ctx;
    bool end = false;

    spawn(ctx, [&end](fiber::this_fiber this_fiber) {
        boost::system::error_code ec;

        // empty
        static_assert(std::is_same<
                          decltype(async_identity(this_fiber)), void
                      >::value,
                      "");
        static_assert(std::is_same<
                          decltype(async_identity(this_fiber, ec)), void
                      >::value,
                      "");
        BOOST_REQUIRE_NO_THROW(async_identity(this_fiber, ec));

        // 1-arg
        BOOST_REQUIRE_EQUAL(async_identity(this_fiber, 42), 42);
        BOOST_REQUIRE_EQUAL(async_identity(this_fiber, 3), 3);
        BOOST_REQUIRE_EQUAL(async_identity(this_fiber, ec, 42), 42);
        BOOST_REQUIRE_EQUAL(async_identity(this_fiber, ec, 3), 3);

        // 2 args (tuple)
        BOOST_REQUIRE(async_identity(this_fiber, 3, 42)
                      == std::make_tuple(3, 42));
        BOOST_REQUIRE(async_identity(this_fiber, 3, 42, 5)
                      == std::make_tuple(3, 42, 5));
        BOOST_REQUIRE(async_identity(this_fiber, ec, 3, 42)
                      == std::make_tuple(3, 42));
        BOOST_REQUIRE(async_identity(this_fiber, ec, 3, 42, 5)
                      == std::make_tuple(3, 42, 5));

        // exception
        ec = make_error_code(boost::system::errc::interrupted);
        BOOST_REQUIRE_EXCEPTION(
            async_identity(this_fiber, ec),
            boost::system::system_error,
            [ec](const boost::system::system_error& e) {
                return e.code() == ec;
            }
        );
        BOOST_REQUIRE_EXCEPTION(
            async_identity(this_fiber, ec, 4),
            boost::system::system_error,
            [ec](const boost::system::system_error& e) {
                return e.code() == ec;
            }
        );
        BOOST_REQUIRE_EXCEPTION(
            async_identity(this_fiber, ec, 4, 42),
            boost::system::system_error,
            [ec](const boost::system::system_error& e) {
                return e.code() == ec;
            }
        );

        // no exception
        this_fiber = this_fiber[ec];
        boost::system::error_code ec2;
        BOOST_REQUIRE_EQUAL(
            ec, make_error_code(boost::system::errc::interrupted));

        async_identity(this_fiber, ec2);
        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE_EQUAL(async_identity(this_fiber, ec2, 4), 4);
        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE(async_identity(this_fiber, ec2, 4, 43)
                      == std::make_tuple(4, 43));
        BOOST_REQUIRE(!ec);

        ec2 = make_error_code(boost::system::errc::interrupted);
        async_identity(this_fiber, ec2);
        BOOST_REQUIRE_EQUAL(ec, ec2);
        async_identity(this_fiber, ec2, 3);
        BOOST_REQUIRE_EQUAL(ec, ec2);
        async_identity(this_fiber, ec2, 3, 534);
        BOOST_REQUIRE_EQUAL(ec, ec2);

        // re-enable exceptions
        this_fiber = this_fiber[nullptr];
        ec = make_error_code(boost::system::errc::interrupted);
        BOOST_REQUIRE_EXCEPTION(
            async_identity(this_fiber, ec),
            boost::system::system_error,
            [ec](const boost::system::system_error& e) {
                return e.code() == ec;
            }
        );

        end = true;
    }).detach();

    ctx.run();
    BOOST_REQUIRE(end);
}

BOOST_AUTO_TEST_CASE(token_preserve_executor)
{
    asio::io_context ctx;
    bool end = false;

    spawn(ctx, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::milliseconds(1));
        BOOST_REQUIRE(this_fiber.get_executor().running_in_this_thread());
        timer.async_wait(this_fiber);
        BOOST_REQUIRE(this_fiber.get_executor().running_in_this_thread());
        end = true;
    }).detach();

    ctx.run();
    BOOST_REQUIRE(end);
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_and_detach_on_yield)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};

    auto f1 = spawn(strand, [](fiber::this_fiber this_fiber) {
        for (;;) {
            this_fiber.yield();
        }
    });

    spawn(strand, [&](fiber::this_fiber /*this_fiber*/) {
        f1.interrupt();
        f1.detach();
    }).detach();

    ctx.run();
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_and_detach_on_yield2)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        for (;;) {
            try {
                this_fiber.yield();
            } catch (const fiber_interrupted&) {
                this_fiber.yield();
            }
        }
    });

    spawn(strand, [&](fiber::this_fiber /*this_fiber*/) {
        f1.interrupt();
        f1.detach();
    }).detach();

    ctx.run();
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_and_detach_on_yield_before_run)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    bool set = false;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        set = true;
        for (;;) {
            this_fiber.yield();
        }
    });

    f1.interrupt();
    f1.detach();

    ctx.run();

    BOOST_REQUIRE(set);
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_and_join_on_yield)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        for (;;) {
            this_fiber.yield();
        }
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(f1.interruption_caught());
    }).detach();

    ctx.run();
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_on_join)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    auto f2 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        try {
            f1.join(this_fiber);
        } catch (const fiber_interrupted&) {
            f1.detach();
            events.push_back(3);
            throw;
        }
        events.push_back(1);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f2.interrupt();
        f2.join(this_fiber);
        BOOST_REQUIRE(f2.interruption_caught());
        events.push_back(2);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{3, 2, 0}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_on_join2)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    auto f2 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        try {
            f1.join(this_fiber);
            events.push_back(1);
        } catch (const fiber_interrupted&) {
            try {
                f1.join(this_fiber);
                events.push_back(3);
            } catch (const fiber_interrupted&) {
                f1.detach();
                events.push_back(4);
            }
        }
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        f2.interrupt();
        f2.join(this_fiber);
        BOOST_REQUIRE(!f2.interruption_caught());
        events.push_back(2);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{4, 2, 0}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_on_token)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(same_strand_interrupt_on_token2)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        try {
            timer.expires_after(std::chrono::seconds(1));
            timer.async_wait(this_fiber);
            events.push_back(0);
        } catch (const fiber_interrupted&) {
            timer.expires_after(std::chrono::seconds(1));
            timer.async_wait(this_fiber);
            events.push_back(2);
        }
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(same_strand_custom_interrupter)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        this_fiber.interrupter = [&timer]() { timer.cancel(); };
        try {
            timer.async_wait(this_fiber);
        } catch (const boost::system::system_error& e) {
            events.push_back(2);
            BOOST_REQUIRE_EQUAL(e.code(), asio::error::operation_aborted);
            return;
        }
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{2, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(auto_custom_interrupter)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber.with_intr(timer));
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(auto_custom_interrupter_without_exception)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        boost::system::error_code ec;
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber[ec].with_intr(timer));
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(auto_custom_interrupter_without_ec_callback)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        dummy_io_object o;
        o.async_foo(42, 4.2, this_fiber.with_intr(o));
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{0, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(auto_custom_interrupter_with_retval)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        dummy_io_object o;
        int x = o.async_bar(this_fiber.with_intr(o));
        assert(x == 3); //< unreachable anyway
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(auto_custom_interrupter_with_retval_no_interrupt)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        dummy_io_object o;
        int x = o.async_baz(33, this_fiber.with_intr(o));
        BOOST_REQUIRE_EQUAL(x, 33);
        events.push_back(0);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{0}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(consume_auto_custom_interrupter)
{
    // An interrupter for one operation should not leak to be chosen as the
    // interrupter of another operation. If operation finishes successfully and
    // it is not interrupted, the custom interrupter should not be used in the
    // next operation.
    asio::io_context ctx;

    spawn(ctx, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::milliseconds(1));

        BOOST_REQUIRE(!this_fiber.interrupter);
        timer.async_wait(this_fiber.with_intr(timer));
        BOOST_REQUIRE(!this_fiber.interrupter);
    }).detach();

    ctx.run();
}

BOOST_AUTO_TEST_CASE(disable_interrupt_on_yield)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        fiber::this_fiber::disable_interruption di(this_fiber);
        boost::ignore_unused(di);
        for (int i = 0 ; i != 10 ; ++i) {
            this_fiber.yield();
        }
        events.push_back(1);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(2);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{1, 2}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(disable_interrupt_on_yield2)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        try {
            for (int i = 0 ; i != 10 ; ++i) {
                this_fiber.yield();
            }
            events.push_back(1);
        } catch (const fiber_interrupted&) {
            fiber::this_fiber::disable_interruption di(this_fiber);
            boost::ignore_unused(di);
            this_fiber.yield();
            events.push_back(3);
        }
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(2);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{3, 2}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(disable_interrupt_on_join)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    auto f2 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        fiber::this_fiber::disable_interruption di(this_fiber);
        boost::ignore_unused(di);
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        f2.interrupt();
        f2.join(this_fiber);
        BOOST_REQUIRE(!f2.interruption_caught());
        events.push_back(2);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{0, 1, 2}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(disable_interrupt_on_join2)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    auto f2 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        try {
            f1.join(this_fiber);
            events.push_back(1);
        } catch (const fiber_interrupted&) {
            fiber::this_fiber::disable_interruption di(this_fiber);
            boost::ignore_unused(di);
            f1.join(this_fiber);
            BOOST_REQUIRE(!f1.interruption_caught());
            events.push_back(3);
        }
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        f2.interrupt();
        f2.join(this_fiber);
        BOOST_REQUIRE(!f2.interruption_caught());
        events.push_back(2);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{0, 3, 2}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(disable_interrupt_on_token)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        fiber::this_fiber::disable_interruption di(this_fiber);
        boost::ignore_unused(di);
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        events.push_back(0);
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{0, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(disable_interrupt_on_token2)
{
    asio::io_context ctx;
    asio::io_context::strand strand{ctx};
    std::vector<int> events;

    auto f1 = spawn(strand, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        try {
            timer.expires_after(std::chrono::seconds(1));
            timer.async_wait(this_fiber);
            events.push_back(0);
        } catch (const fiber_interrupted&) {
            fiber::this_fiber::disable_interruption di(this_fiber);
            boost::ignore_unused(di);
            timer.expires_after(std::chrono::seconds(1));
            timer.async_wait(this_fiber);
            events.push_back(2);
        }
    });

    spawn(strand, [&](fiber::this_fiber this_fiber) {
        f1.interrupt();
        f1.join(this_fiber);
        BOOST_REQUIRE(!f1.interruption_caught());
        events.push_back(1);
    }).detach();

    ctx.run();

    BOOST_TEST(events == (std::vector<int>{2, 1}),
               boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(call_interrupt_on_invalid_fiber)
{
    asio::io_context ctx;

    bool f_finished = false;

    auto f = spawn(ctx, [&](fiber::this_fiber this_fiber) {
        asio::steady_timer timer{this_fiber.get_executor().context()};
        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        f_finished = true;
    });

    f.detach();
    f.interrupt();

    ctx.run();

    BOOST_REQUIRE(f_finished);
}

// Async operations should not even initiate if an interruption request arrived.
// This test will hang if implementation is buggy.
BOOST_AUTO_TEST_CASE(interrupted_fiber_starting_async_op)
{
    asio::io_context ctx;
    std::vector<int> events;
    bool op_started = false;

    auto f = spawn(ctx, [&](fiber::this_fiber this_fiber) {
        {
            fiber::this_fiber::disable_interruption di(this_fiber);
            asio::steady_timer timer{this_fiber.get_executor().context()};
            timer.expires_after(std::chrono::seconds(1));
            events.push_back(0);
            // guarantee that this fiber will know about the interruption
            // request
            timer.async_wait(this_fiber);
        }
        events.push_back(1);
        this_fiber.interrupter = []() {
            // Make sure we don't depend on the interrupter to cancel any
            // operation. Essentially, we want to guarantee that operation won't
            // even start.
        };
        async_never_finishes(op_started, this_fiber);
        events.push_back(2);
    });

    f.interrupt();
    f.detach();

    ctx.run();

    std::vector<int> expected_events;
    expected_events.push_back(0);
    expected_events.push_back(1);
    BOOST_REQUIRE(!op_started);
    BOOST_TEST(events == expected_events, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(terminate_service)
{
    asio::io_context ctx;

    context_aborted(ctx);
    BOOST_REQUIRE(!context_aborted(ctx));

    ctx.run();

    context_aborted(ctx);
    BOOST_REQUIRE(!context_aborted(ctx));
}

BOOST_AUTO_TEST_CASE(terminate_service2)
{
    asio::io_context ctx;

    bool ran = false;

    boost::ignore_unused(spawn(ctx, [&](fiber::this_fiber /*this_fiber*/) {
        ran = true;
    }));

    ctx.run();

    BOOST_REQUIRE(!ran);
    BOOST_REQUIRE(context_aborted(ctx));
}

BOOST_AUTO_TEST_CASE(terminate_service3)
{
    asio::io_context ctx;

    auto f = spawn(ctx, [](fiber::this_fiber /*this_fiber*/) {});
    auto f2 = spawn(ctx, [](fiber::this_fiber /*this_fiber*/) {});

    BOOST_REQUIRE_THROW((f = std::move(f2)), std::logic_error);

    if (f.joinable())
        f.detach();

    if (f2.joinable())
        f2.detach();

    ctx.run();

    BOOST_REQUIRE(context_aborted(ctx));
}

BOOST_AUTO_TEST_CASE(fiber_local_storage)
{
    asio::io_context ctx;

    spawn(ctx, [](fiber::this_fiber this_fiber) {
        auto& x = this_fiber.local<int>(4);
        auto& y = this_fiber.local<char>('c');
        BOOST_REQUIRE_EQUAL(x, 4);
        BOOST_REQUIRE_EQUAL(y, 'c');
        this_fiber.yield();
        BOOST_REQUIRE_EQUAL(this_fiber.local<int>(), 4);
        BOOST_REQUIRE_EQUAL(this_fiber.local<char>(), 'c');
        x = 5;
        y = 'b';
        this_fiber.yield();
        BOOST_REQUIRE_EQUAL(this_fiber.local<int>(), 5);
        BOOST_REQUIRE_EQUAL(this_fiber.local<char>(), 'b');
    }).detach();

    spawn(ctx, [](fiber::this_fiber this_fiber) {
        auto& x = this_fiber.local<int>(14);
        auto& y = this_fiber.local<char>('f');
        BOOST_REQUIRE_EQUAL(x, 14);
        BOOST_REQUIRE_EQUAL(y, 'f');
        this_fiber.yield();
        BOOST_REQUIRE_EQUAL(this_fiber.local<int>(), 14);
        BOOST_REQUIRE_EQUAL(this_fiber.local<char>(), 'f');
        x = 15;
        y = 'g';
        this_fiber.yield();
        BOOST_REQUIRE_EQUAL(this_fiber.local<int>(), 15);
        BOOST_REQUIRE_EQUAL(this_fiber.local<char>(), 'g');
    }).detach();

    ctx.run();
}
