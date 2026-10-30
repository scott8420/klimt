#pragma once
// core::MaxFlow -- a MINIMUM CUT on a graph (s035 m7: Isolate). GTK-free,
// header-only. docs/paths.md.
//
// Boykov & Kolmogorov, "An Experimental Comparison of Min-Cut/Max-Flow
// Algorithms for Energy Minimization in Vision" (PAMI 2004): two search
// trees, one from the source and one from the sink, grown until they touch;
// augment along the path found; re-adopt the orphans the augmentation made.
// The trees are kept between augmentations -- what makes it fast on the
// grid graphs images give.
//
// Use: add_node() for each node; add_tweights(i, to_source, to_sink) -- the
// cost of cutting node i away from the source (labelling it SINK) and away
// from the sink; add_edge(i, j, cap, rev) between neighbours; maxflow();
// then source_side(i) for each node.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace klimt::core {

class MaxFlow {
public:
    using Cap = float;
    int add_node() {
        m_nodes.push_back({});
        return static_cast<int>(m_nodes.size()) - 1;
    }
    void reserve(std::size_t nodes, std::size_t edges) {
        m_nodes.reserve(nodes);
        m_arcs.reserve(edges * 2);
    }
    // to_source: the capacity from the source to i; to_sink: from i to the sink.
    void add_tweights(int i, Cap to_source, Cap to_sink) {
        Node& n = m_nodes[static_cast<std::size_t>(i)];
        const Cap delta = n.tr_cap;
        if (delta > 0) to_source += delta; else to_sink -= delta;
        m_flow += std::min(to_source, to_sink);
        n.tr_cap = to_source - to_sink;
    }
    void add_edge(int i, int j, Cap cap, Cap rev) {
        const int a = static_cast<int>(m_arcs.size()), b = a + 1;
        m_arcs.push_back({j, m_nodes[static_cast<std::size_t>(i)].first, b, cap});
        m_arcs.push_back({i, m_nodes[static_cast<std::size_t>(j)].first, a, rev});
        m_nodes[static_cast<std::size_t>(i)].first = a;
        m_nodes[static_cast<std::size_t>(j)].first = b;
    }
    // The flow; `stop` is asked now and then (true: give up -- the cut is then
    // whatever the trees hold).
    template <class Stop>
    double maxflow(Stop&& stop) {
        init();
        int cur = -1;
        long long steps = 0;
        for (;;) {
            if ((++steps & 4095) == 0 && stop()) break;
            int i = cur;
            if (i >= 0) {
                Node& n = node(i);
                if (n.parent == kNone) i = -1;   // it was freed meanwhile
            }
            if (i < 0) {
                i = next_active();
                if (i < 0) break;
            }
            // Grow the tree from i.
            int mid = -1;   // the arc from the source tree to the sink tree
            Node& ni = node(i);
            if (!ni.sink) {
                for (int a = ni.first; a >= 0; a = arc(a).next) {
                    if (arc(a).r_cap <= 0) continue;
                    const int j = arc(a).head;
                    Node& nj = node(j);
                    if (nj.parent == kNone) {
                        nj.sink = false;
                        nj.parent = arc(a).sister;
                        nj.ts = ni.ts;
                        nj.dist = ni.dist + 1;
                        set_active(j);
                    } else if (nj.sink) {
                        mid = a;
                        break;
                    } else if (nj.ts <= ni.ts && nj.dist > ni.dist) {
                        nj.parent = arc(a).sister;
                        nj.ts = ni.ts;
                        nj.dist = ni.dist + 1;
                    }
                }
            } else {
                for (int a = ni.first; a >= 0; a = arc(a).next) {
                    if (arc(arc(a).sister).r_cap <= 0) continue;
                    const int j = arc(a).head;
                    Node& nj = node(j);
                    if (nj.parent == kNone) {
                        nj.sink = true;
                        nj.parent = arc(a).sister;
                        nj.ts = ni.ts;
                        nj.dist = ni.dist + 1;
                        set_active(j);
                    } else if (!nj.sink) {
                        mid = arc(a).sister;
                        break;
                    } else if (nj.ts <= ni.ts && nj.dist > ni.dist) {
                        nj.parent = arc(a).sister;
                        nj.ts = ni.ts;
                        nj.dist = ni.dist + 1;
                    }
                }
            }
            ++m_time;
            if (mid >= 0) {
                cur = i;   // keep growing from it next time round
                augment(mid);
                while (!m_orphans.empty()) {
                    const int o = m_orphans.front();
                    m_orphans.pop_front();
                    if (node(o).sink) adopt_sink(o); else adopt_source(o);
                }
            } else {
                cur = -1;
            }
        }
        return m_flow;
    }
    double maxflow() { return maxflow([] { return false; }); }
    bool source_side(int i) const {
        const Node& n = m_nodes[static_cast<std::size_t>(i)];
        return n.parent != kNone && !n.sink;
    }

private:
    static constexpr int kNone = -1, kTerminal = -2, kOrphan = -3;
    static constexpr int kInfinite = std::numeric_limits<int>::max();
    struct Node {
        int first = -1;
        int parent = kNone;
        Cap tr_cap = 0;
        bool sink = false;
        bool active = false;
        int ts = 0, dist = 0;
    };
    struct Arc {
        int head, next, sister;
        Cap r_cap;
    };
    Node& node(int i) { return m_nodes[static_cast<std::size_t>(i)]; }
    Arc& arc(int a) { return m_arcs[static_cast<std::size_t>(a)]; }

    void set_active(int i) {
        Node& n = node(i);
        if (n.active) return;
        n.active = true;
        m_queue.push_back(i);
    }
    int next_active() {
        while (m_head < m_queue.size()) {
            const int i = m_queue[m_head++];
            Node& n = node(i);
            n.active = false;
            if (n.parent != kNone) return i;
        }
        m_queue.clear();
        m_head = 0;
        return -1;
    }
    void init() {
        m_time = 0;
        for (int i = 0; i < static_cast<int>(m_nodes.size()); ++i) {
            Node& n = node(i);
            n.ts = 0;
            n.active = false;
            if (n.tr_cap > 0) { n.sink = false; n.parent = kTerminal; n.dist = 1; set_active(i); }
            else if (n.tr_cap < 0) { n.sink = true; n.parent = kTerminal; n.dist = 1; set_active(i); }
            else n.parent = kNone;
        }
    }
    void orphan(int i) {
        node(i).parent = kOrphan;
        m_orphans.push_back(i);
    }
    void augment(int mid) {
        Cap b = arc(mid).r_cap;
        int i;
        // The source side: from the middle arc's tail up to the source.
        for (i = arc(arc(mid).sister).head;;) {
            const int a = node(i).parent;
            if (a == kTerminal) break;
            b = std::min(b, arc(arc(a).sister).r_cap);
            i = arc(a).head;
        }
        b = std::min(b, node(i).tr_cap);
        // The sink side.
        for (i = arc(mid).head;;) {
            const int a = node(i).parent;
            if (a == kTerminal) break;
            b = std::min(b, arc(a).r_cap);
            i = arc(a).head;
        }
        b = std::min(b, -node(i).tr_cap);
        // Push b along it.
        arc(arc(mid).sister).r_cap += b;
        arc(mid).r_cap -= b;
        for (i = arc(arc(mid).sister).head;;) {
            const int a = node(i).parent;
            if (a == kTerminal) break;
            arc(a).r_cap += b;
            arc(arc(a).sister).r_cap -= b;
            if (arc(arc(a).sister).r_cap <= 0) orphan(i);
            i = arc(a).head;
        }
        node(i).tr_cap -= b;
        if (node(i).tr_cap <= 0) orphan(i);
        for (i = arc(mid).head;;) {
            const int a = node(i).parent;
            if (a == kTerminal) break;
            arc(arc(a).sister).r_cap += b;
            arc(a).r_cap -= b;
            if (arc(a).r_cap <= 0) orphan(i);
            i = arc(a).head;
        }
        node(i).tr_cap += b;
        if (node(i).tr_cap >= 0) orphan(i);
        m_flow += b;
    }
    // A new parent in the same tree with an unbroken way to its terminal;
    // none: the orphan is freed and its children orphaned in turn.
    void adopt(int i, bool sink) {
        int best = -1, dmin = kInfinite;
        Node& ni = node(i);
        for (int a0 = ni.first; a0 >= 0; a0 = arc(a0).next) {
            const Cap cap = sink ? arc(a0).r_cap : arc(arc(a0).sister).r_cap;
            if (cap <= 0) continue;
            int j = arc(a0).head;
            if (node(j).sink != sink || node(j).parent == kNone) continue;
            int d = 0;
            for (;;) {
                Node& nj = node(j);
                if (nj.ts == m_time) { d += nj.dist; break; }
                const int a = nj.parent;
                ++d;
                if (a == kTerminal) { nj.ts = m_time; nj.dist = 1; break; }
                if (a == kOrphan || a == kNone) { d = kInfinite; break; }
                j = arc(a).head;
            }
            if (d < kInfinite) {
                if (d < dmin) { best = a0; dmin = d; }
                for (j = arc(a0).head; node(j).ts != m_time; j = arc(node(j).parent).head) {
                    node(j).ts = m_time;
                    node(j).dist = d--;
                }
            }
        }
        if (best >= 0) {
            ni.parent = best;
            ni.ts = m_time;
            ni.dist = dmin + 1;
            return;
        }
        ni.parent = kNone;
        for (int a0 = ni.first; a0 >= 0; a0 = arc(a0).next) {
            const int j = arc(a0).head;
            Node& nj = node(j);
            if (nj.sink != sink || nj.parent == kNone) continue;
            const Cap cap = sink ? arc(a0).r_cap : arc(arc(a0).sister).r_cap;
            if (cap > 0) set_active(j);
            if (nj.parent != kTerminal && nj.parent != kOrphan && arc(nj.parent).head == i) orphan(j);
        }
    }
    void adopt_source(int i) { adopt(i, false); }
    void adopt_sink(int i) { adopt(i, true); }

    std::vector<Node> m_nodes;
    std::vector<Arc>  m_arcs;
    std::vector<int>  m_queue;
    std::size_t       m_head = 0;
    std::deque<int>   m_orphans;
    int               m_time = 0;
    double            m_flow = 0;
};

}  // namespace klimt::core
