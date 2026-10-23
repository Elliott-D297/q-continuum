// impact.cpp -- a DEME starter written by Q-Continuum: a granular bed settles (lunar gravity by default), then a
// projectile strikes it at the chosen speed and angle and digs a crater. Settings in ../params.ini; frames in
// output/particles_NNNN.csv (the projectile is the large sphere); the penetration depth is reported at the end.
#include "qc_deme.hpp"

using namespace deme;

int main(int argc, char** argv) {
    qc::Params P(argc, argv);
    double w = P.num("box_width", 0.12), depth = P.num("bed_depth", 0.1), tSettle = P.num("settle_time", 0.6);
    double Rp = P.num("projectile_radius", 0.006), rhoP = P.num("projectile_density", 7800);
    double v = P.num("impact_speed", 10), ang = P.num("impact_angle", 0) * PI / 180;
    double fps = std::max(1.0, P.num("fps", 20));

    DEMSolver sim;
    sim.SetVerbosity("ERROR");
    sim.SetOutputFormat("CSV");
    sim.SetOutputContent({"ABSV"});
    auto out = qc::out_dir(P);

    auto mat = qc::material(sim, P);
    auto sz = qc::sizes(sim, P, mat);
    double H = depth + 0.5 * w + 4 * Rp;
    sim.InstructBoxDomainDimension({-w / 2, w / 2}, {-w / 2, w / 2}, {0, H});
    sim.InstructBoxDomainBoundingBC("top_open", mat);

    double g = 2 * sz.rmax;
    qc::Progress pr;
    pr.n = qc::fill_box(sim, sz, make_float3(0, 0, g + depth / 2), make_float3(w / 2 - g, w / 2 - g, depth / 2),
                        (size_t)P.num("max_particles", 40000));
    std::cout << "Bed particles: " << pr.n << std::endl;

    // the projectile waits high up, fixed and out of contact (family 2), until the bed has settled
    double mP = rhoP * 4.0 / 3.0 * PI * Rp * Rp * Rp;
    auto projT = sim.LoadSphereType(mP, Rp, mat);
    auto proj = sim.AddClumps(projT, make_float3(0, 0, H - 2 * Rp));
    proj->SetFamily(2);
    sim.SetFamilyFixed(2);
    sim.DisableContactBetweenFamilies(0, 2);
    auto tracker = sim.Track(proj);

    sim.SetTimeStepSize(P.num("dt", 2e-5));
    sim.SetMaxVelocity(std::max(P.num("max_velocity", 20), 1.5 * v));
    sim.SetGravitationalAcceleration(make_float3(0, 0, -qc::gravity(P)));
    pr.attach(sim);
    auto top = sim.CreateInspector("clump_max_z", "return Z < " + std::to_string(depth * 1.6 + g) + ";");
    sim.Initialize();
    pr.n += 1;

    // settle, writing frames as it goes
    int nSettle = (int)(tSettle * fps + 0.5), frame = 0;
    double dtf = 1.0 / fps, simTime = P.num("sim_time", 0.3);
    pr.end = tSettle + simTime;
    pr.frames = nSettle + (int)(simTime * fps + 0.5);
    for (int i = 0; i < nSettle; i++) {
        qc::write_frame(sim, out, frame);
        pr.report(i * dtf, frame++);
        sim.DoDynamics(dtf);
    }
    double bedTop = top->GetValue();
    qc::result("bed_top", bedTop, "m");

    // launch: just above the surface, moving at the impact speed and angle
    sim.ChangeFamily(2, 1);
    tracker->SetPos(make_float3(-std::sin(ang) * 2 * Rp, 0, bedTop + 1.5 * Rp));
    tracker->SetVel(make_float3(std::sin(ang) * v, 0, -std::cos(ang) * v));
    double zMin = 1e9;
    // the impact phase continues the frame numbering
    int nImp = (int)(simTime * fps + 0.5);
    for (int i = 0; i <= nImp; i++) {
        qc::write_frame(sim, out, frame);
        pr.report(tSettle + i * dtf, frame++);
        zMin = std::min(zMin, (double)tracker->Pos().z);
        if (i < nImp) sim.DoDynamics(dtf);
    }
    qc::result("penetration", bedTop - (zMin - Rp), "m");
    qc::result("impact_energy", 0.5 * mP * v * v, "J");
    sim.ShowTimingStats();
    std::cout << "End" << std::endl;
    return 0;
}
