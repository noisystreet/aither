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

#ifndef WALLDATAHEADERDEF
#define WALLDATAHEADERDEF

/* This header file contains the wallData class
*/

#include <vector>
#include <memory>
#include "mpi.h"
#include "multiArray3d.hpp"
#include "inputStates.hpp"
#include "boundaryConditions.hpp"
#include "range.hpp"

using std::vector;
using std::shared_ptr;

// forward declarations
class input;
class eos;
class primitive;

// structure to hold wall variables
struct wallVars {
  vector3d<double> shearStress_ = {0.0, 0.0, 0.0};
  double heatFlux_ = 0.0;
  double yplus_ = 0.0;
  double temperature_ = 0.0;
  double turbEddyVisc_ = 0.0;
  double viscosity_ = 0.0;
  double density_ = 0.0;
  double frictionVelocity_ = 0.0;
  double tke_ = 0.0;
  double sdr_ = 0.0;
  vector<double> mf_;

  // constructor
  explicit wallVars(const int &ns) : mf_(ns, 0.0) {}
  wallVars() : wallVars(0) {}

  bool SwitchToLowRe() const {return yplus_ < 10.;}
  void Pack(char *(&), const int &, int &, const MPI_Datatype &) const;
  void PackSize(int &, const MPI_Datatype &) const;
  void Unpack(char *(&), const int &, int &, const MPI_Datatype &,
              const int &);
};

class wallData {
  double inviscidForce_;
  double viscousForce_;
  int numSpecies_;
  shared_ptr<inputState> bcData_;
  boundarySurface surf_;
  multiArray3d<wallVars> data_;

 public:
  // constructor
  wallData(const boundarySurface &surf, const shared_ptr<inputState> &bc,
           const int &numSpecies)
      : inviscidForce_(0.0),
        viscousForce_(0.0),
        numSpecies_(numSpecies),
        bcData_(bc),
        surf_(surf),
        data_(surf.NumI(), surf.NumJ(), surf.NumK(), 0, 1,
              wallVars(numSpecies)) {}
  wallData() : wallData(boundarySurface(), nullptr, 0) {}

  // move constructor and assignment operator
  wallData(wallData &&) = default;
  wallData &operator=(wallData &&) = default;

  // copy constructor and assignment operator
  wallData(const wallData &) = default;
  wallData &operator=(const wallData &) = default;

  // member functions
  int NumI() const { return data_.NumI(); }
  int NumJ() const { return data_.NumJ(); }
  int NumK() const { return data_.NumK(); }
  int Size() const { return data_.Size(); }
  int NumSpecies() const { return numSpecies_; }
  double InviscidForce() const { return inviscidForce_; }
  double ViscousForce() const { return viscousForce_; }
  vector3d<double> WallShearStress(const int &ii, const int &jj,
                                   const int &kk) const;
  double WallHeatFlux(const int &ii, const int &jj, const int &kk) const;
  double Yplus(const int &ii, const int &jj, const int &kk) const;
  double WallTemperature(const int &ii, const int &jj, const int &kk) const;
  double WallEddyViscosity(const int &ii, const int &jj, const int &kk) const;
  double WallViscosity(const int &ii, const int &jj, const int &kk) const;
  double WallDensity(const int &ii, const int &jj, const int &kk) const;
  vector<double> WallMassFractions(const int &ii, const int &jj, const int &kk) const;
  vector<double> WallDensityVec(const int &ii, const int &jj, const int &kk) const;
  double WallTke(const int &ii, const int &jj, const int &kk) const;
  double WallSdr(const int &ii, const int &jj, const int &kk) const;
  double WallPressure(const int &ii, const int &jj, const int &kk,
                      const unique_ptr<eos> &eqnState) const;
  double WallFrictionVelocity(const int &ii, const int &jj,
                              const int &kk) const;
  vector3d<double> WallVelocity() const {return bcData_->Velocity();}
  void WallState(const int &ii, const int &jj, const int &kk,
                 const unique_ptr<eos> &eqnState, primitive &wState) const;
  int WallVarsSize() const { return data_.Size(); }
  void PackWallData(char *(&), const int &, int &, const MPI_Datatype &) const;
  void PackSize(int &, const MPI_Datatype &) const;
  void UnpackWallData(char *(&), const int &, int &, const MPI_Datatype &,
                      const input &);
  const boundarySurface & Surface() const {return surf_;}
  bool IsWallLaw() const {return bcData_->IsWallLaw();}
  wallData Split(const string &, const int &, bool &, bool &);
  void Join(const wallData &, const string &, bool &);
  void Print(ostream &) const;
  bool SwitchToLowRe(const int &ii, const int &jj,
                     const int &kk, const bool &raw = false) const {
    return (*this)(ii, jj, kk, raw).SwitchToLowRe();
  }

  // operator overloads
  wallVars &operator()(const int &ii, const int &jj, const int &kk,
                       const bool &raw = false) {
    return raw ? data_(ii, jj, kk)
               : data_(ii - surf_.IMin(), jj - surf_.JMin(), kk - surf_.KMin());
  }
  const wallVars &operator()(const int &ii, const int &jj, const int &kk,
                             const bool &raw = false) const {
    return raw ? data_(ii, jj, kk)
               : data_(ii - surf_.IMin(), jj - surf_.JMin(), kk - surf_.KMin());
  }

  // destructor
  ~wallData() noexcept {}
};

// function definitions
ostream &operator<<(ostream &os, const wallData &wd);
ostream &operator<<(ostream &os, const wallVars &wv);

#endif
