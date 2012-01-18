// This file is part of Hermes2D.
//
// Hermes2D is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// Hermes2D is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Hermes2D.  If not, see <http://www.gnu.org/licenses/>.

#include "global.h"
#include "integrals/h1.h"
#include "quadrature/limit_order.h"
#include "discrete_problem.h"
#include "mesh/traverse.h"
#include "space/space.h"
#include "shapeset/precalc.h"
#include "mesh/refmap.h"
#include "function/solution.h"
#include "neighbor.h"

using namespace Hermes::Algebra::DenseMatrixOperations;

namespace Hermes
{
  namespace Hermes2D
  {
    template<typename Scalar>
    double DiscreteProblem<Scalar>::fake_wt = 1.0;

    template<typename Scalar>
    DiscreteProblem<Scalar>::DiscreteProblem(const WeakForm<Scalar>* wf, Hermes::vector<const Space<Scalar> *> spaces) : wf(wf), wf_seq(-1)
    {
      _F_;
      if (spaces.empty()) throw Exceptions::NullException(2);
      unsigned int first_dof_running = 0;
      for(unsigned int i = 0; i < spaces.size(); i++)
      {
        this->spaces.push_back(spaces.at(i));
        this->spaces_first_dofs.push_back(first_dof_running);
        first_dof_running += spaces.at(i)->get_num_dofs();
      }
      init();
    }

    template<typename Scalar>
    DiscreteProblem<Scalar>::DiscreteProblem(const WeakForm<Scalar>* wf, const Space<Scalar>* space)
      : wf(wf), wf_seq(-1)
    {
      _F_;
      spaces.push_back(space);
      this->spaces_first_dofs.push_back(0);

      init();
    }

    template<typename Scalar>
    DiscreteProblem<Scalar>::DiscreteProblem() : wf(NULL)
    {
      // Set all attributes for which we don't need to acces wf or spaces.
      // This is important for the destructor to properly detect what needs to be deallocated.
      sp_seq = NULL;
      is_fvm = false;
      RungeKutta = false;
      RK_original_spaces_count = 0;
      have_matrix = false;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init()
    {
      _F_

        // Initialize special variable for Runge-Kutta time integration.
        RungeKutta = false;
      RK_original_spaces_count = 0;

      ndof = Space<Scalar>::get_num_dofs(spaces);

      // Sanity checks.
      if(wf == NULL)
        error("WeakForm<Scalar>* wf can not be NULL in DiscreteProblem<Scalar>::DiscreteProblem.");

      if (spaces.size() != (unsigned) wf->get_neq())
        error("Bad number of spaces in DiscreteProblem.");
      if (spaces.size() == 0)
        error("Zero number of spaces in DiscreteProblem.");

      // Internal variables settings.
      sp_seq = new int[wf->get_neq()];
      memset(sp_seq, -1, sizeof(int) * wf->get_neq());

      // Matrix<Scalar> related settings.
      have_matrix = false;

      // There is a special function that sets a DiscreteProblem to be FVM.
      // Purpose is that this constructor looks cleaner and is simpler.
      this->is_fvm = false;

      this->DG_matrix_forms_present = false;
      this->DG_vector_forms_present = false;

      for(unsigned int i = 0; i < this->wf->mfsurf.size(); i++)
        if (this->wf->mfsurf[i]->areas[0] == H2D_DG_INNER_EDGE)
          this->DG_matrix_forms_present = true;

      for(unsigned int i = 0; i < this->wf->vfsurf.size(); i++)
        if (this->wf->vfsurf[i]->areas[0] == H2D_DG_INNER_EDGE)
          this->DG_vector_forms_present = true;

      Geom<Hermes::Ord> *tmp = init_geom_ord();
      geom_ord = *tmp;
      delete tmp;

      current_mat = NULL;
      current_rhs = NULL;
      current_block_weights = NULL;
    }

    template<typename Scalar>
    DiscreteProblem<Scalar>::~DiscreteProblem()
    {
      _F_;
      if (wf != NULL)
        memset(sp_seq, -1, sizeof(int) * wf->get_neq());
      wf_seq = -1;
      if (sp_seq != NULL) delete [] sp_seq;
    }

    template<typename Scalar>
    int DiscreteProblem<Scalar>::get_num_dofs()
    {
      _F_;
      ndof = 0;
      for (unsigned int i = 0; i < wf->get_neq(); i++)
        ndof += spaces[i]->get_num_dofs();
      return ndof;
    }

    template<typename Scalar>
    const Space<Scalar>* DiscreteProblem<Scalar>::get_space(int n)
    {
      return this->spaces[n];
    }

    template<typename Scalar>
    const WeakForm<Scalar>* DiscreteProblem<Scalar>::get_weak_formulation()
    {
      return this->wf;
    }

    template<typename Scalar>
    Hermes::vector<const Space<Scalar>*> DiscreteProblem<Scalar>::get_spaces()
    {
      return this->spaces;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::is_matrix_free()
    {
      return wf->is_matrix_free();
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::is_up_to_date()
    {
      _F_;
      // check if we can reuse the matrix structure
      bool up_to_date = true;
      if (!have_matrix)
        up_to_date = false;

      for (unsigned int i = 0; i < wf->get_neq(); i++)
      {
        if (spaces[i]->get_seq() != sp_seq[i])
        {
          up_to_date = false;
          break;
        }
      }

      if (wf->get_seq() != wf_seq)
        up_to_date = false;

      return up_to_date;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::invalidate_matrix()
    {
      have_matrix = false;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::set_fvm()
    {
      this->is_fvm = true;
    }

    template<typename Scalar>
    double DiscreteProblem<Scalar>::block_scaling_coeff(MatrixForm<Scalar>* form)
    {
      if(current_block_weights != NULL)
        return current_block_weights->get_A(form->i, form->j);
      return 1.0;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::form_to_be_assembled(MatrixForm<Scalar>* form, Traverse::State* current_state)
    {
      if (current_state->e[form->i] == NULL || current_state->e[form->j] == NULL)
        return false;
      if (fabs(form->scaling_factor) < 1e-12)
        return false;

      // If a block scaling table is provided, and if the scaling coefficient
      // A_mn for this block is zero, then the form does not need to be assembled.
      if (current_block_weights != NULL)
        if (fabs(current_block_weights->get_A(form->i, form->j)) < 1e-12)
          return false;
      return true;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::form_to_be_assembled(MatrixFormVol<Scalar>* form, Traverse::State* current_state)
    {
      if(!form_to_be_assembled((MatrixForm<Scalar>*)form, current_state))
        return false;

      // Assemble this form only if one of its areas is HERMES_ANY
      // of if the element marker coincides with one of the form's areas.
      bool assemble_this_form = false;
      for (unsigned int ss = 0; ss < form->areas.size(); ss++)
      {
        if(form->areas[ss] == HERMES_ANY)
        {
          assemble_this_form = true;
          break;
        }
        else
        {
          bool marker_on_space_m = this->spaces[form->i]->get_mesh()->get_element_markers_conversion().get_internal_marker(form->areas[ss]).valid;
          if(marker_on_space_m)
            marker_on_space_m = (this->spaces[form->i]->get_mesh()->get_element_markers_conversion().get_internal_marker(form->areas[ss]).marker == current_state->rep->marker);

          bool marker_on_space_n = this->spaces[form->j]->get_mesh()->get_element_markers_conversion().get_internal_marker(form->areas[ss]).valid;
          if(marker_on_space_n)
            marker_on_space_n = (this->spaces[form->j]->get_mesh()->get_element_markers_conversion().get_internal_marker(form->areas[ss]).marker == current_state->rep->marker);

          if (marker_on_space_m && marker_on_space_n)
          {
            assemble_this_form = true;
            break;
          }
        }
      }
      if (!assemble_this_form)
        return false;
      return true;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::form_to_be_assembled(MatrixFormSurf<Scalar>* form, Traverse::State* current_state)
    {
      if(current_state->rep->en[current_state->isurf]->marker == 0)
        return false;

      if (form->areas[0] == H2D_DG_INNER_EDGE)
        return false;
      if(!form_to_be_assembled((MatrixForm<Scalar>*)form, current_state))
        return false;

      bool assemble_this_form = false;
      for (unsigned int ss = 0; ss < form->areas.size(); ss++)
      {
        if(form->areas[ss] == HERMES_ANY || form->areas[ss] == H2D_DG_BOUNDARY_EDGE)
        {
          assemble_this_form = true;
          break;
        }
        else
        {
          bool marker_on_space_m = this->spaces[form->i]->get_mesh()->get_boundary_markers_conversion().get_internal_marker(form->areas[ss]).valid;
          if(marker_on_space_m)
            marker_on_space_m = (this->spaces[form->i]->get_mesh()->get_boundary_markers_conversion().get_internal_marker(form->areas[ss]).marker == current_state->rep->en[current_state->isurf]->marker);

          bool marker_on_space_n = this->spaces[form->j]->get_mesh()->get_boundary_markers_conversion().get_internal_marker(form->areas[ss]).valid;
          if(marker_on_space_n)
            marker_on_space_n = (this->spaces[form->j]->get_mesh()->get_boundary_markers_conversion().get_internal_marker(form->areas[ss]).marker == current_state->rep->en[current_state->isurf]->marker);

          if (marker_on_space_m && marker_on_space_n)
          {
            assemble_this_form = true;
            break;
          }
        }
      }
      if (assemble_this_form == false)
        return false;
      return true;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::form_to_be_assembled(VectorForm<Scalar>* form, Traverse::State* current_state)
    {
      if (current_state->e[form->i] == NULL)
        return false;
      if (fabs(form->scaling_factor) < 1e-12)
        return false;

      return true;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::form_to_be_assembled(VectorFormVol<Scalar>* form, Traverse::State* current_state)
    {
      if(!form_to_be_assembled((VectorForm<Scalar>*)form, current_state))
        return false;

      // Assemble this form only if one of its areas is HERMES_ANY
      // of if the element marker coincides with one of the form's areas.
      bool assemble_this_form = false;
      for (unsigned int ss = 0; ss < form->areas.size(); ss++)
      {
        if(form->areas[ss] == HERMES_ANY)
        {
          assemble_this_form = true;
          break;
        }
        else
        {
          bool marker_on_space_m = this->spaces[form->i]->get_mesh()->get_element_markers_conversion().get_internal_marker(form->areas[ss]).valid;
          if(marker_on_space_m)
            marker_on_space_m = (this->spaces[form->i]->get_mesh()->get_element_markers_conversion().get_internal_marker(form->areas[ss]).marker == current_state->rep->marker);

          if (marker_on_space_m)
          {
            assemble_this_form = true;
            break;
          }
        }
      }
      if (!assemble_this_form)
        return false;
      return true;
    }

    template<typename Scalar>
    bool DiscreteProblem<Scalar>::form_to_be_assembled(VectorFormSurf<Scalar>* form, Traverse::State* current_state)
    {
      if(current_state->rep->en[current_state->isurf]->marker == 0)
        return false;

      if (form->areas[0] == H2D_DG_INNER_EDGE)
        return false;

      if(!form_to_be_assembled((VectorForm<Scalar>*)form, current_state))
        return false;

      bool assemble_this_form = false;
      for (unsigned int ss = 0; ss < form->areas.size(); ss++)
      {
        if(form->areas[ss] == HERMES_ANY || form->areas[ss] == H2D_DG_BOUNDARY_EDGE)
        {
          assemble_this_form = true;
          break;
        }
        else
        {
          bool marker_on_space_m = this->spaces[form->i]->get_mesh()->get_boundary_markers_conversion().get_internal_marker(form->areas[ss]).valid;
          if(marker_on_space_m)
            marker_on_space_m = (this->spaces[form->i]->get_mesh()->get_boundary_markers_conversion().get_internal_marker(form->areas[ss]).marker == current_state->rep->en[current_state->isurf]->marker);

          if (marker_on_space_m)
          {
            assemble_this_form = true;
            break;
          }
        }
      }
      if (assemble_this_form == false)
        return false;
      return true;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::create_sparse_structure(SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs)
    {
      this->current_mat = mat;
      if(rhs != NULL)
        this->current_rhs = rhs;
      this->create_sparse_structure();
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::create_sparse_structure()
    {
      _F_;

      if (is_up_to_date())
      {
        if (current_mat != NULL)
        {
          verbose("Reusing matrix sparse structure.");
          current_mat->zero();
        }
        if (current_rhs != NULL)
        {
          // If we use e.g. a new NewtonSolver (providing a new Vector) for this instance of DiscreteProblem that already assembled a system,
          // we end up with everything up_to_date, but unallocated Vector.
          if(current_rhs->length() == 0)
            current_rhs->alloc(ndof);
          else
            current_rhs->zero();
        }
        return;
      }

      // For DG, the sparse structure is different as we have to
      // account for over-edge calculations.
      bool is_DG = false;
      for(unsigned int i = 0; i < this->wf->mfsurf.size(); i++)
      {
        if(this->wf->mfsurf[i]->areas[0] == H2D_DG_INNER_EDGE)
        {
          is_DG = true;
          break;
        }
      }
      for(unsigned int i = 0; i < this->wf->vfsurf.size() && is_DG == false; i++)
      {
        if(this->wf->vfsurf[i]->areas[0] == H2D_DG_INNER_EDGE)
        {
          is_DG = true;
          break;
        }
      }

      if (current_mat != NULL)
      {
        // Spaces have changed: create the matrix from scratch.
        have_matrix = true;
        current_mat->free();
        current_mat->prealloc(ndof);

        AsmList<Scalar>* al = new AsmList<Scalar>[wf->get_neq()];
        Mesh** meshes = new Mesh*[wf->get_neq()];
        bool **blocks = wf->get_blocks(current_force_diagonal_blocks);

        // Init multi-mesh traversal.
        for (unsigned int i = 0; i < wf->get_neq(); i++)
          meshes[i] = spaces[i]->get_mesh();

        Traverse trav(true);
        trav.begin(wf->get_neq(), meshes);

        if(is_DG)
        {
          Hermes::vector<Space<Scalar>*> mutable_spaces;
          for(unsigned int i = 0; i < this->spaces.size(); i++)
          {
            mutable_spaces.push_back(const_cast<Space<Scalar>*>(spaces.at(i)));
            spaces_first_dofs[i] = 0;
          }
          Space<Scalar>::assign_dofs(mutable_spaces);
        }

        Traverse::State* current_state;
        // Loop through all elements.
        while ((current_state = trav.get_next_state()) != NULL)
        {
          // Obtain assembly lists for the element at all spaces.
          /// \todo do not get the assembly list again if the element was not changed.
          for (unsigned int i = 0; i < wf->get_neq(); i++)
            if (current_state->e[i] != NULL)
              if(is_DG)
                spaces[i]->get_element_assembly_list(current_state->e[i], &(al[i]));
              else
                spaces[i]->get_element_assembly_list(current_state->e[i], &(al[i]), spaces_first_dofs[i]);

          if(is_DG)
          {
            // Number of edges ( =  number of vertices).
            int num_edges = current_state->e[0]->get_num_surf();

            // Allocation an array of arrays of neighboring elements for every mesh x edge.
            Element **** neighbor_elems_arrays = new Element *** [wf->get_neq()];
            for(unsigned int i = 0; i < wf->get_neq(); i++)
              neighbor_elems_arrays[i] = new Element ** [num_edges];

            // The same, only for number of elements
            int ** neighbor_elems_counts = new int * [wf->get_neq()];
            for(unsigned int i = 0; i < wf->get_neq(); i++)
              neighbor_elems_counts[i] = new int [num_edges];

            // Get the neighbors.
            for(unsigned int el = 0; el < wf->get_neq(); el++)
            {
              NeighborSearch<Scalar> ns(current_state->e[el], meshes[el]);

              // Ignoring errors (and doing nothing) in case the edge is a boundary one.
              ns.set_ignore_errors(true);

              for(int ed = 0; ed < num_edges; ed++)
              {
                ns.set_active_edge(ed);
                const Hermes::vector<Element *> *neighbors = ns.get_neighbors();

                neighbor_elems_counts[el][ed] = ns.get_num_neighbors();
                neighbor_elems_arrays[el][ed] = new Element * [neighbor_elems_counts[el][ed]];
                for(int neigh = 0; neigh < neighbor_elems_counts[el][ed]; neigh++)
                  neighbor_elems_arrays[el][ed][neigh] = (*neighbors)[neigh];
              }
            }

            // Pre-add into the stiffness matrix.
            for (unsigned int m = 0; m < wf->get_neq(); m++)
              for(unsigned int el = 0; el < wf->get_neq(); el++)
                for(int ed = 0; ed < num_edges; ed++)
                  for(int neigh = 0; neigh < neighbor_elems_counts[el][ed]; neigh++)
                    if ((blocks[m][el] || blocks[el][m]) && current_state->e[m] != NULL)
                    {
                      AsmList<Scalar>*am = &(al[m]);
                      AsmList<Scalar>*an = new AsmList<Scalar>;
                      spaces[el]->get_element_assembly_list(neighbor_elems_arrays[el][ed][neigh], an);

                      // pretend assembling of the element stiffness matrix
                      // register nonzero elements
                      for (unsigned int i = 0; i < am->cnt; i++)
                        if (am->dof[i] >= 0)
                          for (unsigned int j = 0; j < an->cnt; j++)
                            if (an->dof[j] >= 0)
                            {
                              if(blocks[m][el]) current_mat->pre_add_ij(am->dof[i], an->dof[j]);
                              if(blocks[el][m]) current_mat->pre_add_ij(an->dof[j], am->dof[i]);
                            }
                            delete an;
                    }

                    // Deallocation an array of arrays of neighboring elements
                    // for every mesh x edge.
                    for(unsigned int el = 0; el < wf->get_neq(); el++)
                    {
                      for(int ed = 0; ed < num_edges; ed++)
                        delete [] neighbor_elems_arrays[el][ed];
                      delete [] neighbor_elems_arrays[el];
                    }
                    delete [] neighbor_elems_arrays;

                    // The same, only for number of elements.
                    for(unsigned int el = 0; el < wf->get_neq(); el++)
                      delete [] neighbor_elems_counts[el];
                    delete [] neighbor_elems_counts;
          }

          // Go through all equation-blocks of the local stiffness matrix.
          for (unsigned int m = 0; m < wf->get_neq(); m++)
          {
            for (unsigned int n = 0; n < wf->get_neq(); n++)
            {
              if (blocks[m][n] && current_state->e[m] != NULL && current_state->e[n] != NULL)
              {
                AsmList<Scalar>*am = &(al[m]);
                AsmList<Scalar>*an = &(al[n]);

                // Pretend assembling of the element stiffness matrix.
                for (unsigned int i = 0; i < am->cnt; i++)
                  if (am->dof[i] >= 0)
                    for (unsigned int j = 0; j < an->cnt; j++)
                      if (an->dof[j] >= 0)
                        current_mat->pre_add_ij(am->dof[i], an->dof[j]);
              }
            }
          }
        }

        trav.finish();
        delete [] al;
        delete [] meshes;
        delete [] blocks;

        current_mat->alloc();
      }

      // WARNING: unlike Matrix<Scalar>::alloc(), Vector<Scalar>::alloc(ndof) frees the memory occupied
      // by previous vector before allocating
      if (current_rhs != NULL)
        current_rhs->alloc(ndof);

      // save space seq numbers and weakform seq number, so we can detect their changes
      for (unsigned int i = 0; i < wf->get_neq(); i++)
        sp_seq[i] = spaces[i]->get_seq();

      wf_seq = wf->get_seq();
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble(SparseMatrix<Scalar>* mat, Vector<Scalar>* rhs,
      bool force_diagonal_blocks, Table* block_weights)
    {
      _F_;
      Scalar* coeff_vec = NULL;
      assemble(coeff_vec, mat, rhs, force_diagonal_blocks, block_weights);
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble(Vector<Scalar>* rhs,
      bool force_diagonal_blocks, Table* block_weights)
    {
      _F_;
      assemble(NULL, NULL, rhs, force_diagonal_blocks, block_weights);
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init_assembling(Scalar* coeff_vec, PrecalcShapeset*** pss , PrecalcShapeset*** spss, RefMap*** refmaps, Solution<Scalar>*** u_ext, AsmList<Scalar>*** als, Hermes::vector<MeshFunction<Scalar>*>& ext_functions, MeshFunction<Scalar>*** ext, 
      Hermes::vector<MatrixFormVol<Scalar>*>* mfvol, Hermes::vector<MatrixFormSurf<Scalar>*>* mfsurf, Hermes::vector<VectorFormVol<Scalar>*>* vfvol, Hermes::vector<VectorFormSurf<Scalar>*>* vfsurf)
    {
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        pss[i] = new PrecalcShapeset*[wf->get_neq()];
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          pss[i][j] = new PrecalcShapeset(spaces[j]->shapeset);
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        spss[i] = new PrecalcShapeset*[wf->get_neq()];
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          spss[i][j] = new PrecalcShapeset(pss[i][j]);
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        refmaps[i] = new RefMap*[wf->get_neq()];
        for (unsigned int j = 0; j < wf->get_neq(); j++)
        {
          refmaps[i][j] = new RefMap();
          refmaps[i][j]->set_quad_2d(&g_quad_2d_std);
        }
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        if (coeff_vec != NULL)
        {
          u_ext[i] = new Solution<Scalar>*[wf->get_neq()];
          if(i == 0)
          {
            int first_dof = 0;
            for (int j = 0; j < wf->get_neq(); j++)
            {
              u_ext[i][j] = new Solution<Scalar>(spaces[j]->get_mesh());
              Solution<Scalar>::vector_to_solution(coeff_vec, spaces[j], u_ext[i][j], !RungeKutta, first_dof);
              first_dof += spaces[j]->get_num_dofs();
            }
          }
          else
          {
            for (int j = 0; j < wf->get_neq(); j++)
            {
              u_ext[i][j] = new Solution<Scalar>(spaces[j]->get_mesh());
              u_ext[i][j]->copy(u_ext[0][j]);
            }
          }
        }
        else
          u_ext[i] = NULL;
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        als[i] = new AsmList<Scalar>*[wf->get_neq()];
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          als[i][j] = new AsmList<Scalar>();
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        ext[i] = new MeshFunction<Scalar>*[ext_functions.size()];
        for (int j = 0; j < ext_functions.size(); j++)
          ext[i][j] = ext_functions[j]->clone();
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->mfvol.size(); j++)
        {
          mfvol[i].push_back(wf->mfvol[j]->clone());
          // Inserting proper ext.
          for(int k = 0; k < wf->mfvol[j]->ext.size(); k++)
          {
            for (int l = 0; l < ext_functions.size(); l++)
            {
              if(ext_functions[l] == wf->mfvol[j]->ext[k])
              {
                while(k >= mfvol[i][j]->ext.size())
                  mfvol[i][j]->ext.push_back(NULL);
                mfvol[i][j]->ext[k] = ext[i][l];
                break;
              }
            }
          }
        }
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->mfsurf.size(); j++)
        {
          mfsurf[i].push_back(wf->mfsurf[j]->clone());
          // Inserting proper ext.
          for(int k = 0; k < wf->mfsurf[j]->ext.size(); k++)
          {
            for (int l = 0; l < ext_functions.size(); l++)
            {
              if(ext_functions[l] == wf->mfsurf[j]->ext[k])
              {
                while(k >= mfsurf[i][j]->ext.size())
                  mfsurf[i][j]->ext.push_back(NULL);
                mfsurf[i][j]->ext[k] = ext[i][l];
                break;
              }
            }
          }
        }
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->vfvol.size(); j++)
        {
          vfvol[i].push_back(wf->vfvol[j]->clone());
          // Inserting proper ext.
          for(int k = 0; k < wf->vfvol[j]->ext.size(); k++)
          {
            for (int l = 0; l < ext_functions.size(); l++)
            {
              if(ext_functions[l] == wf->vfvol[j]->ext[k])
              {
                while(k >= vfvol[i][j]->ext.size())
                  vfvol[i][j]->ext.push_back(NULL);

                vfvol[i][j]->ext[k] = ext[i][l];
                break;
              }
            }
          }
        }
      }
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->vfsurf.size(); j++)
        {
          vfsurf[i].push_back(wf->vfsurf[j]->clone());
          // Inserting proper ext.
          for(int k = 0; k < wf->vfsurf[j]->ext.size(); k++)
          {
            for (int l = 0; l < ext_functions.size(); l++)
            {
              if(ext_functions[l] == wf->vfsurf[j]->ext[k])
              {
                while(k >= vfsurf[i][j]->ext.size())
                  vfsurf[i][j]->ext.push_back(NULL);
                vfsurf[i][j]->ext[k] = ext[i][l];
                break;
              }
            }
          }
        }
      }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::deinit_assembling(PrecalcShapeset*** pss , PrecalcShapeset*** spss, RefMap*** refmaps, Solution<Scalar>*** u_ext, AsmList<Scalar>*** als, Hermes::vector<MeshFunction<Scalar>*>& ext_functions, MeshFunction<Scalar>*** ext, 
      Hermes::vector<MatrixFormVol<Scalar>*>* mfvol, Hermes::vector<MatrixFormSurf<Scalar>*>* mfsurf, Hermes::vector<VectorFormVol<Scalar>*>* vfvol, Hermes::vector<VectorFormSurf<Scalar>*>* vfsurf)
    {
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          delete pss[i][j];
        delete [] pss[i];
      }
      delete [] pss;

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          delete spss[i][j];
        delete [] spss[i];
      }
      delete [] spss;

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          delete refmaps[i][j];
        delete [] refmaps[i];
      }
      delete [] refmaps;

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        if(u_ext[i] != NULL)
        {
          for (unsigned int j = 0; j < wf->get_neq(); j++)
            delete u_ext[i][j];
          delete [] u_ext[i];
        }
      }
      delete [] u_ext;

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (unsigned int j = 0; j < wf->get_neq(); j++)
          delete als[i][j];
        delete [] als[i];
      }
      delete [] als;

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (unsigned int j = 0; j < ext_functions.size(); j++)
          delete ext[i][j];
        delete [] ext[i];
      }
      delete [] ext;

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->mfvol.size(); j++)
          delete mfvol[i][j];
        mfvol[i].clear();
      }
      delete [] mfvol;
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->mfsurf.size(); j++)
          delete mfsurf[i][j];
        mfsurf[i].clear();
      }
      delete [] mfsurf;
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->vfvol.size(); j++)
          delete vfvol[i][j];
        vfvol[i].clear();
      }
      delete [] vfvol;
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (int j = 0; j < wf->vfsurf.size(); j++)
          delete vfsurf[i][j];
        vfsurf[i].clear();
      }
      delete [] vfsurf;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble(Scalar* coeff_vec, SparseMatrix<Scalar>* mat,
      Vector<Scalar>* rhs,
      bool force_diagonal_blocks,
      Table* block_weights)
    {
      _F_;

      current_mat = mat;
      current_rhs = rhs;
      current_force_diagonal_blocks = force_diagonal_blocks;
      current_block_weights = block_weights;

      // Check that the block scaling table have proper dimension.
      if (block_weights != NULL)
        if (block_weights->get_size() != wf->get_neq())
          throw Exceptions::LengthException(6, block_weights->get_size(), wf->get_neq());

      // Creating matrix sparse structure.
      create_sparse_structure();

      Hermes::vector<MeshFunction<Scalar>*> ext_functions;
      for(unsigned int form_i = 0; form_i < wf->mfvol.size(); form_i++)
        for(unsigned int ext_i = 0; ext_i < wf->mfvol.at(form_i)->ext.size(); ext_i++)
          ext_functions.push_back(wf->mfvol.at(form_i)->ext[ext_i]);
      for(unsigned int form_i = 0; form_i < wf->mfsurf.size(); form_i++)
        for(unsigned int ext_i = 0; ext_i < wf->mfsurf.at(form_i)->ext.size(); ext_i++)
          ext_functions.push_back(wf->mfsurf.at(form_i)->ext[ext_i]);
      for(unsigned int form_i = 0; form_i < wf->vfvol.size(); form_i++)
        for(unsigned int ext_i = 0; ext_i < wf->vfvol.at(form_i)->ext.size(); ext_i++)
          ext_functions.push_back(wf->vfvol.at(form_i)->ext[ext_i]);
      for(unsigned int form_i = 0; form_i < wf->vfsurf.size(); form_i++)
        for(unsigned int ext_i = 0; ext_i < wf->vfsurf.at(form_i)->ext.size(); ext_i++)
          ext_functions.push_back(wf->vfsurf.at(form_i)->ext[ext_i]);

      // Structures that cloning will be done into.
      PrecalcShapeset*** pss = new PrecalcShapeset**[omp_get_max_threads()];
      PrecalcShapeset*** spss = new PrecalcShapeset**[omp_get_max_threads()];
      RefMap*** refmaps = new RefMap**[omp_get_max_threads()];
      Solution<Scalar>*** u_ext = new Solution<Scalar>**[omp_get_max_threads()];
      AsmList<Scalar>*** als = new AsmList<Scalar>**[omp_get_max_threads()];
      MeshFunction<Scalar>*** ext = new MeshFunction<Scalar>**[omp_get_max_threads()];
      Hermes::vector<MatrixFormVol<Scalar>*>* mfvol = new Hermes::vector<MatrixFormVol<Scalar>*>[omp_get_max_threads()];
      Hermes::vector<MatrixFormSurf<Scalar>*>* mfsurf = new Hermes::vector<MatrixFormSurf<Scalar>*>[omp_get_max_threads()];
      Hermes::vector<VectorFormVol<Scalar>*>* vfvol = new Hermes::vector<VectorFormVol<Scalar>*>[omp_get_max_threads()];
      Hermes::vector<VectorFormSurf<Scalar>*>* vfsurf = new Hermes::vector<VectorFormSurf<Scalar>*>[omp_get_max_threads()];

      // Fill these structures.
      init_assembling(coeff_vec, pss, spss, refmaps, u_ext, als, ext_functions, ext, mfvol, mfsurf, vfvol, vfsurf);

      // Vector of meshes.
      Hermes::vector<Mesh*> meshes;
      for(unsigned int space_i = 0; space_i < spaces.size(); space_i++)
        meshes.push_back(spaces[space_i]->get_mesh());
      for (unsigned j = 0; j < ext_functions.size(); j++)
        meshes.push_back(ext_functions[j]->get_mesh());
      if (coeff_vec != NULL)
        for(unsigned int space_i = 0; space_i < spaces.size(); space_i++)
          meshes.push_back(spaces[space_i]->get_mesh());

      Traverse trav_master(true);
      unsigned int num_states = trav_master.get_num_states(meshes);

      trav_master.begin(meshes.size(), &(meshes.front()));

      Traverse* trav = new Traverse[omp_get_max_threads()];
      Hermes::vector<Transformable *>* fns = new Hermes::vector<Transformable *>[omp_get_max_threads()];
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        for (unsigned j = 0; j < spaces.size(); j++)
          fns[i].push_back(pss[i][j]);
        for (unsigned j = 0; j < ext_functions.size(); j++)
        {
          fns[i].push_back(ext[i][j]);
          ext[i][j]->set_quad_2d(&g_quad_2d_std);
        }
        if (coeff_vec != NULL)
          for (unsigned j = 0; j < wf->get_neq(); j++)
          {
            fns[i].push_back(u_ext[i][j]);
            u_ext[i][j]->set_quad_2d(&g_quad_2d_std);
          }
          trav[i].begin(meshes.size(), &(meshes.front()), &(fns[i].front()));
          trav[i].stack = trav_master.stack;
      }

      int state_i;

      PrecalcShapeset** current_pss;
      PrecalcShapeset** current_spss;
      RefMap** current_refmaps;
      Solution<Scalar>** current_u_ext;
      AsmList<Scalar>** current_als;

      MatrixFormVol<Scalar>** current_mfvol;
      MatrixFormSurf<Scalar>** current_mfsurf;
      VectorFormVol<Scalar>** current_vfvol;
      VectorFormSurf<Scalar>** current_vfsurf;

#define CHUNKSIZE 1
#pragma omp parallel shared(trav_master, mat, rhs) private(state_i, current_pss, current_spss, current_refmaps, current_u_ext, current_als, current_mfvol, current_mfsurf, current_vfvol, current_vfsurf)
      {
#pragma omp for schedule(dynamic, CHUNKSIZE)
        for(state_i = 0; state_i < num_states; state_i++)
        {
          Traverse::State current_state;
#pragma omp critical (get_next_state)
          {
            current_state = trav[omp_get_thread_num()].get_next_state(&trav_master.top, &trav_master.id);

            // Mark the active element on each mesh in order to prevent assembling on its edges from the other side.
            if(DG_matrix_forms_present || DG_vector_forms_present)
              for(unsigned int i = 0; i < current_state.num; i++)
                current_state.e[i]->visited = true;
          }

          current_pss = pss[omp_get_thread_num()];
          current_spss = spss[omp_get_thread_num()];
          current_refmaps = refmaps[omp_get_thread_num()];
          current_u_ext = u_ext[omp_get_thread_num()];
          current_als = als[omp_get_thread_num()];

          current_mfvol = mfvol[omp_get_thread_num()].size() == 0 ? NULL : &(mfvol[omp_get_thread_num()].front());
          current_mfsurf = mfsurf[omp_get_thread_num()].size() == 0 ? NULL : &(mfsurf[omp_get_thread_num()].front());
          current_vfvol = vfvol[omp_get_thread_num()].size() == 0 ? NULL : &(vfvol[omp_get_thread_num()].front());
          current_vfsurf = vfsurf[omp_get_thread_num()].size() == 0 ? NULL : &(vfsurf[omp_get_thread_num()].front());

          // One state is a collection of (virtual) elements sharing
          // the same physical location on (possibly) different meshes.
          // This is then the same element of the virtual union mesh.
          // The proper sub-element mappings to all the functions of
          // this stage is supplied by the function Traverse::get_next_state()
          // called in the while loop.
          assemble_one_state(current_pss, current_spss, current_refmaps, current_u_ext, current_als, &current_state, current_mfvol, current_mfsurf, current_vfvol, current_vfsurf);

          if(DG_matrix_forms_present || DG_vector_forms_present)
            assemble_one_DG_state(current_pss, current_spss, current_refmaps, current_u_ext, current_als, &current_state, current_mfsurf, current_vfsurf, trav[omp_get_thread_num()].fn);
        }
      }

      deinit_assembling(pss, spss, refmaps, u_ext, als, ext_functions, ext, mfvol, mfsurf, vfvol, vfsurf);

      trav_master.finish();
      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
        trav[i].finish();

      for(unsigned int i = 0; i < omp_get_max_threads(); i++)
      {
        fns[i].clear();
      }
      delete [] fns;
      delete [] trav;

      /// \todo Should this be really here? Or in assemble()?
      if (current_mat != NULL)
        current_mat->finish();
      if (current_rhs != NULL)
        current_rhs->finish();

      if(DG_matrix_forms_present || DG_vector_forms_present)
      {
        Element* element_to_set_nonvisited;
        for(unsigned int mesh_i = 0; mesh_i < meshes.size(); mesh_i++)
          for_all_elements(element_to_set_nonvisited, meshes[mesh_i])
          element_to_set_nonvisited->visited = false;
      }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble(Scalar* coeff_vec, Vector<Scalar>* rhs,
      bool force_diagonal_blocks, Table* block_weights)
    {
      _F_;
      assemble(coeff_vec, NULL, rhs, force_diagonal_blocks, block_weights);
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init_state(PrecalcShapeset** current_pss, PrecalcShapeset** current_spss, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, Traverse::State* current_state)
    {
      _F_;

      // Obtain assembly lists for the element at all spaces of the stage, set appropriate mode for each pss.
      // NOTE: Active elements and transformations for external functions (including the solutions from previous
      // Newton's iteration) as well as basis functions (master PrecalcShapesets) have already been set in
      // trav.get_next_state(...).
      for (unsigned int i = 0; i < spaces.size(); i++)
      {
        if (current_state->e[i] == NULL)
          continue;

        // \todo do not obtain again if the element was not changed.
        spaces[i]->get_element_assembly_list(current_state->e[i], current_als[i], spaces_first_dofs[i]);

        // Set active element to all test functions.
        current_spss[i]->set_active_element(current_state->e[i]);
        current_spss[i]->set_master_transform();

        // Set active element to reference mappings.
        current_refmaps[i]->set_active_element(current_state->e[i]);
        current_refmaps[i]->force_transform(current_pss[i]->get_transform(), current_pss[i]->get_ctm());

      }
      return;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init_surface_state(AsmList<Scalar>** current_als, Traverse::State* current_state)
    {
      _F_;
      // Obtain the list of shape functions which are nonzero on this surface.
      for (unsigned int i = 0; i < spaces.size(); i++)
      {
        if (current_state->e[i] == NULL)
          continue;

        spaces[i]->get_boundary_assembly_list(current_state->e[i], current_state->isurf, current_als[i], spaces_first_dofs[i]);
      }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble_one_state(PrecalcShapeset** current_pss, PrecalcShapeset** current_spss, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, Traverse::State* current_state,
      MatrixFormVol<Scalar>** current_mfvol, MatrixFormSurf<Scalar>** current_mfsurf, VectorFormVol<Scalar>** current_vfvol, VectorFormSurf<Scalar>** current_vfsurf)
    {
      _F_;

      // Initialize the state, return a non-NULL element; if no such element found, return.
      init_state(current_pss, current_spss, current_refmaps, current_u_ext, current_als, current_state);

      if (current_mat != NULL)
      {
        for(int current_mfvol_i = 0; current_mfvol_i < wf->mfvol.size(); current_mfvol_i++)
        {
          if(!form_to_be_assembled(current_mfvol[current_mfvol_i], current_state))
            continue;

          Func<double>** base_fns = new Func<double>*[current_als[current_mfvol[current_mfvol_i]->j]->cnt];
          Func<double>** test_fns = new Func<double>*[current_als[current_mfvol[current_mfvol_i]->i]->cnt];

          int order = calc_order_matrix_form(current_mfvol[current_mfvol_i], current_refmaps, current_u_ext, current_state);

          for (unsigned int i = 0; i < current_als[current_mfvol[current_mfvol_i]->i]->cnt; i++)
          {
            if (std::abs(current_als[current_mfvol[current_mfvol_i]->i]->coef[i]) < 1e-12)
              continue;
            if (current_als[current_mfvol[current_mfvol_i]->i]->dof[i] >= 0)
            {
              current_spss[current_mfvol[current_mfvol_i]->i]->set_active_shape(current_als[current_mfvol[current_mfvol_i]->i]->idx[i]);
              test_fns[i] = init_fn(current_spss[current_mfvol[current_mfvol_i]->i], current_refmaps[current_mfvol[current_mfvol_i]->i], order);
            }
          }

          for (unsigned int j = 0; j < current_als[current_mfvol[current_mfvol_i]->j]->cnt; j++)
          {
            if (std::abs(current_als[current_mfvol[current_mfvol_i]->j]->coef[j]) < 1e-12)
              continue;
            if (current_als[current_mfvol[current_mfvol_i]->j]->dof[j] >= 0)
            {
              current_pss[current_mfvol[current_mfvol_i]->j]->set_active_shape(current_als[current_mfvol[current_mfvol_i]->j]->idx[j]);

              base_fns[j] = init_fn(current_pss[current_mfvol[current_mfvol_i]->j], current_refmaps[current_mfvol[current_mfvol_i]->j], order);
            }
          }

          assemble_matrix_form(current_mfvol[current_mfvol_i], order, base_fns, test_fns, current_refmaps, current_u_ext, current_als, current_state);

          for (unsigned int j = 0; j < current_als[current_mfvol[current_mfvol_i]->j]->cnt; j++)
            if (std::abs(current_als[current_mfvol[current_mfvol_i]->j]->coef[j]) >= 1e-12)
              if (current_als[current_mfvol[current_mfvol_i]->j]->dof[j] >= 0)
              {
                base_fns[j]->free_fn();
                delete base_fns[j];
              }
              delete [] base_fns;
              for (unsigned int i = 0; i < current_als[current_mfvol[current_mfvol_i]->i]->cnt; i++)
              {
                if (std::abs(current_als[current_mfvol[current_mfvol_i]->i]->coef[i]) >= 1e-12)
                  if (current_als[current_mfvol[current_mfvol_i]->i]->dof[i] >= 0)
                  {
                    test_fns[i]->free_fn();
                    delete test_fns[i];
                  }
              }
              delete [] test_fns;
        }
      }
      if (current_rhs != NULL)
      {
        for(int current_vfvol_i = 0; current_vfvol_i < wf->vfvol.size(); current_vfvol_i++)
        {
          if(!form_to_be_assembled(current_vfvol[current_vfvol_i], current_state))
            continue;

          Func<double>** test_fns = new Func<double>*[current_als[current_vfvol[current_vfvol_i]->i]->cnt];

          int order = calc_order_vector_form(current_vfvol[current_vfvol_i], current_refmaps, current_u_ext, current_state);

          for (unsigned int i = 0; i < current_als[current_vfvol[current_vfvol_i]->i]->cnt; i++)
          {
            if (std::abs(current_als[current_vfvol[current_vfvol_i]->i]->coef[i]) < 1e-12)
              continue;
            if (current_als[current_vfvol[current_vfvol_i]->i]->dof[i] >= 0)
            {
              current_spss[current_vfvol[current_vfvol_i]->i]->set_active_shape(current_als[current_vfvol[current_vfvol_i]->i]->idx[i]);

              test_fns[i] = init_fn(current_spss[current_vfvol[current_vfvol_i]->i], current_refmaps[current_vfvol[current_vfvol_i]->i], order);
            }
          }

          assemble_vector_form(current_vfvol[current_vfvol_i], order, test_fns, current_refmaps, current_u_ext, current_als, current_state);

          for (unsigned int i = 0; i < current_als[current_vfvol[current_vfvol_i]->i]->cnt; i++)
          {
            if (std::abs(current_als[current_vfvol[current_vfvol_i]->i]->coef[i]) >= 1e-12)
              if (current_als[current_vfvol[current_vfvol_i]->i]->dof[i] >= 0)
              {
                test_fns[i]->free_fn();
                delete test_fns[i];
              }
          }
          delete [] test_fns;
        }
      }
      // Assemble surface integrals now: loop through surfaces of the element.
      for (current_state->isurf = 0; current_state->isurf < current_state->rep->get_num_surf(); current_state->isurf++)
      {
        // \todo DG.
        if(!current_state->bnd[current_state->isurf])
          continue;
        init_surface_state(current_als, current_state);

        if (current_mat != NULL)
        {
          for(int current_mfsurf_i = 0; current_mfsurf_i < wf->mfsurf.size(); current_mfsurf_i++)
          {
            if(!form_to_be_assembled(current_mfsurf[current_mfsurf_i], current_state))
              continue;

            Func<double>** base_fns = new Func<double>*[current_als[current_mfsurf[current_mfsurf_i]->j]->cnt];
            Func<double>** test_fns = new Func<double>*[current_als[current_mfsurf[current_mfsurf_i]->i]->cnt];

            int order = calc_order_matrix_form(current_mfsurf[current_mfsurf_i], current_refmaps, current_u_ext, current_state);

            for (unsigned int i = 0; i < current_als[current_mfsurf[current_mfsurf_i]->i]->cnt; i++)
            {
              if (std::abs(current_als[current_mfsurf[current_mfsurf_i]->i]->coef[i]) < 1e-12)
                continue;

              if (current_als[current_mfsurf[current_mfsurf_i]->i]->dof[i] >= 0)
              {
                current_spss[current_mfsurf[current_mfsurf_i]->i]->set_active_shape(current_als[current_mfsurf[current_mfsurf_i]->i]->idx[i]);

                test_fns[i] = init_fn(current_spss[current_mfsurf[current_mfsurf_i]->i], current_refmaps[current_mfsurf[current_mfsurf_i]->i], current_refmaps[current_mfsurf[current_mfsurf_i]->i]->get_quad_2d()->get_edge_points(current_state->isurf, order, current_state->e[0]->get_mode()));
              }
            }

            for (unsigned int j = 0; j < current_als[current_mfsurf[current_mfsurf_i]->j]->cnt; j++)
            {
              if (std::abs(current_als[current_mfsurf[current_mfsurf_i]->j]->coef[j]) < 1e-12)
                continue;
              if (current_als[current_mfsurf[current_mfsurf_i]->j]->dof[j] >= 0)
              {
                current_pss[current_mfsurf[current_mfsurf_i]->j]->set_active_shape(current_als[current_mfsurf[current_mfsurf_i]->j]->idx[j]);

                base_fns[j] = init_fn(current_pss[current_mfsurf[current_mfsurf_i]->j], current_refmaps[current_mfsurf[current_mfsurf_i]->j], current_refmaps[current_mfsurf[current_mfsurf_i]->j]->get_quad_2d()->get_edge_points(current_state->isurf, order,current_state->e[0]->get_mode()));
              }
            }

            assemble_matrix_form(current_mfsurf[current_mfsurf_i], order, base_fns, test_fns, current_refmaps, current_u_ext, current_als, current_state);

            for (unsigned int j = 0; j < current_als[current_mfsurf[current_mfsurf_i]->j]->cnt; j++)
            {
              if (std::abs(current_als[current_mfsurf[current_mfsurf_i]->j]->coef[j]) >= 1e-12)
                if (current_als[current_mfsurf[current_mfsurf_i]->j]->dof[j] >= 0)
                {
                  base_fns[j]->free_fn();
                  delete base_fns[j];
                }
            }
            delete [] base_fns;
            for (unsigned int i = 0; i < current_als[current_mfsurf[current_mfsurf_i]->i]->cnt; i++)
            {
              if (std::abs(current_als[current_mfsurf[current_mfsurf_i]->i]->coef[i]) >= 1e-12)
                if (current_als[current_mfsurf[current_mfsurf_i]->i]->dof[i] >= 0)
                {
                  test_fns[i]->free_fn();
                  delete test_fns[i];
                }
            }
            delete [] test_fns;
          }
        }

        if (current_rhs != NULL)
        {
          for(int current_vfsurf_i = 0; current_vfsurf_i < wf->vfsurf.size(); current_vfsurf_i++)
          {
            if(!form_to_be_assembled(current_vfsurf[current_vfsurf_i], current_state))
              continue;

            Func<double>** test_fns = new Func<double>*[current_als[current_vfsurf[current_vfsurf_i]->i]->cnt];

            int order = calc_order_vector_form(current_vfsurf[current_vfsurf_i], current_refmaps, current_u_ext, current_state);

            for (unsigned int i = 0; i < current_als[current_vfsurf[current_vfsurf_i]->i]->cnt; i++)
            {
              if (std::abs(current_als[current_vfsurf[current_vfsurf_i]->i]->coef[i]) < 1e-12)
                continue;
              if (current_als[current_vfsurf[current_vfsurf_i]->i]->dof[i] >= 0)
              {
                current_spss[current_vfsurf[current_vfsurf_i]->i]->set_active_shape(current_als[current_vfsurf[current_vfsurf_i]->i]->idx[i]);

                test_fns[i] = init_fn(current_spss[current_vfsurf[current_vfsurf_i]->i], current_refmaps[current_vfsurf[current_vfsurf_i]->i], current_refmaps[current_vfsurf[current_vfsurf_i]->i]->get_quad_2d()->get_edge_points(current_state->isurf, order, current_state->e[0]->get_mode()));
              }
            }

            assemble_vector_form(current_vfsurf[current_vfsurf_i], order, test_fns, current_refmaps, current_u_ext, current_als, current_state);

            for (unsigned int i = 0; i < current_als[current_vfsurf[current_vfsurf_i]->i]->cnt; i++)
            {
              if (std::abs(current_als[current_vfsurf[current_vfsurf_i]->i]->coef[i]) >= 1e-12)
                if (current_als[current_vfsurf[current_vfsurf_i]->i]->dof[i] >= 0)
                {
                  test_fns[i]->free_fn();
                  delete test_fns[i];
                }
            }
            delete [] test_fns;
          }
        }
      }
    }

    template<typename Scalar>
    int DiscreteProblem<Scalar>::calc_order_matrix_form(MatrixForm<Scalar> *form, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, Traverse::State* current_state)
    {
      _F_;

      int order;

      if(is_fvm)
        order = current_refmaps[form->i]->get_inv_ref_order();
      else
      {
        // order of solutions from the previous Newton iteration etc..
        Func<Hermes::Ord>** u_ext_ord = new Func<Hermes::Ord>*[RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset];
        ExtData<Hermes::Ord> ext_ord;
        init_ext_orders(form, u_ext_ord, &ext_ord, current_u_ext, current_state);

        // Order of shape functions.
        int max_order_j = this->spaces[form->j]->get_element_order(current_state->e[form->j]->id);
        int max_order_i = this->spaces[form->i]->get_element_order(current_state->e[form->i]->id);
        if(H2D_GET_V_ORDER(max_order_i) > H2D_GET_H_ORDER(max_order_i))
          max_order_i = H2D_GET_V_ORDER(max_order_i);
        else
          max_order_i = H2D_GET_H_ORDER(max_order_i);
        if(H2D_GET_V_ORDER(max_order_j) > H2D_GET_H_ORDER(max_order_j))
          max_order_j = H2D_GET_V_ORDER(max_order_j);
        else
          max_order_j = H2D_GET_H_ORDER(max_order_j);

        for (unsigned int k = 0; k < current_state->rep->get_num_surf(); k++)
        {
          int eo = this->spaces[form->i]->get_edge_order(current_state->e[form->i], k);
          if (eo > max_order_i) 
            max_order_i = eo;
          eo = this->spaces[form->j]->get_edge_order(current_state->e[form->j], k);
          if (eo > max_order_j) 
            max_order_j = eo;
        }

        Func<Hermes::Ord>* ou = init_fn_ord(max_order_j + (spaces[form->j]->get_shapeset()->get_num_components() > 1 ? 1 : 0));
        Func<Hermes::Ord>* ov = init_fn_ord(max_order_i + (spaces[form->i]->get_shapeset()->get_num_components() > 1 ? 1 : 0));

        // Total order of the vector form.
        Hermes::Ord o = form->ord(1, &fake_wt, u_ext_ord, ou, ov, &geom_ord, &ext_ord);

        adjust_order_to_refmaps(form, order, &o, current_refmaps);

        // Cleanup.
        deinit_ext_orders(form, u_ext_ord, &ext_ord);
        ou->free_ord();
        delete ou;
        ov->free_ord();
        delete ov;
      }
      return order;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble_matrix_form(MatrixForm<Scalar>* form, int order, Func<double>** base_fns, Func<double>** test_fns, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, Traverse::State* current_state)
    {
      _F_;
      bool surface_form = (dynamic_cast<MatrixFormVol<Scalar>*>(form) == NULL);

      double block_scaling_coef = this->block_scaling_coeff(form);

      bool tra = (form->i != form->j) && (form->sym != 0);
      bool sym = (form->i == form->j) && (form->sym == 1);

      // Assemble the local stiffness matrix for the form form.
      Scalar **local_stiffness_matrix = new_matrix<Scalar>(std::max(current_als[form->i]->cnt, current_als[form->j]->cnt));

      // Init external functions.
      Func<Scalar>** u_ext = new Func<Scalar>*[RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset];
      ExtData<Scalar> ext;
      init_ext(form, u_ext, &ext, order, current_u_ext, current_state);

      // Add the previous time level solution previously inserted at the back of ext.
      if(RungeKutta)
        for(int ext_i = 0; ext_i < this->RK_original_spaces_count; ext_i++)
          u_ext[ext_i]->add(*ext.fn[form->ext.size() - this->RK_original_spaces_count + ext_i]);

      // Init geometry.
      int n_quadrature_points;
      Geom<double>* geometry = NULL;
      double* jacobian_x_weights = NULL;
      if(surface_form)
        n_quadrature_points = init_surface_geometry_points(current_refmaps[form->i], order, current_state, geometry, jacobian_x_weights);
      else
        n_quadrature_points = init_geometry_points(current_refmaps[form->i], order, geometry, jacobian_x_weights);

      // Actual form-specific calculation.
      for (unsigned int i = 0; i < current_als[form->i]->cnt; i++)
      {
        if (current_als[form->i]->dof[i] < 0)
          continue;

        if ((!tra || surface_form) && current_als[form->i]->dof[i] < 0) 
          continue;
        if(std::abs(current_als[form->i]->coef[i]) < 1e-12)
          continue;
        if (!sym)
        {
          for (unsigned int j = 0; j < current_als[form->j]->cnt; j++)
          {
            if (current_als[form->j]->dof[j] >= 0)
            {
              // Is this necessary, i.e. is there a coefficient smaller than 1e-12?
              if (std::abs(current_als[form->j]->coef[j]) < 1e-12)
                continue;

              Func<double>* u = base_fns[j];
              Func<double>* v = test_fns[i];

              if(surface_form)
                local_stiffness_matrix[i][j] = 0.5 * block_scaling_coeff(form) * form->value(n_quadrature_points, jacobian_x_weights, u_ext, u, v, geometry, &ext) * form->scaling_factor * current_als[form->j]->coef[j] * current_als[form->i]->coef[i];
              else
                local_stiffness_matrix[i][j] = block_scaling_coeff(form) * form->value(n_quadrature_points, jacobian_x_weights, u_ext, u, v, geometry, &ext) * form->scaling_factor * current_als[form->j]->coef[j] * current_als[form->i]->coef[i];
            }
          }
        }
        // Symmetric block.
        else
        {
          for (unsigned int j = 0; j < current_als[form->j]->cnt; j++)
          {
            if (j < i && current_als[form->j]->dof[j] >= 0)
              continue;
            if (current_als[form->j]->dof[j] >= 0)
            {
              // Is this necessary, i.e. is there a coefficient smaller than 1e-12?
              if (std::abs(current_als[form->j]->coef[j]) < 1e-12)
                continue;

              Func<double>* u = base_fns[j];
              Func<double>* v = test_fns[i];

              Scalar val = block_scaling_coeff(form) * form->value(n_quadrature_points, jacobian_x_weights, u_ext, u, v, geometry, &ext) * form->scaling_factor * current_als[form->j]->coef[j] * current_als[form->i]->coef[i];

              local_stiffness_matrix[i][j] = local_stiffness_matrix[j][i] = val;
            }
          }
        }
      }

      // Insert the local stiffness matrix into the global one.
#pragma omp critical (mat)
      current_mat->add(current_als[form->i]->cnt, current_als[form->j]->cnt, local_stiffness_matrix, current_als[form->i]->dof, current_als[form->j]->dof);

      // Insert also the off-diagonal (anti-)symmetric block, if required.
      if (tra)
      {
        if (form->sym < 0)
          chsgn(local_stiffness_matrix, current_als[form->i]->cnt, current_als[form->j]->cnt);
        transpose(local_stiffness_matrix, current_als[form->i]->cnt, current_als[form->j]->cnt);
#pragma omp critical (mat)
        current_mat->add(current_als[form->j]->cnt, current_als[form->i]->cnt, local_stiffness_matrix, current_als[form->j]->dof, current_als[form->i]->dof);
      }

      // Cleanup.
      deinit_ext(form, u_ext, &ext);
      delete [] local_stiffness_matrix;
      delete [] jacobian_x_weights;
      geometry->free();
      delete geometry;
    }

    template<typename Scalar>
    int DiscreteProblem<Scalar>::calc_order_vector_form(VectorForm<Scalar> *form, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, Traverse::State* current_state)
    {
      _F_;

      int order;

      if(is_fvm)
        order = current_refmaps[form->i]->get_inv_ref_order();
      else
      {
        // order of solutions from the previous Newton iteration etc..
        Func<Hermes::Ord>** u_ext_ord = new Func<Hermes::Ord>*[RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset];
        ExtData<Hermes::Ord> ext_ord;
        init_ext_orders(form, u_ext_ord, &ext_ord, current_u_ext, current_state);

        // Order of shape functions.
        int max_order_i = this->spaces[form->i]->get_element_order(current_state->e[form->i]->id);
        if(H2D_GET_V_ORDER(max_order_i) > H2D_GET_H_ORDER(max_order_i))
          max_order_i = H2D_GET_V_ORDER(max_order_i);
        else
          max_order_i = H2D_GET_H_ORDER(max_order_i);

        for (unsigned int k = 0; k < current_state->rep->get_num_surf(); k++)
        {
          int eo = this->spaces[form->i]->get_edge_order(current_state->e[form->i], k);
          if (eo > max_order_i) 
            max_order_i = eo;
        }
        Func<Hermes::Ord>* ov = init_fn_ord(max_order_i + (spaces[form->i]->get_shapeset()->get_num_components() > 1 ? 1 : 0));

        // Total order of the vector form.
        Hermes::Ord o = form->ord(1, &fake_wt, u_ext_ord, ov, &geom_ord, &ext_ord);

        adjust_order_to_refmaps(form, order, &o, current_refmaps);

        // Cleanup.
        deinit_ext_orders(form, u_ext_ord, &ext_ord);
        ov->free_ord();
        delete ov;
      }
      return order;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble_vector_form(VectorForm<Scalar>* form, int order, Func<double>** test_fns, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, Traverse::State* current_state)
    {
      _F_;
      bool surface_form = (dynamic_cast<VectorFormVol<Scalar>*>(form) == NULL);

      // Init geometry.
      int n_quadrature_points;
      Geom<double>* geometry = NULL;
      double* jacobian_x_weights = NULL;

      if(surface_form)
        n_quadrature_points = init_surface_geometry_points(current_refmaps[form->i], order, current_state, geometry, jacobian_x_weights);
      else
        n_quadrature_points = init_geometry_points(current_refmaps[form->i], order, geometry, jacobian_x_weights);

      // Init external functions.
      Func<Scalar>** u_ext = new Func<Scalar>*[RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset];
      ExtData<Scalar> ext;
      init_ext(form, u_ext, &ext, order, current_u_ext, current_state);

      // Add the previous time level solution previously inserted at the back of ext.
      if(RungeKutta)
        for(int ext_i = 0; ext_i < this->RK_original_spaces_count; ext_i++)
          u_ext[ext_i]->add(*ext.fn[form->ext.size() - this->RK_original_spaces_count + ext_i]);

      // Actual form-specific calculation.
      for (unsigned int i = 0; i < current_als[form->i]->cnt; i++)
      {
        if (current_als[form->i]->dof[i] < 0)
          continue;

        // Is this necessary, i.e. is there a coefficient smaller than 1e-12?
        if (std::abs(current_als[form->i]->coef[i]) < 1e-12)
          continue;

        Func<double>* v = test_fns[i];

        Scalar val;
        if(surface_form)
          val = 0.5 * form->value(n_quadrature_points, jacobian_x_weights, u_ext, v, geometry, &ext) * form->scaling_factor * current_als[form->i]->coef[i];
        else
          val = form->value(n_quadrature_points, jacobian_x_weights, u_ext, v, geometry, &ext) * form->scaling_factor * current_als[form->i]->coef[i];
#pragma omp critical (rhs)
        current_rhs->add(current_als[form->i]->dof[i], val);
      }

      // Cleanup.
      deinit_ext(form, u_ext, &ext);
      delete [] jacobian_x_weights;
      geometry->free();
      delete geometry;
    }

    template<typename Scalar>
    int DiscreteProblem<Scalar>::init_geometry_points(RefMap* reference_mapping, int order, Geom<double>*& geometry, double*& jacobian_x_weights)
    {
      _F_;
      double3* pt = reference_mapping->get_quad_2d()->get_points(order, reference_mapping->get_active_element()->get_mode());
      int np = reference_mapping->get_quad_2d()->get_num_points(order, reference_mapping->get_active_element()->get_mode());

      // Init geometry and jacobian*weights.
      geometry = init_geom_vol(reference_mapping, order);
      double* jac = NULL;
      if(!reference_mapping->is_jacobian_const())
        jac = reference_mapping->get_jacobian(order);
      jacobian_x_weights = new double[np];
      for(int i = 0; i < np; i++)
      {
        if(reference_mapping->is_jacobian_const())
          jacobian_x_weights[i] = pt[i][2] * reference_mapping->get_const_jacobian();
        else
          jacobian_x_weights[i] = pt[i][2] * jac[i];
      }
      return np;
    }

    template<typename Scalar>
    int DiscreteProblem<Scalar>::init_surface_geometry_points(RefMap* reference_mapping, int& order, Traverse::State* current_state, Geom<double>*& geometry, double*& jacobian_x_weights)
    {
      _F_;
      int eo = reference_mapping->get_quad_2d()->get_edge_points(current_state->isurf, order, reference_mapping->get_active_element()->get_mode());
      double3* pt = reference_mapping->get_quad_2d()->get_points(eo, reference_mapping->get_active_element()->get_mode());
      int np = reference_mapping->get_quad_2d()->get_num_points(eo, reference_mapping->get_active_element()->get_mode());

      // Init geometry and jacobian*weights.
      double3* tan;
      geometry = init_geom_surf(reference_mapping, current_state->isurf, current_state->rep->en[current_state->isurf]->marker, eo, tan);
      jacobian_x_weights = new double[np];
      for(int i = 0; i < np; i++)
        jacobian_x_weights[i] = pt[i][2] * tan[i][2];
      order = eo;
      return np;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init_ext_orders(Form<Scalar> *form, Func<Hermes::Ord>** oi, ExtData<Hermes::Ord>* oext, Solution<Scalar>** current_u_ext, Traverse::State* current_state)
    {
      _F_;
      unsigned int prev_size = RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset;
      bool surface_form = (current_state->isurf > -1);

      if (current_u_ext != NULL)
        for(int i = 0; i < prev_size; i++)
          if (current_u_ext[i + form->u_ext_offset] != NULL)
            if(surface_form)
              oi[i] = init_fn_ord(current_u_ext[i + form->u_ext_offset]->get_edge_fn_order(current_state->isurf) + (current_u_ext[i + form->u_ext_offset]->get_num_components() > 1 ? 1 : 0));
            else
              oi[i] = init_fn_ord(current_u_ext[i + form->u_ext_offset]->get_fn_order() + (current_u_ext[i + form->u_ext_offset]->get_num_components() > 1 ? 1 : 0));
          else

            oi[i] = init_fn_ord(0);
      else
        for(int i = 0; i < prev_size; i++)
          oi[i] = init_fn_ord(0);

      oext->nf = form->ext.size();
      oext->fn = new Func<Hermes::Ord>*[oext->nf];
      for (int i = 0; i < oext->nf; i++)
        if(surface_form)
          oext->fn[i] = init_fn_ord(form->ext[i]->get_edge_fn_order(current_state->isurf) + (form->ext[i]->get_num_components() > 1 ? 1 : 0));
        else
          oext->fn[i] = init_fn_ord(form->ext[i]->get_fn_order() + (form->ext[i]->get_num_components() > 1 ? 1 : 0));
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::deinit_ext_orders(Form<Scalar> *form, Func<Hermes::Ord>** oi, ExtData<Hermes::Ord>* oext)
    {
      _F_;
      unsigned int prev_size = RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset;
      for(int i = 0; i < prev_size; i++)
        oi[i]->free_ord();
      delete [] oi;

      if (oext != NULL)
        oext->free_ord();
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init_ext(Form<Scalar> *form, Func<Scalar>** u_ext, ExtData<Scalar>* ext, int order, Solution<Scalar>** current_u_ext, Traverse::State* current_state)
    {
      _F_;
      unsigned int prev_size = RungeKutta ? RK_original_spaces_count : this->wf->get_neq() - form->u_ext_offset;

      if (current_u_ext != NULL)
        for(int i = 0; i < prev_size; i++)
          if (current_u_ext[i + form->u_ext_offset] != NULL)
            u_ext[i] = current_state->e[i] == NULL ? NULL : init_fn(current_u_ext[i + form->u_ext_offset], order);
          else
            u_ext[i] = NULL;
      else
        for(int i = 0; i < prev_size; i++)
          u_ext[i] = NULL;

      ext->nf = form->ext.size();
      ext->fn = new Func<Scalar>*[ext->nf];
      for (unsigned i = 0; i < ext->nf; i++)
      {
        if (form->ext[i] != NULL) 
          ext->fn[i] = init_fn(form->ext[i], order);
        else ext->fn[i] = NULL;
      }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::deinit_ext(Form<Scalar> *form, Func<Scalar>** u_ext, ExtData<Scalar>* ext)
    {
      _F_;
      // Values of the previous Newton iteration, shape functions
      // and external functions in quadrature points.
      int prev_size = this->wf->get_neq() - form->u_ext_offset;
      // In case of Runge-Kutta, this is time-saving, as it is known how many functions are there for the user.
      if(this->RungeKutta)
        prev_size = this->RK_original_spaces_count;

      for(int i = 0; i < prev_size; i++)
        if (u_ext[i] != NULL)
        {
          u_ext[i]->free_fn();
          delete u_ext[i];
        }

        delete [] u_ext;

        if (ext != NULL)
          ext->free();
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::adjust_order_to_refmaps(Form<Scalar> *form, int& order, Hermes::Ord* o, RefMap** current_refmaps)
    {
      _F_;
      // Increase due to reference map.
      order = current_refmaps[form->i]->get_inv_ref_order();
      order += o->get_order();
      limit_order(order, current_refmaps[form->i]->get_active_element()->get_mode());
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble_one_DG_state(PrecalcShapeset** current_pss, PrecalcShapeset** current_spss, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, 
      Traverse::State* current_state, MatrixFormSurf<Scalar>** current_mfsurf, VectorFormSurf<Scalar>** current_vfsurf, Transformable** fn)
    {
      _F_;

      bool DG_state = false;
      for(current_state->isurf = 0; current_state->isurf < current_state->rep->get_num_surf(); current_state->isurf++)
        if(current_state->rep->en[current_state->isurf]->marker == 0)
          DG_state = true;
      if(!DG_state)
        return;

      // Determine the minimum mesh seq.
      min_dg_mesh_seq = 0;
      for(unsigned int i = 0; i < spaces.size(); i++)
        if(spaces[i]->get_mesh()->get_seq() < min_dg_mesh_seq || i == 0)
          min_dg_mesh_seq = spaces[i]->get_mesh()->get_seq();

      // Create neighbor psss, refmaps.
      std::map<unsigned int, PrecalcShapeset *> npss;
      std::map<unsigned int, PrecalcShapeset *> nspss;
      std::map<unsigned int, RefMap *> nrefmap;

      // Initialize neighbor precalc shapesets and refmaps.
      // This is only needed when there are matrix DG forms present.
      if(DG_matrix_forms_present)
      {
        for (unsigned int i = 0; i < spaces.size(); i++)
        {
          PrecalcShapeset* new_ps = new PrecalcShapeset(spaces[i]->get_shapeset());
          new_ps->set_quad_2d(&g_quad_2d_std);
          npss.insert(std::pair<unsigned int, PrecalcShapeset*>(i, new_ps));

          PrecalcShapeset* new_pss = new PrecalcShapeset(new_ps);
          new_pss->set_quad_2d(&g_quad_2d_std);
          nspss.insert(std::pair<unsigned int, PrecalcShapeset*>(i, new_pss));

          RefMap* new_rm = new RefMap();
          new_rm->set_quad_2d(&g_quad_2d_std);
          nrefmap.insert(std::pair<unsigned int, RefMap*>(i, new_rm));
        }
      }

      for(current_state->isurf = 0; current_state->isurf < current_state->rep->get_num_surf(); current_state->isurf++)
      {
        if(current_state->rep->en[current_state->isurf]->marker != 0)
          continue;

        // Initialize the NeighborSearches.
        // 5 is for bits per page in the array.
        LightArray<NeighborSearch<Scalar>*> neighbor_searches(5);
        init_neighbors(neighbor_searches, current_state);

        // Create a multimesh tree;
        NeighborNode* root = new NeighborNode(NULL, 0);
        build_multimesh_tree(root, neighbor_searches);

        // Update all NeighborSearches according to the multimesh tree.
        // After this, all NeighborSearches in neighbor_searches should have the same count
        // of neighbors and proper set of transformations
        // for the central and the neighbor element(s) alike.
        // Also check that every NeighborSearch has the same number of neighbor elements.
        unsigned int num_neighbors = 0;
        for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
        {
          if(neighbor_searches.present(i))
          {
            NeighborSearch<Scalar>* ns = neighbor_searches.get(i);
            update_neighbor_search(ns, root);
            if(num_neighbors == 0)
              num_neighbors = ns->n_neighbors;
            if(ns->n_neighbors != num_neighbors)
              error("Num_neighbors of different NeighborSearches not matching in DiscreteProblem<Scalar>::assemble_surface_integrals().");
          }
        }

        for(unsigned int neighbor_i = 0; neighbor_i < num_neighbors; neighbor_i++)
        {
          // If the active segment has already been processed (when the neighbor element was assembled), it is skipped.
          // We test all neighbor searches, because in the case of intra-element edge, the neighboring (the same as central) element
          // will be marked as visited, even though the edge was not calculated.
          bool processed = true;
          for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
          {
            if(neighbor_searches.present(i))
              if(!neighbor_searches.get(i)->neighbors.at(neighbor_i)->visited)
              {
                processed = false;
                break;
              }
          }

          if(!DG_vector_forms_present && processed)
            continue;

          assemble_DG_one_neighbor(processed, neighbor_i, current_pss, current_spss, current_refmaps, current_u_ext, current_als, 
            current_state, current_mfsurf, current_vfsurf, fn,
            npss, nspss, nrefmap, neighbor_searches);
        }

        // Delete the multimesh tree;
        delete root;

        // Delete the neighbor_searches array.
        for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
          if(neighbor_searches.present(i))
            delete neighbor_searches.get(i);
      }

      // Deinitialize neighbor pss's, refmaps.
      if(DG_matrix_forms_present)
      {
        for(std::map<unsigned int, PrecalcShapeset *>::iterator it = nspss.begin(); it != nspss.end(); it++)
          delete it->second;
        for(std::map<unsigned int, PrecalcShapeset *>::iterator it = npss.begin(); it != npss.end(); it++)
          delete it->second;
        for(std::map<unsigned int, RefMap *>::iterator it = nrefmap.begin(); it != nrefmap.end(); it++)
          delete it->second;
      }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble_DG_one_neighbor(bool edge_processed, unsigned int neighbor_i, 
      PrecalcShapeset** current_pss, PrecalcShapeset** current_spss, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, 
      Traverse::State* current_state, MatrixFormSurf<Scalar>** current_mfsurf, VectorFormSurf<Scalar>** current_vfsurf, Transformable** fn, 
      std::map<unsigned int, PrecalcShapeset *> npss, std::map<unsigned int, PrecalcShapeset *> nspss, std::map<unsigned int, RefMap *> nrefmap, 
      LightArray<NeighborSearch<Scalar>*>& neighbor_searches)
    {
      _F_;
      // Set the active segment in all NeighborSearches
      for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
      {
        if(neighbor_searches.present(i))
        {
          neighbor_searches.get(i)->active_segment = neighbor_i;
          neighbor_searches.get(i)->neighb_el = neighbor_searches.get(i)->neighbors[neighbor_i];
          neighbor_searches.get(i)->neighbor_edge = neighbor_searches.get(i)->neighbor_edges[neighbor_i];
        }
      }

      // Push all the necessary transformations to all functions of this stage.
      // The important thing is that the transformations to the current subelement are already there.
      for(unsigned int fns_i = 0; fns_i < current_state->num; fns_i++)
      {
        Mesh * mesh_i;
        if(dynamic_cast<PrecalcShapeset*>(fn[fns_i]) != NULL)
          mesh_i = spaces[fns_i]->get_mesh();
        else
          mesh_i = (dynamic_cast<MeshFunction<Scalar>*>(fn[fns_i]))->get_mesh();
        NeighborSearch<Scalar>* ns = neighbor_searches.get(mesh_i->get_seq() - min_dg_mesh_seq);
        if (ns->central_transformations.present(neighbor_i))
          ns->central_transformations.get(neighbor_i)->apply_on(fn[fns_i]);
      }

      // For neighbor psss.
      if(current_mat != NULL && DG_matrix_forms_present && !edge_processed)
      {
        for(unsigned int idx_i = 0; idx_i < spaces.size(); idx_i++)
        {
          NeighborSearch<Scalar>* ns = neighbor_searches.get(spaces[idx_i]->get_mesh()->get_seq() - min_dg_mesh_seq);
          npss[idx_i]->set_active_element((*ns->get_neighbors())[neighbor_i]);
          if (ns->neighbor_transformations.present(neighbor_i))
            ns->neighbor_transformations.get(neighbor_i)->apply_on(npss[idx_i]);
        }
      }

      // Also push the transformations to the slave psss and refmaps.
      for (unsigned int i = 0; i < spaces.size(); i++)
      {
        current_spss[i]->set_master_transform();
        current_refmaps[i]->force_transform(current_pss[i]->get_transform(), current_pss[i]->get_ctm());

        // Neighbor.
        if(current_mat != NULL && DG_matrix_forms_present && !edge_processed)
        {
          nspss[i]->set_active_element(npss[i]->get_active_element());
          nspss[i]->set_master_transform();
          nrefmap[i]->set_active_element(npss[i]->get_active_element());
          nrefmap[i]->force_transform(npss[i]->get_transform(), npss[i]->get_ctm());
        }
      }

      /***/
      // The computation takes place here.
      if(current_mat != NULL && DG_matrix_forms_present && !edge_processed)
      {
        int order = 20;

        for(int current_mfsurf_i = 0; current_mfsurf_i < wf->mfsurf.size(); current_mfsurf_i++)
        {
          if(!form_to_be_assembled((MatrixForm<Scalar>*)current_mfsurf[current_mfsurf_i], current_state))
            continue;

          MatrixFormSurf<Scalar>* mfs = current_mfsurf[current_mfsurf_i];
          if (mfs->areas[0] != H2D_DG_INNER_EDGE)
            continue;
          int m = mfs->i;
          int n = mfs->j;

          // Create the extended shapeset on the union of the central element and its current neighbor.
          typename NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_u = neighbor_searches.get(spaces[n]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[n], current_als[n]);
          typename NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_v = neighbor_searches.get(spaces[m]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[m], current_als[m]);

          NeighborSearch<Scalar>* nbs_u = neighbor_searches.get(spaces[n]->get_mesh()->get_seq() - min_dg_mesh_seq);
          NeighborSearch<Scalar>* nbs_v = neighbor_searches.get(spaces[m]->get_mesh()->get_seq() - min_dg_mesh_seq);

          nbs_u->set_quad_order(order);
          nbs_v->set_quad_order(order);

          // Evaluate the form using just calculated order.
          Quad2D* quad = current_pss[0]->get_quad_2d();
          int eo = quad->get_edge_points(current_state->isurf, order, current_state->rep->get_mode());
          int np = quad->get_num_points(eo, current_state->rep->get_mode());
          double3* pt = quad->get_points(eo, current_state->rep->get_mode());

          // Init geometry.
          int n_quadrature_points;
          Geom<double>* geometry = NULL;
          double* jacobian_x_weights = NULL;
          n_quadrature_points = init_surface_geometry_points(current_refmaps[mfs->i], order, current_state, geometry, jacobian_x_weights);

          Geom<double>* e = new InterfaceGeom<double>(geometry, nbs_u->neighb_el->marker,
            nbs_u->neighb_el->id, nbs_u->neighb_el->get_diameter());

          // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
          int prev_size = wf->get_neq() - mfs->u_ext_offset;
          Func<Scalar>** prev = new Func<Scalar>*[prev_size];
          if (current_u_ext != NULL)
          {
            for (int i = 0; i < prev_size; i++)
            { 
              if (current_u_ext[i + mfs->u_ext_offset] != NULL)
              {
                neighbor_searches.get(current_u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
                prev[i]  = neighbor_searches.get(current_u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(current_u_ext[i]);
              }
              else prev[i] = NULL;
            }
          }
          else
            for (int i = 0; i < prev_size; i++)
              prev[i] = NULL;

          ExtData<Scalar>* ext = init_ext_fns(mfs->ext, neighbor_searches, order);

          // Precalc shapeset and refmaps used for the evaluation.
          PrecalcShapeset* fu;
          PrecalcShapeset* fv;
          RefMap* ru;
          RefMap* rv;
          bool support_neigh_u, support_neigh_v;

          Scalar **local_stiffness_matrix = new_matrix<Scalar>(std::max(ext_asmlist_u->cnt, ext_asmlist_v->cnt));
          for (int i = 0; i < ext_asmlist_v->cnt; i++)
          {
            if (ext_asmlist_v->dof[i] < 0)
              continue;
            // Choose the correct shapeset for the test function.
            if (!ext_asmlist_v->has_support_on_neighbor(i))
            {
              current_spss[m]->set_active_shape(ext_asmlist_v->central_al->idx[i]);
              fv = current_spss[m];
              rv = current_refmaps[m];
              support_neigh_v = false;
            }
            else
            {
              nspss[m]->set_active_shape(ext_asmlist_v->neighbor_al->idx[i - ext_asmlist_v->central_al->cnt]);
              fv = nspss[m];
              rv = nrefmap[m];
              support_neigh_v = true;
            }
            for (int j = 0; j < ext_asmlist_u->cnt; j++)
            {
              // Choose the correct shapeset for the solution function.
              if (!ext_asmlist_u->has_support_on_neighbor(j))
              {
                current_pss[n]->set_active_shape(ext_asmlist_u->central_al->idx[j]);
                fu = current_pss[n];
                ru = current_refmaps[n];
                support_neigh_u = false;
              }
              else
              {
                npss[n]->set_active_shape(ext_asmlist_u->neighbor_al->idx[j - ext_asmlist_u->central_al->cnt]);
                fu = npss[n];
                ru = nrefmap[n];
                support_neigh_u = true;
              }

              if (ext_asmlist_u->dof[j] >= 0)
              {
                // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
                DiscontinuousFunc<double>* u = new DiscontinuousFunc<double>(init_fn(fu, ru, nbs_u->get_quad_eo(support_neigh_u)),
                  support_neigh_u, nbs_u->neighbor_edge.orientation);
                DiscontinuousFunc<double>* v = new DiscontinuousFunc<double>(init_fn(fv, rv, nbs_v->get_quad_eo(support_neigh_v)),
                  support_neigh_v, nbs_v->neighbor_edge.orientation);

                Scalar res = mfs->value(np, jacobian_x_weights, prev, u, v, e, ext) * mfs->scaling_factor;

                u->free_fn();
                delete u;
                v->free_fn();
                delete v;

                Scalar val = block_scaling_coeff(mfs) * 0.5 * res * (support_neigh_u ? ext_asmlist_u->neighbor_al->coef[j - ext_asmlist_u->central_al->cnt]: ext_asmlist_u->central_al->coef[j])
                  * (support_neigh_v ? ext_asmlist_v->neighbor_al->coef[i - ext_asmlist_v->central_al->cnt]: ext_asmlist_v->central_al->coef[i]);
                local_stiffness_matrix[i][j] = val;
              }
            }
          }

          // Clean up.
          for (int i = 0; i < prev_size; i++)
          {
            if (prev[i] != NULL)
            {
              prev[i]->free_fn();
              delete prev[i];
            }
          }

          delete [] prev;


          if (ext != NULL)
          {
            ext->free();
            delete ext;
          }

          e->free();
          delete e;

#pragma omp critical (mat)
          current_mat->add(ext_asmlist_v->cnt, ext_asmlist_u->cnt, local_stiffness_matrix, ext_asmlist_v->dof, ext_asmlist_u->dof);
        }
      }

      if (current_rhs != NULL && DG_vector_forms_present)
      {
        int order = 20;

        for (unsigned int ww = 0; ww < wf->vfsurf.size(); ww++)
        {
          VectorFormSurf<Scalar>* vfs = current_vfsurf[ww];
          if (vfs->areas[0] != H2D_DG_INNER_EDGE)
            continue;
          int m = vfs->i;

          if(!form_to_be_assembled((VectorForm<Scalar>*)vfs, current_state))
            continue;

          // Here we use the standard pss, possibly just transformed by NeighborSearch.
          for (unsigned int dof_i = 0; dof_i < current_als[m]->cnt; dof_i++)
          {
            if (current_als[m]->dof[dof_i] < 0)
              continue;
            current_spss[m]->set_active_shape(current_als[m]->idx[dof_i]);

            NeighborSearch<Scalar>* nbs_v = (neighbor_searches.get(spaces[m]->get_mesh()->get_seq() - min_dg_mesh_seq));

            // Evaluate the form using just calculated order.
            Quad2D* quad = current_spss[m]->get_quad_2d();
            int eo = quad->get_edge_points(current_state->isurf, order, current_state->rep->get_mode());
            int np = quad->get_num_points(eo, current_state->rep->get_mode());
            double3* pt = quad->get_points(eo, current_state->rep->get_mode());

            // Init geometry and jacobian*weights.
            // Init geometry.
            int n_quadrature_points;
            Geom<double>* geometry = NULL;
            double* jacobian_x_weights = NULL;
            n_quadrature_points = init_surface_geometry_points(current_refmaps[vfs->i], order, current_state, geometry, jacobian_x_weights);

            Geom<double>* e = new InterfaceGeom<double>(geometry, nbs_v->neighb_el->marker,
              nbs_v->neighb_el->id, nbs_v->neighb_el->get_diameter());

            // Values of the previous Newton iteration, shape functions and external functions in quadrature points.
            int prev_size = wf->get_neq() - vfs->u_ext_offset;
            Func<Scalar>** prev = new Func<Scalar>*[prev_size];
            if (current_u_ext != NULL)
              for (int i = 0; i < prev_size; i++)
                if (current_u_ext[i + vfs->u_ext_offset] != NULL)
                {
                  neighbor_searches.get(current_u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
                  prev[i]  = neighbor_searches.get(current_u_ext[i]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(current_u_ext[i]);
                }
                else prev[i] = NULL;
            else
              for (int i = 0; i < prev_size; i++)
                prev[i] = NULL;

            Func<double>* v = init_fn(current_spss[m], current_refmaps[m], eo);
            ExtData<Scalar>* ext = init_ext_fns(vfs->ext, neighbor_searches, order);

            // Clean up.
            for (int i = 0; i < prev_size; i++)
            {
              if (prev[i] != NULL)
              {
                prev[i]->free_fn();
                delete prev[i];
              }
            }

            delete [] prev;

            if (ext != NULL)
            {
              ext->free();
              delete ext;
            }

            e->free();
            delete e;

#pragma omp critical (rhs)
            current_rhs->add(current_als[m]->dof[dof_i], 0.5 * vfs->value(np, jacobian_x_weights, prev, v, e, ext) * vfs->scaling_factor * current_als[m]->coef[dof_i]);
            v->free_fn();
            delete v;
          }
        }
      }

      // This is just cleaning after ourselves.
      // Clear the transformations from the RefMaps and all functions.
      for(unsigned int fns_i = 0; fns_i < current_state->num; fns_i++)
      {
        Mesh * mesh_i;
        if(dynamic_cast<PrecalcShapeset*>(fn[fns_i]) != NULL)
          mesh_i = spaces[fns_i]->get_mesh();
        else
          mesh_i = (dynamic_cast<MeshFunction<Scalar>*>(fn[fns_i]))->get_mesh();
        
        fn[fns_i]->set_transform(neighbor_searches.get(mesh_i->get_seq() - min_dg_mesh_seq)->original_central_el_transform);
      }

      // Also clear the transformations from the slave psss and refmaps.
      for (unsigned int i = 0; i < spaces.size(); i++)
      {
        current_spss[i]->set_master_transform();
        current_refmaps[i]->force_transform(current_pss[i]->get_transform(), current_pss[i]->get_ctm());
      }
    }

    template<typename Scalar>
    ExtData<Scalar>* DiscreteProblem<Scalar>::init_ext_fns(Hermes::vector<MeshFunction<Scalar>*> &ext,
      LightArray<NeighborSearch<Scalar>*>& neighbor_searches, int order)
    {
      _F_;
      Func<Scalar>** ext_fns = new Func<Scalar>*[ext.size()];
      for(unsigned int j = 0; j < ext.size(); j++)
      {
        neighbor_searches.get(ext[j]->get_mesh()->get_seq() - min_dg_mesh_seq)->set_quad_order(order);
        ext_fns[j] = neighbor_searches.get(ext[j]->get_mesh()->get_seq() - min_dg_mesh_seq)->init_ext_fn(ext[j]);
      }

      ExtData<Scalar>* ext_data = new ExtData<Scalar>;
      ext_data->fn = ext_fns;
      ext_data->nf = ext.size();

      return ext_data;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::init_neighbors(LightArray<NeighborSearch<Scalar>*>& neighbor_searches,
      Traverse::State* current_state)
    {
      _F_;
      // Initialize the NeighborSearches.
      for(unsigned int i = 0; i < current_state->num; i++)
      {
        if(i > 0 && spaces[i - 1]->get_mesh()->get_seq() == spaces[i]->get_mesh()->get_seq())
          continue;
        else
          if(!neighbor_searches.present(spaces[i]->get_mesh()->get_seq() - min_dg_mesh_seq))
          {
            NeighborSearch<Scalar>* ns = new NeighborSearch<Scalar>(current_state->e[i], spaces[i]->get_mesh());
            ns->original_central_el_transform = current_state->sub_idx[i];
            neighbor_searches.add(ns, spaces[i]->get_mesh()->get_seq() - min_dg_mesh_seq);
          }
      }

      // Calculate respective neighbors.
      // Also clear the initial_sub_idxs from the central element transformations
      // of NeighborSearches with multiple neighbors.
      for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
      {
        if(i > 0 && spaces[i - 1]->get_mesh()->get_seq() == spaces[i]->get_mesh()->get_seq())
          continue;
        if(neighbor_searches.present(i))
        {
          neighbor_searches.get(i)->set_active_edge_multimesh(current_state->isurf);
          neighbor_searches.get(i)->clear_initial_sub_idx();
        }
      }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::build_multimesh_tree(NeighborNode* root,
      LightArray<NeighborSearch<Scalar>*>& neighbor_searches)
    {
      _F_;
      for(unsigned int i = 0; i < neighbor_searches.get_size(); i++)
        if(neighbor_searches.present(i))
        {
          NeighborSearch<Scalar>* ns = neighbor_searches.get(i);
          if (ns->n_neighbors == 1 &&
            (ns->central_transformations.get_size() == 0 || ns->central_transformations.get(0)->num_levels == 0))
            continue;
          for(unsigned int j = 0; j < ns->n_neighbors; j++)
            if (ns->central_transformations.present(j))
              insert_into_multimesh_tree(root, ns->central_transformations.get(j)->transf, ns->central_transformations.get(j)->num_levels);
        }
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::insert_into_multimesh_tree(NeighborNode* node,
      unsigned int* transformations,
      unsigned int transformation_count)
    {
      _F_;
      // If we are already in the leaf.
      if(transformation_count == 0)
        return;
      // Both sons are null. We have to add a new Node. Let us do it for the left sone of node.
      if(node->get_left_son() == NULL && node->get_right_son() == NULL)
      {
        node->set_left_son(new NeighborNode(node, transformations[0]));
        insert_into_multimesh_tree(node->get_left_son(), transformations + 1, transformation_count - 1);
      }
      // At least the left son is not null (it is impossible only for the right one to be not null, because
      // the left one always gets into the tree first, as seen above).
      else
      {
        // The existing left son is the right one to continue through.
        if(node->get_left_son()->get_transformation() == transformations[0])
          insert_into_multimesh_tree(node->get_left_son(), transformations + 1, transformation_count - 1);
        // The right one also exists, check that it is the right one, or return an error.
        else if(node->get_right_son() != NULL)
        {
          if(node->get_right_son()->get_transformation() == transformations[0])
            insert_into_multimesh_tree(node->get_right_son(), transformations + 1, transformation_count - 1);
          else error("More than two possible sons in insert_into_multimesh_tree().");
        }
        // If the right one does not exist and the left one was not correct, create a right son and continue this way.
        else
        {
          node->set_right_son(new NeighborNode(node, transformations[0]));
          insert_into_multimesh_tree(node->get_right_son(), transformations + 1, transformation_count - 1);
        }
      }
    }

    template<typename Scalar>
    Hermes::vector<Hermes::vector<unsigned int>*> DiscreteProblem<Scalar>::get_multimesh_neighbors_transformations(NeighborNode* multimesh_tree)
    {
      _F_;
      // Initialize the vector.
      Hermes::vector<Hermes::vector<unsigned int>*> running_transformations;
      // Prepare the first neighbor's vector.
      running_transformations.push_back(new Hermes::vector<unsigned int>);
      // Fill the vector.
      traverse_multimesh_tree(multimesh_tree, running_transformations);
      return running_transformations;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::traverse_multimesh_tree(NeighborNode* node,
      Hermes::vector<Hermes::vector<unsigned int>*>& running_transformations)
    {
      _F_;
      // If we are in the root.
      if(node->get_transformation() == 0)
      {
        if(node->get_left_son() != NULL)
          traverse_multimesh_tree(node->get_left_son(), running_transformations);
        if(node->get_right_son() != NULL)
          traverse_multimesh_tree(node->get_right_son(), running_transformations);
        // Delete the vector prepared by the last accessed leaf.
        delete running_transformations.back();
        running_transformations.pop_back();
        return;
      }
      // If we are in a leaf.
      if(node->get_left_son() == NULL && node->get_right_son() == NULL)
      {
        // Create a vector for the new neighbor.
        Hermes::vector<unsigned int>* new_neighbor_transformations = new Hermes::vector<unsigned int>;
        // Copy there the whole path except for this leaf.
        for(unsigned int i = 0; i < running_transformations.back()->size(); i++)
          new_neighbor_transformations->push_back((*running_transformations.back())[i]);
        // Insert this leaf into the current running transformation, thus complete it.
        running_transformations.back()->push_back(node->get_transformation());
        // Make the new_neighbor_transformations the current running transformation.
        running_transformations.push_back(new_neighbor_transformations);
        return;
      }
      else
      {
        running_transformations.back()->push_back(node->get_transformation());
        if(node->get_left_son() != NULL)
          traverse_multimesh_tree(node->get_left_son(), running_transformations);
        if(node->get_right_son() != NULL)
          traverse_multimesh_tree(node->get_right_son(), running_transformations);
        running_transformations.back()->pop_back();
        return;
      }
      return;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::update_neighbor_search(NeighborSearch<Scalar>* ns, NeighborNode* multimesh_tree)
    {
      _F_;
      // This has to be done, because we pass ns by reference and the number of neighbors is changing.
      unsigned int num_neighbors = ns->get_num_neighbors();

      for(unsigned int i = 0; i < num_neighbors; i++)
      {
        // Find the node corresponding to this neighbor in the tree.
        NeighborNode* node;
        if (ns->central_transformations.present(i))
          node = find_node(ns->central_transformations.get(i)->transf, ns->central_transformations.get(i)->num_levels, multimesh_tree);
        else
          node = multimesh_tree;

        // Update the NeighborSearch.
        unsigned int added = update_ns_subtree(ns, node, i);
        i += added;
        num_neighbors += added;
      }
    }

    template<typename Scalar>
    NeighborNode* DiscreteProblem<Scalar>::find_node(unsigned int* transformations,
      unsigned int transformation_count,
      NeighborNode* node)
    {
      _F_;
      // If there are no transformations left.
      if(transformation_count == 0)
        return node;
      else
      {
        if(node->get_left_son() != NULL)
        {
          if(node->get_left_son()->get_transformation() == transformations[0])
            return find_node(transformations + 1, transformation_count - 1, node->get_left_son());
        }
        if(node->get_right_son() != NULL)
        {
          if(node->get_right_son()->get_transformation() == transformations[0])
            return find_node(transformations + 1, transformation_count - 1, node->get_right_son());
        }
      }
      // We always should be able to empty the transformations array.
      error("Transformation of a central element not found in the multimesh tree.");
      return NULL;
    }

    template<typename Scalar>
    unsigned int DiscreteProblem<Scalar>::update_ns_subtree(NeighborSearch<Scalar>* ns,
      NeighborNode* node, unsigned int ith_neighbor)
    {
      _F_;
      // No subtree => no work.
      // Also check the assertion that if one son is null, then the other too.
      if(node->get_left_son() == NULL)
      {
        if(node->get_right_son() != NULL)
          error("Only one son (right) not null in DiscreteProblem<Scalar>::update_ns_subtree.");
        return 0;
      }

      // Key part.
      // Begin with storing the info about the current neighbor.
      Element* neighbor = ns->neighbors[ith_neighbor];
      typename NeighborSearch<Scalar>::NeighborEdgeInfo edge_info = ns->neighbor_edges[ith_neighbor];

      // Initialize the vector for central transformations->
      Hermes::vector<Hermes::vector<unsigned int>*> running_central_transformations;
      // Prepare the first new neighbor's vector. Push back the current transformations (in case of GO_DOWN neighborhood).
      running_central_transformations.push_back(new Hermes::vector<unsigned int>);
      if (ns->central_transformations.present(ith_neighbor))
        ns->central_transformations.get(ith_neighbor)->copy_to(running_central_transformations.back());

      // Initialize the vector for neighbor transformations->
      Hermes::vector<Hermes::vector<unsigned int>*> running_neighbor_transformations;
      // Prepare the first new neighbor's vector. Push back the current transformations (in case of GO_UP/NO_TRF neighborhood).
      running_neighbor_transformations.push_back(new Hermes::vector<unsigned int>);
      if (ns->neighbor_transformations.present(ith_neighbor))
        ns->neighbor_transformations.get(ith_neighbor)->copy_to(running_neighbor_transformations.back());

      // Delete the current neighbor.
      ns->delete_neighbor(ith_neighbor);

      // Move down the subtree.
      if(node->get_left_son() != NULL)
        traverse_multimesh_subtree(node->get_left_son(), running_central_transformations,
        running_neighbor_transformations, edge_info, ns->active_edge,
        ns->central_el->get_mode());
      if(node->get_right_son() != NULL)
        traverse_multimesh_subtree(node->get_right_son(), running_central_transformations,
        running_neighbor_transformations, edge_info, ns->active_edge,
        ns->central_el->get_mode());

      // Delete the last neighbors' info (this is a dead end, caused by the function traverse_multimesh_subtree.
      delete running_central_transformations.back();
      running_central_transformations.pop_back();
      delete running_neighbor_transformations.back();
      running_neighbor_transformations.pop_back();

      // Insert new neighbors.
      for(unsigned int i = 0; i < running_central_transformations.size(); i++)
      {
        ns->neighbors.push_back(neighbor);
        ns->neighbor_edges.push_back(edge_info);

        if (!ns->central_transformations.present(ns->n_neighbors))
          ns->central_transformations.add(new typename NeighborSearch<Scalar>::Transformations, ns->n_neighbors);
        if (!ns->neighbor_transformations.present(ns->n_neighbors))
          ns->neighbor_transformations.add(new typename NeighborSearch<Scalar>::Transformations, ns->n_neighbors);
        ns->central_transformations.get(ns->n_neighbors)->copy_from(*running_central_transformations[i]);
        ns->neighbor_transformations.get(ns->n_neighbors)->copy_from(*running_neighbor_transformations[i]);

        ns->n_neighbors++;
      }

      for(unsigned int i = 0; i < running_central_transformations.size(); i++)
        delete running_central_transformations[i];
      for(unsigned int i = 0; i < running_neighbor_transformations.size(); i++)
        delete running_neighbor_transformations[i];

      // Return the number of neighbors deleted.
      return -1;
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::assemble_DG_matrix_forms(PrecalcShapeset** current_pss, PrecalcShapeset** current_spss, RefMap** current_refmaps, Solution<Scalar>** current_u_ext, AsmList<Scalar>** current_als, 
      Traverse::State* current_state, MatrixFormSurf<Scalar>** current_mfsurf, std::map<unsigned int, PrecalcShapeset*> npss,
      std::map<unsigned int, PrecalcShapeset*> nspss, std::map<unsigned int, RefMap*> nrefmap, LightArray<NeighborSearch<Scalar>*>& neighbor_searches)
    {
      _F_;
      /*
      for (unsigned int ww = 0; ww < wf->mfsurf.size(); ww++)
      {
      MatrixFormSurf<Scalar>* mfs = mfsurf[ww];
      if (mfs->areas[0] != H2D_DG_INNER_EDGE)
      continue;
      int m = mfs->i;
      int n = mfs->j;

      if (isempty[m] || isempty[n])
      continue;
      if (fabs(mfs->scaling_factor) < 1e-12)
      continue;

      surf_pos.base = trav_base;

      // Create the extended shapeset on the union of the central element and its current neighbor.
      typename NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_u = neighbor_searches.get(spaces[n]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[n], al[n]);
      typename NeighborSearch<Scalar>::ExtendedShapeset* ext_asmlist_v = neighbor_searches.get(spaces[m]->get_mesh()->get_seq() - min_dg_mesh_seq)->create_extended_asmlist(spaces[m], al[m]);

      // If a block scaling table is provided, and if the scaling coefficient
      // A_mn for this block is zero, then the form does not need to be assembled.
      double block_scaling_coeff = 1.;
      if (block_weights != NULL)
      {
      block_scaling_coeff = block_weights->get_A(m, n);
      if (fabs(block_scaling_coeff) < 1e-12)
      continue;
      }

      // Precalc shapeset and refmaps used for the evaluation.
      PrecalcShapeset* fu;
      PrecalcShapeset* fv;
      RefMap* ru;
      RefMap* rv;
      bool support_neigh_u, support_neigh_v;

      Scalar **local_stiffness_matrix = get_matrix_buffer(std::max(ext_asmlist_u->cnt, ext_asmlist_v->cnt));
      for (int i = 0; i < ext_asmlist_v->cnt; i++)
      {
      if (ext_asmlist_v->dof[i] < 0)
      continue;
      // Choose the correct shapeset for the test function.
      if (!ext_asmlist_v->has_support_on_neighbor(i))
      {
      spss[m]->set_active_shape(ext_asmlist_v->central_al->idx[i]);
      fv = spss[m];
      rv = refmap[m];
      support_neigh_v = false;
      }
      else
      {
      nspss[m]->set_active_shape(ext_asmlist_v->neighbor_al->idx[i - ext_asmlist_v->central_al->cnt]);
      fv = nspss[m];
      rv = nrefmap[m];
      support_neigh_v = true;
      }
      for (int j = 0; j < ext_asmlist_u->cnt; j++)
      {
      // Choose the correct shapeset for the solution function.
      if (!ext_asmlist_u->has_support_on_neighbor(j))
      {
      pss[n]->set_active_shape(ext_asmlist_u->central_al->idx[j]);
      fu = pss[n];
      ru = refmap[n];
      support_neigh_u = false;
      }
      else
      {
      npss[n]->set_active_shape(ext_asmlist_u->neighbor_al->idx[j - ext_asmlist_u->central_al->cnt]);
      fu = npss[n];
      ru = nrefmap[n];
      support_neigh_u = true;
      }

      if (ext_asmlist_u->dof[j] >= 0)
      {
      if (mat != NULL)
      {
      Scalar val = block_scaling_coeff * eval_dg_form(mfs, u_ext, fu, fv, refmap[n], ru, rv, support_neigh_u, support_neigh_v, &surf_pos, neighbor_searches, stage.meshes[n]->get_seq() - min_dg_mesh_seq, stage.meshes[m]->get_seq() - min_dg_mesh_seq)
      * (support_neigh_u ? ext_asmlist_u->neighbor_al->coef[j - ext_asmlist_u->central_al->cnt]: ext_asmlist_u->central_al->coef[j])
      * (support_neigh_v ? ext_asmlist_v->neighbor_al->coef[i - ext_asmlist_v->central_al->cnt]: ext_asmlist_v->central_al->coef[i]);
      local_stiffness_matrix[i][j] = val;
      }
      }
      }
      }
      if (mat != NULL)
      mat->add(ext_asmlist_v->cnt, ext_asmlist_u->cnt, local_stiffness_matrix, ext_asmlist_v->dof, ext_asmlist_u->dof);
      }
      */
    }

    template<typename Scalar>
    void DiscreteProblem<Scalar>::traverse_multimesh_subtree(NeighborNode* node,
      Hermes::vector<Hermes::vector<unsigned int>*>& running_central_transformations,
      Hermes::vector<Hermes::vector<unsigned int>*>& running_neighbor_transformations,
      const typename NeighborSearch<Scalar>::NeighborEdgeInfo& edge_info, const int& active_edge, const int& mode)
    {
      _F_;
      // If we are in a leaf.
      if(node->get_left_son() == NULL && node->get_right_son() == NULL)
      {
        // Create vectors for the new neighbor.
        Hermes::vector<unsigned int>* new_neighbor_central_transformations = new Hermes::vector<unsigned int>;
        Hermes::vector<unsigned int>* new_neighbor_neighbor_transformations = new Hermes::vector<unsigned int>;

        // Copy there the whole path except for this leaf.
        for(unsigned int i = 0; i < running_central_transformations.back()->size(); i++)
          new_neighbor_central_transformations->push_back((*running_central_transformations.back())[i]);
        for(unsigned int i = 0; i < running_neighbor_transformations.back()->size(); i++)
          new_neighbor_neighbor_transformations->push_back((*running_neighbor_transformations.back())[i]);

        // Insert this leaf into the current running central transformation, thus complete it.
        running_central_transformations.back()->push_back(node->get_transformation());

        // Make the new_neighbor_central_transformations the current running central transformation.
        running_central_transformations.push_back(new_neighbor_central_transformations);

        // Take care of the neighbor transformation.
        // Insert appropriate info from this leaf into the current running neighbor transformation, thus complete it.
        if(mode == HERMES_MODE_TRIANGLE)
          if ((active_edge == 0 && node->get_transformation() == 0) ||
            (active_edge == 1 && node->get_transformation() == 1) ||
            (active_edge == 2 && node->get_transformation() == 2))
            running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
          else
            running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
        // Quads.
        else
          if ((active_edge == 0 && (node->get_transformation() == 0 || node->get_transformation() == 6)) ||
            (active_edge == 1 && (node->get_transformation() == 1 || node->get_transformation() == 4)) ||
            (active_edge == 2 && (node->get_transformation() == 2 || node->get_transformation() == 7)) ||
            (active_edge == 3 && (node->get_transformation() == 3 || node->get_transformation() == 5)))
            running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));
          else
            running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));

        // Make the new_neighbor_neighbor_transformations the current running neighbor transformation.
        running_neighbor_transformations.push_back(new_neighbor_neighbor_transformations);

        return;
      }
      else
      {
        // Insert this leaf into the current running central transformation, thus complete it.
        running_central_transformations.back()->push_back(node->get_transformation());

        // Insert appropriate info from this leaf into the current running neighbor transformation, thus complete it.
        // Triangles.
        if(mode == HERMES_MODE_TRIANGLE)
          if ((active_edge == 0 && node->get_transformation() == 0) ||
            (active_edge == 1 && node->get_transformation() == 1) ||
            (active_edge == 2 && node->get_transformation() == 2))
            running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
          else
            running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 3));
        // Quads.
        else
          if ((active_edge == 0 && (node->get_transformation() == 0 || node->get_transformation() == 6)) ||
            (active_edge == 1 && (node->get_transformation() == 1 || node->get_transformation() == 4)) ||
            (active_edge == 2 && (node->get_transformation() == 2 || node->get_transformation() == 7)) ||
            (active_edge == 3 && (node->get_transformation() == 3 || node->get_transformation() == 5)))
            running_neighbor_transformations.back()->push_back((!edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));
          else
            running_neighbor_transformations.back()->push_back((edge_info.orientation ? edge_info.local_num_of_edge : (edge_info.local_num_of_edge + 1) % 4));

        // Move down.
        if(node->get_left_son() != NULL)
          traverse_multimesh_subtree(node->get_left_son(), running_central_transformations, running_neighbor_transformations,
          edge_info, active_edge, mode);
        if(node->get_right_son() != NULL)
          traverse_multimesh_subtree(node->get_right_son(), running_central_transformations, running_neighbor_transformations,
          edge_info, active_edge, mode);

        // Take this transformation out.
        running_central_transformations.back()->pop_back();
        running_neighbor_transformations.back()->pop_back();
        return;
      }
      return;
    }

    NeighborNode::NeighborNode(NeighborNode* parent, unsigned int transformation) : parent(parent), transformation(transformation)
    {
      left_son = right_son = NULL;
    }
    NeighborNode::~NeighborNode()
    {
      if(left_son != NULL)
      {
        delete left_son;
        left_son = NULL;
      }
      if(right_son != NULL)
      {
        delete right_son;
        right_son = NULL;
      }
    }
    void NeighborNode::set_left_son(NeighborNode* left_son)
    {
      this->left_son = left_son;
    }
    void NeighborNode::set_right_son(NeighborNode* right_son)
    {
      this->right_son = right_son;
    }
    void NeighborNode::set_transformation(unsigned int transformation)
    {
      this->transformation = transformation;
    }
    NeighborNode* NeighborNode::get_left_son()
    {
      return left_son;
    }
    NeighborNode* NeighborNode::get_right_son()
    {
      return right_son;
    }
    unsigned int NeighborNode::get_transformation()
    {
      return this->transformation;
    }

    template class HERMES_API DiscreteProblem<double>;
    template class HERMES_API DiscreteProblem<std::complex<double> >;
  }
}
