#include "core/Rulers.hpp"   // s031
#include "Shell.hpp"
#include "Log.hpp"
#include "SizeDialog.hpp"
#include "ProfileDialog.hpp"   // s068
#include "ColourSettingsDialog.hpp"   // s070
#include "core/Profile.hpp"
#include "core/Canvas.hpp"

#include <cmath>

// Shell_canvas.cpp -- THE CANVAS (s009 m2): Crop (the tool's box, and Crop to
// Selection), Image > Image Size, Image > Canvas Size. Each is a JOB: the
// plan is made on a worker from a picture of the document (core::canvas::
// before), and the landing writes it as ONE step through canvas::land --
// refused, with a toast, if the document moved while the worker ran.

namespace klimt {

namespace Cv = core::canvas;

void Shell::run_canvas_job(const std::string& label, CanvasPlan plan) {
    if (m_move.active || m_stroke.active) return;
    if (!has_image()) { toast(label + ": no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    const auto before = std::make_shared<const Cv::Before>(Cv::before(m_doc));
    start_job(label, [this, label, before, plan = std::move(plan)](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto r = plan(*before, t);
        if (!r || t.cancelled()) return [] {};
        auto res = std::make_shared<const Cv::Reshape>(std::move(*r));
        return [this, label, before, res] {
            if (!Cv::land(m_journal, m_doc, *before, *res, label)) {
                toast(label + ": the document changed meanwhile -- nothing done");
                return;
            }
            update_summary();
        };
    });
}

// The Crop tool's box, committed (Enter, a double-click, Image > Crop).
void Shell::on_crop_box(core::select::Pt a, core::select::Pt b) {
    const int x0 = static_cast<int>(std::lround(std::min(a.x, b.x))), y0 = static_cast<int>(std::lround(std::min(a.y, b.y)));
    const int x1 = static_cast<int>(std::lround(std::max(a.x, b.x))), y1 = static_cast<int>(std::lround(std::max(a.y, b.y)));
    const pixel::Rect r{x0, y0, x1 - x0, y1 - y0};
    if (const auto why = Cv::size_refusal(r.w, r.h); !why.empty()) { toast("Crop: " + why); return; }
    const auto bg = background_px();
    const bool del = m_strip.crop_deletes();
    run_canvas_job("Crop", [r, bg, del](const Cv::Before& b, core::Jobs::Token&) { return Cv::crop_plan(b, r, bg, del); });
}

void Shell::on_crop_selection() {
    if (!m_doc.selection()) { toast("Crop to Selection: nothing is selected"); return; }
    const auto bg = background_px();
    const bool del = m_strip.crop_deletes();
    run_canvas_job("Crop", [bg, del](const Cv::Before& b, core::Jobs::Token&) -> std::optional<Cv::Reshape> {
        if (!b.mask) return std::nullopt;
        const auto r = Cv::bounds(*b.mask);
        if (r.empty()) return std::nullopt;
        return Cv::crop_plan(b, r, bg, del);
    });
}

// s010: Image > Reveal All -- the canvas grows to every pixel the layers hold.
void Shell::on_reveal_all() {
    if (!has_image()) { toast("Reveal All: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    bool any = false;
    for (const auto& [id, l] : m_doc.snapshot()) any = any || (l.pixels && l.pixels->has_outside());
    if (!any) { toast("Reveal All: nothing lies beyond the canvas"); return; }
    run_canvas_job("Reveal All", [](const Cv::Before& b, core::Jobs::Token&) { return Cv::reveal_plan(b); });
}

void Shell::on_image_size() {
    if (!has_image()) { toast("Image Size: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_image_size) {
        m_image_size = std::make_unique<SizeDialog>(*this, SizeDialog::Mode::Image);
        m_image_size->signal_ok().connect([this](const SizeDialog::Answer& a) {
            run_canvas_job("Image Size", [a](const Cv::Before& b, core::Jobs::Token& t) {
                return Cv::size_plan(b, a.width, a.height, a.dpi, a.filter, [&t] { return t.cancelled(); },
                                     [&t](double p) { t.progress(p); });
            });
        });
    }
    const auto& im = m_doc.image();
    m_image_size->ask(im.width, im.height, im.dpi);
}

// s068 (WEAKNESSES 14): Image > Assign Profile -- one step, at once (the
// numbers stay); Convert to Profile and Image > Mode -- a canvas job.
void Shell::on_profile_assign() {
    if (!has_image()) { toast("Assign Profile: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_profile_assign) {
        m_profile_assign = std::make_unique<ProfileDialog>(*this, ProfileDialog::Kind::Assign);
        m_profile_assign->signal_ok().connect([this](core::profile::IccPtr icc) {
            if (const auto why = core::profile::assign_refusal(m_doc.image(), icc); !why.empty()) { toast("Assign Profile: " + why); return; }
            if (!core::profile::assign(m_journal, m_doc, icc)) { toast("Assign Profile: that is the profile already"); return; }
            update_summary();
            queue_render();
        });
    }
    m_profile_assign->ask(m_doc.image());
}

void Shell::convert_to(const core::profile::IccPtr& icc, tint::Render r) {
    if (const auto why = core::profile::convert_refusal(m_doc.image(), icc); !why.empty()) { toast("Convert to Profile: " + why); return; }
    run_canvas_job("Convert to Profile", [icc, r](const Cv::Before& b, core::Jobs::Token& t) {
        return core::profile::convert_plan(b, icc, r, [&t] { return t.cancelled(); }, [&t](double p) { t.progress(p); });
    });
}

// s070: EDIT > COLOUR SETTINGS -- the working CMYK / Gray (what untagged
// documents of those modes mean; Image > Mode's target) and the
// conversions' intent. Prefs; tint holds the working spaces process-wide.
namespace {
tint::Render render_of(const std::string& key, bool bpc) {
    tint::Render r;
    tint::intent_from(key, r.intent);
    r.bpc = bpc;
    return r;
}
}  // namespace

tint::Render Shell::conversion_render() const { return render_of(m_prefs.intent, m_prefs.bpc); }
tint::Render Shell::proof_render() const { return render_of(m_prefs.proof_intent, m_prefs.proof_bpc); }

void Shell::on_colour_settings() {
    if (!m_colour_settings) {
        m_colour_settings = std::make_unique<ColourSettingsDialog>(*this);
        m_colour_settings->signal_ok().connect([this](ColourSettingsDialog::Answer a) {
            m_prefs.working_cmyk = core::profile::remembered_as(a.cmyk);
            m_prefs.working_gray = core::profile::remembered_as(a.gray);
            m_prefs.intent = tint::intent_key(a.render.intent);
            m_prefs.bpc = a.render.bpc;
            core::save_prefs(m_prefs_file, m_prefs);
            apply_colour_settings();
            if (m_prefs.proof_profile.empty()) m_proof_choice.reset();   // the proof's default follows the working CMYK
            if (m_proof || m_gamut) proof_profile();
            if (has_image()) update_summary();
            toast("Colour Settings: working CMYK " + a.cmyk.name + ", Gray " + a.gray.name);
        });
    }
    const auto pick = [](const std::string& r, pixel::Mode m) {
        if (auto c = core::profile::find(r); c && c->mode == m) return *c;
        for (const auto& c : core::profile::catalog())
            if (c.path.empty() && c.mode == m && (c.name == "Plain CMYK (no dot gain)" || c.name == "Gray (sRGB curve)")) return c;
        return core::profile::catalog().front();
    };
    m_colour_settings->ask(pick(m_prefs.working_cmyk, pixel::Mode::CMYK), pick(m_prefs.working_gray, pixel::Mode::Gray), conversion_render());
}

void Shell::apply_colour_settings() {
    const auto get = [](const std::string& r) -> core::profile::IccPtr {
        const auto c = core::profile::find(r);
        return c ? c->icc : nullptr;
    };
    auto cmyk = get(m_prefs.working_cmyk), gray = get(m_prefs.working_gray);
    // Klimt's plain profiles ARE the plain conversion: kept as none, so the
    // numbers don't take a trip through a table for nothing.
    if (cmyk && *cmyk == *core::profile::builtin(pixel::Mode::CMYK)) cmyk = nullptr;
    if (gray && tint::inspect(*gray).name == "Gray (sRGB curve)") gray = nullptr;
    tint::set_working(tint::Space::CMYK, cmyk);
    tint::set_working(tint::Space::Gray, gray);
    m_dirty = {};   // untagged Gray / CMYK may look different now
    m_shown = {};
    if (has_image()) { update_summary(); queue_render(); }
}

void Shell::on_profile_convert() {
    if (!has_image()) { toast("Convert to Profile: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_profile_convert) {
        m_profile_convert = std::make_unique<ProfileDialog>(*this, ProfileDialog::Kind::Convert);
        m_profile_convert->signal_ok().connect([this](core::profile::IccPtr icc) { convert_to(icc, m_profile_convert->render()); });
    }
    m_profile_convert->ask(m_doc.image(), conversion_render());   // s070: Colour Settings' intent to start
}

void Shell::on_mode(pixel::Mode m) {
    if (!has_image() || m_doc.image().format.mode == m) return;
    convert_to(core::profile::mode_profile(m), conversion_render());   // s070: the working space, Colour Settings' intent
}

// s069 (WEAKNESSES 14): VIEW > PROOF COLORS / GAMUT WARNING -- view state,
// per tab: the tiles shown through the press of View > Proof Setup (the
// prefs keep it), the document untouched. Not a step.
const core::profile::Choice& Shell::proof_profile() {
    if (!m_proof_choice) m_proof_choice = core::profile::proof_choice(m_prefs.proof_profile);
    return *m_proof_choice;
}

core::Proofing Shell::proofing() const {
    core::Proofing p;
    if (!has_image() || (!m_proof && !m_gamut) || !m_proof_choice) return p;
    p.press = m_proof_choice->icc;
    p.colours = m_proof;
    p.paper = m_proof && m_prefs.proof_paper;
    p.gamut = m_gamut;
    p.render = proof_render();   // s070
    return p;
}

// The menu's check marks, this tab's.
void Shell::sync_proof() {
    if (m_proof_act) m_proof_act->change_state(Glib::Variant<bool>::create(m_proof));
    if (m_gamut_act) m_gamut_act->change_state(Glib::Variant<bool>::create(m_gamut));
}

void Shell::on_proof_setup() {
    if (!has_image()) { toast("Proof Setup: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_proof_setup) {
        m_proof_setup = std::make_unique<ProfileDialog>(*this, ProfileDialog::Kind::Proof);
        m_proof_setup->signal_proof().connect([this](core::profile::Choice c, bool paper) {
            m_prefs.proof_profile = core::profile::remembered_as(c);
            m_prefs.proof_paper = paper;
            const auto r = m_proof_setup->render();   // s070
            m_prefs.proof_intent = tint::intent_key(r.intent);
            m_prefs.proof_bpc = r.bpc;
            core::save_prefs(m_prefs_file, m_prefs);
            m_proof_choice = std::move(c);
            m_proof = true;   // as Photoshop: setting the proof up shows it
            update_summary();
            sync_proof();
            queue_render();
        });
    }
    m_proof_setup->ask_proof(m_doc.image(), proof_profile(), m_prefs.proof_paper, proof_render());
}

void Shell::on_proof_colours() {
    if (!has_image()) return;
    proof_profile();
    m_proof = !m_proof;
    update_summary();
    sync_proof();
    queue_render();
    toast(m_proof ? "Proof Colors: shown as " + m_proof_choice->name + " would print it -- View > Proof Setup to change"
                  : "Proof Colors off");
}

void Shell::on_gamut_warning() {
    if (!has_image()) return;
    proof_profile();
    m_gamut = !m_gamut;
    update_summary();
    sync_proof();
    queue_render();
    if (m_gamut) toast("Gamut Warning: grey marks what " + m_proof_choice->name + " can't print");
}

void Shell::on_canvas_size() {
    if (!has_image()) { toast("Canvas Size: no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    if (!m_canvas_size) {
        m_canvas_size = std::make_unique<SizeDialog>(*this, SizeDialog::Mode::Canvas);
        m_canvas_size->signal_ok().connect([this](const SizeDialog::Answer& a) {
            const auto bg = background_px();
            run_canvas_job("Canvas Size", [a, bg](const Cv::Before& b, core::Jobs::Token&) {
                return Cv::canvas_plan(b, a.width, a.height, a.anchor_x, a.anchor_y, bg);
            });
        });
    }
    const auto& im = m_doc.image();
    m_canvas_size->ask(im.width, im.height, im.dpi);
}

// s031: the rulers' numbers -- the document's resolution, the side each
// measures, its zero point.
void Shell::update_rulers() {
    if (!m_hruler) return;
    const auto& im = m_doc.image();
    const bool any = im.width > 0 && im.height > 0;
    m_hruler->set_doc({im.dpi, static_cast<double>(im.width), im.origin_x, any});
    m_vruler->set_doc({im.dpi, static_cast<double>(im.height), im.origin_y, any});
}

// s032: the guides on the canvas follow the document and the View menu.
void Shell::update_guides() {
    m_canvas.set_guides(m_doc.image().guides, m_prefs.show_guides, m_prefs.lock_guides);
    m_canvas.set_snap(m_prefs.snap);
}

void Shell::show_rulers(bool on) {
    if (!m_hruler) return;
    m_prefs.rulers = on;
    m_hruler->set_visible(on);
    m_vruler->set_visible(on);
    m_ruler_corner.set_visible(on);
    core::save_prefs(m_prefs_file, m_prefs);
    update_rulers();
}

}  // namespace klimt
