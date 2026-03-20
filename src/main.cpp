#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dfs_solver.h"
#include "generator.h"
#include "threadpool.h"

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;

void on_interrupt_signal(int) {
	g_stop_requested = 1;
}

struct cli_options {
	bool benchmark = false;
	bool reconstruct_order = false;
	bool n_from_set = false;
	bool n_to_set = false;

	int n = 50;
	int n_from = 50;
	int n_to = 50;
	int n_step = 1;
	int instances = 1;
	int bench_threads = 1;

	int p_min = 1;
	int p_max = 100;
	double due_range = 0.2;
	double due_tardiness = 0.6;

	std::uint64_t seed = 1;
	std::size_t memo_capacity = 0; // 0 -> unlimited by entry count
	std::size_t mem_budget_mb = 1024;
	bool use_lower_bounds = false;
	bool use_decomposition2 = true;
	bool use_lawler_position_filter = true;
	bool use_lawler_rule12 = false;
	bool use_process_memory_gate = false;
	bool use_double_pair_lb_prune = false;
	bool use_lufo_exact_protect = false;
	bool profiling_enabled = true;
	decomposition_policy decomp_policy = decomposition_policy::adaptive;

	std::string bench_csv_path;
	std::string n_list_text;
	std::string input_path;
};

void print_usage() {
	std::cout
		<< "Usage:\n"
		<< "  kursovaya.exe [options]\n\n"
		<< "Single run options:\n"
		<< "  --n <int>                 Number of jobs (default: 50)\n"
		<< "  --instances <int>         Number of instances (default: 1)\n"
		<< "  --seed <u64>              Base seed (default: 1)\n"
		<< "  --input <path>            Solve instance from file (supports 'n + p d' and SDT 'p d' formats)\n\n"
		<< "Benchmark options:\n"
		<< "  --bench-csv <path>        Enable benchmark mode and write CSV\n"
		<< "  --n-from <int>            Start n for range\n"
		<< "  --n-to <int>              End n for range\n"
		<< "  --n-step <int>            Step for n range (default: 1)\n"
		<< "  --n-list <csv>            Explicit n list, e.g. 20,24,28\n"
		<< "  --bench-threads <int>     Parallel workers for benchmark cases (default: 1)\n\n"
		<< "Shared options:\n"
		<< "  --p-min <int>             Min processing time (default: 1)\n"
		<< "  --p-max <int>             Max processing time (default: 100)\n"
		<< "  --due-range <double>      Potts R parameter (default: 0.2)\n"
		<< "  --due-tardiness <double>  Potts T parameter (default: 0.6)\n"
		<< "  --memo-capacity <size>    Max memo entries, 0 = unlimited (default: 0)\n"
		<< "  --mem-budget-mb <size>    Memo memory budget in MB (default: 1024)\n"
		<< "  --no-lb                   Disable lower-bound pruning (default)\n"
		<< "  --use-lb                  Enable lower-bound pruning\n"
		<< "  --no-decomp2              Disable decomposition-2 branching\n"
		<< "  --decomp2                 Enable decomposition-2 branching (default)\n"
		<< "  --double-lb-prune         Extra LB pruning inside double decomposition (experimental)\n"
		<< "  --no-double-lb-prune      Disable extra LB pruning inside double decomposition (default)\n"
		<< "  --lufo-protect-exact      LUFO: age exact memo entries slower (experimental)\n"
		<< "  --no-lufo-protect-exact   Disable LUFO exact-entry protection (default)\n"
		<< "  --no-lawler-filter        Disable Lawler position filter\n"
		<< "  --lawler-filter           Enable Lawler position filter (default)\n"
		<< "  --lawler-rule12          Enable additional proven Lawler rules 1/2 (experimental exact-safe)\n"
		<< "  --no-lawler-rule12       Disable additional proven Lawler rules 1/2 (default)\n"
		<< "  --decomp-policy <mode>    Decomposition policy: adaptive|lawler|szwarc|both (default: adaptive)\n"
		<< "  --process-memory-gate     Cap memo by process working set too (off by default)\n"
		<< "  --profiling              Enable internal profiling counters/timers (default: on)\n"
		<< "  --no-profiling           Disable internal profiling timers for cleaner speed benchmarks\n"
		<< "  --profile-article         Preset: R=0.2 T=0.6 no-LB rule12-off decomp2+LawlerFilter adaptive memo-capacity=0\n"
		<< "  --reconstruct             Enable order reconstruction (default: off)\n"
		<< "  --no-reconstruct          Disable order reconstruction\n"
		<< "  --help                    Show this help\n";
}

bool parse_int_arg(const char* text, int& out) {
	if (text == nullptr || *text == '\0') {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const long value = std::strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0') {
		return false;
	}
	if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
		return false;
	}
	out = static_cast<int>(value);
	return true;
}

bool parse_u64_arg(const char* text, std::uint64_t& out) {
	if (text == nullptr || *text == '\0' || *text == '-') {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const unsigned long long value = std::strtoull(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0') {
		return false;
	}
	out = static_cast<std::uint64_t>(value);
	return true;
}

bool parse_size_arg(const char* text, std::size_t& out) {
	std::uint64_t temp = 0;
	if (!parse_u64_arg(text, temp)) {
		return false;
	}
	if (temp > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		return false;
	}
	out = static_cast<std::size_t>(temp);
	return true;
}

bool parse_double_arg(const char* text, double& out) {
	if (text == nullptr || *text == '\0') {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const double value = std::strtod(text, &end);
	if (errno != 0 || end == text || *end != '\0') {
		return false;
	}
	out = value;
	return true;
}

std::string trim_copy(std::string text) {
	const auto not_space = [](unsigned char c) { return !std::isspace(c); };
	const auto begin = std::find_if(text.begin(), text.end(), not_space);
	if (begin == text.end()) {
		return "";
	}
	const auto end = std::find_if(text.rbegin(), text.rend(), not_space).base();
	return std::string(begin, end);
}

bool parse_n_list(const std::string& raw, std::vector<int>& out, std::string& error) {
	std::string normalized = raw;
	for (char& ch : normalized) {
		if (ch == ';') {
			ch = ',';
		}
	}

	std::stringstream ss(normalized);
	std::string token;
	while (std::getline(ss, token, ',')) {
		const std::string t = trim_copy(token);
		if (t.empty()) {
			continue;
		}
		int v = 0;
		if (!parse_int_arg(t.c_str(), v) || v <= 0) {
			error = "Invalid n value in --n-list: " + t;
			return false;
		}
		out.push_back(v);
	}

	if (out.empty()) {
		error = "Empty --n-list.";
		return false;
	}
	return true;
}

bool parse_line_one_or_two_ints(const std::string& line, long long& a, long long& b, int& count) {
	a = 0;
	b = 0;
	count = 0;

	std::istringstream ss(line);
	char extra = '\0';
	if (!(ss >> a)) {
		return false;
	}
	count = 1;
	if (ss >> b) {
		count = 2;
	}
	if (ss >> extra) {
		return false;
	}
	return true;
}

bool load_instance_from_file_auto(const std::string& path, instance& out, std::string& error) {
	std::ifstream file(path);
	if (!file) {
		error = "Cannot open input file: " + path;
		return false;
	}

	std::vector<std::string> lines;
	std::string line;
	while (std::getline(file, line)) {
		const std::string trimmed = trim_copy(line);
		if (trimmed.empty()) {
			continue;
		}
		if (!trimmed.empty() && trimmed.front() == '#') {
			continue;
		}
		lines.push_back(trimmed);
	}

	if (lines.empty()) {
		error = "Input file is empty: " + path;
		return false;
	}

	long long first_a = 0;
	long long first_b = 0;
	int first_count = 0;
	if (!parse_line_one_or_two_ints(lines.front(), first_a, first_b, first_count)) {
		error = "Unsupported first line format in input file: " + path;
		return false;
	}

	bool sdt_pd_only = (first_count == 2);
	if (sdt_pd_only) {
		for (std::size_t i = 0; i < lines.size(); ++i) {
			long long a = 0;
			long long b = 0;
			int count = 0;
			if (!parse_line_one_or_two_ints(lines[i], a, b, count) || count != 2) {
				error = "Mixed/invalid SDT row format at logical line " + std::to_string(i + 1) + ".";
				return false;
			}
		}

		std::ostringstream normalized;
		normalized << lines.size() << "\n";
		for (const std::string& row : lines) {
			normalized << row << "\n";
		}

		std::string parse_error;
		std::istringstream in(normalized.str());
		if (!parse_instance(in, out, &parse_error)) {
			error = parse_error.empty() ? ("Failed to parse SDT file: " + path) : parse_error;
			return false;
		}
		return true;
	}

	std::ostringstream normalized;
	for (const std::string& row : lines) {
		normalized << row << "\n";
	}
	std::string parse_error;
	std::istringstream in(normalized.str());
	if (!parse_instance(in, out, &parse_error)) {
		error = parse_error.empty() ? ("Failed to parse instance file: " + path) : parse_error;
		return false;
	}
	return true;
}

bool parse_cli(int argc, char** argv, cli_options& opts, std::string& error, bool& show_help) {
	show_help = false;

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			show_help = true;
			return true;
		}
		if (arg == "--no-reconstruct") {
			opts.reconstruct_order = false;
			continue;
		}
		if (arg == "--reconstruct") {
			opts.reconstruct_order = true;
			continue;
		}
		if (arg == "--no-lb") {
			opts.use_lower_bounds = false;
			continue;
		}
		if (arg == "--use-lb") {
			opts.use_lower_bounds = true;
			continue;
		}
		if (arg == "--no-decomp2") {
			opts.use_decomposition2 = false;
			continue;
		}
		if (arg == "--decomp2") {
			opts.use_decomposition2 = true;
			continue;
		}
		if (arg == "--no-lawler-filter") {
			opts.use_lawler_position_filter = false;
			continue;
		}
		if (arg == "--lawler-filter") {
			opts.use_lawler_position_filter = true;
			continue;
		}
		if (arg == "--lawler-rule12") {
			opts.use_lawler_rule12 = true;
			continue;
		}
		if (arg == "--no-lawler-rule12") {
			opts.use_lawler_rule12 = false;
			continue;
		}
		if (arg == "--process-memory-gate") {
			opts.use_process_memory_gate = true;
			continue;
		}
		if (arg == "--double-lb-prune") {
			opts.use_double_pair_lb_prune = true;
			continue;
		}
		if (arg == "--no-double-lb-prune") {
			opts.use_double_pair_lb_prune = false;
			continue;
		}
		if (arg == "--lufo-protect-exact") {
			opts.use_lufo_exact_protect = true;
			continue;
		}
		if (arg == "--no-lufo-protect-exact") {
			opts.use_lufo_exact_protect = false;
			continue;
		}
		if (arg == "--profiling") {
			opts.profiling_enabled = true;
			continue;
		}
		if (arg == "--no-profiling") {
			opts.profiling_enabled = false;
			continue;
		}
		if (arg == "--profile-article") {
			opts.due_range = 0.2;
			opts.due_tardiness = 0.6;
			opts.use_lower_bounds = false;
			opts.use_decomposition2 = true;
			opts.use_lawler_position_filter = true;
			opts.use_lawler_rule12 = false;
			opts.decomp_policy = decomposition_policy::adaptive;
			opts.memo_capacity = 0;
			continue;
		}

		if (i + 1 >= argc) {
			error = "Missing value for argument: " + arg;
			return false;
		}
		const char* value = argv[++i];

		if (arg == "--bench-csv") {
			opts.benchmark = true;
			opts.bench_csv_path = value;
		}
		else if (arg == "--n") {
			if (!parse_int_arg(value, opts.n)) {
				error = "Invalid value for --n.";
				return false;
			}
		}
		else if (arg == "--n-from") {
			if (!parse_int_arg(value, opts.n_from)) {
				error = "Invalid value for --n-from.";
				return false;
			}
			opts.n_from_set = true;
		}
		else if (arg == "--n-to") {
			if (!parse_int_arg(value, opts.n_to)) {
				error = "Invalid value for --n-to.";
				return false;
			}
			opts.n_to_set = true;
		}
		else if (arg == "--n-step") {
			if (!parse_int_arg(value, opts.n_step)) {
				error = "Invalid value for --n-step.";
				return false;
			}
		}
		else if (arg == "--n-list") {
			opts.n_list_text = value;
		}
		else if (arg == "--instances") {
			if (!parse_int_arg(value, opts.instances)) {
				error = "Invalid value for --instances.";
				return false;
			}
		}
		else if (arg == "--bench-threads") {
			if (!parse_int_arg(value, opts.bench_threads)) {
				error = "Invalid value for --bench-threads.";
				return false;
			}
		}
		else if (arg == "--seed") {
			if (!parse_u64_arg(value, opts.seed)) {
				error = "Invalid value for --seed.";
				return false;
			}
		}
		else if (arg == "--input") {
			opts.input_path = value;
		}
		else if (arg == "--p-min") {
			if (!parse_int_arg(value, opts.p_min)) {
				error = "Invalid value for --p-min.";
				return false;
			}
		}
		else if (arg == "--p-max") {
			if (!parse_int_arg(value, opts.p_max)) {
				error = "Invalid value for --p-max.";
				return false;
			}
		}
		else if (arg == "--due-range") {
			if (!parse_double_arg(value, opts.due_range)) {
				error = "Invalid value for --due-range.";
				return false;
			}
		}
		else if (arg == "--due-tardiness") {
			if (!parse_double_arg(value, opts.due_tardiness)) {
				error = "Invalid value for --due-tardiness.";
				return false;
			}
		}
		else if (arg == "--memo-capacity") {
			if (!parse_size_arg(value, opts.memo_capacity)) {
				error = "Invalid value for --memo-capacity.";
				return false;
			}
		}
		else if (arg == "--mem-budget-mb") {
			if (!parse_size_arg(value, opts.mem_budget_mb)) {
				error = "Invalid value for --mem-budget-mb.";
				return false;
			}
		}
		else if (arg == "--decomp-policy") {
			if (!parse_decomposition_policy(value, opts.decomp_policy)) {
				error = "Invalid value for --decomp-policy (expected adaptive|lawler|szwarc|both).";
				return false;
			}
		}
		else {
			error = "Unknown argument: " + arg;
			return false;
		}
	}

	if (opts.n <= 0) {
		error = "--n must be > 0.";
		return false;
	}
	if (opts.instances <= 0) {
		error = "--instances must be > 0.";
		return false;
	}
	if (opts.bench_threads <= 0) {
		error = "--bench-threads must be > 0.";
		return false;
	}
	if (opts.n_step <= 0) {
		error = "--n-step must be > 0.";
		return false;
	}
	if (opts.benchmark && opts.bench_csv_path.empty()) {
		error = "--bench-csv path is empty.";
		return false;
	}
	if (opts.benchmark && !opts.input_path.empty()) {
		error = "--input is only supported in single-run mode (without --bench-csv).";
		return false;
	}
	return true;
}

std::vector<int> build_n_values(const cli_options& opts, std::string& error) {
	std::vector<int> values;

	if (!opts.n_list_text.empty()) {
		if (!parse_n_list(opts.n_list_text, values, error)) {
			return {};
		}
		return values;
	}

	if (!opts.benchmark) {
		values.push_back(opts.n);
		return values;
	}

	int from = opts.n_from_set ? opts.n_from : opts.n;
	int to = opts.n_to_set ? opts.n_to : opts.n;
	if (from <= 0 || to <= 0) {
		error = "--n-from and --n-to must be > 0.";
		return {};
	}
	if (from > to) {
		error = "--n-from must be <= --n-to.";
		return {};
	}
	for (int n = from; n <= to; n += opts.n_step) {
		values.push_back(n);
	}
	if (values.empty()) {
		error = "No n values to run.";
	}
	return values;
}

std::string csv_escape(const std::string& text) {
	bool need_quotes = false;
	for (char c : text) {
		if (c == '"' || c == ',' || c == '\n' || c == '\r') {
			need_quotes = true;
			break;
		}
	}
	if (!need_quotes) {
		return text;
	}
	std::string out;
	out.reserve(text.size() + 2);
	out.push_back('"');
	for (char c : text) {
		if (c == '"') {
			out.push_back('"');
		}
		out.push_back(c);
	}
	out.push_back('"');
	return out;
}

dfs_config make_solver_config(const cli_options& opts) {
	dfs_config cfg{};
	cfg.memo_capacity = opts.memo_capacity;
	cfg.memo_memory_budget_bytes = opts.mem_budget_mb * static_cast<std::size_t>(1024) * static_cast<std::size_t>(1024);
	cfg.reconstruct_order = opts.reconstruct_order;
	cfg.strict_memory_cap = true;
	cfg.use_lower_bounds = opts.use_lower_bounds;
	cfg.use_decomposition2 = opts.use_decomposition2;
	cfg.use_lawler_position_filter = opts.use_lawler_position_filter;
	cfg.use_lawler_rule12 = opts.use_lawler_rule12;
	cfg.use_process_memory_gate = opts.use_process_memory_gate;
	cfg.use_double_pair_lb_prune = opts.use_double_pair_lb_prune;
	cfg.use_lufo_exact_protection = opts.use_lufo_exact_protect;
	cfg.decomp_policy = opts.decomp_policy;
	cfg.profiling.enabled = opts.profiling_enabled;
	return cfg;
}

void write_csv_header(std::ofstream& out) {
	out
		<< "status,error"
		<< ",n,instance_idx,seed,memo_capacity,mem_budget_mb"
		<< ",p_min,p_max,due_range,due_tardiness,reconstruct_order"
		<< ",use_lb,use_decomp2,use_lawler_filter,process_memory_gate,decomp_policy"
		<< ",cost,time_ms,nodes,leaves,pruned_by_bound,pruned_by_memo_exact,pruned_by_memo_lb"
		<< ",max_depth,ordering_scans,ordering_sorts,valid_positions_built,valid_positions_pruned_3a,valid_positions_pruned_3b"
		<< ",memo_hits,memo_misses,memo_inserts,memo_updates,memo_evictions,memo_rejected_no_room,memo_forced_evictions"
		<< ",memo_clean_calls,memo_lufo_decay_passes"
		<< ",duplicate_subproblem_hits,hash_collisions,full_key_rechecks"
		<< ",memo_peak_size,memo_final_size,memo_used_bytes,memo_budget_bytes"
		<< ",bound_time_ms,ordering_time_ms,valid_positions_time_ms,memo_lookup_time_ms,memo_store_time_ms,memo_clean_time_ms,order_size"
		<< "\n";
}

void write_csv_error_row(std::ofstream& out, const cli_options& opts, int n, int instance_idx,
	std::uint64_t seed, const std::string& error_text) {
	out
		<< "error," << csv_escape(error_text)
		<< "," << n
		<< "," << instance_idx
		<< "," << seed
		<< "," << opts.memo_capacity
		<< "," << opts.mem_budget_mb
		<< "," << opts.p_min
		<< "," << opts.p_max
		<< "," << opts.due_range
		<< "," << opts.due_tardiness
		<< "," << (opts.reconstruct_order ? 1 : 0)
		<< "," << (opts.use_lower_bounds ? 1 : 0)
		<< "," << (opts.use_decomposition2 ? 1 : 0)
		<< "," << (opts.use_lawler_position_filter ? 1 : 0)
		<< "," << (opts.use_process_memory_gate ? 1 : 0)
		<< "," << to_string(opts.decomp_policy);
	for (int i = 0; i < 36; ++i) {
		out << ",0";
	}
	out << "\n";
}

void write_csv_ok_row(std::ofstream& out, const cli_options& opts, int n, int instance_idx,
	std::uint64_t seed, const solve_result& result) {
	out
		<< "ok,"
		<< ","
		<< n
		<< "," << instance_idx
		<< "," << seed
		<< "," << opts.memo_capacity
		<< "," << opts.mem_budget_mb
		<< "," << opts.p_min
		<< "," << opts.p_max
		<< "," << opts.due_range
		<< "," << opts.due_tardiness
		<< "," << (opts.reconstruct_order ? 1 : 0)
		<< "," << (opts.use_lower_bounds ? 1 : 0)
		<< "," << (opts.use_decomposition2 ? 1 : 0)
		<< "," << (opts.use_lawler_position_filter ? 1 : 0)
		<< "," << (opts.use_process_memory_gate ? 1 : 0)
		<< "," << to_string(opts.decomp_policy)
		<< "," << result.best.cost
		<< "," << result.stats.elapsed_ms
		<< "," << result.stats.nodes
		<< "," << result.stats.leaves
		<< "," << result.stats.pruned_by_bound
		<< "," << result.stats.pruned_by_memo_exact
		<< "," << result.stats.pruned_by_memo_lb
		<< "," << result.stats.max_depth
		<< "," << result.stats.ordering_scans
		<< "," << result.stats.ordering_sorts
		<< "," << result.stats.valid_positions_built
		<< "," << result.stats.valid_positions_pruned_3a
		<< "," << result.stats.valid_positions_pruned_3b
		<< "," << result.stats.memo_hits
		<< "," << result.stats.memo_misses
		<< "," << result.stats.memo_inserts
		<< "," << result.stats.memo_updates
		<< "," << result.stats.memo_evictions
		<< "," << result.stats.memo_rejected_no_room
		<< "," << result.stats.memo_forced_evictions
		<< "," << result.stats.memo_clean_calls
		<< "," << result.stats.memo_lufo_decay_passes
		<< "," << result.stats.duplicate_subproblem_hits
		<< "," << result.stats.hash_collisions
		<< "," << result.stats.full_key_rechecks
		<< "," << result.stats.memo_peak_size
		<< "," << result.stats.memo_final_size
		<< "," << result.stats.memo_used_bytes
		<< "," << result.stats.memo_budget_bytes
		<< "," << result.stats.bound_time_ms
		<< "," << result.stats.ordering_time_ms
		<< "," << result.stats.valid_positions_time_ms
		<< "," << result.stats.memo_lookup_time_ms
		<< "," << result.stats.memo_store_time_ms
		<< "," << result.stats.memo_clean_time_ms
		<< "," << result.best.order.size()
		<< "\n";
}

struct bench_case {
	int n = 0;
	int instance_idx = 0;
	std::uint64_t seed = 0;
};

struct bench_case_result {
	int n = 0;
	int instance_idx = 0;
	std::uint64_t seed = 0;
	bool generated = false;
	bool solved = false;
	std::string generation_error;
	solve_result result{};
	std::string solve_error;
};

bench_case_result execute_bench_case(
	const cli_options& opts,
	const bench_case& c,
	std::mutex& log_mutex) {
	bench_case_result out{};
	out.n = c.n;
	out.instance_idx = c.instance_idx;
	out.seed = c.seed;

	auto log_line = [&](const std::string& line) {
		std::lock_guard<std::mutex> lock(log_mutex);
		std::cout << line << "\n";
		};

	if (g_stop_requested) {
		out.generated = false;
		out.generation_error = "stopped_by_signal";
		return out;
	}

	instance inst;
	try {
		inst = generate_potts_instance(
			c.n, opts.p_min, opts.p_max, opts.due_range, opts.due_tardiness, c.seed);
		out.generated = true;
	}
	catch (const std::exception& ex) {
		out.generated = false;
		out.generation_error = std::string("generation_failed: ") + ex.what();
		return out;
	}

	if (g_stop_requested) {
		out.solved = false;
		out.solve_error = "stopped_by_signal";
		return out;
	}

	try {
		log_line(
			"[bench] start n=" + std::to_string(c.n) +
			" instance=" + std::to_string(c.instance_idx) +
			" seed=" + std::to_string(c.seed) +
			" decomp=" + to_string(opts.decomp_policy));

		dfs_solver solver(make_solver_config(opts));
		out.result = solver.solve(inst);
		out.solved = true;

		log_line(
			"[bench] done  n=" + std::to_string(c.n) +
			" instance=" + std::to_string(c.instance_idx) +
			" seed=" + std::to_string(c.seed) +
			" decomp=" + to_string(opts.decomp_policy) +
			" cost=" + std::to_string(out.result.best.cost) +
			" time_ms=" + std::to_string(out.result.stats.elapsed_ms) +
			" memo_rejected=" + std::to_string(out.result.stats.memo_rejected_no_room) +
			" memo_forced_evict=" + std::to_string(out.result.stats.memo_forced_evictions) +
			" memo_used_mb=" + std::to_string(
				static_cast<double>(out.result.stats.memo_used_bytes) / (1024.0 * 1024.0)));
	}
	catch (const std::exception& ex) {
		out.solved = false;
		out.solve_error = ex.what();
		log_line(
			"[bench] error n=" + std::to_string(c.n) +
			" instance=" + std::to_string(c.instance_idx) +
			" seed=" + std::to_string(c.seed) +
			" decomp=" + to_string(opts.decomp_policy) +
			" message=" + out.solve_error);
	}

	return out;
}

bool run_single(const cli_options& opts) {
	dfs_config cfg = make_solver_config(opts);
	instance inst;
	if (!opts.input_path.empty()) {
		std::string load_error;
		if (!load_instance_from_file_auto(opts.input_path, inst, load_error)) {
			std::cerr << load_error << "\n";
			return false;
		}
	}
	else {
		inst = generate_potts_instance(
			opts.n, opts.p_min, opts.p_max, opts.due_range, opts.due_tardiness, opts.seed);
	}

	dfs_solver solver(cfg);
	solve_result result = solver.solve(inst);

	std::cout
		<< "[run] n=" << inst.jobs.size()
		<< " seed=" << opts.seed
		<< (!opts.input_path.empty() ? " input=" + opts.input_path : std::string{})
		<< " decomp=" << to_string(opts.decomp_policy)
		<< " cost=" << result.best.cost
		<< " time_ms=" << result.stats.elapsed_ms
		<< " nodes=" << result.stats.nodes
		<< " max_depth=" << result.stats.max_depth
		<< " memo_hits=" << result.stats.memo_hits
		<< " memo_misses=" << result.stats.memo_misses
		<< " lawler_rule12=" << (opts.use_lawler_rule12 ? 1 : 0)
		<< " double_lb_prune=" << (opts.use_double_pair_lb_prune ? 1 : 0)
		<< " lufo_exact_protect=" << (opts.use_lufo_exact_protect ? 1 : 0)
		<< " valid_pos=" << result.stats.valid_positions_built
		<< " p3a=" << result.stats.valid_positions_pruned_3a
		<< " p3b=" << result.stats.valid_positions_pruned_3b
		<< " memo_rejected=" << result.stats.memo_rejected_no_room
		<< " memo_forced_evict=" << result.stats.memo_forced_evictions
		<< " lufo_passes=" << result.stats.memo_lufo_decay_passes
		<< " memo_peak=" << result.stats.memo_peak_size
		<< " memo_used_mb=" << (static_cast<double>(result.stats.memo_used_bytes) / (1024.0 * 1024.0))
		<< "\n";
	return true;
}

bool run_benchmark(const cli_options& opts, std::string& error) {
	std::vector<int> n_values = build_n_values(opts, error);
	if (n_values.empty()) {
		if (error.empty()) {
			error = "No n values to run.";
		}
		return false;
	}

	std::ofstream csv(opts.bench_csv_path, std::ios::out | std::ios::trunc);
	if (!csv) {
		error = "Cannot open CSV file: " + opts.bench_csv_path;
		return false;
	}
	write_csv_header(csv);

	std::cout << "[bench] Writing results to: " << opts.bench_csv_path << "\n";
	std::cout << "[bench] n-count=" << n_values.size()
		<< " instances=" << opts.instances
		<< " decomp=" << to_string(opts.decomp_policy)
		<< " threads=" << opts.bench_threads << "\n";
	std::cout << "[bench] Press Ctrl+C to stop safely after current case.\n";

	std::vector<bench_case> cases;
	cases.reserve(n_values.size() * static_cast<std::size_t>(opts.instances));

	std::uint64_t scenario_id = 0;
	for (int n : n_values) {
		for (int instance_idx = 0; instance_idx < opts.instances; ++instance_idx) {
			bench_case c{};
			c.n = n;
			c.instance_idx = instance_idx;
			c.seed = opts.seed + scenario_id;
			++scenario_id;
			cases.push_back(c);
		}
	}

	if (cases.empty()) {
		error = "No benchmark cases generated.";
		return false;
	}

	int workers = opts.bench_threads;
	if (workers > static_cast<int>(cases.size())) {
		workers = static_cast<int>(cases.size());
	}
	if (workers <= 0) {
		workers = 1;
	}

	std::mutex log_mutex;
	std::vector<bench_case_result> case_results;
	case_results.resize(cases.size());

	if (workers == 1) {
		for (std::size_t i = 0; i < cases.size(); ++i) {
			case_results[i] = execute_bench_case(opts, cases[i], log_mutex);
		}
	}
	else {
		ThreadPool pool(static_cast<std::size_t>(workers));
		std::vector<std::future<bench_case_result>> futures;
		futures.reserve(cases.size());

		for (const bench_case& c : cases) {
			futures.emplace_back(pool.enqueue([&opts, &log_mutex, c]() {
				return execute_bench_case(opts, c, log_mutex);
				}));
		}

		for (std::size_t i = 0; i < futures.size(); ++i) {
			case_results[i] = futures[i].get();
		}
	}

	for (const bench_case_result& r : case_results) {
		if (!r.generated) {
			const std::string msg = r.generation_error.empty() ? "generation_failed" : r.generation_error;
			write_csv_error_row(csv, opts, r.n, r.instance_idx, r.seed, msg);
			csv.flush();
			continue;
		}

		if (r.solved) {
			write_csv_ok_row(csv, opts, r.n, r.instance_idx, r.seed, r.result);
		}
		else {
			const std::string msg = r.solve_error.empty() ? "solver_failed" : r.solve_error;
			write_csv_error_row(csv, opts, r.n, r.instance_idx, r.seed, msg);
		}
		csv.flush();
	}

	if (g_stop_requested) {
		std::cout << "[bench] Stopped by user signal.\n";
	}
	else {
		std::cout << "[bench] Completed.\n";
	}
	return true;
}
} // namespace

int main(int argc, char** argv) {
	std::signal(SIGINT, on_interrupt_signal);
	try {
		cli_options opts{};
		std::string error;
		bool show_help = false;

		if (!parse_cli(argc, argv, opts, error, show_help)) {
			std::cerr << error << "\n\n";
			print_usage();
			return 2;
		}
		if (show_help) {
			print_usage();
			return 0;
		}

		if (opts.benchmark) {
			if (!run_benchmark(opts, error)) {
				std::cerr << error << "\n";
				return 2;
			}
		}
		else {
			if (!run_single(opts)) {
				return 2;
			}
		}
		return 0;
	}
	catch (const std::exception& ex) {
		std::cerr << ex.what() << "\n";
		return 3;
	}
}
