// Shell -- s096: Layer > Merge Down (Ctrl+E), Merge Visible, Flatten Image,
// Stamp Visible (Ctrl+Shift+Alt+E). The composite on a worker (core/Merge),
// landed as ONE step when it's done -- nothing if the picture changed
// meanwhile. Recorded in actions. docs/merge.md.
#include "Shell.hpp"
#include "core/Merge.hpp"
#include "Log.hpp"

namespace klimt {

namespace M = core::merge;

void Shell::on_merge(int how_i) {
    const auto how = static_cast<M::How>(how_i);
    const std::string verb = M::label(how);
    if (!has_image()) { toast(verb + ": no image -- File > New (Ctrl+N) or Open (Ctrl+O)"); return; }
    const auto picked = m_layers.primary();
    if (const auto why = M::refusal(m_doc, how, picked); !why.empty()) { toast(verb + ": " + why); return; }
    const core::Image im = m_doc.image();
    const auto src = M::source(m_doc, how, picked);
    const auto mark = m_journal.mark();
    const int dropped = how == M::How::Flatten ? M::hidden_dropped(m_doc) : 0;
    start_job(verb, [this, im, src, mark, how, verb, picked, dropped](core::Jobs::Token& t) -> core::Jobs::Landing {
        formats::Progress p;
        p.cancelled = [&t] { return t.cancelled(); };
        p.progress = [&t](double f) { t.progress(f); };
        auto px = M::render(im, src, p);
        if (!px) return [] {};
        auto res = *px;
        return [this, res, mark, how, verb, picked, dropped] {
            if (m_journal.mark() != mark) { toast(verb + ": the picture changed meanwhile -- nothing done"); return; }
            const auto id = M::land(m_journal, m_doc, how, picked, res);
            if (id.empty()) { toast(verb + ": refused"); return; }
            m_layers.select({id}, false);
            rec(core::actions::merge(how));
            if (auto lg = log::get(log::Area::Shell)) lg->info("merge: {} landed in {}", verb, id);
            if (dropped > 0)
                toast(verb + " -- " + std::to_string(dropped) + (dropped == 1 ? " hidden layer" : " hidden layers") + " dropped", true);
            else
                toast(m_journal.undo_label(), true);
        };
    });
}

}  // namespace klimt
