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

#ifndef UNCOUPLEDSCALARHEADERDEF  // only if the macro is not defined
                               // execute these lines of code
#define UNCOUPLEDSCALARHEADERDEF  // define the macro

#include <iostream>        // cout
#include <algorithm>       // max
#include <type_traits>
#include "arrayView.hpp"

using std::cout;
using std::endl;
using std::cerr;
using std::ostream;

// forward class declarations
class varArray;

// This class holds variables that may be different for each set of
// equations being solved. For example the spectral radii of the flow
// equations and turbulence equations are different.

class uncoupledScalar {
  double flowVar_;
  double turbVar_;

 public:
  // constructors
  uncoupledScalar(const double &flow, const double &turb) :
      flowVar_(flow), turbVar_(turb) {}
  uncoupledScalar() : uncoupledScalar(0.0, 0.0) {}

  // move constructor and assignment operator
  uncoupledScalar(uncoupledScalar &&) noexcept = default;
  uncoupledScalar& operator=(uncoupledScalar &&) noexcept = default;

  // copy constructor and assignment operator
  uncoupledScalar(const uncoupledScalar &) = default;
  uncoupledScalar& operator=(const uncoupledScalar &) = default;

  // member functions
  double FlowVariable() const {return flowVar_;}
  double TurbVariable() const {return turbVar_;}

  void AddToFlowVariable(const double &s) {flowVar_ += s;}
  void AddToTurbVariable(const double &s) {turbVar_ += s;}
  void SubtractFromFlowVariable(const double &s) {flowVar_ -= s;}
  void SubtractFromTurbVariable(const double &s) {turbVar_ -= s;}

  double Max() const {return std::max(flowVar_, turbVar_);}
  double Min() const {return std::min(flowVar_, turbVar_);}
  
  void Zero() {
    flowVar_ = 0.0;
    turbVar_ = 0.0;
  }

  template <typename T,
            typename = std::enable_if_t<std::is_base_of<varArray, T>::value>>
  T ArrayMult(T arr) const {
    for (auto ii = 0; ii < arr.TurbulenceIndex(); ++ii) {
      arr[ii] *= flowVar_;
    }
    for (auto ii = arr.TurbulenceIndex(); ii < arr.Size(); ++ii) {
      arr[ii] *= turbVar_;
    }
    return arr;
  }
  template <typename T,
            typename = std::enable_if_t<std::is_same<varArrayView, T>::value ||
                                        std::is_same<primitiveView, T>::value ||
                                        std::is_same<conservedView, T>::value ||
                                        std::is_same<residualView, T>::value>>
  auto ArrayMult(const T &arrView) const {
    auto arr = arrView.CopyData();
    return this->ArrayMult(arr);
  }

  inline uncoupledScalar &operator+=(const uncoupledScalar &);
  inline uncoupledScalar & operator-=(const uncoupledScalar &);
  inline uncoupledScalar & operator*=(const uncoupledScalar &);
  inline uncoupledScalar & operator/=(const uncoupledScalar &);

  inline uncoupledScalar & operator+=(const double &);
  inline uncoupledScalar & operator-=(const double &);
  inline uncoupledScalar & operator*=(const double &);
  inline uncoupledScalar & operator/=(const double &);

  inline uncoupledScalar operator+(const double &s) const {
    auto lhs = *this;
    return lhs += s;
  }
  inline uncoupledScalar operator-(const double &s) const {
    auto lhs = *this;
    return lhs -= s;
  }
  inline uncoupledScalar operator*(const double &s) const {
    auto lhs = *this;
    return lhs *= s;
  }
  inline uncoupledScalar operator/(const double &s) const {
    auto lhs = *this;
    return lhs /= s;
  }

  friend inline const uncoupledScalar operator-(const double &lhs,
                                             uncoupledScalar rhs);
  friend inline const uncoupledScalar operator/(const double &lhs,
                                             uncoupledScalar rhs);

  // destructor
  ~uncoupledScalar() noexcept {}
};

// function definitions

// operator overload for addition
uncoupledScalar & uncoupledScalar::operator+=(const uncoupledScalar &other) {
  flowVar_ += other.flowVar_;
  turbVar_ += other.turbVar_;
  return *this;
}

// operator overload for subtraction with a scalar
uncoupledScalar & uncoupledScalar::operator-=(const uncoupledScalar &other) {
  flowVar_ -= other.flowVar_;
  turbVar_ -= other.turbVar_;
  return *this;
}

// operator overload for elementwise multiplication
uncoupledScalar & uncoupledScalar::operator*=(const uncoupledScalar &other) {
  flowVar_ *= other.flowVar_;
  turbVar_ *= other.turbVar_;
  return *this;
}

// operator overload for elementwise division
uncoupledScalar & uncoupledScalar::operator/=(const uncoupledScalar &other) {
  flowVar_ /= other.flowVar_;
  turbVar_ /= other.turbVar_;
  return *this;
}

inline const uncoupledScalar operator+(uncoupledScalar lhs,
                                       const uncoupledScalar &rhs) {
  return lhs += rhs;
}

inline const uncoupledScalar operator-(uncoupledScalar lhs,
                                       const uncoupledScalar &rhs) {
  return lhs -= rhs;
}

inline const uncoupledScalar operator*(uncoupledScalar lhs,
                                       const uncoupledScalar &rhs) {
  return lhs *= rhs;
}

inline const uncoupledScalar operator/(uncoupledScalar lhs,
                                       const uncoupledScalar &rhs) {
  return lhs /= rhs;
}

// operator overloads for double -------------------------------------
// operator overload for addition
uncoupledScalar & uncoupledScalar::operator+=(const double &scalar) {
  flowVar_ += scalar;
  turbVar_ += scalar;
  return *this;
}

// operator overload for subtraction with a scalar
uncoupledScalar & uncoupledScalar::operator-=(const double &scalar) {
  flowVar_ -= scalar;
  turbVar_ -= scalar;
  return *this;
}

// operator overload for elementwise multiplication
uncoupledScalar & uncoupledScalar::operator*=(const double &scalar) {
  flowVar_ *= scalar;
  turbVar_ *= scalar;
  return *this;
}

// operator overload for elementwise division
uncoupledScalar & uncoupledScalar::operator/=(const double &scalar) {
  flowVar_ /= scalar;
  turbVar_ /= scalar;
  return *this;
}

inline const uncoupledScalar operator+(const double &lhs, uncoupledScalar rhs) {
  return rhs += lhs;
}

inline const uncoupledScalar operator-(const double &lhs, uncoupledScalar rhs) {
  rhs.flowVar_ = lhs - rhs.flowVar_;
  rhs.turbVar_ = lhs - rhs.turbVar_;
  return rhs;
}

inline const uncoupledScalar operator*(const double &lhs, uncoupledScalar rhs) {
  return rhs *= lhs;
}

inline const uncoupledScalar operator/(const double &lhs, uncoupledScalar rhs) {
  rhs.flowVar_ = lhs / rhs.flowVar_;
  rhs.turbVar_ = lhs / rhs.turbVar_;
  return rhs;
}

ostream &operator<<(ostream &os, const uncoupledScalar &);

#endif
