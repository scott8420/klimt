#pragma once
// core::actions -- ACTIONS AND BATCH (s084; REQUIREMENTS "Added" 10;
// docs/actions.md). Record the steps that make a picture, play them on
// another, run them over a folder.
//
// A step in the journal is two pictures (core/Undo.hpp) -- it can be undone
// but not repeated elsewhere. So an action records WHAT WAS ASKED, not what
// came of it: each act is a verb from this file's table and its settings as
// JSON text ("filter.apply" {"filter": {Gaussian Blur, radius 2}}), named in
// words for the panel ("Gaussian Blur  2 px"). The Shell records an act when
// the verb it ran lands; playing runs the table's function for each act.
//
// PLAYING is pure: a snapshot of the document (core/Snapshot.hpp) and the
// picked layer in, the acts run one after another on a private Document --
// on a worker -- and the new snapshot out, which the Shell lands as ONE step
// "Action “name”" (undone whole). An act that can't run (no selection to
// fill, a filter on a group) stops the action there and says which and why;
// nothing lands. An act switched off is skipped.
//
// What an act acts on: the PICKED layer, as Photoshop's "current layer" --
// the one picked when play starts, then whatever a step makes (a new layer, a
// duplicate, an adjustment layer is picked as it is made).
//
// Sizes: Image Size remembers whether the proportions were kept -- then a
// picture of another shape keeps ITS proportions (the width as recorded, the
// height to match), or `percent` scales by the recorded ratio. Canvas Size
// is absolute (or relative: grown by the recorded amount).
//
// GTK-free; the library is a JSON file (`load` / `save`).
#include "core/Canvas.hpp"
#include "core/Document.hpp"
#include "core/Snapshot.hpp"
#include "formats/Formats.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::actions {

struct Act {
    std::string verb;          // the table's key: "filter.apply"
    std::string args;          // its settings, a JSON object as text
    std::string label;         // what the panel says: "Gaussian Blur  2 px"
    bool        on = true;     // switched off: skipped
    bool operator==(const Act&) const = default;
};

struct Action {
    std::string      name;
    std::vector<Act> acts;
    bool operator==(const Action&) const = default;
};

// ── the table ─────────────────────────────────────────────────────────────
// Every verb an action can hold: its key and its menu name.
struct Verb { const char* key; const char* name; };
const std::vector<Verb>& verbs();
bool known(const std::string& verb);

// ── making acts (what the Shell records) ─────────────────────────────────
Act image_size(int w, int h, double dpi, canvas::Filter f, int from_w, int from_h);
Act canvas_size(int w, int h, int anchor_x, int anchor_y, int from_w, int from_h);
Act turn(int t);   // xform::Turn as int (Clockwise, CounterClockwise, Half, FlipHorizontal, FlipVertical)
Act rotate_canvas(double degrees);
Act apply_adjust(const Adjust& a);
Act adjust_layer(const Adjust& a);
Act apply_auto(adjust::AutoMethod m);
Act apply_filter(const Filter& f);
Act live_filter(const Filter& f);
Act filter_layer(const Filter& f);
Act select_all();
Act select_none();
Act select_invert();
Act select_subject();
Act remove_background();   // s087: the picked layer's mask from the model   // s086: the local model (libs/seg) -- inside the selection when there is one
Act feather(double radius);
Act fill_selection(const pixel::Px& colour);   // straight, in the document's channels
Act new_layer(const std::string& name);
Act duplicate_layer();
Act delete_layer();
Act add_mask(layers::MaskFrom from);
Act set_opacity(int percent);
Act set_blend(Blend b);
// An adjustment / filter layer's settings changed after it was made (in
// Properties): the act that made it takes them (the panel's line too).
void retune(Act& a, const Adjust& now);
void retune(Act& a, const Filter& now);

// ── playing ───────────────────────────────────────────────────────────────
struct Played {
    bool                ok = false;
    snapshot::Snapshot  result;      // named "Action “name”" -- what lands
    Document::Id        picked;      // picked when it finished
    int                 ran = 0;     // acts that ran (skipped ones not counted)
    std::string         error;       // why it stopped: "step 3, Fill: nothing is selected"
};
// Run `a` on the document `start` (its picked layer `picked`). Pure; on a
// worker. `cancelled` stops between acts (and inside the long ones);
// `progress` gets 0..1 as the acts go.
Played play(const snapshot::Snapshot& start, const Document::Id& picked, const Action& a,
            const std::function<bool()>& cancelled = {}, const std::function<void(double)>& progress = {});
// One act on a live pair (the tests; play() uses it). False with `error`.
bool run(DocJournal& j, Document& d, Document::Id& picked, const Act& act, std::string& error,
         const std::function<bool()>& cancelled = {});

// ── the library (~/.config/klimt/actions.json) ───────────────────────────
std::vector<Action> load(const std::string& file);
bool save(const std::string& file, const std::vector<Action>& all);
// A fresh library: a few useful ones to start from (and to show the shape).
std::vector<Action> starters();
// "Action 1", one past the highest so far.
std::string next_name(const std::vector<Action>& have);

// ── batch ─────────────────────────────────────────────────────────────────
// File > Automate > Batch: every picture in a folder through an action,
// written to another folder (or beside itself, with a suffix) as a format.
struct Batch {
    std::string   source;                     // a folder
    bool          subfolders = false;
    std::string   dest;                       // a folder; "" = beside each source
    std::string   suffix;                     // added to the name ("-web"); never empty when dest is ""
    formats::Kind kind = formats::Kind::Klimt; // what each is written as
    formats::WriteOptions options;
    bool          overwrite = false;          // a file already there: replaced, else skipped (said)
};
// The files a batch will take, sorted (what Klimt opens: by their bytes).
std::vector<std::string> sources(const Batch& b);
// Where one source goes.
std::string target_of(const Batch& b, const std::string& source);
// "" when the batch can run, else why not (no folder, the source is the
// destination with no suffix ...).
std::string refusal(const Batch& b, const Action& a);
struct BatchResult {
    int done = 0, skipped = 0, failed = 0;
    std::vector<std::string> lines;   // "photo.jpg: step 2, Fill: nothing is selected"
    bool cancelled = false;
};
// Run it (a worker). `progress` 0..1 over the files; `at` names the file in hand.
BatchResult run_batch(const Batch& b, const Action& a, const std::function<bool()>& cancelled = {},
                      const std::function<void(double)>& progress = {},
                      const std::function<void(const std::string&)>& at = {});

}  // namespace klimt::core::actions
