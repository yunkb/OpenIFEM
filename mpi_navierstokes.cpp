#include "mpi_navierstokes.h"

namespace Fluid
{
  template <int dim>
  double ParallelNavierStokes<dim>::BoundaryValues::value(
    const Point<dim> &p, const unsigned int component) const
  {
    Assert(component < this->n_components,
           ExcIndexRange(component, 0, this->n_components));
    if (component == 0 && std::abs(p[0] - 0.3) < 1e-10)
      {
        double U = 1.5;
        double y = p[1];
        return 4 * U * y * (0.41 - y) / (0.41 * 0.41);
      }
    return 0;
  }

  template <int dim>
  void ParallelNavierStokes<dim>::BoundaryValues::vector_value(
    const Point<dim> &p, Vector<double> &values) const
  {
    for (unsigned int c = 0; c < this->n_components; ++c)
      values(c) = BoundaryValues::value(p, c);
  }

  /**
   * Initialize tmp1 and tmp2 in the constructor.
   */
  template <int dim>
  ParallelNavierStokes<dim>::BlockSchurPreconditioner::ApproximateMassSchur::
    ApproximateMassSchur(const PETScWrappers::MPI::BlockSparseMatrix &M,
                         const std::vector<IndexSet> &partition)
    : mass_matrix(&M)
  {
    // Initialization
    tmp1.reinit(partition[0], mass_matrix->get_mpi_communicator());
    tmp2.reinit(partition[0], mass_matrix->get_mpi_communicator());
  }

  /**
   * ApproximateMassSchur is nested in BlockSchurPreconditioner so it
   * can use mass_matrix directly.
   */
  template <int dim>
  void ParallelNavierStokes<dim>::BlockSchurPreconditioner::
    ApproximateMassSchur::vmult(PETScWrappers::MPI::Vector &dst,
                                const PETScWrappers::MPI::Vector &src) const
  {
    tmp1 = 0;
    tmp2 = 0;
    mass_matrix->block(0, 1).vmult(tmp1, src);
    // Jacobi preconditioner of matrix A is by definition inverse diag(A),
    // this is exactly what we want to compute.
    PETScWrappers::PreconditionJacobi jacobi(mass_matrix->block(0, 0));
    jacobi.vmult(tmp2, tmp1);
    mass_matrix->block(1, 0).vmult(dst, tmp2);
  }

  /**
   * In serial code, we initialize the direct solver in the constructor
   * to avoid repeatedly allocating memory. However it seems we can't
   * do the same thing to the PETSc direct solver.
   */
  template <int dim>
  ParallelNavierStokes<dim>::BlockSchurPreconditioner::BlockSchurPreconditioner(
    TimerOutput &timer,
    double gamma,
    double viscosity,
    double dt,
    const std::vector<IndexSet> &owned_partitioning,
    const PETScWrappers::MPI::BlockSparseMatrix &system,
    const PETScWrappers::MPI::BlockSparseMatrix &mass)
    : timer(timer),
      gamma(gamma),
      viscosity(viscosity),
      dt(dt),
      system_matrix(&system),
      mass_matrix(&mass),
      A_inverse(dummy_sc, system_matrix->get_mpi_communicator()),
      mass_schur(mass, owned_partitioning)
  {
  }

  /**
   * The vmult operation strictly follows the definition of
   * BlockSchurPreconditioner. Conceptually it computes \f$u = P^{-1}v\f$.
   */
  template <int dim>
  void ParallelNavierStokes<dim>::BlockSchurPreconditioner::vmult(
    PETScWrappers::MPI::BlockVector &dst,
    const PETScWrappers::MPI::BlockVector &src) const
  {
    // First, buffer the velocity block of src vector (\f$v_0\f$).
    PETScWrappers::MPI::Vector utmp(src.block(0));

    // This function is part of "solve linear system", but it
    // is further profiled to get a better idea of how time
    // is spent on different solvers.

    // This block computes \f$u_1 = \tilde{S}^{-1} v_1\f$.
    {
      timer.enter_subsection("CG for Mp");

      // CG solver used for \f$M_p^{-1}\f$ and \f$S_m^{-1}\f$.
      SolverControl solver_control(src.block(1).size(),
                                   1e-6 * src.block(1).l2_norm());
      SolverCG<PETScWrappers::MPI::Vector> cg(solver_control);

      // \f$-(\nu + \gamma)M_p^{-1}v_1\f$
      PETScWrappers::MPI::Vector tmp(src.block(1));
      tmp = 0;
      PETScWrappers::PreconditionBlockJacobi Mp_preconditioner;
      Mp_preconditioner.initialize(mass_matrix->block(1, 1));
      cg.solve(mass_matrix->block(1, 1), tmp, src.block(1), Mp_preconditioner);
      tmp *= -(viscosity + gamma);

      timer.leave_subsection("CG for Mp");
      timer.enter_subsection("CG for Sm");

      // \f$-\frac{1}{dt}S_m^{-1}v_1\f$
      // NOTE: mass_schur is not derived from PETScWrappers::MatrixBase
      // so PETScWrappers::SolverCG cannot be used.
      PreconditionIdentity Sm_preconditioner;
      cg.solve(mass_schur, dst.block(1), src.block(1), Sm_preconditioner);
      dst.block(1) *= -1 / dt;

      // Adding up these two, we get \f$\tilde{S}^{-1}v_1\f$.
      dst.block(1) += tmp;

      timer.leave_subsection("CG for Sm");
    }

    // This block computes \f$v_0 - B^T\tilde{S}^{-1}v_1\f$ based on \f$u_1\f$.
    {
      system_matrix->block(0, 1).vmult(utmp, dst.block(1));
      utmp *= -1.0;
      utmp += src.block(0);
    }

    // Finally, compute the product of \f$\tilde{A}^{-1}\f$ and utmp with
    // the direct solver.
    {
      TimerOutput::Scope timer_section(timer, "MUMPS for A_inv");
      A_inverse.solve(system_matrix->block(0, 0), dst.block(0), utmp);
    }
  }

  template <int dim>
  ParallelNavierStokes<dim>::ParallelNavierStokes(
    parallel::distributed::Triangulation<dim> &tria,
    const Parameters::AllParameters &parameters)
    : viscosity(parameters.viscosity),
      gamma(parameters.grad_div),
      degree(parameters.fluid_degree),
      triangulation(tria),
      fe(FE_Q<dim>(degree + 1), dim, FE_Q<dim>(degree), 1),
      dof_handler(triangulation),
      volume_quad_formula(degree + 2),
      face_quad_formula(degree + 2),
      tolerance(parameters.fluid_tolerance),
      max_iteration(parameters.fluid_max_iterations),
      parameters(parameters),
      mpi_communicator(MPI_COMM_WORLD),
      pcout(std::cout, Utilities::MPI::this_mpi_process(mpi_communicator) == 0),
      time(parameters.end_time,
           parameters.time_step,
           parameters.output_interval,
           parameters.refinement_interval),
      timer(
        mpi_communicator, pcout, TimerOutput::summary, TimerOutput::wall_times)

  {
  }

  template <int dim>
  void ParallelNavierStokes<dim>::setup_dofs()
  {
    // The first step is to associate DoFs with a given mesh.
    dof_handler.distribute_dofs(fe);

    // We renumber the components to have all velocity DoFs come before
    // the pressure DoFs to be able to split the solution vector in two blocks
    // which are separately accessed in the block preconditioner.
    DoFRenumbering::Cuthill_McKee(dof_handler);
    std::vector<unsigned int> block_component(dim + 1, 0);
    block_component[dim] = 1;
    DoFRenumbering::component_wise(dof_handler, block_component);

    dofs_per_block.resize(2);
    DoFTools::count_dofs_per_block(
      dof_handler, dofs_per_block, block_component);
    unsigned int dof_u = dofs_per_block[0];
    unsigned int dof_p = dofs_per_block[1];

    // This part is new compared to serial code: we need to split up the
    // IndexSet
    // based on how we want to create the block matrices and vectors
    owned_partitioning.resize(2);
    owned_partitioning[0] = dof_handler.locally_owned_dofs().get_view(0, dof_u);
    owned_partitioning[1] =
      dof_handler.locally_owned_dofs().get_view(dof_u, dof_u + dof_p);

    DoFTools::extract_locally_relevant_dofs(dof_handler, locally_relevant_dofs);

    relevant_partitioning.resize(2);
    relevant_partitioning[0] = locally_relevant_dofs.get_view(0, dof_u);
    relevant_partitioning[1] =
      locally_relevant_dofs.get_view(dof_u, dof_u + dof_p);

    // In Newton's scheme, we first apply the boundary condition on the solution
    // obtained from the initial step. To make sure the boundary conditions
    // remain
    // satisfied during Newton's iteration, zero boundary conditions are used
    // for
    // the update \f$\delta u^k\f$. Therefore we set up two different constraint
    // objects.
    // Dirichlet boundary conditions are applied to both boundaries 0 and 1.

    // For inhomogeneous BC, only constant input values can be read from
    // the input file. If time or space dependent Dirichlet BCs are
    // desired, this block of code has to be modified.
    {
      nonzero_constraints.clear();
      zero_constraints.clear();
      nonzero_constraints.reinit(locally_relevant_dofs);
      zero_constraints.reinit(locally_relevant_dofs);
      DoFTools::make_hanging_node_constraints(dof_handler, nonzero_constraints);
      DoFTools::make_hanging_node_constraints(dof_handler, zero_constraints);
      for (auto itr = parameters.fluid_dirichlet_bcs.begin();
           itr != parameters.fluid_dirichlet_bcs.end();
           ++itr)
        {
          // First get the id, flag and value from the input file
          unsigned int id = itr->first;
          unsigned int flag = itr->second.first;
          std::vector<double> value = itr->second.second;

          // To make VectorTools::interpolate_boundary_values happy,
          // a vector of bool and a vector of double which are of size
          // dim + 1 are required.
          std::vector<bool> mask(dim + 1, false);
          std::vector<double> augmented_value(dim + 1, 0.0);
          // 1-x, 2-y, 3-xy, 4-z, 5-xz, 6-yz, 7-xyz
          switch (flag)
            {
            case 1:
              mask[0] = true;
              augmented_value[0] = value[0];
              break;
            case 2:
              mask[1] = true;
              augmented_value[1] = value[0];
              break;
            case 3:
              mask[0] = true;
              mask[1] = true;
              augmented_value[0] = value[0];
              augmented_value[1] = value[1];
              break;
            case 4:
              mask[2] = true;
              augmented_value[2] = value[0];
              break;
            case 5:
              mask[0] = true;
              mask[2] = true;
              augmented_value[0] = value[0];
              augmented_value[2] = value[1];
              break;
            case 6:
              mask[1] = true;
              mask[2] = true;
              augmented_value[1] = value[0];
              augmented_value[2] = value[1];
              break;
            case 7:
              mask[0] = true;
              mask[1] = true;
              mask[2] = true;
              augmented_value[0] = value[0];
              augmented_value[1] = value[1];
              augmented_value[2] = value[2];
              break;
            default:
              AssertThrow(false, ExcMessage("Unrecogonized component flag!"));
              break;
            }
          VectorTools::interpolate_boundary_values(
            dof_handler,
            id,
            // Functions::ConstantFunction<dim>(augmented_value),
            BoundaryValues(),
            nonzero_constraints,
            ComponentMask(mask));
          VectorTools::interpolate_boundary_values(
            dof_handler,
            id,
            Functions::ZeroFunction<dim>(dim + 1),
            zero_constraints,
            ComponentMask(mask));
        }
    }
    nonzero_constraints.close();
    zero_constraints.close();

    pcout << "   Number of active fluid cells: "
          << triangulation.n_active_cells() << std::endl
          << "   Number of degrees of freedom: " << dof_handler.n_dofs() << " ("
          << dof_u << '+' << dof_p << ')' << std::endl;
  }

  template <int dim>
  void ParallelNavierStokes<dim>::initialize_system()
  {
    preconditioner.reset();
    system_matrix.clear();
    mass_matrix.clear();

    BlockDynamicSparsityPattern dsp(dofs_per_block, dofs_per_block);
    DoFTools::make_sparsity_pattern(dof_handler, dsp, nonzero_constraints);
    sparsity_pattern.copy_from(dsp);
    SparsityTools::distribute_sparsity_pattern(
      dsp,
      dof_handler.locally_owned_dofs_per_processor(),
      mpi_communicator,
      locally_relevant_dofs);

    system_matrix.reinit(owned_partitioning, dsp, mpi_communicator);
    mass_matrix.reinit(owned_partitioning, dsp, mpi_communicator);

    // present_solution is ghosted because it is used in the
    // output and mesh refinement functions.
    present_solution.reinit(
      owned_partitioning, relevant_partitioning, mpi_communicator);
    // newton_update is non-ghosted because the linear solver needs
    // a completely distributed vector.
    newton_update.reinit(owned_partitioning, mpi_communicator);
    // evaluation_point is ghosted because it is used in the assembly.
    evaluation_point.reinit(
      owned_partitioning, relevant_partitioning, mpi_communicator);
    // system_rhs is non-ghosted because it is only used in the linear
    // solver and residual evaluation.
    system_rhs.reinit(owned_partitioning, mpi_communicator);
  }

  template <int dim>
  void ParallelNavierStokes<dim>::assemble(const bool initial_step,
                                           const bool assemble_matrix)
  {
    TimerOutput::Scope timer_section(timer, "Assemble system");

    if (assemble_matrix)
      {
        system_matrix = 0;
        mass_matrix = 0;
      }

    system_rhs = 0;

    FEValues<dim> fe_values(fe,
                            volume_quad_formula,
                            update_values | update_quadrature_points |
                              update_JxW_values | update_gradients);
    FEFaceValues<dim> fe_face_values(fe,
                                     face_quad_formula,
                                     update_values | update_normal_vectors |
                                       update_quadrature_points |
                                       update_JxW_values);

    const unsigned int dofs_per_cell = fe.dofs_per_cell;
    const unsigned int n_q_points = volume_quad_formula.size();
    const unsigned int n_face_q_points = face_quad_formula.size();

    const FEValuesExtractors::Vector velocities(0);
    const FEValuesExtractors::Scalar pressure(dim);

    FullMatrix<double> local_matrix(dofs_per_cell, dofs_per_cell);
    FullMatrix<double> local_mass_matrix(dofs_per_cell, dofs_per_cell);
    Vector<double> local_rhs(dofs_per_cell);

    std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

    // For the linearized system, we create temporary storage for current
    // velocity
    // and gradient, current pressure, and present velocity. In practice, they
    // are
    // all obtained through their shape functions at quadrature points.

    std::vector<Tensor<1, dim>> current_velocity_values(n_q_points);
    std::vector<Tensor<2, dim>> current_velocity_gradients(n_q_points);
    std::vector<double> current_pressure_values(n_q_points);
    std::vector<Tensor<1, dim>> present_velocity_values(n_q_points);

    std::vector<double> div_phi_u(dofs_per_cell);
    std::vector<Tensor<1, dim>> phi_u(dofs_per_cell);
    std::vector<Tensor<2, dim>> grad_phi_u(dofs_per_cell);
    std::vector<double> phi_p(dofs_per_cell);

    for (auto cell = dof_handler.begin_active(); cell != dof_handler.end();
         ++cell)
      {
        if (cell->is_locally_owned())
          {
            fe_values.reinit(cell);

            local_matrix = 0;
            local_mass_matrix = 0;
            local_rhs = 0;

            fe_values[velocities].get_function_values(evaluation_point,
                                                      current_velocity_values);

            fe_values[velocities].get_function_gradients(
              evaluation_point, current_velocity_gradients);

            fe_values[pressure].get_function_values(evaluation_point,
                                                    current_pressure_values);

            fe_values[velocities].get_function_values(present_solution,
                                                      present_velocity_values);

            // Assemble the system matrix and mass matrix simultaneouly.
            // We assemble \f$B^T\f$ and \f$B\f$ into the mass matrix for
            // convience
            // because they will be used in the preconditioner.
            //
            for (unsigned int q = 0; q < n_q_points; ++q)
              {
                for (unsigned int k = 0; k < dofs_per_cell; ++k)
                  {
                    div_phi_u[k] = fe_values[velocities].divergence(k, q);
                    grad_phi_u[k] = fe_values[velocities].gradient(k, q);
                    phi_u[k] = fe_values[velocities].value(k, q);
                    phi_p[k] = fe_values[pressure].value(k, q);
                  }

                for (unsigned int i = 0; i < dofs_per_cell; ++i)
                  {
                    if (assemble_matrix)
                      {
                        for (unsigned int j = 0; j < dofs_per_cell; ++j)
                          {
                            // Let the linearized diffusion, continuity and
                            // Grad-Div
                            // term be written as
                            // the bilinear operator: \f$A = a((\delta{u},
                            // \delta{p}), (\delta{v}, \delta{q}))\f$,
                            // the linearized convection term be: \f$C =
                            // c(u;\delta{u}, \delta{v})\f$,
                            // and the linearized inertial term be:
                            // \f$M = m(\delta{u}, \delta{v})$, then LHS is: $(A
                            // +
                            // C) + M/{\Delta{t}}\f$
                            local_matrix(i, j) +=
                              ((viscosity *
                                  scalar_product(grad_phi_u[j], grad_phi_u[i]) +
                                current_velocity_gradients[q] * phi_u[j] *
                                  phi_u[i] +
                                grad_phi_u[j] * current_velocity_values[q] *
                                  phi_u[i] -
                                div_phi_u[i] * phi_p[j] -
                                phi_p[i] * div_phi_u[j] +
                                gamma * div_phi_u[j] * div_phi_u[i]) +
                               phi_u[i] * phi_u[j] / time.get_delta_t()) *
                              fe_values.JxW(q);
                            local_mass_matrix(i, j) +=
                              (phi_u[i] * phi_u[j] + phi_p[i] * phi_p[j] -
                               div_phi_u[i] * phi_p[j] -
                               phi_p[i] * div_phi_u[j]) *
                              fe_values.JxW(q);
                          }
                      }

                    // RHS is \f$-(A_{current} + C_{current}) -
                    // M_{present-current}/\Delta{t}\f$.
                    double current_velocity_divergence =
                      trace(current_velocity_gradients[q]);
                    local_rhs(i) +=
                      ((-viscosity *
                          scalar_product(current_velocity_gradients[q],
                                         grad_phi_u[i]) -
                        current_velocity_gradients[q] *
                          current_velocity_values[q] * phi_u[i] +
                        current_pressure_values[q] * div_phi_u[i] +
                        current_velocity_divergence * phi_p[i] -
                        gamma * current_velocity_divergence * div_phi_u[i]) -
                       (current_velocity_values[q] -
                        present_velocity_values[q]) *
                         phi_u[i] / time.get_delta_t()) *
                      fe_values.JxW(q);
                  }
              }

            // Impose pressure boundary here if specified, loop over faces on
            // the
            // cell
            // and apply pressure boundary conditions:
            // \f$\int_{\Gamma_n} -p\bold{n}d\Gamma\f$
            if (parameters.n_fluid_neumann_bcs != 0)
              {
                for (unsigned int face_n = 0;
                     face_n < GeometryInfo<dim>::faces_per_cell;
                     ++face_n)
                  {
                    if (cell->at_boundary(face_n) &&
                        parameters.fluid_neumann_bcs.find(
                          cell->face(face_n)->boundary_id()) !=
                          parameters.fluid_neumann_bcs.end())
                      {
                        fe_face_values.reinit(cell, face_n);
                        unsigned int p_bc_id =
                          cell->face(face_n)->boundary_id();
                        double boundary_values_p =
                          parameters.fluid_neumann_bcs[p_bc_id];
                        for (unsigned int q = 0; q < n_face_q_points; ++q)
                          {
                            for (unsigned int i = 0; i < dofs_per_cell; ++i)
                              {
                                local_rhs(i) +=
                                  -(fe_face_values[velocities].value(i, q) *
                                    fe_face_values.normal_vector(q) *
                                    boundary_values_p * fe_face_values.JxW(q));
                              }
                          }
                      }
                  }
              }

            cell->get_dof_indices(local_dof_indices);

            const ConstraintMatrix &constraints_used =
              initial_step ? nonzero_constraints : zero_constraints;

            if (assemble_matrix)
              {
                constraints_used.distribute_local_to_global(local_matrix,
                                                            local_rhs,
                                                            local_dof_indices,
                                                            system_matrix,
                                                            system_rhs);
                constraints_used.distribute_local_to_global(
                  local_mass_matrix, local_dof_indices, mass_matrix);
              }
            else
              {
                constraints_used.distribute_local_to_global(
                  local_rhs, local_dof_indices, system_rhs);
              }
          }
      }

    if (assemble_matrix)
      {
        system_matrix.compress(VectorOperation::add);
        mass_matrix.compress(VectorOperation::add);
      }
    system_rhs.compress(VectorOperation::add);
  }

  template <int dim>
  void ParallelNavierStokes<dim>::assemble_system(const bool initial_step)
  {
    assemble(initial_step, true);
  }

  template <int dim>
  void ParallelNavierStokes<dim>::assemble_rhs(const bool initial_step)
  {
    assemble(initial_step, false);
  }

  template <int dim>
  std::pair<unsigned int, double>
  ParallelNavierStokes<dim>::solve(const bool initial_step)
  {
    // This section includes the work done in the preconditioner
    // and GMRES solver.
    TimerOutput::Scope timer_section(timer, "Solve linear system");
    preconditioner.reset(new BlockSchurPreconditioner(timer,
                                                      gamma,
                                                      viscosity,
                                                      time.get_delta_t(),
                                                      owned_partitioning,
                                                      system_matrix,
                                                      mass_matrix));

    SolverControl solver_control(
      system_matrix.m(), 1e-8 * system_rhs.l2_norm(), true);

    // Because PETScWrappers::SolverGMRES requires preconditioner derived
    // from PETScWrappers::PreconditionBase, we use dealii SolverFGMRES.
    GrowingVectorMemory<PETScWrappers::MPI::BlockVector> vector_memory;
    SolverFGMRES<PETScWrappers::MPI::BlockVector> gmres(solver_control,
                                                        vector_memory);

    // The solution vector must be non-ghosted
    gmres.solve(system_matrix, newton_update, system_rhs, *preconditioner);

    const ConstraintMatrix &constraints_used =
      initial_step ? nonzero_constraints : zero_constraints;
    constraints_used.distribute(newton_update);

    return {solver_control.last_step(), solver_control.last_value()};
  }

  template <int dim>
  void ParallelNavierStokes<dim>::refine_mesh(const unsigned int min_grid_level,
                                              const unsigned int max_grid_level)
  {
    TimerOutput::Scope timer_section(timer, "Refine mesh");
    pcout << "Refining mesh..." << std::endl;

    PETScWrappers::MPI::BlockVector solution(present_solution);
    Vector<float> estimated_error_per_cell(triangulation.n_active_cells());
    FEValuesExtractors::Vector velocity(0);

    // Evaluate the error
    KellyErrorEstimator<dim>::estimate(dof_handler,
                                       face_quad_formula,
                                       typename FunctionMap<dim>::type(),
                                       solution,
                                       estimated_error_per_cell,
                                       fe.component_mask(velocity));

    // Set the refine and coarsen flag
    parallel::distributed::GridRefinement::refine_and_coarsen_fixed_fraction(
      triangulation, estimated_error_per_cell, 0.6, 0.4);
    if (triangulation.n_levels() > max_grid_level)
      {
        for (auto cell = triangulation.begin_active(max_grid_level);
             cell != triangulation.end();
             ++cell)
          {
            cell->clear_refine_flag();
          }
      }
    for (auto cell = triangulation.begin_active(min_grid_level);
         cell != triangulation.end_active(min_grid_level);
         ++cell)
      {
        cell->clear_coarsen_flag();
      }

    // Prepare to transfer
    parallel::distributed::SolutionTransfer<dim,
                                            PETScWrappers::MPI::BlockVector>
      trans(dof_handler);

    triangulation.prepare_coarsening_and_refinement();

    trans.prepare_for_coarsening_and_refinement(solution);

    // Refine the mesh
    triangulation.execute_coarsening_and_refinement();

    // Reinitialize the system
    setup_dofs();
    initialize_system();

    // Transfer solution
    // Need a non-ghosted vector for interpolation
    PETScWrappers::MPI::BlockVector tmp(newton_update);
    trans.interpolate(tmp);
    nonzero_constraints.distribute(tmp);
    present_solution = tmp;
  }

  template <int dim>
  void ParallelNavierStokes<dim>::run()
  {
    pcout << "Running with PETSc on "
          << Utilities::MPI::n_mpi_processes(mpi_communicator)
          << " MPI rank(s)..." << std::endl;

    triangulation.refine_global(parameters.global_refinement);
    setup_dofs();
    initialize_system();

    bool first_step = true;

    // Time loop.
    std::cout.precision(6);
    std::cout.width(12);
    output_results(time.get_timestep());
    while (time.end() - time.current() > 1e-12)
      {
        time.increment();
        pcout << std::string(96, '*') << std::endl
              << "Time step = " << time.get_timestep()
              << ", at t = " << std::scientific << time.current() << std::endl;

        // Resetting
        double current_residual = 1.0;
        double initial_residual = 1.0;
        double relative_residual = 1.0;
        unsigned int outer_iteration = 0;
        evaluation_point = present_solution;
        while (first_step || relative_residual > tolerance)
          {
            AssertThrow(outer_iteration < max_iteration,
                        ExcMessage("Too many Newton iterations!"));

            newton_update = 0;

            // Since evaluation_point changes at every iteration,
            // we have to reassemble both the lhs and rhs of the system
            // before solving it.
            assemble_system(first_step);
            auto state = solve(first_step);
            current_residual = system_rhs.l2_norm();

            // Update evaluation_point and do not forget to modify it
            // with constraints.
            // Note we have to use a non-ghosted vector as a buffer in order
            // to do addition.
            PETScWrappers::MPI::BlockVector tmp;
            tmp.reinit(owned_partitioning, mpi_communicator);
            tmp = evaluation_point;
            tmp += newton_update;
            nonzero_constraints.distribute(tmp);
            evaluation_point = tmp;
            first_step = false;

            if (outer_iteration == 0)
              {
                initial_residual = current_residual;
              }
            relative_residual = current_residual / initial_residual;

            pcout << std::scientific << std::left << " ITR = " << std::setw(2)
                  << outer_iteration << " ABS_RES = " << current_residual
                  << " REL_RES = " << relative_residual
                  << " GMRES_ITR = " << std::setw(3) << state.first
                  << " GMRES_RES = " << state.second << std::endl;

            outer_iteration++;
          }
        // Newton iteration converges, update time and solution
        present_solution = evaluation_point;
        // Output
        if (time.time_to_output())
          {
            output_results(time.get_timestep());
          }
        if (time.time_to_refine())
          {
            refine_mesh(1, 3);
          }
      }
  }

  template <int dim>
  void ParallelNavierStokes<dim>::output_results(
    const unsigned int output_index) const
  {
    TimerOutput::Scope timer_section(timer, "Output results");

    pcout << "Writing results..." << std::endl;
    std::vector<std::string> solution_names(dim, "velocity");
    solution_names.push_back("pressure");

    std::vector<DataComponentInterpretation::DataComponentInterpretation>
      data_component_interpretation(
        dim, DataComponentInterpretation::component_is_part_of_vector);
    data_component_interpretation.push_back(
      DataComponentInterpretation::component_is_scalar);
    DataOut<dim> data_out;
    data_out.attach_dof_handler(dof_handler);
    // vector to be output must be ghosted
    data_out.add_data_vector(present_solution,
                             solution_names,
                             DataOut<dim>::type_dof_data,
                             data_component_interpretation);

    Vector<float> subdomain(triangulation.n_active_cells());
    for (unsigned int i = 0; i < subdomain.size(); ++i)
      {
        subdomain(i) = triangulation.locally_owned_subdomain();
      }
    data_out.add_data_vector(subdomain, "subdomain");
    data_out.build_patches();

    std::string basename =
      "navierstokes" + Utilities::int_to_string(output_index, 6) + "-";

    std::string filename =
      basename +
      Utilities::int_to_string(triangulation.locally_owned_subdomain(), 4) +
      ".vtu";

    std::ofstream output(filename);
    data_out.write_vtu(output);

    static std::vector<std::pair<double, std::string>> times_and_names;
    if (Utilities::MPI::this_mpi_process(mpi_communicator) == 0)
      {
        for (unsigned int i = 0;
             i < Utilities::MPI::n_mpi_processes(mpi_communicator);
             ++i)
          {
            times_and_names.push_back(
              {time.current(),
               basename + Utilities::int_to_string(i, 4) + ".vtu"});
          }
        std::ofstream pvd_output("navierstokes.pvd");
        DataOutBase::write_pvd_record(pvd_output, times_and_names);
      }
  }

  template class ParallelNavierStokes<2>;
  template class ParallelNavierStokes<3>;
}