// Shell -- s096: Edit > Cut (Ctrl+X), Copy (Ctrl+C), Copy Merged
// (Ctrl+Shift+C), Paste (Ctrl+V), Paste in Place (Ctrl+Shift+V). The pixels
// are cut out on a worker (core/Clipboard); the piece goes on the SYSTEM
// clipboard as an 8-bit RGBA picture (other apps paste it) and stays here at
// full depth -- while the clipboard is still Klimt's, Paste uses that one.
// docs/clipboard.md.
#include "Shell.hpp"
#include "core/Selection.hpp"
#include "Log.hpp"

#include <gdkmm/clipboard.h>
#include <gdkmm/texture.h>
#include <gdk/gdk.h>

namespace klimt {

namespace C = core::clipboard;
namespace S = core::select;

namespace {

std::string size_words(const C::Piece& p) {
    return std::to_string(p.width()) + " × " + std::to_string(p.height()) + " px";
}

}  // namespace

void Shell::on_copy(bool merged, bool cut) {
    const std::string verb = cut ? "Cut" : merged ? "Copy Merged" : "Copy";
    if (!has_image()) { toast(verb + ": no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    const auto id = m_layers.primary();
    const auto l = id.empty() ? std::nullopt : m_doc.get(id);
    if (!merged) {
        if (!l) { toast(verb + ": pick a layer first"); return; }
        if (!core::draws_pixels(l->kind)) { toast(verb + ": " + core::layers::not_pixels(m_doc, id)); return; }
        if (!l->pixels) { toast(verb + ": “" + l->name + "” is empty"); return; }
    }
    if (cut) {
        if (l->kind != core::Kind::Pixel) { toast(verb + ": " + core::layers::not_pixels(m_doc, id)); return; }
        if (!m_doc.selection()) { toast("Cut: nothing is selected -- Copy (Ctrl+C) takes the whole layer"); return; }
        if (m_doc.locked_all(id) || (l->locks & core::lock::Pixels)) { toast(verb + ": “" + l->name + "” is locked"); return; }
        if ((l->locks & core::lock::Alpha) && m_doc.image().format.alpha) { toast(verb + ": “" + l->name + "” has its transparent pixels locked"); return; }
    }
    const core::Image im = m_doc.image();
    const auto layers = merged ? m_doc.snapshot() : std::map<core::Document::Id, core::Layer>{};
    const auto before = merged ? nullptr : l->pixels;
    const auto sel = m_doc.selection_ptr();
    const auto area = C::area(m_doc);
    const pixel::Px bg = core::from_srgb(im, m_rail.background().r, m_rail.background().g, m_rail.background().b);
    const std::string label = cut ? "Cut “" + l->name + "”" : verb;
    start_job(label, [this, im, layers, before, sel, area, bg, cut, merged, id, verb, label](core::Jobs::Token& t) -> core::Jobs::Landing {
        const auto stop = [&t] { return t.cancelled(); };
        std::optional<pixel::TileMap> flat;
        if (merged) {
            formats::Progress p;
            p.cancelled = stop;
            flat = core::file::flatten(im, layers, p);
            if (!flat) return [] {};
        }
        const pixel::TileMap& src = merged ? *flat : *before;
        auto piece = C::cut_out(src, sel.get(), area, stop);
        if (t.cancelled()) return [] {};
        if (!piece) return [this, verb] { toast(verb + ": nothing there -- the selected area is clear"); };
        std::shared_ptr<pixel::TileMap> after;
        if (cut) after = std::make_shared<pixel::TileMap>(S::cleared(*before, sel.get(), bg));
        auto rgba = std::make_shared<std::vector<std::uint8_t>>(C::rgba8(*piece));
        auto pc = std::make_shared<C::Piece>(std::move(*piece));
        return [this, pc, rgba, after, before, sel, cut, id, verb, label] {
            if (cut && (m_doc.selection_ptr() != sel ||
                        !core::layers::set_pixels_if(m_journal, m_doc, id, before, std::move(*after), label))) {
                toast(verb + ": the layer changed meanwhile -- nothing done");
                return;
            }
            m_clip = *pc;
            if (!rgba->empty()) {
                const gsize n = rgba->size();
                GBytes* bytes = g_bytes_new(rgba->data(), n);
                GdkTexture* tex = gdk_memory_texture_new(pc->width(), pc->height(), GDK_MEMORY_R8G8B8A8, bytes,
                                                         static_cast<gsize>(pc->width()) * 4);
                g_bytes_unref(bytes);
                gdk_clipboard_set_texture(get_clipboard()->gobj(), tex);
                g_object_unref(tex);
            }
            if (auto lg = log::get(log::Area::Shell)) lg->info("clipboard: {} {}", verb, size_words(*pc));
            toast(verb + " -- " + size_words(*pc) + (rgba->empty() ? " (kept in Klimt: other apps take RGB)" : ""), cut);
        };
    });
}

void Shell::on_paste(bool in_place) {
    const std::string verb = in_place ? "Paste in Place" : "Paste";
    if (!has_image()) { toast(verb + ": no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    auto cb = get_clipboard();
    // Ours still (nobody has copied since): the full-depth piece, where it was.
    if (m_clip && cb->is_local()) { paste_piece(*m_clip, in_place); return; }
    cb->read_texture_async([this, cb, in_place, verb](Glib::RefPtr<Gio::AsyncResult>& res) {
        Glib::RefPtr<Gdk::Texture> tex;
        try { tex = cb->read_texture_finish(res); } catch (const Glib::Error&) {}
        if (!tex) { toast(verb + ": the clipboard holds no picture"); return; }
        const int w = tex->get_width(), h = tex->get_height();
        GdkTextureDownloader* dl = gdk_texture_downloader_new(tex->gobj());
        gdk_texture_downloader_set_format(dl, GDK_MEMORY_R8G8B8A8);
        gsize stride = 0;
        GBytes* bytes = gdk_texture_downloader_download_bytes(dl, &stride);
        gdk_texture_downloader_free(dl);
        gsize n = 0;
        const auto* data = static_cast<const std::uint8_t*>(g_bytes_get_data(bytes, &n));
        C::Piece p = C::from_rgba8(data, w, h, static_cast<int>(stride));
        g_bytes_unref(bytes);
        if (auto lg = log::get(log::Area::Shell)) lg->info("clipboard: {} from another app, {} x {}", verb, w, h);
        paste_piece(p, in_place);
    });
}

void Shell::paste_piece(const C::Piece& p, bool in_place) {
    const std::string verb = in_place ? "Paste in Place" : "Paste";
    if (const auto why = C::refusal(m_doc, p); !why.empty()) { toast(verb + ": " + why); return; }
    const core::Image im = m_doc.image();
    const auto above = m_layers.primary();
    const auto mark = m_journal.mark();
    start_job(verb, [this, im, p, in_place, above, mark, verb](core::Jobs::Token& t) -> core::Jobs::Landing {
        auto px = C::placed(im, p, in_place);
        if (t.cancelled()) return [] {};
        if (!px) return [this, verb] { toast(verb + ": refused"); };
        auto res = *px;
        return [this, res, in_place, above, mark, verb] {
            if (m_journal.mark() != mark) { toast(verb + ": the picture changed meanwhile -- nothing done"); return; }
            const auto id = C::land(m_journal, m_doc, res, above, in_place);
            if (id.empty()) { toast(verb + ": refused"); return; }
            m_layers.select({id}, false);
        };
    });
}

}  // namespace klimt
