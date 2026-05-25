#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "memo.h"

/// Учебный reference backend для memoization.
/// Он хранит полный owned key = (S,t) в std::unordered_map и всегда сверяет
/// t и все uint64_t-блоки S через equality. Это не performance default.
struct std_unordered_memo_key {
	/// Полный bitset множества оставшихся работ S.
	std::vector<std::uint64_t> blocks;
	/// t — момент старта подзадачи.
	schedule_time_t start_time = 0;
};

/// Лёгкое представление ключа для lookup без копирования S.
/// Используется только на время поиска: blocks ссылается на runtime bitset.
struct std_unordered_memo_key_view {
	/// Текущий bitset S, не владеет памятью.
	const std::vector<std::uint64_t>& blocks;
	/// t — момент старта подзадачи.
	schedule_time_t start_time = 0;
};

/// Hash для owned key и view key.
/// is_transparent позволяет искать по std_unordered_memo_key_view без создания
/// временного owned std_unordered_memo_key.
struct std_unordered_memo_key_hash {
	using is_transparent = void;

	/// Hash для ключа, который уже хранится в unordered_map.
	std::size_t operator()(const std_unordered_memo_key& key) const noexcept {
		return hash_blocks_and_time(key.blocks, key.start_time);
	}

	/// Hash для lookup-view без копирования S.
	std::size_t operator()(std_unordered_memo_key_view key) const noexcept {
		return hash_blocks_and_time(key.blocks, key.start_time);
	}

private:
	/// SplitMix64-подобное перемешивание для устойчивого распределения buckets.
	static std::uint64_t mix64(std::uint64_t x) noexcept {
		x ^= (x >> 30);
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= (x >> 27);
		x *= 0x94D049BB133111EBULL;
		x ^= (x >> 31);
		return x;
	}

	/// Hash полного ключа (S,t): учитывает t и все uint64_t-блоки bitset S.
	static std::size_t hash_blocks_and_time(const std::vector<std::uint64_t>& blocks,
		schedule_time_t start_time) noexcept {
		std::uint64_t h = mix64(static_cast<std::uint64_t>(start_time) + 0x9E3779B97F4A7C15ULL);
		for (std::uint64_t block : blocks) {
			h ^= mix64(block + 0xD1B54A32D192ED03ULL + (h << 6) + (h >> 2));
		}
		return static_cast<std::size_t>(mix64(h));
	}
};

/// Equality для owned key и view key.
/// Exact-safe режим требует полного сравнения t и всех blocks S.
struct std_unordered_memo_key_equal {
	using is_transparent = void;

	/// Сравнение двух ключей, уже лежащих в unordered_map.
	bool operator()(const std_unordered_memo_key& lhs,
		const std_unordered_memo_key& rhs) const {
		return lhs.start_time == rhs.start_time && lhs.blocks == rhs.blocks;
	}

	/// Сравнение stored key с lookup-view.
	bool operator()(const std_unordered_memo_key& lhs,
		std_unordered_memo_key_view rhs) const {
		return lhs.start_time == rhs.start_time && lhs.blocks == rhs.blocks;
	}

	/// Сравнение lookup-view со stored key.
	bool operator()(std_unordered_memo_key_view lhs,
		const std_unordered_memo_key& rhs) const {
		return lhs.start_time == rhs.start_time && lhs.blocks == rhs.blocks;
	}
};

/// Значение, сохранённое для состояния (S,t).
/// В reference backend exact и LB хранятся отдельными полями, чтобы контракт был
/// максимально читаемым: exact нужен для решения и reconstruction, LB — только для pruning.
struct std_unordered_memo_entry {
	/// Точное значение OPT(S,t), используется для закрытия подзадачи и reconstruction.
	long long exact_value = 0;
	/// Нижняя оценка LB(S,t), используется только для pruning.
	long long lower_bound_value = 0;
	/// Первая работа оптимального продолжения. Имеет смысл только при has_exact.
	int best_job = -1;
	memo_reconstruction_trace reconstruction_trace{};
	/// Простейший счётчик обращений для reference eviction.
	std::int16_t use_count = 0;
	/// Есть ли точное значение OPT(S,t).
	bool has_exact = false;
	/// Есть ли нижняя оценка LB(S,t).
	bool has_lb = false;
	/// Оценка памяти этой записи для memory budget.
	std::size_t estimated_bytes = 0;
};

/// Reference backend на std::unordered_map.
/// Он проще custom memo_table: ключ хранится как vector<uint64_t> + t прямо в map.
/// Цена этой понятности — больше памяти, больше allocation и более грубая eviction.
class memo_std_unordered_table {
public:
	/// cap == 0 означает отсутствие ограничения на число записей.
	explicit memo_std_unordered_table(std::size_t cap = 0)
		: capacity_(cap) {}

	/// Очищает таблицу, но сохраняет настройки memory budget.
	void clear() {
		const std::size_t budget_bytes = memory_.budget_bytes;
		const bool strict_cap = memory_.strict_cap;
		const bool process_memory_gate = memory_.process_memory_gate;

		table_.clear();
		stats_ = {};
		diagnostics_ = {};
		memory_ = {};
		memory_.budget_bytes = budget_bytes;
		memory_.strict_cap = strict_cap;
		memory_.process_memory_gate = process_memory_gate;
	}

	/// Меняет ограничение на число записей и сразу чистит таблицу, если лимит нарушен.
	void set_capacity(std::size_t capacity, bool count_stats = true) {
		capacity_ = capacity;
		if (capacity_ > 0) {
			table_.reserve(capacity_);
		}
		clean_if_needed(count_stats);
		stats_.final_size = table_.size();
	}

	/// Задаёт memory budget и запускает cleanup, если текущая оценка памяти выше лимита.
	void set_memory_budget_bytes(std::size_t budget_bytes, bool strict_cap) {
		memory_.budget_bytes = budget_bytes;
		memory_.strict_cap = strict_cap;
		clean_if_needed(false);
		stats_.final_size = table_.size();
	}

	void set_process_memory_gate(bool enabled) {
		// Reference backend учитывает собственную оценку памяти. Process gate оставлен
		// в accounting для одинакового config API с custom backend.
		memory_.process_memory_gate = enabled;
	}

	/// Reference backend не измеряет отдельное время cleanup; метод нужен для общего API.
	void set_profiling_timers_enabled(bool) {}

	/// Текущее число записей в unordered_map.
	std::size_t size() const {
		return table_.size();
	}

	/// Лимит на число записей; 0 означает без лимита.
	std::size_t capacity() const {
		return capacity_;
	}

	/// Exact lookup: возвращает hit только если для (S,t) есть OPT(S,t).
	/// LB-only entry здесь считается miss, потому что reconstruction не может использовать LB.
	memo_lookup_result query_exact(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t, std::uint64_t, bool count_stats = true) {
		auto it = find_entry(bits, time);
		if (it == table_.end() || !it->second.has_exact) {
			if (count_stats) {
				++stats_.misses;
			}
			return {};
		}
		touch_entry(it->second);
		if (count_stats) {
			++stats_.hits;
			++diagnostics_.duplicate_subproblem_hits;
		}
		memo_lookup_result result{};
		result.found = true;
		result.has_exact = true;
		result.exact = it->second.exact_value;
		result.lower_bound = it->second.exact_value;
		result.best_job = it->second.best_job;
		result.reconstruction_trace = it->second.reconstruction_trace;
		return result;
	}

	/// LB lookup: exact entry тоже является корректной нижней оценкой.
	memo_lookup_result query_lb(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t, std::uint64_t, bool count_stats = true) {
		auto it = find_entry(bits, time);
		if (it == table_.end() || (!it->second.has_exact && !it->second.has_lb)) {
			if (count_stats) {
				++stats_.misses;
			}
			return {};
		}
		touch_entry(it->second);
		if (count_stats) {
			++stats_.hits;
			++diagnostics_.duplicate_subproblem_hits;
		}
		memo_lookup_result result{};
		result.found = true;
		result.has_exact = it->second.has_exact;
		result.exact = it->second.has_exact ? it->second.exact_value : 0;
		result.lower_bound = it->second.has_exact ? it->second.exact_value : it->second.lower_bound_value;
		result.best_job = it->second.has_exact ? it->second.best_job : -1;
		if (it->second.has_exact) {
			result.reconstruction_trace = it->second.reconstruction_trace;
		}
		return result;
	}

	/// Универсальный lookup для solve_state: возвращает exact, если он есть,
	/// иначе возвращает LB, если она сохранена.
	memo_lookup_result lookup(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats = true) {
		(void)subset_hash;
		(void)subset_fingerprint;
		auto it = find_entry(bits, time);
		if (it == table_.end()) {
			if (count_stats) {
				++stats_.misses;
			}
			return {};
		}
		touch_entry(it->second);
		if (count_stats) {
			++stats_.hits;
			++diagnostics_.duplicate_subproblem_hits;
		}
		memo_lookup_result result{};
		result.found = true;
		result.has_exact = it->second.has_exact;
		result.exact = it->second.has_exact ? it->second.exact_value : 0;
		result.lower_bound = it->second.has_exact ? it->second.exact_value : it->second.lower_bound_value;
		result.best_job = it->second.has_exact ? it->second.best_job : -1;
		if (it->second.has_exact) {
			result.reconstruction_trace = it->second.reconstruction_trace;
		}
		return result;
	}

	/// Сохраняет точное значение OPT(S,t) и best_job.
	/// Если раньше была только LB-запись, она повышается до exact-записи.
	void store_exact(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long exact, int best_job, bool count_stats = true) {
		(void)subset_hash;
		(void)subset_fingerprint;
		auto it = find_entry(bits, time);
		if (it != table_.end()) {
			std_unordered_memo_entry& entry = it->second;
			const bool changed =
				!entry.has_exact || entry.exact_value != exact || entry.best_job != best_job;
			entry.has_exact = true;
			entry.exact_value = exact;
			entry.best_job = best_job;
			touch_entry(entry);
			if (count_stats && changed) {
				++stats_.updates;
				++stats_.exact_stores;
			}
			return;
		}

		const std::size_t estimated_bytes = estimate_entry_bytes(bits);
		if (!ensure_room_for_insert(estimated_bytes, count_stats)) {
			if (count_stats) {
				++stats_.rejected_no_room;
			}
			return;
		}

		std_unordered_memo_key key{ bits, time };
		std_unordered_memo_entry entry{};
		entry.has_exact = true;
		entry.exact_value = exact;
		entry.best_job = best_job;
		entry.use_count = 1;
		entry.estimated_bytes = estimated_bytes;
		insert_new_entry(std::move(key), std::move(entry), count_stats);
	}

	/// Короткий alias для общего memo API.
	bool store_reconstruction_trace(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		const memo_reconstruction_trace& trace, bool = true) {
		(void)subset_hash;
		(void)subset_fingerprint;
		if (!trace.has_trace) {
			return false;
		}
		auto it = find_entry(bits, time);
		if (it == table_.end() || !it->second.has_exact) {
			return false;
		}
		if (!it->second.reconstruction_trace.has_trace) {
			++stats_.reconstruction_trace_entries;
		}
		it->second.reconstruction_trace = trace;
		touch_entry(it->second);
		return true;
	}

	void store_lb(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long lb, bool count_stats = true) {
		store_lower_bound(bits, time, subset_hash, subset_fingerprint, lb, count_stats);
	}

	/// Сохраняет LB(S,t). LB не перезаписывает exact-запись.
	void store_lower_bound(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long lb, bool count_stats = true) {
		(void)subset_hash;
		(void)subset_fingerprint;
		auto it = find_entry(bits, time);
		if (it != table_.end()) {
			std_unordered_memo_entry& entry = it->second;
			const long long old_lb = entry.lower_bound_value;
			if (!entry.has_exact && (!entry.has_lb || lb > entry.lower_bound_value)) {
				entry.has_lb = true;
				entry.lower_bound_value = lb;
			}
			touch_entry(entry);
			if (count_stats && entry.lower_bound_value != old_lb) {
				++stats_.updates;
				++stats_.lb_stores;
			}
			return;
		}

		const std::size_t estimated_bytes = estimate_entry_bytes(bits);
		if (!ensure_room_for_insert(estimated_bytes, count_stats)) {
			if (count_stats) {
				++stats_.rejected_no_room;
			}
			return;
		}

		std_unordered_memo_key key{ bits, time };
		std_unordered_memo_entry entry{};
		entry.has_lb = true;
		entry.lower_bound_value = lb;
		entry.best_job = -1;
		entry.use_count = 1;
		entry.estimated_bytes = estimated_bytes;
		insert_new_entry(std::move(key), std::move(entry), count_stats);
	}

	/// Удаляет записи, пока не выполняются capacity и memory budget.
	/// Для reference backend eviction — простой scan по unordered_map.
	void clean_if_needed(bool count_stats = true) {
		while ((capacity_ > 0 && table_.size() > capacity_) ||
			(memory_.strict_cap && memory_.budget_bytes > 0 &&
				memory_.used_bytes > memory_.budget_bytes)) {
			if (!evict_one(count_stats)) {
				break;
			}
		}
		stats_.final_size = table_.size();
	}

	/// Возвращает статистику с актуальным final_size.
	memo_table_stats stats() const {
		memo_table_stats s = stats_;
		s.final_size = table_.size();
		if (s.peak_size < table_.size()) {
			s.peak_size = table_.size();
		}
		return s;
	}

	/// Возвращает оценку памяти и настройки budget.
	memo_memory_accounting memory_accounting() const {
		return memory_;
	}

	/// Возвращает диагностику full-key reuse. Collision отдельно не считается:
	/// std::unordered_map сам вызывает equality по полному (S,t).
	memo_diagnostics diagnostics() const {
		return diagnostics_;
	}

	/// Оценка памяти, занятая reference backend.
	std::size_t memory_used_bytes() const {
		return memory_.used_bytes;
	}

private:
	/// Основной контейнер reference backend.
	/// Ключ владеет bitset S, value хранит exact/LB данные.
	using table_type = std::unordered_map<std_unordered_memo_key,
		std_unordered_memo_entry,
		std_unordered_memo_key_hash,
		std_unordered_memo_key_equal>;

	/// Ищет запись по view-key без копирования текущего S.
	table_type::iterator find_entry(const std::vector<std::uint64_t>& bits, schedule_time_t time) {
		return table_.find(std_unordered_memo_key_view{ bits, time });
	}

	/// Увеличивает use_count: часто используемая запись позже попадёт под eviction.
	static void touch_entry(std_unordered_memo_entry& entry) {
		if (entry.use_count < std::numeric_limits<std::int16_t>::max()) {
			++entry.use_count;
		}
	}

	/// Грубая оценка памяти: payload bitset S + node/key/value overhead unordered_map.
	std::size_t estimate_entry_bytes(const std::vector<std::uint64_t>& bits) const {
		const std::size_t payload = bits.size() * sizeof(std::uint64_t);
		const std::size_t node_overhead = sizeof(std_unordered_memo_key) +
			sizeof(std_unordered_memo_entry) + sizeof(void*) * 4U;
		return node_overhead + payload;
	}

	/// Проверяет capacity и memory budget без удаления записей.
	bool has_room_for_insert(std::size_t estimated_bytes) const {
		if (capacity_ > 0 && table_.size() >= capacity_) {
			return false;
		}
		if (memory_.strict_cap && memory_.budget_bytes > 0 &&
			memory_.used_bytes + estimated_bytes > memory_.budget_bytes) {
			return false;
		}
		return true;
	}

	/// Пытается освободить место под новую запись.
	/// Если место не удалось получить, store просто не сохраняет entry.
	bool ensure_room_for_insert(std::size_t estimated_bytes, bool count_stats) {
		while (!has_room_for_insert(estimated_bytes)) {
			if (table_.empty()) {
				break;
			}
			if (!evict_one(count_stats)) {
				break;
			}
			if (count_stats) {
				++stats_.forced_evictions;
			}
		}
		return has_room_for_insert(estimated_bytes);
	}

	/// Вставляет новый owned key и entry в unordered_map и обновляет stats/memory.
	void insert_new_entry(std_unordered_memo_key key, std_unordered_memo_entry entry, bool count_stats) {
		const bool has_exact = entry.has_exact;
		memory_.used_bytes += entry.estimated_bytes;
		table_.emplace(std::move(key), std::move(entry));
		if (count_stats) {
			++stats_.inserts;
			if (has_exact) {
				++stats_.exact_stores;
			}
			else {
				++stats_.lb_stores;
			}
		}
		if (table_.size() > stats_.peak_size) {
			stats_.peak_size = table_.size();
		}
		stats_.final_size = table_.size();
	}

	/// Простой reference eviction.
	/// Сначала предпочитает удалять LB-only записи, затем запись с меньшим use_count.
	bool evict_one(bool count_stats) {
		if (table_.empty()) {
			return false;
		}
		auto victim = table_.end();
		for (auto it = table_.begin(); it != table_.end(); ++it) {
			if (victim == table_.end()) {
				victim = it;
				continue;
			}
			const bool it_lb_only = !it->second.has_exact;
			const bool victim_lb_only = !victim->second.has_exact;
			if ((it_lb_only && !victim_lb_only) ||
				(it_lb_only == victim_lb_only &&
					it->second.use_count < victim->second.use_count)) {
				victim = it;
			}
		}
		if (victim == table_.end()) {
			return false;
		}
		const bool evicted_exact = victim->second.has_exact;
		const bool evicted_trace = victim->second.reconstruction_trace.has_trace;
		if (memory_.used_bytes >= victim->second.estimated_bytes) {
			memory_.used_bytes -= victim->second.estimated_bytes;
		}
		else {
			memory_.used_bytes = 0;
		}
		if (evicted_trace && stats_.reconstruction_trace_entries > 0) {
			--stats_.reconstruction_trace_entries;
		}
		table_.erase(victim);
		if (count_stats) {
			++stats_.evictions;
			if (evicted_exact) {
				++stats_.evictions_exact;
			}
			else {
				++stats_.evictions_lb;
			}
			++stats_.clean_calls;
		}
		stats_.final_size = table_.size();
		return true;
	}

	/// Собственно unordered_map с owned ключом (S,t).
	table_type table_;
	/// Лимит на число записей; 0 означает без лимита.
	std::size_t capacity_ = 0;
	/// Статистика операций reference backend.
	memo_table_stats stats_{};
	/// Оценка памяти и memory budget.
	memo_memory_accounting memory_{};
	/// Диагностика повторных подзадач.
	memo_diagnostics diagnostics_{};
};
