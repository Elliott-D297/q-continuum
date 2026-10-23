// qc_deme.hpp -- helpers for the DEME drivers Q-Continuum writes (settle, drum, hopper, impact).
//
//  * Params: the run's settings from params.ini (the DEME page edits it; a change needs no rebuild). The driver looks
//    for --params <file>, then ../params.ini (it runs in <project>/run), then ./params.ini.
//  * Sizes: N sphere types spanning radius_min..radius_max, each sample point given one at random, equal numbers of
//    each size (a binned size distribution; radius_min == radius_max is one size).
//  * Progress: one line per frame, "QC t=<s> end=<s> frame=<i>/<n> ke=<J> vmax=<m/s> n=<particles>", which the
//    DEME page turns into a progress bar, an ETA and an energy chart.
#pragma once
#include <DEM/API.h>
#include <DEM/utils/Samplers.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace qc {

struct Params {
    std::map<std::string, std::string> kv;
    std::string path;

    Params(int argc, char** argv) {
        std::vector<std::string> tries;
        for (int i = 1; i + 1 < argc; i++)
            if (!std::strcmp(argv[i], "--params")) tries.push_back(argv[i + 1]);
        tries.push_back("../params.ini");
        tries.push_back("params.ini");
        for (auto& t : tries) {
            std::ifstream f(t);
            if (!f) continue;
            path = t;
            std::string line;
            while (std::getline(f, line)) {
                auto h = line.find('#');
                if (h != std::string::npos) line.erase(h);
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                kv[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            }
            break;
        }
        std::cout << "Parameters: " << (path.empty() ? "none found, using the defaults" : path) << std::endl;
    }
    double num(const std::string& k, double def) const {
        auto it = kv.find(k);
        if (it == kv.end() || it->second.empty()) return def;
        char* end = nullptr;
        double v = std::strtod(it->second.c_str(), &end);
        return end && end != it->second.c_str() ? v : def;
    }
    std::string str(const std::string& k, const std::string& def) const {
        auto it = kv.find(k);
        return it == kv.end() || it->second.empty() ? def : it->second;
    }
    // keep a copy next to the frames: which settings made this output
    void save_copy(const std::filesystem::path& dir) const {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::copy_file(path, dir / "params_used.ini", std::filesystem::copy_options::overwrite_existing, ec);
    }

  private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? "" : s.substr(a, b - a + 1);
    }
};

inline std::shared_ptr<deme::DEMMaterial> material(deme::DEMSolver& sim, const Params& P) {
    return sim.LoadMaterial({{"E", P.num("youngs_modulus", 1e8)},
                             {"nu", P.num("poisson_ratio", 0.3)},
                             {"CoR", P.num("restitution", 0.5)},
                             {"mu", P.num("friction", 0.5)},
                             {"Crr", P.num("rolling_friction", 0.0)}});
}

struct Sizes {
    std::vector<std::shared_ptr<deme::DEMClumpTemplate>> types;
    std::vector<double> radii;
    double rmin = 0, rmax = 0;
};

inline Sizes sizes(deme::DEMSolver& sim, const Params& P, const std::shared_ptr<deme::DEMMaterial>& mat) {
    Sizes s;
    s.rmin = P.num("radius_min", 0.002);
    s.rmax = std::max(s.rmin, P.num("radius_max", s.rmin));
    int bins = s.rmax > s.rmin ? std::max(1, (int)P.num("size_bins", 5)) : 1;
    double rho = P.num("density", 2500);
    for (int i = 0; i < bins; i++) {
        double r = bins == 1 ? s.rmin : s.rmin + (s.rmax - s.rmin) * i / (bins - 1);
        double m = rho * 4.0 / 3.0 * deme::PI * r * r * r;
        s.types.push_back(sim.LoadSphereType(m, r, mat));
        s.radii.push_back(r);
    }
    return s;
}

// give each point one of the sizes at random and add them all; max_count cuts the list first (0: no limit)
inline size_t place(deme::DEMSolver& sim, const Sizes& s, std::vector<float3> xyz, size_t max_count, unsigned seed = 42) {
    if (max_count && xyz.size() > max_count) xyz.resize(max_count);
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> pick(0, (int)s.types.size() - 1);
    std::vector<std::shared_ptr<deme::DEMClumpTemplate>> types(xyz.size());
    for (auto& t : types) t = s.types[pick(rng)];
    if (!xyz.empty()) sim.AddClumps(types, xyz);
    return xyz.size();
}

// sample points spaced so the largest spheres never overlap
inline deme::PDSampler sampler(const Sizes& s) { return deme::PDSampler(2.02 * s.rmax); }

// fill a box (centre c, half sizes h)
inline size_t fill_box(deme::DEMSolver& sim, const Sizes& s, float3 c, float3 h, size_t max_count) {
    auto smp = sampler(s);
    return place(sim, s, smp.SampleBox(c, h), max_count);
}

// a number the DEME page shows in its Results table: "QC result <name>=<value> <unit>"
inline void result(const char* name, double v, const char* unit) {
    std::printf("QC result %s=%.6g %s\n", name, v, unit);
    std::fflush(stdout);
}

inline double gravity(const Params& P) { return P.num("gravity", 9.81); }

// output/, the frames and a copy of the settings
inline std::filesystem::path out_dir(const Params& P) {
    auto out = std::filesystem::current_path() / P.str("output_dir", "output");
    std::filesystem::create_directories(out);
    P.save_copy(out);
    return out;
}

struct Progress {
    std::shared_ptr<deme::DEMInspector> ke, vmax;
    double end = 0;
    int frames = 0;
    size_t n = 0;
    void attach(deme::DEMSolver& sim) {
        ke = sim.CreateInspector("clump_kinetic_energy");
        vmax = sim.CreateInspector("clump_max_absv");
    }
    void report(double t, int frame) const {
        std::printf("QC t=%.6g end=%.6g frame=%d/%d ke=%.6g vmax=%.6g n=%zu\n", t, end, frame, frames,
                    ke ? (double)ke->GetValue() : 0.0, vmax ? (double)vmax->GetValue() : 0.0, n);
        std::fflush(stdout);
    }
};

inline void write_frame(deme::DEMSolver& sim, const std::filesystem::path& out, int i) {
    char name[64];
    std::snprintf(name, sizeof name, "particles_%04d.csv", i);
    sim.WriteSphereFile(out / name);
}

// the standard loop: frames at fps over sim_time, after the setup; stop_when returns true to end early
template <class F>
inline void run(deme::DEMSolver& sim, const Params& P, Progress& pr, const std::filesystem::path& out, double t0, F step_hook) {
    double sim_time = P.num("sim_time", 0.5), fps = std::max(1.0, P.num("fps", 20));
    int frames = (int)(sim_time * fps + 0.5);
    double dtf = 1.0 / fps;
    pr.end = t0 + sim_time;
    pr.frames = frames;
    for (int i = 0; i <= frames; i++) {
        write_frame(sim, out, i);
        pr.report(t0 + i * dtf, i);
        if (i == frames) break;
        step_hook(t0 + i * dtf);
        sim.DoDynamics(dtf);
    }
}

}  // namespace qc
