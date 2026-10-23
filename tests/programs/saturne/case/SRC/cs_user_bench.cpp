#define TEST 0
#define OUTFILE "/home/user/heatBench/results/code_saturne_A.csv"
/* Conduction benchmark for code_saturne 9.0 (legacy FV, frozen velocity, temperature scalar).
   TEST: 0 = A (transient 1D sine decay), 1 = B (steady 2D Laplace). */
#include "base/cs_defs.h"
#include "cs_headers.h"
#include <cmath>
#include <cstdio>

#define L_   0.1
static const double T0 = 300., AMP = 100.;

void cs_user_mesh_cartesian_define(void)
{
#if TEST == 0
  int nxyz[3] = {50, 1, 1};  cs_real_t xyz[6] = {0., 0., 0., L_, 0.01, 0.01};
#else
  int nxyz[3] = {40, 40, 1}; cs_real_t xyz[6] = {0., 0., 0., L_, L_, 0.01};
#endif
  cs_mesh_cartesian_define_simple(nullptr, nxyz, xyz);
}

void cs_user_model(void)
{
  cs_thermal_model_t *tm = cs_get_glob_thermal_model();
  tm->thermal_variable = CS_THERMAL_MODEL_TEMPERATURE;
  tm->temperature_scale = CS_TEMPERATURE_SCALE_KELVIN;
  cs_get_glob_turb_model()->model = CS_TURB_NONE;   /* laminar: no turbulent diffusivity */
}

void cs_user_parameters(cs_domain_t *domain)
{
  cs_get_glob_time_scheme()->iccvfg = 1;              /* frozen (zero) velocity: pure conduction */
  cs_get_glob_time_step_options()->idtvar = CS_TIME_STEP_CONSTANT;
  cs_time_step_t *ts = cs_get_glob_time_step();
#if TEST == 0
  ts->nt_max = 250; ts->dt_ref = 1.0;
#else
  ts->nt_max = 100; ts->dt_ref = 1.0e4;                /* pseudo-time march to steady state */
#endif
  cs_fluid_properties_t *fp = cs_get_glob_fluid_properties();
  fp->ro0 = 8000.; fp->cp0 = 500.; fp->viscl0 = 1.e-3;
  cs_field_t *tf = cs_thermal_model_field();
  cs_field_set_key_double(tf, cs_field_key_id("diffusivity_ref"), 16.2);  /* lambda [W/m/K] */
  cs_equation_param_t *eqp = cs_field_get_equation_param(tf);
  eqp->epsilo = 1e-12; eqp->epsrsm = 1e-12;
}

void cs_user_initialization(cs_domain_t *domain)
{
  const cs_real_3_t *cc = domain->mesh_quantities->cell_cen;
  cs_real_t *T = cs_thermal_model_field()->val;
  for (cs_lnum_t c = 0; c < domain->mesh->n_cells; c++)
#if TEST == 0
    T[c] = T0 + AMP*sin(M_PI*cc[c][0]/L_);
#else
    T[c] = T0;
#endif
}

void cs_user_boundary_conditions(cs_domain_t *domain, int bc_type[])
{
  const cs_mesh_t *m = domain->mesh;
  const cs_real_3_t *cog = domain->mesh_quantities->b_face_cog;
  cs_field_t *tf = cs_thermal_model_field();
  for (cs_lnum_t f = 0; f < m->n_b_faces; f++) {
    bc_type[f] = CS_SMOOTHWALL;                         /* default: adiabatic wall */
    const double x = cog[f][0], y = cog[f][1], eps = 1e-9;
    bool dir = (x < eps || x > L_ - eps);
#if TEST == 1
    dir = dir || y < eps || y > L_ - eps;
#endif
    if (dir) {
      tf->bc_coeffs->icodcl[f] = 1;
      tf->bc_coeffs->rcodcl1[f] = (TEST == 1 && y > L_ - eps) ? T0 + AMP*sin(M_PI*x/L_) : T0;
    }
  }
}

void cs_user_extra_operations(cs_domain_t *domain)
{
  const cs_time_step_t *ts = cs_glob_time_step;
  if (ts->nt_cur != ts->nt_max) return;
  const cs_real_3_t *cc = domain->mesh_quantities->cell_cen;
  const cs_real_t *T = cs_thermal_model_field()->val;
  FILE *fp = fopen(OUTFILE, "w");
  fprintf(fp, "x,y,T\n");
  for (cs_lnum_t c = 0; c < domain->mesh->n_cells; c++)
    fprintf(fp, "%.12g,%.12g,%.12g\n", cc[c][0], cc[c][1], T[c]);
  fclose(fp);
  bft_printf("BENCH wrote %s at t=%g\n", OUTFILE, ts->t_cur);
}
