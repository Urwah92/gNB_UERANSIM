#include <ue/rls/mobility.hpp>

#include <climits>
#include <iostream>
#include <limits>
#include <stdexcept>

static void expect(const std::vector<Vector3> &points, double velocity, double elapsed, bool loop, Vector3 expected)
{
    const auto position = nr::ue::SampleMobilityPosition(points, velocity, elapsed, loop);
    if (position.x != expected.x || position.y != expected.y || position.z != expected.z)
        throw std::runtime_error("Unexpected mobility position");
}

int main()
{
    expect({}, 10, 20, false, {0, 0, 0});
    expect({{3, 4, 5}}, 10, 20, true, {3, 4, 5});
    expect({{0, 0, 0}, {100, 0, 0}}, 0, 20, true, {0, 0, 0});
    expect({{0, 0, 0}, {100, 0, 0}}, 2.5, 10, false, {25, 0, 0});
    expect({{0, 0, 0}, {100, 0, 0}}, 10, 1000, false, {100, 0, 0});
    expect({{0, 0, 0}, {100, 0, 0}}, 10, 15, true, {50, 0, 0});
    expect({{0, 0, 0}, {100, 0, 0}}, 10, 20, true, {0, 0, 0});
    expect({{1, 2, 3}, {1, 2, 3}}, 10, 20, true, {1, 2, 3});
    expect({{0, 0, 0}, {0, 0, 0}, {30, 40, 0}}, 5, 5, false, {15, 20, 0});
    expect({{INT_MIN, 0, 0}, {INT_MAX, 0, 0}}, 1, 0, false, {INT_MIN, 0, 0});
    expect({{0, 0, 0}, {100, 0, 0}}, std::numeric_limits<double>::infinity(), 1, false, {0, 0, 0});
    std::cout << "Mobility tests passed\n";
}
