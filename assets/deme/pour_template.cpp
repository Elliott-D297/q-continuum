// pour.cpp -- a DEME project written by Q-Continuum from New case > DEME > Blank case: grains poured onto your own part
// (geometry/part.obj: in metres, centred on x and y, standing on the floor) inside an open-topped box, or into the box
// alone when there is no part. Settings in ../params.ini (the DEME page's Setup form: no rebuild); frames in
// output/particles_NNNN.csv (X,Y,Z,r,absv). Edit this file to move the part, add more, or change what is reported.
#include "qc_deme.hpp"

using namespace deme;

int main(int argc, char** argv) {
    qc::Params P(argc, argv);
    double X = P.num("box_x", 0.2), Y = P.num("box_y", 0.2), H = P.num("box_height", 0.3);
    double fz = P.num("fill_bottom", 0.1), fh = P.num("fill_height", 0.05);
    std::string mesh = P.str("mesh", "");
    if (!mesh.empty() && std::filesystem::path(mesh).is_relative() && !P.path.empty())   // relative to params.ini
        mesh = (std::filesystem::path(P.path).parent_path() / mesh).string();

    DEMSolver sim;
    sim.SetVerbosity("ERROR");
    sim.SetOutputFormat("CSV");
    sim.SetOutputContent({"ABSV"});
    auto out = qc::out_dir(P);

    auto mat = qc::material(sim, P);
    auto sz = qc::sizes(sim, P, mat);
    double top = std::max(H, fz + fh + 4 * sz.rmax);
    sim.InstructBoxDomainDimension({-X / 2, X / 2}, {-Y / 2, Y / 2}, {0, top});
    sim.InstructBoxDomainBoundingBC("top_open", mat);

    if (!mesh.empty() && std::filesystem::exists(mesh)) {     // the part: a fixed wall (family 10)
        auto part = sim.AddWavefrontMeshObject(mesh, mat);
        part->SetFamily(10);
        sim.SetFamilyFixed(10);
        std::cout << "Part: " << mesh << std::endl;
    } else if (!mesh.empty()) std::cout << "No part at " << mesh << ": grains into the empty box" << std::endl;

    double g = 2 * sz.rmax;                                   // keep clear of the walls
    qc::Progress pr;
    pr.n = qc::fill_box(sim, sz, make_float3(0, 0, fz + fh / 2), make_float3(X / 2 - g, Y / 2 - g, fh / 2),
                        (size_t)P.num("max_particles", 20000));
    std::cout << "Particles: " << pr.n << std::endl;

    sim.SetTimeStepSize(P.num("dt", 2e-5));
    sim.SetMaxVelocity(P.num("max_velocity", 10));
    sim.SetGravitationalAcceleration(make_float3(0, 0, -qc::gravity(P)));
    pr.attach(sim);
    auto bedTop = sim.CreateInspector("clump_max_z");
    sim.Initialize();

    qc::run(sim, P, pr, out, 0, [](double) {});
    qc::result("bed_top", bedTop->GetValue(), "m");
    qc::result("final_kinetic_energy", pr.ke->GetValue(), "J");
    sim.ShowTimingStats();
    std::cout << "End" << std::endl;
    return 0;
}
