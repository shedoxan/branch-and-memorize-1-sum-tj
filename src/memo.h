#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "solver.h"

struct memo_reconstruction_trace {
	bool has_trace = false;
	int pivot_job = -1;
	int before_count = 0;
	int after_count = 0;
	std::uint64_t before_hash = 0;
	std::uint64_t before_fingerprint = 0;
	std::uint64_t after_hash = 0;
	std::uint64_t after_fingerprint = 0;
	schedule_time_t pivot_completion = 0;
	long long before_exact = 0;
	long long pivot_tardiness = 0;
	long long after_exact = 0;
};

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

/// Результат поиска подзадачи в M[(S,t)].
/// Exact-запись означает, что найдено OPT(S,t); LB-запись содержит только
/// допустимую нижнюю оценку и не подходит для восстановления расписания.
struct memo_lookup_result {
	/// Найдена ли какая-либо запись для полного ключа (S,t).
	bool found = false;
	/// true только для точной записи OPT(S,t).
	bool has_exact = false;
	/// Точное значение OPT(S,t), если has_exact == true.
	long long exact = 0;
	/// Нижняя оценка LB(S,t). Exact-запись также является корректной LB.
	long long lower_bound = 0;
	/// Первая работа оптимального продолжения; имеет смысл только для exact-записи.
	int best_job = -1;
	memo_reconstruction_trace reconstruction_trace{};
};

/// Внутренняя статистика custom memo backend.
/// Эти счётчики затем переносятся в solver_stats и попадают в benchmark CSV.
struct memo_table_stats {
	/// Успешные lookup по полному ключу (S,t).
	std::uint64_t hits = 0;
	/// Неуспешные lookup: состояние не найдено и будет решаться заново.
	std::uint64_t misses = 0;
	/// Новые записи, добавленные в M.
	std::uint64_t inserts = 0;
	/// Сохранения точных значений OPT(S,t).
	std::uint64_t exact_stores = 0;
	/// Сохранения нижних оценок LB(S,t).
	std::uint64_t lb_stores = 0;
	/// Обновления уже существующих записей.
	std::uint64_t updates = 0;
	/// Все удаления записей из-за capacity или memory limit.
	std::uint64_t evictions = 0;
	/// Удаления exact-записей. Это ухудшает скорость, но не нарушает точность.
	std::uint64_t evictions_exact = 0;
	/// Удаления LB-записей.
	std::uint64_t evictions_lb = 0;
	/// Запись не сохранена, потому что место освободить не удалось.
	std::uint64_t rejected_no_room = 0;
	/// Сколько записей удалено принудительно перед вставкой.
	std::uint64_t forced_evictions = 0;
	/// Сколько раз запускалась процедура очистки.
	std::uint64_t clean_calls = 0;
	/// Сколько LUFO-проходов выполнено.
	std::uint64_t lufo_decay_passes = 0;
	/// Максимальное число записей в M за запуск.
	std::size_t peak_size = 0;
	/// Число записей в M после последней операции.
	std::size_t final_size = 0;
	/// Время, потраченное на очистку/eviction.
	double clean_time_ms = 0.0;
	std::size_t reconstruction_trace_entries = 0;
};

/// Учёт памяти custom memo backend.
/// Значение used_bytes является оценкой, а не побайтовым отчётом allocator-а.
struct memo_memory_accounting {
	/// Оценка памяти, занятой записями, слотами и payload ключей.
	std::size_t used_bytes = 0;
	/// Бюджет памяти для M; 0 означает отсутствие бюджетного ограничения.
	std::size_t budget_bytes = 0;
	/// Если true, backend старается не превышать budget_bytes.
	bool strict_cap = true;
	/// Если true, на Windows дополнительно проверяется память всего процесса.
	bool process_memory_gate = false;
};

/// Диагностические счётчики для проверки качества ключей и hash table.
struct memo_diagnostics {
	/// Столкновения hash/start_time или несовпадения полного bitset S.
	std::uint64_t hash_collisions = 0;
	/// Сколько раз после hash match выполнялась полная проверка S.
	std::uint64_t full_key_rechecks = 0;
	/// Успешные повторные подзадачи: прямой эффект Branch-and-Memorize.
	std::uint64_t duplicate_subproblem_hits = 0;
};

/// Custom таблица памяти M для solution memorization.
/// Ключ exact-записи — полная подзадача (S,t): bitset S хранится явно,
/// t хранится отдельно, а hash/fingerprint используются только для ускорения.
class memo_table {
public:
	/// Создаёт memo table с ограничением на число записей.
	/// cap == 0 означает, что ограничение по количеству записей не задано.
	explicit memo_table(std::size_t cap = 0)
		: capacity_(cap) {}

	/// Полностью очищает M, но сохраняет настройки memory budget.
	void clear() {
		const std::size_t budget_bytes = memory_.budget_bytes;
		const bool strict_cap = memory_.strict_cap;
		const bool process_memory_gate = memory_.process_memory_gate;

		entries_.clear();
		slot_indices_.clear();
		slots_.clear();
		tombstones_ = 0;
		bits_words_ = 0;
		key_storage_.clear();
		free_key_blocks_.clear();
		reconstruction_traces_.clear();
		reconstruction_trace_entries_ = 0;
		lufo_cursor_ = 0;

		stats_ = {};
		diagnostics_ = {};
		memory_ = {};
		memory_.budget_bytes = budget_bytes;
		memory_.strict_cap = strict_cap;
		memory_.process_memory_gate = process_memory_gate;
	}

	/// Меняет ограничение на число записей и при необходимости запускает eviction.
	void set_capacity(std::size_t capacity, bool count_stats = true) {
		capacity_ = capacity;
		if (capacity_ > 0) {
			entries_.reserve(capacity_);
			slot_indices_.reserve(capacity_);
			if (!reconstruction_traces_.empty()) {
				reconstruction_traces_.reserve(capacity_);
			}
			ensure_slot_table_for_insert(capacity_);
		}
		while (capacity_ > 0 && entries_.size() > capacity_) {
			if (!evict_one_by_policy(count_stats)) {
				break;
			}
		}
		stats_.final_size = entries_.size();
	}

	/// Задаёт бюджет памяти для M и вытесняет записи, если текущая таблица его нарушает.
	void set_memory_budget_bytes(std::size_t budget_bytes, bool strict_cap) {
		memory_.budget_bytes = budget_bytes;
		memory_.strict_cap = strict_cap;

		if (memory_.strict_cap && memory_.budget_bytes > 0) {
			while (!has_room_for_insert(0) && !entries_.empty()) {
				if (!evict_one_by_policy(false)) {
					break;
				}
			}
		}
		stats_.final_size = entries_.size();
	}

	/// Включает дополнительную проверку памяти процесса. Используется только на Windows.
	void set_process_memory_gate(bool enabled) {
		memory_.process_memory_gate = enabled;
	}

	/// Управляет измерением времени очистки; выключается для меньших накладных расходов.
	void set_profiling_timers_enabled(bool enabled) {
		profiling_timers_enabled_ = enabled;
	}

	/// Текущее число memo-записей.
	std::size_t size() const {
		return entries_.size();
	}

	/// Текущее ограничение на число записей; 0 означает без такого ограничения.
	std::size_t capacity() const {
		return capacity_;
	}

	/// Ищет состояние (S,t) в M.
	/// Возвращает exact, если он есть; иначе может вернуть LB-запись.
	memo_lookup_result lookup(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats = true) {
		// bits задаёт S, time задаёт t. Хеш и fingerprint только ускоряют поиск:
		// при совпадении hash обязательно сверяется полный bitset S.
		memo_lookup_result result{};

		const entry_id id = find_entry(bits, time, subset_hash, subset_fingerprint, count_stats);
		if (id == invalid_entry_id()) {
			if (count_stats) {
				++stats_.misses;
			}
			return result;
		}

		touch_entry(id);
		const memo_entry& entry = entries_[id];
		result.found = true;
		result.has_exact = entry.has_exact;
		result.exact = entry.value;
		result.lower_bound = entry.value;
		result.best_job = entry.best_job;
		if (!reconstruction_traces_.empty() && id < reconstruction_traces_.size()) {
			result.reconstruction_trace = reconstruction_traces_[id];
		}
		if (count_stats) {
			++stats_.hits;
			++diagnostics_.duplicate_subproblem_hits;
		}
		return result;
	}

	/// Сохраняет нижнюю оценку LB(S,t).
	/// Если для того же ключа уже есть exact, запись не меняется.
	void store_lower_bound(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long lb, bool count_stats = true) {
		// Безопасная запись M[(S,t)] = LB(S,t), когда точное OPT ещё не известно.
		// LB не перезаписывает exact: точная запись важнее любой нижней оценки.
		if (!prepare_for_bits_size(bits.size())) {
			if (count_stats) {
				++stats_.rejected_no_room;
			}
			return;
		}

		const entry_id existing_id = find_entry(bits, time, subset_hash, subset_fingerprint, count_stats);
		if (existing_id != invalid_entry_id()) {
			memo_entry& entry = entries_[existing_id];
			const std::int64_t old_lb = entry.value;

			if (!entry.has_exact && lb > entry.value) {
				entry.value = lb;
			}

			touch_entry(existing_id);
			if (count_stats && entry.value != old_lb) {
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

		insert_new_entry(bits, time, subset_hash, subset_fingerprint, false, 0, lb, -1, estimated_bytes, count_stats);
	}

	/// Сохраняет точное значение OPT(S,t) и best_job для reconstruction.
	/// Если раньше была LB-запись, она повышается до exact-записи.
	void store_exact(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long exact, int best_job, bool count_stats = true) {
		// Точная запись M[(S,t)] = OPT(S,t) после полного решения подзадачи.
		// best_job хранит первую работу оптимального продолжения для reconstruction.
		if (!prepare_for_bits_size(bits.size())) {
			if (count_stats) {
				++stats_.rejected_no_room;
			}
			return;
		}

		const entry_id existing_id = find_entry(bits, time, subset_hash, subset_fingerprint, count_stats);
		if (existing_id != invalid_entry_id()) {
			memo_entry& entry = entries_[existing_id];
			bool changed = false;

			if (!entry.has_exact || entry.value != exact) {
				entry.has_exact = true;
				entry.value = exact;
				changed = true;
			}
			if (entry.best_job != best_job) {
				entry.best_job = best_job;
				changed = true;
			}

			touch_entry(existing_id);
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

		insert_new_entry(bits, time, subset_hash, subset_fingerprint, true, exact, exact, best_job, estimated_bytes, count_stats);
	}

	bool store_reconstruction_trace(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		const memo_reconstruction_trace& trace, bool count_stats = true) {
		if (!trace.has_trace) {
			return false;
		}
		const entry_id id = find_entry(bits, time, subset_hash, subset_fingerprint, count_stats);
		if (id == invalid_entry_id() || !entries_[id].has_exact) {
			return false;
		}
		if (!ensure_trace_storage()) {
			if (count_stats) {
				++stats_.rejected_no_room;
			}
			return false;
		}
		if (!reconstruction_traces_[id].has_trace) {
			++reconstruction_trace_entries_;
		}
		reconstruction_traces_[id] = trace;
		touch_entry(id);
		return true;
	}

	/// Возвращает снимок статистики; final_size пересчитывается по текущему entries_.
	memo_table_stats stats() const {
		memo_table_stats s = stats_;
		s.final_size = entries_.size();
		if (s.peak_size < entries_.size()) {
			s.peak_size = entries_.size();
		}
		s.reconstruction_trace_entries = reconstruction_trace_entries_;
		return s;
	}

	/// Возвращает текущую оценку памяти и настройки memory budget.
	memo_memory_accounting memory_accounting() const {
		return memory_;
	}

	/// Возвращает диагностические счётчики hash/full-key verification.
	memo_diagnostics diagnostics() const {
		return diagnostics_;
	}

	/// Проверяет, можно ли добавить запись с оценочным размером estimated_bytes.
	/// Функция ничего не удаляет; eviction выполняется в ensure_room_for_insert().
	bool has_room_for_insert(std::size_t estimated_bytes) const {
		if (capacity_ > 0 && entries_.size() >= capacity_) {
			return false;
		}
		if (memory_.strict_cap && memory_.budget_bytes > 0) {
			if (memory_.used_bytes + estimated_bytes > memory_.budget_bytes) {
				return false;
			}

			if (memory_.process_memory_gate) {
				const std::size_t process_bytes = current_process_memory_bytes();
				if (process_bytes > 0 && process_bytes + estimated_bytes > memory_.budget_bytes) {
					return false;
				}
			}
		}
		return true;
	}

private:
	/// Индекс записи в entries_. uint32_t экономит память в slot table.
	using entry_id = std::uint32_t;
	/// Значение ячейки hash table slots_: entry id или служебный маркер.
	using slot_word = std::uint32_t;

	/// Одна запись custom memo table.
	/// Полный ключ: (S,t), где S лежит в key_storage_, а t — в start_time.
	struct memo_entry {
		/// Hash пары (S,t) для быстрой адресации в slots_.
		std::uint64_t key_hash = 0;
		/// OPT(S,t), если has_exact == true; иначе LB(S,t).
		std::int64_t value = 0;

		/// t — стартовый момент подзадачи.
		schedule_time_t start_time = 0;
		/// Смещение bitset S в key_storage_.
		std::uint32_t key_offset = 0;
		/// Первая работа оптимального продолжения; используется только для exact.
		int best_job = -1;
		/// Счётчик полезности записи для LUFO eviction.
		std::int16_t use_count = 0;
		/// true означает, что value — это точный OPT(S,t), а не LB.
		bool has_exact = false;
	};

	/// Пустая ячейка open-addressing hash table.
	static constexpr slot_word slot_empty_id = std::numeric_limits<slot_word>::max();
	/// Tombstone: запись удалена, но цепочку пробирования разрывать нельзя.
	static constexpr slot_word slot_tombstone_id = std::numeric_limits<slot_word>::max() - 1;

	static constexpr entry_id invalid_entry_id() {
		return std::numeric_limits<entry_id>::max();
	}

	/// В slots_ обычные entry id меньше служебных маркеров empty/tombstone.
	static bool slot_is_occupied(slot_word value) {
		return value < slot_tombstone_id;
	}

	/// Небольшой битовый rotate для смешивания hash компонентов.
	static std::uint64_t rotate_left64(std::uint64_t x, unsigned int r) {
		return (x << r) | (x >> (64U - r));
	}

	/// SplitMix64-подобное перемешивание; снижает кластеризацию hash table.
	static std::uint64_t mix64(std::uint64_t x) {
		x ^= (x >> 30);
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= (x >> 27);
		x *= 0x94D049BB133111EBULL;
		x ^= (x >> 31);
		return x;
	}

	/// Размер slots_ держится степенью двойки, чтобы индекс считать через mask.
	static std::size_t next_pow2(std::size_t x) {
		if (x <= 1) {
			return 1;
		}
		--x;
		x |= x >> 1;
		x |= x >> 2;
		x |= x >> 4;
		x |= x >> 8;
		x |= x >> 16;
		if constexpr (sizeof(std::size_t) >= 8) {
			x |= x >> 32;
		}
		return x + 1;
	}

	/// Текущий working set процесса. На не-Windows возвращает 0.
	static std::size_t current_process_memory_bytes() {
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS_EX pmc{};
		if (GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
			sizeof(pmc)) == 0) {
			return 0;
		}
		return static_cast<std::size_t>(pmc.WorkingSetSize);
#else
		return 0;
#endif
	}

	/// Проверяет, что все ключи в таблице имеют одинаковый размер bitset.
	/// Для одной задачи n фиксировано, поэтому изменение words допускается только
	/// до появления первой записи.
	bool prepare_for_bits_size(std::size_t words) {
		if (entries_.empty()) {
			if (bits_words_ != words) {
				bits_words_ = words;
				key_storage_.clear();
				free_key_blocks_.clear();
				slots_.clear();
				tombstones_ = 0;
				lufo_cursor_ = 0;
			}
			return true;
		}
		return bits_words_ == words;
	}

	/// Оценка памяти новой записи для memory budget.
	std::size_t estimate_entry_bytes(const std::vector<std::uint64_t>& bits) const {
		return estimate_entry_bytes_from_words(bits.size());
	}

	/// Оценка памяти уже сохранённой записи.
	std::size_t estimate_stored_entry_bytes() const {
		return estimate_entry_bytes_from_words(bits_words_);
	}

	/// Грубая оценка: entry + payload bitset + доля hash slots и bookkeeping.
	std::size_t estimate_entry_bytes_from_words(std::size_t word_count) const {
		const std::size_t payload = word_count * sizeof(std::uint64_t);
		const std::size_t slot_share = sizeof(slot_word) * 3; // приближённая доля slots_ с запасом на load factor
		const std::size_t arena_bookkeeping = sizeof(std::size_t) * 2;
		const std::size_t trace_payload =
			reconstruction_traces_.empty() ? 0 : sizeof(memo_reconstruction_trace);
		return sizeof(memo_entry) + sizeof(std::uint32_t) +
			payload + slot_share + arena_bookkeeping + trace_payload;
	}

	bool ensure_trace_storage() {
		if (!reconstruction_traces_.empty()) {
			return reconstruction_traces_.size() == entries_.size();
		}
		const std::size_t estimated_bytes = entries_.size() * sizeof(memo_reconstruction_trace);
		if (memory_.strict_cap && memory_.budget_bytes > 0 &&
			memory_.used_bytes + estimated_bytes > memory_.budget_bytes) {
			return false;
		}
		reconstruction_traces_.resize(entries_.size());
		memory_.used_bytes += estimated_bytes;
		return true;
	}

	/// Строит hash для пары (S,t). subset_hash/subset_fingerprint описывают только S,
	/// поэтому time обязательно добавляется отдельно.
	std::uint64_t probe_hash(std::uint64_t subset_hash,
		std::uint64_t subset_fingerprint,
		schedule_time_t time) const {
		// Хеширует пару (S,t): Zobrist-хеш множества S плюс стартовый момент t.
		const std::uint64_t t = static_cast<std::uint64_t>(time);
		return mix64(subset_hash ^ rotate_left64(subset_fingerprint, 21) ^
			mix64(t + 0x9E3779B97F4A7C15ULL));
	}

	/// Готовит open-addressing table к вставке: расширяет или чистит tombstones.
	void ensure_slot_table_for_insert(std::size_t target_entries) {
		if (target_entries == 0) {
			return;
		}

		const std::size_t min_slots_for_load =
			next_pow2(std::max<std::size_t>(16, ((target_entries * 10) + 6) / 7));

		bool need_rehash = false;
		std::size_t new_slots = min_slots_for_load;

		if (slots_.empty()) {
			need_rehash = true;
		}
		else {
			const std::size_t occupied_or_tomb =
				entries_.size() + tombstones_ + 1;
			if (occupied_or_tomb * 10 > slots_.size() * 7) {
				need_rehash = true;
				new_slots = std::max(slots_.size() * 2, min_slots_for_load);
			}
			else if (tombstones_ > slots_.size() / 5) {
				need_rehash = true;
				new_slots = std::max(slots_.size(), min_slots_for_load);
			}
		}

		if (need_rehash) {
			rehash_slots(new_slots);
		}
	}

	/// Перестраивает slots_ без изменения entries_ и payload ключей.
	void rehash_slots(std::size_t requested_slots) {
		const std::size_t slot_count = next_pow2(std::max<std::size_t>(16, requested_slots));
		std::vector<slot_word> new_slots(slot_count, slot_empty_id);

		for (entry_id id = 0; id < static_cast<entry_id>(entries_.size()); ++id) {
			memo_entry& entry = entries_[id];
			const std::size_t idx = find_insert_slot_in(new_slots, entry.key_hash);
			new_slots[idx] = id;
			slot_indices_[id] = static_cast<std::uint32_t>(idx);
		}

		slots_.swap(new_slots);
		tombstones_ = 0;
	}

	/// Ищет bucket для вставки по linear probing, переиспользуя первый tombstone.
	static std::size_t find_insert_slot_in(std::vector<slot_word>& slots, std::uint64_t h) {
		const std::size_t mask = slots.size() - 1;
		std::size_t idx = static_cast<std::size_t>(h) & mask;
		std::size_t first_tombstone = std::numeric_limits<std::size_t>::max();

		for (std::size_t step = 0; step < slots.size(); ++step) {
			const slot_word s = slots[idx];
			if (s == slot_empty_id) {
				return (first_tombstone != std::numeric_limits<std::size_t>::max()) ? first_tombstone : idx;
			}
			if (s == slot_tombstone_id && first_tombstone == std::numeric_limits<std::size_t>::max()) {
				first_tombstone = idx;
			}
			idx = (idx + 1) & mask;
		}

		return (first_tombstone != std::numeric_limits<std::size_t>::max()) ? first_tombstone : 0;
	}

	/// Выделяет блок в key_storage_ или переиспользует освобождённый блок того же размера.
	std::uint32_t allocate_key_block(std::size_t word_count) {
		if (word_count == 0) {
			return 0;
		}
		if (free_key_blocks_.size() <= word_count) {
			free_key_blocks_.resize(static_cast<std::size_t>(word_count) + 1U);
		}
		std::vector<std::uint32_t>& free_list = free_key_blocks_[word_count];
		if (!free_list.empty()) {
			const std::uint32_t offset = free_list.back();
			free_list.pop_back();
			return offset;
		}

		const std::size_t offset = key_storage_.size();
		key_storage_.resize(offset + word_count, 0);
		return static_cast<std::uint32_t>(offset);
	}

	/// Mutable pointer на сохранённый bitset S.
	std::uint64_t* key_block_ptr_mut(std::uint32_t offset) {
		if (bits_words_ == 0) {
			return nullptr;
		}
		return key_storage_.data() + offset;
	}

	/// Const pointer на сохранённый bitset S.
	const std::uint64_t* key_block_ptr(std::uint32_t offset) const {
		if (bits_words_ == 0) {
			return nullptr;
		}
		return key_storage_.data() + offset;
	}

	/// Копирует полный bitset S в key_storage_ и записывает offset в entry.
	void store_key_payload(memo_entry& entry, const std::vector<std::uint64_t>& bits) {
		entry.key_offset = allocate_key_block(bits_words_);

		std::uint64_t* dst = key_block_ptr_mut(entry.key_offset);
		if (bits_words_ == 0) {
			return;
		}
		std::fill(dst, dst + bits_words_, 0);
		std::memcpy(dst, bits.data(), bits_words_ * sizeof(std::uint64_t));
	}

	/// Возвращает блок bitset S в free list; память арены не уменьшается.
	void release_key_block(const memo_entry& entry) {
		if (bits_words_ == 0) {
			return;
		}
		if (free_key_blocks_.size() <= bits_words_) {
			free_key_blocks_.resize(bits_words_ + 1U);
		}
		free_key_blocks_[bits_words_].push_back(entry.key_offset);
	}

	/// Полная проверка S. Это защита от hash/fingerprint collisions.
	bool bits_equal(const memo_entry& entry, const std::vector<std::uint64_t>& bits) const {
		if (bits.size() != bits_words_) {
			return false;
		}
		if (bits_words_ == 0) {
			return true;
		}
		const std::uint64_t* stored = key_block_ptr(entry.key_offset);
		if (stored == nullptr) {
			return false;
		}
		return std::memcmp(stored, bits.data(), bits_words_ * sizeof(std::uint64_t)) == 0;
	}

	/// Отмечает использование записи: часто используемые состояния живут дольше.
	void touch_entry(entry_id id) {
		if (id >= entries_.size()) {
			return;
		}
		memo_entry& entry = entries_[id];
		if (entry.use_count < std::numeric_limits<std::int16_t>::max()) {
			++entry.use_count;
		}
	}

	/// Ищет запись по полному ключу (S,t).
	/// Hash указывает область поиска, но результат принимается только после bits_equal().
	entry_id find_entry(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats) {
		if (entries_.empty() || slots_.empty()) {
			return invalid_entry_id();
		}
		if (bits.size() != bits_words_) {
			return invalid_entry_id();
		}

		const std::uint64_t key_hash = probe_hash(subset_hash, subset_fingerprint, time);
		const std::size_t mask = slots_.size() - 1;
		std::size_t idx = static_cast<std::size_t>(key_hash) & mask;

		for (std::size_t step = 0; step < slots_.size(); ++step) {
			const slot_word s = slots_[idx];
			if (s == slot_empty_id) {
				return invalid_entry_id();
			}
			if (slot_is_occupied(s)) {
				const entry_id id = static_cast<entry_id>(s);
				const memo_entry& entry = entries_[id];
				if (entry.key_hash != key_hash) {
					if (count_stats) {
						++diagnostics_.hash_collisions;
					}
				}
				else if (entry.start_time != time) {
					if (count_stats) {
						++diagnostics_.hash_collisions;
					}
				}
				else {
					if (count_stats) {
						++diagnostics_.full_key_rechecks;
					}
					if (bits_equal(entry, bits)) {
						return id;
					}
					if (count_stats) {
						++diagnostics_.hash_collisions;
					}
				}
			}
			idx = (idx + 1) & mask;
		}

		return invalid_entry_id();
	}

	/// Добавляет новую запись. Перед вызовом место уже должно быть подготовлено.
	void insert_new_entry(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		bool has_exact, long long exact, long long lower_bound, int best_job,
		std::size_t estimated_bytes, bool count_stats) {
		ensure_slot_table_for_insert(entries_.size() + 1);
		if (capacity_ > 0) {
			entries_.reserve(capacity_);
		}

		memo_entry entry;
		entry.start_time = time;
		entry.key_hash = probe_hash(subset_hash, subset_fingerprint, time);
		entry.has_exact = has_exact;
		entry.value = has_exact ? exact : lower_bound;
		entry.best_job = best_job;
		entry.use_count = 1;
		store_key_payload(entry, bits);

		entries_.push_back(std::move(entry));
		const entry_id new_id = static_cast<entry_id>(entries_.size() - 1);
		slot_indices_.push_back(0U);
		if (!reconstruction_traces_.empty()) {
			reconstruction_traces_.push_back({});
		}

		const std::size_t slot_idx = find_insert_slot_in(slots_, entry.key_hash);
		if (slots_[slot_idx] == slot_tombstone_id && tombstones_ > 0) {
			--tombstones_;
		}
		slots_[slot_idx] = new_id;
		slot_indices_[new_id] = static_cast<std::uint32_t>(slot_idx);

		memory_.used_bytes += estimated_bytes;
		if (count_stats) {
			++stats_.inserts;
			if (has_exact) {
				++stats_.exact_stores;
			}
			else {
				++stats_.lb_stores;
			}
		}
		if (entries_.size() > stats_.peak_size) {
			stats_.peak_size = entries_.size();
		}
		stats_.final_size = entries_.size();
	}

	/// Удаляет запись из M.
	/// Удаление exact-записи не нарушает оптимальность: состояние будет решено заново.
	void erase_entry(entry_id id, bool count_stats) {
		if (id >= entries_.size()) {
			return;
		}

		const std::size_t removed_slot = slot_indices_[id];
		const std::size_t removed_bytes = estimate_stored_entry_bytes();
		const bool removed_exact = entries_[id].has_exact;
		const bool removed_trace =
			!reconstruction_traces_.empty() &&
			id < reconstruction_traces_.size() &&
			reconstruction_traces_[id].has_trace;

		if (removed_slot < slots_.size() && slot_is_occupied(slots_[removed_slot])) {
			slots_[removed_slot] = slot_tombstone_id;
			++tombstones_;
		}

		release_key_block(entries_[id]);

		const entry_id last_id = static_cast<entry_id>(entries_.size() - 1);
		if (id != last_id) {
			entries_[id] = std::move(entries_[last_id]);
			slot_indices_[id] = slot_indices_[last_id];
			if (!reconstruction_traces_.empty()) {
				reconstruction_traces_[id] = reconstruction_traces_[last_id];
			}
			const std::size_t moved_slot = slot_indices_[id];
			if (moved_slot < slots_.size() && slot_is_occupied(slots_[moved_slot])) {
				slots_[moved_slot] = id;
			}
		}
		entries_.pop_back();
		slot_indices_.pop_back();
		if (!reconstruction_traces_.empty()) {
			reconstruction_traces_.pop_back();
		}
		if (removed_trace && reconstruction_trace_entries_ > 0) {
			--reconstruction_trace_entries_;
		}
		if (lufo_cursor_ >= entries_.size()) {
			lufo_cursor_ = 0;
		}

		if (memory_.used_bytes >= removed_bytes) {
			memory_.used_bytes -= removed_bytes;
		}
		else {
			memory_.used_bytes = 0;
		}

		if (count_stats) {
			++stats_.evictions;
			if (removed_exact) {
				++stats_.evictions_exact;
			}
			else {
				++stats_.evictions_lb;
			}
		}
		stats_.final_size = entries_.size();

		if (!slots_.empty() && tombstones_ > slots_.size() / 4) {
			rehash_slots(std::max<std::size_t>(slots_.size(), 16));
		}
	}

	/// Размер одного LUFO-прохода: чистка должна быть ограниченной, чтобы store не зависал.
	std::size_t lufo_decay_batch_size() const {
		const std::size_t n = entries_.size();
		if (n <= 256) {
			return n;
		}
		std::size_t batch = n / 32;
		if (batch < 256) {
			batch = 256;
		}
		if (batch > 8192) {
			batch = 8192;
		}
		return std::min(batch, n);
	}

	/// LUFO-подобное старение: use_count уменьшается, записи с отрицательным счётчиком удаляются.
	std::size_t evict_lufo_decay_pass(bool count_stats) {
		if (entries_.empty()) {
			lufo_cursor_ = 0;
			return 0;
		}

		++stats_.lufo_decay_passes;
		std::size_t removed = 0;
		std::size_t scanned = 0;
		const std::size_t scan_budget = lufo_decay_batch_size();
		const std::int16_t use_count_min = std::numeric_limits<std::int16_t>::min();
		while (!entries_.empty() && scanned < scan_budget) {
			if (lufo_cursor_ >= entries_.size()) {
				lufo_cursor_ = 0;
			}

			const entry_id i = static_cast<entry_id>(lufo_cursor_);
			memo_entry& entry = entries_[i];
			if (entry.use_count > use_count_min) {
				--entry.use_count;
			}
			if (entry.use_count < 0) {
				erase_entry(i, count_stats);
				++removed;
				continue;
			}
			++lufo_cursor_;
			++scanned;
		}
		return removed;
	}

	/// Пытается удалить хотя бы одну запись текущей политикой.
	bool evict_one_by_policy(bool count_stats) {
		return evict_by_policy(count_stats) > 0;
	}

	/// Единая точка запуска eviction. Сейчас политика фактически LUFO decay.
	std::size_t evict_by_policy(bool count_stats) {
		if (entries_.empty()) {
			return 0;
		}
		const bool measure_clean_time = profiling_timers_enabled_;
		const auto start = measure_clean_time
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		const std::size_t removed = evict_lufo_decay_pass(count_stats);
		++stats_.clean_calls;
		if (measure_clean_time) {
			const auto finish = std::chrono::steady_clock::now();
			stats_.clean_time_ms +=
				std::chrono::duration<double, std::milli>(finish - start).count();
		}
		return removed;
	}

	/// Освобождает место для новой записи. Если место получить не удалось,
	/// store просто откажется от сохранения, что не меняет exact objective.
	bool ensure_room_for_insert(std::size_t estimated_bytes, bool count_stats) {
		while (!has_room_for_insert(estimated_bytes)) {
			if (entries_.empty()) {
				break;
			}
			const std::size_t removed = evict_by_policy(count_stats);
			if (removed == 0) {
				continue;
			}
			if (count_stats) {
				stats_.forced_evictions += static_cast<std::uint64_t>(removed);
			}
		}
		return has_room_for_insert(estimated_bytes);
	}

	/// Плотный массив записей M. entry_id является индексом в этом массиве.
	std::vector<memo_entry> entries_;
	/// Для каждой entries_[id] хранит индекс bucket-а в slots_.
	std::vector<std::uint32_t> slot_indices_;
	/// Open-addressing hash table: bucket -> entry_id / empty / tombstone.
	std::vector<slot_word> slots_;
	/// Число tombstone bucket-ов; при избытке запускается rehash.
	std::size_t tombstones_ = 0;

	/// Сколько uint64_t занимает bitset S для данной задачи.
	std::size_t bits_words_ = 0;
	/// Арена для полных bitset-ключей S.
	std::vector<std::uint64_t> key_storage_;
	/// Free lists блоков key_storage_ по размеру word_count.
	std::vector<std::vector<std::uint32_t>> free_key_blocks_;
	std::vector<memo_reconstruction_trace> reconstruction_traces_;
	std::size_t reconstruction_trace_entries_ = 0;

	/// Текущая позиция кругового LUFO-сканирования.
	std::size_t lufo_cursor_ = 0;
	/// Максимальное число записей; 0 означает без ограничения по количеству.
	std::size_t capacity_ = 0;
	/// Включены ли внутренние таймеры очистки.
	bool profiling_timers_enabled_ = true;
	/// Статистика операций memo table.
	memo_table_stats stats_{};
	/// Оценка памяти и бюджет.
	memo_memory_accounting memory_{};
	/// Диагностика hash/full-key verification.
	memo_diagnostics diagnostics_{};
};
