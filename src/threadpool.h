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

/// Служебный пул потоков
/// Запускает независимые экземпляры задачи параллельно.
/// Не участвует в рекурсии Branch-and-Memorize и не меняет exactness solver-а.
class ThreadPool {
public:
	/// Создаёт num_threads рабочих потоков; значение 0 трактуется как 1.
	explicit ThreadPool(std::size_t num_threads);

	/// Дожидается завершения уже взятых задач и останавливает рабочие потоки.
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
	/// Основной цикл рабочего потока: ждать задачу, забрать её из очереди, выполнить.
	void worker_loop();

	/// Рабочие потоки.
	std::vector<std::thread> workers_;
	/// Очередь независимых задач.
	std::queue<std::function<void()>> tasks_;
	/// Защищает очередь задач и флаг остановки.
	std::mutex queue_mutex_;
	/// Будит worker-ы при появлении задач или остановке.
	std::condition_variable condition_;
	/// Флаг завершения: новые задачи больше не принимаются.
	bool stopping_ = false;
};
