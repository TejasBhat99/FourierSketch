#pragma once

#include <complex>
#include <vector>

namespace fourier {

using Complex = std::complex<double>;

struct Component {
    int frequency{};
    Complex coefficient{};

    double amplitude() const;
    double phase() const;
};

struct StrokeSamples {
    std::vector<Complex> points;
    std::vector<int> strokeIds;
};

std::vector<Complex> resamplePath(const std::vector<Complex>& points,
                                  std::size_t sampleCount);

StrokeSamples resampleStrokes(const std::vector<std::vector<Complex>>& strokes,
                              std::size_t sampleCount,
                              std::size_t travelSamples = 10);

std::vector<Component> decompose(const std::vector<Complex>& samples,
                                 std::size_t maxComponents);

Complex evaluate(const std::vector<Component>& components, double t,
                 std::size_t componentCount);

}
