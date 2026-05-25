#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dfs_solver.h"
#include "generator.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {
namespace fs = std::filesystem;

// Ключ нужен только для resume: если строка с теми же n, R, T, seed и режимами уже есть
// в CSV, benchmark не запускает этот случай повторно. Так сохраняется воспроизводимость
// длинных экспериментов после остановки.
struct run_key {
	std::string series;
	std::string config;
	int n = 0;
	std::string r;
	std::string t;
	std::uint64_t seed = 0;

	std::string str() const {
		return series + "|" + config + "|" + std::to_string(n) + "|" + r + "|" + t + "|" + std::to_string(seed)
			+ "|" + rule4 + "|" + terminal_rules + "|" + memo_backend + "|" + adaptive_policy
			+ "|" + reconstruct_order + "|" + reconstruction_trace;
	}

	std::string rule4 = "true";
	std::string terminal_rules = "true";
	std::string memo_backend = "custom";
	std::string adaptive_policy = "v1";
	std::string reconstruct_order = "false";
	std::string reconstruction_trace = "false";
};

// Общие параметры запуска benchmark-а.
// n_values задаёт размеры задач, R/T — параметры генератора Potts,
// seeds фиксируют конкретные экземпляры, чтобы разные режимы решали один и тот же вход.
struct benchmark_options {
	std::string series = "branching";
	fs::path out = fs::path("results") / "branching_modes_results.csv";
	double time_limit_sec = 3600.0;
	double memory_limit_gb = -1.0;

	std::vector<int> n_values;
	std::vector<double> r_values;
	std::vector<double> t_values;
	std::vector<std::uint64_t> seeds;
	bool n_values_set = false;
	bool r_values_set = false;
	bool t_values_set = false;
	bool seeds_set = false;

	DecompositionMode decomposition_mode = DecompositionMode::Adaptive;
	std::string hard_pairs = "auto";
	fs::path hard_source_csv;
	int hard_auto_n = 600;
	bool use_lower_bounds = false;
	bool use_lower_bounds_set = false;
	bool use_upper_bounds = false;
	bool use_upper_bounds_set = false;
	bool use_terminal_rules = true;
	bool enable_rule4 = true;
	bool position_filtering_enabled = true;
	bool enable_lawler_basic_rules = true;
	int ub_depth_limit = -1;
	int lb_depth_limit = -1;
	bool enable_terminal_all_tardy_spt = true;
	bool enable_terminal_edd_at_most_one_tardy = true;
	std::string lower_bounds_mode = "baseline-lb";

	std::size_t memo_capacity = 0;
	bool memo_capacity_set = false;
	bool enable_memo = true;
	bool enable_exact_memo = true;
	bool memo_full_key_verification = true;
	bool use_process_memory_gate = true;
	bool reconstruct_order = false;
	bool reconstruction_trace = false;
	memo_backend_kind memo_backend = memo_backend_kind::custom;
	adaptive_policy_kind adaptive_policy = adaptive_policy_kind::v1;

	bool resume = true;
	bool append = true;
	bool progress = true;
	int limit = 0;
};

// Один сравниваемый режим solver-а внутри серии экспериментов.
// Например, серия branching сравнивает Lawler, Szwarc, Both и adaptive v1/v2/v3
// на одинаковых наборах (n, R, T, seed).
struct benchmark_config {
	std::string name;
	DecompositionMode policy = DecompositionMode::Adaptive;
	bool use_lower_bounds = false;
	bool use_upper_bounds = false;
	std::size_t memo_capacity = 0;
	std::string notes;
	bool use_terminal_rules = true;
	adaptive_policy_kind adaptive_policy = adaptive_policy_kind::v1;
};

// Одна строка будущего CSV: входные параметры, выбранный режим и метрики solver-а.
struct benchmark_row {
	std::string series;
	std::string config;
	int n = 0;
	double r = 0.0;
	double t = 0.0;
	std::uint64_t seed = 0;
	std::string status = "ERROR";
	std::string optimum;
	double time_ms = 0.0;
	solver_stats stats{};
	std::size_t memory_bytes_peak = 0;
	std::string best_first;
	DecompositionMode decomposition_mode = DecompositionMode::Adaptive;
	std::string model_name;
	std::string active_components;
	bool use_lower_bounds = false;
	bool use_upper_bounds = false;
	bool use_terminal_rules = true;
	std::size_t memo_capacity = 0;
	double time_limit_sec = 0.0;
	double memory_limit_gb = 0.0;
	memo_backend_kind memo_backend = memo_backend_kind::custom;
	bool enable_rule4 = true;
	bool position_filtering_enabled = true;
	bool enable_lawler_basic_rules = true;
	bool enable_simple_lb = false;
	bool enable_lb_memo = false;
	bool enable_edd_ub = false;
	int ub_depth_limit = -1;
	int lb_depth_limit = -1;
	bool enable_terminal_all_tardy_spt = true;
	bool enable_terminal_edd_at_most_one_tardy = true;
	bool enable_memo = true;
	bool enable_exact_memo = true;
	bool memo_full_key_verification = true;
	bool reconstruct_order = false;
	bool reconstruction_trace = false;
	adaptive_policy_kind adaptive_policy = adaptive_policy_kind::v1;
	std::string notes;
};

const std::vector<std::string> csv_header = {
	// Вход, режимы solver-а и поддерживаемые метрики одного запуска.
	"series",
	"config",
	"status",
	"n",
	"R",
	"T",
	"seed",
	"decomposition_mode",
	"model_name",
	"active_components",
	"memo_backend",
	"adaptive_policy",
	"objective",
	"time_ms",
	"nodes",
	"recursive_calls",
	"branches_generated",
	"branches_pruned",
	"max_depth",
	"lawler_nodes",
	"szwarc_nodes",
	"both_nodes",
	"adaptive_choices_lawler",
	"adaptive_choices_szwarc",
	"adaptive_choices_both",
	"adaptive_choice_lawler",
	"adaptive_choice_szwarc",
	"adaptive_v1_choices_lawler",
	"adaptive_v1_choices_szwarc",
	"adaptive_v1_choices_both",
	"adaptive_v2_choices_lawler",
	"adaptive_v2_choices_szwarc",
	"adaptive_v2_choices_both",
	"adaptive_v3_choices_lawler",
	"adaptive_v3_choices_szwarc",
	"adaptive_v3_choices_both",
	"adaptive_policy_used",
	"memo_stores_exact",
	"memo_stores_lb",
	"enable_simple_lb",
	"enable_lb_memo",
	"enable_edd_ub",
	"ub_depth_limit",
	"lb_depth_limit",
	"position_filtering_enabled",
	"enable_lawler_basic_rules",
	"enable_rule4",
	"enable_terminal_all_tardy_spt",
	"enable_terminal_edd_at_most_one_tardy",
	"enable_memo",
	"enable_exact_memo",
	"memo_full_key_verification",
	"reconstruct_order",
	"reconstruction_trace",
	"memo_capacity",
	"memo_memory_limit_mb",
	"memo_exact_queries",
	"memo_exact_hits",
	"memo_lb_queries",
	"memo_lb_hits",
	"memo_exact_stores",
	"memo_lb_stores",
	"memo_evictions_exact",
	"memo_evictions_lb",
	"memo_cleanup_calls",
	"memo_cleanup_time_ms",
	"cleanup_time_ms",
	"memo_memory_used_bytes",
	"memo_peak_size",
	"memo_final_size",
	"memo_rejected_no_room",
	"memo_forced_evictions",
	"reconstruction_time_ms",
	"reconstruction_steps",
	"reconstruction_current_exact_hits",
	"reconstruction_current_exact_misses",
	"reconstruction_child_exact_hits",
	"reconstruction_child_exact_misses",
	"reconstruction_repair_solves",
	"reconstruction_candidate_scans",
	"reconstruction_trace_stores",
	"reconstruction_trace_entries",
	"reconstruction_trace_hits",
	"reconstruction_trace_misses",
	"reconstruction_trace_terminal_hits",
	"reconstruction_trace_fallbacks",
	"simple_lb_calls",
	"simple_lb_prunes",
	"ub_calls",
	"ub_improvements",
	"bound_time_ms",
	"upper_bound_time_ms",
	"valid_positions_before",
	"valid_positions_after",
	"positions_pruned",
	"candidate_positions_before",
	"candidate_positions_after",
	"positions_pruned_by_lawler_basic",
	"positions_pruned_by_lawler_rule4",
	"positions_pruned_by_szwarc_rule4",
	"time_spent_in_position_filtering_ms",
	"terminal_all_tardy_spt_hits",
	"terminal_edd_one_tardy_hits",
	"terminal_time_ms",
	"memory_bytes_peak",
	"time_limit_sec",
	"memory_limit_gb",
	"notes"
};

void ensure_parent_directory(const fs::path& path);

std::string trim_copy(const std::string& text) {
	const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char c) {
		return std::isspace(c) != 0;
	});
	if (begin == text.end()) {
		return "";
	}
	const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) {
		return std::isspace(c) != 0;
	}).base();
	return std::string(begin, end);
}

std::string lowercase_copy(std::string text) {
	for (char& ch : text) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return text;
}

std::string format_double(double value) {
	if (!std::isfinite(value)) {
		return "";
	}
	const double scaled10 = value * 10.0;
	const double rounded10 = std::round(scaled10);
	std::ostringstream out;
	if (std::fabs(scaled10 - rounded10) < 1e-9) {
		out << std::fixed << std::setprecision(1) << value;
	}
	else {
		out << std::fixed << std::setprecision(10) << value;
		std::string s = out.str();
		while (!s.empty() && s.back() == '0') {
			s.pop_back();
		}
		if (!s.empty() && s.back() == '.') {
			s.push_back('0');
		}
		if (s == "-0.0") {
			s = "0.0";
		}
		return s;
	}
	std::string s = out.str();
	if (s == "-0.0") {
		s = "0.0";
	}
	return s;
}

template <class T>
std::string join_values(const std::vector<T>& values) {
	std::ostringstream out;
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i > 0) {
			out << ",";
		}
		out << values[i];
	}
	return out.str();
}

std::string csv_escape(const std::string& text) {
	bool quote = false;
	for (char c : text) {
		if (c == '"' || c == ',' || c == '\n' || c == '\r') {
			quote = true;
			break;
		}
	}
	if (!quote) {
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

std::vector<std::string> parse_csv_line(const std::string& line) {
	std::vector<std::string> fields;
	std::string field;
	bool in_quotes = false;
	for (std::size_t i = 0; i < line.size(); ++i) {
		const char c = line[i];
		if (in_quotes) {
			if (c == '"') {
				if (i + 1 < line.size() && line[i + 1] == '"') {
					field.push_back('"');
					++i;
				}
				else {
					in_quotes = false;
				}
			}
			else {
				field.push_back(c);
			}
		}
		else {
			if (c == '"') {
				in_quotes = true;
			}
			else if (c == ',') {
				fields.push_back(field);
				field.clear();
			}
			else {
				field.push_back(c);
			}
		}
	}
	fields.push_back(field);
	return fields;
}

bool parse_bool_value(const std::string& raw, bool& out) {
	const std::string v = lowercase_copy(trim_copy(raw));
	if (v == "1" || v == "true" || v == "yes" || v == "on") {
		out = true;
		return true;
	}
	if (v == "0" || v == "false" || v == "no" || v == "off") {
		out = false;
		return true;
	}
	return false;
}

template <class T, class Parser>
bool parse_list(const std::string& raw, std::vector<T>& out, Parser parser, std::string& error) {
	out.clear();
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
		T value{};
		if (!parser(t, value)) {
			error = "Invalid list value: " + t;
			return false;
		}
		out.push_back(value);
	}
	if (out.empty()) {
		error = "Empty list argument.";
		return false;
	}
	return true;
}

bool parse_int_token(const std::string& text, int& out) {
	char* end = nullptr;
	errno = 0;
	const long v = std::strtol(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0') {
		return false;
	}
	if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
		return false;
	}
	out = static_cast<int>(v);
	return true;
}

bool parse_u64_token(const std::string& text, std::uint64_t& out) {
	if (!text.empty() && text.front() == '-') {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const unsigned long long v = std::strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0') {
		return false;
	}
	out = static_cast<std::uint64_t>(v);
	return true;
}

bool parse_size_token(const std::string& text, std::size_t& out) {
	std::uint64_t v = 0;
	if (!parse_u64_token(text, v)) {
		return false;
	}
	if (v > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
		return false;
	}
	out = static_cast<std::size_t>(v);
	return true;
}

bool parse_double_token(const std::string& text, double& out) {
	char* end = nullptr;
	errno = 0;
	const double v = std::strtod(text.c_str(), &end);
	if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
		return false;
	}
	out = v;
	return true;
}

std::string model_name_text(DecompositionMode mode, adaptive_policy_kind adaptive_policy) {
	if (mode == DecompositionMode::Adaptive) {
		switch (adaptive_policy) {
		case adaptive_policy_kind::v2:
			return "adaptive_v2";
		case adaptive_policy_kind::v3:
			return "adaptive_v3";
		case adaptive_policy_kind::v1:
		default:
			return "adaptive_v1";
		}
	}
	return to_string(mode);
}

std::string active_components_text(DecompositionMode mode) {
	std::string components;
	switch (mode) {
	case DecompositionMode::Lawler:
		components = "lawler";
		break;
	case DecompositionMode::Szwarc:
		components = "szwarc";
		break;
	case DecompositionMode::BothLawlerSzwarc:
		components = "lawler+szwarc";
		break;
	case DecompositionMode::Adaptive:
		components = "adaptive(lawler,szwarc,both)";
		break;
	default:
		components = "unknown";
		break;
	}
	return components;
}

bool apply_model_name(const std::string& value, benchmark_options& opts) {
	std::string model = lowercase_copy(value);
	if (model == "adaptive_v1" || model == "adaptive-v1") {
		opts.decomposition_mode = DecompositionMode::Adaptive;
		opts.adaptive_policy = adaptive_policy_kind::v1;
		return true;
	}
	if (model == "adaptive_v2" || model == "adaptive-v2") {
		opts.decomposition_mode = DecompositionMode::Adaptive;
		opts.adaptive_policy = adaptive_policy_kind::v2;
		return true;
	}
	if (model == "adaptive_v3" || model == "adaptive-v3") {
		opts.decomposition_mode = DecompositionMode::Adaptive;
		opts.adaptive_policy = adaptive_policy_kind::v3;
		return true;
	}
	DecompositionMode parsed{};
	if (!parse_decomposition_mode(model, parsed)) {
		return false;
	}
	opts.decomposition_mode = parsed;
	return true;
}

void apply_best_final_preset(benchmark_options& opts) {
	opts.decomposition_mode = DecompositionMode::Adaptive;
	opts.adaptive_policy = adaptive_policy_kind::v3;
	opts.memo_backend = memo_backend_kind::custom;
	opts.memo_full_key_verification = true;
	opts.enable_memo = true;
	opts.enable_exact_memo = true;
}

bool apply_preset_name(const std::string& value, benchmark_options& opts) {
	const std::string preset = lowercase_copy(value);
	if (preset == "best-final" || preset == "best_final" || preset == "bestfinal") {
		apply_best_final_preset(opts);
		return true;
	}
	return false;
}

void print_usage() {
	std::cout
		<< "Usage:\n"
		<< "  solver_bench --series branching --out results/branching_modes_results.csv --time-limit-sec 3600\n"
		<< "  solver_bench --series lower-bounds --decomposition adaptive --out results/lower_bounds_results.csv --time-limit-sec 3600\n"
		<< "  solver_bench --series memory --decomposition adaptive --out results/memory_results.csv --time-limit-sec 3600\n"
		<< "  solver_bench --series hard --decomposition adaptive --hard-pairs auto --out results/hard_scaling_results.csv --time-limit-sec 12600\n\n"
		<< "Options:\n"
		<< "  --out <path>\n"
		<< "  --time-limit-sec <seconds>\n"
		<< "  --memory-limit-gb <gb>          Default is derived from installed RAM when available.\n"
		<< "  --n-values <csv>\n"
		<< "  --R-values <csv>\n"
		<< "  --T-values <csv>\n"
		<< "  --seeds <csv>\n"
		<< "  --preset best-final             Recommended final config: adaptive v3 + custom memo.\n"
		<< "  --model <name>                  Model alias: lawler|szwarc|both|adaptive_v1|adaptive_v2|adaptive_v3.\n"
		<< "  --decomposition lawler|szwarc|both|adaptive\n"
		<< "  --hard-pairs auto|R:T;R:T\n"
		<< "  --hard-source-csv <path>        Source CSV for --hard-pairs auto.\n"
		<< "  --hard-auto-n <n>               n used for hard-pair scoring (default: 600).\n"
		<< "  --use-lower-bounds true|false   Used by hard final config unless a series config overrides it.\n"
		<< "  --use-upper-bounds true|false   Used by hard final config unless a series config overrides it.\n"
		<< "  --terminal-rules true|false     Exact terminal rules (default true).\n"
		<< "  --enable-rule4 true|false      Rule 4 position reduction (default true).\n"
		<< "  --lower-bounds-mode baseline-lb|lb-only|ub-only|ub-lb|new-only|all\n"
		<< "  --memo-capacity <entries>       0 means unlimited by entry count.\n"
		<< "  --memo-backend custom|std_unordered Memo backend for comparison (default custom).\n"
		<< "  --enable-memo true|false        Toggle memoization globally (default true).\n"
		<< "  --enable-exact-memo true|false  Toggle exact memo entries (default true).\n"
		<< "  --memo-full-key-verification true|false Exact-safe memo key checking (default true).\n"
		<< "  --process-memory-gate true|false Also cap memo by process working set (default true).\n"
		<< "  --reconstruct true|false        Include order reconstruction in each run (default false).\n"
		<< "  --reconstruction-trace true|false Use split trace for reconstruction (default false).\n"
		<< "  --enable-position-filtering true|false Toggle all position filters.\n"
		<< "  --enable-lawler-basic-rules true|false Toggle Lawler position rules.\n"
		<< "  --ub-depth-limit <int>          Limit UB calls by recursion depth, -1 = unlimited.\n"
		<< "  --lb-depth-limit <int>          Limit LB calls by recursion depth, -1 = unlimited.\n"
	<< "  --enable-terminal-all-tardy-spt true|false\n"
	<< "  --enable-terminal-edd-at-most-one-tardy true|false\n"
		<< "  --adaptive-policy v1|v2|v3       Used by single-model series; branching runs v1/v2/v3 together.\n"
	<< "  --resume true|false\n"
		<< "  --append true|false\n"
		<< "  --progress true|false\n"
		<< "  --limit <int>                   Cap on generated runs for short checks.\n";
}

double default_memory_limit_gb() {
#ifdef _WIN32
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status) != 0) {
		const double total_gb = static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
		if (total_gb <= 20.0) {
			return 12.0;
		}
		if (total_gb <= 40.0) {
			return 24.0;
		}
		if (total_gb <= 80.0) {
			return 48.0;
		}
		return std::floor(total_gb * 0.75);
	}
#endif
	return 0.0;
}

std::size_t gb_to_bytes(double gb) {
	if (gb <= 0.0 || !std::isfinite(gb)) {
		return 0;
	}
	const long double bytes = static_cast<long double>(gb) * 1024.0L * 1024.0L * 1024.0L;
	if (bytes > static_cast<long double>(std::numeric_limits<std::size_t>::max())) {
		return std::numeric_limits<std::size_t>::max();
	}
	return static_cast<std::size_t>(bytes);
}

std::size_t current_process_peak_memory_bytes() {
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX pmc{};
	if (GetProcessMemoryInfo(GetCurrentProcess(),
		reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
		sizeof(pmc)) == 0) {
		return 0;
	}
	return static_cast<std::size_t>(pmc.PeakWorkingSetSize);
#else
	return 0;
#endif
}

std::string compiler_version() {
#if defined(__clang__)
	return std::string("clang ") + __clang_version__;
#elif defined(_MSC_VER)
	return "MSVC " + std::to_string(_MSC_VER);
#elif defined(__GNUC__)
	return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
	return "unknown";
#endif
}

std::string build_type() {
#ifdef NDEBUG
	return "Release/NDEBUG";
#else
	return "Debug";
#endif
}

std::string get_env_var(const char* name) {
#ifdef _WIN32
	char* value = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
		return "";
	}
	std::string out(value);
	std::free(value);
	return out;
#else
	const char* value = std::getenv(name);
	return value == nullptr ? std::string{} : std::string(value);
#endif
}

std::string capture_command(const std::string& cmd) {
#ifdef _WIN32
	FILE* pipe = _popen(cmd.c_str(), "r");
#else
	FILE* pipe = popen(cmd.c_str(), "r");
#endif
	if (pipe == nullptr) {
		return "";
	}
	std::string output;
	char buffer[256];
	while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
		output += buffer;
	}
#ifdef _WIN32
	_pclose(pipe);
#else
	pclose(pipe);
#endif
	return trim_copy(output);
}

void write_metadata(const benchmark_options& opts) {
	ensure_parent_directory(opts.out);
	const fs::path metadata_path = opts.out.parent_path() / "metadata.txt";
	std::ofstream out(metadata_path, std::ios::out | std::ios::app);
	if (!out) {
		return;
	}

	const auto now = std::chrono::system_clock::now();
	const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
	std::tm local_tm{};
#ifdef _WIN32
	localtime_s(&local_tm, &now_time);
#else
	localtime_r(&now_time, &local_tm);
#endif
	out << "=== solver_bench run ===\n";
	out << "date_local=" << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S") << "\n";
	const std::string git_commit = capture_command("git rev-parse HEAD 2>nul");
	out << "git_commit=" << (git_commit.empty() ? "unavailable" : git_commit) << "\n";
	out << "compiler=" << compiler_version() << "\n";
#ifdef _WIN32
	out << "os=Windows\n";
	const std::string cpu = get_env_var("PROCESSOR_IDENTIFIER");
	out << "cpu=" << (cpu.empty() ? "unavailable" : cpu) << "\n";
	MEMORYSTATUSEX status{};
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status) != 0) {
		out << "ram_bytes=" << static_cast<unsigned long long>(status.ullTotalPhys) << "\n";
	}
	else {
		out << "ram_bytes=unavailable\n";
	}
#else
	out << "os=non-windows\n";
	out << "cpu=unavailable\n";
	out << "ram_bytes=unavailable\n";
#endif
	out << "build_type=" << build_type() << "\n";
	out << "series=" << opts.series << "\n";
	out << "out=" << opts.out.string() << "\n";
	out << "n_values=" << join_values(opts.n_values) << "\n";
	out << "R_values=" << join_values(opts.r_values) << "\n";
	out << "T_values=" << join_values(opts.t_values) << "\n";
	out << "seeds=" << join_values(opts.seeds) << "\n";
	out << "time_limit_sec=" << opts.time_limit_sec << "\n";
	out << "memory_limit_gb=" << opts.memory_limit_gb << "\n";
	out << "memo_capacity=" << opts.memo_capacity << " (0 means unlimited by entry count)\n";
	out << "memo_backend=" << to_string(opts.memo_backend) << "\n";
	out << "enable_memo=" << (opts.enable_memo ? "true" : "false") << "\n";
	out << "enable_exact_memo=" << (opts.enable_exact_memo ? "true" : "false") << "\n";
	out << "memo_full_key_verification=" << (opts.memo_full_key_verification ? "true" : "false") << "\n";
	out << "process_memory_gate=" << (opts.use_process_memory_gate ? "true" : "false") << "\n";
	out << "reconstruct_order=" << (opts.reconstruct_order ? "true" : "false") << "\n";
	out << "reconstruction_trace=" << (opts.reconstruction_trace ? "true" : "false") << "\n";
	out << "model_name=" << model_name_text(opts.decomposition_mode, opts.adaptive_policy) << "\n";
	out << "active_components=" << active_components_text(opts.decomposition_mode) << "\n";
	out << "adaptive_policy=" << to_string(opts.adaptive_policy) << "\n";
	out << "position_filtering_enabled=" << (opts.position_filtering_enabled ? "true" : "false") << "\n";
	out << "enable_lawler_basic_rules=" << (opts.enable_lawler_basic_rules ? "true" : "false") << "\n";
	out << "rule4=" << (opts.enable_rule4 ? "true" : "false") << "\n";
	out << "terminal_rules=" << (opts.use_terminal_rules ? "true" : "false") << "\n";
	out << "terminal_all_tardy_spt=" << (opts.enable_terminal_all_tardy_spt ? "true" : "false") << "\n";
	out << "terminal_edd_at_most_one_tardy=" << (opts.enable_terminal_edd_at_most_one_tardy ? "true" : "false") << "\n";
	out << "ub_depth_limit=" << opts.ub_depth_limit << "\n";
	out << "lb_depth_limit=" << opts.lb_depth_limit << "\n";
	out << "lower_bounds_mode=" << opts.lower_bounds_mode << "\n";
	out << "generator=generate_potts_instance_article; p_j~U{1,...,100}; d_j~U[p(N)*(1-T-R/2),p(N)*(1-T+R/2)] then negative due dates are clamped to 0\n";
	out << "\n";
}

void set_default_grid(benchmark_options& opts) {
	// Стандартная сетка повторяет типичный план экспериментов:
	// R — разброс due dates, T — фактор запаздывания, seeds — фиксированные экземпляры.
	if (!opts.r_values_set) {
		opts.r_values = { 0.2, 0.4, 0.6, 0.8, 1.0 };
	}
	if (!opts.t_values_set) {
		opts.t_values = { 0.2, 0.4, 0.6, 0.8 };
	}
	if (!opts.seeds_set) {
		opts.seeds = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	}
	if (!opts.n_values_set) {
		if (opts.series == "memory") {
			opts.n_values = { 300, 500, 700, 900 };
		}
		else if (opts.series == "hard") {
			opts.n_values = { 900, 1000, 1100, 1200, 1300, 1400, 1500 };
		}
		else {
			opts.n_values = { 50, 100, 200, 300, 400, 500, 600, 700, 800, 900 };
		}
	}
	if (opts.memory_limit_gb < 0.0) {
		opts.memory_limit_gb = default_memory_limit_gb();
	}
}

std::vector<benchmark_config> make_series_configs(const benchmark_options& opts) {
	const std::size_t default_cap = opts.memo_capacity;
	if (opts.series == "branching") {
		// Сравниваем только порядок декомпозиции: Lawler, Szwarc, Both
		// и три версии adaptive-эвристики на одних и тех же экземплярах.
		std::vector<benchmark_config> configs = {
			{ "Lawler", DecompositionMode::Lawler, false, false, default_cap, "" },
			{ "Szwarc", DecompositionMode::Szwarc, false, false, default_cap, "" },
			{ "Both", DecompositionMode::BothLawlerSzwarc, false, false, default_cap, "" },
			{ "Adaptive_v1", DecompositionMode::Adaptive, false, false, default_cap, "" },
			{ "Adaptive_v2", DecompositionMode::Adaptive, false, false, default_cap, "" },
			{ "Adaptive_v3", DecompositionMode::Adaptive, false, false, default_cap, "" }
		};
		configs[3].adaptive_policy = adaptive_policy_kind::v1;
		configs[4].adaptive_policy = adaptive_policy_kind::v2;
		configs[5].adaptive_policy = adaptive_policy_kind::v3;
		return configs;
	}
	if (opts.series == "lower-bounds") {
		// Сравниваем влияние дешёвых LB/UB на тот же базовый режим декомпозиции.
		std::vector<benchmark_config> configs;
		const std::string mode = opts.lower_bounds_mode;
		if (mode == "baseline-lb" || mode == "all") {
			configs.push_back({ "Best", opts.decomposition_mode, false, false, default_cap, "" });
			configs.back().adaptive_policy = opts.adaptive_policy;
		}
		if (mode == "ub-only" || mode == "new-only" || mode == "all") {
			configs.push_back({ "Best+UB", opts.decomposition_mode, false, true, default_cap, "" });
			configs.back().adaptive_policy = opts.adaptive_policy;
		}
		if (mode == "baseline-lb" || mode == "lb-only" || mode == "new-only" || mode == "all") {
			configs.push_back({ "Best+LB", opts.decomposition_mode, true, false, default_cap, "" });
			configs.back().adaptive_policy = opts.adaptive_policy;
		}
		if (mode == "ub-lb" || mode == "new-only" || mode == "all") {
			configs.push_back({ "Best+UB+LB", opts.decomposition_mode, true, true, default_cap, "" });
			configs.back().adaptive_policy = opts.adaptive_policy;
		}
		if (configs.empty()) {
			throw std::runtime_error("Invalid lower-bounds mode: " + opts.lower_bounds_mode);
		}
		return configs;
	}
	if (opts.series == "memory") {
		// Проверяем чувствительность к лимиту числа memo entries.
		std::vector<benchmark_config> configs = {
			{ "capacity_50000", opts.decomposition_mode, false, false, 50000, "" },
			{ "capacity_200000", opts.decomposition_mode, false, false, 200000, "" },
			{ "capacity_1000000", opts.decomposition_mode, false, false, 1000000, "" },
			{ "capacity_unlimited", opts.decomposition_mode, false, false, 0, "memo_capacity=0 means unlimited by entry count" }
		};
		for (benchmark_config& config : configs) {
			config.adaptive_policy = opts.adaptive_policy;
		}
		return configs;
	}
	if (opts.series == "hard") {
		// Финальный прогон на выбранных сложных парах (R,T) и выбранной лучшей политике.
		std::string name = std::string("BestFinal_") +
			model_name_text(opts.decomposition_mode, opts.adaptive_policy);
		if (opts.use_upper_bounds) {
			name += "_UB";
		}
		if (opts.use_lower_bounds) {
			name += "_LB";
		}
		name += "_cap" + std::to_string(default_cap);
		std::vector<benchmark_config> configs = {
			{ name, opts.decomposition_mode, opts.use_lower_bounds, opts.use_upper_bounds, default_cap, "" }
		};
		configs.front().adaptive_policy = opts.adaptive_policy;
		return configs;
	}
	throw std::runtime_error("Unknown series: " + opts.series);
}

dfs_config make_solver_config(const benchmark_options& opts, const benchmark_config& config) {
	dfs_config cfg{};
	cfg.memo.capacity = config.memo_capacity;
	cfg.memo.memory_limit_bytes = gb_to_bytes(opts.memory_limit_gb);
	cfg.zobrist_seed = 1;
	cfg.time_limit_sec = opts.time_limit_sec;
	cfg.reconstruct_order = opts.reconstruct_order;
	cfg.reconstruction_trace = opts.reconstruction_trace;
	cfg.bounds.enable_simple_lb = config.use_lower_bounds;
	cfg.bounds.enable_lb_memo = config.use_lower_bounds;
	cfg.bounds.enable_edd_ub = config.use_upper_bounds;
	cfg.bounds.ub_depth_limit = opts.ub_depth_limit;
	cfg.bounds.lb_depth_limit = opts.lb_depth_limit;
	cfg.terminal_rules.enable_all_tardy_spt =
		config.use_terminal_rules && opts.use_terminal_rules && opts.enable_terminal_all_tardy_spt;
	cfg.terminal_rules.enable_edd_at_most_one_tardy =
		config.use_terminal_rules && opts.use_terminal_rules && opts.enable_terminal_edd_at_most_one_tardy;
	cfg.position_filtering.enabled = opts.position_filtering_enabled;
	cfg.position_filtering.enable_lawler_basic_rules = opts.enable_lawler_basic_rules;
	cfg.position_filtering.enable_rule4 = opts.enable_rule4;
	cfg.memo.use_process_memory_gate =
		opts.use_process_memory_gate && cfg.memo.memory_limit_bytes > 0;
	cfg.memo.enable_memo = opts.enable_memo;
	cfg.memo.enable_exact_memo = opts.enable_exact_memo;
	cfg.memo.enable_lb_memo = config.use_lower_bounds;
	cfg.memo.full_key_verification = opts.memo_full_key_verification;
	cfg.memo.strict_memory_cap = true;
	cfg.memo.backend = opts.memo_backend;
	cfg.decomposition_mode = config.policy;
	cfg.adaptive_policy = config.adaptive_policy;
	cfg.profiling.enabled = true;
	return cfg;
}

benchmark_row execute_case(const benchmark_options& opts, const benchmark_config& config,
	int n, double r, double t, std::uint64_t seed) {
	benchmark_row row{};
	row.series = opts.series;
	row.config = config.name;
	row.n = n;
	row.r = r;
	row.t = t;
	row.seed = seed;
	row.decomposition_mode = config.policy;
	row.model_name = model_name_text(config.policy, config.adaptive_policy);
	row.active_components = active_components_text(config.policy);
	row.use_lower_bounds = config.use_lower_bounds;
	row.use_upper_bounds = config.use_upper_bounds;
	row.use_terminal_rules = config.use_terminal_rules && opts.use_terminal_rules;
	row.memo_capacity = config.memo_capacity;
	row.time_limit_sec = opts.time_limit_sec;
	row.memory_limit_gb = opts.memory_limit_gb;
	row.memo_backend = opts.memo_backend;
	row.enable_rule4 = opts.enable_rule4;
	row.position_filtering_enabled = opts.position_filtering_enabled;
	row.enable_lawler_basic_rules = opts.enable_lawler_basic_rules;
	row.enable_simple_lb = config.use_lower_bounds;
	row.enable_lb_memo = config.use_lower_bounds;
	row.enable_edd_ub = config.use_upper_bounds;
	row.ub_depth_limit = opts.ub_depth_limit;
	row.lb_depth_limit = opts.lb_depth_limit;
	row.enable_terminal_all_tardy_spt = opts.enable_terminal_all_tardy_spt;
	row.enable_terminal_edd_at_most_one_tardy = opts.enable_terminal_edd_at_most_one_tardy;
	row.enable_memo = opts.enable_memo;
	row.enable_exact_memo = opts.enable_exact_memo;
	row.memo_full_key_verification = opts.memo_full_key_verification;
	row.reconstruct_order = opts.reconstruct_order;
	row.reconstruction_trace = opts.reconstruction_trace;
	row.adaptive_policy = config.adaptive_policy;
	row.stats.adaptive_policy_used = static_cast<std::uint64_t>(config.adaptive_policy);
	row.notes = config.notes;

	const auto start = std::chrono::steady_clock::now();
	try {
		// Каждый режим получает один и тот же экземпляр: n, R, T и seed записываются в CSV.
		// Это главное условие корректного сравнения времени, памяти и числа узлов.
		potts_generation_config gen{};
		gen.n = n;
		gen.p_min = 1;
		gen.p_max = 100;
		gen.due_range = r;
		gen.tardiness_factor = t;
		gen.seed = seed;
		instance inst = generate_potts_instance_article(gen);

		dfs_solver solver(make_solver_config(opts, config));
		const solve_result result = solver.solve(inst);
		if (opts.reconstruct_order) {
			if (result.best.order.size() != static_cast<std::size_t>(n)) {
				throw std::runtime_error("reconstruction_failed: order size mismatch");
			}
			const schedule_cost_t reconstructed_cost =
				evaluate_sum_tardiness(inst, result.best.order);
			if (reconstructed_cost != result.best.cost) {
				throw std::runtime_error("reconstruction_failed: order cost mismatch");
			}
		}
		row.status = "SOLVED";
		row.optimum = std::to_string(result.best.cost);
		row.time_ms = result.stats.elapsed_ms;
		row.stats = result.stats;
		if (!result.best.order.empty()) {
			row.best_first = std::to_string(result.best.order.front());
		}
	}
	catch (const solver_time_limit_exceeded& ex) {
		row.status = "OOT";
		row.stats = ex.stats;
		row.time_ms = ex.stats.elapsed_ms > 0.0 ? ex.stats.elapsed_ms : opts.time_limit_sec * 1000.0;
		row.notes = row.notes.empty() ? "time_limit_exceeded" : row.notes + "; time_limit_exceeded";
	}
	catch (const std::bad_alloc&) {
		const auto finish = std::chrono::steady_clock::now();
		row.status = "OOM";
		row.time_ms = std::chrono::duration<double, std::milli>(finish - start).count();
		row.notes = row.notes.empty() ? "std::bad_alloc" : row.notes + "; std::bad_alloc";
	}
	catch (const std::exception& ex) {
		const auto finish = std::chrono::steady_clock::now();
		row.status = "ERROR";
		row.time_ms = std::chrono::duration<double, std::milli>(finish - start).count();
		row.notes = row.notes.empty() ? ex.what() : row.notes + "; " + ex.what();
	}
	catch (...) {
		const auto finish = std::chrono::steady_clock::now();
		row.status = "ERROR";
		row.time_ms = std::chrono::duration<double, std::milli>(finish - start).count();
		row.notes = row.notes.empty() ? "unknown_exception" : row.notes + "; unknown_exception";
	}
	row.memory_bytes_peak = current_process_peak_memory_bytes();
	return row;
}

void write_header(std::ofstream& out) {
	for (std::size_t i = 0; i < csv_header.size(); ++i) {
		if (i > 0) {
			out << ",";
		}
		out << csv_header[i];
	}
	out << "\n";
}

void write_row(std::ofstream& out, const benchmark_row& row) {
	const auto bool_text = [](bool v) { return v ? "true" : "false"; };
	const auto uint_text = [](std::uint64_t v) { return std::to_string(v); };
	std::map<std::string, std::string> values;
	values["series"] = row.series;
	values["config"] = row.config;
	values["status"] = row.status;
	values["n"] = std::to_string(row.n);
	values["R"] = format_double(row.r);
	values["T"] = format_double(row.t);
	values["seed"] = std::to_string(row.seed);
	values["decomposition_mode"] = to_string(row.decomposition_mode);
	values["model_name"] = row.model_name;
	values["active_components"] = row.active_components;
	values["memo_backend"] = to_string(row.memo_backend);
	values["adaptive_policy"] = to_string(row.adaptive_policy);
	values["objective"] = row.optimum;
	values["time_ms"] = format_double(row.time_ms);
	values["nodes"] = uint_text(row.stats.nodes);
	values["recursive_calls"] = uint_text(row.stats.recursive_calls);
	values["branches_generated"] = uint_text(row.stats.branches_generated);
	values["branches_pruned"] = uint_text(row.stats.branches_pruned);
	values["max_depth"] = uint_text(row.stats.max_depth);
	values["lawler_nodes"] = uint_text(row.stats.lawler_nodes);
	values["szwarc_nodes"] = uint_text(row.stats.szwarc_nodes);
	values["both_nodes"] = uint_text(row.stats.both_nodes);
	values["adaptive_choices_lawler"] = uint_text(row.stats.adaptive_choices_lawler);
	values["adaptive_choices_szwarc"] = uint_text(row.stats.adaptive_choices_szwarc);
	values["adaptive_choices_both"] = uint_text(row.stats.adaptive_choices_both);
	values["adaptive_choice_lawler"] = uint_text(row.stats.adaptive_choice_lawler);
	values["adaptive_choice_szwarc"] = uint_text(row.stats.adaptive_choice_szwarc);
	values["adaptive_v1_choices_lawler"] = uint_text(row.stats.adaptive_v1_choices_lawler);
	values["adaptive_v1_choices_szwarc"] = uint_text(row.stats.adaptive_v1_choices_szwarc);
	values["adaptive_v1_choices_both"] = uint_text(row.stats.adaptive_v1_choices_both);
	values["adaptive_v2_choices_lawler"] = uint_text(row.stats.adaptive_v2_choices_lawler);
	values["adaptive_v2_choices_szwarc"] = uint_text(row.stats.adaptive_v2_choices_szwarc);
	values["adaptive_v2_choices_both"] = uint_text(row.stats.adaptive_v2_choices_both);
	values["adaptive_v3_choices_lawler"] = uint_text(row.stats.adaptive_v3_choices_lawler);
	values["adaptive_v3_choices_szwarc"] = uint_text(row.stats.adaptive_v3_choices_szwarc);
	values["adaptive_v3_choices_both"] = uint_text(row.stats.adaptive_v3_choices_both);
	values["adaptive_policy_used"] = uint_text(row.stats.adaptive_policy_used);
	values["memo_stores_exact"] = uint_text(row.stats.memo_stores_exact);
	values["memo_stores_lb"] = uint_text(row.stats.memo_stores_lb);
	values["enable_simple_lb"] = bool_text(row.enable_simple_lb);
	values["enable_lb_memo"] = bool_text(row.enable_lb_memo);
	values["enable_edd_ub"] = bool_text(row.enable_edd_ub);
	values["ub_depth_limit"] = std::to_string(row.ub_depth_limit);
	values["lb_depth_limit"] = std::to_string(row.lb_depth_limit);
	values["position_filtering_enabled"] = bool_text(row.position_filtering_enabled);
	values["enable_lawler_basic_rules"] = bool_text(row.enable_lawler_basic_rules);
	values["enable_rule4"] = bool_text(row.enable_rule4);
	values["enable_terminal_all_tardy_spt"] = bool_text(row.enable_terminal_all_tardy_spt);
	values["enable_terminal_edd_at_most_one_tardy"] = bool_text(row.enable_terminal_edd_at_most_one_tardy);
	values["enable_memo"] = bool_text(row.enable_memo);
	values["enable_exact_memo"] = bool_text(row.enable_exact_memo);
	values["memo_full_key_verification"] = bool_text(row.memo_full_key_verification);
	values["reconstruct_order"] = bool_text(row.reconstruct_order);
	values["reconstruction_trace"] = bool_text(row.reconstruction_trace);
	values["memo_capacity"] = std::to_string(row.memo_capacity);
	values["memo_memory_limit_mb"] =
		std::to_string(static_cast<std::uint64_t>(row.memory_limit_gb * 1024.0));
	values["memo_exact_queries"] = uint_text(row.stats.memo_exact_queries);
	values["memo_exact_hits"] = uint_text(row.stats.memo_exact_hits);
	values["memo_lb_queries"] = uint_text(row.stats.memo_lb_queries);
	values["memo_lb_hits"] = uint_text(row.stats.memo_lb_hits);
	values["memo_exact_stores"] = uint_text(row.stats.memo_exact_stores);
	values["memo_lb_stores"] = uint_text(row.stats.memo_lb_stores);
	values["memo_evictions_exact"] = uint_text(row.stats.memo_evictions_exact);
	values["memo_evictions_lb"] = uint_text(row.stats.memo_evictions_lb);
	values["memo_cleanup_calls"] = uint_text(row.stats.memo_clean_calls);
	values["memo_cleanup_time_ms"] = format_double(row.stats.memo_clean_time_ms);
	values["cleanup_time_ms"] = format_double(row.stats.cleanup_time_ms);
	values["memo_memory_used_bytes"] = std::to_string(row.stats.memo_memory_used_bytes);
	values["memo_peak_size"] = std::to_string(row.stats.memo_peak_size);
	values["memo_final_size"] = std::to_string(row.stats.memo_final_size);
	values["memo_rejected_no_room"] = uint_text(row.stats.memo_rejected_no_room);
	values["memo_forced_evictions"] = uint_text(row.stats.memo_forced_evictions);
	values["reconstruction_time_ms"] = format_double(row.stats.reconstruction_time_ms);
	values["reconstruction_steps"] = uint_text(row.stats.reconstruction_steps);
	values["reconstruction_current_exact_hits"] =
		uint_text(row.stats.reconstruction_current_exact_hits);
	values["reconstruction_current_exact_misses"] =
		uint_text(row.stats.reconstruction_current_exact_misses);
	values["reconstruction_child_exact_hits"] =
		uint_text(row.stats.reconstruction_child_exact_hits);
	values["reconstruction_child_exact_misses"] =
		uint_text(row.stats.reconstruction_child_exact_misses);
	values["reconstruction_repair_solves"] =
		uint_text(row.stats.reconstruction_repair_solves);
	values["reconstruction_candidate_scans"] =
		uint_text(row.stats.reconstruction_candidate_scans);
	values["reconstruction_trace_stores"] =
		uint_text(row.stats.reconstruction_trace_stores);
	values["reconstruction_trace_entries"] =
		uint_text(row.stats.reconstruction_trace_entries);
	values["reconstruction_trace_hits"] =
		uint_text(row.stats.reconstruction_trace_hits);
	values["reconstruction_trace_misses"] =
		uint_text(row.stats.reconstruction_trace_misses);
	values["reconstruction_trace_terminal_hits"] =
		uint_text(row.stats.reconstruction_trace_terminal_hits);
	values["reconstruction_trace_fallbacks"] =
		uint_text(row.stats.reconstruction_trace_fallbacks);
	values["simple_lb_calls"] = uint_text(row.stats.simple_lb_calls);
	values["simple_lb_prunes"] = uint_text(row.stats.simple_lb_prunes);
	values["ub_calls"] = uint_text(row.stats.ub_calls);
	values["ub_improvements"] = uint_text(row.stats.ub_improvements);
	values["bound_time_ms"] = format_double(row.stats.bound_time_ms);
	values["upper_bound_time_ms"] = format_double(row.stats.upper_bound_time_ms);
	values["valid_positions_before"] = uint_text(row.stats.valid_positions_before);
	values["valid_positions_after"] = uint_text(row.stats.valid_positions_after);
	values["positions_pruned"] = uint_text(row.stats.positions_pruned);
	values["candidate_positions_before"] = uint_text(row.stats.candidate_positions_before);
	values["candidate_positions_after"] = uint_text(row.stats.candidate_positions_after);
	values["positions_pruned_by_lawler_basic"] = uint_text(row.stats.positions_pruned_by_lawler_basic);
	values["positions_pruned_by_lawler_rule4"] =
		uint_text(row.stats.positions_pruned_by_lawler_rule4);
	values["positions_pruned_by_szwarc_rule4"] =
		uint_text(row.stats.positions_pruned_by_szwarc_rule4);
	values["time_spent_in_position_filtering_ms"] =
		format_double(row.stats.time_spent_in_position_filtering_ms);
	values["terminal_all_tardy_spt_hits"] = uint_text(row.stats.terminal_all_tardy_spt_hits);
	values["terminal_edd_one_tardy_hits"] = uint_text(row.stats.terminal_edd_one_tardy_hits);
	values["terminal_time_ms"] = format_double(row.stats.terminal_time_ms);
	values["memory_bytes_peak"] = std::to_string(row.memory_bytes_peak);
	values["time_limit_sec"] = format_double(row.time_limit_sec);
	values["memory_limit_gb"] = format_double(row.memory_limit_gb);
	values["notes"] = row.notes;

	for (std::size_t i = 0; i < csv_header.size(); ++i) {
		if (i > 0) {
			out << ",";
		}
		out << csv_escape(values[csv_header[i]]);
	}
	out << "\n";
}
std::unordered_set<std::string> load_completed_keys(const fs::path& csv_path) {
	std::unordered_set<std::string> keys;
	std::ifstream in(csv_path);
	if (!in) {
		return keys;
	}
	std::string header_line;
	if (!std::getline(in, header_line)) {
		return keys;
	}
	const std::vector<std::string> header = parse_csv_line(header_line);
	std::map<std::string, std::size_t> idx;
	for (std::size_t i = 0; i < header.size(); ++i) {
		idx[header[i]] = i;
	}
	const std::vector<std::string> required = { "series", "config", "n", "R", "T", "seed" };
	for (const std::string& name : required) {
		if (idx.find(name) == idx.end()) {
			return keys;
		}
	}
	std::string line;
	while (std::getline(in, line)) {
		if (trim_copy(line).empty()) {
			continue;
		}
		const std::vector<std::string> fields = parse_csv_line(line);
		if (fields.size() < header.size()) {
			continue;
		}
		double r = 0.0;
		double t = 0.0;
		int n = 0;
		std::uint64_t seed = 0;
		if (!parse_int_token(fields[idx["n"]], n)
			|| !parse_double_token(fields[idx["R"]], r)
			|| !parse_double_token(fields[idx["T"]], t)
			|| !parse_u64_token(fields[idx["seed"]], seed)) {
			continue;
		}
		run_key key;
		key.series = fields[idx["series"]];
		key.config = fields[idx["config"]];
		key.n = n;
		key.r = format_double(r);
		key.t = format_double(t);
		key.seed = seed;
		const auto rule4_it = idx.find("enable_rule4");
		key.rule4 = (rule4_it == idx.end()) ? "true" : lowercase_copy(fields[rule4_it->second]);
		const auto terminal_it = idx.find("use_terminal_rules");
		key.terminal_rules = (terminal_it == idx.end()) ? "true" : lowercase_copy(fields[terminal_it->second]);
		const auto memo_backend_it = idx.find("memo_backend");
		key.memo_backend = (memo_backend_it == idx.end()) ? "custom" : lowercase_copy(fields[memo_backend_it->second]);
		const auto adaptive_policy_it = idx.find("adaptive_policy");
		key.adaptive_policy = (adaptive_policy_it == idx.end()) ? "v1" : lowercase_copy(fields[adaptive_policy_it->second]);
		const auto reconstruct_it = idx.find("reconstruct_order");
		key.reconstruct_order = (reconstruct_it == idx.end()) ? "false" : lowercase_copy(fields[reconstruct_it->second]);
		const auto trace_it = idx.find("reconstruction_trace");
		key.reconstruction_trace = (trace_it == idx.end()) ? "false" : lowercase_copy(fields[trace_it->second]);
		keys.insert(key.str());
	}
	return keys;
}

run_key make_key(const benchmark_options& opts, const benchmark_config& config,
	int n, double r, double t, std::uint64_t seed) {
	run_key key;
	key.series = opts.series;
	key.config = config.name;
	key.n = n;
	key.r = format_double(r);
	key.t = format_double(t);
	key.seed = seed;
	key.rule4 = opts.enable_rule4 ? "true" : "false";
	key.terminal_rules = (config.use_terminal_rules && opts.use_terminal_rules) ? "true" : "false";
	key.memo_backend = to_string(opts.memo_backend);
	key.adaptive_policy = to_string(config.adaptive_policy);
	key.reconstruct_order = opts.reconstruct_order ? "true" : "false";
	key.reconstruction_trace = opts.reconstruction_trace ? "true" : "false";
	return key;
}

std::vector<std::pair<double, double>> parse_hard_pairs_manual(const std::string& raw, std::string& error) {
	std::vector<std::pair<double, double>> pairs;
	std::string normalized = raw;
	for (char& ch : normalized) {
		if (ch == '|') {
			ch = ';';
		}
	}
	std::stringstream ss(normalized);
	std::string token;
	while (std::getline(ss, token, ';')) {
		token = trim_copy(token);
		if (token.empty()) {
			continue;
		}
		const std::size_t sep = token.find(':');
		if (sep == std::string::npos) {
			error = "Manual --hard-pairs must use R:T;R:T format.";
			return {};
		}
		double r = 0.0;
		double t = 0.0;
		if (!parse_double_token(trim_copy(token.substr(0, sep)), r)
			|| !parse_double_token(trim_copy(token.substr(sep + 1)), t)) {
			error = "Invalid --hard-pairs token: " + token;
			return {};
		}
		pairs.emplace_back(r, t);
	}
	if (pairs.empty()) {
		error = "No hard pairs selected.";
	}
	return pairs;
}

struct hard_score_acc {
	int runs = 0;
	int oot = 0;
	double solved_time_sum = 0.0;
	int solved = 0;
	double time_limit_ms = 0.0;
};

std::string upper_status(std::string text) {
	for (char& ch : text) {
		ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
	}
	return text;
}

std::vector<std::pair<double, double>> select_hard_pairs_auto_fixed(const benchmark_options& opts) {
	std::vector<fs::path> candidates;
	if (!opts.hard_source_csv.empty()) {
		candidates.push_back(opts.hard_source_csv);
	}
	candidates.push_back(opts.out.parent_path() / "lower_bounds_results.csv");
	candidates.push_back(opts.out.parent_path() / "branching_modes_results.csv");

	fs::path source;
	for (const fs::path& p : candidates) {
		if (fs::exists(p)) {
			source = p;
			break;
		}
	}
	if (source.empty()) {
		std::cerr << "[bench] warning: --hard-pairs auto found no source CSV; using fallback pair 0.2:0.6\n";
		return { {0.2, 0.6} };
	}

	std::ifstream in(source);
	if (!in) {
		return { {0.2, 0.6} };
	}
	std::string header_line;
	if (!std::getline(in, header_line)) {
		return { {0.2, 0.6} };
	}
	const std::vector<std::string> header = parse_csv_line(header_line);
	std::map<std::string, std::size_t> idx;
	for (std::size_t i = 0; i < header.size(); ++i) {
		idx[header[i]] = i;
	}
	const std::vector<std::string> required = { "n", "R", "T", "status", "time_ms", "time_limit_sec" };
	for (const std::string& name : required) {
		if (idx.find(name) == idx.end()) {
			return { {0.2, 0.6} };
		}
	}

	std::map<std::pair<std::string, std::string>, hard_score_acc> groups;
	std::string line;
	while (std::getline(in, line)) {
		if (trim_copy(line).empty()) {
			continue;
		}
		const std::vector<std::string> fields = parse_csv_line(line);
		if (fields.size() < header.size()) {
			continue;
		}
		int n = 0;
		double r = 0.0;
		double t = 0.0;
		double time_ms = 0.0;
		double time_limit_sec = opts.time_limit_sec;
		if (!parse_int_token(fields[idx["n"]], n) || n != opts.hard_auto_n
			|| !parse_double_token(fields[idx["R"]], r)
			|| !parse_double_token(fields[idx["T"]], t)) {
			continue;
		}
		(void)parse_double_token(fields[idx["time_ms"]], time_ms);
		(void)parse_double_token(fields[idx["time_limit_sec"]], time_limit_sec);
		const std::string status = upper_status(fields[idx["status"]]);
		auto& acc = groups[{ format_double(r), format_double(t) }];
		++acc.runs;
		acc.time_limit_ms = time_limit_sec * 1000.0;
		if (status == "SOLVED") {
			++acc.solved;
			acc.solved_time_sum += time_ms;
		}
		else if (status == "OOT") {
			++acc.oot;
		}
	}

	std::vector<std::pair<double, std::pair<double, double>>> scored;
	for (const auto& item : groups) {
		const hard_score_acc& acc = item.second;
		if (acc.runs == 0) {
			continue;
		}
		const double avg_solved = acc.solved > 0 ? acc.solved_time_sum / static_cast<double>(acc.solved) : 0.0;
		const double limit_ms = acc.time_limit_ms > 0.0 ? acc.time_limit_ms : opts.time_limit_sec * 1000.0;
		const double score = avg_solved + limit_ms * static_cast<double>(acc.oot);
		double r = 0.0;
		double t = 0.0;
		if (parse_double_token(item.first.first, r) && parse_double_token(item.first.second, t)) {
			scored.push_back({ score, { r, t } });
		}
	}
	std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
		return a.first > b.first;
	});
	std::vector<std::pair<double, double>> out;
	for (const auto& item : scored) {
		out.push_back(item.second);
		if (out.size() == 3) {
			break;
		}
	}
	if (out.empty()) {
		std::cerr << "[bench] warning: --hard-pairs auto found no scored pairs in " << source << "; using fallback pair 0.2:0.6\n";
		out.push_back({ 0.2, 0.6 });
	}
	return out;
}

bool parse_cli(int argc, char** argv, benchmark_options& opts, std::string& error, bool& help) {
	help = false;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			help = true;
			return true;
		}
		if (i + 1 >= argc) {
			error = "Missing value for argument: " + arg;
			return false;
		}
		const std::string value = argv[++i];

		if (arg == "--series") {
			opts.series = lowercase_copy(value);
		}
		else if (arg == "--out") {
			opts.out = value;
		}
		else if (arg == "--time-limit-sec") {
			if (!parse_double_token(value, opts.time_limit_sec) || opts.time_limit_sec <= 0.0) {
				error = "Invalid --time-limit-sec.";
				return false;
			}
		}
		else if (arg == "--memory-limit-gb") {
			if (!parse_double_token(value, opts.memory_limit_gb) || opts.memory_limit_gb < 0.0) {
				error = "Invalid --memory-limit-gb.";
				return false;
			}
		}
		else if (arg == "--n-values") {
			if (!parse_list<int>(value, opts.n_values, parse_int_token, error)) {
				error = "Invalid --n-values: " + error;
				return false;
			}
			opts.n_values_set = true;
		}
		else if (arg == "--R-values") {
			if (!parse_list<double>(value, opts.r_values, parse_double_token, error)) {
				error = "Invalid --R-values: " + error;
				return false;
			}
			opts.r_values_set = true;
		}
		else if (arg == "--T-values") {
			if (!parse_list<double>(value, opts.t_values, parse_double_token, error)) {
				error = "Invalid --T-values: " + error;
				return false;
			}
			opts.t_values_set = true;
		}
		else if (arg == "--seeds") {
			if (!parse_list<std::uint64_t>(value, opts.seeds, parse_u64_token, error)) {
				error = "Invalid --seeds: " + error;
				return false;
			}
			opts.seeds_set = true;
		}
		else if (arg == "--model") {
			if (!apply_model_name(value, opts)) {
				error = "Invalid --model.";
				return false;
			}
		}
		else if (arg == "--preset") {
			if (!apply_preset_name(value, opts)) {
				error = "Invalid --preset.";
				return false;
			}
		}
		else if (arg == "--decomposition") {
			if (!parse_decomposition_mode(value, opts.decomposition_mode)) {
				error = "Invalid --decomposition.";
				return false;
			}
		}
		else if (arg == "--hard-pairs") {
			opts.hard_pairs = value;
		}
		else if (arg == "--hard-source-csv") {
			opts.hard_source_csv = value;
		}
		else if (arg == "--hard-auto-n") {
			if (!parse_int_token(value, opts.hard_auto_n) || opts.hard_auto_n <= 0) {
				error = "Invalid --hard-auto-n.";
				return false;
			}
		}
		else if (arg == "--use-lower-bounds") {
			if (!parse_bool_value(value, opts.use_lower_bounds)) {
				error = "Invalid --use-lower-bounds.";
				return false;
			}
			opts.use_lower_bounds_set = true;
		}
		else if (arg == "--use-upper-bounds") {
			if (!parse_bool_value(value, opts.use_upper_bounds)) {
				error = "Invalid --use-upper-bounds.";
				return false;
			}
			opts.use_upper_bounds_set = true;
		}
		else if (arg == "--terminal-rules") {
			if (!parse_bool_value(value, opts.use_terminal_rules)) {
				error = "Invalid --terminal-rules.";
				return false;
			}
		}
		else if (arg == "--enable-rule4") {
			if (!parse_bool_value(value, opts.enable_rule4)) {
				error = "Invalid --enable-rule4.";
				return false;
			}
		}
		else if (arg == "--lower-bounds-mode") {
			opts.lower_bounds_mode = lowercase_copy(value);
		}
		else if (arg == "--memo-capacity") {
			if (!parse_size_token(value, opts.memo_capacity)) {
				error = "Invalid --memo-capacity.";
				return false;
			}
			opts.memo_capacity_set = true;
		}
		else if (arg == "--memo-backend") {
			if (!parse_memo_backend_kind(value, opts.memo_backend)) {
				error = "Invalid --memo-backend.";
				return false;
			}
		}
		else if (arg == "--enable-memo") {
			if (!parse_bool_value(value, opts.enable_memo)) {
				error = "Invalid --enable-memo.";
				return false;
			}
		}
		else if (arg == "--enable-exact-memo") {
			if (!parse_bool_value(value, opts.enable_exact_memo)) {
				error = "Invalid --enable-exact-memo.";
				return false;
			}
		}
		else if (arg == "--memo-full-key-verification") {
			if (!parse_bool_value(value, opts.memo_full_key_verification)) {
				error = "Invalid --memo-full-key-verification.";
				return false;
			}
		}
		else if (arg == "--process-memory-gate") {
			if (!parse_bool_value(value, opts.use_process_memory_gate)) {
				error = "Invalid --process-memory-gate.";
				return false;
			}
		}
		else if (arg == "--reconstruct") {
			if (!parse_bool_value(value, opts.reconstruct_order)) {
				error = "Invalid --reconstruct.";
				return false;
			}
		}
		else if (arg == "--reconstruction-trace") {
			if (!parse_bool_value(value, opts.reconstruction_trace)) {
				error = "Invalid --reconstruction-trace.";
				return false;
			}
		}
		else if (arg == "--enable-position-filtering") {
			if (!parse_bool_value(value, opts.position_filtering_enabled)) {
				error = "Invalid --enable-position-filtering.";
				return false;
			}
		}
		else if (arg == "--enable-lawler-basic-rules") {
			if (!parse_bool_value(value, opts.enable_lawler_basic_rules)) {
				error = "Invalid --enable-lawler-basic-rules.";
				return false;
			}
		}
		else if (arg == "--ub-depth-limit") {
			if (!parse_int_token(value, opts.ub_depth_limit)) {
				error = "Invalid --ub-depth-limit.";
				return false;
			}
		}
		else if (arg == "--lb-depth-limit") {
			if (!parse_int_token(value, opts.lb_depth_limit)) {
				error = "Invalid --lb-depth-limit.";
				return false;
			}
		}
		else if (arg == "--enable-terminal-all-tardy-spt") {
			if (!parse_bool_value(value, opts.enable_terminal_all_tardy_spt)) {
				error = "Invalid --enable-terminal-all-tardy-spt.";
				return false;
			}
		}
		else if (arg == "--enable-terminal-edd-at-most-one-tardy") {
			if (!parse_bool_value(value, opts.enable_terminal_edd_at_most_one_tardy)) {
				error = "Invalid --enable-terminal-edd-at-most-one-tardy.";
				return false;
			}
		}
		else if (arg == "--adaptive-policy") {
			if (!parse_adaptive_policy_kind(value, opts.adaptive_policy)) {
				error = "Invalid --adaptive-policy.";
				return false;
			}
		}
		else if (arg == "--resume") {
			if (!parse_bool_value(value, opts.resume)) {
				error = "Invalid --resume.";
				return false;
			}
		}
		else if (arg == "--append") {
			if (!parse_bool_value(value, opts.append)) {
				error = "Invalid --append.";
				return false;
			}
		}
		else if (arg == "--progress") {
			if (!parse_bool_value(value, opts.progress)) {
				error = "Invalid --progress.";
				return false;
			}
		}
		else if (arg == "--limit") {
			if (!parse_int_token(value, opts.limit) || opts.limit < 0) {
				error = "Invalid --limit.";
				return false;
			}
		}
		else {
			error = "Unknown argument: " + arg;
			return false;
		}
	}

	if (opts.series != "branching" && opts.series != "lower-bounds"
		&& opts.series != "memory" && opts.series != "hard") {
		error = "Unknown --series: " + opts.series;
		return false;
	}
	if (opts.lower_bounds_mode != "baseline-lb"
		&& opts.lower_bounds_mode != "lb-only"
		&& opts.lower_bounds_mode != "ub-only"
		&& opts.lower_bounds_mode != "ub-lb"
		&& opts.lower_bounds_mode != "new-only"
		&& opts.lower_bounds_mode != "all") {
		error = "Invalid --lower-bounds-mode.";
		return false;
	}
	return true;
}

bool file_has_content(const fs::path& path) {
	std::error_code ec;
	return fs::exists(path, ec) && fs::file_size(path, ec) > 0;
}

void ensure_parent_directory(const fs::path& path) {
	const fs::path parent = path.parent_path();
	if (!parent.empty()) {
		fs::create_directories(parent);
	}
}

int run_regular_series(const benchmark_options& opts, const std::vector<benchmark_config>& configs,
	std::ofstream& csv, std::unordered_set<std::string>& completed) {
	int executed = 0;
	int skipped = 0;
	int planned = 0;
	for (const benchmark_config& config : configs) {
		for (int n : opts.n_values) {
			for (double r : opts.r_values) {
				for (double t : opts.t_values) {
					for (std::uint64_t seed : opts.seeds) {
						if (opts.limit > 0 && planned >= opts.limit) {
							continue;
						}
						++planned;
						const run_key key = make_key(opts, config, n, r, t, seed);
						if (opts.resume && completed.find(key.str()) != completed.end()) {
							++skipped;
							continue;
						}
						if (opts.progress) {
							std::cout << "[bench] " << opts.series
								<< " config=" << config.name
								<< " n=" << n
								<< " R=" << format_double(r)
								<< " T=" << format_double(t)
								<< " seed=" << seed << "\n";
						}
						benchmark_row row = execute_case(opts, config, n, r, t, seed);
						write_row(csv, row);
						csv.flush();
						completed.insert(key.str());
						++executed;
					}
				}
			}
		}
	}
	if (opts.progress) {
		std::cout << "[bench] completed: executed=" << executed << " skipped=" << skipped << "\n";
	}
	return 0;
}

int run_hard_series(const benchmark_options& opts, const std::vector<benchmark_config>& configs,
	std::ofstream& csv, std::unordered_set<std::string>& completed) {
	std::string error;
	std::vector<std::pair<double, double>> pairs;
	if (lowercase_copy(opts.hard_pairs) == "auto") {
		pairs = select_hard_pairs_auto_fixed(opts);
	}
	else {
		pairs = parse_hard_pairs_manual(opts.hard_pairs, error);
		if (!error.empty()) {
			std::cerr << error << "\n";
			return 2;
		}
	}
	if (opts.progress) {
		std::cout << "[bench] hard pairs:";
		for (const auto& p : pairs) {
			std::cout << " (" << format_double(p.first) << "," << format_double(p.second) << ")";
		}
		std::cout << "\n";
	}

	int executed = 0;
	int skipped = 0;
	int planned = 0;
	const benchmark_config& config = configs.front();
	for (const auto& pair : pairs) {
		const double r = pair.first;
		const double t = pair.second;
		for (int n : opts.n_values) {
			int n_executed = 0;
			int n_solved = 0;
			for (std::uint64_t seed : opts.seeds) {
				if (opts.limit > 0 && planned >= opts.limit) {
					continue;
				}
				++planned;
				const run_key key = make_key(opts, config, n, r, t, seed);
				if (opts.resume && completed.find(key.str()) != completed.end()) {
					++skipped;
					continue;
				}
				if (opts.progress) {
					std::cout << "[bench] hard config=" << config.name
						<< " n=" << n
						<< " R=" << format_double(r)
						<< " T=" << format_double(t)
						<< " seed=" << seed << "\n";
				}
				benchmark_row row = execute_case(opts, config, n, r, t, seed);
				if (row.status == "SOLVED") {
					++n_solved;
				}
				++n_executed;
				write_row(csv, row);
				csv.flush();
				completed.insert(key.str());
				++executed;
			}
			if (n_executed > 0) {
				const double solved_ratio = static_cast<double>(n_solved) / static_cast<double>(n_executed);
				if (n_solved == 0 || solved_ratio < 0.20) {
					if (opts.progress) {
						std::cout << "[bench] stopping pair R=" << format_double(r)
							<< " T=" << format_double(t)
							<< " after n=" << n
							<< " solved=" << n_solved << "/" << n_executed << "\n";
					}
					break;
				}
			}
		}
	}
	if (opts.progress) {
		std::cout << "[bench] completed: executed=" << executed << " skipped=" << skipped << "\n";
	}
	return 0;
}
} // namespace

int main(int argc, char** argv) {
	try {
		benchmark_options opts;
		std::string error;
		bool help = false;
		if (!parse_cli(argc, argv, opts, error, help)) {
			std::cerr << error << "\n\n";
			print_usage();
			return 2;
		}
		if (help) {
			print_usage();
			return 0;
		}
		set_default_grid(opts);

		ensure_parent_directory(opts.out);
		write_metadata(opts);

		std::unordered_set<std::string> completed;
		if (opts.resume && file_has_content(opts.out)) {
			completed = load_completed_keys(opts.out);
		}

		const bool append_existing = opts.append && file_has_content(opts.out);
		std::ofstream csv(opts.out, append_existing ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc));
		if (!csv) {
			std::cerr << "Cannot open output CSV: " << opts.out << "\n";
			return 2;
		}
		if (!append_existing) {
			write_header(csv);
		}

		const std::vector<benchmark_config> configs = make_series_configs(opts);
		if (opts.series == "hard") {
			return run_hard_series(opts, configs, csv, completed);
		}
		return run_regular_series(opts, configs, csv, completed);
	}
	catch (const std::exception& ex) {
		std::cerr << ex.what() << "\n";
		return 3;
	}
}
