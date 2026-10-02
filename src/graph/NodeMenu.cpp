#include "graph/NodeMenu.h"

#include <algorithm>
#include <map>

#include "graph/NodeRegistry.h"

namespace nodemenu {

namespace {

// Blender's grouping where it has one (Color Key under Matte, File Output with Output, Separate /
// Combine as their own menu), with the 12 split/combine nodes moved out of Color so no submenu
// is more than about 20 rows.
const std::vector<Menu> kLayout = {
    {"Input / Output", {"io.image_input", "io.number", "util.image_info", "", "io.output", "util.file_output"}},
    {"Color",
     {"color.basic", "color.color_grading", "color.color_mixer", "",                                  //
      "color.exposure", "color.brightness_contrast", "color.gamma", "color.levels", "color.curves",  //
      "color.tone_map", "",                                                                           //
      "color.saturation", "color.hue_shift", "color.hue_correct", "color.color_balance", "",         //
      "color.invert", "color.luminance", "color.convert_colorspace"}},
    {"Split / Combine",
     {"color.split_rgb", "color.combine_rgb", "color.split_hsv", "color.combine_hsv", "color.split_hsl",
      "color.combine_hsl", "color.split_lab", "color.combine_lab", "color.split_ycbcr", "color.combine_ycbcr",
      "color.split_yuv", "color.combine_yuv"}},
    {"Mix", {"math.mix", "math.blend", "math.alpha_over"}},
    {"Filter",
     {"filter.blur", "filter.directional_blur", "filter.bilateral_blur", "filter.denoise", "filter.sharpen", "filter.spot_removal", "",  //
      "filter.filter", "filter.kuwahara", "filter.dilate_erode", "",                              //
      "filter.pixelate", "filter.posterize", "",                                                  //
      "filter.glare", "filter.sun_beams"}},
    {"Matte",
     {"matte.linear_gradient", "matte.radial_gradient", "matte.brush_mask", "matte.range_mask", "",  //
      "matte.box_mask", "matte.ellipse_mask", "matte.double_edge_mask", "",                           //
      "matte.channel_key", "matte.luminance_key", "conv.color_key", "matte.difference_key",
      "matte.distance_key", "matte.chroma_key", "matte.color_spill"}},
    {"Transform",
     {"xform.transform", "xform.flip", "xform.crop", "",              //
      "xform.lens_distortion", "xform.lens_correction", "",          //
      "xform.displace", "xform.map_uv", "xform.corner_pin"}},
    {"Texture", {"tex.noise", "tex.voronoi", "tex.gradient", "tex.wave", "tex.checker", "tex.white_noise"}},
    {"Converter",
     {"conv.math", "conv.map_range", "conv.clamp", "conv.threshold", "conv.normalize", "conv.float_curve", "",  //
      "conv.color_ramp", "conv.wavelength", "conv.blackbody", "",                                              //
      "conv.set_alpha", "",                                                                                    //
      "conv.expression", "conv.image_expression"}},
    {"Utility", {"util.reroute", "util.switch", "util.split"}},
};

struct Built {
    std::vector<Menu> menus;
    std::map<std::string, std::string> menuOf;
};

const Built& built() {
    static const Built b = [] {
        Built r;
        const auto& reg = NodeRegistry::instance();
        for (const Menu& m : kLayout) {
            Menu out{m.name, {}};
            for (const auto& t : m.items) {
                if (t.empty()) {
                    // No separator first, last or doubled (a section whose nodes are all missing).
                    if (!out.items.empty() && !out.items.back().empty()) out.items.push_back(t);
                    continue;
                }
                const NodeInfo* inf = reg.find(t);
                if (!inf || inf->hidden || r.menuOf.count(t)) continue;
                out.items.push_back(t);
                r.menuOf[t] = m.name;
            }
            if (!out.items.empty() && out.items.back().empty()) out.items.pop_back();
            r.menus.push_back(std::move(out));
        }
        for (const auto& t : reg.types()) {
            const NodeInfo* inf = reg.find(t);
            if (inf->hidden || r.menuOf.count(t)) continue;
            auto it = std::find_if(r.menus.begin(), r.menus.end(), [&](const Menu& m) { return m.name == inf->category; });
            if (it == r.menus.end()) it = r.menus.insert(r.menus.end(), Menu{inf->category, {}});
            it->items.push_back(t);
            r.menuOf[t] = inf->category;
        }
        r.menus.erase(std::remove_if(r.menus.begin(), r.menus.end(), [](const Menu& m) { return m.items.empty(); }),
                      r.menus.end());
        return r;
    }();
    return b;
}

}  // namespace

const std::vector<Menu>& menus() { return built().menus; }

const std::string& menuOf(const std::string& type) {
    static const std::string none;
    const auto& m = built().menuOf;
    auto it = m.find(type);
    return it == m.end() ? none : it->second;
}

}  // namespace nodemenu
