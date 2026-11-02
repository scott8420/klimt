#pragma once
// core::select::MagneticTrace -- the MAGNETIC LASSO (s079). GTK-free; the
// canvas feeds it the pointer, the selftest walks it. docs/selection.md.
//
// Photoshop's: click to start, then just move -- a LIVE WIRE runs from the
// last anchor to the pointer along the strongest edges (the cheapest path
// over 1 - edge strength, 8-connected, in a window round the two), the
// pointer itself snapped to the strongest edge within Width. Anchors are
// laid as the wire grows (Frequency) and by a click; Backspace takes the
// last; a click back on the start, a double-click or Enter closes it (the
// wire to the start); Esc drops the lot.
//
// Edge strength is the gradient (Sobel) of what `Lum` reads -- the canvas
// passes the picked layer's luminance (alpha-weighted: a cut-out's outline is
// an edge). Edges weaker than Contrast (of the strongest in the window) are
// not edges. No `Lum`: the wire is straight (a polygon lasso).
#include "core/Selection.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace klimt::core::select {

class MagneticTrace {
public:
    using Lum = std::function<float(int x, int y)>;   // 0..1, inside the image
    struct Params {
        double width = 10;       // px: how far from the pointer an edge is looked for
        double contrast = 0.10;  // 0..1: weaker edges are ignored
        int    frequency = 57;   // 0..100: how often anchors are laid (Photoshop's 57)
    };
    enum class State { Idle, Open, Done, Cancelled };

    // `near`: how close (image px) counts as "on the start".
    void start(Pt p, Lum lum, int width, int height, const Params& prm, double near);
    void set_near(double near) { m_near = near; }
    void move(Pt p);          // the wire follows (anchors may be laid)
    void click(Pt p);         // an anchor where the wire ends -- or, on the start, the close
    bool back();              // Backspace: the last anchor (false: only the start is left)
    void finish();            // Enter / double-click: the wire to the start, closed
    void cancel();

    State state() const { return m_state; }
    bool active() const { return m_state == State::Open; }
    bool done() const { return m_state == State::Done; }
    // The outline so far: what is fixed, then the live wire.
    std::vector<Pt> points() const;
    const std::vector<Pt>& fixed() const { return m_fixed; }
    const std::vector<Pt>& wire() const { return m_wire; }
    std::vector<Pt> anchors() const;
    // The edge strength at a pixel (0..1 of the strongest the trace has
    // seen; 0 outside the image) -- the tests' window on it.
    float edge(int x, int y) const;

    // The cheapest path from a to b (pixel centres, both included), in the
    // window round them. Pure but for the luminance cache.
    std::vector<Pt> path(Pt a, Pt b) const;
    Pt snap(Pt p) const;     // the strongest edge within Width (p if none)

private:
    float lum(int x, int y) const;
    float grad(int x, int y) const;   // raw gradient magnitude
    void lay_auto();          // anchors along a long wire
    double spacing() const;

    State m_state = State::Idle;
    Lum m_lum;
    int m_w = 0, m_h = 0;
    Params m_p;
    double m_near = 4;
    std::vector<Pt> m_fixed;              // anchors and the paths between them
    std::vector<std::size_t> m_anchor_at; // indices in m_fixed of each anchor (the start first)
    std::vector<Pt> m_wire;               // last anchor .. the pointer (snapped)
    Pt m_tip{};                           // where the pointer is, snapped
    mutable std::map<std::pair<int, int>, std::vector<float>> m_lum_cache;   // 64 x 64 blocks
};

}  // namespace klimt::core::select
