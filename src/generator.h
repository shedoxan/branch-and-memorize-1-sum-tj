#pragma once

#include <cstdint>
#include <string>

#include "core.h"

struct potts_generation_config {
	int n = 0;
	int p_min = 1;
	int p_max = 100;
	double due_range = 0.2;			// RDD
	double tardiness_factor = 0.6;	// TF
	std::uint64_t seed = 1;
};

bool validate_potts_generation_config(const potts_generation_config& cfg, std::string* error = nullptr);

// Default generator kept for CLI/backward compatibility.
// It matches the article-style due-date generation.
instance generate_potts_instance(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed);
instance generate_potts_instance(const potts_generation_config& cfg);

// 1) Article 
// p_j ~ U[p_min, p_max]
// d_j ~ U[minD, maxD], then d_j := max(0, d_j)
instance generate_potts_instance_article(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed);
instance generate_potts_instance_article(const potts_generation_config& cfg);

// 2) Mathematically cleaner
// clamp bounds first: d_j ~ U[clamp(minD), clamp(maxD)], with clamp to [0, DMAX]
instance generate_potts_instance_uniform(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed);
instance generate_potts_instance_uniform(const potts_generation_config& cfg);
