// body.cpp -- your own part (or a block) dropped onto the ground, Chrono with smooth (SMC) contact, written by
// Q-Continuum from New case > Chrono > Blank case. Runs in <project>/run: reads ../params.ini (change it and run again,
// no rebuild), writes body.csv, prints "QC t=... end=... ke=..." lines for the progress bar and chart and
// "QC result name=value unit" lines for the Results table. The part is geometry/part.obj, already in metres, centred
// on x and y and standing on z = 0 (Q-Continuum wrote it from your file); without one it drops a box_x by box_y by
// box_z block. Edit this file to add more bodies, joints or motors: the next run rebuilds it.
#include "chrono/physics/ChSystemSMC.h"
#include "chrono/physics/ChBodyEasy.h"
#include "chrono/physics/ChContactMaterialSMC.h"
#include "chrono/geometry/ChTriangleMeshConnected.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace chrono;

static std::map<std::string, std::string> read_params(const std::string& path) {
    std::map<std::string, std::string> p;
    std::ifstream f(path);
    std::string line;
    auto trim = [](std::string s) { s.erase(0, s.find_first_not_of(" \t\r")); s.erase(s.find_last_not_of(" \t\r") + 1); return s; };
    while (std::getline(f, line)) {
        line = line.substr(0, line.find('#'));
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        p[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return p;
}

int main(int argc, char** argv) {
    std::string ppath = argc > 2 && std::string(argv[1]) == "--params" ? argv[2] : "../params.ini";
    auto P = read_params(ppath);
    auto num = [&](const char* k, double d) { auto it = P.find(k); return it == P.end() || it->second.empty() ? d : std::atof(it->second.c_str()); };
    const double T = num("sim_time", 2), dt = num("dt", 1e-4), g = num("gravity", 9.81), rho = num("density", 7800);
    const double h = num("drop_height", 0.1), tilt = num("tilt", 15) * CH_PI / 180;
    std::string mesh = P.count("mesh") ? P["mesh"] : "";
    if (!mesh.empty() && std::filesystem::path(mesh).is_relative())     // relative to params.ini's folder
        mesh = (std::filesystem::path(ppath).parent_path() / mesh).string();

    ChSystemSMC sys;
    sys.SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
    sys.SetGravitationalAcceleration(ChVector3d(0, 0, -g));
    auto mat = chrono_types::make_shared<ChContactMaterialSMC>();
    mat->SetYoungModulus((float)num("youngs_modulus", 1e7));
    mat->SetPoissonRatio((float)num("poisson_ratio", 0.3));
    mat->SetRestitution((float)num("restitution", 0.4));
    mat->SetFriction((float)num("friction", 0.5));

    // the part in its own frame, and the points that can touch the ground first
    ChQuaterniond q = QuatFromAngleX(tilt);
    std::vector<ChVector3d> pts;
    std::shared_ptr<ChBody> body;
    double size = 0;
    if (!mesh.empty() && std::filesystem::exists(mesh)) {
        auto tm = ChTriangleMeshConnected::CreateFromWavefrontFile(mesh, false, true);
        if (!tm || tm->GetNumTriangles() == 0) throw std::runtime_error("Could not read the part " + mesh);
        ChAABB bb = tm->GetBoundingBox();
        size = (bb.max - bb.min).Length();
        auto b = chrono_types::make_shared<ChBodyEasyMesh>(tm, rho, true, false, true, mat, 0.002 * size);
        if (!(b->GetMass() > 0))
            throw std::runtime_error("The part's mass came out " + std::to_string(b->GetMass()) +
                                     " kg: it needs to be a closed surface (a watertight STL) for its volume");
        pts = tm->GetCoordsVertices();
        double zmin = 1e300;
        for (auto& p : pts) zmin = std::min(zmin, q.Rotate(p).z());
        b->SetFrameRefToAbs(ChFramed(ChVector3d(0, 0, h - zmin), q));
        body = b;
        std::printf("Part: %s, %zu triangles\n", mesh.c_str(), (size_t)tm->GetNumTriangles());
    } else {
        if (!mesh.empty()) std::printf("No part at %s: dropping the block instead\n", mesh.c_str());
        double bx = num("box_x", 0.1), by = num("box_y", 0.05), bz = num("box_z", 0.02);
        size = std::sqrt(bx * bx + by * by + bz * bz);
        auto b = chrono_types::make_shared<ChBodyEasyBox>(bx, by, bz, rho, false, true, mat);
        double zmin = 1e300;
        for (int c = 0; c < 8; c++) zmin = std::min(zmin, q.Rotate(ChVector3d(c & 1 ? bx / 2 : -bx / 2, c & 2 ? by / 2 : -by / 2, c & 4 ? bz / 2 : -bz / 2)).z());
        b->SetRot(q);
        b->SetPos(ChVector3d(0, 0, h - zmin));
        body = b;
    }
    sys.AddBody(body);

    // the ground: a wide slab with its top at z = 0
    const double L = 20 * size;
    auto ground = chrono_types::make_shared<ChBodyEasyBox>(L, L, 0.1 * L, 1000, false, true, mat);
    ground->SetPos(ChVector3d(0, 0, -0.05 * L));
    ground->SetFixed(true);
    sys.AddBody(ground);

    const double m = body->GetMass(), z0 = body->GetPos().z();
    std::printf("Mass %.6g kg, centre of mass starts at z = %.6g m\n", m, z0);
    std::ofstream csv("body.csv");
    csv << "t,x,y,z,vx,vy,vz,speed,ke,pe\n";
    int steps = (int)std::lround(T / dt), every = std::max(1, steps / 200);
    double vmax = 0;
    for (int i = 0; i <= steps; i++) {
        double t = sys.GetChTime();
        ChVector3d p = body->GetPos(), v = body->GetPosDt();
        double ke = 0.5 * m * v.Length2() + 0.5 * body->GetAngVelLocal().Dot(body->GetInertia() * body->GetAngVelLocal());
        double pe = m * g * p.z();
        vmax = std::max(vmax, v.Length());
        if (i % every == 0) {
            csv << t << "," << p.x() << "," << p.y() << "," << p.z() << "," << v.x() << "," << v.y() << "," << v.z() << ","
                << v.Length() << "," << ke << "," << pe << "\n";
            std::printf("QC t=%.6g end=%.6g ke=%.6g\n", t, T, ke);
            std::fflush(stdout);
        }
        if (i < steps) sys.DoStepDynamics(dt);
    }
    // falling from rest, the lowest point drops drop_height before it touches: v = sqrt(2 g h) then
    std::printf("QC result mass=%.6g kg\n", m);
    std::printf("QC result impact_speed=%.6g m/s\n", vmax);
    std::printf("QC result impact_speed_theory=%.6g m/s\n", std::sqrt(2 * g * h));
    std::printf("QC result rest_height=%.6g m\n", body->GetPos().z());
    std::printf("QC result final_speed=%.6g m/s\n", body->GetPosDt().Length());
    return 0;
}
