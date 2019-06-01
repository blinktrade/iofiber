#include <boost/chrono.hpp>
#include <iostream>
#include <boost/thread/thread.hpp>
#include <vector>

boost::thread sleepsort(const std::vector<int>& v)
{
    // The grouping thread
    return boost::thread{ [v]() {
        std::vector<boost::thread> threads;
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
    sleepsort(v).join();
}
