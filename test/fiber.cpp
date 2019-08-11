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
#include <trial/iofiber/fiber.hpp>

namespace asio = boost::asio;
using namespace trial::iofiber;

template<class CompletionToken, class... Args>
BOOST_ASIO_INITFN_RESULT_TYPE(
    CompletionToken, void(typename std::decay<Args>::type...))
async_identity(CompletionToken&& token, Args&&... args)
{
    asio::async_completion<
        CompletionToken, void(typename std::decay<Args>::type...)
    > init{std::forward<CompletionToken>(token)};
    auto ex = asio::get_associated_executor(init.completion_handler);
    ex.post(std::bind(std::move(init.completion_handler),
                      std::forward<Args>(args)...),
            std::allocator<void>{});
    return init.result.get();
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        auto this_fiber2 = this_fiber[ec];
        boost::system::error_code ec2;
        BOOST_REQUIRE_EQUAL(
            ec, make_error_code(boost::system::errc::interrupted));

        async_identity(this_fiber2, ec2);
        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE_EQUAL(async_identity(this_fiber2, ec2, 4), 4);
        BOOST_REQUIRE(!ec);
        BOOST_REQUIRE(async_identity(this_fiber2, ec2, 4, 43)
                      == std::make_tuple(4, 43));
        BOOST_REQUIRE(!ec);

        ec2 = make_error_code(boost::system::errc::interrupted);
        async_identity(this_fiber2, ec2);
        BOOST_REQUIRE_EQUAL(ec, ec2);
        async_identity(this_fiber2, ec2, 3);
        BOOST_REQUIRE_EQUAL(ec, ec2);
        async_identity(this_fiber2, ec2, 3, 534);
        BOOST_REQUIRE_EQUAL(ec, ec2);

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
        timer.expires_from_now(std::chrono::milliseconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(2));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
            timer.expires_from_now(std::chrono::seconds(1));
            timer.async_wait(this_fiber);
            events.push_back(0);
        } catch (const fiber_interrupted&) {
            timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
        this_fiber(timer, &asio::steady_timer::async_wait);
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
        timer.expires_from_now(std::chrono::seconds(1));
        this_fiber[ec](timer, &asio::steady_timer::async_wait);
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
        this_fiber(o, &dummy_io_object::async_foo, 42, 4.2);
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
        int x = this_fiber.call<int>(o, &dummy_io_object::async_bar);
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
        int x = this_fiber.call<int>(o, &dummy_io_object::async_baz, 33);
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
        timer.expires_from_now(std::chrono::milliseconds(1));

        BOOST_REQUIRE(!this_fiber.interrupter);
        this_fiber(timer, &asio::steady_timer::async_wait);
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
        timer.expires_from_now(std::chrono::seconds(2));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(2));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
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
            timer.expires_from_now(std::chrono::seconds(1));
            timer.async_wait(this_fiber);
            events.push_back(0);
        } catch (const fiber_interrupted&) {
            fiber::this_fiber::disable_interruption di(this_fiber);
            boost::ignore_unused(di);
            timer.expires_from_now(std::chrono::seconds(1));
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
        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        f_finished = true;
    });

    f.detach();
    f.interrupt();

    ctx.run();

    BOOST_REQUIRE(f_finished);
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
