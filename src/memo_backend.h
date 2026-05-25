#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "memo.h"
#include "memo_std_unordered.h"
#include "solver.h"

/// Фасад над реализациями memoization M[(S,t)].
/// Solver работает с этим классом, а конкретный backend выбирается через dfs_config.
/// Это позволяет сравнивать быстрый custom backend и понятный reference backend
/// без изменения рекурсии Branch-and-Memorize.
class memo_backend_table {
public:
	explicit memo_backend_table(memo_backend_kind backend = memo_backend_kind::custom,
		std::size_t cap = 0)
		: backend_(backend) {
		switch (backend_) {
		case memo_backend_kind::custom:
			impl_.emplace<memo_table>(cap);
			break;
		case memo_backend_kind::std_unordered:
			impl_.emplace<memo_std_unordered_table>(cap);
			break;
		}
	}

	memo_backend_kind backend() const {
		return backend_;
	}

	void clear() {
		std::visit([](auto& table) { table.clear(); }, impl_);
	}

	void set_capacity(std::size_t capacity, bool count_stats = true) {
		std::visit([&](auto& table) { table.set_capacity(capacity, count_stats); }, impl_);
	}

	void set_memory_budget_bytes(std::size_t budget_bytes, bool strict_cap) {
		std::visit([&](auto& table) { table.set_memory_budget_bytes(budget_bytes, strict_cap); }, impl_);
	}

	void set_process_memory_gate(bool enabled) {
		std::visit([&](auto& table) { table.set_process_memory_gate(enabled); }, impl_);
	}

	void set_profiling_timers_enabled(bool enabled) {
		std::visit([&](auto& table) { table.set_profiling_timers_enabled(enabled); }, impl_);
	}

	std::size_t size() const {
		return std::visit([](const auto& table) { return table.size(); }, impl_);
	}

	std::size_t capacity() const {
		return std::visit([](const auto& table) { return table.capacity(); }, impl_);
	}

	/// Универсальный lookup: возвращает exact, если он есть, иначе LB.
	/// Используется текущим dfs_solver в hot path.
	memo_lookup_result lookup(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats = true) {
		return std::visit([&](auto& table) {
			return table.lookup(bits, time, subset_hash, subset_fingerprint, count_stats);
			}, impl_);
	}

	/// Exact-safe lookup: reconstruction должна полагаться только на exact-записи.
	memo_lookup_result query_exact(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats = true) {
		return std::visit([&](auto& table) {
			return query_exact_impl(table, bits, time, subset_hash, subset_fingerprint, count_stats);
			}, impl_);
	}

	/// Lookup нижней оценки LB(S,t). Exact-запись тоже является корректной LB.
	memo_lookup_result query_lb(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats = true) {
		return std::visit([&](auto& table) {
			return query_lb_impl(table, bits, time, subset_hash, subset_fingerprint, count_stats);
			}, impl_);
	}

	void store_exact(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long exact, int best_job, bool count_stats = true) {
		std::visit([&](auto& table) {
			table.store_exact(bits, time, subset_hash, subset_fingerprint, exact, best_job, count_stats);
			}, impl_);
	}

	void store_lower_bound(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long lb, bool count_stats = true) {
		std::visit([&](auto& table) {
			table.store_lower_bound(bits, time, subset_hash, subset_fingerprint, lb, count_stats);
			}, impl_);
	}

	bool store_reconstruction_trace(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		const memo_reconstruction_trace& trace, bool count_stats = true) {
		return std::visit([&](auto& table) {
			return table.store_reconstruction_trace(bits, time, subset_hash, subset_fingerprint, trace, count_stats);
			}, impl_);
	}

	void store_lb(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long lb, bool count_stats = true) {
		store_lower_bound(bits, time, subset_hash, subset_fingerprint, lb, count_stats);
	}

	void clean_if_needed(bool count_stats = true) {
		std::visit([&](auto& table) { clean_if_needed_impl(table, count_stats); }, impl_);
	}

	memo_table_stats stats() const {
		return std::visit([](const auto& table) { return table.stats(); }, impl_);
	}

	memo_memory_accounting memory_accounting() const {
		return std::visit([](const auto& table) { return table.memory_accounting(); }, impl_);
	}

	memo_diagnostics diagnostics() const {
		return std::visit([](const auto& table) { return table.diagnostics(); }, impl_);
	}

	std::size_t memory_used_bytes() const {
		return std::visit([](const auto& table) { return memory_used_bytes_impl(table); }, impl_);
	}

private:
	using backend_variant = std::variant<memo_table, memo_std_unordered_table>;

	static memo_lookup_result query_exact_impl(memo_std_unordered_table& table,
		const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats) {
		return table.query_exact(bits, time, subset_hash, subset_fingerprint, count_stats);
	}

	static memo_lookup_result query_exact_impl(memo_table& table,
		const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats) {
		memo_lookup_result result = table.lookup(bits, time, subset_hash, subset_fingerprint, count_stats);
		if (!result.has_exact) {
			return {};
		}
		return result;
	}

	static memo_lookup_result query_lb_impl(memo_std_unordered_table& table,
		const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats) {
		return table.query_lb(bits, time, subset_hash, subset_fingerprint, count_stats);
	}

	static memo_lookup_result query_lb_impl(memo_table& table,
		const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats) {
		return table.lookup(bits, time, subset_hash, subset_fingerprint, count_stats);
	}

	static void clean_if_needed_impl(memo_std_unordered_table& table, bool count_stats) {
		table.clean_if_needed(count_stats);
	}

	static void clean_if_needed_impl(memo_table&, bool) {
		// У custom backend очистка запускается внутри store/set_capacity/set_memory_budget_bytes.
	}

	static std::size_t memory_used_bytes_impl(const memo_std_unordered_table& table) {
		return table.memory_used_bytes();
	}

	static std::size_t memory_used_bytes_impl(const memo_table& table) {
		return table.memory_accounting().used_bytes;
	}

	memo_backend_kind backend_ = memo_backend_kind::custom;
	backend_variant impl_{ memo_table{} };
};
