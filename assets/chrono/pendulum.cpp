// pendulum.cpp -- a rigid rod swinging on a hinge (Chrono), written by Q-Continuum.
// Runs in <project>/run: reads ../params.ini, writes pendulum.csv, prints "QC t=... end=... ke=..." lines for the
// progress bar and chart, and "QC result name=value unit" lines for the Results table.
#include "chrono/physics/ChSystemNSC.h"
#include "chrono/physics/ChBodyEasy.h"
#include "chrono/physics/ChLinkLock.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

using namespace chrono;

static std::map<std::string, double> read_params(const char* path) {
    std::map<std::string, double> p;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        k.erase(0, k.find_first_not_of(" \t")); k.erase(k.find_last_not_of(" \t") + 1);
        p[k] = std::atof(line.c_str() + eq + 1);
    }
    return p;
}

int main(int argc, char** argv) {
    auto P = read_params(argc > 2 && std::string(argv[1]) == "--params" ? argv[2] : "../params.ini");
    auto num = [&](const char* k, double d) { auto it = P.find(k); return it == P.end() ? d : it->second; };
    const double T = num("sim_time", 10), dt = num("dt", 1e-3), L = num("length", 1.0), m = num("mass", 1.0);
    const double th0 = num("angle", 30) * CH_PI / 180, g = num("gravity", 9.81), c = num("damping", 0.0);

    ChSystemNSC sys;
    sys.SetGravitationalAcceleration(ChVector3d(0, 0, -g));

    auto ground = chrono_types::make_shared<ChBody>();
    ground->SetFixed(true);
    sys.AddBody(ground);

    // a thin rod along its own -z, hinged at its top end (the origin), turned by th0 about y
    const double w = 0.02 * L;
    auto rod = chrono_types::make_shared<ChBodyEasyBox>(w, w, L, m / (w * w * L), false, false);
    ChQuaterniond q = QuatFromAngleY(-th0);
    rod->SetRot(q);
    rod->SetPos(q.Rotate(ChVector3d(0, 0, -L / 2)));
    sys.AddBody(rod);
    const unsigned acc = rod->AddAccumulator();                       // the hinge damping torque

    auto hinge = chrono_types::make_shared<ChLinkLockRevolute>();     // revolute about z of the frame: turn it onto y
    hinge->Initialize(ground, rod, ChFramed(ChVector3d(0, 0, 0), QuatFromAngleX(CH_PI_2)));
    sys.AddLink(hinge);

    std::ofstream csv("pendulum.csv");
    csv << "t,angle_deg,omega,ke,pe\n";
    double lastCross = -1, firstCross = -1, prevAng = th0, maxErr = 0;
    int crossings = 0, steps = (int)std::lround(T / dt), every = std::max(1, steps / 200);
    const double E0 = m * g * (L / 2) * (1 - std::cos(th0));
    for (int i = 0; i <= steps; i++) {
        double t = sys.GetChTime();
        ChVector3d tip = rod->GetPos();                               // centre of mass
        double ang = std::atan2(tip.x(), -tip.z());
        double om = rod->GetAngVelParent().y();
        double ke = 0.5 * (m * L * L / 3) * om * om, pe = m * g * (L / 2 + tip.z());
        if (c == 0) maxErr = std::max(maxErr, std::abs(ke + pe - E0) / E0);
        if (prevAng < 0 && ang >= 0) {                                // a downward pass through the bottom, one per period
            if (firstCross < 0) firstCross = t; else crossings++;
            lastCross = t;
        }
        prevAng = ang;
        if (i % every == 0) {
            csv << t << "," << ang * 180 / CH_PI << "," << om << "," << ke << "," << pe << "\n";
            std::printf("QC t=%.6g end=%.6g ke=%.6g\n", t, T, ke);
            std::fflush(stdout);
        }
        if (i < steps) {
            rod->EmptyAccumulator(acc);
            if (c > 0) rod->AccumulateTorque(acc, ChVector3d(0, -c * om, 0), false);
            sys.DoStepDynamics(dt);
        }
    }
    // a rod about its end: T0 = 2 pi sqrt(2L / 3g), and the large-angle factor 1 + th0^2/16
    const double T0 = 2 * CH_PI * std::sqrt(2 * L / (3 * g)), Tth = T0 * (1 + th0 * th0 / 16);
    if (crossings > 0) {
        double Tsim = (lastCross - firstCross) / crossings;
        std::printf("QC result period=%.6g s\n", Tsim);
        std::printf("QC result period_theory=%.6g s\n", Tth);
        std::printf("QC result period_error=%.3g %%\n", 100 * (Tsim - Tth) / Tth);
    }
    if (c == 0) std::printf("QC result energy_drift=%.3g %%\n", 100 * maxErr);
    return 0;
}
