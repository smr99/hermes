#include "hermes2d.h"

// This test makes sure that example 08-system works correctly.
// CAUTION: This test will fail when any changes to the shapeset
// are made, but it is easy to fix (see below).

// problem constants
const double E  = 200e9;                                   // Young modulus (steel)
const double nu = 0.3;                                     // Poisson ratio
const double f_0  = 0;                                     // external force in x-direction
const double f_1  = 1e4;                                   // external force in y-direction
const double lambda = (E * nu) / ((1 + nu) * (1 - 2*nu));  // first Lame constant
const double mu = E / (2*(1 + nu));                        // second Lame constant
const int P_INIT = 8;                                      // Initial polynomial degree of all elements.
MatrixSolverType matrix_solver = SOLVER_UMFPACK;           // Possibilities: SOLVER_UMFPACK, SOLVER_PETSC,
                                                           // SOLVER_MUMPS, and more are coming.

// Boundary marker (external force).
const int GAMMA_3_BDY = 3;

// Boundary condition types.
BCType bc_types(int marker)
  { return (marker == 1) ? BC_ESSENTIAL : BC_NATURAL; }

// Function values for Dirichlet boundary conditions.
scalar essential_bc_values(int ess_bdy_marker, double x, double y)
  { return 0; }

// Bilinear forms.
template<typename Real, typename Scalar>
Scalar bilinear_form_0_0(int n, double *wt, Func<Scalar> *u_ext[], Func<Real> *u, Func<Real> *v,
                         Geom<Real> *e, ExtData<Scalar> *ext)
{
  return (lambda + 2*mu) * int_dudx_dvdx<Real, Scalar>(n, wt, u, v) +
                      mu * int_dudy_dvdy<Real, Scalar>(n, wt, u, v);
}

template<typename Real, typename Scalar>
Scalar bilinear_form_0_1(int n, double *wt, Func<Scalar> *u_ext[], Func<Real> *u, Func<Real> *v,
                         Geom<Real> *e, ExtData<Scalar> *ext)
{
  return lambda * int_dudy_dvdx<Real, Scalar>(n, wt, u, v) +
             mu * int_dudx_dvdy<Real, Scalar>(n, wt, u, v);
}

template<typename Real, typename Scalar>
Scalar bilinear_form_1_1(int n, double *wt, Func<Scalar> *u_ext[], Func<Real> *u, Func<Real> *v,
                         Geom<Real> *e, ExtData<Scalar> *ext)
{
  return              mu * int_dudx_dvdx<Real, Scalar>(n, wt, u, v) +
         (lambda + 2*mu) * int_dudy_dvdy<Real, Scalar>(n, wt, u, v);
}

// Linear forms.
template<typename Real, typename Scalar>
Scalar linear_form_surf_0(int n, double *wt, Func<Scalar> *u_ext[], Func<Real> *v, Geom<Real> *e,
                          ExtData<Scalar> *ext)
{
  return f_0 * int_v<Real, Scalar>(n, wt, v);
}

template<typename Real, typename Scalar>
Scalar linear_form_surf_1(int n, double *wt, Func<Scalar> *u_ext[], Func<Real> *v, Geom<Real> *e,
                          ExtData<Scalar> *ext)
{
  return f_1 * int_v<Real, Scalar>(n, wt, v);
}

int main(int argc, char* argv[])
{
  // Load the mesh file.
  Mesh mesh;
  H2DReader mloader;
  mloader.load("sample.mesh", &mesh);


  // Create x- and y- displacement space using the default H1 shapeset.
  H1Space u_space(&mesh, bc_types, essential_bc_values, P_INIT);
  H1Space v_space(&mesh, bc_types, essential_bc_values, P_INIT);
  info("ndof = %d.", get_num_dofs(Tuple<Space *>(&u_space, &v_space)));

  // Initialize the weak formulation.
  WeakForm wf(2);
  wf.add_matrix_form(0, 0, callback(bilinear_form_0_0), HERMES_SYM);  // Note that only one symmetric part is
  wf.add_matrix_form(0, 1, callback(bilinear_form_0_1), HERMES_SYM);  // added in the case of symmetric bilinear
  wf.add_matrix_form(1, 1, callback(bilinear_form_1_1), HERMES_SYM);  // forms.
  wf.add_vector_form_surf(0, callback(linear_form_surf_0), GAMMA_3_BDY);
  wf.add_vector_form_surf(1, callback(linear_form_surf_1), GAMMA_3_BDY);

  // Testing n_dof and correctness of solution vector
  // for p_init = 1, 2, ..., 10
  int success = 1;
  Solution xsln, ysln;
  for (int p_init = 1; p_init <= 10; p_init++) {
    printf("********* p_init = %d *********\n", p_init);
    u_space.set_uniform_order(p_init);
    v_space.set_uniform_order(p_init);

    // Initialize the FE problem.
    bool is_linear = true;
    FeProblem fep(&wf, Tuple<Space *>(&u_space, &v_space), is_linear);

    // Set up the solver, matrix, and rhs according to the solver selection.
    SparseMatrix* matrix = create_matrix(matrix_solver);
    Vector* rhs = create_vector(matrix_solver);
    Solver* solver = create_linear_solver(matrix_solver, matrix, rhs);

    // Initialize the solutions.
    Solution u_sln, v_sln;

    // Assemble the stiffness matrix and right-hand side vector.
    info("Assembling the stiffness matrix and right-hand side vector.");
    fep.assemble(matrix, rhs);

    // Solve the linear system and if successful, obtain the solutions.
    info("Solving the matrix problem.");
    if(solver->solve())
      Solution::vector_to_solutions(solver->get_solution(), Tuple<Space *>(&u_space, &v_space), Tuple<Solution *>(&u_sln, &v_sln));
    else
      error ("Matrix solver failed.\n");

    int ndof = get_num_dofs(Tuple<Space *>(&u_space, &v_space));
    printf("ndof = %d\n", ndof);
    double sum = 0;
    for (int i=0; i < ndof; i++) sum += rhs->get(i);
    printf("coefficient sum = %g\n", sum);

    // Actual test. The values of 'sum' depend on the
    // current shapeset. If you change the shapeset,
    // you need to correct these numbers.
    if (p_init == 1 && fabs(sum - 3.50185e-06) > 1e-3) success = 0;
    if (p_init == 2 && fabs(sum - 4.34916e-06) > 1e-3) success = 0;
    if (p_init == 3 && fabs(sum - 4.60553e-06) > 1e-3) success = 0;
    if (p_init == 4 && fabs(sum - 4.65616e-06) > 1e-3) success = 0;
    if (p_init == 5 && fabs(sum - 4.62893e-06) > 1e-3) success = 0;
    if (p_init == 6 && fabs(sum - 4.64336e-06) > 1e-3) success = 0;
    if (p_init == 7 && fabs(sum - 4.63724e-06) > 1e-3) success = 0;
    if (p_init == 8 && fabs(sum - 4.64491e-06) > 1e-3) success = 0;
    if (p_init == 9 && fabs(sum - 4.64582e-06) > 1e-3) success = 0;
    if (p_init == 10 && fabs(sum - 4.65028e-06) > 1e-3) success = 0;
  }

#define ERROR_SUCCESS                               0
#define ERROR_FAILURE                               -1
  if (success == 1) {
    printf("Success!\n");
    return ERROR_SUCCESS;
  }
  else {
    printf("Failure!\n");
    return ERROR_FAILURE;
  }
}

