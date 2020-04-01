/* Copyright (c) 2018, 2019, 2020 BlinkTrade, Inc.

   Distributed under the Boost Software License, Version 1.0. (See accompanying
   file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt) */

#ifndef TRIAL_IOFIBER_FIBER_H
#define TRIAL_IOFIBER_FIBER_H

#include <stdexcept>
#include <atomic>

#include <boost/asio/async_result.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

#include <boost/context/fiber.hpp>

#include <boost/intrusive/slist.hpp>

#include <boost/mp11/integer_sequence.hpp>

#include <boost/core/ignore_unused.hpp>

namespace trial {
namespace iofiber {

class fiber_interrupted
{};

template<class... Args>
struct interrupter_for;

namespace detail {

template<class>
struct is_strand: std::false_type {};

template<>
struct is_strand<boost::asio::io_context::strand>: std::true_type {};

template<class T>
struct is_strand<boost::asio::strand<T>>: std::true_type {};

using resume_token_type = std::uint32_t;

template<class Strand, class IntrTrait>
struct with_intr_token;

class local_data_base
    : public boost::intrusive::slist_base_hook<
#ifdef NDEBUG
        boost::intrusive::link_mode<boost::intrusive::normal_link>
#else
        boost::intrusive::link_mode<boost::intrusive::safe_link>
#endif
    >
{
public:
    local_data_base(void* type_index)
        : type_index(type_index)
    {}

    virtual void* get() = 0;

    virtual ~local_data_base() noexcept = default;

    void* type_index;
};

template<class T>
class local_data: public local_data_base
{
public:
    static_assert(noexcept(~T()), "T can't throw");

    template<class... Args>
    local_data(Args&&... args)
        : local_data_base(&id)
        , value(std::forward<Args>(args)...)
    {}

    void* get() override
    {
        return &value;
    }

    T& operator*()
    {
        return value;
    }

    static char id;

private:
    T value;
};

template<class T>
char local_data<T>::id;

template<class Strand>
class basic_this_fiber
{
public:
    struct impl
    {
        impl(Strand executor) : executor(std::move(executor)) {}

        ~impl() noexcept
        {
            local_data.clear_and_dispose([](local_data_base* p) { delete p; });
        }

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

        boost::intrusive::slist<
            local_data_base,
            boost::intrusive::constant_time_size<false>,
            boost::intrusive::linear<true>,
            boost::intrusive::cache_last<false>
        > local_data;
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
#ifndef NDEBUG
            // You already assume there will be no suspensions so it doesn't
            // make sense to worry about interruptions. Refactor your code.
            assert(this_fiber.suspension_disallowed == 0);
#endif // NDEBUG
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

#ifdef NDEBUG
    basic_this_fiber(const basic_this_fiber<Strand>&) = default;
#else
    basic_this_fiber(const basic_this_fiber<Strand>& o)
        : interrupter(o.interrupter)
        , pimpl_(o.pimpl_)
        , out_ec_(o.out_ec_)
    {
        assert(o.suspension_disallowed == 0);
    }
#endif // NDEBUG

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
#ifndef NDEBUG
        assert(suspension_disallowed == 0);
#endif // NDEBUG
        if (pimpl_->interruption_enabled && pimpl_->interrupted.load()) {
            throw fiber_interrupted();
        }

        {
            auto token = ++pimpl_->resume_token;
            auto pimpl = this->pimpl_;
            pimpl_->executor.defer([pimpl,token]() {
                if (token != pimpl->resume_token)
                    return;

                pimpl->coro = std::move(pimpl->coro).resume();
            }, std::allocator<void>{});
        }
        pimpl_->coro = std::move(pimpl_->coro).resume();
    }

    template<class... Args>
    with_intr_token<
        Strand,
        trial::iofiber::interrupter_for<typename std::decay<Args>::type...>
    >
    with_intr(Args&&... args);

    void forbid_suspend()
    {
#ifndef NDEBUG
        ++suspension_disallowed;
#endif // NDEBUG
    }

    void allow_suspend()
    {
#ifndef NDEBUG
        assert(suspension_disallowed > 0);
        --suspension_disallowed;
#endif // NDEBUG
    }

    // Having some “global” state to be used by the next call resembles OpenGL
    // state machine, but the actual inspiration came from a realization that
    // this style of API can be simpler to configure suspending calls after
    // reading Giacomo Tesio's awake syscall idea:
    // <http://jehanne.io/2018/11/15/simplicity-awakes.html>.
    std::function<void()>& interrupter;

    template<class T, class... Args>
    T& local(Args&&... args)
    {
        for (auto& e: pimpl_->local_data) {
            if (e.type_index == &local_data<T>::id)
                return *reinterpret_cast<T*>(e.get());
        }

        auto& e = *new local_data<T>(std::forward<Args>(args)...);
        pimpl_->local_data.push_front(e);
        return *e;
    }

    // Private {{{
    std::shared_ptr<impl> pimpl_;
    boost::system::error_code *out_ec_ = nullptr;
#ifndef NDEBUG
    int suspension_disallowed = 0;
#endif // NDEBUG
    // }}}
};

template<class Strand, class IntrTrait>
struct with_intr_token
{
    with_intr_token(
        std::shared_ptr<typename basic_this_fiber<Strand>::impl> pimpl,
        boost::system::error_code* out_ec
    )
        : pimpl(std::move(pimpl))
        , out_ec(out_ec)
    {}

    with_intr_token(const with_intr_token&) = default;
    with_intr_token(with_intr_token&&) = default;

    std::shared_ptr<typename basic_this_fiber<Strand>::impl> pimpl;
    boost::system::error_code* out_ec;
};

template<class Strand>
template<class... Args>
inline
with_intr_token<
    Strand, trial::iofiber::interrupter_for<typename std::decay<Args>::type...>
>
basic_this_fiber<Strand>::with_intr(Args&&... args)
{
#ifndef NDEBUG
    assert(suspension_disallowed == 0);
#endif // NDEBUG
    using IntrTrait = trial::iofiber::interrupter_for<
        typename std::decay<Args>::type...>;
    IntrTrait::assign(*this, std::forward<Args>(args)...);
    return with_intr_token<Strand, IntrTrait>{pimpl_, out_ec_};
}

template<class T = void>
class service_aborted: public boost::asio::execution_context::service
{
public:
    using key_type = service_aborted<T>;

    explicit service_aborted(boost::asio::execution_context& ctx)
        : boost::asio::execution_context::service(ctx)
    {}

    static boost::asio::execution_context::id id;

private:
    virtual void shutdown() noexcept override {}
};

template<class T>
boost::asio::execution_context::id service_aborted<T>::id;

} // namespace detail {

template<class ExecutionContext>
bool context_aborted(ExecutionContext& ctx)
{
    return boost::asio::has_service<detail::service_aborted<>>(ctx);
}

template<class Strand>
class [[nodiscard]] basic_fiber
{
public:
    static_assert(detail::is_strand<Strand>::value, "");

    using this_fiber = detail::basic_this_fiber<Strand>;
    using impl = typename this_fiber::impl;

    basic_fiber()
        : joinable_state(joinable_type::DETACHED)
    {}

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
            boost::asio::use_service<detail::service_aborted<>>(
                pimpl_->executor.context());
            pimpl_->executor.context().stop();
        }
    }

    basic_fiber& operator=(basic_fiber&& o)
    {
        if (joinable_state == joinable_type::JOINABLE) {
            boost::asio::use_service<detail::service_aborted<>>(
                pimpl_->executor.context());
            pimpl_->executor.context().stop();
            throw std::logic_error{"fiber handle leak"};
        }

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
#ifndef NDEBUG
        assert(this_fiber.suspension_disallowed == 0);
#endif // NDEBUG
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
#ifndef NDEBUG
        assert(this_fiber.suspension_disallowed == 0);
#endif // NDEBUG
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
            }, std::allocator<void>{});
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
            active_coro->executor.defer([active_coro,token]() {
                if (token != active_coro->resume_token)
                    return;

                active_coro->coro = std::move(active_coro->coro).resume();
            }, std::allocator<void>{});
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

struct detach
{
    template<class Fiber, class Yield>
    void operator()(Fiber& fib, Yield&)
    {
        if (fib.joinable())
            fib.detach();
    }
};

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
    scoped_fiber(typename basic_fiber<JoinerStrand>::this_fiber this_fiber)
        : yield(std::move(this_fiber))
    {}

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

    scoped_fiber& operator=(scoped_fiber&& o)
    {
        static_cast<CallableFiber&>(*this)(fib, yield);
        fib = std::move(o.fib);
        return *this;
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

template<class T, class Strand = boost::asio::io_context::strand>
class assert_exclusive_strand_ref
{
public:
    assert_exclusive_strand_ref(
        T& o,
        typename basic_fiber<Strand>::this_fiber& this_fiber
    )
        : obj(nullptr)
        , this_fiber(this_fiber)
    {
        reset(o);
    }

    ~assert_exclusive_strand_ref()
    {
        release();
    }

    // This wrapper is always tied to a finite lexical scope. Its purpose is
    // *NOT* to manage lifetimes.
    assert_exclusive_strand_ref(const assert_exclusive_strand_ref&) = delete;
    assert_exclusive_strand_ref&
    operator=(const assert_exclusive_strand_ref&) = delete;

    T& operator*() const
    {
        assert(obj);
        return *obj;
    }

    T* operator->() const
    {
        assert(obj);
        return obj;
    }

    void release()
    {
        if (obj)
            this_fiber.allow_suspend();
        obj = nullptr;
    }

    void reset(T& o)
    {
        release();
        this_fiber.forbid_suspend();
        obj = &o;
    }

private:
    T* obj;
    typename basic_fiber<Strand>::this_fiber& this_fiber;
};

template<class Strand>
class assert_exclusive_strand_ref<void, Strand>
{
public:
    assert_exclusive_strand_ref(
        typename basic_fiber<Strand>::this_fiber& this_fiber
    )
        : obj(false)
        , this_fiber(this_fiber)
    {
        reset();
    }

    ~assert_exclusive_strand_ref()
    {
        release();
    }

    assert_exclusive_strand_ref(const assert_exclusive_strand_ref&) = delete;
    assert_exclusive_strand_ref&
    operator=(const assert_exclusive_strand_ref&) = delete;

    void release()
    {
        if (obj)
            this_fiber.allow_suspend();
        obj = false;
    }

    void reset()
    {
        release();
        this_fiber.forbid_suspend();
        obj = true;
    }

private:
    bool obj;
    typename basic_fiber<Strand>::this_fiber& this_fiber;
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

template<class Executor>
class remap_post_to_defer: private Executor
{
public:
    remap_post_to_defer(const remap_post_to_defer&) = default;
    remap_post_to_defer(remap_post_to_defer&&) = default;

    explicit remap_post_to_defer(const Executor& ex)
        : Executor(ex)
    {}

    explicit remap_post_to_defer(Executor&& ex)
        : Executor(std::move(ex))
    {}

    bool operator==(const remap_post_to_defer& o) const noexcept
    {
        return static_cast<const Executor&>(*this) ==
            static_cast<const Executor&>(o);
    }

    bool operator!=(const remap_post_to_defer& o) const noexcept
    {
        return static_cast<const Executor&>(*this) !=
            static_cast<const Executor&>(o);
    }

    decltype(std::declval<Executor>().context())
    context() const noexcept
    {
        return Executor::context();
    }

    void on_work_started() const noexcept
    {
        Executor::on_work_started();
    }

    void on_work_finished() const noexcept
    {
        Executor::on_work_finished();
    }

    template<class F, class A>
    void dispatch(F&& f, const A& a) const
    {
        Executor::dispatch(std::forward<F>(f), a);
    }

    template<class F, class A>
    void post(F&& f, const A& a) const
    {
        Executor::defer(std::forward<F>(f), a);
    }

    template<class F, class A>
    void defer(F&& f, const A& a) const
    {
        Executor::defer(std::forward<F>(f), a);
    }
};

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

// bind + apply {{{

template<class IntrTrait, class Strand, class T, std::size_t... Idxs>
void apply_on_result_impl(basic_this_fiber<Strand>& this_fiber,
                          boost::system::error_code& ec, T& args,
                          boost::mp11::index_sequence<Idxs...>)
{
    IntrTrait::on_result(this_fiber, ec, std::get<Idxs>(args)...);
}

template<class IntrTrait, class Strand, class... Args>
void apply_on_result(basic_this_fiber<Strand> this_fiber,
                     boost::system::error_code& ec, std::tuple<Args...>& args)
{
    using Idxs = boost::mp11::make_index_sequence<sizeof...(Args)>;
    apply_on_result_impl<IntrTrait>(this_fiber, ec, args, Idxs{});
}

template<class IntrTrait, class Strand, class T>
void apply_on_result(basic_this_fiber<Strand> this_fiber,
                     boost::system::error_code& ec, T& val)
{
    IntrTrait::on_result(this_fiber, ec, val);
}

template<class IntrTrait, class Strand, class T, std::size_t... Idxs>
void apply_on_result_impl(basic_this_fiber<Strand>& this_fiber, T& args,
                          boost::mp11::index_sequence<Idxs...>)
{
    IntrTrait::on_result(this_fiber, std::get<Idxs>(args)...);
}

template<class IntrTrait, class Strand, class... Args>
void apply_on_result(basic_this_fiber<Strand> this_fiber,
                     std::tuple<Args...>& args)
{
    using Idxs = boost::mp11::make_index_sequence<sizeof...(Args)>;
    apply_on_result_impl<IntrTrait>(this_fiber, args, Idxs{});
}

template<class IntrTrait, class Strand, class T>
void apply_on_result(basic_this_fiber<Strand> this_fiber, T& val)
{
    IntrTrait::on_result(this_fiber, val);
}

// }}}

struct dummy_intr_trait
{
    template<class... Args>
    static void on_result(Args&&...)
    {}
};

template<class, class, class>
class fiber_async_result;

template<class Strand, class IntrTrait, class R, class... Args>
class fiber_async_result<
    Strand, IntrTrait, R(boost::system::error_code, Args...)>
{
public:
    static_assert(std::is_same<R, void>::value,
                  "completion handler return type must be void");

    using return_type = typename ReturnType<
        typename std::decay<Args>::type...>::type;

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

        using executor_type = remap_post_to_defer<Strand>;

        completion_handler_type(
            const typename trial::iofiber::basic_fiber<Strand>::this_fiber& tkn
        )
            : pimpl(tkn.pimpl_)
            , token(++tkn.pimpl_->resume_token)
            , out_ec(tkn.out_ec_)
            , executor(tkn.get_executor())
        {
#ifndef NDEBUG
            assert(tkn.suspension_disallowed == 0);
#endif // NDEBUG
            if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
                pimpl->interrupter = nullptr;
                throw trial::iofiber::fiber_interrupted();
            }
        }

        completion_handler_type(
            const trial::iofiber::detail::with_intr_token<Strand, IntrTrait>&
            token
        )
            : pimpl(token.pimpl)
            , token(++token.pimpl->resume_token)
            , out_ec(token.out_ec)
            , executor(token.pimpl->executor)
        {
            if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
                pimpl->interrupter = nullptr;
                throw trial::iofiber::fiber_interrupted();
            }
        }

        template<class... Tail>
        void operator()(const boost::system::error_code &ec, Tail&&... args)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            pimpl->interrupter = nullptr;
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

            pimpl->interrupter = nullptr;
            this->args->ec = ec;
            this->args->ret = std::forward<T>(arg);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        executor_type get_executor() const
        {
            return executor;
        }

        std::shared_ptr<typename basic_this_fiber<Strand>::impl> pimpl;
        resume_token_type token;
        boost::system::error_code* out_ec;
        executor_type executor;
        packed_args_type* args;
    };

    explicit fiber_async_result(completion_handler_type& handler)
        : pimpl(handler.pimpl)
        , out_ec(handler.out_ec)
    {
        handler.args = &args;
    }

    return_type get()
    {
        pimpl->coro = std::move(pimpl->coro).resume();
        apply_on_result<IntrTrait>(
            basic_this_fiber<Strand>{pimpl}, args.ec, args.ret);
        if (out_ec)
            *out_ec = args.ec;
        else if (args.ec)
            throw boost::system::system_error(args.ec);
        return GetImpl<return_type>::get(args.ret);
    }

private:
    std::shared_ptr<typename basic_this_fiber<Strand>::impl> pimpl;
    boost::system::error_code* out_ec;
    typename completion_handler_type::packed_args_type args;
};

template<class Strand, class IntrTrait, class R, class... Args>
class fiber_async_result<Strand, IntrTrait, R(Args...)>
{
public:
    static_assert(std::is_same<R, void>::value,
                  "completion handler return type must be void");

    using return_type = typename ReturnType<
        typename std::decay<Args>::type...>::type;

    struct completion_handler_type
    {
        using packed_args_type = typename std::conditional<
            std::is_same<return_type, void>::value,
            std::tuple<>,
            return_type
        >::type;

        using executor_type = remap_post_to_defer<Strand>;

        completion_handler_type(
            const typename trial::iofiber::basic_fiber<Strand>::this_fiber &tkn
        )
            : pimpl(tkn.pimpl_)
            , token(++tkn.pimpl_->resume_token)
            , executor(tkn.get_executor())
        {
#ifndef NDEBUG
            assert(tkn.suspension_disallowed == 0);
#endif // NDEBUG
            if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
                pimpl->interrupter = nullptr;
                throw trial::iofiber::fiber_interrupted();
            }
        }

        completion_handler_type(
            const trial::iofiber::detail::with_intr_token<Strand, IntrTrait>&
            token
        )
            : pimpl(token.pimpl)
            , token(++token.pimpl->resume_token)
            , executor(token.pimpl->executor)
        {
            if (pimpl->interruption_enabled && pimpl->interrupted.load()) {
                pimpl->interrupter = nullptr;
                throw trial::iofiber::fiber_interrupted();
            }
        }

        template<class... Args2>
        void operator()(Args2&&... args)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            pimpl->interrupter = nullptr;
            *this->args = std::forward_as_tuple(args...);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        template<class T>
        void operator()(T &&arg)
        {
            assert(this->args != NULL);
            if (token != pimpl->resume_token)
                return;

            pimpl->interrupter = nullptr;
            *this->args = std::forward<T>(arg);
            pimpl->coro = std::move(pimpl->coro).resume();
        }

        executor_type get_executor() const
        {
            return executor;
        }

        std::shared_ptr<typename basic_this_fiber<Strand>::impl> pimpl;
        resume_token_type token;
        executor_type executor;
        packed_args_type* args;
    };

    explicit fiber_async_result(completion_handler_type& handler)
        : pimpl(handler.pimpl)
    {
        handler.args = &args;
    }

    return_type get()
    {
        pimpl->coro = std::move(pimpl->coro).resume();
        apply_on_result<IntrTrait>(basic_this_fiber<Strand>{pimpl}, args);
        return GetImpl<return_type>::get(args);
    }

private:
    std::shared_ptr<typename basic_this_fiber<Strand>::impl> pimpl;
    typename completion_handler_type::packed_args_type args;
};

} // namespace detail {

} // namespace iofiber {
} // namespace trial {

namespace boost {
namespace asio {

#ifndef TRIAL_IOFIBER_DISABLE_DEFAULT_INTERRUPTER

template<class Strand, class T>
class async_result<trial::iofiber::detail::basic_this_fiber<Strand>, T>
    : public trial::iofiber::detail::fiber_async_result<
        Strand, trial::iofiber::detail::dummy_intr_trait, T>
{
public:
    explicit async_result(
        typename trial::iofiber::detail::fiber_async_result<
            Strand, trial::iofiber::detail::dummy_intr_trait, T
        >::completion_handler_type&
        handler
    )
        : trial::iofiber::detail::fiber_async_result<
            Strand, trial::iofiber::detail::dummy_intr_trait, T>(handler)
    {}
};

#endif // !defined(TRIAL_IOFIBER_DISABLE_DEFAULT_INTERRUPTER)

template<class Strand, class T, class U>
class async_result<trial::iofiber::detail::with_intr_token<Strand, T>, U>
    : public trial::iofiber::detail::fiber_async_result<Strand, T, U>
{
public:
    explicit async_result(
        typename trial::iofiber::detail::fiber_async_result<
            Strand, T, U>::completion_handler_type&
        handler
    )
        : trial::iofiber::detail::fiber_async_result<Strand, T, U>(handler)
    {}
};

} // namespace asio {
} // namespace boost {

#endif // TRIAL_IOFIBER_FIBER_H
