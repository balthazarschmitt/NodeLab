#pragma once
// Lens profiles from the lensfun database (https://lensfun.github.io, CC BY-SA 3.0), like
// Lightroom's "Enable Profile Corrections": distortion, lateral chromatic aberration (TCA) and
// vignetting measured per lens and focal length. The database isn't part of Refractory.exe; it is
// downloaded on request into %APPDATA%\Refractory\lensfun, as the AI models are.
//
// A Lens Profile node stores the corrections resolved for its photo (Profile), so projects
// render the same on machines without the database and headless renders don't need it.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lensdb {

// The corrections for one photo: a lens calibration interpolated at its focal length and
// aperture, scaled for the camera's crop factor.
struct Profile {
    std::string lens, camera;  // shown in the Inspector
    float focal = 0, aperture = 0;
    enum Dist { DistNone = 0, Poly3, Poly5, PTLens };
    int distModel = DistNone;
    float dist[3] = {};  // poly3: k1; poly5: k1 k2; ptlens: a b c
    enum Tca { TcaNone = 0, TcaLinear, TcaPoly3 };
    int tcaModel = TcaNone;
    float tcaR[3] = {}, tcaB[3] = {};  // linear: k; poly3: b c v (Rd = Ru (b Ru^2 + c Ru + v))
    bool vig = false;
    float vigK[3] = {};  // pa: Cd = Cs (1 + k1 r^2 + k2 r^4 + k3 r^6)
    // Lensfun's radii for an image radius of 1 at its half diagonal: distortion and TCA use half
    // the short edge of the calibration sensor, vignetting its half diagonal, both scaled by the
    // crop factors (a crop camera sees the middle of the lens' image circle).
    float distScale = 1, vigScale = 1;

    bool valid() const { return distModel != DistNone || tcaModel != TcaNone || vig; }
    nlohmann::json toJson() const;
    static Profile fromJson(const nlohmann::json& j);  // damaged values are dropped
    std::string signature() const;
};

// Where a point of the corrected image (radius ru, in distortion units) is in the photo.
double distort(const Profile& p, double ru);
// The radius scale of the red (c = 0) or blue (c = 2) channel at a distorted radius rd.
double tcaScale(const Profile& p, int c, double rd);
// The gain undoing the vignetting at radius r (vignetting units).
double vignetteGain(const Profile& p, double r);

// One measurement from the database.
struct Calib {
    float focal = 0, aperture = 0, distance = 0;
    int model = 0;   // Profile::Dist or Profile::Tca; unused for vignetting
    float k[6] = {};  // distortion k1 k2 / a b c; TCA red then blue; vignetting k1 k2 k3
    float crop = 1, aspect = 1.5f;  // the calibration's sensor
};

struct Lens {
    std::string maker, model;
    std::vector<std::string> mounts;
    float crop = 1, aspect = 1.5f;
    bool fisheye = false;
    std::vector<Calib> dist, tca, vig;
};

struct Camera {
    std::string maker, model, mount;
    float crop = 1;
};

struct Match {
    const Lens* lens = nullptr;
    double score = 0;
};

class Database {
public:
    // Adds the lenses and cameras of one lensfun XML file. False when it isn't one.
    bool loadXml(const std::string& xml);
    // Every .xml file in a folder; the number of files read.
    int loadFolder(const std::string& dirU8);

    const std::vector<Lens>& lenses() const { return lenses_; }
    const std::vector<Camera>& cameras() const { return cameras_; }

    // The camera with this EXIF make and model, or null.
    const Camera* findCamera(const std::string& make, const std::string& model) const;
    // Lenses matching an EXIF lens name, best first. With no name (compact cameras), the lenses
    // of the camera's fixed mount.
    std::vector<Match> findLenses(const std::string& lensName, const Camera* camera) const;

private:
    std::vector<Lens> lenses_;
    std::vector<Camera> cameras_;
};

// The lens' calibration at a focal length and aperture (0 = unknown), for a camera (null: the
// lens' calibration crop factor).
Profile resolve(const Lens& lens, const Camera* camera, float focal, float aperture);

// ---- The downloaded database

// %APPDATA%\Refractory\lensfun, created on demand. Tests point it elsewhere.
std::string folder();
void setFolder(const std::string& dirU8);
bool installed();
// The database in the folder, loaded on first use and again after a download.
std::shared_ptr<const Database> shared();

struct DownloadState {
    bool running = false;
    int done = 0, total = 0;  // files
    std::string error;
};
void startDownload();
void cancelDownload();
DownloadState downloadState();

}  // namespace lensdb
