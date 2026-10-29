#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <utils/common_types.hpp>

namespace nr::ue
{

// Coordinates are metres. Sample by elapsed time without expanding long/slow paths in memory.
inline Vector3 SampleMobilityPosition(const std::vector<Vector3> &points, double velocity, double elapsedSeconds,
                                     bool loop)
{
    if (points.empty())
        return {};
    if (points.size() == 1 || !std::isfinite(velocity) || velocity <= 0.0 || !std::isfinite(elapsedSeconds) ||
        elapsedSeconds <= 0.0)
        return points.front();

    const size_t segments = loop ? points.size() : points.size() - 1;
    auto length = [&points](size_t i) {
        const auto &a = points[i];
        const auto &b = points[(i + 1) % points.size()];
        return std::hypot(std::hypot(static_cast<double>(b.x) - a.x, static_cast<double>(b.y) - a.y),
                          static_cast<double>(b.z) - a.z);
    };
    double total = 0.0;
    for (size_t i = 0; i < segments; ++i)
        total += length(i);
    if (total == 0.0)
        return points.front();

    const double duration = total / velocity;
    if (!loop && elapsedSeconds >= duration)
        return points.back();
    double remaining = (loop ? std::fmod(elapsedSeconds, duration) : elapsedSeconds) * velocity;
    for (size_t i = 0; i < segments; ++i)
    {
        const double distance = length(i);
        if (distance == 0.0)
            continue;
        if (remaining <= distance)
        {
            const auto &a = points[i];
            const auto &b = points[(i + 1) % points.size()];
            const double ratio = std::clamp(remaining / distance, 0.0, 1.0);
            return {static_cast<int>(std::lround(a.x + (static_cast<double>(b.x) - a.x) * ratio)),
                    static_cast<int>(std::lround(a.y + (static_cast<double>(b.y) - a.y) * ratio)),
                    static_cast<int>(std::lround(a.z + (static_cast<double>(b.z) - a.z) * ratio))};
        }
        remaining -= distance;
    }
    return loop ? points.front() : points.back();
}

} // namespace nr::ue
