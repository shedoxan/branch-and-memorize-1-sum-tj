#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool {
public:
	explicit ThreadPool(std::size_t num_threads);
	~ThreadPool();

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
	ThreadPool(ThreadPool&&) = delete;
	ThreadPool& operator=(ThreadPool&&) = delete;

	template <typename F, typename... Args>
	auto enqueue(F&& f, Args&&... args)
		-> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>> {
		using return_t = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

		auto task = std::make_shared<std::packaged_task<return_t()>>(
			[fn = std::forward<F>(f), tuple = std::make_tuple(std::forward<Args>(args)...)]() mutable {
				return std::apply(std::move(fn), std::move(tuple));
			});

		std::future<return_t> result = task->get_future();
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			if (stopping_) {
				throw std::runtime_error("ThreadPool is stopping, cannot enqueue");
			}
			tasks_.emplace([task]() { (*task)(); });
		}
		condition_.notify_one();
		return result;
	}

private:
	void worker_loop();

	std::vector<std::thread> workers_;
	std::queue<std::function<void()>> tasks_;
	std::mutex queue_mutex_;
	std::condition_variable condition_;
	bool stopping_ = false;
};
