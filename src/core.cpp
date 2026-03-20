#include "core.h"

#include <limits>
#include <sstream>
#include <utility>

schedule_cost_t tardiness(schedule_time_t completion_time, due_date_t due_date) {
	return (completion_time > due_date) ? (completion_time - due_date) : 0;
}

schedule_cost_t evaluate_sum_tardiness(const instance& inst, const std::vector<int>& order) {
	schedule_time_t t = 0;
	schedule_cost_t sum = 0;
	for (int i : order) {
		const std::size_t idx = static_cast<std::size_t>(i);
		t += inst.jobs[idx].p;
		sum += tardiness(t, inst.jobs[idx].d);
	}
	return sum;
}

bool parse_instance(std::istream& in, instance& out, std::string* error) {
	if (error) {
		error->clear();
	}

	instance parsed;

	std::string line;
	int line_number = 0;
	int n = -1;

	while (std::getline(in, line)) {
		++line_number;

		if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos) {
			continue;
		}

		long long n_value = 0;
		char extra = '\0';
		std::istringstream ss(line);
		if (!(ss >> n_value) || (ss >> extra) || n_value < 0 || n_value > std::numeric_limits<int>::max()) {
			if (error) {
				*error = "Expected non-negative integer n on line " + std::to_string(line_number) + ".";
			}
			return false;
		}

		n = static_cast<int>(n_value);
		break;
	}

	if (n < 0) {
		if (error) {
			*error = "Input does not contain number of jobs.";
		}
		return false;
	}

	parsed.jobs.reserve(static_cast<std::size_t>(n));
	long long total_processing_time = 0;

	while (parsed.jobs.size() < static_cast<std::size_t>(n) && std::getline(in, line)) {
		++line_number;

		if (line.empty() || line.find_first_not_of(" \t\r") == std::string::npos) {
			continue;
		}

		long long p_value = 0;
		long long d_value = 0;
		char extra = '\0';
		std::istringstream ss(line);
		if (!(ss >> p_value >> d_value) || (ss >> extra)) {
			if (error) {
				*error = "Invalid job format at line " + std::to_string(line_number) + ".";
			}
			return false;
		}

		if (p_value <= 0) {
			if (error) {
				*error = "Processing time must be positive at line " + std::to_string(line_number) + ".";
			}
			return false;
		}

		if (p_value > static_cast<long long>(std::numeric_limits<processing_time_t>::max())
			|| d_value < 0
			|| d_value > static_cast<long long>(std::numeric_limits<due_date_t>::max())) {
			if (error) {
				*error = "Job value out of range at line " + std::to_string(line_number) + ".";
			}
			return false;
		}

		total_processing_time += p_value;
		if (total_processing_time > std::numeric_limits<int>::max()) {
			if (error) {
				*error = "Total processing time exceeds supported limit.";
			}
			return false;
		}

		job j;
		j.p = static_cast<processing_time_t>(p_value);
		j.d = static_cast<due_date_t>(d_value);
		parsed.jobs.push_back(j);
	}

	if (parsed.jobs.size() != static_cast<std::size_t>(n)) {
		if (error) {
			*error = "Expected " + std::to_string(n) + " jobs but parsed " + std::to_string(parsed.jobs.size()) + ".";
		}
		return false;
	}

	out = std::move(parsed);
	return true;
}
