#ifndef INPUTHEADERDEF             //only if the macro INPUTHEADERDEF is not defined execute these lines of code
#define INPUTHEADERDEF             //define the macro

#include <vector>  //vector
#include <string>  //string
#include "vector3d.h"
#include "boundaryConditions.h"

using std::vector;
using std::string;

class input {

  string gName;                         //grid file name
  double dt;                            //time step
  int iterations;                       //number of iterations
  vector<string> vars;                  //variable names that are regcognized by the input file parser
  double pRef;                          //reference pressure
  double rRef;                          //reference density
  double lRef;                          //reference length
  double gamma;                         //ratio of specific heats
  double gasConst;                      //gas constant of fluid
  vector3d<double> velRef;              //reference velocity
  vector<boundaryConditions> bc;        //vector of boundary conditions for each block
  string timeIntegration;               //time integration method
  double cfl;                           //cfl number for local time stepping
  double kappa;                         //kappa paramenter for MUSCL face reconstruction
  string limiter;                       //limiter to use in higher order calculations
  int outputFrequency;                  //how often to output results
  string equationSet;                   //which set of equations to solver Euler/Navier-Stokes
  double tRef;                          //reference temperature

 public:
  //constructor
  input();

  //member functions
  string GridName()const{return gName;}
  void SetGridName(const string &name){gName = name;}

  double Dt()const{return dt;}
  void SetDt(const double &a){dt = a;}

  int Iterations()const{return iterations;}
  void SetIterations(const int &i){iterations = i;}

  double PRef()const{return pRef;}
  void SetPRef(const double &a){pRef = a;}

  double RRef()const{return rRef;}
  void SetRRef(const double &a){rRef = a;}

  double LRef()const{return lRef;}
  void SetLRef(const double &a){lRef = a;}

  double TRef()const{return tRef;}
  void SetTRef(const double &a){tRef = a;}

  double Gamma()const{return gamma;}
  void SetGamma(const double &a){gamma = a;}

  double R()const{return gasConst;}
  void SetR(const double &a){gasConst = a;}

  vector3d<double> VelRef()const{return velRef;}
  void SetVelRef(const vector3d<double> &a){velRef = a;}

  vector<boundaryConditions> BC()const{return bc;}
  void SetBC(const vector<boundaryConditions> &a){bc = a;}
  void SetBCVec(const int &a);

  string TimeIntegration()const{return timeIntegration;}
  void SetTimeIntegration(const string &a){timeIntegration = a;}

  double CFL()const{return cfl;}
  void SetCFL(const double &a){cfl = a;}

  double Kappa()const{return kappa;}
  void SetKappa(const double &a){kappa = a;}

  string Limiter()const{return limiter;}
  void SetLimiter(const string &a){limiter = a;}

  int OutputFrequency()const{return outputFrequency;}
  void SetOutputFrequency(const int &a){outputFrequency = a;}

  string EquationSet()const{return equationSet;}
  void SetEquationSet(const string &a){equationSet = a;}

  vector<string> Vars()const{return vars;}

  //destructor
  ~input() {}

};

//function declarations
input ReadInput(const string &inputName);
void PrintTime();


#endif
