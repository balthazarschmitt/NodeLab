#pragma once
#include <array>
#include <vector>

#include <nlohmann/json.hpp>

// Tone curve through control points, interpolated with monotone cubic splines (Fritsch-Carlson),
// so curves never overshoot between points.
using CurvePoints = std::vector<std::array<float, 2>>;

// Parses [[x,y],...]; falls back to the identity line. Result is sorted by x.
CurvePoints curveFromJson(const nlohmann::json& j);
nlohmann::json curveToJson(const CurvePoints& pts);
CurvePoints identityCurve();
bool isIdentityCurve(const CurvePoints& pts);

// Evaluate at x (points must be sorted). Outside the first/last point the curve is flat.
float evalCurve(const CurvePoints& pts, float x);

// Lookup table over x in [0,1] with n entries; sample with lutLookup.
std::vector<float> curveLut(const CurvePoints& pts, int n = 1024);
float lutLookup(const std::vector<float>& lut, float x);

// Default value for a Curve param: identity curves for master, r, g, b.
nlohmann::json defaultCurves();
