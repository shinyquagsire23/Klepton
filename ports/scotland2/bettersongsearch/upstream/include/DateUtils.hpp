#pragma once

#include <chrono>
#include <string>

namespace BetterSongSearch {
    int GetMonthsSinceDate(int64_t since) {
        auto currentEpoch = std::chrono::system_clock::now().time_since_epoch();
        auto epoch2 = std::chrono::seconds(since);
        auto monthsSince = std::chrono::duration_cast<std::chrono::months>(currentEpoch - epoch2);
        return monthsSince.count();
    }

    // Add calendar months to a date
    std::chrono::system_clock::time_point GetTimepointAfterMonths(int64_t sinceEpoch, int64_t addMonths) {
        std::chrono::seconds tp(sinceEpoch);

        // Get timepoint
        std::chrono::system_clock::time_point timePoint{tp};
        auto sd = floor<std::chrono::days>(tp);
        // Record the time of day
        auto time_of_day = tp - sd;
        // Convert to a y/m/d calendar data structure
        std::chrono::year_month_day ymd{(std::chrono::sys_days) sd};
        // Add the months
        ymd += std::chrono::months{addMonths};
        // Add some policy for overflowing the day-of-month if desired
        if (!ymd.ok()) {
            ymd = ymd.year() / ymd.month() / std::chrono::last;
        }
        // Convert back to system_clock::time_point
        return std::chrono::sys_days{ymd} + time_of_day;
    }

    // Add calendar months to a date
    std::chrono::seconds GetDateAfterMonths(int64_t sinceEpoch, int64_t addMonths) {
        auto timepoint = GetTimepointAfterMonths(sinceEpoch, addMonths);

        return duration_cast<std::chrono::seconds>(timepoint.time_since_epoch());
    }

}  // namespace BetterSongSearch
