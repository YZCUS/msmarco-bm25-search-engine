// test_thread_pool: submit many jobs, ensure each job runs exactly once and
// returned futures resolve to the expected values.

#include <atomic>
#include <future>
#include <vector>

#include "test_helpers.hpp"
#include "thread_pool.hpp"

namespace {

void test_each_job_runs_exactly_once() {
    constexpr int kJobs = 10000;
    std::atomic<int> counter{0};
    {
        idx::concurrent::ThreadPool pool(4);
        std::vector<std::future<int>> futs;
        futs.reserve(kJobs);
        for (int i = 0; i < kJobs; ++i) {
            futs.push_back(pool.submit([i, &counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
                return i * 2;
            }));
        }
        for (int i = 0; i < kJobs; ++i) {
            IDX_CHECK_EQ(futs[i].get(), i * 2);
        }
    }
    IDX_CHECK_EQ(counter.load(), kJobs);
}

void test_zero_threads_is_promoted_to_one() {
    idx::concurrent::ThreadPool pool(0);
    IDX_CHECK(pool.size() >= 1);
    auto fut = pool.submit([] { return 42; });
    IDX_CHECK_EQ(fut.get(), 42);
}

void test_void_returning_jobs() {
    idx::concurrent::ThreadPool pool(2);
    std::atomic<int> done{0};
    auto f = pool.submit([&done] { done.fetch_add(1); });
    f.get();
    IDX_CHECK_EQ(done.load(), 1);
}

}  // namespace

int main() {
    test_each_job_runs_exactly_once();
    test_zero_threads_is_promoted_to_one();
    test_void_returning_jobs();
    return idx::testing::report("test_thread_pool");
}
