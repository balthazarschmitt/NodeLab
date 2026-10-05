// Lens profiles (lensfun's database): parsing, matching EXIF names, interpolation, and the Lens
// Profile node's corrections. Uses a small made-up database, never the downloaded one.
#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "io/LensProfiles.h"
#include "nodes/transform/TransformNodes.h"

using namespace lensdb;

namespace {

const char* kXml = R"(<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE lensdatabase SYSTEM "lensfun-database.dtd">
<lensdatabase version="1">
    <!-- cameras -->
    <camera>
        <maker>Canon</maker>
        <model>Canon EOS 80D</model>
        <model lang="en">EOS 80D</model>
        <mount>Canon EF-S</mount>
        <cropfactor>1.6</cropfactor>
    </camera>
    <camera>
        <maker>Nikon Corporation</maker>
        <maker lang="en">Nikon</maker>
        <model>Nikon D750</model>
        <mount>Nikon F AF</mount>
        <cropfactor>1.0</cropfactor>
    </camera>
    <camera>
        <maker>Olympus Imaging Corp.</maker>
        <model>XZ-1</model>
        <mount>olympusXZ1</mount>
        <cropfactor>4.7</cropfactor>
    </camera>
    <lens>
        <maker>Canon</maker>
        <model>Canon EF 24-105mm f/4L IS USM</model>
        <mount>Canon EF</mount>
        <mount>Canon EF-S</mount>
        <cropfactor>1</cropfactor>
        <calibration>
            <distortion model="ptlens" focal="24" a="0.01" b="-0.04" c="0"/>
            <distortion model="ptlens" focal="105" a="0.03" b="-0.02" c="0.01"/>
            <tca model="poly3" focal="24" br="0.0002" vr="1.0004" bb="-0.0001" vb="0.9996"/>
            <vignetting model="pa" focal="24" aperture="4" distance="10" k1="-0.6" k2="0.2" k3="-0.1"/>
            <vignetting model="pa" focal="24" aperture="4" distance="1" k1="-0.9" k2="0" k3="0"/>
            <vignetting model="pa" focal="24" aperture="8" distance="10" k1="-0.3" k2="0" k3="0"/>
        </calibration>
    </lens>
    <lens>
        <maker>Canon</maker>
        <model>Canon EF 24-70mm f/4L IS USM</model>
        <mount>Canon EF</mount>
        <cropfactor>1</cropfactor>
        <calibration>
            <distortion model="poly3" focal="24" k1="-0.02"/>
        </calibration>
    </lens>
    <lens>
        <maker>Sigma &amp; Co</maker>
        <model>18-35mm f/1.8 DC HSM [A]</model>
        <mount>Canon EF</mount>
        <cropfactor>1.534</cropfactor>
        <aspect-ratio>3:2</aspect-ratio>
        <calibration>
            <distortion model="poly5" focal="18" k1="-0.05" k2="0.01"/>
        </calibration>
    </lens>
    <lens>
        <maker>Olympus</maker>
        <model>fixed lens</model>
        <mount>olympusXZ1</mount>
        <cropfactor>4.7</cropfactor>
        <calibration>
            <distortion model="poly3" focal="6" k1="0.03"/>
        </calibration>
    </lens>
    <lens>
        <maker>Nobody</maker>
        <model>Uncalibrated 50mm f/2</model>
        <mount>Canon EF</mount>
    </lens>
</lensdatabase>
)";

Database fixture() {
    Database db;
    REQUIRE(db.loadXml(kXml));
    return db;
}

const Lens& lensNamed(const Database& db, const std::string& model) {
    for (const Lens& l : db.lenses())
        if (l.model == model) return l;
    FAIL("no lens " << model);
    throw 0;
}

}  // namespace

TEST_CASE("Lens profiles: the database is read") {
    const Database db = fixture();
    CHECK(db.cameras().size() == 3);
    CHECK(db.lenses().size() == 4);  // the uncalibrated lens is left out
    CHECK(db.cameras()[0].model == "Canon EOS 80D");  // not the translated name
    CHECK(db.cameras()[0].crop == doctest::Approx(1.6f));
    const Lens& l = lensNamed(db, "Canon EF 24-105mm f/4L IS USM");
    CHECK(l.mounts.size() == 2);
    CHECK(l.dist.size() == 2);
    CHECK(l.tca.size() == 1);
    CHECK(l.vig.size() == 3);
    CHECK(lensNamed(db, "18-35mm f/1.8 DC HSM [A]").maker == "Sigma & Co");

    Database bad;
    CHECK_FALSE(bad.loadXml(""));
    CHECK_FALSE(bad.loadXml("<notalensdb/>"));
    CHECK_FALSE(bad.loadXml("<lensdatabase><lens>"));
    CHECK(bad.lenses().empty());
}

TEST_CASE("Lens profiles: EXIF names find the camera and lens") {
    const Database db = fixture();
    const Camera* canon = db.findCamera("Canon", "Canon EOS 80D");
    REQUIRE(canon);
    CHECK(db.findCamera("NIKON CORPORATION", "NIKON D750") != nullptr);
    CHECK(db.findCamera("Canon", "EOS 80D") == canon);
    CHECK(db.findCamera("Nikon", "Canon EOS 80D") == nullptr);
    CHECK(db.findCamera("Canon", "") == nullptr);

    // Canon writes "EF24-105mm f/4L IS USM": the numbers pick the zoom, not the 24-70.
    auto m = db.findLenses("EF24-105mm f/4L IS USM", canon);
    REQUIRE(!m.empty());
    CHECK(m[0].lens->model == "Canon EF 24-105mm f/4L IS USM");
    for (const Match& x : m) CHECK(x.lens->model != "Canon EF 24-70mm f/4L IS USM");
    m = db.findLenses("18-35mm F1.8 DC HSM | Art 013", canon);
    REQUIRE(!m.empty());
    CHECK(m[0].lens->model == "18-35mm f/1.8 DC HSM [A]");
    CHECK(db.findLenses("EF50mm f/1.8 STM", canon).empty());
    // A compact camera writes no lens name: its fixed lens.
    m = db.findLenses("", db.findCamera("OLYMPUS IMAGING CORP.", "XZ-1"));
    REQUIRE(m.size() == 1);
    CHECK(m[0].lens->model == "fixed lens");
    CHECK(db.findLenses("", nullptr).empty());
}

TEST_CASE("Lens profiles: calibrations are interpolated and scaled for the camera") {
    const Database db = fixture();
    const Lens& l = lensNamed(db, "Canon EF 24-105mm f/4L IS USM");
    const Camera* canon = db.findCamera("Canon", "Canon EOS 80D");
    Profile p = resolve(l, canon, 64.5f, 4.0f);
    CHECK(p.distModel == Profile::PTLens);
    CHECK(p.dist[0] == doctest::Approx(0.02f));
    CHECK(p.dist[1] == doctest::Approx(-0.03f));
    CHECK(p.dist[2] == doctest::Approx(0.005f));
    CHECK(p.tcaModel == Profile::TcaPoly3);
    CHECK(p.tcaR[2] == doctest::Approx(1.0004f));
    // r = 1 is half the short edge of a full frame 3:2 sensor: an APS-C photo's half diagonal
    // reaches sqrt(1.5^2 + 1) / 1.6 of it.
    CHECK(p.distScale == doctest::Approx(std::sqrt(3.25f) / 1.6f));
    CHECK(p.vigScale == doctest::Approx(1.0f / 1.6f));
    CHECK(p.camera == "Canon EOS 80D");
    CHECK(p.lens == "Canon EF 24-105mm f/4L IS USM");

    // Vignetting at a measured setting: the far focus distance's.
    p = resolve(l, nullptr, 24.0f, 4.0f);
    CHECK(p.vigK[0] == doctest::Approx(-0.6f));
    CHECK(p.distScale == doctest::Approx(std::sqrt(3.25f)));
    // Between f/4 and f/8: between their falloffs.
    p = resolve(l, nullptr, 24.0f, 5.6f);
    CHECK(p.vigK[0] < -0.3f);
    CHECK(p.vigK[0] > -0.6f);
    // Outside the measured focal lengths: the nearest.
    p = resolve(l, nullptr, 200.0f, 0.0f);
    CHECK(p.dist[0] == doctest::Approx(0.03f));
    // Unknown focal length: the shortest.
    CHECK(resolve(l, nullptr, 0.0f, 0.0f).focal == 24.0f);
}

TEST_CASE("Lens profiles: the maths follows lensfun's models") {
    Profile p;
    p.distModel = Profile::Poly3, p.dist[0] = 0.1f;
    CHECK(distort(p, 1.0) == doctest::Approx(1.0));  // r = 1 stays put
    CHECK(distort(p, 2.0) == doctest::Approx(2.0 * (0.9 + 0.4)));
    p.distModel = Profile::PTLens, p.dist[0] = 0.01f, p.dist[1] = -0.02f, p.dist[2] = 0.03f;
    CHECK(distort(p, 1.0) == doctest::Approx(1.0));
    CHECK(distort(p, 0.5) == doctest::Approx(0.5 * (0.01 * 0.125 - 0.02 * 0.25 + 0.03 * 0.5 + 0.98)));
    p.tcaModel = Profile::TcaLinear, p.tcaR[0] = 1.001f, p.tcaB[0] = 0.999f;
    CHECK(tcaScale(p, 0, 0.7) == doctest::Approx(1.001));
    CHECK(tcaScale(p, 2, 0.7) == doctest::Approx(0.999));
    p.vig = true, p.vigK[0] = -0.5f;
    CHECK(vignetteGain(p, 1.0) == doctest::Approx(2.0));
    p.vigK[0] = -4.0f;  // nonsense stays finite
    CHECK(std::isfinite(vignetteGain(p, 1.0)));
}

TEST_CASE("Lens Profile: vignetting is undone in linear light") {
    LensProfileNode n;
    n.initParams();
    n.profile.vig = true;
    n.profile.vigK[0] = -0.4f, n.profile.vigK[1] = 0.05f;
    n.profile.vigScale = 0.8f;
    // A flat grey photographed through the lens: darker toward the corners.
    const int w = 60, h = 40;
    auto img = std::make_shared<Image>(w, h);
    const float half = std::hypot(w * 0.5f, h * 0.5f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double r = std::hypot(x + 0.5 - w * 0.5, y + 0.5 - h * 0.5) / half * 0.8, r2 = r * r;
            float* px = img->pixel(size_t(y) * w + x);
            px[0] = px[1] = px[2] = float(0.5 * (1 - 0.4 * r2 + 0.05 * r2 * r2));
            px[3] = 1;
        }
    EvalContext ctx;
    ctx.colorManagement.linear = true;
    std::vector<Value> out(1);
    n.evaluate(ctx, {Value(img)}, out);
    ImagePtr o = toImage(out[0], 0, 0);
    REQUIRE(o);
    for (size_t i = 0; i < o->pixelCount(); ++i) CHECK(o->pixel(i)[1] == doctest::Approx(0.5f).epsilon(1e-4));
    // Vignetting 0 leaves the photo alone; with nothing else to do the input passes through.
    n.params[LensProfileNode::Vignetting] = 0.0f;
    n.evaluate(ctx, {Value(img)}, out);
    CHECK(toImage(out[0], 0, 0) == img);
}

TEST_CASE("Lens Profile: distortion moves pixels and Constrain Crop hides the gaps") {
    LensProfileNode n;
    n.initParams();
    // Pincushion: correcting it reads beyond the photo's corners.
    n.profile.distModel = Profile::Poly3;
    n.profile.dist[0] = 0.05f;
    n.profile.distScale = std::sqrt(3.25f);
    // The source radius is the model's, scaled by Distortion's amount.
    const float r = 0.9f, ru = r * n.profile.distScale;
    CHECK(n.sampleAt(r).m[1] * ru == doctest::Approx(float(distort(n.profile, ru))));
    n.params[LensProfileNode::Distortion] = 50.0f;
    CHECK(n.sampleAt(r).m[1] * ru == doctest::Approx(0.5f * (ru + float(distort(n.profile, ru)))));
    n.params[LensProfileNode::Distortion] = 100.0f;

    auto img = std::make_shared<Image>(60, 40);
    for (float& v : img->px) v = 1.0f;
    EvalContext ctx;
    std::vector<Value> out(1);
    n.evaluate(ctx, {Value(img)}, out);
    ImagePtr o = toImage(out[0], 0, 0);
    REQUIRE(o);
    CHECK(o->pixel(0)[3] < 0.5f);
    CHECK(n.zoom(60, 40) == 1.0f);  // off
    n.params[LensProfileNode::Constrain] = true;
    CHECK(n.zoom(60, 40) < 1.0f);
    n.evaluate(ctx, {Value(img)}, out);
    o = toImage(out[0], 0, 0);
    for (int x = 0; x < o->w; ++x) {
        CHECK(o->pixel(size_t(x))[3] > 0.99f);
        CHECK(o->pixel(size_t(o->h - 1) * o->w + x)[3] > 0.99f);
    }
}

TEST_CASE("Lens Profile: the profile is saved, and damaged ones are tamed") {
    const Database db = fixture();
    LensProfileNode n;
    n.initParams();
    n.profile = resolve(lensNamed(db, "Canon EF 24-105mm f/4L IS USM"), nullptr, 50.0f, 5.6f);
    nlohmann::json j;
    n.saveExtra(j);
    LensProfileNode m;
    m.initParams();
    m.loadExtra(j);
    CHECK(m.signatureExtra() == n.signatureExtra());
    CHECK(m.profile.lens == n.profile.lens);
    CHECK_FALSE(m.signatureExtra().empty());

    m.loadExtra({{"profile", {{"distortion", {{"model", 9}, {"k", {1, 2, 3}}}},
                              {"tca", {{"model", 2}, {"red", {"x", 1e30, -1e30}}}},
                              {"vignetting", {{"k", {NAN, 3, 1}}}},
                              {"distScale", -4}, {"lens", 3}}}});
    CHECK(m.profile.distModel == Profile::DistNone);
    CHECK(m.profile.tcaModel == Profile::TcaPoly3);
    CHECK(m.profile.tcaR[1] == 2.0f);
    CHECK(m.profile.distScale > 0.0f);
    CHECK(m.profile.lens.empty());
    m.loadExtra({{"profile", "nonsense"}});
    CHECK_FALSE(m.profile.valid());
    m.loadExtra(nullptr);
    CHECK_FALSE(m.profile.valid());
}

TEST_CASE("Lens profiles: damaged database files don't crash") {
    const std::string xml = kXml;
    std::mt19937 rng(7);
    for (int run = 0; run < 400; ++run) {
        std::string s = xml;
        const int edits = 1 + int(rng() % 6);
        for (int e = 0; e < edits; ++e) {
            const size_t at = rng() % s.size();
            switch (rng() % 3) {
            case 0: s[at] = char(rng() % 256); break;
            case 1: s.erase(at, rng() % 40); break;
            default: s.insert(at, std::string(1, "<>&\"/='"[rng() % 7])); break;
            }
            if (s.empty()) s = "<";
        }
        Database db;
        db.loadXml(s);
        for (const Lens& l : db.lenses()) {
            const Profile p = resolve(l, nullptr, 35.0f, 4.0f);
            CHECK(std::isfinite(p.distScale));
            for (float k : p.vigK) CHECK(std::isfinite(k));
        }
        db.findLenses("EF24-105mm f/4L IS USM", db.findCamera("Canon", "Canon EOS 80D"));
    }
    // Deep nesting is refused rather than recursing without end.
    std::string deep = "<lensdatabase>";
    for (int i = 0; i < 100000; ++i) deep += "<a>";
    Database db;
    CHECK_FALSE(db.loadXml(deep));
}
