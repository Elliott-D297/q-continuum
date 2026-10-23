# Test A: transient 1D sine decay in SS304 slab (QUAD4, implicit Euler, dt = 1 s)
[Mesh]
  [gen]
    type = GeneratedMeshGenerator
    dim = 2
    nx = 50
    ny = 1
    xmax = 0.1
    ymax = 0.01
  []
[]
[Variables]
  [T]
  []
[]
[Functions]
  [T_init]
    type = ParsedFunction
    expression = '300 + 100*sin(pi*x/0.1)'
  []
[]
[ICs]
  [T_ic]
    type = FunctionIC
    variable = T
    function = T_init
  []
[]
[Kernels]
  [cond]
    type = HeatConduction
    variable = T
  []
  [ddt]
    type = HeatConductionTimeDerivative
    variable = T
  []
[]
[Materials]
  [ss304]
    type = HeatConductionMaterial
    thermal_conductivity = 16.2
    specific_heat = 500
  []
  [rho]
    type = GenericConstantMaterial
    prop_names = density
    prop_values = 8000
  []
[]
[BCs]
  [ends]
    type = DirichletBC
    variable = T
    boundary = 'left right'
    value = 300
  []
[]
[Executioner]
  type = Transient
  scheme = implicit-euler
  dt = 1
  end_time = 250
  solve_type = NEWTON
  petsc_options_iname = '-pc_type -pc_factor_mat_solver_type'
  petsc_options_value = 'lu mumps'
  nl_abs_tol = 1e-10
  nl_rel_tol = 1e-12
[]
[VectorPostprocessors]
  [nodes]
    type = NodalValueSampler
    variable = T
    sort_by = id
    execute_on = final
  []
[]
[Outputs]
  [csv]
    type = CSV
    execute_on = final
    file_base = A_out
  []
[]
