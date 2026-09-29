#pragma once
#include <string>
#include <vector>

class Graph;
class ImageCache;

// Renders every enabled File Output node in `g` at full resolution and writes it to disk.
// Returns one human-readable line per node (written path or error).
std::vector<std::string> writeFileOutputs(const Graph& g, ImageCache& cache);
