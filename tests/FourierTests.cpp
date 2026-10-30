#include "Fourier.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
constexpr double kTau = 6.283185307179586476925286766559;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

int main() {
    std::vector<fourier::Complex> circle;
    constexpr std::size_t n = 128;
    for (std::size_t i = 0; i < n; ++i) {
        circle.push_back(std::polar(75.0, kTau * static_cast<double>(i) / n));
    }

    const auto components = fourier::decompose(circle, 31);
    require(components.size() == 31, "component count");
    require(components[1].frequency == 1, "positive frequency order");
    require(std::abs(components[1].amplitude() - 75.0) < 1e-8,
            "circle has one dominant coefficient");
    require(std::abs(fourier::evaluate(components, 0.37, components.size()) -
                     std::polar(75.0, kTau * 0.37)) < 1e-7,
            "circle reconstructs exactly");

    std::vector<fourier::Complex> line{{0, 0}, {100, 0}};
    const auto samples = fourier::resamplePath(line, 5);
    require(samples.size() == 5, "resample count");
    require(std::abs(samples[2].real() - 40.0) < 1e-9, "arc-length interpolation");

    const std::vector<std::vector<fourier::Complex>> strokes{
        {{0, 0}, {10, 0}}, {{40, 30}, {50, 30}}};
    const auto disconnected = fourier::resampleStrokes(strokes, 64, 4);
    require(disconnected.points.size() == 64, "multi-stroke sample count");
    require(disconnected.strokeIds.size() == disconnected.points.size(),
            "multi-stroke labels align with samples");
    require(std::count(disconnected.strokeIds.begin(), disconnected.strokeIds.end(), -1) == 8,
            "pen-up travel is marked after each stroke");
    require(std::find(disconnected.strokeIds.begin(), disconnected.strokeIds.end(), 0) !=
                disconnected.strokeIds.end() &&
            std::find(disconnected.strokeIds.begin(), disconnected.strokeIds.end(), 1) !=
                disconnected.strokeIds.end(),
            "both disconnected strokes are retained");

    std::cout << "All Fourier tests passed.\n";
    return 0;
}
