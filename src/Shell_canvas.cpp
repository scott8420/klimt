#include "core/Rulers.hpp"   // s031
#include "Shell.hpp"
#include "Log.hpp"
#include "SizeDialog.hpp"
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
