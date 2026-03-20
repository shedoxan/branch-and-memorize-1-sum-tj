#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

using processing_time_t = std::uint16_t;	
using due_date_t = std::uint32_t;
using schedule_time_t = std::uint64_t;		// C			
using schedule_cost_t = std::uint64_t;		// Sum-Tj

struct job {
	processing_time_t p = 0;
	due_date_t d = 0;
};

struct instance {
	std::vector<job> jobs;
};

struct schedule {
	std::vector<int> order;
	schedule_cost_t cost = 0;
};

schedule_cost_t tardiness(schedule_time_t completion_time, due_date_t due_date);
schedule_cost_t evaluate_sum_tardiness(const instance& inst, const std::vector<int>& order);

bool parse_instance(std::istream& in, instance& out, std::string* error = nullptr);
