#include <boost/asio/steady_timer.hpp>
#include <boost/asio/signal_set.hpp>
#include <trial/iofiber/fiber.hpp>
#include <iostream>

namespace asio = boost::asio;
namespace fib = trial::iofiber;
using namespace std;

fib::fiber sleepsort(asio::io_context& ioctx, std::vector<int> v)
{
    // The grouping fiber to dispatch multiple interrupt requests
    return fib::spawn(
        ioctx,
        [v = std::move(v)](fib::fiber::this_fiber this_fiber) {
            std::vector<
                fib::scoped_fiber<fib::interrupt_and_join_if_joinable>
            > fibers;
            fibers.reserve(v.size());

            // The sleep-sort algo {{{
            for (auto i: v) {
                auto f = fib::spawn(
                    this_fiber,
                    [i](fib::fiber::this_fiber this_fiber) {
                        asio::steady_timer timer{
                            this_fiber.get_executor().context()};
                        timer.expires_from_now(chrono::seconds(i));
                        timer.async_wait(this_fiber);

                        cout << i << ' ' << std::flush;
                    }
                );
                fibers.push_back({ std::move(f), this_fiber });
            }
            // }}}

            for (auto& f: fibers) {
                f.join(this_fiber);
            }
        }
    );
}

int main()
{
    asio::io_context ioctx;

    std::vector<int> v{8, 42, 38, 111, 2, 39, 1};

    auto sleeper = sleepsort(ioctx, v);
    asio::io_context::strand sleeper_fib_obj_strand{ioctx};

    auto sigwaiter = fib::spawn(
        sleeper_fib_obj_strand,
        [&](fib::fiber::this_fiber this_fiber) {
            asio::signal_set sigusr1(ioctx, SIGUSR1);
            sigusr1.async_wait(this_fiber);
            sleeper.interrupt();
        }
    );

    fib::spawn(
        sleeper_fib_obj_strand,
        [&](fib::fiber::this_fiber this_fiber) {
            sleeper.join(this_fiber);
            sigwaiter.interrupt();
            sigwaiter.detach();
        }
    ).detach();

    ioctx.run();
    cout << endl;
}
