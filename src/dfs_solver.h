#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

#include "memo_backend.h"
#include "solver.h"

/// Рабочие буферы одной глубины DFS.
/// Они не являются частью математического состояния: состояние подзадачи — только (S,t).
/// Буферы выделяются заранее и переиспользуются, чтобы не создавать вектора на каждой ветви.
struct dfs_depth_scratch {
	/// Работы из текущего S в EDD-порядке: по неубыванию d_j.
	std::vector<int> edd_jobs;
	/// Работы из текущего S в LPT-порядке: по невозрастанию p_j.
	std::vector<int> lpt_jobs;
	/// Кэш локальных EDD/LPT-порядков для текущего множества S.
	bool order_cache_ready = false;
	std::uint64_t order_cache_hash = 0;
	std::uint64_t order_cache_fingerprint = 0;
	int order_cache_count = -1;

	/// Допустимые позиции r для центральной работы Lawler [q]_S,
	/// где q = max { i : p_[i]_S = p_max(S) } в EDD(S).
	std::vector<int> lawler_r_positions;
	/// Prefix sums p(prefix) в локальном EDD(S); используются для C_[q]_S(r,t).
	std::vector<long long> edd_prefix_p;

	/// Кандидаты, которые могут стоять перед [1]_S в Decomposition II.
	std::vector<int> candidates_before_earliest;
	/// Допустимые размеры левого блока перед [1]_S в Szwarc / Decomposition II.
	std::vector<int> szwarc_positions;
	/// Prefix sums p(A) для блоков A перед [1]_S.
	std::vector<long long> before_prefix;
	/// Значения Rule 4 для позиционного фильтра Lawler/Szwarc.
	std::vector<long long> trial_tardiness_values;
	/// Временные метки для быстрого выбора кандидатов без очистки массива bool.
	std::vector<std::uint32_t> candidate_marks;
	std::uint32_t candidate_mark_epoch = 1;

	/// Временные bitset-блоки. uint64_t оставлен специально: это компактное хранение S.
	std::vector<std::uint64_t> prefix_bits;
	std::vector<std::uint64_t> b_bits;
	std::vector<std::uint64_t> a_bits;
	std::vector<std::uint64_t> c_bits;
};

/// Изменяемое состояние текущего пути DFS.
/// Инвариант перед входом в solve_state(depth,t): remaining_bits задаёт S,
/// remaining_count = |S|, а perm_jobs[0..depth) уже зафиксированы в префиксе расписания.
struct solver_runtime_state {
	/// Префикс восстанавливаемого расписания; best_job из memo уточняет следующий элемент.
	std::vector<int> perm_jobs;
	/// S в подзадаче (S,t): bitset работ, не зафиксированных в частичном расписании.
	std::vector<std::uint64_t> remaining_bits;
	/// Zobrist-ключи для быстрого хеширования S; fingerprint -- независимая проверка.
	std::vector<std::uint64_t> zobrist_job;
	std::vector<std::uint64_t> zobrist_job_fp;
	/// Глобальный EDD-порядок N; локальный EDD(S) получается фильтрацией по remaining_bits.
	std::vector<int> edd_order;
	/// Глобальный LPT-порядок N; локальный LPT(S) нужен для Lawler и Szwarc.
	std::vector<int> lpt_order;
	/// Хеш текущего S. В memo ключ всё равно проверяется полным bitset-ом.
	std::uint64_t subset_hash = 0;
	std::uint64_t subset_fingerprint = 0;
	/// |S|, хранится отдельно, чтобы не пересчитывать popcount на каждом узле.
	int remaining_count = 0;
	/// Рабочие буферы по глубинам recursion tree.
	std::vector<dfs_depth_scratch> scratch_by_depth;
	/// Дешёвая оценка числа ветвей в узле; используется adaptive v2/v3 при выборе режима.
	std::vector<int> estimated_branches_by_depth;
};

/// Точный DFS Branch-and-Memorize solver для 1||ΣT_j.
/// Математическое состояние подзадачи: (S,t), где S — оставшиеся работы,
/// t — момент начала этой подзадачи. t входит в ключ, так как T_j зависит от C_j,
/// а C_j для тех же S меняется при другом старте t.
class dfs_solver final : public solver {
public:
	explicit dfs_solver(dfs_config config = {});
	solve_result solve(const instance& inst) override;

private:
	void initialize_runtime_state(const instance& inst);
	void finalize_stats_from_memo();
	void check_time_limit();
	void build_jobs_in_order_for_bits(const std::vector<int>& source_order,
		const std::vector<std::uint64_t>& bits,
		int remaining_count,
		std::vector<int>& out) const;
	void build_remaining_jobs_in_order(const std::vector<int>& global_order, std::vector<int>& out) const;

	/// Решает OPT(S,t): минимальную дополнительную сумму запаздываний для текущего S
	/// при старте в момент current_time = t.
	/// runtime_.remaining_bits/remaining_count должны уже задавать S.
	/// known_lookup передаёт уже найденную memo-запись, чтобы не делать повторный lookup.
	long long solve_state(int depth, schedule_time_t current_time, const memo_lookup_result* known_lookup, bool track_stats);

	/// Проверяет точные terminal rules для простых S; при успехе возвращает exact OPT(S,t).
	bool try_terminal_rules(int depth, schedule_time_t current_time,
		const std::vector<int>& edd_jobs,
		const std::vector<int>& lpt_jobs,
		long long& exact,
		int& first_job,
		std::uint64_t*& hit_counter);
	/// Дешёвая допустимая нижняя оценка LB(S,t). Она не является полной LB1/LB2 из конспекта.
	long long lower_bound_additional(int depth, schedule_time_t current_time) const;
	/// Строит допустимое продолжение расписания и возвращает feasible UB для OPT(S,t).
	long long heuristic_upper_bound_edd(int depth, schedule_time_t current_time,
		const std::vector<int>& jobs, int* first_job, bool track_stats);
	/// Восстанавливает оптимальный порядок по exact memo-записям и полю best_job.
	std::vector<job_id_t> reconstruct_order(long long optimal_cost);

	/// Таблица памяти M ключуется полной подзадачей (S,t), а не только S.
	/// Для одной и той же S разные t дают разные C_j и, значит, разные T_j.
	memo_lookup_result query_memo(schedule_time_t current_time, bool track_stats);
	/// Сохраняет M[(S,t)] = OPT(S,t) и лучшую первую работу best_job для reconstruction.
	void store_exact_memo(schedule_time_t current_time, long long exact, int best_job, bool track_stats);
	/// Сохраняет только LB(S,t). Такая запись полезна для pruning, но не для reconstruction.
	void store_lower_bound_memo(schedule_time_t current_time, long long lower_bound, bool track_stats);

	/// Удаляет/возвращает работу из текущего S при спуске и откате DFS.
	void remove_job_from_state(int job_idx);
	void restore_job_to_state(int job_idx);
	bool is_job_remaining(int job_idx) const;

	dfs_config config_;
	const instance* inst_ = nullptr;
	int n_ = 0;

	solver_runtime_state runtime_{};
	memo_backend_table memo_;
	solver_stats stats_{};
	std::chrono::steady_clock::time_point solve_start_{};
	std::chrono::steady_clock::time_point deadline_{};
	bool has_time_limit_ = false;
	std::uint64_t time_check_counter_ = 0;
};
