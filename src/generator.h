#pragma once

#include <cstdint>
#include <string>

#include "core.h"

/// Параметры генератора Potts-style экземпляров 1||sum T_j.
/// Генератор строит N из n работ, затем задаёт p_j и d_j.
struct potts_generation_config {
	/// n = |N| -- число работ.
	int n = 0;

	/// Нижняя граница равномерной генерации p_j.
	int p_min = 1;

	/// Верхняя граница равномерной генерации p_j.
	int p_max = 100;

	/// RDD: относительный разброс директивных сроков d_j.
	double due_range = 0.2;

	/// TF: tardiness factor, сдвигает окно сроков относительно p(N).
	double tardiness_factor = 0.6;

	/// Seed псевдослучайного генератора; нужен для воспроизводимых экспериментов.
	std::uint64_t seed = 1;
};

/// Проверяет параметры генератора до построения экземпляра.
/// Возвращает false, если возможны значения вне диапазонов типов p_j или d_j.
bool validate_potts_generation_config(const potts_generation_config& cfg, std::string* error = nullptr);

/// Генератор по умолчанию
/// Использует article-compatible способ генерации d_j.
instance generate_potts_instance(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed);
instance generate_potts_instance(const potts_generation_config& cfg);

/// Article-compatible режим:
/// p_j ~ U[p_min, p_max];
/// d_j сначала выбираем значения из исходного окна, затем обрезаем снизу нулём.
instance generate_potts_instance_article(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed);
instance generate_potts_instance_article(const potts_generation_config& cfg);

/// Uniform-clamped режим:
/// сначала обрезаются границы окна d_j, затем выполняется равномерная генерация.
instance generate_potts_instance_uniform(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed);
instance generate_potts_instance_uniform(const potts_generation_config& cfg);
