#pragma once
#include <string>
#include <vector>

class Graph;
class ImageCache;
class Evaluator;
struct EvalContext;

// Renders every enabled File Output node in `g` at full resolution and writes it to disk.
// Returns one human-readable line per node (written path or error).
std::vector<std::string> writeFileOutputs(const Graph& g, ImageCache& cache);
// Same, reusing an evaluator (and its cache) that already rendered other parts of the graph.
std::vector<std::string> writeFileOutputs(const Graph& g, Evaluator& ev, EvalContext& ctx);
