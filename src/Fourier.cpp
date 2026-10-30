#include "Fourier.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace fourier {
namespace {
constexpr double kTau = 6.283185307179586476925286766559;
}

double Component::amplitude() const { return std::abs(coefficient); }
double Component::phase() const { return std::arg(coefficient); }

std::vector<Complex> resamplePath(const std::vector<Complex>& points,
                                  std::size_t sampleCount) {
    if (points.empty() || sampleCount == 0) return {};
    if (points.size() == 1) return std::vector<Complex>(sampleCount, points.front());

    std::vector<double> cumulative(points.size(), 0.0);
    for (std::size_t i = 1; i < points.size(); ++i) {
        cumulative[i] = cumulative[i - 1] + std::abs(points[i] - points[i - 1]);
    }
    const double total = cumulative.back();
    if (total < 1e-9) return std::vector<Complex>(sampleCount, points.front());

    std::vector<Complex> result;
    result.reserve(sampleCount);
    std::size_t segment = 1;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const double target = total * static_cast<double>(i) /
                              static_cast<double>(sampleCount);
        while (segment + 1 < cumulative.size() && cumulative[segment] < target) {
            ++segment;
        }
        const double startDistance = cumulative[segment - 1];
        const double segmentLength = cumulative[segment] - startDistance;
        const double u = segmentLength > 1e-12 ? (target - startDistance) / segmentLength : 0.0;
        result.push_back(points[segment - 1] * (1.0 - u) + points[segment] * u);
    }
    return result;
}

StrokeSamples resampleStrokes(const std::vector<std::vector<Complex>>& strokes,
                              std::size_t sampleCount,
                              std::size_t travelSamples) {
    std::vector<const std::vector<Complex>*> valid;
    std::vector<double> lengths;
    double totalInkLength = 0.0;
    for (const auto& stroke : strokes) {
        if (stroke.size() < 2) continue;
        double length = 0.0;
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            length += std::abs(stroke[i] - stroke[i - 1]);
        }
        if (length < 1e-9) continue;
        valid.push_back(&stroke);
        lengths.push_back(length);
        totalInkLength += length;
    }
    if (valid.empty() || sampleCount == 0) return {};

    const std::size_t minimumInk = valid.size() * 2;
    sampleCount = std::max(sampleCount, minimumInk + valid.size() * travelSamples);
    const std::size_t inkBudget = sampleCount - valid.size() * travelSamples;

    std::vector<std::size_t> counts(valid.size(), 2);
    std::size_t remaining = inkBudget - minimumInk;
    std::vector<double> fractions(valid.size(), 0.0);
    std::size_t assigned = 0;
    for (std::size_t i = 0; i < valid.size(); ++i) {
        const double exact = remaining * lengths[i] / totalInkLength;
        const auto extra = static_cast<std::size_t>(std::floor(exact));
        counts[i] += extra;
        fractions[i] = exact - extra;
        assigned += extra;
    }
    while (assigned < remaining) {
        const auto best = static_cast<std::size_t>(std::distance(
            fractions.begin(), std::max_element(fractions.begin(), fractions.end())));
        ++counts[best];
        fractions[best] = -1.0;
        ++assigned;
    }

    StrokeSamples result;
    result.points.reserve(sampleCount);
    result.strokeIds.reserve(sampleCount);
    for (std::size_t strokeIndex = 0; strokeIndex < valid.size(); ++strokeIndex) {
        const auto& stroke = *valid[strokeIndex];
        std::vector<double> cumulative(stroke.size(), 0.0);
        for (std::size_t i = 1; i < stroke.size(); ++i) {
            cumulative[i] = cumulative[i - 1] + std::abs(stroke[i] - stroke[i - 1]);
        }

        std::size_t segment = 1;
        for (std::size_t i = 0; i < counts[strokeIndex]; ++i) {
            const double target = lengths[strokeIndex] * static_cast<double>(i) /
                                  static_cast<double>(counts[strokeIndex] - 1);
            while (segment + 1 < cumulative.size() && cumulative[segment] < target) {
                ++segment;
            }
            const double startDistance = cumulative[segment - 1];
            const double segmentLength = cumulative[segment] - startDistance;
            const double u = segmentLength > 1e-12
                ? (target - startDistance) / segmentLength : 0.0;
            result.points.push_back(stroke[segment - 1] * (1.0 - u) + stroke[segment] * u);
            result.strokeIds.push_back(static_cast<int>(strokeIndex));
        }

        const Complex from = stroke.back();
        const Complex to = valid[(strokeIndex + 1) % valid.size()]->front();
        for (std::size_t i = 1; i <= travelSamples; ++i) {
            const double u = static_cast<double>(i) / static_cast<double>(travelSamples + 1);
            result.points.push_back(from * (1.0 - u) + to * u);
            result.strokeIds.push_back(-1);
        }
    }
    return result;
}

std::vector<Component> decompose(const std::vector<Complex>& samples,
                                 std::size_t maxComponents) {
    if (samples.empty() || maxComponents == 0) return {};
    const std::size_t count = std::min(maxComponents, samples.size());
    std::vector<Component> result;
    result.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        int frequency = 0;
        if (index != 0) {
            const int magnitude = static_cast<int>((index + 1) / 2);
            frequency = (index % 2 == 1) ? magnitude : -magnitude;
        }

        Complex coefficient{0.0, 0.0};
        for (std::size_t n = 0; n < samples.size(); ++n) {
            const double angle = -kTau * static_cast<double>(frequency) *
                                 static_cast<double>(n) /
                                 static_cast<double>(samples.size());
            coefficient += samples[n] * std::polar(1.0, angle);
        }
        coefficient /= static_cast<double>(samples.size());
        result.push_back({frequency, coefficient});
    }
    return result;
}

Complex evaluate(const std::vector<Component>& components, double t,
                 std::size_t componentCount) {
    Complex value{0.0, 0.0};
    const std::size_t count = std::min(componentCount, components.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& component = components[i];
        value += component.coefficient *
                 std::polar(1.0, kTau * static_cast<double>(component.frequency) * t);
    }
    return value;
}

}
