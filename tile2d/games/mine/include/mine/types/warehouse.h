// Mine - the thing that holds what is dragged into it.
//
// A **warehouse** is a **scene plot** (types/scene_tile.h): it occupies its cells, it is scenery, and it
// is **never ticked** - nothing about it changes on its own (docs/GAME_DESIGN.md section 1.27). What it
// does, it does when something happens to it, and there are exactly two such things:
//
//   * a **drop** is dragged onto it (types/drop.h, pointer_drag.h) - its **draginto** half takes it into
//     its **container**, or says no because there is no room;
//   * it is **clicked** - its **click** half opens a panel that shows what is inside, drawn at the plot's
//     bottom-right corner by the world view.
//
// The three halves are components of their own (types/container.h, types/drag_into.h, types/click.h)
// because they are useful on their own: a machine's input side is a container and a draginto, a demand
// terminal is a container and a click. A warehouse is the first thing that has all three, and the first
// thing that is *only* that. The capacity it can hold comes from its entry (docs/MODS.md).
#pragma once

#include <mine/types/click.h>
#include <mine/types/container.h>
#include <mine/types/drag_into.h>
#include <mine/types/scene_tile.h>

namespace mine::types {

/// A scene plot that holds things: a container, the draginto that fills it, and the click that shows it.
class Warehouse : public SceneTile {
public:
    Warehouse(ContentKind kind, ContentId id, i32 layer, GridPos anchor, i32 width = 1, i32 height = 1);

    // --- its three halves -------------------------------------------------------------------------
    /// What it holds. This is the half other code talks to: a machine that should deliver here, a demand
    /// that counts what arrived, a save that writes it down. It is also the generic answer a plot gives
    /// (types/scene_tile.h) - the one a double click asks for - which is why it returns a pointer there:
    /// a floor has no container, a warehouse always does (docs/GAME_DESIGN.md section 1.31).
    [[nodiscard]] Container* container() override { return &container_; }
    [[nodiscard]] const Container* container() const override { return &container_; }
    /// What takes dragged drops in.
    [[nodiscard]] DragInto& draginto() { return draginto_; }
    [[nodiscard]] const DragInto& draginto() const { return draginto_; }
    /// What receives clicks.
    [[nodiscard]] Click& click() { return click_; }
    [[nodiscard]] const Click& click() const { return click_; }

    // --- what the layer and the screen ask it -----------------------------------------------------
    /// How much it can hold. It is a **definition** attribute: it comes from the entry's `capacity`, which
    /// every warehouse of that entry shares, and there is no setter - nothing may change it after the plot
    /// is built (docs/GAME_DESIGN.md section 1.35). A plot built without a definition (a test) falls back
    /// to the container it was made with.
    [[nodiscard]] usize capacity() const;
    /// A click arrived at \p cell: the warehouse takes it when the cell is one of its own, which is what
    /// opens (or closes) its panel. Every warehouse is told about every click, so the ones the click did
    /// not land on close theirs (types/click.h). A warehouse that is **still going up** takes no click at
    /// all: it answers as if the click had landed elsewhere, which also closes a panel it had open before
    /// its build started (types/scene_tile.h, docs/GAME_DESIGN.md section 1.37).
    bool on_click(GridPos cell) {
        if (under_construction()) return click_.receive(false);
        return click_.receive(covers(cell));
    }
    /// Whether \p cell is one of its cells: what a drag and a click are measured against.
    [[nodiscard]] bool holds(GridPos cell) const { return covers(cell); }

private:
    /// The container first: the draginto feeds it, so it has to exist before it.
    Container container_{0};
    DragInto draginto_{container_};
    Click click_;
};

} // namespace mine::types
