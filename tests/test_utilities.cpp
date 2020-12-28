// Copyright (c) 2019-2020, Lawrence Livermore National Security, LLC and
// other Serac Project Developers. See the top-level LICENSE file for
// details.
//
// SPDX-License-Identifier: (BSD-3-Clause)

#include "test_utilities.hpp"

#include <gtest/gtest.h>

#include "serac/infrastructure/input.hpp"
#include "serac/numerics/mesh_utils.hpp"
#include "serac/physics/nonlinear_solid.hpp"
#include "serac/physics/thermal_conduction.hpp"

namespace serac {

namespace test_utils {

void defineNonlinSolidInputFileSchema(axom::inlet::Inlet& inlet)
{
  // Simulation time parameters
  inlet.addDouble("dt", "Time step.");
  inlet.addDouble("t_final", "Stopping point");

  // Integration test parameters
  inlet.addDouble("expected_x_l2norm", "Correct L2 norm of the displacement field");
  inlet.addDouble("expected_v_l2norm", "Correct L2 norm of the velocity field");
  inlet.addDouble("epsilon", "Threshold to be used in the comparison");

  auto& mesh_table = inlet.addTable("main_mesh", "The main mesh for the problem");
  serac::mesh::InputOptions::defineInputFileSchema(mesh_table);

  // Physics
  auto& solid_solver_table = inlet.addTable("nonlinear_solid", "Finite deformation solid mechanics module");
  serac::NonlinearSolid::InputOptions::defineInputFileSchema(solid_solver_table);

  // Verify input file
  if (!inlet.verify()) {
    SLIC_ERROR("Input file failed to verify.");
  }
}

void runNonlinSolidTest(const std::string& input_file)
{
  // Create DataStore
  axom::sidre::DataStore datastore;

  // Initialize Inlet and read input file
  auto inlet = serac::input::initialize(datastore, input_file);

  defineNonlinSolidInputFileSchema(inlet);

  // Build the mesh
  auto mesh_options   = inlet["main_mesh"].get<serac::mesh::InputOptions>();
  auto full_mesh_path = serac::input::findMeshFilePath(mesh_options.relative_mesh_file_name, input_file);
  auto mesh = serac::buildMeshFromFile(full_mesh_path, mesh_options.ser_ref_levels, mesh_options.par_ref_levels);

  // Define the solid solver object
  auto           solid_solver_options = inlet["nonlinear_solid"].get<serac::NonlinearSolid::InputOptions>();
  NonlinearSolid solid_solver(mesh, solid_solver_options);

  const bool is_dynamic = inlet["nonlinear_solid"].contains("dynamics");

  if (is_dynamic) {
    auto visc = std::make_unique<mfem::ConstantCoefficient>(0.0);
    solid_solver.setViscosity(std::move(visc));
  }

  // Initialize the output
  solid_solver.initializeOutput(serac::OutputType::VisIt, "nonlin_solid");

  // Complete the solver setup
  solid_solver.completeSetup();
  // Output the initial state
  solid_solver.outputState();

  double dt = inlet["dt"];

  // Check if dynamic
  if (is_dynamic) {
    double t       = 0.0;
    double t_final = inlet["t_final"];

    // Perform time-integration
    // (looping over the time iterations, ti, with a time-step dt).
    bool last_step = false;
    for (int ti = 1; !last_step; ti++) {
      double dt_real = std::min(dt, t_final - t);
      t += dt_real;
      last_step = (t >= t_final - 1e-8 * dt);

      solid_solver.advanceTimestep(dt_real);
    }
  } else {
    solid_solver.advanceTimestep(dt);
  }

  // Output the final state
  solid_solver.outputState();

  mfem::Vector zero(mesh->Dimension());
  zero = 0.0;
  mfem::VectorConstantCoefficient zerovec(zero);

  if (inlet.contains("expected_x_l2norm")) {
    double x_norm = solid_solver.displacement().gridFunc().ComputeLpError(2.0, zerovec);
    EXPECT_NEAR(inlet["expected_x_l2norm"], x_norm, inlet["epsilon"]);
  }
  if (inlet.contains("expected_v_l2norm")) {
    double v_norm = solid_solver.velocity().gridFunc().ComputeLpError(2.0, zerovec);
    EXPECT_NEAR(inlet["expected_v_l2norm"], v_norm, inlet["epsilon"]);
  }
}

void defineThermalConductionInputFileSchema(axom::inlet::Inlet& inlet)
{
  // Simulation time parameters
  inlet.addDouble("dt", "Time step.");
  inlet.addDouble("t_final", "Stopping point");

  // Integration test parameters
  inlet.addDouble("expected_t_l2norm", "Correct L2 norm of the temperature field");
  inlet.addDouble("epsilon", "Threshold to be used in the comparison");

  auto& mesh_table = inlet.addTable("main_mesh", "The main mesh for the problem");
  serac::mesh::InputOptions::defineInputFileSchema(mesh_table);

  // Physics
  auto& conduction_table = inlet.addTable("thermal_conduction", "Thermal conduction module");
  serac::ThermalConduction::InputOptions::defineInputFileSchema(conduction_table);

  // Verify input file
  if (!inlet.verify()) {
    SLIC_ERROR("Input file failed to verify.");
  }
}

void runThermalConductionTest(const std::string& input_file, std::shared_ptr<mfem::ParMesh> custom_mesh)
{
  // Create DataStore
  axom::sidre::DataStore datastore;

  // Initialize Inlet and read input file
  auto inlet = serac::input::initialize(datastore, input_file);

  defineThermalConductionInputFileSchema(inlet);

  // Build the mesh
  std::shared_ptr<mfem::ParMesh> mesh;
  if (custom_mesh) {
    mesh = custom_mesh;
  } else {
    auto mesh_options   = inlet["main_mesh"].get<serac::mesh::InputOptions>();
    auto full_mesh_path = serac::input::findMeshFilePath(mesh_options.relative_mesh_file_name, input_file);
    mesh = serac::buildMeshFromFile(full_mesh_path, mesh_options.ser_ref_levels, mesh_options.par_ref_levels);
  }

  // Define the thermal solver object
  auto              thermal_solver_options = inlet["thermal_conduction"].get<serac::ThermalConduction::InputOptions>();
  ThermalConduction therm_solver(mesh, thermal_solver_options);

  const bool is_dynamic = inlet["thermal_conduction"].contains("dynamics");
  therm_solver.initializeOutput(serac::OutputType::GLVis, "thermal_solver");

  // Complete the setup without allocating the mass matrices and dynamic
  // operator
  therm_solver.completeSetup();

  // Output the initial state
  therm_solver.outputState();

  double dt = inlet["dt"];

  // Check if dynamic
  if (is_dynamic) {
    double t       = 0.0;
    double t_final = inlet["t_final"];

    // Perform time-integration
    // (looping over the time iterations, ti, with a time-step dt).
    bool last_step = false;
    for (int ti = 1; !last_step; ti++) {
      double dt_real = std::min(dt, t_final - t);
      t += dt_real;
      last_step = (t >= t_final - 1e-8 * dt);

      // Advance the timestep
      therm_solver.advanceTimestep(dt_real);
    }
  } else {
    // Perform the static solve
    therm_solver.advanceTimestep(dt);
  }

  // Output the final state
  therm_solver.outputState();

  mfem::ConstantCoefficient zero(0.0);

  // Measure the L2 norm of the solution and check the value
  if (inlet.contains("expected_t_l2norm")) {
    double t_norm = therm_solver.temperature().gridFunc().ComputeLpError(2.0, zero);
    EXPECT_NEAR(inlet["expected_t_l2norm"], t_norm, inlet["epsilon"]);
  }
}

}  // end namespace test_utils

}  // end namespace serac
