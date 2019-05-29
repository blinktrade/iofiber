// The code could be easier and just call std::exit(), but the purpose here is
// to teach how to interrupt a task while the remaining tasks of the program
// continue to run and std::exit() wouldn't work in such case.

#include <boost/chrono.hpp>
#include <iostream>
#include <boost/thread/scoped_thread.hpp>
#include <vector>
#include <signal.h>
#include <cstdlib>

static int pfd[2];

void handler(int sig)
{
    if (write(pfd[1], ".", 1) == -1)
        std::exit(-1);
}

// Given pthread_cancel() behaviour under C++ mode[1], I assume the default
// expectation for thread programmers is that they can't call join() or any
// other possibly long-running function after the thread is interrupted. So,
// `interrupt_and_detach` is used instead `interrupt_and_join` here.
//
// [1] https://udrepper.livejournal.com/21541.html
struct interrupt_and_detach_if_joinable
{
    template <class Thread>
    void operator()(Thread& t)
    {
        t.interrupt();
        if (t.joinable()) {
            t.detach();
        }
    }
};

boost::thread sleepsort(const std::vector<int>& v)
{
    // The grouping thread to dispatch multiple interrupt requests
    return boost::thread{ [v]() {
        std::vector<
            boost::scoped_thread<interrupt_and_detach_if_joinable>
        > threads;
        threads.reserve(v.size());

        // The sleep-sort algo {{{
        for (auto e: v) {
            threads.emplace_back(
                [e]() {
                    boost::this_thread::sleep_for(boost::chrono::seconds(e));
                    std::cout << e << std::endl;
                }
            );
        }
        // }}}

        for (auto& t : threads) {
            t.join();
        }
    } };
}

int main(int argc, char* argv[]) {
    std::vector<int> v{8, 42, 38, 111, 2, 39, 1};

    if (pipe(pfd) == -1)
        std::exit(-1);

    if (signal(SIGUSR1, handler) == SIG_ERR)
        std::exit(-1);

    auto sleeper = sleepsort(v);

    auto sigwaiter = boost::thread([&]() {
        char ch;
        if (read(pfd[0], &ch, 1) == -1)
            std::exit(-1);
        sleeper.interrupt();
    });

    sleeper.join();

    // This piece is optional if you start `sigwaiter` thread detached, but it
    // is included anyway to show how to synchronize tasks so you can properly
    // manage resources in more complex code.
    //
    // For instance: were sigwaiter thread to keep running after we deallocated
    // the `sleeper` object, we'd have a bug. {{{
    if (raise(SIGUSR1) != 0)
        std::exit(-1);
    sigwaiter.join();
    // }}}
}
