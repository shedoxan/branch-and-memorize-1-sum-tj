#pragma once

#include <cstddef>
#include <cstdint>

struct solver_stats {
	std::uint64_t nodes = 0;
	std::uint64_t leaves = 0;
	std::uint64_t max_depth = 0;
	std::uint64_t pruned_by_bound = 0;
	std::uint64_t pruned_by_memo_exact = 0;
	std::uint64_t pruned_by_memo_lb = 0;

	std::uint64_t ordering_scans = 0;
	std::uint64_t ordering_sorts = 0;
	std::uint64_t valid_positions_built = 0;
	std::uint64_t valid_positions_pruned_3a = 0;
	std::uint64_t valid_positions_pruned_3b = 0;

	std::uint64_t memo_hits = 0;
	std::uint64_t memo_misses = 0;
	std::uint64_t memo_inserts = 0;
	std::uint64_t memo_updates = 0;
	std::uint64_t memo_evictions = 0;
	std::uint64_t memo_rejected_no_room = 0;
	std::uint64_t memo_forced_evictions = 0;
	std::uint64_t memo_clean_calls = 0;
	std::uint64_t memo_lufo_decay_passes = 0;

	std::uint64_t duplicate_subproblem_hits = 0;
	std::uint64_t hash_collisions = 0;
	std::uint64_t full_key_rechecks = 0;

	std::size_t memo_peak_size = 0;
	std::size_t memo_final_size = 0;
	std::size_t memo_used_bytes = 0;
	std::size_t memo_budget_bytes = 0;

	double elapsed_ms = 0.0;
	double bound_time_ms = 0.0;
	double ordering_time_ms = 0.0;
	double valid_positions_time_ms = 0.0;
	double memo_lookup_time_ms = 0.0;
	double memo_store_time_ms = 0.0;
	double memo_clean_time_ms = 0.0;
};
