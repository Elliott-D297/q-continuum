// hopper.cpp -- a DEME starter written by Q-Continuum: grains settle in a conical hopper above a closed gate, then the
// gate opens and they drain onto the floor. Settings in ../params.ini; frames in output/particles_NNNN.csv; the mass
// that has left the hopper is reported each frame and its mean flow rate at the end.
#include "qc_deme.hpp"

using namespace deme;

int main(int argc, char** argv) {
    qc::Params P(argc, argv);
    double Rt = P.num("top_radius", 0.05), Ro = P.num("outlet_radius", 0.01);
    double ang = P.num("wall_angle", 30) * PI / 180, zo = P.num("outlet_height", 0.06), fill = P.num("fill_height", 0.12);
    double tOpen = P.num("gate_open_time", 0.3);
    double slope = std::tan(ang);                             // cone radius per unit height above its tip
    double zTip = zo - Ro / slope, zTop = zTip + Rt / slope;

    DEMSolver sim;
    sim.SetVerbosity("ERROR");
    sim.SetOutputFormat("CSV");
    sim.SetOutputContent({"ABSV"});
    auto out = qc::out_dir(P);

    auto mat = qc::material(sim, P);
    auto sz = qc::sizes(sim, P, mat);
    double H = std::max(zTop, zo + fill) + 4 * Rt;
    sim.InstructBoxDomainDimension({-2 * Rt, 2 * Rt}, {-2 * Rt, 2 * Rt}, {0, H});
    sim.InstructBoxDomainBoundingBC("top_open", mat);

    // the hopper: a cone from the outlet to the top radius, then a straight bin above it (family 10, fixed)
    auto hopper = sim.AddExternalObject();
    hopper->AddConeSegment(make_float3(0, 0, zTip), make_float3(0, 0, 1), slope, Ro / slope, Rt / slope, mat, ENTITY_NORMAL_INWARD);
    hopper->AddZCylinder(make_float3(0, 0, 0), Rt, mat, ENTITY_NORMAL_INWARD);
    hopper->SetFamily(10);
    sim.SetFamilyFixed(10);
    // the gate: a plate across the outlet (family 5), switched to family 6 (no contact with the grains) to open it
    auto gate = sim.AddExternalObject();
    gate->AddPlane(make_float3(0, 0, zo), make_float3(0, 0, 1), mat);
    gate->SetFamily(5);
    sim.SetFamilyFixed(5);
    sim.SetFamilyFixed(6);
    sim.DisableContactBetweenFamilies(0, 6);
    // the bin wall stops at the top of the cone: grains that spill past the outlet fall freely inside it, which the
    // infinite cylinder allows since the outlet is narrower than the bin

    // grains: the cone and the bin above it, up to fill_height above the outlet, inside the walls
    double g = 2 * sz.rmax;
    auto smp = qc::sampler(sz);
    auto all = smp.SampleCylinderZ(make_float3(0, 0, zo + fill / 2), (float)(Rt - g), (float)(fill / 2));
    std::vector<float3> xyz;
    for (auto& p : all) {
        double r = std::sqrt(p.x * p.x + p.y * p.y), rWall = p.z < zTop ? (p.z - zTip) * slope : Rt;
        if (p.z > zo + g && r < rWall - g * std::sqrt(1 + slope * slope)) xyz.push_back(p);
    }
    qc::Progress pr;
    pr.n = qc::place(sim, sz, xyz, (size_t)P.num("max_particles", 20000));
    std::cout << "Particles: " << pr.n << std::endl;

    sim.SetTimeStepSize(P.num("dt", 2e-5));
    sim.SetMaxVelocity(P.num("max_velocity", 10));
    sim.SetGravitationalAcceleration(make_float3(0, 0, -qc::gravity(P)));
    pr.attach(sim);
    auto total = sim.CreateInspector("clump_mass");
    auto below = sim.CreateInspector("clump_mass", "return Z < " + std::to_string(zo - 2 * sz.rmax) + ";");
    sim.Initialize();
    double m0 = total->GetValue();

    bool open = false;
    double tFirst = -1, mAtEnd = 0;
    qc::run(sim, P, pr, out, 0, [&](double t) {
        if (!open && t >= tOpen) { sim.ChangeFamily(5, 6); open = true; std::cout << "Gate open at t = " << t << " s" << std::endl; }
        double m = below->GetValue();
        if (open && tFirst < 0 && m > 0) tFirst = t;
        std::printf("QC discharged=%.6g kg of %.6g\n", m, m0);
        mAtEnd = m;
    });
    double tEnd = pr.end;
    if (tFirst > 0 && tEnd > tFirst) qc::result("mean_flow_rate", mAtEnd / (tEnd - tFirst), "kg/s");
    qc::result("discharged_fraction", m0 > 0 ? mAtEnd / m0 : 0, "");
    sim.ShowTimingStats();
    std::cout << "End" << std::endl;
    return 0;
}
