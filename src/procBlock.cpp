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

#include <iostream>               // cout, cerr, endl
#include <algorithm>              // max, min
#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <map>
#include "procBlock.hpp"
#include "plot3d.hpp"              // plot3d
#include "eos.hpp"                 // equation of state
#include "transport.hpp"           // transport model
#include "thermodynamic.hpp"       // thermodynamic model
#include "inviscidFlux.hpp"        // inviscidFlux
#include "viscousFlux.hpp"         // viscousFlux
#include "input.hpp"               // inputVars
#include "turbulence.hpp"
#include "slices.hpp"
#include "source.hpp"
#include "resid.hpp"
#include "uncoupledScalar.hpp"
#include "fluxJacobian.hpp"
#include "kdtree.hpp"
#include "utility.hpp"
#include "wallData.hpp"
#include "reconstruction.hpp"
#include "spectralRadius.hpp"
#include "ghostStates.hpp"
#include "matMultiArray3d.hpp"
#include "physicsModels.hpp"
#include "output.hpp"

using std::cout;
using std::endl;
using std::cerr;
using std::vector;
using std::string;
using std::max;
using std::min;
using std::unique_ptr;
using std::ifstream;

// constructors for procBlock class
procBlock::procBlock(const plot3dBlock &blk, const int &numBlk,
                     const boundaryConditions &bound, const int &pos,
                     const int &r, const int &lpos, const input &inp) {
  // blk -- plot3d block of which this procBlock is a subset of
  // numBlk -- the block number of blk (the parent block)
  // bound -- boundary conditions for block
  // pos -- global position of block, an identifying number unique to this block
  // r -- processor rank that procBlock should be on
  // lpos -- local position of block on processor
  // inp -- input variables

  nodes_ = blk;
  numGhosts_ = inp.NumberGhostLayers();
  parBlock_ = numBlk;

  rank_ = r;
  globalPos_ = pos;
  localPos_ = lpos;

  bc_ = bound;
  for (auto ii = 0; ii < bound.NumSurfaces(); ++ii) {
    if (bound.GetBCTypes(ii) == "viscousWall") {
      const auto surf = bound.GetSurface(ii);
      const auto &bcData = inp.BCData(surf.Tag());
      wallData_.push_back(wallData(surf, bcData, inp.NumSpecies()));
    }
  }

  isViscous_ = inp.IsViscous();
  isTurbulent_ = inp.IsTurbulent();
  isRANS_ = inp.IsRANS();
  storeTimeN_ = inp.NeedToStoreTimeN();
  isMultiLevelTime_ = inp.IsMultilevelInTime();
  isMultiSpecies_ = inp.IsMultiSpecies();

  // dimensions for multiArray3d located at cell centers
  const auto numI = blk.NumI() - 1;
  const auto numJ = blk.NumJ() - 1;
  const auto numK = blk.NumK() - 1;

  // pad stored variable vectors with ghost cells
  state_ = PadWithGhosts(
      blkMultiArray3d<primitive>(numI, numJ, numK, 0, inp.NumEquations(),
                                 inp.NumSpecies(), 0.0),
      numGhosts_);
  if (storeTimeN_) {
    consVarsN_ = {numI, numJ, numK, 0, inp.NumEquations(), inp.NumSpecies(),
                  0.0};
  } else {
    consVarsN_ = {};
  }
  if (isMultiLevelTime_) {
    consVarsNm1_ = {numI, numJ, numK, 0, inp.NumEquations(), inp.NumSpecies(),
                    0.0};
  } else {
    consVarsNm1_ = {};
  }

  vol_ = PadWithGhosts(blk.Volume(), numGhosts_);
  center_ = PadWithGhosts(blk.Centroid(), numGhosts_);
  fAreaI_ = PadWithGhosts(blk.FaceAreaI(), numGhosts_);
  fAreaJ_ = PadWithGhosts(blk.FaceAreaJ(), numGhosts_);
  fAreaK_ = PadWithGhosts(blk.FaceAreaK(), numGhosts_);
  fCenterI_ = PadWithGhosts(blk.FaceCenterI(), numGhosts_);
  fCenterJ_ = PadWithGhosts(blk.FaceCenterJ(), numGhosts_);
  fCenterK_ = PadWithGhosts(blk.FaceCenterK(), numGhosts_);

  cellWidthI_ = {numI, numJ, numK, numGhosts_};
  cellWidthJ_ = {numI, numJ, numK, numGhosts_};
  cellWidthK_ = {numI, numJ, numK, numGhosts_};

  wallDist_ = {numI, numJ, numK, numGhosts_, 1, DEFAULT_WALL_DIST};

  specRadius_ = {numI, numJ, numK, 0};
  dt_ = {numI, numJ, numK, 0};
  residual_ = {numI, numJ, numK, 0, inp.NumEquations(), inp.NumSpecies(), 0.0};

  temperature_ = {numI, numJ, numK, numGhosts_, 1, 0.0};

  // gradients
  // vel grad ghosts for off diag term along interblock bc in implicit solver 
  velocityGrad_ = {numI, numJ, numK, numGhosts_};
  temperatureGrad_ = {numI, numJ, numK, 0};
  densityGrad_ = {numI, numJ, numK, 0};
  pressureGrad_ = {numI, numJ, numK, 0};

  if (isViscous_) {
    viscosity_ = {numI, numJ, numK, numGhosts_, 1, 0.0};
  } else {
    viscosity_ = {};
  }

  if (isTurbulent_) {
    eddyViscosity_ = {numI, numJ, numK, numGhosts_, 1, 0.0};
  } else {
    eddyViscosity_ = {};
  }

  if (isRANS_) {
    tkeGrad_ = {numI, numJ, numK, 0};
    omegaGrad_ = {numI, numJ, numK, 0};
    f1_ = {numI, numJ, numK, numGhosts_, 1, 1.0};
    f2_ = {numI, numJ, numK, numGhosts_, 1, 0.0};
  } else {
    tkeGrad_ = {};
    omegaGrad_ = {};
    f1_ = {};
    f2_ = {};
  }

  if (isMultiSpecies_) {
    mixtureGrad_ = {numI, numJ, numK, 0, inp.NumSpecies()};
  } else {
    mixtureGrad_ = {};
  }
}

// constructor -- allocate space for procBlock
procBlock::procBlock(const int &ni, const int &nj, const int &nk,
                     const int &numG, const int &numEqns, const int &numSpecies,
                     const bool &isViscous, const bool &isTurbulent,
                     const bool &isRANS, const bool &storeTimeN,
                     const bool &isMultiLevelInTime,
                     const bool &isMultiSpecies) {
  // ni -- i-dimension (cell)
  // nj -- j-dimension (cell)
  // nk -- k-dimension (cell)
  // numG -- number of ghost cell layers
  // isViscous -- flag to determine if solution is viscous
  // isTurbulent -- flag to determine if solution is turbulent

  numGhosts_ = numG;
  parBlock_ = 0;

  rank_ = 0;
  globalPos_ = 0;
  localPos_ = 0;

  bc_ = {};
  wallData_ = {};

  isViscous_ = isViscous;
  isTurbulent_ = isTurbulent;
  isRANS_ = isRANS;
  storeTimeN_ = storeTimeN;
  isMultiLevelTime_ = isMultiLevelInTime;
  isMultiSpecies_ = isMultiSpecies;

  // pad stored variable vectors with ghost cells
  state_ = {ni, nj, nk, numGhosts_, numEqns, numSpecies};
  if (storeTimeN) {
    consVarsN_ = {ni, nj, nk, 0, numEqns, numSpecies};
  } else {
    consVarsN_ = {};
  }
  if (isMultiLevelTime_) {
    consVarsNm1_ = {ni, nj, nk, 0, numEqns, numSpecies};
  } else {
    consVarsNm1_ = {};
  }
  nodes_ = {ni + 1, nj + 1, nk + 1};
  center_ = {ni, nj, nk, numGhosts_};
  fAreaI_ = {ni + 1, nj, nk, numGhosts_};
  fAreaJ_ = {ni, nj + 1, nk, numGhosts_};
  fAreaK_ = {ni, nj, nk + 1, numGhosts_};
  fCenterI_ = {ni + 1, nj, nk, numGhosts_};
  fCenterJ_ = {ni, nj + 1, nk, numGhosts_};
  fCenterK_ = {ni, nj, nk + 1, numGhosts_};
  residual_ = {ni, nj, nk, 0, numEqns, numSpecies};
  vol_ = {ni, nj, nk, numGhosts_};
  wallDist_ = {ni, nj, nk, numGhosts_, 1, DEFAULT_WALL_DIST};

  cellWidthI_ = {ni, nj, nk, numGhosts_};
  cellWidthJ_ = {ni, nj, nk, numGhosts_};
  cellWidthK_ = {ni, nj, nk, numGhosts_};

  specRadius_ = {ni, nj, nk, 0};
  dt_ = {ni, nj, nk, 0};

  temperature_ = {ni, nj, nk, numGhosts_};

  // gradients
  // vel grad ghosts for off diag term along interblock bc in implicit solver 
  velocityGrad_ = {ni, nj, nk, numGhosts_};
  temperatureGrad_ = {ni, nj, nk, 0};
  densityGrad_ = {ni, nj, nk, 0};
  pressureGrad_ = {ni, nj, nk, 0};

  if (isViscous_) {
    viscosity_ = {ni, nj, nk, numGhosts_};
  } else {
    viscosity_ = {};
  }

  if (isTurbulent_) {
    eddyViscosity_ = {ni, nj, nk, numGhosts_};
  } else {
    eddyViscosity_ = {};
  }

  if (isRANS_) {
    tkeGrad_ = {ni, nj, nk, 0};
    omegaGrad_ = {ni, nj, nk, 0};
    f1_ = {ni, nj, nk, numGhosts_};
    f2_ = {ni, nj, nk, numGhosts_};
  } else {
    tkeGrad_ = {};
    omegaGrad_ = {};
    f1_ = {};
    f2_ = {};
  }

  if (isMultiSpecies) {
    mixtureGrad_ = {ni, nj, nk, 0, numSpecies};
  } else {
    mixtureGrad_ = {};
  }
}

//---------------------------------------------------------------------
// function declarations
void procBlock::InitializeStates(const input &inp, const physics &phys) {
  // inp -- input variables
  // phys -- physics models

  // get initial condition state for parent block
  auto ic = inp.ICStateForBlock(parBlock_);

  if (ic.IsFromFile()) {
    // create k-d tree from cloud of points
    vector<primitive> cloudStates;
    vector<string> species;
    const auto tree = CalcTreeFromCloud(ic.File(), inp, phys.Transport(),
                                        cloudStates, species);

    auto maxDist = std::numeric_limits<double>::min();
    vector3d<double> neighbor;
    auto id = 0;
    // loop over physical cells
    for (auto kk = this->StartK(); kk < this->EndK(); kk++) {
      for (auto jj = this->StartJ(); jj < this->EndJ(); jj++) {
        for (auto ii = this->StartI(); ii < this->EndI(); ii++) {
          auto dist = tree.NearestNeighbor(center_(ii, jj, kk), neighbor, id);
          maxDist = std::max(dist, maxDist);
          state_.InsertBlock(ii, jj, kk, cloudStates[id]);
          MSG_ASSERT(state_(ii, jj, kk).Rho() > 0, "nonphysical density");
          MSG_ASSERT(state_(ii, jj, kk).P() > 0, "nonphysical pressure");
          temperature_(ii, jj, kk) = state_(ii, jj, kk).Temperature(phys.EoS());
          MSG_ASSERT(temperature_(ii, jj, kk) > 0, "nonphysical temperature");
          if (inp.IsViscous()) {
            viscosity_(ii, jj, kk) = phys.Transport()->Viscosity(
                temperature_(ii, jj, kk), state_(ii, jj, kk).MassFractions());
            if (inp.IsTurbulent()) {
              eddyViscosity_(ii, jj, kk) =
                  phys.Turbulence()->EddyViscNoLim(state_(ii, jj, kk));
            }
          }
        }
      }
    }

    cout << "Initializing parent block " << parBlock_
         << " with global position " << globalPos_ << endl;
    cout << "Maximum distance from cell center to point cloud is " << maxDist
         << endl;

  } else {
    // get nondimensional state for initialization
    primitive inputState(inp.NumEquations(), inp.NumSpecies());
    inputState.NondimensionalInitialize(phys, inp, parBlock_);
    MSG_ASSERT(inputState.Rho() > 0, "nonphysical density");
    MSG_ASSERT(inputState.P() > 0, "nonphysical pressure");

    const auto numI = this->NumI();
    const auto numJ = this->NumJ();
    const auto numK = this->NumK();

    // pad stored variable vectors with ghost cells
    state_ = PadWithGhosts(
        blkMultiArray3d<primitive>(numI, numJ, numK, 0, inputState),
        numGhosts_);

    const auto inputTemperature = inputState.Temperature(phys.EoS());
    MSG_ASSERT(inputTemperature > 0, "nonphysical temperature");
    temperature_ = {numI, numJ, numK, numGhosts_, 1, inputTemperature};

    if (isViscous_) {
      const auto inputViscosity = phys.Transport()->Viscosity(
          inputTemperature, inputState.MassFractions());
      viscosity_ = {numI, numJ, numK, numGhosts_, 1, inputViscosity};
      if (isTurbulent_) {
        eddyViscosity_ = {numI, numJ, numK, numGhosts_, 1,
                          ic.EddyViscosityRatio() * inputViscosity};
      }
    }
  }
}

/* Function to calculate the inviscid fluxes on the i-faces. All phyiscal
(non-ghost) i-faces are looped over. The left and right states are
calculated, and then the flux at the face is calculated. The flux at the
face contributes to the residual of the cells to the left and right of
the face. This contribution from the flux is added to the residuals and
the wave speed is accumulated as well.
  ___________________________
  |            |            |
  |            |            |
  |   Ui       -->   Ui+1   |
  |            |            |
  |____________|____________|
Ui-1/2       Ui+1/2       Ui+3/2

Using the above diagram, the flux is calculated at face Ui+1/2. Since the area
vector at the face always points from lower indices to higher indices it
points from Ui to Ui+1. For the residual calculation the convention is for the area
vector to point out of the cell. Therefore it is in the correct position for the
residual at Ui, but the sign needs to be flipped when adding the contribution of
the flux to the residual at Ui+1.

The spectral radius in the i-direction is also calculated. Since this is done on
a cell basis instead of a face bases, it is only calculated for the upper cell
(Ui+1 in this case). The spectral radius is added to the average wave speed
variable and is eventually used in the time step calculation if the time step
isn't explicitly specified.
*/
void procBlock::CalcInvFluxI(const physics &phys, const input &inp,
                             matMultiArray3d &mainDiagonal) {
  // phys -- physics models
  // inp -- all input variables
  // mainDiagonal -- main diagonal of LHS to store flux jacobians for implicit
  //                 solver

  // loop over all physical i-faces
  for (auto kk = fAreaI_.PhysStartK(); kk < fAreaI_.PhysEndK(); kk++) {
    for (auto jj = fAreaI_.PhysStartJ(); jj < fAreaI_.PhysEndJ(); jj++) {
      for (auto ii = fAreaI_.PhysStartI(); ii < fAreaI_.PhysEndI(); ii++) {
        primitive faceStateLower;
        primitive faceStateUpper;

        // use constant reconstruction (first order)
        if (inp.OrderOfAccuracy() == "first") {
          faceStateLower = FaceReconConst(state_(ii - 1, jj, kk));
          faceStateUpper = FaceReconConst(state_(ii, jj, kk));
        } else {  // second order accuracy
          if (inp.UsingMUSCLReconstruction()) {
            faceStateLower = FaceReconMUSCL(
                state_(ii - 2, jj, kk), state_(ii - 1, jj, kk),
                state_(ii, jj, kk), inp.Kappa(), inp.Limiter(),
                cellWidthI_(ii - 2, jj, kk), cellWidthI_(ii - 1, jj, kk),
                cellWidthI_(ii, jj, kk));

            faceStateUpper = FaceReconMUSCL(
                state_(ii + 1, jj, kk), state_(ii, jj, kk),
                state_(ii - 1, jj, kk), inp.Kappa(), inp.Limiter(),
                cellWidthI_(ii + 1, jj, kk), cellWidthI_(ii, jj, kk),
                cellWidthI_(ii - 1, jj, kk));

          } else {  // using higher order reconstruction (weno, wenoz)
            faceStateLower = FaceReconWENO(
                state_(ii - 3, jj, kk), state_(ii - 2, jj, kk),
                state_(ii - 1, jj, kk), state_(ii, jj, kk),
                state_(ii + 1, jj, kk), cellWidthI_(ii - 3, jj, kk),
                cellWidthI_(ii - 2, jj, kk), cellWidthI_(ii - 1, jj, kk),
                cellWidthI_(ii, jj, kk), cellWidthI_(ii + 1, jj, kk),
                inp.IsWenoZ());

            faceStateUpper = FaceReconWENO(
                state_(ii + 2, jj, kk), state_(ii + 1, jj, kk),
                state_(ii, jj, kk), state_(ii - 1, jj, kk),
                state_(ii - 2, jj, kk), cellWidthI_(ii + 2, jj, kk),
                cellWidthI_(ii + 1, jj, kk), cellWidthI_(ii, jj, kk),
                cellWidthI_(ii - 1, jj, kk), cellWidthI_(ii - 2, jj, kk),
                inp.IsWenoZ());
          }
        }
        MSG_ASSERT(faceStateLower.Rho() > 0.0, "nonphysical density");
        MSG_ASSERT(faceStateLower.P() > 0.0, "nonphysical pressure");
        MSG_ASSERT(faceStateUpper.Rho() > 0.0, "nonphysical density");
        MSG_ASSERT(faceStateUpper.P() > 0.0, "nonphysical pressure");

        // calculate inviscid flux at face
        const inviscidFlux tempFlux =
            InviscidFlux(faceStateLower, faceStateUpper, phys,
                         this->FAreaUnitI(ii, jj, kk), inp.InviscidFlux());

        // area vector points from left to right, so add to left cell, subtract
        // from right cell
        // at left boundary there is no left cell to add to
        if (ii > fAreaI_.PhysStartI()) {
          this->AddToResidual(ii - 1, jj, kk,
                              tempFlux * this->FAreaMagI(ii, jj, kk));

          // if using a block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            fluxJacobian fluxJac;
            fluxJac.RusanovFluxJacobian(faceStateLower, phys,
                                        this->FAreaI(ii, jj, kk), true, inp);
            mainDiagonal.Add(ii - 1, jj, kk, fluxJac);
          }
        }

        // at right boundary there is no right cell to add to
        if (ii < fAreaI_.PhysEndI() - 1) {
          this->SubtractFromResidual(ii, jj, kk,
                                     tempFlux * this->FAreaMagI(ii, jj, kk));

          // calculate component of wave speed. This is done on a cell by cell
          // basis, so only at the upper faces
          const auto invSpecRad =
              InvCellSpectralRadius(state_(ii, jj, kk), fAreaI_(ii, jj, kk),
                                    fAreaI_(ii + 1, jj, kk), phys);

          const auto turbInvSpecRad =
              isRANS_ ? phys.Turbulence()->InviscidCellSpecRad(
                            state_(ii, jj, kk), fAreaI_(ii, jj, kk),
                            fAreaI_(ii + 1, jj, kk))
                      : 0.0;

          const uncoupledScalar specRad(invSpecRad, turbInvSpecRad);
          specRadius_(ii, jj, kk) += specRad;

          // if using a block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            fluxJacobian fluxJac;
            fluxJac.RusanovFluxJacobian(faceStateUpper, phys,
                                        this->FAreaI(ii, jj, kk), false, inp);
            mainDiagonal.Subtract(ii, jj, kk, fluxJac);
          } else if (inp.IsImplicit()) {
            mainDiagonal.Add(ii, jj, kk, fluxJacobian(specRad, isRANS_));
          }
        }
      }
    }
  }
}

/* Function to calculate the inviscid fluxes on the j-faces. All physical
(non-ghost) j-faces are looped over. The left and right states are
calculated, and then the flux at the face is calculated. The flux at the
face contributes to the residual of the cells to the left and right of the
face. This contribution from the flux is added to the residuals and the wave
speed is accumulated as well.
  ___________________________
  |            |            |
  |            |            |
  |   Uj       -->   Uj+1   |
  |            |            |
  |____________|____________|
Uj-1/2       Uj+1/2       Uj+3/2

Using the above diagram, the flux is calculated at face Uj+1/2. Since the area
vector at the face always points from lower indices to higher indices it points
from Uj to Uj+1. For the residual calculation the convention is for the area
vector to point out of the cell. Therefore it is in the correct position for the
residual at Uj, but the sign needs to be flipped when adding the contribution of
the flux to the residual at Uj+1.

The spectral radius in the j-direction is also calculated. Since this is done on
a cell basis instead of a face bases, it is only calculated for the upper cell
(Uj+1 in this case). The spectral radius is added to the average wave speed
variable and is eventually used in the time step calculation if the time step
isn't explicitly specified.
*/
void procBlock::CalcInvFluxJ(const physics &phys,
                             const input &inp,
                             matMultiArray3d &mainDiagonal) {
  // physics -- physics models
  // inp -- all input variables
  // mainDiagonal -- main diagonal of LHS to store flux jacobians for implicit
  //                 solver

  // loop over all physical j-faces
  for (auto kk = fAreaJ_.PhysStartK(); kk < fAreaJ_.PhysEndK(); kk++) {
    for (auto jj = fAreaJ_.PhysStartJ(); jj < fAreaJ_.PhysEndJ(); jj++) {
      for (auto ii = fAreaJ_.PhysStartI(); ii < fAreaJ_.PhysEndI(); ii++) {
        primitive faceStateLower;
        primitive faceStateUpper;

        // use constant reconstruction (first order)
        if (inp.OrderOfAccuracy() == "first") {
          faceStateLower = FaceReconConst(state_(ii, jj - 1, kk));
          faceStateUpper = FaceReconConst(state_(ii, jj, kk));
        } else {  // second order accuracy
          if (inp.UsingMUSCLReconstruction()) {
            faceStateLower = FaceReconMUSCL(
                state_(ii, jj - 2, kk), state_(ii, jj - 1, kk),
                state_(ii, jj, kk), inp.Kappa(), inp.Limiter(),
                cellWidthJ_(ii, jj - 2, kk), cellWidthJ_(ii, jj - 1, kk),
                cellWidthJ_(ii, jj, kk));

            faceStateUpper = FaceReconMUSCL(
                state_(ii, jj + 1, kk), state_(ii, jj, kk),
                state_(ii, jj - 1, kk), inp.Kappa(), inp.Limiter(),
                cellWidthJ_(ii, jj + 1, kk), cellWidthJ_(ii, jj, kk),
                cellWidthJ_(ii, jj - 1, kk));

          } else {  // using higher order reconstruction (weno, wenoz)
            faceStateLower = FaceReconWENO(
                state_(ii, jj - 3, kk), state_(ii, jj - 2, kk),
                state_(ii, jj - 1, kk), state_(ii, jj, kk),
                state_(ii, jj + 1, kk), cellWidthJ_(ii, jj - 3, kk),
                cellWidthJ_(ii, jj - 2, kk), cellWidthJ_(ii, jj - 1, kk),
                cellWidthJ_(ii, jj, kk), cellWidthJ_(ii, jj + 1, kk),
                inp.IsWenoZ());

            faceStateUpper = FaceReconWENO(
                state_(ii, jj + 2, kk), state_(ii, jj + 1, kk),
                state_(ii, jj, kk), state_(ii, jj - 1, kk),
                state_(ii, jj - 2, kk), cellWidthJ_(ii, jj + 2, kk),
                cellWidthJ_(ii, jj + 1, kk), cellWidthJ_(ii, jj, kk),
                cellWidthJ_(ii, jj - 1, kk), cellWidthJ_(ii, jj - 2, kk),
                inp.IsWenoZ());
          }
        }
        MSG_ASSERT(faceStateLower.Rho() > 0.0, "nonphysical density");
        MSG_ASSERT(faceStateLower.P() > 0.0, "nonphysical pressure");
        MSG_ASSERT(faceStateUpper.Rho() > 0.0, "nonphysical density");
        MSG_ASSERT(faceStateUpper.P() > 0.0, "nonphysical pressure");

        // calculate inviscid flux at face
        const inviscidFlux tempFlux =
            InviscidFlux(faceStateLower, faceStateUpper, phys,
                         this->FAreaUnitJ(ii, jj, kk), inp.InviscidFlux());

        // area vector points from left to right, so add to left cell, subtract
        // from right cell
        // at left boundary no left cell to add to
        if (jj > fAreaJ_.PhysStartJ()) {
          this->AddToResidual(ii, jj - 1, kk,
                              tempFlux * this->FAreaMagJ(ii, jj, kk));

          // if using block matrix on main diagonal, calculate flux jacobian
          if (inp.IsBlockMatrix()) {
            fluxJacobian fluxJac;
            fluxJac.RusanovFluxJacobian(faceStateLower, phys,
                                        this->FAreaJ(ii, jj, kk), true, inp);
            mainDiagonal.Add(ii, jj - 1, kk, fluxJac);
          }
        }
        // at right boundary no right cell to add to
        if (jj < fAreaJ_.PhysEndJ() - 1) {
          this->SubtractFromResidual(ii, jj, kk,
                                     tempFlux * this->FAreaMagJ(ii, jj, kk));

          // calculate component of wave speed. This is done on a cell by cell
          // basis, so only at the upper faces
          const auto invSpecRad =
              InvCellSpectralRadius(state_(ii, jj, kk), fAreaJ_(ii, jj, kk),
                                    fAreaJ_(ii, jj + 1, kk), phys);

          const auto turbInvSpecRad =
              isRANS_ ? phys.Turbulence()->InviscidCellSpecRad(
                            state_(ii, jj, kk), fAreaJ_(ii, jj, kk),
                            fAreaJ_(ii, jj + 1, kk))
                      : 0.0;

          const uncoupledScalar specRad(invSpecRad, turbInvSpecRad);
          specRadius_(ii, jj, kk) += specRad;

          // if using block matrix on main diagonal, calculate flux jacobian
          if (inp.IsBlockMatrix()) {
            fluxJacobian fluxJac;
            fluxJac.RusanovFluxJacobian(faceStateUpper, phys,
                                        this->FAreaJ(ii, jj, kk), false, inp);
            mainDiagonal.Subtract(ii, jj, kk, fluxJac);
          } else if (inp.IsImplicit()) {
            mainDiagonal.Add(ii, jj, kk, fluxJacobian(specRad, isRANS_));
          }
        }
      }
    }
  }
}

/* Function to calculate the inviscid fluxes on the k-faces. All phyiscal
(non-ghost) k-faces are looped over. The left and right states are calculated,
and then the flux at the face is calculated. The flux at the face contributes
to the residual of the cells to the left and right of the face. This
contribution from the flux is added to the residuals and the wave speed is
accumulated as well.
  ___________________________
  |            |            |
  |            |            |
  |   Uk       -->   Uk+1   |
  |            |            |
  |____________|____________|
Uk-1/2       Uk+1/2       Uk+3/2

Using the above diagram, the flux is calculated at face Uk+1/2. Since the area
vector at the face always points from lower indices to higher indices it points
from Uk to Uk+1. For the residual calculation the convention is for the area
vector to point out of the cell. Therefore it is in the correct position for the
residual at Uk, but the sign needs to be flipped when adding the contribution of
the flux to the residual at Uk+1.

The spectral radius in the k-direction is also calculated. Since this is done on
a cell basis instead of a face bases, it is only calculated for the upper cell
(Uk+1 in this case). The spectral radius is added to the average wave speed
variable and is eventually used in the time step calculation if the time step
isn't explicitly specified.
*/
void procBlock::CalcInvFluxK(const physics &phys,
                             const input &inp,
                             matMultiArray3d &mainDiagonal) {
  // phys -- physics models
  // inp -- all input variables
  // mainDiagonal -- main diagonal of LHS to store flux jacobians for implicit
  //                 solver

  // loop over all physical k-faces
  for (auto kk = fAreaK_.PhysStartK(); kk < fAreaK_.PhysEndK(); kk++) {
    for (auto jj = fAreaK_.PhysStartJ(); jj < fAreaK_.PhysEndJ(); jj++) {
      for (auto ii = fAreaK_.PhysStartI(); ii < fAreaK_.PhysEndI(); ii++) {
        primitive faceStateLower;
        primitive faceStateUpper;

        // use constant reconstruction (first order)
        if (inp.OrderOfAccuracy() == "first") {
          faceStateLower = FaceReconConst(state_(ii, jj, kk - 1));
          faceStateUpper = FaceReconConst(state_(ii, jj, kk));
        } else {  // second order accuracy
          if (inp.UsingMUSCLReconstruction()) {
            faceStateLower = FaceReconMUSCL(
                state_(ii, jj, kk - 2), state_(ii, jj, kk - 1),
                state_(ii, jj, kk), inp.Kappa(), inp.Limiter(),
                cellWidthK_(ii, jj, kk - 2), cellWidthK_(ii, jj, kk - 1),
                cellWidthK_(ii, jj, kk));

            faceStateUpper = FaceReconMUSCL(
                state_(ii, jj, kk + 1), state_(ii, jj, kk),
                state_(ii, jj, kk - 1), inp.Kappa(), inp.Limiter(),
                cellWidthK_(ii, jj, kk + 1), cellWidthK_(ii, jj, kk),
                cellWidthK_(ii, jj, kk - 1));

          } else {  // using higher order reconstruction (weno, wenoz)
            faceStateLower = FaceReconWENO(
                state_(ii, jj, kk - 3), state_(ii, jj, kk - 2),
                state_(ii, jj, kk - 1), state_(ii, jj, kk),
                state_(ii, jj, kk + 1), cellWidthK_(ii, jj, kk - 3),
                cellWidthK_(ii, jj, kk - 2), cellWidthK_(ii, jj, kk - 1),
                cellWidthK_(ii, jj, kk), cellWidthK_(ii, jj, kk + 1),
                inp.IsWenoZ());

            faceStateUpper = FaceReconWENO(
                state_(ii, jj, kk + 2), state_(ii, jj, kk + 1),
                state_(ii, jj, kk), state_(ii, jj, kk - 1),
                state_(ii, jj, kk - 2), cellWidthK_(ii, jj, kk + 2),
                cellWidthK_(ii, jj, kk + 1), cellWidthK_(ii, jj, kk),
                cellWidthK_(ii, jj, kk - 1), cellWidthK_(ii, jj, kk - 2),
                inp.IsWenoZ());
          }
        }
        MSG_ASSERT(faceStateLower.Rho() > 0.0, "nonphysical density");
        MSG_ASSERT(faceStateLower.P() > 0.0, "nonphysical pressure");
        MSG_ASSERT(faceStateUpper.Rho() > 0.0, "nonphysical density");
        MSG_ASSERT(faceStateUpper.P() > 0.0, "nonphysical pressure");        

        // calculate inviscid flux at face
        const inviscidFlux tempFlux =
            InviscidFlux(faceStateLower, faceStateUpper, phys,
                         this->FAreaUnitK(ii, jj, kk), inp.InviscidFlux());

        // area vector points from left to right, so add to left cell, subtract
        // from right cell
        // at left boundary no left cell to add to
        if (kk > fAreaK_.PhysStartK()) {
          this->AddToResidual(ii, jj, kk - 1,
                              tempFlux * this->FAreaMagK(ii, jj, kk));

          // if using block matrix on main diagonal, calculate flux jacobian
          if (inp.IsBlockMatrix()) {
            fluxJacobian fluxJac;
            fluxJac.RusanovFluxJacobian(faceStateLower, phys,
                                        this->FAreaK(ii, jj, kk), true, inp);
            mainDiagonal.Add(ii, jj, kk - 1, fluxJac);
          }
        }
        // at right boundary no right cell to add to
        if (kk < fAreaK_.PhysEndK() - 1) {
          this->SubtractFromResidual(ii, jj, kk,
                                     tempFlux * this->FAreaMagK(ii, jj, kk));

          // calculate component of wave speed. This is done on a cell by cell
          // basis, so only at the upper faces
          const auto invSpecRad =
              InvCellSpectralRadius(state_(ii, jj, kk), fAreaK_(ii, jj, kk),
                                    fAreaK_(ii, jj, kk + 1), phys);

          const auto turbInvSpecRad =
              isRANS_ ? phys.Turbulence()->InviscidCellSpecRad(
                            state_(ii, jj, kk), fAreaK_(ii, jj, kk),
                            fAreaK_(ii, jj, kk + 1))
                      : 0.0;

          const uncoupledScalar specRad(invSpecRad, turbInvSpecRad);
          specRadius_(ii, jj, kk) += specRad;

          // if using block matrix on main diagonal, calculate flux jacobian
          if (inp.IsBlockMatrix()) {
            fluxJacobian fluxJac;
            fluxJac.RusanovFluxJacobian(faceStateUpper, phys,
                                        this->FAreaK(ii, jj, kk), false, inp);
            mainDiagonal.Subtract(ii, jj, kk, fluxJac);
          } else if (inp.IsImplicit()) {
            mainDiagonal.Add(ii, jj, kk, fluxJacobian(specRad, isRANS_));
          }
        }
      }
    }
  }
}

/* Member function to calculate the local time step. (i,j,k) are cell indices.
The following equation is used:

dt = CFL * V / (Lci + Lcj + Lck + C * (Lvi + Lvj + Lvk)) (Blazek 6.18)

In the above equation dt is the time step, CFL is the CFL number, V is the cell
volume, Lci, Lcj, Lck are the convective (inviscid) spectral radii in the i, j,
and k directions, C is a constant (typical value b/w 1 and 4), and Lvi, Lvj,
Lvk are the viscous spectral radi This function is only used when the time
step isn't explicitly defined by the user.
*/
void procBlock::CalcCellDt(const int &ii, const int &jj, const int &kk,
                           const double &cfl) {
  // ii -- i index of cell
  // jj -- j index of cell
  // kk -- k index of cell
  // cfl -- cfl number

  // use nondimensional time
  dt_(ii, jj, kk) = cfl * (vol_(ii, jj, kk) / specRadius_(ii, jj, kk).Max());
}

/* Member function to calculate the time step for all cells in the procBlock. If
the time step is user specified assign that time step (after
nondimensionalization) to dt variable. If time step is to be determined using CFL
number, call function to do so.
*/
void procBlock::CalcBlockTimeStep(const input &inp) {
  // inp -- all input variables

  // loop over all physical cells - no ghost cells for dt variable
  for (auto kk = 0; kk < this->NumK(); kk++) {
    for (auto jj = 0; jj < this->NumJ(); jj++) {
      for (auto ii = 0; ii < this->NumI(); ii++) {
        // dt specified, use global time stepping
        if (inp.Dt() > 0.0) {
          // nondimensional time
          dt_(ii, jj, kk) = inp.Dt() * inp.ARef() / inp.LRef();

        // cfl specified, use local time stepping
        } else if (inp.CFL() > 0.0) {
          this->CalcCellDt(ii, jj, kk, inp.CFL());
        } else {
          cerr << "ERROR: Neither dt or cfl was specified!" << endl;
          exit(EXIT_FAILURE);
        }
      }
    }
  }
}

/* Member function to update the procBlock to advance to a new time step. For
explicit methods it calls the appropriate explicit method to update. For
implicit methods it uses the correction du and calls the implicit updater.
*/
void procBlock::UpdateBlock(const input &inputVars, const physics &phys,
                            const blkMultiArray3d<varArray> &du, const int &rr,
                            residual &l2, resid &linf) {
  // inputVars -- all input variables
  // eos -- equation of state
  // thermo -- thermodynamic model
  // trans -- viscous transport model
  // du -- updates to conservative variables (only used in implicit solver)
  // turb -- turbulence model
  // rr -- nonlinear iteration number
  // l2 -- l-2 norm of residual
  // linf -- l-infinity norm of residual

  // loop over all physical cells
  for (auto kk = this->StartK(); kk < this->EndK(); kk++) {
    for (auto jj = this->StartJ(); jj < this->EndJ(); jj++) {
      for (auto ii = this->StartI(); ii < this->EndI(); ii++) {
        // explicit euler time integration
        if (inputVars.TimeIntegration() == "explicitEuler") {
          this->ExplicitEulerTimeAdvance(phys, ii, jj, kk);
        // 4-stage runge-kutta method (explicit)
        } else if (inputVars.TimeIntegration() == "rk4") {
          // advance 1 RK stage
          this->RK4TimeAdvance(consVarsN_(ii, jj, kk), phys, ii, jj, kk, rr);
        } else if (inputVars.IsImplicit()) {  // if implicit use update (du)
          this->ImplicitTimeAdvance(du(ii, jj, kk), phys, ii, jj, kk);
        } else {
          cerr << "ERROR: Time integration scheme " <<
              inputVars.TimeIntegration() << " is not recognized!" << endl;
        }

        // accumulate l2 norm of residual
        l2 += residual_(ii, jj, kk) * residual_(ii, jj, kk);

        // if any residual is larger than previous residual, a new linf
        // residual is found
        for (auto ll = 0; ll < inputVars.NumEquations(); ll++) {
          if (this->Residual(ii, jj, kk, ll) > linf.Linf()) {
            linf.UpdateMax(this->Residual(ii, jj, kk, ll),
                           parBlock_, ii, jj, kk, ll + 1);
          }
        }
      }
    }
  }
}

/* Member function to advance the state vector to time n+1 using explicit Euler
method. The following equation is used:

 Un+1 = Un - dt_/V * R

Un is the conserved variables at time n, Un+1 is the conserved variables at time
n+1, dt_ is the cell's time step, V is the cell's volume, and R is the cell's
residual.
 */
void procBlock::ExplicitEulerTimeAdvance(const physics &phys, const int &ii,
                                         const int &jj, const int &kk) {
  // phys -- physics models
  // ii -- i-location of cell
  // jj -- j-location of cell
  // kk -- k-location of cell

  // Get conserved variables for current state (time n)
  auto consVars = state_(ii, jj, kk).ConsVars(phys);
  // calculate updated conserved variables
  consVars -= dt_(ii, jj, kk) / vol_(ii, jj, kk) * residual_(ii, jj, kk);

  // calculate updated primitive variables and update state
  state_.InsertBlock(ii, jj, kk, primitive(consVars, phys));
  MSG_ASSERT(state_(ii, jj, kk).Rho() > 0, "nonphysical density");
  MSG_ASSERT(state_(ii, jj, kk).P() > 0, "nonphysical pressure");
}

// member function to advance the state vector to time n+1 (for implicit
// methods)
void procBlock::ImplicitTimeAdvance(const varArrayView &du, const physics &phys,
                                    const int &ii, const int &jj,
                                    const int &kk) {
  // du -- update for a specific cell (to move from time n to n+1)
  // phys -- physics models
  // ii -- i-location of cell
  // jj -- j-location of cell
  // kk -- k-location of cell

  // calculate updated state (primitive variables)
  state_.InsertBlock(ii, jj, kk,
                     state_(ii, jj, kk).UpdateWithConsVars(phys, du));
  MSG_ASSERT(state_(ii, jj, kk).Rho() > 0, "nonphysical density");
  MSG_ASSERT(state_(ii, jj, kk).P() > 0, "nonphysical pressure");
}

/* member function to advance the state vector to time n+1 using 4th order
(minimum storage) Runge-Kutta method (2nd order accurate)

 Un+1 = Un - dt/V * alpha * R

Un is the conserved variables at time n, Un+1 is the conserved variables at time
n+1, dt is the cell's time step, V is the cell's volume, alpha is the runge-kutta
coefficient, and R is the cell's residual.
 */
void procBlock::RK4TimeAdvance(const conservedView &currState,
                               const physics &phys, const int &ii,
                               const int &jj, const int &kk, const int &rk) {
  // currState -- current state (including steps within RK4)
  // physcs -- physics models
  // ii -- i-location of cell (including ghost cells)
  // jj -- j-location of cell (including ghost cells)
  // kk -- k-location of cell (including ghost cells)
  // rk -- runge-kutta step number

  // runge-kutta step coefficients (low storage 4 step)
  const double alpha[4] = {0.25, 1.0 / 3.0, 0.5, 1.0};

  // update conserved variables
  auto consVars = currState - dt_(ii, jj, kk) / vol_(ii, jj, kk) *
      alpha[rk] * residual_(ii, jj, kk);

  // calculate updated primitive variables
  state_.InsertBlock(ii, jj, kk, primitive(consVars, phys));
  MSG_ASSERT(state_(ii, jj, kk).Rho() > 0, "nonphysical density");
  MSG_ASSERT(state_(ii, jj, kk).P() > 0, "nonphysical pressure");
}

// member function to reset the residual and wave speed back to zero after an
// iteration. This is done because the residual and wave
// speed are accumulated over many function calls.
void procBlock::ResetResidWS() {
  residual_.Zero(0.0);
  specRadius_.Zero();
}

// member function to reset the gradients back to zero after an
// iteration. This is done because the gradients are accumulated over many
// function calls.
void procBlock::ResetGradients() {
  velocityGrad_.Zero();
  temperatureGrad_.Zero();
  densityGrad_.Zero();
  pressureGrad_.Zero();
  if (isRANS_) {
    tkeGrad_.Zero();
    omegaGrad_.Zero();
  }
}

// member function to reset the turbulence variables back to zero after an
// iteration. This is done because these variables are accumulated over many
// function calls.
void procBlock::ResetTurbVars() {
  eddyViscosity_.Zero(0.0);
  if (isRANS_) {
    f1_.Zero(0.0);
    f2_.Zero(0.0);
  }
}

/* Member function to add the cell volume divided by the cell time step to the
main diagonal of the time m minus time n term.

dU/dt = V/t * [ ((1 + zeta) * FD - zeta * BD) / ((1 + theta) * FD )] * Un = -Rn

The above equation shows the governing equations written in the Beam & Warming
format for time integration. U is the vector of conserved variables
where n represents the time step. Theta and zeta are Beam & Warming parameters,
t is the time step, V is the cell volume, and R is the residual.
FD and BD are the forward and backward difference operators respectively. These
opererators operate in the time domain. For example FD(U) =
Un+1 - Un and BD(U) = Un - Un-1. Solving the above equation for FD(Un) we get
the following:

FD(Un) = (-t * Rn - t * theta * FD(Rn) + zeta * V * FD(Un-1)) / ((1 + zeta) * V)

FD(Rn) requires us to know the residual at time n+1, but this is unknown. To
bypass this difficulty we linearize the residual_ using a Taylor series
expansion about time n. Rn+1 = Rn + J*FD(Un) where J is the flux jacobian dR/dU.
Rearranging the above equation we get the following:

[J + (1+zeta)*V/(t*theta)] * FD(Un) = -Rn/theta + zeta*V/(t*theta) * FD(Un-1)

The above equation shows that the time m minus time n term (FD(Un)) requires a
(1+zeta)V/(t*theta) term multiplied by it. That is the purpose of this
function.
*/
double procBlock::SolDeltaNCoeff(const int &ii, const int &jj, const int &kk,
                                 const input &inp) const {
  return (vol_(ii, jj, kk) * (1.0 + inp.Zeta())) / (dt_(ii, jj, kk) * inp.Theta());
}

varArray procBlock::SolDeltaMmN(const int &ii, const int &jj, const int &kk,
                                const input &inp, const physics &phys) const {
  const auto coeff = this->SolDeltaNCoeff(ii, jj, kk, inp);
  return coeff * (state_(ii, jj, kk).ConsVars(phys) - consVarsN_(ii, jj, kk));
}

double procBlock::SolDeltaNm1Coeff(const int &ii, const int &jj, const int &kk,
                                   const input &inp) const {
  return (vol_(ii, jj, kk) * inp.Zeta()) / (dt_(ii, jj, kk) * inp.Theta());
}

varArray procBlock::SolDeltaNm1(const int &ii, const int &jj, const int &kk,
                                const input &inp) const {
  if (isMultiLevelTime_) {
    const auto coeff = this->SolDeltaNm1Coeff(ii, jj, kk, inp);
    return coeff * (consVarsN_(ii, jj, kk) - consVarsNm1_(ii, jj, kk));
  } else {
    return varArray(inp.NumEquations(), inp.NumSpecies());
  }
}

// assign current solution held in state_ to time n solution held in consVarsN_
void procBlock::AssignSolToTimeN(const physics &phys) {
  // loop over physical cells
  for (auto kk = this->StartK(); kk < this->EndK(); kk++) {
    for (auto jj = this->StartJ(); jj < this->EndJ(); jj++) {
      for (auto ii = this->StartI(); ii < this->EndI(); ii++) {
        // convert state to conservative variables
        consVarsN_.InsertBlock(ii, jj, kk, state_(ii, jj, kk).ConsVars(phys));
      }
    }
  }
}

// assign current solution held in consVarsN_ to time n-1 solution held in
// consVarsNm1_
void procBlock::AssignSolToTimeNm1() {
  consVarsNm1_ = consVarsN_;
}


varArray procBlock::ImplicitLower(const int &ii, const int &jj, const int &kk,
                                  const blkMultiArray3d<varArray> &du,
                                  const physics &phys, const input &inp) const {
  // initialize term for contribution from lower triangular matrix
  varArray L(inp.NumEquations(), inp.NumSpecies());

  // if i lower diagonal cell is in physical location there is a contribution
  // from it
  if (this->IsPhysical(ii - 1, jj, kk) || bc_.BCIsConnection(ii, jj, kk, 1)) {
    // calculate projected center to center distance along face area
    const auto projDist = this->ProjC2CDist(ii, jj, kk, "i");

    // update L matrix
    L += OffDiagonal(
        state_(ii - 1, jj, kk), state_(ii, jj, kk), du(ii - 1, jj, kk),
        fAreaI_(ii, jj, kk), this->Viscosity(ii - 1, jj, kk),
        this->EddyViscosity(ii - 1, jj, kk), this->F1(ii - 1, jj, kk), projDist,
        this->VelGrad(ii - 1, jj, kk), phys, inp, true);
  }

  // if j lower diagonal cell is in physical location there is a contribution
  // from it
  if (this->IsPhysical(ii, jj - 1, kk) || bc_.BCIsConnection(ii, jj, kk, 3)) {
    // calculate projected center to center distance along face area
    const auto projDist = this->ProjC2CDist(ii, jj, kk, "j");

    // update L matrix
    L += OffDiagonal(
        state_(ii, jj - 1, kk), state_(ii, jj, kk), du(ii, jj - 1, kk),
        fAreaJ_(ii, jj, kk), this->Viscosity(ii, jj - 1, kk),
        this->EddyViscosity(ii, jj - 1, kk), this->F1(ii, jj - 1, kk), projDist,
        this->VelGrad(ii, jj - 1, kk), phys, inp, true);
  }

  // if k lower diagonal cell is in physical location there is a contribution
  // from it
  if (this->IsPhysical(ii, jj, kk - 1) || bc_.BCIsConnection(ii, jj, kk, 5)) {
    // calculate projected center to center distance along face area
    const auto projDist = this->ProjC2CDist(ii, jj, kk, "k");

    // update L matrix
    L += OffDiagonal(
        state_(ii, jj, kk - 1), state_(ii, jj, kk), du(ii, jj, kk - 1),
        fAreaK_(ii, jj, kk), this->Viscosity(ii, jj, kk - 1),
        this->EddyViscosity(ii, jj, kk - 1), this->F1(ii, jj, kk - 1), projDist,
        this->VelGrad(ii, jj, kk - 1), phys, inp, true);
  }
  return L;
}

varArray procBlock::ImplicitUpper(const int &ii, const int &jj, const int &kk,
                                  const blkMultiArray3d<varArray> &du,
                                  const physics &phys, const input &inp) const {
  // initialize term for contribution from upper/lower triangular matrix
  varArray U(inp.NumEquations(), inp.NumSpecies());

  // -----------------------------------------------------------------------
  // if i upper diagonal cell is in physical location there is a contribution
  // from it
  if (this->IsPhysical(ii + 1, jj, kk) ||
      bc_.BCIsConnection(ii + 1, jj, kk, 2)) {
    // calculate projected center to center distance along face area
    const auto projDist = this->ProjC2CDist(ii + 1, jj, kk, "i");

    // update U matrix
    U += OffDiagonal(
        state_(ii + 1, jj, kk), state_(ii, jj, kk), du(ii + 1, jj, kk),
        fAreaI_(ii + 1, jj, kk), this->Viscosity(ii + 1, jj, kk),
        this->EddyViscosity(ii + 1, jj, kk), this->F1(ii + 1, jj, kk), projDist,
        this->VelGrad(ii + 1, jj, kk), phys, inp, false);
  }

  // -----------------------------------------------------------------------
  // if j upper diagonal cell is in physical location there is a contribution
  // from it
  if (this->IsPhysical(ii, jj + 1, kk) ||
      bc_.BCIsConnection(ii, jj + 1, kk, 4)) {
    // calculate projected center to center distance along face area
    const auto projDist = this->ProjC2CDist(ii, jj + 1, kk, "j");

    // update U matrix
    U += OffDiagonal(
        state_(ii, jj + 1, kk), state_(ii, jj, kk), du(ii, jj + 1, kk),
        fAreaJ_(ii, jj + 1, kk), this->Viscosity(ii, jj + 1, kk),
        this->EddyViscosity(ii, jj + 1, kk), this->F1(ii, jj + 1, kk), projDist,
        this->VelGrad(ii, jj + 1, kk), phys, inp, false);
  }

  // -----------------------------------------------------------------------
  // if k upper diagonal cell is in physical location there is a contribution
  // from it
  if (this->IsPhysical(ii, jj, kk + 1) ||
      bc_.BCIsConnection(ii, jj, kk + 1, 6)) {
    // calculate projected center to center distance along face area
    const auto projDist = this->ProjC2CDist(ii, jj, kk + 1, "k");

    // update U matrix
    U += OffDiagonal(
        state_(ii, jj, kk + 1), state_(ii, jj, kk), du(ii, jj, kk + 1),
        fAreaK_(ii, jj, kk + 1), this->Viscosity(ii, jj, kk + 1),
        this->EddyViscosity(ii, jj, kk + 1), this->F1(ii, jj, kk + 1), projDist,
        this->VelGrad(ii, jj, kk + 1), phys, inp, false);
  }

  return U;
}

/* Function to calculate the viscous fluxes on the i-faces. All phyiscal
(non-ghost) i-faces are looped over. The left and right states are
calculated, and then the flux at the face is calculated. The flux at the
face contributes to the residual of the cells to the left and right of the
face. This contribution from the flux is added to the residuals and the wave
speed is accumulated as well.
  ___________________________
  |            |            |
  |            |            |
  |   Ui       -->   Ui+1   |
  |            |            |
  |____________|____________|
Ui-1/2       Ui+1/2       Ui+3/2

Using the above diagram, the flux is calculated at face Ui+1/2. Since the area
vector at the face always points from lower indices to higher indices it points
from Ui to Ui+1. For the residual calculation the convention is for the area
vector to point out of the cell. Therefore it is in the correct position for the
residual at Ui, but the sign needs to be flipped when adding the contribution of
the flux to the residual at Ui+1.

The spectral radius in the i-direction is also calculated. Since this is done on
a cell basis instead of a face bases, it is only calculated for the upper cell
(Ui+1 in this case). The spectral radius is added to the average wave speed
variable and is eventually used in the time step calculation if the time step
isn't explicitly specified.

The velocity and temperature gradients are calculated at each cell face by
constructing an alternative control volume centered around that face as shown
 below.
  ___________________________
  |            |            |
  |            |            |
  |   Ui,j+1   |   Ui+1,j+1 |
  |            |            |
  |_____*******|*******_____|
  |     *      |      *     |
  |     *      |      *     |
  |   Ui,j     |   Ui+1,j   |
  |     *      |      *     |
  |_____*******|*******_____|
  |            |            |
  |            |            |
  |   Ui,j-1   |   Ui+1,j-1 |
  |            |            |
  |____________|____________|

The above diagram shows a 2D schematic of how the gradient calculation is done.
In this example the gradient is being calculated at the face between cell Ui,j
and Ui+1,j. The dashes represent the grid cells, and the astrisks represent the
alternative control volume. The grid cells themselves cannot be used as the
control volume for the gradients (averaging values at adjacent cells to get
gradients at the face) because this leads to odd/even decoupling. The face areas,
volumes, and states at the center_ of the faces are needed for the alternative
control volume. The left and right sides of the alternate control volume pass
through the center_ of cells Ui,j and Ui+1,j respectively. Therefore these values
are used for the face states. The left and right face areas are calculated as the
average of the face areas of the cells that they split. For example, the left face
area would be calculated as 0.5 * (Ai+1/2,j + Ai-1/2,j). The top and bottom sides
of the alternative control volume pass through 4 cells each. Therefore the value
of the state at the face center is the average of these four states. For example
the state at the top face is calculated as 0.25 * (Ui,j + Ui+1,j + Ui,j+1 +
Ui+1,j+1). The face areas of the top and bottom sides are calculated as the
average of the 2 face areas that each one passes through. For example, the top
face area is calculated as 0.5 * (Ai,j+1/2 + Ai+1,j+1/2). In three dimensions
each gradient calculation touches the values at 10 cells (6 shown and 4 more
in/out of the page). The stencil for the gradients of all faces in a cell
touches 15 cells. The gradient calculation with this stencil uses the "edge"
ghost cells, but not the "corner" ghost cells.
*/
void procBlock::CalcViscFluxI(const physics &phys, const input &inp,
                              matMultiArray3d &mainDiagonal) {
  // phys -- physics models
  // inp -- all input variables
  // mainDiagonal -- main diagonal of LHS used to store flux jacobians for
  //                 implicit solver

  const auto viscCoeff = inp.ViscousCFLCoefficient();
  constexpr auto sixth = 1.0 / 6.0;

  // loop over all physical i-faces
  for (auto kk = fAreaI_.PhysStartK(); kk < fAreaI_.PhysEndK(); kk++) {
    for (auto jj = fAreaI_.PhysStartJ(); jj < fAreaI_.PhysEndJ(); jj++) {
      for (auto ii = fAreaI_.PhysStartI(); ii < fAreaI_.PhysEndI(); ii++) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsI(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        // declare variables needed throughout function
        primitive state(inp.NumEquations(), inp.NumSpecies());
        auto f1 = 0.0;
        auto f2 = 0.0;
        auto mu = 0.0;
        auto mut = 0.0;
        viscousFlux tempViscFlux(inp.NumEquations(), inp.NumSpecies());

        // get surface info it at boundary
        auto surfType = 0;
        if (ii == fAreaI_.PhysStartI()) {
          surfType = 1;
        } else if (ii == fAreaI_.PhysEndI() - 1) {
          surfType = 2;
        }
        const auto isBoundary = (surfType > 0) ? true : false;
        auto isWallLawBoundary = false;
        auto isLowReBoundary = false;
        auto wallDataInd = 0;

        if (isBoundary) {
          // get boundary surface information
          const auto surf = bc_.GetBCSurface(ii, jj, kk, surfType);
          if (surf.BCType() == "viscousWall") {
            wallDataInd = this->WallDataIndex(surf);
            isWallLawBoundary =
                wallData_[wallDataInd].IsWallLaw() &&
                !wallData_[wallDataInd].SwitchToLowRe(ii, jj, kk);
            isLowReBoundary = !isWallLawBoundary;
          }
        }

        if (isWallLawBoundary) {
          // wall law wall boundary
          f1 = 1.0;
          f2 = 1.0;
          mu = wallData_[wallDataInd].WallViscosity(ii, jj, kk) *
               phys.Transport()->InvNondimScaling();
          mut = wallData_[wallDataInd].WallEddyViscosity(ii, jj, kk) *
                phys.Transport()->InvNondimScaling();
          wallData_[wallDataInd].WallState(ii, jj, kk, phys.EoS(), state);
          tempViscFlux.CalcWallLawFlux(
              wallData_[wallDataInd].WallShearStress(ii, jj, kk),
              wallData_[wallDataInd].WallHeatFlux(ii, jj, kk),
              wallData_[wallDataInd].WallViscosity(ii, jj, kk),
              wallData_[wallDataInd].WallEddyViscosity(ii, jj, kk),
              wallData_[wallDataInd].WallVelocity(),
              this->FAreaUnitI(ii, jj, kk), tkeGrad, omegaGrad,
              phys.Turbulence());
        } else {  // not boundary, or low Re wall boundary
          auto wDist = 0.0;
          if (inp.ViscousFaceReconstruction() == "central") {
            // get cell widths
            const vector<double> cellWidth = {cellWidthI_(ii - 1, jj, kk),
                                              cellWidthI_(ii, jj, kk)};

            // Get state at face
            state = FaceReconCentral(state_(ii - 1, jj, kk), state_(ii, jj, kk),
                                     cellWidth);
            state.LimitTurb(phys.Turbulence());

            // Get wall distance at face
            wDist = FaceReconCentral(wallDist_(ii - 1, jj, kk),
                                     wallDist_(ii, jj, kk), cellWidth);

            // Get viscosity at face
            mu = FaceReconCentral(viscosity_(ii - 1, jj, kk),
                                  viscosity_(ii, jj, kk), cellWidth);

          } else {  // use 4th order reconstruction
            // get cell widths
            const vector<double> cellWidth = {
                cellWidthI_(ii - 2, jj, kk), cellWidthI_(ii - 1, jj, kk),
                cellWidthI_(ii, jj, kk), cellWidthI_(ii + 1, jj, kk)};

            // Get state at face
            state = FaceReconCentral4th(
                state_(ii - 2, jj, kk), state_(ii - 1, jj, kk),
                state_(ii, jj, kk), state_(ii + 1, jj, kk), cellWidth);
            state.LimitTurb(phys.Turbulence());

            // Get wall distance at face
            // Use regular central to avoid negative values
            wDist = FaceReconCentral(
                wallDist_(ii - 1, jj, kk),
                wallDist_(ii, jj, kk), {cellWidth[1], cellWidth[2]});

            // Get viscosity at face
            mu = FaceReconCentral4th(
                viscosity_(ii - 2, jj, kk), viscosity_(ii - 1, jj, kk),
                viscosity_(ii, jj, kk), viscosity_(ii + 1, jj, kk), cellWidth);
          }
          // correct wall distance if within tolerance
          if (wDist < 0.0 && wDist > WALL_DIST_NEG_TOL) {
            wDist = 0.0;
          }

          // sanity checks
          MSG_ASSERT(state.Rho() > 0.0, "nonphysical density");
          MSG_ASSERT(state.P() > 0.0, "nonphysical pressure");
          MSG_ASSERT(mu > 0.0, "nonphysical viscosity");
          MSG_ASSERT(wDist >= 0.0, "nonphysical wall distance");

          // calculate turbulent eddy viscosity and blending coefficients
          if (isTurbulent_) {
            // calculate length scale
            const auto lengthScale =
                0.5 * (cellWidthI_(ii - 1, jj, kk) + cellWidthI_(ii, jj, kk));
            phys.Turbulence()->EddyViscAndBlending(
                state, velGrad, tkeGrad, omegaGrad, mu, wDist, phys.Transport(),
                lengthScale, mut, f1, f2);
          }

          if (isLowReBoundary) {
            // calculate viscous flux
            auto wVars = tempViscFlux.CalcWallFlux(
                velGrad, phys, tempGrad, this->FAreaUnitI(ii, jj, kk), tkeGrad,
                omegaGrad, state, mu, mut, f1);
            auto y = (surfType == 1) ? wallDist_(ii, jj, kk)
                                     : wallDist_(ii - 1, jj, kk);
            wVars.yplus_ = y * wVars.frictionVelocity_ * wVars.density_ /
                           (wVars.viscosity_ + wVars.turbEddyVisc_);
            wallData_[wallDataInd](ii, jj, kk) = wVars;
          } else {
            // calculate viscous flux
            tempViscFlux.CalcFlux(velGrad, phys, tempGrad,
                                  this->FAreaUnitI(ii, jj, kk), tkeGrad,
                                  omegaGrad, mixGrad, state, mu, mut, f1);
          }
        }

        // calculate projected center to center distance
        const auto c2cDist = this->ProjC2CDist(ii, jj, kk, "i");

        // area vector points from left to right, so add to left cell, subtract
        // from right cell but viscous fluxes are subtracted from inviscid
        // fluxes, so sign is reversed
        // at left boundary there is no left cell to add to
        if (ii > fAreaI_.PhysStartI()) {
          this->SubtractFromResidual(
              ii - 1, jj, kk, tempViscFlux * this->FAreaMagI(ii, jj, kk));

          // store gradients
          velocityGrad_(ii - 1, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii - 1, jj, kk) += sixth * tempGrad;
          densityGrad_(ii - 1, jj, kk) += sixth * denGrad;
          pressureGrad_(ii - 1, jj, kk) += sixth * pressGrad;
          if (isTurbulent_) {
            eddyViscosity_(ii - 1, jj, kk) += sixth * mut;
            if (isRANS_) {
              tkeGrad_(ii - 1, jj, kk) += sixth * tkeGrad;
              omegaGrad_(ii - 1, jj, kk) += sixth * omegaGrad;
              f1_(ii - 1, jj, kk) += sixth * f1;
              f2_(ii - 1, jj, kk) += sixth * f2;
            }
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii - 1, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }

          // if using block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            // using mu, mut, and f1 at face
            fluxJacobian fluxJac;
            fluxJac.ApproxTSLJacobian(state, mu, mut, f1, phys,
                                      this->FAreaI(ii, jj, kk), c2cDist, inp,
                                      true, velGrad);
            mainDiagonal.Subtract(ii - 1, jj, kk, fluxJac);
          }
        }
        // at right boundary there is no right cell to add to
        if (ii < fAreaI_.PhysEndI() - 1) {
          this->AddToResidual(ii, jj, kk,
                              tempViscFlux * this->FAreaMagI(ii, jj, kk));

          // store gradients
          velocityGrad_(ii, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk) += sixth * tempGrad;
          densityGrad_(ii, jj, kk) += sixth * denGrad;
          pressureGrad_(ii, jj, kk) += sixth * pressGrad;
          if (isTurbulent_) {
            eddyViscosity_(ii, jj, kk) += sixth * mut;
            if (isRANS_) {
              tkeGrad_(ii, jj, kk) += sixth * tkeGrad;
              omegaGrad_(ii, jj, kk) += sixth * omegaGrad;
              f1_(ii, jj, kk) += sixth * f1;
              f2_(ii, jj, kk) += sixth * f2;
            }
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }

          // calculate component of wave speed. This is done on a cell by cell
          // basis, so only at the upper faces
          const auto viscSpecRad = ViscCellSpectralRadius(
              state_(ii, jj, kk), fAreaI_(ii, jj, kk), fAreaI_(ii + 1, jj, kk),
              phys, vol_(ii, jj, kk), viscosity_(ii, jj, kk), mut);

          const auto turbViscSpecRad =
              isRANS_ ? phys.Turbulence()->ViscCellSpecRad(
                            state_(ii, jj, kk), fAreaI_(ii, jj, kk),
                            fAreaI_(ii + 1, jj, kk), viscosity_(ii, jj, kk),
                            phys.Transport(), vol_(ii, jj, kk), mut, f1)
                      : 0.0;

          const uncoupledScalar specRad(viscSpecRad, turbViscSpecRad);
          specRadius_(ii, jj, kk) += specRad * viscCoeff;

          // if using block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            // using mu, mut, and f1 at face
            fluxJacobian fluxJac;
            fluxJac.ApproxTSLJacobian(state, mu, mut, f1, phys,
                                      this->FAreaI(ii, jj, kk), c2cDist, inp,
                                      false, velGrad);
            mainDiagonal.Add(ii, jj, kk, fluxJac);
          } else if (inp.IsImplicit()) {
            // factor 2 because visc spectral radius is not halved (Blazek 6.53)
            mainDiagonal.Add(ii, jj, kk, fluxJacobian(2.0 * specRad, isRANS_));
          }
        }
      }
    }
  }
}

/* Function to calculate the viscous fluxes on the j-faces. All phyiscal
(non-ghost) j-faces are looped over. The left and right states are calculated,
and then the flux at the face is calculated. The flux at the face contributes
to the residual of the cells to the left and right of the face. This
contribution from the flux is added to the residuals and the wave speed is
accumulated as well.
  ___________________________
  |            |            |
  |            |            |
  |   Uj       -->   Uj+1   |
  |            |            |
  |____________|____________|
Uj-1/2       Uj+1/2       Uj+3/2

Using the above diagram, the flux is calculated at face Uj+1/2. Since the area
vector at the face always points from lower indices to higher indices it points
from Uj to Uj+1. For the residual calculation the convention is for the area
vector to point out of the cell. Therefore it is in the correct position for the
residual at Uj, but the sign needs to be flipped when adding the contribution of
the flux to the residual at Uj+1.

The spectral radius in the j-direction is also calculated. Since this is done on
a cell basis instead of a face bases, it is only calculated for the upper cell
(Uj+1 in this case). The spectral radius is added to the average wave speed
variable and is eventually used in the time step calculation if the time step
isn't explicitly specified.

The velocity and temperature gradients are calculated at each cell face by
constructing an alternative control volume centered around that face as shown
below.

  ___________________________
  |            |            |
  |            |            |
  |   Ui,j+1   |   Ui+1,j+1 |
  |            |            |
  |_____*******|*******_____|
  |     *      |      *     |
  |     *      |      *     |
  |   Ui,j     |   Ui+1,j   |
  |     *      |      *     |
  |_____*******|*******_____|
  |            |            |
  |            |            |
  |   Ui,j-1   |   Ui+1,j-1 |
  |            |            |
  |____________|____________|

The above diagram shows a 2D schematic of how the gradient calculation is done.
In this example the gradient is being calculated at the face between cell Ui,j
and Ui+1,j. The dashes represent the grid cells, and the astrisks represent the
alternative control volume. The grid cells themselves cannot be used as the
control volume for the gradients (averaging values at adjacent cells to get
gradients at the face) because this leads to odd/even decoupling. The face
areas, volumes, and states at the center_ of the faces are needed for the
alternative control volume. The left and right sides of the alternate control
volume pass through the center of cells Ui,j and Ui+1,j respectively. Therefore
these values are used for the face states. The left and right face areas are
calculated as the average of the face areas of the cells that they split. For
example, the left face area would be calculated as 0.5 * (Ai+1/2,j + Ai-1/2,j).
The top and bottom sides of the alternative control volume pass through 4 cells
each. Therefore the value of the state at the face center is the average of
these four states. For example the state at the top face is calculated as 0.25 *
(Ui,j + Ui+1,j + Ui,j+1, Ui+1,j+1). The face areas of the top and bottom sides
are calculated as the average of the 2 face areas that each one passes through.
For example, the top face area is calculated as 0.5 * (Ai,j+1/2 + Ai+1,j+1/2).
In three dimensions each gradient calculation touches the values at 10 cells
(6 shown and 4 more in/out of the page). The stencil for the gradients of all
faces in a cell touches 15 cells. The gradient calculation with this stencil uses
the "edge" ghost cells, but not the "corner" ghost cells.
*/
void procBlock::CalcViscFluxJ(const physics &phys, const input &inp,
                              matMultiArray3d &mainDiagonal) {
  // phys -- physics models
  // inp -- all input variables
  // mainDiagonal -- main diagonal of LHS used to store flux jacobians for
  //                 implicit solver

  const auto viscCoeff = inp.ViscousCFLCoefficient();
  constexpr auto sixth = 1.0 / 6.0;

  // loop over all physical j-faces
  for (auto kk = fAreaJ_.PhysStartK(); kk < fAreaJ_.PhysEndK(); kk++) {
    for (auto jj = fAreaJ_.PhysStartJ(); jj < fAreaJ_.PhysEndJ(); jj++) {
      for (auto ii = fAreaJ_.PhysStartI(); ii < fAreaJ_.PhysEndI(); ii++) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsJ(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        // declare variables needed throughout function
        primitive state(inp.NumEquations(), inp.NumSpecies());
        auto f1 = 0.0;
        auto f2 = 0.0;
        auto mu = 0.0;
        auto mut = 0.0;
        viscousFlux tempViscFlux(inp.NumEquations(), inp.NumSpecies());

        // get surface info if at boundary
        auto surfType = 0;
        if (jj == fAreaJ_.PhysStartJ()) {
          surfType = 3;
        } else if (jj == fAreaJ_.PhysEndJ() - 1) {
          surfType = 4;
        }
        const auto isBoundary = (surfType > 0) ? true : false;
        auto isWallLawBoundary = false;
        auto isLowReBoundary = false;
        auto wallDataInd = 0;

        if (isBoundary) {
          // get boundary surface information
          const auto surf = bc_.GetBCSurface(ii, jj, kk, surfType);
          if (surf.BCType() == "viscousWall") {
            wallDataInd = this->WallDataIndex(surf);
            isWallLawBoundary =
                wallData_[wallDataInd].IsWallLaw() &&
                !wallData_[wallDataInd].SwitchToLowRe(ii, jj, kk);
            isLowReBoundary = !isWallLawBoundary;
          }
        }

        if (isWallLawBoundary) {
          // wall law wall boundary
          f1 = 1.0;
          f2 = 1.0;
          mu = wallData_[wallDataInd].WallViscosity(ii, jj, kk) *
               phys.Transport()->InvNondimScaling();
          mut = wallData_[wallDataInd].WallEddyViscosity(ii, jj, kk) *
                phys.Transport()->InvNondimScaling();
          wallData_[wallDataInd].WallState(ii, jj, kk, phys.EoS(), state);
          tempViscFlux.CalcWallLawFlux(
              wallData_[wallDataInd].WallShearStress(ii, jj, kk),
              wallData_[wallDataInd].WallHeatFlux(ii, jj, kk),
              wallData_[wallDataInd].WallViscosity(ii, jj, kk),
              wallData_[wallDataInd].WallEddyViscosity(ii, jj, kk),
              wallData_[wallDataInd].WallVelocity(),
              this->FAreaUnitJ(ii, jj, kk), tkeGrad, omegaGrad,
              phys.Turbulence());
        } else {  // not boundary, or low Re wall boundary
          auto wDist = 0.0;
          if (inp.ViscousFaceReconstruction() == "central") {
            // get cell widths
            const vector<double> cellWidth = {cellWidthJ_(ii, jj - 1, kk),
                                              cellWidthJ_(ii, jj, kk)};

            // Get velocity at face
            state = FaceReconCentral(state_(ii, jj - 1, kk), state_(ii, jj, kk),
                                     cellWidth);
            state.LimitTurb(phys.Turbulence());

            // Get wall distance at face
            wDist = FaceReconCentral(wallDist_(ii, jj - 1, kk),
                                     wallDist_(ii, jj, kk), cellWidth);

            // Get wall distance at face
            mu = FaceReconCentral(viscosity_(ii, jj - 1, kk),
                                  viscosity_(ii, jj, kk), cellWidth);

          } else {  // use 4th order reconstruction
            // get cell widths
            const vector<double> cellWidth = {
                cellWidthJ_(ii, jj - 2, kk), cellWidthJ_(ii, jj - 1, kk),
                cellWidthJ_(ii, jj, kk), cellWidthJ_(ii, jj + 1, kk)};

            // Get velocity at face
            state = FaceReconCentral4th(
                state_(ii, jj - 2, kk), state_(ii, jj - 1, kk),
                state_(ii, jj, kk), state_(ii, jj + 1, kk), cellWidth);
            state.LimitTurb(phys.Turbulence());

            // Get wall distance at face
            // Use regular central to avoid negative values
            wDist = FaceReconCentral(
                wallDist_(ii, jj - 1, kk),
                wallDist_(ii, jj, kk), {cellWidth[1], cellWidth[2]});

            // Get wall distance at face
            mu = FaceReconCentral4th(
                viscosity_(ii, jj - 2, kk), viscosity_(ii, jj - 1, kk),
                viscosity_(ii, jj, kk), viscosity_(ii, jj + 1, kk), cellWidth);
          }
          // correct wall distance if within tolerance
          if (wDist < 0.0 && wDist > WALL_DIST_NEG_TOL) {
            wDist = 0.0;
          }

          MSG_ASSERT(state.Rho() > 0.0, "nonphysical density");
          MSG_ASSERT(state.P() > 0.0, "nonphysical pressure");
          MSG_ASSERT(mu > 0.0, "nonphysical viscosity");
          MSG_ASSERT(wDist >= 0.0, "nonphysical wall distance");

          // calculate turbulent eddy viscosity and blending coefficients
          if (isTurbulent_) {
            // calculate length scale
            const auto lengthScale =
                0.5 * (cellWidthJ_(ii, jj - 1, kk) + cellWidthJ_(ii, jj, kk));
            phys.Turbulence()->EddyViscAndBlending(
                state, velGrad, tkeGrad, omegaGrad, mu, wDist, phys.Transport(),
                lengthScale, mut, f1, f2);
          }

          if (isLowReBoundary) {
            // calculate viscous flux
            auto wVars = tempViscFlux.CalcWallFlux(
                velGrad, phys, tempGrad, this->FAreaUnitJ(ii, jj, kk), tkeGrad,
                omegaGrad, state, mu, mut, f1);
            auto y = (surfType == 3) ? wallDist_(ii, jj, kk)
                                     : wallDist_(ii, jj - 1, kk);
            wVars.yplus_ = y * wVars.frictionVelocity_ * wVars.density_ /
                           (wVars.viscosity_ + wVars.turbEddyVisc_);
            wallData_[wallDataInd](ii, jj, kk) = wVars;
          } else {
            // calculate viscous flux
            tempViscFlux.CalcFlux(velGrad, phys, tempGrad,
                                  this->FAreaUnitJ(ii, jj, kk), tkeGrad,
                                  omegaGrad, mixGrad, state, mu, mut, f1);
          }
        }

        // calculate projected center to center distance
        const auto c2cDist = this->ProjC2CDist(ii, jj, kk, "j");


        // area vector points from left to right, so add to left cell, subtract
        // from right cell but viscous fluxes are subtracted from inviscid
        // fluxes, so sign is reversed
        // at left boundary there is no left cell to add to
        if (jj > fAreaJ_.PhysStartJ()) {
          this->SubtractFromResidual(
              ii, jj - 1, kk, tempViscFlux * this->FAreaMagJ(ii, jj, kk));

          // store gradients
          velocityGrad_(ii, jj - 1, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj - 1, kk) += sixth * tempGrad;
          densityGrad_(ii, jj - 1, kk) += sixth * denGrad;
          pressureGrad_(ii, jj - 1, kk) += sixth * pressGrad;
          if (isTurbulent_) {
            eddyViscosity_(ii, jj - 1, kk) += sixth * mut;
            if (isRANS_) {
              tkeGrad_(ii, jj - 1, kk) += sixth * tkeGrad;
              omegaGrad_(ii, jj - 1, kk) += sixth * omegaGrad;
              f1_(ii, jj - 1, kk) += sixth * f1;
              f2_(ii, jj - 1, kk) += sixth * f2;
            }
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj - 1, kk, ss) += sixth * mixGrad[ss];
            }
          }

          // if using block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            // using mu, mut, and f1 at face
            fluxJacobian fluxJac;
            fluxJac.ApproxTSLJacobian(state, mu, mut, f1, phys,
                                      this->FAreaJ(ii, jj, kk), c2cDist, inp,
                                      true, velGrad);
            mainDiagonal.Subtract(ii, jj - 1, kk, fluxJac);
          }
        }
        // at right boundary there is no right cell to add to
        if (jj < fAreaJ_.PhysEndJ() - 1) {
          this->AddToResidual(ii, jj, kk,
                              tempViscFlux * this->FAreaMagJ(ii, jj, kk));

          // store gradients
          velocityGrad_(ii, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk) += sixth * tempGrad;
          densityGrad_(ii, jj, kk) += sixth * denGrad;
          pressureGrad_(ii, jj, kk) += sixth * pressGrad;
          if (isTurbulent_) {
            eddyViscosity_(ii, jj, kk) += sixth * mut;
            if (isRANS_) {
              tkeGrad_(ii, jj, kk) += sixth * tkeGrad;
              omegaGrad_(ii, jj, kk) += sixth * omegaGrad;
              f1_(ii, jj, kk) += sixth * f1;
              f2_(ii, jj, kk) += sixth * f2;
            }
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }

          // calculate component of wave speed. This is done on a cell by cell
          // basis, so only at the upper faces
          const auto viscSpecRad = ViscCellSpectralRadius(
              state_(ii, jj, kk), fAreaJ_(ii, jj, kk), fAreaJ_(ii, jj + 1, kk),
              phys, vol_(ii, jj, kk), viscosity_(ii, jj, kk), mut);

          const auto turbViscSpecRad =
              isRANS_ ? phys.Turbulence()->ViscCellSpecRad(
                            state_(ii, jj, kk), fAreaJ_(ii, jj, kk),
                            fAreaJ_(ii, jj + 1, kk), viscosity_(ii, jj, kk),
                            phys.Transport(), vol_(ii, jj, kk), mut, f1)
                      : 0.0;

          const uncoupledScalar specRad(viscSpecRad, turbViscSpecRad);
          specRadius_(ii, jj, kk) += specRad * viscCoeff;


          // if using block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            // using mu, mut, and f1 at face
            fluxJacobian fluxJac;
            fluxJac.ApproxTSLJacobian(state, mu, mut, f1, phys,
                                      this->FAreaJ(ii, jj, kk), c2cDist, inp,
                                      false, velGrad);
            mainDiagonal.Add(ii, jj, kk, fluxJac);
          } else if (inp.IsImplicit()) {
            // factor 2 because visc spectral radius is not halved (Blazek 6.53)
            mainDiagonal.Add(ii, jj, kk, fluxJacobian(2.0 * specRad, isRANS_));
          }
        }
      }
    }
  }
}

/* Function to calculate the viscous fluxes on the k-faces. All phyiscal
(non-ghost) k-faces are looped over. The left and right states are calculated,
and then the flux at the face is calculated. The flux at the face contributes
to the residual of the cells to the left and right of the face. This
contribution from the flux is added to the residuals and the wave speed is
accumulated as well.
  ___________________________
  |            |            |
  |            |            |
  |   Uk       -->   Uk+1   |
  |            |            |
  |____________|____________|
Uk-1/2       Uk+1/2       Uk+3/2

Using the above diagram, the flux is calculated at face Uk+1/2. Since the area
vector at the face always points from lower indices to higher indices it points
from Uk to Uk+1. For the residual calculation the convention is for the area
vector to point out of the cell. Therefore it is in the correct position for the
residual at Uk, but the sign needs to be flipped when adding the contribution of
the flux to the residual at Uk+1.

The spectral radius in the k-direction is also calculated. Since this is done on
a cell basis instead of a face bases, it is only calculated for the upper cell
(Uk+1 in this case). The spectral radius is added to the average wave speed
variable and is eventually used in the time step calculation if the time step
isn't explicitly specified.

The velocity and temperature gradients are calculated at each cell face by
constructing an alternative control volume centered around that face as shown
below.
  ___________________________
  |            |            |
  |            |            |
  |   Ui,j+1   |   Ui+1,j+1 |
  |            |            |
  |_____*******|*******_____|
  |     *      |      *     |
  |     *      |      *     |
  |   Ui,j     |   Ui+1,j   |
  |     *      |      *     |
  |_____*******|*******_____|
  |            |            |
  |            |            |
  |   Ui,j-1   |   Ui+1,j-1 |
  |            |            |
  |____________|____________|

The above diagram shows a 2D schematic of how the gradient calculation is done.
In this example the gradient is being calculated at the face between cell Ui,j
and Ui+1,j. The dashes represent the grid cells, and the astrisks represent the
alternative control volume. The grid cells themselves cannot be used as the
control volume for the gradients (averaging values at adjacent cells to get
gradients at the face) because this leads to odd/even decoupling. The face
areas, volumes, and states at the center_ of the faces are needed for the
alternative control volume. The left and right sides of the alternate control
volume pass through the center_ of cells Ui,j and Ui+1,j respectively. Therefore
these values are used for the face states. The left and right face areas are
calculated as the average of the face areas of the cells that they split. For
example, the left face area would be calculated as 0.5 * (Ai+1/2,j + Ai-1/2,j).
The top and bottom sides of the alternative control volume pass through 4 cells
each. Therefore the value of the state_ at the face center_ is the average of
these four states. For example the state_ at the top face is calculated as 0.25 *
(Ui,j + Ui+1,j + Ui,j+1, Ui+1,j+1). The face areas of the top and bottom sides
are calculated as the average of the 2 face areas that each one passes through.
For example, the top face area is calculated as 0.5 * (Ai,j+1/2 + Ai+1,j+1/2).
In three dimensions each gradient calculation touches the values at 10 cells
(6 shown and 4 more in/out of the page). The stencil for the gradients of all
faces in a cell touches 15 cells. The gradient calculation with this stencil uses
the "edge" ghost cells, but not the "corner" ghost cells.
*/
void procBlock::CalcViscFluxK(const physics &phys, const input &inp,
                              matMultiArray3d &mainDiagonal) {
  // trans -- viscous transport model
  // thermo -- thermodynamic model
  // eqnState -- equation of state
  // inp -- all input variables
  // grads -- class holding gradients at face for velocity, temperature, tke,
  //          and omega
  // turb -- turbulence model
  // mainDiagonal -- main diagonal of LHS used to store flux jacobians for
  //                 implicit solver

  const auto viscCoeff = inp.ViscousCFLCoefficient();
  constexpr auto sixth = 1.0 / 6.0;

  // loop over all physical k-faces
  for (auto kk = fAreaK_.PhysStartK(); kk < fAreaK_.PhysEndK(); kk++) {
    for (auto jj = fAreaK_.PhysStartJ(); jj < fAreaK_.PhysEndJ(); jj++) {
      for (auto ii = fAreaK_.PhysStartI(); ii < fAreaK_.PhysEndI(); ii++) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsK(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        // declare variables needed throughout function
        primitive state(inp.NumEquations(), inp.NumSpecies());
        auto f1 = 0.0;
        auto f2 = 0.0;
        auto mu = 0.0;
        auto mut = 0.0;
        viscousFlux tempViscFlux(inp.NumEquations(), inp.NumSpecies());

        // get surface info if at boundary
        auto surfType = 0;
        if (kk == fAreaK_.PhysStartK()) {
          surfType = 5;
        } else if (kk == fAreaK_.PhysEndK() - 1) {
          surfType = 6;
        }
        const auto isBoundary = (surfType > 0) ? true : false;
        auto isWallLawBoundary = false;
        auto isLowReBoundary = false;
        auto wallDataInd = 0;

        if (isBoundary) {
          // get boundary surface information
          const auto surf = bc_.GetBCSurface(ii, jj, kk, surfType);
          if (surf.BCType() == "viscousWall") {
            wallDataInd = this->WallDataIndex(surf);
            isWallLawBoundary =
                wallData_[wallDataInd].IsWallLaw() &&
                !wallData_[wallDataInd].SwitchToLowRe(ii, jj, kk);
            isLowReBoundary = !isWallLawBoundary;
          }
        }

        if (isWallLawBoundary) {
          // wall law wall boundary
          f1 = 1.0;
          f2 = 1.0;
          mu = wallData_[wallDataInd].WallViscosity(ii, jj, kk) *
               phys.Transport()->InvNondimScaling();
          mut = wallData_[wallDataInd].WallEddyViscosity(ii, jj, kk) *
                phys.Transport()->InvNondimScaling();
          wallData_[wallDataInd].WallState(ii, jj, kk, phys.EoS(), state);
          tempViscFlux.CalcWallLawFlux(
              wallData_[wallDataInd].WallShearStress(ii, jj, kk),
              wallData_[wallDataInd].WallHeatFlux(ii, jj, kk),
              wallData_[wallDataInd].WallViscosity(ii, jj, kk),
              wallData_[wallDataInd].WallEddyViscosity(ii, jj, kk),
              wallData_[wallDataInd].WallVelocity(),
              this->FAreaUnitK(ii, jj, kk), tkeGrad, omegaGrad,
              phys.Turbulence());
        } else {  // not boundary, or low Re wall boundary
          auto wDist = 0.0;
          if (inp.ViscousFaceReconstruction() == "central") {
            // get cell widths
            const vector<double> cellWidth = {cellWidthK_(ii, jj, kk - 1),
                                              cellWidthK_(ii, jj, kk)};

            // Get state at face
            state = FaceReconCentral(state_(ii, jj, kk - 1), state_(ii, jj, kk),
                                     cellWidth);
            state.LimitTurb(phys.Turbulence());

            // Get wall distance at face
            wDist = FaceReconCentral(wallDist_(ii, jj, kk - 1),
                                     wallDist_(ii, jj, kk), cellWidth);

            // Get wall distance at face
            mu = FaceReconCentral(viscosity_(ii, jj, kk - 1),
                                  viscosity_(ii, jj, kk), cellWidth);

          } else {  // use 4th order reconstruction
            // get cell widths
            const vector<double> cellWidth = {
                cellWidthK_(ii, jj, kk - 2), cellWidthK_(ii, jj, kk - 1),
                cellWidthK_(ii, jj, kk), cellWidthK_(ii, jj, kk + 1)};

            // Get state at face
            state = FaceReconCentral4th(
                state_(ii, jj, kk - 2), state_(ii, jj, kk - 1),
                state_(ii, jj, kk), state_(ii, jj, kk + 1), cellWidth);
            state.LimitTurb(phys.Turbulence());

            // Get wall distance at face
            // Use regular central to avoid negative values
            wDist = FaceReconCentral(
                wallDist_(ii, jj, kk - 1),
                wallDist_(ii, jj, kk), {cellWidth[1], cellWidth[2]});

            // Get wall distance at face
            mu = FaceReconCentral4th(
                viscosity_(ii, jj, kk - 2), viscosity_(ii, jj, kk - 1),
                viscosity_(ii, jj, kk), viscosity_(ii, jj, kk + 1), cellWidth);
          }
          // correct wall distance if within tolerance
          if (wDist < 0.0 && wDist > WALL_DIST_NEG_TOL) {
            wDist = 0.0;
          }

          // sanity checks
          MSG_ASSERT(state.Rho() > 0.0, "nonphysical density");
          MSG_ASSERT(state.P() > 0.0, "nonphysical pressure");
          MSG_ASSERT(mu > 0.0, "nonphysical viscosity");
          MSG_ASSERT(wDist >= 0.0, "nonphysical wall distance");

          // calculate turbulent eddy viscosity and blending coefficients
          if (isTurbulent_) {
            // calculate length scale
            const auto lengthScale =
                0.5 * (cellWidthK_(ii, jj, kk - 1) + cellWidthK_(ii, jj, kk));
            phys.Turbulence()->EddyViscAndBlending(
                state, velGrad, tkeGrad, omegaGrad, mu, wDist, phys.Transport(),
                lengthScale, mut, f1, f2);
          }

          if (isLowReBoundary) {
            // calculate viscous flux
            auto wVars = tempViscFlux.CalcWallFlux(
                velGrad, phys, tempGrad, this->FAreaUnitK(ii, jj, kk), tkeGrad,
                omegaGrad, state, mu, mut, f1);
            auto y = (surfType == 5) ? wallDist_(ii, jj, kk)
                                     : wallDist_(ii, jj, kk - 1);
            wVars.yplus_ = y * wVars.frictionVelocity_ * wVars.density_ /
                           (wVars.viscosity_ + wVars.turbEddyVisc_);
            wallData_[wallDataInd](ii, jj, kk) = wVars;
          } else {
            // calculate viscous flux
            tempViscFlux.CalcFlux(velGrad, phys, tempGrad,
                                  this->FAreaUnitK(ii, jj, kk), tkeGrad,
                                  omegaGrad, mixGrad, state, mu, mut, f1);
          }
        }

        // calculate projected center to center distance
        const auto c2cDist = this->ProjC2CDist(ii, jj, kk, "k");


        // area vector points from left to right, so add to left cell, subtract
        // from right cell but viscous fluxes are subtracted from inviscid
        // fluxes, so sign is reversed
        // at left boundary there is no left cell to add to
        if (kk > fAreaK_.PhysStartK()) {
          this->SubtractFromResidual(
              ii, jj, kk - 1, tempViscFlux * this->FAreaMagK(ii, jj, kk));

          // store gradients
          velocityGrad_(ii, jj, kk - 1) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk - 1) += sixth * tempGrad;
          densityGrad_(ii, jj, kk - 1) += sixth * denGrad;
          pressureGrad_(ii, jj, kk - 1) += sixth * pressGrad;
          if (isTurbulent_) {
            eddyViscosity_(ii, jj, kk - 1) += sixth * mut;
            if (isRANS_) {
              tkeGrad_(ii, jj, kk - 1) += sixth * tkeGrad;
              omegaGrad_(ii, jj, kk - 1) += sixth * omegaGrad;
              f1_(ii, jj, kk - 1) += sixth * f1;
              f2_(ii, jj, kk - 1) += sixth * f2;
            }
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk - 1, ss) += sixth * mixGrad[ss];
            }
          }

          // if using block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            // using mu, mut, and f1 at face
            fluxJacobian fluxJac;
            fluxJac.ApproxTSLJacobian(state, mu, mut, f1, phys,
                                      this->FAreaK(ii, jj, kk), c2cDist, inp,
                                      true, velGrad);
            mainDiagonal.Subtract(ii, jj, kk - 1, fluxJac);
          }
        }
        // at right boundary there is no right cell to add to
        if (kk < fAreaK_.PhysEndK() - 1) {
          this->AddToResidual(ii, jj, kk,
                              tempViscFlux * this->FAreaMagK(ii, jj, kk));

          // store gradients
          velocityGrad_(ii, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk) += sixth * tempGrad;
          densityGrad_(ii, jj, kk) += sixth * denGrad;
          pressureGrad_(ii, jj, kk) += sixth * pressGrad;
          if (isTurbulent_) {
            eddyViscosity_(ii, jj, kk) += sixth * mut;
            if (isRANS_) {
              tkeGrad_(ii, jj, kk) += sixth * tkeGrad;
              omegaGrad_(ii, jj, kk) += sixth * omegaGrad;
              f1_(ii, jj, kk) += sixth * f1;
              f2_(ii, jj, kk) += sixth * f2;
            }
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }

          // calculate component of wave speed. This is done on a cell by cell
          // basis, so only at the upper faces
          const auto viscSpecRad = ViscCellSpectralRadius(
              state_(ii, jj, kk), fAreaK_(ii, jj, kk), fAreaK_(ii, jj, kk + 1),
              phys, vol_(ii, jj, kk), viscosity_(ii, jj, kk), mut);

          const auto turbViscSpecRad =
              isRANS_ ? phys.Turbulence()->ViscCellSpecRad(
                            state_(ii, jj, kk), fAreaK_(ii, jj, kk),
                            fAreaK_(ii, jj, kk + 1), viscosity_(ii, jj, kk),
                            phys.Transport(), vol_(ii, jj, kk), mut, f1)
                      : 0.0;

          const uncoupledScalar specRad(viscSpecRad, turbViscSpecRad);
          specRadius_(ii, jj, kk) += specRad * viscCoeff;

          // if using block matrix on main diagonal, accumulate flux jacobian
          if (inp.IsBlockMatrix()) {
            // using mu, mut, and f1 at face
            fluxJacobian fluxJac;
            fluxJac.ApproxTSLJacobian(state, mu, mut, f1, phys,
                                      this->FAreaK(ii, jj, kk), c2cDist, inp,
                                      false, velGrad);
            mainDiagonal.Add(ii, jj, kk, fluxJac);
          } else if (inp.IsImplicit()) {
            // factor 2 because visc spectral radius is not halved (Blazek 6.53)
            mainDiagonal.Add(ii, jj, kk, fluxJacobian(2.0 * specRad, isRANS_));
          }
        }
      }
    }
  }
}

/* Member function to assign geometric quantities such as volume, face area,
cell centroid, and face center to ghost cells. This assigns values for
regular ghost cells and "edge" ghost cells. "Corner" cells are left with no
value as they are not used.
           ____ ____ ____ ____ ____ ____ ____ ____
          | E  | E  | G2 | G2 | G2 | G2 | E  | E  |
          |____|____|____|____|____|____|____|____|
          | E  | E  | G1 | G1 | G1 | G1 | E  | E  |
          |____|____|____|____|____|____|____|____|
          | G2 | G1 | X  | X  | X  | X  | G1 | G2 |
          |____|____|____|____|____|____|____|____|
          | G2 | G1 | X  | X  | X  | X  | G1 | G2 |
          |____|____|____|____|____|____|____|____|
          | E  | E  | G1 | G1 | G1 | G1 | E  | E  |
          |____|____|____|____|____|____|____|____|
          | E  | E  | G2 | G2 | G2 | G2 | E  | E  |
          |____|____|____|____|____|____|____|____|

In the above diagram where X represents the physical cells, cells marked G
(regular ghost cells) and E ("edge" ghost cells) are assigned geometric
values. G1 represents the first layer of ghost cells and G2 represents the
second layer.
*/
void procBlock::AssignGhostCellsGeom() {
  // loop over all layers of ghost cells
  for (auto layer = 1; layer <= numGhosts_; layer++) {
    // loop over all boundary surfaces
    for (auto ii = 0; ii < bc_.NumSurfaces(); ii++) {
      // get ranges for boundary surface
      const auto r1 = bc_.RangeDir1(ii);
      const auto r2 = bc_.RangeDir2(ii);
      const auto r3 = bc_.RangeDir3(ii);

      const auto dir = bc_.Direction3(ii);
      const auto surfType = bc_.GetSurfaceType(ii);

      // 'g' indicates ghost, 'p' indicates previous, 'i' indicates interior
      auto gCell = 0, iCell = 0, pCell = 0, piCell = 0;    // indices for cells
      auto iFace = 0, piFace = 0;                          // indices for faces
      // adjust interior indices to be in physical range in case block is only a
      // couple of cells thick
      if (surfType % 2 == 0) {  // upper surface
        gCell = r3.Start() + layer - 1;
        iCell = r3.Start() - layer;
        pCell = gCell - 1;  // ghost cell for previous layer
        if (iCell < this->Start(dir)) {iCell = this->Start(dir);}
        piCell = iCell + 1;

        iFace = r3.Start() - layer;
        if (iFace < this->Start(dir)) {iFace = this->Start(dir);}
        piFace = iFace + 1;
      } else {  // lower surface
        gCell = r3.Start() - layer;
        iCell = r3.Start() + layer - 1;
        pCell = gCell + 1;  // ghost cell for previous layer
        if (iCell >= this->End(dir)) {iCell = this->End(dir) - 1;}
        piCell = iCell - 1;

        iFace = r3.Start() + layer;
        if (iFace > this->End(dir)) {iFace = this->End(dir);}
        piFace = iFace - 1;
      }

      // -----------------------------------------------------------------------
      // only supply geometry values for non interblock BCs (including periodic)
      // for interblock do nothing
      if (bc_.GetBCTypes(ii) != "interblock") {
        // assign volume for layer of ghost cells
        vol_.Insert(dir, gCell, r1, r2,
                    vol_.Slice(dir, iCell, r1, r2));

        // assign face areas for layer
        fAreaI_.Insert(dir, gCell, r1, r2,
                       fAreaI_.Slice(dir, iCell, r1, r2, "i", surfType),
                       "i", surfType);

        fAreaJ_.Insert(dir, gCell, r1, r2,
                       fAreaJ_.Slice(dir, iCell, r1, r2, "j", surfType),
                       "j", surfType);

        fAreaK_.Insert(dir, gCell, r1, r2,
                       fAreaK_.Slice(dir, iCell, r1, r2, "k", surfType),
                       "k", surfType);

        // centroid / face centers are moved interior cell width in the boundary
        // normal direction
        multiArray3d<vector3d<double>> distF2F;
        if (dir == "i") {  // i-surface, dir1 = j, dir2 = k
          distF2F = fCenterI_.Slice(piFace, r1, r2) -
              fCenterI_.Slice(iFace, r1, r2);
        } else if (dir == "j") {  // j-surface, dir1 = k, dir2 = i
          distF2F = fCenterJ_.Slice(r2, piFace, r1) -
              fCenterJ_.Slice(r2, iFace, r1);
        } else {  // k-surface, dir1 = i, dir2 = j
          distF2F = fCenterK_.Slice(r1, r2, piFace) -
              fCenterK_.Slice(r1, r2, iFace);
        }

        // for first ghost layer, use face distance because previous interior
        // cell is undefined
        const auto distC2C = (layer > 1) ? center_.Slice(dir, piCell, r1, r2) -
            center_.Slice(dir, iCell, r1, r2) : distF2F;

        // Assign cell centroid, and face centers
        fCenterI_.Insert(dir, gCell, r1, r2,
                         ((dir != "i") ? distC2C.GrowI() : distF2F) +
                         fCenterI_.Slice(dir, pCell, r1, r2, "i", surfType),
                         "i", surfType);

        fCenterJ_.Insert(dir, gCell, r1, r2,
                         ((dir != "j") ? distC2C.GrowJ() : distF2F) +
                         fCenterJ_.Slice(dir, pCell, r1, r2, "j", surfType),
                         "j", surfType);

        fCenterK_.Insert(dir, gCell, r1, r2,
                         ((dir != "k") ? distC2C.GrowK() : distF2F) +
                         fCenterK_.Slice(dir, pCell, r1, r2, "k", surfType),
                         "k", surfType);

        // assign cell centroid
        center_.Insert(dir, gCell, r1, r2,
                       center_.Slice(dir, pCell, r1, r2) + distC2C);
      }
    }
  }
}

/* Member function to assign geometric quantities such as volume, face area,
cell centroid, and face center to ghost cells located on the 12 block edges.
Assumes AssignGhostCellsGeom has already been run.

           ____ ____ ____ ____ ____ ____ ____ ____
          | G2 | G1 | X  | X  | X  | X  | X  | X  |
          |____|____|____|____|____|____|____|____|
         K| G2 | G1 | X  | X  | X  | X  | X  | X  |
         ^|____|____|____|____|____|____|____|____|
         || G2 | G1 | X  | X**| X  | X  | X  | X  |
         ||____|____|____|____|____|____|____|____|
         e| G2 | G1 | X* | X  | X  | X  | X  | X  |
          |____|____|____|____|____|____|____|____|
         1| E  | E  | G1 | G1 | G1 | G1 | G1 | G1 |
          |____|____|____|____|____|____|____|____|
         2| E  | E  | G2 | G2 | G2 | G2 | G2 | G2 |
          |____|____|____|____|____|____|____|____|
            2    1     e ----> J

In the above diagram the cells marked X represent physical cells. Cells marked
G1 and G2 represent the first and second layer of ghost cells respectively. The
cells marked E are the edge ghost cells that need to be assigned values. At each
corner location (X*) there are 4 edge ghost cells that need to be filled. The
axes on the side of the diagram indicate the coordinates of the edge ghost cells
(1, 2) as well as the coordinates of the adjacent regualar ghost cells (e).

The values at edge cell 1,1 are mirrored from the values at either of the two
ghost cells it touches at level "e". The values at edge cells 1,2 and 2,1 are
identical to the values of the ghost cells they tough at level "e". The values
at edge cell 2,2 are mirrored from either of the two directions because both will
result in the same geometric values. The values should be the same as (X**).
*/
void procBlock::AssignGhostCellsGeomEdge() {
  // loop over directions i, j, k
  for (auto dd = 0; dd < 3; dd++) {
    string dir = "";
    auto max2 = 0, max3 = 0;
    if (dd == 0) {
      dir = "i";
      max2 = this->NumJ();
      max3 = this->NumK();
    } else if (dd == 1) {
      dir = "j";
      max2 = this->NumK();
      max3 = this->NumI();
    } else {
      dir = "k";
      max2 = this->NumI();
      max3 = this->NumJ();
    }

    // direction 1 is direction of line of cells, directions 2/3 are next in
    // cyclic order
    // loop over layer2, and layer3
    for (auto layer3 = 1; layer3 <= numGhosts_; layer3++) {
      for (auto layer2 = 1; layer2 <= numGhosts_; layer2++) {
        // loop over 4 edges that run in direction 1
        for (auto cc = 0; cc < 4; cc++) {
          // cc = 0 -> dir2-l/dir3-l
          // cc = 1 -> dir2-l/dir3-u
          // cc = 2 -> dir2-u/dir3-l
          // cc = 3 -> dir2-u/dir3-u

          const auto upper2 = cc > 1;
          const auto upper3 = cc % 2 == 1;

          // cell indices (g-ghost at current layer, p-ghost at previous layer,
          // i-interior cell)
          const auto pCellD2 = upper2 ? max2 + layer2 - 2 : 1 - layer2;
          const auto gCellD2 = upper2 ? pCellD2 + 1 : pCellD2 - 1;
          const auto iCellD2 = upper2 ? max2 - layer2 : layer2 - 1;

          const auto pCellD3 = upper3 ? max3 + layer3 - 2 : 1 - layer3;
          const auto gCellD3 = upper3 ? pCellD3 + 1 : pCellD3 - 1;

          // values come from direction 2
          // assign volumes
          vol_.Insert(dir, gCellD2, gCellD3,
                      vol_.Slice(dir, iCellD2, gCellD3, true), true);

          // assign face areas
          fAreaI_.Insert(dir, gCellD2, gCellD3,
                         fAreaI_.Slice(dir, iCellD2, gCellD3, true, "i", upper2, upper3),
                         true, "i", upper2, upper3);
          fAreaJ_.Insert(dir, gCellD2, gCellD3,
                         fAreaJ_.Slice(dir, iCellD2, gCellD3, true, "j", upper2, upper3),
                         true, "j", upper2, upper3);
          fAreaK_.Insert(dir, gCellD2, gCellD3,
                         fAreaK_.Slice(dir, iCellD2, gCellD3, true, "k", upper2, upper3),
                         true, "k", upper2, upper3);

          // get distance to move centroids & face centers
          multiArray3d<vector3d<double>> distF2F;
          if (dir == "i") {  // i-line, dir2 = j
            distF2F = fCenterJ_.Slice(dir, gCellD2, pCellD3, true, "j", upper2, upper3)
                - fCenterJ_.Slice(dir, pCellD2, pCellD3, true, "j", upper2, upper3);
          } else if (dir == "j") {  // j-line, dir2 = k
            distF2F = fCenterK_.Slice(dir, gCellD2, pCellD3, true, "k", upper2, upper3)
                - fCenterK_.Slice(dir, pCellD2, pCellD3, true, "k", upper2, upper3);
          } else {  // k-line, dir2 = i
            distF2F = fCenterI_.Slice(dir, gCellD2, pCellD3, true, "i", upper2, upper3)
                - fCenterI_.Slice(dir, pCellD2, pCellD3, true, "i", upper2, upper3);
          }

          const auto distC2C = center_.Slice(dir, gCellD2, pCellD3, true) -
              center_.Slice(dir, pCellD2, pCellD3, true);

          // assign centroids
          center_.Insert(dir, gCellD2, gCellD3,
                         distC2C + center_.Slice(dir, pCellD2, gCellD3, true),
                         true);

          // assign face centers
          // use lambda to get distance to move for i, j, k face data
          // when face id matches direction 2, distF2F is used, otherwise
          // distC2C is used
          auto distI = [&dir, &distF2F, &distC2C] () {
            if (dir == "i") {
              return distC2C.GrowI();
            } else if (dir == "j") {
              return distC2C;
            } else {
              return distF2F;
            }
          };
          fCenterI_.Insert(dir, gCellD2, gCellD3, distI() +
                           fCenterI_.Slice(dir, pCellD2, gCellD3, true, "i",
                                             upper2, upper3),
                           true, "i", upper2, upper3);

          auto distJ = [&dir, &distF2F, &distC2C] () {
            if (dir == "i") {
              return distF2F;
            } else if (dir == "j") {
              return distC2C.GrowJ();
            } else {
              return distC2C;
            }
          };
          fCenterJ_.Insert(dir, gCellD2, gCellD3, distJ() +
                           fCenterJ_.Slice(dir, pCellD2, gCellD3, true, "j",
                                           upper2, upper3),
                           true, "j", upper2, upper3);

          auto distK = [&dir, &distF2F, &distC2C] () {
            if (dir == "i") {
              return distC2C;
            } else if (dir == "j") {
              return distF2F;
            } else {
              return distC2C.GrowK();
            }
          };
          fCenterK_.Insert(dir, gCellD2, gCellD3, distK() +
                           fCenterK_.Slice(dir, pCellD2, gCellD3, true, "k",
                                           upper2, upper3),
                           true, "k", upper2, upper3);
        }
      }
    }
  }
}

/* Member function to assign values for ghost cells for the inviscid flux
calculation. This function assigns values for regular ghost cells and "edge"
ghost cells. "Corner" cells are left with no value as they are not used.
           ____ ____ ____ ____ ____ ____ ____ ____
          | E  | E  | G2 | G2 | G2 | G2 | E  | E  |
          |____|____|____|____|____|____|____|____|
          | E  | E  | G1 | G1 | G1 | G1 | E  | E  |
          |____|____|____|____|____|____|____|____|
          | G2 | G1 | X  | X  | X  | X  | G1 | G2 |
          |____|____|____|____|____|____|____|____|
          | G2 | G1 | X  | X  | X  | X  | G1 | G2 |
          |____|____|____|____|____|____|____|____|
          | E  | E  | G1 | G1 | G1 | G1 | E  | E  |
          |____|____|____|____|____|____|____|____|
          | E  | E  | G2 | G2 | G2 | G2 | E  | E  |
          |____|____|____|____|____|____|____|____|

In the above diagram where X represents the physical cells, cells marked G
(regular ghost cells) and E ("edge" ghost cells) are assigned geometric
values. G1 represents the first layer of ghost cells and G2 represents the
second layer.
*/
void procBlock::AssignInviscidGhostCells(const input &inp,
                                         const physics &phys) {
  // inp -- all input variables
  // phys -- physics models

  // loop over all layers of ghost cells
  for (auto layer = 1; layer <= numGhosts_; ++layer) {
    // loop over all boundary surfaces
    for (auto ii = 0; ii < bc_.NumSurfaces(); ++ii) {
      // get ranges for boundary surface
      const auto r1 = bc_.RangeDir1(ii);
      const auto r2 = bc_.RangeDir2(ii);
      const auto r3 = bc_.RangeDir3(ii);

      const auto dir = bc_.Direction3(ii);
      const auto surf = bc_.GetSurface(ii);
      const auto surfType = surf.SurfaceType();

      auto gCell = 0, iCell = 0, aCell = 0;           // indices for cells
      auto bnd = 0;                                   // indices for faces
      // adjust interior indices to be in physical range in case block is only a
      // couple of cells thick
      if (surfType % 2 == 0) {  // upper surface
        gCell = r3.Start() + layer - 1;
        iCell = r3.Start() - layer;
        aCell = r3.Start() - 1;  // adjacent cell to bnd regardless of ghost layer
        if (iCell < this->Start(dir)) {iCell = this->Start(dir);}

        bnd = r3.Start();
      } else {  // lower surface
        gCell = r3.Start() - layer;
        iCell = r3.Start() + layer - 1;
        aCell = r3.Start();  // adjacent cell to bnd regardless of ghost layer
        if (iCell >= this->End(dir)) {iCell = this->End(dir) - 1;}

        bnd = r3.Start();
      }

      // -----------------------------------------------------------------------
      // only supply cell values for non connection BCs
      // for connection do nothing
      if (!bc_.IsConnection(ii)) {
        // get boundary condtion type (no viscous walls in invicid BCs)
        auto bcName = bc_.GetBCTypes(ii);
        if (bcName == "viscousWall") {bcName = "slipWall";}

        // get face areas on boundary
        multiArray3d<unitVec3dMag<double>> faceAreas;
        if (dir == "i") {  // i-surface, dir1 = j, dir2 = k
          faceAreas = this->fAreaI_.Slice(bnd, r1, r2);
        } else if (dir == "j") {  // j-surface, dir1 = k, dir2 = i
          faceAreas = this->fAreaJ_.Slice(r2, bnd, r1);
        } else {  // k-surface, dir1 = i, dir2 = j
          faceAreas = this->fAreaK_.Slice(r1, r2, bnd);
        }

        const auto wDist = wallDist_.Slice(dir, aCell, r1, r2);
        const auto dt = dt_.Slice(dir, aCell, r1, r2);
        // nu wall not used for inviscid BCs
        const multiArray3d<double> nuW;
        // get boundary state at time n
        const auto consVarsN = consVarsN_.IsEmpty()
                                   ? consVarsN_
                                   : consVarsN_.Slice(dir, aCell, r1, r2);
        // get gradients at time n
        const auto pGrad = pressureGrad_.Slice(dir, aCell, r1, r2);
        const auto velGrad = velocityGrad_.Slice(dir, aCell, r1, r2);

        // if slipWall reflect interior state instead of extrapolation
        const auto boundaryStates = (bcName == "slipWall") ?
            state_.Slice(dir, iCell, r1, r2) : state_.Slice(dir, aCell, r1, r2);

        const auto ghostStates = this->GetGhostStates(
            boundaryStates, bcName, faceAreas, wDist, surf, inp, phys, layer,
            nuW, dt, consVarsN, pGrad, velGrad);

        state_.Insert(dir, gCell, r1, r2, ghostStates);
      }
    }
  }
}

/* Member function to assign values to ghost cells located on the 12 block edges
for the inviscid flux calculation. Assumes AssignInviscidGhostCells has already
been run.
           ____ ____ ____ ____ ____ ____ ____ ____
          | G2 | G1 | X  | X  | X  | X  | X  | X  |
          |____|____|____|____|____|____|____|____|
         K| G2 | G1 | X  | X  | X  | X  | X  | X  |
         ^|____|____|____|____|____|____|____|____|
         || G2 | G1 | X  | X  | X  | X  | X  | X  |
         ||____|____|____|____|____|____|____|____|
         e| G2 | G1 | X* | X  | X  | X  | X  | X  |
          |____|____|____|____|____|____|____|____|
         1| E  | E  | G1 | G1 | G1 | G1 | G1 | G1 |
          |____|____|____|____|____|____|____|____|
         2| E  | E  | G2 | G2 | G2 | G2 | G2 | G2 |
          |____|____|____|____|____|____|____|____|
            2    1     e ----> J

In the above diagram the cells marked X represent physical cells. Cells marked
G1 and G2 represent the first and second layer of ghost cells respectively. The
cells marked E are the edge ghost cells that need to be assigned values. At each
corner location (X*) there are 4 edge ghost cells that need to be filled. The
axes on the side of the diagram indicate the coordinates of the edge ghost cells
(1, 2) as well as the coordinates of the adjacent regualar ghost cells (e).

The values at edge cell 1,1 are the average of the values at the two ghost cells
it touches at level "e". The values at edge cells 1,2 and 2,1 are identical to
the values of the ghost cells they tough at level "e". The values at edge cell
2,2 are the average of the values at the two (1,2 & 2,1) edge ghost cells it
touches. The exception to this rule occurs when either of the boundaries that meet
at the corner are wall boundaries (slipWall, viscousWall) and the other is not.
When this occurs the wall boundaries are "extended" into the ghost cells. This
implementation is described in Blazek.
*/
void procBlock::AssignInviscidGhostCellsEdge(
    const input &inp, const physics &phys) {
  // inp -- all input variables
  // phys -- physics models

  // loop over directions i, j, k
  for (auto dd = 0; dd < 3; dd++) {
    string dir = "";
    auto max1 = 0, max2 = 0, max3 = 0, surfStart2 = 0, surfStart3 = 0;
    if (dd == 0) {
      dir = "i";
      max1 = this->NumI();
      max2 = this->NumJ();
      max3 = this->NumK();
      surfStart2 = 3;
      surfStart3 = 5;
    } else if (dd == 1) {
      dir = "j";
      max1 = this->NumJ();
      max2 = this->NumK();
      max3 = this->NumI();
      surfStart2 = 5;
      surfStart3 = 1;
    } else {
      dir = "k";
      max1 = this->NumK();
      max2 = this->NumI();
      max3 = this->NumJ();
      surfStart2 = 1;
      surfStart3 = 3;
    }

    // direction 1 is direction of line of cells, directions 2/3 are next in
    // cyclic order
    // loop over layer2, and layer3
    for (auto layer3 = 1; layer3 <= numGhosts_; layer3++) {
      for (auto layer2 = 1; layer2 <= numGhosts_; layer2++) {
        // loop over 4 edges that run in direction 1
        for (auto cc = 0; cc < 4; cc++) {
          // cc = 0 -> dir2-l/dir3-l
          // cc = 1 -> dir2-l/dir3-u
          // cc = 2 -> dir2-u/dir3-l
          // cc = 3 -> dir2-u/dir3-u

          const auto upper2 = cc > 1;
          const auto upper3 = cc % 2 == 1;

          // cell indices (g-ghost at current layer, p-ghost at previous layer)
          const auto pCellD2 = upper2 ? max2 + layer2 - 2 : 1 - layer2;
          const auto gCellD2 = upper2 ? pCellD2 + 1 : pCellD2 - 1;

          const auto pCellD3 = upper3 ? max3 + layer3 - 2 : 1 - layer3;
          const auto gCellD3 = upper3 ? pCellD3 + 1 : pCellD3 - 1;

          // surface types of surfaces forming edge
          const auto surf2 = upper2 ? surfStart2 + 1 : surfStart2;
          const auto surf3 = upper3 ? surfStart3 + 1 : surfStart3;

          // face indices at corner
          // these only change from cell indices for upper edges
          // use these to access face quantities when direction 2 is same as face
          // type -- i.e. to access fAreaJ if J is direction 2
          const auto cFaceD2_2 = upper2 ? max2 : 0;
          const auto cFaceD2_3 = upper3 ? max3 - 1 : 0;
          // use these to access face quantities when direction 3 is same as face
          // type -- i.e. to access fAreaK if K is direction 3
          const auto cFaceD3_2 = upper2 ? max2 - 1: 0;
          const auto cFaceD3_3 = upper3 ? max3 : 0;

          // loop over edge
          for (auto d1 = 0; d1 < max1; d1++) {
            boundarySurface bcSurf_2, bcSurf_3;
            vector3d<double> fArea2, fArea3;
            if (dir == "i") {
              // boundary conditions at corner
              bcSurf_2 = bc_.GetBCSurface(d1, cFaceD2_2, cFaceD2_3, surf2);
              bcSurf_3 = bc_.GetBCSurface(d1, cFaceD3_2, cFaceD3_3, surf3);

              // get face area
              fArea2 = fAreaJ_(dir, d1, cFaceD2_2, gCellD3).UnitVector();
              fArea3 = fAreaK_(dir, d1, gCellD2, cFaceD3_3).UnitVector();

            } else if (dir == "j") {
              // boundary conditions at corner
              bcSurf_2 = bc_.GetBCSurface(cFaceD2_3, d1, cFaceD2_2, surf2);
              bcSurf_3 = bc_.GetBCSurface(cFaceD3_3, d1, cFaceD3_2, surf3);

              // get face area
              fArea2 = fAreaK_(dir, d1, cFaceD2_2, gCellD3).UnitVector();
              fArea3 = fAreaI_(dir, d1, gCellD2, cFaceD3_3).UnitVector();

            } else {
              // boundary conditions at corner
              bcSurf_2 = bc_.GetBCSurface(cFaceD2_2, cFaceD2_3, d1, surf2);
              bcSurf_3 = bc_.GetBCSurface(cFaceD3_2, cFaceD3_3, d1, surf3);

              // get face area
              fArea2 = fAreaI_(dir, d1, cFaceD2_2, gCellD3).UnitVector();
              fArea3 = fAreaJ_(dir, d1, gCellD2, cFaceD3_3).UnitVector();
            }

            // get bc type and tag
            auto bc_2 = bcSurf_2.BCType();
            if (bc_2 == "viscousWall") {bc_2 = "slipWall";}

            auto bc_3 = bcSurf_3.BCType();
            if (bc_3 == "viscousWall") {bc_3 = "slipWall";}

            const auto tag2 = bcSurf_2.Tag();
            const auto tag3 = bcSurf_3.Tag();

            // get wall distance
            const auto wDist2 = wallDist_(dir, d1, cFaceD3_2, gCellD3);
            const auto wDist3 = wallDist_(dir, d1, gCellD2, cFaceD2_3);

            // not used, only for calling GetGhostState
            wallVars wVars(this->NumSpecies());

            // assign states -------------------------------------------------
            if (bc_2 == "slipWall" && bc_3 != "slipWall") {
              // surface-2 is a wall, but surface-3 is not - extend wall bc
              auto ghost =
                  GetGhostState(state_(dir, d1, pCellD2, gCellD3), bc_2, fArea2,
                                wDist2, surf2, inp, tag2, phys, wVars, layer2);
              state_.InsertBlock(dir, d1, gCellD2, gCellD3, ghost);
            } else if (bc_2 != "slipWall" && bc_3 == "slipWall") {
              // surface-3 is a wall, but surface-2 is not - extend wall bc
              auto ghost =
                  GetGhostState(state_(dir, d1, gCellD2, pCellD3), bc_3, fArea3,
                                wDist3, surf3, inp, tag3, phys, wVars, layer3);
              state_.InsertBlock(dir, d1, gCellD2, gCellD3, ghost);
            } else {  // both surfaces or neither are walls - proceed as normal
              if (layer2 == layer3) {  // need to average
                auto ghost = 0.5 * (state_(dir, d1, pCellD2, gCellD3) +
                                    state_(dir, d1, gCellD2, pCellD3));
                state_.InsertBlock(dir, d1, gCellD2, gCellD3, ghost);
              } else if (layer2 > layer3) {  // values come from direction 3
                state_.InsertBlock(dir, d1, gCellD2, gCellD3,
                                   state_(dir, d1, gCellD2, pCellD3));
              } else {  // values come from direction 2
                state_.InsertBlock(dir, d1, gCellD2, gCellD3,
                                   state_(dir, d1, pCellD2, gCellD3));
              }
            }
          }
        }
      }
    }
  }
}

void procBlock::AssignCornerGhostCells() {
  // assign "corner" cells - only used for cell to node interpolation
  constexpr auto third = 1.0 / 3.0;
  auto ig = state_.PhysStartI() - 1;
  auto jg = state_.PhysStartJ() - 1;
  auto kg = state_.PhysStartK() - 1;
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig + 1, jg, kg) + state_(ig, jg + 1, kg) +
                              state_(ig, jg, kg + 1)));
  ig = state_.PhysEndI();
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig - 1, jg, kg) + state_(ig, jg + 1, kg) +
                              state_(ig, jg, kg + 1)));
  jg = state_.PhysEndJ();
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig - 1, jg, kg) + state_(ig, jg - 1, kg) +
                              state_(ig, jg, kg + 1)));
  ig = state_.PhysStartI() - 1;
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig + 1, jg, kg) + state_(ig, jg - 1, kg) +
                              state_(ig, jg, kg + 1)));
  kg = state_.PhysEndK();
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig + 1, jg, kg) + state_(ig, jg - 1, kg) +
                              state_(ig, jg, kg - 1)));
  ig = state_.PhysEndI();
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig - 1, jg, kg) + state_(ig, jg - 1, kg) +
                              state_(ig, jg, kg - 1)));
  jg = state_.PhysStartJ() - 1;
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig - 1, jg, kg) + state_(ig, jg + 1, kg) +
                              state_(ig, jg, kg - 1)));
  ig = state_.PhysStartI() - 1;
  state_.InsertBlock(ig, jg, kg,
                     third * (state_(ig + 1, jg, kg) + state_(ig, jg + 1, kg) +
                              state_(ig, jg, kg - 1)));
}

/* Member function to assign ghost cells for the viscous flow calculation. This
 function assumes AssignInviscidGhostCells has been run first as
 it only overwrites the ghost cells associated with the viscousWall boundary
 condition. It overwrites both regular and edge ghost cells.
*/
void procBlock::AssignViscousGhostCells(const input &inp,
                                        const physics &phys) {
  // inp -- all input variables
  // phys -- physics models

  // loop over all layers of ghost cells
  for (auto layer = 1; layer <= numGhosts_; layer++) {
    // loop over all boundary surfaces
    for (auto ii = 0; ii < bc_.NumSurfaces(); ii++) {
      // get ranges for boundary surface
      const auto r1 = bc_.RangeDir1(ii);
      const auto r2 = bc_.RangeDir2(ii);
      const auto r3 = bc_.RangeDir3(ii);

      const auto dir = bc_.Direction3(ii);
      const auto surf = bc_.GetSurface(ii);
      const auto surfType = surf.SurfaceType();

      auto gCell = 0, iCell = 0, aCell = 0;           // indices for cells
      auto bnd = 0;                                   // indices for faces
      // adjust interior indices to be in physical range in case block is only a
      // couple of cells thick
      if (surfType % 2 == 0) {  // upper surface
        gCell = r3.Start() + layer - 1;
        iCell = r3.Start() - layer;
        aCell = r3.Start() - 1;  // adjacent cell to bnd regardless of ghost layer
        if (iCell < this->Start(dir)) iCell = this->Start(dir);

        bnd = r3.Start();
      } else {  // lower surface
        gCell = r3.Start() - layer;
        iCell = r3.Start() + layer - 1;
        aCell = r3.Start();  // adjacent cell to bnd regardless of ghost layer
        if (iCell >= this->End(dir)) iCell = this->End(dir) - 1;

        bnd = r3.Start();
      }

      // -----------------------------------------------------------------------
      // only overwrite cell values for viscous walls
      if (bc_.GetBCTypes(ii) == "viscousWall") {
        const string bcName = "viscousWall";

        // get face areas on boundary
        multiArray3d<unitVec3dMag<double>> faceAreas;
        if (dir == "i") {  // i-surface, dir1 = j, dir2 = k --------------------
          faceAreas = this->fAreaI_.Slice(bnd, r1, r2);
        } else if (dir == "j") {  // j-surface, dir1 = k, dir2 = i -------------
          faceAreas = this->fAreaJ_.Slice(r2, bnd, r1);
        } else {  // k-surface, dir1 = i, dir2 = j -----------------------------
          faceAreas = this->fAreaK_.Slice(r1, r2, bnd);
        }

        const auto wDist = wallDist_.Slice(dir, aCell, r1, r2);
        auto nuW = viscosity_.Slice(dir, aCell, r1, r2);
        const auto adjacentStates = state_.Slice(dir, aCell, r1, r2);
        for (auto kk = nuW.StartK(); kk < nuW.EndK(); ++kk) {
          for (auto jj = nuW.StartJ(); jj < nuW.EndJ(); ++jj) {
            for (auto ii = nuW.StartI(); ii < nuW.EndI(); ++ii) {
              nuW(ii, jj, kk) /= adjacentStates(ii, jj, kk).Rho();
            }
          }
        }

        // get interior boundary states and ghost states
        const auto boundaryStates = state_.Slice(dir, iCell, r1, r2);
        const auto ghostStates =
            this->GetGhostStates(boundaryStates, bcName, faceAreas, wDist, surf,
                                 inp, phys, layer, nuW);

        state_.Insert(dir, gCell, r1, r2, ghostStates);
      }
    }
  }
  // Assign edge ghost cells
  this->AssignViscousGhostCellsEdge(inp, phys);
}


/* Member function to assign values to ghost cells located on the 12 block edges
for the viscous flux calculation. Assumes AssignViscousGhostCells has already
been run. Only overwrites edge ghost cells if one of the boundaries at the corner
is a viscousWall boundary condition.
           ____ ____ ____ ____ ____ ____ ____ ____
          | G2 | G1 | X  | X  | X  | X  | X  | X  |
          |____|____|____|____|____|____|____|____|
         K| G2 | G1 | X  | X  | X  | X  | X  | X  |
         ^|____|____|____|____|____|____|____|____|
         || G2 | G1 | X  | X  | X  | X  | X  | X  |
         ||____|____|____|____|____|____|____|____|
         e| G2 | G1 | X* | X  | X  | X  | X  | X  |
          |____|____|____|____|____|____|____|____|
         1| E  | E  | G1 | G1 | G1 | G1 | G1 | G1 |
          |____|____|____|____|____|____|____|____|
         2| E  | E  | G2 | G2 | G2 | G2 | G2 | G2 |
          |____|____|____|____|____|____|____|____|
            2    1     e ----> J

In the above diagram the cells marked X represent physical cells. Cells marked
G1 and G2 represent the first and second layer of ghost cells respectively. The
cells marked E are the edge ghost cells that need to be assigned values. At each
corner location (X*) there are 4 edge ghost cells that need to be filled. The
axes on the side of the diagram indicate the coordinates of the edge ghost cells
(1, 2) as well as the coordinates of the adjacent regualar ghost cells (e).

The values at edge cell 1,1 are the average of the values at the two ghost cells
it touches at level "e". The values at edge cells 1,2 and 2,1 are identical to
the values of the ghost cells they tough at level "e". The values at edge cell
2,2 are the average of the values at the two (1,2 & 2,1) edge ghost cells it
touches. The exception to this rule occurs when either of the boundaries that
meet at the corner are viscousWall boundaries and the other is not. When this
occurs the viscousWall boundaries are "extended" into the ghost cells. This
implementation is described in Blazek.
*/
void procBlock::AssignViscousGhostCellsEdge(const input &inp,
                                            const physics &phys) {
  // inp -- all input variables
  // phys -- physics models

  // loop over directions i, j, k
  for (auto dd = 0; dd < 3; dd++) {
    string dir = "";
    auto max1 = 0, max2 = 0, max3 = 0, surfStart2 = 0, surfStart3 = 0;
    if (dd == 0) {
      dir = "i";
      max1 = this->NumI();
      max2 = this->NumJ();
      max3 = this->NumK();
      surfStart2 = 3;
      surfStart3 = 5;
    } else if (dd == 1) {
      dir = "j";
      max1 = this->NumJ();
      max2 = this->NumK();
      max3 = this->NumI();
      surfStart2 = 5;
      surfStart3 = 1;
    } else {
      dir = "k";
      max1 = this->NumK();
      max2 = this->NumI();
      max3 = this->NumJ();
      surfStart2 = 1;
      surfStart3 = 3;
    }

    // direction 1 is direction of line of cells, directions 2/3 are next in
    // cyclic order
    // loop over layer2, and layer3
    for (auto layer3 = 1; layer3 <= numGhosts_; layer3++) {
      for (auto layer2 = 1; layer2 <= numGhosts_; layer2++) {
        // loop over 4 edges that run in direction 1
        for (auto cc = 0; cc < 4; cc++) {
          // cc = 0 -> dir2-l/dir3-l
          // cc = 1 -> dir2-l/dir3-u
          // cc = 2 -> dir2-u/dir3-l
          // cc = 3 -> dir2-u/dir3-u

          const auto upper2 = cc > 1;
          const auto upper3 = cc % 2 == 1;

          // cell indices (g-ghost at current layer, p-ghost at previous layer)
          const auto pCellD2 = upper2 ? max2 + layer2 - 2 : 1 - layer2;
          const auto gCellD2 = upper2 ? pCellD2 + 1 : pCellD2 - 1;

          const auto pCellD3 = upper3 ? max3 + layer3 - 2 : 1 - layer3;
          const auto gCellD3 = upper3 ? pCellD3 + 1 : pCellD3 - 1;

          // surface types of surfaces forming edge
          const auto surf2 = upper2 ? surfStart2 + 1 : surfStart2;
          const auto surf3 = upper3 ? surfStart3 + 1 : surfStart3;

          // face indices at corner
          // these only change from cell indices for upper edges
          // use these to access face quantities when direction 2 is same as face
          // type -- i.e. to access fAreaJ if J is direction 2
          const auto cFaceD2_2 = upper2 ? max2 : 0;
          const auto cFaceD2_3 = upper3 ? max3 - 1 : 0;
          // use these to access face quantities when direction 3 is same as face
          // type -- i.e. to access fAreaK if K is direction 3
          const auto cFaceD3_2 = upper2 ? max2 - 1: 0;
          const auto cFaceD3_3 = upper3 ? max3 : 0;

          // loop over edge
          for (auto d1 = 0; d1 < max1; d1++) {
            boundarySurface bcSurf_2, bcSurf_3;
            vector3d<double> fArea2, fArea3;
            if (dir == "i") {
              // boundary conditions at corner
              bcSurf_2 = bc_.GetBCSurface(d1, cFaceD2_2, cFaceD2_3, surf2);
              bcSurf_3 = bc_.GetBCSurface(d1, cFaceD3_2, cFaceD3_3, surf3);

              // get face area
              fArea2 = fAreaJ_(dir, d1, cFaceD2_2, gCellD3).UnitVector();
              fArea3 = fAreaK_(dir, d1, gCellD2, cFaceD3_3).UnitVector();

            } else if (dir == "j") {
              // boundary conditions at corner
              bcSurf_2 = bc_.GetBCSurface(cFaceD2_3, d1, cFaceD2_2, surf2);
              bcSurf_3 = bc_.GetBCSurface(cFaceD3_3, d1, cFaceD3_2, surf3);

              // get face area
              fArea2 = fAreaK_(dir, d1, cFaceD2_2, gCellD3).UnitVector();
              fArea3 = fAreaI_(dir, d1, gCellD2, cFaceD3_3).UnitVector();

            } else {
              // boundary conditions at corner
              bcSurf_2 = bc_.GetBCSurface(cFaceD2_2, cFaceD2_3, d1, surf2);
              bcSurf_3 = bc_.GetBCSurface(cFaceD3_2, cFaceD3_3, d1, surf3);

              // get face area
              fArea2 = fAreaI_(dir, d1, cFaceD2_2, gCellD3).UnitVector();
              fArea3 = fAreaJ_(dir, d1, gCellD2, cFaceD3_3).UnitVector();
            }

            // get bc type and tag
            const auto bc_2 = bcSurf_2.BCType();
            const auto bc_3 = bcSurf_3.BCType();

            const auto tag2 = bcSurf_2.Tag();
            const auto tag3 = bcSurf_3.Tag();

            // get wall distance
            const auto wDist2 = wallDist_(dir, d1, cFaceD3_2, gCellD3);
            const auto wDist3 = wallDist_(dir, d1, gCellD2, cFaceD2_3);

            // get wall adjacent viscosity, nu
            auto nuW2 = viscosity_(dir, d1, cFaceD3_2, gCellD3) /
                        state_(dir, d1, cFaceD3_2, gCellD3).Rho();

            auto nuW3 = viscosity_(dir, d1, gCellD2, cFaceD2_3) /
                        state_(dir, d1, gCellD2, cFaceD2_3).Rho();

            // not used, only for calling GetGhostState
            wallVars wVars(this->NumSpecies());

            // assign states -------------------------------------------------
            // surface-2 is a wall, but surface-3 is not - extend wall bc
            if (bc_2 == "slipWall" && bc_3 != "slipWall") {
              auto ghost = GetGhostState(state_(dir, d1, pCellD2, gCellD3),
                                         bc_2, fArea2, wDist2, surf2, inp, tag2,
                                         phys, wVars, layer2, nuW2);
              state_.InsertBlock(dir, d1, gCellD2, gCellD3, ghost);
              // surface-3 is a wall, but surface-2 is not - extend wall bc
            } else if (bc_2 != "slipWall" && bc_3 == "slipWall") {
              auto ghost = GetGhostState(state_(dir, d1, gCellD2, pCellD3),
                                         bc_3, fArea3, wDist3, surf3, inp, tag3,
                                         phys, wVars, layer3, nuW3);
              state_.InsertBlock(dir, d1, gCellD2, gCellD3, ghost);
              // both surfaces are walls - proceed as normal
            } else if (bc_2 == "viscousWall" && bc_3 == "viscousWall") {
              if (layer2 == layer3) {  // need to average
                auto ghost = 0.5 * (state_(dir, d1, pCellD2, gCellD3) +
                                    state_(dir, d1, gCellD2, pCellD3));
                state_.InsertBlock(dir, d1, gCellD2, gCellD3, ghost);
              } else if (layer2 > layer3) {  // values come from direction 3
                state_.InsertBlock(dir, d1, gCellD2, gCellD3,
                                   state_(dir, d1, gCellD2, pCellD3));
              } else {  // values come from direction 2
                state_.InsertBlock(dir, d1, gCellD2, gCellD3,
                                   state_(dir, d1, pCellD2, gCellD3));
              }
            }
            // if neither surfaces are walls, do nothing
          }
        }
      }
    }
  }
}

/* Function to swap ghost cells between two blocks at an connection
boundary. Slices are removed from the physical cells (extending into ghost cells
at the edges) of one block and inserted into the ghost cells of its partner
block. The reverse is also true. The slices are taken in the coordinate system
orientation of their parent block.

   Interior Cells    Ghost Cells               Ghost Cells   Interior Cells
   ________ ______|________ _________       _______________|_______ _________
Ui-3/2   Ui-1/2   |    Uj+1/2    Uj+3/2  Ui-3/2    Ui-1/2  |    Uj+1/2    Uj+3/2
  |        |      |        |         |     |        |      |       |         |
  | Ui-1   |  Ui  |  Uj    |  Uj+1   |     |  Ui-1  |   Ui |  Uj   |  Uj+1   |
  |        |      |        |         |     |        |      |       |         |
  |________|______|________|_________|     |________|______|_______|_________|
                  |                                        |

The above diagram shows the resulting values after the ghost cell swap. The
logic ensures that the ghost cells at the connection boundary exactly match
their partner block as if there were no separation in the grid.
*/
void procBlock::SwapStateSlice(const connection &inter, procBlock &blk) {
  // inter -- connection boundary information
  // blk -- second block involved in connection boundary

  state_.SwapSlice(inter, blk.state_);
}

void procBlock::SwapTurbSlice(const connection &inter, procBlock &blk) {
  // inter -- connection boundary information
  // blk -- second block involved in connection boundary

  f1_.SwapSlice(inter, blk.f1_);
  f2_.SwapSlice(inter, blk.f2_);
}

void procBlock::SwapWallDistSlice(const connection &inter, procBlock &blk) {
  // inter -- connection boundary information
  // blk -- second block involved in connection boundary

  wallDist_.SwapSlice(inter, blk.wallDist_);
}

// This is done for the implicit solver so the off diagonal data adjacent to
// an interblock boundary condition can be accessed
void procBlock::SwapEddyViscAndGradientSlice(const connection &inter,
                                             procBlock &blk) {
  // inter -- connection boundary information
  // blk -- second block involved in connection boundary
                                            
  velocityGrad_.SwapSlice(inter, blk.velocityGrad_);

  if (isTurbulent_) {
    eddyViscosity_.SwapSlice(inter, blk.eddyViscosity_);
  }
}


/* Function to swap slice using MPI. This is similar to the SwapSlice
function, but is called when the neighboring procBlocks are on different
processors.
*/
void procBlock::SwapStateSliceMPI(const connection &inter, const int &rank) {
  // inter -- connection boundary information
  // rank -- processor rank
  state_.SwapSliceMPI(inter, rank, MPI_DOUBLE);
}

void procBlock::SwapTurbSliceMPI(const connection &inter, const int &rank) {
  // inter -- connection boundary information
  // rank -- processor rank

  f1_.SwapSliceMPI(inter, rank, MPI_DOUBLE, 2);
  f2_.SwapSliceMPI(inter, rank, MPI_DOUBLE, 3);
}

void procBlock::SwapWallDistSliceMPI(const connection &inter, const int &rank) {
  // inter -- connection boundary information
  // rank -- processor rank

  wallDist_.SwapSliceMPI(inter, rank, MPI_DOUBLE, 1);
}

void procBlock::SwapEddyViscAndGradientSliceMPI(
    const connection &inter, const int &rank,
    const MPI_Datatype &MPI_tensorDouble, const MPI_Datatype &MPI_vec3d) {
  // inter -- connection boundary information
  // rank -- processor rank

  velocityGrad_.SwapSliceMPI(inter, rank, MPI_tensorDouble, 1);

  if (isTurbulent_) {
    eddyViscosity_.SwapSliceMPI(inter, rank, MPI_DOUBLE, 5);
  }
}


/* Member function to overwrite a section of a procBlock's geometry with a
geomSlice. The function uses the orientation supplied in the connection to
orient the geomSlice relative to the procBlock. It assumes that the procBlock
is listed first, and the geomSlice second in the connection data structure.
It returns a vector of 4 bools that are returned true if one of the 4 edges
of the connection need to be updated because they border an connection,
creating a possible "t" intersection.
     __________     _________
    |       |  |   |  |      |
    |       |  |   |  |  2   |
    |       |  |   |__|______|
    |       |  |   |X_|______|
    |  0    |--|    _________
    |       |  |   |X_|______|
    |       |  |   |  |      |
    |       |  |   |  |  1   |
    |_______|__|   |__|______|

The block configuration shown above shows a "t"intersection. If blocks 0/1 are
swapped first, block 1 gets all of its ghost cells including the edge cells
(marked X) from block 0, but block 0 knows that the connection swapping with
block 1 borders another connection (the one swapping with block 2), so it does
not take the edge ghost cells from block 1. This is because the ghost cells that
the edge of block 1 would supply will be supplied by the swap with block 2. If
next, blocks 1/2 swap, block 1 now has garbage in its edge ghost cells. This is
because it is the first swap for block 2, so its ghost cells are empty (filled
with garbage). If next blocks 0/2 swap, block 0 does not take the ghost cells
from block 2 for the same reason it did not take them from block 1. This leaves
blocks 0 and 2 with the correct ghost cells, but block 1 has garbage in its edge
ghost cells. This problem occurs if the 1/2 swap is done between the 0/1 and
0/2 swaps!

To solve this problem, in this situation the 1/2 swap is done without edge ghost
cells. This fixes the problem because the correct ghost cells block 1 got from
block 0 are not overwritten with the garbage ghost cells from block 2. The way
this is enforced is the following. Initially all ghost cell volumes are
initialized to 0. Therefore, if a cell from a geomSlice with a volume of 0 is
trying to be inserted into the procBlock, this cell has not been given its proper
boundary conditions and should be ignored. Subsequently, the connection should be
updated so that in the future this cell is not inserted.
*/
vector<bool> procBlock::PutGeomSlice(const geomSlice &slice, connection &inter,
                                     const int &d3) {
  // slice -- geomSlice to insert int procBlock
  // inter -- connection data structure describing the patches and their
  // orientation
  // d3 -- distance of direction normal to patch to insert

  // check that number of cells to insert matches
  const auto blkCell = inter.Dir1LenFirst() * inter.Dir2LenFirst() * d3;
  if (blkCell != slice.NumCells()) {
    cerr << "ERROR: Error in procBlock::PutGeomSlice(). Number of cells being "
            "inserted does not match designated space to insert to." << endl;
    cerr << "Direction 1, 2, 3 of procBlock: "
         << inter.Dir1LenFirst() << ", " << inter.Dir2LenFirst() << ", " << d3
         << endl;
    cerr << "Direction I, J, K of geomSlice: " << slice.NumI() << ", "
         << slice.NumJ() << ", " << slice.NumK() << endl;
    exit(EXIT_FAILURE);
  }

  // adjust insertion indices if patch borders another connection on the same
  // surface of the block
  const auto adjS1 = (inter.Dir1StartInterBorderFirst()) ? numGhosts_ : 0;
  const auto adjE1 = (inter.Dir1EndInterBorderFirst()) ? numGhosts_ : 0;
  const auto adjS2 = (inter.Dir2StartInterBorderFirst()) ? numGhosts_ : 0;
  const auto adjE2 = (inter.Dir2EndInterBorderFirst()) ? numGhosts_ : 0;

  vector<bool> adjEdge(4, false);  // initialize all return values to false
  // determine if area direction needs to be reversed
  const auto aFac3 = ((inter.BoundaryFirst() + inter.BoundarySecond()) % 2 == 0)
      ? -1.0 : 1.0;
  const auto aFac1 = (inter.Orientation() == 3 || inter.Orientation() == 4 ||
                  inter.Orientation() == 7 || inter.Orientation() == 8)
      ? -1.0 : 1.0;
  const auto aFac2 = (inter.Orientation() == 5 || inter.Orientation() == 6 ||
                  inter.Orientation() == 7 || inter.Orientation() == 8)
      ? -1.0 : 1.0;

  // loop over cells to insert
  for (auto l3 = 0; l3 < d3; l3++) {
    for (auto l2 = adjS2; l2 < inter.Dir2LenFirst() - adjE2; l2++) {
      for (auto l1 = adjS1; l1 < inter.Dir1LenFirst() - adjE1; l1++) {
        // get block and slice indices
        const auto indB = GetSwapLoc(l1, l2, l3, numGhosts_, inter, d3, true);
        auto indS = GetSwapLoc(l1, l2, l3, slice.GhostLayers(), inter, d3, false);

        // don't overwrite with garbage from partner block that hasn't recieved
        // its ghost value yet (needed at "t" intersection)
        if (slice.Vol(indS[0], indS[1], indS[2]) == 0.0) {
          // find out if on edge, if so save edge
          // at a block edge -- possible need to adjust connection
          string edgeDir = "undefined";
          if (this->AtEdgeInclusive(indB[0], indB[1], indB[2], edgeDir)) {
            auto dir1 = 0;
            auto dir2 = 0;
            if (inter.Direction1First() == "i") {
              dir1 = 0;  // direction 1 is i
              dir2 = 1;  // direction 2 is j
            } else if (inter.Direction1First() == "j") {
              dir1 = 1;  // direction 1 is j
              dir2 = 2;  // direction 2 is k
            } else {
              dir1 = 2;  // direction 1 is k
              dir2 = 0;  // direction 2 is i
            }

            // find out edge direction
            // edge direction matches connection direction 1
            if (edgeDir == inter.Direction1First()) {
              // adjust edge on lower dir2 side
              if (indB[dir2] < inter.Dir2StartFirst() + numGhosts_) {
                adjEdge[2] = true;
              } else {  // adjust edge on upper dir2 side
                adjEdge[3] = true;
              }
            // edge direction matches connection direction 2
            } else if (edgeDir == inter.Direction2First()) {
              // adjust edge on lower dir1 side
              if (indB[dir1] < inter.Dir1StartFirst() + numGhosts_) {
                adjEdge[0] = true;
              } else {  // adjust edge on upper dir1 side
                adjEdge[1] = true;
              }
            } else {
              cerr << "ERROR: Error in procBlock::PutGeomSlice(). Ghost cell "
                      "edge direction does not match connection direction 1 or "
                      "2." << endl;
              cerr << "Edge direction is " << edgeDir << ", direction 1 is "
                   << inter.Direction1First() << ", and direction 2 is "
                   << inter.Direction2First() << endl;
              cerr << "Location is: " << indB[0] << ", " << indB[1] << ", "
                   << indB[2] << endl;
              exit(EXIT_FAILURE);
            }
          }

        // volume is not 0, ok to overwrite variables
        } else {
          // swap cell data
          vol_(indB[0], indB[1], indB[2]) = slice.Vol(indS[0], indS[1], indS[2]);
          center_(indB[0], indB[1], indB[2]) = slice.Center(indS[0], indS[1],
                                                            indS[2]);

          //-------------------------------------------------------------------
          // swap face data

          // if lower/lower or upper/upper, direction 3 must be reversed
          // for face indices direction 3 should also be incremented one when
          // accessing direction 3 face data, and then decremented to access
          // direction 1/2 face data
          auto fac3 = 1;
          if (inter.IsLowerLowerOrUpperUpper()) {
            fac3 = -1;
            if (inter.Direction3Second() == "i") {
              indS[0]++;
            } else if (inter.Direction3Second() == "j") {
              indS[1]++;
            } else {
              indS[2]++;
            }
          }

          // both patches i, i to i, j to j, k to k
          if (inter.Direction3First() == "i" &&
              inter.Direction3Second() == "i") {
            // swap face data for direction 3
            fCenterI_(indB[0], indB[1], indB[2]) =
                slice.FCenterI(indS[0], indS[1], indS[2]);
            fAreaI_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaI(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + fac3, indS[1], indS[2]);
              fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac3 *
                  slice.FAreaI(indS[0] + fac3, indS[1], indS[2]);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[0]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaJ(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac1 *
                    slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterJ(indS[0], indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac1 *
                    slice.FAreaJ(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaK(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterK(indS[0], indS[1], indS[2] + 1);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac2 *
                    slice.FAreaK(indS[0], indS[1], indS[2] + 1);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + 1);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + 1);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterK(indS[0], indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac2 *
                    slice.FAreaK(indS[0], indS[1], indS[2]);
              }
            }

          //----------------------------------------------------------------
          // both patches j, j to j, k to k, i to i
          } else if (inter.Direction3First() == "j" &&
                     inter.Direction3Second() == "j") {
            // swap face data for direction 3
            fCenterJ_(indB[0], indB[1], indB[2]) =
                slice.FCenterJ(indS[0], indS[1], indS[2]);
            fAreaJ_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaJ(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + fac3, indS[2]);
              fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac3 *
                  slice.FAreaJ(indS[0], indS[1] + fac3, indS[2]);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[1]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaK(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterK(indS[0], indS[1], indS[2] + 1);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac1 *
                    slice.FAreaK(indS[0], indS[1], indS[2] + 1);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + 1);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + 1);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterK(indS[0], indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac1 *
                    slice.FAreaK(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0], indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaI(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac2 *
                    slice.FAreaI(indS[0] + 1, indS[1], indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaI(indS[0] + 1, indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterI(indS[0], indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac2 *
                    slice.FAreaI(indS[0], indS[1], indS[2]);
              }
            }

          //-------------------------------------------------------------
          // both patches k, k to k, i to i, j to j
          } else if (inter.Direction3First() == "k" &&
                     inter.Direction3Second() == "k") {
            // swap face data for direction 3
            fCenterK_(indB[0], indB[1], indB[2]) =
                slice.FCenterK(indS[0], indS[1], indS[2]);
            fAreaK_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaK(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterK_(indB[0], indB[1], indB[2] + 1) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + fac3);
              fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac3 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + fac3);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[2]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0], indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaI(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac1 *
                    slice.FAreaI(indS[0] + 1, indS[1], indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaI(indS[0] + 1, indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterI(indS[0], indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac1 *
                    slice.FAreaI(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaJ(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac2 *
                    slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterJ(indS[0], indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac2 *
                    slice.FAreaJ(indS[0], indS[1], indS[2]);
              }
            }

          //---------------------------------------------------------------
          // patches are i/j  - i to j, j to k, k to i
          } else if (inter.Direction3First() == "i" &&
                     inter.Direction3Second() == "j") {
            // swap face data for direction 3
            fCenterI_(indB[0], indB[1], indB[2]) =
                slice.FCenterJ(indS[0], indS[1], indS[2]);
            fAreaI_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaJ(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + fac3, indS[2]);
              fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac3 *
                  slice.FAreaJ(indS[0], indS[1] + fac3, indS[2]);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[1]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaK(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2] + 1);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac1 *
                    slice.FAreaK(indS[0], indS[1], indS[2] + 1);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + 1);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + 1);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac1 *
                    slice.FAreaK(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0], indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaI(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac2 *
                    slice.FAreaI(indS[0] + 1, indS[1], indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaI(indS[0] + 1, indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterI(indS[0], indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac2 *
                    slice.FAreaI(indS[0], indS[1], indS[2]);
              }
            }

          //------------------------------------------------------------------
          // patches are i/k  - i to k, j to i, k to j
          } else if (inter.Direction3First() == "i" &&
                     inter.Direction3Second() == "k") {
            // swap face data for direction 3
            fCenterI_(indB[0], indB[1], indB[2]) =
                slice.FCenterK(indS[0], indS[1], indS[2]);
            fAreaI_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaK(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + fac3);
              fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac3 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + fac3);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[2]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0], indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaI(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac1 *
                    slice.FAreaI(indS[0] + 1, indS[1], indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaI(indS[0] + 1, indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterI(indS[0], indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac1 *
                    slice.FAreaI(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaJ(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac2 *
                    slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterJ(indS[0], indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac2 *
                    slice.FAreaJ(indS[0], indS[1], indS[2]);
              }
            }

          //--------------------------------------------------------------
          // patches are j/i, j to i, k to j, i to k
          } else if (inter.Direction3First() == "j" &&
                     inter.Direction3Second() == "i") {
            // swap face data for direction 3
            fCenterJ_(indB[0], indB[1], indB[2]) =
                slice.FCenterI(indS[0], indS[1], indS[2]);
            fAreaJ_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaI(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                  slice.FCenterI(indS[0] + fac3, indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac3 *
                  slice.FAreaI(indS[0] + fac3, indS[1], indS[2]);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[0]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaJ(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac1 *
                    slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterJ(indS[0], indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac1 *
                    slice.FAreaJ(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaK(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2] + 1);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac2 *
                    slice.FAreaK(indS[0], indS[1], indS[2] + 1);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + 1);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + 1);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac2 *
                    slice.FAreaK(indS[0], indS[1], indS[2]);
              }
            }


          //-----------------------------------------------------------------
          // patches are j/k, j to k, k to i, i to j
          } else if (inter.Direction3First() == "j" &&
                     inter.Direction3Second() == "k") {
            // swap face data for direction 3
            fCenterJ_(indB[0], indB[1], indB[2]) =
                slice.FCenterK(indS[0], indS[1], indS[2]);
            fAreaJ_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaK(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + fac3);
              fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac3 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + fac3);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[2]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0], indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaI(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac1 *
                    slice.FAreaI(indS[0] + 1, indS[1], indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterK_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaI(indS[0] + 1, indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterK_(indB[0], indB[1], indB[2] + 1) =
                    slice.FCenterI(indS[0], indS[1], indS[2]);
                fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac1 *
                    slice.FAreaI(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaJ(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac2 *
                    slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterJ(indS[0], indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac2 *
                    slice.FAreaJ(indS[0], indS[1], indS[2]);
              }
            }

          //------------------------------------------------------------------
          // patches are k/i, k to i, i to j, j to k
          } else if (inter.Direction3First() == "k" &&
                     inter.Direction3Second() == "i") {
            // swap face data for direction 3
            fCenterK_(indB[0], indB[1], indB[2]) =
                slice.FCenterI(indS[0], indS[1], indS[2]);
            fAreaK_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaI(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterK_(indB[0], indB[1], indB[2] + 1) =
                  slice.FCenterI(indS[0] + fac3, indS[1], indS[2]);
              fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac3 *
                  slice.FAreaI(indS[0] + fac3, indS[1], indS[2]);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[0]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaJ(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac1 *
                    slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterJ(indS[0], indS[1] + 1, indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaJ(indS[0], indS[1] + 1, indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterJ(indS[0], indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac1 *
                    slice.FAreaJ(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaK(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2] + 1);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac2 *
                    slice.FAreaK(indS[0], indS[1], indS[2] + 1);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + 1);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + 1);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac2 *
                    slice.FAreaK(indS[0], indS[1], indS[2]);
              }
            }

          //-------------------------------------------------------------------
          // patches are k/j, k to j, i to k, j to i
          } else if (inter.Direction3First() == "k" &&
                     inter.Direction3Second() == "j") {
            // swap face data for direction 3
            fCenterK_(indB[0], indB[1], indB[2]) =
                slice.FCenterJ(indS[0], indS[1], indS[2]);
            fAreaK_(indB[0], indB[1], indB[2]) = aFac3 *
                slice.FAreaJ(indS[0], indS[1], indS[2]);

            if (l3 == (d3 - 1)) {  // at end of direction 3 line
              fCenterK_(indB[0], indB[1], indB[2] + 1) =
                  slice.FCenterJ(indS[0], indS[1] + fac3, indS[2]);
              fAreaK_(indB[0], indB[1], indB[2] + 1) = aFac3 *
                  slice.FAreaJ(indS[0], indS[1] + fac3, indS[2]);
            }
            if (inter.IsLowerLowerOrUpperUpper()) {
              indS[1]--;
            }

            // swap face data for direction 1
            if (aFac1 == 1.0) {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2]);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaK(indS[0], indS[1], indS[2]);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2] + 1);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac1 *
                    slice.FAreaK(indS[0], indS[1], indS[2] + 1);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterI_(indB[0], indB[1], indB[2]) =
                  slice.FCenterK(indS[0], indS[1], indS[2] + 1);
              fAreaI_(indB[0], indB[1], indB[2]) = aFac1 *
                  slice.FAreaK(indS[0], indS[1], indS[2] + 1);

              // at end of direction 1 line
              if (l1 == (inter.Dir1EndFirst() - inter.Dir1StartFirst() - 1)) {
                fCenterI_(indB[0] + 1, indB[1], indB[2]) =
                    slice.FCenterK(indS[0], indS[1], indS[2]);
                fAreaI_(indB[0] + 1, indB[1], indB[2]) = aFac1 *
                    slice.FAreaK(indS[0], indS[1], indS[2]);
              }
            }

            // swap face data for direction 2
            if (aFac2 == 1.0) {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0], indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaI(indS[0], indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac2 *
                    slice.FAreaI(indS[0] + 1, indS[1], indS[2]);
              }
            // if direction is reversed, upper/lower faces need to be swapped
            } else {
              fCenterJ_(indB[0], indB[1], indB[2]) =
                  slice.FCenterI(indS[0] + 1, indS[1], indS[2]);
              fAreaJ_(indB[0], indB[1], indB[2]) = aFac2 *
                  slice.FAreaI(indS[0] + 1, indS[1], indS[2]);

              // at end of direction 2 line
              if (l2 == (inter.Dir2EndFirst() - inter.Dir2StartFirst() - 1)) {
                fCenterJ_(indB[0], indB[1] + 1, indB[2]) =
                    slice.FCenterI(indS[0], indS[1], indS[2]);
                fAreaJ_(indB[0], indB[1] + 1, indB[2]) = aFac2 *
                    slice.FAreaI(indS[0], indS[1], indS[2]);
              }
            }

          //-------------------------------------------------------------------
          } else {
            cerr << "ERROR: Error in procBlock::PutGeomSlice(). Unable to swap "
                    "face quantities because behavior for interface with "
                    "boundary pair " << inter.Direction3First() << ", "
                 << inter.Direction3Second() << " is not defined." << endl;
            exit(EXIT_FAILURE);
          }
        }
      }
    }
  }
  return adjEdge;
}

/* Member function to overwrite a section of a procBlock's states with a
slice of states. The function uses the orientation supplied in the connection to
orient the slice relative to the procBlock. It assumes that the procBlock
is listed first, and the slice second in the connection data structure.
*/
void procBlock::PutStateSlice(const blkMultiArray3d<primitive> &slice,
                              const connection &inter,
                              const int &d3, const int &numG) {
  // slice -- slice to insert in procBlock
  // inter -- connection data structure defining the patches and their
  // orientation
  // d3 -- distance of direction normal to patch to insert
  // numG -- number of ghost cells

  state_.PutSlice(slice, inter, d3);
}

/*Member function to pack and send procBlock geometry data to appropriate
 * processor. */
void procBlock::PackSendGeomMPI(const MPI_Datatype &MPI_vec3d,
                                const MPI_Datatype &MPI_vec3dMag) const {
  // MPI_vec3d -- MPI data type for a vector3d
  // MPI_vec3dMag -- MPI data type for a unitVect3dMag
  
  // determine size of buffer to send
  auto sendBufSize = 0;
  auto tempSize = 0;
  // adding 3 more ints for block dimensions
  MPI_Pack_size(8, MPI_INT, MPI_COMM_WORLD,
                &tempSize);  // add size for ints in class procBlock
  sendBufSize += tempSize;
  MPI_Pack_size(6, MPI_CXX_BOOL, MPI_COMM_WORLD,
                &tempSize);  // add size for bools in class procBlock
  sendBufSize += tempSize;
  MPI_Pack_size(state_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for states
  sendBufSize += tempSize;
  MPI_Pack_size(consVarsNm1_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for solution n-1
  sendBufSize += tempSize;
  MPI_Pack_size(nodes_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for nodes
  sendBufSize += tempSize;
  MPI_Pack_size(center_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for cell centers
  sendBufSize += tempSize;
  MPI_Pack_size(fAreaI_.Size(), MPI_vec3dMag, MPI_COMM_WORLD,
                &tempSize);  // add size for face area I
  sendBufSize += tempSize;
  MPI_Pack_size(fAreaJ_.Size(), MPI_vec3dMag, MPI_COMM_WORLD,
                &tempSize);  // add size for face area J
  sendBufSize += tempSize;
  MPI_Pack_size(fAreaK_.Size(), MPI_vec3dMag, MPI_COMM_WORLD,
                &tempSize);  // add size for face area K
  sendBufSize += tempSize;
  MPI_Pack_size(fCenterI_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for face center I
  sendBufSize += tempSize;
  MPI_Pack_size(fCenterJ_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for face center J
  sendBufSize += tempSize;
  MPI_Pack_size(fCenterK_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for face center K
  sendBufSize += tempSize;
  MPI_Pack_size(vol_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for volumes
  sendBufSize += tempSize;
  MPI_Pack_size(3, MPI_INT, MPI_COMM_WORLD,
                &tempSize);  // add size for number of bc surfaces
  sendBufSize += tempSize;
  // 8x because iMin, iMax, jMin, jMax, kMin, kMax, tags, string sizes
  MPI_Pack_size(bc_.NumSurfaces() * 8, MPI_INT, MPI_COMM_WORLD,
                &tempSize);  // add size for BCs
  sendBufSize += tempSize;

  auto stringSize = 0;
  for (auto jj = 0; jj < bc_.NumSurfaces(); jj++) {
    // add size for bc_ types (+1 for c_str end character)
    MPI_Pack_size(bc_.GetBCTypes(jj).size() + 1, MPI_CHAR, MPI_COMM_WORLD,
                  &tempSize);
    stringSize += tempSize;
  }
  sendBufSize += stringSize;

  for (auto &wd : wallData_) {
    wd.PackSize(sendBufSize, MPI_vec3d);
  }

  // allocate buffer to pack data into
  // use unique_ptr to manage memory; use underlying pointer with MPI calls
  auto sendBuffer = std::make_unique<char[]>(sendBufSize);
  auto *rawSendBuffer = sendBuffer.get();

  const auto numI = this->NumI();
  const auto numJ = this->NumJ();
  const auto numK = this->NumK();

  // pack data to send into buffer
  auto position = 0;
  // int and vector data
  MPI_Pack(&numI, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&numJ, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&numK, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&numGhosts_, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&parBlock_, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&rank_, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&localPos_, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&globalPos_, 1, MPI_INT, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&isViscous_, 1, MPI_CXX_BOOL, rawSendBuffer, sendBufSize,
           &position, MPI_COMM_WORLD);
  MPI_Pack(&isTurbulent_, 1, MPI_CXX_BOOL, rawSendBuffer, sendBufSize,
           &position, MPI_COMM_WORLD);
  MPI_Pack(&isRANS_, 1, MPI_CXX_BOOL, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&storeTimeN_, 1, MPI_CXX_BOOL, rawSendBuffer, sendBufSize,
           &position, MPI_COMM_WORLD);
  MPI_Pack(&isMultiLevelTime_, 1, MPI_CXX_BOOL, rawSendBuffer, sendBufSize,
           &position, MPI_COMM_WORLD);
  MPI_Pack(&isMultiSpecies_, 1, MPI_CXX_BOOL, rawSendBuffer, sendBufSize,
           &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(state_)), state_.Size(), MPI_DOUBLE, rawSendBuffer,
           sendBufSize, &position, MPI_COMM_WORLD);
  if (isMultiLevelTime_) {
    MPI_Pack(&(*std::begin(consVarsNm1_)), consVarsNm1_.Size(), MPI_DOUBLE,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  }
  MPI_Pack(&(*std::begin(nodes_)), nodes_.Size(), MPI_vec3d, rawSendBuffer,
           sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(center_)), center_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(fAreaI_)), fAreaI_.Size(), MPI_vec3dMag,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(fAreaJ_)), fAreaJ_.Size(), MPI_vec3dMag,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(fAreaK_)), fAreaK_.Size(), MPI_vec3dMag,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(fCenterI_)), fCenterI_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(fCenterJ_)), fCenterJ_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(fCenterK_)), fCenterK_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(vol_)), vol_.Size(), MPI_DOUBLE, rawSendBuffer,
           sendBufSize, &position, MPI_COMM_WORLD);

  // pack boundary condition data
  bc_.PackBC(rawSendBuffer, sendBufSize, position);

  // pack wall data
  for (auto &wd : wallData_) {
    wd.PackWallData(rawSendBuffer, sendBufSize, position, MPI_vec3d);
  }

  // send buffer to appropriate processor
  MPI_Send(rawSendBuffer, sendBufSize, MPI_PACKED, rank_, 2,
           MPI_COMM_WORLD);
}

void procBlock::RecvUnpackGeomMPI(const MPI_Datatype &MPI_vec3d,
                                  const MPI_Datatype &MPI_vec3dMag,
                                  const input &inp) {
  // MPI_vec3d -- MPI data type for a vector3d
  // MPI_vec3dMag -- MPI data type for a unitVect3dMag
  // input -- input variables

  MPI_Status status;  // allocate MPI_Status structure

  // probe message to get correct data size
  auto recvBufSize = 0;
  MPI_Probe(ROOTP, 2, MPI_COMM_WORLD, &status);
  // use MPI_CHAR because sending buffer was allocated with chars
  MPI_Get_count(&status, MPI_CHAR, &recvBufSize);

  // allocate buffer of correct size
  // use unique_ptr to manage memory; use underlying pointer with MPI
  auto recvBuffer = std::make_unique<char[]>(recvBufSize);
  auto *rawRecvBuffer = recvBuffer.get();

  // receive message from ROOT
  MPI_Recv(rawRecvBuffer, recvBufSize, MPI_PACKED, ROOTP, 2, MPI_COMM_WORLD,
           &status);

  auto numI = 0, numJ = 0, numK = 0;
  // unpack procBlock INTs
  auto position = 0;
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &numI, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &numJ, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &numK, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &numGhosts_, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &parBlock_, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &rank_, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &localPos_, 1, MPI_INT,
             MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &globalPos_, 1, MPI_INT,
             MPI_COMM_WORLD);

  // unpack procBlock bools
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &isViscous_, 1,
             MPI_CXX_BOOL, MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &isTurbulent_, 1,
             MPI_CXX_BOOL, MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &isRANS_, 1,
             MPI_CXX_BOOL, MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &storeTimeN_, 1,
             MPI_CXX_BOOL, MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &isMultiLevelTime_, 1,
             MPI_CXX_BOOL, MPI_COMM_WORLD);
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &isMultiSpecies_, 1,
             MPI_CXX_BOOL, MPI_COMM_WORLD);

  // clean and resize the vectors in the class to
  this->CleanResizeVecs(numI, numJ, numK, numGhosts_, inp.NumEquations(),
                        inp.NumSpecies());

  // unpack vector data into allocated vectors
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(state_)),
             state_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack states
  if (isMultiLevelTime_) {
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(consVarsNm1_)), consVarsNm1_.Size(), MPI_DOUBLE,
               MPI_COMM_WORLD);  // unpack sol n-1
  }
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(nodes_)),
             nodes_.Size(), MPI_vec3d,
             MPI_COMM_WORLD);  // unpack nodes
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(center_)),
             center_.Size(), MPI_vec3d,
             MPI_COMM_WORLD);  // unpack cell centers
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(fAreaI_)),
             fAreaI_.Size(), MPI_vec3dMag,
             MPI_COMM_WORLD);  // unpack face area I
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(fAreaJ_)),
             fAreaJ_.Size(), MPI_vec3dMag,
             MPI_COMM_WORLD);  // unpack face area J
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(fAreaK_)),
             fAreaK_.Size(), MPI_vec3dMag,
             MPI_COMM_WORLD);  // unpack face area K
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(fCenterI_)), fCenterI_.Size(), MPI_vec3d,
             MPI_COMM_WORLD);  // unpack face center I
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(fCenterJ_)), fCenterJ_.Size(), MPI_vec3d,
             MPI_COMM_WORLD);  // unpack face center J
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(fCenterK_)), fCenterK_.Size(), MPI_vec3d,
             MPI_COMM_WORLD);  // unpack face center K
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(vol_)),
             vol_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack volumes

  // unpack boundary conditions
  bc_.UnpackBC(rawRecvBuffer, recvBufSize, position);

  // unpack wall data
  wallData_.resize(bc_.NumViscousSurfaces());
  for (auto &wd : wallData_) {
    wd.UnpackWallData(rawRecvBuffer, recvBufSize, position, MPI_vec3d, inp);
  }
}

/*Member function to zero and resize the vectors in a procBlock to their
 * appropriate size given the i, j, and k dimensions.*/
void procBlock::CleanResizeVecs(const int &numI, const int &numJ,
                                const int &numK, const int &numGhosts,
                                const int &numEqns, const int &numSpecies) {
  // numI -- i-dimension to resize to (no ghosts)
  // numJ -- j-dimension to resize to (no ghosts)
  // numK -- k-dimension to resize to (no ghosts)
  // numGhosts -- number of ghost cells

  state_.ClearResize(numI, numJ, numK, numGhosts, numEqns, numSpecies);
  if (storeTimeN_) {
    consVarsN_.ClearResize(numI, numJ, numK, 0, numEqns, numSpecies);
  }
  if (isMultiLevelTime_) {
    consVarsNm1_.ClearResize(numI, numJ, numK, 0, numEqns, numSpecies);
  }

  nodes_.ClearResize(numI + 1, numJ + 1, numK + 1);
  center_.ClearResize(numI, numJ, numK, numGhosts);
  vol_.ClearResize(numI, numJ, numK, numGhosts);

  fCenterI_.ClearResize(numI + 1, numJ, numK, numGhosts);
  fAreaI_.ClearResize(numI + 1, numJ, numK, numGhosts);

  fCenterJ_.ClearResize(numI, numJ + 1, numK, numGhosts);
  fAreaJ_.ClearResize(numI, numJ + 1, numK, numGhosts);

  fCenterK_.ClearResize(numI, numJ, numK + 1, numGhosts);
  fAreaK_.ClearResize(numI, numJ, numK + 1, numGhosts);

  cellWidthI_.ClearResize(numI, numJ, numK, numGhosts, 1, 0.0);
  cellWidthJ_.ClearResize(numI, numJ, numK, numGhosts, 1, 0.0);
  cellWidthK_.ClearResize(numI, numJ, numK, numGhosts, 1, 0.0);

  wallDist_.ClearResize(numI, numJ, numK, numGhosts, 1, DEFAULT_WALL_DIST);

  residual_.ClearResize(numI, numJ, numK, 0, numEqns, numSpecies);
  specRadius_.ClearResize(numI, numJ, numK, 0);
  dt_.ClearResize(numI, numJ, numK, 0);

  temperature_.ClearResize(numI, numJ, numK, numGhosts);

  // vel grad ghosts for off diag term along interblock bc in implicit solver 
  velocityGrad_.ClearResize(numI, numJ, numK, numGhosts);
  temperatureGrad_.ClearResize(numI, numJ, numK, 0);
  densityGrad_.ClearResize(numI, numJ, numK, 0);
  pressureGrad_.ClearResize(numI, numJ, numK, 0);

  if (isViscous_) {
    viscosity_.ClearResize(numI, numJ, numK, numGhosts);
  }

  if (isTurbulent_) {
    eddyViscosity_.ClearResize(numI, numJ, numK, numGhosts);
  }

  if (isRANS_) {
    tkeGrad_.ClearResize(numI, numJ, numK, 0);
    omegaGrad_.ClearResize(numI, numJ, numK, 0);
    f1_.ClearResize(numI, numJ, numK, numGhosts);
    f2_.ClearResize(numI, numJ, numK, numGhosts);
  }

  if (isMultiSpecies_) {
    mixtureGrad_.ClearResize(numI, numJ, numK, 0, numSpecies);
  }
}

/*Member function to receive and unpack procBlock state data. This is used to
 * gather the solution on the ROOT processor to write out the solution. */
void procBlock::RecvUnpackSolMPI(const MPI_Datatype &MPI_uncoupledScalar,
                                 const MPI_Datatype &MPI_vec3d,
                                 const MPI_Datatype &MPI_tensorDouble,
                                 const input &inp) {
  // MPI_uncoupledScalar -- MPI data type for uncoupledScalar
  // MPI_vec3d -- MPI data type for vector3d<double>
  // MPI_tensorDouble -- MPI data taype for tensor<double>
  // input -- input variables

  MPI_Status status;  // allocate MPI_Status structure

  // probe message to get correct data size
  auto recvBufSize = 0;
  // global position used as tag because each block has a unique one
  MPI_Probe(rank_, globalPos_, MPI_COMM_WORLD, &status);
  MPI_Get_count(&status, MPI_CHAR, &recvBufSize);  // use MPI_CHAR because
                                                   // sending buffer was
                                                   // allocated with chars
  // allocate buffer of correct size
  // use unique_ptr to manage memory; use underlying pointer for MPI calls
  auto recvBuffer = std::make_unique<char[]>(recvBufSize);
  auto *rawRecvBuffer = recvBuffer.get();

  // receive message from non-ROOT
  MPI_Recv(rawRecvBuffer, recvBufSize, MPI_PACKED, rank_, globalPos_,
           MPI_COMM_WORLD, &status);

  // unpack vector data into allocated vectors
  auto position = 0;
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(state_)),
             state_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack states
  if (isMultiLevelTime_) {
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(consVarsNm1_)), consVarsNm1_.Size(), MPI_DOUBLE,
               MPI_COMM_WORLD);  // unpack states
  }
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(residual_)), residual_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack residuals
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(dt_)),
             dt_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack time steps
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(cellWidthI_)), cellWidthI_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack cell width I
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(cellWidthJ_)), cellWidthJ_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack cell width J
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(cellWidthK_)), cellWidthK_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack cell width K
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(wallDist_)), wallDist_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack wall distance
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(specRadius_)), specRadius_.Size(),
             MPI_uncoupledScalar,
             MPI_COMM_WORLD);  // unpack average wave speeds
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(temperature_)), temperature_.Size(), MPI_DOUBLE,
             MPI_COMM_WORLD);  // unpack temperature

  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(velocityGrad_)), velocityGrad_.Size(),
             MPI_tensorDouble,
             MPI_COMM_WORLD);  // unpack velocity gradient
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(temperatureGrad_)), temperatureGrad_.Size(),
             MPI_vec3d, MPI_COMM_WORLD);  // unpack temperature gradient
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(densityGrad_)), densityGrad_.Size(),
             MPI_vec3d, MPI_COMM_WORLD);  // unpack density gradient
  MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
             &(*std::begin(pressureGrad_)), pressureGrad_.Size(),
             MPI_vec3d, MPI_COMM_WORLD);  // unpack pressure gradient

  if (isViscous_) {
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(viscosity_)), viscosity_.Size(), MPI_DOUBLE,
               MPI_COMM_WORLD);  // unpack viscosity
  }

  if (isTurbulent_) {
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(eddyViscosity_)), eddyViscosity_.Size(),
               MPI_DOUBLE, MPI_COMM_WORLD);  // unpack eddy viscosity
  }

  if (isRANS_) {
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(f1_)),
               f1_.Size(), MPI_DOUBLE,
               MPI_COMM_WORLD);  // unpack blending variable f1
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position, &(*std::begin(f2_)),
               f2_.Size(), MPI_DOUBLE,
               MPI_COMM_WORLD);  // unpack blending variable f2
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(tkeGrad_)), tkeGrad_.Size(), MPI_vec3d,
               MPI_COMM_WORLD);  // unpack tke gradient
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(omegaGrad_)), omegaGrad_.Size(), MPI_vec3d,
               MPI_COMM_WORLD);  // unpack omega gradient
  }

  if (isMultiSpecies_) {
    // unpack mixture gradients
    MPI_Unpack(rawRecvBuffer, recvBufSize, &position,
               &(*std::begin(mixtureGrad_)), mixtureGrad_.Size(), MPI_vec3d,
               MPI_COMM_WORLD);
  }

  // unpack wall data
  wallData_.resize(bc_.NumViscousSurfaces());
  for (auto &wd : wallData_) {
    wd.UnpackWallData(rawRecvBuffer, recvBufSize, position, MPI_vec3d, inp);
  }
}

/*Member function to pack and send procBlock state data to the ROOT proecessor.
 * This is used to gather the solution on the ROOT processor to write out the
 * solution. */
void procBlock::PackSendSolMPI(const MPI_Datatype &MPI_uncoupledScalar,
                               const MPI_Datatype &MPI_vec3d,
                               const MPI_Datatype &MPI_tensorDouble) const {
  // MPI_uncoupledScalar -- MPI data type for uncoupledScalar
  // MPI_vec3d -- MPI data type for vector3d<double>
  // MPI_tensorDouble -- MPI data taype for tensor<double>

  // determine size of buffer to send
  auto sendBufSize = 0;
  auto tempSize = 0;
  MPI_Pack_size(state_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for states
  sendBufSize += tempSize;
  if (isMultiLevelTime_) {
    MPI_Pack_size(consVarsNm1_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                  &tempSize);  // add size for sol n-1
    sendBufSize += tempSize;
  }
  MPI_Pack_size(residual_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for residuals
  sendBufSize += tempSize;
  MPI_Pack_size(dt_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for time steps
  sendBufSize += tempSize;
  MPI_Pack_size(cellWidthI_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for cell width I
  sendBufSize += tempSize;
  MPI_Pack_size(cellWidthJ_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for cell width J
  sendBufSize += tempSize;
  MPI_Pack_size(cellWidthK_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for cell width K
  sendBufSize += tempSize;
  MPI_Pack_size(wallDist_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for wall distance
  sendBufSize += tempSize;
  MPI_Pack_size(specRadius_.Size(), MPI_uncoupledScalar, MPI_COMM_WORLD,
                &tempSize);  // add size for average wave speed
  sendBufSize += tempSize;
  MPI_Pack_size(temperature_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                &tempSize);  // add size for temperature
  sendBufSize += tempSize;

  MPI_Pack_size(velocityGrad_.Size(), MPI_tensorDouble, MPI_COMM_WORLD,
                &tempSize);  // add size for velocity gradient
  sendBufSize += tempSize;
  MPI_Pack_size(temperatureGrad_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for temperature gradient
  sendBufSize += tempSize;
  MPI_Pack_size(densityGrad_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for density gradient
  sendBufSize += tempSize;
  MPI_Pack_size(pressureGrad_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                &tempSize);  // add size for pressure gradient
  sendBufSize += tempSize;

  if (isViscous_) {
    MPI_Pack_size(viscosity_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                  &tempSize);  // add size for viscosity
    sendBufSize += tempSize;
  }

  if (isTurbulent_) {
    MPI_Pack_size(eddyViscosity_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                  &tempSize);  // add size for eddy viscosity
    sendBufSize += tempSize;
  }

  if (isRANS_) {
    MPI_Pack_size(f1_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                  &tempSize);  // add size for blending variable f1
    sendBufSize += tempSize;
    MPI_Pack_size(f2_.Size(), MPI_DOUBLE, MPI_COMM_WORLD,
                  &tempSize);  // add size for blending variable f2
    sendBufSize += tempSize;
    MPI_Pack_size(tkeGrad_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                  &tempSize);  // add size for tke gradient
    sendBufSize += tempSize;
    MPI_Pack_size(omegaGrad_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                  &tempSize);  // add size for omega gradient
    sendBufSize += tempSize;
  }

  if (isMultiSpecies_) {
    MPI_Pack_size(mixtureGrad_.Size(), MPI_vec3d, MPI_COMM_WORLD,
                  &tempSize);  // add size for mixture gradients
    sendBufSize += tempSize;
  }

  for (auto &wd : wallData_) {
    wd.PackSize(sendBufSize, MPI_vec3d);
  }

  // allocate buffer to pack data into
  // use unique_ptr to manage memory; use underlying pointer for MPI calls
  auto sendBuffer = std::make_unique<char[]>(sendBufSize);
  auto *rawSendBuffer = sendBuffer.get();

  // pack data to send into buffer
  auto position = 0;
  MPI_Pack(&(*std::begin(state_)), state_.Size(), MPI_DOUBLE, rawSendBuffer,
           sendBufSize, &position, MPI_COMM_WORLD);
  if (isMultiLevelTime_) {
    MPI_Pack(&(*std::begin(consVarsNm1_)), consVarsNm1_.Size(), MPI_DOUBLE,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  }
  MPI_Pack(&(*std::begin(residual_)), residual_.Size(), MPI_DOUBLE,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(dt_)), dt_.Size(), MPI_DOUBLE, rawSendBuffer,
           sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(cellWidthI_)), cellWidthI_.Size(), MPI_DOUBLE,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(cellWidthJ_)), cellWidthJ_.Size(), MPI_DOUBLE,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(cellWidthK_)), cellWidthK_.Size(), MPI_DOUBLE,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);           
  MPI_Pack(&(*std::begin(wallDist_)), wallDist_.Size(), MPI_DOUBLE,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(specRadius_)), specRadius_.Size(), MPI_uncoupledScalar,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(temperature_)), temperature_.Size(), MPI_DOUBLE,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);

  MPI_Pack(&(*std::begin(velocityGrad_)), velocityGrad_.Size(),
           MPI_tensorDouble, rawSendBuffer, sendBufSize, &position,
           MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(temperatureGrad_)), temperatureGrad_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(densityGrad_)), densityGrad_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  MPI_Pack(&(*std::begin(pressureGrad_)), pressureGrad_.Size(), MPI_vec3d,
           rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);

  if (isViscous_) {
    MPI_Pack(&(*std::begin(viscosity_)), viscosity_.Size(), MPI_DOUBLE,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  }

  if (isTurbulent_) {
    MPI_Pack(&(*std::begin(eddyViscosity_)), eddyViscosity_.Size(), MPI_DOUBLE,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  }

  if (isRANS_) {
    MPI_Pack(&(*std::begin(f1_)), f1_.Size(), MPI_DOUBLE,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
    MPI_Pack(&(*std::begin(f2_)), f2_.Size(), MPI_DOUBLE,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
    MPI_Pack(&(*std::begin(tkeGrad_)), tkeGrad_.Size(), MPI_vec3d,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
    MPI_Pack(&(*std::begin(omegaGrad_)), omegaGrad_.Size(), MPI_vec3d,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  }

  if (isMultiSpecies_) {
    MPI_Pack(&(*std::begin(mixtureGrad_)), mixtureGrad_.Size(), MPI_vec3d,
             rawSendBuffer, sendBufSize, &position, MPI_COMM_WORLD);
  }

  // pack wall data
  for (auto &wd : wallData_) {
    wd.PackWallData(rawSendBuffer, sendBufSize, position, MPI_vec3d);
  }

  // send buffer to appropriate processor
  MPI_Send(rawSendBuffer, sendBufSize, MPI_PACKED, ROOTP, globalPos_,
           MPI_COMM_WORLD);
}

/* Member function to split a procBlock along a plane defined by a direction and
an index. The calling instance will retain the lower portion of the split,
and the returned instance will retain the upper portion of the split.
*/
procBlock procBlock::Split(const string &dir, const int &ind, const int &num,
                           vector<boundarySurface> &alteredSurf) {
  // dir -- plane to split along, either i, j, or k
  // ind -- index (face) to split at (w/o counting ghost cells)
  // num -- new block number
  // alteredSurf -- vector of surfaces whose partners will need to be altered
  //                after this split

  auto bound1 = bc_;
  auto bound2 = bound1.Split(dir, ind, parBlock_, num, alteredSurf);
  auto wd2 = this->SplitWallData(dir, ind);

  auto numI1 = 0, numI2 = 0;
  auto numJ1 = 0, numJ2 = 0;
  auto numK1 = 0, numK2 = 0;
  if (dir == "i") {  // split along i-plane
    numI2 = this->NumI() - ind;
    numI1 = this->NumI() - numI2;
    numJ2 = this->NumJ();
    numJ1 = this->NumJ();
    numK2 = this->NumK();
    numK1 = this->NumK();
  } else if (dir == "j") {  // split along j-plane
    numI2 = this->NumI();
    numI1 = this->NumI();
    numJ2 = this->NumJ() - ind;
    numJ1 = this->NumJ() - numJ2;
    numK2 = this->NumK();
    numK1 = this->NumK();
  } else if (dir == "k") {  // split along k-plane
    numI2 = this->NumI();
    numI1 = this->NumI();
    numJ2 = this->NumJ();
    numJ1 = this->NumJ();
    numK2 = this->NumK() - ind;
    numK1 = this->NumK() - numK2;
  } else {
    cerr << "ERROR: Error in procBlock::Split(). Direction " << dir
         << " is not recognized! Choose either i, j, or k." << endl;
    exit(EXIT_FAILURE);
  }

  procBlock blk1(numI1, numJ1, numK1, numGhosts_, this->NumEquations(),
                 this->NumSpecies(), isViscous_, isTurbulent_, isRANS_,
                 storeTimeN_, isMultiLevelTime_, isMultiSpecies_);
  procBlock blk2(numI2, numJ2, numK2, numGhosts_, this->NumEquations(),
                 this->NumSpecies(), isViscous_, isTurbulent_, isRANS_,
                 storeTimeN_, isMultiLevelTime_, isMultiSpecies_);

  blk1.parBlock_ = parBlock_;
  blk2.parBlock_ = parBlock_;

  blk1.nodes_ = nodes_;
  blk2.nodes_ = blk1.nodes_.Split(dir, ind);

  // ------------------------------------------------------------------
  // assign variables for lower split
  // assign cell variables with ghost cells
  blk1.state_.Fill(state_.Slice(dir, {state_.Start(dir),
            blk1.state_.End(dir)}));
  blk1.vol_.Fill(vol_.Slice(dir, {vol_.Start(dir), blk1.vol_.End(dir)}));
  blk1.center_.Fill(center_.Slice(dir, {center_.Start(dir),
            blk1.center_.End(dir)}));
  blk1.cellWidthI_.Fill(cellWidthI_.Slice(dir, {cellWidthI_.Start(dir),
            blk1.cellWidthI_.End(dir)}));
  blk1.cellWidthJ_.Fill(cellWidthJ_.Slice(dir, {cellWidthJ_.Start(dir),
            blk1.cellWidthJ_.End(dir)}));
  blk1.cellWidthK_.Fill(cellWidthK_.Slice(dir, {cellWidthK_.Start(dir),
            blk1.cellWidthK_.End(dir)}));                        
  blk1.wallDist_.Fill(wallDist_.Slice(dir, {wallDist_.Start(dir),
            blk1.wallDist_.End(dir)}));
  blk1.temperature_.Fill(temperature_.Slice(dir, {temperature_.Start(dir),
            blk1.temperature_.End(dir)}));
  if (isViscous_) {
    blk1.viscosity_.Fill(viscosity_.Slice(dir, {viscosity_.Start(dir),
              blk1.viscosity_.End(dir)}));
  }
  if (isTurbulent_) {
    blk1.eddyViscosity_.Fill(
        eddyViscosity_.Slice(dir, {eddyViscosity_.Start(dir),
                blk1.eddyViscosity_.End(dir)}));
  }
  if (isRANS_) {
    blk1.f1_.Fill(f1_.Slice(dir, {f1_.Start(dir), blk1.f1_.End(dir)}));
    blk1.f2_.Fill(f2_.Slice(dir, {f2_.Start(dir), blk1.f2_.End(dir)}));
  }

  // assign cell variables without ghost cells
  blk1.specRadius_.Fill(specRadius_.Slice(dir, {specRadius_.Start(dir),
            blk1.specRadius_.End(dir)}));
  blk1.dt_.Fill(dt_.Slice(dir, {dt_.Start(dir), blk1.dt_.End(dir)}));
  blk1.residual_.Fill(residual_.Slice(dir, {residual_.Start(dir),
            blk1.residual_.End(dir)}));
  blk1.velocityGrad_.Fill(velocityGrad_.Slice(
      dir, {velocityGrad_.Start(dir), blk1.velocityGrad_.End(dir)}));
  blk1.temperatureGrad_.Fill(temperatureGrad_.Slice(
      dir, {temperatureGrad_.Start(dir), blk1.temperatureGrad_.End(dir)}));
  blk1.densityGrad_.Fill(densityGrad_.Slice(
      dir, {densityGrad_.Start(dir), blk1.densityGrad_.End(dir)}));
  blk1.pressureGrad_.Fill(pressureGrad_.Slice(
      dir, {pressureGrad_.Start(dir), blk1.pressureGrad_.End(dir)}));
  if (isRANS_) {
    blk1.tkeGrad_.Fill(tkeGrad_.Slice(dir, {tkeGrad_.Start(dir),
              blk1.tkeGrad_.End(dir)}));
    blk1.omegaGrad_.Fill(omegaGrad_.Slice(dir, {omegaGrad_.Start(dir),
              blk1.omegaGrad_.End(dir)}));
  }
  if (isMultiSpecies_) {
    blk1.mixtureGrad_.Fill(mixtureGrad_.Slice(
        dir, {mixtureGrad_.Start(dir), blk1.mixtureGrad_.End(dir)}));
  }

  // assign face variables
  blk1.fAreaI_.Fill(fAreaI_.Slice(dir, {fAreaI_.Start(dir),
            blk1.fAreaI_.End(dir)}));
  blk1.fAreaJ_.Fill(fAreaJ_.Slice(dir, {fAreaJ_.Start(dir),
            blk1.fAreaJ_.End(dir)}));
  blk1.fAreaK_.Fill(fAreaK_.Slice(dir, {fAreaK_.Start(dir),
            blk1.fAreaK_.End(dir)}));

  blk1.fCenterI_.Fill(fCenterI_.Slice(dir, {fCenterI_.Start(dir),
            blk1.fCenterI_.End(dir)}));
  blk1.fCenterJ_.Fill(fCenterJ_.Slice(dir, {fCenterJ_.Start(dir),
            blk1.fCenterJ_.End(dir)}));
  blk1.fCenterK_.Fill(fCenterK_.Slice(dir, {fCenterK_.Start(dir) ,
            blk1.fCenterK_.End(dir)}));

  // ------------------------------------------------------------------
  // assign variables for upper split
  // assign cell variables with ghost cells
  blk2.state_.Fill(state_.Slice(dir, {ind, state_.End(dir)}));
  blk2.vol_.Fill(vol_.Slice(dir, {ind, vol_.End(dir)}));
  blk2.center_.Fill(center_.Slice(dir, {ind, center_.End(dir)}));
  blk2.cellWidthI_.Fill(cellWidthI_.Slice(dir, {ind, cellWidthI_.End(dir)}));
  blk2.cellWidthJ_.Fill(cellWidthJ_.Slice(dir, {ind, cellWidthJ_.End(dir)}));
  blk2.cellWidthK_.Fill(cellWidthK_.Slice(dir, {ind, cellWidthK_.End(dir)}));
  blk2.wallDist_.Fill(wallDist_.Slice(dir, {ind, wallDist_.End(dir)}));
  blk2.temperature_.Fill(temperature_.Slice(dir, {ind, temperature_.End(dir)}));
  if (isViscous_) {
    blk2.viscosity_.Fill(viscosity_.Slice(dir, {ind, viscosity_.End(dir)}));
  }
  if (isTurbulent_) {
    blk2.eddyViscosity_.Fill(eddyViscosity_.Slice(dir, {ind,
              eddyViscosity_.End(dir)}));
  }
  if (isRANS_) {
    blk2.f1_.Fill(f1_.Slice(dir, {ind, f1_.End(dir)}));
    blk2.f2_.Fill(f2_.Slice(dir, {ind, f2_.End(dir)}));
  }

  // assign cell variables without ghost cells
  blk2.specRadius_.Fill(specRadius_.Slice(dir, {ind, specRadius_.End(dir)}));
  blk2.dt_.Fill(dt_.Slice(dir, {ind, dt_.End(dir)}));
  blk2.residual_.Fill(residual_.Slice(dir, {ind, residual_.End(dir)}));
  blk2.velocityGrad_.Fill(
      velocityGrad_.Slice(dir, {ind, velocityGrad_.End(dir)}));
  blk2.temperatureGrad_.Fill(
      temperatureGrad_.Slice(dir, {ind, temperatureGrad_.End(dir)}));
  blk2.densityGrad_.Fill(
      densityGrad_.Slice(dir, {ind, densityGrad_.End(dir)}));
  blk2.pressureGrad_.Fill(
      pressureGrad_.Slice(dir, {ind, pressureGrad_.End(dir)}));

  if (isRANS_) {
    blk2.tkeGrad_.Fill(tkeGrad_.Slice(dir, {ind, tkeGrad_.End(dir)}));
    blk2.omegaGrad_.Fill(omegaGrad_.Slice(dir, {ind, omegaGrad_.End(dir)}));
  }

  if (isMultiSpecies_) {
    blk2.mixtureGrad_.Fill(
        mixtureGrad_.Slice(dir, {ind, mixtureGrad_.End(dir)}));
  }

  // assign face variables
  blk2.fAreaI_.Fill(fAreaI_.Slice(dir, {ind, fAreaI_.End(dir)}));
  blk2.fAreaJ_.Fill(fAreaJ_.Slice(dir, {ind, fAreaJ_.End(dir)}));
  blk2.fAreaK_.Fill(fAreaK_.Slice(dir, {ind, fAreaK_.End(dir)}));

  blk2.fCenterI_.Fill(fCenterI_.Slice(dir, {ind, fCenterI_.End(dir)}));
  blk2.fCenterJ_.Fill(fCenterJ_.Slice(dir, {ind, fCenterJ_.End(dir)}));
  blk2.fCenterK_.Fill(fCenterK_.Slice(dir, {ind, fCenterK_.End(dir)}));

  // assign boundary conditions
  blk1.bc_ = bound1;
  blk1.wallData_ = wallData_;
  (*this) = blk1;
  blk2.bc_ = bound2;
  blk2.wallData_ = wd2;
  return blk2;
}

/* Member function to join a procBlock along a plane defined by a direction.
 * Assumes that calling instance is lower, and input instance is upper.*/
void procBlock::Join(const procBlock &blk, const string &dir,
                     vector<boundarySurface> &alteredSurf) {
  // blk -- block to join with
  // dir -- plane to split along, either i, j, or k
  // alteredSurf -- vector of surfaces whose partners will need to be altered
  // after this join

  auto iTot = this->NumI();
  auto jTot = this->NumJ();
  auto kTot = this->NumK();

  // for face variables, boundary face is duplicated in lower/upper splits
  // start upper block one index later to avoid duplication
  auto iFaceFac = 0;
  auto jFaceFac = 0;
  auto kFaceFac = 0;

  if (dir == "i") {
    iTot += blk.NumI();
    iFaceFac = 1;
  } else if (dir == "j") {
    jTot += blk.NumJ();
    jFaceFac = 1;
  } else if (dir == "k") {
    kTot += blk.NumK();
    kFaceFac = 1;
  } else {
    cerr << "ERROR: Error in procBlock::Join(). Direction " << dir
         << " is not recognized! Choose either i, j, or k." << endl;
    exit(EXIT_FAILURE);
  }

  procBlock newBlk(iTot, jTot, kTot, numGhosts_, this->NumEquations(),
                   this->NumSpecies(), isViscous_, isTurbulent_, isRANS_,
                   storeTimeN_, isMultiLevelTime_, isMultiSpecies_);

  newBlk.bc_ = bc_;
  newBlk.bc_.Join(blk.bc_, dir, alteredSurf);
  newBlk.wallData_ = wallData_;
  newBlk.JoinWallData(blk.wallData_, dir);
  newBlk.nodes_ = nodes_;
  newBlk.nodes_.Join(blk.nodes_, dir);

  // assign variables from lower block -----------------------------
  // assign cell variables with ghost cells

  newBlk.state_.Insert(dir, {state_.Start(dir), state_.PhysEnd(dir)},
                       state_.Slice(dir, {state_.Start(dir),
                               state_.PhysEnd(dir)}));

  newBlk.vol_.Insert(dir, {vol_.Start(dir), vol_.PhysEnd(dir)},
                     vol_.Slice(dir, {vol_.Start(dir), vol_.PhysEnd(dir)}));

  newBlk.center_.Insert(dir, {center_.Start(dir), center_.PhysEnd(dir)},
                        center_.Slice(dir, {center_.Start(dir),
                                center_.PhysEnd(dir)}));

  newBlk.cellWidthI_.Insert(dir, {cellWidthI_.Start(dir), cellWidthI_.PhysEnd(dir)},
                          cellWidthI_.Slice(dir, {cellWidthI_.Start(dir),
                                  cellWidthI_.PhysEnd(dir)}));
  newBlk.cellWidthJ_.Insert(dir, {cellWidthJ_.Start(dir), cellWidthJ_.PhysEnd(dir)},
                          cellWidthJ_.Slice(dir, {cellWidthJ_.Start(dir),
                                  cellWidthJ_.PhysEnd(dir)}));
  newBlk.cellWidthK_.Insert(dir, {cellWidthK_.Start(dir), cellWidthK_.PhysEnd(dir)},
                          cellWidthK_.Slice(dir, {cellWidthK_.Start(dir),
                                  cellWidthK_.PhysEnd(dir)}));                                    


  newBlk.wallDist_.Insert(dir, {wallDist_.Start(dir), wallDist_.PhysEnd(dir)},
                          wallDist_.Slice(dir, {wallDist_.Start(dir),
                                  wallDist_.PhysEnd(dir)}));

  newBlk.temperature_.Insert(dir, {temperature_.Start(dir),
          temperature_.PhysEnd(dir)},
    temperature_.Slice(dir, {temperature_.Start(dir),
            temperature_.PhysEnd(dir)}));

  if (isViscous_) {
    newBlk.viscosity_.Insert(dir, {viscosity_.Start(dir),
            viscosity_.PhysEnd(dir)},
      viscosity_.Slice(dir, {viscosity_.Start(dir), viscosity_.PhysEnd(dir)}));
  }
  if (isTurbulent_) {
    newBlk.eddyViscosity_.Insert(dir, {eddyViscosity_.Start(dir),
            eddyViscosity_.PhysEnd(dir)},
      eddyViscosity_.Slice(
          dir, {eddyViscosity_.Start(dir), eddyViscosity_.PhysEnd(dir)}));
  }
  if (isRANS_) {
    newBlk.f1_.Insert(dir, {f1_.Start(dir), f1_.PhysEnd(dir)},
                      f1_.Slice(dir, {f1_.Start(dir), f1_.PhysEnd(dir)}));
    newBlk.f2_.Insert(dir, {f2_.Start(dir), f2_.PhysEnd(dir)},
                      f2_.Slice(dir, {f2_.Start(dir), f2_.PhysEnd(dir)}));
  }

  // assign cell variables without ghost cells
  newBlk.specRadius_.Insert(dir, {specRadius_.Start(dir),
          specRadius_.PhysEnd(dir)},
    specRadius_.Slice(dir, {specRadius_.Start(dir), specRadius_.PhysEnd(dir)}));

  newBlk.dt_.Insert(dir, {dt_.Start(dir), dt_.PhysEnd(dir)},
                    dt_.Slice(dir, {dt_.Start(dir), dt_.PhysEnd(dir)}));

  newBlk.residual_.Insert(dir, {residual_.Start(dir), residual_.PhysEnd(dir)},
                          residual_.Slice(dir, {residual_.Start(dir),
                                  residual_.PhysEnd(dir)}));

  newBlk.velocityGrad_.Insert(
      dir, {velocityGrad_.Start(dir), velocityGrad_.PhysEnd(dir)},
      velocityGrad_.Slice(
          dir, {velocityGrad_.Start(dir), velocityGrad_.PhysEnd(dir)}));

  newBlk.temperatureGrad_.Insert(
      dir, {temperatureGrad_.Start(dir), temperatureGrad_.PhysEnd(dir)},
      temperatureGrad_.Slice(
          dir, {temperatureGrad_.Start(dir), temperatureGrad_.PhysEnd(dir)}));

  newBlk.densityGrad_.Insert(
      dir, {densityGrad_.Start(dir), densityGrad_.PhysEnd(dir)},
      densityGrad_.Slice(
          dir, {densityGrad_.Start(dir), densityGrad_.PhysEnd(dir)}));

  newBlk.pressureGrad_.Insert(
      dir, {pressureGrad_.Start(dir), pressureGrad_.PhysEnd(dir)},
      pressureGrad_.Slice(
          dir, {pressureGrad_.Start(dir), pressureGrad_.PhysEnd(dir)}));

  if (isRANS_) {
    newBlk.tkeGrad_.Insert(dir, {tkeGrad_.Start(dir), tkeGrad_.PhysEnd(dir)},
                           tkeGrad_.Slice(dir, {tkeGrad_.Start(dir),
                                   tkeGrad_.PhysEnd(dir)}));

    newBlk.omegaGrad_.Insert(dir, {omegaGrad_.Start(dir),
            omegaGrad_.PhysEnd(dir)},
      omegaGrad_.Slice(dir, {omegaGrad_.Start(dir), omegaGrad_.PhysEnd(dir)}));
  }

  if (isMultiSpecies_) {
    newBlk.mixtureGrad_.Insert(
        dir, {mixtureGrad_.Start(dir), mixtureGrad_.PhysEnd(dir)},
        mixtureGrad_.Slice(
            dir, {mixtureGrad_.Start(dir), mixtureGrad_.PhysEnd(dir)}));
  }

  // assign face variables
  newBlk.fAreaI_.Insert(dir, {fAreaI_.Start(dir), fAreaI_.PhysEnd(dir)},
                        fAreaI_.Slice(dir, {fAreaI_.Start(dir),
                                fAreaI_.PhysEnd(dir)}));

  newBlk.fAreaJ_.Insert(dir, {fAreaJ_.Start(dir), fAreaJ_.PhysEnd(dir)},
                        fAreaJ_.Slice(dir, {fAreaJ_.Start(dir),
                                fAreaJ_.PhysEnd(dir)}));

  newBlk.fAreaK_.Insert(dir, {fAreaK_.Start(dir), fAreaK_.PhysEnd(dir)},
                        fAreaK_.Slice(dir, {fAreaK_.Start(dir),
                                fAreaK_.PhysEnd(dir)}));

  newBlk.fCenterI_.Insert(dir, {fCenterI_.Start(dir), fCenterI_.PhysEnd(dir)},
                          fCenterI_.Slice(dir, {fCenterI_.Start(dir),
                                  fCenterI_.PhysEnd(dir)}));

  newBlk.fCenterJ_.Insert(dir, {fCenterJ_.Start(dir), fCenterJ_.PhysEnd(dir)},
                          fCenterJ_.Slice(dir, {fCenterJ_.Start(dir),
                                  fCenterJ_.PhysEnd(dir)}));

  newBlk.fCenterK_.Insert(dir, {fCenterK_.Start(dir), fCenterK_.PhysEnd(dir)},
                          fCenterK_.Slice(dir, {fCenterK_.Start(dir),
                                  fCenterK_.PhysEnd(dir)}));

  // assign variables from upper block -------------------------------------
  // assign cell variables with ghost cells
  newBlk.state_.Insert(dir, {state_.PhysEnd(dir), newBlk.state_.End(dir)},
                       blk.state_.Slice(dir, {blk.state_.PhysStart(dir),
                               blk.state_.End(dir)}));

  newBlk.vol_.Insert(dir, {vol_.PhysEnd(dir), newBlk.vol_.End(dir)},
                     blk.vol_.Slice(dir, {blk.vol_.PhysStart(dir),
                             blk.vol_.End(dir)}));

  newBlk.center_.Insert(dir, {center_.PhysEnd(dir), newBlk.center_.End(dir)},
                        blk.center_.Slice(dir, {blk.center_.PhysStart(dir),
                                blk.center_.End(dir)}));

  newBlk.cellWidthI_.Insert(dir, {cellWidthI_.PhysEnd(dir),
          newBlk.cellWidthI_.End(dir)},
    blk.cellWidthI_.Slice(dir, {blk.cellWidthI_.PhysStart(dir),
            blk.cellWidthI_.End(dir)}));
  newBlk.cellWidthJ_.Insert(dir, {cellWidthJ_.PhysEnd(dir),
          newBlk.cellWidthJ_.End(dir)},
    blk.cellWidthJ_.Slice(dir, {blk.cellWidthJ_.PhysStart(dir),
            blk.cellWidthJ_.End(dir)}));
  newBlk.cellWidthK_.Insert(dir, {cellWidthK_.PhysEnd(dir),
          newBlk.cellWidthK_.End(dir)},
    blk.cellWidthK_.Slice(dir, {blk.cellWidthK_.PhysStart(dir),
            blk.cellWidthK_.End(dir)}));

  newBlk.wallDist_.Insert(dir, {wallDist_.PhysEnd(dir),
          newBlk.wallDist_.End(dir)},
    blk.wallDist_.Slice(dir, {blk.wallDist_.PhysStart(dir),
            blk.wallDist_.End(dir)}));

  newBlk.temperature_.Insert(dir, {temperature_.PhysEnd(dir),
          newBlk.temperature_.End(dir)},
    blk.temperature_.Slice(dir, {blk.temperature_.PhysStart(dir),
            blk.temperature_.End(dir)}));

  if (isViscous_) {
    newBlk.viscosity_.Insert(dir, {viscosity_.PhysEnd(dir),
            newBlk.viscosity_.End(dir)},
      blk.viscosity_.Slice(dir, {blk.viscosity_.PhysStart(dir),
              blk.viscosity_.End(dir)}));
  }
  if (isTurbulent_) {
    newBlk.eddyViscosity_.Insert(dir, {eddyViscosity_.PhysEnd(dir),
            newBlk.eddyViscosity_.End(dir)},
      blk.eddyViscosity_.Slice(dir, {blk.eddyViscosity_.PhysStart(dir),
              blk.eddyViscosity_.End(dir)}));
  }
  if (isRANS_) {
    newBlk.f1_.Insert(dir, {f1_.PhysEnd(dir), newBlk.f1_.End(dir)},
                      blk.f1_.Slice(dir, {blk.f1_.PhysStart(dir),
                              blk.f1_.End(dir)}));

    newBlk.f2_.Insert(dir, {f2_.PhysEnd(dir), newBlk.f2_.End(dir)},
                      blk.f2_.Slice(dir, {blk.f2_.PhysStart(dir),
                              blk.f2_.End(dir)}));
  }

  // assign cell variables without ghost cells
  newBlk.specRadius_.Insert(dir, {specRadius_.PhysEnd(dir),
          newBlk.specRadius_.End(dir)},
    blk.specRadius_.Slice(dir, {blk.specRadius_.PhysStart(dir),
            blk.specRadius_.End(dir)}));

  newBlk.dt_.Insert(dir, {dt_.PhysEnd(dir), newBlk.dt_.End(dir)},
                    blk.dt_.Slice(dir, {blk.dt_.PhysStart(dir),
                            blk.dt_.End(dir)}));

  newBlk.residual_.Insert(dir, {residual_.PhysEnd(dir),
          newBlk.residual_.End(dir)},
    blk.residual_.Slice(dir, {blk.residual_.PhysStart(dir),
            blk.residual_.End(dir)}));

  newBlk.velocityGrad_.Insert(
      dir, {velocityGrad_.PhysEnd(dir), newBlk.velocityGrad_.End(dir)},
      blk.velocityGrad_.Slice(
          dir, {blk.velocityGrad_.PhysStart(dir), blk.velocityGrad_.End(dir)}));

  newBlk.temperatureGrad_.Insert(
      dir, {temperatureGrad_.PhysEnd(dir), newBlk.temperatureGrad_.End(dir)},
      blk.temperatureGrad_.Slice(dir, {blk.temperatureGrad_.PhysStart(dir),
                                       blk.temperatureGrad_.End(dir)}));

  newBlk.densityGrad_.Insert(
      dir, {densityGrad_.PhysEnd(dir), newBlk.densityGrad_.End(dir)},
      blk.densityGrad_.Slice(dir, {blk.densityGrad_.PhysStart(dir),
                                       blk.densityGrad_.End(dir)}));

  newBlk.pressureGrad_.Insert(
      dir, {pressureGrad_.PhysEnd(dir), newBlk.pressureGrad_.End(dir)},
      blk.pressureGrad_.Slice(dir, {blk.pressureGrad_.PhysStart(dir),
                                       blk.pressureGrad_.End(dir)}));

  if (isRANS_) {
    newBlk.tkeGrad_.Insert(dir, {tkeGrad_.PhysEnd(dir),
            newBlk.tkeGrad_.End(dir)},
      blk.tkeGrad_.Slice(dir, {blk.tkeGrad_.PhysStart(dir),
              blk.tkeGrad_.End(dir)}));

    newBlk.omegaGrad_.Insert(dir, {omegaGrad_.PhysEnd(dir),
            newBlk.omegaGrad_.End(dir)},
      blk.omegaGrad_.Slice(dir, {blk.omegaGrad_.PhysStart(dir),
              blk.omegaGrad_.End(dir)}));
  }

  if (isMultiSpecies_) {
    newBlk.mixtureGrad_.Insert(
        dir, {mixtureGrad_.PhysEnd(dir), newBlk.mixtureGrad_.End(dir)},
        blk.mixtureGrad_.Slice(
            dir, {blk.mixtureGrad_.PhysStart(dir), blk.mixtureGrad_.End(dir)}));
  }

  // assign face variables
  newBlk.fAreaI_.Insert(dir, {fAreaI_.PhysEnd(dir), newBlk.fAreaI_.End(dir)},
                        blk.fAreaI_.Slice(dir,
                                          {blk.fAreaI_.PhysStart(dir) + iFaceFac,
                                                blk.fAreaI_.End(dir)}));

  newBlk.fAreaJ_.Insert(dir, {fAreaJ_.PhysEnd(dir), newBlk.fAreaJ_.End(dir)},
                        blk.fAreaJ_.Slice(dir,
                                          {blk.fAreaJ_.PhysStart(dir) + jFaceFac,
                                                blk.fAreaJ_.End(dir)}));

  newBlk.fAreaK_.Insert(dir, {fAreaK_.PhysEnd(dir), newBlk.fAreaK_.End(dir)},
                        blk.fAreaK_.Slice(dir,
                                          {blk.fAreaK_.PhysStart(dir) + kFaceFac,
                                                blk.fAreaK_.End(dir)}));

  newBlk.fCenterI_.Insert(dir, {fCenterI_.PhysEnd(dir),
          newBlk.fCenterI_.End(dir)},
    blk.fCenterI_.Slice(dir, {blk.fCenterI_.PhysStart(dir) + iFaceFac,
            blk.fCenterI_.End(dir)}));

  newBlk.fCenterJ_.Insert(dir, {fCenterJ_.PhysEnd(dir),
          newBlk.fCenterJ_.End(dir)},
    blk.fCenterJ_.Slice(dir, {blk.fCenterJ_.PhysStart(dir) + jFaceFac,
            blk.fCenterJ_.End(dir)}));

  newBlk.fCenterK_.Insert(dir, {fCenterK_.PhysEnd(dir),
          newBlk.fCenterK_.End(dir)},
    blk.fCenterK_.Slice(dir, {blk.fCenterK_.PhysStart(dir) + kFaceFac,
            blk.fCenterK_.End(dir)}));

  *this = newBlk;
}

void procBlock::CalcGradsI(const int &ii, const int &jj, const int &kk,
                           tensor<double> &velGrad, vector3d<double> &tGrad,
                           vector3d<double> &dGrad, vector3d<double> &pGrad,
                           vector3d<double> &tkeGrad,
                           vector3d<double> &omegaGrad,
                           vector<vector3d<double>> &mixGrad) const {
  // ii -- i-index for face (including ghosts)
  // jj -- j-index for face (including ghosts)
  // kk -- k-index for face (including ghosts)
  // velGrad -- tensor to store velocity gradient
  // tGrad -- vector3d to store temperature gradient
  // dGrad -- vector3d to store density gradient
  // pGrad -- vector3d to store pressure gradient
  // tkeGrad -- vector3d to store tke gradient
  // omegaGrad -- vector3d to store omega gradient
  // mixGrad -- mixture gradients

  // calculate areas of faces in alternate control volume
  const auto aiu = 0.5 * (fAreaI_(ii, jj, kk).Vector() +
                          fAreaI_(ii + 1, jj, kk).Vector());
  const auto ail = 0.5 * (fAreaI_(ii, jj, kk).Vector() +
                          fAreaI_(ii - 1, jj, kk).Vector());

  const auto aju = 0.5 * (fAreaJ_(ii, jj + 1, kk).Vector() +
                          fAreaJ_(ii - 1, jj + 1, kk).Vector());
  const auto ajl = 0.5 * (fAreaJ_(ii, jj, kk).Vector() +
                          fAreaJ_(ii - 1, jj, kk).Vector());

  const auto aku = 0.5 * (fAreaK_(ii, jj, kk + 1).Vector() +
                          fAreaK_(ii - 1, jj, kk + 1).Vector());
  const auto akl = 0.5 * (fAreaK_(ii, jj, kk).Vector() +
                          fAreaK_(ii - 1, jj, kk).Vector());

  // calculate volume of alternate control volume
  const auto vol = 0.5 * (vol_(ii - 1, jj, kk) + vol_(ii, jj, kk));

  // calculate average velocity on j and k faces of alternate control volume
  const auto vju = 0.25 *
      (state_(ii - 1, jj, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj + 1, kk).Velocity() +
       state_(ii - 1, jj + 1, kk).Velocity());
  const auto vjl = 0.25 *
      (state_(ii - 1, jj, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj - 1, kk).Velocity() +
       state_(ii - 1, jj - 1, kk).Velocity());

  const auto vku = 0.25 *
      (state_(ii - 1, jj, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj, kk + 1).Velocity() +
       state_(ii - 1, jj, kk + 1).Velocity());
  const auto vkl = 0.25 *
      (state_(ii - 1, jj, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj, kk - 1).Velocity() +
       state_(ii - 1, jj, kk - 1).Velocity());

  // Get velocity gradient at face
  velGrad = VectorGradGG(state_(ii - 1, jj, kk).Velocity(),
                         state_(ii, jj, kk).Velocity(), vjl, vju, vkl, vku,
                         ail, aiu, ajl, aju, akl, aku, vol);

  // calculate average density on j and k faces of alternate control volume
  const auto dju = 0.25 * (state_(ii - 1, jj, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj + 1, kk).Rho() +
                           state_(ii - 1, jj + 1, kk).Rho());
  const auto djl = 0.25 * (state_(ii - 1, jj, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj - 1, kk).Rho() +
                           state_(ii - 1, jj - 1, kk).Rho());

  const auto dku = 0.25 * (state_(ii - 1, jj, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj, kk + 1).Rho() +
                           state_(ii - 1, jj, kk + 1).Rho());
  const auto dkl = 0.25 * (state_(ii - 1, jj, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj, kk - 1).Rho() +
                           state_(ii - 1, jj, kk - 1).Rho());

  // Get density gradient at face
  dGrad = ScalarGradGG(state_(ii - 1, jj, kk).Rho(),
                       state_(ii, jj, kk).Rho(), djl, dju,
                       dkl, dku, ail, aiu, ajl, aju, akl, aku, vol);

  // calculate average pressure on j and k faces of alternate control volume
  const auto pju = 0.25 * (state_(ii - 1, jj, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj + 1, kk).P() +
                           state_(ii - 1, jj + 1, kk).P());
  const auto pjl = 0.25 * (state_(ii - 1, jj, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj - 1, kk).P() +
                           state_(ii - 1, jj - 1, kk).P());

  const auto pku = 0.25 * (state_(ii - 1, jj, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj, kk + 1).P() +
                           state_(ii - 1, jj, kk + 1).P());
  const auto pkl = 0.25 * (state_(ii - 1, jj, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj, kk - 1).P() +
                           state_(ii - 1, jj, kk - 1).P());

  // Get pressure gradient at face
  pGrad = ScalarGradGG(state_(ii - 1, jj, kk).P(),
                       state_(ii, jj, kk).P(), pjl, pju,
                       pkl, pku, ail, aiu, ajl, aju, akl, aku, vol);

  // calculate average temperature on j and k faces of alternate control volume
  const auto tju = 0.25 * (temperature_(ii - 1, jj, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj + 1, kk) +
                           temperature_(ii - 1, jj + 1, kk));
  const auto tjl = 0.25 * (temperature_(ii - 1, jj, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj - 1, kk) +
                           temperature_(ii - 1, jj - 1, kk));

  const auto tku = 0.25 * (temperature_(ii - 1, jj, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj, kk + 1) +
                           temperature_(ii - 1, jj, kk + 1));
  const auto tkl = 0.25 * (temperature_(ii - 1, jj, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj, kk - 1) +
                           temperature_(ii - 1, jj, kk - 1));

  // Get temperature gradient at face
  tGrad = ScalarGradGG(temperature_(ii - 1, jj, kk),
                       temperature_(ii, jj, kk), tjl, tju,
                       tkl, tku, ail, aiu, ajl, aju, akl, aku, vol);

  if (isRANS_) {
    // calculate average tke on j and k faces of alternate control volume
    const auto tkeju = 0.25 *
        (state_(ii - 1, jj, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj + 1, kk).Tke() + state_(ii - 1, jj + 1, kk).Tke());
    const auto tkejl = 0.25 *
        (state_(ii - 1, jj, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj - 1, kk).Tke() + state_(ii - 1, jj - 1, kk).Tke());

    const auto tkeku = 0.25 *
        (state_(ii - 1, jj, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj, kk + 1).Tke() + state_(ii - 1, jj, kk + 1).Tke());
    const auto tkekl = 0.25 *
        (state_(ii - 1, jj, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj, kk - 1).Tke() + state_(ii - 1, jj, kk - 1).Tke());

    // Get tke gradient at face
    tkeGrad = ScalarGradGG(state_(ii - 1, jj, kk).Tke(),
                           state_(ii, jj, kk).Tke(), tkejl, tkeju, tkekl,
                           tkeku, ail, aiu, ajl, aju, akl, aku, vol);

    // calculate average Omega on j and k faces of alternate control volume
    const auto omgju = 0.25 *
        (state_(ii - 1, jj, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj + 1, kk).Omega() + state_(ii - 1, jj + 1, kk).Omega());
    const auto omgjl = 0.25 *
        (state_(ii - 1, jj, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj - 1, kk).Omega() + state_(ii - 1, jj - 1, kk).Omega());

    const auto omgku = 0.25 *
        (state_(ii - 1, jj, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj, kk + 1).Omega() + state_(ii - 1, jj, kk + 1).Omega());
    const auto omgkl = 0.25 *
        (state_(ii - 1, jj, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj, kk - 1).Omega() + state_(ii - 1, jj, kk - 1).Omega());

    // Get omega gradient at face
    omegaGrad = ScalarGradGG(
        state_(ii - 1, jj, kk).Omega(), state_(ii, jj, kk).Omega(), omgjl,
        omgju, omgkl, omgku, ail, aiu, ajl, aju, akl, aku, vol);
  }

  if (isMultiSpecies_) {
    mixGrad.resize(this->NumSpecies());
    for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
      // calculate average mf on j and k faces of alternate control volume
      const auto mfju = 0.25 * (state_(ii - 1, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj + 1, kk).MassFractionN(ss) +
                                state_(ii - 1, jj + 1, kk).MassFractionN(ss));
      const auto mfjl = 0.25 * (state_(ii - 1, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj - 1, kk).MassFractionN(ss) +
                                state_(ii - 1, jj - 1, kk).MassFractionN(ss));

      const auto mfku = 0.25 * (state_(ii - 1, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk + 1).MassFractionN(ss) +
                                state_(ii - 1, jj, kk + 1).MassFractionN(ss));
      const auto mfkl = 0.25 * (state_(ii - 1, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk - 1).MassFractionN(ss) +
                                state_(ii - 1, jj, kk - 1).MassFractionN(ss));

      // Get tke gradient at face
      mixGrad[ss] =
          ScalarGradGG(state_(ii - 1, jj, kk).MassFractionN(ss),
                       state_(ii, jj, kk).MassFractionN(ss), mfjl, mfju, mfkl,
                       mfku, ail, aiu, ajl, aju, akl, aku, vol);
    }
  }
}

void procBlock::CalcGradsJ(const int &ii, const int &jj, const int &kk,
                           tensor<double> &velGrad, vector3d<double> &tGrad,
                           vector3d<double> &dGrad, vector3d<double> &pGrad,
                           vector3d<double> &tkeGrad,
                           vector3d<double> &omegaGrad,
                           vector<vector3d<double>> &mixGrad) const {
  // ii -- i-index for face (including ghosts)
  // jj -- j-index for face (including ghosts)
  // kk -- k-index for face (including ghosts)
  // velGrad -- tensor to store velocity gradient
  // tGrad -- vector3d to store temperature gradient
  // dGrad -- vector3d to store density gradient
  // pGrad -- vector3d to store pressure gradient
  // tkeGrad -- vector3d to store tke gradient
  // omegaGrad -- vector3d to store omega gradient
  // mixGrad -- mixture gradients

  // calculate areas of faces in alternate control volume
  const auto aju = 0.5 * (fAreaJ_(ii, jj, kk).Vector() +
                          fAreaJ_(ii, jj + 1, kk).Vector());
  const auto ajl = 0.5 * (fAreaJ_(ii, jj, kk).Vector() +
                          fAreaJ_(ii, jj - 1, kk).Vector());

  const auto aiu = 0.5 * (fAreaI_(ii + 1, jj, kk).Vector() +
                          fAreaI_(ii + 1, jj - 1, kk).Vector());
  const auto ail = 0.5 * (fAreaI_(ii, jj, kk).Vector() +
                          fAreaI_(ii, jj - 1, kk).Vector());

  const auto aku = 0.5 * (fAreaK_(ii, jj, kk + 1).Vector() +
                          fAreaK_(ii, jj - 1, kk + 1).Vector());
  const auto akl = 0.5 * (fAreaK_(ii, jj, kk).Vector() +
                          fAreaK_(ii, jj - 1, kk).Vector());

  // calculate volume of alternate control volume
  const auto vol = 0.5 * (vol_(ii, jj - 1, kk) + vol_(ii, jj, kk));

  // calculate average velocity on i and k faces of alternate control volume
  const auto viu = 0.25 *
      (state_(ii, jj - 1, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii + 1, jj, kk).Velocity() +
       state_(ii + 1, jj - 1, kk).Velocity());
  const auto vil = 0.25 *
      (state_(ii, jj - 1, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii - 1, jj, kk).Velocity() +
       state_(ii - 1, jj - 1, kk).Velocity());

  const auto vku = 0.25 *
      (state_(ii, jj - 1, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj, kk + 1).Velocity() +
       state_(ii, jj - 1, kk + 1).Velocity());
  const auto vkl = 0.25 *
      (state_(ii, jj - 1, kk).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj, kk - 1).Velocity() +
       state_(ii, jj - 1, kk - 1).Velocity());

  // Get velocity gradient at face
  velGrad = VectorGradGG(vil, viu, state_(ii, jj - 1, kk).Velocity(),
                         state_(ii, jj, kk).Velocity(), vkl, vku, ail, aiu,
                         ajl, aju, akl, aku, vol);

  // calculate average density on i and k faces of alternate control volume
  const auto diu = 0.25 * (state_(ii, jj - 1, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii + 1, jj, kk).Rho() +
                           state_(ii + 1, jj - 1, kk).Rho());
  const auto dil = 0.25 * (state_(ii, jj - 1, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii - 1, jj, kk).Rho() +
                           state_(ii - 1, jj - 1, kk).Rho());

  const auto dku = 0.25 * (state_(ii, jj - 1, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj, kk + 1).Rho() +
                           state_(ii, jj - 1, kk + 1).Rho());
  const auto dkl = 0.25 * (state_(ii, jj - 1, kk).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj, kk - 1).Rho() +
                           state_(ii, jj - 1, kk - 1).Rho());

  // Get density gradient at face
  dGrad = ScalarGradGG(dil, diu, state_(ii, jj - 1, kk).Rho(),
                       state_(ii, jj, kk).Rho(), dkl, dku,
                       ail, aiu, ajl, aju, akl, aku, vol);

  // calculate average pressure on i and k faces of alternate control volume
  const auto piu = 0.25 * (state_(ii, jj - 1, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii + 1, jj, kk).P() +
                           state_(ii + 1, jj - 1, kk).P());
  const auto pil = 0.25 * (state_(ii, jj - 1, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii - 1, jj, kk).P() +
                           state_(ii - 1, jj - 1, kk).P());

  const auto pku = 0.25 * (state_(ii, jj - 1, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj, kk + 1).P() +
                           state_(ii, jj - 1, kk + 1).P());
  const auto pkl = 0.25 * (state_(ii, jj - 1, kk).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj, kk - 1).P() +
                           state_(ii, jj - 1, kk - 1).P());

  // Get pressure gradient at face
  pGrad = ScalarGradGG(pil, piu, state_(ii, jj - 1, kk).P(),
                       state_(ii, jj, kk).P(), pkl, pku,
                       ail, aiu, ajl, aju, akl, aku, vol);


  // calculate average temperature on i and k faces of alternate control volume
  const auto tiu = 0.25 * (temperature_(ii, jj - 1, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii + 1, jj, kk) +
                           temperature_(ii + 1, jj - 1, kk));
  const auto til = 0.25 * (temperature_(ii, jj - 1, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii - 1, jj, kk) +
                           temperature_(ii - 1, jj - 1, kk));

  const auto tku = 0.25 * (temperature_(ii, jj - 1, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj, kk + 1) +
                           temperature_(ii, jj - 1, kk + 1));
  const auto tkl = 0.25 * (temperature_(ii, jj - 1, kk) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj, kk - 1) +
                           temperature_(ii, jj - 1, kk - 1));

  // Get temperature gradient at face
  tGrad = ScalarGradGG(til, tiu, temperature_(ii, jj - 1, kk),
                       temperature_(ii, jj, kk), tkl, tku,
                       ail, aiu, ajl, aju, akl, aku, vol);

  if (isRANS_) {
    // calculate average tke on i and k faces of alternate control volume
    const auto tkeiu = 0.25 *
        (state_(ii, jj - 1, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii + 1, jj, kk).Tke() + state_(ii + 1, jj - 1, kk).Tke());
    const auto tkeil = 0.25 *
        (state_(ii, jj - 1, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii - 1, jj, kk).Tke() + state_(ii - 1, jj - 1, kk).Tke());

    const auto tkeku = 0.25 *
        (state_(ii, jj - 1, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj, kk + 1).Tke() + state_(ii, jj - 1, kk + 1).Tke());
    const auto tkekl = 0.25 *
        (state_(ii, jj - 1, kk).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj, kk - 1).Tke() + state_(ii, jj - 1, kk - 1).Tke());

    // Get temperature gradient at face
    tkeGrad = ScalarGradGG(tkeil, tkeiu, state_(ii, jj - 1, kk).Tke(),
                           state_(ii, jj, kk).Tke(), tkekl, tkeku, ail, aiu,
                           ajl, aju, akl, aku, vol);

    // calculate average omega on i and k faces of alternate control volume
    const auto omgiu = 0.25 *
        (state_(ii, jj - 1, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii + 1, jj, kk).Omega() + state_(ii + 1, jj - 1, kk).Omega());
    const auto omgil = 0.25 *
        (state_(ii, jj - 1, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii - 1, jj, kk).Omega() + state_(ii - 1, jj - 1, kk).Omega());

    const auto omgku = 0.25 *
        (state_(ii, jj - 1, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj, kk + 1).Omega() + state_(ii, jj - 1, kk + 1).Omega());
    const auto omgkl = 0.25 *
        (state_(ii, jj - 1, kk).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj, kk - 1).Omega() + state_(ii, jj - 1, kk - 1).Omega());

    // Get temperature gradient at face
    omegaGrad = ScalarGradGG(omgil, omgiu, state_(ii, jj - 1, kk).Omega(),
                             state_(ii, jj, kk).Omega(), omgkl, omgku, ail,
                             aiu, ajl, aju, akl, aku, vol);
  }

  if (isMultiSpecies_) {
    mixGrad.resize(this->NumSpecies());
    for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
      // calculate average mf on i and k faces of alternate control volume
      const auto mfiu = 0.25 * (state_(ii, jj - 1, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii + 1, jj, kk).MassFractionN(ss) +
                                state_(ii + 1, jj - 1, kk).MassFractionN(ss));
      const auto mfil = 0.25 * (state_(ii, jj - 1, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii - 1, jj, kk).MassFractionN(ss) +
                                state_(ii - 1, jj - 1, kk).MassFractionN(ss));

      const auto mfku = 0.25 * (state_(ii, jj - 1, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk + 1).MassFractionN(ss) +
                                state_(ii, jj - 1, kk + 1).MassFractionN(ss));
      const auto mfkl = 0.25 * (state_(ii, jj - 1, kk).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj, kk - 1).MassFractionN(ss) +
                                state_(ii, jj - 1, kk - 1).MassFractionN(ss));

      // Get temperature gradient at face
      mixGrad[ss] =
          ScalarGradGG(mfil, mfiu, state_(ii, jj - 1, kk).MassFractionN(ss),
                       state_(ii, jj, kk).MassFractionN(ss), mfkl, mfku, ail,
                       aiu, ajl, aju, akl, aku, vol);
    }
  }
}

void procBlock::CalcGradsK(const int &ii, const int &jj, const int &kk,
                           tensor<double> &velGrad, vector3d<double> &tGrad,
                           vector3d<double> &dGrad, vector3d<double> &pGrad,
                           vector3d<double> &tkeGrad,
                           vector3d<double> &omegaGrad,
                           vector<vector3d<double>> &mixGrad) const {
  // ii -- i-index for face (including ghosts)
  // jj -- j-index for face (including ghosts)
  // kk -- k-index for face (including ghosts)
  // velGrad -- tensor to store velocity gradient
  // tGrad -- vector3d to store temperature gradient
  // dGrad -- vector3d to store density gradient
  // pGrad -- vector3d to store pressure gradient
  // tkeGrad -- vector3d to store tke gradient
  // omegaGrad -- vector3d to store omega gradient
  // mixGrad -- mixture gradients

  // calculate areas of faces in alternate control volume
  const auto aku = 0.5 * (fAreaK_(ii, jj, kk).Vector() +
                          fAreaK_(ii, jj, kk + 1).Vector());
  const auto akl = 0.5 * (fAreaK_(ii, jj, kk).Vector() +
                          fAreaK_(ii, jj, kk - 1).Vector());

  const auto aiu = 0.5 * (fAreaI_(ii + 1, jj, kk).Vector() +
                          fAreaI_(ii + 1, jj, kk - 1).Vector());
  const auto ail = 0.5 * (fAreaI_(ii, jj, kk).Vector() +
                          fAreaI_(ii, jj, kk - 1).Vector());

  const auto aju = 0.5 * (fAreaJ_(ii, jj + 1, kk).Vector() +
                          fAreaJ_(ii, jj + 1, kk - 1).Vector());
  const auto ajl = 0.5 * (fAreaJ_(ii, jj, kk).Vector() +
                          fAreaJ_(ii, jj, kk - 1).Vector());

  // calculate volume of alternate control volume
  const auto vol = 0.5 * (vol_(ii, jj, kk - 1) + vol_(ii, jj, kk));

  // calculate average velocity on i and j faces of alternate control volume
  const auto viu = 0.25 *
      (state_(ii, jj, kk - 1).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii + 1, jj, kk).Velocity() +
       state_(ii + 1, jj, kk - 1).Velocity());
  const auto vil = 0.25 *
      (state_(ii, jj, kk - 1).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii - 1, jj, kk).Velocity() +
       state_(ii - 1, jj, kk - 1).Velocity());

  const auto vju = 0.25 *
      (state_(ii, jj, kk - 1).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj + 1, kk).Velocity() +
       state_(ii, jj + 1, kk - 1).Velocity());
  const auto vjl = 0.25 *
      (state_(ii, jj, kk - 1).Velocity() + state_(ii, jj, kk).Velocity() +
       state_(ii, jj - 1, kk).Velocity() +
       state_(ii, jj - 1, kk - 1).Velocity());

  // Get velocity gradient at face
  velGrad = VectorGradGG(vil, viu, vjl, vju, state_(ii, jj, kk - 1).Velocity(),
                         state_(ii, jj, kk).Velocity(), ail, aiu, ajl, aju,
                         akl, aku, vol);

  // calculate average density on i and j faces of alternate control volume
  const auto diu = 0.25 * (state_(ii, jj, kk - 1).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii + 1, jj, kk).Rho() +
                           state_(ii + 1, jj, kk - 1).Rho());
  const auto dil = 0.25 * (state_(ii, jj, kk - 1).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii - 1, jj, kk).Rho() +
                           state_(ii - 1, jj, kk - 1).Rho());

  const auto dju = 0.25 * (state_(ii, jj, kk - 1).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj + 1, kk).Rho() +
                           state_(ii, jj + 1, kk - 1).Rho());
  const auto djl = 0.25 * (state_(ii, jj, kk - 1).Rho() +
                           state_(ii, jj, kk).Rho() +
                           state_(ii, jj - 1, kk).Rho() +
                           state_(ii, jj - 1, kk - 1).Rho());

  // Get density gradient at face
  dGrad = ScalarGradGG(dil, diu, djl, dju, state_(ii, jj, kk - 1).Rho(),
                       state_(ii, jj, kk).Rho(), ail, aiu,
                       ajl, aju, akl, aku, vol);

  // calculate average pressure on i and j faces of alternate control volume
  const auto piu = 0.25 * (state_(ii, jj, kk - 1).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii + 1, jj, kk).P() +
                           state_(ii + 1, jj, kk - 1).P());
  const auto pil = 0.25 * (state_(ii, jj, kk - 1).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii - 1, jj, kk).P() +
                           state_(ii - 1, jj, kk - 1).P());

  const auto pju = 0.25 * (state_(ii, jj, kk - 1).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj + 1, kk).P() +
                           state_(ii, jj + 1, kk - 1).P());
  const auto pjl = 0.25 * (state_(ii, jj, kk - 1).P() +
                           state_(ii, jj, kk).P() +
                           state_(ii, jj - 1, kk).P() +
                           state_(ii, jj - 1, kk - 1).P());

  // Get density gradient at face
  pGrad = ScalarGradGG(pil, piu, pjl, pju, state_(ii, jj, kk - 1).P(),
                       state_(ii, jj, kk).P(), ail, aiu,
                       ajl, aju, akl, aku, vol);


  // calculate average temperature on i and j faces of alternate control volume
  const auto tiu = 0.25 * (temperature_(ii, jj, kk - 1) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii + 1, jj, kk) +
                           temperature_(ii + 1, jj, kk - 1));
  const auto til = 0.25 * (temperature_(ii, jj, kk - 1) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii - 1, jj, kk) +
                           temperature_(ii - 1, jj, kk - 1));

  const auto tju = 0.25 * (temperature_(ii, jj, kk - 1) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj + 1, kk) +
                           temperature_(ii, jj + 1, kk - 1));
  const auto tjl = 0.25 * (temperature_(ii, jj, kk - 1) +
                           temperature_(ii, jj, kk) +
                           temperature_(ii, jj - 1, kk) +
                           temperature_(ii, jj - 1, kk - 1));

  // Get temperature gradient at face
  tGrad = ScalarGradGG(til, tiu, tjl, tju, temperature_(ii, jj, kk - 1),
                       temperature_(ii, jj, kk), ail, aiu,
                       ajl, aju, akl, aku, vol);

  if (isRANS_) {
    // calculate average tke on i and j faces of alternate control volume
    const auto tkeiu = 0.25 *
        (state_(ii, jj, kk - 1).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii + 1, jj, kk).Tke() + state_(ii + 1, jj, kk - 1).Tke());
    const auto tkeil = 0.25 *
        (state_(ii, jj, kk - 1).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii - 1, jj, kk).Tke() + state_(ii - 1, jj, kk - 1).Tke());

    const auto tkeju = 0.25 *
        (state_(ii, jj, kk - 1).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj + 1, kk).Tke() + state_(ii, jj + 1, kk - 1).Tke());
    const auto tkejl = 0.25 *
        (state_(ii, jj, kk - 1).Tke() + state_(ii, jj, kk).Tke() +
         state_(ii, jj - 1, kk).Tke() + state_(ii, jj - 1, kk - 1).Tke());

    // Get temperature gradient at face
    tkeGrad = ScalarGradGG(
        tkeil, tkeiu, tkejl, tkeju, state_(ii, jj, kk - 1).Tke(),
        state_(ii, jj, kk).Tke(), ail, aiu, ajl, aju, akl, aku, vol);

    // calculate average omega on i and j faces of alternate control volume
    const auto omgiu = 0.25 *
        (state_(ii, jj, kk - 1).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii + 1, jj, kk).Omega() + state_(ii + 1, jj, kk - 1).Omega());
    const auto omgil = 0.25 *
        (state_(ii, jj, kk - 1).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii - 1, jj, kk).Omega() + state_(ii - 1, jj, kk - 1).Omega());

    const auto omgju = 0.25 *
        (state_(ii, jj, kk - 1).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj + 1, kk).Omega() + state_(ii, jj + 1, kk - 1).Omega());
    const auto omgjl = 0.25 *
        (state_(ii, jj, kk - 1).Omega() + state_(ii, jj, kk).Omega() +
         state_(ii, jj - 1, kk).Omega() + state_(ii, jj - 1, kk - 1).Omega());

    // Get temperature gradient at face
    omegaGrad = ScalarGradGG(
        omgil, omgiu, omgjl, omgju, state_(ii, jj, kk - 1).Omega(),
        state_(ii, jj, kk).Omega(), ail, aiu, ajl, aju, akl, aku, vol);
  }

  if (isMultiSpecies_) {
    mixGrad.resize(this->NumSpecies());
    for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
      // calculate average mf on i and j faces of alternate control volume
      const auto mfiu = 0.25 * (state_(ii, jj, kk - 1).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii + 1, jj, kk).MassFractionN(ss) +
                                state_(ii + 1, jj, kk - 1).MassFractionN(ss));
      const auto mfil = 0.25 * (state_(ii, jj, kk - 1).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii - 1, jj, kk).MassFractionN(ss) +
                                state_(ii - 1, jj, kk - 1).MassFractionN(ss));

      const auto mfju = 0.25 * (state_(ii, jj, kk - 1).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj + 1, kk).MassFractionN(ss) +
                                state_(ii, jj + 1, kk - 1).MassFractionN(ss));
      const auto mfjl = 0.25 * (state_(ii, jj, kk - 1).MassFractionN(ss) +
                                state_(ii, jj, kk).MassFractionN(ss) +
                                state_(ii, jj - 1, kk).MassFractionN(ss) +
                                state_(ii, jj - 1, kk - 1).MassFractionN(ss));

      // Get temperature gradient at face
      mixGrad[ss] = ScalarGradGG(mfil, mfiu, mfjl, mfju,
                                 state_(ii, jj, kk - 1).MassFractionN(ss),
                                 state_(ii, jj, kk).MassFractionN(ss), ail, aiu,
                                 ajl, aju, akl, aku, vol);
    }
  }
}

void procBlock::CalcGradsI() {

  constexpr auto sixth = 1.0 / 6.0;

  // loop over all physical i-faces
  for (auto kk = fAreaI_.PhysStartK(); kk < fAreaI_.PhysEndK(); kk++) {
    for (auto jj = fAreaI_.PhysStartJ(); jj < fAreaI_.PhysEndJ(); jj++) {
      for (auto ii = fAreaI_.PhysStartI(); ii < fAreaI_.PhysEndI(); ii++) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsI(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        // at left boundary there is no left cell to add to
        if (ii > fAreaI_.PhysStartI()) {
          // store gradients
          velocityGrad_(ii - 1, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii - 1, jj, kk) += sixth * tempGrad;
          densityGrad_(ii - 1, jj, kk) += sixth * denGrad;
          pressureGrad_(ii - 1, jj, kk) += sixth * pressGrad;
          if (isRANS_) {
            tkeGrad_(ii - 1, jj, kk) += sixth * tkeGrad;
            omegaGrad_(ii - 1, jj, kk) += sixth * omegaGrad;
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii - 1, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }
        }

        // at right boundary there is no right cell to add to
        if (ii < fAreaI_.PhysEndI() - 1) {
          // store gradients
          velocityGrad_(ii, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk) += sixth * tempGrad;
          densityGrad_(ii, jj, kk) += sixth * denGrad;
          pressureGrad_(ii, jj, kk) += sixth * pressGrad;
          if (isRANS_) {
            tkeGrad_(ii, jj, kk) += sixth * tkeGrad;
            omegaGrad_(ii, jj, kk) += sixth * omegaGrad;
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }
        }
      }
    }
  }
}

void procBlock::CalcGradsJ() {

  constexpr auto sixth = 1.0 / 6.0;

  // loop over all physical j-faces
  for (auto kk = fAreaJ_.PhysStartK(); kk < fAreaJ_.PhysEndK(); kk++) {
    for (auto jj = fAreaJ_.PhysStartJ(); jj < fAreaJ_.PhysEndJ(); jj++) {
      for (auto ii = fAreaJ_.PhysStartI(); ii < fAreaJ_.PhysEndI(); ii++) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsJ(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        // at left boundary there is no left cell to add to
        if (jj > fAreaJ_.PhysStartJ()) {
          // store gradients
          velocityGrad_(ii, jj - 1, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj - 1, kk) += sixth * tempGrad;
          densityGrad_(ii, jj - 1, kk) += sixth * denGrad;
          pressureGrad_(ii, jj - 1, kk) += sixth * pressGrad;
          if (isRANS_) {
            tkeGrad_(ii, jj - 1, kk) += sixth * tkeGrad;
            omegaGrad_(ii, jj - 1, kk) += sixth * omegaGrad;
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj - 1, kk, ss) += sixth * mixGrad[ss];
            }
          }
        }

        // at right boundary there is no right cell to add to
        if (jj < fAreaJ_.PhysEndJ() - 1) {
          // store gradients
          velocityGrad_(ii, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk) += sixth * tempGrad;
          densityGrad_(ii, jj, kk) += sixth * denGrad;
          pressureGrad_(ii, jj, kk) += sixth * pressGrad;
          if (isRANS_) {
            tkeGrad_(ii, jj, kk) += sixth * tkeGrad;
            omegaGrad_(ii, jj, kk) += sixth * omegaGrad;
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }
        }
      }
    }
  }
}

void procBlock::CalcGradsK() {

  constexpr auto sixth = 1.0 / 6.0;

  // loop over all physical k-faces
  for (auto kk = fAreaK_.PhysStartK(); kk < fAreaK_.PhysEndK(); kk++) {
    for (auto jj = fAreaK_.PhysStartJ(); jj < fAreaK_.PhysEndJ(); jj++) {
      for (auto ii = fAreaK_.PhysStartI(); ii < fAreaK_.PhysEndI(); ii++) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsK(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        // at left boundary there is no left cell to add to
        if (kk > fAreaK_.PhysStartK()) {
          // store gradients
          velocityGrad_(ii, jj, kk - 1) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk - 1) += sixth * tempGrad;
          densityGrad_(ii, jj, kk - 1) += sixth * denGrad;
          pressureGrad_(ii, jj, kk - 1) += sixth * pressGrad;
          if (isRANS_) {
            tkeGrad_(ii, jj, kk - 1) += sixth * tkeGrad;
            omegaGrad_(ii, jj, kk - 1) += sixth * omegaGrad;
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk - 1, ss) += sixth * mixGrad[ss];
            }
          }
        }

        // at right boundary there is no right cell to add to
        if (kk < fAreaK_.PhysEndK() - 1) {
          // store gradients
          velocityGrad_(ii, jj, kk) += sixth * velGrad;
          temperatureGrad_(ii, jj, kk) += sixth * tempGrad;
          densityGrad_(ii, jj, kk) += sixth * denGrad;
          pressureGrad_(ii, jj, kk) += sixth * pressGrad;
          if (isRANS_) {
            tkeGrad_(ii, jj, kk) += sixth * tkeGrad;
            omegaGrad_(ii, jj, kk) += sixth * omegaGrad;
          }
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              mixtureGrad_(ii, jj, kk, ss) += sixth * mixGrad[ss];
            }
          }
        }
      }
    }
  }
}

// Member function to calculate the source terms and add them to the residual
void procBlock::CalcSrcTerms(const physics &phys, const input &inp,
                             matMultiArray3d &mainDiagonal) {
  // phys -- phyics models
  // mainDiagonal -- main diagonal of LHS used to store flux jacobians for
  //                 implicit solver

  // loop over all physical cells - no ghost cells needed for source terms
  for (auto kk = 0; kk < this->NumK(); kk++) {
    for (auto jj = 0; jj < this->NumJ(); jj++) {
      for (auto ii = 0; ii < this->NumI(); ii++) {
        source src(this->NumEquations(), this->NumSpecies());

        if (phys.Chemistry()->IsReacting()) {
          // calculate chemistry source terms
          auto chemSpecRad = 0.0;
          const auto chemJac = src.CalcChemSrc(phys, state_(ii, jj, kk),
                                               temperature_(ii, jj, kk), 
                                               vol_(ii, jj, kk),
                                               inp.IsBlockMatrix(), 
                                               chemSpecRad);

          // add source spectral radius for species equations
          // subtract because residual is initially on opposite side of equation
          specRadius_(ii, jj, kk).SubtractFromFlowVariable(chemSpecRad);

          // add contribution of source spectral radius to flux jacobian
          if (inp.IsBlockMatrix()) {
            mainDiagonal.SubtractFromFlow(ii, jj, kk, chemJac);
          } else if (inp.IsImplicit()) {
            const uncoupledScalar srcJacScalar(chemSpecRad, 0.0);
            mainDiagonal.Subtract(ii, jj, kk,
                                  fluxJacobian(srcJacScalar, isRANS_));
          }
        }

        if (isRANS_) {
          // calculate turbulent source terms
          const auto phi = phys.Turbulence()->UsePhi()
                               ? this->MaxCellWidth(ii, jj, kk)
                               : 1.0;
          const auto srcJac = src.CalcTurbSrc(
              phys.Turbulence(), state_(ii, jj, kk), velocityGrad_(ii, jj, kk),
              temperatureGrad_(ii, jj, kk), tkeGrad_(ii, jj, kk),
              omegaGrad_(ii, jj, kk), phys.Transport(), vol_(ii, jj, kk),
              eddyViscosity_(ii, jj, kk), f1_(ii, jj, kk), f2_(ii, jj, kk),
              phi);

          // add source spectral radius for turbulence equations
          // subtract because residual is initially on opposite side of equation
          const auto turbSpecRad = phys.Turbulence()->SrcSpecRad(
              state_(ii, jj, kk), phys.Transport(), vol_(ii, jj, kk), phi);
          specRadius_(ii, jj, kk).SubtractFromTurbVariable(turbSpecRad);

          // add contribution of source spectral radius to flux jacobian
          if (inp.IsBlockMatrix()) {
            mainDiagonal.SubtractFromTurb(ii, jj, kk, srcJac);
          } else if (inp.IsImplicit()) {
            const uncoupledScalar srcJacScalar(0.0, turbSpecRad);
            mainDiagonal.Subtract(ii, jj, kk,
                                  fluxJacobian(srcJacScalar, isRANS_));
          }
        }

        // add source terms to residual
        // subtract because residual is initially on opposite side of equation
        this->SubtractFromResidual(ii, jj, kk, src * vol_(ii, jj, kk));
      }
    }
  }
}


// member function to calculate the distance to the nearest viscous wall of
// all cell centers
void procBlock::CalcWallDistance(const kdtree &tree) {
  vector3d<double> neighbor;
  auto id = 0;
  // loop over physical cells
  for (auto kk = this->StartK(); kk < this->EndK(); kk++) {
    for (auto jj = this->StartJ(); jj < this->EndJ(); jj++) {
      for (auto ii = this->StartI(); ii < this->EndI(); ii++) {
        wallDist_(ii, jj, kk) =
            tree.NearestNeighbor(center_(ii, jj, kk), neighbor, id);
      }
    }
  }

  string surf = "none";
  auto type = 0;
  // populate ghost cells (not edge ghosts)
  for (auto kk = wallDist_.StartK(); kk < wallDist_.EndK(); kk++) {
    for (auto jj = wallDist_.StartJ(); jj < wallDist_.EndJ(); jj++) {
      for (auto ii = wallDist_.StartI(); ii < wallDist_.EndI(); ii++) {
        // ghost cells across viscous boundaries should have negative wall
        // distance so that wall distance at viscous face will be 0 during flux
        // calculation
        // other ghost cells just use distance of interior cell
        if (this->AtGhostNonEdge(ii, jj, kk, surf, type)) {
          if (type == 1) {
            auto bcType = bc_.GetBCName(this->StartI(), jj, kk, type);
            if (bcType == "viscousWall") {
              auto index = this->StartI() + std::abs(ii) - 1;
              wallDist_(ii, jj, kk) = -1.0 * wallDist_(index, jj, kk);
            } else {
              wallDist_(ii, jj, kk) = wallDist_(this->StartI(), jj, kk);
            }
          } else if (type == 2) {
            auto bcType = bc_.GetBCName(this->EndI(), jj, kk, type);
            if (bcType == "viscousWall") {
              auto index = this->EndI() - (ii - this->EndI() + 1);
              wallDist_(ii, jj, kk) = -1.0 * wallDist_(index, jj, kk);
            } else {
              wallDist_(ii, jj, kk) = wallDist_(this->EndI() - 1, jj, kk);
            }
          } else if (type == 3) {
            auto bcType = bc_.GetBCName(ii, this->StartJ(), kk, type);
            if (bcType == "viscousWall") {
              auto index = this->StartJ() + std::abs(jj) - 1;
              wallDist_(ii, jj, kk) = -1.0 * wallDist_(ii, index, kk);
            } else {
              wallDist_(ii, jj, kk) = wallDist_(ii, this->StartJ(), kk);
            }
          } else if (type == 4) {
            auto bcType = bc_.GetBCName(ii, this->EndJ(), kk, type);
            if (bcType == "viscousWall") {
              auto index = this->EndJ() - (jj - this->EndJ() + 1);
              wallDist_(ii, jj, kk) = -1.0 * wallDist_(ii, index, kk);
            } else {
              wallDist_(ii, jj, kk) = wallDist_(ii, this->EndJ() - 1, kk);
            }
          } else if (type == 5) {
            auto bcType = bc_.GetBCName(ii, jj, this->StartK(), type);
            if (bcType == "viscousWall") {
              auto index = this->StartK() + std::abs(kk) - 1;
              wallDist_(ii, jj, kk) = -1.0 * wallDist_(ii, jj, index);
            } else {
              wallDist_(ii, jj, kk) = wallDist_(ii, jj, this->StartK());
            }
          } else if (type == 6) {
            auto bcType = bc_.GetBCName(ii, jj, this->EndK(), type);
            if (bcType == "viscousWall") {
              auto index = this->EndK() - (kk - this->EndK() + 1);
              wallDist_(ii, jj, kk) = -1.0 * wallDist_(ii, jj, index);
            } else {
              wallDist_(ii, jj, kk) = wallDist_(ii, jj, this->EndK() - 1);
            }
          }
        }
      }
    }
  }
}

// member function to calculate the residual (RHS) excluding any contributions
// from source terms
void procBlock::CalcResidualNoSource(const physics &phys, const input &inp,
                                     matMultiArray3d &mainDiagonal) {
  // Zero spectral radii, residuals, gradients, turbulence variables
  this->ResetResidWS();
  this->ResetGradients();
  if (isTurbulent_) {
    this->ResetTurbVars();
  }

  // Calculate inviscid fluxes
  this->CalcInvFluxI(phys, inp, mainDiagonal);
  this->CalcInvFluxJ(phys, inp, mainDiagonal);
  this->CalcInvFluxK(phys, inp, mainDiagonal);

  // If viscous change ghost cells and calculate viscous fluxes
  if (isViscous_) {
    // Determine ghost cell values for viscous fluxes
    this->AssignViscousGhostCells(inp, phys);

    // Update temperature and viscosity
    this->UpdateAuxillaryVariables(phys);

    // Calculate viscous fluxes
    this->CalcViscFluxI(phys, inp, mainDiagonal);
    this->CalcViscFluxJ(phys, inp, mainDiagonal);
    this->CalcViscFluxK(phys, inp, mainDiagonal);

  } else {
    // Update temperature
    this->UpdateAuxillaryVariables(phys);

    // calculate gradients
    this->CalcGradsI();
    this->CalcGradsJ();
    this->CalcGradsK();
  }
}

// member function to get a slice of the state variables
blkMultiArray3d<primitive> procBlock::SliceState(const int &is, const int &ie,
                                                 const int &js, const int &je,
                                                 const int &ks,
                                                 const int &ke) const {
  return state_.Slice({is, ie}, {js, je}, {ks, ke});
}

// member function to get a slice of the face centers of the cells on a
// boundary
multiArray3d<vector3d<double>> procBlock::SliceBoundaryCenters(
    const int &surfInd) const {
  const auto surf = bc_.GetSurface(surfInd);
  if (surf.SurfaceType() <= 2) {  // i-surface
    return fCenterI_.Slice(surf.RangeI(), surf.RangeJ(), surf.RangeK());
  } else if (surf.SurfaceType() <= 4) {  // j-surface
    return fCenterJ_.Slice(surf.RangeI(), surf.RangeJ(), surf.RangeK());
  } else {  // k-surface
    return fCenterK_.Slice(surf.RangeI(), surf.RangeJ(), surf.RangeK());
  }
}

void procBlock::UpdateAuxillaryVariables(const physics &phys,
                                         const bool includeGhosts) {
  for (auto kk = temperature_.StartK(); kk < temperature_.EndK(); kk++) {
    for (auto jj = temperature_.StartJ(); jj < temperature_.EndJ(); jj++) {
      for (auto ii = temperature_.StartI(); ii < temperature_.EndI(); ii++) {
        if (!this->AtCorner(ii, jj, kk) &&
            (includeGhosts || this->IsPhysical(ii, jj, kk))) {
          temperature_(ii, jj, kk) = state_(ii, jj, kk).Temperature(phys.EoS());
          MSG_ASSERT(temperature_(ii, jj, kk) > 0.0, "nonphysical temperature");
          if (isViscous_) {
            viscosity_(ii, jj, kk) = phys.Transport()->Viscosity(
                temperature_(ii, jj, kk), state_(ii, jj, kk).MassFractions());
            MSG_ASSERT(viscosity_(ii, jj, kk) >= 0.0, "nonphysical viscosity");
          }
        }
      }
    }
  }
}

void procBlock::UpdateUnlimTurbEddyVisc(const unique_ptr<turbModel> &turb,
                                        const bool &includeGhosts) {
  if (isTurbulent_) {
    for (auto kk = eddyViscosity_.StartK(); kk < eddyViscosity_.EndK(); kk++) {
      for (auto jj = eddyViscosity_.StartJ(); jj < eddyViscosity_.EndJ(); jj++) {
        for (auto ii = eddyViscosity_.StartI(); ii < eddyViscosity_.EndI(); ii++) {
          if (!this->AtCorner(ii, jj, kk) &&
              (includeGhosts || this->IsPhysical(ii, jj, kk))) {
            eddyViscosity_(ii, jj, kk) =
                turb->EddyViscNoLim(state_(ii, jj, kk));
          }
        }
      }
    }
  }
}

blkMultiArray3d<primitive> procBlock::GetGhostStates(
    const blkMultiArray3d<primitive> &bndStates, const string &bcName,
    const multiArray3d<unitVec3dMag<double>> &faceAreas,
    const multiArray3d<double> &wDist, const boundarySurface &surf,
    const input &inp, const physics &phys, const int &layer,
    const multiArray3d<double> &nuW, const multiArray3d<double> &dt,
    const blkMultiArray3d<conserved> &consVarsN,
    const multiArray3d<vector3d<double>> &pGrad,
    const multiArray3d<tensor<double>> &velGrad) {
  // bndStates -- states at cells adjacent to boundary
  // bcName -- boundary condition type
  // faceAreas -- face areas of boundary
  // surf -- boundary surface
  // inp -- input variables
  // phys -- physics models
  // layer -- layer of ghost cell to return
  //          (1 closest to boundary, or 2 farthest)
  // nuW -- wall adjacent kinematic viscosity
  // dt -- time step at cells adjacent to boundary
  // consVarsN -- conservative variables at time n
  // pGrad -- pressure gradient at adjacent cell
  // velGrad -- velocity gradient at adjacent cell

  // get surface type and tag
  const auto surfType = surf.SurfaceType();
  const auto tag = surf.Tag();

  // find average and max mach on boundary for nonreflecting pressure outlet
  // only using data on local surface patch here, not bothering to make MPI
  // calls to get data over global patch
  auto avgMach = 0.0;
  auto maxMach = -1.0 * std::numeric_limits<double>::max();
  if (bcName == "pressureOutlet" || bcName == "inlet") {
    const auto &bcData = inp.BCData(tag);
    if (bcData->IsNonreflecting()) {
      // face area vector (should always point out of domain)
      // at lower surface normal should point out of domain for ghost cell calc
      const auto isLower = surfType % 2 == 1;
      for (auto kk = bndStates.StartK(); kk < bndStates.EndK(); kk++) {
        for (auto jj = bndStates.StartJ(); jj < bndStates.EndJ(); jj++) {
          for (auto ii = bndStates.StartI(); ii < bndStates.EndI(); ii++) {
            const auto area = isLower
                                  ? -1.0 * faceAreas(ii, jj, kk).UnitVector()
                                  : faceAreas(ii, jj, kk).UnitVector();
            auto mach = bndStates(ii, jj, kk).Velocity().DotProd(area) /
                        bndStates(ii, jj, kk).SoS(phys);
            maxMach = std::max(maxMach, mach);
            avgMach += mach;
          }
        }
      }
      avgMach /= bndStates.NumBlocks();
    }
  }

  blkMultiArray3d<primitive> ghostStates(
      bndStates.NumINoGhosts(), bndStates.NumJNoGhosts(),
      bndStates.NumKNoGhosts(), bndStates.GhostLayers(), bndStates.BlockSize(),
      this->NumSpecies());
  for (auto kk = bndStates.StartK(); kk < bndStates.EndK(); kk++) {
    for (auto jj = bndStates.StartJ(); jj < bndStates.EndJ(); jj++) {
      for (auto ii = bndStates.StartI(); ii < bndStates.EndI(); ii++) {
        wallVars wVars(this->NumSpecies());
        const auto nuWall = nuW.IsEmpty() ? 0.0 : nuW(ii, jj, kk);
        if (consVarsN.IsEmpty()) {
          const auto ghost = GetGhostState(bndStates(ii, jj, kk), bcName,
                                           faceAreas(ii, jj, kk).UnitVector(),
                                           wDist(ii, jj, kk), surfType, inp,
                                           tag, phys, wVars, layer, nuWall);
          ghostStates.InsertBlock(ii, jj, kk, ghost);
        } else {  // using dt, state at time n, press grad, vel grad in BC
          const auto stateN = primitive(consVarsN(ii, jj, kk), phys);
          const auto ghost = GetGhostState(
              bndStates(ii, jj, kk), bcName, faceAreas(ii, jj, kk).UnitVector(),
              wDist(ii, jj, kk), surfType, inp, tag, phys, wVars, layer, nuWall,
              dt(ii, jj, kk), stateN, pGrad(ii, jj, kk), velGrad(ii, jj, kk),
              avgMach, maxMach);
          ghostStates.InsertBlock(ii, jj, kk, ghost);
        }
        if (bcName == "viscousWall" && layer == 1) {
          const auto ind = this->WallDataIndex(surf);
          wallData_[ind](ii, jj, kk, true) = wVars;
        }
      }
    }
  }
  return ghostStates;
}

int procBlock::WallDataIndex(const boundarySurface &surf) const {
  auto ind = -1;
  for (auto ii = 0U; ii < wallData_.size(); ++ii) {
    if (surf == wallData_[ii].Surface()) {
      ind = ii;
      break;
    }
  }
  if (ind < 0) {
    cerr << "ERROR. Given boundary surface does not match any in wallData"
         << endl;
    cerr << "Given boundary surface is: " << endl << surf << endl;
    exit(EXIT_FAILURE);
  }
  return ind;
}

// member function to calculate the center to center distance across a cell face
// projected along that face's area vector
double procBlock::ProjC2CDist(const int &ii, const int &jj, const int &kk,
                              const string &dir) const {
  // ii -- cell face index in i-direction
  // jj -- cell face index in j-direction
  // kk -- cell face index in k-direction
  // dir -- direction of face (i, j, or k)

  auto projDist = 0.0;
  // always subtract "higher" center from "lower" center because area vector
  // points from lower to higher
  if (dir == "i") {
    const auto c2cVec = this->Center(ii, jj, kk) - this->Center(ii - 1, jj, kk);
    projDist = c2cVec.DotProd(this->FAreaUnitI(ii, jj, kk));
  } else if (dir == "j") {
    const auto c2cVec = this->Center(ii, jj, kk) - this->Center(ii, jj - 1, kk);
    projDist = c2cVec.DotProd(this->FAreaUnitJ(ii, jj, kk));
  } else if (dir == "k") {
    const auto c2cVec = this->Center(ii, jj, kk) - this->Center(ii, jj, kk - 1);
    projDist = c2cVec.DotProd(this->FAreaUnitK(ii, jj, kk));
  } else {
    cerr << "ERROR: Error in procBlock::ProjC2CDist(). Direction " << dir
         << " is not recognized. Please choose i, j, or k." << endl;
    exit(EXIT_FAILURE);
  }
  return projDist;
}

// member function to write the contents of a given variable to a given file
void procBlock::DumpToFile(const string &var, const string &fName) const {
  // var -- variable name to write out
  // fname -- file name

  ofstream outFile(fName, ios::out);
  // check to see if file opened correctly
  if (outFile.fail()) {
    cerr << "ERROR: File " << fName << " did not open correctly!!!" << endl;
    exit(EXIT_FAILURE);
  }

  if (var == "volume") {
    outFile << vol_ << endl;
  } else if (var == "faceAreaI") {
    outFile << fAreaI_ << endl;
  } else if (var == "faceAreaJ") {
    outFile << fAreaJ_ << endl;
  } else if (var == "faceAreaK") {
    outFile << fAreaK_ << endl;
  } else if (var == "center") {
    outFile << center_ << endl;
  } else if (var == "faceCenterI") {
    outFile << fCenterI_ << endl;
  } else if (var == "faceCenterJ") {
    outFile << fCenterJ_ << endl;
  } else if (var == "faceCenterK") {
    outFile << fCenterK_ << endl;
  } else if (var == "state") {
    outFile << state_ << endl;
  } else if (var == "residual") {
    outFile << residual_ << endl;
  } else if (var == "velocityGradient") {
    outFile << velocityGrad_ << endl;
  } else if (var == "temperatureGradient") {
    outFile << temperatureGrad_ << endl;
  } else if (var == "densityGradient") {
    outFile << densityGrad_ << endl;
  } else if (var == "pressureGradient") {
    outFile << pressureGrad_ << endl;
  } else if (var == "viscosity") {
    outFile << viscosity_ << endl;
  } else if (var == "eddyViscosity") {
    outFile << eddyViscosity_ << endl;
  } else {
    cerr << "ERROR: Error in procBlock::DumpToFile(). Variable " << var
         << " is not supported!" << endl;
    exit(EXIT_FAILURE);
  }

  // close file
  outFile.close();
}

void procBlock::CalcCellWidths() {
  // loop over all cells
  for (auto kk = this->StartKG(); kk < this->EndKG(); ++kk) {
    for (auto jj = this->StartJG(); jj < this->EndJG(); ++jj) {
      for (auto ii = this->StartIG(); ii < this->EndIG(); ++ii) {
        cellWidthI_(ii, jj, kk) = fCenterI_(ii, jj, kk).Distance(
            fCenterI_(ii + 1, jj, kk));
        cellWidthJ_(ii, jj, kk) = fCenterJ_(ii, jj, kk).Distance(
            fCenterJ_(ii, jj + 1, kk));
        cellWidthK_(ii, jj, kk) = fCenterK_(ii, jj, kk).Distance(
            fCenterK_(ii, jj, kk + 1));
      }
    }
  }
}


void procBlock::GetStatesFromRestart(const blkMultiArray3d<primitive> &restart) {
  state_.Insert(restart.RangeI(), restart.RangeJ(), restart.RangeK(), restart);
}

void procBlock::GetSolNm1FromRestart(const blkMultiArray3d<conserved> &restart) {
  consVarsNm1_ = restart;
}

// split all wallData in procBlock
// The calling instance keeps the lower wallData in the split, and the upper
// wallData is returned
vector<wallData> procBlock::SplitWallData(const string &dir, const int &ind) {
  vector<wallData> upper;
  vector<int> delLower;
  auto count = 0;
  for (auto &lower : wallData_) {
    auto split = false, low = false;
    auto up = lower.Split(dir, ind, split, low);
    if (split) {  // surface split; upper and lower are valid
      upper.push_back(up);
    } else if (!low) {  // not split; valid surface is upper
      upper.push_back(up);
      delLower.push_back(count);  // need to remove b/c lower is invalid
    }
    count++;
  }

  // delete lower wall data not in lower split in reverse
  for (auto ii = static_cast<int>(delLower.size()) - 1; ii >= 0; --ii) {
    wallData_.erase(std::begin(wallData_) + delLower[ii]);
  }

  return upper;
}

// Join all wallData in procBlock if possible. 
void procBlock::JoinWallData(const vector<wallData> &upper, const string &dir) {
  vector<int> joinedData;
  for (auto ll = 0U; ll < wallData_.size(); ++ll) {
    for (auto uu = 0U; uu < upper.size(); ++uu) {
      auto joined = false;
      wallData_[ll].Join(upper[uu], dir, joined);
      if (joined) {  // if joined, don't need to add upper data
        joinedData.push_back(uu);
      }
    }
  }

  // add in unjoined upper data
  for (auto ii = 0; ii < static_cast<int>(upper.size()); ++ii) {
    if (std::none_of(std::begin(joinedData), std::end(joinedData),
                     [&ii](const int &val) { return ii == val; })) {
      wallData_.push_back(upper[ii]);
    }
  }
}

void procBlock::GetCoarseMeshAndBCs(
    vector<plot3dBlock> &mesh, vector<boundaryConditions> &bcs,
    vector<multiArray3d<vector3d<int>>> &toCoarse,
    vector<multiArray3d<double>> &volFac) const {
  bcs.push_back(this->BC());

  // determine the i-indices of the fine mesh to keep
  vector<int> iIndex;
  auto sinceLastKept = 0;
  iIndex.reserve(nodes_.NumI() / 2);
  for (auto ii = 0; ii < nodes_.NumI(); ++ii) {
    // if index is part of a surface patch, need to keep it
    if (bc_.IsSurfaceBoundary("i", ii)) {
      iIndex.push_back(ii);
      bcs.back().UpdateSurfacesForCoarseMesh("i", ii, iIndex.size() - 1);
      sinceLastKept = 0;
    } else if (sinceLastKept > 0) {  // keep every other index
      iIndex.push_back(ii);
      sinceLastKept = 0;
    } else {
      sinceLastKept++;
    }
  }

  // determine the j-indices of the fine mesh to keep
  vector<int> jIndex;
  sinceLastKept = 0;
  jIndex.reserve(nodes_.NumJ() / 2);
  for (auto jj = 0; jj < nodes_.NumJ(); ++jj) {
    // if index is part of a surface patch, need to keep it
    if (bc_.IsSurfaceBoundary("j", jj)) {
      jIndex.push_back(jj);
      bcs.back().UpdateSurfacesForCoarseMesh("j", jj, jIndex.size() - 1);
      sinceLastKept = 0;
    } else if (sinceLastKept > 0) {  // keep every other index
      jIndex.push_back(jj);
      sinceLastKept = 0;
    } else {
      sinceLastKept++;
    }
  }

  // determine the k-indices of the fine mesh to keep
  vector<int> kIndex;
  sinceLastKept = 0;
  kIndex.reserve(nodes_.NumK() / 2);
  for (auto kk = 0; kk < nodes_.NumK(); ++kk) {
    // if index is part of a surface patch, need to keep it
    if (bc_.IsSurfaceBoundary("k", kk)) {
      kIndex.push_back(kk);
      bcs.back().UpdateSurfacesForCoarseMesh("k", kk, kIndex.size() - 1);
      sinceLastKept = 0;
    } else if (sinceLastKept > 0) {  // keep every other index
      kIndex.push_back(kk);
      sinceLastKept = 0;
    } else {
      sinceLastKept++;
    }
  }

  // create plot3dBlk of coarse nodes
  multiArray3d<vector3d<double>> coarseNodes(iIndex.size(), jIndex.size(),
                                             kIndex.size(), 0);
  for (auto kk = coarseNodes.StartK(); kk < coarseNodes.EndK(); ++kk) {
    for (auto jj = coarseNodes.StartJ(); jj < coarseNodes.EndJ(); ++jj) {
      for (auto ii = coarseNodes.StartI(); ii < coarseNodes.EndI(); ++ii) {
        coarseNodes(ii, jj, kk) =
            this->Node(iIndex[ii], jIndex[jj], kIndex[kk]);
      }
    }
  }
  mesh.emplace_back(coarseNodes);
  WriteNodes("coarse", mesh);

  // create map of fine to coarse cells
  toCoarse.emplace_back(this->NumI(), this->NumJ(), this->NumK(), 0);
  // custom comparator for vector3d<>
  auto compareV3d = [](const auto &ll, const auto &rr) {
    if (ll[0] != rr[0]) {
      return ll[0] < rr[0];
    } else if (ll[1] != rr[1]) {
      return ll[1] < rr[1];
    } else {
      return ll[2] < rr[2];
    }
  };
  std::multimap<vector3d<int>, vector3d<int>, decltype(compareV3d)>
      coarseToFine(compareV3d);

  for (auto fk = this->StartK(); fk < this->EndK(); ++fk) {
    for (auto fj = this->StartJ(); fj < this->EndJ(); ++fj) {
      for (auto fi = this->StartI(); fi < this->EndI(); ++fi) {
        // find first coarse i, j, k index >= fi, fj, fk
        int ci = std::distance(
            std::begin(iIndex),
            std::find_if(std::begin(iIndex), std::end(iIndex),
                         [&fi](const auto &ind) { return ind > fi; }));
        if (ci != this->StartI()) {ci--;}  // convert to cell index
        int cj = std::distance(
            std::begin(jIndex),
            std::find_if(std::begin(jIndex), std::end(jIndex),
                         [&fj](const auto &ind) { return ind > fj; }));
        if (cj != this->StartJ()) {cj--;}  // convert to cell index
        int ck = std::distance(
            std::begin(kIndex),
            std::find_if(std::begin(kIndex), std::end(kIndex),
                         [&fk](const auto &ind) { return ind > fk; }));
        if (ck != this->StartK()) {ck--;}  // convert to cell index
        toCoarse.back()(fi, fj, fk) = {ci, cj, ck};
        coarseToFine.insert(std::make_pair(vector3d<int>(ci, cj, ck),
                                           vector3d<int>(fi, fj, fk)));
      }
    }
  }

  // populate volume weighting factor
  volFac.emplace_back(this->NumI(), this->NumJ(), this->NumK(), 0);
  for (auto ck = coarseNodes.StartK(); ck < coarseNodes.EndK() - 1; ++ck) {
    for (auto cj = coarseNodes.StartJ(); cj < coarseNodes.EndJ() - 1; ++cj) {
      for (auto ci = coarseNodes.StartI(); ci < coarseNodes.EndI() - 1; ++ci) {
        auto range = coarseToFine.equal_range({ci, cj, ck});
        // iterate over range and sum all volumes that map to same coarse cell
        auto volSum = 0.0;
        for (auto ii = range.first; ii != range.second; ++ii) {
          volSum += vol_(ii->second[0], ii->second[1], ii->second[2]);
        }
        // iterate over same loops and normalize fine volume by sum
        for (auto ii = range.first; ii != range.second; ++ii) {
          volFac.back()(ii->second[0], ii->second[1], ii->second[2]) =
              vol_(ii->second[0], ii->second[1], ii->second[2]) / volSum;
        }
      }
    }
  }
}

procBlock procBlock::CellToNode() const {
  procBlock nodeData(this->NumI() + 1, this->NumJ() + 1, this->NumK() + 1, 0, 
                     this->NumEquations(), this->NumSpecies(), 
                     isViscous_, isTurbulent_, isRANS_,
                     storeTimeN_, isMultiLevelTime_, isMultiSpecies_);
  // solution data
  nodeData.state_ = ConvertCellToNode(state_);
  nodeData.residual_ = ConvertCellToNode(residual_, true);
  nodeData.dt_ = ConvertCellToNode(dt_, true);
  nodeData.wallDist_ = ConvertCellToNode(wallDist_, true);
  nodeData.temperature_ = ConvertCellToNode(temperature_);
  nodeData.viscosity_ = ConvertCellToNode(viscosity_);
  nodeData.eddyViscosity_ = ConvertCellToNode(eddyViscosity_);
  nodeData.f1_ = ConvertCellToNode(f1_);
  nodeData.f2_ = ConvertCellToNode(f2_);

  // gradients -------------------
  // i-faces
  for (auto kk = fAreaI_.PhysStartK(); kk < fAreaI_.PhysEndK(); ++kk) {
    for (auto jj = fAreaI_.PhysStartJ(); jj < fAreaI_.PhysEndJ(); ++jj) {
      for (auto ii = fAreaI_.PhysStartI(); ii < fAreaI_.PhysEndI(); ++ii) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsI(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        nodeData.velocityGrad_(ii, jj, kk) += velGrad;
        nodeData.velocityGrad_(ii, jj + 1, kk) += velGrad;
        nodeData.velocityGrad_(ii, jj, kk + 1) += velGrad;
        nodeData.velocityGrad_(ii, jj + 1, kk + 1) += velGrad;

        nodeData.temperatureGrad_(ii, jj, kk) += tempGrad;
        nodeData.temperatureGrad_(ii, jj + 1, kk) += tempGrad;
        nodeData.temperatureGrad_(ii, jj, kk + 1) += tempGrad;
        nodeData.temperatureGrad_(ii, jj + 1, kk + 1) += tempGrad;

        nodeData.densityGrad_(ii, jj, kk) += denGrad;
        nodeData.densityGrad_(ii, jj + 1, kk) += denGrad;
        nodeData.densityGrad_(ii, jj, kk + 1) += denGrad;
        nodeData.densityGrad_(ii, jj + 1, kk + 1) += denGrad;

        nodeData.pressureGrad_(ii, jj, kk) += pressGrad;
        nodeData.pressureGrad_(ii, jj + 1, kk) += pressGrad;
        nodeData.pressureGrad_(ii, jj, kk + 1) += pressGrad;
        nodeData.pressureGrad_(ii, jj + 1, kk + 1) += pressGrad;

        nodeData.tkeGrad_(ii, jj, kk) += tkeGrad;
        nodeData.tkeGrad_(ii, jj + 1, kk) += tkeGrad;
        nodeData.tkeGrad_(ii, jj, kk + 1) += tkeGrad;
        nodeData.tkeGrad_(ii, jj + 1, kk + 1) += tkeGrad;

        nodeData.omegaGrad_(ii, jj, kk) += omegaGrad;
        nodeData.omegaGrad_(ii, jj + 1, kk) += omegaGrad;
        nodeData.omegaGrad_(ii, jj, kk + 1) += omegaGrad;
        nodeData.omegaGrad_(ii, jj + 1, kk + 1) += omegaGrad;

        if (isMultiSpecies_) {
          for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
            nodeData.mixtureGrad_(ii, jj, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii, jj + 1, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii, jj, kk + 1, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii, jj + 1, kk + 1, ss) += mixGrad[ss];
          }
        }
      }
    }
  }

  // j-faces
  for (auto kk = fAreaJ_.PhysStartK(); kk < fAreaJ_.PhysEndK(); ++kk) {
    for (auto jj = fAreaJ_.PhysStartJ(); jj < fAreaJ_.PhysEndJ(); ++jj) {
      for (auto ii = fAreaJ_.PhysStartI(); ii < fAreaJ_.PhysEndI(); ++ii) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsJ(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        nodeData.velocityGrad_(ii, jj, kk) += velGrad;
        nodeData.velocityGrad_(ii + 1, jj, kk) += velGrad;
        nodeData.velocityGrad_(ii, jj, kk + 1) += velGrad;
        nodeData.velocityGrad_(ii + 1, jj, kk + 1) += velGrad;

        nodeData.temperatureGrad_(ii, jj, kk) += tempGrad;
        nodeData.temperatureGrad_(ii + 1, jj, kk) += tempGrad;
        nodeData.temperatureGrad_(ii, jj, kk + 1) += tempGrad;
        nodeData.temperatureGrad_(ii + 1, jj, kk + 1) += tempGrad;

        nodeData.densityGrad_(ii, jj, kk) += denGrad;
        nodeData.densityGrad_(ii + 1, jj, kk) += denGrad;
        nodeData.densityGrad_(ii, jj, kk + 1) += denGrad;
        nodeData.densityGrad_(ii + 1, jj, kk + 1) += denGrad;

        nodeData.pressureGrad_(ii, jj, kk) += pressGrad;
        nodeData.pressureGrad_(ii + 1, jj, kk) += pressGrad;
        nodeData.pressureGrad_(ii, jj, kk + 1) += pressGrad;
        nodeData.pressureGrad_(ii + 1, jj, kk + 1) += pressGrad;

        nodeData.tkeGrad_(ii, jj, kk) += tkeGrad;
        nodeData.tkeGrad_(ii + 1, jj, kk) += tkeGrad;
        nodeData.tkeGrad_(ii, jj, kk + 1) += tkeGrad;
        nodeData.tkeGrad_(ii + 1, jj, kk + 1) += tkeGrad;

        nodeData.omegaGrad_(ii, jj, kk) += omegaGrad;
        nodeData.omegaGrad_(ii + 1, jj, kk) += omegaGrad;
        nodeData.omegaGrad_(ii, jj, kk + 1) += omegaGrad;
        nodeData.omegaGrad_(ii + 1, jj, kk + 1) += omegaGrad;

        if (isMultiSpecies_) {
          for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
            nodeData.mixtureGrad_(ii, jj, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii + 1, jj, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii, jj, kk + 1, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii + 1, jj, kk + 1, ss) += mixGrad[ss];
          }
        }
      }
    }
  }

  // k-faces
  for (auto kk = fAreaK_.PhysStartK(); kk < fAreaK_.PhysEndK(); ++kk) {
    for (auto jj = fAreaK_.PhysStartJ(); jj < fAreaK_.PhysEndJ(); ++jj) {
      for (auto ii = fAreaK_.PhysStartI(); ii < fAreaK_.PhysEndI(); ++ii) {
        // calculate gradients
        tensor<double> velGrad;
        vector3d<double> tempGrad, denGrad, pressGrad, tkeGrad, omegaGrad;
        vector<vector3d<double>> mixGrad;
        this->CalcGradsK(ii, jj, kk, velGrad, tempGrad, denGrad, pressGrad,
                         tkeGrad, omegaGrad, mixGrad);

        nodeData.velocityGrad_(ii, jj, kk) += velGrad;
        nodeData.velocityGrad_(ii + 1, jj, kk) += velGrad;
        nodeData.velocityGrad_(ii, jj + 1, kk) += velGrad;
        nodeData.velocityGrad_(ii + 1, jj + 1, kk) += velGrad;

        nodeData.temperatureGrad_(ii, jj, kk) += tempGrad;
        nodeData.temperatureGrad_(ii + 1, jj, kk) += tempGrad;
        nodeData.temperatureGrad_(ii, jj + 1, kk) += tempGrad;
        nodeData.temperatureGrad_(ii + 1, jj + 1, kk) += tempGrad;

        nodeData.densityGrad_(ii, jj, kk) += denGrad;
        nodeData.densityGrad_(ii + 1, jj, kk) += denGrad;
        nodeData.densityGrad_(ii, jj + 1, kk) += denGrad;
        nodeData.densityGrad_(ii + 1, jj + 1, kk) += denGrad;

        nodeData.pressureGrad_(ii, jj, kk) += pressGrad;
        nodeData.pressureGrad_(ii + 1, jj, kk) += pressGrad;
        nodeData.pressureGrad_(ii, jj + 1, kk) += pressGrad;
        nodeData.pressureGrad_(ii + 1, jj + 1, kk) += pressGrad;

        nodeData.tkeGrad_(ii, jj, kk) += tkeGrad;
        nodeData.tkeGrad_(ii + 1, jj, kk) += tkeGrad;
        nodeData.tkeGrad_(ii, jj + 1, kk) += tkeGrad;
        nodeData.tkeGrad_(ii + 1, jj + 1, kk) += tkeGrad;

        nodeData.omegaGrad_(ii, jj, kk) += omegaGrad;
        nodeData.omegaGrad_(ii + 1, jj, kk) += omegaGrad;
        nodeData.omegaGrad_(ii, jj + 1, kk) += omegaGrad;
        nodeData.omegaGrad_(ii + 1, jj + 1, kk) += omegaGrad;
        if (isMultiSpecies_) {
          for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
            nodeData.mixtureGrad_(ii, jj, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii + 1, jj, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii, jj + 1, kk, ss) += mixGrad[ss];
            nodeData.mixtureGrad_(ii + 1, jj + 1, kk, ss) += mixGrad[ss];
          }
        }
      }
    }
  }

  constexpr auto interiorFactor = 1.0 / 12.0;
  constexpr auto boundaryFactor = 1.0 / 8.0;
  constexpr auto edgeFactor = 1.0 / 5.0;
  constexpr auto cornerFactor = 1.0 / 3.0;
  string edge = "";
  auto bnd = 0;
  for (auto kk = nodeData.StartK(); kk < nodeData.EndK(); ++kk) {
    for (auto jj = nodeData.StartJ(); jj < nodeData.EndJ(); ++jj) {
      for (auto ii = nodeData.StartI(); ii < nodeData.EndI(); ++ii) {
        if (nodeData.velocityGrad_.AtInteriorCorner(ii, jj, kk)) {
          nodeData.velocityGrad_(ii, jj, kk) *= cornerFactor;
          nodeData.temperatureGrad_(ii, jj, kk) *= cornerFactor;
          nodeData.densityGrad_(ii, jj, kk) *= cornerFactor;
          nodeData.pressureGrad_(ii, jj, kk) *= cornerFactor;
          nodeData.tkeGrad_(ii, jj, kk) *= cornerFactor;
          nodeData.omegaGrad_(ii, jj, kk) *= cornerFactor;
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              nodeData.mixtureGrad_(ii, jj, kk, ss) *= cornerFactor;
            }
          }
        } else if (nodeData.velocityGrad_.AtInteriorEdge(ii, jj, kk, edge)) {
          nodeData.velocityGrad_(ii, jj, kk) *= edgeFactor;
          nodeData.temperatureGrad_(ii, jj, kk) *= edgeFactor;
          nodeData.densityGrad_(ii, jj, kk) *= edgeFactor;
          nodeData.pressureGrad_(ii, jj, kk) *= edgeFactor;
          nodeData.tkeGrad_(ii, jj, kk) *= edgeFactor;
          nodeData.omegaGrad_(ii, jj, kk) *= edgeFactor;
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              nodeData.mixtureGrad_(ii, jj, kk, ss) *= edgeFactor;
            }
          }
        } else if (nodeData.velocityGrad_.AtInterior(ii, jj, kk, edge, bnd)) {
          nodeData.velocityGrad_(ii, jj, kk) *= boundaryFactor;
          nodeData.temperatureGrad_(ii, jj, kk) *= boundaryFactor;
          nodeData.densityGrad_(ii, jj, kk) *= boundaryFactor;
          nodeData.pressureGrad_(ii, jj, kk) *= boundaryFactor;
          nodeData.tkeGrad_(ii, jj, kk) *= boundaryFactor;
          nodeData.omegaGrad_(ii, jj, kk) *= boundaryFactor;
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              nodeData.mixtureGrad_(ii, jj, kk, ss) *= boundaryFactor;
            }
          }
        } else {
          nodeData.velocityGrad_(ii, jj, kk) *= interiorFactor;
          nodeData.temperatureGrad_(ii, jj, kk) *= interiorFactor;
          nodeData.densityGrad_(ii, jj, kk) *= interiorFactor;
          nodeData.pressureGrad_(ii, jj, kk) *= interiorFactor;
          nodeData.tkeGrad_(ii, jj, kk) *= interiorFactor;
          nodeData.omegaGrad_(ii, jj, kk) *= interiorFactor;
          if (isMultiSpecies_) {
            for (auto ss = 0; ss < this->NumSpecies(); ++ss) {
              nodeData.mixtureGrad_(ii, jj, kk, ss) *= interiorFactor;
            }
          }
        }
      }
    }
  }

  return nodeData;
}

void procBlock::Restriction(const procBlock &fine,
                            const multiArray3d<vector3d<int>> &toCoarse,
                            const multiArray3d<double> &volWeightFactor) {
  BlockRestriction(fine.state_, toCoarse, volWeightFactor, state_);
}