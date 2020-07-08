#include <boost/asio/steady_timer.hpp>
#include <trial/iofiber/fiber.hpp>
#include <iostream>

namespace asio = boost::asio;
namespace fib = trial::iofiber;
using namespace std;

void sleepsort(asio::io_context& ioctx, std::vector<int> v)
{
    for (auto i: v) {
        fib::fiber(
            ioctx,
            [i](fib::fiber::this_fiber this_fiber) {
                asio::steady_timer timer{this_fiber.get_executor().context()};
                timer.expires_after(chrono::seconds(i));
                timer.async_wait(this_fiber);

                cout << i << ' ' << std::flush;
            }
        ).detach();
    }
}

int main()
{
    asio::io_context ioctx;

    std::vector<int> v{8, 42, 38, 111, 2, 39, 1};

    sleepsort(ioctx, v);

    ioctx.run();
    cout << endl;
}
