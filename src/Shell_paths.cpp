// Shell -- PATHS (s035): path LAYERS. The picked layer, when it's a path
// layer, is the path on the canvas and the one the Pen edits; on any other
// layer the Pen's first click makes a new path layer above it. Ctrl+Enter,
// Make Path from Selection (a job), Isolate (a job). docs/paths.md.
#include "Shell.hpp"
#include "Log.hpp"
#include "core/Isolate.hpp"
#include "core/Clip.hpp"   // s080
#include "ClipDialog.hpp"
#include "MakePathDialog.hpp"   // s081

namespace klimt {

namespace P = core::paths;
namespace L = core::layers;

// The picked path layer -- "" when the picked layer isn't one, or is hidden
// (Scott: a path layer switched off is invisible, and the Pen leaves it be).
std::string Shell::path_layer() const {
    const auto id = m_layers.primary();
    const auto l = id.empty() ? std::nullopt : m_doc.get(id);
    return l && l->kind == core::Kind::Path && m_doc.shown(id) ? id : std::string{};
}

namespace {
bool hidden_path(const core::Document& d, const std::string& id) {
    const auto l = id.empty() ? std::nullopt : d.get(id);
    return l && l->kind == core::Kind::Path && !d.shown(id);
}
}  // namespace

void Shell::update_paths() {
    const auto id = path_layer();
    const auto p = id.empty() ? nullptr : P::of(m_doc, id);
    // A picked path layer shows whatever the tool; the Pen on another layer
    // shows nothing yet (its first click starts a new path layer).
    const bool hidden = hidden_path(m_doc, m_layers.primary());   // a hidden path layer: nothing shows
    const bool outline = id.empty() || !m_outlines_off.count(id);
    m_canvas.set_path(p ? *p : core::path::Path{}, outline && !hidden && (p != nullptr || m_rail.tool() == "pen"));
    m_properties.set_path_outline(outline);
    {   // s037: its Isolate strokes, while it's picked and shown
        const auto l = id.empty() ? std::nullopt : m_doc.get(id);
        m_canvas.set_hints(l && l->hints ? *l->hints : core::path::Hints{}, l.has_value());
    }
    // Forget the hidden outlines of layers that are gone.
    for (auto it = m_outlines_off.begin(); it != m_outlines_off.end();)
        it = m_doc.get(*it) ? std::next(it) : m_outlines_off.erase(it);
    m_layers.set_outlines_off(m_outlines_off);
}

void Shell::toggle_path_outline(const std::string& id) {
    if (!m_outlines_off.erase(id)) {
        m_outlines_off.insert(id);
        toast("Outline hidden -- the mask still works (its node icon, or Ctrl+Shift+H, shows it)");
    }
    update_paths();
}

void Shell::show_path_outline(bool on) {
    const auto id = path_layer();
    if (id.empty()) return;
    if (on == !m_outlines_off.count(id)) return;
    toggle_path_outline(id);
}

void Shell::on_path_edit(const core::path::Path& p, const std::string& label) {
    if (!path_layer().empty() && m_outlines_off.count(path_layer())) show_path_outline(true);   // the Pen never edits blind
    if (hidden_path(m_doc, m_layers.primary())) {   // the Pen on a hidden path layer: nothing changes
        toast("“" + m_doc.get(m_layers.primary())->name + "” is hidden -- turn its eye on to edit the path");
        update_paths();
        return;
    }
    const auto id = path_layer();
    if (!id.empty()) {
        if (!L::set_path_layer(m_journal, m_doc, id, p, label)) update_paths();   // refused: the canvas goes back to the document's
    } else {
        // The Pen on a layer that isn't a path: a new path layer above it, picked.
        const auto made = L::add_path_layer(m_journal, m_doc, p, m_layers.primary());
        if (made.empty()) { update_paths(); return; }
        m_layers.select({made}, false);
        on_selection();
    }
    if (auto lg = log::get(log::Area::Shell)) lg->info("path {}: {} ({} anchors)", id.empty() ? "new" : id, label, p.anchors());
}

void Shell::on_path_selection(core::select::Op op) {
    const auto id = path_layer();
    const auto p = id.empty() ? nullptr : P::of(m_doc, id);
    if (!p || p->empty()) { toast("No path picked -- pick a path layer, or draw one with the Pen (P)"); return; }
    if (P::to_selection(m_journal, m_doc, id, op)) toast(m_journal.undo_label(), true);
}

void Shell::on_path_make_work() {
    if (!m_doc.selection_ptr()) { toast("Make Path from Selection: select something first"); return; }
    make_path(core::paths::Source::Selection, {}, m_prefs.path_tolerance);
}

// s081: any source traced -- a job; ONE step when it lands, unless what it
// read changed meanwhile.
void Shell::make_path(core::paths::Source source, const std::string& id, double tolerance) {
    auto mask = P::source_mask(m_doc, source, id);
    const std::string label = P::source_label(source);
    if (!mask) { toast(label + ": nothing to trace there"); return; }
    if (m_path_job) m_jobs.cancel(m_path_job);
    auto shared = std::make_shared<const klimt::pixel::TileMap>(std::move(*mask));
    const auto before = m_journal.undo_label() + "#" + std::to_string(m_journal.position()) + "/" + std::to_string(m_journal.size());
    m_path_job = start_job(label, [this, shared, tolerance, label, before](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto p = P::traced(*shared, tolerance, [&t] { return t.cancelled(); });
        if (!p) return {};
        return [this, label, before, path = std::move(*p)] {
            m_path_job = 0;
            if (m_journal.undo_label() + "#" + std::to_string(m_journal.position()) + "/" + std::to_string(m_journal.size()) != before) {
                toast(label + ": the document changed while tracing -- try again");   // what it read is gone
                return;
            }
            if (path.empty()) { toast(label + ": nothing to trace"); return; }
            const auto made = L::add_path_layer(m_journal, m_doc, path, m_layers.primary(), label);
            if (made.empty()) return;
            m_layers.select({made}, false);
            on_selection();
            toast(label + " -- " + std::to_string(path.anchors()) + " anchors", true);
            if (auto lg = log::get(log::Area::Shell)) lg->info("make path: {} -> {} ({} anchors)", label, made, path.anchors());
        };
    });
}

// s081: Layer > Make Path from Layer Mask / from Transparency -- the picked layer.
void Shell::on_path_from_layer(core::paths::Source source) {
    const auto id = m_layers.primary();
    if (id.empty()) { toast("Pick a layer first"); return; }
    make_path(source, id, m_prefs.path_tolerance);
}

// s081: Select > Make Path… -- the source and the tolerance, with a readout.
void Shell::on_make_path_dialog() {
    if (!has_image()) return;
    using Src = core::paths::Source;
    std::vector<MakePathDialog::Choice> choices;
    if (m_doc.selection_ptr()) choices.push_back({Src::Selection, {}, "Selection"});
    const auto id = m_layers.primary();
    if (const auto l = id.empty() ? std::nullopt : m_doc.get(id)) {
        if (l->mask) choices.push_back({Src::LayerMask, id, "Layer Mask of \u201c" + l->name + "\u201d"});
        if (l->pixels && l->kind != core::Kind::Path) choices.push_back({Src::Transparency, id, "Transparency of \u201c" + l->name + "\u201d"});
    }
    for (const auto& cid : m_doc.channel_order()) choices.push_back({Src::Channel, cid, "Saved selection \u201c" + m_doc.channels().at(cid).name + "\u201d"});
    if (choices.empty()) { toast("Make Path: select something, or pick a layer with pixels or a mask"); return; }
    if (!m_make_path_dialog) {
        m_make_path_dialog = std::make_unique<MakePathDialog>(*this);
        m_make_path_dialog->signal_changed().connect([this](std::size_t i, double tol) {
            const auto& cs = m_make_path_dialog->choices();
            if (i >= cs.size()) return;
            auto mask = P::source_mask(m_doc, cs[i].source, cs[i].id);
            if (!mask) { m_make_path_dialog->set_count("nothing to trace"); return; }
            if (m_path_count_job) m_jobs.cancel(m_path_count_job);
            m_make_path_dialog->set_count("\u2026");
            auto shared = std::make_shared<const klimt::pixel::TileMap>(std::move(*mask));
            m_path_count_job = m_jobs.submit("Count anchors", [this, shared, tol](core::Jobs::Token& t) -> core::Jobs::Landing {
                auto p = P::traced(*shared, tol, [&t] { return t.cancelled(); });
                if (!p) return {};
                std::size_t subs = p->subs.size(), n = p->anchors();
                return [this, subs, n] {
                    m_path_count_job = 0;
                    if (!m_make_path_dialog) return;
                    m_make_path_dialog->set_count(n == 0 ? std::string("nothing to trace")
                                                  : std::to_string(n) + (n == 1 ? " anchor" : " anchors") +
                                                    (subs > 1 ? " in " + std::to_string(subs) + " subpaths" : std::string()));
                };
            });
        });
        m_make_path_dialog->signal_ok().connect([this](std::size_t i, double tol) {
            const auto& cs = m_make_path_dialog->choices();
            if (i >= cs.size()) return;
            m_prefs.path_tolerance = tol;   // remembered -- Make Path from Selection uses it too
            core::save_prefs(m_prefs_file, m_prefs);
            make_path(cs[i].source, cs[i].id, tol);
        });
    }
    m_make_path_dialog->ask(choices, 0, m_prefs.path_tolerance);
}

// s037: a stroke painted -- ONE step on the picked path layer; then, when
// Properties says so, Isolate again with it.
void Shell::on_path_stroke(const core::path::Stroke& st) {
    const auto id = path_layer();
    if (id.empty()) { m_properties.stop_strokes(); return; }
    if (m_doc.locked_all(id)) { toast("The path layer is locked"); return; }
    if (!L::add_path_stroke(m_journal, m_doc, id, st)) return;
    if (auto lg = log::get(log::Area::Shell)) lg->info("isolate stroke on {}: {} r {} ({} points)", id, st.figure ? "figure" : "background", st.radius, st.pts.size());
    if (m_properties.isolate_after_stroke()) on_path_isolate(id, m_properties.isolate_band(), m_properties.isolate_soft());
}

void Shell::on_path_isolate(const std::string& id, double band, double soft) {
    const auto p = P::of(m_doc, id);
    if (!p || p->empty()) { toast("Isolate: the path is empty"); return; }
    if (m_path_job) m_jobs.cancel(m_path_job);
    const auto expect = m_doc.selection_ptr();
    const core::Image im = m_doc.image();
    const auto layers = m_doc.snapshot();
    const auto mf = m_doc.mask_format();
    const auto lay = m_doc.get(id);
    const auto hints = lay ? lay->hints : nullptr;   // s037: the strokes, as they are now
    m_path_job = start_job("Isolate", [this, im, layers, mf, path = *p, band, soft, expect, hints](core::Jobs::Token& t) -> core::Jobs::Landing {
        const auto pic = P::picture_for_isolate(im, layers);
        if (!pic || t.cancelled()) return {};
        core::isolate::Options o;
        o.band = band;
        o.soft = soft;   // s036
        o.hints = hints.get();   // s037
        auto m = core::isolate::isolated(*pic, path, mf, o, [&t] { return t.cancelled(); }, [&t](double f) { t.progress(f); });
        if (!m && t.cancelled()) return {};
        if (!m) return [this] {   // s038 (52): say so, never silently nothing
            m_path_job = 0;
            toast("Isolate: no figure found -- the path may cut off too much of it; Refine to Edges, then the Refine brush (W) on what's missing");
        };
        return [this, expect, mask = std::move(*m)]() mutable {
            m_path_job = 0;
            if (core::select::set_if(m_journal, m_doc, expect, core::select::empty(mask) ? std::nullopt : std::optional<pixel::TileMap>(std::move(mask)),
                                     "Isolate (Path)"))
                toast("Isolate -- the figure is selected", true);
            else toast("Isolate: the selection changed meanwhile -- try again");
        };
    });
}

// s080: Select > Clipping Path... -- which path layer cuts the picture out
// where it's placed, and its flatness. ONE step (core::clip::set).
void Shell::on_clipping_path() {
    if (!has_image()) return;
    std::vector<ClipDialog::Choice> paths;
    for (const auto& id : m_doc.walk()) {
        const auto l = m_doc.get(id);
        if (l && l->kind == core::Kind::Path && l->path && !l->path->empty()) paths.push_back({id, l->name});
    }
    if (paths.empty()) {
        toast("Clipping Path: draw a path first (the Pen, P) -- or Make Path from Selection");
        return;
    }
    if (!m_clip_dialog) {
        m_clip_dialog = std::make_unique<ClipDialog>(*this);
        m_clip_dialog->signal_ok().connect([this](std::string id, double flatness) {
            if (core::clip::set(m_journal, m_doc, id, flatness)) {
                const auto l = id.empty() ? std::nullopt : m_doc.get(id);
                toast(l ? "\u201c" + l->name + "\u201d is the clipping path -- EPS, TIFF and PSD exports carry it" : std::string("No clipping path"));
            }
            if (auto lg = log::get(log::Area::Shell)) lg->info("clipping path: {} (flatness {})", id.empty() ? "none" : id, flatness);
        });
    }
    // The picked path layer is offered first when there's no clipping path yet.
    std::string current = core::clip::of(m_doc);
    if (current.empty()) current = path_layer();
    m_clip_dialog->ask(paths, current, m_doc.image().clip_flatness);
}

}  // namespace klimt
