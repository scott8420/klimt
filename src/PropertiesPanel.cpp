#include "PropertiesPanel.hpp"

#include <gtkmm/gesturedrag.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

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
      m_later("properties.later") {
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

    // Where live filters and styles will be edited -- no dialogs.
    m_later.set_text("Live filters and layer styles will be edited here too, on the layer, "
                     "with the canvas updating.");
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
    m_later.set_visible(!adj);
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
