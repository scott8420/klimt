#include "PropertiesPanel.hpp"
#include "Log.hpp"
#include "core/Live.hpp"   // s018
#include "core/Paths.hpp"  // s035: what a path masks
#include <glibmm/main.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/gestureclick.h>
#include <cstdio>
#include <functional>

#include <gtkmm/gesturedrag.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/scale.h>
#include <gtkmm/dropdown.h>
#include <cmath>

namespace klimt {

namespace L = core::layers;

PropertiesPanel::PropertiesPanel(core::Document& doc, core::DocJournal& journal)
    : widgets::Box("properties", Gtk::Orientation::VERTICAL, 10),
      m_doc(doc),
      m_journal(journal),
      m_title("properties.title"),
      m_subtitle("properties.subtitle"),
      m_fields("properties.fields", Gtk::Orientation::VERTICAL, 10),
      m_blend("properties.blend"),
      m_opacity("properties.opacity", Gtk::Orientation::HORIZONTAL),
      m_fill("properties.fill", Gtk::Orientation::HORIZONTAL),
      m_lock_alpha("properties.lock.alpha"),
      m_lock_pixels("properties.lock.pixels"),
      m_lock_position("properties.lock.position"),
      m_lock_all("properties.lock.all"),
      m_later("properties.later"),
      m_live("properties.live", Gtk::Orientation::VERTICAL, 4),
      m_live_rows("properties.live.rows", Gtk::Orientation::VERTICAL, 2) {
    set_margin(12);

    auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto h = Gtk::make_managed<Gtk::Label>();
    h->set_markup("<b>Properties</b>");
    head->append(*h);
    m_title.set_ellipsize(Pango::EllipsizeMode::END);
    m_title.set_xalign(0.0f);
    m_title.set_hexpand(true);
    head->append(m_title);
    m_subtitle.add_css_class("dim-label");
    head->append(m_subtitle);
    append(*head);

    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(8);
    grid->set_column_spacing(10);
    const auto label = [&](const char* text, int row) {
        auto l = Gtk::make_managed<Gtk::Label>(text);
        l->set_xalign(0.0f);
        l->add_css_class("dim-label");
        grid->attach(*l, 0, row);
    };

    std::vector<Glib::ustring> labels;
    for (const auto& b : core::blend_modes()) labels.emplace_back(b.label);
    m_blend.set_model(Gtk::StringList::create(labels));
    m_blend.set_hexpand(true);
    m_blend.property_selected().signal_changed().connect(sigc::mem_fun(*this, &PropertiesPanel::on_blend));
    label("Blend", 0);
    grid->attach(m_blend, 1, 0);

    // Opacity: a slider; a drag is ONE step (the gesture opens at press, in
    // CAPTURE so it sees the press before the slider claims it; a claimed
    // sequence arrives as cancel -- also the end). Arrow presses merge.
    m_opacity.set_range(0, 100);
    m_opacity.set_increments(1, 10);
    m_opacity.set_digits(0);
    m_opacity.set_round_digits(0);
    m_opacity.set_draw_value(true);
    m_opacity.set_value_pos(Gtk::PositionType::RIGHT);
    m_opacity.set_hexpand(true);
    m_opacity.signal_value_changed().connect(sigc::mem_fun(*this, &PropertiesPanel::on_opacity));
    auto drag = Gtk::GestureDrag::create();
    drag->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    drag->signal_drag_begin().connect([this](double, double) {
        if (!m_sel.empty()) L::opacity_begin(m_journal, m_doc, m_sel);
    });
    const auto end = [this]() { L::opacity_end(m_journal); };
    drag->signal_drag_end().connect([end](double, double) { end(); });
    drag->signal_cancel().connect([end](const Gdk::EventSequence*) { end(); });
    m_opacity.add_controller(drag);
    label("Opacity", 1);
    grid->attach(m_opacity, 1, 1);

    m_fill.set_range(0, 100);
    m_fill.set_increments(1, 10);
    m_fill.set_digits(0);
    m_fill.set_round_digits(0);
    m_fill.set_draw_value(true);
    m_fill.set_value_pos(Gtk::PositionType::RIGHT);
    m_fill.set_hexpand(true);
    m_fill.set_tooltip_text("Fill -- the content only, not styles");
    m_fill.signal_value_changed().connect(sigc::mem_fun(*this, &PropertiesPanel::on_fill));
    label("Fill", 2);
    grid->attach(m_fill, 1, 2);

    auto locks = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    locks->add_css_class("linked");
    const auto lockb = [&](widgets::ToggleButton& t, const char* text, const char* tip) {
        t.set_label(text);
        t.set_tooltip_text(tip);
        t.set_focus_on_click(false);
        t.signal_toggled().connect(sigc::mem_fun(*this, &PropertiesPanel::on_lock));
        locks->append(t);
    };
    lockb(m_lock_alpha, "Alpha", "Lock transparent pixels");
    lockb(m_lock_pixels, "Pixels", "Lock image pixels");
    lockb(m_lock_position, "Move", "Lock position");
    lockb(m_lock_all, "All", "Lock all -- opacity, fill, blend and clip refuse");
    label("Lock", 3);
    grid->attach(*locks, 1, 3);
    m_fields.append(*grid);

    // s013: an adjustment layer's controls -- live, no dialog. A drag on a
    // slider or the graph is ONE step; presses fold.
    m_adjust.set_visible(false);
    m_adjust.signal_begin().connect([this] {
        if (!m_adjust_id.empty()) L::adjust_begin(m_journal, m_doc, m_adjust_id);
    });
    m_adjust.signal_changed().connect([this](const core::Adjust& a) {
        if (m_painting || m_adjust_id.empty()) return;
        if (m_journal.in_gesture()) L::adjust_to(m_journal, m_doc, m_adjust_id, a);
        else L::set_adjust(m_journal, m_doc, m_adjust_id, a);
    });
    m_adjust.signal_end().connect([this] { L::adjust_end(m_journal); });
    m_fields.append(m_adjust);
    // s030: a filter layer's filter.
    m_filter.set_visible(false);
    m_filter.signal_begin().connect([this] { if (!m_filter_id.empty()) L::filter_layer_begin(m_journal, m_doc, m_filter_id); });
    m_filter.signal_changed().connect([this](const core::Filter& f) {
        if (m_painting || m_filter_id.empty()) return;
        if (m_journal.in_gesture()) L::filter_layer_to(m_journal, m_doc, m_filter_id, f);
        else L::set_filter_layer(m_journal, m_doc, m_filter_id, f);
    });
    m_filter.signal_end().connect([this] { L::filter_layer_end(m_journal); });
    m_fields.append(m_filter);
    // s035: a path layer.
    {
        m_path_box.set_name("properties.path");
        m_path_box.set_visible(false);
        auto head = Gtk::make_managed<Gtk::Label>();
        head->set_markup("<b>Path</b>");
        head->set_xalign(0.0f);
        m_path_box.append(*head);
        m_path_info.set_xalign(0.0f);
        m_path_info.add_css_class("dim-label");
        m_path_box.append(m_path_info);
        m_path_mask.set_name("properties.path.mask");
        m_path_mask.set_tooltip_text("Everything below it in its group shows only inside the path -- live: edit the path, the mask follows");
        m_path_select.set_name("properties.path.select");
        m_path_select.set_tooltip_text("The selection is the path's area, and follows every edit of it");
        m_path_box.append(m_path_mask);
        m_path_target.set_xalign(0.0f);
        m_path_target.set_margin_start(28);
        m_path_target.add_css_class("dim-label");
        m_path_target.add_css_class("caption");
        m_path_target.set_wrap(true);
        m_path_box.append(m_path_target);
        m_path_box.append(m_path_select);
        m_path_show.set_name("properties.path.show");
        m_path_show.set_active(true);
        m_path_show.set_tooltip_text("Hide the outline and its points to see the effect alone -- the mask still works  "
                                     "(Ctrl+Shift+H; the Pen shows it again)");
        m_path_show.signal_toggled().connect([this] { if (!m_painting) m_path_outline.emit(m_path_show.get_active()); });
        m_path_box.append(m_path_show);
        const auto switched = [this] {
            if (m_painting || m_path_id.empty()) return;
            L::set_path_switches(m_journal, m_doc, m_path_id, m_path_mask.get_active(), m_path_select.get_active());
            if (auto lg = log::get(log::Area::Shell)) {   // s035: what it masks, for when the eyes and the trace differ
                const auto t = core::paths::masked_by(m_doc.snapshot(), m_path_id);
                std::string ids;
                for (const auto& x : t) ids += (ids.empty() ? "" : " ") + x;
                lg->info("path {}: mask {} (masks {}), selection {}", m_path_id, m_path_mask.get_active(), t.empty() ? "nothing" : ids,
                         m_path_select.get_active());
            }
        };
        m_path_mask.signal_toggled().connect(switched);
        m_path_select.signal_toggled().connect(switched);
        auto load = Gtk::make_managed<Gtk::Button>("Load as Selection");
        load->set_name("properties.path.load");
        load->set_tooltip_text("The path's area as the selection, once (Ctrl+Enter)");
        load->signal_clicked().connect([this] { if (!m_path_id.empty()) m_path_load.emit(m_path_id); });
        m_path_box.append(*load);
        // Isolate: the path as a hint -- the figure it goes round.
        auto iso_head = Gtk::make_managed<Gtk::Label>();
        iso_head->set_markup("<b>Isolate</b>");
        iso_head->set_xalign(0.0f);
        iso_head->set_margin_top(6);
        m_path_box.append(*iso_head);
        auto note = Gtk::make_managed<Gtk::Label>("Look here: the figure the path goes round, found in the picture -- "
                                                  "its real edge, cleaner than the path. Then the Refine brush (W) puts right "
                                                  "what it got wrong: paint what belongs, Alt-paint what doesn't.");
        note->set_wrap(true);
        note->set_xalign(0.0f);
        note->add_css_class("dim-label");
        note->add_css_class("caption");
        m_path_box.append(*note);
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto bl = Gtk::make_managed<Gtk::Label>("Edge within");
        bl->set_xalign(0.0f);
        bl->set_hexpand(true);
        row->append(*bl);
        m_band.set_name("properties.path.band");
        m_band.set_range(2, 64);
        m_band.set_increments(1, 4);
        m_band.set_digits(0);
        m_band.set_value(12);
        m_band.set_width_chars(4);
        m_band.set_tooltip_text("How far your path may cut INTO the figure, in pixels (outside, it may be as loose as you like)");
        row->append(m_band);
        auto px = Gtk::make_managed<Gtk::Label>("px");
        px->add_css_class("dim-label");
        row->append(*px);
        m_path_box.append(*row);
        // s036: the soft edge -- hair, fur: coverage from colour, this far either side of the cut.
        auto srow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto sl = Gtk::make_managed<Gtk::Label>("Soft edge");
        sl->set_xalign(0.0f);
        sl->set_hexpand(true);
        srow->append(*sl);
        m_soft.set_name("properties.path.soft");
        m_soft.set_range(0, 64);
        m_soft.set_increments(1, 4);
        m_soft.set_digits(0);
        m_soft.set_value(0);
        m_soft.set_width_chars(4);
        m_soft.set_tooltip_text("For hair, fur, soft focus: within this many pixels of the edge, how much is figure is read "
                                "from each pixel's colour (between the figure's and the background's) -- 0: a clean, hard edge");
        srow->append(m_soft);
        auto spx = Gtk::make_managed<Gtk::Label>("px");
        spx->add_css_class("dim-label");
        srow->append(*spx);
        m_path_box.append(*srow);
        auto iso = Gtk::make_managed<Gtk::Button>("Isolate the Figure");
        iso->set_name("properties.path.isolate");
        iso->add_css_class("suggested-action");
        iso->set_tooltip_text("Select what the path goes round, by its colours and edges -- outside the band is background, "
                              "inside it the figure, the band decided from the picture");
        iso->signal_clicked().connect([this] { if (!m_path_id.empty()) m_path_isolate.emit(m_path_id, m_band.get_value(), m_soft.get_value()); });
        m_path_box.append(*iso);
        // s037: STROKES -- paint "this is figure" / "this is background" on the
        // canvas; Isolate takes them as sure. s038 (Scott: "confusing ... could
        // it like pixelmator"): the toggles are OUT -- the Refine brush (W) in
        // the rail paints them now, on the selection. Only the count and Clear
        // stay, shown while a layer still holds strokes from s037.
        auto st_head = Gtk::make_managed<Gtk::Label>();
        st_head->set_markup("<b>Strokes</b>");
        st_head->set_xalign(0.0f);
        st_head->set_margin_top(4);
        auto st_note = Gtk::make_managed<Gtk::Label>("Where Isolate got it wrong, paint on the picture: Figure for what belongs, "
                                                     "Background for what doesn't. Alt paints the other kind; Esc stops.");
        st_note->set_wrap(true);
        st_note->set_xalign(0.0f);
        st_note->add_css_class("dim-label");
        st_note->add_css_class("caption");
        auto st_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
        st_row->add_css_class("linked");
        m_stroke_fig.set_name("properties.path.stroke_figure");
        m_stroke_fig.set_tooltip_text("Paint what IS the figure -- Isolate keeps it (Alt while painting: Background)");
        m_stroke_fig.set_hexpand(true);
        m_stroke_bg.set_name("properties.path.stroke_background");
        m_stroke_bg.set_tooltip_text("Paint what is NOT the figure -- Isolate leaves it out (Alt while painting: Figure)");
        m_stroke_bg.set_hexpand(true);
        st_row->append(m_stroke_fig);
        st_row->append(m_stroke_bg);
        m_stroke_fig.signal_toggled().connect([this] { stroke_toggled(true); });
        m_stroke_bg.signal_toggled().connect([this] { stroke_toggled(false); });
        auto zrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto zl = Gtk::make_managed<Gtk::Label>("Stroke size");
        zl->set_xalign(0.0f);
        zl->set_hexpand(true);
        zrow->append(*zl);
        m_stroke_size.set_name("properties.path.stroke_size");
        m_stroke_size.set_range(1, 200);
        m_stroke_size.set_increments(1, 10);
        m_stroke_size.set_digits(0);
        m_stroke_size.set_value(16);
        m_stroke_size.set_width_chars(4);
        m_stroke_size.set_tooltip_text("The stroke's width, in picture pixels");
        m_stroke_size.signal_value_changed().connect([this] {
            const int mode = m_stroke_fig.get_active() ? 1 : m_stroke_bg.get_active() ? 2 : 0;
            if (mode) m_stroke_mode.emit(mode, m_stroke_size.get_value() / 2);
        });
        zrow->append(m_stroke_size);
        auto zpx = Gtk::make_managed<Gtk::Label>("px");
        zpx->add_css_class("dim-label");
        zrow->append(*zpx);
        m_stroke_auto.set_name("properties.path.stroke_auto");
        m_stroke_auto.set_active(true);
        m_stroke_auto.set_tooltip_text("Run Isolate again as each stroke ends, with the edge settings above");
        auto crow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        m_stroke_info.set_xalign(0.0f);
        m_stroke_info.set_hexpand(true);
        m_stroke_info.add_css_class("dim-label");
        crow->append(m_stroke_info);
        m_stroke_clear.set_name("properties.path.stroke_clear");
        m_stroke_clear.set_tooltip_text("Take every stroke off this path layer (one step)");
        m_stroke_clear.signal_clicked().connect([this] { if (!m_path_id.empty()) m_strokes_clear.emit(m_path_id); });
        crow->append(m_stroke_clear);
        m_path_box.append(*crow);
        m_stroke_row = crow;   // s038: only while the layer still holds s037's strokes
        m_fields.append(m_path_box);
    }
    build_edges();   // s036

    // s019: the layer's styles -- live; a slider drag is ONE step, presses fold.
    m_styles.set_visible(false);
    m_styles.signal_begin().connect([this] { if (!primary().empty()) L::styles_begin(m_journal, m_doc, primary()); });
    m_styles.signal_changed().connect([this](const core::Styles& s) {
        if (m_painting || primary().empty()) return;
        if (m_journal.in_gesture()) L::styles_to(m_journal, m_doc, primary(), s);
        else L::set_styles(m_journal, m_doc, primary(), s);
    });
    m_styles.signal_end().connect([this] { L::styles_end(m_journal); });
    m_fields.append(m_styles);

    // s018: the live filters under a pixel layer.
    {
        auto head = Gtk::make_managed<Gtk::Label>();
        head->set_markup("<b>Live filters</b>");
        head->set_xalign(0.0f);
        m_live.append(*head);
    }
    m_live.append(m_live_rows);
    m_live.set_visible(false);
    m_fields.append(m_live);

    // Where layer styles will be edited -- no dialogs.
    m_later.set_text("Live filters: tick Live in a Filter's dialog -- they show here, editable later.");
    m_later.set_wrap(true);
    m_later.set_xalign(0.0f);
    m_later.add_css_class("dim-label");
    m_later.add_css_class("caption");
    m_fields.append(m_later);
    append(m_fields);

    paint();
}

void PropertiesPanel::show(const std::vector<Id>& selection) {
    m_sel = selection;
    paint();
}

// s037: one of the stroke toggles moved -- the other lets go; the mode told.
void PropertiesPanel::stroke_toggled(bool figure) {
    if (m_stroke_guard) return;
    m_stroke_guard = true;
    auto& me = figure ? m_stroke_fig : m_stroke_bg;
    auto& other = figure ? m_stroke_bg : m_stroke_fig;
    if (me.get_active()) other.set_active(false);
    m_stroke_guard = false;
    const int mode = m_stroke_fig.get_active() ? 1 : m_stroke_bg.get_active() ? 2 : 0;
    m_stroke_mode.emit(mode, m_stroke_size.get_value() / 2);
}

void PropertiesPanel::stop_strokes() {
    if (!m_stroke_fig.get_active() && !m_stroke_bg.get_active()) return;
    m_stroke_guard = true;
    m_stroke_fig.set_active(false);
    m_stroke_bg.set_active(false);
    m_stroke_guard = false;
    m_stroke_mode.emit(0, m_stroke_size.get_value() / 2);
}

void PropertiesPanel::set_path_outline(bool shown) {
    const bool was = m_painting;
    m_painting = true;
    m_path_show.set_active(shown);
    m_painting = was;
}

void PropertiesPanel::paint() {
    m_painting = true;
    const auto l = primary().empty() ? std::nullopt : m_doc.get(primary());
    m_fields.set_visible(l.has_value());
    if (!l) {
        m_title.set_text("");
        m_subtitle.set_text("nothing selected");
        m_painting = false;
        return;
    }
    m_title.set_text(l->name.empty() ? "Untitled" : l->name);
    std::string sub = l->kind == core::Kind::Group ? "group of " + std::to_string(m_doc.panel(primary()).size())
                    : l->kind == core::Kind::Adjustment ? std::string("adjustment layer")
                    : l->kind == core::Kind::Filter ? std::string("filter layer")   // s030
                    : l->kind == core::Kind::Path ? std::string("path layer")       // s035
                    : std::string("pixel layer");
    if (m_sel.size() > 1) sub += " · +" + std::to_string(m_sel.size() - 1) + " more";
    m_subtitle.set_text(sub);

    // Editable if ANY selected layer is (the locked ones are skipped).
    bool editable = false;
    for (const auto& id : m_sel) editable = editable || !m_doc.locked_all(id);
    m_blend.set_sensitive(editable);
    m_opacity.set_sensitive(editable);
    m_fill.set_sensitive(editable);
    const auto& modes = core::blend_modes();
    for (std::size_t i = 0; i < modes.size(); ++i)
        if (modes[i].mode == l->blend) m_blend.set_selected(static_cast<guint>(i));
    if (!m_journal.in_gesture()) m_opacity.set_value(l->opacity);
    m_fill.set_value(l->fill);
    m_lock_alpha.set_active(l->locks & core::lock::Alpha);
    m_lock_pixels.set_active(l->locks & core::lock::Pixels);
    m_lock_position.set_active(l->locks & core::lock::Position);
    m_lock_all.set_active(l->locks & core::lock::All);
    // s013: one adjustment layer picked: its controls.
    const bool adj = l->kind == core::Kind::Adjustment && m_sel.size() == 1;
    m_adjust.set_visible(adj);
    // s030: one filter layer picked: its filter.
    const bool flt = l->kind == core::Kind::Filter && m_sel.size() == 1 && l->filters.size() == 1;
    m_filter.set_visible(flt);
    m_filter_id = flt ? primary() : Id{};
    if (flt) {
        m_filter.set_sensitive(!m_doc.locked_all(primary()));
        if (!m_journal.in_gesture()) m_filter.set(l->filters[0].f);
    }
    // s035: one path layer picked: its switches and actions.
    const bool pth = l->kind == core::Kind::Path && m_sel.size() == 1 && l->path;
    m_path_box.set_visible(pth);
    if (m_path_id != (pth ? primary() : Id{})) Glib::signal_idle().connect_once([this] { stop_strokes(); });   // s037: another layer -- painting strokes stops
    m_path_id = pth ? primary() : Id{};
    if (pth) {
        m_path_box.set_sensitive(!m_doc.locked_all(primary()));
        m_path_mask.set_active(l->path_mask);
        {   // what the Mask switch masks (Scott: "a path in a group does not allow the mask to present")
            const auto layers = m_doc.snapshot();
            const auto t = core::paths::masked_by(layers, primary());
            const auto group = l->parent.empty() ? std::string("the document") : "“" + layers.at(l->parent).name + "”";
            std::string what = t.size() == 1 ? "“" + layers.at(t[0]).name + "”"
                             : "everything below it in " + group + " (" + std::to_string(t.size()) + " layers)";
            m_path_target.set_text(t.empty() ? "Masks nothing -- there is no layer below it in " + group
                                             : "Masks " + what + (l->path_mask ? "" : " (when on)"));
        }
        m_path_select.set_active(l->path_select);
        {   // s037: the strokes it holds
            std::size_t nf = 0, nb = 0;
            if (l->hints) for (const auto& st : l->hints->strokes) (st.figure ? nf : nb) += 1;
            m_stroke_info.set_text(nf + nb == 0 ? "No strokes" : "Strokes: " + std::to_string(nf) + " figure, " + std::to_string(nb) + " background");
            if (m_stroke_row) m_stroke_row->set_visible(nf + nb > 0);
            m_stroke_clear.set_sensitive(nf + nb > 0);
        }
        std::size_t open = 0;
        for (const auto& s : l->path->subs) open += s.closed ? 0 : 1;
        m_path_info.set_text(std::to_string(l->path->anchors()) + " anchors, " + std::to_string(l->path->subs.size()) + " subpath" +
                             (l->path->subs.size() == 1 ? "" : "s") + (open ? " (" + std::to_string(open) + " open -- they close for the mask)" : ""));
    }
    m_blend.set_sensitive(editable && l->kind != core::Kind::Path);   // a path layer draws nothing: no blend / opacity / fill
    m_opacity.set_sensitive(editable && l->kind != core::Kind::Path);
    m_fill.set_sensitive(editable && l->kind != core::Kind::Path);
    m_later.set_visible(!adj && !flt && !pth);
    paint_edges(*l);  // s036
    paint_live(*l);   // s018
    // s019: one pixel layer picked: its styles.
    const bool styled = (l->kind == core::Kind::Pixel || l->kind == core::Kind::Group) && m_sel.size() == 1;   // s024: groups too
    m_styles.set_visible(styled);
    if (styled) {
        m_styles.set_sensitive(!m_doc.locked_all(primary()));
        if (!m_journal.in_gesture()) m_styles.set(l->styles);
    }
    if (adj && primary() != m_adjust_id) m_adjust.set(core::Adjust{});   // another layer: fresh controls
    m_adjust_id = adj ? primary() : Id{};
    if (adj) {
        m_adjust.set_sensitive(!m_doc.locked_all(primary()));
        if (!m_journal.in_gesture()) m_adjust.set(l->adjust);   // mid-drag the editor is ahead of the document
        const bool wants = l->adjust.kind == core::AdjustKind::Levels || l->adjust.kind == core::AdjustKind::Curves;
        if (wants && m_hist_id != primary()) {
            m_hist_id = primary();
            m_adjust.set_histogram(std::nullopt);
            m_hist_wanted.emit(primary());
        }
    } else {
        m_hist_id.clear();
    }
    m_painting = false;
}

// s036: the MASK'S EDGES. A section under the layer's switches whenever the
// one picked layer has a mask (or is a path layer: its edges shape the path's
// mask). Each change is one call into core::layers::set_mask_edges; a drag's
// quick changes fold into one step there.
void PropertiesPanel::build_edges() {
    m_edges_box.set_name("properties.edges");
    m_edges_box.set_visible(false);
    m_edges_head.set_markup("<b>Mask edges</b>");
    m_edges_head.set_xalign(0.0f);
    m_edges_head.set_margin_top(4);
    m_edges_box.append(m_edges_head);
    m_edges_note.set_xalign(0.0f);
    m_edges_note.set_wrap(true);
    m_edges_note.add_css_class("dim-label");
    m_edges_note.add_css_class("caption");
    m_edges_box.append(m_edges_note);
    struct Spec { const char* name; const char* label; const char* unit; double hi; bool sq; int digits; const char* tip; };
    const Spec specs[3] = {
        {"properties.edges.density", "Density", "%", 100, false, 0,
         "How strongly the mask hides -- 100: fully; 0: not at all (the mask stays, to bring back)"},
        {"properties.edges.feather", "Feather", "px", 250, true, 1,
         "Soften the mask's edge -- live; the mask itself doesn't change"},
        {"properties.edges.defringe", "Defringe", "px", 64, true, 1,
         "Take the old background's colour out of the edge of a cut-out -- the halo; "
         "within this distance of the edge, colours come from the figure (the pixels don't change)"},
    };
    for (int i = 0; i < 3; ++i) {
        const Spec& sp = specs[i];
        auto top = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        top->set_margin_top(4);
        auto l = Gtk::make_managed<Gtk::Label>(sp.label);
        l->set_xalign(0.0f);
        l->set_hexpand(true);
        top->append(*l);
        auto n = Gtk::make_managed<Gtk::SpinButton>();
        n->set_name(std::string(sp.name) + ".value");
        n->set_range(0, sp.hi);
        n->set_increments(sp.digits ? 0.5 : 1, sp.digits ? 5 : 10);
        n->set_digits(static_cast<guint>(sp.digits));
        n->set_width_chars(5);
        n->set_numeric(true);
        n->set_tooltip_text(sp.tip);
        top->append(*n);
        auto u = Gtk::make_managed<Gtk::Label>(sp.unit);
        u->set_size_request(20, -1);
        u->set_xalign(0.0f);
        u->add_css_class("dim-label");
        top->append(*u);
        m_edges_box.append(*top);
        auto sc = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
        sc->set_name(std::string(sp.name) + ".slider");
        sc->set_range(0, 1000);   // a position: linear, or squared onto 0..hi
        sc->set_increments(1, 50);
        sc->set_draw_value(false);
        sc->set_hexpand(true);
        sc->set_tooltip_text(sp.tip);
        m_edges_box.append(*sc);
        m_er[i] = {sc, n, top, sp.hi, sp.sq};
        sc->signal_value_changed().connect([this, i] {
            if (m_edges_sync || m_painting) return;
            const EdgeRow& r = m_er[i];
            const double t = r.s->get_value() / 1000;
            const double v = r.hi * (r.sq ? t * t : t);
            const double q = r.n->get_digits() ? 10 : 1;
            m_edges_sync = true;
            r.n->set_value(std::round(v * q) / q);
            m_edges_sync = false;
            edges_changed(i);
        });
        n->signal_value_changed().connect([this, i] {
            if (m_edges_sync || m_painting) return;
            const EdgeRow& r = m_er[i];
            const double t = r.hi > 0 ? r.n->get_value() / r.hi : 0;
            m_edges_sync = true;
            r.s->set_value(1000 * (r.sq ? std::sqrt(t) : t));
            m_edges_sync = false;
            edges_changed(i);
        });
    }
    m_fields.append(m_edges_box);
}

void PropertiesPanel::edges_changed(int) {
    if (m_edges_id.empty()) return;
    core::MaskEdges e;
    e.density = m_er[0].n->get_value();
    e.feather = m_er[1].n->get_value();
    e.defringe = m_er[2].n->get_value();
    m_edges_shown = e;
    L::set_mask_edges(m_journal, m_doc, m_edges_id, e);
}

void PropertiesPanel::paint_edges(const core::Layer& l) {
    const bool path = l.kind == core::Kind::Path && l.path;
    const bool show = m_sel.size() == 1 && (l.mask || path);
    m_edges_box.set_visible(show);
    if (!show) { m_edges_id.clear(); return; }
    const bool pixel = l.kind == core::Kind::Pixel;
    m_er[2].top->set_visible(pixel);   // colours: a pixel layer's
    m_er[2].s->set_visible(pixel);
    m_edges_box.set_sensitive(!m_doc.locked_all(primary()));
    m_edges_note.set_text(path ? "The path's mask: its outline softened, its hold weakened."
                        : !l.mask_on ? "The mask is off (Shift-click its chip) -- these act when it's on."
                        : "Live -- the mask itself never changes; Apply Layer Mask bakes them.");
    // The same layer, the same values as shown: leave the widgets be (a
    // drag in progress must not be set back by its own echo).
    if (primary() == m_edges_id && l.edges == m_edges_shown) return;
    m_edges_id = primary();
    m_edges_shown = l.edges;
    const double v[3] = {l.edges.density, l.edges.feather, l.edges.defringe};
    m_edges_sync = true;
    for (int i = 0; i < 3; ++i) {
        const EdgeRow& r = m_er[i];
        r.n->set_value(v[i]);
        const double t = r.hi > 0 ? v[i] / r.hi : 0;
        r.s->set_value(1000 * (r.sq ? std::sqrt(t) : t));
    }
    m_edges_sync = false;
}

// s018: the stack, bottom (runs first) at the top of the list, as Photoshop
// lists smart filters under the layer. A row: on / off, its name and main
// value, up / down, delete; double-click the name to edit it. Rasterize below.
void PropertiesPanel::paint_live(const core::Layer& l) {
    namespace F = core::filter;
    const bool show = l.kind == core::Kind::Pixel && m_sel.size() == 1 && !l.filters.empty();
    m_live.set_visible(show);
    m_later.set_visible(m_later.get_visible() && !show);   // the stack says it better
    if (!m_fmask_target.empty() && (!show || primary() != m_fmask_target)) {   // m4: another layer -- let go
        m_fmask_target.clear();
        m_fmask_signal.emit(false);
        m_live_shown.clear();   // rebuild: the toggle shows off
    }
    if (!show) { m_live_shown.clear(); m_live_id.clear(); return; }
    // s023: only a filter's blending moved (its popover's slider, mid-drag):
    // keep the rows -- a rebuild would close the popover under the pointer.
    const auto same_but_blend = [](const std::vector<core::LiveFilter>& a, const std::vector<core::LiveFilter>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!(a[i].f == b[i].f) || a[i].on != b[i].on) return false;
        return true;
    };
    if (primary() == m_live_id && same_but_blend(l.filters, m_live_shown) && !(l.filters == m_live_shown) &&
        (l.filter_mask != nullptr) == m_live_had_mask) {
        m_live_shown = l.filters;
        for (const auto& f : m_blend_follow) f();
        return;
    }
    if (primary() == m_live_id && l.filters == m_live_shown && (l.filter_mask != nullptr) == m_live_had_mask) return;
    m_live_had_mask = l.filter_mask != nullptr;
    m_live_id = primary();
    m_live_shown = l.filters;
    m_blend_follow.clear();
    while (auto* c = m_live_rows.get_first_child()) m_live_rows.remove(*c);
    const bool editable = !m_doc.locked_all(primary()) && !(l.locks & core::lock::Pixels);
    for (std::size_t i = 0; i < l.filters.size(); ++i) {
        const auto& lf = l.filters[i];
        auto row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
        auto on = Gtk::make_managed<Gtk::CheckButton>();
        on->set_active(lf.on);
        on->set_tooltip_text("Show / hide this filter");
        on->set_sensitive(editable);
        on->signal_toggled().connect([this, i] { if (!m_painting) L::toggle_live(m_journal, m_doc, m_live_id, i); });
        row->append(*on);
        std::string what(F::label(lf.f.kind));
        char num[32];
        if (lf.f.kind == core::FilterKind::AddNoise) std::snprintf(num, sizeof num, "  %.1f %%", lf.f.amount);
        else if (lf.f.kind == core::FilterKind::MotionBlur) std::snprintf(num, sizeof num, "  %d°, %.0f px", lf.f.angle, lf.f.radius);
        else if (lf.f.kind == core::FilterKind::SmartSharpen) std::snprintf(num, sizeof num, "  %.0f %%, %.1f px", lf.f.amount, lf.f.radius);   // s034
        else if (lf.f.kind == core::FilterKind::ReduceNoise) std::snprintf(num, sizeof num, "  %.0f", lf.f.amount);
        else if (lf.f.kind == core::FilterKind::DustScratches || lf.f.kind == core::FilterKind::SurfaceBlur) std::snprintf(num, sizeof num, "  %.0f px, %d", lf.f.radius, lf.f.threshold);
        else if (lf.f.kind == core::FilterKind::Twirl) std::snprintf(num, sizeof num, "  %d°", lf.f.angle);   // s040 m4
        else if (lf.f.kind == core::FilterKind::Pinch || lf.f.kind == core::FilterKind::Spherize) std::snprintf(num, sizeof num, "  %.0f%%", lf.f.amount);
        else if (lf.f.kind == core::FilterKind::RadialBlur) std::snprintf(num, sizeof num, "  %.0f, %s", lf.f.amount, lf.f.zoom ? "zoom" : "spin");   // s040
        else if (lf.f.kind == core::FilterKind::LensBlur) std::snprintf(num, sizeof num, "  %.0f px, %.0f blades", lf.f.radius, lf.f.blades);   // s037
        else if (lf.f.kind == core::FilterKind::Maximum || lf.f.kind == core::FilterKind::Minimum)
            std::snprintf(num, sizeof num, "  %.0f px%s", lf.f.radius, lf.f.round ? ", round" : "");
        else std::snprintf(num, sizeof num, "  %.1f px", lf.f.radius);
        auto name = Gtk::make_managed<Gtk::Label>(what + num);
        name->set_xalign(0.0f);
        name->set_hexpand(true);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        name->set_tooltip_text("Double-click to change its settings");
        if (!lf.on) name->add_css_class("dim-label");
        auto click = Gtk::GestureClick::create();
        click->signal_pressed().connect([this, i](int n, double, double) { if (n == 2) m_live_edit.emit(m_live_id, i); });
        name->add_controller(click);
        row->append(*name);
        const auto button = [&](const char* icon, const char* tip, bool sensitive, std::function<void()> act) {
            auto b = Gtk::make_managed<Gtk::Button>();
            b->set_icon_name(icon);
            b->add_css_class("flat");
            b->set_tooltip_text(tip);
            b->set_sensitive(editable && sensitive);
            b->signal_clicked().connect(std::move(act));
            row->append(*b);
        };
        // s023: BLENDING OPTIONS -- the filter's opacity and mode, in a popover.
        {
            auto mb = Gtk::make_managed<Gtk::MenuButton>();
            mb->add_css_class("flat");
            const auto label_of = [](const core::LiveFilter& f) {
                std::string t = std::to_string(f.opacity) + "%";
                if (f.mode != core::Blend::Normal) t += " " + std::string(core::blend_label(f.mode));
                return t;
            };
            mb->set_label(label_of(lf));
            mb->set_tooltip_text("Blending options: this filter's opacity and blend mode over what goes into it");
            mb->set_sensitive(editable);
            auto pop = Gtk::make_managed<Gtk::Popover>();
            auto grid = Gtk::make_managed<Gtk::Grid>();
            grid->set_row_spacing(8);
            grid->set_column_spacing(8);
            grid->set_margin(10);
            auto ol = Gtk::make_managed<Gtk::Label>("Opacity");
            ol->add_css_class("dim-label");
            ol->set_xalign(0.f);
            grid->attach(*ol, 0, 0);
            auto sc = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
            sc->set_range(0, 100);
            sc->set_increments(1, 10);
            sc->set_digits(0);
            sc->set_draw_value(true);
            sc->set_value_pos(Gtk::PositionType::RIGHT);
            sc->set_size_request(200, -1);
            sc->set_value(lf.opacity);
            grid->attach(*sc, 1, 0);
            auto ml = Gtk::make_managed<Gtk::Label>("Mode");
            ml->add_css_class("dim-label");
            ml->set_xalign(0.f);
            grid->attach(*ml, 0, 1);
            std::vector<Glib::ustring> names;
            auto modes = std::make_shared<std::vector<core::Blend>>();
            guint at = 0;
            for (const auto& b : core::blend_modes()) {
                if (b.mode == core::Blend::PassThrough) continue;
                if (b.mode == lf.mode) at = static_cast<guint>(modes->size());
                modes->push_back(b.mode);
                names.emplace_back(b.label);
            }
            auto dd = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(names));
            dd->set_selected(at);
            grid->attach(*dd, 1, 1);
            const auto put = [this, i, sc, dd, modes] {
                if (m_painting) return;
                const auto k = dd->get_selected();
                L::set_live_blend(m_journal, m_doc, m_live_id, i, static_cast<int>(std::lround(sc->get_value())),
                                  k < modes->size() ? (*modes)[k] : core::Blend::Normal);
            };
            sc->signal_value_changed().connect(put);
            dd->property_selected().signal_changed().connect(put);
            // The button's label follows (the rows stay while only blending moves).
            m_blend_follow.push_back([this, i, mb, label_of] {
                if (i < m_live_shown.size()) mb->set_label(label_of(m_live_shown[i]));
            });
            pop->set_child(*grid);
            mb->set_popover(*pop);
            row->append(*mb);
        }
        button("go-up-symbolic", "Earlier in the stack (runs first)", i > 0, [this, i] { L::move_live(m_journal, m_doc, m_live_id, i, i - 1); });
        button("go-down-symbolic", "Later in the stack", i + 1 < l.filters.size(), [this, i] { L::move_live(m_journal, m_doc, m_live_id, i, i + 1); });
        button("user-trash-symbolic", "Delete this filter", true, [this, i] { L::remove_live(m_journal, m_doc, m_live_id, i); });
        m_live_rows.append(*row);
    }
    // m4: the filter mask -- paint it (the Brush, Eraser: black hides the
    // filters, white shows them), or delete it.
    auto mrow = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    auto paint_mask = Gtk::make_managed<Gtk::ToggleButton>(l.filter_mask ? "Paint Filter Mask" : "Add Filter Mask");
    paint_mask->set_tooltip_text("The Brush and Eraser paint where the filters show: black hides them, white shows them");
    paint_mask->set_active(m_fmask_target == primary());
    paint_mask->set_sensitive(editable);
    paint_mask->signal_toggled().connect([this, paint_mask] {
        if (m_painting) return;
        m_fmask_target = paint_mask->get_active() ? m_live_id : Id{};
        m_fmask_signal.emit(paint_mask->get_active());
    });
    mrow->append(*paint_mask);
    if (l.filter_mask) {
        auto del = Gtk::make_managed<Gtk::Button>("Delete Mask");
        del->add_css_class("flat");
        del->set_sensitive(editable);
        del->signal_clicked().connect([this] { L::set_filter_mask(m_journal, m_doc, m_live_id, nullptr, "Delete Filter Mask"); });
        mrow->append(*del);
    }
    m_live_rows.append(*mrow);
    auto bake = Gtk::make_managed<Gtk::Button>("Rasterize Filters");
    bake->set_tooltip_text("Into the pixels for good: the layer becomes what it looks like, the filters go");
    bake->set_sensitive(editable);
    bake->set_halign(Gtk::Align::START);
    bake->signal_clicked().connect([this] { L::bake_live(m_journal, m_doc, m_live_id); });
    m_live_rows.append(*bake);
}

void PropertiesPanel::set_histogram(const Id& id, const core::adjust::Histogram& h) {
    if (id == m_hist_id && id == m_adjust_id) m_adjust.set_histogram(h);
}

void PropertiesPanel::on_blend() {
    if (m_painting || primary().empty()) return;
    const auto i = m_blend.get_selected();
    const auto& modes = core::blend_modes();
    if (i >= modes.size()) return;
    // Refused (Pass Through on a layer, a lock): paint puts the menu back.
    if (!L::set_blend(m_journal, m_doc, m_sel, modes[i].mode)) paint();
}

void PropertiesPanel::on_opacity() {
    if (m_painting || primary().empty()) return;
    const int p = static_cast<int>(m_opacity.get_value() + 0.5);
    if (m_journal.in_gesture()) L::opacity_to(m_journal, m_doc, m_sel, p);
    else if (!L::set_opacity(m_journal, m_doc, m_sel, p)) paint();
}

void PropertiesPanel::on_fill() {
    if (m_painting || primary().empty()) return;
    if (!L::set_fill(m_journal, m_doc, m_sel, static_cast<int>(m_fill.get_value() + 0.5))) paint();
}

void PropertiesPanel::on_lock() {
    if (m_painting || primary().empty()) return;
    std::uint8_t m = 0;
    if (m_lock_alpha.get_active())    m |= core::lock::Alpha;
    if (m_lock_pixels.get_active())   m |= core::lock::Pixels;
    if (m_lock_position.get_active()) m |= core::lock::Position;
    if (m_lock_all.get_active())      m |= core::lock::All;
    L::set_locks(m_journal, m_doc, m_sel, m);
}

}  // namespace klimt
