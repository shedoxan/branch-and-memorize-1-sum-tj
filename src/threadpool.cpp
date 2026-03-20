#include "threadpool.h"

ThreadPool::ThreadPool(std::size_t num_threads) {
	if (num_threads == 0) {
		num_threads = 1;
	}

	try {
		workers_.reserve(num_threads);
		for (std::size_t i = 0; i < num_threads; ++i) {
			workers_.emplace_back(&ThreadPool::worker_loop, this);
		}
	} catch (...) {
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			stopping_ = true;
		}
		condition_.notify_all();

		for (std::thread& worker : workers_) {
			if (worker.joinable()) {
				worker.join();
			}
		}
		throw;
	}
}

ThreadPool::~ThreadPool() {
	{
		std::lock_guard<std::mutex> lock(queue_mutex_);
		stopping_ = true;
	}
	condition_.notify_all();

	for (std::thread& worker : workers_) {
		if (worker.joinable()) {
			worker.join();
		}
	}
}

void ThreadPool::worker_loop() {
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);
			condition_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
			if (stopping_ && tasks_.empty()) {
				return;
			}

			job = std::move(tasks_.front());
			tasks_.pop();
		}

		job();
	}
}
