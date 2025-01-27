// ---------------------------------------------------------------------
//
// Copyright (C) 1998 - 2022 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

#include <deal.II/base/array_view.h>
#include <deal.II/base/memory_consumption.h>
#include <deal.II/base/multithread_info.h>
#include <deal.II/base/numbers.h>
#include <deal.II/base/quadrature.h>
#include <deal.II/base/signaling_nan.h>
#include <deal.II/base/thread_management.h>

#include <deal.II/differentiation/ad.h>

#include <deal.II/dofs/dof_accessor.h>

#include <deal.II/fe/fe.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/mapping.h>

#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>

#include <deal.II/lac/block_vector.h>
#include <deal.II/lac/la_parallel_block_vector.h>
#include <deal.II/lac/la_parallel_vector.h>
#include <deal.II/lac/la_vector.h>
#include <deal.II/lac/petsc_block_vector.h>
#include <deal.II/lac/petsc_vector.h>
#include <deal.II/lac/trilinos_epetra_vector.h>
#include <deal.II/lac/trilinos_parallel_block_vector.h>
#include <deal.II/lac/trilinos_tpetra_vector.h>
#include <deal.II/lac/trilinos_vector.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/vector_element_access.h>

#include <boost/container/small_vector.hpp>

#include <iomanip>
#include <memory>
#include <type_traits>

DEAL_II_NAMESPACE_OPEN


namespace internal
{
  template <class VectorType>
  typename VectorType::value_type inline get_vector_element(
    const VectorType &            vector,
    const types::global_dof_index cell_number)
  {
    return internal::ElementAccess<VectorType>::get(vector, cell_number);
  }



  IndexSet::value_type inline get_vector_element(
    const IndexSet &              is,
    const types::global_dof_index cell_number)
  {
    return (is.is_element(cell_number) ? 1 : 0);
  }



  template <int dim, int spacedim>
  inline std::vector<unsigned int>
  make_shape_function_to_row_table(const FiniteElement<dim, spacedim> &fe)
  {
    std::vector<unsigned int> shape_function_to_row_table(
      fe.n_dofs_per_cell() * fe.n_components(), numbers::invalid_unsigned_int);
    unsigned int row = 0;
    for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
      {
        // loop over all components that are nonzero for this particular
        // shape function. if a component is zero then we leave the
        // value in the table unchanged (at the invalid value)
        // otherwise it is mapped to the next free entry
        unsigned int nth_nonzero_component = 0;
        for (unsigned int c = 0; c < fe.n_components(); ++c)
          if (fe.get_nonzero_components(i)[c] == true)
            {
              shape_function_to_row_table[i * fe.n_components() + c] =
                row + nth_nonzero_component;
              ++nth_nonzero_component;
            }
        row += fe.n_nonzero_components(i);
      }

    return shape_function_to_row_table;
  }

  namespace
  {
    // Check to see if a DoF value is zero, implying that subsequent operations
    // with the value have no effect.
    template <typename Number, typename T = void>
    struct CheckForZero
    {
      static bool
      value(const Number &value)
      {
        return value == dealii::internal::NumberType<Number>::value(0.0);
      }
    };

    // For auto-differentiable numbers, the fact that a DoF value is zero
    // does not imply that its derivatives are zero as well. So we
    // can't filter by value for these number types.
    // Note that we also want to avoid actually checking the value itself,
    // since some AD numbers are not contextually convertible to booleans.
    template <typename Number>
    struct CheckForZero<
      Number,
      std::enable_if_t<Differentiation::AD::is_ad_number<Number>::value>>
    {
      static bool
      value(const Number & /*value*/)
      {
        return false;
      }
    };
  } // namespace
} // namespace internal



namespace FEValuesViews
{
  template <int dim, int spacedim>
  Scalar<dim, spacedim>::Scalar(const FEValuesBase<dim, spacedim> &fe_values,
                                const unsigned int                 component)
    : fe_values(&fe_values)
    , component(component)
    , shape_function_data(this->fe_values->fe->n_dofs_per_cell())
  {
    const FiniteElement<dim, spacedim> &fe = *this->fe_values->fe;
    AssertIndexRange(component, fe.n_components());

    // TODO: we'd like to use the fields with the same name as these
    // variables from FEValuesBase, but they aren't initialized yet
    // at the time we get here, so re-create it all
    const std::vector<unsigned int> shape_function_to_row_table =
      dealii::internal::make_shape_function_to_row_table(fe);

    for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
      {
        const bool is_primitive = fe.is_primitive() || fe.is_primitive(i);

        if (is_primitive == true)
          shape_function_data[i].is_nonzero_shape_function_component =
            (component == fe.system_to_component_index(i).first);
        else
          shape_function_data[i].is_nonzero_shape_function_component =
            (fe.get_nonzero_components(i)[component] == true);

        if (shape_function_data[i].is_nonzero_shape_function_component == true)
          shape_function_data[i].row_index =
            shape_function_to_row_table[i * fe.n_components() + component];
        else
          shape_function_data[i].row_index = numbers::invalid_unsigned_int;
      }
  }



  template <int dim, int spacedim>
  Scalar<dim, spacedim>::Scalar()
    : fe_values(nullptr)
    , component(numbers::invalid_unsigned_int)
  {}



  template <int dim, int spacedim>
  Vector<dim, spacedim>::Vector(const FEValuesBase<dim, spacedim> &fe_values,
                                const unsigned int first_vector_component)
    : fe_values(&fe_values)
    , first_vector_component(first_vector_component)
    , shape_function_data(this->fe_values->fe->n_dofs_per_cell())
  {
    const FiniteElement<dim, spacedim> &fe = *this->fe_values->fe;
    AssertIndexRange(first_vector_component + spacedim - 1, fe.n_components());

    // TODO: we'd like to use the fields with the same name as these
    // variables from FEValuesBase, but they aren't initialized yet
    // at the time we get here, so re-create it all
    const std::vector<unsigned int> shape_function_to_row_table =
      dealii::internal::make_shape_function_to_row_table(fe);

    for (unsigned int d = 0; d < spacedim; ++d)
      {
        const unsigned int component = first_vector_component + d;

        for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
          {
            const bool is_primitive = fe.is_primitive() || fe.is_primitive(i);

            if (is_primitive == true)
              shape_function_data[i].is_nonzero_shape_function_component[d] =
                (component == fe.system_to_component_index(i).first);
            else
              shape_function_data[i].is_nonzero_shape_function_component[d] =
                (fe.get_nonzero_components(i)[component] == true);

            if (shape_function_data[i].is_nonzero_shape_function_component[d] ==
                true)
              shape_function_data[i].row_index[d] =
                shape_function_to_row_table[i * fe.n_components() + component];
            else
              shape_function_data[i].row_index[d] =
                numbers::invalid_unsigned_int;
          }
      }

    for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
      {
        unsigned int n_nonzero_components = 0;
        for (unsigned int d = 0; d < spacedim; ++d)
          if (shape_function_data[i].is_nonzero_shape_function_component[d] ==
              true)
            ++n_nonzero_components;

        if (n_nonzero_components == 0)
          shape_function_data[i].single_nonzero_component = -2;
        else if (n_nonzero_components > 1)
          shape_function_data[i].single_nonzero_component = -1;
        else
          {
            for (unsigned int d = 0; d < spacedim; ++d)
              if (shape_function_data[i]
                    .is_nonzero_shape_function_component[d] == true)
                {
                  shape_function_data[i].single_nonzero_component =
                    shape_function_data[i].row_index[d];
                  shape_function_data[i].single_nonzero_component_index = d;
                  break;
                }
          }
      }
  }



  template <int dim, int spacedim>
  Vector<dim, spacedim>::Vector()
    : fe_values(nullptr)
    , first_vector_component(numbers::invalid_unsigned_int)
  {}



  template <int dim, int spacedim>
  SymmetricTensor<2, dim, spacedim>::SymmetricTensor(
    const FEValuesBase<dim, spacedim> &fe_values,
    const unsigned int                 first_tensor_component)
    : fe_values(&fe_values)
    , first_tensor_component(first_tensor_component)
    , shape_function_data(this->fe_values->fe->n_dofs_per_cell())
  {
    const FiniteElement<dim, spacedim> &fe = *this->fe_values->fe;
    Assert(first_tensor_component + (dim * dim + dim) / 2 - 1 <
             fe.n_components(),
           ExcIndexRange(
             first_tensor_component +
               dealii::SymmetricTensor<2, dim>::n_independent_components - 1,
             0,
             fe.n_components()));
    // TODO: we'd like to use the fields with the same name as these
    // variables from FEValuesBase, but they aren't initialized yet
    // at the time we get here, so re-create it all
    const std::vector<unsigned int> shape_function_to_row_table =
      dealii::internal::make_shape_function_to_row_table(fe);

    for (unsigned int d = 0;
         d < dealii::SymmetricTensor<2, dim>::n_independent_components;
         ++d)
      {
        const unsigned int component = first_tensor_component + d;

        for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
          {
            const bool is_primitive = fe.is_primitive() || fe.is_primitive(i);

            if (is_primitive == true)
              shape_function_data[i].is_nonzero_shape_function_component[d] =
                (component == fe.system_to_component_index(i).first);
            else
              shape_function_data[i].is_nonzero_shape_function_component[d] =
                (fe.get_nonzero_components(i)[component] == true);

            if (shape_function_data[i].is_nonzero_shape_function_component[d] ==
                true)
              shape_function_data[i].row_index[d] =
                shape_function_to_row_table[i * fe.n_components() + component];
            else
              shape_function_data[i].row_index[d] =
                numbers::invalid_unsigned_int;
          }
      }

    for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
      {
        unsigned int n_nonzero_components = 0;
        for (unsigned int d = 0;
             d < dealii::SymmetricTensor<2, dim>::n_independent_components;
             ++d)
          if (shape_function_data[i].is_nonzero_shape_function_component[d] ==
              true)
            ++n_nonzero_components;

        if (n_nonzero_components == 0)
          shape_function_data[i].single_nonzero_component = -2;
        else if (n_nonzero_components > 1)
          shape_function_data[i].single_nonzero_component = -1;
        else
          {
            for (unsigned int d = 0;
                 d < dealii::SymmetricTensor<2, dim>::n_independent_components;
                 ++d)
              if (shape_function_data[i]
                    .is_nonzero_shape_function_component[d] == true)
                {
                  shape_function_data[i].single_nonzero_component =
                    shape_function_data[i].row_index[d];
                  shape_function_data[i].single_nonzero_component_index = d;
                  break;
                }
          }
      }
  }



  template <int dim, int spacedim>
  SymmetricTensor<2, dim, spacedim>::SymmetricTensor()
    : fe_values(nullptr)
    , first_tensor_component(numbers::invalid_unsigned_int)
  {}



  template <int dim, int spacedim>
  Tensor<2, dim, spacedim>::Tensor(const FEValuesBase<dim, spacedim> &fe_values,
                                   const unsigned int first_tensor_component)
    : fe_values(&fe_values)
    , first_tensor_component(first_tensor_component)
    , shape_function_data(this->fe_values->fe->n_dofs_per_cell())
  {
    const FiniteElement<dim, spacedim> &fe = *this->fe_values->fe;
    AssertIndexRange(first_tensor_component + dim * dim - 1, fe.n_components());
    // TODO: we'd like to use the fields with the same name as these
    // variables from FEValuesBase, but they aren't initialized yet
    // at the time we get here, so re-create it all
    const std::vector<unsigned int> shape_function_to_row_table =
      dealii::internal::make_shape_function_to_row_table(fe);

    for (unsigned int d = 0; d < dim * dim; ++d)
      {
        const unsigned int component = first_tensor_component + d;

        for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
          {
            const bool is_primitive = fe.is_primitive() || fe.is_primitive(i);

            if (is_primitive == true)
              shape_function_data[i].is_nonzero_shape_function_component[d] =
                (component == fe.system_to_component_index(i).first);
            else
              shape_function_data[i].is_nonzero_shape_function_component[d] =
                (fe.get_nonzero_components(i)[component] == true);

            if (shape_function_data[i].is_nonzero_shape_function_component[d] ==
                true)
              shape_function_data[i].row_index[d] =
                shape_function_to_row_table[i * fe.n_components() + component];
            else
              shape_function_data[i].row_index[d] =
                numbers::invalid_unsigned_int;
          }
      }

    for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
      {
        unsigned int n_nonzero_components = 0;
        for (unsigned int d = 0; d < dim * dim; ++d)
          if (shape_function_data[i].is_nonzero_shape_function_component[d] ==
              true)
            ++n_nonzero_components;

        if (n_nonzero_components == 0)
          shape_function_data[i].single_nonzero_component = -2;
        else if (n_nonzero_components > 1)
          shape_function_data[i].single_nonzero_component = -1;
        else
          {
            for (unsigned int d = 0; d < dim * dim; ++d)
              if (shape_function_data[i]
                    .is_nonzero_shape_function_component[d] == true)
                {
                  shape_function_data[i].single_nonzero_component =
                    shape_function_data[i].row_index[d];
                  shape_function_data[i].single_nonzero_component_index = d;
                  break;
                }
          }
      }
  }



  template <int dim, int spacedim>
  Tensor<2, dim, spacedim>::Tensor()
    : fe_values(nullptr)
    , first_tensor_component(numbers::invalid_unsigned_int)
  {}



  namespace internal
  {
    // Given values of degrees of freedom, evaluate the
    // values/gradients/... at quadrature points

    // ------------------------- scalar functions --------------------------
    template <int dim, int spacedim, typename Number>
    void
    do_function_values(
      const ArrayView<Number> &dof_values,
      const Table<2, double> & shape_values,
      const std::vector<typename Scalar<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename ProductType<Number, double>::type> &values)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = values.size();

      std::fill(values.begin(),
                values.end(),
                dealii::internal::NumberType<Number>::value(0.0));

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        if (shape_function_data[shape_function]
              .is_nonzero_shape_function_component)
          {
            const Number &value = dof_values[shape_function];
            // For auto-differentiable numbers, the fact that a DoF value is
            // zero does not imply that its derivatives are zero as well. So we
            // can't filter by value for these number types.
            if (dealii::internal::CheckForZero<Number>::value(value) == true)
              continue;

            const double *shape_value_ptr =
              &shape_values(shape_function_data[shape_function].row_index, 0);
            for (unsigned int q_point = 0; q_point < n_quadrature_points;
                 ++q_point)
              values[q_point] += value * (*shape_value_ptr++);
          }
    }



    // same code for gradient and Hessian, template argument 'order' to give
    // the order of the derivative (= rank of gradient/Hessian tensor)
    template <int order, int dim, int spacedim, typename Number>
    void
    do_function_derivatives(
      const ArrayView<Number> &                        dof_values,
      const Table<2, dealii::Tensor<order, spacedim>> &shape_derivatives,
      const std::vector<typename Scalar<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<
        typename ProductType<Number, dealii::Tensor<order, spacedim>>::type>
        &derivatives)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = derivatives.size();

      std::fill(
        derivatives.begin(),
        derivatives.end(),
        typename ProductType<Number, dealii::Tensor<order, spacedim>>::type());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        if (shape_function_data[shape_function]
              .is_nonzero_shape_function_component)
          {
            const Number &value = dof_values[shape_function];
            // For auto-differentiable numbers, the fact that a DoF value is
            // zero does not imply that its derivatives are zero as well. So we
            // can't filter by value for these number types.
            if (dealii::internal::CheckForZero<Number>::value(value) == true)
              continue;

            const dealii::Tensor<order, spacedim> *shape_derivative_ptr =
              &shape_derivatives[shape_function_data[shape_function].row_index]
                                [0];
            for (unsigned int q_point = 0; q_point < n_quadrature_points;
                 ++q_point)
              derivatives[q_point] += value * (*shape_derivative_ptr++);
          }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_laplacians(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<2, spacedim>> &shape_hessians,
      const std::vector<typename Scalar<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename Scalar<dim, spacedim>::
                    template solution_laplacian_type<Number>> &laplacians)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = laplacians.size();

      std::fill(
        laplacians.begin(),
        laplacians.end(),
        typename Scalar<dim,
                        spacedim>::template solution_laplacian_type<Number>());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        if (shape_function_data[shape_function]
              .is_nonzero_shape_function_component)
          {
            const Number &value = dof_values[shape_function];
            // For auto-differentiable numbers, the fact that a DoF value is
            // zero does not imply that its derivatives are zero as well. So we
            // can't filter by value for these number types.
            if (dealii::internal::CheckForZero<Number>::value(value) == true)
              continue;

            const dealii::Tensor<2, spacedim> *shape_hessian_ptr =
              &shape_hessians[shape_function_data[shape_function].row_index][0];
            for (unsigned int q_point = 0; q_point < n_quadrature_points;
                 ++q_point)
              laplacians[q_point] += value * trace(*shape_hessian_ptr++);
          }
    }



    // ----------------------------- vector part ---------------------------

    template <int dim, int spacedim, typename Number>
    void
    do_function_values(
      const ArrayView<Number> &dof_values,
      const Table<2, double> & shape_values,
      const std::vector<typename Vector<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<
        typename ProductType<Number, dealii::Tensor<1, spacedim>>::type>
        &values)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = values.size();

      std::fill(
        values.begin(),
        values.end(),
        typename ProductType<Number, dealii::Tensor<1, spacedim>>::type());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;
              const double *shape_value_ptr = &shape_values(snc, 0);
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                values[q_point][comp] += value * (*shape_value_ptr++);
            }
          else
            for (unsigned int d = 0; d < spacedim; ++d)
              if (shape_function_data[shape_function]
                    .is_nonzero_shape_function_component[d])
                {
                  const double *shape_value_ptr = &shape_values(
                    shape_function_data[shape_function].row_index[d], 0);
                  for (unsigned int q_point = 0; q_point < n_quadrature_points;
                       ++q_point)
                    values[q_point][d] += value * (*shape_value_ptr++);
                }
        }
    }



    template <int order, int dim, int spacedim, typename Number>
    void
    do_function_derivatives(
      const ArrayView<Number> &                        dof_values,
      const Table<2, dealii::Tensor<order, spacedim>> &shape_derivatives,
      const std::vector<typename Vector<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<
        typename ProductType<Number, dealii::Tensor<order + 1, spacedim>>::type>
        &derivatives)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = derivatives.size();

      std::fill(
        derivatives.begin(),
        derivatives.end(),
        typename ProductType<Number,
                             dealii::Tensor<order + 1, spacedim>>::type());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;
              const dealii::Tensor<order, spacedim> *shape_derivative_ptr =
                &shape_derivatives[snc][0];
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                derivatives[q_point][comp] += value * (*shape_derivative_ptr++);
            }
          else
            for (unsigned int d = 0; d < spacedim; ++d)
              if (shape_function_data[shape_function]
                    .is_nonzero_shape_function_component[d])
                {
                  const dealii::Tensor<order, spacedim> *shape_derivative_ptr =
                    &shape_derivatives[shape_function_data[shape_function]
                                         .row_index[d]][0];
                  for (unsigned int q_point = 0; q_point < n_quadrature_points;
                       ++q_point)
                    derivatives[q_point][d] +=
                      value * (*shape_derivative_ptr++);
                }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_symmetric_gradients(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<1, spacedim>> &shape_gradients,
      const std::vector<typename Vector<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<
        typename ProductType<Number,
                             dealii::SymmetricTensor<2, spacedim>>::type>
        &symmetric_gradients)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = symmetric_gradients.size();

      std::fill(
        symmetric_gradients.begin(),
        symmetric_gradients.end(),
        typename ProductType<Number,
                             dealii::SymmetricTensor<2, spacedim>>::type());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;
              const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                &shape_gradients[snc][0];
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                symmetric_gradients[q_point] +=
                  value * dealii::SymmetricTensor<2, spacedim>(
                            symmetrize_single_row(comp, *shape_gradient_ptr++));
            }
          else
            for (unsigned int q_point = 0; q_point < n_quadrature_points;
                 ++q_point)
              {
                typename ProductType<Number, dealii::Tensor<2, spacedim>>::type
                  grad;
                for (unsigned int d = 0; d < spacedim; ++d)
                  if (shape_function_data[shape_function]
                        .is_nonzero_shape_function_component[d])
                    grad[d] =
                      value *
                      shape_gradients[shape_function_data[shape_function]
                                        .row_index[d]][q_point];
                symmetric_gradients[q_point] += symmetrize(grad);
              }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_divergences(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<1, spacedim>> &shape_gradients,
      const std::vector<typename Vector<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename Vector<dim, spacedim>::
                    template solution_divergence_type<Number>> &divergences)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = divergences.size();

      std::fill(
        divergences.begin(),
        divergences.end(),
        typename Vector<dim,
                        spacedim>::template solution_divergence_type<Number>());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;
              const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                &shape_gradients[snc][0];
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                divergences[q_point] += value * (*shape_gradient_ptr++)[comp];
            }
          else
            for (unsigned int d = 0; d < spacedim; ++d)
              if (shape_function_data[shape_function]
                    .is_nonzero_shape_function_component[d])
                {
                  const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                    &shape_gradients[shape_function_data[shape_function]
                                       .row_index[d]][0];
                  for (unsigned int q_point = 0; q_point < n_quadrature_points;
                       ++q_point)
                    divergences[q_point] += value * (*shape_gradient_ptr++)[d];
                }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_curls(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<1, spacedim>> &shape_gradients,
      const std::vector<typename Vector<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename ProductType<
        Number,
        typename dealii::internal::CurlType<spacedim>::type>::type> &curls)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = curls.size();

      std::fill(curls.begin(),
                curls.end(),
                typename ProductType<
                  Number,
                  typename dealii::internal::CurlType<spacedim>::type>::type());

      switch (spacedim)
        {
          case 1:
            {
              Assert(false,
                     ExcMessage(
                       "Computing the curl in 1d is not a useful operation"));
              break;
            }

          case 2:
            {
              for (unsigned int shape_function = 0;
                   shape_function < dofs_per_cell;
                   ++shape_function)
                {
                  const int snc = shape_function_data[shape_function]
                                    .single_nonzero_component;

                  if (snc == -2)
                    // shape function is zero for the selected components
                    continue;

                  const Number &value = dof_values[shape_function];
                  // For auto-differentiable numbers, the fact that a DoF value
                  // is zero does not imply that its derivatives are zero as
                  // well. So we can't filter by value for these number types.
                  if (dealii::internal::CheckForZero<Number>::value(value) ==
                      true)
                    continue;

                  if (snc != -1)
                    {
                      const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                        &shape_gradients[snc][0];

                      Assert(shape_function_data[shape_function]
                                 .single_nonzero_component >= 0,
                             ExcInternalError());
                      // we're in 2d, so the formula for the curl is simple:
                      if (shape_function_data[shape_function]
                            .single_nonzero_component_index == 0)
                        for (unsigned int q_point = 0;
                             q_point < n_quadrature_points;
                             ++q_point)
                          curls[q_point][0] -=
                            value * (*shape_gradient_ptr++)[1];
                      else
                        for (unsigned int q_point = 0;
                             q_point < n_quadrature_points;
                             ++q_point)
                          curls[q_point][0] +=
                            value * (*shape_gradient_ptr++)[0];
                    }
                  else
                    // we have multiple non-zero components in the shape
                    // functions. not all of them must necessarily be within the
                    // 2-component window this FEValuesViews::Vector object
                    // considers, however.
                    {
                      if (shape_function_data[shape_function]
                            .is_nonzero_shape_function_component[0])
                        {
                          const dealii::Tensor<1,
                                               spacedim> *shape_gradient_ptr =
                            &shape_gradients[shape_function_data[shape_function]
                                               .row_index[0]][0];

                          for (unsigned int q_point = 0;
                               q_point < n_quadrature_points;
                               ++q_point)
                            curls[q_point][0] -=
                              value * (*shape_gradient_ptr++)[1];
                        }

                      if (shape_function_data[shape_function]
                            .is_nonzero_shape_function_component[1])
                        {
                          const dealii::Tensor<1,
                                               spacedim> *shape_gradient_ptr =
                            &shape_gradients[shape_function_data[shape_function]
                                               .row_index[1]][0];

                          for (unsigned int q_point = 0;
                               q_point < n_quadrature_points;
                               ++q_point)
                            curls[q_point][0] +=
                              value * (*shape_gradient_ptr++)[0];
                        }
                    }
                }
              break;
            }

          case 3:
            {
              for (unsigned int shape_function = 0;
                   shape_function < dofs_per_cell;
                   ++shape_function)
                {
                  const int snc = shape_function_data[shape_function]
                                    .single_nonzero_component;

                  if (snc == -2)
                    // shape function is zero for the selected components
                    continue;

                  const Number &value = dof_values[shape_function];
                  // For auto-differentiable numbers, the fact that a DoF value
                  // is zero does not imply that its derivatives are zero as
                  // well. So we can't filter by value for these number types.
                  if (dealii::internal::CheckForZero<Number>::value(value) ==
                      true)
                    continue;

                  if (snc != -1)
                    {
                      const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                        &shape_gradients[snc][0];

                      switch (shape_function_data[shape_function]
                                .single_nonzero_component_index)
                        {
                          case 0:
                            {
                              for (unsigned int q_point = 0;
                                   q_point < n_quadrature_points;
                                   ++q_point)
                                {
                                  curls[q_point][1] +=
                                    value * (*shape_gradient_ptr)[2];
                                  curls[q_point][2] -=
                                    value * (*shape_gradient_ptr++)[1];
                                }

                              break;
                            }

                          case 1:
                            {
                              for (unsigned int q_point = 0;
                                   q_point < n_quadrature_points;
                                   ++q_point)
                                {
                                  curls[q_point][0] -=
                                    value * (*shape_gradient_ptr)[2];
                                  curls[q_point][2] +=
                                    value * (*shape_gradient_ptr++)[0];
                                }

                              break;
                            }

                          case 2:
                            {
                              for (unsigned int q_point = 0;
                                   q_point < n_quadrature_points;
                                   ++q_point)
                                {
                                  curls[q_point][0] +=
                                    value * (*shape_gradient_ptr)[1];
                                  curls[q_point][1] -=
                                    value * (*shape_gradient_ptr++)[0];
                                }
                              break;
                            }

                          default:
                            Assert(false, ExcInternalError());
                        }
                    }

                  else
                    // we have multiple non-zero components in the shape
                    // functions. not all of them must necessarily be within the
                    // 3-component window this FEValuesViews::Vector object
                    // considers, however.
                    {
                      if (shape_function_data[shape_function]
                            .is_nonzero_shape_function_component[0])
                        {
                          const dealii::Tensor<1,
                                               spacedim> *shape_gradient_ptr =
                            &shape_gradients[shape_function_data[shape_function]
                                               .row_index[0]][0];

                          for (unsigned int q_point = 0;
                               q_point < n_quadrature_points;
                               ++q_point)
                            {
                              curls[q_point][1] +=
                                value * (*shape_gradient_ptr)[2];
                              curls[q_point][2] -=
                                value * (*shape_gradient_ptr++)[1];
                            }
                        }

                      if (shape_function_data[shape_function]
                            .is_nonzero_shape_function_component[1])
                        {
                          const dealii::Tensor<1,
                                               spacedim> *shape_gradient_ptr =
                            &shape_gradients[shape_function_data[shape_function]
                                               .row_index[1]][0];

                          for (unsigned int q_point = 0;
                               q_point < n_quadrature_points;
                               ++q_point)
                            {
                              curls[q_point][0] -=
                                value * (*shape_gradient_ptr)[2];
                              curls[q_point][2] +=
                                value * (*shape_gradient_ptr++)[0];
                            }
                        }

                      if (shape_function_data[shape_function]
                            .is_nonzero_shape_function_component[2])
                        {
                          const dealii::Tensor<1,
                                               spacedim> *shape_gradient_ptr =
                            &shape_gradients[shape_function_data[shape_function]
                                               .row_index[2]][0];

                          for (unsigned int q_point = 0;
                               q_point < n_quadrature_points;
                               ++q_point)
                            {
                              curls[q_point][0] +=
                                value * (*shape_gradient_ptr)[1];
                              curls[q_point][1] -=
                                value * (*shape_gradient_ptr++)[0];
                            }
                        }
                    }
                }
            }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_laplacians(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<2, spacedim>> &shape_hessians,
      const std::vector<typename Vector<dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename Vector<dim, spacedim>::
                    template solution_laplacian_type<Number>> &laplacians)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = laplacians.size();

      std::fill(
        laplacians.begin(),
        laplacians.end(),
        typename Vector<dim,
                        spacedim>::template solution_laplacian_type<Number>());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;
              const dealii::Tensor<2, spacedim> *shape_hessian_ptr =
                &shape_hessians[snc][0];
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                laplacians[q_point][comp] +=
                  value * trace(*shape_hessian_ptr++);
            }
          else
            for (unsigned int d = 0; d < spacedim; ++d)
              if (shape_function_data[shape_function]
                    .is_nonzero_shape_function_component[d])
                {
                  const dealii::Tensor<2, spacedim> *shape_hessian_ptr =
                    &shape_hessians[shape_function_data[shape_function]
                                      .row_index[d]][0];
                  for (unsigned int q_point = 0; q_point < n_quadrature_points;
                       ++q_point)
                    laplacians[q_point][d] +=
                      value * trace(*shape_hessian_ptr++);
                }
        }
    }



    // ---------------------- symmetric tensor part ------------------------

    template <int dim, int spacedim, typename Number>
    void
    do_function_values(
      const ArrayView<Number> &       dof_values,
      const dealii::Table<2, double> &shape_values,
      const std::vector<
        typename SymmetricTensor<2, dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<
        typename ProductType<Number,
                             dealii::SymmetricTensor<2, spacedim>>::type>
        &values)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = values.size();

      std::fill(
        values.begin(),
        values.end(),
        typename ProductType<Number,
                             dealii::SymmetricTensor<2, spacedim>>::type());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const TableIndices<2> comp = dealii::
                SymmetricTensor<2, spacedim>::unrolled_to_component_indices(
                  shape_function_data[shape_function]
                    .single_nonzero_component_index);
              const double *shape_value_ptr = &shape_values(snc, 0);
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                values[q_point][comp] += value * (*shape_value_ptr++);
            }
          else
            for (unsigned int d = 0;
                 d <
                 dealii::SymmetricTensor<2, spacedim>::n_independent_components;
                 ++d)
              if (shape_function_data[shape_function]
                    .is_nonzero_shape_function_component[d])
                {
                  const TableIndices<2> comp =
                    dealii::SymmetricTensor<2, spacedim>::
                      unrolled_to_component_indices(d);
                  const double *shape_value_ptr = &shape_values(
                    shape_function_data[shape_function].row_index[d], 0);
                  for (unsigned int q_point = 0; q_point < n_quadrature_points;
                       ++q_point)
                    values[q_point][comp] += value * (*shape_value_ptr++);
                }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_divergences(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<1, spacedim>> &shape_gradients,
      const std::vector<
        typename SymmetricTensor<2, dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename SymmetricTensor<2, dim, spacedim>::
                    template solution_divergence_type<Number>> &divergences)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = divergences.size();

      std::fill(divergences.begin(),
                divergences.end(),
                typename SymmetricTensor<2, dim, spacedim>::
                  template solution_divergence_type<Number>());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;

              const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                &shape_gradients[snc][0];

              const unsigned int ii = dealii::SymmetricTensor<2, spacedim>::
                unrolled_to_component_indices(comp)[0];
              const unsigned int jj = dealii::SymmetricTensor<2, spacedim>::
                unrolled_to_component_indices(comp)[1];

              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point, ++shape_gradient_ptr)
                {
                  divergences[q_point][ii] += value * (*shape_gradient_ptr)[jj];

                  if (ii != jj)
                    divergences[q_point][jj] +=
                      value * (*shape_gradient_ptr)[ii];
                }
            }
          else
            {
              for (unsigned int d = 0;
                   d <
                   dealii::SymmetricTensor<2,
                                           spacedim>::n_independent_components;
                   ++d)
                if (shape_function_data[shape_function]
                      .is_nonzero_shape_function_component[d])
                  {
                    Assert(false, ExcNotImplemented());

                    // the following implementation needs to be looked over -- I
                    // think it can't be right, because we are in a case where
                    // there is no single nonzero component
                    //
                    // the following is not implemented! we need to consider the
                    // interplay between multiple non-zero entries in shape
                    // function and the representation as a symmetric
                    // second-order tensor
                    const unsigned int comp =
                      shape_function_data[shape_function]
                        .single_nonzero_component_index;

                    const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                      &shape_gradients[shape_function_data[shape_function]
                                         .row_index[d]][0];
                    for (unsigned int q_point = 0;
                         q_point < n_quadrature_points;
                         ++q_point, ++shape_gradient_ptr)
                      {
                        for (unsigned int j = 0; j < spacedim; ++j)
                          {
                            const unsigned int vector_component =
                              dealii::SymmetricTensor<2, spacedim>::
                                component_to_unrolled_index(
                                  TableIndices<2>(comp, j));
                            divergences[q_point][vector_component] +=
                              value * (*shape_gradient_ptr++)[j];
                          }
                      }
                  }
            }
        }
    }

    // ---------------------- non-symmetric tensor part ------------------------

    template <int dim, int spacedim, typename Number>
    void
    do_function_values(
      const ArrayView<Number> &       dof_values,
      const dealii::Table<2, double> &shape_values,
      const std::vector<typename Tensor<2, dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<
        typename ProductType<Number, dealii::Tensor<2, spacedim>>::type>
        &values)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = values.size();

      std::fill(
        values.begin(),
        values.end(),
        typename ProductType<Number, dealii::Tensor<2, spacedim>>::type());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;

              const TableIndices<2> indices =
                dealii::Tensor<2, spacedim>::unrolled_to_component_indices(
                  comp);

              const double *shape_value_ptr = &shape_values(snc, 0);
              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point)
                values[q_point][indices] += value * (*shape_value_ptr++);
            }
          else
            for (unsigned int d = 0; d < dim * dim; ++d)
              if (shape_function_data[shape_function]
                    .is_nonzero_shape_function_component[d])
                {
                  const TableIndices<2> indices =
                    dealii::Tensor<2, spacedim>::unrolled_to_component_indices(
                      d);

                  const double *shape_value_ptr = &shape_values(
                    shape_function_data[shape_function].row_index[d], 0);
                  for (unsigned int q_point = 0; q_point < n_quadrature_points;
                       ++q_point)
                    values[q_point][indices] += value * (*shape_value_ptr++);
                }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_divergences(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<1, spacedim>> &shape_gradients,
      const std::vector<typename Tensor<2, dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename Tensor<2, dim, spacedim>::
                    template solution_divergence_type<Number>> &divergences)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = divergences.size();

      std::fill(
        divergences.begin(),
        divergences.end(),
        typename Tensor<2, dim, spacedim>::template solution_divergence_type<
          Number>());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;

              const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                &shape_gradients[snc][0];

              const TableIndices<2> indices =
                dealii::Tensor<2, spacedim>::unrolled_to_component_indices(
                  comp);
              const unsigned int ii = indices[0];
              const unsigned int jj = indices[1];

              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point, ++shape_gradient_ptr)
                {
                  divergences[q_point][ii] += value * (*shape_gradient_ptr)[jj];
                }
            }
          else
            {
              for (unsigned int d = 0; d < dim * dim; ++d)
                if (shape_function_data[shape_function]
                      .is_nonzero_shape_function_component[d])
                  {
                    Assert(false, ExcNotImplemented());
                  }
            }
        }
    }



    template <int dim, int spacedim, typename Number>
    void
    do_function_gradients(
      const ArrayView<Number> &                    dof_values,
      const Table<2, dealii::Tensor<1, spacedim>> &shape_gradients,
      const std::vector<typename Tensor<2, dim, spacedim>::ShapeFunctionData>
        &shape_function_data,
      std::vector<typename Tensor<2, dim, spacedim>::
                    template solution_gradient_type<Number>> &gradients)
    {
      const unsigned int dofs_per_cell       = dof_values.size();
      const unsigned int n_quadrature_points = gradients.size();

      std::fill(
        gradients.begin(),
        gradients.end(),
        typename Tensor<2, dim, spacedim>::template solution_gradient_type<
          Number>());

      for (unsigned int shape_function = 0; shape_function < dofs_per_cell;
           ++shape_function)
        {
          const int snc =
            shape_function_data[shape_function].single_nonzero_component;

          if (snc == -2)
            // shape function is zero for the selected components
            continue;

          const Number &value = dof_values[shape_function];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (snc != -1)
            {
              const unsigned int comp = shape_function_data[shape_function]
                                          .single_nonzero_component_index;

              const dealii::Tensor<1, spacedim> *shape_gradient_ptr =
                &shape_gradients[snc][0];

              const TableIndices<2> indices =
                dealii::Tensor<2, spacedim>::unrolled_to_component_indices(
                  comp);
              const unsigned int ii = indices[0];
              const unsigned int jj = indices[1];

              for (unsigned int q_point = 0; q_point < n_quadrature_points;
                   ++q_point, ++shape_gradient_ptr)
                {
                  gradients[q_point][ii][jj] += value * (*shape_gradient_ptr);
                }
            }
          else
            {
              for (unsigned int d = 0; d < dim * dim; ++d)
                if (shape_function_data[shape_function]
                      .is_nonzero_shape_function_component[d])
                  {
                    Assert(false, ExcNotImplemented());
                  }
            }
        }
    }

  } // end of namespace internal



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_values(
    const InputVector &fe_function,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell and call internal worker
    // function
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_values_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_gradients(
    const InputVector &fe_function,
    std::vector<solution_gradient_type<typename InputVector::value_type>>
      &gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_derivatives<1, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_gradients_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_gradient_type<typename InputVector::value_type>>
      &gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_derivatives<1, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_hessians(
    const InputVector &fe_function,
    std::vector<solution_hessian_type<typename InputVector::value_type>>
      &hessians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_derivatives<2, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      hessians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_hessians_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_hessian_type<typename InputVector::value_type>>
      &hessians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_derivatives<2, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      hessians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_laplacians(
    const InputVector &fe_function,
    std::vector<solution_laplacian_type<typename InputVector::value_type>>
      &laplacians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_laplacians<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      laplacians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_laplacians_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_laplacian_type<typename InputVector::value_type>>
      &laplacians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_laplacians<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      laplacians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_third_derivatives(
    const InputVector &fe_function,
    std::vector<
      solution_third_derivative_type<typename InputVector::value_type>>
      &third_derivatives) const
  {
    Assert(fe_values->update_flags & update_3rd_derivatives,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_3rd_derivatives")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_derivatives<3, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_3rd_derivatives,
      shape_function_data,
      third_derivatives);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Scalar<dim, spacedim>::get_function_third_derivatives_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<
      solution_third_derivative_type<typename InputVector::value_type>>
      &third_derivatives) const
  {
    Assert(fe_values->update_flags & update_3rd_derivatives,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_3rd_derivatives")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_derivatives<3, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_3rd_derivatives,
      shape_function_data,
      third_derivatives);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_values(
    const InputVector &fe_function,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_values_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_gradients(
    const InputVector &fe_function,
    std::vector<solution_gradient_type<typename InputVector::value_type>>
      &gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_derivatives<1, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_gradients_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_gradient_type<typename InputVector::value_type>>
      &gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_derivatives<1, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_symmetric_gradients(
    const InputVector &fe_function,
    std::vector<
      solution_symmetric_gradient_type<typename InputVector::value_type>>
      &symmetric_gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_symmetric_gradients<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      symmetric_gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_symmetric_gradients_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<
      solution_symmetric_gradient_type<typename InputVector::value_type>>
      &symmetric_gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_symmetric_gradients<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      symmetric_gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_divergences(
    const InputVector &fe_function,
    std::vector<solution_divergence_type<typename InputVector::value_type>>
      &divergences) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs
    // on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_divergences<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      divergences);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_divergences_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_divergence_type<typename InputVector::value_type>>
      &divergences) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_divergences<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      divergences);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_curls(
    const InputVector &fe_function,
    std::vector<solution_curl_type<typename InputVector::value_type>> &curls)
    const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           ExcMessage("FEValues object is not reinited to any cell"));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_curls<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      curls);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_curls_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_curl_type<typename InputVector::value_type>> &curls)
    const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           ExcMessage("FEValues object is not reinited to any cell"));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_curls<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      curls);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_hessians(
    const InputVector &fe_function,
    std::vector<solution_hessian_type<typename InputVector::value_type>>
      &hessians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_derivatives<2, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      hessians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_hessians_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_hessian_type<typename InputVector::value_type>>
      &hessians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_derivatives<2, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      hessians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_laplacians(
    const InputVector &fe_function,
    std::vector<solution_value_type<typename InputVector::value_type>>
      &laplacians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(laplacians.size() == fe_values->n_quadrature_points,
           ExcDimensionMismatch(laplacians.size(),
                                fe_values->n_quadrature_points));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    Assert(
      fe_function.size() == fe_values->present_cell.n_dofs_for_dof_handler(),
      ExcDimensionMismatch(fe_function.size(),
                           fe_values->present_cell.n_dofs_for_dof_handler()));

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_laplacians<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      laplacians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_laplacians_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_laplacian_type<typename InputVector::value_type>>
      &laplacians) const
  {
    Assert(fe_values->update_flags & update_hessians,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_hessians")));
    Assert(laplacians.size() == fe_values->n_quadrature_points,
           ExcDimensionMismatch(laplacians.size(),
                                fe_values->n_quadrature_points));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_laplacians<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_hessians,
      shape_function_data,
      laplacians);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_third_derivatives(
    const InputVector &fe_function,
    std::vector<
      solution_third_derivative_type<typename InputVector::value_type>>
      &third_derivatives) const
  {
    Assert(fe_values->update_flags & update_3rd_derivatives,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_3rd_derivatives")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_derivatives<3, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_3rd_derivatives,
      shape_function_data,
      third_derivatives);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Vector<dim, spacedim>::get_function_third_derivatives_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<
      solution_third_derivative_type<typename InputVector::value_type>>
      &third_derivatives) const
  {
    Assert(fe_values->update_flags & update_3rd_derivatives,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_3rd_derivatives")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_derivatives<3, dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_3rd_derivatives,
      shape_function_data,
      third_derivatives);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  SymmetricTensor<2, dim, spacedim>::get_function_values(
    const InputVector &fe_function,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  SymmetricTensor<2, dim, spacedim>::get_function_values_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  SymmetricTensor<2, dim, spacedim>::get_function_divergences(
    const InputVector &fe_function,
    std::vector<solution_divergence_type<typename InputVector::value_type>>
      &divergences) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs
    // on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_divergences<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      divergences);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  SymmetricTensor<2, dim, spacedim>::
    get_function_divergences_from_local_dof_values(
      const InputVector &dof_values,
      std::vector<solution_divergence_type<typename InputVector::value_type>>
        &divergences) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_divergences<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      divergences);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Tensor<2, dim, spacedim>::get_function_values(
    const InputVector &fe_function,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Tensor<2, dim, spacedim>::get_function_values_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_value_type<typename InputVector::value_type>> &values)
    const
  {
    Assert(fe_values->update_flags & update_values,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_values")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_values<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_values,
      shape_function_data,
      values);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Tensor<2, dim, spacedim>::get_function_divergences(
    const InputVector &fe_function,
    std::vector<solution_divergence_type<typename InputVector::value_type>>
      &divergences) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs
    // on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_divergences<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      divergences);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Tensor<2, dim, spacedim>::get_function_divergences_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_divergence_type<typename InputVector::value_type>>
      &divergences) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_divergences<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      divergences);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Tensor<2, dim, spacedim>::get_function_gradients(
    const InputVector &fe_function,
    std::vector<solution_gradient_type<typename InputVector::value_type>>
      &gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(fe_function.size(),
                    fe_values->present_cell.n_dofs_for_dof_handler());

    // get function values of dofs
    // on this cell
    dealii::Vector<typename InputVector::value_type> dof_values(
      fe_values->dofs_per_cell);
    fe_values->present_cell.get_interpolated_dof_values(fe_function,
                                                        dof_values);
    internal::do_function_gradients<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      gradients);
  }



  template <int dim, int spacedim>
  template <class InputVector>
  void
  Tensor<2, dim, spacedim>::get_function_gradients_from_local_dof_values(
    const InputVector &dof_values,
    std::vector<solution_gradient_type<typename InputVector::value_type>>
      &gradients) const
  {
    Assert(fe_values->update_flags & update_gradients,
           (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
             "update_gradients")));
    Assert(fe_values->present_cell.is_initialized(),
           (typename FEValuesBase<dim, spacedim>::ExcNotReinited()));
    AssertDimension(dof_values.size(), fe_values->dofs_per_cell);

    internal::do_function_gradients<dim, spacedim>(
      make_array_view(dof_values.begin(), dof_values.end()),
      fe_values->finite_element_output.shape_gradients,
      shape_function_data,
      gradients);
  }

} // namespace FEValuesViews


namespace internal
{
  namespace FEValuesViews
  {
    template <int dim, int spacedim>
    Cache<dim, spacedim>::Cache(const FEValuesBase<dim, spacedim> &fe_values)
    {
      const FiniteElement<dim, spacedim> &fe = fe_values.get_fe();

      const unsigned int n_scalars = fe.n_components();
      scalars.reserve(n_scalars);
      for (unsigned int component = 0; component < n_scalars; ++component)
        scalars.emplace_back(fe_values, component);

      // compute number of vectors that we can fit into this finite element.
      // note that this is based on the dimensionality 'dim' of the manifold,
      // not 'spacedim' of the output vector
      const unsigned int n_vectors =
        (fe.n_components() >= Tensor<1, spacedim>::n_independent_components ?
           fe.n_components() - Tensor<1, spacedim>::n_independent_components +
             1 :
           0);
      vectors.reserve(n_vectors);
      for (unsigned int component = 0; component < n_vectors; ++component)
        vectors.emplace_back(fe_values, component);

      // compute number of symmetric tensors in the same way as above
      const unsigned int n_symmetric_second_order_tensors =
        (fe.n_components() >=
             SymmetricTensor<2, spacedim>::n_independent_components ?
           fe.n_components() -
             SymmetricTensor<2, spacedim>::n_independent_components + 1 :
           0);
      symmetric_second_order_tensors.reserve(n_symmetric_second_order_tensors);
      for (unsigned int component = 0;
           component < n_symmetric_second_order_tensors;
           ++component)
        symmetric_second_order_tensors.emplace_back(fe_values, component);


      // compute number of symmetric tensors in the same way as above
      const unsigned int n_second_order_tensors =
        (fe.n_components() >= Tensor<2, spacedim>::n_independent_components ?
           fe.n_components() - Tensor<2, spacedim>::n_independent_components +
             1 :
           0);
      second_order_tensors.reserve(n_second_order_tensors);
      for (unsigned int component = 0; component < n_second_order_tensors;
           ++component)
        second_order_tensors.emplace_back(fe_values, component);
    }
  } // namespace FEValuesViews
} // namespace internal


/* ---------------- FEValuesBase<dim,spacedim>::CellIteratorContainer ---------
 */

template <int dim, int spacedim>
FEValuesBase<dim, spacedim>::CellIteratorContainer::CellIteratorContainer()
  : initialized(false)
  , cell(typename Triangulation<dim, spacedim>::cell_iterator(nullptr, -1, -1))
  , dof_handler(nullptr)
  , level_dof_access(false)
{}



template <int dim, int spacedim>
FEValuesBase<dim, spacedim>::CellIteratorContainer::CellIteratorContainer(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell)
  : initialized(true)
  , cell(cell)
  , dof_handler(nullptr)
  , level_dof_access(false)
{}



template <int dim, int spacedim>
bool
FEValuesBase<dim, spacedim>::CellIteratorContainer::is_initialized() const
{
  return initialized;
}



template <int dim, int spacedim>
FEValuesBase<dim, spacedim>::CellIteratorContainer::
operator typename Triangulation<dim, spacedim>::cell_iterator() const
{
  Assert(is_initialized(), ExcNotReinited());

  return cell;
}



template <int dim, int spacedim>
types::global_dof_index
FEValuesBase<dim, spacedim>::CellIteratorContainer::n_dofs_for_dof_handler()
  const
{
  Assert(is_initialized(), ExcNotReinited());
  Assert(dof_handler != nullptr, ExcNeedsDoFHandler());

  return dof_handler->n_dofs();
}



template <int dim, int spacedim>
template <typename VectorType>
void
FEValuesBase<dim, spacedim>::CellIteratorContainer::get_interpolated_dof_values(
  const VectorType &                       in,
  Vector<typename VectorType::value_type> &out) const
{
  Assert(is_initialized(), ExcNotReinited());
  Assert(dof_handler != nullptr, ExcNeedsDoFHandler());

  if (level_dof_access)
    DoFCellAccessor<dim, spacedim, true>(&cell->get_triangulation(),
                                         cell->level(),
                                         cell->index(),
                                         dof_handler)
      .get_interpolated_dof_values(in, out);
  else
    DoFCellAccessor<dim, spacedim, false>(&cell->get_triangulation(),
                                          cell->level(),
                                          cell->index(),
                                          dof_handler)
      .get_interpolated_dof_values(in, out);
}



template <int dim, int spacedim>
void
FEValuesBase<dim, spacedim>::CellIteratorContainer::get_interpolated_dof_values(
  const IndexSet &              in,
  Vector<IndexSet::value_type> &out) const
{
  Assert(is_initialized(), ExcNotReinited());
  Assert(dof_handler != nullptr, ExcNeedsDoFHandler());
  Assert(level_dof_access == false, ExcNotImplemented());

  const DoFCellAccessor<dim, spacedim, false> cell_dofs(
    &cell->get_triangulation(), cell->level(), cell->index(), dof_handler);

  std::vector<types::global_dof_index> dof_indices(
    cell_dofs.get_fe().n_dofs_per_cell());
  cell_dofs.get_dof_indices(dof_indices);

  for (unsigned int i = 0; i < cell_dofs.get_fe().n_dofs_per_cell(); ++i)
    out[i] = (in.is_element(dof_indices[i]) ? 1 : 0);
}



namespace internal
{
  namespace FEValuesImplementation
  {
    template <int dim, int spacedim>
    void
    FiniteElementRelatedData<dim, spacedim>::initialize(
      const unsigned int                  n_quadrature_points,
      const FiniteElement<dim, spacedim> &fe,
      const UpdateFlags                   flags)
    {
      // initialize the table mapping from shape function number to
      // the rows in the tables storing the data by shape function and
      // nonzero component
      this->shape_function_to_row_table =
        dealii::internal::make_shape_function_to_row_table(fe);

      // count the total number of non-zero components accumulated
      // over all shape functions
      unsigned int n_nonzero_shape_components = 0;
      for (unsigned int i = 0; i < fe.n_dofs_per_cell(); ++i)
        n_nonzero_shape_components += fe.n_nonzero_components(i);
      Assert(n_nonzero_shape_components >= fe.n_dofs_per_cell(),
             ExcInternalError());

      // with the number of rows now known, initialize those fields
      // that we will need to their correct size
      if (flags & update_values)
        {
          this->shape_values.reinit(n_nonzero_shape_components,
                                    n_quadrature_points);
          this->shape_values.fill(numbers::signaling_nan<double>());
        }

      if (flags & update_gradients)
        {
          this->shape_gradients.reinit(n_nonzero_shape_components,
                                       n_quadrature_points);
          this->shape_gradients.fill(
            numbers::signaling_nan<Tensor<1, spacedim>>());
        }

      if (flags & update_hessians)
        {
          this->shape_hessians.reinit(n_nonzero_shape_components,
                                      n_quadrature_points);
          this->shape_hessians.fill(
            numbers::signaling_nan<Tensor<2, spacedim>>());
        }

      if (flags & update_3rd_derivatives)
        {
          this->shape_3rd_derivatives.reinit(n_nonzero_shape_components,
                                             n_quadrature_points);
          this->shape_3rd_derivatives.fill(
            numbers::signaling_nan<Tensor<3, spacedim>>());
        }
    }



    template <int dim, int spacedim>
    std::size_t
    FiniteElementRelatedData<dim, spacedim>::memory_consumption() const
    {
      return (
        MemoryConsumption::memory_consumption(shape_values) +
        MemoryConsumption::memory_consumption(shape_gradients) +
        MemoryConsumption::memory_consumption(shape_hessians) +
        MemoryConsumption::memory_consumption(shape_3rd_derivatives) +
        MemoryConsumption::memory_consumption(shape_function_to_row_table));
    }
  } // namespace FEValuesImplementation
} // namespace internal



/*------------------------------- FEValuesBase ---------------------------*/


template <int dim, int spacedim>
FEValuesBase<dim, spacedim>::FEValuesBase(
  const unsigned int                  n_q_points,
  const unsigned int                  dofs_per_cell,
  const UpdateFlags                   flags,
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe)
  : n_quadrature_points(n_q_points)
  , max_n_quadrature_points(n_q_points)
  , dofs_per_cell(dofs_per_cell)
  , mapping(&mapping, typeid(*this).name())
  , fe(&fe, typeid(*this).name())
  , cell_similarity(CellSimilarity::Similarity::none)
  , fe_values_views_cache(*this)
{
  Assert(n_q_points > 0,
         ExcMessage("There is nothing useful you can do with an FEValues "
                    "object when using a quadrature formula with zero "
                    "quadrature points!"));
  this->update_flags = flags;
}



template <int dim, int spacedim>
FEValuesBase<dim, spacedim>::~FEValuesBase()
{
  tria_listener_refinement.disconnect();
  tria_listener_mesh_transform.disconnect();
}



namespace internal
{
  // put shape function part of get_function_xxx methods into separate
  // internal functions. this allows us to reuse the same code for several
  // functions (e.g. both the versions with and without indices) as well as
  // the same code for gradients and Hessians. Moreover, this speeds up
  // compilation and reduces the size of the final file since all the
  // different global vectors get channeled through the same code.

  template <typename Number, typename Number2>
  void
  do_function_values(const Number2 *                 dof_values_ptr,
                     const dealii::Table<2, double> &shape_values,
                     std::vector<Number> &           values)
  {
    // scalar finite elements, so shape_values.size() == dofs_per_cell
    const unsigned int dofs_per_cell       = shape_values.n_rows();
    const unsigned int n_quadrature_points = values.size();

    // initialize with zero
    std::fill_n(values.begin(),
                n_quadrature_points,
                dealii::internal::NumberType<Number>::value(0.0));

    // add up contributions of trial functions. note that here we deal with
    // scalar finite elements, so no need to check for non-primitivity of
    // shape functions. in order to increase the speed of this function, we
    // directly access the data in the shape_values array, and increment
    // pointers for accessing the data. this saves some lookup time and
    // indexing. moreover, the order of the loops is such that we can access
    // the shape_values data stored contiguously
    for (unsigned int shape_func = 0; shape_func < dofs_per_cell; ++shape_func)
      {
        const Number2 value = dof_values_ptr[shape_func];
        // For auto-differentiable numbers, the fact that a DoF value is zero
        // does not imply that its derivatives are zero as well. So we
        // can't filter by value for these number types.
        if (!Differentiation::AD::is_ad_number<Number2>::value)
          if (value == dealii::internal::NumberType<Number2>::value(0.0))
            continue;

        const double *shape_value_ptr = &shape_values(shape_func, 0);
        for (unsigned int point = 0; point < n_quadrature_points; ++point)
          values[point] += value * (*shape_value_ptr++);
      }
  }



  template <int dim, int spacedim, typename VectorType>
  void
  do_function_values(
    const typename VectorType::value_type *dof_values_ptr,
    const dealii::Table<2, double> &       shape_values,
    const FiniteElement<dim, spacedim> &   fe,
    const std::vector<unsigned int> &      shape_function_to_row_table,
    ArrayView<VectorType>                  values,
    const bool                             quadrature_points_fastest = false,
    const unsigned int                     component_multiple        = 1)
  {
    using Number = typename VectorType::value_type;
    // initialize with zero
    for (unsigned int i = 0; i < values.size(); ++i)
      std::fill_n(values[i].begin(),
                  values[i].size(),
                  typename VectorType::value_type());

    // see if there the current cell has DoFs at all, and if not
    // then there is nothing else to do.
    const unsigned int dofs_per_cell = fe.n_dofs_per_cell();
    if (dofs_per_cell == 0)
      return;

    const unsigned int n_quadrature_points =
      quadrature_points_fastest ? values[0].size() : values.size();
    const unsigned int n_components = fe.n_components();

    // Assert that we can write all components into the result vectors
    const unsigned result_components = n_components * component_multiple;
    (void)result_components;
    if (quadrature_points_fastest)
      {
        AssertDimension(values.size(), result_components);
        for (unsigned int i = 0; i < values.size(); ++i)
          AssertDimension(values[i].size(), n_quadrature_points);
      }
    else
      {
        AssertDimension(values.size(), n_quadrature_points);
        for (unsigned int i = 0; i < values.size(); ++i)
          AssertDimension(values[i].size(), result_components);
      }

    // add up contributions of trial functions.  now check whether the shape
    // function is primitive or not. if it is, then set its only non-zero
    // component, otherwise loop over components
    for (unsigned int mc = 0; mc < component_multiple; ++mc)
      for (unsigned int shape_func = 0; shape_func < dofs_per_cell;
           ++shape_func)
        {
          const Number &value = dof_values_ptr[shape_func + mc * dofs_per_cell];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (fe.is_primitive(shape_func))
            {
              const unsigned int comp =
                fe.system_to_component_index(shape_func).first +
                mc * n_components;
              const unsigned int row =
                shape_function_to_row_table[shape_func * n_components + comp];

              const double *shape_value_ptr = &shape_values(row, 0);

              if (quadrature_points_fastest)
                {
                  VectorType &values_comp = values[comp];
                  for (unsigned int point = 0; point < n_quadrature_points;
                       ++point)
                    values_comp[point] += value * (*shape_value_ptr++);
                }
              else
                for (unsigned int point = 0; point < n_quadrature_points;
                     ++point)
                  values[point][comp] += value * (*shape_value_ptr++);
            }
          else
            for (unsigned int c = 0; c < n_components; ++c)
              {
                if (fe.get_nonzero_components(shape_func)[c] == false)
                  continue;

                const unsigned int row =
                  shape_function_to_row_table[shape_func * n_components + c];

                const double *     shape_value_ptr = &shape_values(row, 0);
                const unsigned int comp            = c + mc * n_components;

                if (quadrature_points_fastest)
                  {
                    VectorType &values_comp = values[comp];
                    for (unsigned int point = 0; point < n_quadrature_points;
                         ++point)
                      values_comp[point] += value * (*shape_value_ptr++);
                  }
                else
                  for (unsigned int point = 0; point < n_quadrature_points;
                       ++point)
                    values[point][comp] += value * (*shape_value_ptr++);
              }
        }
  }



  // use the same implementation for gradients and Hessians, distinguish them
  // by the rank of the tensors
  template <int order, int spacedim, typename Number>
  void
  do_function_derivatives(
    const Number *                                   dof_values_ptr,
    const dealii::Table<2, Tensor<order, spacedim>> &shape_derivatives,
    std::vector<Tensor<order, spacedim, Number>> &   derivatives)
  {
    const unsigned int dofs_per_cell       = shape_derivatives.size()[0];
    const unsigned int n_quadrature_points = derivatives.size();

    // initialize with zero
    std::fill_n(derivatives.begin(),
                n_quadrature_points,
                Tensor<order, spacedim, Number>());

    // add up contributions of trial functions. note that here we deal with
    // scalar finite elements, so no need to check for non-primitivity of
    // shape functions. in order to increase the speed of this function, we
    // directly access the data in the shape_gradients/hessians array, and
    // increment pointers for accessing the data. this saves some lookup time
    // and indexing. moreover, the order of the loops is such that we can
    // access the shape_gradients/hessians data stored contiguously
    for (unsigned int shape_func = 0; shape_func < dofs_per_cell; ++shape_func)
      {
        const Number &value = dof_values_ptr[shape_func];
        // For auto-differentiable numbers, the fact that a DoF value is zero
        // does not imply that its derivatives are zero as well. So we
        // can't filter by value for these number types.
        if (dealii::internal::CheckForZero<Number>::value(value) == true)
          continue;

        const Tensor<order, spacedim> *shape_derivative_ptr =
          &shape_derivatives[shape_func][0];
        for (unsigned int point = 0; point < n_quadrature_points; ++point)
          derivatives[point] += value * (*shape_derivative_ptr++);
      }
  }



  template <int order, int dim, int spacedim, typename Number>
  void
  do_function_derivatives(
    const Number *                                   dof_values_ptr,
    const dealii::Table<2, Tensor<order, spacedim>> &shape_derivatives,
    const FiniteElement<dim, spacedim> &             fe,
    const std::vector<unsigned int> &shape_function_to_row_table,
    ArrayView<std::vector<Tensor<order, spacedim, Number>>> derivatives,
    const bool         quadrature_points_fastest = false,
    const unsigned int component_multiple        = 1)
  {
    // initialize with zero
    for (unsigned int i = 0; i < derivatives.size(); ++i)
      std::fill_n(derivatives[i].begin(),
                  derivatives[i].size(),
                  Tensor<order, spacedim, Number>());

    // see if there the current cell has DoFs at all, and if not
    // then there is nothing else to do.
    const unsigned int dofs_per_cell = fe.n_dofs_per_cell();
    if (dofs_per_cell == 0)
      return;


    const unsigned int n_quadrature_points =
      quadrature_points_fastest ? derivatives[0].size() : derivatives.size();
    const unsigned int n_components = fe.n_components();

    // Assert that we can write all components into the result vectors
    const unsigned result_components = n_components * component_multiple;
    (void)result_components;
    if (quadrature_points_fastest)
      {
        AssertDimension(derivatives.size(), result_components);
        for (unsigned int i = 0; i < derivatives.size(); ++i)
          AssertDimension(derivatives[i].size(), n_quadrature_points);
      }
    else
      {
        AssertDimension(derivatives.size(), n_quadrature_points);
        for (unsigned int i = 0; i < derivatives.size(); ++i)
          AssertDimension(derivatives[i].size(), result_components);
      }

    // add up contributions of trial functions.  now check whether the shape
    // function is primitive or not. if it is, then set its only non-zero
    // component, otherwise loop over components
    for (unsigned int mc = 0; mc < component_multiple; ++mc)
      for (unsigned int shape_func = 0; shape_func < dofs_per_cell;
           ++shape_func)
        {
          const Number &value = dof_values_ptr[shape_func + mc * dofs_per_cell];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (fe.is_primitive(shape_func))
            {
              const unsigned int comp =
                fe.system_to_component_index(shape_func).first +
                mc * n_components;
              const unsigned int row =
                shape_function_to_row_table[shape_func * n_components + comp];

              const Tensor<order, spacedim> *shape_derivative_ptr =
                &shape_derivatives[row][0];

              if (quadrature_points_fastest)
                for (unsigned int point = 0; point < n_quadrature_points;
                     ++point)
                  derivatives[comp][point] += value * (*shape_derivative_ptr++);
              else
                for (unsigned int point = 0; point < n_quadrature_points;
                     ++point)
                  derivatives[point][comp] += value * (*shape_derivative_ptr++);
            }
          else
            for (unsigned int c = 0; c < n_components; ++c)
              {
                if (fe.get_nonzero_components(shape_func)[c] == false)
                  continue;

                const unsigned int row =
                  shape_function_to_row_table[shape_func * n_components + c];

                const Tensor<order, spacedim> *shape_derivative_ptr =
                  &shape_derivatives[row][0];
                const unsigned int comp = c + mc * n_components;

                if (quadrature_points_fastest)
                  for (unsigned int point = 0; point < n_quadrature_points;
                       ++point)
                    derivatives[comp][point] +=
                      value * (*shape_derivative_ptr++);
                else
                  for (unsigned int point = 0; point < n_quadrature_points;
                       ++point)
                    derivatives[point][comp] +=
                      value * (*shape_derivative_ptr++);
              }
        }
  }



  template <int spacedim, typename Number, typename Number2>
  void
  do_function_laplacians(
    const Number2 *                              dof_values_ptr,
    const dealii::Table<2, Tensor<2, spacedim>> &shape_hessians,
    std::vector<Number> &                        laplacians)
  {
    const unsigned int dofs_per_cell       = shape_hessians.size()[0];
    const unsigned int n_quadrature_points = laplacians.size();

    // initialize with zero
    std::fill_n(laplacians.begin(),
                n_quadrature_points,
                dealii::internal::NumberType<Number>::value(0.0));

    // add up contributions of trial functions. note that here we deal with
    // scalar finite elements and also note that the Laplacian is
    // the trace of the Hessian.
    for (unsigned int shape_func = 0; shape_func < dofs_per_cell; ++shape_func)
      {
        const Number2 value = dof_values_ptr[shape_func];
        // For auto-differentiable numbers, the fact that a DoF value is zero
        // does not imply that its derivatives are zero as well. So we
        // can't filter by value for these number types.
        if (!Differentiation::AD::is_ad_number<Number2>::value)
          if (value == dealii::internal::NumberType<Number2>::value(0.0))
            continue;

        const Tensor<2, spacedim> *shape_hessian_ptr =
          &shape_hessians[shape_func][0];
        for (unsigned int point = 0; point < n_quadrature_points; ++point)
          laplacians[point] += value * trace(*shape_hessian_ptr++);
      }
  }



  template <int dim, int spacedim, typename VectorType, typename Number>
  void
  do_function_laplacians(
    const Number *                               dof_values_ptr,
    const dealii::Table<2, Tensor<2, spacedim>> &shape_hessians,
    const FiniteElement<dim, spacedim> &         fe,
    const std::vector<unsigned int> &            shape_function_to_row_table,
    std::vector<VectorType> &                    laplacians,
    const bool         quadrature_points_fastest = false,
    const unsigned int component_multiple        = 1)
  {
    // initialize with zero
    for (unsigned int i = 0; i < laplacians.size(); ++i)
      std::fill_n(laplacians[i].begin(),
                  laplacians[i].size(),
                  typename VectorType::value_type());

    // see if there the current cell has DoFs at all, and if not
    // then there is nothing else to do.
    const unsigned int dofs_per_cell = fe.n_dofs_per_cell();
    if (dofs_per_cell == 0)
      return;


    const unsigned int n_quadrature_points = laplacians.size();
    const unsigned int n_components        = fe.n_components();

    // Assert that we can write all components into the result vectors
    const unsigned result_components = n_components * component_multiple;
    (void)result_components;
    if (quadrature_points_fastest)
      {
        AssertDimension(laplacians.size(), result_components);
        for (unsigned int i = 0; i < laplacians.size(); ++i)
          AssertDimension(laplacians[i].size(), n_quadrature_points);
      }
    else
      {
        AssertDimension(laplacians.size(), n_quadrature_points);
        for (unsigned int i = 0; i < laplacians.size(); ++i)
          AssertDimension(laplacians[i].size(), result_components);
      }

    // add up contributions of trial functions.  now check whether the shape
    // function is primitive or not. if it is, then set its only non-zero
    // component, otherwise loop over components
    for (unsigned int mc = 0; mc < component_multiple; ++mc)
      for (unsigned int shape_func = 0; shape_func < dofs_per_cell;
           ++shape_func)
        {
          const Number &value = dof_values_ptr[shape_func + mc * dofs_per_cell];
          // For auto-differentiable numbers, the fact that a DoF value is zero
          // does not imply that its derivatives are zero as well. So we
          // can't filter by value for these number types.
          if (dealii::internal::CheckForZero<Number>::value(value) == true)
            continue;

          if (fe.is_primitive(shape_func))
            {
              const unsigned int comp =
                fe.system_to_component_index(shape_func).first +
                mc * n_components;
              const unsigned int row =
                shape_function_to_row_table[shape_func * n_components + comp];

              const Tensor<2, spacedim> *shape_hessian_ptr =
                &shape_hessians[row][0];
              if (quadrature_points_fastest)
                {
                  VectorType &laplacians_comp = laplacians[comp];
                  for (unsigned int point = 0; point < n_quadrature_points;
                       ++point)
                    laplacians_comp[point] +=
                      value * trace(*shape_hessian_ptr++);
                }
              else
                for (unsigned int point = 0; point < n_quadrature_points;
                     ++point)
                  laplacians[point][comp] +=
                    value * trace(*shape_hessian_ptr++);
            }
          else
            for (unsigned int c = 0; c < n_components; ++c)
              {
                if (fe.get_nonzero_components(shape_func)[c] == false)
                  continue;

                const unsigned int row =
                  shape_function_to_row_table[shape_func * n_components + c];

                const Tensor<2, spacedim> *shape_hessian_ptr =
                  &shape_hessians[row][0];
                const unsigned int comp = c + mc * n_components;

                if (quadrature_points_fastest)
                  {
                    VectorType &laplacians_comp = laplacians[comp];
                    for (unsigned int point = 0; point < n_quadrature_points;
                         ++point)
                      laplacians_comp[point] +=
                        value * trace(*shape_hessian_ptr++);
                  }
                else
                  for (unsigned int point = 0; point < n_quadrature_points;
                       ++point)
                    laplacians[point][comp] +=
                      value * trace(*shape_hessian_ptr++);
              }
        }
  }
} // namespace internal



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_values(
  const InputVector &                            fe_function,
  std::vector<typename InputVector::value_type> &values) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_values,
         ExcAccessToUninitializedField("update_values"));
  AssertDimension(fe->n_components(), 1);
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_values(dof_values.begin(),
                               this->finite_element_output.shape_values,
                               values);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_values(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  std::vector<typename InputVector::value_type> & values) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_values,
         ExcAccessToUninitializedField("update_values"));
  AssertDimension(fe->n_components(), 1);
  AssertDimension(indices.size(), dofs_per_cell);

  boost::container::small_vector<Number, 200> dof_values(dofs_per_cell);
  for (unsigned int i = 0; i < dofs_per_cell; ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_values(dof_values.data(),
                               this->finite_element_output.shape_values,
                               values);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_values(
  const InputVector &                                    fe_function,
  std::vector<Vector<typename InputVector::value_type>> &values) const
{
  using Number = typename InputVector::value_type;
  Assert(present_cell.is_initialized(), ExcNotReinited());

  Assert(this->update_flags & update_values,
         ExcAccessToUninitializedField("update_values"));
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_values(
    dof_values.begin(),
    this->finite_element_output.shape_values,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(values.begin(), values.end()));
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_values(
  const InputVector &                                    fe_function,
  const ArrayView<const types::global_dof_index> &       indices,
  std::vector<Vector<typename InputVector::value_type>> &values) const
{
  using Number = typename InputVector::value_type;
  // Size of indices must be a multiple of dofs_per_cell such that an integer
  // number of function values is generated in each point.
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));
  Assert(this->update_flags & update_values,
         ExcAccessToUninitializedField("update_values"));

  boost::container::small_vector<Number, 200> dof_values(dofs_per_cell);
  for (unsigned int i = 0; i < dofs_per_cell; ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_values(
    dof_values.data(),
    this->finite_element_output.shape_values,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(values.begin(), values.end()),
    false,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_values(
  const InputVector &                                      fe_function,
  const ArrayView<const types::global_dof_index> &         indices,
  ArrayView<std::vector<typename InputVector::value_type>> values,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_values,
         ExcAccessToUninitializedField("update_values"));

  // Size of indices must be a multiple of dofs_per_cell such that an integer
  // number of function values is generated in each point.
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));

  boost::container::small_vector<Number, 200> dof_values(indices.size());
  for (unsigned int i = 0; i < indices.size(); ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_values(
    dof_values.data(),
    this->finite_element_output.shape_values,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(values.begin(), values.end()),
    quadrature_points_fastest,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_gradients(
  const InputVector &fe_function,
  std::vector<Tensor<1, spacedim, typename InputVector::value_type>> &gradients)
  const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_gradients,
         ExcAccessToUninitializedField("update_gradients"));
  AssertDimension(fe->n_components(), 1);
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_derivatives(dof_values.begin(),
                                    this->finite_element_output.shape_gradients,
                                    gradients);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_gradients(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  std::vector<Tensor<1, spacedim, typename InputVector::value_type>> &gradients)
  const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_gradients,
         ExcAccessToUninitializedField("update_gradients"));
  AssertDimension(fe->n_components(), 1);
  AssertDimension(indices.size(), dofs_per_cell);

  boost::container::small_vector<Number, 200> dof_values(dofs_per_cell);
  for (unsigned int i = 0; i < dofs_per_cell; ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_derivatives(dof_values.data(),
                                    this->finite_element_output.shape_gradients,
                                    gradients);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_gradients(
  const InputVector &fe_function,
  std::vector<
    std::vector<Tensor<1, spacedim, typename InputVector::value_type>>>
    &gradients) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_gradients,
         ExcAccessToUninitializedField("update_gradients"));
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_derivatives(
    dof_values.begin(),
    this->finite_element_output.shape_gradients,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(gradients.begin(), gradients.end()));
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_gradients(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  ArrayView<std::vector<Tensor<1, spacedim, typename InputVector::value_type>>>
             gradients,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  // Size of indices must be a multiple of dofs_per_cell such that an integer
  // number of function values is generated in each point.
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));
  Assert(this->update_flags & update_gradients,
         ExcAccessToUninitializedField("update_gradients"));

  boost::container::small_vector<Number, 200> dof_values(indices.size());
  for (unsigned int i = 0; i < indices.size(); ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_derivatives(
    dof_values.data(),
    this->finite_element_output.shape_gradients,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(gradients.begin(), gradients.end()),
    quadrature_points_fastest,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_hessians(
  const InputVector &fe_function,
  std::vector<Tensor<2, spacedim, typename InputVector::value_type>> &hessians)
  const
{
  using Number = typename InputVector::value_type;
  AssertDimension(fe->n_components(), 1);
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_derivatives(dof_values.begin(),
                                    this->finite_element_output.shape_hessians,
                                    hessians);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_hessians(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  std::vector<Tensor<2, spacedim, typename InputVector::value_type>> &hessians)
  const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());
  AssertDimension(indices.size(), dofs_per_cell);

  boost::container::small_vector<Number, 200> dof_values(dofs_per_cell);
  for (unsigned int i = 0; i < dofs_per_cell; ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_derivatives(dof_values.data(),
                                    this->finite_element_output.shape_hessians,
                                    hessians);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_hessians(
  const InputVector &fe_function,
  std::vector<
    std::vector<Tensor<2, spacedim, typename InputVector::value_type>>>
    &        hessians,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_derivatives(
    dof_values.begin(),
    this->finite_element_output.shape_hessians,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(hessians.begin(), hessians.end()),
    quadrature_points_fastest);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_hessians(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  ArrayView<std::vector<Tensor<2, spacedim, typename InputVector::value_type>>>
             hessians,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));

  boost::container::small_vector<Number, 200> dof_values(indices.size());
  for (unsigned int i = 0; i < indices.size(); ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_derivatives(
    dof_values.data(),
    this->finite_element_output.shape_hessians,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(hessians.begin(), hessians.end()),
    quadrature_points_fastest,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_laplacians(
  const InputVector &                            fe_function,
  std::vector<typename InputVector::value_type> &laplacians) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  AssertDimension(fe->n_components(), 1);
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_laplacians(dof_values.begin(),
                                   this->finite_element_output.shape_hessians,
                                   laplacians);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_laplacians(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  std::vector<typename InputVector::value_type> & laplacians) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  AssertDimension(fe->n_components(), 1);
  AssertDimension(indices.size(), dofs_per_cell);

  boost::container::small_vector<Number, 200> dof_values(dofs_per_cell);
  for (unsigned int i = 0; i < dofs_per_cell; ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_laplacians(dof_values.data(),
                                   this->finite_element_output.shape_hessians,
                                   laplacians);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_laplacians(
  const InputVector &                                    fe_function,
  std::vector<Vector<typename InputVector::value_type>> &laplacians) const
{
  using Number = typename InputVector::value_type;
  Assert(present_cell.is_initialized(), ExcNotReinited());
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_laplacians(
    dof_values.begin(),
    this->finite_element_output.shape_hessians,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    laplacians);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_laplacians(
  const InputVector &                                    fe_function,
  const ArrayView<const types::global_dof_index> &       indices,
  std::vector<Vector<typename InputVector::value_type>> &laplacians) const
{
  using Number = typename InputVector::value_type;
  // Size of indices must be a multiple of dofs_per_cell such that an integer
  // number of function values is generated in each point.
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));

  boost::container::small_vector<Number, 200> dof_values(indices.size());
  for (unsigned int i = 0; i < indices.size(); ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_laplacians(
    dof_values.data(),
    this->finite_element_output.shape_hessians,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    laplacians,
    false,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_laplacians(
  const InputVector &                                         fe_function,
  const ArrayView<const types::global_dof_index> &            indices,
  std::vector<std::vector<typename InputVector::value_type>> &laplacians,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));
  Assert(this->update_flags & update_hessians,
         ExcAccessToUninitializedField("update_hessians"));

  boost::container::small_vector<Number, 200> dof_values(indices.size());
  for (unsigned int i = 0; i < indices.size(); ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_laplacians(
    dof_values.data(),
    this->finite_element_output.shape_hessians,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    laplacians,
    quadrature_points_fastest,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_third_derivatives(
  const InputVector &fe_function,
  std::vector<Tensor<3, spacedim, typename InputVector::value_type>>
    &third_derivatives) const
{
  using Number = typename InputVector::value_type;
  AssertDimension(fe->n_components(), 1);
  Assert(this->update_flags & update_3rd_derivatives,
         ExcAccessToUninitializedField("update_3rd_derivatives"));
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_derivatives(
    dof_values.begin(),
    this->finite_element_output.shape_3rd_derivatives,
    third_derivatives);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_third_derivatives(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  std::vector<Tensor<3, spacedim, typename InputVector::value_type>>
    &third_derivatives) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_3rd_derivatives,
         ExcAccessToUninitializedField("update_3rd_derivatives"));
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());
  AssertDimension(indices.size(), dofs_per_cell);

  boost::container::small_vector<Number, 200> dof_values(dofs_per_cell);
  for (unsigned int i = 0; i < dofs_per_cell; ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_derivatives(
    dof_values.data(),
    this->finite_element_output.shape_3rd_derivatives,
    third_derivatives);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_third_derivatives(
  const InputVector &fe_function,
  std::vector<
    std::vector<Tensor<3, spacedim, typename InputVector::value_type>>>
    &        third_derivatives,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_3rd_derivatives,
         ExcAccessToUninitializedField("update_3rd_derivatives"));
  Assert(present_cell.is_initialized(), ExcNotReinited());
  AssertDimension(fe_function.size(), present_cell.n_dofs_for_dof_handler());

  // get function values of dofs on this cell
  Vector<Number> dof_values(dofs_per_cell);
  present_cell.get_interpolated_dof_values(fe_function, dof_values);
  internal::do_function_derivatives(
    dof_values.begin(),
    this->finite_element_output.shape_3rd_derivatives,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(third_derivatives.begin(), third_derivatives.end()),
    quadrature_points_fastest);
}



template <int dim, int spacedim>
template <class InputVector>
void
FEValuesBase<dim, spacedim>::get_function_third_derivatives(
  const InputVector &                             fe_function,
  const ArrayView<const types::global_dof_index> &indices,
  ArrayView<std::vector<Tensor<3, spacedim, typename InputVector::value_type>>>
             third_derivatives,
  const bool quadrature_points_fastest) const
{
  using Number = typename InputVector::value_type;
  Assert(this->update_flags & update_3rd_derivatives,
         ExcAccessToUninitializedField("update_3rd_derivatives"));
  Assert(indices.size() % dofs_per_cell == 0,
         ExcNotMultiple(indices.size(), dofs_per_cell));

  boost::container::small_vector<Number, 200> dof_values(indices.size());
  for (unsigned int i = 0; i < indices.size(); ++i)
    dof_values[i] = internal::get_vector_element(fe_function, indices[i]);
  internal::do_function_derivatives(
    dof_values.data(),
    this->finite_element_output.shape_3rd_derivatives,
    *fe,
    this->finite_element_output.shape_function_to_row_table,
    make_array_view(third_derivatives.begin(), third_derivatives.end()),
    quadrature_points_fastest,
    indices.size() / dofs_per_cell);
}



template <int dim, int spacedim>
const typename Triangulation<dim, spacedim>::cell_iterator
FEValuesBase<dim, spacedim>::get_cell() const
{
  return present_cell;
}



template <int dim, int spacedim>
const std::vector<Tensor<1, spacedim>> &
FEValuesBase<dim, spacedim>::get_normal_vectors() const
{
  Assert(this->update_flags & update_normal_vectors,
         (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
           "update_normal_vectors")));

  return this->mapping_output.normal_vectors;
}



template <int dim, int spacedim>
std::size_t
FEValuesBase<dim, spacedim>::memory_consumption() const
{
  return (sizeof(this->update_flags) +
          MemoryConsumption::memory_consumption(n_quadrature_points) +
          MemoryConsumption::memory_consumption(max_n_quadrature_points) +
          sizeof(cell_similarity) +
          MemoryConsumption::memory_consumption(dofs_per_cell) +
          MemoryConsumption::memory_consumption(mapping) +
          MemoryConsumption::memory_consumption(mapping_data) +
          MemoryConsumption::memory_consumption(*mapping_data) +
          MemoryConsumption::memory_consumption(mapping_output) +
          MemoryConsumption::memory_consumption(fe) +
          MemoryConsumption::memory_consumption(fe_data) +
          MemoryConsumption::memory_consumption(*fe_data) +
          MemoryConsumption::memory_consumption(finite_element_output));
}



template <int dim, int spacedim>
UpdateFlags
FEValuesBase<dim, spacedim>::compute_update_flags(
  const UpdateFlags update_flags) const
{
  // first find out which objects need to be recomputed on each
  // cell we visit. this we have to ask the finite element and mapping.
  // elements are first since they might require update in mapping
  //
  // there is no need to iterate since mappings will never require
  // the finite element to compute something for them
  UpdateFlags flags = update_flags | fe->requires_update_flags(update_flags);
  flags |= mapping->requires_update_flags(flags);

  return flags;
}



template <int dim, int spacedim>
void
FEValuesBase<dim, spacedim>::invalidate_present_cell()
{
  // if there is no present cell, then we shouldn't be
  // connected via a signal to a triangulation
  Assert(present_cell.is_initialized(), ExcInternalError());

  // so delete the present cell and
  // disconnect from the signal we have with
  // it
  tria_listener_refinement.disconnect();
  tria_listener_mesh_transform.disconnect();
  present_cell = {};
}



template <int dim, int spacedim>
void
FEValuesBase<dim, spacedim>::maybe_invalidate_previous_present_cell(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell)
{
  if (present_cell.is_initialized())
    {
      if (&cell->get_triangulation() !=
          &present_cell
             .
             operator typename Triangulation<dim, spacedim>::cell_iterator()
             ->get_triangulation())
        {
          // the triangulations for the previous cell and the current cell
          // do not match. disconnect from the previous triangulation and
          // connect to the current one; also invalidate the previous
          // cell because we shouldn't be comparing cells from different
          // triangulations
          invalidate_present_cell();
          tria_listener_refinement =
            cell->get_triangulation().signals.any_change.connect(
              [this]() { this->invalidate_present_cell(); });
          tria_listener_mesh_transform =
            cell->get_triangulation().signals.mesh_movement.connect(
              [this]() { this->invalidate_present_cell(); });
        }
    }
  else
    {
      // if this FEValues has never been set to any cell at all, then
      // at least subscribe to the triangulation to get notified of
      // changes
      tria_listener_refinement =
        cell->get_triangulation().signals.post_refinement.connect(
          [this]() { this->invalidate_present_cell(); });
      tria_listener_mesh_transform =
        cell->get_triangulation().signals.mesh_movement.connect(
          [this]() { this->invalidate_present_cell(); });
    }
}



template <int dim, int spacedim>
inline void
FEValuesBase<dim, spacedim>::check_cell_similarity(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell)
{
  // Unfortunately, the detection of simple geometries with CellSimilarity is
  // sensitive to the first cell detected. When doing this with multiple
  // threads, each thread will get its own scratch data object with an
  // FEValues object in the implementation framework from late 2013, which is
  // initialized to the first cell the thread sees. As this number might
  // different between different runs (after all, the tasks are scheduled
  // dynamically onto threads), this slight deviation leads to difference in
  // roundoff errors that propagate through the program. Therefore, we need to
  // disable CellSimilarity in case there is more than one thread in the
  // problem. This will likely not affect many MPI test cases as there
  // multithreading is disabled on default, but in many other situations
  // because we rarely explicitly set the number of threads.
  //
  // TODO: Is it reasonable to introduce a flag "unsafe" in the constructor of
  // FEValues to re-enable this feature?
  if (MultithreadInfo::n_threads() > 1)
    {
      cell_similarity = CellSimilarity::none;
      return;
    }

  // case that there has not been any cell before
  if (this->present_cell.is_initialized() == false)
    cell_similarity = CellSimilarity::none;
  else
    // in MappingQ, data can have been modified during the last call. Then, we
    // can't use that data on the new cell.
    if (cell_similarity == CellSimilarity::invalid_next_cell)
    cell_similarity = CellSimilarity::none;
  else
    cell_similarity =
      (cell->is_translation_of(
         static_cast<const typename Triangulation<dim, spacedim>::cell_iterator
                       &>(this->present_cell)) ?
         CellSimilarity::translation :
         CellSimilarity::none);

  if ((dim < spacedim) && (cell_similarity == CellSimilarity::translation))
    {
      if (static_cast<const typename Triangulation<dim, spacedim>::cell_iterator
                        &>(this->present_cell)
            ->direction_flag() != cell->direction_flag())
        cell_similarity = CellSimilarity::inverted_translation;
    }
  // TODO: here, one could implement other checks for similarity, e.g. for
  // children of a parallelogram.
}



template <int dim, int spacedim>
CellSimilarity::Similarity
FEValuesBase<dim, spacedim>::get_cell_similarity() const
{
  return cell_similarity;
}



template <int dim, int spacedim>
const unsigned int FEValuesBase<dim, spacedim>::dimension;



template <int dim, int spacedim>
const unsigned int FEValuesBase<dim, spacedim>::space_dimension;

/*------------------------------- FEValues -------------------------------*/

template <int dim, int spacedim>
const unsigned int FEValues<dim, spacedim>::integral_dimension;



template <int dim, int spacedim>
FEValues<dim, spacedim>::FEValues(const Mapping<dim, spacedim> &      mapping,
                                  const FiniteElement<dim, spacedim> &fe,
                                  const Quadrature<dim> &             q,
                                  const UpdateFlags update_flags)
  : FEValuesBase<dim, spacedim>(q.size(),
                                fe.n_dofs_per_cell(),
                                update_default,
                                mapping,
                                fe)
  , quadrature(q)
{
  initialize(update_flags);
}



template <int dim, int spacedim>
FEValues<dim, spacedim>::FEValues(const Mapping<dim, spacedim> &      mapping,
                                  const FiniteElement<dim, spacedim> &fe,
                                  const hp::QCollection<dim> &        q,
                                  const UpdateFlags update_flags)
  : FEValues(mapping, fe, q[0], update_flags)
{
  AssertDimension(q.size(), 1);
}



template <int dim, int spacedim>
FEValues<dim, spacedim>::FEValues(const FiniteElement<dim, spacedim> &fe,
                                  const Quadrature<dim> &             q,
                                  const UpdateFlags update_flags)
  : FEValuesBase<dim, spacedim>(
      q.size(),
      fe.n_dofs_per_cell(),
      update_default,
      fe.reference_cell().template get_default_linear_mapping<dim, spacedim>(),
      fe)
  , quadrature(q)
{
  initialize(update_flags);
}



template <int dim, int spacedim>
FEValues<dim, spacedim>::FEValues(const FiniteElement<dim, spacedim> &fe,
                                  const hp::QCollection<dim> &        q,
                                  const UpdateFlags update_flags)
  : FEValues(fe, q[0], update_flags)
{
  AssertDimension(q.size(), 1);
}



template <int dim, int spacedim>
void
FEValues<dim, spacedim>::initialize(const UpdateFlags update_flags)
{
  // You can compute normal vectors to the cells only in the
  // codimension one case.
  if (dim != spacedim - 1)
    Assert((update_flags & update_normal_vectors) == false,
           ExcMessage("You can only pass the 'update_normal_vectors' "
                      "flag to FEFaceValues or FESubfaceValues objects, "
                      "but not to an FEValues object unless the "
                      "triangulation it refers to is embedded in a higher "
                      "dimensional space."));

  const UpdateFlags flags = this->compute_update_flags(update_flags);

  // initialize the base classes
  if (flags & update_mapping)
    this->mapping_output.initialize(this->max_n_quadrature_points, flags);
  this->finite_element_output.initialize(this->max_n_quadrature_points,
                                         *this->fe,
                                         flags);

  // then get objects into which the FE and the Mapping can store
  // intermediate data used across calls to reinit. we can do this in parallel
  Threads::Task<
    std::unique_ptr<typename FiniteElement<dim, spacedim>::InternalDataBase>>
    fe_get_data = Threads::new_task([&]() {
      return this->fe->get_data(flags,
                                *this->mapping,
                                quadrature,
                                this->finite_element_output);
    });

  Threads::Task<
    std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>>
    mapping_get_data;
  if (flags & update_mapping)
    mapping_get_data = Threads::new_task(
      [&]() { return this->mapping->get_data(flags, quadrature); });

  this->update_flags = flags;

  // then collect answers from the two task above
  this->fe_data = std::move(fe_get_data.return_value());
  if (flags & update_mapping)
    this->mapping_data = std::move(mapping_get_data.return_value());
  else
    this->mapping_data =
      std::make_unique<typename Mapping<dim, spacedim>::InternalDataBase>();
}



template <int dim, int spacedim>
void
FEValues<dim, spacedim>::reinit(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell)
{
  // Check that mapping and reference cell type are compatible:
  Assert(this->get_mapping().is_compatible_with(cell->reference_cell()),
         ExcMessage(
           "You are trying to call FEValues::reinit() with a cell of type " +
           cell->reference_cell().to_string() +
           " with a Mapping that is not compatible with it."));

  // no FE in this cell, so no assertion
  // necessary here
  this->maybe_invalidate_previous_present_cell(cell);
  this->check_cell_similarity(cell);

  this->present_cell = {cell};

  // this was the part of the work that is dependent on the actual
  // data type of the iterator. now pass on to the function doing
  // the real work.
  do_reinit();
}



template <int dim, int spacedim>
template <bool lda>
void
FEValues<dim, spacedim>::reinit(
  const TriaIterator<DoFCellAccessor<dim, spacedim, lda>> &cell)
{
  // assert that the finite elements passed to the constructor and
  // used by the DoFHandler used by this cell, are the same
  Assert(static_cast<const FiniteElementData<dim> &>(*this->fe) ==
           static_cast<const FiniteElementData<dim> &>(cell->get_fe()),
         (typename FEValuesBase<dim, spacedim>::ExcFEDontMatch()));

  // Check that mapping and reference cell type are compatible:
  Assert(this->get_mapping().is_compatible_with(cell->reference_cell()),
         ExcMessage(
           "You are trying to call FEValues::reinit() with a cell of type " +
           cell->reference_cell().to_string() +
           " with a Mapping that is not compatible with it."));

  this->maybe_invalidate_previous_present_cell(cell);
  this->check_cell_similarity(cell);

  this->present_cell = {cell};

  // this was the part of the work that is dependent on the actual
  // data type of the iterator. now pass on to the function doing
  // the real work.
  do_reinit();
}



template <int dim, int spacedim>
void
FEValues<dim, spacedim>::do_reinit()
{
  // first call the mapping and let it generate the data
  // specific to the mapping. also let it inspect the
  // cell similarity flag and, if necessary, update
  // it
  if (this->update_flags & update_mapping)
    {
      this->cell_similarity =
        this->get_mapping().fill_fe_values(this->present_cell,
                                           this->cell_similarity,
                                           quadrature,
                                           *this->mapping_data,
                                           this->mapping_output);
    }

  // then call the finite element and, with the data
  // already filled by the mapping, let it compute the
  // data for the mapped shape function values, gradients,
  // etc.
  this->get_fe().fill_fe_values(this->present_cell,
                                this->cell_similarity,
                                this->quadrature,
                                this->get_mapping(),
                                *this->mapping_data,
                                this->mapping_output,
                                *this->fe_data,
                                this->finite_element_output);
}



template <int dim, int spacedim>
std::size_t
FEValues<dim, spacedim>::memory_consumption() const
{
  return (FEValuesBase<dim, spacedim>::memory_consumption() +
          MemoryConsumption::memory_consumption(quadrature));
}


/*------------------------------- FEFaceValuesBase --------------------------*/


template <int dim, int spacedim>
FEFaceValuesBase<dim, spacedim>::FEFaceValuesBase(
  const unsigned int                  dofs_per_cell,
  const UpdateFlags                   flags,
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe,
  const Quadrature<dim - 1> &         quadrature)
  : FEFaceValuesBase<dim, spacedim>(dofs_per_cell,
                                    flags,
                                    mapping,
                                    fe,
                                    hp::QCollection<dim - 1>(quadrature))
{}



template <int dim, int spacedim>
FEFaceValuesBase<dim, spacedim>::FEFaceValuesBase(
  const unsigned int dofs_per_cell,
  const UpdateFlags,
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe,
  const hp::QCollection<dim - 1> &    quadrature)
  : FEValuesBase<dim, spacedim>(quadrature.max_n_quadrature_points(),
                                dofs_per_cell,
                                update_default,
                                mapping,
                                fe)
  , present_face_index(numbers::invalid_unsigned_int)
  , quadrature(quadrature)
{
  Assert(quadrature.size() == 1 ||
           quadrature.size() == fe.reference_cell().n_faces(),
         ExcInternalError());
}



template <int dim, int spacedim>
const std::vector<Tensor<1, spacedim>> &
FEFaceValuesBase<dim, spacedim>::get_boundary_forms() const
{
  Assert(this->update_flags & update_boundary_forms,
         (typename FEValuesBase<dim, spacedim>::ExcAccessToUninitializedField(
           "update_boundary_forms")));
  return this->mapping_output.boundary_forms;
}



template <int dim, int spacedim>
std::size_t
FEFaceValuesBase<dim, spacedim>::memory_consumption() const
{
  return (FEValuesBase<dim, spacedim>::memory_consumption() +
          MemoryConsumption::memory_consumption(quadrature));
}


/*------------------------------- FEFaceValues -------------------------------*/

template <int dim, int spacedim>
const unsigned int FEFaceValues<dim, spacedim>::dimension;



template <int dim, int spacedim>
const unsigned int FEFaceValues<dim, spacedim>::integral_dimension;



template <int dim, int spacedim>
FEFaceValues<dim, spacedim>::FEFaceValues(
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe,
  const Quadrature<dim - 1> &         quadrature,
  const UpdateFlags                   update_flags)
  : FEFaceValues<dim, spacedim>(mapping,
                                fe,
                                hp::QCollection<dim - 1>(quadrature),
                                update_flags)
{}



template <int dim, int spacedim>
FEFaceValues<dim, spacedim>::FEFaceValues(
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe,
  const hp::QCollection<dim - 1> &    quadrature,
  const UpdateFlags                   update_flags)
  : FEFaceValuesBase<dim, spacedim>(fe.n_dofs_per_cell(),
                                    update_flags,
                                    mapping,
                                    fe,
                                    quadrature)
{
  initialize(update_flags);
}



template <int dim, int spacedim>
FEFaceValues<dim, spacedim>::FEFaceValues(
  const FiniteElement<dim, spacedim> &fe,
  const Quadrature<dim - 1> &         quadrature,
  const UpdateFlags                   update_flags)
  : FEFaceValues<dim, spacedim>(fe,
                                hp::QCollection<dim - 1>(quadrature),
                                update_flags)
{}



template <int dim, int spacedim>
FEFaceValues<dim, spacedim>::FEFaceValues(
  const FiniteElement<dim, spacedim> &fe,
  const hp::QCollection<dim - 1> &    quadrature,
  const UpdateFlags                   update_flags)
  : FEFaceValuesBase<dim, spacedim>(
      fe.n_dofs_per_cell(),
      update_flags,
      fe.reference_cell().template get_default_linear_mapping<dim, spacedim>(),
      fe,
      quadrature)
{
  initialize(update_flags);
}



template <int dim, int spacedim>
void
FEFaceValues<dim, spacedim>::initialize(const UpdateFlags update_flags)
{
  const UpdateFlags flags = this->compute_update_flags(update_flags);

  // initialize the base classes
  if (flags & update_mapping)
    this->mapping_output.initialize(this->max_n_quadrature_points, flags);
  this->finite_element_output.initialize(this->max_n_quadrature_points,
                                         *this->fe,
                                         flags);

  // then get objects into which the FE and the Mapping can store
  // intermediate data used across calls to reinit. this can be done in parallel

  std::unique_ptr<typename FiniteElement<dim, spacedim>::InternalDataBase> (
    FiniteElement<dim, spacedim>::*finite_element_get_face_data)(
    const UpdateFlags,
    const Mapping<dim, spacedim> &,
    const hp::QCollection<dim - 1> &,
    dealii::internal::FEValuesImplementation::FiniteElementRelatedData<dim,
                                                                       spacedim>
      &) const = &FiniteElement<dim, spacedim>::get_face_data;

  std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase> (
    Mapping<dim, spacedim>::*mapping_get_face_data)(
    const UpdateFlags, const hp::QCollection<dim - 1> &) const =
    &Mapping<dim, spacedim>::get_face_data;


  Threads::Task<
    std::unique_ptr<typename FiniteElement<dim, spacedim>::InternalDataBase>>
    fe_get_data = Threads::new_task(finite_element_get_face_data,
                                    *this->fe,
                                    flags,
                                    *this->mapping,
                                    this->quadrature,
                                    this->finite_element_output);
  Threads::Task<
    std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>>
    mapping_get_data;
  if (flags & update_mapping)
    mapping_get_data = Threads::new_task(mapping_get_face_data,
                                         *this->mapping,
                                         flags,
                                         this->quadrature);

  this->update_flags = flags;

  // then collect answers from the two task above
  this->fe_data = std::move(fe_get_data.return_value());
  if (flags & update_mapping)
    this->mapping_data = std::move(mapping_get_data.return_value());
  else
    this->mapping_data =
      std::make_unique<typename Mapping<dim, spacedim>::InternalDataBase>();
}



template <int dim, int spacedim>
template <bool lda>
void
FEFaceValues<dim, spacedim>::reinit(
  const TriaIterator<DoFCellAccessor<dim, spacedim, lda>> &cell,
  const unsigned int                                       face_no)
{
  // assert that the finite elements passed to the constructor and
  // used by the DoFHandler used by this cell, are the same
  Assert(static_cast<const FiniteElementData<dim> &>(*this->fe) ==
           static_cast<const FiniteElementData<dim> &>(
             cell->get_dof_handler().get_fe(cell->active_fe_index())),
         (typename FEValuesBase<dim, spacedim>::ExcFEDontMatch()));

  AssertIndexRange(face_no, GeometryInfo<dim>::faces_per_cell);

  this->maybe_invalidate_previous_present_cell(cell);
  this->present_cell = {cell};

  // this was the part of the work that is dependent on the actual
  // data type of the iterator. now pass on to the function doing
  // the real work.
  do_reinit(face_no);
}



template <int dim, int spacedim>
template <bool lda>
void
FEFaceValues<dim, spacedim>::reinit(
  const TriaIterator<DoFCellAccessor<dim, spacedim, lda>> &   cell,
  const typename Triangulation<dim, spacedim>::face_iterator &face)
{
  const auto face_n = cell->face_iterator_to_index(face);
  reinit(cell, face_n);
}



template <int dim, int spacedim>
void
FEFaceValues<dim, spacedim>::reinit(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const unsigned int                                          face_no)
{
  AssertIndexRange(face_no, GeometryInfo<dim>::faces_per_cell);

  this->maybe_invalidate_previous_present_cell(cell);
  this->present_cell = {cell};

  // this was the part of the work that is dependent on the actual
  // data type of the iterator. now pass on to the function doing
  // the real work.
  do_reinit(face_no);
}



template <int dim, int spacedim>
void
FEFaceValues<dim, spacedim>::reinit(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const typename Triangulation<dim, spacedim>::face_iterator &face)
{
  const auto face_n = cell->face_iterator_to_index(face);
  reinit(cell, face_n);
}



template <int dim, int spacedim>
void
FEFaceValues<dim, spacedim>::do_reinit(const unsigned int face_no)
{
  this->present_face_no = face_no;

  // first of all, set the present_face_index (if available)
  const typename Triangulation<dim, spacedim>::cell_iterator cell =
    this->present_cell;
  this->present_face_index = cell->face_index(face_no);

  if (this->update_flags & update_mapping)
    {
      this->get_mapping().fill_fe_face_values(this->present_cell,
                                              face_no,
                                              this->quadrature,
                                              *this->mapping_data,
                                              this->mapping_output);
    }

  this->get_fe().fill_fe_face_values(this->present_cell,
                                     face_no,
                                     this->quadrature,
                                     this->get_mapping(),
                                     *this->mapping_data,
                                     this->mapping_output,
                                     *this->fe_data,
                                     this->finite_element_output);

  const_cast<unsigned int &>(this->n_quadrature_points) =
    this->quadrature[this->quadrature.size() == 1 ? 0 : face_no].size();
}


/* ---------------------------- FESubFaceValues ---------------------------- */


template <int dim, int spacedim>
const unsigned int FESubfaceValues<dim, spacedim>::dimension;



template <int dim, int spacedim>
const unsigned int FESubfaceValues<dim, spacedim>::integral_dimension;



template <int dim, int spacedim>
FESubfaceValues<dim, spacedim>::FESubfaceValues(
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe,
  const Quadrature<dim - 1> &         quadrature,
  const UpdateFlags                   update_flags)
  : FEFaceValuesBase<dim, spacedim>(fe.n_dofs_per_cell(),
                                    update_flags,
                                    mapping,
                                    fe,
                                    quadrature)
{
  initialize(update_flags);
}



template <int dim, int spacedim>
FESubfaceValues<dim, spacedim>::FESubfaceValues(
  const Mapping<dim, spacedim> &      mapping,
  const FiniteElement<dim, spacedim> &fe,
  const hp::QCollection<dim - 1> &    quadrature,
  const UpdateFlags                   update_flags)
  : FESubfaceValues(mapping, fe, quadrature[0], update_flags)
{
  AssertDimension(quadrature.size(), 1);
}



template <int dim, int spacedim>
FESubfaceValues<dim, spacedim>::FESubfaceValues(
  const FiniteElement<dim, spacedim> &fe,
  const Quadrature<dim - 1> &         quadrature,
  const UpdateFlags                   update_flags)
  : FEFaceValuesBase<dim, spacedim>(
      fe.n_dofs_per_cell(),
      update_flags,
      fe.reference_cell().template get_default_linear_mapping<dim, spacedim>(),
      fe,
      quadrature)
{
  initialize(update_flags);
}



template <int dim, int spacedim>
FESubfaceValues<dim, spacedim>::FESubfaceValues(
  const FiniteElement<dim, spacedim> &fe,
  const hp::QCollection<dim - 1> &    quadrature,
  const UpdateFlags                   update_flags)
  : FESubfaceValues(fe, quadrature[0], update_flags)
{
  AssertDimension(quadrature.size(), 1);
}



template <int dim, int spacedim>
void
FESubfaceValues<dim, spacedim>::initialize(const UpdateFlags update_flags)
{
  const UpdateFlags flags = this->compute_update_flags(update_flags);

  // initialize the base classes
  if (flags & update_mapping)
    this->mapping_output.initialize(this->max_n_quadrature_points, flags);
  this->finite_element_output.initialize(this->max_n_quadrature_points,
                                         *this->fe,
                                         flags);

  // then get objects into which the FE and the Mapping can store
  // intermediate data used across calls to reinit. this can be done
  // in parallel
  Threads::Task<
    std::unique_ptr<typename FiniteElement<dim, spacedim>::InternalDataBase>>
    fe_get_data =
      Threads::new_task(&FiniteElement<dim, spacedim>::get_subface_data,
                        *this->fe,
                        flags,
                        *this->mapping,
                        this->quadrature[0],
                        this->finite_element_output);
  Threads::Task<
    std::unique_ptr<typename Mapping<dim, spacedim>::InternalDataBase>>
    mapping_get_data;
  if (flags & update_mapping)
    mapping_get_data =
      Threads::new_task(&Mapping<dim, spacedim>::get_subface_data,
                        *this->mapping,
                        flags,
                        this->quadrature[0]);

  this->update_flags = flags;

  // then collect answers from the two task above
  this->fe_data = std::move(fe_get_data.return_value());
  if (flags & update_mapping)
    this->mapping_data = std::move(mapping_get_data.return_value());
  else
    this->mapping_data =
      std::make_unique<typename Mapping<dim, spacedim>::InternalDataBase>();
}



template <int dim, int spacedim>
template <bool lda>
void
FESubfaceValues<dim, spacedim>::reinit(
  const TriaIterator<DoFCellAccessor<dim, spacedim, lda>> &cell,
  const unsigned int                                       face_no,
  const unsigned int                                       subface_no)
{
  // assert that the finite elements passed to the constructor and
  // used by the DoFHandler used by this cell, are the same
  Assert(static_cast<const FiniteElementData<dim> &>(*this->fe) ==
           static_cast<const FiniteElementData<dim> &>(
             cell->get_dof_handler().get_fe(cell->active_fe_index())),
         (typename FEValuesBase<dim, spacedim>::ExcFEDontMatch()));
  AssertIndexRange(face_no, GeometryInfo<dim>::faces_per_cell);
  // We would like to check for subface_no < cell->face(face_no)->n_children(),
  // but unfortunately the current function is also called for
  // faces without children (see tests/fe/mapping.cc). Therefore,
  // we must use following workaround of two separate assertions
  Assert(cell->face(face_no)->has_children() ||
           subface_no < GeometryInfo<dim>::max_children_per_face,
         ExcIndexRange(subface_no,
                       0,
                       GeometryInfo<dim>::max_children_per_face));
  Assert(!cell->face(face_no)->has_children() ||
           subface_no < cell->face(face_no)->n_active_descendants(),
         ExcIndexRange(subface_no,
                       0,
                       cell->face(face_no)->n_active_descendants()));
  Assert(cell->has_children() == false,
         ExcMessage("You can't use subface data for cells that are "
                    "already refined. Iterate over their children "
                    "instead in these cases."));

  this->maybe_invalidate_previous_present_cell(cell);
  this->present_cell = {cell};

  // this was the part of the work that is dependent on the actual
  // data type of the iterator. now pass on to the function doing
  // the real work.
  do_reinit(face_no, subface_no);
}



template <int dim, int spacedim>
template <bool lda>
void
FESubfaceValues<dim, spacedim>::reinit(
  const TriaIterator<DoFCellAccessor<dim, spacedim, lda>> &   cell,
  const typename Triangulation<dim, spacedim>::face_iterator &face,
  const typename Triangulation<dim, spacedim>::face_iterator &subface)
{
  reinit(cell,
         cell->face_iterator_to_index(face),
         face->child_iterator_to_index(subface));
}



template <int dim, int spacedim>
void
FESubfaceValues<dim, spacedim>::reinit(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const unsigned int                                          face_no,
  const unsigned int                                          subface_no)
{
  AssertIndexRange(face_no, GeometryInfo<dim>::faces_per_cell);
  // We would like to check for subface_no < cell->face(face_no)->n_children(),
  // but unfortunately the current function is also called for
  // faces without children for periodic faces, which have hanging nodes on
  // the other side (see include/deal.II/matrix_free/mapping_info.templates.h).
  AssertIndexRange(subface_no,
                   (cell->has_periodic_neighbor(face_no) ?
                      cell->periodic_neighbor(face_no)
                        ->face(cell->periodic_neighbor_face_no(face_no))
                        ->n_children() :
                      cell->face(face_no)->n_children()));

  this->maybe_invalidate_previous_present_cell(cell);
  this->present_cell = {cell};

  // this was the part of the work that is dependent on the actual
  // data type of the iterator. now pass on to the function doing
  // the real work.
  do_reinit(face_no, subface_no);
}



template <int dim, int spacedim>
void
FESubfaceValues<dim, spacedim>::reinit(
  const typename Triangulation<dim, spacedim>::cell_iterator &cell,
  const typename Triangulation<dim, spacedim>::face_iterator &face,
  const typename Triangulation<dim, spacedim>::face_iterator &subface)
{
  reinit(cell,
         cell->face_iterator_to_index(face),
         face->child_iterator_to_index(subface));
}



template <int dim, int spacedim>
void
FESubfaceValues<dim, spacedim>::do_reinit(const unsigned int face_no,
                                          const unsigned int subface_no)
{
  this->present_face_no = face_no;

  // first of all, set the present_face_index (if available)
  const typename Triangulation<dim, spacedim>::cell_iterator cell =
    this->present_cell;

  if (!cell->face(face_no)->has_children())
    // no subfaces at all, so set present_face_index to this face rather
    // than any subface
    this->present_face_index = cell->face_index(face_no);
  else if (dim != 3)
    this->present_face_index = cell->face(face_no)->child_index(subface_no);
  else
    {
      // this is the same logic we use in cell->neighbor_child_on_subface(). See
      // there for an explanation of the different cases
      unsigned int subface_index = numbers::invalid_unsigned_int;
      switch (cell->subface_case(face_no))
        {
          case internal::SubfaceCase<3>::case_x:
          case internal::SubfaceCase<3>::case_y:
          case internal::SubfaceCase<3>::case_xy:
            subface_index = cell->face(face_no)->child_index(subface_no);
            break;
          case internal::SubfaceCase<3>::case_x1y2y:
          case internal::SubfaceCase<3>::case_y1x2x:
            subface_index = cell->face(face_no)
                              ->child(subface_no / 2)
                              ->child_index(subface_no % 2);
            break;
          case internal::SubfaceCase<3>::case_x1y:
          case internal::SubfaceCase<3>::case_y1x:
            switch (subface_no)
              {
                case 0:
                case 1:
                  subface_index =
                    cell->face(face_no)->child(0)->child_index(subface_no);
                  break;
                case 2:
                  subface_index = cell->face(face_no)->child_index(1);
                  break;
                default:
                  Assert(false, ExcInternalError());
              }
            break;
          case internal::SubfaceCase<3>::case_x2y:
          case internal::SubfaceCase<3>::case_y2x:
            switch (subface_no)
              {
                case 0:
                  subface_index = cell->face(face_no)->child_index(0);
                  break;
                case 1:
                case 2:
                  subface_index =
                    cell->face(face_no)->child(1)->child_index(subface_no - 1);
                  break;
                default:
                  Assert(false, ExcInternalError());
              }
            break;
          default:
            Assert(false, ExcInternalError());
            break;
        }
      Assert(subface_index != numbers::invalid_unsigned_int,
             ExcInternalError());
      this->present_face_index = subface_index;
    }

  // now ask the mapping and the finite element to do the actual work
  if (this->update_flags & update_mapping)
    {
      this->get_mapping().fill_fe_subface_values(this->present_cell,
                                                 face_no,
                                                 subface_no,
                                                 this->quadrature[0],
                                                 *this->mapping_data,
                                                 this->mapping_output);
    }

  this->get_fe().fill_fe_subface_values(this->present_cell,
                                        face_no,
                                        subface_no,
                                        this->quadrature[0],
                                        this->get_mapping(),
                                        *this->mapping_data,
                                        this->mapping_output,
                                        *this->fe_data,
                                        this->finite_element_output);
}


/*------------------------------- Explicit Instantiations -------------*/
#define SPLIT_INSTANTIATIONS_COUNT 6
#ifndef SPLIT_INSTANTIATIONS_INDEX
#  define SPLIT_INSTANTIATIONS_INDEX 0
#endif
#include "fe_values.inst"

DEAL_II_NAMESPACE_CLOSE
