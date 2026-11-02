#pragma once
// core::snapshot -- History's SNAPSHOTS (s077; REQUIREMENTS added 7).
//
// A snapshot is the whole document as it was at a moment, named: every
// record -- the layers, the image, the selection, the saved channels -- by
// value (the pixels are shared pointers to immutable maps, so a snapshot of a
// big picture costs the records, not the pixels). Going back to one is ONE
// step through the one door ("Snapshot 1"): each record put back as the
// snapshot holds it, a record it doesn't hold removed -- so the step undoes
// like any other, and the steps after it in History stay redoable until the
// next step. Snapshots themselves are view state (Photoshop's: not saved,
// not undone); the Shell keeps them per document tab.
#include "core/Document.hpp"

#include <map>
#include <string>
#include <vector>

namespace klimt::core::snapshot {

struct Snapshot {
    std::string name;
    std::map<Document::Id, Layer> records;   // every record, the image's and the selection's too
};

// Every record id the document holds now (layers, channels, the selection,
// the image).
std::vector<Document::Id> ids(const Document& d);
Snapshot take(const Document& d, const std::string& name);
// "Snapshot N", N one past the highest number taken so far.
std::string next_name(const std::vector<Snapshot>& have);
// The document back as `s` holds it -- one step "<name>". False: nothing
// would change.
bool restore(DocJournal& j, Document& d, const Snapshot& s);

}  // namespace klimt::core::snapshot
