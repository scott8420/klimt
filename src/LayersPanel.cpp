#include "LayersPanel.hpp"
#include "Log.hpp"

#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/gesturedrag.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/dragsource.h>
#include <gtkmm/droptarget.h>
#include <gtkmm/stylecontext.h>
#include <gtkmm/widgetpaintable.h>
#include <gdkmm/contentprovider.h>
#include <gdkmm/display.h>
#include <gtkmm/image.h>
#include <gtkmm/menubutton.h>
#include <giomm/menu.h>
#include <gtkmm/label.h>
#include <gtkmm/stringlist.h>
#include <glibmm/main.h>

#include <algorithm>

namespace klimt {

namespace L = core::layers;

namespace {
// "Layer 4" -- one more than the highest "<base> N" already in the document.
std::string next_name(const core::Document& d, const std::string& base) {
    int top = 0;
    for (const auto& id : d.walk()) {
        const std::string n = d.get(id)->name;   // by value: get() returns a temporary
        if (n.rfind(base + " ", 0) != 0) continue;
        try { top = std::max(top, std::stoi(n.substr(base.size() + 1))); } catch (...) {}
    }
    return base + " " + std::to_string(top + 1);
}

Gtk::Button* icon_button(const char* icon, const char* tip) {
    auto b = Gtk::make_managed<Gtk::Button>();
    b->set_icon_name(icon);
    b->set_has_frame(false);
    b->set_tooltip_text(tip);
    b->set_focus_on_click(false);
    return b;
}
}  // namespace

LayersPanel::LayersPanel(core::Document& doc, core::DocJournal& journal)
    : widgets::Box("layers.panel", Gtk::Orientation::VERTICAL, 6),
      m_doc(doc),
      m_journal(journal),
      m_scroll("layers.scroll"),
      m_list("layers.list") {

    // The drop line: above / below a row, or the row itself lit for "inside".
    static bool css_done = false;
    if (!css_done) {
        css_done = true;
        auto css = Gtk::CssProvider::create();
        css->load_from_data(
            "row.drop-above  { box-shadow: inset 0 3px 0 0 #3584e4; }\n"
            "row.drop-below  { box-shadow: inset 0 -3px 0 0 #3584e4; }\n"
            "row.drop-inside { box-shadow: inset 0 0 0 2px #3584e4; background-color: alpha(#3584e4, 0.18); }\n");
        if (auto display = Gdk::Display::get_default())
            Gtk::StyleContext::add_provider_for_display(display, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    // ── the rows ────────────────────────────────────────────────────────────
    m_list.set_selection_mode(Gtk::SelectionMode::MULTIPLE);
    m_list.set_activate_on_single_click(false);
    m_list.signal_selected_rows_changed().connect(sigc::mem_fun(*this, &LayersPanel::on_rows_selected));
    // Delete / F2 when the list has the keyboard. BUBBLE: a rename field in a
    // row takes its own keys first.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
        const auto mods = state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK |
                                   Gdk::ModifierType::ALT_MASK);
        if (mods != Gdk::ModifierType{}) return false;
        // Through the window's action, so Delete from the key, the button and
        // the menu all say the same (the Shell's toast offers Undo).
        if (keyval == GDK_KEY_Delete || keyval == GDK_KEY_BackSpace) {
            activate_action("win.layer-delete");
            return true;
        }
        if (keyval == GDK_KEY_F2) { rename_primary(); return true; }
        return false;
    }, false);
    m_list.add_controller(keys);
    // While a name is a field, Ctrl+A is the FIELD's (select its text). The
    // ListBox's own Ctrl+A select-all fired as well (seen under Xvfb, s003:
    // the text selected AND every row). CAPTURE on the list runs before both.
    auto rename_keys = Gtk::EventControllerKey::create();
    rename_keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    rename_keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
        if (m_renaming.empty() || !m_rename_field) return false;
        const auto mods = state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::SHIFT_MASK |
                                   Gdk::ModifierType::ALT_MASK);
        if ((keyval == GDK_KEY_a || keyval == GDK_KEY_A) && mods == Gdk::ModifierType::CONTROL_MASK) {
            m_rename_field->select_region(0, -1);
            return true;
        }
        return false;
    }, false);
    m_list.add_controller(rename_keys);
    // Dropped on the empty space under the rows: to the bottom of the document.
    auto tail = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);
    tail->signal_motion().connect([this](double, double) {
        const auto top = m_doc.panel({});
        if (m_dragging.empty() || top.empty() ||
            !core::layers::can_move(m_doc, m_dragging, top.back(), core::layers::Where::Below))
            return Gdk::DragAction{};
        return Gdk::DragAction::MOVE;
    }, false);
    tail->signal_drop().connect([this](const Glib::ValueBase&, double, double) {
        const auto top = m_doc.panel({});
        return !top.empty() && drop(top.back(), core::layers::Where::Below);
    }, false);
    m_list.add_controller(tail);
    m_scroll.set_child(m_list);
    m_scroll.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    m_scroll.set_vexpand(true);
    m_scroll.set_has_frame(true);
    append(m_scroll);

    // ── the verbs ───────────────────────────────────────────────────────────
    auto bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
    const auto verb = [&](const char* icon, const char* label, const char* action, const char* tip) {
        auto b = Gtk::make_managed<Gtk::Button>();
        if (icon) b->set_icon_name(icon);
        else      b->set_label(label);
        b->set_action_name(action);
        b->set_tooltip_text(tip);
        b->add_css_class("flat");
        bar->append(*b);
    };
    verb("list-add-symbolic", nullptr, "win.layer-new", "New layer (Ctrl+Shift+N)");
    verb("folder-new-symbolic", nullptr, "win.layer-new-group", "New group");
    verb("edit-copy-symbolic", nullptr, "win.layer-duplicate", "Duplicate (Ctrl+J)");
    // The rest behind one button (s005: less clutter; each keeps its key).
    auto more_model = Gio::Menu::create();
    more_model->append("Group Layers", "win.layer-group");
    more_model->append("Ungroup", "win.layer-ungroup");
    more_model->append("Clip / Release", "win.layer-clip");
    more_model->append("Rename", "win.layer-rename");
    more_model->append("Sort by Name", "win.layer-sort");
    auto more = Gtk::make_managed<Gtk::MenuButton>();
    more->set_icon_name("view-more-symbolic");
    more->set_menu_model(more_model);
    more->set_tooltip_text("Group, ungroup, clip, rename, sort");
    more->add_css_class("flat");
    bar->append(*more);
    auto sp = Gtk::make_managed<Gtk::Box>();
    sp->set_hexpand(true);
    bar->append(*sp);
    verb("go-up-symbolic", nullptr, "win.layer-raise", "Bring forward (Ctrl+])");
    verb("go-down-symbolic", nullptr, "win.layer-lower", "Send backward (Ctrl+[)");
    verb("user-trash-symbolic", nullptr, "win.layer-delete", "Delete (Delete)");
    append(*bar);
}

// ─────────────────────────────────────────────────────────────────────────────
// Selection and repaint
// ─────────────────────────────────────────────────────────────────────────────
std::vector<LayersPanel::Id> LayersPanel::in_walk_order(const std::vector<Id>& ids) const {
    std::vector<Id> out;
    for (const auto& id : m_doc.walk())
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) out.push_back(id);
    return out;
}

void LayersPanel::select(const std::vector<Id>& ids, bool keys) {
    m_sel = in_walk_order(ids);
    m_keys_to_list = m_keys_to_list || keys;
    // A selected layer inside a collapsed group opens the group (an undo that
    // brings back a layer must show it).
    for (const auto& id : m_sel)
        for (auto p = m_doc.get(id) ? m_doc.get(id)->parent : Id{}; !p.empty(); p = m_doc.get(p)->parent)
            m_collapsed.erase(p);
    queue_repaint();
}

void LayersPanel::queue_repaint() {
    if (m_repaint_queued) return;
    m_repaint_queued = true;
    Glib::signal_idle().connect_once([this]() {
        m_repaint_queued = false;
        repaint();
    });
}

void LayersPanel::repaint() {
    m_painting = true;
    // Drop what no longer exists (a redo removed it, a delete took it).
    std::erase_if(m_sel, [this](const Id& id) { return !m_doc.get(id); });
    std::erase_if(m_collapsed, [this](const Id& id) { return !m_doc.get(id); });

    while (auto* c = m_list.get_first_child()) m_list.remove(*c);
    Gtk::ListBoxRow* first_selected = nullptr;
    m_rename_field = nullptr;
    for (const auto& id : m_doc.walk()) {
        // Inside a collapsed group: not shown.
        bool hidden = false;
        for (auto p = m_doc.get(id)->parent; !p.empty(); p = m_doc.get(p)->parent)
            if (m_collapsed.count(p)) { hidden = true; break; }
        if (hidden) continue;
        auto* row = build_row(id);
        m_list.append(*row);
        if (std::find(m_sel.begin(), m_sel.end(), id) != m_sel.end()) {
            m_list.select_row(*row);
            if (!first_selected) first_selected = row;
        }
    }
    m_painting = false;

    if (m_rename_field) {
        m_rename_field->grab_focus();
        m_rename_field->select_region(0, -1);
    } else if (m_keys_to_list) {
        if (first_selected) first_selected->grab_focus();
        else                m_list.grab_focus();
    }
    m_keys_to_list = false;
    if (m_changed) m_changed();
}

Gtk::ListBoxRow* LayersPanel::build_row(const Id& id) {
    const core::Layer l = *m_doc.get(id);
    const bool group = l.kind == core::Kind::Group;
    auto row = Gtk::make_managed<Gtk::ListBoxRow>();
    row->set_name(id);   // the row carries its layer's id

    auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    box->set_margin_start(4);
    box->set_margin_end(6);
    box->set_margin_top(2);
    box->set_margin_bottom(2);

    // Eye. Hidden by a group above it: the eye is on, but dimmed.
    auto eye = icon_button(l.visible ? "view-reveal-symbolic" : "view-conceal-symbolic",
                           l.visible ? "Hide" : "Show");
    if (l.visible && !m_doc.shown(id)) eye->set_opacity(0.4);
    eye->signal_clicked().connect([this, id, v = l.visible] { L::set_visible(m_journal, m_doc, id, !v); });
    box->append(*eye);
    // The indent comes AFTER the eye: the eyes stay one column (Photoshop's).
    if (const int depth = m_doc.depth(id); depth > 0) {
        auto indent = Gtk::make_managed<Gtk::Box>();
        indent->set_size_request(18 * depth, -1);
        box->append(*indent);
    }

    // Expander (groups) -- view state, not a step.
    if (group) {
        const bool open = !m_collapsed.count(id);
        auto ex = icon_button(open ? "pan-down-symbolic" : "pan-end-symbolic", open ? "Collapse" : "Expand");
        ex->signal_clicked().connect([this, id] {
            if (!m_collapsed.erase(id)) m_collapsed.insert(id);
            queue_repaint();
        });
        box->append(*ex);
    } else {
        auto pad = Gtk::make_managed<Gtk::Box>();
        pad->set_size_request(30, -1);
        box->append(*pad);
    }

    if (l.clipped) {
        auto clip = Gtk::make_managed<Gtk::Label>("↳");
        clip->set_tooltip_text(m_doc.clip_base(id).empty() ? "Clipped (nothing below to clip to)"
                                                          : "Clipped to " + m_doc.get(m_doc.clip_base(id))->name);
        box->append(*clip);
    }
    auto kind = Gtk::make_managed<Gtk::Image>();
    kind->set_from_icon_name(group ? "folder-symbolic" : "image-x-generic-symbolic");
    box->append(*kind);

    if (id == m_renaming) {
        auto field = Gtk::make_managed<Gtk::Entry>();
        field->set_name("layers.rename");
        field->set_text(l.name);
        field->set_hexpand(true);
        field->signal_activate().connect([this, field] { commit_rename(field->get_text()); });
        auto leave = Gtk::EventControllerFocus::create();
        leave->signal_leave().connect([this, field] { commit_rename(field->get_text()); });
        field->add_controller(leave);
        auto esc = Gtk::EventControllerKey::create();
        esc->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval != GDK_KEY_Escape) return false;
            m_renaming.clear();
            m_keys_to_list = true;
            queue_repaint();
            return true;
        }, false);
        field->add_controller(esc);
        box->append(*field);
        m_rename_field = field;
    } else {
        auto name = Gtk::make_managed<Gtk::Label>(l.name.empty() ? "Untitled" : l.name);
        name->set_xalign(0.0f);
        name->set_hexpand(true);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        if (!m_doc.shown(id)) name->add_css_class("dim-label");
        auto dbl = Gtk::GestureClick::create();
        dbl->signal_pressed().connect([this, id](int n, double, double) { if (n == 2) start_rename(id); });
        name->add_controller(dbl);
        box->append(*name);
    }

    // What differs from a plain layer: blend, opacity, fill.
    std::string info;
    if (l.blend != core::Blend::Normal && l.blend != core::Blend::PassThrough)
        info += std::string(core::blend_label(l.blend));
    if (l.opacity < 100) info += (info.empty() ? "" : " · ") + std::to_string(l.opacity) + "%";
    if (l.fill < 100) info += (info.empty() ? "" : " · ") + std::string("fill ") + std::to_string(l.fill) + "%";
    if (!info.empty()) {
        auto il = Gtk::make_managed<Gtk::Label>(info);
        il->add_css_class("dim-label");
        il->add_css_class("caption");
        box->append(*il);
    }
    if (l.locks) {
        auto lk = Gtk::make_managed<Gtk::Image>();
        lk->set_from_icon_name("changes-prevent-symbolic");
        lk->set_tooltip_text((l.locks & core::lock::All) ? "Locked" : "Partly locked");
        box->append(*lk);
    } else if (m_doc.locked_all(id)) {
        auto lk = Gtk::make_managed<Gtk::Image>();
        lk->set_from_icon_name("changes-prevent-symbolic");
        lk->set_opacity(0.4);
        lk->set_tooltip_text("Locked by its group");
        box->append(*lk);
    }
    row->set_child(*box);
    add_dnd(*row, id);
    return row;
}

// ─────────────────────────────────────────────────────────────────────────────
// Drag and drop. The rows are rebuilt on every change, so NOTHING repaints
// while a drag is in flight (a rebuilt source row cancels the drag): the drop
// line is a CSS class on the row under the pointer, and the selection moves
// only after the drop. The drop itself is ONE call: core::layers::move.
// ─────────────────────────────────────────────────────────────────────────────
void LayersPanel::add_dnd(Gtk::ListBoxRow& row, const Id& id) {
    auto src = Gtk::DragSource::create();
    src->set_actions(Gdk::DragAction::MOVE);
    src->signal_prepare().connect([this, id](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
        if (!m_renaming.empty()) return nullptr;
        // A selected row drags the whole selection; an unselected one, itself.
        m_dragging = std::find(m_sel.begin(), m_sel.end(), id) != m_sel.end() ? m_sel : std::vector<Id>{id};
        std::string ids;
        for (const auto& d : m_dragging) ids += (ids.empty() ? "" : "\n") + d;
        Glib::Value<Glib::ustring> v;
        v.init(v.value_type());
        v.set(ids);
        return Gdk::ContentProvider::create(v);
    }, false);
    // Raw pointer: the controller owns this slot, so a RefPtr here would be a cycle.
    src->signal_drag_begin().connect([s = src.get(), r = &row](const Glib::RefPtr<Gdk::Drag>&) {
        s->set_icon(Gtk::WidgetPaintable::create(*r), 20, 12);
    });
    src->signal_drag_end().connect([this](const Glib::RefPtr<Gdk::Drag>&, bool) {
        m_dragging.clear();
        mark_drop(nullptr, nullptr);
    });
    row.add_controller(src);

    auto tgt = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);
    tgt->signal_motion().connect([this, id, r = &row](double, double y) {
        const auto where = where_on(id, y, r->get_height());
        if (m_dragging.empty() || !core::layers::can_move(m_doc, m_dragging, id, where)) {
            mark_drop(nullptr, nullptr);
            return Gdk::DragAction{};   // no drop here: the cursor says so
        }
        mark_drop(r, where == core::layers::Where::Above ? "drop-above"
                   : where == core::layers::Where::Below ? "drop-below" : "drop-inside");
        return Gdk::DragAction::MOVE;
    }, false);
    tgt->signal_leave().connect([this, r = &row] { if (m_drop_row == r) mark_drop(nullptr, nullptr); });
    tgt->signal_drop().connect([this, id, r = &row](const Glib::ValueBase&, double, double y) {
        return drop(id, where_on(id, y, r->get_height()));
    }, false);
    row.add_controller(tgt);
}

// Top quarter: above. Bottom quarter: below. The middle of a group: inside;
// of a layer: whichever half. Below an OPEN group is its first row, so that
// is inside it too (the line would sit between the group and its contents).
core::layers::Where LayersPanel::where_on(const Id& id, double y, int height) const {
    using W = core::layers::Where;
    const double h = height > 0 ? height : 1;
    const bool group = m_doc.is_group(id);
    W w;
    if (y < h * 0.25)      w = W::Above;
    else if (y > h * 0.75) w = W::Below;
    else if (group)        w = W::Inside;
    else                   w = y < h * 0.5 ? W::Above : W::Below;
    if (w == W::Below && group && !m_collapsed.count(id) && !m_doc.panel(id).empty()) w = W::Inside;
    return w;
}

void LayersPanel::mark_drop(Gtk::ListBoxRow* row, const char* css) {
    if (m_drop_row == row && m_drop_css == css) return;
    if (m_drop_row && m_drop_css) m_drop_row->remove_css_class(m_drop_css);
    m_drop_row = row;
    m_drop_css = css;
    if (row && css) row->add_css_class(css);
}

bool LayersPanel::drop(const Id& target, core::layers::Where where) {
    const auto moving = m_dragging;
    m_dragging.clear();
    mark_drop(nullptr, nullptr);
    if (moving.empty() || !core::layers::move(m_journal, m_doc, moving, target, where)) return false;
    if (where == core::layers::Where::Inside) m_collapsed.erase(target);   // show where it went
    m_journal.reselect(moving);
    select(moving, true);
    if (auto lg = log::get(log::Area::Shell)) lg->info("drop: {}", m_journal.undo_label());
    return true;
}

bool LayersPanel::primary_is_group() const { return m_doc.is_group(primary()); }
bool LayersPanel::primary_is_clipped() const {
    const auto l = primary().empty() ? std::nullopt : m_doc.get(primary());
    return l && l->clipped;
}

// ─────────────────────────────────────────────────────────────────────────────
// User choices -- each is ONE call into core::layers, then the selection.
// ─────────────────────────────────────────────────────────────────────────────
void LayersPanel::on_rows_selected() {
    if (m_painting) return;
    std::vector<Id> ids;
    for (auto* row : m_list.get_selected_rows()) ids.push_back(row->get_name());
    m_sel = in_walk_order(ids);
    if (m_changed) m_changed();
}

void LayersPanel::new_layer() {
    const auto id = L::add_layer(m_journal, m_doc, next_name(m_doc, "Layer"), primary());
    if (id.empty()) return;
    m_journal.reselect({id});
    select({id}, true);
}

void LayersPanel::new_group() {
    const auto id = L::add_group(m_journal, m_doc, next_name(m_doc, "Group"), primary());
    if (id.empty()) return;
    m_journal.reselect({id});
    select({id}, true);
}

void LayersPanel::duplicate() {
    const auto made = L::duplicate(m_journal, m_doc, m_sel);
    if (made.empty()) return;
    m_journal.reselect(made);
    select(made, true);
}

void LayersPanel::group() {
    const auto g = L::group(m_journal, m_doc, m_sel, next_name(m_doc, "Group"));
    if (g.empty()) return;
    m_journal.reselect({g});
    select({g}, true);
}

void LayersPanel::ungroup() {
    const auto g = primary();
    if (!m_doc.is_group(g)) return;
    const auto inside = m_doc.panel(g);
    if (!L::ungroup(m_journal, m_doc, g)) return;
    m_journal.reselect(inside);
    select(inside, true);
}

void LayersPanel::remove() {
    if (m_sel.empty()) return;
    // The row that takes the selection: the first survivor below the first
    // removed row in the panel, else the last survivor above it.
    const auto before = m_doc.walk();
    const auto at = std::find(before.begin(), before.end(), m_sel.front()) - before.begin();
    if (!L::remove(m_journal, m_doc, m_sel)) return;
    Id next;
    for (auto i = at; i < static_cast<long>(before.size()) && next.empty(); ++i)
        if (m_doc.get(before[static_cast<std::size_t>(i)])) next = before[static_cast<std::size_t>(i)];
    for (auto i = at - 1; i >= 0 && next.empty(); --i)
        if (m_doc.get(before[static_cast<std::size_t>(i)])) next = before[static_cast<std::size_t>(i)];
    const std::vector<Id> sel = next.empty() ? std::vector<Id>{} : std::vector<Id>{next};
    m_journal.reselect(sel);
    select(sel, true);   // the KEYBOARD goes with it: the next Ctrl+Z lands
}

void LayersPanel::raise() { if (L::raise(m_journal, m_doc, primary())) select(m_sel, true); }
void LayersPanel::lower() { if (L::lower(m_journal, m_doc, primary())) select(m_sel, true); }

void LayersPanel::toggle_clip() {
    const auto l = primary().empty() ? std::nullopt : m_doc.get(primary());
    if (l) L::set_clipped(m_journal, m_doc, primary(), !l->clipped);
}

void LayersPanel::rename_primary() { if (!primary().empty()) start_rename(primary()); }

void LayersPanel::start_rename(const Id& id) {
    if (!m_doc.get(id)) return;
    m_renaming = id;
    if (std::find(m_sel.begin(), m_sel.end(), id) == m_sel.end()) m_sel = {id};
    queue_repaint();
}

void LayersPanel::commit_rename(const std::string& text) {
    if (m_renaming.empty() || m_painting) return;
    const Id id = m_renaming;
    m_renaming.clear();
    const auto l = m_doc.get(id);
    if (l && l->name != text && !text.empty()) L::rename(m_journal, m_doc, id, text);
    m_keys_to_list = true;
    queue_repaint();
}

}  // namespace klimt
