#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core.h"
#include "dfs_solver.h"
#include "generator.h"
#include "memo.h"
#include "memo_std_unordered.h"

namespace {
// Минимальный test runner без внешнего framework-а.
// Проверки маленькие: они нужны не для скорости, а чтобы после каждого патча
// убедиться, что exact solver не меняет optimum и memo остаётся exact-safe.
class test_failure : public std::runtime_error {
public:
	explicit test_failure(const std::string& message)
		: std::runtime_error(message) {}
};

void require(bool condition, const std::string& message) {
	if (!condition) {
		throw test_failure(message);
	}
}

void require_order_is_permutation(const std::vector<job_id_t>& order, std::size_t job_count,
	const std::string& label) {
	require(order.size() == job_count, label + ": order length differs from n");
	std::vector<int> seen(job_count, 0);
	for (job_id_t job_id : order) {
		const std::size_t index = static_cast<std::size_t>(job_id);
		require(index < job_count, label + ": job id is out of range");
		++seen[index];
	}
	for (std::size_t i = 0; i < seen.size(); ++i) {
		require(seen[i] == 1, label + ": schedule must contain each job exactly once");
	}
}

void require_instances_equal(const instance& left, const instance& right, const std::string& label) {
	require(left.jobs.size() == right.jobs.size(), label + ": generated n differs");
	for (std::size_t i = 0; i < left.jobs.size(); ++i) {
		require(left.jobs[i].p == right.jobs[i].p, label + ": p_j differs at job " + std::to_string(i));
		require(left.jobs[i].d == right.jobs[i].d, label + ": d_j differs at job " + std::to_string(i));
	}
}

schedule_cost_t brute_force_optimum_cost(const instance& inst, std::vector<job_id_t>* best_order = nullptr) {
	// Полный перебор допустим только на малых n. Он служит эталоном OPT для 1||sum T_j.
	std::vector<job_id_t> order(inst.jobs.size());
	std::iota(order.begin(), order.end(), job_id_t{0});

	schedule_cost_t best = std::numeric_limits<schedule_cost_t>::max();
	do {
		const schedule_cost_t cost = evaluate_sum_tardiness(inst, order);
		if (cost < best) {
			best = cost;
			if (best_order != nullptr) {
				*best_order = order;
			}
		}
	} while (std::next_permutation(order.begin(), order.end()));

	return best;
}

dfs_config make_solver_test_config() {
	dfs_config cfg{};
	// В тестах reconstruction включён: проверяем не только стоимость, но и порядок,
	// который восстанавливается через exact memo entries и best_job.
	cfg.reconstruct_order = true;
	cfg.memo.capacity = 0;
	cfg.memo.memory_limit_bytes = 0;
	return cfg;
}

void check_solver_matches_bruteforce(const instance& inst, const dfs_config& cfg, const std::string& label) {
	std::vector<job_id_t> brute_order;
	const schedule_cost_t brute_cost = brute_force_optimum_cost(inst, &brute_order);

	dfs_solver solver(cfg);
	const solve_result result = solver.solve(inst);

	require(result.best.cost == brute_cost, label + ": solver cost differs from brute force");
	require_order_is_permutation(result.best.order, inst.jobs.size(), label + ": reconstruction");
	require(evaluate_sum_tardiness(inst, result.best.order) == brute_cost,
		label + ": reconstructed order does not match optimal cost");
}

void test_core() {
	require(tardiness(10, 7) == 3, "tardiness should be C_j - d_j when job is late");
	require(tardiness(7, 10) == 0, "tardiness should be zero before due date");
	require(tardiness(10, 10) == 0, "tardiness should be zero at due date");

	instance inst;
	inst.jobs = {
		{3, 4},
		{2, 3},
		{1, 10}
	};

	const std::vector<job_id_t> order = { 1, 0, 2 };
	require_order_is_permutation(order, inst.jobs.size(), "manual schedule");
	require(evaluate_sum_tardiness(inst, order) == 1,
		"manual schedule objective should be sum T_j = 1");
	require(evaluate_sum_tardiness(inst, order, 5) == 11,
		"manual schedule objective with start time t should include shifted completion times");
}

void test_generator() {
	potts_generation_config cfg;
	cfg.n = 8;
	cfg.p_min = 2;
	cfg.p_max = 9;
	cfg.due_range = 0.2;
	cfg.tardiness_factor = 0.6;
	cfg.seed = 123;

	std::string error;
	require(validate_potts_generation_config(cfg, &error), "generator config should be valid");
	require(error.empty(), "valid generator config should not return an error");

	const instance first = generate_potts_instance(cfg);
	const instance second = generate_potts_instance(cfg);
	require_instances_equal(first, second, "same seed reproducibility");
	require(first.jobs.size() == static_cast<std::size_t>(cfg.n), "generator should produce n jobs");
	for (std::size_t i = 0; i < first.jobs.size(); ++i) {
		require(first.jobs[i].p >= static_cast<processing_time_t>(cfg.p_min),
			"generated p_j is below p_min");
		require(first.jobs[i].p <= static_cast<processing_time_t>(cfg.p_max),
			"generated p_j is above p_max");
		require(first.jobs[i].d <= std::numeric_limits<due_date_t>::max(),
			"generated d_j is outside due_date_t range");
	}
}

void test_bruteforce_small() {
	for (std::uint64_t seed : { 0ULL, 1ULL, 2ULL }) {
		const instance inst = generate_potts_instance(8, 1, 10, 0.2, 0.6, seed);
		dfs_config cfg = make_solver_test_config();
		cfg.decomposition_mode = DecompositionMode::Adaptive;
		cfg.adaptive_policy = adaptive_policy_kind::v3;
		check_solver_matches_bruteforce(inst, cfg, "bruteforce_small_seed_" + std::to_string(seed));
	}
}

void test_decomposition_modes() {
	const instance inst = generate_potts_instance(8, 1, 10, 0.2, 0.6, 123);
	for (DecompositionMode mode : {
		DecompositionMode::Lawler,
		DecompositionMode::Szwarc,
		DecompositionMode::BothLawlerSzwarc,
		DecompositionMode::Adaptive
	}) {
		dfs_config cfg = make_solver_test_config();
		cfg.decomposition_mode = mode;
		cfg.adaptive_policy = adaptive_policy_kind::v3;
		check_solver_matches_bruteforce(inst, cfg, std::string("decomposition_") + to_string(mode));
	}
}

void test_memo_large_time_roundtrip();
void test_memo_hash_collision_rechecks_full_bits();
void test_memo_bit_boundaries_64_and_65();
void test_std_unordered_memo_roundtrip();

void test_memo_backends() {
	const instance inst = generate_potts_instance(8, 1, 10, 0.2, 0.6, 37);
	const schedule_cost_t brute = brute_force_optimum_cost(inst);

	for (memo_backend_kind backend : {
		memo_backend_kind::custom,
		memo_backend_kind::std_unordered
	}) {
		dfs_config cfg = make_solver_test_config();
		cfg.decomposition_mode = DecompositionMode::Adaptive;
		cfg.adaptive_policy = adaptive_policy_kind::v3;
		cfg.memo.backend = backend;
		cfg.memo.full_key_verification = true;

		dfs_solver solver(cfg);
		const solve_result result = solver.solve(inst);
		require(result.best.cost == brute,
			std::string("memo backend objective differs from brute force: ") + to_string(backend));
		require_order_is_permutation(result.best.order, inst.jobs.size(),
			std::string("memo backend reconstruction: ") + to_string(backend));
		require(evaluate_sum_tardiness(inst, result.best.order) == result.best.cost,
			std::string("memo backend reconstruction cost differs: ") + to_string(backend));
	}

	test_memo_large_time_roundtrip();
	test_memo_hash_collision_rechecks_full_bits();
	test_memo_bit_boundaries_64_and_65();
	test_std_unordered_memo_roundtrip();
}

void test_reconstruction() {
	const instance inst = generate_potts_instance(8, 1, 10, 0.4, 0.6, 29);
	dfs_config cfg = make_solver_test_config();
	cfg.decomposition_mode = DecompositionMode::BothLawlerSzwarc;
	const schedule_cost_t brute = brute_force_optimum_cost(inst);

	dfs_solver solver(cfg);
	const solve_result result = solver.solve(inst);
	require(result.best.cost == brute, "reconstruction test objective differs from brute force");
	require_order_is_permutation(result.best.order, inst.jobs.size(), "reconstruction test");
	require(evaluate_sum_tardiness(inst, result.best.order) == result.best.cost,
		"reconstructed order cost should equal solver objective");

}

void test_final_preset() {
	const instance inst = generate_potts_instance(8, 1, 10, 0.2, 0.6, 123);
	dfs_config cfg = make_solver_test_config();
	cfg.decomposition_mode = DecompositionMode::Adaptive;
	cfg.adaptive_policy = adaptive_policy_kind::v3;
	cfg.memo.backend = memo_backend_kind::custom;
	cfg.memo.full_key_verification = true;

	check_solver_matches_bruteforce(inst, cfg, "best-final equivalent config");
}

void test_solver_small_instances() {
	// Сравниваем solver с brute force на фиксированных маленьких экземплярах.
	// Параметры генератора Potts явно заданы: n, p-range, R, T и seed.
	std::vector<instance> cases;

	instance a;
	a.jobs = {
		{3, 4}, {2, 8}, {4, 7}, {1, 2}, {2, 3}
	};
	cases.push_back(a);

	instance b;
	b.jobs = {
		{6, 5}, {1, 2}, {4, 9}, {3, 7}, {2, 4}, {5, 8}
	};
	cases.push_back(b);

	cases.push_back(generate_potts_instance(6, 1, 7, 0.4, 0.5, 17));
	cases.push_back(generate_potts_instance(7, 1, 9, 0.2, 0.6, 29));
	cases.push_back(generate_potts_instance(7, 1, 9, 0.6, 0.2, 31));

	dfs_config baseline = make_solver_test_config();
	dfs_config lawler = baseline;
	lawler.decomposition_mode = DecompositionMode::Lawler;
	dfs_config szwarc = baseline;
	szwarc.decomposition_mode = DecompositionMode::Szwarc;
	dfs_config both = baseline;
	both.decomposition_mode = DecompositionMode::BothLawlerSzwarc;
	dfs_config adaptive_v1 = baseline;
	adaptive_v1.decomposition_mode = DecompositionMode::Adaptive;
	adaptive_v1.adaptive_policy = adaptive_policy_kind::v1;
	dfs_config adaptive_v2 = adaptive_v1;
	adaptive_v2.adaptive_policy = adaptive_policy_kind::v2;
	dfs_config adaptive_v3 = adaptive_v1;
	adaptive_v3.adaptive_policy = adaptive_policy_kind::v3;

	dfs_config aggressive = baseline;
	// Aggressive-режим включает дополнительные exact-safe идеи:
	// простую LB, LB memo и Both decomposition.
	aggressive.bounds.enable_simple_lb = true;
	aggressive.memo.enable_lb_memo = true;
	aggressive.decomposition_mode = DecompositionMode::BothLawlerSzwarc;

	dfs_config reference_memo = baseline;
	// std::unordered_map backend используется как читаемая reference-реализация memo.
	reference_memo.memo.backend = memo_backend_kind::std_unordered;

	dfs_config trace_reconstruction = baseline;
	trace_reconstruction.reconstruction_trace = true;

	for (std::size_t i = 0; i < cases.size(); ++i) {
		check_solver_matches_bruteforce(cases[i], baseline, "baseline_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], lawler, "lawler_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], szwarc, "szwarc_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], both, "both_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], adaptive_v1, "adaptive_v1_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], adaptive_v2, "adaptive_v2_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], adaptive_v3, "adaptive_v3_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], aggressive, "aggressive_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], reference_memo,
			"std_unordered_memo_case_" + std::to_string(i));
		check_solver_matches_bruteforce(cases[i], trace_reconstruction,
			"trace_reconstruction_case_" + std::to_string(i));
	}
}

void test_adaptive_policies_match_exact_modes_fixed_seeds() {
	// Adaptive v1/v2/v3 отличаются только выбором порядка точной декомпозиции.
	// На фиксированных seed их objective должен совпадать с Lawler/Szwarc/Both и brute force.
	std::vector<dfs_config> configs;
	dfs_config base = make_solver_test_config();
	base.reconstruct_order = false;

	dfs_config lawler = base;
	lawler.decomposition_mode = DecompositionMode::Lawler;
	configs.push_back(lawler);

	dfs_config szwarc = base;
	szwarc.decomposition_mode = DecompositionMode::Szwarc;
	configs.push_back(szwarc);

	dfs_config both = base;
	both.decomposition_mode = DecompositionMode::BothLawlerSzwarc;
	configs.push_back(both);

	for (adaptive_policy_kind policy : {
		adaptive_policy_kind::v1,
		adaptive_policy_kind::v2,
		adaptive_policy_kind::v3
	}) {
		dfs_config adaptive = base;
		adaptive.decomposition_mode = DecompositionMode::Adaptive;
		adaptive.adaptive_policy = policy;
		configs.push_back(adaptive);
	}

	for (std::uint64_t seed : { 0ULL, 1ULL, 2ULL, 7ULL }) {
		const instance inst = generate_potts_instance(8, 1, 10, 0.2, 0.6, seed);
		const schedule_cost_t brute = brute_force_optimum_cost(inst);
		for (std::size_t i = 0; i < configs.size(); ++i) {
			dfs_solver solver(configs[i]);
			const solve_result result = solver.solve(inst);
			require(result.best.cost == brute,
				"adaptive/exact mode mismatch on fixed seed " + std::to_string(seed) +
				", config " + std::to_string(i));
		}
	}
}

void test_custom_memo_matches_reference_backend() {
	// Custom backend и std::unordered_map reference backend должны давать одинаковый objective.
	const instance inst = generate_potts_instance(8, 1, 10, 0.4, 0.6, 37);
	dfs_config custom = make_solver_test_config();
	custom.reconstruct_order = false;

	dfs_config reference = custom;
	reference.memo.backend = memo_backend_kind::std_unordered;

	dfs_solver custom_solver(custom);
	dfs_solver reference_solver(reference);
	const solve_result custom_result = custom_solver.solve(inst);
	const solve_result reference_result = reference_solver.solve(inst);
	require(custom_result.best.cost == reference_result.best.cost,
		"custom memo should match std_unordered reference backend");
}

void test_memo_large_time_roundtrip() {
	// t входит в точный ключ (S,t) как 64-битное время: этот тест ловит ошибочную
	// 32-битную усечённую сериализацию времени.
	memo_table memo(16);
	const std::vector<std::uint64_t> bits = {
		0xA5A5A5A5A5A5A5A5ULL,
		0x0000000000000001ULL
	};
	const std::uint64_t subset_hash = 0x1122334455667788ULL;
	const std::uint64_t subset_fingerprint = 0x8877665544332211ULL;
	const schedule_time_t big_time = (schedule_time_t{1} << 40) + 12345;
	const schedule_time_t aliased_if_truncated = big_time + (schedule_time_t{1} << 32);

	memo.store_exact(bits, big_time, subset_hash, subset_fingerprint, 77, 4, true);

	const memo_lookup_result same = memo.lookup(bits, big_time, subset_hash, subset_fingerprint, true);
	require(same.found && same.has_exact, "memo large-time lookup did not find exact entry");
	require(same.exact == 77, "memo large-time lookup returned wrong exact value");
	require(same.best_job == 4, "memo large-time lookup returned wrong best job");

	const memo_lookup_result aliased =
		memo.lookup(bits, aliased_if_truncated, subset_hash, subset_fingerprint, true);
	require(!aliased.found, "memo lookup should distinguish 64-bit times beyond 32-bit truncation");

	const memo_lookup_result other =
		memo.lookup(bits, big_time + 1, subset_hash, subset_fingerprint, true);
	require(!other.found, "memo large-time lookup should be time-sensitive");
}

void test_memo_hash_collision_rechecks_full_bits() {
	// Hash/fingerprint не являются доказательством равенства состояний.
	// Exact-safe memo обязан сверять полный bitset S при совпавших hash и t.
	memo_table memo(16);
	const std::vector<std::uint64_t> bits_a = { 0x00000000000000AAULL };
	const std::vector<std::uint64_t> bits_b = { 0x00000000000000BBULL };
	const std::uint64_t subset_hash = 0x1234567890ABCDEFULL;
	const std::uint64_t subset_fingerprint = 0x0FEDCBA098765432ULL;
	const schedule_time_t time = 99;

	memo.store_exact(bits_a, time, subset_hash, subset_fingerprint, 15, 2, true);

	const memo_lookup_result miss = memo.lookup(bits_b, time, subset_hash, subset_fingerprint, true);
	require(!miss.found, "memo lookup should reject colliding key with different bits");

	const memo_diagnostics diags = memo.diagnostics();
	require(diags.full_key_rechecks >= 1, "memo should recheck full key on hash collision");
	require(diags.hash_collisions >= 1, "memo should record collision on mismatched full key");
}

void test_memo_bit_boundaries_64_and_65() {
	// Проверяем границы хранения S в uint64_t-блоках: ровно 64 работы и переход на 65-ю.
	{
		memo_table memo64(8);
		const std::vector<std::uint64_t> bits64 = { std::uint64_t{1} << 63 };
		memo64.store_exact(bits64, 7, 0xAAULL, 0xBBULL, 11, 0, true);
		const memo_lookup_result found64 = memo64.lookup(bits64, 7, 0xAAULL, 0xBBULL, true);
		require(found64.found && found64.has_exact && found64.exact == 11,
			"memo 64-bit boundary lookup failed");
	}

	{
		memo_table memo65(8);
		const std::vector<std::uint64_t> bits65 = { 0ULL, 1ULL };
		memo65.store_lower_bound(bits65, 13, 0xCCULL, 0xDDULL, 21, true);
		const memo_lookup_result found65 = memo65.lookup(bits65, 13, 0xCCULL, 0xDDULL, true);
		require(found65.found, "memo 65-bit boundary lookup failed");
		require(!found65.has_exact, "memo 65-bit boundary entry should not be exact");
		require(found65.lower_bound == 21, "memo 65-bit boundary lookup returned wrong lower bound");
	}
}

void test_std_unordered_memo_roundtrip() {
	// Reference backend должен соблюдать контракт memo:
	// LB не заменяет exact, exact продвигает LB-entry, ключ содержит S и t.
	memo_std_unordered_table memo(16);
	const std::vector<std::uint64_t> bits = { 0x00000000000000AAULL, 0x1ULL };
	const std::vector<std::uint64_t> other_bits = { 0x00000000000000ABULL, 0x1ULL };
	const schedule_time_t time = 42;

	memo.store_lower_bound(bits, time, 0, 0, 10, true);
	const memo_lookup_result lb = memo.query_lb(bits, time, 0, 0, true);
	require(lb.found && !lb.has_exact, "std_unordered memo should return stored LB");
	require(lb.lower_bound == 10, "std_unordered memo returned wrong LB");

	const memo_lookup_result exact_miss = memo.query_exact(bits, time, 0, 0, true);
	require(!exact_miss.found, "std_unordered query_exact must ignore LB-only entries");

	memo.store_exact(bits, time, 0, 0, 17, 3, true);
	const memo_lookup_result exact = memo.query_exact(bits, time, 0, 0, true);
	require(exact.found && exact.has_exact, "std_unordered memo should promote LB entry to exact");
	require(exact.exact == 17, "std_unordered memo returned wrong exact value");
	require(exact.best_job == 3, "std_unordered memo returned wrong best job");

	memo.store_lower_bound(bits, time, 0, 0, 99, true);
	const memo_lookup_result after_lb = memo.lookup(bits, time, 0, 0, true);
	require(after_lb.found && after_lb.has_exact && after_lb.exact == 17,
		"std_unordered LB store must not overwrite exact entry");

	const memo_lookup_result other_time = memo.lookup(bits, time + 1, 0, 0, true);
	require(!other_time.found, "std_unordered memo key must include t");
	const memo_lookup_result other_set = memo.lookup(other_bits, time, 0, 0, true);
	require(!other_set.found, "std_unordered memo key must include all S blocks");
}

void test_parse_instance_smoke() {
	// Smoke-тест формата входа: n, затем пары p_j d_j.
	std::istringstream input(
		"4\n"
		"3 5\n"
		"2 7\n"
		"4 8\n"
		"1 1\n");
	instance inst;
	std::string error;
	const bool ok = parse_instance(input, inst, &error);
	require(ok, "parse_instance should accept valid input");
	require(error.empty(), "parse_instance returned unexpected error text");
	require(inst.jobs.size() == 4, "parse_instance produced wrong job count");
	require(inst.jobs[0].p == 3 && inst.jobs[3].d == 1, "parse_instance produced wrong job values");
}

void run_test(const std::string& name, const std::function<void()>& fn) {
	fn();
	std::cout << "[ok] " << name << "\n";
}

struct named_test {
	const char* name;
	void (*fn)();
};

const std::vector<named_test>& test_registry() {
	static const std::vector<named_test> tests = {
		{ "test_core", test_core },
		{ "test_generator", test_generator },
		{ "test_bruteforce_small", test_bruteforce_small },
		{ "test_decomposition_modes", test_decomposition_modes },
		{ "test_memo_backends", test_memo_backends },
		{ "test_reconstruction", test_reconstruction },
		{ "test_final_preset", test_final_preset },
		{ "solver_small_instances", test_solver_small_instances },
		{ "adaptive_policies_match_exact_modes_fixed_seeds",
			test_adaptive_policies_match_exact_modes_fixed_seeds },
		{ "custom_memo_matches_reference_backend",
			test_custom_memo_matches_reference_backend },
		{ "memo_large_time_roundtrip", test_memo_large_time_roundtrip },
		{ "memo_hash_collision_rechecks_full_bits", test_memo_hash_collision_rechecks_full_bits },
		{ "memo_bit_boundaries_64_and_65", test_memo_bit_boundaries_64_and_65 },
		{ "std_unordered_memo_roundtrip", test_std_unordered_memo_roundtrip },
		{ "parse_instance_smoke", test_parse_instance_smoke }
	};
	return tests;
}

const named_test* find_test(const std::string& name) {
	for (const named_test& test : test_registry()) {
		if (name == test.name) {
			return &test;
		}
	}
	return nullptr;
}

void print_available_tests() {
	std::cerr << "Available tests:\n";
	for (const named_test& test : test_registry()) {
		std::cerr << "  " << test.name << "\n";
	}
}
} // namespace

int main(int argc, char** argv) {
	try {
		if (argc == 1) {
			for (const named_test& test : test_registry()) {
				run_test(test.name, test.fn);
			}
			return 0;
		}

		for (int i = 1; i < argc; ++i) {
			const std::string name = argv[i];
			const named_test* test = find_test(name);
			if (test == nullptr) {
				std::cerr << "[fail] unknown test: " << name << "\n";
				print_available_tests();
				return 1;
			}
			run_test(test->name, test->fn);
		}
		return 0;
	}
	catch (const std::exception& ex) {
		std::cerr << "[fail] " << ex.what() << "\n";
		return 1;
	}
}
