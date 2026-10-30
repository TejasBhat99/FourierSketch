#include "Fourier.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#ifdef Complex
#undef Complex
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kTau = 6.283185307179586476925286766559;

struct Rect {
    int x{}, y{}, w{}, h{};
    bool contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

struct TracePoint {
    fourier::Complex point;
    bool penDown{false};
};

struct AnimatedStroke {
    std::vector<fourier::Complex> source;
    std::vector<fourier::Complex> samples;
    std::vector<int> sampleStates;
    std::vector<fourier::Component> components;
    std::vector<TracePoint> trace;
};

struct Palette {
    unsigned long background{};
    unsigned long panel{};
    unsigned long panelRaised{};
    unsigned long border{};
    unsigned long text{};
    unsigned long muted{};
    unsigned long cyan{};
    unsigned long violet{};
    unsigned long coral{};
    unsigned long yellow{};
    unsigned long ghost{};
};

unsigned long color(Display* display, int screen, const char* hex) {
    XColor parsed{}, exact{};
    if (XAllocNamedColor(display, DefaultColormap(display, screen), hex, &parsed, &exact)) {
        return parsed.pixel;
    }
    return WhitePixel(display, screen);
}

class Painter {
public:
    Painter(Display* display, Drawable drawable, GC gc) : d_(display), target_(drawable), gc_(gc) {}

    void set(unsigned long c, int width = 1) {
        XSetForeground(d_, gc_, c);
        XSetLineAttributes(d_, gc_, width, LineSolid, CapRound, JoinRound);
    }
    void fill(const Rect& r, unsigned long c) {
        set(c);
        XFillRectangle(d_, target_, gc_, r.x, r.y, std::max(0, r.w), std::max(0, r.h));
    }
    void line(int x1, int y1, int x2, int y2, unsigned long c, int width = 1) {
        set(c, width);
        XDrawLine(d_, target_, gc_, x1, y1, x2, y2);
    }
    void circle(int cx, int cy, int radius, unsigned long c, int width = 1) {
        if (radius < 1) return;
        set(c, width);
        XDrawArc(d_, target_, gc_, cx - radius, cy - radius, radius * 2, radius * 2, 0, 360 * 64);
    }
    void dot(int cx, int cy, int radius, unsigned long c) {
        set(c);
        XFillArc(d_, target_, gc_, cx - radius, cy - radius, radius * 2, radius * 2, 0, 360 * 64);
    }
    void text(int x, int y, const std::string& value, unsigned long c) {
        set(c);
        XDrawString(d_, target_, gc_, x, y, value.c_str(), static_cast<int>(value.size()));
    }
    void polyline(const std::vector<fourier::Complex>& points, int ox, int oy,
                  unsigned long c, int width = 1) {
        if (points.size() < 2) return;
        std::vector<XPoint> converted;
        converted.reserve(points.size());
        for (const auto& point : points) {
            converted.push_back({static_cast<short>(std::lround(ox + point.real())),
                                 static_cast<short>(std::lround(oy + point.imag()))});
        }
        set(c, width);
        XDrawLines(d_, target_, gc_, converted.data(), static_cast<int>(converted.size()),
                   CoordModeOrigin);
    }

private:
    Display* d_;
    Drawable target_;
    GC gc_;
};

class App {
public:
    int run() {
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            std::cerr << "Fourier Sketch: cannot open an X11 display.\n"
                      << "Run inside WSLg, or start an X server and set DISPLAY.\n";
            return 1;
        }
        screen_ = DefaultScreen(display_);
        window_ = XCreateSimpleWindow(display_, RootWindow(display_, screen_), 40, 40,
                                      width_, height_, 0, 0, 0);
        XStoreName(display_, window_, "Fourier Sketch - Epicycle Drawing");
        XSelectInput(display_, window_, ExposureMask | KeyPressMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
        wmDelete_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &wmDelete_, 1);

        XSizeHints hints{};
        hints.flags = PMinSize;
        hints.min_width = 900;
        hints.min_height = 600;
        XSetWMNormalHints(display_, window_, &hints);

        gc_ = XCreateGC(display_, window_, 0, nullptr);
        palette_ = {
            color(display_, screen_, "#0B1020"), color(display_, screen_, "#11182B"),
            color(display_, screen_, "#18213A"), color(display_, screen_, "#2B3655"),
            color(display_, screen_, "#ECF4FF"), color(display_, screen_, "#8290AD"),
            color(display_, screen_, "#3DE7E1"), color(display_, screen_, "#A78BFA"),
            color(display_, screen_, "#FF718A"), color(display_, screen_, "#FFD166"),
            color(display_, screen_, "#24304C")};

        recreateBuffer();
        XMapWindow(display_, window_);
        lastTick_ = Clock::now();

        while (running_) {
            processEvents();
            tick();
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        if (buffer_) XFreePixmap(display_, buffer_);
        XFreeGC(display_, gc_);
        XDestroyWindow(display_, window_);
        XCloseDisplay(display_);
        return 0;
    }

private:
    using Clock = std::chrono::steady_clock;

    Rect canvas() const { return {22, 86, splitX() - 38, height_ - 124}; }
    int splitX() const { return static_cast<int>(width_ * 0.65); }
    Rect clearButton() const { return {width_ - 102, 22, 78, 34}; }
    Rect playButton() const { return {width_ - 205, 22, 92, 34}; }
    Rect componentSlider() const { return {splitX() + 24, height_ - 62, width_ - splitX() - 50, 12}; }

    void recreateBuffer() {
        if (buffer_) XFreePixmap(display_, buffer_);
        buffer_ = XCreatePixmap(display_, window_, width_, height_,
                                DefaultDepth(display_, screen_));
    }

    void processEvents() {
        while (XPending(display_)) {
            XEvent event{};
            XNextEvent(display_, &event);
            switch (event.type) {
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == wmDelete_) running_ = false;
                break;
            case ConfigureNotify:
                if (event.xconfigure.width != width_ || event.xconfigure.height != height_) {
                    width_ = event.xconfigure.width;
                    height_ = event.xconfigure.height;
                    recreateBuffer();
                    if (!rawStrokes_.empty()) rebuildDrawing(false);
                }
                break;
            case ButtonPress:
                onMouseDown(event.xbutton.x, event.xbutton.y);
                break;
            case ButtonRelease:
                onMouseUp(event.xbutton.x, event.xbutton.y);
                break;
            case MotionNotify:
                onMouseMove(event.xmotion.x, event.xmotion.y);
                break;
            case KeyPress:
                onKey(XLookupKeysym(&event.xkey, 0));
                break;
            default:
                break;
            }
        }
    }

    void onMouseDown(int x, int y) {
        if (clearButton().contains(x, y)) {
            clear();
            return;
        }
        if (playButton().contains(x, y) && !strokes_.empty()) {
            if (collecting_) {
                collecting_ = false;
                drawing_ = false;
                rawPath_.clear();
                restartTrace();
                playing_ = true;
            } else {
                playing_ = !playing_;
            }
            return;
        }
        if (componentSlider().contains(x, y)) {
            setComponentsFromMouse(x);
            draggingSlider_ = true;
            return;
        }
        if (canvas().contains(x, y)) {
            rawPath_.clear();
            collecting_ = true;
            playing_ = false;
            drawing_ = true;
            rawPath_.push_back({static_cast<double>(x), static_cast<double>(y)});
        }
    }

    void onMouseMove(int x, int y) {
        if (draggingSlider_) {
            setComponentsFromMouse(x);
            return;
        }
        if (!drawing_) return;
        const Rect area = canvas();
        x = std::clamp(x, area.x, area.x + area.w);
        y = std::clamp(y, area.y, area.y + area.h);
        const fourier::Complex point{x * 1.0, y * 1.0};
        if (std::abs(point - rawPath_.back()) >= 2.0) rawPath_.push_back(point);
    }

    void onMouseUp(int, int) {
        draggingSlider_ = false;
        if (!drawing_) return;
        drawing_ = false;
        if (rawPath_.size() >= 2) {
            rawStrokes_.push_back(rawPath_);
            finalizeDrawing();
        }
        rawPath_.clear();
    }

    void onKey(KeySym key) {
        if (key == XK_Escape) running_ = false;
        if (key == XK_c || key == XK_C || key == XK_r || key == XK_R) clear();
        if (key == XK_space && !strokes_.empty()) {
            if (collecting_) {
                collecting_ = false;
                drawing_ = false;
                rawPath_.clear();
                restartTrace();
                playing_ = true;
            } else {
                playing_ = !playing_;
            }
        }
        if ((key == XK_bracketleft || key == XK_Down) && activeComponents_ > 3) {
            activeComponents_ = std::max<std::size_t>(3, activeComponents_ - 2);
            restartTrace();
        }
        if ((key == XK_bracketright || key == XK_Up) &&
            activeComponents_ + 2 <= maxComponents()) {
            activeComponents_ += 2;
            restartTrace();
        }
        if (key == XK_minus || key == XK_underscore) speed_ = std::max(0.25, speed_ / 2.0);
        if (key == XK_plus || key == XK_equal) speed_ = std::min(4.0, speed_ * 2.0);
    }

    void setComponentsFromMouse(int x) {
        if (strokes_.empty()) return;
        const Rect slider = componentSlider();
        const double fraction = std::clamp(static_cast<double>(x - slider.x) / slider.w, 0.0, 1.0);
        std::size_t value = 3 + static_cast<std::size_t>(fraction * (maxComponents() - 3));
        if (value % 2 == 0) ++value;
        activeComponents_ = std::min(value, maxComponents());
        restartTrace();
    }

    std::size_t maxComponents() const {
        std::size_t result = 0;
        for (const auto& stroke : strokes_) result = std::max(result, stroke.components.size());
        return result;
    }

    void clear() {
        rawPath_.clear();
        rawStrokes_.clear();
        strokes_.clear();
        time_ = 0.0;
        drawing_ = false;
        collecting_ = true;
        playing_ = false;
    }

    void finalizeDrawing() {
        rebuildDrawing(true);
        activeComponents_ = std::min<std::size_t>(31, maxComponents());
        if (activeComponents_ % 2 == 0 && activeComponents_ > 1) --activeComponents_;
        playing_ = false;
        restartTrace();
    }

    void rebuildDrawing(bool resetTime) {
        if (rawStrokes_.empty()) return;
        fourier::Complex centroid{0.0, 0.0};
        std::size_t pointCount = 0;
        for (const auto& stroke : rawStrokes_) {
            for (const auto& p : stroke) {
                centroid += p;
                ++pointCount;
            }
        }
        if (pointCount == 0) return;
        centroid /= static_cast<double>(pointCount);

        double maxX = 1.0, maxY = 1.0;
        for (const auto& stroke : rawStrokes_) {
            for (const auto& p : stroke) {
                maxX = std::max(maxX, std::abs(p.real() - centroid.real()));
                maxY = std::max(maxY, std::abs(p.imag() - centroid.imag()));
            }
        }
        const Rect area = canvas();
        const double scale = std::min((area.w * 0.43) / maxX, (area.h * 0.43) / maxY);

        std::vector<AnimatedStroke> rebuilt;
        rebuilt.reserve(rawStrokes_.size());
        for (const auto& raw : rawStrokes_) {
            auto timeline = fourier::resampleStrokes({raw}, 256, 10);
            if (timeline.points.empty()) continue;
            AnimatedStroke stroke;
            stroke.sampleStates = std::move(timeline.strokeIds);
            stroke.source.reserve(raw.size());
            for (const auto& p : raw) stroke.source.push_back((p - centroid) * scale);
            stroke.samples.reserve(timeline.points.size());
            for (const auto& p : timeline.points) stroke.samples.push_back((p - centroid) * scale);
            stroke.components = fourier::decompose(stroke.samples, 101);
            rebuilt.push_back(std::move(stroke));
        }
        strokes_ = std::move(rebuilt);
        if (resetTime) restartTrace();
    }

    void restartTrace() {
        for (auto& stroke : strokes_) stroke.trace.clear();
        time_ = 0.0;
    }

    void tick() {
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
        lastTick_ = now;
        if (!playing_ || strokes_.empty() || drawing_) return;

        const double previous = time_;
        time_ += elapsed * speed_ / 8.0;
        if (time_ >= 1.0) {
            time_ = std::fmod(time_, 1.0);
            for (auto& stroke : strokes_) stroke.trace.clear();
        } else if (time_ < previous) {
            for (auto& stroke : strokes_) stroke.trace.clear();
        }
        for (auto& stroke : strokes_) {
            const std::size_t sampleIndex = std::min(
                static_cast<std::size_t>(time_ * stroke.sampleStates.size()),
                stroke.sampleStates.size() - 1);
            stroke.trace.push_back({fourier::evaluate(
                stroke.components, time_, activeComponents_),
                stroke.sampleStates[sampleIndex] >= 0});
            if (stroke.trace.size() > 1000) stroke.trace.erase(stroke.trace.begin());
        }
    }

    void button(Painter& p, const Rect& rect, const std::string& label, bool active = false) {
        p.fill(rect, active ? palette_.violet : palette_.panelRaised);
        p.line(rect.x, rect.y + rect.h - 1, rect.x + rect.w, rect.y + rect.h - 1,
               active ? palette_.violet : palette_.border);
        const int textX = rect.x + std::max(7, (rect.w - static_cast<int>(label.size()) * 6) / 2);
        p.text(textX, rect.y + 22, label, palette_.text);
    }

    void render() {
        Painter p(display_, buffer_, gc_);
        p.fill({0, 0, width_, height_}, palette_.background);
        drawHeader(p);
        drawCanvas(p);
        drawAnalysis(p);
        XCopyArea(display_, buffer_, window_, gc_, 0, 0, width_, height_, 0, 0);
        XFlush(display_);
    }

    void drawHeader(Painter& p) {
        p.text(24, 29, "FOURIER SKETCH", palette_.text);
        p.text(24, 49, "draw  ->  decompose  ->  reconstruct", palette_.muted);
        button(p, playButton(), playing_ ? "Pause" : "Play", playing_);
        button(p, clearButton(), "Clear");
        p.line(20, 70, width_ - 20, 70, palette_.border);
    }

    void drawCanvas(Painter& p) {
        const Rect area = canvas();
        p.fill(area, palette_.panel);
        p.line(area.x, area.y, area.x + area.w, area.y, palette_.border);
        p.line(area.x, area.y + area.h, area.x + area.w, area.y + area.h, palette_.border);
        p.line(area.x, area.y, area.x, area.y + area.h, palette_.border);
        p.line(area.x + area.w, area.y, area.x + area.w, area.y + area.h, palette_.border);

        const int cx = area.x + area.w / 2;
        const int cy = area.y + area.h / 2;
        p.line(cx - 8, cy, cx + 8, cy, palette_.ghost);
        p.line(cx, cy - 8, cx, cy + 8, palette_.ghost);

        if (collecting_) {
            for (const auto& stroke : rawStrokes_) {
                std::vector<fourier::Complex> local;
                local.reserve(stroke.size());
                for (const auto& point : stroke) local.push_back(point - fourier::Complex(cx, cy));
                p.polyline(local, cx, cy, palette_.cyan, 3);
            }
            std::vector<fourier::Complex> current;
            current.reserve(rawPath_.size());
            for (const auto& point : rawPath_) current.push_back(point - fourier::Complex(cx, cy));
            p.polyline(current, cx, cy, palette_.yellow, 3);
            p.text(area.x + 18, area.y + 28,
                   drawing_ ? "DRAWING STROKE - release to keep it"
                            : "SKETCH MODE - add strokes, then click Play",
                   palette_.cyan);
            if (rawStrokes_.empty() && !drawing_) {
                const int middle = area.y + area.h / 2;
                p.text(cx - 104, middle - 8, "CLICK + DRAG TO DRAW", palette_.text);
            }
            return;
        }

        if (strokes_.empty()) {
            const int middle = area.y + area.h / 2;
            p.text(cx - 104, middle - 14, "CLICK + DRAG TO DRAW", palette_.text);
            p.text(cx - 153, middle + 12,
                   "Add multiple strokes, then click Play", palette_.muted);
            p.circle(cx, middle - 7, 42, palette_.ghost, 1);
            return;
        }

        for (const auto& stroke : strokes_) {
            p.polyline(stroke.source, cx, cy, palette_.ghost, 2);
            for (std::size_t i = 1; i < stroke.trace.size(); ++i) {
                if (stroke.trace[i - 1].penDown && stroke.trace[i].penDown) {
                    p.line(cx + static_cast<int>(std::lround(stroke.trace[i - 1].point.real())),
                           cy + static_cast<int>(std::lround(stroke.trace[i - 1].point.imag())),
                           cx + static_cast<int>(std::lround(stroke.trace[i].point.real())),
                           cy + static_cast<int>(std::lround(stroke.trace[i].point.imag())),
                           palette_.cyan, 3);
                }
            }
        }

        for (const auto& stroke : strokes_) {
            if (stroke.components.empty()) continue;
            fourier::Complex cursor{static_cast<double>(cx), static_cast<double>(cy)};
            cursor += stroke.components.front().coefficient;
            const std::size_t count = std::min(activeComponents_, stroke.components.size());
            for (std::size_t i = 1; i < count; ++i) {
                const auto& component = stroke.components[i];
                const double radius = component.amplitude();
                if (radius >= 1.5) {
                    p.circle(static_cast<int>(std::lround(cursor.real())),
                             static_cast<int>(std::lround(cursor.imag())),
                             static_cast<int>(std::lround(radius)), palette_.border);
                }
                const auto vector = component.coefficient *
                    std::polar(1.0, kTau * component.frequency * time_);
                const auto next = cursor + vector;
                p.line(static_cast<int>(std::lround(cursor.real())),
                       static_cast<int>(std::lround(cursor.imag())),
                       static_cast<int>(std::lround(next.real())),
                       static_cast<int>(std::lround(next.imag())),
                       i % 2 ? palette_.violet : palette_.yellow, 2);
                cursor = next;
            }
            p.dot(static_cast<int>(std::lround(cursor.real())),
                  static_cast<int>(std::lround(cursor.imag())), 4, palette_.coral);
        }

        p.text(area.x + area.w - 226, area.y + 24,
               collecting_ ? "Add strokes, then click Play"
                           : "Drag canvas to add another stroke",
               collecting_ ? palette_.cyan : palette_.muted);

        p.text(area.x + 14, area.y + 24, "SOURCE", palette_.muted);
        p.line(area.x + 72, area.y + 19, area.x + 98, area.y + 19, palette_.ghost, 2);
        p.text(area.x + 115, area.y + 24, "RECONSTRUCTION", palette_.muted);
        p.line(area.x + 226, area.y + 19, area.x + 252, area.y + 19, palette_.cyan, 3);
    }

    void graph(Painter& p, const Rect& rect, bool realPart, const std::string& label,
               unsigned long graphColor) {
        p.fill(rect, palette_.panel);
        p.text(rect.x + 10, rect.y + 18, label, graphColor);
        const int mid = rect.y + rect.h / 2;
        p.line(rect.x + 8, mid, rect.x + rect.w - 8, mid, palette_.ghost);
        if (strokes_.empty()) return;

        double bound = 1.0;
        for (const auto& stroke : strokes_) {
            for (int i = 0; i <= 120; ++i) {
                const auto value = fourier::evaluate(
                    stroke.components, i / 120.0, activeComponents_);
                bound = std::max(bound, std::abs(realPart ? value.real() : value.imag()));
            }
        }
        for (const auto& stroke : strokes_) {
            int lastX = rect.x + 8;
            auto first = fourier::evaluate(stroke.components, 0.0, activeComponents_);
            int lastY = mid - static_cast<int>((realPart ? first.real() : first.imag()) *
                                               (rect.h * 0.37 / bound));
            for (int i = 1; i <= 120; ++i) {
                const double t = i / 120.0;
                const auto value = fourier::evaluate(stroke.components, t, activeComponents_);
                const int x = rect.x + 8 + static_cast<int>(t * (rect.w - 16));
                const int y = mid - static_cast<int>((realPart ? value.real() : value.imag()) *
                                                      (rect.h * 0.37 / bound));
                p.line(lastX, lastY, x, y, graphColor, 1);
                lastX = x;
                lastY = y;
            }
        }
        const int marker = rect.x + 8 + static_cast<int>(time_ * (rect.w - 16));
        p.line(marker, rect.y + 25, marker, rect.y + rect.h - 7, palette_.coral);
    }

    void drawAnalysis(Painter& p) {
        const int left = splitX() + 18;
        const int right = width_ - 22;
        const int panelWidth = std::max(100, right - left);
        p.text(left, 100, "FOURIER DECOMPOSITION", palette_.text);
        p.text(left, 120, "z(t): reconstructed complex-valued path", palette_.muted);
        p.text(left, 138, "z(t) = sum_k c_k exp(i*2*pi*k*t)", palette_.text);
        p.text(left, 156, "x(t) = Re[z(t)] - horizontal position", palette_.cyan);
        p.text(left, 174, "y(t) = Im[z(t)] - vertical position", palette_.violet);

        const int graphHeight = std::max(80, (height_ - 320) / 3);
        drawSpectrum(p, {left, 192, panelWidth, graphHeight});
        graph(p, {left, 206 + graphHeight, panelWidth, graphHeight}, true,
              "X(t) - real part of z(t)", palette_.cyan);
        graph(p, {left, 220 + graphHeight * 2, panelWidth, graphHeight}, false,
              "Y(t) - imaginary part of z(t)", palette_.violet);

        if (!strokes_.empty()) {
            std::ostringstream status;
            status << rawStrokes_.size() << (rawStrokes_.size() == 1 ? " stroke  |  " : " strokes  |  ")
                   << activeComponents_ << "/" << maxComponents() << " terms each  |  "
                   << std::fixed << std::setprecision(2) << speed_ << "x speed";
            p.text(left, height_ - 75, status.str(), palette_.text);
            const Rect slider = componentSlider();
            p.fill(slider, palette_.ghost);
            const double fraction = maxComponents() <= 3 ? 1.0 :
                static_cast<double>(activeComponents_ - 3) / (maxComponents() - 3);
            p.fill({slider.x, slider.y, static_cast<int>(slider.w * fraction), slider.h},
                   palette_.violet);
            p.dot(slider.x + static_cast<int>(slider.w * fraction), slider.y + slider.h / 2,
                  7, palette_.text);
        } else {
            p.text(left, height_ - 54, "Waveforms appear after you draw.", palette_.muted);
        }
        p.text(24, height_ - 16,
               "Space play/pause   [ ] components   - + speed   C clear   Esc quit",
               palette_.muted);
    }

    void drawSpectrum(Painter& p, const Rect& rect) {
        p.fill(rect, palette_.panel);
        p.text(rect.x + 10, rect.y + 18, "AMPLITUDE BY FREQUENCY", palette_.yellow);
        if (strokes_.empty()) return;

        const std::size_t count = std::min<std::size_t>(31, activeComponents_);
        std::vector<double> amplitudes(count, 0.0);
        for (const auto& stroke : strokes_) {
            for (std::size_t i = 0; i < count && i < stroke.components.size(); ++i) {
                amplitudes[i] += stroke.components[i].amplitude();
            }
        }
        double maximum = 1.0;
        for (std::size_t i = 1; i < count; ++i) maximum = std::max(maximum, amplitudes[i]);
        const int plotTop = rect.y + 28;
        const int plotBottom = rect.y + rect.h - 18;
        const double cell = static_cast<double>(rect.w - 20) / count;
        for (std::size_t i = 0; i < count; ++i) {
            const int barHeight = i == 0 ? 0 : static_cast<int>(
                (plotBottom - plotTop) * amplitudes[i] / maximum);
            const int x = rect.x + 10 + static_cast<int>(i * cell);
            const int width = std::max(1, static_cast<int>(cell) - 2);
            p.fill({x, plotBottom - barHeight, width, barHeight},
                   (i == 0 || i % 2 == 1) ? palette_.yellow : palette_.coral);
        }
        p.line(rect.x + 8, plotBottom, rect.x + rect.w - 8, plotBottom, palette_.border);
    }

    Display* display_{};
    int screen_{};
    Window window_{};
    Pixmap buffer_{};
    GC gc_{};
    Atom wmDelete_{};
    Palette palette_{};
    int width_{1280};
    int height_{780};
    bool running_{true};
    bool drawing_{false};
    bool collecting_{true};
    bool draggingSlider_{false};
    bool playing_{false};
    double time_{0.0};
    double speed_{1.0};
    std::size_t activeComponents_{31};
    Clock::time_point lastTick_{};
    std::vector<fourier::Complex> rawPath_;
    std::vector<std::vector<fourier::Complex>> rawStrokes_;
    std::vector<AnimatedStroke> strokes_;
};

}

int main() { return App{}.run(); }
