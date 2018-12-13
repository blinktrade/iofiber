/* Copyright (c) 2018 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE sync
#include <boost/test/unit_test.hpp>

#include <vector>

#include <trial/iofiber/fiber.hpp>
#include <boost/asio/steady_timer.hpp>

#include <trial/iofiber/mutex.hpp>
#include <trial/iofiber/semaphore.hpp>
#include "barrier.hpp"

namespace asio = boost::asio;
using namespace trial;

BOOST_AUTO_TEST_CASE(mutex) {
    asio::io_context ios;
    asio::io_context::strand strand{ios};

    std::vector<int> output;
    iofiber::mutex mutex{strand};

    output.push_back(-1);

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        mutex.lock(this_fiber);
        output.push_back(101);

        timer.expires_from_now(std::chrono::seconds(3));
        timer.async_wait(this_fiber);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {101, 102} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(102);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        timer.expires_from_now(std::chrono::seconds(1));
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

        timer.expires_from_now(std::chrono::seconds(2));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(301);
        mutex.unlock();
        // Testing that fibers aren't interleaved and {301, 302} stick together.
        // IOW, we `post` pending tasks instead jumping to them.
        output.push_back(302);

        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        mutex.lock(this_fiber);
        output.push_back(311);
        mutex.unlock();
    };

    iofiber::spawn(strand, w1).detach();
    iofiber::spawn(strand, w2).detach();
    iofiber::spawn(strand, w3).detach();

    output.push_back(-2);

    ios.run();

    output.push_back(-3);

    // Actually, it could be this alternative version, but then we'd remove from
    // the beginning of the vector (an inefficient operation):
    //
    //std::vector<int> expected{-1, -2, 101, 102, 201, 202, 301, 302, 311, -3};
    //
    // You may argue that LIFO semantics would provoke starvation, but our usage
    // patterns (maximum number of running fibers per mutex specially) allow us
    // to rely on FIFO semantics.
    std::vector<int> expected{-1, -2, 101, 102, 301, 302, 201, 202, 311, -3};
    BOOST_REQUIRE(output == expected);
}

BOOST_AUTO_TEST_CASE(barrier) {
    boost::asio::io_context ios;

    std::vector<bool> values(3, false);
    iofiber::fiber_barrier barrier(ios, 3);

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};
        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(this_fiber);

        BOOST_REQUIRE(values[0] == false);
        BOOST_REQUIRE(values[1] == false);
        BOOST_REQUIRE(values[2] == false);

        values[0] = true;
        BOOST_REQUIRE(barrier.wait(this_fiber) == false);

        BOOST_REQUIRE(values[0] == true);
        BOOST_REQUIRE(values[1] == true);
        BOOST_REQUIRE(values[2] == true);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};
        timer.expires_from_now(std::chrono::seconds(2));
        timer.async_wait(this_fiber);

        BOOST_REQUIRE(values[0] == true);
        BOOST_REQUIRE(values[1] == false);
        BOOST_REQUIRE(values[2] == false);

        values[1] = true;
        BOOST_REQUIRE(barrier.wait(this_fiber) == false);

        BOOST_REQUIRE(values[0] == true);
        BOOST_REQUIRE(values[1] == true);
        BOOST_REQUIRE(values[2] == true);
    };

    auto w3 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};
        timer.expires_from_now(std::chrono::seconds(3));
        timer.async_wait(this_fiber);

        BOOST_REQUIRE(values[0] == true);
        BOOST_REQUIRE(values[1] == true);
        BOOST_REQUIRE(values[2] == false);

        values[2] = true;
        BOOST_REQUIRE(barrier.wait(this_fiber) == true);

        BOOST_REQUIRE(values[0] == true);
        BOOST_REQUIRE(values[1] == true);
        BOOST_REQUIRE(values[2] == true);
    };

    iofiber::spawn(ios, w1).detach();
    iofiber::spawn(ios, w2).detach();
    iofiber::spawn(ios, w3).detach();

    BOOST_REQUIRE(values[0] == false);
    BOOST_REQUIRE(values[1] == false);
    BOOST_REQUIRE(values[2] == false);

    ios.run();

    BOOST_REQUIRE(values[0] == true);
    BOOST_REQUIRE(values[1] == true);
    BOOST_REQUIRE(values[2] == true);
}

BOOST_AUTO_TEST_CASE(semaphore) {
    boost::asio::io_context ios;
    boost::asio::io_context::strand strand{ios};

    iofiber::semaphore sem{strand};
    iofiber::fiber_barrier barrier(ios, 2);

    strand.post([&sem]() {
        BOOST_REQUIRE(sem.get_value() == 0);

        sem.post();
        sem.post();
        sem.post();

        BOOST_REQUIRE_EQUAL(sem.get_value(), 3);

        sem.reset();

        BOOST_REQUIRE_EQUAL(sem.get_value(), 0);
    });
    ios.run();
    ios.restart();

    auto w1 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        // #1

        sem.post();

        barrier.wait(this_fiber);

        // #2

        barrier.wait(this_fiber);

        // #3

        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(this_fiber);
        sem.post();

        barrier.wait(this_fiber);

        // #4

        sem.post();
        barrier.wait(this_fiber);
    };

    auto w2 = [&](iofiber::fiber::this_fiber this_fiber) {
        boost::asio::steady_timer timer{ios};

        // #1
        timer.expires_from_now(std::chrono::seconds(1));
        timer.async_wait(this_fiber);

        BOOST_REQUIRE(sem.get_value() == 1);
        {
            auto start = std::chrono::steady_clock::now();
            boost::system::error_code ec;
            sem.wait_for(std::chrono::seconds(5), this_fiber[ec]);
            BOOST_REQUIRE(!ec);
            auto end = std::chrono::steady_clock::now();
            BOOST_REQUIRE(end - start < std::chrono::seconds(1));
        }

        barrier.wait(this_fiber);

        // #2

        BOOST_REQUIRE(sem.get_value() == 0);
        {
            auto start = std::chrono::steady_clock::now();
            boost::system::error_code ec;
            sem.wait_for(std::chrono::seconds(2), this_fiber[ec]);
            BOOST_REQUIRE_EQUAL(ec, boost::system::errc::timed_out);
            auto end = std::chrono::steady_clock::now();
            BOOST_REQUIRE(end - start > std::chrono::seconds(1));
        }

        barrier.wait(this_fiber);

        // #3

        BOOST_REQUIRE(sem.get_value() == 0);
        {
            auto start = std::chrono::steady_clock::now();
            boost::system::error_code ec;
            sem.wait_for(std::chrono::seconds(5), this_fiber[ec]);
            BOOST_REQUIRE(!ec);
            auto end = std::chrono::steady_clock::now();
            BOOST_REQUIRE(end - start < std::chrono::seconds(2));
        }

        barrier.wait(this_fiber);

        // #4

        {
            auto start = std::chrono::steady_clock::now();
            boost::system::error_code ec;
            sem.wait_for(std::chrono::seconds(5), this_fiber[ec]);
            BOOST_REQUIRE(!ec);
            auto end = std::chrono::steady_clock::now();
            BOOST_REQUIRE(end - start < std::chrono::seconds(1));
        }
        barrier.wait(this_fiber);
    };

    iofiber::spawn(strand, w1).detach();
    iofiber::spawn(strand, w2).detach();

    ios.run();
}
