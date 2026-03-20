#pragma once

#include <cstdint>
#include <vector>

#include "memo.h"
#include "solver.h"

struct dfs_depth_scratch {
	std::vector<int> edd_jobs;
	std::vector<int> lpt_jobs;
	bool order_cache_ready = false;
	std::uint64_t order_cache_hash = 0;
	std::uint64_t order_cache_fingerprint = 0;
	int order_cache_count = -1;

	std::vector<int> longest_positions;
	std::vector<long long> edd_prefix_processing;

	std::vector<int> candidates_before_earliest;
	std::vector<int> szwarc_positions;
	std::vector<long long> before_prefix;
	std::vector<long long> tni_values;
	std::vector<std::uint32_t> candidate_marks;
	std::uint32_t candidate_mark_epoch = 1;

	std::vector<int> tmp_b_jobs;
	std::vector<int> tmp_a_jobs;

	std::vector<std::uint64_t> prefix_bits;
	std::vector<std::uint64_t> b_bits;
	std::vector<std::uint64_t> a_bits;
	std::vector<std::uint64_t> c_bits;
};

struct solver_runtime_state {
	std::vector<int> perm_jobs;
	std::vector<std::uint64_t> remaining_bits;
	std::vector<std::uint64_t> zobrist_job;
	std::vector<std::uint64_t> zobrist_job_fp;
	std::vector<int> edd_order;
	std::vector<int> lpt_order;
	std::uint64_t subset_hash = 0;
	std::uint64_t subset_fingerprint = 0;
	int remaining_count = 0;
	std::vector<dfs_depth_scratch> scratch_by_depth;
};

class dfs_solver final : public solver {
public:
	explicit dfs_solver(dfs_config config = {});
	solve_result solve(const instance& inst) override;

private:
	void initialize_runtime_state(const instance& inst);
	void finalize_stats_from_memo();
	void build_jobs_in_order_for_bits(const std::vector<int>& source_order,
		const std::vector<std::uint64_t>& bits,
		int remaining_count,
		std::vector<int>& out) const;
	void build_remaining_jobs_in_order(const std::vector<int>& global_order, std::vector<int>& out) const;

	long long solve_state(int depth, schedule_time_t current_time, const memo_lookup_result* known_lookup, bool track_stats);

	long long lower_bound_additional(int depth, schedule_time_t current_time) const;
	long long heuristic_upper_bound_edd(int depth, schedule_time_t current_time,
		const std::vector<int>& jobs, int* first_job);
	std::vector<int> reconstruct_order(long long optimal_cost);

	memo_lookup_result query_memo(schedule_time_t current_time, bool track_stats);
	void store_exact_memo(schedule_time_t current_time, long long exact, int best_job, bool track_stats);
	void store_lower_bound_memo(schedule_time_t current_time, long long lower_bound, bool track_stats);

	void remove_job_from_state(int job_idx);
	void restore_job_to_state(int job_idx);
	bool is_job_remaining(int job_idx) const;

	dfs_config config_;
	const instance* inst_ = nullptr;
	int n_ = 0;

	solver_runtime_state runtime_{};
	memo_table memo_;
	solver_stats stats_{};
};
