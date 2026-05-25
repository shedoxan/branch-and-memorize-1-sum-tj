#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "core.h"
#include "stats.h"

/// Стратегия выбора декомпозиции множества S в узлах Branch-and-Memorize.
enum class DecompositionMode : int {
	/// Adaptive: эвристически выбрать между Decomposition I и II.
	Adaptive = 0,

	/// Lawler: Decomposition I.
	/// EDD(S)=([1]_S,...,[ell]_S).
	/// Центральная работа — [q]_S, где
	/// q = max { i : p_[i]_S = p_max(S) }.
	Lawler = 1,

	/// Szwarc: Decomposition II.
	/// Центральная работа — b = [1]_S,
	/// то есть первая работа в EDD(S), работа с минимальным d_j.
	Szwarc = 2,

	/// BothLawlerSzwarc: совместное применение Decomposition I и II:
	/// используются центральные работы [q]_S и [1]_S.
	BothLawlerSzwarc = 3
};

/// Реализация таблицы memoization M[(S,t)].
enum class memo_backend_kind {
	custom = 0,
	std_unordered = 1
};

/// Версия эвристики adaptive.
enum class adaptive_policy_kind {
	v1 = 0,
	v2 = 1,
	v3 = 2
};

/// Управляет сбором внутренних таймеров.
struct profiling_control {
	bool enabled = true;
};

/// Группа позиционных правил.
struct position_filtering_config {
	/// Общий выключатель position filtering.
	bool enabled = true;
	/// Базовые правила сокращения позиций для центральной работы Lawler.
	bool enable_lawler_basic_rules = true;
	/// Rule 4: сравнение значений T(π^(r)) для пробных EDD-последовательностей.
	bool enable_rule4 = true;
};

/// Группа нижних/верхних оценок.
struct bounds_config {
	/// Простая LB(S,t): каждая работа завершается не раньше t+p_j.
	bool enable_simple_lb = false;
	/// Использовать сохранённые LB из memo для отсечений.
	bool enable_lb_memo = false;
	/// EDD даёт допустимое UB-продолжение.
	bool enable_edd_ub = false;
	/// Дополнительный ограничитель глубины UB; -1 означает без нового ограничения.
	int ub_depth_limit = -1;
	/// Дополнительный ограничитель глубины LB; -1 означает без нового ограничения.
	int lb_depth_limit = -1;
};

/// Группа terminal rules: точные частные случаи.
struct terminal_rules_config {
	bool enable_all_tardy_spt = true;
	bool enable_edd_at_most_one_tardy = true;
};

/// Группа memoization M[(S,t)].
struct memo_config {
	bool enable_memo = true;
	bool enable_exact_memo = true;
	bool enable_lb_memo = false;
	bool full_key_verification = true;
	std::size_t capacity = 200000;
	std::size_t memory_limit_bytes = 0;
	bool strict_memory_cap = true;
	bool use_process_memory_gate = false;
	memo_backend_kind backend = memo_backend_kind::custom;
};

/// Конфигурация точного DFS Branch-and-Memorize solver.
/// Состояние подзадачи: (S,t), где S — оставшиеся работы, t — старт подзадачи.
struct dfs_config {
	std::uint64_t zobrist_seed = 1;
	double time_limit_sec = 0.0;
	bool reconstruct_order = true;
	bool reconstruction_trace = false;

	/// Какой точный способ ветвления использовать для состояния (S,t).
	DecompositionMode decomposition_mode = DecompositionMode::Adaptive;
	/// Версия adaptive-эвристики; используется только при decomposition_mode == Adaptive.
	adaptive_policy_kind adaptive_policy = adaptive_policy_kind::v3;
	position_filtering_config position_filtering{};
	bounds_config bounds{};
	terminal_rules_config terminal_rules{};
	memo_config memo{};

	profiling_control profiling{};
};

const char* to_string(DecompositionMode mode);
const char* to_string(memo_backend_kind backend);
const char* to_string(adaptive_policy_kind policy);

bool parse_decomposition_mode(const std::string& text, DecompositionMode& out);
bool parse_memo_backend_kind(const std::string& text, memo_backend_kind& out);
bool parse_adaptive_policy_kind(const std::string& text, adaptive_policy_kind& out);

/// Результат решения одного экземпляра.
struct solve_result {
	schedule best;
	solver_stats stats;
};

/// Штатная остановка по лимиту времени.
class solver_time_limit_exceeded final : public std::runtime_error {
public:
	explicit solver_time_limit_exceeded(const solver_stats& partial_stats)
		: std::runtime_error("solver time limit exceeded"), stats(partial_stats) {}

	solver_stats stats;
};

/// Абстрактный интерфейс solver для задачи 1||ΣT_j.
class solver {
public:
	virtual ~solver() = default;
	virtual solve_result solve(const instance& inst) = 0;
};
