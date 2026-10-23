// settle.cpp -- a DEME starter written by Q-Continuum: spheres poured into an open box settle under gravity.
// Its settings are in ../params.ini (the DEME page's Setup form): change them and run again, no rebuild. Rebuild only
// after editing this file. Every frame goes to output/particles_NNNN.csv (X,Y,Z,r,absv).
#include "qc_deme.hpp"

using namespace deme;

int main(int argc, char** argv) {
    qc::Params P(argc, argv);
    double w = P.num("box_width", 0.1), fill = P.num("fill_height", 0.08);

    DEMSolver sim;
    sim.SetVerbosity("ERROR");
    sim.SetOutputFormat("CSV");
    sim.SetOutputContent({"ABSV"});
    auto out = qc::out_dir(P);

    auto mat = qc::material(sim, P);
    auto sz = qc::sizes(sim, P, mat);
    double h = std::max(3 * w, fill + w);
    sim.InstructBoxDomainDimension({-w / 2, w / 2}, {-w / 2, w / 2}, {0, h});
    sim.InstructBoxDomainBoundingBC("top_open", mat);

    double g = 2 * sz.rmax;                                   // keep clear of the walls and the floor
    qc::Progress pr;
    pr.n = qc::fill_box(sim, sz, make_float3(0, 0, g + fill / 2), make_float3(w / 2 - g, w / 2 - g, fill / 2),
                        (size_t)P.num("max_particles", 20000));
    std::cout << "Particles: " << pr.n << std::endl;

    sim.SetTimeStepSize(P.num("dt", 2e-5));
    sim.SetMaxVelocity(P.num("max_velocity", 10));
    sim.SetGravitationalAcceleration(make_float3(0, 0, -qc::gravity(P)));
    pr.attach(sim);
    auto top = sim.CreateInspector("clump_max_z");
    sim.Initialize();

    qc::run(sim, P, pr, out, 0, [](double) {});
    qc::result("bed_top", top->GetValue(), "m");
    sim.ShowTimingStats();
    std::cout << "End" << std::endl;
    return 0;
}
