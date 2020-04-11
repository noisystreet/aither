/*  This file is part of aither.
    Copyright (C) 2015-19  Michael Nucci (mnucci@pm.me)

    Aither is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Aither is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>. */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <string>
#include <utility>  // pair
#include <cmath>
#include "output.hpp"
#include "vector3d.hpp"  // vector3d
#include "multiArray3d.hpp"  // multiArray3d
#include "tensor.hpp"    // tensor
#include "plot3d.hpp"    // plot3d
#include "physicsModels.hpp"
#include "primitive.hpp"           // primitive
#include "procBlock.hpp"           // procBlock
#include "inviscidFlux.hpp"        // inviscidFlux
#include "input.hpp"               // inputVars
#include "parallel.hpp"            // decomposition
#include "resid.hpp"               // resid
#include "conserved.hpp"           // conserved
#include "varArray.hpp"            // residual
#include "utility.hpp"
#include "gridLevel.hpp"

using std::cout;
using std::endl;
using std::cerr;
using std::vector;
using std::string;
using std::ios;
using std::ofstream;
using std::to_string;
using std::max;
using std::pair;
using std::setw;
using std::setprecision;

//-----------------------------------------------------------------------
// function declarations
// function to write out cell centers of grid in plot3d format
void WriteCellCenter(const string &gridName, const vector<procBlock> &vars,
                     const decomposition &decomp, const input &inp) {
  // recombine procblocks into original configuration
  auto recombVars = Recombine(vars, decomp);

  // open binary output file
  const string fEnd = "_center";
  const string fPostfix = ".xyz";
  const auto writeName = gridName + fEnd + fPostfix;
  ofstream outFile(writeName, ios::out | ios::binary);

  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: Grid file " << writeName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  WriteBlockDims(outFile, recombVars);

  // write out x, y, z coordinates of cell centers
  for (auto &blk : recombVars) {  // loop over all blocks
    for (auto nn = 0; nn < 3; nn++) {  // loop over dimensions (3)
      for (auto kk = blk.StartK(); kk < blk.EndK(); kk++) {
        for (auto jj = blk.StartJ(); jj < blk.EndJ(); jj++) {
          for (auto ii = blk.StartI(); ii < blk.EndI(); ii++) {
            // get the cell center coordinates (dimensionalized)
            auto dumVec = blk.Center(ii, jj, kk) * inp.LRef();

            // for a given block, first write out all x coordinates, then all y
            // coordinates, then all z coordinates
            auto dumDouble = dumVec[nn];
            // write to file
            outFile.write(reinterpret_cast<char *>(&dumDouble),
                          sizeof(dumDouble));
          }
        }
      }
    }
  }
  // close output file
  outFile.close();

  if (inp.NumWallVarsOutput() > 0) {
    WriteWallFaceCenter(gridName, recombVars, inp.LRef());
  }
}

// function to write out cell centers of grid in plot3d format
void WriteNodes(const string &gridName, const vector<plot3dBlock> &blks) {
  // open binary output file
  const string fEnd = "_nodes";
  const string fPostfix = ".xyz";
  const auto writeName = gridName + fEnd + fPostfix;
  ofstream outFile(writeName, ios::out | ios::binary);

  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: Grid file " << writeName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  WriteBlockDims(outFile, blks);

  // write out x, y, z coordinates of cell centers
  for (const auto &blk : blks) {
    for (auto nn = 0; nn < 3; nn++) {  // loop over dimensions (3)
      for (auto kk = 0; kk < blk.NumK(); kk++) {
        for (auto jj = 0; jj < blk.NumJ(); jj++) {
          for (auto ii = 0; ii < blk.NumI(); ii++) {
            // get the cell center coordinates (dimensionalized)
            auto dumVec = blk.Coords(ii, jj, kk);
            // for a given block, first write out all x coordinates, then all y
            // coordinates, then all z coordinates
            auto dumDouble = dumVec[nn];
            // write to file
            outFile.write(reinterpret_cast<char *>(&dumDouble),
                          sizeof(dumDouble));
          }
        }
      }
    }
  }
  // close output file
  outFile.close();
}

// function to write out wall face centers of grid in plot3d format
void WriteWallFaceCenter(const string &gridName, const vector<procBlock> &vars,
                         const double &LRef) {
  // open binary output file
  const string fEnd = "_wall_center";
  const string fPostfix = ".xyz";
  const auto writeName = gridName + fEnd + fPostfix;
  ofstream outFile(writeName, ios::out | ios::binary);

  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: Grid file " << writeName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  // get wall face centers
  auto numWallSurfs = 0;
  for (auto &var : vars) {
    numWallSurfs += var.BC().NumViscousSurfaces();
  }
  vector<multiArray3d<vector3d<double>>> wallCenters;
  wallCenters.reserve(numWallSurfs);

  for (auto &var : vars) {
    const auto bc = var.BC();
    for (auto jj = 0; jj < bc.NumSurfaces(); ++jj) {
      if (bc.GetBCTypes(jj) == "viscousWall") {
        auto wall = var.SliceBoundaryCenters(jj);
        wallCenters.push_back(wall);
      }
    }
  }

  WriteBlockDims(outFile, wallCenters);

  // write out x, y, z coordinates of cell centers
  for (auto &wBlk : wallCenters) {  // loop over all blocks
    for (auto nn = 0; nn < 3; nn++) {  // loop over dimensions (3)
      for (auto kk = wBlk.StartK(); kk < wBlk.EndK(); kk++) {
        for (auto jj = wBlk.StartJ(); jj < wBlk.EndJ(); jj++) {
          for (auto ii = wBlk.StartI(); ii < wBlk.EndI(); ii++) {
            // get the cell center coordinates (dimensionalized)
            auto dumVec = wBlk(ii, jj, kk) * LRef;

            // for a given block, first write out all x coordinates, then all y
            // coordinates, then all z coordinates
            auto dumDouble = dumVec[nn];
            // write to file
            outFile.write(reinterpret_cast<char *>(&dumDouble),
                          sizeof(dumDouble));
          }
        }
      }
    }
  }

  // close output file
  outFile.close();
}


//----------------------------------------------------------------------
// function to write out variables in function file format
void WriteFunFile(const vector<procBlock> &vars,
                  const vector<procBlock> &recombVars, const physics &phys,
                  const decomposition &decomp, const string &writeName,
                  const input &inp) {
  // open binary plot3d function file
  ofstream outFile(writeName, ios::out | ios::binary);

  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: Function file " << writeName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  WriteBlockDims(outFile, recombVars, inp.NumVarsOutput());

  // write out variables
  auto ll = 0;
  for (auto &blk : recombVars) {  // loop over all blocks
    // loop over the number of variables to write out
    for (auto &var : inp.OutputVariables()) {
      // write out dimensional variables -- loop over physical cells
      for (auto kk = blk.StartK(); kk < blk.EndK(); kk++) {
        for (auto jj = blk.StartJ(); jj < blk.EndJ(); jj++) {
          for (auto ii = blk.StartI(); ii < blk.EndI(); ii++) {
            auto value = 0.0;
            if (var == "density") {
              value = blk.State(ii, jj, kk).Rho();
              value *= inp.RRef();
            } else if (var == "vel_x") {
              value = blk.State(ii, jj, kk).U();
              value *= inp.ARef();
            } else if (var == "vel_y") {
              value = blk.State(ii, jj, kk).V();
              value *= inp.ARef();
            } else if (var == "vel_z") {
              value = blk.State(ii, jj, kk).W();
              value *= inp.ARef();
            } else if (var == "pressure") {
              value = blk.State(ii, jj, kk).P();
              value *= inp.RRef() * inp.ARef() * inp.ARef();
            } else if (var == "mach") {
              auto vel = blk.State(ii, jj, kk).Velocity();
              value = vel.Mag() / blk.State(ii, jj, kk).SoS(phys);
            } else if (var == "sos") {
              value = blk.State(ii, jj, kk).SoS(phys);
              value *= inp.ARef();
            } else if (var == "dt") {
              value = blk.Dt(ii, jj, kk);
              value /= inp.ARef() * inp.LRef();
            } else if (var == "temperature") {
              value = blk.Temperature(ii, jj, kk);
              value *= inp.TRef();
            } else if (var == "energy") {
              value = blk.State(ii, jj, kk).Energy(phys);
              value *= inp.ARef() * inp.ARef();
            } else if (var == "enthalpy") {
              value = blk.State(ii, jj, kk).Enthalpy(phys);
              value *= inp.ARef() * inp.ARef();
            } else if (var == "cp") {
              value = phys.Thermodynamic()->Cp(
                  blk.Temperature(ii, jj, kk),
                  blk.State(ii, jj, kk).MassFractions());
              value *= inp.ARef() * inp.ARef() / inp.TRef();
            } else if (var == "cv") {
              value = phys.Thermodynamic()->Cv(
                  blk.Temperature(ii, jj, kk),
                  blk.State(ii, jj, kk).MassFractions());
              value *= inp.ARef() * inp.ARef() / inp.TRef();
            } else if (var == "rank") {
              value = vars[SplitBlockNumber(recombVars, decomp,
                                            ll, ii, jj, kk)].Rank();
            } else if (var == "globalPosition") {
              value = vars[SplitBlockNumber(recombVars, decomp,
                                            ll, ii, jj, kk)].GlobalPos();
            } else if (var == "viscosityRatio") {
              value = blk.IsTurbulent() ?
                  blk.EddyViscosity(ii, jj, kk) /
                  blk.Viscosity(ii, jj, kk)
                  : 0.0;
            } else if (var == "turbulentViscosity") {
              value = blk.EddyViscosity(ii, jj, kk);
              value *= phys.Transport()->MuRef();
            } else if (var == "viscosity") {
              value = blk.Viscosity(ii, jj, kk);
              value *= phys.Transport()->MuRef();
            } else if (var == "tke") {
              value = blk.State(ii, jj, kk).Tke();
              value *= inp.ARef() * inp.ARef();
            } else if (var == "sdr") {
              value = blk.State(ii, jj, kk).Omega();
              value *= inp.ARef() * inp.ARef() * inp.RRef() /
                       phys.Transport()->MuRef();
            } else if (var == "f1") {
              value = blk.F1(ii, jj, kk);
            } else if (var == "f2") {
              value = blk.F2(ii, jj, kk);
            } else if (var == "wallDistance") {
              value = blk.WallDist(ii, jj, kk);
              value *= inp.LRef();
            } else if (var == "velGrad_ux") {
              value = blk.VelGrad(ii, jj, kk).XX();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_vx") {
              value = blk.VelGrad(ii, jj, kk).XY();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_wx") {
              value = blk.VelGrad(ii, jj, kk).XZ();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_uy") {
              value = blk.VelGrad(ii, jj, kk).YX();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_vy") {
              value = blk.VelGrad(ii, jj, kk).YY();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_wy") {
              value = blk.VelGrad(ii, jj, kk).YZ();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_uz") {
              value = blk.VelGrad(ii, jj, kk).ZX();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_vz") {
              value = blk.VelGrad(ii, jj, kk).ZY();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "velGrad_wz") {
              value = blk.VelGrad(ii, jj, kk).ZZ();
              value *= inp.ARef() / inp.LRef();
            } else if (var == "tempGrad_x") {
              value = blk.TempGrad(ii, jj, kk).X();
              value *= inp.TRef() / inp.LRef();
            } else if (var == "tempGrad_y") {
              value = blk.TempGrad(ii, jj, kk).Y();
              value *= inp.TRef() / inp.LRef();
            } else if (var == "tempGrad_z") {
              value = blk.TempGrad(ii, jj, kk).Z();
              value *= inp.TRef() / inp.LRef();
            } else if (var == "densityGrad_x") {
              value = blk.DensityGrad(ii, jj, kk).X();
              value *= inp.RRef() / inp.LRef();
            } else if (var == "densityGrad_y") {
              value = blk.DensityGrad(ii, jj, kk).Y();
              value *= inp.RRef() / inp.LRef();
            } else if (var == "densityGrad_z") {
              value = blk.DensityGrad(ii, jj, kk).Z();
              value *= inp.RRef() / inp.LRef();
            } else if (var == "pressGrad_x") {
              value = blk.PressureGrad(ii, jj, kk).X();
              value *= inp.RRef() * inp.ARef() * inp.ARef() / inp.LRef();
            } else if (var == "pressGrad_y") {
              value = blk.PressureGrad(ii, jj, kk).Y();
              value *= inp.RRef() * inp.ARef() * inp.ARef() / inp.LRef();
            } else if (var == "pressGrad_z") {
              value = blk.PressureGrad(ii, jj, kk).Z();
              value *= inp.RRef() * inp.ARef() * inp.ARef() / inp.LRef();
            } else if (var == "tkeGrad_x") {
              value = blk.TkeGrad(ii, jj, kk).X();
              value *= inp.ARef() * inp.ARef() / inp.LRef();
            } else if (var == "tkeGrad_y") {
              value = blk.TkeGrad(ii, jj, kk).Y();
              value *= inp.ARef() * inp.ARef() / inp.LRef();
            } else if (var == "tkeGrad_z") {
              value = blk.TkeGrad(ii, jj, kk).Z();
              value *= inp.ARef() * inp.ARef() / inp.LRef();
            } else if (var == "omegaGrad_x") {
              value = blk.OmegaGrad(ii, jj, kk).X();
              value *= inp.ARef() * inp.ARef() * inp.RRef() /
                  (phys.Transport()->MuRef() * inp.LRef());
            } else if (var == "omegaGrad_y") {
              value = blk.OmegaGrad(ii, jj, kk).Y();
              value *= inp.ARef() * inp.ARef() * inp.RRef() /
                  (phys.Transport()->MuRef() * inp.LRef());
            } else if (var == "omegaGrad_z") {
              value = blk.OmegaGrad(ii, jj, kk).Z();
              value *= inp.ARef() * inp.ARef() * inp.RRef() /
                  (phys.Transport()->MuRef() * inp.LRef());
            } else if (var == "resid_mass") {
              value = blk.Residual(ii, jj, kk, 0);
              value *= inp.RRef() * inp.ARef() * inp.LRef() * inp.LRef();
            } else if (var == "resid_mom_x") {
              value = blk.Residual(ii, jj, kk, 1);
              value *= inp.RRef() * inp.ARef() * inp.ARef() * inp.LRef() *
                  inp.LRef();
            } else if (var == "resid_mom_y") {
              value = blk.Residual(ii, jj, kk, 2);
              value *= inp.RRef() * inp.ARef() * inp.ARef() * inp.LRef() *
                  inp.LRef();
            } else if (var == "resid_mom_z") {
              value = blk.Residual(ii, jj, kk, 3);
              value *= inp.RRef() * inp.ARef() * inp.ARef() * inp.LRef() *
                  inp.LRef();
            } else if (var == "resid_energy") {
              value = blk.Residual(ii, jj, kk, 4);
              value *= inp.RRef() * pow(inp.ARef(), 3.0) * inp.LRef() *
                  inp.LRef();
            } else if (var == "resid_tke") {
              value = blk.Residual(ii, jj, kk, 5);
              value *= inp.RRef() * pow(inp.ARef(), 3.0) * inp.LRef() *
                  inp.LRef();
            } else if (var == "resid_sdr") {
              value = blk.Residual(ii, jj, kk, 6);
              value *= inp.RRef() * inp.RRef() * pow(inp.ARef(), 4.0) *
                  inp.LRef() * inp.LRef() / phys.Transport()->MuRef();
            } else if (var.substr(0, 3) == "mf_" &&
                       inp.HaveSpecies(var.substr(3, string::npos))) {
              auto ind = inp.SpeciesIndex(var.substr(3, string::npos));
              value = blk.State(ii, jj, kk).MassFractionN(ind);
            } else if (var.substr(0, 3) == "vf_" &&
                       inp.HaveSpecies(var.substr(3, string::npos))) {
              auto ind = inp.SpeciesIndex(var.substr(3, string::npos));
              value =
                  blk.State(ii, jj, kk).VolumeFractions(phys.Transport())[ind];
            } else {
              cerr << "ERROR: Variable " << var
                   << " to write to function file is not defined!" << endl;
              exit(EXIT_FAILURE);
            }

            outFile.write(reinterpret_cast<char *>(&value), sizeof(value));
          }
        }
      }
    }
    ll++;
  }

  // close plot3d function file
  outFile.close();
}

// function to write out variables in function file format
void WriteCenterFun(const vector<procBlock> &vars,
                    const vector<procBlock> &recombVars, const physics &phys,
                    const int &solIter, const decomposition &decomp,
                    const input &inp) {
  // open binary plot3d function file
  const string fEnd = "_center";
  const string fPostfix = ".fun";
  const auto writeName = inp.SimNameRoot() + "_" + to_string(solIter) + fEnd +
      fPostfix;
  WriteFunFile(vars, recombVars, phys, decomp, writeName, inp);
}

// function to write out variables in function file format
void WriteNodeFun(const vector<procBlock> &vars,
                  vector<procBlock> &recombVarsCells, const physics &phys,
                  const int &solIter, const decomposition &decomp,
                  const input &inp) {
  // interpolate data from cell centers to nodes
  vector<procBlock> recombVars;
  recombVars.reserve(recombVarsCells.size());
  for (auto &rvc : recombVarsCells) {
    rvc.AssignCornerGhostCells();
    recombVars.push_back(rvc.CellToNode());
  }

  // open binary plot3d function file
  const string fPostfix = ".fun";
  const auto writeName =
      inp.SimNameRoot() + "_" + to_string(solIter) + fPostfix;
  WriteFunFile(vars, recombVars, phys, decomp, writeName, inp);
}

// function to write out variables in function file format
void WriteWallFun(const vector<procBlock> &vars, const physics &phys,
                  const int &solIter, const input &inp) {
  // open binary plot3d function file
  const string fEnd = "_wall_center";
  const string fPostfix = ".fun";
  const auto writeName =
      inp.SimNameRoot() + "_" + to_string(solIter) + fEnd + fPostfix;
  ofstream outFile(writeName, ios::out | ios::binary);

  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: Function file " << writeName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  // get wall surfaces to write out dimensions
  auto numWallSurfs = 0;
  for (auto &blk : vars) {
    numWallSurfs += blk.WallDataSize();
  }
  vector<boundarySurface> wallSurfs;
  wallSurfs.reserve(numWallSurfs);
  for (auto &blk : vars) {
    for (auto jj = 0; jj < blk.WallDataSize(); ++jj) {
      const auto surf = blk.WallSurface(jj);
      wallSurfs.push_back(surf);
    }
  }

  WriteBlockDims(outFile, wallSurfs, inp.NumWallVarsOutput());

  // write out variables
  for (auto &blk : vars) {  // loop over all blocks
    // loop over the number of variables to write out
    for (auto &var : inp.WallOutputVariables()) {
      // loop over wall boundaries
      for (auto ll = 0; ll < blk.WallDataSize(); ++ll) {
        const auto surf = blk.WallSurface(ll);
        // write out dimensional variables -- loop over physical cells
        for (auto kk = surf.RangeK().Start(); kk < surf.RangeK().End(); kk++) {
          for (auto jj = surf.RangeJ().Start(); jj < surf.RangeJ().End();
               jj++) {
            for (auto ii = surf.RangeI().Start(); ii < surf.RangeI().End();
                 ii++) {
              // now calculate wall variables
              auto value = 0.0;
              if (var == "yplus") {
                value = blk.WallYplus(ll, ii, jj, kk);
              } else if (var == "shearStress") {
                value = blk.WallShearStress(ll, ii, jj, kk).Mag();
                value *= phys.Transport()->InvNondimScaling() *
                         phys.Transport()->MuRef() * inp.ARef() / inp.LRef();
              } else if (var == "viscosityRatio") {
                value = blk.WallEddyVisc(ll, ii, jj, kk) /
                        (blk.WallViscosity(ll, ii, jj, kk) + EPS);
              } else if (var == "heatFlux") {
                value = blk.WallHeatFlux(ll, ii, jj, kk);
                value *= phys.Transport()->MuRef() * inp.TRef() / inp.LRef();
              } else if (var == "frictionVelocity") {
                value = blk.WallFrictionVelocity(ll, ii, jj, kk);
                value *= inp.ARef();
              } else if (var == "density") {
                value = blk.WallDensity(ll, ii, jj, kk);
                value *= inp.RRef();
              } else if (var == "pressure") {
                value = blk.WallPressure(ll, ii, jj, kk, phys.EoS());
                value *= inp.RRef() * inp.ARef() * inp.ARef();
              } else if (var == "temperature") {
                value = blk.WallTemperature(ll, ii, jj, kk);
                value *= inp.TRef();
              } else if (var == "viscosity") {
                value = blk.WallViscosity(ll, ii, jj, kk);
                value *= phys.Transport()->MuRef() *
                         phys.Transport()->InvNondimScaling();
              } else if (var == "tke") {
                value = blk.WallTke(ll, ii, jj, kk);
                value *= inp.ARef() * inp.ARef();
              } else if (var == "sdr") {
                value = blk.WallSdr(ll, ii, jj, kk);
                value *= inp.ARef() * inp.ARef() * inp.RRef() /
                         phys.Transport()->MuRef();
              } else {
                cerr << "ERROR: Variable " << var
                     << " to write to wall function file is not defined!"
                     << endl;
                exit(EXIT_FAILURE);
              }

              outFile.write(reinterpret_cast<char *>(&value), sizeof(value));
            }
          }
        }
      }
    }
  }

  // close plot3d function file
  outFile.close();
}

void WriteOutput(const vector<procBlock> &vars, const physics &phys,
                 const int &solIter, const decomposition &decomp,
                 const input &inp) {
  auto recombVarsCells = Recombine(vars, decomp);
  WriteCenterFun(vars, recombVarsCells, phys, solIter, decomp, inp);
  WriteMeta(inp, solIter, true);
  if (inp.NumWallVarsOutput() > 0) {
    WriteWallFun(recombVarsCells, phys, solIter, inp);
    WriteWallMeta(inp, solIter);
  }

  if (inp.OutputNodalVariables()) {
    WriteNodeFun(vars, recombVarsCells, phys, solIter, decomp, inp);
    WriteMeta(inp, solIter, false);
  }
}

// function to write out restart variables
void WriteRestart(const vector<procBlock> &splitVars, const physics &phys,
                  const int &solIter, const decomposition &decomp,
                  const input &inp, const residual &residL2First) {
  // recombine blocks into original structure
  auto vars = Recombine(splitVars, decomp);

  // open binary restart file
  const string fPostfix = ".rst";
  const auto writeName =
      inp.SimNameRoot() + "_" + to_string(solIter) + fPostfix;
  ofstream outFile(writeName, ios::out | ios::binary);

  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: Restart file " << writeName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  // write number of time steps contained in file
  auto numSols = inp.IsMultilevelInTime() ? 2 : 1;
  outFile.write(reinterpret_cast<char *>(&numSols), sizeof(numSols));

  // write iteration number
  outFile.write(const_cast<char *>(reinterpret_cast<const char *>(&solIter)),
                sizeof(solIter));

  // write number of equations
  auto numEqns = inp.NumEquations();
  outFile.write(reinterpret_cast<char *>(&numEqns), sizeof(numEqns));

  // write number of species
  auto numSpecies = inp.NumSpecies();
  outFile.write(reinterpret_cast<char *>(&numSpecies), sizeof(numSpecies));

  // write species names (including sizes)
  for (auto ii = 0; ii < numSpecies; ++ii) {
    auto specName = inp.Fluid(ii).Name();
    auto specSize = specName.size();
    outFile.write(reinterpret_cast<char *>(&specSize), sizeof(specSize));
    outFile.write(specName.c_str(), specSize * sizeof(char));
  }

  // write residual values
  outFile.write(
      const_cast<char *>(reinterpret_cast<const char *>(&residL2First[0])),
      residL2First.Size() * sizeof(residL2First[0]));

  // variables to write to restart file
  vector<string> restartVars = {"density", "vel_x", "vel_y", "vel_z",
                                "pressure"};
  if (inp.IsRANS()) {
    restartVars.push_back("tke");
    restartVars.push_back("sdr");
  }
  for (auto ii = 0; ii < numSpecies; ++ii) {
    auto var = "mf_" + inp.Fluid(ii).Name();
    restartVars.push_back(var);
  }

  WriteBlockDims(outFile, vars, restartVars.size());

  // write out variables
  for (auto &blk : vars) {  // loop over all blocks
    // write out dimensional variables -- loop over physical cells
    for (auto kk = blk.StartK(); kk < blk.EndK(); kk++) {
      for (auto jj = blk.StartJ(); jj < blk.EndJ(); jj++) {
        for (auto ii = blk.StartI(); ii < blk.EndI(); ii++) {
          // loop over the number of variables to write out
          for (auto &var : restartVars) {
            auto value = 0.0;
            if (var == "density") {
              value = blk.State(ii, jj, kk).Rho();
              value *= inp.RRef();
            } else if (var == "vel_x") {
              value = blk.State(ii, jj, kk).U();
              value *= inp.ARef();
            } else if (var == "vel_y") {
              value = blk.State(ii, jj, kk).V();
              value *= inp.ARef();
            } else if (var == "vel_z") {
              value = blk.State(ii, jj, kk).W();
              value *= inp.ARef();
            } else if (var == "pressure") {
              value = blk.State(ii, jj, kk).P();
              value *= inp.RRef() * inp.ARef() * inp.ARef();
            } else if (var == "tke") {
              value = blk.State(ii, jj, kk).Tke();
              value *= inp.ARef() * inp.ARef();
            } else if (var == "sdr") {
              value = blk.State(ii, jj, kk).Omega();
              value *= inp.ARef() * inp.ARef() * inp.RRef() /
                       phys.Transport()->MuRef();
            } else if (var.substr(0, 3) == "mf_" &&
                       inp.HaveSpecies(var.substr(3, string::npos))) {
              auto ind = inp.SpeciesIndex(var.substr(3, string::npos));
              value = blk.State(ii, jj, kk).MassFractionN(ind);
            } else {
              cerr << "ERROR: Variable " << var
                   << " to write to restart file is not defined!" << endl;
              exit(EXIT_FAILURE);
            }

            outFile.write(reinterpret_cast<char *>(&value), sizeof(value));
          }
        }
      }
    }
  }

  // write out 2nd solution
  if (numSols == 2) {
    // these variables are conserved variables
    for (auto &blk : vars) {  // loop over all blocks
      // write out dimensional variables -- loop over physical cells
      for (auto kk = blk.StartK(); kk < blk.EndK(); kk++) {
        for (auto jj = blk.StartJ(); jj < blk.EndJ(); jj++) {
          for (auto ii = blk.StartI(); ii < blk.EndI(); ii++) {
            // loop over the number of variables to write out
            for (auto &var : restartVars) {
              auto value = 0.0;
              if (var == "density") {
                value = blk.ConsVarsNm1(ii, jj, kk)[0];
                value *= inp.RRef();
              } else if (var == "vel_x") {  // conserved var is rho-u
                value = blk.ConsVarsNm1(ii, jj, kk)[1];
                value *= inp.ARef() * inp.RRef();
              } else if (var == "vel_y") {  // conserved var is rho-v
                value = blk.ConsVarsNm1(ii, jj, kk)[2];
                value *= inp.ARef() * inp.RRef();
              } else if (var == "vel_z") {  // conserved var is rho-w
                value = blk.ConsVarsNm1(ii, jj, kk)[3];
                value *= inp.ARef() * inp.RRef();
              } else if (var == "pressure") {  // conserved var is rho-E
                value = blk.ConsVarsNm1(ii, jj, kk)[4];
                value *= inp.ARef() * inp.ARef() * inp.RRef();
              } else if (var == "tke") {  // conserved var is rho-tke
                value = blk.ConsVarsNm1(ii, jj, kk)[5];
                value *= inp.ARef() * inp.ARef() * inp.RRef();
              } else if (var == "sdr") {  // conserved var is rho-sdr
                value = blk.ConsVarsNm1(ii, jj, kk)[6];
                value *= inp.ARef() * inp.ARef() * inp.RRef() * inp.RRef() /
                         phys.Transport()->MuRef();
              } else if (var.substr(0, 3) == "mf_" &&
                         inp.HaveSpecies(var.substr(3, string::npos))) {
                auto ind = inp.SpeciesIndex(var.substr(3, string::npos));
                value = blk.ConsVarsNm1(ii, jj, kk).MassFractionN(ind);
              } else {
                cerr << "ERROR: Variable " << var
                     << " to write to restart file is not defined!" << endl;
                exit(EXIT_FAILURE);
              }

              outFile.write(reinterpret_cast<char *>(&value), sizeof(value));
            }
          }
        }
      }
    }
  }

  // close restart file
  outFile.close();
}

void ReadRestart(gridLevel &vars, const string &restartName,
                 const decomposition &decomp, input &inp, const physics &phys,
                 residual &residL2First,
                 const vector<vector3d<int>> &gridSizes) {
  // open binary restart file
  ifstream fName(restartName, ios::in | ios::binary);

  // check to see if file opened correctly
  if (fName.fail()) {
    cerr << "ERROR: Error in ReadRestart(). Restart file " << restartName
         << " did not open correctly!!!" << endl;
    exit(EXIT_FAILURE);
  }

  // read the number of time levels in file
  cout << "Reading restart file..." << endl;
  auto numSols = 0;
  fName.read(reinterpret_cast<char *>(&numSols), sizeof(numSols));
  cout << "Number of time levels: " << numSols << endl;

  if (inp.IsMultilevelInTime() && numSols != 2) {
    cerr << "WARNING: Using multilevel time integration scheme, but only one "
         << "time level found in restart file" << endl;
  }

  // iteration number
  auto iterNum = 0;
  fName.read(reinterpret_cast<char *>(&iterNum), sizeof(iterNum));
  cout << "Data from iteration: " << iterNum << endl;
  inp.SetIterationStart(iterNum);

  // read the number of equations
  auto numEqns = 0;
  fName.read(reinterpret_cast<char *>(&numEqns), sizeof(numEqns));
  cout << "Number of equations: " << numEqns << endl;

  // read the number of species
  auto numSpecies = 0;
  fName.read(reinterpret_cast<char *>(&numSpecies), sizeof(numSpecies));
  cout << "Number of species: " << numSpecies << endl;

  // read species names (including sizes)
  vector<string> speciesNames(numSpecies);
  for (auto ii = 0; ii < numSpecies; ++ii) {
    size_t nameSize = 0;
    fName.read(reinterpret_cast<char *>(&nameSize), sizeof(nameSize));
    
    auto buffer = std::make_unique<char[]>(nameSize);
    fName.read(buffer.get(), nameSize * sizeof(char));
    string sname(buffer.get(), nameSize);
    speciesNames[ii] = sname;
  }
  // check that species are defined
  inp.CheckSpecies(speciesNames);

  // read the residuals to normalize by
  fName.read(reinterpret_cast<char *>(&residL2First[0]),
             residL2First.Size() * sizeof(residL2First[0]));

  // read the number of blocks
  auto numBlks = 0;
  fName.read(reinterpret_cast<char *>(&numBlks), sizeof(numBlks));
  if (numBlks != static_cast<int>(gridSizes.size())) {
    cerr << "ERROR: Number of blocks in restart file does not match grid!"
         << endl;
    cerr << "Found " << numBlks << " blocks in restart file and "
         << gridSizes.size() << " blocks in grid." << endl;
    exit(EXIT_FAILURE);
  }

  // read the block sizes and check for match with grid
  for (auto ii = 0; ii < numBlks; ii++) {
    auto numI = 0, numJ = 0, numK = 0, numVars = 0;
    fName.read(reinterpret_cast<char *>(&numI), sizeof(numI));
    fName.read(reinterpret_cast<char *>(&numJ), sizeof(numJ));
    fName.read(reinterpret_cast<char *>(&numK), sizeof(numK));
    fName.read(reinterpret_cast<char *>(&numVars), sizeof(numVars));
    if (numI != gridSizes[ii].X() || numJ != gridSizes[ii].Y() ||
        numK != gridSizes[ii].Z() || numVars - 1 != numEqns) {
      cerr << "ERROR: Problem with restart file. Block size does not match "
           << "grid, or number of variables in block does not match number of "
           << "equations!" << endl;
      exit(EXIT_FAILURE);
    }
  }

  // variables to read from restart file
  vector<string> restartVars = {"density", "vel_x", "vel_y", "vel_z",
                                "pressure"};
  if (numEqns == numSpecies + 6) {  // have turbulence variables
    restartVars.push_back("tke");
    restartVars.push_back("sdr");
  }
  for (auto &spec : speciesNames) {
    auto var = "mf_" + spec;
    restartVars.push_back(var);
  }

  // loop over blocks and initialize
  cout << "Reading solution from time n..." << endl;
  vector<blkMultiArray3d<primitive>> solN(numBlks);
  for (auto ii = 0U; ii < solN.size(); ++ii) {
    solN[ii] =
        ReadSolFromRestart(fName, inp, phys, restartVars, gridSizes[ii].X(),
                           gridSizes[ii].Y(), gridSizes[ii].Z(), numSpecies);
  }
  // decompose solution
  decomp.DecompArray(solN);

  // assign to procBlocks
  for (auto ii = 0U; ii < solN.size(); ++ii) {
    vars.Block(ii).GetStatesFromRestart(solN[ii]);
  }

  if (inp.IsMultilevelInTime()) {
    if (numSols == 2) {
      cout << "Reading solution from time n-1..." << endl;
      vector<blkMultiArray3d<conserved>> solNm1(numBlks);
      for (auto ii = 0U; ii < solNm1.size(); ++ii) {
        solNm1[ii] = ReadSolNm1FromRestart(fName, inp, phys, restartVars,
                                           gridSizes[ii].X(), gridSizes[ii].Y(),
                                           gridSizes[ii].Z(), numSpecies);
      }
      // decompose solution
      decomp.DecompArray(solNm1);

      // assign to procBlocks
      for (auto ii = 0U; ii < solNm1.size(); ++ii) {
        vars.Block(ii).GetSolNm1FromRestart(solNm1[ii]);
      }

    } else {
      cerr << "WARNING: Using multilevel time integration scheme, but only one "
           << "time level found in restart file" << endl;
      // assign solution at time n to n-1
      vars.AssignSolToTimeN(phys);
      vars.AssignSolToTimeNm1();
    }
  }

  // close restart file
  fName.close();
  cout << "Done with restart file" << endl << endl;
}


// function to write out plot3d meta data for Paraview
void WriteMeta(const input &inp, const int &iter, const bool &isCenter) {
  // open meta file
  const string fMetaPostfix = ".p3d";
  const string fEnd = isCenter ? "_center" : "";
  const auto metaName = inp.SimNameRoot() + fEnd + fMetaPostfix;
  ofstream metaFile(metaName, ios::out);

  const auto gridName = inp.GridName() + fEnd + ".xyz";
  const auto funName = inp.SimNameRoot() + "_" + to_string(iter) + fEnd + ".fun";

  // check to see if file opened correctly
  if (metaFile.fail()) {
    cerr << "ERROR: Results file " << metaName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  const auto outputVars = inp.OutputVariables();

  // write to meta file
  metaFile << "{" << endl;
  metaFile << "\"auto-detect-format\" : true," << endl;
  metaFile << "\"format\" : \"binary\"," << endl;
  metaFile << "\"language\" : \"C\"," << endl;
  if (inp.IsTimeAccurate()) {
    metaFile << "\"filenames\" : [";
    for (auto nn = 0; nn <= iter; nn += inp.OutputFrequency()) {
      const auto currFunName =
          inp.SimNameRoot() + "_" + to_string(nn) + fEnd + ".fun";
      metaFile << "{ \"time\" : " << nn * inp.Dt() << ", \"xyz\" : \""
               << gridName << "\", \"function\" : \"" << currFunName << "\" }";
      if (nn != iter) {
        metaFile << ", " << endl;
      }
    }
    metaFile << "]," << endl;
  } else {
    metaFile << "\"filenames\" : [{ \"time\" : " << iter << ", \"xyz\" : \""
             << gridName << "\", \"function\" : \"" << funName << "\" }],"
             << endl;
  }

  // Write out scalar variables
  auto numVar = 0U;
  metaFile << "\"function-names\" : [ ";
  for (auto &var : outputVars) {
    metaFile << "\"" << var << "\"";
    if (numVar < outputVars.size() - 1) {
      metaFile << ", ";
    }
    numVar++;
  }
  metaFile << " ]" << endl;
  metaFile << "}" << endl;

  // Close results file
  metaFile.close();
}

// function to write out plot3d meta data for Paraview
void WriteWallMeta(const input &inp, const int &iter) {
  // open meta file
  const string fMetaPostfix = ".p3d";
  const string fEnd = "_wall_center";
  const auto metaName = inp.SimNameRoot() + fEnd + fMetaPostfix;
  ofstream metaFile(metaName, ios::out);

  const auto gridName = inp.GridName() + fEnd + ".xyz";
  const auto funName = inp.SimNameRoot() + "_" + to_string(iter) + fEnd + ".fun";

  // check to see if file opened correctly
  if (metaFile.fail()) {
    cerr << "ERROR: Results file " << metaName << " did not open correctly!!!"
         << endl;
    exit(EXIT_FAILURE);
  }

  const auto outputVars = inp.WallOutputVariables();

  // write to meta file
  metaFile << "{" << endl;
  metaFile << "\"auto-detect-format\" : true," << endl;
  metaFile << "\"format\" : \"binary\"," << endl;
  metaFile << "\"language\" : \"C\"," << endl;
  metaFile << "\"filenames\" : [{ \"time\" : " << iter << ", \"xyz\" : \""
           << gridName << "\", \"function\" : \"" << funName << "\" }]," << endl;

  // Write out scalar variables
  auto numVar = 0U;
  metaFile << "\"function-names\" : [ ";
  for (auto &var : outputVars) {
    metaFile << "\"" << var << "\"";
    if (numVar < outputVars.size() - 1) {
      metaFile << ", ";
    }
    numVar++;
  }
  metaFile << " ]" << endl;
  metaFile << "}" << endl;

  // Close results file
  metaFile.close();
}

void PrintHeaders(const input &inp, ostream &os) {
  // write out column headers
  os << std::left << setw(7) << "Step" << setw(8) << "NL-Iter";
  if (inp.Dt() > 0.0) {
    os << std::left << setw(12) << "Time-Step";
  } else if (inp.CFL() > 0.0) {
    os << std::left << setw(12) << "CFL";
  }
  os << std::left << setw(12) << "Res-Mass" << setw(12)
     << "Res-Mom-X" << setw(12) << "Res-Mom-Y" << setw(12) << "Res-Mom-Z"
     << setw(12) << "Res-Energy";
  if (inp.IsRANS()) {
    os << std::left << setw(12) << "Res-Tke" << setw(12) << "Res-Omega";
  }
  os << std::left << setw(8) << "Max-Eqn" << setw(8)
     << "Max-Blk" << setw(8) << "Max-I" << setw(8)
     << "Max-J" << setw(8) << "Max-K" << setw(12) << "Max-Res"
     << setw(12) << "Res-Matrix" << endl;
}

// function to write out residual information
void PrintResiduals(const input &inp, residual &residL2First,
                    const residual &residL2, const resid &residLinf,
                    const double &matrixResid, const int &nn, const int &mm,
                    ostream &os) {
  // determine normalization
  // if at first iteration and not restart, normalize by itself
  if (nn == 0 && mm == 0 && !inp.IsRestart()) {
    residL2First = residL2;
  // if within first 5 iterations reset normalization if resid at new max
  } else if ((nn < 5) && mm == 0 && !inp.IsRestart()) {
    if (residL2.SpeciesSum() > residL2First.SpeciesSum()) {
      for (auto cc = 0; cc < residL2.NumSpecies(); ++cc) {
        residL2First[cc] = residL2[cc];
      }
    }
    for (auto cc = residL2.NumSpecies(); cc < residL2.Size(); ++cc) {
      residL2First[cc] = std::max(residL2First[cc], residL2[cc]);
    }
  }

  os << std::left << setw(7) << nn << setw(8) << mm;
  if (inp.Dt() > 0.0) {
    os << std::left << setw(12) << setprecision(4) << std::scientific
       << inp.Dt();
  } else if (inp.CFL() > 0.0) {
    os << std::left << setw(12) << setprecision(4) << std::scientific
       << inp.CFL();
  }

  // normalize residuals
  // mass residual is sum of species residuals
  const auto resMass =
      (residL2.SpeciesSum() + EPS) / (residL2First.SpeciesSum() + EPS);
  const auto resNormL2 = (residL2 + EPS) / (residL2First + EPS);

  // get indices
  const auto imx = resNormL2.MomentumXIndex();
  const auto imy = resNormL2.MomentumYIndex();
  const auto imz = resNormL2.MomentumZIndex();
  const auto ie = resNormL2.EnergyIndex();

  os << std::left << setw(12) << resMass << setw(12)
     << resNormL2[imx] << setw(12) << resNormL2[imy] << setw(12)
     << resNormL2[imz] << setw(12) << resNormL2[ie];
  if (inp.IsRANS()) {
    const auto it = resNormL2.TurbulenceIndex();
    os << std::left << setw(12) << resNormL2[it] << setw(12)
       << resNormL2[it + 1];
  }
  os.unsetf(std::ios::fixed | std::ios::scientific);
  os << std::left << setw(8) << residLinf.Eqn() << setw(8)
     << residLinf.Block() << setw(8) << residLinf.ILoc() << setw(8)
     << residLinf.JLoc() << setw(8) << residLinf.KLoc() << setw(12)
     << setprecision(4) << std::scientific << residLinf.Linf() << setw(12)
     << matrixResid << endl;

  os.unsetf(std::ios::fixed | std::ios::scientific);
}

/*Function to take in a vector of split procBlocks and return a vector of joined
 * procblocks (in their original configuration before grid decomposition).*/
vector<procBlock> Recombine(const vector<procBlock> &vars,
                            const decomposition &decomp) {
  // vars -- vector of split procBlocks
  // decomp -- decomposition

  auto recombVars = vars;
  vector<boundarySurface> dumSurf;
  for (auto ii = decomp.NumSplits() - 1; ii >= 0; ii--) {
    // recombine blocks and resize vector
    recombVars[decomp.SplitHistBlkLower(ii)]
        .Join(recombVars[decomp.SplitHistBlkUpper(ii)], decomp.SplitHistDir(ii),
              dumSurf);
    recombVars.resize(recombVars.size() - 1);
  }

  return recombVars;
}

/*Function to take in indices from the recombined procBlocks and determine which
 * split procBlock index the cell is associated with.*/
int SplitBlockNumber(const vector<procBlock> &vars, const decomposition &decomp,
                     const int &blk, const int &ii, const int &jj,
                     const int &kk) {
  // vars -- vector of recombined procblocks
  // decomp -- decomposition data structure
  // blk -- block number
  // ii -- i index of cell in recombined block to find split block number
  // jj -- j index of cell in recombined block to find split block number
  // kk -- k index of cell in recombined block to find split block number

  // Get block dimensions (both lower and upper extents)
  vector<pair<vector3d<int>, vector3d<int>>> blkDims(vars.size());
  vector3d<int> initialLower(0, 0, 0);
  for (auto bb = 0U; bb < blkDims.size(); bb++) {
    vector3d<int> dims(vars[bb].NumI(), vars[bb].NumJ(), vars[bb].NumK());
    blkDims[bb].first = initialLower;
    blkDims[bb].second = dims;
  }

  auto ind = blk;

  // no splits, cell must be in parent block already
  if (decomp.NumSplits() == 0) {
    return ind;
  } else {  // cell is in lower split already
    for (auto ss = 0; ss < decomp.NumSplits(); ss++) {  // loop over all splits
      // wrong parent block - split won't effect search so use dummy value
      if (blk != decomp.ParentBlock(ss + vars.size())) {
        pair<vector3d<int>, vector3d<int>> dumBlk(initialLower, initialLower);
        blkDims.push_back(dumBlk);
      } else {
        // "split" blocks - change lower limits of block
        if (decomp.SplitHistDir(ss) == "i") {
          pair<vector3d<int>, vector3d<int>> splitBlk =
              blkDims[decomp.SplitHistBlkLower(ss)];
          splitBlk.first[0] += decomp.SplitHistIndex(ss);
          blkDims.push_back(splitBlk);
        } else if (decomp.SplitHistDir(ss) == "j") {
          pair<vector3d<int>, vector3d<int>> splitBlk =
              blkDims[decomp.SplitHistBlkLower(ss)];
          splitBlk.first[1] += decomp.SplitHistIndex(ss);
          blkDims.push_back(splitBlk);
        } else {  // direction is k
          pair<vector3d<int>, vector3d<int>> splitBlk =
              blkDims[decomp.SplitHistBlkLower(ss)];
          splitBlk.first[2] += decomp.SplitHistIndex(ss);
          blkDims.push_back(splitBlk);
        }

        // test to see if block is in upper split
        if (!(ii <= blkDims[decomp.SplitHistBlkUpper(ss)].second.X() &&
              jj <= blkDims[decomp.SplitHistBlkUpper(ss)].second.Y() &&
              kk <= blkDims[decomp.SplitHistBlkUpper(ss)].second.Z() &&
              ii >= blkDims[decomp.SplitHistBlkUpper(ss)].first.X() &&
              jj >= blkDims[decomp.SplitHistBlkUpper(ss)].first.Y() &&
              kk >= blkDims[decomp.SplitHistBlkUpper(ss)].first.Z())) {
          // cell not in upper split, but in lower split - found block index
          return decomp.SplitHistBlkLower(ss);
        } else {  // cell in upper split (and lower split)
          ind = decomp.SplitHistBlkUpper(ss);
        }
      }
    }
  }

  return ind;  // cell was in uppermost split for given parent block
}

blkMultiArray3d<primitive> ReadSolFromRestart(
    ifstream &resFile, const input &inp, const physics &phys,
    const vector<string> &restartVars, const int &numI, const int &numJ,
    const int &numK, const int &numSpecies) {
  // intialize multiArray3d
  // -1 b/c mf and density written to file
  auto numEqns = restartVars.size() - 1;
  blkMultiArray3d<primitive> sol(numI, numJ, numK, 0, numEqns, numSpecies);

  // read the primitive variables
  // read dimensional variables -- loop over physical cells
  for (auto kk = sol.StartK(); kk < sol.EndK(); kk++) {
    for (auto jj = sol.StartJ(); jj < sol.EndJ(); jj++) {
      for (auto ii = sol.StartI(); ii < sol.EndI(); ii++) {
        primitive value(numEqns, numSpecies);
        auto rho = 0.0;
        // loop over the number of variables to read
        for (auto &var : restartVars) {
          if (var == "density") {
            resFile.read(reinterpret_cast<char *>(&rho), sizeof(rho));
            rho /= inp.RRef();
          } else if (var == "vel_x") {
            auto n = value.MomentumXIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef();
          } else if (var == "vel_y") {
            auto n = value.MomentumYIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef();
          } else if (var == "vel_z") {
            auto n = value.MomentumZIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef();
          } else if (var == "pressure") {
            auto n = value.EnergyIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.RRef() * inp.ARef() * inp.ARef();
          } else if (var == "tke") {
            auto n = value.TurbulenceIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.ARef();
          } else if (var == "sdr") {
            auto n = value.TurbulenceIndex() + 1;
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.ARef() * inp.RRef() /
                        phys.Transport()->MuRef();
          } else if (var.substr(0, 3) == "mf_" &&
                     inp.HaveSpecies(var.substr(3, string::npos))) {
            auto n = inp.SpeciesIndex(var.substr(3, string::npos));
            auto mf = 0.0;
            resFile.read(reinterpret_cast<char *>(&mf), sizeof(mf));
            value[n] = rho * mf;
          } else {
            cerr << "ERROR: Variable " << var
                 << " to read from restart file is not defined!" << endl;
            exit(EXIT_FAILURE);
          }
        }
        sol.InsertBlock(ii, jj, kk, value);
      }
    }
  }
  return sol;
}

blkMultiArray3d<conserved> ReadSolNm1FromRestart(
    ifstream &resFile, const input &inp, const physics &phys,
    const vector<string> &restartVars, const int &numI, const int &numJ,
    const int &numK, const int &numSpecies) {
  // intialize multiArray3d
  // -1 b/c mf and density written to file
  auto numEqns = restartVars.size() - 1;
  blkMultiArray3d<conserved> sol(numI, numJ, numK, 0, numEqns, numSpecies);

  // data is conserved variables
  // read dimensional variables -- loop over physical cells
  for (auto kk = sol.StartK(); kk < sol.EndK(); kk++) {
    for (auto jj = sol.StartJ(); jj < sol.EndJ(); jj++) {
      for (auto ii = sol.StartI(); ii < sol.EndI(); ii++) {
        conserved value(numEqns, numSpecies);
        auto rho = 0.0;
        // loop over the number of variables to read
        for (auto &var : restartVars) {
          if (var == "density") {
            resFile.read(reinterpret_cast<char *>(&rho), sizeof(rho));
            rho /= inp.RRef();
          } else if (var == "vel_x") {  // conserved var is rho-u
            auto n = value.MomentumXIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.RRef();
          } else if (var == "vel_y") {  // conserved var is rho-v
            auto n = value.MomentumYIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.RRef();
          } else if (var == "vel_z") {  // conserved var is rho-w
            auto n = value.MomentumZIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.RRef();
          } else if (var == "pressure") {  // conserved var is rho-E
            auto n = value.EnergyIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.RRef() * inp.ARef() * inp.ARef();
          } else if (var == "tke") {  // conserved var is rho-tke
            auto n = value.TurbulenceIndex();
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.ARef() * inp.RRef();
          } else if (var == "sdr") {  // conserved var is rho-sdr
            auto n = value.TurbulenceIndex() + 1;
            resFile.read(reinterpret_cast<char *>(&value[n]), sizeof(value[n]));
            value[n] /= inp.ARef() * inp.ARef() * inp.RRef() * inp.RRef() /
                        phys.Transport()->MuRef();
          } else if (var.substr(0, 3) == "mf_" &&
                     inp.HaveSpecies(var.substr(3, string::npos))) {
            auto n = inp.SpeciesIndex(var.substr(3, string::npos));
            auto mf = 0.0;
            resFile.read(reinterpret_cast<char *>(&mf), sizeof(mf));
            value[n] = rho * mf;
          } else {
            cerr << "ERROR: Variable " << var
                 << " to read from restart file is not defined!" << endl;
            exit(EXIT_FAILURE);
          }
        }
        sol.InsertBlock(ii, jj, kk, value);
      }
    }
  }
  return sol;
}
