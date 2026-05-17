#pragma once

#include <cstddef>
#include <cstdint>

/// Счётчики и таймеры одного запуска solver.
/// Структура не участвует в алгоритме; она нужна для проверки точности,
/// сравнения режимов и анализа работы памяти M[(S,t)].
struct solver_stats {
	/// Число посещённых узлов рекурсии Solve(S,t).
	std::uint64_t nodes = 0;
	/// Число входов в рекурсивную функцию; обычно совпадает с nodes.
	std::uint64_t recursive_calls = 0;
	/// Число пустых или завершённых без ветвления подзадач.
	std::uint64_t leaves = 0;
	/// Максимальная глубина рекурсии, то есть длина зафиксированного префикса расписания.
	std::uint64_t max_depth = 0;

	/// Сколько ветвей отброшено по LB/UB-оценкам.
	std::uint64_t pruned_by_bound = 0;
	/// Сколько подзадач закрыто точной записью M[(S,t)] = OPT(S,t).
	std::uint64_t pruned_by_memo_exact = 0;
	/// Сколько подзадач закрыто сохранённой нижней оценкой M[(S,t)] = LB(S,t).
	std::uint64_t pruned_by_memo_lb = 0;

	/// Число построенных ветвей декомпозиции.
	std::uint64_t branches_generated = 0;
	/// Число построенных, но не решённых полностью ветвей.
	std::uint64_t branches_pruned = 0;
	/// Отсечения, где решающим было LB(S,t).
	std::uint64_t lb_prunes = 0;
	/// Отсечения, где решающим был текущий UB.
	std::uint64_t ub_prunes = 0;
	/// Вызовы дешёвой нижней оценки LB(S,t).
	std::uint64_t simple_lb_calls = 0;
	/// Отсечения, в которых участвовала дешёвая LB(S,t).
	std::uint64_t simple_lb_prunes = 0;
	/// Вызовы построения допустимого расписания-UB.
	std::uint64_t ub_calls = 0;
	/// Сколько раз текущий UB улучшался.
	std::uint64_t ub_improvements = 0;
	/// Попадания в сохранённые lower bound записи.
	std::uint64_t memo_lb_hits = 0;
	/// Попадания в точные записи OPT(S,t).
	std::uint64_t memo_exact_hits = 0;

	/// Terminal rule: все оставшиеся работы уже запаздывают, оптимален SPT.
	std::uint64_t terminal_all_tardy_spt_hits = 0;
	/// Terminal rule: EDD даёт не более одной запаздывающей работы.
	std::uint64_t terminal_edd_one_tardy_hits = 0;

	/// Сколько раз строились локальные EDD/LPT/SPT списки для текущего S.
	std::uint64_t ordering_scans = 0;
	/// Сколько глобальных сортировок по EDD/LPT выполнено при инициализации.
	std::uint64_t ordering_sorts = 0;
	/// Сколько кандидатов позиции r рассмотрено до отсечения.
	std::uint64_t valid_positions_built = 0;
	/// Сколько позиций r убрано Rule 1: центральная работа r уже не должна стоять дальше.
	std::uint64_t valid_positions_pruned_3a = 0;
	/// Сколько позиций r убрано обобщённым Rule 3; Rule 2 является его частным случаем.
	std::uint64_t valid_positions_pruned_3b = 0;
	/// Кандидаты позиций r до position filtering.
	std::uint64_t valid_positions_before = 0;
	/// Кандидаты позиций r после position filtering.
	std::uint64_t valid_positions_after = 0;
	/// Сколько позиций r отброшено position filtering.
	std::uint64_t positions_pruned = 0;
	/// Общее число кандидатных позиций до фильтрации.
	std::uint64_t candidate_positions_before = 0;
	/// Общее число кандидатных позиций после фильтрации.
	std::uint64_t candidate_positions_after = 0;
	/// Позиции, отброшенные базовыми правилами Lawler: Rule 1 и обобщённым Rule 3.
	std::uint64_t positions_pruned_by_lawler_basic = 0;
	/// Позиции, отброшенные Rule 4: сравнением T(pi^(r)) для пробных EDD-последовательностей.
	std::uint64_t positions_pruned_by_lawler_rule4 = 0;
	/// Позиции, отброшенные Szwarc-вариантом Rule 4.
	std::uint64_t positions_pruned_by_szwarc_rule4 = 0;

	/// Узлы, где фактически выполнялся блок Decomposition I (Lawler).
	std::uint64_t lawler_nodes = 0;
	/// Узлы, где фактически выполнялся блок Decomposition II (Szwarc).
	std::uint64_t szwarc_nodes = 0;
	/// Узлы, где фактически выполнялся совместный блок Decomposition I + II.
	std::uint64_t both_nodes = 0;
	/// Adaptive выбрал Lawler для текущего состояния (S,t).
	std::uint64_t adaptive_choices_lawler = 0;
	/// Adaptive выбрал Szwarc / Decomposition II.
	std::uint64_t adaptive_choices_szwarc = 0;
	/// Adaptive выбрал double decomposition I + II.
	std::uint64_t adaptive_choices_both = 0;
	/// Унифицированный счётчик adaptive-выбора Lawler.
	std::uint64_t adaptive_choice_lawler = 0;
	/// Унифицированный счётчик adaptive-выбора Szwarc.
	std::uint64_t adaptive_choice_szwarc = 0;

	/// Adaptive v1 выбрал Lawler / Decomposition I.
	std::uint64_t adaptive_v1_choices_lawler = 0;
	/// Adaptive v1 выбрал Szwarc / Decomposition II.
	std::uint64_t adaptive_v1_choices_szwarc = 0;
	/// Adaptive v1 выбрал double decomposition I + II.
	std::uint64_t adaptive_v1_choices_both = 0;
	/// Adaptive v2 выбрал Lawler / Decomposition I.
	std::uint64_t adaptive_v2_choices_lawler = 0;
	/// Adaptive v2 выбрал Szwarc / Decomposition II.
	std::uint64_t adaptive_v2_choices_szwarc = 0;
	/// Adaptive v2 выбрал double decomposition I + II.
	std::uint64_t adaptive_v2_choices_both = 0;
	/// Adaptive v3 выбрал Lawler / Decomposition I.
	std::uint64_t adaptive_v3_choices_lawler = 0;
	/// Adaptive v3 выбрал Szwarc / Decomposition II.
	std::uint64_t adaptive_v3_choices_szwarc = 0;
	/// Adaptive v3 выбрал double decomposition I + II.
	std::uint64_t adaptive_v3_choices_both = 0;

	/// Версия adaptive policy, использованная в запуске: 0=v1, 1=v2, 2=v3.
	std::uint64_t adaptive_policy_used = 0;

	/// Любое успешное обращение к memo_table.
	std::uint64_t memo_hits = 0;
	/// Неуспешное обращение к memo_table.
	std::uint64_t memo_misses = 0;
	/// Запросы к exact-части M[(S,t)].
	std::uint64_t memo_exact_queries = 0;
	/// Запросы к LB-части M[(S,t)].
	std::uint64_t memo_lb_queries = 0;
	/// Новые записи в M.
	std::uint64_t memo_inserts = 0;
	/// Сохранения exact-записей OPT(S,t).
	std::uint64_t memo_exact_stores = 0;
	/// То же значение, что memo_exact_stores, под коротким именем для CSV.
	std::uint64_t memo_stores_exact = 0;
	/// Сохранения LB-записей.
	std::uint64_t memo_lb_stores = 0;
	/// То же значение, что memo_lb_stores, под коротким именем для CSV.
	std::uint64_t memo_stores_lb = 0;
	/// Обновления существующих записей M.
	std::uint64_t memo_updates = 0;
	/// Удаления записей M политикой очистки.
	std::uint64_t memo_evictions = 0;
	/// Удаления записей, которые содержали exact OPT(S,t).
	std::uint64_t memo_evictions_exact = 0;
	/// Удаления записей, которые содержали только LB.
	std::uint64_t memo_evictions_lb = 0;
	/// Запись не добавлена: места в M не удалось освободить.
	std::uint64_t memo_rejected_no_room = 0;
	/// Принудительные удаления для освобождения места.
	std::uint64_t memo_forced_evictions = 0;
	/// Сколько раз запускалась очистка memo_table.
	std::uint64_t memo_clean_calls = 0;
	/// Сколько LUFO-проходов старения записей выполнено.
	std::uint64_t memo_lufo_decay_passes = 0;

	/// Повторные подзадачи: успешные memo hits как проявление Branch-and-Memorize.
	std::uint64_t duplicate_subproblem_hits = 0;
	/// Коллизии hash/start_time, после которых полный ключ S не совпал.
	std::uint64_t hash_collisions = 0;
	/// Сколько раз выполнялась полная проверка bitset-ключа S.
	std::uint64_t full_key_rechecks = 0;

	/// Максимальное число записей в M за запуск.
	std::size_t memo_peak_size = 0;
	/// Число записей в M в конце запуска.
	std::size_t memo_final_size = 0;
	/// Оценка памяти, занятой M, в байтах.
	std::size_t memo_used_bytes = 0;
	/// Та же оценка памяти M под финальным именем CSV.
	std::size_t memo_memory_used_bytes = 0;
	/// Бюджет памяти M, если он задан.
	std::size_t memo_budget_bytes = 0;

	/// Полное время решения.
	double elapsed_ms = 0.0;
	/// Время вычисления lower bounds.
	double bound_time_ms = 0.0;
	/// Время построения heuristic UB.
	double upper_bound_time_ms = 0.0;
	/// Время проверки terminal rules.
	double terminal_time_ms = 0.0;
	/// Время построения локальных порядков EDD/LPT/SPT.
	double ordering_time_ms = 0.0;
	/// Время построения и фильтрации допустимых позиций r.
	double valid_positions_time_ms = 0.0;
	/// То же время, что valid_positions_time_ms, под явным именем для CSV.
	double time_spent_in_position_filtering_ms = 0.0;
	/// Время lookup в M[(S,t)].
	double memo_lookup_time_ms = 0.0;
	/// Время сохранения записей в M.
	double memo_store_time_ms = 0.0;
	/// Время очистки/eviction в M.
	double memo_clean_time_ms = 0.0;
	/// То же время, что memo_clean_time_ms, под коротким именем для CSV.
	double cleanup_time_ms = 0.0;
};
