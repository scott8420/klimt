// core::rulers -- units and ruler ticks (s031). See core/Rulers.hpp.
#include "core/Rulers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace klimt::core::rulers {

const std::vector<Unit>& units() {
    static const std::vector<Unit> v = {Unit::Pixels, Unit::Inches, Unit::Centimetres, Unit::Millimetres,
                                        Unit::Points, Unit::Picas, Unit::Percent};
    return v;
}

std::string_view label(Unit u) {
    switch (u) {
        case Unit::Pixels: return "Pixels";
        case Unit::Inches: return "Inches";
        case Unit::Centimetres: return "Centimetres";
        case Unit::Millimetres: return "Millimetres";
        case Unit::Points: return "Points";
        case Unit::Picas: return "Picas";
        case Unit::Percent: return "Percent";
    }
    return "Pixels";
}

std::string_view key(Unit u) {
    switch (u) {
        case Unit::Pixels: return "px";
        case Unit::Inches: return "in";
        case Unit::Centimetres: return "cm";
        case Unit::Millimetres: return "mm";
        case Unit::Points: return "pt";
        case Unit::Picas: return "pc";
        case Unit::Percent: return "%";
    }
    return "px";
}

Unit from_key(std::string_view k) {
    for (const auto u : units()) if (key(u) == k) return u;
    return Unit::Pixels;
}

double px_per_unit(Unit u, double dpi, double extent) {
    dpi = dpi > 0 ? dpi : 72;
    switch (u) {
        case Unit::Pixels: return 1;
        case Unit::Inches: return dpi;
        case Unit::Centimetres: return dpi / 2.54;
        case Unit::Millimetres: return dpi / 25.4;
        case Unit::Points: return dpi / 72;
        case Unit::Picas: return dpi / 6;
        case Unit::Percent: return std::max(1.0, extent) / 100;
    }
    return 1;
}

namespace {
// A number without trailing zeros: up to `decimals` places.
std::string trimmed(double v, int decimals) {
    if (std::abs(v) < 1e-9) v = 0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
int decimals_for(double step) {
    int d = 0;
    while (d < 4 && std::abs(step * std::pow(10, d) - std::round(step * std::pow(10, d))) > 1e-6) ++d;
    return d;
}
}  // namespace

std::string format(double px, Unit u, double dpi, double extent, bool with_unit) {
    const double v = px / px_per_unit(u, dpi, extent);
    const int dec = u == Unit::Pixels ? 0 : u == Unit::Millimetres || u == Unit::Points || u == Unit::Percent ? 1 : 2;
    std::string s = trimmed(v, dec);
    if (with_unit) s += u == Unit::Percent ? "%" : " " + std::string(key(u));
    return s;
}

double major_step(Unit u, double dpi, double extent, double zoom, double min_gap) {
    const double ppu = px_per_unit(u, dpi, extent) * std::max(1e-9, zoom);   // screen px per unit
    const double want = min_gap / ppu;                                          // units between labels, at least
    // Inches below one: halves down to sixteenths (a rule's marks).
    if (u == Unit::Inches && want < 1)
        for (const double s : {1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2})
            if (s >= want) return s;
    double p = std::pow(10.0, std::floor(std::log10(std::max(want, 1e-12))));
    for (int guard = 0; guard < 40; ++guard, p *= 10)
        for (const double m : {1.0, 2.0, 5.0})
            if (m * p >= want - 1e-12) {
                // Pixels never step below one.
                if (u == Unit::Pixels && m * p < 1) continue;
                return m * p;
            }
    return want;
}

std::vector<Tick> ticks(Unit u, double dpi, double extent, double origin, double from, double to, double zoom, double min_gap) {
    std::vector<Tick> out;
    if (!(zoom > 0) || to <= from) return out;
    const double ppu = px_per_unit(u, dpi, extent);
    const double step = major_step(u, dpi, extent, zoom, min_gap);   // units
    // Minor divisions: as fine as stays 4+ screen px apart.
    const bool binary = u == Unit::Inches;
    int div = 1;
    for (const int n : binary ? std::initializer_list<int>{8, 4, 2} : std::initializer_list<int>{10, 5, 2})
        if (step / n * ppu * zoom >= 4) { div = n; break; }
    if (u == Unit::Pixels) while (div > 1 && step / div < 1) div /= 2;   // never between pixels
    const double minor = step / div;
    const int dec = decimals_for(step);
    // From the zero point, in units.
    const double u0 = (from - origin) / ppu, u1 = (to - origin) / ppu;
    const long long k0 = static_cast<long long>(std::floor(u0 / minor)), k1 = static_cast<long long>(std::ceil(u1 / minor));
    if (k1 - k0 > 20000) return out;   // never a runaway
    for (long long k = k0; k <= k1; ++k) {
        const double v = k * minor;
        Tick t;
        t.at = origin + v * ppu;
        const long long i = ((k % div) + div) % div;
        t.level = i == 0 ? 0 : (div % 2 == 0 && i == div / 2) ? 1 : 2;
        if (t.level == 0) t.label = trimmed(v, dec);
        out.push_back(std::move(t));
    }
    return out;
}

}  // namespace klimt::core::rulers

namespace klimt::core::canvas {

bool set_ruler_origin(DocJournal& j, Document& d, double x, double y) {
    Image next = d.image();
    if (next.origin_x == x && next.origin_y == y) return false;
    next.origin_x = x;
    next.origin_y = y;
    return j.run("Ruler Origin", {kImageId}, [&] {
        Layer im;
        im.name = "Image";
        im.image = std::make_shared<const Image>(next);
        d.put(kImageId, im);
    });
}

}  // namespace klimt::core::canvas

namespace klimt::core::canvas {

namespace {
bool put_image(DocJournal& j, Document& d, const Image& next, const std::string& label) {
    return j.run(label, {kImageId}, [&] {
        Layer im;
        im.name = "Image";
        im.image = std::make_shared<const Image>(next);
        d.put(kImageId, im);
    });
}
bool off_picture(const Image& im, const Guide& g) {
    const double side = g.vertical ? im.width : im.height;
    return g.at < 0 || g.at > side;
}
}  // namespace

bool add_guide(DocJournal& j, Document& d, Guide g) {
    Image next = d.image();
    g.at = std::round(g.at);
    if (next.width <= 0 || off_picture(next, g)) return false;
    for (const auto& o : next.guides) if (o == g) return false;
    next.guides.push_back(g);
    return put_image(j, d, next, g.vertical ? "New Vertical Guide" : "New Horizontal Guide");
}

bool move_guide(DocJournal& j, Document& d, std::size_t i, double at) {
    Image next = d.image();
    if (i >= next.guides.size()) return false;
    Guide g = next.guides[i];
    g.at = std::round(at);
    if (off_picture(next, g)) return remove_guide(j, d, i);   // dragged off: gone
    if (g == next.guides[i]) return false;
    next.guides[i] = g;
    return put_image(j, d, next, "Move Guide");
}

bool remove_guide(DocJournal& j, Document& d, std::size_t i) {
    Image next = d.image();
    if (i >= next.guides.size()) return false;
    next.guides.erase(next.guides.begin() + static_cast<long>(i));
    return put_image(j, d, next, "Delete Guide");
}

bool clear_guides(DocJournal& j, Document& d) {
    Image next = d.image();
    if (next.guides.empty()) return false;
    next.guides.clear();
    return put_image(j, d, next, "Clear Guides");
}

}  // namespace klimt::core::canvas

namespace klimt::core::guides {

double snap(double v, bool vertical, const Image& im, double tol, bool edges, bool* snapped) {
    double best = v, dist = tol;
    bool hit_any = false;
    const auto consider = [&](double t) {
        const double dd = std::abs(v - t);
        if (dd <= dist) { dist = dd; best = t; hit_any = true; }
    };
    for (const auto& g : im.guides) if (g.vertical == vertical) consider(g.at);
    if (edges) {
        const double side = vertical ? im.width : im.height;
        consider(0);
        consider(side);
        consider(side / 2);
    }
    if (snapped) *snapped = hit_any;
    return best;
}

int hit(const Image& im, double x, double y, double tol) {
    int best = -1;
    double dist = tol;
    for (std::size_t i = 0; i < im.guides.size(); ++i) {
        const auto& g = im.guides[i];
        const double dd = std::abs((g.vertical ? x : y) - g.at);
        if (dd <= dist) { dist = dd; best = static_cast<int>(i); }
    }
    return best;
}

}  // namespace klimt::core::guides
