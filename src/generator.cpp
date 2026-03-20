#include "generator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>


namespace {
void set_error(std::string* error, const std::string& text) {
	if (error) {
		*error = text;
	}
}

enum class due_mode {
    article,
    uniform,
};

instance generate_potts_instance_impl(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed, due_mode mode) {
    potts_generation_config cfg;
    cfg.n = n;
    cfg.p_min = p_min;
    cfg.p_max = p_max;
    cfg.due_range = due_range;
    cfg.tardiness_factor = tardiness_factor;
    cfg.seed = seed;

    std::string error;
    if (!validate_potts_generation_config(cfg, &error)) {
        throw std::invalid_argument(error.empty() ? "Invalid Potts generator configuration." : error);
    }

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> p_dist(p_min, p_max);

    instance inst;
    inst.jobs.reserve(static_cast<std::size_t>(n));

    long long total_processing = 0;
    for (int i = 0; i < n; ++i) {
        const int p = p_dist(rng);
        total_processing += p;

        job j;
        j.p = static_cast<processing_time_t>(p);
        j.d = 0;
        inst.jobs.push_back(j);
    }

    const double lower_factor = 1.0 - tardiness_factor - due_range * 0.5;
    const double upper_factor = 1.0 - tardiness_factor + due_range * 0.5;

    long long due_low  = static_cast<long long>(static_cast<double>(total_processing) * lower_factor);
    long long due_high = static_cast<long long>(static_cast<double>(total_processing) * upper_factor);
    if (due_low > due_high) std::swap(due_low, due_high);

    const long long D_MIN = 0;
    const long long D_MAX = static_cast<long long>(std::numeric_limits<due_date_t>::max());

    if (mode == due_mode::uniform) {
        due_low  = std::clamp(due_low,  D_MIN, D_MAX);
        due_high = std::clamp(due_high, D_MIN, D_MAX);
        if (due_low > due_high) std::swap(due_low, due_high);

        std::uniform_int_distribution<long long> d_dist(due_low, due_high);
        for (job& j : inst.jobs) {
            j.d = static_cast<due_date_t>(d_dist(rng));
        }
        return inst;
    }

    std::uniform_int_distribution<long long> d_dist(due_low, due_high);
    for (job& j : inst.jobs) {
        long long d = d_dist(rng);
        if (d < 0) d = 0;
        if (d > D_MAX) d = D_MAX;
        j.d = static_cast<due_date_t>(d);
    }
    return inst;
}
} // namespace


bool validate_potts_generation_config(const potts_generation_config& cfg, std::string* error) {
	if (error) {
		error->clear();
	}

	if (cfg.n <= 0) {
		set_error(error, "n must be > 0.");
		return false;
	}
	if (cfg.p_min <= 0) {
		set_error(error, "p_min must be > 0.");
		return false;
	}
	if (cfg.p_min > cfg.p_max) {
		set_error(error, "p_min must be <= p_max.");
		return false;
	}
	if (cfg.p_max > static_cast<int>(std::numeric_limits<processing_time_t>::max())) {
		set_error(error, "p_max is out of processing_time_t range.");
		return false;
	}
	if (!std::isfinite(cfg.due_range) || cfg.due_range < 0.0 || cfg.due_range > 2.0) {
		set_error(error, "due_range must be finite and in [0, 2].");
		return false;
	}
	if (!std::isfinite(cfg.tardiness_factor) || cfg.tardiness_factor < 0.0 || cfg.tardiness_factor > 1.0) {
		set_error(error, "tardiness_factor must be finite and in [0, 1].");
		return false;
	}

	const long double max_total_processing = static_cast<long double>(cfg.n) * static_cast<long double>(cfg.p_max);
	if (max_total_processing > static_cast<long double>(std::numeric_limits<int>::max())) {
		set_error(error, "n * p_max exceeds supported total processing limit (INT_MAX).");
		return false;
	}

	const long double due_upper_factor =
		1.0L - static_cast<long double>(cfg.tardiness_factor) + static_cast<long double>(cfg.due_range) * 0.5L;
	const long double max_due_value = max_total_processing * std::max<long double>(0.0L, due_upper_factor);
	if (max_due_value > static_cast<long double>(std::numeric_limits<due_date_t>::max())) {
		set_error(error, "Generated due dates may exceed due_date_t range. Reduce n/p_max or due_range.");
		return false;
	}

	return true;
}

instance generate_potts_instance(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed) {
	return generate_potts_instance_article(n, p_min, p_max, due_range, tardiness_factor, seed);
}

instance generate_potts_instance(const potts_generation_config& cfg) {
	return generate_potts_instance_article(cfg);
}

instance generate_potts_instance_article(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed) {
    return generate_potts_instance_impl(n, p_min, p_max, due_range, tardiness_factor, seed, due_mode::article);
}

instance generate_potts_instance_uniform(int n, int p_min, int p_max, double due_range, double tardiness_factor, std::uint64_t seed) {
    return generate_potts_instance_impl(n, p_min, p_max, due_range, tardiness_factor, seed, due_mode::uniform);
}

instance generate_potts_instance_article(const potts_generation_config& cfg) {
    return generate_potts_instance_article(cfg.n, cfg.p_min, cfg.p_max, cfg.due_range, cfg.tardiness_factor, cfg.seed);
}

instance generate_potts_instance_uniform(const potts_generation_config& cfg) {
    return generate_potts_instance_uniform(cfg.n, cfg.p_min, cfg.p_max, cfg.due_range, cfg.tardiness_factor, cfg.seed);
}
