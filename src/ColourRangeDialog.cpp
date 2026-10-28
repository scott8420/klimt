#include "ColourRangeDialog.hpp"
#include "Registry.hpp"

#include <gtkmm/box.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/grid.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>

namespace klimt {

ColourRangeDialog::ColourRangeDialog(Gtk::Window& parent)
    : m_fuzz("range.fuzziness"), m_fuzz_n("range.fuzziness_n"), m_op("range.op"), m_busy("range.busy"),
      m_add("range.add", "Add"), m_clear("range.clear", "Clear"), m_ok_button("range.ok", "OK"),
      m_cancel_button("range.cancel", "Cancel") {
    set_name("range");
    registry::add("range", this);
    set_title("Color Range");
    set_modal(false);   // the canvas stays live: the Eyedropper picks
    set_resizable(false);
    set_transient_for(parent);
    set_hide_on_close(true);
    auto* hb = Gtk::make_managed<Gtk::HeaderBar>();
    hb->set_show_title_buttons(false);
    m_ok_button.add_css_class("suggested-action");
    hb->pack_start(m_cancel_button);
    hb->pack_end(m_ok_button);
    set_titlebar(*hb);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);
    auto* hint = Gtk::make_managed<Gtk::Label>("Pick colours with the Eyedropper (I) on the canvas.\n"
                                                "Add keeps this colour and picks another.");
    hint->set_xalign(0.f);
    hint->add_css_class("dim-label");
    body->append(*hint);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(10);
    grid->set_column_spacing(10);
    const auto label = [&](const char* t, int row) {
        auto* l = Gtk::make_managed<Gtk::Label>(t);
        l->set_xalign(0.f);
        l->add_css_class("dim-label");
        grid->attach(*l, 0, row);
    };
    label("Colours", 0);
    m_swatches.set_content_height(26);
    m_swatches.set_content_width(200);
    m_swatches.set_hexpand(true);
    m_swatches.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
        std::vector<std::array<float, 3>> all = m_kept;
        all.push_back(m_live);
        const double sw = std::min(30.0, static_cast<double>(w) / static_cast<double>(all.size()));
        for (std::size_t i = 0; i < all.size(); ++i) {
            cr->set_source_rgb(all[i][0], all[i][1], all[i][2]);
            cr->rectangle(i * sw + 1, 1, sw - 3, h - 2);
            cr->fill_preserve();
            cr->set_source_rgba(0.5, 0.5, 0.5, 0.8);
            cr->set_line_width(i + 1 == all.size() ? 2.0 : 1.0);   // the live one: framed
            cr->stroke();
        }
    });
    grid->attach(m_swatches, 1, 0, 2, 1);
    auto* btns = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    m_add.set_tooltip_text("Keep this colour in the range, and pick another with the Eyedropper");
    m_clear.set_tooltip_text("Only the colour being picked");
    btns->append(m_add);
    btns->append(m_clear);
    grid->attach(*btns, 1, 1, 2, 1);

    label("Fuzziness", 2);
    m_fuzz.set_range(0, 200);
    m_fuzz.set_increments(1, 10);
    m_fuzz.set_draw_value(false);
    m_fuzz.set_hexpand(true);
    m_fuzz.set_size_request(200, -1);
    m_fuzz.set_value(40);
    grid->attach(m_fuzz, 1, 2);
    m_fuzz_n.set_range(0, 200);
    m_fuzz_n.set_increments(1, 10);
    m_fuzz_n.set_digits(0);
    m_fuzz_n.set_width_chars(4);
    m_fuzz_n.set_value(40);
    grid->attach(m_fuzz_n, 2, 2);

    label("Selection", 3);
    m_op.set_model(Gtk::StringList::create({"New", "Add", "Subtract", "Intersect"}));
    grid->attach(m_op, 1, 3, 2, 1);
    body->append(*grid);

    auto* foot = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
    foot->append(m_invert);
    m_preview.set_active(true);
    foot->append(m_preview);
    m_busy.set_hexpand(true);
    m_busy.set_xalign(1.f);
    m_busy.add_css_class("dim-label");
    foot->append(m_busy);
    body->append(*foot);
    set_child(*body);

    m_fuzz.signal_value_changed().connect([this] {
        if (m_syncing) return;
        m_syncing = true; m_fuzz_n.set_value(m_fuzz.get_value()); m_syncing = false;
        m_changed.emit();
    });
    m_fuzz_n.signal_value_changed().connect([this] {
        if (m_syncing) return;
        m_syncing = true; m_fuzz.set_value(m_fuzz_n.get_value()); m_syncing = false;
        m_changed.emit();
    });
    m_op.property_selected().signal_changed().connect([this] { m_changed.emit(); });
    m_invert.signal_toggled().connect([this] { m_changed.emit(); });
    m_preview.signal_toggled().connect([this] { m_changed.emit(); });
    m_add.signal_clicked().connect([this] {
        m_kept.push_back(m_live);
        m_swatches.queue_draw();
        m_changed.emit();
    });
    m_clear.signal_clicked().connect([this] {
        m_kept.clear();
        m_swatches.queue_draw();
        m_changed.emit();
    });
    m_cancel_button.signal_clicked().connect([this] { close(); });
    m_ok_button.signal_clicked().connect([this] {
        m_fuzz_n.update();
        m_ok.emit();
        close();
    });
    signal_hide().connect([this] { m_closed.emit(); });
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect([this](guint k, guint, Gdk::ModifierType) {
        if (k == GDK_KEY_Escape) { close(); return true; }
        if (k == GDK_KEY_Return || k == GDK_KEY_KP_Enter) { m_ok_button.activate(); return true; }
        return false;
    }, false);
    add_controller(keys);
}

ColourRangeDialog::~ColourRangeDialog() { registry::remove(this); }

void ColourRangeDialog::ask(const std::array<float, 3>& fg) {
    m_kept.clear();
    m_live = fg;
    m_swatches.queue_draw();
    set_busy(false);
    present();
}

void ColourRangeDialog::follow(const std::array<float, 3>& fg) {
    if (fg == m_live) return;
    m_live = fg;
    m_swatches.queue_draw();
    m_changed.emit();
}

core::select::ColourRange ColourRangeDialog::value() const {
    core::select::ColourRange r;
    r.samples = m_kept;
    r.samples.push_back(m_live);
    r.fuzziness = static_cast<int>(m_fuzz_n.get_value());
    r.invert = m_invert.get_active();
    return r;
}

core::select::Op ColourRangeDialog::op() const {
    switch (m_op.get_selected()) {
        case 1: return core::select::Op::Add;
        case 2: return core::select::Op::Subtract;
        case 3: return core::select::Op::Intersect;
        default: return core::select::Op::Replace;
    }
}

void ColourRangeDialog::set_busy(bool b) { m_busy.set_text(b ? "Previewing…" : ""); }

}  // namespace klimt
