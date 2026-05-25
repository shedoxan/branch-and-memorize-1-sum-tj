#include "dfs_solver.h"

#include <bit>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cctype>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace {
std::string normalize_text(std::string text) {
	for (char& ch : text) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return text;
}

bool any_terminal_rule_enabled(const terminal_rules_config& terminal_rules) {
	return terminal_rules.enable_all_tardy_spt ||
		terminal_rules.enable_edd_at_most_one_tardy;
}

bool any_lb_enabled(const dfs_config& config) {
	return config.bounds.enable_simple_lb ||
		(config.memo.enable_memo &&
			(config.bounds.enable_lb_memo || config.memo.enable_lb_memo));
}

bool any_ub_enabled(const bounds_config& bounds) {
	return bounds.enable_edd_ub;
}

bool depth_allowed(int depth, int limit) {
	return limit < 0 || depth <= limit;
}

bool position_filtering_enabled(const dfs_config& config) {
	return config.position_filtering.enabled;
}

bool lawler_basic_position_filter_enabled(const dfs_config& config) {
	return position_filtering_enabled(config) &&
		config.position_filtering.enable_lawler_basic_rules;
}

bool lawler_rule4_position_filter_enabled(const dfs_config& config) {
	return position_filtering_enabled(config) &&
		config.position_filtering.enable_rule4;
}

bool szwarc_rule4_position_filter_enabled(const dfs_config& config) {
	return position_filtering_enabled(config) &&
		config.position_filtering.enable_rule4;
}

void validate_solver_config(const dfs_config& config) {
	if (!config.memo.full_key_verification) {
		throw std::runtime_error("Memo without full-key verification is disabled in the exact course build");
	}
	(void)config;
}
} // namespace

const char* to_string(DecompositionMode mode) {
	switch (mode) {
	case DecompositionMode::Adaptive:
		return "adaptive";
	case DecompositionMode::Lawler:
		return "lawler";
	case DecompositionMode::Szwarc:
		return "szwarc";
	case DecompositionMode::BothLawlerSzwarc:
		return "both";
	default:
		return "unknown";
	}
}

bool parse_decomposition_mode(const std::string& text, DecompositionMode& out) {
	const std::string v = normalize_text(text);
	if (v == "adaptive") {
		out = DecompositionMode::Adaptive;
		return true;
	}
	if (v == "lawler") {
		out = DecompositionMode::Lawler;
		return true;
	}
	if (v == "szwarc") {
		out = DecompositionMode::Szwarc;
		return true;
	}
	if (v == "both") {
		out = DecompositionMode::BothLawlerSzwarc;
		return true;
	}
	return false;
}

const char* to_string(memo_backend_kind backend) {
	switch (backend) {
	case memo_backend_kind::custom:
		return "custom";
	case memo_backend_kind::std_unordered:
		return "std_unordered";
	default:
		return "unknown";
	}
}

bool parse_memo_backend_kind(const std::string& text, memo_backend_kind& out) {
	const std::string v = normalize_text(text);
	if (v == "custom" || v == "current") {
		out = memo_backend_kind::custom;
		return true;
	}
	if (v == "std_unordered" || v == "std-unordered" || v == "unordered" || v == "reference") {
		out = memo_backend_kind::std_unordered;
		return true;
	}
	return false;
}

const char* to_string(adaptive_policy_kind policy) {
	switch (policy) {
	case adaptive_policy_kind::v1:
		return "v1";
	case adaptive_policy_kind::v2:
		return "v2";
	case adaptive_policy_kind::v3:
		return "v3";
	default:
		return "unknown";
	}
}

bool parse_adaptive_policy_kind(const std::string& text, adaptive_policy_kind& out) {
	const std::string v = normalize_text(text);
	if (v == "v1") {
		out = adaptive_policy_kind::v1;
		return true;
	}
	if (v == "v2") {
		out = adaptive_policy_kind::v2;
		return true;
	}
	if (v == "v3") {
		out = adaptive_policy_kind::v3;
		return true;
	}
	return false;
}

dfs_solver::dfs_solver(dfs_config config)
	: config_(config), memo_(config.memo.backend, config.memo.capacity) {}

solve_result dfs_solver::solve(const instance& inst) {
	validate_solver_config(config_);

	inst_ = &inst;
	n_ = static_cast<int>(inst.jobs.size());
	stats_ = {};
	stats_.adaptive_policy_used = static_cast<std::uint64_t>(config_.adaptive_policy);

	memo_.clear();
	memo_.set_profiling_timers_enabled(config_.profiling.enabled);
	// Если нужен порядок, важно сохранить цепочку exact-записей с best_job.
	// Поэтому ограничение по числу memo entries отключается только для reconstruction;
	// memory budget, если он задан, продолжает действовать.
	const std::size_t effective_memo_capacity =
		config_.reconstruct_order ? 0 : config_.memo.capacity;
	memo_.set_capacity(effective_memo_capacity, false);
	memo_.set_memory_budget_bytes(config_.memo.memory_limit_bytes, config_.memo.strict_memory_cap);
	memo_.set_process_memory_gate(config_.memo.use_process_memory_gate);

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
	solve_start_ = start;
	has_time_limit_ = config_.time_limit_sec > 0.0;
	time_check_counter_ = 1023;
	if (has_time_limit_) {
		const auto limit = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<double>(config_.time_limit_sec));
		deadline_ = solve_start_ + limit;
	}
	// Корневая подзадача: S = N, t = 0.
	const long long optimal = solve_state(0, 0, nullptr, true);
	const auto finish = std::chrono::steady_clock::now();
	stats_.elapsed_ms = std::chrono::duration<double, std::milli>(finish - start).count();

	if (config_.reconstruct_order) {
		result.best.order = reconstruct_order(optimal);
	}
	result.best.cost = static_cast<schedule_cost_t>(optimal);
	bool reconstruction_ok = false;
	if (result.best.order.size() == static_cast<std::size_t>(n_)) {
		const schedule_cost_t reconstructed_cost = evaluate_sum_tardiness(inst, result.best.order);
		reconstruction_ok = reconstructed_cost == static_cast<schedule_cost_t>(optimal);
	}
	if (config_.reconstruct_order && !reconstruction_ok) {
		result.best.order.clear();
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
	// EDD(S): порядок неубывания d_j; tie-break по p_j нужен для теоремы Lawler.
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
	// LPT используется для выбора [q]_S с максимальным p_j и для списка кандидатов Decomposition II.
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

	// S в подзадаче (S,t): единичный бит означает, что работа еще не упорядочена.
	const std::size_t words = static_cast<std::size_t>((n_ + 63) / 64);
	runtime_.remaining_bits.assign(words, 0);
	for (int j = 0; j < n_; ++j) {
		const std::size_t word = static_cast<std::size_t>(j >> 6);
		const std::uint64_t bit = std::uint64_t{ 1 } << (j & 63);
		runtime_.remaining_bits[word] |= bit;
	}
	runtime_.remaining_count = n_;

	// Zobrist hash/fingerprint для множества S; полный bitset хранится для проверки коллизий.
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
	runtime_.estimated_branches_by_depth.assign(static_cast<std::size_t>(n_ + 1), 0);
	for (int depth = 0; depth <= n_; ++depth) {
		dfs_depth_scratch& scratch = runtime_.scratch_by_depth[static_cast<std::size_t>(depth)];
		const std::size_t max_remaining_here = static_cast<std::size_t>(n_ - depth);
		scratch.edd_jobs.reserve(max_remaining_here);
		scratch.lpt_jobs.reserve(max_remaining_here);
		scratch.order_cache_ready = false;
		scratch.order_cache_hash = 0;
		scratch.order_cache_fingerprint = 0;
		scratch.order_cache_count = -1;
		scratch.lawler_r_positions.reserve(max_remaining_here);
		scratch.edd_prefix_p.reserve(max_remaining_here + 1);

		scratch.candidates_before_earliest.reserve(max_remaining_here);
		scratch.szwarc_positions.reserve(max_remaining_here);
		scratch.before_prefix.reserve(max_remaining_here + 1);
		scratch.trial_tardiness_values.reserve(max_remaining_here);
		scratch.candidate_marks.assign(static_cast<std::size_t>(n_), 0);
		scratch.candidate_mark_epoch = 1;


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
	stats_.memo_exact_stores = table_stats.exact_stores;
	stats_.memo_lb_stores = table_stats.lb_stores;
	stats_.memo_stores_exact = stats_.memo_exact_stores;
	stats_.memo_stores_lb = stats_.memo_lb_stores;
	stats_.memo_updates = table_stats.updates;
	stats_.memo_evictions = table_stats.evictions;
	stats_.memo_evictions_exact = table_stats.evictions_exact;
	stats_.memo_evictions_lb = table_stats.evictions_lb;
	stats_.memo_rejected_no_room = table_stats.rejected_no_room;
	stats_.memo_forced_evictions = table_stats.forced_evictions;
	stats_.memo_clean_calls = table_stats.clean_calls;
	stats_.memo_lufo_decay_passes = table_stats.lufo_decay_passes;
	stats_.memo_peak_size = table_stats.peak_size;
	stats_.memo_final_size = table_stats.final_size;

	stats_.memo_used_bytes = mem_stats.used_bytes;
	stats_.memo_memory_used_bytes = mem_stats.used_bytes;
	stats_.memo_budget_bytes = mem_stats.budget_bytes;
	stats_.memo_clean_time_ms = table_stats.clean_time_ms;
	stats_.cleanup_time_ms = stats_.memo_clean_time_ms;

	stats_.duplicate_subproblem_hits = diags.duplicate_subproblem_hits;
	stats_.hash_collisions = diags.hash_collisions;
	stats_.full_key_rechecks = diags.full_key_rechecks;
	stats_.reconstruction_trace_entries = table_stats.reconstruction_trace_entries;
}

void dfs_solver::check_time_limit() {
	if (!has_time_limit_) {
		return;
	}
	if ((++time_check_counter_ & 1023ULL) != 0ULL) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (now < deadline_) {
		return;
	}

	stats_.elapsed_ms = std::chrono::duration<double, std::milli>(now - solve_start_).count();
	finalize_stats_from_memo();
	throw solver_time_limit_exceeded(stats_);
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

/// Рекурсивное ядро Branch-and-Memorize.
///
/// Инвариант входа: runtime_.remaining_bits задаёт текущее множество S,
/// current_time задаёт t, а depth равен длине уже зафиксированного префикса расписания.
/// Возвращаемое значение — OPT(S,t), то есть минимальная дополнительная сумма T_j
/// для работ из S при старте в момент t. Ключ exact memo обязан включать t:
/// одно и то же S при разных t даёт разные C_j и, следовательно, разные T_j.
///
/// Общая схема: lookup M[(S,t)] -> terminal/bounds -> построение ветвей ->
/// рекурсивное решение подмножеств -> store exact M[(S,t)].
long long dfs_solver::solve_state(int depth, schedule_time_t current_time, const memo_lookup_result* known_lookup,
	bool track_stats) {
	// current_time -- это t в подзадаче OPT(S,t), S задано runtime_.remaining_bits.
	const bool profiling_timers = track_stats && config_.profiling.enabled;
	if (track_stats) {
		++stats_.nodes;
		++stats_.recursive_calls;
		const std::uint64_t depth_u64 = static_cast<std::uint64_t>(depth);
		if (depth_u64 > stats_.max_depth) {
			stats_.max_depth = depth_u64;
		}
	}
	check_time_limit();

	const int remaining_count = runtime_.remaining_count;
	if (remaining_count == 0 || depth >= n_) {
		if (track_stats) {
			++stats_.leaves;
		}
		return 0;
	}

	// Solution memorization: exact-запись M[(S,t)] полностью закрывает подзадачу.
	// LB-запись здесь не достаточна: она даёт только нижнюю оценку, а не OPT(S,t).
	memo_lookup_result lookup = known_lookup ? *known_lookup : memo_lookup_result{};
	if (lookup.found && lookup.has_exact) {
		if (track_stats) {
			++stats_.pruned_by_memo_exact;
			++stats_.memo_exact_hits;
		}
		return lookup.exact;
	}

	if (!known_lookup) {
		lookup = query_memo(current_time, track_stats);
	}
	if (lookup.found && lookup.has_exact) {
		if (track_stats) {
			++stats_.pruned_by_memo_exact;
			++stats_.memo_exact_hits;
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

	if (any_terminal_rule_enabled(config_.terminal_rules)) {
		// Terminal rules дают точное значение OPT(S,t) без дальнейшего ветвления.
		const auto terminal_start = profiling_timers
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		long long terminal_exact = 0;
		int terminal_first = -1;
		std::uint64_t* terminal_counter = nullptr;
		if (try_terminal_rules(depth, current_time, edd_jobs, lpt_jobs,
			terminal_exact, terminal_first, terminal_counter)) {
			if (track_stats && terminal_counter != nullptr) {
				++(*terminal_counter);
			}
			if (profiling_timers) {
				const auto terminal_finish = std::chrono::steady_clock::now();
				stats_.terminal_time_ms += std::chrono::duration<double, std::milli>(terminal_finish - terminal_start).count();
			}
			store_exact_memo(current_time, terminal_exact, terminal_first, track_stats);
			return terminal_exact;
		}
		if (profiling_timers) {
			const auto terminal_finish = std::chrono::steady_clock::now();
			stats_.terminal_time_ms += std::chrono::duration<double, std::milli>(terminal_finish - terminal_start).count();
		}
	}

	const bool lb_depth_ok = depth_allowed(depth, config_.bounds.lb_depth_limit);
	const bool ub_depth_ok = depth_allowed(depth, config_.bounds.ub_depth_limit);
	const bool use_simple_lb_here = lb_depth_ok && config_.bounds.enable_simple_lb;
	const bool use_lb_memo_here = lb_depth_ok && config_.memo.enable_memo &&
		(config_.memo.enable_lb_memo || config_.bounds.enable_lb_memo);
	const bool use_lower_bound_here = use_simple_lb_here || use_lb_memo_here;
	const bool use_upper_bound_here = ub_depth_ok && any_ub_enabled(config_.bounds);
	long long lower_bound_LB = 0;
	if (use_lower_bound_here) {
		// LB(S,t) обязана быть admissible: LB <= OPT(S,t). Иначе можно ошибочно
		// отсечь оптимальную ветвь. Текущая LB дешёвая и не является полной LB1/LB2 из конспекта.
		const auto bound_start = profiling_timers
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		if (use_simple_lb_here) {
			if (track_stats) {
				++stats_.simple_lb_calls;
			}
			lower_bound_LB = lower_bound_additional(depth, current_time);
		}
		if (use_lb_memo_here && lookup.found && lookup.lower_bound > lower_bound_LB) {
			lower_bound_LB = lookup.lower_bound;
			if (track_stats) {
				++stats_.memo_lb_hits;
			}
		}
		if (use_simple_lb_here) {
			store_lower_bound_memo(current_time, lower_bound_LB, track_stats);
		}
		if (profiling_timers) {
			const auto bound_finish = std::chrono::steady_clock::now();
			stats_.bound_time_ms += std::chrono::duration<double, std::milli>(bound_finish - bound_start).count();
		}
	}

	int first_job = -1;
	long long upper_bound_UB = std::numeric_limits<long long>::max() / 4;
	int best_first_job = -1;
	if (use_upper_bound_here) {
		// UB строится из допустимого расписания-продолжения, поэтому UB >= OPT(S,t).
		// Такой incumbent безопасен: он только помогает отсекать ветви с LB >= UB.
		const auto ub_start = profiling_timers
			? std::chrono::steady_clock::now()
			: std::chrono::steady_clock::time_point{};
		upper_bound_UB = heuristic_upper_bound_edd(depth, current_time, edd_jobs, &first_job, track_stats);
		if (track_stats) {
			++stats_.ub_calls;
			if (upper_bound_UB < std::numeric_limits<long long>::max() / 4) {
				++stats_.ub_improvements;
			}
		}
		best_first_job = first_job;
		if (profiling_timers) {
			const auto ub_finish = std::chrono::steady_clock::now();
			stats_.upper_bound_time_ms += std::chrono::duration<double, std::milli>(ub_finish - ub_start).count();
		}
	}

	if (use_lb_memo_here
		&& lookup.found
		&& !lookup.has_exact
		&& lookup.lower_bound >= upper_bound_UB) {
		if (track_stats) {
			++stats_.pruned_by_memo_lb;
			++stats_.branches_pruned;
			++stats_.lb_prunes;
		}
		store_exact_memo(current_time, upper_bound_UB, best_first_job, track_stats);
		return upper_bound_UB;
	}

	if (use_lower_bound_here && lower_bound_LB >= upper_bound_UB) {
		if (track_stats) {
			++stats_.branches_pruned;
			++stats_.lb_prunes;
			if (use_simple_lb_here) {
				++stats_.simple_lb_prunes;
			}
		}
		store_exact_memo(current_time, upper_bound_UB, best_first_job, track_stats);
		return upper_bound_UB;
	}

	// Decomposition I (Lawler) выбирает [q]_S: работу с максимальным p_j в локальном S.
	// Decomposition II (Szwarc) выбирает [1]_S: первую работу в EDD(S), то есть min d_j.
	const int longest_job = lpt_jobs.front();
	const int earliest_job = edd_jobs.front();
	const job* const jobs = inst_->jobs.data();
	// q -- позиция выбранной самой длинной работы [q]_S в локальном EDD(S).
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

	// P_r = sum_{h=1}^r p_[h]_S для локального EDD(S), с нулевой базой индексов.
	std::vector<long long>& edd_prefix_p = scratch.edd_prefix_p;
	edd_prefix_p.resize(edd_jobs.size() + 1);
	edd_prefix_p[0] = 0;
	for (int i = 0; i < static_cast<int>(edd_jobs.size()); ++i) {
		edd_prefix_p[static_cast<std::size_t>(i + 1)] =
			edd_prefix_p[static_cast<std::size_t>(i)] +
			static_cast<long long>(jobs[static_cast<std::size_t>(
				edd_jobs[static_cast<std::size_t>(i)])].p);
	}

	std::vector<int>& lawler_r_positions = scratch.lawler_r_positions;
	std::vector<long long>& trial_tardiness_values = scratch.trial_tardiness_values;
	lawler_r_positions.clear();
	trial_tardiness_values.clear();
	const bool use_lawler_rule4 = lawler_rule4_position_filter_enabled(config_);

	if (use_lawler_rule4
		&& longest_pos_in_edd < static_cast<int>(edd_jobs.size())) {
		// Rule 4 сравнивает значения T(π^(r)) для пробных EDD-последовательностей.
		// Фильтр отключается флагом, поэтому его вклад можно сравнивать отдельно от базовой декомпозиции.
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

		trial_tardiness_values.resize(static_cast<std::size_t>(m - k));
		trial_tardiness_values[0] = total_tardiness_edd;
		schedule_time_t start_t_before_pivot =
			current_time + static_cast<schedule_time_t>(edd_prefix_p[static_cast<std::size_t>(k)]);
		for (int rel = 1; rel < m - k; ++rel) {
			const int next_job = edd_jobs[static_cast<std::size_t>(k + rel)];
			const job& nxt = jobs[static_cast<std::size_t>(next_job)];
			long long t_next = trial_tardiness_values[static_cast<std::size_t>(rel - 1)];
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
			trial_tardiness_values[static_cast<std::size_t>(rel)] = t_next;
			start_t_before_pivot += static_cast<schedule_time_t>(nxt.p);
		}
	}

	const auto valid_positions_start = profiling_timers
		? std::chrono::steady_clock::now()
		: std::chrono::steady_clock::time_point{};
	const bool use_lawler_basic_rules = lawler_basic_position_filter_enabled(config_);
	const bool use_rule4_reductions = use_lawler_rule4 && !trial_tardiness_values.empty();
	std::uint64_t lawler_positions_before = 0;
	// Position filtering уменьшает branching factor: вместо всех r >= q
	// оставляем только позиции K(S), которые не запрещены правилами отсечения.
	// lawler_r_positions хранит допустимые позиции r из K(S); h_idx = r - 1.
	if (!use_lawler_basic_rules && !use_rule4_reductions) {
		if (track_stats) {
			lawler_positions_before = static_cast<std::uint64_t>(
				static_cast<int>(edd_jobs.size()) - longest_pos_in_edd);
			stats_.valid_positions_built += lawler_positions_before;
		}
		for (int h_idx = longest_pos_in_edd; h_idx < static_cast<int>(edd_jobs.size()); ++h_idx) {
			lawler_r_positions.push_back(h_idx);
		}
	}
	else if (use_rule4_reductions) {
		// Здесь применяется Rule 4 для позиций Lawler.
		// Все оставленные позиции r затем ветвятся обычной Decomposition I, поэтому смысл OPT(S,t) не меняется.
		schedule_time_t rule3_max_dp = 0;
		long long rule4_min_trial_tardiness = std::numeric_limits<long long>::max();
		for (int h_idx = longest_pos_in_edd; h_idx < static_cast<int>(edd_jobs.size()); ++h_idx) {
			if (track_stats) {
				++stats_.valid_positions_built;
				++lawler_positions_before;
			}

			const schedule_time_t completion_at_h =
				current_time + static_cast<schedule_time_t>(edd_prefix_p[static_cast<std::size_t>(h_idx + 1)]);
			bool eliminated = false;

			const int rel = h_idx - longest_pos_in_edd;
			if (h_idx >= longest_pos_in_edd + 2) {
				const job& prev = jobs[static_cast<std::size_t>(edd_jobs[static_cast<std::size_t>(h_idx - 1)])];
				const schedule_time_t rhs =
					static_cast<schedule_time_t>(prev.d) + static_cast<schedule_time_t>(prev.p);
				if (rhs > rule3_max_dp) {
					rule3_max_dp = rhs;
				}
			}
			if (rel > 0) {
				const long long prev_trial_tardiness = trial_tardiness_values[static_cast<std::size_t>(rel - 1)];
				if (prev_trial_tardiness < rule4_min_trial_tardiness) {
					rule4_min_trial_tardiness = prev_trial_tardiness;
				}
			}
			if (!eliminated && h_idx >= longest_pos_in_edd + 2 && completion_at_h <= rule3_max_dp) {
				eliminated = true;
			}
			if (!eliminated) {
				const long long t_curr = trial_tardiness_values[static_cast<std::size_t>(rel)];
				const bool dominated_by_prev = (rel > 0) && (t_curr >= rule4_min_trial_tardiness);
				const bool dominated_by_next =
					(rel + 1 < static_cast<int>(trial_tardiness_values.size())) &&
					(t_curr > trial_tardiness_values[static_cast<std::size_t>(rel + 1)]);
				if (dominated_by_prev || dominated_by_next) {
					eliminated = true;
				}
			}

			if (!eliminated) {
				lawler_r_positions.push_back(h_idx);
			}
			else if (track_stats) {
				++stats_.positions_pruned_by_lawler_rule4;
			}
		}
	}
	else {
		// Базовый позиционный фильтр для Lawler/Potts: Rule 1 и обобщённое Rule 3
		// сокращают позиции r для центральной работы [q]_S.
		schedule_time_t property3b_prefix_max = 0;
		for (int h_idx = longest_pos_in_edd; h_idx < static_cast<int>(edd_jobs.size()); ++h_idx) {
			if (track_stats) {
				++stats_.valid_positions_built;
				++lawler_positions_before;
			}

			const schedule_time_t completion_at_h =
				current_time + static_cast<schedule_time_t>(edd_prefix_p[static_cast<std::size_t>(h_idx + 1)]);
			bool eliminated = false;

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
						++stats_.positions_pruned_by_lawler_basic;
					}
				}
			}

			if (!eliminated && h_idx > longest_pos_in_edd && completion_at_h < property3b_prefix_max) {
				eliminated = true;
				if (track_stats) {
					++stats_.valid_positions_pruned_3b;
					++stats_.positions_pruned_by_lawler_basic;
				}
			}

			if (!eliminated) {
				lawler_r_positions.push_back(h_idx);
			}
		}
	}
	if (profiling_timers) {
		const auto valid_positions_finish = std::chrono::steady_clock::now();
		const double elapsed =
			std::chrono::duration<double, std::milli>(valid_positions_finish - valid_positions_start).count();
		stats_.valid_positions_time_ms += elapsed;
		stats_.time_spent_in_position_filtering_ms += elapsed;
	}
	if (lawler_r_positions.empty()) {
		// Для точности нельзя оставлять K(S) пустым: хотя бы исходная позиция q допустима.
		lawler_r_positions.push_back(longest_pos_in_edd);
	}
	if (track_stats) {
		const std::uint64_t lawler_positions_after =
			static_cast<std::uint64_t>(lawler_r_positions.size());
		stats_.valid_positions_before += lawler_positions_before;
		stats_.valid_positions_after += lawler_positions_after;
		stats_.candidate_positions_before += lawler_positions_before;
		stats_.candidate_positions_after += lawler_positions_after;
		if (lawler_positions_before > lawler_positions_after) {
			stats_.positions_pruned += lawler_positions_before - lawler_positions_after;
		}
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
			// На время lookup делаем переданный subset текущим состоянием solver.
			// Инвариант memo сохраняется: запрос всегда проверяет полный ключ (S,t).
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
				if (track_stats) {
					++stats_.memo_exact_hits;
				}
				return memo_lb.exact;
			}
			if (!any_lb_enabled(config_)) {
				return 0;
			}

			// LB для дочернего подмножества считается в том же формате OPT(S',t'),
			// но используется только как допустимая оценка, не как ответ.
			const std::uint64_t prev_hash = runtime_.subset_hash;
			const std::uint64_t prev_fp = runtime_.subset_fingerprint;
			const int prev_count = runtime_.remaining_count;
			runtime_.remaining_bits.swap(bits);
			runtime_.subset_hash = subset_hash;
			runtime_.subset_fingerprint = subset_fingerprint;
			runtime_.remaining_count = subset_count;
			if (track_stats) {
				++stats_.simple_lb_calls;
			}
			long long lb = lower_bound_additional(n_ - subset_count, start_time);
			if (memo_lb.found && memo_lb.lower_bound > lb) {
				lb = memo_lb.lower_bound;
				if (track_stats) {
					++stats_.memo_lb_hits;
				}
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
				if (track_stats) {
					++stats_.memo_exact_hits;
				}
				return lk.exact;
			}

			// Если exact-записи нет, рекурсивно решаем подзадачу (S',t').
			// known_lookup может содержать LB-запись, но она не заменяет exact OPT.
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

	// Одна ветвь: before, pivot, after соответствует L_r,[q]_S,R_r
	// для Lawler или аналогичному разрезу A,[1]_S,rest для Szwarc.
	// pivot_completion — это C_pivot; вклад pivot равен T_pivot.
	// fallback_first_in_before нужен только для best_job, если child memo ещё не вернул первую работу блока.
	memo_reconstruction_trace best_reconstruction_trace{};
	auto evaluate_branch_bits = [&](int pivot_job,
		std::vector<std::uint64_t>& before_bits, int before_count, std::uint64_t before_hash, std::uint64_t before_fp,
		std::vector<std::uint64_t>& after_bits, int after_count, std::uint64_t after_hash, std::uint64_t after_fp,
		schedule_time_t pivot_completion, int fallback_first_in_before) {
			if (track_stats) {
				++stats_.branches_generated;
			}
			const long long pivot_tardiness_T = static_cast<long long>(
				tardiness(pivot_completion,
					inst_->jobs[static_cast<std::size_t>(pivot_job)].d));

			long long LB_before = 0;
			long long LB_after = 0;
			if (any_lb_enabled(config_)) {
				LB_before = subset_lower_bound_bits(before_bits, before_count, before_hash, before_fp, current_time);
				LB_after = subset_lower_bound_bits(after_bits, after_count, after_hash, after_fp, pivot_completion);
				if (LB_before + pivot_tardiness_T + LB_after >= upper_bound_UB) {
					if (track_stats) {
						++stats_.pruned_by_bound;
						++stats_.branches_pruned;
						++stats_.lb_prunes;
						if (config_.bounds.enable_simple_lb) {
							++stats_.simple_lb_prunes;
						}
					}
					return;
				}
			}

			int first_in_before = -1;
			const long long OPT_before =
				solve_subset_exact_bits(before_bits, before_count, before_hash, before_fp, current_time, &first_in_before);
			if (use_upper_bound_here && OPT_before + pivot_tardiness_T + LB_after >= upper_bound_UB) {
				if (track_stats) {
					++stats_.pruned_by_bound;
					++stats_.branches_pruned;
					if (any_lb_enabled(config_)) {
						++stats_.lb_prunes;
						if (config_.bounds.enable_simple_lb) {
							++stats_.simple_lb_prunes;
						}
					}
					else {
						++stats_.ub_prunes;
					}
				}
				return;
			}

			const long long OPT_after =
				solve_subset_exact_bits(after_bits, after_count, after_hash, after_fp, pivot_completion, nullptr);
			const long long total = OPT_before + pivot_tardiness_T + OPT_after;
			if (total < upper_bound_UB) {
				upper_bound_UB = total;
				if (track_stats) {
					++stats_.ub_improvements;
				}
				if (before_count == 0) {
					best_first_job = pivot_job;
				}
				else if (first_in_before >= 0) {
					best_first_job = first_in_before;
				}
				else {
					best_first_job = fallback_first_in_before;
				}
				if (config_.reconstruct_order && config_.reconstruction_trace && track_stats) {
					best_reconstruction_trace = {};
					best_reconstruction_trace.has_trace = true;
					best_reconstruction_trace.pivot_job = pivot_job;
					best_reconstruction_trace.before_count = before_count;
					best_reconstruction_trace.after_count = after_count;
					best_reconstruction_trace.before_hash = before_hash;
					best_reconstruction_trace.before_fingerprint = before_fp;
					best_reconstruction_trace.after_hash = after_hash;
					best_reconstruction_trace.after_fingerprint = after_fp;
					best_reconstruction_trace.pivot_completion = pivot_completion;
					best_reconstruction_trace.before_exact = OPT_before;
					best_reconstruction_trace.pivot_tardiness = pivot_tardiness_T;
					best_reconstruction_trace.after_exact = OPT_after;
				}
			}
		};

	const int max_earliest_pos = static_cast<int>(edd_jobs.size()) - earliest_pos_in_lpt - 1;
	// Szwarc / Decomposition II применим, когда центральная работа [1]_S
	// отличается от Lawler-работы l. LPT задаёт, какие работы могут оказаться
	// перед [1]_S в разрезе Decomposition II.
	const bool szwarc_available = earliest_job != longest_job
		&& max_earliest_pos >= 0;

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

		auto prepare_szwarc_positions = [&](std::vector<int>& out_positions, bool count_position_stats) {
			const auto szwarc_positions_start = (count_position_stats && profiling_timers)
				? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point{};
			const auto finish_szwarc_position_timer = [&]() {
				if (!count_position_stats || !profiling_timers) {
					return;
				}
				const auto finish = std::chrono::steady_clock::now();
				const double elapsed =
					std::chrono::duration<double, std::milli>(finish - szwarc_positions_start).count();
				stats_.valid_positions_time_ms += elapsed;
				stats_.time_spent_in_position_filtering_ms += elapsed;
				};
			const auto add_szwarc_position_stats =
				[&](std::uint64_t positions_before, std::uint64_t positions_after) {
					if (!count_position_stats) {
						return;
					}
					stats_.valid_positions_before += positions_before;
					stats_.valid_positions_after += positions_after;
					stats_.candidate_positions_before += positions_before;
					stats_.candidate_positions_after += positions_after;
					if (positions_before > positions_after) {
						const std::uint64_t pruned = positions_before - positions_after;
						stats_.positions_pruned += pruned;
						if (szwarc_rule4_position_filter_enabled(config_)) {
							stats_.positions_pruned_by_szwarc_rule4 += pruned;
						}
					}
				};
			// Decomposition II двигает работу [1]_S (минимальный d_j) относительно
			// работ, которые идут после неё в LPT(S). Поэтому сначала берём LPT-хвост,
			// а затем восстанавливаем для него EDD-порядок левого блока A.
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
			if (count_position_stats) {
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
			if (!szwarc_rule4_position_filter_enabled(config_)) {
				// Без position filtering перебираем все размеры A: от пустого блока
				// до максимального числа кандидатов перед [1]_S.
				for (int r = 0; r <= max_earliest_pos; ++r) {
					out_positions.push_back(r);
				}
				add_szwarc_position_stats(static_cast<std::uint64_t>(max_earliest_pos + 1),
					static_cast<std::uint64_t>(out_positions.size()));
				finish_szwarc_position_timer();
				return;
			}

			const job& earliest = inst_->jobs[static_cast<std::size_t>(earliest_job)];
			// Rule 4 для Szwarc сравнивает значения T(π^(r)) при переносе
			// работы [1]_S относительно кандидатов перед ней.
			std::vector<long long>& trial_tardiness = scratch.trial_tardiness_values;
			trial_tardiness.resize(candidates_before_earliest.size() + 1);

		long long t_cur = 0;
		schedule_time_t c = current_time;
		c += static_cast<schedule_time_t>(earliest.p);
		t_cur += static_cast<long long>(tardiness(c, earliest.d));
		for (int j : candidates_before_earliest) {
			const job& jj = inst_->jobs[static_cast<std::size_t>(j)];
			c += static_cast<schedule_time_t>(jj.p);
			t_cur += static_cast<long long>(tardiness(c, jj.d));
		}
		trial_tardiness[0] = t_cur;

		schedule_time_t start_t_before_pivot = current_time;
		for (int rel = 1; rel <= static_cast<int>(candidates_before_earliest.size()); ++rel) {
			const int next_job = candidates_before_earliest[static_cast<std::size_t>(rel - 1)];
			const job& nxt = inst_->jobs[static_cast<std::size_t>(next_job)];
			long long t_next = trial_tardiness[static_cast<std::size_t>(rel - 1)];
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
			trial_tardiness[static_cast<std::size_t>(rel)] = t_next;
			start_t_before_pivot += static_cast<schedule_time_t>(nxt.p);
		}

		long long min_trial_tardiness = std::numeric_limits<long long>::max();
		for (int r = 0; r <= max_earliest_pos; ++r) {
			if (r >= 1) {
				const long long prev = trial_tardiness[static_cast<std::size_t>(r - 1)];
				if (prev < min_trial_tardiness) {
					min_trial_tardiness = prev;
				}
			}
			const long long t_r = trial_tardiness[static_cast<std::size_t>(r)];
			const bool dominated_by_prev = (r >= 1) && (t_r >= min_trial_tardiness);
			const bool dominated_by_next =
				(r < max_earliest_pos) &&
				(t_r > trial_tardiness[static_cast<std::size_t>(r + 1)]);
			if (!dominated_by_prev && !dominated_by_next) {
				out_positions.push_back(r);
			}
		}
		if (out_positions.empty()) {
			out_positions.push_back(0);
		}
		add_szwarc_position_stats(static_cast<std::uint64_t>(max_earliest_pos + 1),
			static_cast<std::uint64_t>(out_positions.size()));
		finish_szwarc_position_timer();
	};

	enum class adaptive_choice {
		lawler,
		szwarc,
		both
	};

	auto choose_v1 = [&]() {
		const int lawler_count = static_cast<int>(lawler_r_positions.size());
		const int szwarc_count = max_earliest_pos + 1;
		return (szwarc_available && szwarc_count < lawler_count)
			? adaptive_choice::szwarc
			: adaptive_choice::lawler;
	};

	auto estimate_szwarc_positions = [&]() -> int {
		if (!szwarc_available) {
			return std::numeric_limits<int>::max() / 4;
		}
		std::vector<int>& estimated_positions = scratch.szwarc_positions;
		prepare_szwarc_positions(estimated_positions, false);
		return std::max(1, static_cast<int>(estimated_positions.size()));
	};

	auto average_lawler_position_cost = [&]() -> double {
		if (lawler_r_positions.empty()) {
			return 0.0;
		}
		long long total = 0;
		for (int h_idx : lawler_r_positions) {
			const int before_count = h_idx;
			const int after_count = saved_count - before_count - 1;
			total += std::max(before_count, after_count);
		}
		return static_cast<double>(total) / static_cast<double>(lawler_r_positions.size());
	};

	auto average_szwarc_position_cost = [&]() -> double {
		if (!szwarc_available) {
			return static_cast<double>(saved_count);
		}
		std::vector<int>& estimated_positions = scratch.szwarc_positions;
		if (estimated_positions.empty()) {
			prepare_szwarc_positions(estimated_positions, false);
		}
		long long total = 0;
		for (int b_size : estimated_positions) {
			const int after_count = saved_count - b_size - 1;
			total += std::max(b_size, after_count);
		}
		return static_cast<double>(total) / static_cast<double>(estimated_positions.size());
	};

	auto estimate_double_children_count = [&]() -> int {
		if (!szwarc_available) {
			return std::numeric_limits<int>::max() / 4;
		}
		std::vector<int>& estimated_positions = scratch.szwarc_positions;
		if (estimated_positions.empty()) {
			prepare_szwarc_positions(estimated_positions, false);
		}
		int pair_count = 0;
		for (int h_idx : lawler_r_positions) {
			const int lawler_prefix_count = h_idx;
			for (int b_size : estimated_positions) {
				if (b_size > static_cast<int>(scratch.candidates_before_earliest.size())) {
					break;
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
				++pair_count;
			}
		}
		return pair_count > 0 ? pair_count : std::numeric_limits<int>::max() / 4;
	};

	auto min_count_choice = [](int lawler_count, int szwarc_count, int both_count) {
		adaptive_choice best = adaptive_choice::lawler;
		int best_count = lawler_count;
		if (szwarc_count < best_count) {
			best = adaptive_choice::szwarc;
			best_count = szwarc_count;
		}
		if (both_count < best_count) {
			best = adaptive_choice::both;
		}
		return best;
	};

	auto choose_v2 = [&]() {
		if (!szwarc_available) {
			return adaptive_choice::lawler;
		}
		const int lawler_count = static_cast<int>(lawler_r_positions.size());
		const int szwarc_count = estimate_szwarc_positions();
		const int both_count = estimate_double_children_count();
		const bool tiny_state = saved_count <= 12;
		if (tiny_state) {
			return min_count_choice(lawler_count, szwarc_count, both_count);
		}
		constexpr int decisive_ratio = 2;
		if (lawler_count * decisive_ratio <= szwarc_count &&
			lawler_count * decisive_ratio <= both_count) {
			return adaptive_choice::lawler;
		}
		if (szwarc_count * decisive_ratio <= lawler_count &&
			szwarc_count * decisive_ratio <= both_count) {
			return adaptive_choice::szwarc;
		}
		if (both_count * decisive_ratio <= lawler_count &&
			both_count * decisive_ratio <= szwarc_count) {
			return adaptive_choice::both;
		}
		return choose_v1();
	};

	auto choose_v3 = [&]() {
		if (!szwarc_available) {
			return adaptive_choice::lawler;
		}
		constexpr double alpha = 0.05;
		const int lawler_count = static_cast<int>(lawler_r_positions.size());
		const int szwarc_count = estimate_szwarc_positions();
		const int both_count = estimate_double_children_count();
		const double lawler_score =
			static_cast<double>(lawler_count) + alpha * average_lawler_position_cost();
		const double szwarc_score =
			static_cast<double>(szwarc_count) + alpha * average_szwarc_position_cost();
		const double both_score =
			static_cast<double>(both_count) + alpha * static_cast<double>(saved_count);
		if (both_score < lawler_score && both_score < szwarc_score) {
			return adaptive_choice::both;
		}
		return (szwarc_score < lawler_score) ? adaptive_choice::szwarc : adaptive_choice::lawler;
	};

	bool run_double = false;
	bool run_lawler = false;
	bool run_szwarc = false;
	bool used_adaptive_selection = false;
	adaptive_choice selected_adaptive_choice = adaptive_choice::lawler;

	auto choose_adaptive_flags = [&]() {
		used_adaptive_selection = true;
		// Adaptive v1/v2/v3 меняют только порядок выбора точной декомпозиции.
		// Ни один вариант не добавляет pruning и не исключает ветви выбранного разложения,
		// поэтому значение OPT(S,t) остаётся тем же.
		switch (config_.adaptive_policy) {
		case adaptive_policy_kind::v2:
			selected_adaptive_choice = choose_v2();
			break;
		case adaptive_policy_kind::v3:
			selected_adaptive_choice = choose_v3();
			break;
		case adaptive_policy_kind::v1:
		default:
			selected_adaptive_choice = choose_v1();
			break;
		}
		if (selected_adaptive_choice == adaptive_choice::both) {
			run_double = szwarc_available;
		}
		else if (selected_adaptive_choice == adaptive_choice::szwarc) {
			run_szwarc = szwarc_available;
		}
		else {
			run_lawler = true;
		}
	};

	switch (config_.decomposition_mode) {
	case DecompositionMode::Lawler:
		run_lawler = true;
		break;
	case DecompositionMode::Szwarc:
		run_szwarc = szwarc_available;
		break;
	case DecompositionMode::BothLawlerSzwarc:
		run_double = szwarc_available;
		if (!run_double) {
			run_lawler = true;
		}
		break;
	case DecompositionMode::Adaptive:
	default:
		choose_adaptive_flags();
		break;
	}
	if (!run_double && !run_lawler && !run_szwarc) {
		// Например, явно выбранный Szwarc может быть недоступен в данном S.
		// Переход к Lawler сохраняет точность: Decomposition I доступна всегда.
		run_lawler = true;
		selected_adaptive_choice = adaptive_choice::lawler;
	}
	int estimated_branches_here = 0;
	if (run_double) {
		estimated_branches_here += estimate_double_children_count();
	}
	if (run_lawler) {
		estimated_branches_here += static_cast<int>(lawler_r_positions.size());
	}
	if (run_szwarc) {
		estimated_branches_here += estimate_szwarc_positions();
	}
	if (depth >= 0 && depth < static_cast<int>(runtime_.estimated_branches_by_depth.size())) {
		runtime_.estimated_branches_by_depth[static_cast<std::size_t>(depth)] = estimated_branches_here;
	}
	if (track_stats && used_adaptive_selection) {
		std::uint64_t* generic_counter = nullptr;
		std::uint64_t* version_counter = nullptr;
		if (run_double) {
			generic_counter = &stats_.adaptive_choices_both;
			if (config_.adaptive_policy == adaptive_policy_kind::v2) {
				version_counter = &stats_.adaptive_v2_choices_both;
			}
			else if (config_.adaptive_policy == adaptive_policy_kind::v3) {
				version_counter = &stats_.adaptive_v3_choices_both;
			}
			else {
				version_counter = &stats_.adaptive_v1_choices_both;
			}
		}
		else if (run_szwarc && !run_lawler) {
			generic_counter = &stats_.adaptive_choices_szwarc;
			++stats_.adaptive_choice_szwarc;
			if (config_.adaptive_policy == adaptive_policy_kind::v2) {
				version_counter = &stats_.adaptive_v2_choices_szwarc;
			}
			else if (config_.adaptive_policy == adaptive_policy_kind::v3) {
				version_counter = &stats_.adaptive_v3_choices_szwarc;
			}
			else {
				version_counter = &stats_.adaptive_v1_choices_szwarc;
			}
		}
		else if (run_lawler && !run_szwarc) {
			generic_counter = &stats_.adaptive_choices_lawler;
			++stats_.adaptive_choice_lawler;
			if (config_.adaptive_policy == adaptive_policy_kind::v2) {
				version_counter = &stats_.adaptive_v2_choices_lawler;
			}
			else if (config_.adaptive_policy == adaptive_policy_kind::v3) {
				version_counter = &stats_.adaptive_v3_choices_lawler;
			}
			else {
				version_counter = &stats_.adaptive_v1_choices_lawler;
			}
		}
		if (generic_counter != nullptr) {
			++(*generic_counter);
		}
		if (version_counter != nullptr) {
			++(*version_counter);
		}
	}

	if (run_double) {
		if (track_stats) {
			++stats_.both_nodes;
		}
		// Double Decomposition: совместно фиксируем позиции [1]_S и l=[q]_S.
		// Реализован порядок блоков A,[1]_S,B,l,C, то есть это не простой
		// последовательный запуск Lawler и Szwarc, а парное разбиение одной подзадачи.
		std::vector<int>& szwarc_positions = scratch.szwarc_positions;
		prepare_szwarc_positions(szwarc_positions, track_stats);

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

		for (int h_idx : lawler_r_positions) {
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
				static_cast<schedule_time_t>(edd_prefix_p[static_cast<std::size_t>(h_idx + 1)]);
			const long long longest_cost = static_cast<long long>(
				tardiness(longest_completion, longest.d));
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
				if (track_stats) {
					++stats_.branches_generated;
				}

				int first_in_left = -1;
				const long long left_cost = solve_subset_exact_bits(
					left_bits, b_size, left_hash, left_fp, current_time, &first_in_left);

				const long long mid_cost = solve_subset_exact_bits(
					mid_bits, mid_count, mid_hash, mid_fp, earliest_completion, nullptr);

				const long long total =
					left_cost + earliest_cost + mid_cost + longest_cost + right_cost;
				if (total < upper_bound_UB) {
					upper_bound_UB = total;
					if (track_stats) {
						++stats_.ub_improvements;
					}
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
				if (upper_bound_UB == 0) {
					break;
				}
			}
			if (upper_bound_UB == 0) {
				break;
			}
		}

		if (!evaluated_pair) {
			run_lawler = true;
			run_szwarc = true;
		}
	}

	if (run_lawler) {
		if (track_stats) {
			++stats_.lawler_nodes;
		}
		// Decomposition I (Lawler): центральная работа [q]_S имеет максимальное p_j.
		// "Вставить [q]_S в позицию r" означает выбрать EDD-префикс L_r перед ней;
		// остальные работы образуют правый блок R_r. Ветвь имеет вид L_r, [q]_S, R_r.
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

		for (int h_idx : lawler_r_positions) {
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
				current_time + static_cast<schedule_time_t>(edd_prefix_p[static_cast<std::size_t>(h_idx + 1)]);
			// C_[q](r,t) = t + p(L_r) + p_[q].
			evaluate_branch_bits(
				longest_job,
				b_bits, prefix_count, prefix_hash, prefix_fp,
				a_bits, a_count, a_hash, a_fp,
				pivot_completion,
				fallback_first_in_b);
		}
	}

	if (run_szwarc) {
		if (track_stats) {
			++stats_.szwarc_nodes;
		}
		// Decomposition II (Szwarc): центральная работа [1]_S имеет минимальный d_j.
		// LPT используется, чтобы определить кандидатов, которые могут стоять перед ней;
		// b_size задаёт левый блок A, а остальные работы образуют правый блок.
		std::vector<int>& szwarc_positions = scratch.szwarc_positions;
		prepare_szwarc_positions(szwarc_positions, track_stats);

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
			// C_[1](r,t) = t + p(A) + p_[1] для выбранного левого блока A.
			evaluate_branch_bits(
				earliest_job,
				b_bits, b_size, prefix_hash, prefix_fp,
				a_bits, a_count, a_hash, a_fp,
				pivot_completion,
				fallback_first_in_b);
		}
	}

	runtime_.perm_jobs[static_cast<std::size_t>(depth)] = best_first_job;
	// После полного решения подзадачи сохраняем M[(S,t)] = OPT(S,t).
	store_exact_memo(current_time, upper_bound_UB, best_first_job, track_stats);
	store_reconstruction_trace(current_time, best_reconstruction_trace, track_stats);
	return upper_bound_UB;
}

bool dfs_solver::try_terminal_rules(int, schedule_time_t current_time,
	const std::vector<int>& edd_jobs,
	const std::vector<int>& lpt_jobs,
	long long& exact,
	int& first_job,
	std::uint64_t*& hit_counter) {
	// Terminal rules — точные частные случаи. Они сразу дают OPT(S,t),
	// поэтому результат можно сохранять как exact memo entry.
	if (edd_jobs.empty()) {
		exact = 0;
		first_job = -1;
		hit_counter = &stats_.terminal_edd_one_tardy_hits;
		return true;
	}

	if (config_.terminal_rules.enable_edd_at_most_one_tardy) {
		long long edd_cost = 0;
		int edd_tardy_count = 0;
		schedule_time_t edd_time = current_time;
		for (int job_idx : edd_jobs) {
			const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
			edd_time += static_cast<schedule_time_t>(j.p);
			const schedule_cost_t tardy = tardiness(edd_time, j.d);
			if (tardy > 0) {
				++edd_tardy_count;
			}
			edd_cost += static_cast<long long>(tardy);
		}
		if (edd_tardy_count <= 1) {
			// Если в EDD не больше одной запаздывающей работы, EDD уже оптимален.
			exact = edd_cost;
			first_job = edd_jobs.front();
			hit_counter = &stats_.terminal_edd_one_tardy_hits;
			return true;
		}
	}

	if (!config_.terminal_rules.enable_all_tardy_spt) {
		return false;
	}

	bool all_tardy_even_first = true;
	for (int job_idx : edd_jobs) {
		const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
		if (current_time + static_cast<schedule_time_t>(j.p) <= static_cast<schedule_time_t>(j.d)) {
			all_tardy_even_first = false;
			break;
		}
	}
	if (!all_tardy_even_first) {
		return false;
	}

	// Если любая оставшаяся работа запаздывает даже первой, минимизация ΣC_j
	// эквивалентна минимизации ΣT_j; тогда оптимален SPT. Получаем SPT как reverse LPT.
	long long spt_cost = 0;
	schedule_time_t spt_time = current_time;
	first_job = -1;
	for (auto it = lpt_jobs.rbegin(); it != lpt_jobs.rend(); ++it) {
		const int job_idx = *it;
		if (first_job < 0) {
			first_job = job_idx;
		}
		const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
		spt_time += static_cast<schedule_time_t>(j.p);
		spt_cost += static_cast<long long>(tardiness(spt_time, j.d));
	}
	exact = spt_cost;
	hit_counter = &stats_.terminal_all_tardy_spt_hits;
	return true;
}

long long dfs_solver::lower_bound_additional(int depth, schedule_time_t current_time) const {
	(void)depth;
	// Простая admissible LB: каждая оставшаяся работа j завершается не раньше t + p_j.
	// Это безопасно для отсечения, но слабее специальных LB1/LB2 из конспекта.
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
	const std::vector<int>& jobs, int* first_job, bool track_stats) {
	(void)depth;
	(void)track_stats;

	// UB строится как стоимость допустимого продолжения расписания pi.
	// Любой такой pi даёт верхнюю оценку OPT(S,t), но не доказывает оптимальность.
	long long upper_bound_UB = std::numeric_limits<long long>::max() / 4;
	int best_first = jobs.empty() ? -1 : jobs.front();

	if (config_.bounds.enable_edd_ub) {
		long long edd_cost = 0;
		schedule_time_t t = current_time;
		for (int job_idx : jobs) {
			t += static_cast<schedule_time_t>(inst_->jobs[static_cast<std::size_t>(job_idx)].p);
			edd_cost += static_cast<long long>(
				tardiness(t, inst_->jobs[static_cast<std::size_t>(job_idx)].d));
		}
		upper_bound_UB = edd_cost;
	}

	if (first_job != nullptr) {
		*first_job = best_first;
	}
	return upper_bound_UB;
}

void dfs_solver::store_reconstruction_trace(schedule_time_t current_time,
	const memo_reconstruction_trace& trace,
	bool track_stats) {
	if (!config_.reconstruct_order || !config_.reconstruction_trace || !track_stats || !trace.has_trace) {
		return;
	}
	if (memo_.store_reconstruction_trace(runtime_.remaining_bits, current_time,
		runtime_.subset_hash, runtime_.subset_fingerprint, trace, track_stats)) {
		++stats_.reconstruction_trace_stores;
	}
}

bool dfs_solver::append_terminal_order(schedule_time_t current_time, long long exact, std::vector<job_id_t>& order) {
	if (!any_terminal_rule_enabled(config_.terminal_rules)) {
		return false;
	}
	if (runtime_.remaining_count == 0) {
		return exact == 0;
	}

	std::vector<int> edd_jobs;
	build_remaining_jobs_in_order(runtime_.edd_order, edd_jobs);
	if (config_.terminal_rules.enable_edd_at_most_one_tardy) {
		long long edd_cost = 0;
		int edd_tardy_count = 0;
		schedule_time_t edd_time = current_time;
		for (int job_idx : edd_jobs) {
			const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
			edd_time += static_cast<schedule_time_t>(j.p);
			const schedule_cost_t tardy = tardiness(edd_time, j.d);
			if (tardy > 0) {
				++edd_tardy_count;
			}
			edd_cost += static_cast<long long>(tardy);
		}
		if (edd_tardy_count <= 1 && edd_cost == exact) {
			for (int job_idx : edd_jobs) {
				order.push_back(static_cast<job_id_t>(job_idx));
			}
			++stats_.reconstruction_trace_terminal_hits;
			return true;
		}
	}

	if (!config_.terminal_rules.enable_all_tardy_spt) {
		return false;
	}
	bool all_tardy_even_first = true;
	for (int job_idx : edd_jobs) {
		const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
		if (current_time + static_cast<schedule_time_t>(j.p) <= static_cast<schedule_time_t>(j.d)) {
			all_tardy_even_first = false;
			break;
		}
	}
	if (!all_tardy_even_first) {
		return false;
	}

	std::vector<int> lpt_jobs;
	build_remaining_jobs_in_order(runtime_.lpt_order, lpt_jobs);
	long long spt_cost = 0;
	schedule_time_t spt_time = current_time;
	for (auto it = lpt_jobs.rbegin(); it != lpt_jobs.rend(); ++it) {
		const int job_idx = *it;
		const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
		spt_time += static_cast<schedule_time_t>(j.p);
		spt_cost += static_cast<long long>(tardiness(spt_time, j.d));
	}
	if (spt_cost != exact) {
		return false;
	}
	for (auto it = lpt_jobs.rbegin(); it != lpt_jobs.rend(); ++it) {
		order.push_back(static_cast<job_id_t>(*it));
	}
	++stats_.reconstruction_trace_terminal_hits;
	return true;
}

bool dfs_solver::append_order_linearly(schedule_time_t current_time, long long exact, std::vector<job_id_t>& order) {
	const std::size_t start_size = order.size();
	std::vector<int> removed;
	removed.reserve(static_cast<std::size_t>(runtime_.remaining_count));

	long long remaining_optimal = exact;
	while (runtime_.remaining_count > 0) {
		++stats_.reconstruction_steps;
		int best_job = -1;
		long long best_total = std::numeric_limits<long long>::max();
		schedule_time_t best_completion = current_time;
		long long best_incremental = 0;
		bool found_exact_continuation = false;

		memo_lookup_result current_lookup =
			memo_.query_exact(runtime_.remaining_bits, current_time,
				runtime_.subset_hash, runtime_.subset_fingerprint, false);
		bool current_exact_ok =
			current_lookup.found && current_lookup.has_exact &&
			current_lookup.exact == remaining_optimal;
		if (current_exact_ok) {
			++stats_.reconstruction_current_exact_hits;
		}
		else {
			++stats_.reconstruction_current_exact_misses;
		}

		auto try_job = [&](int job_idx) {
			if (!is_job_remaining(job_idx)) {
				return false;
			}
			++stats_.reconstruction_candidate_scans;
			const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
			const schedule_time_t completion = current_time + static_cast<schedule_time_t>(j.p);
			const long long incremental = static_cast<long long>(tardiness(completion, j.d));
			if (incremental > remaining_optimal) {
				return false;
			}

			remove_job_from_state(job_idx);
			memo_lookup_result lookup =
				memo_.query_exact(runtime_.remaining_bits, completion,
					runtime_.subset_hash, runtime_.subset_fingerprint, false);
			long long rest = 0;
			if (lookup.found && lookup.has_exact) {
				++stats_.reconstruction_child_exact_hits;
				rest = lookup.exact;
			}
			else {
				++stats_.reconstruction_child_exact_misses;
				++stats_.reconstruction_repair_solves;
				rest = solve_state(n_ - runtime_.remaining_count, completion, nullptr, false);
			}
			restore_job_to_state(job_idx);

			const long long total = incremental + rest;
			if (total == remaining_optimal) {
				best_total = total;
				best_job = job_idx;
				best_completion = completion;
				best_incremental = incremental;
				found_exact_continuation = true;
				return true;
			}
			if (total < best_total || (total == best_total && (best_job < 0 || job_idx < best_job))) {
				best_total = total;
				best_job = job_idx;
				best_completion = completion;
				best_incremental = incremental;
			}
			return false;
		};

		const int hinted_job = current_exact_ok ? current_lookup.best_job : -1;
		if (hinted_job >= 0) {
			(void)try_job(hinted_job);
		}

		for (int job_idx = 0; job_idx < n_; ++job_idx) {
			if (found_exact_continuation) {
				break;
			}
			if (job_idx == hinted_job) {
				continue;
			}
			(void)try_job(job_idx);
		}

		if (!found_exact_continuation || best_job < 0 || !is_job_remaining(best_job)) {
			for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
				restore_job_to_state(*it);
			}
			order.resize(start_size);
			return false;
		}

		order.push_back(static_cast<job_id_t>(best_job));
		removed.push_back(best_job);
		remove_job_from_state(best_job);
		current_time = best_completion;
		remaining_optimal -= best_incremental;
	}

	for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
		restore_job_to_state(*it);
	}
	return true;
}

bool dfs_solver::append_order_by_trace(schedule_time_t current_time, long long exact, std::vector<job_id_t>& order) {
	if (runtime_.remaining_count == 0) {
		return exact == 0;
	}
	if (runtime_.remaining_count == 1) {
		for (int job_idx = 0; job_idx < n_; ++job_idx) {
			if (!is_job_remaining(job_idx)) {
				continue;
			}
			const job& j = inst_->jobs[static_cast<std::size_t>(job_idx)];
			const schedule_time_t completion = current_time + static_cast<schedule_time_t>(j.p);
			if (static_cast<long long>(tardiness(completion, j.d)) == exact) {
				order.push_back(static_cast<job_id_t>(job_idx));
				return true;
			}
			break;
		}
		++stats_.reconstruction_trace_fallbacks;
		return append_order_linearly(current_time, exact, order);
	}
	if (append_terminal_order(current_time, exact, order)) {
		return true;
	}

	memo_lookup_result trace_lookup =
		memo_.query_exact(runtime_.remaining_bits, current_time,
			runtime_.subset_hash, runtime_.subset_fingerprint, false);
	if (!trace_lookup.found || !trace_lookup.has_exact ||
		trace_lookup.exact != exact ||
		!trace_lookup.reconstruction_trace.has_trace) {
		++stats_.reconstruction_trace_misses;
		++stats_.reconstruction_trace_fallbacks;
		return append_order_linearly(current_time, exact, order);
	}
	++stats_.reconstruction_trace_hits;
	const memo_reconstruction_trace& entry = trace_lookup.reconstruction_trace;
	if (entry.pivot_job < 0 ||
		!is_job_remaining(entry.pivot_job) ||
		entry.before_count + entry.after_count + 1 != runtime_.remaining_count ||
		entry.before_exact + entry.pivot_tardiness + entry.after_exact != exact) {
		++stats_.reconstruction_trace_fallbacks;
		return append_order_linearly(current_time, exact, order);
	}

	const std::size_t words = runtime_.remaining_bits.size();
	std::vector<std::uint64_t> trace_before_bits(words, 0);
	std::vector<std::uint64_t> trace_after_bits(words, 0);
	std::uint64_t trace_before_hash = 0;
	std::uint64_t trace_before_fingerprint = 0;
	schedule_time_t before_processing = 0;
	int built_before_count = 0;

	std::vector<int> edd_jobs;
	std::vector<int> lpt_jobs;
	build_remaining_jobs_in_order(runtime_.edd_order, edd_jobs);
	build_remaining_jobs_in_order(runtime_.lpt_order, lpt_jobs);
	if (edd_jobs.empty() || lpt_jobs.empty()) {
		++stats_.reconstruction_trace_fallbacks;
		return append_order_linearly(current_time, exact, order);
	}

	auto add_before_job = [&](int job_idx) {
		const std::size_t idx = static_cast<std::size_t>(job_idx);
		trace_before_bits[idx >> 6] |= std::uint64_t{ 1 } << (idx & 63);
		trace_before_hash ^= runtime_.zobrist_job[idx];
		trace_before_fingerprint ^= runtime_.zobrist_job_fp[idx];
		before_processing += static_cast<schedule_time_t>(inst_->jobs[idx].p);
		++built_before_count;
	};

	const int longest_job = lpt_jobs.front();
	const int earliest_job = edd_jobs.front();
	bool partition_built = false;
	if (entry.pivot_job == longest_job) {
		for (int job_idx : edd_jobs) {
			if (job_idx == entry.pivot_job) {
				continue;
			}
			if (built_before_count >= entry.before_count) {
				break;
			}
			add_before_job(job_idx);
		}
		partition_built = built_before_count == entry.before_count;
	}
	else if (entry.pivot_job == earliest_job) {
		std::vector<unsigned char> candidate(static_cast<std::size_t>(n_), 0);
		int earliest_pos_in_lpt = static_cast<int>(lpt_jobs.size()) - 1;
		for (int i = 0; i < static_cast<int>(lpt_jobs.size()); ++i) {
			if (lpt_jobs[static_cast<std::size_t>(i)] == earliest_job) {
				earliest_pos_in_lpt = i;
				break;
			}
		}
		for (int i = earliest_pos_in_lpt + 1; i < static_cast<int>(lpt_jobs.size()); ++i) {
			candidate[static_cast<std::size_t>(lpt_jobs[static_cast<std::size_t>(i)])] = 1;
		}
		for (int job_idx : edd_jobs) {
			if (candidate[static_cast<std::size_t>(job_idx)] == 0) {
				continue;
			}
			if (built_before_count >= entry.before_count) {
				break;
			}
			add_before_job(job_idx);
		}
		partition_built = built_before_count == entry.before_count;
	}
	if (!partition_built) {
		++stats_.reconstruction_trace_fallbacks;
		return append_order_linearly(current_time, exact, order);
	}

	const std::size_t pivot_idx = static_cast<std::size_t>(entry.pivot_job);
	for (std::size_t w = 0; w < words; ++w) {
		trace_after_bits[w] = runtime_.remaining_bits[w] & ~trace_before_bits[w];
	}
	trace_after_bits[pivot_idx >> 6] &= ~(std::uint64_t{ 1 } << (pivot_idx & 63));
	const std::uint64_t pivot_hash = runtime_.zobrist_job[pivot_idx];
	const std::uint64_t pivot_fingerprint = runtime_.zobrist_job_fp[pivot_idx];
	const std::uint64_t trace_after_hash = runtime_.subset_hash ^ trace_before_hash ^ pivot_hash;
	const std::uint64_t trace_after_fingerprint =
		runtime_.subset_fingerprint ^ trace_before_fingerprint ^ pivot_fingerprint;
	const schedule_time_t trace_pivot_completion =
		current_time + before_processing + static_cast<schedule_time_t>(inst_->jobs[pivot_idx].p);
	if (trace_before_hash != entry.before_hash ||
		trace_before_fingerprint != entry.before_fingerprint ||
		trace_after_hash != entry.after_hash ||
		trace_after_fingerprint != entry.after_fingerprint ||
		trace_pivot_completion != entry.pivot_completion) {
		++stats_.reconstruction_trace_fallbacks;
		return append_order_linearly(current_time, exact, order);
	}

	const std::size_t start_size = order.size();
	const std::vector<std::uint64_t> saved_bits = runtime_.remaining_bits;
	const std::uint64_t saved_hash = runtime_.subset_hash;
	const std::uint64_t saved_fingerprint = runtime_.subset_fingerprint;
	const int saved_count = runtime_.remaining_count;

	auto restore_parent = [&]() {
		runtime_.remaining_bits = saved_bits;
		runtime_.subset_hash = saved_hash;
		runtime_.subset_fingerprint = saved_fingerprint;
		runtime_.remaining_count = saved_count;
	};
	auto set_state = [&](const std::vector<std::uint64_t>& bits,
		int count,
		std::uint64_t hash,
		std::uint64_t fingerprint) {
			runtime_.remaining_bits = bits;
			runtime_.remaining_count = count;
			runtime_.subset_hash = hash;
			runtime_.subset_fingerprint = fingerprint;
	};

	if (entry.before_count > 0) {
		set_state(trace_before_bits, entry.before_count, entry.before_hash, entry.before_fingerprint);
		if (!append_order_by_trace(current_time, entry.before_exact, order)) {
			restore_parent();
			order.resize(start_size);
			return false;
		}
	}

	order.push_back(static_cast<job_id_t>(entry.pivot_job));

	if (entry.after_count > 0) {
		set_state(trace_after_bits, entry.after_count, entry.after_hash, entry.after_fingerprint);
		if (!append_order_by_trace(entry.pivot_completion, entry.after_exact, order)) {
			restore_parent();
			order.resize(start_size);
			return false;
		}
	}

	restore_parent();
	return true;
}

std::vector<job_id_t> dfs_solver::reconstruct_order(long long optimal_cost) {
	const auto reconstruction_start = std::chrono::steady_clock::now();
	// Reconstruction идёт по exact-записям M[(S,t)].
	// Поле best_job хранит первую работу оптимального продолжения для этого состояния.
	// LB-запись использовать нельзя: она не содержит ни OPT(S,t), ни корректного best_job.
	std::vector<job_id_t> order;
	order.reserve(static_cast<std::size_t>(n_));

	{
		const bool ok = config_.reconstruction_trace
			? append_order_by_trace(0, optimal_cost, order)
			: append_order_linearly(0, optimal_cost, order);
		if (!ok) {
			order.clear();
		}
		const auto reconstruction_finish = std::chrono::steady_clock::now();
		stats_.reconstruction_time_ms +=
			std::chrono::duration<double, std::milli>(
				reconstruction_finish - reconstruction_start).count();
		return order;
	}
}

memo_lookup_result dfs_solver::query_memo(schedule_time_t current_time, bool track_stats) {
	// Полный ключ memo: bitset S + t. Хеш ускоряет поиск, но при совпадении
	// memo_table всё равно сверяет полный ключ S, чтобы не зависеть от коллизий.
	if (!config_.memo.enable_memo) {
		return {};
	}
	if (track_stats) {
		if (config_.memo.enable_exact_memo) {
			++stats_.memo_exact_queries;
		}
		if (config_.memo.enable_lb_memo || config_.bounds.enable_lb_memo) {
			++stats_.memo_lb_queries;
		}
	}
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
	// Exact-запись хранит полный ответ OPT(S,t) и best_job для восстановления порядка.
	if (!config_.memo.enable_memo || !config_.memo.enable_exact_memo) {
		return;
	}
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
	// LB-запись хранит только нижнюю оценку. Она подходит для pruning,
	// но не должна использоваться как готовое решение состояния.
	if (!config_.memo.enable_memo ||
		!(config_.memo.enable_lb_memo || config_.bounds.enable_lb_memo)) {
		return;
	}
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
