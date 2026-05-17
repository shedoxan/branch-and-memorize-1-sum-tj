#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

/// Номер работы в массивах экземпляра.
/// uint32_t выбран как безопасный компактный тип.
/// В теории работы образуют N = {1,...,n}; в коде используется индексация 0..n-1.
using job_id_t = std::uint32_t;

/// Время обработки p_j.
/// Вход сейчас явно ограничивается диапазоном uint16_t.
/// Накопленные суммы времен обработки считаются в schedule_time_t.
using processing_time_t = std::uint16_t;

/// Директивный срок d_j.
/// Вход ограничивается диапазоном uint32_t.
using due_date_t = std::uint32_t;

/// Время расписания: старт t, завершение C_j и суммы p(S).
/// Используется для накопленных сумм времен обработки.
using schedule_time_t = std::uint64_t;

/// Значение целевой функции F(pi) = sum_j T_j(pi).
/// Суммарное запаздывание может быть существенно больше отдельных p_j и d_j.
using schedule_cost_t = std::uint64_t;

/// Работа j из множества N.
/// Теоретические поля: p_j -- время обработки, d_j -- директивный срок.
struct job {
	processing_time_t p = 0;
	due_date_t d = 0;
};

/// Экземпляр задачи 1||sum T_j.
/// Поле jobs представляет множество всех работ N; номер работы -- индекс в этом векторе.
struct instance {
	std::vector<job> jobs;
};

/// Расписание pi и его стоимость.
/// order хранит номера работ в порядке выполнения; cost = sum_j T_j(pi).
struct schedule {
	std::vector<job_id_t> order;
	schedule_cost_t cost = 0;
};

/// Запаздывание одной работы:
/// T_j = max(C_j - d_j, 0).
schedule_cost_t tardiness(schedule_time_t completion_C, due_date_t due_date_d);

/// Вычисляет F(pi) = sum_j T_j(pi) для заданного порядка работ.
/// Функция не оптимизирует расписание, а только оценивает заданную перестановку pi.
/// По умолчанию расписание начинается в момент t = 0.
schedule_cost_t evaluate_sum_tardiness(const instance& inst, const std::vector<job_id_t>& order, schedule_time_t start_t = 0);

/// Читает экземпляр из потока в формате:
/// n, затем n строк "p_j d_j".
/// Возвращает false и заполняет error, если вход нарушает формат или диапазоны типов.
bool parse_instance(std::istream& in, instance& out, std::string* error = nullptr);
