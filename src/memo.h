#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "solver.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

struct memo_lookup_result {
	bool found = false;
	bool has_exact = false;
	long long exact = 0;
	long long lower_bound = 0;
	int best_job = -1;
};

struct memo_table_stats {
	std::uint64_t hits = 0;
	std::uint64_t misses = 0;
	std::uint64_t inserts = 0;
	std::uint64_t updates = 0;
	std::uint64_t evictions = 0;
	std::uint64_t rejected_no_room = 0;
	std::uint64_t forced_evictions = 0;
	std::uint64_t clean_calls = 0;
	std::uint64_t lufo_decay_passes = 0;
	std::size_t peak_size = 0;
	std::size_t final_size = 0;
	double clean_time_ms = 0.0;
};

struct memo_memory_accounting {
	std::size_t used_bytes = 0;
	std::size_t budget_bytes = 0;
	bool strict_cap = true;
	bool process_memory_gate = false;
};

struct memo_diagnostics {
	std::uint64_t hash_collisions = 0;
	std::uint64_t full_key_rechecks = 0;
	std::uint64_t duplicate_subproblem_hits = 0;
	std::uint64_t fingerprint_mismatches = 0;
};

class memo_table {
public:
	explicit memo_table(std::size_t cap = 0)
		: capacity_(cap) {}

	void clear() {
		const std::size_t budget_bytes = memory_.budget_bytes;
		const bool strict_cap = memory_.strict_cap;
		const bool process_memory_gate = memory_.process_memory_gate;

		entries_.clear();
		entry_cold_.clear();
		slots_.clear();
		tombstones_ = 0;
		bits_words_ = 0;
		bits_segments_.clear();
		free_bits_blocks_.clear();
		next_bits_block_id_ = 0;
		bits_blocks_per_segment_ = 0;
		lufo_cursor_ = 0;
		lufo_exact_skip_toggle_ = false;

		stats_ = {};
		diagnostics_ = {};
		memory_ = {};
		memory_.budget_bytes = budget_bytes;
		memory_.strict_cap = strict_cap;
		memory_.process_memory_gate = process_memory_gate;
	}

	void set_capacity(std::size_t capacity, bool count_stats = true) {
		capacity_ = capacity;
		if (capacity_ > 0) {
			entries_.reserve(capacity_);
			entry_cold_.reserve(capacity_);
			ensure_slot_table_for_insert(capacity_);
		}
		while (capacity_ > 0 && entries_.size() > capacity_) {
			if (!evict_one_by_policy(count_stats)) {
				break;
			}
		}
		stats_.final_size = entries_.size();
	}

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

	void set_process_memory_gate(bool enabled) {
		memory_.process_memory_gate = enabled;
	}

	void set_profiling_timers_enabled(bool enabled) {
		profiling_timers_enabled_ = enabled;
	}

	void set_lufo_exact_protection(bool enabled) {
		lufo_exact_protection_ = enabled;
	}

	std::size_t size() const {
		return entries_.size();
	}

	std::size_t capacity() const {
		return capacity_;
	}

	memo_lookup_result lookup(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats = true) {
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
		result.exact = entry.exact;
		result.lower_bound = entry.lower_bound;
		result.best_job = entry.best_job;
		if (count_stats) {
			++stats_.hits;
			++diagnostics_.duplicate_subproblem_hits;
		}
		return result;
	}

	void store_lower_bound(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long lb, bool count_stats = true) {
		if (!prepare_for_bits_size(bits.size())) {
			if (count_stats) {
				++stats_.rejected_no_room;
			}
			return;
		}

		const entry_id existing_id = find_entry(bits, time, subset_hash, subset_fingerprint, count_stats);
		if (existing_id != invalid_entry_id()) {
			memo_entry& entry = entries_[existing_id];
			const std::int64_t old_lb = entry.lower_bound;

			if (entry.has_exact) {
				entry.lower_bound = entry.exact;
			}
			else if (lb > entry.lower_bound) {
				entry.lower_bound = lb;
			}

			touch_entry(existing_id);
			if (count_stats && entry.lower_bound != old_lb) {
				++stats_.updates;
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

	void store_exact(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		long long exact, int best_job, bool count_stats = true) {
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

			if (!entry.has_exact || entry.exact != exact) {
				entry.has_exact = true;
				entry.exact = exact;
				changed = true;
			}
			if (entry.lower_bound != exact) {
				entry.lower_bound = exact;
				changed = true;
			}
			if (entry.best_job != best_job) {
				entry.best_job = best_job;
				changed = true;
			}

			touch_entry(existing_id);
			if (count_stats && changed) {
				++stats_.updates;
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

	memo_table_stats stats() const {
		memo_table_stats s = stats_;
		s.final_size = entries_.size();
		if (s.peak_size < entries_.size()) {
			s.peak_size = entries_.size();
		}
		return s;
	}

	memo_memory_accounting memory_accounting() const {
		return memory_;
	}

	memo_diagnostics diagnostics() const {
		return diagnostics_;
	}

	void reset_diagnostics() {
		diagnostics_ = {};
	}

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
	using entry_id = std::uint32_t;

	struct slot {
		std::uint32_t id = 0;
		std::uint8_t state = 0; // 0=empty, 1=occupied, 2=tombstone
	};

	struct memo_entry {
		std::uint64_t subset_hash = 0;
		std::uint64_t subset_fingerprint = 0;
		std::int64_t exact = 0;
		std::int64_t lower_bound = 0;

		schedule_time_t start_time = 0;
		int best_job = -1;
		std::int32_t use_count = 0;
		std::uint32_t bits_block = 0;
		bool has_exact = false;
	};

	struct memo_entry_cold {
		std::uint32_t approx_bytes = 0;
		std::uint32_t slot_index = 0;
	};

	struct bits_segment {
		std::vector<std::uint64_t> words;
		std::uint32_t live_blocks = 0;
	};

	static constexpr std::uint8_t slot_empty = 0;
	static constexpr std::uint8_t slot_occupied = 1;
	static constexpr std::uint8_t slot_tombstone = 2;

	static constexpr entry_id invalid_entry_id() {
		return std::numeric_limits<entry_id>::max();
	}

	static std::uint64_t rotate_left64(std::uint64_t x, unsigned int r) {
		return (x << r) | (x >> (64U - r));
	}

	static std::uint64_t mix64(std::uint64_t x) {
		x ^= (x >> 30);
		x *= 0xBF58476D1CE4E5B9ULL;
		x ^= (x >> 27);
		x *= 0x94D049BB133111EBULL;
		x ^= (x >> 31);
		return x;
	}

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

	bool prepare_for_bits_size(std::size_t words) {
		if (entries_.empty()) {
			if (bits_words_ != words) {
				bits_words_ = words;
				bits_segments_.clear();
				free_bits_blocks_.clear();
				next_bits_block_id_ = 0;
				bits_blocks_per_segment_ = 0;
				slots_.clear();
				tombstones_ = 0;
				lufo_cursor_ = 0;
			}
			return true;
		}
		return bits_words_ == words;
	}

	std::size_t estimate_entry_bytes(const std::vector<std::uint64_t>& bits) const {
		const std::size_t bits_payload = bits.size() * sizeof(std::uint64_t);
		const std::size_t slot_share = sizeof(slot) * 3; // rough share with load factor slack
		const std::size_t arena_bookkeeping = sizeof(std::size_t) * 2;
		return sizeof(memo_entry) + sizeof(memo_entry_cold) +
			bits_payload + slot_share + arena_bookkeeping;
	}

	std::uint64_t probe_hash(std::uint64_t subset_hash,
		std::uint64_t subset_fingerprint,
		schedule_time_t time) const {
		const std::uint64_t t = static_cast<std::uint64_t>(time);
		return mix64(subset_hash ^ rotate_left64(subset_fingerprint, 21) ^
			mix64(t + 0x9E3779B97F4A7C15ULL));
	}

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

	void rehash_slots(std::size_t requested_slots) {
		const std::size_t slot_count = next_pow2(std::max<std::size_t>(16, requested_slots));
		std::vector<slot> new_slots(slot_count);

		for (entry_id id = 0; id < static_cast<entry_id>(entries_.size()); ++id) {
			memo_entry& entry = entries_[id];
			const std::size_t idx = find_insert_slot_in(new_slots, probe_hash(
				entry.subset_hash, entry.subset_fingerprint, entry.start_time));
			new_slots[idx].state = slot_occupied;
			new_slots[idx].id = id;
			entry_cold_[id].slot_index = static_cast<std::uint32_t>(idx);
		}

		slots_.swap(new_slots);
		tombstones_ = 0;
	}

	static std::size_t find_insert_slot_in(std::vector<slot>& slots, std::uint64_t h) {
		const std::size_t mask = slots.size() - 1;
		std::size_t idx = static_cast<std::size_t>(h) & mask;
		std::size_t first_tombstone = std::numeric_limits<std::size_t>::max();

		for (std::size_t step = 0; step < slots.size(); ++step) {
			slot& s = slots[idx];
			if (s.state == slot_empty) {
				return (first_tombstone != std::numeric_limits<std::size_t>::max()) ? first_tombstone : idx;
			}
			if (s.state == slot_tombstone && first_tombstone == std::numeric_limits<std::size_t>::max()) {
				first_tombstone = idx;
			}
			idx = (idx + 1) & mask;
		}

		return (first_tombstone != std::numeric_limits<std::size_t>::max()) ? first_tombstone : 0;
	}

	std::size_t compute_bits_blocks_per_segment() const {
		if (bits_words_ == 0) {
			return 0;
		}
		constexpr std::size_t target_segment_bytes = 2u * 1024u * 1024u; // 2 MiB
		const std::size_t target_words =
			std::max<std::size_t>(bits_words_, target_segment_bytes / sizeof(std::uint64_t));
		std::size_t blocks = target_words / bits_words_;
		if (blocks == 0) {
			blocks = 1;
		}
		if (blocks < 256) {
			blocks = 256;
		}
		if (blocks > 16384) {
			blocks = 16384;
		}
		return blocks;
	}

	void ensure_bits_segment_config() {
		if (bits_words_ == 0) {
			bits_blocks_per_segment_ = 0;
			return;
		}
		if (bits_blocks_per_segment_ == 0) {
			bits_blocks_per_segment_ = compute_bits_blocks_per_segment();
		}
	}

	void ensure_bits_segment_for_block(std::uint32_t block) {
		if (bits_words_ == 0) {
			return;
		}
		ensure_bits_segment_config();
		if (bits_blocks_per_segment_ == 0) {
			return;
		}
		const std::size_t seg_idx =
			static_cast<std::size_t>(block) / bits_blocks_per_segment_;
		const std::size_t segment_words = bits_blocks_per_segment_ * bits_words_;
		while (bits_segments_.size() <= seg_idx) {
			bits_segment seg;
			seg.words.resize(segment_words, 0);
			bits_segments_.push_back(std::move(seg));
		}
	}

	std::size_t bits_segment_index(std::uint32_t block) const {
		if (bits_blocks_per_segment_ == 0) {
			return 0;
		}
		return static_cast<std::size_t>(block) / bits_blocks_per_segment_;
	}

	std::size_t bits_segment_offset_words(std::uint32_t block) const {
		if (bits_blocks_per_segment_ == 0) {
			return 0;
		}
		const std::size_t in_segment = static_cast<std::size_t>(block) % bits_blocks_per_segment_;
		return in_segment * bits_words_;
	}

	std::uint64_t* bits_block_ptr_mut(std::uint32_t block) {
		if (bits_words_ == 0) {
			return nullptr;
		}
		ensure_bits_segment_for_block(block);
		const std::size_t seg_idx = bits_segment_index(block);
		if (seg_idx >= bits_segments_.size()) {
			return nullptr;
		}
		return bits_segments_[seg_idx].words.data() + bits_segment_offset_words(block);
	}

	const std::uint64_t* bits_block_ptr(std::uint32_t block) const {
		if (bits_words_ == 0 || bits_blocks_per_segment_ == 0) {
			return nullptr;
		}
		const std::size_t seg_idx = bits_segment_index(block);
		if (seg_idx >= bits_segments_.size()) {
			return nullptr;
		}
		return bits_segments_[seg_idx].words.data() + bits_segment_offset_words(block);
	}

	std::uint32_t allocate_bits_block(const std::vector<std::uint64_t>& bits) {
		if (bits_words_ == 0) {
			return 0;
		}
		ensure_bits_segment_config();

		std::uint32_t block = 0;
		if (!free_bits_blocks_.empty()) {
			block = free_bits_blocks_.back();
			free_bits_blocks_.pop_back();
		}
		else {
			block = next_bits_block_id_++;
		}

		std::uint64_t* dst = bits_block_ptr_mut(block);
		if (dst == nullptr) {
			return 0;
		}
		const std::size_t seg_idx = bits_segment_index(block);
		if (seg_idx < bits_segments_.size()) {
			++bits_segments_[seg_idx].live_blocks;
		}
		for (std::size_t i = 0; i < bits_words_; ++i) {
			dst[i] = bits[i];
		}
		return block;
	}

	void release_bits_block(std::uint32_t block) {
		if (bits_words_ == 0) {
			return;
		}
		if (bits_blocks_per_segment_ > 0) {
			const std::size_t seg_idx = bits_segment_index(block);
			if (seg_idx < bits_segments_.size() && bits_segments_[seg_idx].live_blocks > 0) {
				--bits_segments_[seg_idx].live_blocks;
			}
		}
		free_bits_blocks_.push_back(block);
	}

	bool bits_equal(const memo_entry& entry, const std::vector<std::uint64_t>& bits) const {
		if (bits.size() != bits_words_) {
			return false;
		}
		if (bits_words_ == 0) {
			return true;
		}
		const std::uint64_t* stored = bits_block_ptr(entry.bits_block);
		if (stored == nullptr) {
			return false;
		}
		for (std::size_t i = 0; i < bits_words_; ++i) {
			if (stored[i] != bits[i]) {
				return false;
			}
		}
		return true;
	}

	void touch_entry(entry_id id) {
		if (id >= entries_.size()) {
			return;
		}
		memo_entry& entry = entries_[id];
		if (entry.use_count < std::numeric_limits<std::int32_t>::max()) {
			++entry.use_count;
		}
	}

	entry_id find_entry(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint, bool count_stats) {
		if (entries_.empty() || slots_.empty()) {
			return invalid_entry_id();
		}
		if (bits.size() != bits_words_) {
			return invalid_entry_id();
		}

		const std::size_t mask = slots_.size() - 1;
		std::size_t idx = static_cast<std::size_t>(probe_hash(subset_hash, subset_fingerprint, time)) & mask;

		for (std::size_t step = 0; step < slots_.size(); ++step) {
			const slot& s = slots_[idx];
			if (s.state == slot_empty) {
				return invalid_entry_id();
			}
			if (s.state == slot_occupied) {
				const memo_entry& entry = entries_[s.id];
				if (entry.subset_hash != subset_hash) {
					if (count_stats) {
						++diagnostics_.hash_collisions;
					}
				}
				else if (entry.start_time != time) {
					if (count_stats) {
						++diagnostics_.hash_collisions;
					}
				}
				else if (entry.subset_fingerprint != subset_fingerprint) {
					if (count_stats) {
						++diagnostics_.hash_collisions;
						++diagnostics_.fingerprint_mismatches;
					}
				}
				else {
					if (count_stats) {
						++diagnostics_.full_key_rechecks;
					}
					if (bits_equal(entry, bits)) {
						return s.id;
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

	void insert_new_entry(const std::vector<std::uint64_t>& bits, schedule_time_t time,
		std::uint64_t subset_hash, std::uint64_t subset_fingerprint,
		bool has_exact, long long exact, long long lower_bound, int best_job,
		std::size_t estimated_bytes, bool count_stats) {
		ensure_slot_table_for_insert(entries_.size() + 1);
		if (capacity_ > 0) {
			entries_.reserve(capacity_);
		}

		memo_entry entry;
		entry.bits_block = allocate_bits_block(bits);
		entry.start_time = time;
		entry.subset_hash = subset_hash;
		entry.subset_fingerprint = subset_fingerprint;
		entry.has_exact = has_exact;
		entry.exact = exact;
		entry.lower_bound = lower_bound;
		entry.best_job = best_job;
		entry.use_count = 1;

		entries_.push_back(std::move(entry));
		const entry_id new_id = static_cast<entry_id>(entries_.size() - 1);
		entry_cold_.push_back({ static_cast<std::uint32_t>(estimated_bytes), 0U });

		const std::uint64_t h = probe_hash(subset_hash, subset_fingerprint, time);
		const std::size_t slot_idx = find_insert_slot_in(slots_, h);
		if (slots_[slot_idx].state == slot_tombstone && tombstones_ > 0) {
			--tombstones_;
		}
		slots_[slot_idx].state = slot_occupied;
		slots_[slot_idx].id = new_id;
		entry_cold_[new_id].slot_index = static_cast<std::uint32_t>(slot_idx);

		memory_.used_bytes += estimated_bytes;
		if (count_stats) {
			++stats_.inserts;
		}
		if (entries_.size() > stats_.peak_size) {
			stats_.peak_size = entries_.size();
		}
		stats_.final_size = entries_.size();
	}

	void erase_entry(entry_id id, bool count_stats) {
		if (id >= entries_.size()) {
			return;
		}

		const std::size_t removed_slot = entry_cold_[id].slot_index;
		const std::uint32_t removed_block = entries_[id].bits_block;
		const std::size_t removed_bytes = entry_cold_[id].approx_bytes;

		if (removed_slot < slots_.size() && slots_[removed_slot].state == slot_occupied) {
			slots_[removed_slot].state = slot_tombstone;
			++tombstones_;
		}

		release_bits_block(removed_block);

		const entry_id last_id = static_cast<entry_id>(entries_.size() - 1);
		if (id != last_id) {
			entries_[id] = std::move(entries_[last_id]);
			entry_cold_[id] = entry_cold_[last_id];
			const std::size_t moved_slot = entry_cold_[id].slot_index;
			if (moved_slot < slots_.size() && slots_[moved_slot].state == slot_occupied) {
				slots_[moved_slot].id = id;
			}
		}
		entries_.pop_back();
		entry_cold_.pop_back();
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
		}
		stats_.final_size = entries_.size();

		if (!slots_.empty() && tombstones_ > slots_.size() / 4) {
			rehash_slots(std::max<std::size_t>(slots_.size(), 16));
		}
	}

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

	std::size_t evict_lufo_decay_pass(bool count_stats) {
		if (entries_.empty()) {
			lufo_cursor_ = 0;
			return 0;
		}

		++stats_.lufo_decay_passes;
		if (lufo_exact_protection_) {
			lufo_exact_skip_toggle_ = !lufo_exact_skip_toggle_;
		}
		std::size_t removed = 0;
		std::size_t scanned = 0;
		const std::size_t scan_budget = lufo_decay_batch_size();
		const std::int32_t use_count_min = std::numeric_limits<std::int32_t>::min();
		while (!entries_.empty() && scanned < scan_budget) {
			if (lufo_cursor_ >= entries_.size()) {
				lufo_cursor_ = 0;
			}

			const entry_id i = static_cast<entry_id>(lufo_cursor_);
			memo_entry& entry = entries_[i];
			const bool skip_exact_decay =
				lufo_exact_protection_ && lufo_exact_skip_toggle_ && entry.has_exact;
			if (!skip_exact_decay && entry.use_count > use_count_min) {
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

	bool evict_one_by_policy(bool count_stats) {
		return evict_by_policy(count_stats) > 0;
	}

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

	std::vector<memo_entry> entries_;
	std::vector<memo_entry_cold> entry_cold_;
	std::vector<slot> slots_;
	std::size_t tombstones_ = 0;

	std::size_t bits_words_ = 0;
	std::vector<bits_segment> bits_segments_;
	std::vector<std::uint32_t> free_bits_blocks_;
	std::uint32_t next_bits_block_id_ = 0;
	std::size_t bits_blocks_per_segment_ = 0;

	std::size_t lufo_cursor_ = 0;
	std::size_t capacity_ = 0;
	bool profiling_timers_enabled_ = true;
	bool lufo_exact_protection_ = false;
	bool lufo_exact_skip_toggle_ = false;
	memo_table_stats stats_{};
	memo_memory_accounting memory_{};
	memo_diagnostics diagnostics_{};
};
