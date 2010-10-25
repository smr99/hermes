#define HERMES_REPORT_WARN
#define HERMES_REPORT_INFO
#define HERMES_REPORT_VERBOSE
#define HERMES_REPORT_FILE "application.log"
#include "hermes2d.h"

using namespace RefinementSelectors;

// The time-dependent laminar incompressible Navier-Stokes equations are
// discretized in time via the implicit Euler method. The Newton's method 
// is used to solve the nonlinear problem at each time step. We show how
// to use discontinuous ($L^2$) elements for pressure and thus make the
// velocity discreetely divergence free. Comparison to approximating the
// pressure with the standard (continuous) Taylor-Hood elements is enabled.
// The Reynolds number Re = 200 which is embarrassingly low. You
// can increase it but then you will need to make the mesh finer, and the
// computation will take more time.
//
// PDE: incompressible Navier-Stokes equations in the form
// \partial v / \partial t - \Delta v / Re + (v \cdot \nabla) v + \nabla p = 0,
// div v = 0
//
// BC: u_1 is a time-dependent constant and u_2 = 0 on Gamma_4 (inlet)
//     u_1 = u_2 = 0 on Gamma_1 (bottom), Gamma_3 (top) and Gamma_5 (obstacle)
//     "do nothing" on Gamma_2 (outlet)
//
// Geometry: Rectangular channel containing an off-axis circular obstacle. The
//           radius and position of the circle, as well as other geometry
//           parameters can be changed in the mesh file "domain.mesh".
//
// The following parameters can be changed:

const bool SOLVE_ON_COARSE_MESH = false; // true... Newton is done on coarse mesh in every adaptivity step.
                                         // false...Newton is done on coarse mesh only once, then projection
                                         // of the fine mesh solution to coarse mesh is used.
const int INIT_REF_NUM = 0;              // Number of initial uniform mesh refinements.
const int INIT_REF_NUM_BDY = 3;          // Number of initial mesh refinements towards boundary.
#define PRESSURE_IN_L2                   // If this is defined, the pressure is approximated using
                                         // discontinuous L2 elements (making the velocity discreetely
                                         // divergence-free, more accurate than using a continuous
                                         // pressure approximation). Otherwise the standard continuous
                                         // elements are used. The results are striking - check the
                                         // tutorial for comparisons.
const int P_INIT_VEL = 2;                // Initial polynomial degree for velocity components
const int P_INIT_PRESSURE = 1;           // Initial polynomial degree for pressure
                                         // Note: P_INIT_VEL should always be greater than
                                         // P_INIT_PRESSURE because of the inf-sup condition

// Adaptivity
const int UNREF_FREQ = 1;        // Every UNREF_FREQth time step the mesh is unrefined.
const double THRESHOLD = 0.3;    // This is a quantitative parameter of the adapt(...) function and
                                 // it has different meanings for various adaptive strategies (see below).
const int STRATEGY = 1;          // Adaptive strategy:
                                 // STRATEGY = 0 ... refine elements until sqrt(THRESHOLD) times total
                                 //   error is processed. If more elements have similar errors, refine
                                 //   all to keep the mesh symmetric.
                                 // STRATEGY = 1 ... refine all elements whose error is larger
                                 //   than THRESHOLD times maximum element error.
                                 // STRATEGY = 2 ... refine all elements whose error is larger
                                 //   than THRESHOLD.
                                 // More adaptive strategies can be created in adapt_ortho_h1.cpp.
const CandList CAND_LIST = H2D_H_ANISO;  // Predefined list of element refinement candidates. Possible values are
                                         // H2D_P_ISO, H2D_P_ANISO, H2D_H_ISO, H2D_H_ANISO, H2D_HP_ISO,
                                         // H2D_HP_ANISO_H, H2D_HP_ANISO_P, H2D_HP_ANISO.
                                         // See the Used Documentation for details.
const int MESH_REGULARITY = -1;          // Maximum allowed level of hanging nodes:
                                         // MESH_REGULARITY = -1 ... arbitrary level hangning nodes (default),
                                         // MESH_REGULARITY = 1 ... at most one-level hanging nodes,
                                         // MESH_REGULARITY = 2 ... at most two-level hanging nodes, etc.
                                         // Note that regular meshes are not supported, this is due to
                                         // their notoriously bad performance.
const double CONV_EXP = 1.0;             // Default value is 1.0. This parameter influences the selection of
                                         // cancidates in hp-adaptivity. See get_optimal_refinement() for details.
const double ERR_STOP = 5.0;             // Stopping criterion for adaptivity (rel. error tolerance between the
                                         // fine mesh and coarse mesh solution in percent).
const int NDOF_STOP = 60000;             // Adaptivity process stops when the number of degrees of freedom grows over
                                         // this limit. This is mainly to prevent h-adaptivity to go on forever.
MatrixSolverType matrix_solver = SOLVER_UMFPACK;  // Possibilities: SOLVER_UMFPACK, SOLVER_PETSC,
                                                  // SOLVER_MUMPS, and more are coming.

// Problem parameters
const double RE = 200.0;             // Reynolds number.
const double VEL_INLET = 1.0;        // Inlet velocity (reached after STARTUP_TIME).
const double STARTUP_TIME = 1.0;     // During this time, inlet velocity increases gradually
                                     // from 0 to VEL_INLET, then it stays constant.
const double TAU = 0.01;             // Time step.
const double T_FINAL = 30000.0;      // Time interval length.

// Newton's method
const double NEWTON_TOL_COARSE = 0.01;     // Stopping criterion for Newton on coarse mesh.
const double NEWTON_TOL_FINE = 0.05;       // Stopping criterion for Newton on fine mesh.
const int NEWTON_MAX_ITER = 20;            // Maximum allowed number of Newton iterations.

// Geometry
const double H = 5;                  // Domain height (necessary to define the parabolic
                                     // velocity profile at inlet)

// Boundary markers.
int bdy_bottom = 1;
int bdy_right  = 2;
int bdy_top = 3;
int bdy_left = 4;
int bdy_obstacle = 5;

// Current time (defined as global since needed in weak forms)
double TIME = 0;

// Boundary condition types for x-velocity
BCType xvel_bc_type(int marker) {
  if (marker == bdy_right) return BC_NONE;
  else return BC_ESSENTIAL;
}

// Boundary condition values for x-velocity
scalar essential_bc_values_xvel(int ess_bdy_marker, double x, double y) {
  if (ess_bdy_marker == bdy_left) {
    // time-dependent inlet velocity (parabolic profile)
    double val_y = VEL_INLET * y*(H-y) / (H/2.)/(H/2.); //parabolic profile with peak VEL_INLET at y = H/2
    if (TIME <= STARTUP_TIME) return val_y * TIME/STARTUP_TIME;
    else return val_y;
  }
  else return 0;
}

// Essential (Dirichlet) boundary condition values for y-velocity.
scalar essential_bc_values_yvel(int ess_bdy_marker, double x, double y) 
{
  return 0;
}

// Boundary condition types for y-velocity
BCType yvel_bc_type(int marker) {
  if (marker == bdy_right) return BC_NONE;
  else return BC_ESSENTIAL;
}

BCType p_bc_type(int marker)
  { return BC_NONE; }

// Weak forms
#include "forms.cpp"

void mag(int n, scalar* a, scalar* dadx, scalar* dady,
                scalar* b, scalar* dbdx, scalar* dbdy,
                scalar* out, scalar* outdx, scalar* outdy)
{
  for (int i = 0; i < n; i++)
  {
    out[i] = sqrt(sqr(a[i]) + sqr(b[i]));
    outdx[i] = (0.5 / out[i]) * (2.0 * a[i] * dadx[i] + 2.0 * b[i] * dbdx[i]);
    outdy[i] = (0.5 / out[i]) * (2.0 * a[i] * dady[i] + 2.0 * b[i] * dbdy[i]);
  }
}

int main(int argc, char* argv[])
{
  // Load the mesh file.
  Mesh basemesh, mesh;
  H2DReader mloader;
  mloader.load("domain.mesh", &basemesh);  // Master mesh.

  // Perform initial mesh refinements.
  for (int i=0; i < INIT_REF_NUM; i++) basemesh.refine_all_elements();
  basemesh.refine_towards_boundary(bdy_obstacle, INIT_REF_NUM_BDY, false); // 'true' stands for anisotropic refinements,
  basemesh.refine_towards_boundary(bdy_top, INIT_REF_NUM_BDY, true);       // 'false' for isotropic.
  basemesh.refine_towards_boundary(bdy_bottom, INIT_REF_NUM_BDY, true);
  mesh.copy(&basemesh);

  // Create spaces with default shapesets. 
  H1Space* xvel_space = new H1Space(&mesh, xvel_bc_type, essential_bc_values_xvel, P_INIT_VEL);
  H1Space* yvel_space = new H1Space(&mesh, yvel_bc_type, essential_bc_values_yvel, P_INIT_VEL);
#ifdef PRESSURE_IN_L2
  L2Space* p_space = new L2Space(&mesh, P_INIT_PRESSURE);
#else
  H1Space* p_space = new H1Space(&mesh, p_bc_type, NULL, P_INIT_PRESSURE);
#endif

  // Calculate and report the number of degrees of freedom.
  int ndof = Space::get_num_dofs(Tuple<Space *>(xvel_space, yvel_space, p_space));
  info("ndof = %d.", ndof);

  // Define projection norms.
  ProjNormType vel_proj_norm = HERMES_H1_NORM;
#ifdef PRESSURE_IN_L2
  ProjNormType p_proj_norm = HERMES_L2_NORM;
#else
  ProjNormType p_proj_norm = HERMES_H1_NORM;
#endif

  // Solutions for the Newton's iteration and time stepping.
  info("Setting initial conditions.");
//  Solution xvel_fine, yvel_fine, p_fine;
  Solution xvel_sln, yvel_sln, p_sln;
  Solution xvel_ref_sln, yvel_ref_sln, p_ref_sln;
  Solution xvel_prev_time, yvel_prev_time, p_prev_time;

  // Define initial conditions on the coarse mesh.
  xvel_prev_time.set_zero(&mesh);
  yvel_prev_time.set_zero(&mesh);
  p_prev_time.set_zero(&mesh);

  xvel_sln.copy(&xvel_prev_time);
  yvel_sln.copy(&yvel_prev_time);
  p_sln.copy(&p_prev_time);

  // Initialize the weak formulation.
  WeakForm wf(3);
  wf.add_matrix_form(0, 0, callback(bilinear_form_sym_0_0_1_1), HERMES_SYM);
  wf.add_matrix_form(0, 0, callback(newton_bilinear_form_unsym_0_0), HERMES_UNSYM, HERMES_ANY);
  wf.add_matrix_form(0, 1, callback(newton_bilinear_form_unsym_0_1), HERMES_UNSYM, HERMES_ANY);
  wf.add_matrix_form(0, 2, callback(bilinear_form_unsym_0_2), HERMES_ANTISYM);
  wf.add_matrix_form(1, 0, callback(newton_bilinear_form_unsym_1_0), HERMES_UNSYM, HERMES_ANY);
  wf.add_matrix_form(1, 1, callback(bilinear_form_sym_0_0_1_1), HERMES_SYM);
  wf.add_matrix_form(1, 1, callback(newton_bilinear_form_unsym_1_1), HERMES_UNSYM, HERMES_ANY);
  wf.add_matrix_form(1, 2, callback(bilinear_form_unsym_1_2), HERMES_ANTISYM);
  wf.add_vector_form(0, callback(newton_F_0), HERMES_ANY, Tuple<MeshFunction*>(&xvel_prev_time, &yvel_prev_time));
  wf.add_vector_form(1, callback(newton_F_1), HERMES_ANY, Tuple<MeshFunction*>(&xvel_prev_time, &yvel_prev_time));
  wf.add_vector_form(2, callback(newton_F_2), HERMES_ANY);

  // Initialize views.
  VectorView vview("velocity [m/s]", new WinGeom(0, 0, 600, 500));
  ScalarView pview("pressure [Pa]", new WinGeom(610, 0, 600, 500));
  vview.fix_scale_width(80);
  pview.fix_scale_width(80);
  pview.show_mesh(true);

  // Create a selector which will select optimal candidate.
  H1ProjBasedSelector selector(CAND_LIST, CONV_EXP, H2DRS_DEFAULT_ORDER);

  // Time-stepping loop:
  char title[100];
  int num_time_steps = (int)(T_FINAL/TAU + 0.5);
  for(int ts = 1; ts <= num_time_steps; ts++)
  {
    info("---- Time step %d:", ts);

    // Periodic global derefinements.
    if (ts > 1 && ts % UNREF_FREQ == 0) {
      info("Global mesh derefinement.");
      mesh.copy(&basemesh);
      xvel_space->set_uniform_order(P_INIT_VEL);
      yvel_space->set_uniform_order(P_INIT_VEL);
      p_space->set_uniform_order(P_INIT_PRESSURE);
    }

    // Adaptivity loop:
    bool done = false; int as = 1;
    double err_est;
    do {
      info("Time step %d, adaptivity step %d:", ts, as);

      // Construct globally refined reference mesh
      // and setup reference space.
      Tuple<Space *>* ref_spaces = construct_refined_spaces(Tuple<Space *>(xvel_space, yvel_space, p_space));

      scalar* coeff_vec = new scalar[Space::get_num_dofs(*ref_spaces)];

      bool is_linear = false;
      DiscreteProblem dp(&wf, *ref_spaces, is_linear);
      SparseMatrix* matrix = create_matrix(matrix_solver);
      Vector* rhs = create_vector(matrix_solver);
      Solver* solver = create_linear_solver(matrix_solver, matrix, rhs);

      // Calculate initial coefficient vector for Newton on the fine mesh.
      if (as == 1) {
        info("Projecting coarse mesh solution to obtain coefficient vector on new fine mesh.");
        OGProjection::project_global(*ref_spaces, Tuple<MeshFunction *>(&xvel_sln, &yvel_sln, &p_sln), coeff_vec, matrix_solver, Tuple<ProjNormType>(vel_proj_norm, vel_proj_norm, p_proj_norm));
      }
      else {
        info("Projecting previous fine mesh solution to obtain coefficient vector on new fine mesh.");
        OGProjection::project_global(*ref_spaces, Tuple<MeshFunction *>(&xvel_ref_sln, &yvel_ref_sln, &p_ref_sln), coeff_vec, matrix_solver, Tuple<ProjNormType>(vel_proj_norm, vel_proj_norm, p_proj_norm));
      }

      // Newton's loop on the fine mesh.
      info("Solving on fine mesh:");
      int it = 1;
      while (1)
      {
        // Obtain the number of degrees of freedom.
        int ndof = Space::get_num_dofs(*ref_spaces);

        // Assemble the Jacobian matrix and residual vector.
        dp.assemble(coeff_vec, matrix, rhs, false);

        // Multiply the residual vector with -1 since the matrix 
        // equation reads J(Y^n) \deltaY^{n+1} = -F(Y^n).
        for (int i = 0; i < ndof; i++) rhs->set(i, -rhs->get(i));
        
        // Calculate the l2-norm of residual vector.
        double res_l2_norm = get_l2_norm(rhs);

        // Info for user.
        info("---- Newton iter %d, ndof %d, res. l2 norm %g", it, Space::get_num_dofs(*ref_spaces), res_l2_norm);

        // If l2 norm of the residual vector is within tolerance, or the maximum number 
        // of iteration has been reached, then quit.
        if (res_l2_norm < NEWTON_TOL_FINE || it > NEWTON_MAX_ITER) break;

        // Solve the linear system.
        if(!solver->solve())
          error ("Matrix solver failed.\n");

        // Add \deltaY^{n+1} to Y^n.
        for (int i = 0; i < ndof; i++) coeff_vec[i] += solver->get_solution()[i];
        
        if (it >= NEWTON_MAX_ITER)
          error ("Newton method did not converge.");

        it++;
      }

      // Store the result in ref_sln.
      Solution::vector_to_solutions(coeff_vec, *ref_spaces, Tuple<Solution *>(&xvel_ref_sln, &yvel_ref_sln, &p_ref_sln));

      // Project the fine mesh solution onto the coarse mesh.
      info("Projecting reference solution on coarse mesh.");
      OGProjection::project_global(Tuple<Space *>(xvel_space, yvel_space, p_space), Tuple<Solution *>(&xvel_ref_sln, &yvel_ref_sln, &p_ref_sln), Tuple<Solution *>(&xvel_sln, &yvel_sln, &p_sln), matrix_solver); 

      // Calculate element errors and total error estimate.
      info("Calculating error estimate.");
      Adapt* adaptivity = new Adapt(Tuple<Space *>(xvel_space, yvel_space, p_space), Tuple<ProjNormType>(vel_proj_norm, vel_proj_norm, p_proj_norm));
      bool solutions_for_adapt = true;
      double err_est_rel_total = adaptivity->calc_err_est(Tuple<Solution *>(&xvel_sln, &yvel_sln, &p_sln), Tuple<Solution *>(&xvel_ref_sln, &yvel_ref_sln, &p_ref_sln), solutions_for_adapt, HERMES_TOTAL_ERROR_REL | HERMES_ELEMENT_ERROR_REL) * 100.;

      // Report results.
      info("ndof: %d, ref_ndof: %d, err_est_rel: %g%%", 
           Space::get_num_dofs(Tuple<Space *>(xvel_space, yvel_space, p_space)), Space::get_num_dofs(*ref_spaces), err_est_rel_total);

      // If err_est too large, adapt the mesh.
      if (err_est_rel_total < ERR_STOP) done = true;
      else 
      {
        info("Adapting the coarse mesh.");
        done = adaptivity->adapt(Tuple<RefinementSelectors::Selector *>(&selector, &selector, &selector), THRESHOLD, STRATEGY, MESH_REGULARITY);

        if (Space::get_num_dofs(Tuple<Space *>(xvel_space, yvel_space, p_space)) >= NDOF_STOP) 
          done = true;
        else
          // Increase the counter of performed adaptivity steps.
          as++;
      }

      // Clean up.
      delete solver;
      delete matrix;
      delete rhs;
      delete adaptivity;
      for(int i = 0; i < ref_spaces->size(); i++)
        delete (*ref_spaces)[i]->get_mesh();
      delete ref_spaces;
    }
    while (done == false);

    // Copy new time level reference solution into prev_time.
    xvel_prev_time.copy(&xvel_ref_sln);
    yvel_prev_time.copy(&yvel_ref_sln);
    p_prev_time.copy(&p_ref_sln);

    // Show the solution at the end of time step.
    sprintf(title, "Velocity, time %g", TIME);
    vview.set_title(title);
    vview.show(&xvel_prev_time, &yvel_prev_time, HERMES_EPS_LOW);
    sprintf(title, "Pressure, time %g", TIME);
    pview.set_title(title);
    pview.show(&p_prev_time);
  }

  ndof = Space::get_num_dofs(Tuple<Space *>(xvel_space, yvel_space, p_space));
  info("ndof = %d", ndof);

  // Wait for all views to be closed.
  View::wait();
  return 0;
}
