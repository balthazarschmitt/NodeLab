#pragma once
// One-click graph edits built from ordinary nodes, for workflows that would otherwise mean
// wiring several nodes by hand (Lightroom's "new mask").
#include "graph/Graph.h"

#include <set>

namespace recipes {

// Lightroom's mask tools: Linear Gradient, Radial Gradient, Brush, and Range (luminance).
enum class MaskKind { Linear, Radial, Brush, Range };

struct AddedMask {
    int adjust = 0;  // the new Basic
    int mask = 0;    // the mask node driving its Factor
    bool ok() const { return adjust && mask; }
};

// Adds a local adjustment at the end of the chain: a new Basic inserted right before the Output
// (the first Output node), with a new mask wired into its Factor. Brush and Range masks also get
// the image entering the Basic on their Image input (for Auto Mask and the range). The Basic is
// labelled "Mask N". Returns nothing (and changes nothing) if there is no Output fed by an image.
AddedMask addMask(Graph& g, MaskKind kind);

// Limits the edit made by a chain of nodes to a mask, like Blender's compositor recipe of a Mix
// after the edit: a Mix (labelled "Mask N") goes after the chain's last node, with A = the image
// entering the chain (from outside it), B = the edited image and Factor = a new mask. Wires that
// left the chain now leave the Mix, and the nodes downstream move right to make room. Range and
// Brush masks get the entering image. `adjust` is the Mix. Fails (changing nothing) if the
// chain has no image output.
AddedMask maskNodes(Graph& g, const std::set<int>& ids, MaskKind kind);

}  // namespace recipes
