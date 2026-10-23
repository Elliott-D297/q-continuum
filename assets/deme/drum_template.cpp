// drum.cpp -- a DEME starter written by Q-Continuum: a part-filled drum turns about the x axis and the grains tumble
// (rolling, cascading or cataracting with the speed). Settings in ../params.ini; frames in output/particles_NNNN.csv.
#include "qc_deme.hpp"

using namespace deme;

int main(int argc, char** argv) {
    qc::Params P(argc, argv);
    double R = P.num("drum_radius", 0.05), L = P.num("drum_length", 0.04);
    double fillFrac = std::min(0.9, std::max(0.05, P.num("fill_fraction", 0.35)));
    double omega = P.num("rpm", 30) * 2 * PI / 60;

    DEMSolver sim;
    sim.SetVerbosity("ERROR");
    sim.SetOutputFormat("CSV");
    sim.SetOutputContent({"ABSV"});
    auto out = qc::out_dir(P);

    auto mat = qc::material(sim, P);
    auto sz = qc::sizes(sim, P, mat);
    sim.InstructBoxDomainDimension({-L, L}, {-1.5 * R, 1.5 * R}, {-1.5 * R, 1.5 * R});

    // the drum: its curved wall and two end plates, turning together (family 10)
    auto drum = sim.AddExternalObject();
    drum->AddCylinder(make_float3(0, 0, 0), make_float3(1, 0, 0), R, mat, ENTITY_NORMAL_INWARD);
    drum->AddPlane(make_float3(L / 2, 0, 0), make_float3(-1, 0, 0), mat);
    drum->AddPlane(make_float3(-L / 2, 0, 0), make_float3(1, 0, 0), mat);
    drum->SetFamily(10);
    sim.SetFamilyPrescribedAngVel(10, std::to_string(omega), "0", "0");

    // grains: the drum's inside up to the height that holds fill_fraction of its cross-section
    double lo = -R, hi = R;
    for (int it = 0; it < 60; it++) {                         // circular-segment area by bisection
        double z = (lo + hi) / 2, a = std::acos(-z / R);
        double frac = (a * R * R - (-z) * std::sqrt(std::max(0.0, R * R - z * z))) / (PI * R * R);
        (frac < fillFrac ? lo : hi) = z;
    }
    double zFill = lo, g = 2 * sz.rmax;
    auto smp = qc::sampler(sz);
    auto all = smp.SampleCylinderX(make_float3(0, 0, 0), (float)(R - g), (float)(L / 2 - g));
    std::vector<float3> xyz;
    for (auto& p : all) if (p.z < zFill) xyz.push_back(p);
    qc::Progress pr;
    pr.n = qc::place(sim, sz, xyz, (size_t)P.num("max_particles", 20000));
    std::cout << "Particles: " << pr.n << ", drum at " << P.num("rpm", 30) << " rpm" << std::endl;

    sim.SetTimeStepSize(P.num("dt", 2e-5));
    sim.SetMaxVelocity(std::max(P.num("max_velocity", 10), 2 * omega * R));
    sim.SetGravitationalAcceleration(make_float3(0, 0, -qc::gravity(P)));
    pr.attach(sim);
    sim.Initialize();

    qc::run(sim, P, pr, out, 0, [](double) {});
    qc::result("rim_speed", omega * R, "m/s");
    qc::result("froude", omega * omega * R / qc::gravity(P), "");
    sim.ShowTimingStats();
    std::cout << "End" << std::endl;
    return 0;
}
