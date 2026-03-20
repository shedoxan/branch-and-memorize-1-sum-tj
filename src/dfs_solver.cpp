#include "dfs_solver.h"

#include <bit>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <random>
#include <string>

namespace {
std::string normalize_text(std::string text) {
	for (char& ch : text) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return text;
}
} // namespace

const char* to_string(decomposition_policy policy) {
	switch (policy) {
	case decomposition_policy::adaptive:
		return "adaptive";
	case decomposition_policy::lawler:
		return "lawler";
	case decomposition_policy::szwarc:
		return "szwarc";
	case decomposition_policy::both:
		return "both";
	default:
		return "unknown";
	}
}

bool parse_decomposition_policy(const std::string& text, decomposition_policy& out) {
	const std::string v = normalize_text(text);
	if (v == "adaptive" || v == "auto") {
		out = decomposition_policy::adaptive;
		return true;
	}
	if (v == "lawler") {
		out = decomposition_policy::lawler;
		return true;
	}
	if (v == "szwarc") {
		out = decomposition_policy::szwarc;
		return true;
	}
	if (v == "both") {
		out = decomposition_policy::both;
		return true;
	}
	return false;
}

dfs_solver::dfs_solver(dfs_config config)
	: config_(config), memo_(config.memo_capacity) {}

solve_result dfs_solver::solve(const instance& inst) {
	inst_ = &inst;
	n_ = static_cast<int>(inst.jobs.size());
	stats_ = {};

	memo_.clear();
	memo_.set_profiling_timers_enabled(config_.profiling.enabled);
	memo_.set_capacity(config_.memo_capacity, false);
	memo_.set_memory_budget_bytes(config_.memo_memory_budget_bytes, config_.strict_memory_cap);
	memo_.set_process_memory_gate(config_.use_process_memory_gate);
	memo_.set_lufo_exact_protection(config_.use_lufo_exact_protection);

	initialize_runtime_state(inst);

	solve_result result{};
	if (n_ == 0) {
		result.best.order.clear();
		result.best.cost = 0;
		finalize_stats_from_memo();
		result.stats = stats_;
		return result;
	}

	const auto start = std::chrono::steady_clock::now();
	const long long optimal = solve_state(0, 0, nullptr, true);
	const auto finish = std::chrono::steady_clock::now();
	stats_.elapsed_ms = std::chrono::duration<double, std::milli>(finish - start).count();

	if (config_.reconstruct_order) {
		result.best.order = reconstruct_order(optimal);
	}
	result.best.cost = static_cast<schedule_cost_t>(optimal);
	if (result.best.order.size() == static_cast<std::size_t>(n_)) {
		const schedule_cost_t reconstructed_cost = evaluate_sum_tardiness(inst, result.best.order);
		if (reconstructed_cost != static_cast<schedule_cost_t>(optimal)) {
			result.best.order.clear();
		}
	}

	finalize_stats_from_memo();
	result.stats = stats_;
	return result;
}

void dfs_solver::initialize_runtime_state(const instance& inst) {
	n_ = static_cast<int>(inst.jobs.size());
	runtime_.perm_jobs.assign(static_cast<std::size_t>(n_), -1);
	runtime_.edd_order.resize(static_cast<std::size_t>(n_));
	runtime_.lpt_order.resize(static_cast<std::size_t>(n_));
	for (int j = 0; j < n_; ++j) {
		runtime_.edd_order[static_cast<std::size_t>(j)] = j;
		runtime_.lpt_order[static_cast<std::size_t>(j)] = j;
	}
	std::sort(runtime_.edd_order.begin(), runtime_.edd_order.end(), [&](int a, int b) {
		const job& ja = inst.jobs[static_cast<std::size_t>(a)];
		const job& jb = inst.jobs[static_cast<std::size_t>(b)];
		if (ja.d != jb.d) {
			return ja.d < jb.d;
		}
		if (ja.p != jb.p) {
			return ja.p < jb.p;
		}
		return a < b;
		});
	std::sort(runtime_.lpt_order.begin(), runtime_.lpt_order.end(), [&](int a, int b) {
		const job& ja = inst.jobs[static_cast<std::size_t>(a)];
		const job& jb = inst.jobs[static_cast<std::size_t>(b)];
		if (ja.p != jb.p) {
			return ja.p > jb.p;
		}
		if (ja.d != jb.d) {
			return ja.d > jb.d;
		}
		return a > b;
		});
	stats_.ordering_sorts += 2;

	const std::size_t words = static_cast<std::size_t>((n_ + 63) / 64);
	runtime_.remaining_bits.assign(words, 0);
	for (int j = 0; j < n_; ++j) {
		const std::size_t word = static_cast<std::size_t>(j >> 6);
		const std::uint64_t bit = std::uint64_t{ 1 } << (j & 63);
		runtime_.remaining_bits[word] |= bit;
	}
	runtime_.remaining_count = n_;

	runtime_.zobrist_job.assign(static_cast<std::size_t>(n_), 0);
	runtime_.zobrist_job_fp.assign(static_cast<std::size_t>(n_), 0);
	std::mt19937_64 rng(config_.zobrist_seed);
	std::mt19937_64 rng_fp(config_.zobrist_seed ^ 0xD1B54A32D192ED03ULL);
	runtime_.subset_hash = 0;
	runtime_.subset_fingerprint = 0;
	for (int j = 0; j < n_; ++j) {
		std::uint64_t key = rng();
		if (key == 0) {
			key = 0x9E3779B97F4A7C15ULL ^ static_cast<std::uint64_t>(j + 1);
		}
		std::uint64_t fp = rng_fp();
		if (fp == 0) {
			fp = 0xC2B2AE3D27D4EB4FULL ^ (static_cast<std::uint64_t>(j + 1) << 1);
		}
		runtime_.zobrist_job[static_cast<std::size_t>(j)] = key;
		runtime_.zobrist_job_fp[static_cast<std::size_t>(j)] = fp;
		runtime_.subset_hash ^= key;
		runtime_.subset_fingerprint ^= fp;
	}

	runtime_.scratch_by_depth.clear();
	runtime_.scratch_by_depth.resize(static_cast<std::size_t>(n_ + 1));
	for (dfs_depth_scratch& scratch : runtime_.scratch_by_depth) {
		scratch.edd_jobs.reserve(static_cast<std::size_t>(n_));
		scratch.lpt_jobs.reserve(static_cast<std::size_t>(n_));
		scratch.order_cache_ready = false;
		scratch.order_cache_hash = 0;
		scratch.order_cache_fingerprint = 0;
		scratch.order_cache_count = -1;
		scratch.longest_positions.reserve(static_cast<std::size_t>(n_));
		scratch.edd_prefix_processing.reserve(static_cast<std::size_t>(n_ + 1));

		scratch.candidates_before_earliest.reserve(static_cast<std::size_t>(n_));
		scratch.szwarc_positions.reserve(static_cast<std::size_t>(n_));
		scratch.before_prefix.reserve(static_cast<std::size_t>(n_ + 1));
		scratch.tni_values.reserve(static_cast<std::size_t>(n_));
		scratch.candidate_marks.assign(static_cast<std::size_t>(n_), 0);
		scratch.candidate_mark_epoch = 1;

		scratch.tmp_b_jobs.reserve(static_cast<std::size_t>(n_));
		scratch.tmp_a_jobs.reserve(static_cast<std::size_t>(n_));

		scratch.prefix_bits.assign(words, 0);
		scratch.b_bits.assign(words, 0);
		scratch.a_bits.assign(words, 0);
		scratch.c_bits.assign(words, 0);
	}
}

void dfs_solver::finalize_stats_from_memo() {
	const memo_table_stats table_stats = memo_.stats();
	const memo_memory_accounting mem_stats = memo_.memory_accounting();
	const memo_diagnostics diags = memo_.diagnostics();

	stats_.memo_hits = table_stats.hits;
	stats_.memo_misses = table_stats.misses;
	stats_.memo_inserts = table_stats.inserts;
	stats_.memo_updates = table_stats.updates;
	stats_.memo_evictions = table_stats.evictions;
	stats_.memo_rejected_no_room = table_stats.rejected_no_room;
	stats_.memo_forced_evictions = table_stats.forced_evictions;
	stats_.memo_clean_calls = table_stats.clean_calls;
	stats_.memo_lufo_decay_passes = table_stats.lufo_decay_passes;
	stats_.memo_peak_size = table_stats.peak_size;
	stats_.memo_final_size = table_stats.final_size;

	stats_.memo_used_bytes = mem_stats.used_bytes;
	stats_.memo_budget_bytes = mem_stats.budget_bytes;
	stats_.memo_clean_time_ms = table_stats.clean_time_ms;

	stats_.duplicate_subproblem_hits = diags.duplicate_subproblem_hits;
	stats_.hash_collisions = diags.hash_collisions;
	stats_.full_key_rechecks = diags.full_key_rechecks;
}

void dfs_solver::build_jobs_in_order_for_bits(const std::vector<int>& source_order,
	const std::vector<std::uint64_t>& bits,
	int remaining_count,
	std::vector<int>& out) const {
	out.resize(static_cast<std::size_t>(remaining_count));
	std::size_t out_size = 0;
	const std::uint64_t* remaining_words = bits.data();
	for (int job_idx : source_order) {
		const std::size_t idx = static_cast<std::size_t>(job_idx);
		const std::uint64_t bit = std::uint64_t{ 1 } << (idx & 63);
		if ((remaining_words[idx >> 6] & bit) != 0) {
			out[out_size++] = job_idx;
		}
	}
	out.resize(out_size);
}

void dfs_solver::build_remaining_jobs_in_order(const std::vector<int>& global_order, std::vector<int>& out) const {
	build_jobs_in_order_for_bits(global_order, runtime_.remaining_bits, runtime_.remaining_count, out);
}

long long dfs_solver::solve_state(int depth, schedule_time_t current_time, const memo_lookup_result* known_lookup,
	bool track_stats) {
	const bool profiling_timers = track_stats && config_.profiling.enabled;
	if (track_stats) {
		++stats_.nodes;
		const std::uint64_t depth_u64 = static_cast<std::uint64_t>(depth);
		if (depth_u64 > stats_.max_depth) {
			stats_.max_depth = depth_u64;
		}
	}

	const int remaining_count = runtime_.remaining_count;
	if (remaining_count == 0 || depth >= n_) {
		if (track_stats) {
			++stats_.leaves;
		}
		return 0;
	}

	memo_lookup_result lookup = known_lookup ? *known_lookup : memo_lookup_result{};
	if (lookup.found && lookup.has_exact) {
		if (track_stats) {
			++stats_.pruned_by_memo_exact;
		}
		return lookup.exact;
	}

	if (!known_lookup) {
		lookup = query_memo(current_time, track_stats);
	}
	if (lookup.found && lookup.has_exact) {
		if (track_stats) {
			++stats_.pruned_by_memo_exact;
		}
		return lookup.exact;
	}

	auto build_remaining_jobs_timed = [&](const std::vector<int>& global_order, std::vector<int>& out) {
		const auto order_start = profiling_timers
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		build_remaining_jobs_in_order(global_order, out);
		if (profiling_timers) {
			const auto order_finish = std::chrono::steady_clock::now();
			stats_.ordering_time_ms += std::chrono::duration<double, std::milli>(order_finish - order_start).count();
		}
		if (track_stats) {
			++stats_.ordering_scans;
		}
	};

	dfs_depth_scratch& scratch = runtime_.scratch_by_depth[static_cast<std::size_t>(depth)];
	std::vector<int>& edd_jobs = scratch.edd_jobs;
	std::vector<int>& lpt_jobs = scratch.lpt_jobs;
	const bool use_cached_orders =
		scratch.order_cache_ready &&
		scratch.order_cache_hash == runtime_.subset_hash &&
		scratch.order_cache_fingerprint == runtime_.subset_fingerprint &&
		scratch.order_cache_count == remaining_count;
	if (!use_cached_orders) {
		build_remaining_jobs_timed(runtime_.edd_order, edd_jobs);
		build_remaining_jobs_timed(runtime_.lpt_order, lpt_jobs);
		scratch.order_cache_ready = true;
		scratch.order_cache_hash = runtime_.subset_hash;
		scratch.order_cache_fingerprint = runtime_.subset_fingerprint;
		scratch.order_cache_count = remaining_count;
	}

	const bool use_lower_bound_here = config_.use_lower_bounds;
	long long state_lb = 0;
	if (use_lower_bound_here) {
		const auto bound_start = profiling_timers
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		state_lb = lower_bound_additional(depth, current_time);
		if (lookup.found && lookup.lower_bound > state_lb) {
			state_lb = lookup.lower_bound;
		}
		store_lower_bound_memo(current_time, state_lb, track_stats);
		if (profiling_timers) {
			const auto bound_finish = std::chrono::steady_clock::now();
			stats_.bound_time_ms += std::chrono::duration<double, std::milli>(bound_finish - bound_start).count();
		}
	}

	int first_job = -1;
	long long best_additional = std::numeric_limits<long long>::max() / 4;
	int best_first_job = -1;
	if (use_lower_bound_here) {
		best_additional = heuristic_upper_bound_edd(depth, current_time, edd_jobs, &first_job);
		best_first_job = first_job;
	}

	if (use_lower_bound_here
		&& lookup.found
		&& !lookup.has_exact
		&& lookup.lower_bound >= best_additional) {
		if (track_stats) {
			++stats_.pruned_by_memo_lb;
		}
		store_exact_memo(current_time, best_additional, best_first_job, track_stats);
		return best_additional;
	}

	if (use_lower_bound_here && state_lb >= best_additional) {
		store_exact_memo(current_time, best_additional, best_first_job, track_stats);
		return best_additional;
	}

	const int longest_job = lpt_jobs.front();
	const int earliest_job = edd_jobs.front();
	const job* const jobs = inst_->jobs.data();
	int longest_pos_in_edd = 0;
	for (int i = 0; i < static_cast<int>(edd_jobs.size()); ++i) {
		if (edd_jobs[static_cast<std::size_t>(i)] == longest_job) {
			longest_pos_in_edd = i;
			break;
		}
	}

	int earliest_pos_in_lpt = static_cast<int>(lpt_jobs.size()) - 1;
	for (int i = 0; i < static_cast<int>(lpt_jobs.size()); ++i) {
		if (lpt_jobs[static_cast<std::size_t>(i)] == earliest_job) {
			earliest_pos_in_lpt = i;
			break;
		}
	}

	std::vector<long long>& edd_prefix_processing = scratch.edd_prefix_processing;
	edd_prefix_processing.resize(edd_jobs.size() + 1);
	edd_prefix_processing[0] = 0;
	for (int i = 0; i < static_cast<int>(edd_jobs.size()); ++i) {
		edd_prefix_processing[static_cast<std::size_t>(i + 1)] =
			edd_prefix_processing[static_cast<std::size_t>(i)] +
			static_cast<long long>(jobs[static_cast<std::size_t>(
				edd_jobs[static_cast<std::size_t>(i)])].p);
	}

	std::vector<int>& longest_positions = scratch.longest_positions;
	std::vector<long long>& tni_values = scratch.tni_values;
	longest_positions.clear();
	const bool use_article_rule4 = true;

	if (use_article_rule4
		&& config_.use_lawler_position_filter
		&& longest_pos_in_edd < static_cast<int>(edd_jobs.size())) {
		const int k = longest_pos_in_edd;
		const int m = static_cast<int>(edd_jobs.size());
		const int pivot_job = longest_job;
		const job& pivot = jobs[static_cast<std::size_t>(pivot_job)];

		long long total_tardiness_edd = 0;
		schedule_time_t completion = current_time;
		for (int j : edd_jobs) {
			const job& jj = jobs[static_cast<std::size_t>(j)];
			completion += static_cast<schedule_time_t>(jj.p);
			total_tardiness_edd += static_cast<long long>(
				tardiness(completion, jj.d));
		}

		tni_values.resize(static_cast<std::size_t>(m - k));
		tni_values[0] = total_tardiness_edd;
		schedule_time_t start_t_before_pivot =
			current_time + static_cast<schedule_time_t>(edd_prefix_processing[static_cast<std::size_t>(k)]);
		for (int rel = 1; rel < m - k; ++rel) {
			const int next_job = edd_jobs[static_cast<std::size_t>(k + rel)];
			const job& nxt = jobs[static_cast<std::size_t>(next_job)];
			long long t_next = tni_values[static_cast<std::size_t>(rel - 1)];
			const schedule_time_t c_pivot_old = start_t_before_pivot + static_cast<schedule_time_t>(pivot.p);
			const schedule_time_t c_next_old = c_pivot_old + static_cast<schedule_time_t>(nxt.p);
			const schedule_time_t c_next_new = start_t_before_pivot + static_cast<schedule_time_t>(nxt.p);
			const schedule_time_t c_pivot_new = c_next_new + static_cast<schedule_time_t>(pivot.p);
			t_next -= static_cast<long long>(
				tardiness(c_pivot_old, pivot.d));
			t_next -= static_cast<long long>(
				tardiness(c_next_old, nxt.d));
			t_next += static_cast<long long>(
				tardiness(c_next_new, nxt.d));
			t_next += static_cast<long long>(
				tardiness(c_pivot_new, pivot.d));
			tni_values[static_cast<std::size_t>(rel)] = t_next;
			start_t_before_pivot += static_cast<schedule_time_t>(nxt.p);
		}
	}

	const auto valid_positions_start = profiling_timers
		? std::chrono::steady_clock::now()
		: std::chrono::steady_clock::time_point{};
	const bool use_position_filter = config_.use_lawler_position_filter;
	const bool use_rule12 = config_.use_lawler_rule12;
	const bool use_article_rule4_reductions = use_article_rule4 && !tni_values.empty();
	if (!use_position_filter) {
		if (track_stats) {
			stats_.valid_positions_built += static_cast<std::uint64_t>(
				static_cast<int>(edd_jobs.size()) - longest_pos_in_edd);
		}
		for (int h_idx = longest_pos_in_edd; h_idx < static_cast<int>(edd_jobs.size()); ++h_idx) {
			longest_positions.push_back(h_idx);
		}
	}
	else if (use_article_rule4_reductions) {
		schedule_time_t article_rule3_max_dp = 0;
		long long article_rule4_min_tni = std::numeric_limits<long long>::max();
		for (int h_idx = longest_pos_in_edd; h_idx < static_cast<int>(edd_jobs.size()); ++h_idx) {
			if (track_stats) {
				++stats_.valid_positions_built;
			}

			const schedule_time_t completion_at_h =
				current_time + static_cast<schedule_time_t>(edd_prefix_processing[static_cast<std::size_t>(h_idx + 1)]);
			bool eliminated = false;

			if (use_rule12) {
				if (h_idx + 1 < static_cast<int>(edd_jobs.size())) {
					const int next_job = edd_jobs[static_cast<std::size_t>(h_idx + 1)];
					if (completion_at_h > static_cast<schedule_time_t>(jobs[static_cast<std::size_t>(next_job)].d)) {
						eliminated = true;
					}
				}
				if (!eliminated && h_idx > longest_pos_in_edd) {
					const job& curr = jobs[static_cast<std::size_t>(edd_jobs[static_cast<std::size_t>(h_idx)])];
					if (completion_at_h <= static_cast<schedule_time_t>(curr.d) + static_cast<schedule_time_t>(curr.p)) {
						eliminated = true;
					}
				}
			}

			const int rel = h_idx - longest_pos_in_edd;
			if (h_idx >= longest_pos_in_edd + 2) {
				const job& prev = jobs[static_cast<std::size_t>(edd_jobs[static_cast<std::size_t>(h_idx - 1)])];
				const schedule_time_t rhs =
					static_cast<schedule_time_t>(prev.d) + static_cast<schedule_time_t>(prev.p);
				if (rhs > article_rule3_max_dp) {
					article_rule3_max_dp = rhs;
				}
			}
			if (rel > 0) {
				const long long prev_tni = tni_values[static_cast<std::size_t>(rel - 1)];
				if (prev_tni < article_rule4_min_tni) {
					article_rule4_min_tni = prev_tni;
				}
			}
			if (!eliminated && h_idx >= longest_pos_in_edd + 2 && completion_at_h <= article_rule3_max_dp) {
				eliminated = true;
			}
			if (!eliminated) {
				const long long t_curr = tni_values[static_cast<std::size_t>(rel)];
				const bool dominated_by_prev = (rel > 0) && (t_curr >= article_rule4_min_tni);
				const bool dominated_by_next =
					(rel + 1 < static_cast<int>(tni_values.size())) &&
					(t_curr > tni_values[static_cast<std::size_t>(rel + 1)]);
				if (dominated_by_prev || dominated_by_next) {
					eliminated = true;
				}
			}

			if (!eliminated) {
				longest_positions.push_back(h_idx);
			}
		}
	}
	else {
		schedule_time_t property3b_prefix_max = 0;
		for (int h_idx = longest_pos_in_edd; h_idx < static_cast<int>(edd_jobs.size()); ++h_idx) {
			if (track_stats) {
				++stats_.valid_positions_built;
			}

			const schedule_time_t completion_at_h =
				current_time + static_cast<schedule_time_t>(edd_prefix_processing[static_cast<std::size_t>(h_idx + 1)]);
			bool eliminated = false;

			if (use_rule12) {
				if (h_idx + 1 < static_cast<int>(edd_jobs.size())) {
					const int next_job = edd_jobs[static_cast<std::size_t>(h_idx + 1)];
					if (completion_at_h > static_cast<schedule_time_t>(jobs[static_cast<std::size_t>(next_job)].d)) {
						eliminated = true;
					}
				}
				if (!eliminated && h_idx > longest_pos_in_edd) {
					const job& curr = jobs[static_cast<std::size_t>(edd_jobs[static_cast<std::size_t>(h_idx)])];
					if (completion_at_h <= static_cast<schedule_time_t>(curr.d) + static_cast<schedule_time_t>(curr.p)) {
						eliminated = true;
					}
				}
			}

			if (h_idx > longest_pos_in_edd) {
				const job& r = jobs[static_cast<std::size_t>(edd_jobs[static_cast<std::size_t>(h_idx)])];
				const schedule_time_t rhs =
					static_cast<schedule_time_t>(r.d) + static_cast<schedule_time_t>(r.p);
				if (rhs > property3b_prefix_max) {
					property3b_prefix_max = rhs;
				}
			}

			if (!eliminated && h_idx + 1 < static_cast<int>(edd_jobs.size())) {
				const int next_job = edd_jobs[static_cast<std::size_t>(h_idx + 1)];
				const schedule_time_t d_next = static_cast<schedule_time_t>(jobs[static_cast<std::size_t>(next_job)].d);
				if (completion_at_h >= d_next) {
					eliminated = true;
					if (track_stats) {
						++stats_.valid_positions_pruned_3a;
					}
				}
			}

			if (!eliminated && h_idx > longest_pos_in_edd && completion_at_h < property3b_prefix_max) {
				eliminated = true;
				if (track_stats) {
					++stats_.valid_positions_pruned_3b;
				}
			}

			if (!eliminated) {
				longest_positions.push_back(h_idx);
			}
		}
	}
	if (profiling_timers) {
		const auto valid_positions_finish = std::chrono::steady_clock::now();
		stats_.valid_positions_time_ms +=
			std::chrono::duration<double, std::milli>(valid_positions_finish - valid_positions_start).count();
	}
	if (longest_positions.empty()) {
		// Keep exactness: never allow an empty candidate set.
		longest_positions.push_back(longest_pos_in_edd);
	}

	const std::vector<std::uint64_t>& saved_bits = runtime_.remaining_bits;
	const std::uint64_t saved_hash = runtime_.subset_hash;
	const std::uint64_t saved_fp = runtime_.subset_fingerprint;
	const int saved_count = runtime_.remaining_count;

	auto lookup_subset_bits = [&](std::vector<std::uint64_t>& bits,
		int subset_count,
		std::uint64_t subset_hash,
		std::uint64_t subset_fingerprint,
		schedule_time_t start_time,
		bool count_stats) {
			const std::uint64_t prev_hash = runtime_.subset_hash;
			const std::uint64_t prev_fp = runtime_.subset_fingerprint;
			const int prev_count = runtime_.remaining_count;
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = subset_hash;
			runtime_.subset_fingerprint = subset_fingerprint;
			runtime_.remaining_count = subset_count;
			memo_lookup_result res = query_memo(start_time, count_stats);
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = prev_hash;
			runtime_.subset_fingerprint = prev_fp;
			runtime_.remaining_count = prev_count;
			return res;
		};

	auto subset_lower_bound_bits = [&](std::vector<std::uint64_t>& bits,
		int subset_count,
		std::uint64_t subset_hash,
		std::uint64_t subset_fingerprint,
		schedule_time_t start_time) -> long long {
			if (subset_count <= 0) {
				return 0;
			}

			const memo_lookup_result memo_lb =
				lookup_subset_bits(bits, subset_count, subset_hash, subset_fingerprint, start_time, false);
			if (memo_lb.found && memo_lb.has_exact) {
				return memo_lb.exact;
			}
			if (!config_.use_lower_bounds) {
				return 0;
			}

			const std::uint64_t prev_hash = runtime_.subset_hash;
			const std::uint64_t prev_fp = runtime_.subset_fingerprint;
			const int prev_count = runtime_.remaining_count;
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = subset_hash;
			runtime_.subset_fingerprint = subset_fingerprint;
			runtime_.remaining_count = subset_count;
			long long lb = lower_bound_additional(n_ - subset_count, start_time);
			if (memo_lb.found && memo_lb.lower_bound > lb) {
				lb = memo_lb.lower_bound;
			}
			store_lower_bound_memo(start_time, lb, false);
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = prev_hash;
			runtime_.subset_fingerprint = prev_fp;
			runtime_.remaining_count = prev_count;
			return lb;
		};

	auto solve_subset_exact_bits = [&](std::vector<std::uint64_t>& bits,
		int subset_count,
		std::uint64_t subset_hash,
		std::uint64_t subset_fingerprint,
		schedule_time_t start_time,
		int* out_first_job) -> long long {
			if (subset_count <= 0) {
				if (out_first_job != nullptr) {
					*out_first_job = -1;
				}
				return 0;
			}

			const memo_lookup_result lk = lookup_subset_bits(
				bits, subset_count, subset_hash, subset_fingerprint, start_time, track_stats);
			if (lk.found && lk.has_exact) {
				if (out_first_job != nullptr) {
					*out_first_job = lk.best_job;
				}
				return lk.exact;
			}

			const std::uint64_t prev_hash = runtime_.subset_hash;
			const std::uint64_t prev_fp = runtime_.subset_fingerprint;
			const int prev_count = runtime_.remaining_count;
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = subset_hash;
			runtime_.subset_fingerprint = subset_fingerprint;
			runtime_.remaining_count = subset_count;
			const int child_depth = n_ - subset_count;
			dfs_depth_scratch& child_scratch =
				runtime_.scratch_by_depth[static_cast<std::size_t>(child_depth)];
			build_jobs_in_order_for_bits(edd_jobs, runtime_.remaining_bits, subset_count, child_scratch.edd_jobs);
			build_jobs_in_order_for_bits(lpt_jobs, runtime_.remaining_bits, subset_count, child_scratch.lpt_jobs);
			child_scratch.order_cache_ready = true;
			child_scratch.order_cache_hash = subset_hash;
			child_scratch.order_cache_fingerprint = subset_fingerprint;
			child_scratch.order_cache_count = subset_count;
			const memo_lookup_result* known = lk.found ? &lk : nullptr;
			const long long exact = solve_state(
				n_ - subset_count,
				start_time,
				known,
				track_stats);

			const memo_lookup_result after = query_memo(start_time, false);
			if (out_first_job != nullptr) {
				if (after.found && after.has_exact) {
					*out_first_job = after.best_job;
				}
				else {
					*out_first_job = -1;
				}
			}
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = prev_hash;
			runtime_.subset_fingerprint = prev_fp;
			runtime_.remaining_count = prev_count;
			return exact;
		};

	auto evaluate_branch_bits = [&](int pivot_job,
		std::vector<std::uint64_t>& b_bits, int b_count, std::uint64_t b_hash, std::uint64_t b_fp,
		std::vector<std::uint64_t>& a_bits, int a_count, std::uint64_t a_hash, std::uint64_t a_fp,
		schedule_time_t pivot_completion, int fallback_first_in_b) {
			const long long pivot_cost = static_cast<long long>(
				tardiness(pivot_completion,
					inst_->jobs[static_cast<std::size_t>(pivot_job)].d));

			long long lb_b = 0;
			long long lb_a = 0;
			if (config_.use_lower_bounds) {
				lb_b = subset_lower_bound_bits(b_bits, b_count, b_hash, b_fp, current_time);
				lb_a = subset_lower_bound_bits(a_bits, a_count, a_hash, a_fp, pivot_completion);
				if (lb_b + pivot_cost + lb_a >= best_additional) {
					if (track_stats) {
						++stats_.pruned_by_bound;
					}
					return;
				}
			}

			int first_in_b = -1;
			const long long cost_b =
				solve_subset_exact_bits(b_bits, b_count, b_hash, b_fp, current_time, &first_in_b);
			if (config_.use_lower_bounds && cost_b + pivot_cost + lb_a >= best_additional) {
				if (track_stats) {
					++stats_.pruned_by_bound;
				}
				return;
			}

			const long long cost_a =
				solve_subset_exact_bits(a_bits, a_count, a_hash, a_fp, pivot_completion, nullptr);
			const long long total = cost_b + pivot_cost + cost_a;
			if (total < best_additional) {
				best_additional = total;
				if (b_count == 0) {
					best_first_job = pivot_job;
				}
				else if (first_in_b >= 0) {
					best_first_job = first_in_b;
				}
				else {
					best_first_job = fallback_first_in_b;
				}
			}
		};

	const int max_earliest_pos = static_cast<int>(edd_jobs.size()) - earliest_pos_in_lpt - 1;
	const bool szwarc_available = config_.use_decomposition2
		&& earliest_job != longest_job
		&& max_earliest_pos >= 0;

	bool run_double = false;
	bool run_lawler = false;
	bool run_szwarc = false;
	switch (config_.decomp_policy) {
	case decomposition_policy::lawler:
		run_lawler = true;
		break;
	case decomposition_policy::szwarc:
		run_szwarc = szwarc_available;
		break;
	case decomposition_policy::both:
		run_double = szwarc_available;
		if (!run_double) {
			run_lawler = true;
		}
		break;
	case decomposition_policy::adaptive:
	default:
		if (!szwarc_available) {
			run_lawler = true;
		}
		else {
			const int lawler_count = static_cast<int>(longest_positions.size());
			const int szwarc_count = max_earliest_pos + 1;
			if (lawler_count <= szwarc_count) {
				run_lawler = true;
			}
			else {
				run_szwarc = true;
			}
		}
		break;
	}
	if (!run_double && !run_lawler && !run_szwarc) {
		run_lawler = true;
	}

	auto edd_less = [&](int lhs_job, int rhs_job) {
		const job& lhs = inst_->jobs[static_cast<std::size_t>(lhs_job)];
		const job& rhs = inst_->jobs[static_cast<std::size_t>(rhs_job)];
		if (lhs.d != rhs.d) {
			return lhs.d < rhs.d;
		}
		if (lhs.p != rhs.p) {
			return lhs.p < rhs.p;
		}
		return lhs_job < rhs_job;
		};

		auto prepare_szwarc_positions = [&](std::vector<int>& out_positions) {
			std::vector<std::uint32_t>& candidate_marks = scratch.candidate_marks;
			std::uint32_t mark = scratch.candidate_mark_epoch + 1;
			if (mark == 0) {
				std::fill(candidate_marks.begin(), candidate_marks.end(), 0);
				mark = 1;
			}
			scratch.candidate_mark_epoch = mark;
			for (int i = earliest_pos_in_lpt + 1; i < static_cast<int>(lpt_jobs.size()); ++i) {
				candidate_marks[static_cast<std::size_t>(lpt_jobs[static_cast<std::size_t>(i)])] = mark;
			}

			std::vector<int>& candidates_before_earliest = scratch.candidates_before_earliest;
			candidates_before_earliest.clear();
			for (int j : edd_jobs) {
				if (candidate_marks[static_cast<std::size_t>(j)] == mark) {
					candidates_before_earliest.push_back(j);
				}
			}
			if (track_stats) {
				++stats_.ordering_scans;
			}

			std::vector<long long>& before_prefix = scratch.before_prefix;
			before_prefix.resize(candidates_before_earliest.size() + 1);
			before_prefix[0] = 0;
			for (int i = 0; i < static_cast<int>(candidates_before_earliest.size()); ++i) {
				before_prefix[static_cast<std::size_t>(i + 1)] =
					before_prefix[static_cast<std::size_t>(i)] +
					static_cast<long long>(inst_->jobs[static_cast<std::size_t>(
						candidates_before_earliest[static_cast<std::size_t>(i)])].p);
			}

			out_positions.clear();
			if (!config_.use_lawler_position_filter || !use_article_rule4) {
				for (int r = 0; r <= max_earliest_pos; ++r) {
					out_positions.push_back(r);
				}
				return;
			}
			

			const job& earliest = inst_->jobs[static_cast<std::size_t>(earliest_job)];
		std::vector<long long>& local_tni = scratch.tni_values;
		local_tni.resize(candidates_before_earliest.size() + 1);

		long long t_cur = 0;
		schedule_time_t c = current_time;
		c += static_cast<schedule_time_t>(earliest.p);
		t_cur += static_cast<long long>(tardiness(c, earliest.d));
		for (int j : candidates_before_earliest) {
			const job& jj = inst_->jobs[static_cast<std::size_t>(j)];
			c += static_cast<schedule_time_t>(jj.p);
			t_cur += static_cast<long long>(tardiness(c, jj.d));
		}
		local_tni[0] = t_cur;

		schedule_time_t start_t_before_pivot = current_time;
		for (int rel = 1; rel <= static_cast<int>(candidates_before_earliest.size()); ++rel) {
			const int next_job = candidates_before_earliest[static_cast<std::size_t>(rel - 1)];
			const job& nxt = inst_->jobs[static_cast<std::size_t>(next_job)];
			long long t_next = local_tni[static_cast<std::size_t>(rel - 1)];
			const schedule_time_t c_pivot_old = start_t_before_pivot + static_cast<schedule_time_t>(earliest.p);
			const schedule_time_t c_next_old = c_pivot_old + static_cast<schedule_time_t>(nxt.p);
			const schedule_time_t c_next_new = start_t_before_pivot + static_cast<schedule_time_t>(nxt.p);
			const schedule_time_t c_pivot_new = c_next_new + static_cast<schedule_time_t>(earliest.p);
			t_next -= static_cast<long long>(
				tardiness(c_pivot_old, earliest.d));
			t_next -= static_cast<long long>(
				tardiness(c_next_old, nxt.d));
			t_next += static_cast<long long>(
				tardiness(c_next_new, nxt.d));
			t_next += static_cast<long long>(
				tardiness(c_pivot_new, earliest.d));
			local_tni[static_cast<std::size_t>(rel)] = t_next;
			start_t_before_pivot += static_cast<schedule_time_t>(nxt.p);
		}

		long long min_tni = std::numeric_limits<long long>::max();
		for (int r = 0; r <= max_earliest_pos; ++r) {
			if (r >= 1) {
				const long long prev = local_tni[static_cast<std::size_t>(r - 1)];
				if (prev < min_tni) {
					min_tni = prev;
				}
			}
			const long long t_r = local_tni[static_cast<std::size_t>(r)];
			const bool dominated_by_prev = (r >= 1) && (t_r >= min_tni);
			const bool dominated_by_next =
				(r < max_earliest_pos) &&
				(t_r > local_tni[static_cast<std::size_t>(r + 1)]);
			if (!dominated_by_prev && !dominated_by_next) {
				out_positions.push_back(r);
			}
		}
		if (out_positions.empty()) {
			out_positions.push_back(0);
		}
		};

	if (run_double) {
		std::vector<int>& szwarc_positions = scratch.szwarc_positions;
		prepare_szwarc_positions(szwarc_positions);

		std::vector<std::uint64_t>& lawler_prefix_bits = scratch.prefix_bits;
		std::vector<std::uint64_t>& mid_bits = scratch.b_bits;
		std::vector<std::uint64_t>& right_bits = scratch.a_bits;
		std::vector<std::uint64_t>& left_bits = scratch.c_bits;
		std::fill(lawler_prefix_bits.begin(), lawler_prefix_bits.end(), 0);

		const std::size_t words = runtime_.remaining_bits.size();
		const std::size_t longest_idx = static_cast<std::size_t>(longest_job);
		const std::size_t earliest_idx = static_cast<std::size_t>(earliest_job);
		const std::size_t longest_word = longest_idx >> 6;
		const std::size_t earliest_word = earliest_idx >> 6;
		const std::uint64_t longest_mask = std::uint64_t{ 1 } << (longest_idx & 63);
		const std::uint64_t earliest_mask = std::uint64_t{ 1 } << (earliest_idx & 63);
		const std::uint64_t longest_hash = runtime_.zobrist_job[longest_idx];
		const std::uint64_t longest_fp = runtime_.zobrist_job_fp[longest_idx];
		const std::uint64_t earliest_hash = runtime_.zobrist_job[earliest_idx];
		const std::uint64_t earliest_fp = runtime_.zobrist_job_fp[earliest_idx];
		const job& earliest = inst_->jobs[earliest_idx];
		const job& longest = inst_->jobs[longest_idx];

		int lawler_prefix_scan = 0;
		int lawler_prefix_count = 0;
		std::uint64_t lawler_prefix_hash = 0;
		std::uint64_t lawler_prefix_fp = 0;
		bool evaluated_pair = false;

		for (int h_idx : longest_positions) {
			while (lawler_prefix_scan <= h_idx) {
				const int j = edd_jobs[static_cast<std::size_t>(lawler_prefix_scan)];
				if (j != longest_job) {
					const std::size_t j_idx = static_cast<std::size_t>(j);
					const std::size_t j_word = j_idx >> 6;
					const std::uint64_t j_mask = std::uint64_t{ 1 } << (j_idx & 63);
					lawler_prefix_bits[j_word] |= j_mask;
					lawler_prefix_hash ^= runtime_.zobrist_job[j_idx];
					lawler_prefix_fp ^= runtime_.zobrist_job_fp[j_idx];
					++lawler_prefix_count;
				}
				++lawler_prefix_scan;
			}

			for (std::size_t w = 0; w < words; ++w) {
				right_bits[w] = saved_bits[w] & ~lawler_prefix_bits[w];
			}
			right_bits[longest_word] &= ~longest_mask;
			const int right_count = saved_count - lawler_prefix_count - 1;
			const std::uint64_t right_hash = saved_hash ^ lawler_prefix_hash ^ longest_hash;
			const std::uint64_t right_fp = saved_fp ^ lawler_prefix_fp ^ longest_fp;

			const schedule_time_t longest_completion =
				current_time +
				static_cast<schedule_time_t>(edd_prefix_processing[static_cast<std::size_t>(h_idx + 1)]);
			const long long longest_cost = static_cast<long long>(
				tardiness(longest_completion, longest.d));
			if (config_.use_lower_bounds && config_.use_double_pair_lb_prune) {
				const long long right_lb = subset_lower_bound_bits(
					right_bits, right_count, right_hash, right_fp, longest_completion);
				if (longest_cost + right_lb >= best_additional) {
					if (track_stats) {
						++stats_.pruned_by_bound;
					}
					continue;
				}
			}
			const long long right_cost = solve_subset_exact_bits(
				right_bits, right_count, right_hash, right_fp, longest_completion, nullptr);

			std::fill(left_bits.begin(), left_bits.end(), 0);
			int left_prefix_size = 0;
			std::uint64_t left_hash = 0;
			std::uint64_t left_fp = 0;
			const int left_fallback_first = scratch.candidates_before_earliest.empty()
				? -1
				: scratch.candidates_before_earliest.front();

			for (int b_size : szwarc_positions) {
				if (b_size > static_cast<int>(scratch.candidates_before_earliest.size())) {
					break;
				}

				while (left_prefix_size < b_size) {
					const int j = scratch.candidates_before_earliest[static_cast<std::size_t>(left_prefix_size)];
					const std::size_t j_idx = static_cast<std::size_t>(j);
					const std::size_t j_word = j_idx >> 6;
					const std::uint64_t j_mask = std::uint64_t{ 1 } << (j_idx & 63);
					left_bits[j_word] |= j_mask;
					left_hash ^= runtime_.zobrist_job[j_idx];
					left_fp ^= runtime_.zobrist_job_fp[j_idx];
					++left_prefix_size;
				}

				if (b_size > lawler_prefix_count - 1) {
					continue;
				}

				if (h_idx < static_cast<int>(edd_jobs.size()) - 1 && b_size > 0) {
					const int left_last = scratch.candidates_before_earliest[static_cast<std::size_t>(b_size - 1)];
					const int right_first = edd_jobs[static_cast<std::size_t>(h_idx + 1)];
					if (!edd_less(left_last, right_first)) {
						break;
					}
				}

				for (std::size_t w = 0; w < words; ++w) {
					mid_bits[w] = lawler_prefix_bits[w] & ~left_bits[w];
				}
				mid_bits[earliest_word] &= ~earliest_mask;

				const int mid_count = lawler_prefix_count - b_size - 1;
				if (mid_count < 0) {
					continue;
				}
				const std::uint64_t mid_hash = lawler_prefix_hash ^ left_hash ^ earliest_hash;
				const std::uint64_t mid_fp = lawler_prefix_fp ^ left_fp ^ earliest_fp;

				const schedule_time_t earliest_completion =
					current_time +
					static_cast<schedule_time_t>(scratch.before_prefix[static_cast<std::size_t>(b_size)]) +
					static_cast<schedule_time_t>(earliest.p);
				const long long earliest_cost = static_cast<long long>(
					tardiness(earliest_completion, earliest.d));

				evaluated_pair = true;

				if (config_.use_lower_bounds && config_.use_double_pair_lb_prune) {
					const long long left_lb = subset_lower_bound_bits(
						left_bits, b_size, left_hash, left_fp, current_time);
					const long long mid_lb = subset_lower_bound_bits(
						mid_bits, mid_count, mid_hash, mid_fp, earliest_completion);
					if (left_lb + earliest_cost + mid_lb + longest_cost + right_cost >= best_additional) {
						if (track_stats) {
							++stats_.pruned_by_bound;
						}
						continue;
					}
				}

				int first_in_left = -1;
				const long long left_cost = solve_subset_exact_bits(
					left_bits, b_size, left_hash, left_fp, current_time, &first_in_left);
				if (config_.use_lower_bounds && config_.use_double_pair_lb_prune) {
					const long long mid_lb = subset_lower_bound_bits(
						mid_bits, mid_count, mid_hash, mid_fp, earliest_completion);
					if (left_cost + earliest_cost + mid_lb + longest_cost + right_cost >= best_additional) {
						if (track_stats) {
							++stats_.pruned_by_bound;
						}
						continue;
					}
				}

				const long long mid_cost = solve_subset_exact_bits(
					mid_bits, mid_count, mid_hash, mid_fp, earliest_completion, nullptr);

				const long long total =
					left_cost + earliest_cost + mid_cost + longest_cost + right_cost;
				if (total < best_additional) {
					best_additional = total;
					if (b_size == 0) {
						best_first_job = earliest_job;
					}
					else if (first_in_left >= 0) {
						best_first_job = first_in_left;
					}
					else {
						best_first_job = left_fallback_first;
					}
				}
				if (best_additional == 0) {
					break;
				}
			}
			if (best_additional == 0) {
				break;
			}
		}

		if (!evaluated_pair) {
			run_lawler = true;
			run_szwarc = true;
		}
	}

	if (run_lawler) {
		// Decomposition 1 (Lawler): branch longest job on h >= k.
		std::vector<std::uint64_t>& prefix_bits = scratch.prefix_bits;
		std::vector<std::uint64_t>& b_bits = scratch.b_bits;
		std::vector<std::uint64_t>& a_bits = scratch.a_bits;
		std::fill(prefix_bits.begin(), prefix_bits.end(), 0);

		const std::size_t words = runtime_.remaining_bits.size();
		const std::size_t pivot_idx = static_cast<std::size_t>(longest_job);
		const std::size_t pivot_word = pivot_idx >> 6;
		const std::uint64_t pivot_mask = std::uint64_t{ 1 } << (pivot_idx & 63);
		const std::uint64_t pivot_hash = runtime_.zobrist_job[pivot_idx];
		const std::uint64_t pivot_fp = runtime_.zobrist_job_fp[pivot_idx];

		int prefix_scan = 0;
		int prefix_count = 0;
		int fallback_first_in_b = -1;
		std::uint64_t prefix_hash = 0;
		std::uint64_t prefix_fp = 0;

		for (int h_idx : longest_positions) {
			while (prefix_scan <= h_idx) {
				const int j = edd_jobs[static_cast<std::size_t>(prefix_scan)];
				if (j != longest_job) {
					const std::size_t j_idx = static_cast<std::size_t>(j);
					const std::size_t j_word = j_idx >> 6;
					const std::uint64_t j_mask = std::uint64_t{ 1 } << (j_idx & 63);
					prefix_bits[j_word] |= j_mask;
					prefix_hash ^= runtime_.zobrist_job[j_idx];
					prefix_fp ^= runtime_.zobrist_job_fp[j_idx];
					++prefix_count;
					if (fallback_first_in_b < 0) {
						fallback_first_in_b = j;
					}
				}
				++prefix_scan;
			}

			for (std::size_t w = 0; w < words; ++w) {
				b_bits[w] = prefix_bits[w];
				a_bits[w] = saved_bits[w] & ~prefix_bits[w];
			}
			a_bits[pivot_word] &= ~pivot_mask;

			const int a_count = saved_count - prefix_count - 1;
			const std::uint64_t a_hash = saved_hash ^ prefix_hash ^ pivot_hash;
			const std::uint64_t a_fp = saved_fp ^ prefix_fp ^ pivot_fp;

			const schedule_time_t pivot_completion =
				current_time + static_cast<schedule_time_t>(edd_prefix_processing[static_cast<std::size_t>(h_idx + 1)]);
			evaluate_branch_bits(
				longest_job,
				b_bits, prefix_count, prefix_hash, prefix_fp,
				a_bits, a_count, a_hash, a_fp,
				pivot_completion,
				fallback_first_in_b);
		}
	}

	if (run_szwarc) {
		// Decomposition 2 (Szwarc): branch earliest due-date job.
		std::vector<int>& szwarc_positions = scratch.szwarc_positions;
		prepare_szwarc_positions(szwarc_positions);

		std::vector<int>& candidates_before_earliest = scratch.candidates_before_earliest;
		std::vector<long long>& before_prefix = scratch.before_prefix;

		std::vector<std::uint64_t>& prefix_bits = scratch.prefix_bits;
		std::vector<std::uint64_t>& b_bits = scratch.b_bits;
		std::vector<std::uint64_t>& a_bits = scratch.a_bits;
		std::fill(prefix_bits.begin(), prefix_bits.end(), 0);

		const std::size_t words = runtime_.remaining_bits.size();
		const std::size_t pivot_idx = static_cast<std::size_t>(earliest_job);
		const std::size_t pivot_word = pivot_idx >> 6;
		const std::uint64_t pivot_mask = std::uint64_t{ 1 } << (pivot_idx & 63);
		const std::uint64_t pivot_hash = runtime_.zobrist_job[pivot_idx];
		const std::uint64_t pivot_fp = runtime_.zobrist_job_fp[pivot_idx];

		int prefix_size = 0;
		std::uint64_t prefix_hash = 0;
		std::uint64_t prefix_fp = 0;
		int fallback_first_in_b = candidates_before_earliest.empty()
			? -1
			: candidates_before_earliest.front();

		for (int b_size : szwarc_positions) {
			if (b_size > static_cast<int>(candidates_before_earliest.size())) {
				break;
			}

			while (prefix_size < b_size) {
				const int j = candidates_before_earliest[static_cast<std::size_t>(prefix_size)];
				const std::size_t j_idx = static_cast<std::size_t>(j);
				const std::size_t j_word = j_idx >> 6;
				const std::uint64_t j_mask = std::uint64_t{ 1 } << (j_idx & 63);
				prefix_bits[j_word] |= j_mask;
				prefix_hash ^= runtime_.zobrist_job[j_idx];
				prefix_fp ^= runtime_.zobrist_job_fp[j_idx];
				++prefix_size;
			}

			for (std::size_t w = 0; w < words; ++w) {
				b_bits[w] = prefix_bits[w];
				a_bits[w] = saved_bits[w] & ~prefix_bits[w];
			}
			a_bits[pivot_word] &= ~pivot_mask;

			const int a_count = saved_count - b_size - 1;
			const std::uint64_t a_hash = saved_hash ^ prefix_hash ^ pivot_hash;
			const std::uint64_t a_fp = saved_fp ^ prefix_fp ^ pivot_fp;

			const schedule_time_t pivot_completion =
				current_time +
				static_cast<schedule_time_t>(before_prefix[static_cast<std::size_t>(b_size)]) +
				static_cast<schedule_time_t>(inst_->jobs[static_cast<std::size_t>(earliest_job)].p);
			evaluate_branch_bits(
				earliest_job,
				b_bits, b_size, prefix_hash, prefix_fp,
				a_bits, a_count, a_hash, a_fp,
				pivot_completion,
				fallback_first_in_b);
		}
	}

	runtime_.perm_jobs[static_cast<std::size_t>(depth)] = best_first_job;
	store_exact_memo(current_time, best_additional, best_first_job, track_stats);
	return best_additional;
}

long long dfs_solver::lower_bound_additional(int depth, schedule_time_t current_time) const {
	(void)depth;
	long long lb = 0;
	for (int job_idx = 0; job_idx < n_; ++job_idx) {
		if (!is_job_remaining(job_idx)) {
			continue;
		}
		const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
		const schedule_time_t min_completion = current_time + static_cast<schedule_time_t>(j.p);
		const schedule_time_t due = static_cast<schedule_time_t>(j.d);
		if (min_completion > due) {
			lb += static_cast<long long>(min_completion - due);
		}
	}
	return lb;
}

long long dfs_solver::heuristic_upper_bound_edd(int depth, schedule_time_t current_time,
	const std::vector<int>& jobs, int* first_job) {
	dfs_depth_scratch& scratch = runtime_.scratch_by_depth[static_cast<std::size_t>(depth)];

	long long best_cost = 0;
	int best_first = jobs.empty() ? -1 : jobs.front();

	schedule_time_t t = current_time;
	for (int job_idx : jobs) {
		t += static_cast<schedule_time_t>(inst_->jobs[static_cast<std::size_t>(job_idx)].p);
		best_cost += static_cast<long long>(
			tardiness(t, inst_->jobs[static_cast<std::size_t>(job_idx)].d));
	}

	const int m = static_cast<int>(jobs.size());
	const bool try_mdd =
		(m > 1) &&
		((m <= 96) || (depth <= 2 && m <= 512));
	if (try_mdd) {
		std::vector<int>& pool = scratch.tmp_a_jobs;
		pool = jobs;

		long long mdd_cost = 0;
		int mdd_first = -1;
		schedule_time_t t_mdd = current_time;
		while (!pool.empty()) {
			int best_pos = 0;
			schedule_time_t best_key = std::numeric_limits<schedule_time_t>::max();
			due_date_t tie_due = std::numeric_limits<due_date_t>::max();
			int tie_p = std::numeric_limits<int>::min();
			int tie_job = std::numeric_limits<int>::max();

			for (int i = 0; i < static_cast<int>(pool.size()); ++i) {
				const int job_idx = pool[static_cast<std::size_t>(i)];
				const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
				const due_date_t due_i = j.d;
				const schedule_time_t completion = t_mdd + static_cast<schedule_time_t>(j.p);
				const schedule_time_t mdd_key = (completion > static_cast<schedule_time_t>(due_i))
					? completion
					: static_cast<schedule_time_t>(due_i);

				const bool better =
					(mdd_key < best_key) ||
					(mdd_key == best_key && due_i < tie_due) ||
					(mdd_key == best_key && due_i == tie_due && j.p > tie_p) ||
					(mdd_key == best_key && due_i == tie_due && j.p == tie_p && job_idx < tie_job);
				if (better) {
					best_pos = i;
					best_key = mdd_key;
					tie_due = due_i;
					tie_p = j.p;
					tie_job = job_idx;
				}
			}

			const int chosen = pool[static_cast<std::size_t>(best_pos)];
			if (mdd_first < 0) {
				mdd_first = chosen;
			}
			const job& chosen_job = inst_->jobs[static_cast<std::size_t>(chosen)];
			t_mdd += static_cast<schedule_time_t>(chosen_job.p);
			mdd_cost += static_cast<long long>(
				tardiness(t_mdd, chosen_job.d));

			pool[static_cast<std::size_t>(best_pos)] = pool.back();
			pool.pop_back();

			if (mdd_cost >= best_cost) {
				// Early cutoff: already not improving the incumbent heuristic.
				break;
			}
		}

		if (pool.empty() && mdd_cost < best_cost) {
			best_cost = mdd_cost;
			best_first = mdd_first;
		}
	}

	const bool try_mit =
		(m > 1) &&
		((m <= 80) || (depth <= 1 && m <= 256));
	if (try_mit) {
		std::vector<int>& pool = scratch.tmp_b_jobs;
		pool = jobs;

		long long mit_cost = 0;
		int mit_first = -1;
		schedule_time_t t_mit = current_time;
		while (!pool.empty()) {
			int best_pos = 0;
			long long best_inc = std::numeric_limits<long long>::max();
			due_date_t tie_due = std::numeric_limits<due_date_t>::max();
			int tie_p = std::numeric_limits<int>::min();
			int tie_job = std::numeric_limits<int>::max();

			for (int i = 0; i < static_cast<int>(pool.size()); ++i) {
				const int job_idx = pool[static_cast<std::size_t>(i)];
				const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
				const schedule_time_t completion = t_mit + static_cast<schedule_time_t>(j.p);
				const long long inc = static_cast<long long>(
					tardiness(completion, j.d));

				const bool better =
					(inc < best_inc) ||
					(inc == best_inc && j.d < tie_due) ||
					(inc == best_inc && j.d == tie_due && j.p > tie_p) ||
					(inc == best_inc && j.d == tie_due && j.p == tie_p && job_idx < tie_job);
				if (better) {
					best_pos = i;
					best_inc = inc;
					tie_due = j.d;
					tie_p = j.p;
					tie_job = job_idx;
				}
			}

			const int chosen = pool[static_cast<std::size_t>(best_pos)];
			if (mit_first < 0) {
				mit_first = chosen;
			}
			const job& chosen_job = inst_->jobs[static_cast<std::size_t>(chosen)];
			t_mit += static_cast<schedule_time_t>(chosen_job.p);
			mit_cost += static_cast<long long>(
				tardiness(t_mit, chosen_job.d));

			pool[static_cast<std::size_t>(best_pos)] = pool.back();
			pool.pop_back();

			if (mit_cost >= best_cost) {
				break;
			}
		}

		if (pool.empty() && mit_cost < best_cost) {
			best_cost = mit_cost;
			best_first = mit_first;
		}
	}

	if (first_job != nullptr) {
		*first_job = best_first;
	}
	return best_cost;
}

std::vector<int> dfs_solver::reconstruct_order(long long optimal_cost) {
	std::vector<int> order;
	order.reserve(static_cast<std::size_t>(n_));

	schedule_time_t current_time = 0;
	long long remaining_optimal = optimal_cost;

	for (int depth = 0; depth < n_; ++depth) {
		int best_job = -1;
		long long best_total = std::numeric_limits<long long>::max();
		schedule_time_t best_completion = current_time;
		long long best_incremental = 0;

		for (int job_idx = 0; job_idx < n_; ++job_idx) {
			if (!is_job_remaining(job_idx)) {
				continue;
			}

			const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
			const schedule_time_t completion = current_time + static_cast<schedule_time_t>(j.p);
			const long long incremental = static_cast<long long>(
				tardiness(completion, j.d));
			if (incremental > remaining_optimal) {
				continue;
			}

			remove_job_from_state(job_idx);
			memo_lookup_result lookup = query_memo(completion, false);
			long long rest = 0;
			if (lookup.found && lookup.has_exact) {
				rest = lookup.exact;
			}
			else {
				const memo_lookup_result* known = lookup.found ? &lookup : nullptr;
				rest = solve_state(depth + 1, completion, known, false);
			}
			restore_job_to_state(job_idx);

			const long long total = incremental + rest;
			if (total < best_total || (total == best_total && job_idx < best_job)) {
				best_total = total;
				best_job = job_idx;
				best_completion = completion;
				best_incremental = incremental;
			}
		}

		if (best_job < 0 || !is_job_remaining(best_job) || best_total > remaining_optimal) {
			break;
		}

		order.push_back(best_job);
		remove_job_from_state(best_job);
		current_time = best_completion;
		remaining_optimal -= best_incremental;
	}

	for (auto it = order.rbegin(); it != order.rend(); ++it) {
		restore_job_to_state(*it);
	}
	return order;
}

memo_lookup_result dfs_solver::query_memo(schedule_time_t current_time, bool track_stats) {
	if (!track_stats || !config_.profiling.enabled) {
		return memo_.lookup(runtime_.remaining_bits, current_time,
			runtime_.subset_hash, runtime_.subset_fingerprint, track_stats);
	}

	const auto start = std::chrono::steady_clock::now();
	memo_lookup_result res = memo_.lookup(runtime_.remaining_bits, current_time,
		runtime_.subset_hash, runtime_.subset_fingerprint, true);
	const auto finish = std::chrono::steady_clock::now();
	stats_.memo_lookup_time_ms += std::chrono::duration<double, std::milli>(finish - start).count();
	return res;
}

void dfs_solver::store_exact_memo(schedule_time_t current_time, long long exact, int best_job, bool track_stats) {
	if (!config_.profiling.enabled) {
		memo_.store_exact(runtime_.remaining_bits, current_time,
			runtime_.subset_hash, runtime_.subset_fingerprint, exact, best_job, track_stats);
		return;
	}

	const auto start = std::chrono::steady_clock::now();
	memo_.store_exact(runtime_.remaining_bits, current_time,
		runtime_.subset_hash, runtime_.subset_fingerprint, exact, best_job, track_stats);
	const auto finish = std::chrono::steady_clock::now();
	stats_.memo_store_time_ms += std::chrono::duration<double, std::milli>(finish - start).count();
}

void dfs_solver::store_lower_bound_memo(schedule_time_t current_time, long long lower_bound, bool track_stats) {
	if (!config_.profiling.enabled) {
		memo_.store_lower_bound(runtime_.remaining_bits, current_time,
			runtime_.subset_hash, runtime_.subset_fingerprint, lower_bound, track_stats);
		return;
	}

	const auto start = std::chrono::steady_clock::now();
	memo_.store_lower_bound(runtime_.remaining_bits, current_time,
		runtime_.subset_hash, runtime_.subset_fingerprint, lower_bound, track_stats);
	const auto finish = std::chrono::steady_clock::now();
	stats_.memo_store_time_ms += std::chrono::duration<double, std::milli>(finish - start).count();
}

void dfs_solver::remove_job_from_state(int job_idx) {
	if (job_idx < 0 || job_idx >= n_) {
		return;
	}
	const std::size_t idx = static_cast<std::size_t>(job_idx);
	const std::size_t word = idx >> 6;
	const std::uint64_t bit = std::uint64_t{ 1 } << (idx & 63);
	if ((runtime_.remaining_bits[word] & bit) == 0) {
		return;
	}
	runtime_.remaining_bits[word] &= ~bit;
	runtime_.subset_hash ^= runtime_.zobrist_job[idx];
	runtime_.subset_fingerprint ^= runtime_.zobrist_job_fp[idx];
	--runtime_.remaining_count;
}

void dfs_solver::restore_job_to_state(int job_idx) {
	if (job_idx < 0 || job_idx >= n_) {
		return;
	}
	const std::size_t idx = static_cast<std::size_t>(job_idx);
	const std::size_t word = idx >> 6;
	const std::uint64_t bit = std::uint64_t{ 1 } << (idx & 63);
	if ((runtime_.remaining_bits[word] & bit) != 0) {
		return;
	}
	runtime_.remaining_bits[word] |= bit;
	runtime_.subset_hash ^= runtime_.zobrist_job[idx];
	runtime_.subset_fingerprint ^= runtime_.zobrist_job_fp[idx];
	++runtime_.remaining_count;
}

bool dfs_solver::is_job_remaining(int job_idx) const {
	if (job_idx < 0 || job_idx >= n_) {
		return false;
	}
	const std::size_t idx = static_cast<std::size_t>(job_idx);
	const std::size_t word = idx >> 6;
	const std::uint64_t bit = std::uint64_t{ 1 } << (idx & 63);
	return (runtime_.remaining_bits[word] & bit) != 0;
}
