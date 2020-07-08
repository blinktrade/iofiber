/* Copyright (c) 2018, 2019 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE sync
#include <boost/test/unit_test.hpp>

#include <vector>

#include <trial/iofiber/fiber.hpp>
#include <boost/asio/steady_timer.hpp>

#include <trial/iofiber/mutex.hpp>
#include <trial/iofiber/condition_variable.hpp>

namespace asio = boost::asio;
using namespace trial;

BOOST_AUTO_TEST_CASE(mutex)
{
    asio::io_context ios;
    asio::io_context::strand strand{ios};

    std::vector<int> output;
    iofiber::mutex mutex{strand};

    output.push_back(-1);

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        mutex.lock(this_fiber);
        output.push_back(101);

        timer.expires_after(std::chrono::seconds(3));
        timer.async_wait(this_fiber);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {101, 102} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(102);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(201);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {201, 202} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(202);
    };

    auto w3 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(301);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {301, 302} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(302);

        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(311);
        mutex.unlock();
    };

    iofiber::fiber(ios, w1).detach();
    iofiber::fiber(ios, w2).detach();
    iofiber::fiber(ios, w3).detach();

    output.push_back(-2);

    ios.run();

    output.push_back(-3);

    std::vector<int> expected{-1, -2, 101, 102, 201, 202, 301, 302, 311, -3};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(mutex_same_strand)
{
    asio::io_context ios;
    asio::io_context::strand strand{ios};

    std::vector<int> output;
    iofiber::mutex mutex{strand};

    output.push_back(-1);

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        mutex.lock(this_fiber);
        output.push_back(101);

        timer.expires_after(std::chrono::seconds(3));
        timer.async_wait(this_fiber);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {101, 102} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(102);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(201);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {201, 202} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(202);
    };

    auto w3 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        timer.expires_after(std::chrono::seconds(2));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(301);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {301, 302} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(302);

        timer.expires_after(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(311);
        mutex.unlock();
    };

    iofiber::fiber(strand, w1).detach();
    iofiber::fiber(strand, w2).detach();
    iofiber::fiber(strand, w3).detach();

    output.push_back(-2);

    ios.run();

    output.push_back(-3);

    std::vector<int> expected{-1, -2, 101, 102, 201, 202, 301, 302, 311, -3};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(condvar)
{
    asio::io_context ioc;
    asio::io_context::strand strand{ioc};

    std::vector<int> output;
    iofiber::mutex mutex{strand};
    iofiber::condition_variable cond{strand};
    int state = 0;

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(101);
        this_fiber.yield();
        this_fiber.yield();
        this_fiber.yield();
        output.push_back(102);
        mutex.lock(this_fiber);
        output.push_back(103);
        state = 1;
        mutex.unlock();
        this_fiber.yield();
        this_fiber.yield();
        this_fiber.yield();
        output.push_back(104);
        cond.notify_one();
        output.push_back(105);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(201);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(202);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 1);
        output.push_back(203);
    };

    iofiber::fiber(ioc, w1).detach();
    iofiber::fiber(ioc, w2).detach();

    ioc.run();

    std::vector<int> expected{101, 201, 202, 102, 103, 104, 105, 203};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(condvar_samestrand)
{
    asio::io_context ioc;
    asio::io_context::strand strand{ioc};

    std::vector<int> output;
    iofiber::mutex mutex{strand};
    iofiber::condition_variable cond{strand};
    int state = 0;

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(101);
        this_fiber.yield();
        this_fiber.yield();
        this_fiber.yield();
        output.push_back(102);
        mutex.lock(this_fiber);
        output.push_back(103);
        state = 1;
        mutex.unlock();
        this_fiber.yield();
        this_fiber.yield();
        this_fiber.yield();
        output.push_back(104);
        cond.notify_one();
        output.push_back(105);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(201);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(202);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 1);
        output.push_back(203);
    };

    iofiber::fiber(strand, w1).detach();
    iofiber::fiber(strand, w2).detach();

    ioc.run();

    std::vector<int> expected{101, 201, 202, 102, 103, 104, 105, 203};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(condvar_multiple_waiters)
{
    asio::io_context ioc;
    asio::io_context::strand strand{ioc};

    std::vector<int> output;
    iofiber::mutex mutex{strand};
    iofiber::condition_variable cond{strand};
    int state = 0;
    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        this_fiber.yield();
        output.push_back(101);
        mutex.lock(this_fiber);
        output.push_back(102);
        state = 1;
        mutex.unlock();
        output.push_back(103);
        cond.notify_all();
        output.push_back(104);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(201);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(202);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 1);
        output.push_back(203);
        state = 2;
    };

    auto w3 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(301);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(302);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 2);
        output.push_back(303);
    };

    iofiber::fiber(ioc, w1).detach();
    iofiber::fiber(ioc, w2).detach();
    iofiber::fiber(ioc, w3).detach();

    ioc.run();

    std::vector<int> expected{201, 301, 101, 202, 302, 102, 103, 104, 203, 303};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(condvar_multiple_waiters_samestrand)
{
    asio::io_context ioc;
    asio::io_context::strand strand{ioc};

    std::vector<int> output;
    iofiber::mutex mutex{strand};
    iofiber::condition_variable cond{strand};
    int state = 0;
    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        this_fiber.yield();
        output.push_back(101);
        mutex.lock(this_fiber);
        output.push_back(102);
        state = 1;
        mutex.unlock();
        output.push_back(103);
        cond.notify_all();
        output.push_back(104);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(201);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(202);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 1);
        output.push_back(203);
        state = 2;
    };

    auto w3 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(301);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(302);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 2);
        output.push_back(303);
    };

    iofiber::fiber(strand, w1).detach();
    iofiber::fiber(strand, w2).detach();
    iofiber::fiber(strand, w3).detach();

    ioc.run();

    std::vector<int> expected{201, 202, 301, 302, 101, 102, 103, 104, 203, 303};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(condvar_can_lockless_notify_on_same_strand)
{
    asio::io_context ioc;
    asio::io_context::strand strand{ioc};

    std::vector<int> output;
    iofiber::mutex mutex{strand};
    iofiber::condition_variable cond{strand};
    int state = 0;

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(101);
        this_fiber.yield();
        output.push_back(102);
        state = 1;
        cond.notify_one();
        output.push_back(103);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        output.push_back(201);
        iofiber::unique_lock lk{mutex, this_fiber};
        output.push_back(202);
        BOOST_REQUIRE_EQUAL(state, 0);
        cond.wait(lk, this_fiber);
        BOOST_REQUIRE_EQUAL(state, 1);
        output.push_back(203);
    };

    iofiber::fiber(strand, w1).detach();
    iofiber::fiber(strand, w2).detach();

    ioc.run();

    std::vector<int> expected{101, 201, 202, 102, 103, 203};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(condvar_interrupt)
{
    asio::io_context ioc;
    asio::io_context::strand strand{ioc};

    bool output = false;
    iofiber::mutex mutex{strand};
    iofiber::condition_variable cond{strand};

    iofiber::fiber f(ioc, [&](iofiber::fiber::this_fiber this_fiber) {
        iofiber::unique_lock lk{mutex, this_fiber};
        try {
            cond.wait(lk, this_fiber);
        } catch(const iofiber::fiber_interrupted&) {
            output = true;
            throw;
        }
    });

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ioc};
        timer.expires_after(std::chrono::milliseconds(500));
        timer.async_wait(this_fiber);
        f.interrupt();
        f.join(this_fiber);
        BOOST_REQUIRE_EQUAL(f.interruption_caught(), true);
    };

    iofiber::fiber(ioc, w2).detach();

    ioc.run();

    BOOST_REQUIRE_EQUAL(output, true);
}
