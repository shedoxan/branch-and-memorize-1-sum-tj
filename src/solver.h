#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "core.h"
#include "stats.h"

enum class decomposition_policy {
	adaptive = 0,
	lawler = 1,
	szwarc = 2,
	both = 3
};

struct profiling_control {
	bool enabled = true;
};

struct dfs_config {
	std::size_t memo_capacity = 200000;
	std::size_t memo_memory_budget_bytes = 0;
	std::uint64_t zobrist_seed = 1;

	bool reconstruct_order = true;
	bool strict_memory_cap = true;
	bool use_lower_bounds = false;
	bool use_decomposition2 = true;
	bool use_lawler_position_filter = true;
	bool use_lawler_rule12 = false;
	bool use_process_memory_gate = false;
	bool use_double_pair_lb_prune = false;
	bool use_lufo_exact_protection = false;
	decomposition_policy decomp_policy = decomposition_policy::adaptive;

	profiling_control profiling{};
};

const char* to_string(decomposition_policy policy);

bool parse_decomposition_policy(const std::string& text, decomposition_policy& out);

struct solve_result {
	schedule best;
	solver_stats stats;
};

class solver {
public:
	virtual ~solver() = default;
	virtual solve_result solve(const instance& inst) = 0;
};
