#include <iostream>      //cout, cerr, endl
#include "plot3d.h"
#include "vector3d.h"
#include "tensor.h"
#include "input.h"
#include <vector>        //stl vector
#include <algorithm>     //max_element
#include <numeric>       //accumulate
#include "blockVars.h"
#include "inviscidFlux.h"
#include "viscousFlux.h"
#include "viscBlockVars.h"
#include "primVars.h"
#include "eos.h"
#include "boundaryConditions.h"
#include "output.h"
#include <fenv.h>
#include <ctime>

using std::cout;
using std::cerr;
using std::endl;
using std::vector;
using std::clock_t;
using std::clock;

int main( int argc, char *argv[] ) {

  clock_t start;
  double duration;
  start = clock();

  feenableexcept(FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW);

  const string inputFile = argv[1];  //name of input file is the second argument (the executable being the first)

  const double eps = 1.0e-20;

  //Parse input file
  input inputVars = ReadInput(inputFile);

  //Read grid
  plot3dMesh mesh = ReadP3dGrid(inputVars.GridName());

  //Initialize state vector with nondimensional variables

  //get reference speed of sound
  idealGas eos(inputVars.Gamma(),inputVars.R());                          //create an equation of state
  double aRef = eos.GetSoS(inputVars.PRef(),inputVars.RRef());

  //get a reference velocity
  vector3d<double> velRef = inputVars.VelRef();

  //initialize a single state
  primVars state(1.0, 1.0/eos.Gamma(), velRef/aRef);

  //initialize sutherland's law for viscosity
  sutherland suth(inputVars.TRef());

  //initialize the whole mesh
  vector<blockVars> stateBlocks( mesh.Blocks().size() );
  vector<viscBlockVars> viscBlocks( mesh.Blocks().size() );
  unsigned int ll = 0;
  for ( ll = 0; ll < mesh.Blocks().size(); ll++) {
    stateBlocks[ll] = blockVars(state, mesh.Blocks()[ll]);
    if (inputVars.EquationSet() == "navierStokes"){
      viscBlocks[ll] = viscBlockVars(mesh.Blocks()[ll]);
    }
    else if (inputVars.EquationSet() == "euler"){
      //do nothing extra
    }
    else{
      cerr << "ERROR: Equation set " << inputVars.EquationSet() << " is not recognized!" << endl;
      exit(0);
    }

  }


  //vector<blockVars> newStateBlocks = stateBlocks;

  cout << endl << "Solution Initialized" << endl;

  unsigned int bb = 0;
  unsigned int cc = 0;
  int nn = 0;

  //HARD CODED
  //int totalNumCells = 144;

  int locMaxB = 0;

  vector<double> residL2(5, 0.0);
  vector<double> residL2First(5, 0.0);
  vector<double> residLinf(5, 0.0);


  //Write out cell centers grid file
  WriteCellCenter(inputVars.GridName(),stateBlocks);

  for ( nn = 0; nn < inputVars.Iterations(); nn++ ){            //loop over time


    for ( bb = 0; bb < mesh.Blocks().size(); bb++ ){             //loop over number of blocks

      //calculate inviscid fluxes
      stateBlocks[bb].CalcInvFluxI(eos, inputVars, bb);
      stateBlocks[bb].CalcInvFluxJ(eos, inputVars, bb);
      stateBlocks[bb].CalcInvFluxK(eos, inputVars, bb);

      //if viscous, calculate gradients and viscous fluxes
      if (inputVars.EquationSet() == "navierStokes"){
	viscBlocks[bb].CalcCellGrads(stateBlocks[bb], eos, inputVars, bb);
	viscBlocks[bb].CalcViscFluxI(stateBlocks[bb], suth, eos, inputVars, bb);
	viscBlocks[bb].CalcViscFluxJ(stateBlocks[bb], suth, eos, inputVars, bb);
	viscBlocks[bb].CalcViscFluxK(stateBlocks[bb], suth, eos, inputVars, bb);
      }

      //calculate residuals and cell time step
      if (inputVars.EquationSet() == "navierStokes"){
      	viscBlocks[bb].CalcBlockResidDT(stateBlocks[bb], inputVars, aRef);
      }
      else{
      	stateBlocks[bb].CalcBlockResidDT(inputVars, aRef);
      }

      //update solution
      stateBlocks[bb].UpdateBlock(inputVars, eos, aRef, bb, residL2, residLinf, locMaxB);

      //get block residuals
      //stateBlocks[bb].TotalResidual(residL2, residLinf, locMaxB, bb);

    } //loop for blocks


    //finish calculation of L2 norm of residual
    for ( cc = 0; cc < residL2.size(); cc++ ){
      residL2[cc] = sqrt(residL2[cc]);

      if (nn == 0){
	residL2First[cc] = residL2[cc];
      }

      //normalize residuals
      residL2[cc] = (residL2[cc]+eps) / (residL2First[cc]+eps) ;
    }


    //print out run information
    if (nn%100 == 0){  
      if (inputVars.Dt() > 0.0){
	cout << "STEP     DT     RES-Mass     Res-Mom-X     Res-Mom-Y     Res-Mom-Z     Res-Energy    Max Res Eqn    Max Res Blk    Max Res I    Max Res J    Max Res K    Max Res" << endl;
      }
      else if (inputVars.CFL() > 0.0){
	cout << "STEP     CFL     RES-Mass     Res-Mom-X     Res-Mom-Y     Res-Mom-Z     Res-Energy   Max Res Eqn    Max Res Blk    Max Res I    Max Res J    Max Res K    Max Res" << endl;
      }

    }
    if (inputVars.Dt() > 0.0){
      cout << nn << "     " << inputVars.Dt() << "     " << residL2[0] <<  "     " << residL2[1] << "     " << residL2[2] << "     " << residL2[3] << "     " << residL2[4] << "     " 
           << residLinf[3] << "     " << locMaxB << "     " << residLinf[0] <<"     " << residLinf[1] << "     " << residLinf[2] << "     " << residLinf[4] << endl;
    }
    else if (inputVars.CFL() > 0.0){
      cout << nn << "     " << inputVars.CFL() << "     " << residL2[0] <<  "     " << residL2[1] << "     " << residL2[2] << "     " << residL2[3] << "     " << residL2[4] << "     " 
           << residLinf[3] << "     " << locMaxB << "     " << residLinf[0] <<"     " << residLinf[1] << "     " << residLinf[2] << "     " << residLinf[4] << endl;
    }

    //reset residuals
    for ( cc = 0; cc < residL2.size(); cc++ ){
      residL2[cc] = 0.0;
      residLinf[cc] = 0.0;
    }
    locMaxB = 0;

    if ( (nn+1)  % inputVars.OutputFrequency() == 0 ){ //write out function file
      cout << "write out function file at iteration " << nn << endl;
      //Write out function file
      WriteFun(inputVars.GridName(),stateBlocks, viscBlocks, eos, (double) (nn+1), inputVars.RRef(), aRef, inputVars.TRef());
      WriteRes(inputVars.GridName(), (nn+1), inputVars.OutputFrequency());
    }


  } //loop for time


  cout << endl;
  cout << "Program Complete" << endl;
  PrintTime();

  duration = (clock() - start)/(double) CLOCKS_PER_SEC;
  cout << "Total Time: " << duration << " seconds" << endl;

  return 0;
}