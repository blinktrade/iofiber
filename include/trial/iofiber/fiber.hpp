/* Copyright (c) 2018 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_FIBER_H
#define TRIAL_IOFIBER_FIBER_H

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <boost/context/fiber.hpp>

#include <boost/core/ignore_unused.hpp>

#include <atomic>

namespace trial {
namespace iofiber {

class fiber_interrupted
{};

namespace detail {

template<class>
struct is_strand: std::false_type {};

template<>
struct is_strand<boost::asio::io_context::strand>: std::true_type {};

template<class T>
struct is_strand<boost::asio::strand<T>>: std::true_type {};

using resume_token_type = std::uint32_t;

template<class Strand>
class basic_this_fiber
{
public:
    struct impl
    {
        impl(Strand executor) : executor(std::move(executor)) {}

        // Thread-safe. {{{
        Strand executor;

        // This variable may only be set once and its value doesn't revert back
        // to false. Therefore this pattern is safe. If a synchronized action is
        // required, use the strand.
        std::atomic_bool interrupted{false};
        // }}}

        // Strand-safe. {{{
        boost::context::fiber coro;
        std::function<void()> interrupter;
        bool interruption_enabled = true;

        // Only useful when fiber finishes.
        bool interruption_caught = false;

        // Increment and store this token before you suspend the coroutine and
        // free its strand and check whether its value is same just before
        // resuming it. If values differ, do not resume the coroutine and
        // discard any would-deliver value.
        resume_token_type resume_token = 0;

        // Must NOT block/suspend the calling fiber/thread. IOW, the joiner
        // fiber must be scheduled as if `joiner_pimpl->executor.post()` has
        // been called. This property simplifies critical/synchronization
        // sections.
        std::function<void()> joiner_executor;
        // }}}
    };

    class restore_interruption;

    class disable_interruption
    {
    public:
        disable_interruption(const basic_this_fiber<Strand> &this_fiber)
            noexcept
            : pimpl(*this_fiber.pimpl_)
            , previous_state(pimpl.interruption_enabled)
        {
            pimpl.interruption_enabled = false;
        }

        ~disable_interruption() noexcept
        {
            pimpl.interruption_enabled = previous_state;
        }

        disable_interruption(const disable_interruption&) = delete;
        disable_interruption& operator=(const disable_interruption&) = delete;

    private:
        impl& pimpl;
        bool previous_state;

        friend class restore_interruption;
    };

    class restore_interruption
    {
    public:
        explicit restore_interruption(disable_interruption& disabler) noexcept
            : pimpl(disabler.pimpl)
        {
            pimpl.interruption_enabled = disabler.previous_state;
        }

        ~restore_interruption() noexcept
        {
            pimpl.interruption_enabled = false;
        }

        restore_interruption(const restore_interruption&) = delete;
        restore_interruption& operator=(const restore_interruption&) = delete;

    private:
        impl& pimpl;
    };

    basic_this_fiber(std::shared_ptr<impl> pimpl)
        : interrupter(pimpl->interrupter)
        , pimpl_(std::move(pimpl))
    {}

    basic_this_fiber(const basic_this_fiber<Strand>&) = default;

    basic_this_fiber<Strand> operator[](boost::system::error_code &ec)
    {
        basic_this_fiber<Strand> ret{*this};
        ret.out_ec_ = &ec;
        return std::move(ret);
    }

    Strand get_executor() const
    {
        return pimpl_->executor;
    }

    void yield()
    {
        if (pimpl_->interruption_enabled && pimpl_->interrupted.load()) {
            throw fiber_interrupted();
        }

        {
            auto token = ++pimpl_->resume_token;
            auto pimpl = this->pimpl_;
            pimpl_->executor.post([pimpl,token]() {
                if (token != pimpl->resume_token)
                    return;

                pimpl->coro = std::move(pimpl->coro).resume();
            }, std::allocator<void>{});
        }
        pimpl_->coro = std::move(pimpl_->coro).resume();
    }

    template<class R, class T, class... Args>
    typename std::enable_if<!std::is_same<R, void>::value, R>::type
    call(T& o, R(T::*async_fn)(Args..., const basic_this_fiber<Strand>&),
         Args&&... args)
    {
        interrupter = [&o]() { o.cancel(); };
        try {
            auto ret = (o.*async_fn)(std::forward<Args>(args)..., *this);
            if (out_ec_ && *out_ec_ == boost::asio::error::operation_aborted)
                throw fiber_interrupted();
            return std::move(ret);
        } catch (const boost::system::system_error& e) {
            if (e.code() == boost::asio::error::operation_aborted) {
                throw fiber_interrupted();
            } else {
                throw;
            }
        }
    }

    template<class R, class T, class... Args>
    void call(T& o,
              void(T::*async_fn)(Args..., const basic_this_fiber<Strand>&),
              Args&&... args)
    {
        interrupter = [&o]() { o.cancel(); };
        try {
            (o.*async_fn)(std::forward<Args>(args)..., *this);
            if (out_ec_ && *out_ec_ == boost::asio::error::operation_aborted)
                throw fiber_interrupted();
        } catch (const boost::system::system_error& e) {
            if (e.code() == boost::asio::error::operation_aborted) {
                throw fiber_interrupted();
            } else {
                throw;
            }
        }
    }

    template<class T, class... Args>
    void operator()(T& o,
                    void(T::*async_fn)(
                        Args..., const basic_this_fiber<Strand>&),
                    Args&&... args)
    {
        call<void, T>(o, async_fn, std::forward<Args>(args)...);
    }

    // Having some “global” state to be used by the next call resembles OpenGL
    // state machine, but the actual inspiration came from a realization that
    // this style of API can be simpler to configure suspending calls after
    // reading Giacomo Tesio's awake syscall idea:
    // <http://jehanne.io/2018/11/15/simplicity-awakes.html>.
    std::function<void()>& interrupter;

    // Private {{{
    std::shared_ptr<impl> pimpl_;
    boost::system::error_code *out_ec_ = nullptr;
    // }}}
};

} // namespace detail {

template<class Strand>
class basic_fiber
{
public:
    static_assert(detail::is_strand<Strand>::value, "");

    using this_fiber = detail::basic_this_fiber<Strand>;
    using impl = typename this_fiber::impl;

    basic_fiber(std::shared_ptr<impl> pimpl)
        : pimpl_(std::move(pimpl))
        , joinable_state(joinable_type::JOINABLE)
    {}

    basic_fiber(basic_fiber&& o)
        : pimpl_(std::move(o.pimpl_))
        , joinable_state(o.joinable_state)
    {
        o.joinable_state = joinable_type::DETACHED;
    }

    ~basic_fiber()
    {
        if (joinable_state == joinable_type::JOINABLE) {
            // TODO: inject a `ServiceTerminated` service into service to
            // signalize abnormal termination.
            pimpl_->executor.context().stop();
        }
    }

    basic_fiber& operator=(basic_fiber&& o)
    {
        pimpl_ = std::move(o.pimpl_);
        joinable_state = o.joinable_state;
        o.joinable_state = joinable_type::DETACHED;
        return *this;
    }

    basic_fiber(const basic_fiber&) = delete;
    basic_fiber& operator=(const basic_fiber&) = delete;

    bool joinable() const
    {
        return joinable_state == joinable_type::JOINABLE;
    }

    void join(this_fiber this_fiber)
    {
        assert(joinable());
        if (this_fiber.pimpl_->interruption_enabled
            && this_fiber.pimpl_->interrupted.load()) {
            throw fiber_interrupted();
        }

        if (pimpl_->executor != this_fiber.get_executor()) {
            inter_strand_join(this_fiber);
            joinable_state = joinable_type::JOINED;
            pimpl_->executor.on_work_finished();
            return;
        }

        same_strand_join(this_fiber.pimpl_);
        joinable_state = joinable_type::JOINED;
        pimpl_->executor.on_work_finished();
    }

    template<class Strand2>
    void join(typename basic_fiber<Strand2>::this_fiber this_fiber)
    {
        assert(joinable());
        if (this_fiber.pimpl_->interruption_enabled
            && this_fiber.pimpl_->interrupted.load()) {
            throw fiber_interrupted();
        }

        inter_strand_join(this_fiber);
        joinable_state = joinable_type::JOINED;
        pimpl_->executor.on_work_finished();
    }

    void detach()
    {
        assert(joinable());
        joinable_state = joinable_type::DETACHED;
        pimpl_->executor.on_work_finished();
    }

    void interrupt()
    {
        if (!joinable())
            return;

        bool interruption_in_progress = pimpl_->interrupted.exchange(true);
        if (!interruption_in_progress) {
            auto pimpl = pimpl_;
            pimpl_->executor.post([pimpl]() {
                if (!pimpl->coro || !pimpl->interruption_enabled)
                    return;

                if (pimpl->interrupter)
                    return pimpl->interrupter();

                // We must prevent double-resuming the coroutine. If we're here,
                // then there is a second scheduled resumption in progress. We
                // use a token approach where only the matched token gets to
                // resume the suspended coroutine and the competitors just
                // discard their result.
                ++pimpl->resume_token;

                pimpl->coro = std::move(pimpl->coro).resume_with(
                    [pimpl](boost::context::fiber&& sink)
                        -> boost::context::fiber {
                        pimpl->coro.swap(sink);
                        throw fiber_interrupted();
                        return {};
                    }
                );
            });
        }
    }

    // Equivalent to `PTHREAD_CANCELED`, but name inspired by Python's trio:
    // https://trio.readthedocs.io/en/latest/reference-core.html#trio.The%20cancel%20scope%20interface.cancelled_caught
    bool interruption_caught() const
    {
        assert(joinable_state == joinable_type::JOINED);
        return pimpl_->interruption_caught;
    }

    std::shared_ptr<impl> pimpl_;

private:
    template<class Executor>
    struct work_handle
    {
        work_handle(Executor& executor) : executor(executor)
        {
            executor.on_work_started();
        }

        ~work_handle()
        {
            executor.on_work_finished();
        }

    private:
        Executor& executor;
    };

    void same_strand_join(std::shared_ptr<impl> &active_coro)
    {
        if (!pimpl_->coro)
            return;

        auto token = ++active_coro->resume_token;
        pimpl_->joiner_executor = [active_coro,token]() {
            // This isn't strand-migration. This is just to let the joining
            // fiber die and perform any shutdown sequence it should.
            active_coro->executor.post([active_coro,token]() {
                if (token != active_coro->resume_token)
                    return;

                active_coro->coro = std::move(active_coro->coro).resume();
            });
        };
        active_coro->coro = std::move(active_coro->coro).resume();
    }

    template<class Strand2>
    void inter_strand_join(detail::basic_this_fiber<Strand2> this_fiber)
    {
        auto joiner_pimpl = this_fiber.pimpl_;
        auto token = ++joiner_pimpl->resume_token;
        pimpl_->executor.post([joiner_pimpl,token,this]() {
            auto awaker = [joiner_pimpl,token]() {
                joiner_pimpl->executor.post([joiner_pimpl,token]() {
                    if (token != joiner_pimpl->resume_token)
                        return;

                    joiner_pimpl->coro = std::move(joiner_pimpl->coro).resume();
                }, std::allocator<void>{});
            };

            // Same-strand-join
            if (!pimpl_->coro)
                awaker();
            else
                pimpl_->joiner_executor = std::move(awaker);
        }, std::allocator<void>{});
        work_handle<Strand2> _w{joiner_pimpl->executor};
        boost::ignore_unused(_w);
        joiner_pimpl->coro = std::move(joiner_pimpl->coro).resume();
    }

    enum class joinable_type: std::uint_fast8_t
    {
        JOINABLE,
        DETACHED,
        JOINED
    } joinable_state;
};

using fiber = basic_fiber<boost::asio::io_context::strand>;

struct join_if_joinable
{
    template<class Fiber, class Yield>
    void operator()(Fiber& fib, Yield& this_fiber)
    {
        try {
            if (fib.joinable())
                fib.join(this_fiber);
        } catch (const fiber_interrupted&) {
            fib.interrupt();
            typename Yield::disable_interruption di(this_fiber);
            boost::ignore_unused(di);
            fib.join(this_fiber);
        }
    }
};

struct interrupt_and_join_if_joinable
{
    template<class Fiber, class Yield>
    void operator()(Fiber& fib, Yield& this_fiber)
    {
        fib.interrupt();
        typename Yield::disable_interruption di(this_fiber);
        boost::ignore_unused(di);
        if (fib.joinable())
            fib.join(this_fiber);
    }
};

template<class CallableFiber = join_if_joinable,
         class JoinerStrand = boost::asio::io_context::strand,
         class JoineeStrand = boost::asio::io_context::strand>
class strict_scoped_fiber: private CallableFiber
{
public:
    strict_scoped_fiber(
        basic_fiber<JoineeStrand> &&fib,
        typename basic_fiber<JoinerStrand>::this_fiber this_fiber
    )
        : fib(std::move(fib))
        , yield(std::move(this_fiber))
    {}

    ~strict_scoped_fiber()
    {
        static_cast<CallableFiber&>(*this)(fib, yield);
    }

private:
    basic_fiber<JoineeStrand> fib;
    typename basic_fiber<JoinerStrand>::this_fiber yield;
};

template<class CallableFiber = join_if_joinable,
         class JoinerStrand = boost::asio::io_context::strand,
         class JoineeStrand = boost::asio::io_context::strand>
class scoped_fiber: private CallableFiber
{
public:
    scoped_fiber(
        basic_fiber<JoineeStrand> &&fib,
        typename basic_fiber<JoinerStrand>::this_fiber this_fiber
    )
        : fib(std::move(fib))
        , yield(std::move(this_fiber))
    {}

    scoped_fiber(scoped_fiber&& o)
        : fib(std::move(o.fib))
        , yield(std::move(o.yield))
    {}

    ~scoped_fiber()
    {
        static_cast<CallableFiber&>(*this)(fib, yield);
    }

    bool joinable() const
    {
        return fib.joinable();
    }

    template<class T>
    void join(const T& this_fiber)
    {
        static_assert(
            std::is_same<
                T, typename basic_fiber<JoinerStrand>::this_fiber
            >::value,
            ""
        );
        assert(this_fiber.pimpl_ == this->yield.pimpl_);
        boost::ignore_unused(this_fiber);
        join();
    }

    void join()
    {
        fib.join(yield);
    }

    void detach()
    {
        fib.detach();
    }

    void interrupt()
    {
        fib.interrupt();
    }

    bool interruption_caught() const
    {
        return fib.interruption_caught();
    }

private:
    basic_fiber<JoineeStrand> fib;
    typename basic_fiber<JoinerStrand>::this_fiber yield;
};

namespace detail {
template<class Strand, class F>
struct SpawnFunctor
{
    using impl = typename basic_fiber<Strand>::impl;

    template<class F2>
    SpawnFunctor(F2&& f, std::shared_ptr<impl> pimpl)
        : f(std::forward<F2>(f))
        , pimpl(std::move(pimpl))
    {}

    boost::context::fiber operator()(boost::context::fiber&& sink)
    {
        pimpl->coro.swap(sink);
        try {
            f(typename basic_fiber<Strand>::this_fiber{pimpl});
        } catch (const fiber_interrupted&) {
            pimpl->interruption_caught = true;
        }
        if (pimpl->joiner_executor) {
            pimpl->joiner_executor();
        }
        return std::move(pimpl->coro);
    }

    F f;
    std::shared_ptr<impl> pimpl;
};
} // namespace detail

template<class Strand, class F,
         class StackAllocator = boost::context::default_stack>
typename std::enable_if<
    detail::is_strand<Strand>::value, basic_fiber<Strand>>::type
spawn(Strand executor, F&& f, StackAllocator salloc = StackAllocator())
{
    auto pimpl = std::make_shared<typename basic_fiber<Strand>::impl>(executor);
    pimpl->coro = boost::context::fiber{
        std::allocator_arg,
        salloc,
        detail::SpawnFunctor<Strand, F>{std::forward<F>(f), pimpl}
    };
    executor.post([pimpl]() {
        pimpl->coro = std::move(pimpl->coro).resume();
    }, std::allocator<void>{});

    executor.on_work_started();
    return {pimpl};
}

template<class Strand, class F,
         class StackAllocator = boost::context::default_stack>
typename std::enable_if<
    detail::is_strand<Strand>::value, basic_fiber<Strand>>::type
spawn(detail::basic_this_fiber<Strand> this_fiber, F &&f,
      StackAllocator salloc = StackAllocator())
{
    return spawn(this_fiber.get_executor(), std::forward<F>(f),
                 std::move(salloc));
}

template<class F, class StackAllocator = boost::context::default_stack>
basic_fiber<boost::asio::io_context::strand>
spawn(boost::asio::io_context &ctx, F &&f,
      StackAllocator salloc = StackAllocator())
{
    return spawn(boost::asio::io_context::strand{ctx}, std::forward<F>(f),
                 std::move(salloc));
}

namespace detail {

template<class... Args>
struct ReturnType
{
    using type = std::tuple<Args...>;
};

template<class T>
struct ReturnType<T>
{
    using type = T;
};

template<>
struct ReturnType<>
{
    using type = void;
};

static_assert(std::is_same<typename ReturnType<>::type, void>::value, "");

static_assert(std::is_same<typename ReturnType<int>::type, int>::value, "");

static_assert(std::is_same<
    typename ReturnType<int, int>::type, std::tuple<int, int>
>::value, "");

template<class T>
struct GetImpl
{
    static T get(T &t)
    {
        return std::move(t);
    }
};

template<>
struct GetImpl<void>
{
    static void get(const std::tuple<>&) {}
};

} // namespace detail {

} // namespace iofiber {
} // namespace trial {

namespace boost {
namespace asio {

template<class Strand, class R, class... Args>
class async_result<
    trial::iofiber::detail::basic_this_fiber<Strand>,
    R(boost::system::error_code, Args...)>
{
public:
    static_assert(std::is_same<R, void>::value,
                  "completion handler return type must be void");

    using return_type = typename trial::iofiber::detail::ReturnType<
        typename decay<Args>::type...>::type;

    struct completion_handler_type
    {
        struct packed_args_type
        {
            boost::system::error_code ec;
            typename std::conditional<
                std::is_same<return_type, void>::value,
                std::tuple<>,
                return_type
            >::type ret;
        };

        using executor_type = Strand;

        completion_handler_type(
            const typename trial::iofiber::basic_fiber<Strand>::this_fiber& tkn
        )
            : pimpl(tkn.pimpl_)
            , token(++tkn.pimpl_->resume_token)
            , out_ec(tkn.out_ec_)
            , executor(tkn.get_executor())
        {
            if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
                pimpl->interrupter = std::function<void()>{};
                throw trial::iofiber::fiber_interrupted();
            }
        }

        template<class... Tail>
        void operator()(const boost::system::error_code &ec, Tail&&... args)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            this->args->ec = ec;
            this->args->ret = std::forward_as_tuple(args...);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        template<class T>
        void operator()(const boost::system::error_code &ec, T &&arg)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            this->args->ec = ec;
            this->args->ret = std::forward<T>(arg);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        executor_type get_executor() const
        {
            return executor;
        }

        std::shared_ptr<
            typename trial::iofiber::detail::basic_this_fiber<Strand>::impl
        > pimpl;
        trial::iofiber::detail::resume_token_type token;
        boost::system::error_code *out_ec;
        executor_type executor;
        packed_args_type *args;
    };

    async_result(completion_handler_type &handler)
        : pimpl(handler.pimpl)
        , out_ec(handler.out_ec)
    {
        handler.args = &args;
    }

    return_type get()
    {
        pimpl->coro = std::move(pimpl->coro).resume();
        if (out_ec)
            *out_ec = args.ec;
        else if (args.ec)
            throw boost::system::system_error(args.ec);
        return trial::iofiber::detail::GetImpl<return_type>::get(args.ret);
    }

private:
    std::shared_ptr<
        typename trial::iofiber::detail::basic_this_fiber<Strand>::impl> pimpl;
    boost::system::error_code *out_ec;
    typename completion_handler_type::packed_args_type args;
};

template<class Strand, class R, class... Args>
class async_result<
    trial::iofiber::detail::basic_this_fiber<Strand>, R(Args...)>
{
public:
    static_assert(std::is_same<R, void>::value,
                  "completion handler return type must be void");

    using return_type = typename trial::iofiber::detail::ReturnType<
        typename decay<Args>::type...>::type;

    struct completion_handler_type
    {
        using packed_args_type = typename std::conditional<
            std::is_same<return_type, void>::value,
            std::tuple<>,
            return_type
        >::type;

        using executor_type = Strand;

        completion_handler_type(
            const typename trial::iofiber::basic_fiber<Strand>::this_fiber &tkn
        )
            : pimpl(tkn.pimpl_)
            , token(++tkn.pimpl_->resume_token)
            , executor(tkn.get_executor())
        {
            if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
                pimpl->interrupter = std::function<void()>{};
                throw trial::iofiber::fiber_interrupted();
            }
        }

        template<class... Args2>
        void operator()(Args2&&... args)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            *this->args = std::forward_as_tuple(args...);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        template<class T>
        void operator()(T &&arg)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            *this->args = std::forward<T>(arg);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        executor_type get_executor() const
        {
            return executor;
        }

        std::shared_ptr<
            typename trial::iofiber::detail::basic_this_fiber<Strand>::impl
        > pimpl;
        trial::iofiber::detail::resume_token_type token;
        executor_type executor;
        packed_args_type *args;
    };

    async_result(completion_handler_type &handler)
        : pimpl(handler.pimpl)
    {
        handler.args = &args;
    }

    return_type get()
    {
        pimpl->coro = std::move(pimpl->coro).resume();
        return trial::iofiber::detail::GetImpl<return_type>::get(args);
    }

private:
    std::shared_ptr<
        typename trial::iofiber::detail::basic_this_fiber<Strand>::impl> pimpl;
    typename completion_handler_type::packed_args_type args;
};

} // namespace asio {
} // namespace boost {

#endif // TRIAL_IOFIBER_FIBER_H
