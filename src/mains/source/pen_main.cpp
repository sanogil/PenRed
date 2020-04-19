
//
//
//    Copyright (C) 2019-2020 Universitat de València - UV
//    Copyright (C) 2019-2020 Universitat Politècnica de València - UPV
//
//    This file is part of PenRed: Parallel Engine for Radiation Energy Deposition.
//
//    PenRed is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    PenRed is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with PenRed.  If not, see <https://www.gnu.org/licenses/>. 
//
//    contact emails:
//
//        vicent.gimenez.alventosa@gmail.com
//        vicente.gimenez@uv.es
//    
//


#include <thread>
#include <limits>
#include "pen_loops.hh"

int createParticleGenerators(std::vector<pen_specificStateGen<pen_particleState>>& genericSources,
			     std::vector<pen_specificStateGen<pen_state_gPol>>& polarisedGammaSources,
			     std::vector<double>& genericSourceHists,
			     std::vector<double>& polarisedSourceHists,
			     const unsigned nthreads,
			     const pen_parserSection& config,
			     const unsigned verbose);
int createTallies(std::vector<pen_commonTallyCluster>& tallyGroups,
		  const size_t nthreads,
		  const pen_context& context,
		  const pen_parserSection& config,
		  const unsigned verbose);

int createGeometry(wrapper_geometry*& geometry,
		   const pen_parserSection& config,
		   const pen_context& context,
		   const unsigned verbose);

int createMaterials(pen_context& context,
		    std::string filenames[constants::MAXMAT],
		    const pen_parserSection& config,
		    const unsigned verbose);

int setVarianceReduction(pen_context& context,
			 const wrapper_geometry& geometry,
			 const pen_parserSection& config,
			 const unsigned verbose);

template<class stateType>
int configureSource(pen_specificStateGen<stateType>& source,
		    const unsigned& nthreads,
		    const pen_parserSection& config,
		    const unsigned verbose){
  
  //Check if specific sampler exists
  bool useSpecific = config.isSection("specific");
  
  //Get source kpar
  std::string auxkpar;
  int err = config.read("kpar",auxkpar);
  if(err != INTDATA_SUCCESS && !useSpecific){
    if(verbose > 0){
      printf("configureSource: Error: Particle type field ('kpar') not found and no specific sampling specified. String expected:\n");
      printf("\telectron\n");
      printf("\tgamma\n");
      printf("\tpositron\n");
    }
    return -1;
  } else if(!useSpecific && verbose > 1) {
    printf("Selected particle: %s\n",auxkpar.c_str());
  }

  //Obtain particle ID
  if(auxkpar.compare("electron") == 0)
    source.kpar = PEN_ELECTRON;
  else if(auxkpar.compare("gamma") == 0)
    source.kpar = PEN_PHOTON;
  else if(auxkpar.compare("positron") == 0)
    source.kpar = PEN_POSITRON;
  else if(!useSpecific){
    if(verbose > 0)
      printf("\nInvalid selected particle type: %s\n",auxkpar.c_str());
    return -2;
  }

  //Configure source
  source.configure(config,nthreads,verbose);
  if(source.configureStatus() != 0){
    if(verbose > 0){
      printf("\nError on source configuration\n");
    }
    return -3;
  }
  
  return 0;
}

template<class stateType>
void simulate(const unsigned ithread,
	      const double nhists,
	      const double initHist,
	      const pen_context* pcontext,
	      pen_specificStateGen<stateType>* psource,
	      pen_commonTallyCluster* ptallies,
	      int* seed1, int* seed2,
	      const double dumpTime,
	      const std::string dumpFilename,
	      double* nsimulated){

  const pen_context& context = *pcontext;
  pen_specificStateGen<stateType>& source = *psource;
  pen_commonTallyCluster& tallies = *ptallies;
  
  //Create random generator
  pen_rand random;
  random.setSeeds(*seed1,*seed2);
  
  pen_timer timer;
  double time0 = CPUtime();
  
  double hist = initHist+(*nsimulated); //Add previously simulated hists
  double lastHist = initHist+nhists+0.5;

  //Skip already simulated histories
  double simulated = (*nsimulated);
  if(simulated > 0){
    printf("Thread %u: Source '%s': Skip %.0f already simulated histories.\n",ithread,source.name.c_str(),simulated);
    source.skip(simulated,ithread);
  }
  
  //Check if there are histories to simulate
  if(lastHist - hist < 1.0){
    printf("Thread %u: Source '%s' has no histories to simulate (initial history %.0f/%.0f). Random seeds (%d,%d), Simulation CPU time: %12.4E s\n",ithread,source.name.c_str(),hist-initHist-1.0,nhists,*seed1,*seed2, CPUtime()-time0);
    fflush(stdout);
    
    //Save simulated histories
    (*nsimulated) = (hist-initHist);
    return;
  }
  
  //The function will report the number of simulated particles when the
  // simulation has been completed at 1/4, 1/2, 3/4 and at finish.
  const double printInterval = std::max(floor(0.1*nhists),1.0);
  double nextPrint = printInterval+initHist+0.3;
  
  //Create particle stacks for electrons, positrons and gamma
  pen_particleStack<pen_particleState> stackE;
  pen_particleStack<pen_particleState> stackP;
  pen_particleStack<pen_state_gPol> stackG;
  
  
  //Create particle simulations
  pen_betaE betaE(context,stackE,stackG);
  pen_gamma gamma(context,stackE,stackP,stackG);
  pen_betaP betaP(context,stackE,stackG);
  
  //History bucle
  stateType genState;
  pen_KPAR genKpar;
  unsigned long dhist;

  if(hist > initHist){
    printf("Thread %u: Resuming simulation of source '%s' with initial seeds %d %d at history %.0f\n",ithread,source.name.c_str(),*seed1,*seed2,hist);
  }
  else{
    printf("Thread %u: Starting simulation of source '%s' with initial seeds %d %d at history %.0f\n",ithread,source.name.c_str(),*seed1,*seed2,hist);
  }
  fflush(stdout);
  
  //Update tallies last hist
  tallies.run_lastHist(hist);
  
  //Sample first particle
  source.sample(genState,genKpar,dhist,ithread,random);
  
  //Check if is a valid kpar
  if(genKpar != PEN_ELECTRON &&
     genKpar != PEN_PHOTON &&
     genKpar != PEN_POSITRON &&
     hist + dhist < lastHist){
    //Invalid kpar or history number reached the limit
    //finish simulation
    printf("Thread %u: Source '%s' finished with %.0f/%.0f histories simulated\n",ithread,source.name.c_str(),simulated,nhists);
    fflush(stdout);

    //Update seeds for next source
    random.getSeeds(*seed1,*seed2);
    
    //Save simulated histories
    (*nsimulated) = (hist-initHist);
    return;
  }
  
  //Increment history counter
  hist += dhist;

  //Get detector ID
  unsigned firstKdet = context.getDET(genState.IBODY);
  //Tally first history begin
  tallies.run_beginHist(hist,firstKdet,genKpar,genState);
  
  for(;;){

    //Control if particle reached the geometry
    bool inGeometry = false;
    
    //Check particle type and simulate it
    if(genKpar == PEN_ELECTRON){
      
      //Copy generated state
      stateCopy(betaE.getState(),genState);

      //Init Page variables
      betaE.page0();
      
      //Try to move generated particle to geometry system
      if(move2geo(hist,betaE,tallies)){

	//particle reahed the geometry system
	inGeometry = true;
	
	//Get detector ID
	unsigned kdet = betaE.getDET();	

	//Simulate primary particle
	tallies.run_beginPart(hist,kdet,PEN_ELECTRON,betaE.readState());
	if(dhist == 0){
	  //Compensate extracted energy on "beginPart" functions
	  tallies.run_localEdep(hist,PEN_ELECTRON,betaE.readState(),
				betaE.getState().E);
	}
	simulatePart(hist,betaE,tallies,random);
	
      }
    }
    else if(genKpar == PEN_PHOTON){
      
      //Copy generated state
      stateCopy(gamma.getState(),genState);      

      //Init Page variables
      gamma.page0();

      //Try to move generated particle to geometry system
      if(move2geo(hist,gamma,tallies)){

	//particle reahed the geometry system
	inGeometry = true;
	
	//Get detector ID
	unsigned kdet = gamma.getDET();
	
	//Simulate primary particle
	tallies.run_beginPart(hist,kdet,PEN_PHOTON,gamma.readState());
	if(dhist == 0){
	  //Compensate extracted energy on "beginPart" functions
	  tallies.run_localEdep(hist,PEN_PHOTON,gamma.readState(),
				gamma.getState().E);
	}
	simulatePart(hist,gamma,tallies,random);
	
      }
    }
    else if(genKpar == PEN_POSITRON){

      //Copy generated state
      stateCopy(betaP.getState(),genState);      

      //Init Page variables
      betaP.page0();

      //Try to move generated particle to geometry system
      if(move2geo(hist,betaP,tallies)){

	//particle reahed the geometry system
	inGeometry = true;
	
	//Get detector ID
	unsigned kdet = betaP.getDET();	
	
	//Simulate primary particle
	tallies.run_beginPart(hist,kdet,PEN_POSITRON,betaP.readState());
	if(dhist == 0){
	  //Compensate extracted energy on "beginPart" functions
	  tallies.run_localEdep(hist,PEN_POSITRON,betaP.readState(),
				betaP.getState().E);
	}
	simulatePart(hist,betaP,tallies,random);
	
      }
    }
    else{
      //Unknown particle ID signals end of source
      printf("Thread %u: Source '%s' reached its end at %.0f/%.0f particles simulated\n",ithread,source.name.c_str(),(hist-initHist)-1.0,nhists);
      //End current hist
      tallies.run_endHist(hist);
      break;
    }
    
    //Simulate until all stacks are empty
    if(inGeometry){
      for(;;){

	//Get the state and number of stacked particles
	unsigned nBetaE = stackE.getNSec();
	unsigned nGamma = stackG.getNSec();
	unsigned nBetaP = stackP.getNSec();

	unsigned nBetaE05 = 0;
	unsigned nGamma05 = 0;
	unsigned nBetaP05 = 0;
      
	//Check end of simulation
	bool remaining = false;
	if(nBetaE > 0){
	  remaining = true;
	  nBetaE05 = nBetaE/2+1;
	}
	if(nGamma > 0){
	  remaining = true;
	  nGamma05 = nGamma/2+1;
	}
	if(nBetaP > 0){
	  remaining = true;
	  nBetaP05 = nBetaP/2+1;
	}

	//Check if the stacks are empty
	if(!remaining)
	  break;
      
      
	//Simulate half stacked particles
	unsigned nbetaEsim = 0;
	while(nbetaEsim < nBetaE05){
	  stackE.get(betaE.getState());

	  betaE.updateBody();
	  
	  //Check if this particle has sufficient energy
	  if(betaE.getState().E < betaE.getEABS()){
	    nbetaEsim++;
	    continue;
	  }
	  
	  betaE.updateMat();
	  
	  //Get kdet
	  unsigned kdet = betaE.getDET();
	
	  tallies.run_beginPart(hist,kdet,PEN_ELECTRON,betaE.readState());
	  simulatePart(hist,betaE,tallies,random);
	
	  nbetaEsim++;
	}
	unsigned ngammasim = 0;
	while(ngammasim < nGamma05){
	  stackG.get(gamma.getState());

	  gamma.updateBody();
	  
	  //Check if this particle has sufficient energy
	  if(gamma.getState().E < gamma.getEABS()){
	    ngammasim++;
	    continue;
	  }
	  
	  gamma.updateMat();

	  //Get kdet
	  unsigned kdet = gamma.getDET();
	
	  // ****  x-ray splitting
	
	  //Get gamma state
	  pen_state_gPol& state = gamma.getState();

	  if(context.LXRSPL[state.IBODY] && state.ILB[3] > 0){
	    //Is a characteristic x-ray in a body with x-ray splitting enabled
	    if(state.ILB[0] == 2 && state.ILB[2] < 9){
	      // Unsplitted 2nd generation photon
	      state.WGHT /= (double) context.IXRSPL[state.IBODY];
	      state.ILB[2] = 9; //Labels split x rays
	    
	      //Create and store 'IXRSPL[IBODY]' states
	      pen_state_gPol stateSplit;
	      stateSplit = state;

	      for(unsigned isplit = 1; isplit < context.IXRSPL[state.IBODY]; isplit++){
	      
		stateSplit.W = -1.0 + 2.0 * random.rand();
		double SDTS = sqrt(1.0 - stateSplit.W*stateSplit.W);
		double DF = constants::TWOPI*random.rand();
		stateSplit.U = cos(DF)*SDTS;
		stateSplit.V = sin(DF)*SDTS;
		stackG.store(stateSplit);
	      }
	    }
	  }
	  // **********************

	  tallies.run_beginPart(hist,kdet,PEN_PHOTON,state);
	  simulatePart(hist,gamma,tallies,random);
	
	  ngammasim++;
	}
	unsigned nbetaPsim = 0;
	while(nbetaPsim < nBetaP05){
	  stackP.get(betaP.getState());

	  betaP.updateMat();
	  betaP.updateBody();
	  
	  //Check if this particle has sufficient energy
	  if(betaP.readState().E < betaP.getEABS()){

	    //Get kdet
	    unsigned kdet = betaP.getDET();
	    //Tally positron beginning
	    tallies.run_beginPart(hist,kdet,PEN_POSITRON,betaP.readState());
	    
	    // run annihilation process
	    double Eprod = betaP.annihilationEDep;
	    betaP.annihilate(random);
	    tallies.run_localEdep(hist,PEN_POSITRON,
				  betaP.readState(),betaP.readState().E+Eprod);

	    //Call tallies with end particle collect function
	    tallies.run_endPart(hist,PEN_POSITRON,betaP.readState());
	    
	    nbetaPsim++;
	    continue;
	  }
	  
	  //Get kdet
	  unsigned kdet = betaP.getDET();
	
	  tallies.run_beginPart(hist,kdet,PEN_POSITRON,betaP.readState());
	  simulatePart(hist,betaP,tallies,random);
	
	  nbetaPsim++;
	}
      }
    }

    //Get last seeds
    int lseed1, lseed2;
    random.getSeeds(lseed1,lseed2);
    
    //Sample new particle state
    source.sample(genState,genKpar,dhist,ithread,random);
    
    //Check if is a valid particle
    if(genKpar >= ALWAYS_AT_END){

      //End last hist
      tallies.run_endHist(hist);      
      break;
    }
    
    if(dhist > 0){
      //End previous history
      tallies.run_endHist(hist);

      //Check if elapsed time reaches dump interval
      if(timer.timer() > dumpTime){
	//Save dump
	double currentHists = hist-initHist;
	printf("Dumping simulation with last hist %.0f and seeds %d %d\n",currentHists,lseed1,lseed2);
	fflush(stdout);
	tallies.dump2file(dumpFilename.c_str(),
			  hist,
			  lseed1,lseed2,
			  3);
	tallies.saveData(currentHists,false); //Save data but doesn't repeats flush calls
	timer.time0();
      }
      
      //Increment history counter
      hist += dhist;

      //Check history limit
      if(hist < lastHist){
	//Get detector ID
	unsigned kdet = context.getDET(genState.IBODY);
	//Tally new history
	tallies.run_beginHist(hist,kdet,genKpar,genState);
      }
      else{
	//End of simulation
	break;
      }
      
      //Print simulated histories report
      if(hist > nextPrint){
	printf("Thread %u: Simulated %.0f/%.0f histories from source '%s'\n",ithread,hist-initHist,nhists,source.name.c_str());
	fflush(stdout);
	nextPrint += printInterval;
      }
      
    }
  }

  //Update seeds for next source
  random.getSeeds(*seed1,*seed2);
  
  printf("Thread %u: Source '%s' finished with %.0f/%.0f histories simulated and random seeds (%d,%d)          Simulation CPU time: %12.4E s\n",ithread,source.name.c_str(),hist-initHist-1.0,nhists,*seed1,*seed2, CPUtime()-time0);
  fflush(stdout);
  
  //Save simulated histories
  (*nsimulated) = (hist-initHist)-1.0;
  
}

int main(int argc, char** argv){

  if(argc < 2){
    printf("usage: %s config-filename\n",argv[0]);
    return 0;
  }
	
  if(strcmp(argv[1],"--version") == 0 || strcmp(argv[1],"-v") == 0){
    printf("PenRed 1.0.0\n");
    printf("Copyright (c) 2019 Universitat Politecnica de Valencia\n");
    printf("Copyright (c) 2019 Universitat de Valencia\n");
    printf("This is free software; see the source for copying conditions. "
	   " There is NO\n warranty; not even for MERCHANTABILITY or "
           "FITNESS FOR A PARTICULAR PURPOSE.\n");
    printf("Please, report bugs and suggestions at our github repository\n"
	  "     https://github.com/PenRed/PenRed\n");
    return 0;
  }

  unsigned verbose = 3;

  // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_
  //Initialize MPI
  int rank;
  int mpiSize;

  int MThProvided;
  int MPIinitErr = MPI_Init_thread(nullptr, nullptr, MPI_THREAD_SERIALIZED,&MThProvided);
  if(MPIinitErr != MPI_SUCCESS){
    printf("Unable to initialize MPI. Error code: %d\n",MPIinitErr);
    return -1;
  }
  if(MThProvided != MPI_THREAD_SERIALIZED){
    printf("Warning: The MPI implementation used doesn't provide"
	   "support for serialized thread communication.\n"
	   "This could produce unexpected behaviours or performance issues.\n");
  }
  
  //Get current process "rank" and the total number of processes
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);

  if(verbose > 1){
    if(rank == 0){
      printf("%d MPI processes started.\n",mpiSize);
    }
  }
  
  //Redirect stdout to individual log files
  char stdoutFilename[100];
  snprintf(stdoutFilename,100,"rank-%03d.log",rank);
  printf("Rank %d: Redirect 'stdout' to file '%s'\n",rank,stdoutFilename);
  if(freopen(stdoutFilename, "w", stdout) == NULL){
    printf("Rank %d: Can't redirect stdout to '%s'\n",rank,stdoutFilename);
  }
  else if(verbose > 1){
    printf("\n**** Rank %d Log ****\n\n",rank);
  }
    
#endif
  // ***************************** MPI END ********************************** //
  
  //**************************
  // Parse configuration
  //**************************

  //Parse configuration file
  pen_parserSection config;
  int err = parseFile(argv[1],config);

  //printf("Configuration:\n");
  //printf("%s\n", config.stringify().c_str());
  
  if(err != INTDATA_SUCCESS){
    printf("Error parsing configuration.\n");
    printf("Error code: %d\n",err);
    return -1;
  }

  // Get number of threads to use
  //*******************************
  int auxThreads;
  if(config.read("simulation/threads",auxThreads) != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("\n\nNumber of threads not specified, only one thread will be used.\n\n");
    }
    auxThreads = 1;
  }
  // ************************** MULTI-THREADING ***************************** //
#ifndef _PEN_USE_THREADS_
  else{
    printf("\n\nMulti-threading has not been activated at compilation\n\n");
  }
  auxThreads = 1;
#endif
  // ************************ MULTI-THREADING END *************************** //
  
  if(auxThreads <= 0){
    if(verbose > 0){
      printf("Warning: The simulation require as least one thread.\n");
      printf("         Number of threads will be set to 1.\n");
    }
    auxThreads = 1;
  }
  unsigned nthreads = (unsigned)auxThreads;
  
  if(verbose > 1){
    printf("\nNumber of simulating threads: %u\n",nthreads);
    if(nthreads > 1){
      printf("Initial random seeds will be selected using \"rand0\" function to ensure truly independent sequences of random numbers.\n");
    }
  }


  // Get initial seeds
  //*******************************
  int iseed1, iseed2;
  if(config.read("simulation/seed1",iseed1) != INTDATA_SUCCESS){
    iseed1 = 1;
  }  
  if(config.read("simulation/seed2",iseed2) != INTDATA_SUCCESS){
    iseed2 = 1;
  }

  if(iseed1 <= 0 || iseed2 <= 0){
    if(verbose > 0){
      printf("Invalid initial seeds: %d %d\n",iseed1,iseed2);
      printf(" Both must be greater than zero\n");
    }
    return -2;
  }
  
  if(verbose > 1){
    printf("Initial seeds: %d %d\n",iseed1,iseed2);
  }

  // Get initial seed pair number
  //*******************************
  int nseedPair;
  if(config.read("simulation/seedPair",nseedPair) != INTDATA_SUCCESS){
     nseedPair = -1;
  }
  else if(nseedPair < 0 || nseedPair > 1000){
    if(verbose > 0){
      printf("Invalid initial seed pair number %d\n",nseedPair);
      printf("Available seed pair range is [0,1000]\n");
    }
    return -2;
  }
  else if(verbose > 1){
    printf("Selected rand0 seed pair number: %d\n",nseedPair);
  }
  
  // Get CPU affinity option
  //*******************************
  bool CPUaffinity = false;
  if(config.read("simulation/thread-affinity",CPUaffinity) != INTDATA_SUCCESS){
    CPUaffinity = false;
  }  

  if(verbose > 1){
    if(CPUaffinity){
      printf("Threads CPU affinity enabled\n");
    }
    else{
      printf("Threads CPU affinity disabled.\n");
      printf("    Enable it with: 'simulation/thread-affinity true'\n");
    }
  }
  
  // Get time between dumps
  //*******************************

  double dumpTime;
  if(config.read("simulation/dump-interval",dumpTime) != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("\n\nInterval between dumps not specified. Will be set to 1e35 s.\n\n");
    }
    dumpTime = 1.0e35;
  }
  printf("Time between dumps (s): %12.4E\n",dumpTime);

  // Get recovery and write dump filename
  //***************************************
  std::string dump2read;  
  if(config.read("simulation/dump2read",dump2read) != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("\n\nNo recovery dump filename specified.\n\n");
    }
    dump2read.clear();
  }

  std::string dump2write;  
  if(config.read("simulation/dump2write",dump2write) != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("\n\nNo write dump filename specified. 'dump.dat' will be used.\n\n");
    }
    dump2write = "dump.dat";
  }

  // Check if the user only needs to read dump and write ASCII tally report
  bool dump2ascii;
  if(config.read("simulation/dump2ascii",dump2ascii) != INTDATA_SUCCESS){
    dump2ascii = false;
  }
  else if(dump2ascii && dump2read.length() == 0){
    if(verbose > 0){
      printf("\n\nError: Conversion from dump to ascii required but no dump file specified (field 'simulation/dump2read').\n\n");
    }
    return -2;
  }
  
  //*******************************
  // Obtain initial random seeds
  //*******************************

  const size_t nRand0Seeds = 1001;
  std::vector<int> seeds1(nthreads);
  std::vector<int> seeds2(nthreads);

  int seedPos0 = nseedPair; //First seed pair to use
  //Set required seeds equal to the number of threads
  int nReqSeeds = nthreads; 

  // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_

  //Each MPI process requires a couple of initial seeds
  //equal to the number of threads.

  nReqSeeds = nthreads*mpiSize;
  if(seedPos0 >= 0)
    seedPos0 = (seedPos0 + nthreads*rank) % nRand0Seeds;
  else
    seedPos0 = (nthreads*rank) % nRand0Seeds; //Avoid negative values of seedPos0
  
#endif
  // ***************************** MPI END ********************************** //

  //First, ensure that rand0 can provide sufficient
  //initial seeds for all MPI processes and threads.
  
  if(nReqSeeds > 1001){
    printf("Error: Unsuficient initial seeds for all processes and seeds (%d required).\n        Please, use less threads to use, as maximum, 1001 initial seeds.\n",nReqSeeds);
    return -3;
  }
  
  if(nthreads > 1){
    for(unsigned i = 0; i < nthreads; i++){
      int seedPos = i;
      if(seedPos0 >= 0)
	seedPos = (seedPos0+i) % nRand0Seeds;
      rand0(seedPos, seeds1[i], seeds2[i]);
    }
  }
  else{
    
    if(seedPos0 >= 0){
      rand0(seedPos0, seeds1[0], seeds2[0]);
    } else{    
      seeds1[0] = iseed1;
      seeds2[0] = iseed2;
    }
    
  }
  
  //****************************
  // Create particle generators
  //****************************
  std::vector<pen_specificStateGen<pen_particleState>> genericSources;
  std::vector<pen_specificStateGen<pen_state_gPol>> polarisedGammaSources;
  std::vector<double> genericSourceHists;
  std::vector<double> polarisedSourceHists;
      
  //Create and configure sources
  err = createParticleGenerators(genericSources,
				 polarisedGammaSources,
				 genericSourceHists,
				 polarisedSourceHists,
				 nthreads,config,verbose);

  if(err != 0){
    if(verbose > 0){
      if(err > 0){
	printf("\n");
	printf("Errors at sources creation and configuration.\n");
	printf("\n");
      }
    }
    return -3;
  }
  if(verbose > 0)
    printf("\n");
  
  //****************************
  // Context
  //****************************

  //Create elements data base
  pen_elementDataBase* elementsDB = new pen_elementDataBase;
  
  //Create simulation context
  pen_context context(*elementsDB);
  
  //Get global maximum energy
  double globEmax = -1.0;
  for(unsigned i = 0; i < genericSources.size(); i++){
    double localEmax = genericSources[i].maxEnergy();
    if(genericSources[i].kpar == PEN_POSITRON){
      //  ----  Positrons eventually give annihilation gamma rays. The maximum
      //        energy of annihilation photons is .lt. 1.21*(E0+me*c**2).
      localEmax = 1.21*(localEmax+5.12E5);
    }
    if(globEmax < localEmax)
      globEmax = localEmax;
  }
  for(unsigned i = 0; i < polarisedGammaSources.size(); i++){
    if(globEmax < polarisedGammaSources[i].maxEnergy())
      globEmax = polarisedGammaSources[i].maxEnergy();
  }

  if(verbose > 1){
    printf("Maximum global energy: %14.2E eV\n",globEmax);
  }

  //****************************
  // Materials
  //****************************

  printf("\n\n------------------------------------\n\n");
  printf(" **** Materials ****\n");
  printf(" *******************\n\n");
  
  std::string matFilenames[constants::MAXMAT];
  
  if(createMaterials(context,matFilenames,config,verbose) != 0)
    return -4;


  //Initialize context with specified materials
  FILE* fcontext = nullptr;
  fcontext = fopen("context.rep","w");
  if(fcontext == nullptr){
    printf("Error: unable to open file 'context.rep'\n");
  }
  if(context.init(globEmax,fcontext,verbose,matFilenames) != PEN_SUCCESS){
    fclose(fcontext);
    printf("Error at context initialization.\n");
    return -5;
  }
  fclose(fcontext);
  
  //****************************
  // Geometry parameters
  //****************************

  printf("\n\n------------------------------------\n\n");
  printf(" **** Geometry parameters\n\n");

  wrapper_geometry* geometry;
  
  //Create and configure geometry
  if(createGeometry(geometry,config,context,verbose) != 0)
    return -6;

  //Set geometry to context
  context.setGeometry(geometry);
  
  //Set source geometry
  for(unsigned i = 0; i < genericSources.size(); i++)
    genericSources[i].setGeometry(geometry);  
  for(unsigned i = 0; i < polarisedGammaSources.size(); i++)
    polarisedGammaSources[i].setGeometry(geometry);  

  //------------------------------------------------------------
  
  //Check if all required materials for the specified geometry have been created
  bool usedMaterials[constants::MAXMAT+1];
  geometry->usedMat(usedMaterials);

  unsigned missingMats = 0;
  for(unsigned i = 1; i <= constants::MAXMAT; i++){  //Avoid void material (0)
    if(matFilenames[i-1].length() == 0 && usedMaterials[i]){
      if(verbose > 0){
	printf("Error: Material %d is required by the geometry, but has not been configured.\n", i);
      }
      missingMats++;
    }
    else if(matFilenames[i-1].length() > 0 && !usedMaterials[i]){
      if(verbose > 0){
	printf("Warning: Material %d has been configured but is not used at specified geometry.\n",i);
      }
    }
  }

  if(missingMats > 0){
    printf("Error: Missing %d materials to use the specified geometry.\n",missingMats);
    return -6;
  }

  //Update absortion energies ussing geometry information
  err = context.updateEABS();
  if(err != 0){
    printf("Error calculating absorption energies\n");
    return -8;
  }
  
  //****************************
  // Create tallies
  //****************************
  std::vector<pen_commonTallyCluster> talliesVect;

  //Create one tally group per thread
  talliesVect.resize(nthreads);

  //Init tallies
  if(createTallies(talliesVect, nthreads, context, config, verbose) != 0)
    return -9;

  printf("\n%d tallies created for each thread.\n",talliesVect[0].numTallies());
  if(talliesVect[0].numTallies() == 0){
    printf("The simulation will not extract any information.\n");
    return 0;
  }

  // Check if we are restoring a simulation from dumpfile
  std::vector<double> lastHist(nthreads,0.0);
  if(dump2read.length() > 0){

    if(verbose > 1){
      printf("Load dump files '%s' for %d threads\n",dump2read.c_str(), nthreads);
    }
    //Read dump file for each thread
    for(unsigned i = 0; i < nthreads; i++){
      //Create filename

  // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_
  
      std::string filenameDump = std::string("MPI") + std::to_string(rank) +
	std::string("-th") + std::to_string(i) + dump2read;
#else
  // ***************************** MPI END ********************************** //
      std::string filenameDump = std::string("th") + std::to_string(i) + dump2read;
#endif      
      
      //Read dump file
      int errDump = talliesVect[i].readDumpfile(filenameDump.c_str(),
						lastHist[i],
						seeds1[i],seeds2[i],
						verbose);
      if(errDump != 0){
	if(verbose > 0){
	  printf("Error loading dumped data for thread %d: %d\n",i,errDump);
	}
	return -10;
      }

      if(dump2ascii){
	//Report data in dump file and finish execution
	printf(" *** Dump information of thread %u:\n\n",i);
	printf(" Last simulated history: %.0f\n",lastHist[i]);
	printf("             Last seeds: %9d %9d\n",seeds1[i],seeds2[i]);

	// Save data of this thread
	talliesVect[i].saveData(lastHist[0]); //Save data with previous flush call
      }      
    }

    if(dump2ascii){
      printf("Note: Partial data will be normalized to the number of histories simulated by the first thread (%.0lf)\n",lastHist[0]);
      //Finish execution
      return 0;
    }
    
  }

  
  
  //****************************
  // Variance Reduction 
  //****************************

  int vrRet = setVarianceReduction(context,*geometry,config,verbose);
  if(vrRet < 0){
    if(verbose > 0){
      printf("Error on variance reduction section.\n");
      printf("             Error code: %d\n",vrRet);
    }
    return -11;
  }
  if(vrRet > 0 && verbose > 1){
    printf("No variance reduction technics enabled.\n");
  }  
    
  //****************************
  // History loop
  //****************************
  
  // ************************** MULTI-THREADING ***************************** //
#ifdef _PEN_USE_THREADS_
  std::vector<std::thread> simThreads;
#endif
  // ************************ MULTI-THREADING END *************************** //

  std::vector<double> nsimulated(nthreads);
  pen_timer timer;
  double time0 = CPUtime();
  
  for(unsigned i = 0; i < nthreads; i++){
    talliesVect[i].run_beginSim();
  }

  double totalHists = 0.0;
  //Iterate over generic sources
  for(unsigned iSource = 0; iSource < genericSources.size(); ++iSource){
    
    //Calculate histories per thread
    double sourceHists = genericSourceHists[iSource];

    // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_
    //Recalculate number of histories to simulate
    sourceHists = ceil(sourceHists/double(mpiSize));

#endif
    // ***************************** MPI END ********************************** //

    double histsPerThread = ceil(sourceHists/double(nthreads));

    //Check if this source has been already simulated
    if(lastHist[0] > totalHists+sourceHists){
      printf("Source '%s' simulated before checkpoint",genericSources[iSource].name.c_str());
      totalHists += sourceHists;
      continue;
    }
    

  // ************************** MULTI-THREADING ***************************** //
#ifdef _PEN_USE_THREADS_
    //Run simulations for each thread
    if(nthreads > 1){
      // Multi-thread
      //****************
      for(unsigned ithread = 0; ithread < nthreads; ++ithread){
	// Initial history
	double initHist = histsPerThread*ithread + totalHists;
	// Calculate already simulated histories
	double offset = lastHist[ithread] - initHist;
	nsimulated[ithread] = (offset <= 0.0 ? 0.0 : offset);
	// Simulate
	simThreads.push_back(std::thread(simulate<pen_particleState>,
					 ithread,
					 histsPerThread,
					 initHist,
					 &context,
					 &genericSources[iSource],
					 &talliesVect[ithread],
					 &(seeds1[ithread]),&(seeds2[ithread]),
					 dumpTime,
					 dump2write,
					 &nsimulated[ithread]
					 ));


#ifdef _PEN_UNIX_
	//Set affinity for unix systems using pthreads
	if(CPUaffinity){
	  cpu_set_t cpuset;
	  CPU_ZERO(&cpuset);
	  CPU_SET(ithread,&cpuset);
	  int raff = pthread_setaffinity_np(simThreads.back().native_handle(),
					    sizeof(cpu_set_t), &cpuset);
	  if(raff != 0){
	    printf(" Unable to set affinity for thread %d\n",ithread);
	  }
	  else{
	    printf(" Affinity for thread %d set to CPU %d\n",ithread,ithread);
	  }
	}
#endif

      
      }
    
      //Join threads
      for(unsigned ithread = 0; ithread < nthreads; ++ithread){
	simThreads[ithread].join();
      }

      //Clear threads
      simThreads.clear();
    
    }
    else{

#endif
  // ************************ MULTI-THREADING END *************************** //

      // Single thread
      //****************
      
      //Calculate already simulated histories
      double offset = lastHist[0] - totalHists;
      nsimulated[0] = (offset <= 0.0 ? 0.0 : offset);
      // Simulate      
      simulate(0,
	       histsPerThread,
	       totalHists,
	       &context,
	       &genericSources[iSource],
	       &talliesVect[0],
	       &(seeds1[0]),&(seeds2[0]),
	       dumpTime,
	       dump2write,
	       &nsimulated[0]);      

#ifdef _PEN_USE_THREADS_
    }
#endif
    fflush(stdout);

    //Actualize total simulated hists
    for(unsigned ithread = 0; ithread < nthreads; ++ithread){
      totalHists += nsimulated[ithread];
    }
    
  }

  //Iterate over polarized sources
  for(unsigned iSource = 0; iSource < polarisedGammaSources.size(); iSource++){

    //Calculate histories per thread
    double sourceHists = polarisedSourceHists[iSource];

    // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_
    //Recalculate number of histories to simulate
    sourceHists = ceil(sourceHists/double(mpiSize));
#endif
    // ***************************** MPI END ********************************** //
    
    double histsPerThread = ceil(sourceHists/double(nthreads));

    //Check if this source has been already simulated
    if(lastHist[0] > totalHists+sourceHists){
      printf("Source '%s' simulated before checkpoint, skip.",polarisedGammaSources[iSource].name.c_str());
      totalHists += sourceHists;
      continue;
    }
    
    //Run simulations for each thread

  // ************************** MULTI-THREADING ***************************** //
#ifdef _PEN_USE_THREADS_
    if(nthreads > 1){
      // Multi-thread
      //***************
      for(unsigned ithread = 0; ithread < nthreads; ++ithread){
	// Initial history
	double initHist = histsPerThread*ithread + totalHists;
	// Calculate already simulated histories
	double offset = lastHist[ithread] - initHist;
	nsimulated[ithread] = (offset <= 0.0 ? 0.0 : offset);
	// Simulate
	simThreads.push_back(std::thread(simulate<pen_state_gPol>,
					 ithread,
					 histsPerThread,
					 initHist,
					 &context,
					 &polarisedGammaSources[iSource],
					 &talliesVect[ithread],
					 &(seeds1[ithread]),&(seeds2[ithread]),
					 dumpTime,
					 dump2write,
					 &nsimulated[ithread]
					 ));

#ifdef _PEN_UNIX_
	//Set affinity for unix systems using pthreads
	if(CPUaffinity){
	  cpu_set_t cpuset;
	  CPU_ZERO(&cpuset);
	  CPU_SET(ithread,&cpuset);
	  int raff = pthread_setaffinity_np(simThreads.back().native_handle(),
					    sizeof(cpu_set_t), &cpuset);
	  if(raff != 0){
	    printf(" Unable to set affinity for thread %d\n",ithread);
	  }
	  else{
	    printf(" Affinity for thread %d set to CPU %d\n",ithread,ithread);
	  }
	}
#endif
	
      }

      //Join threads
      for(unsigned ithread = 0; ithread < nthreads; ++ithread){
	simThreads[ithread].join();
      }

      //Clear threads
      simThreads.clear();
      
    }
    else{
#endif
  // ************************ MULTI-THREADING END *************************** //

      // Single thread
      //****************
      
      //Calculate already simulated histories
      double offset = lastHist[0] - totalHists;
      nsimulated[0] = (offset <= 0.0 ? 0.0 : offset);
      // Simulate 
      simulate<pen_state_gPol>(0,
			       histsPerThread,
			       totalHists,
			       &context,
			       &polarisedGammaSources[iSource],
			       &talliesVect[0],
			       &(seeds1[0]),&(seeds2[0]),
			       dumpTime,
			       dump2write,
			       &nsimulated[0]);
#ifdef _PEN_USE_THREADS_
    }
#endif

    //Actualize total simulated hists
    for(unsigned ithread = 0; ithread < nthreads; ithread++){
      totalHists += nsimulated[ithread];
    }
    
  }

  
  //End simulations
  for(unsigned ithread = 0; ithread < nthreads; ithread++){
    talliesVect[ithread].run_endSim(totalHists);
  }

  double simtime = timer.timer();
  double CPUendSim = CPUtime();
  double usertime = CPUendSim-time0;

  if(verbose > 1){
  // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_
    printf("Rank %u: Simulation finished, starting results reduce step\n",rank);
#else    
  // ***************************** MPI END ********************************** //
    printf("Simulation finished, starting results reduce step\n");
#endif
  }
  
  //Sum up tallies of all threads
  for(unsigned ithread = 1; ithread < nthreads; ithread++){
    talliesVect[0].sum(talliesVect[ithread],verbose);
  }

  // ******************************* MPI ************************************ //
#ifdef _PEN_USE_MPI_
  double localHists = totalHists;
  //Reduce the results stored at the first thread of all ranks
  talliesVect[0].reduceMPI(totalHists,MPI_COMM_WORLD,verbose);

  double postProcessTime = CPUtime() - CPUendSim;
  
  //Save results stored at the first thread of rank 0 process
  if(rank == 0){
    talliesVect[0].saveData(totalHists);    
  }

  //Print local report information

  printf("\n\n");
  printf("\n*********** Rank %03u *************\n",rank);
  printf("Simulated histories: %20.0f\n",localHists);
  printf("Simulation real time: %12.4E s\n",simtime);
  printf("Simulation user time: %12.4E s\n",usertime);
  printf("Histories per second and thread: %12.4E s\n",localHists/usertime);
  printf("Histories per second: %12.4E s\n",localHists/(usertime/double(nthreads)));
  printf("Results processing time: %12.4E s\n",postProcessTime);
  printf("\n*********** END REPORT *************\n");
  fflush(stdout);
  

  //Calculate total user time and used threads
  double globalUserTime = 0.0;
  MPI_Reduce(&usertime,&globalUserTime,1,MPI_DOUBLE,MPI_SUM,0,MPI_COMM_WORLD);
  unsigned totalThreads = 0;
  MPI_Reduce(&nthreads,&totalThreads,1,MPI_UNSIGNED,MPI_SUM,0,MPI_COMM_WORLD);

  double globalSimTime = 0.0;
  //Obtain maximum simulation time
  MPI_Reduce(&simtime,&globalSimTime,1,MPI_DOUBLE,MPI_MAX,0,MPI_COMM_WORLD);

  //Close stdout
  fclose(stdout);

  //Print joined reports on stderr to print it on global log
  //Print ordered local report information
  for(int ip = 0; ip < mpiSize; ++ip){

    if(rank == ip){
      fprintf( stderr,"\n\n");
      fprintf( stderr,"\n*********** Rank %03u *************\n",rank);
      fprintf( stderr,"Simulated histories: %20.0f\n",localHists);
      fprintf( stderr,"Simulation real time: %12.4E s\n",simtime);
      fprintf( stderr,"Simulation user time: %12.4E s\n",usertime);
      fprintf( stderr,"Histories per second and thread: %12.4E s\n",localHists/usertime);
      fprintf( stderr,"Histories per second: %12.4E s\n",localHists/(usertime/double(nthreads)));
      fprintf( stderr,"Results processing time: %12.4E s\n",postProcessTime);
      fprintf( stderr,"\n*********** END REPORT *************\n");
      fflush(stderr);
      std::this_thread::sleep_for (std::chrono::seconds(1));
    }
    MPI_Barrier(MPI_COMM_WORLD); //Sync all MPI processes
  }

  //Print global report
  if(rank == 0){
    fprintf( stderr,"\n\n");
    fprintf( stderr,"\n*********** Global Report *************\n");
    fprintf( stderr,"Simulated histories: %20.0f\n",totalHists);
    fprintf( stderr,"Simulation real time: %12.4E s\n",globalSimTime);
    fprintf( stderr,"Simulation user time: %12.4E s\n",globalUserTime);
    fprintf( stderr,"Histories per second and thread: %12.4E s\n",totalHists/globalUserTime);
    fprintf( stderr,"Histories per second: %12.4E s\n",totalHists/(globalUserTime/double(totalThreads)));
    fprintf( stderr,"Results processing time: %12.4E s\n",postProcessTime);  
  }

  // Finalize the MPI environment.
  MPI_Finalize();
  
#else
  // ***************************** MPI END ********************************** //

  double postProcessTime = CPUtime() - CPUendSim;
  
  //Save results stored at first thread
  talliesVect[0].saveData(totalHists);

  printf("\n\nSimulated histories: %20.0f\n",totalHists);
  printf("Simulation real time: %12.4E s\n",simtime);
  printf("Simulation user time: %12.4E s\n",usertime);
  printf("Histories per second and thread: %12.4E s\n",totalHists/usertime);
  printf("Histories per second: %12.4E s\n",totalHists/(usertime/double(nthreads)));
  printf("Results processing time: %12.4E s\n",postProcessTime);
  
#endif

  //Free memory
  delete geometry;
  geometry = nullptr;

  delete elementsDB;
  elementsDB = nullptr;
  
  return 0;
}

int createParticleGenerators(std::vector<pen_specificStateGen<pen_particleState>>& genericSources,
			     std::vector<pen_specificStateGen<pen_state_gPol>>& polarisedGammaSources,
			     std::vector<double>& genericSourceHists,
			     std::vector<double>& polarisedSourceHists,
			     const unsigned nthreads,
			     const pen_parserSection& config,
			     const unsigned verbose){

  int errG = 0;
  int errP = 0;
  int err = 0;
  
  //Extract source sections
  pen_parserSection genericSourceSection;
  pen_parserSection polarisedSourceSection;
  errG = config.readSubsection("sources/generic",genericSourceSection);
  errP = config.readSubsection("sources/polarized",polarisedSourceSection);
  if(errG != INTDATA_SUCCESS && errP != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("createParticleGenerators: Error: Fields 'sources/generic' nor 'sources/polarized' don't exists. No source specified\n");
    }
    return -2;
  }

  //Extract source names
  std::vector<std::string> genericSourceNames;
  std::vector<std::string> polarisezSourceNames;
  if(errG == INTDATA_SUCCESS)
    genericSourceSection.ls(genericSourceNames);
  if(errP == INTDATA_SUCCESS)
    polarisedSourceSection.ls(polarisezSourceNames);

  unsigned nGenericSources = genericSourceNames.size();
  unsigned nPolarizedSources = polarisezSourceNames.size();  

  if(nGenericSources < 1 && nPolarizedSources < 1){
    if(verbose > 0)
      printf("createParticleGenerators: Error: Simulation requires, at last, one particle source.\n");
    return -3;    
  }

  //Resize source vectors to store all defined sourceSection
  genericSources.clear();
  genericSourceHists.clear();
  if(nGenericSources > 0){
    genericSources.resize(nGenericSources);
    genericSourceHists.resize(nGenericSources,0.0);
  }
  
  polarisedGammaSources.clear();
  polarisedSourceHists.clear();
  if(nPolarizedSources > 0){
    polarisedGammaSources.resize(nPolarizedSources);
    polarisedSourceHists.resize(nPolarizedSources,0.0);
  }

  //Iterate over all generic sources
  for(unsigned i = 0; i < nGenericSources; i++){
    //Get source section
    pen_parserSection genSection;
    if(genericSourceSection.readSubsection(genericSourceNames[i],genSection) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createParticleGenerators: Error: Unable to extract section for generic source '%s'\n",genericSourceNames[i].c_str());
      }
      err++;
      continue;
    }
    else{
      //Set source name
      genericSources[i].name.assign(genericSourceNames[i]);

      //Load history number
      if(genSection.read("nhist",genericSourceHists[i]) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("createParticleGenerators: Error: Unable to read field 'nhist' for generic source '%s'\n",genericSourceNames[i].c_str());
	}
	err++;
	continue;
      }

      if(genericSourceHists[i] <= 0.5){
	if(verbose > 0){
	  printf("createParticleGenerators: Error on generic source %s. Number of histories must be greater than zero\n",polarisezSourceNames[i].c_str());
	}
	err++;
	continue;	
      }
      
      //Load source
      if(configureSource(genericSources[i],
			 nthreads,
			 genSection,verbose) != 0){
	if(verbose > 0)
	  printf("createParticleGenerators: Error: Can't create and configure source '%s'.\n",genericSourceNames[i].c_str());	
	err++;
      }
    }
  }

  //Iterate over all specific sources
  for(unsigned i = 0; i < nPolarizedSources; i++){
    //Get source section
    pen_parserSection genSection;
    if(polarisedSourceSection.readSubsection(polarisezSourceNames[i],genSection) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createParticleGenerators: Error: Unable to extract section for polarized gamma source '%s'\n",polarisezSourceNames[i].c_str());
      }
      err++;
      continue;
    }
    else{
      //Set source name
      polarisedGammaSources[i].name.assign(polarisezSourceNames[i]);

      //Load history number
      if(genSection.read("nhist",polarisedSourceHists[i]) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("createParticleGenerators: Error: Unable to read field 'nhist' for polarized gamma source '%s'\n",polarisezSourceNames[i].c_str());
	}
	err++;
	continue;
      }

      if(polarisedSourceHists[i] <= 0.5){
	if(verbose > 0){
	  printf("createParticleGenerators: Error on polarized gamma source %s. Number of histories must be greater than zero\n",polarisezSourceNames[i].c_str());
	}
	err++;
	continue;	
      }
      
      //Load source
      if(configureSource(polarisedGammaSources[i],
			 nthreads,
			 genSection,verbose) != 0){
	if(verbose > 0)
	  printf("createParticleGenerators: Error: Can't create and configure source '%s'.\n",polarisezSourceNames[i].c_str());	
	err++;
      }
    }
  }
      
  return err;
}

int createTallies(std::vector<pen_commonTallyCluster>& tallyGroups,
		  const size_t nthreads,
		  const pen_context& context,
		  const pen_parserSection& config,
		  const unsigned verbose){

  //Extract source section
  pen_parserSection talliesSection;
  int err = config.readSubsection("tallies",talliesSection);
  if(err != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("createTallies: Error: Configuration 'tallies' section doesn't exists.\n");
    }
    return -1;
  }

  if(nthreads < 1){
    printf("createTallies: Error: Simulation requires, at last, one thread for execution.");
    return -2;
  }

  // ************************** MULTI-THREADING ***************************** //
#ifdef _PEN_USE_THREADS_
  //Create a vector of threads
  std::vector<std::thread> threads;
#endif
  // ************************** MULTI-THREADING ***************************** //

  //Get materials array
  const abc_material* mats[constants::MAXMAT];
  context.getMatBaseArray(mats);
  
  // ************************** MULTI-THREADING ***************************** //
#ifdef _PEN_USE_THREADS_
  //Configure non zero threads with no verbose
  for(unsigned j = 1; j < nthreads; j++){
    tallyGroups[j].name.assign("common");
    //Configure tallies
    threads.push_back(tallyGroups[j].configure_async(context.readGeometry(),
						     mats,
						     j,
						     talliesSection,
						     std::min(verbose,unsigned(1))));
  }
#endif
  // ************************** MULTI-THREADING ***************************** //
  
  //Configure main thread with verbose option
  tallyGroups[0].name.assign("common");    
  //Configure tallies
  tallyGroups[0].configure(context.readGeometry(),
			   mats,
			   0,
			   talliesSection,
			   verbose);
  
  // ************************** MULTI-THREADING ***************************** //
#ifdef _PEN_USE_THREADS_
  //Sincronize all threads
  for(unsigned j = 0; j < nthreads-1; j++){
    threads[j].join();
  }
#endif
  // ************************** MULTI-THREADING ***************************** //
  
  //Check errors
  unsigned failedClusters = 0;
  for(unsigned j = 0; j < nthreads; j++){
    err = tallyGroups[j].configureStatus();
    if(err != 0){
      if(verbose > 0)
	printf("createTallies: Error on tally cluster %u "
	       "creation and configuration (err code %d).\n",j,err);
      failedClusters++;
    }
  }

  if(failedClusters > 0)
    return -3;
  
  return 0;
}

int createGeometry(wrapper_geometry*& geometry,
		   const pen_parserSection& config,
		   const pen_context& context,
		   const unsigned verbose){
  
  //Get geometry section
  pen_parserSection geometrySection;
  int err = config.readSubsection("geometry",geometrySection);
  if(err != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("createGeometry: Error: Configuration 'geometry' section doesn't exist.\n");
    }
    return -2;
  }

  //Append material information to geometry section
  
  for(unsigned imat = 0; imat < context.getNMats(); imat++){
    char key[400];
    sprintf(key,"materials/mat%03d/ID",imat+1);
    geometrySection.set(key,(int)imat+1);
    sprintf(key,"materials/mat%03d/density",imat+1);
    geometrySection.set(key,context.readBaseMaterial(imat).readDens());
  }
  
  //Get geometry type
  std::string geoType;
  if(geometrySection.read("type",geoType) != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("createGeometry: Error: field 'geometry/type' not specified. String expected.\n");
    }
    return -3;
  }

  //Create geometry
  geometry = penGeoRegister_create(geoType.c_str());
  if(geometry == nullptr){
    if(verbose > 0){
      printf("createGeometry: Error creating geometry instance of type '%s'\n", geoType.c_str());
    }
    return -4;
  }

  //Configure geometry  
  geometry->name.assign("geometry");    
  geometry->configure(geometrySection,verbose);    
  
  //Check errors
  if(geometry->configureStatus() != 0){
    if(verbose > 0)
      printf("createGeometry: Error: Fail on geometry configuration.\n");
    return -5;
  }

  return 0;
}

int createMaterials(pen_context& context,
		    std::string filenames[constants::MAXMAT],
		    const pen_parserSection& config,
		    const unsigned verbose){

  //Extract materials section
  pen_parserSection matSection;
  if(config.readSubsection("materials",matSection) != INTDATA_SUCCESS){
    if(verbose > 0){
      printf("createMaterials: Error: Configuration 'materials' section doesn't exist.\n");
    }
    return -1;
  }

  //Get "materials" subsections (each one correspond to one material)
  std::vector<std::string> matNames;
  matSection.ls(matNames);

  if(verbose > 1){
    printf("Number of materials: %u\n", unsigned(matNames.size()));
    
    for(unsigned i = 0; i < matNames.size(); i++)
      printf("\t%s\n",matNames[i].c_str());
  }
  
  if(matNames.size() == 0){
    if(verbose > 0){
      printf("Error: No material found at configuration. Simulation requires, at last, one material.\n");
    }
    return -2;
  }
  
  if(matNames.size() > constants::MAXMAT){
    if(verbose > 0){
      printf("Error: %d materials found. The maximum number of materials is %d.\n",int(matNames.size()),constants::MAXMAT);
    }
    return -2;
  }
  
  //Set materials to context
  int errmat = context.setMats<pen_material>(matNames.size());
  if(errmat != 0)
    {
      printf("Error creating materials: %d.\n",errmat);
      return -7;
    }
  
  //Iterate over each material
  int err = 0;
  for(unsigned i = 0; i < matNames.size(); i++){

    printf("\n\n------------------------------------\n\n");
    printf(" **** Material '%s'\n\n",matNames[i].c_str());

    //Extract material subsection
    pen_parserSection oneMatSec;
    if(matSection.readSubsection(matNames[i],oneMatSec) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read mat section 'materials/%s'.\n",matNames[i].c_str());
      }
      err++;
      continue;
    }

    //Get material number
    int index;
    //Get absortion energies
    if(oneMatSec.read("number",index) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/number'. Integer expected.\n",matNames[i].c_str());
      }
      err++;
      continue;
    }

    if(index < 1 || index > int(constants::MAXMAT)){
      if(verbose > 0){
	printf("createMaterials: Error: Material index ('materials/%s/number') out of range (1-%d).\n",matNames[i].c_str(),constants::MAXMAT);
      }
      err++;
      continue;
    }

    int j = index-1;
    //Get material
    pen_material& mat = context.getBaseMaterial(j);
        
    //Get absortion energies
    //#######################
    if(oneMatSec.read("eabs_e-",mat.EABS[PEN_ELECTRON]) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/eabs_e-'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }
    if(oneMatSec.read("eabs_e+",mat.EABS[PEN_POSITRON]) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/eabs_e+'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }
    if(oneMatSec.read("eabs_gamma",mat.EABS[PEN_PHOTON]) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/eabs_gamma'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }
    
    //Get C1 and C2
    //##################
    if(oneMatSec.read("C1",mat.C1) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/C1'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }
    if(oneMatSec.read("C2",mat.C2) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/C2'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }

    //Read WCC and WCR
    //##################
    if(oneMatSec.read("WCC",mat.WCC) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/WCC'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }
    if(oneMatSec.read("WCR",mat.WCR) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/WCR'. Double expected.\n",matNames[j].c_str());
      }
      err++;
    }

    //Read material filename
    //#######################
    if(oneMatSec.read("filename",filenames[j]) != INTDATA_SUCCESS){
      if(verbose > 0){
	printf("createMaterials: Error: Unable to read 'materials/%s/filename'. String expected.\n",matNames[j].c_str());
      }
      err++;
    }
    
    if(verbose > 0){
      printf("      C1 =%11.4E       C2 =%11.4E\n",mat.C1,mat.C2);
      printf("     WCC =%11.4E eV,   WCR =%11.4E eV\n",mat.WCC,(mat.WCR > 10.0E0 ? mat.WCR : 10.0E0));
      printf("  electron EABS: %11.4E eV\n",mat.EABS[PEN_ELECTRON]);
      printf("     gamma EABS: %11.4E eV\n",mat.EABS[PEN_PHOTON]);
      printf("  positron EABS: %11.4E eV\n",mat.EABS[PEN_POSITRON]);

      printf("\n Material filename: '%s'.\n",filenames[j].c_str());
      printf("\n Material number: %d.\n",index);      
    }
    
    printf("\n\n------------------------------------\n\n");
  }

  return err;
}

int setVarianceReduction(pen_context& context,
			 const wrapper_geometry& geometry,
			 const pen_parserSection& config,
			 const unsigned verbose){

  //Extract variance reduction section
  pen_parserSection VRSection;
  if(config.readSubsection("VR",VRSection) != INTDATA_SUCCESS){
    if(verbose > 1){
      printf("No variance reduction specified.\n");
    }
    return 1;
  }

  int err;

  if(verbose > 1)printf("\n");
  
  //Get materials used by the current geometry
  bool usedMat[constants::MAXMAT+1];
  geometry.usedMat(usedMat);
  
  //*********************
  // Interaction Forcing
  //*********************
  
  //Try to Read interaction forcing parameters for materials and bodies.
  //Notice that body configuration will overwrite material one.

  // Materials
  //************
  
  std::vector<std::string> IFmats;
  VRSection.ls("IForcing/materials",IFmats);

  if(IFmats.size() > 0){
    if(verbose > 1){
      printf("\n\n **** Material interaction forcing:\n\n");
      printf("  Mat |    Particle    | Interaction |  IF factor  | Weight Range\n");      
    }
    
    for(unsigned imname = 0; imname < IFmats.size(); imname++){

      //Get ibody name section
      pen_parserSection matSection;
      std::string matSecKey = std::string("IForcing/materials/") + IFmats[imname];
      if(VRSection.readSubsection(matSecKey,matSection) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: 'VR/%s' is not a section, skip this material.\n",matSecKey.c_str());
	}
	continue;
      }

      // Material index
      //***********************

      //Read index
      int imat;
      err = matSection.read("mat-index",imat);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Material index not specified for material section '%s'. Integer expected\n",IFmats[imname].c_str());
	}
      }

      //Check if material index is in range
      if(imat < 1 || imat > (int)constants::MAXMAT){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Specified index (%d) for material section '%s' out of range.\n",imat,IFmats[imname].c_str());
	}
	return -1;
      }

      //Check if material is used at current geometry
      if(!usedMat[imat]){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Specified index (%d) for material section '%s' is not used at current geometry.\n",imat,IFmats[imname].c_str());
	}
	return -2;
      }

      // IF Parameters
      //****************

      std::string particle;
      unsigned kpar;
      int icol;
      double weightL,weightU,forcer;
      
      //Read particle name
      err = matSection.read("particle",particle);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Missing field 'particle' for interaction forcing on material section '%s'. String expected.\n",IFmats[imname].c_str());
	return -3;
      }
      //Get kpar
      kpar = particleID(particle.c_str());
      if(kpar == ALWAYS_AT_END){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Unknown particle type specified on interaction forcing for material section '%s': '%s'\n",IFmats[imname].c_str(),particle.c_str());
	}
	return -4;
      }

      //Get interaction index
      err = matSection.read("interaction",icol);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'interaction' for interaction forcing on material section '%s'. Integer expected.\n",IFmats[imname].c_str());
	return -5;
      }

      //Check interaction
      if(icol < 0 || icol >= (int)constants::MAXINTERACTIONS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Interaction index (%d) out of range at interaction forcing on material section '%s'.\n",icol,IFmats[imname].c_str());
	return -6;
      }

      //Get forcing factor
      err = matSection.read("factor",forcer);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'factor' for interaction forcing on material section '%s', interaction %d. Real number expected.\n",IFmats[imname].c_str(),icol);
	return -7;
      }

      //Check forcing factor
      if(forcer < 1.0){
	if(verbose > 0){
	  printf("Interaction forcing factor for material '%s', interaction %d lesser than minimum (1.0), will be set to 1.0\n",IFmats[imname].c_str(),icol);
	  forcer = 1.0;
	}
      }

      //Read weight range
      err = matSection.read("min-weight",weightL);
      if(err != INTDATA_SUCCESS){
	weightL = 0.0;
	if(verbose > 2){
	  printf("field 'min-weight' no specified, real number expected.\n");
	  printf(" minimum weight will be set to 0.0\n");
	}
      }
      err = matSection.read("max-weight",weightU);
      if(err != INTDATA_SUCCESS){
	weightU = 1.0e6;
	if(verbose > 2){
	  printf("field 'max-weight' no specified, real number expected.\n");
	  printf(" maximum weight will be set to 1.0e6\n");
	}
      }

      if(weightL < 0.0 || weightU <= weightL){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Invalid weight range for interaction forcing on material '%s', interaction %d:\n",IFmats[imname].c_str(),icol);
	  printf("               minimum weight: %12.4E\n",weightL);
	  printf("               maximum weight: %12.4E\n",weightU);
	}
	return -8;	
      }

      //Print information
      if(verbose > 1){
	printf(" %4u  %15.15s   %11d   %11.4E   %12.4E - %12.4E\n",imat,particle.c_str(),icol,forcer,weightL,weightU);
      }

      //Set parameters for bodies with this material index
      for(unsigned ibody = 0; ibody < geometry.getBodies(); ibody++){
	
	if(geometry.getMat(ibody) != (unsigned)imat) continue;

	
	if(ibody >= context.NBV){
	  if(verbose > 0){
	    printf("setVarianceReduction: Error: Maximum body index for IF reached (%u)\n",context.NBV);
	  }
	  return -8;
	}
	
	//Store information
	context.FORCE[ibody][kpar][icol] = forcer;

	if(context.WRANGES[ibody][2*kpar] < weightL)
	  context.WRANGES[ibody][2*kpar] = weightL;

	if(context.WRANGES[ibody][2*kpar+1] > weightU)
	  context.WRANGES[ibody][2*kpar+1] = weightU;

	context.LFORCE[ibody][kpar] = true;
	
      }
    }
    
  }
  else if(verbose > 1){
    printf("No materials specified to use interaction forcing.\n");
  }
  

  // Bodies
  //************
  
  std::vector<std::string> IFbodies;
  VRSection.ls("IForcing/bodies",IFbodies);

  if(IFbodies.size() > 0){
    if(verbose > 1){
      printf("\n\n **** Body interaction forcing:\n\n");
      printf(" Body |    Particle    | Interaction |  IF factor  | Weight Range\n");      
    }

    for(unsigned ibname = 0; ibname < IFbodies.size(); ibname++){

      //Get ibody name section
      pen_parserSection bodySection;
      std::string bodySecKey = std::string("IForcing/bodies/") + IFbodies[ibname];
      if(VRSection.readSubsection(bodySecKey,bodySection) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: 'VR/%s' is not a section, skip this body.\n",bodySecKey.c_str());
	}
	continue;
      }

      //Read body alias
      std::string bodyAlias;
      unsigned ibody;
      err = bodySection.read("body",bodyAlias);
      if(err == INTDATA_SUCCESS){
	//Check if specified body exists ang get the corresponding index
	ibody = geometry.getIBody(bodyAlias.c_str());
	if(ibody >= geometry.getBodies()){
	  if(verbose > 0){
	    printf("setVarianceReduction: Body '%s' doesn't exists in loaded geometry.\n",bodyAlias.c_str());
	  }
	  return -9;
	}
      }
      else{
	//Try to read body as integer index
	int auxibody;
	err = bodySection.read("body",auxibody);
	if(err != INTDATA_SUCCESS){
	  if(verbose > 0){
	    printf("setVarianceReduction: Error: Unable to read 'VR/%s/body'. Integer or string expected.\n",bodySecKey.c_str());
	  }
	  return -9;
	}
	if(auxibody < 0 || auxibody >= (int)context.NBV){
	  if(verbose > 0)
	    printf("setVarianceReduction: Error: Specified body index (%d) out of range.\n",auxibody);
	  return -9;
	}
	ibody = (unsigned)auxibody;
      }

      if(ibody >= context.NBV){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Maximum body index for IF is (%u)\n",context.NBV);
	  printf("                  specified index: %u\n",ibody);
	}
	return -9;	  
      }
      
      //Read kpar, icol and weight limits
      std::string particle;
      unsigned kpar;
      int icol;
      double weightL,weightU,forcer;
      
      //Read particle name
      err = bodySection.read("particle",particle);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Missing field 'particle' for interaction forcing on body section '%s'. String expected.\n",IFbodies[ibname].c_str());
	return -10;
      }
      //Get kpar
      kpar = particleID(particle.c_str());
      if(kpar == ALWAYS_AT_END){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Unknown particle type specified on interaction forcing for body section '%s': '%s'\n",IFbodies[ibname].c_str(),particle.c_str());
	}
	return -11;
      }

      //Get interaction index
      err = bodySection.read("interaction",icol);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'interaction' for interaction forcing on body section '%s'. Integer expected.\n",IFbodies[ibname].c_str());
	return -12;
      }

      //Check interaction
      if(icol < 0 || icol >= (int)constants::MAXINTERACTIONS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Interaction index (%d) out of range at interaction forcing on body section '%s'. Integer expected.\n",icol,IFbodies[ibname].c_str());
	return -13;
      }

      //Get forcing factor
      err = bodySection.read("factor",forcer);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'factor' for interaction forcing on body section '%s', interaction %d. Real number expected.\n",IFbodies[ibname].c_str(),icol);
	return -14;
      }

      //Check forcing factor
      if(forcer < 1.0){
	if(verbose > 0){
	  printf("Interaction forcing factor for body '%s', interaction %d lesser than minimum (1.0), will be set to 1.0\n",IFbodies[ibname].c_str(),icol);
	  forcer = 1.0;
	}
      }

      //Read weight range
      err = bodySection.read("min-weight",weightL);
      if(err != INTDATA_SUCCESS){
	weightL = 0.0;
	if(verbose > 2){
	  printf("field 'min-weight' no specified, real number expected.\n");
	  printf(" minimum weight will be set to 0.0\n");
	}
      }
      err = bodySection.read("max-weight",weightU);
      if(err != INTDATA_SUCCESS){
	weightU = 1.0;
	if(verbose > 2){
	  printf("field 'max-weight' no specified, real number expected.\n");
	  printf(" maximum weight will be set to 1.0\n");
	}
      }

      if(weightL < 0.0 || weightU <= weightL){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Invalid weight range for interaction forcing on body section '%s', interaction %d:\n",IFbodies[ibname].c_str(),icol);
	  printf("               minimum weight: %12.4E\n",weightL);
	  printf("               maximum weight: %12.4E\n",weightU);
	}
	return -15;	
      }

      //Print information
      if(verbose > 1){
	printf(" %4u  %15.15s   %11d   %11.4E   %12.4E - %12.4E\n",ibody,particle.c_str(),icol,forcer,weightL,weightU);
      }

      //Store information
      context.FORCE[ibody][kpar][icol] = forcer;

      if(context.WRANGES[ibody][2*kpar] < weightL)
	context.WRANGES[ibody][2*kpar] = weightL;

      if(context.WRANGES[ibody][2*kpar+1] > weightU)
	context.WRANGES[ibody][2*kpar+1] = weightU;

      context.LFORCE[ibody][kpar] = true;	
    }    
  }
  else if(verbose > 1){
    printf("No bodies specified to use interaction forcing.\n");
  }

  //Print final interaction forcing information
  if(verbose > 1){
    printf("\n\nEnabled Interaction forcing:\n\n");
    printf(" Body |    Particle    | Interaction |  IF factor  | Weight Range\n");
    for(unsigned i = 0; i < context.NBV; i++){
      for(unsigned j = 0; j < constants::nParTypes; j++){
	if(!context.LFORCE[i][j]){continue;}
	for(unsigned k = 0; k < constants::MAXINTERACTIONS; k++){
	  if(context.FORCE[i][j][k] > 1.0){
	    printf(" %4u  %15.15s   %11d   %11.4E   %12.4E - %12.4E\n",
		   i,particleName(j),k,context.FORCE[i][j][k],
		   context.WRANGES[i][2*j],context.WRANGES[i][2*j+1]);
	  }
	}
      }
    }
    printf("\n\n");
  }


  //**************************
  // Bremsstrahlung splitting
  //**************************

  // Materials
  //************

  std::vector<std::string> bremssMat;
  VRSection.ls("bremss/materials",bremssMat);

  if(bremssMat.size() > 0){
    if(verbose > 1){
      printf("\n\n **** Material bremsstrahlung splitting:\n\n");
      printf("  Mat  | Bremss splitting\n");      
    }
    
    for(unsigned imname = 0; imname < bremssMat.size(); imname++){

      //Get ibody name section
      pen_parserSection matSection;
      std::string matSecKey = std::string("bremss/materials/") + bremssMat[imname];
      if(VRSection.readSubsection(matSecKey,matSection) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: 'VR/%s' is not a section, skip this material.\n",matSecKey.c_str());
	}
	continue;
      }

      // Material index
      //***********************

      //Read index
      int imat;
      err = matSection.read("mat-index",imat);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Material index not specified for material '%s'. Integer expected\n",bremssMat[imname].c_str());
	}
      }

      //Check if material index is in range
      if(imat < 1 || imat > (int)constants::MAXMAT){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Specified index (%d) for material '%s' out of range.\n",imat,bremssMat[imname].c_str());
	}
	return -16;
      }

      //Check if material is used at current geometry
      if(!usedMat[imat]){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Specified index (%d) for material '%s' is not used at current geometry.\n",imat,bremssMat[imname].c_str());
	}
	return -17;
      }

      //Get splitting factor
      int splitting;
      err = matSection.read("splitting",splitting);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'splitting' for bremsstrahlung splitting on material '%s'. Integer expected.\n",bremssMat[imname].c_str());
	return -18;
      }

      //Check splitting factor
      if(splitting < 1){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Invalid bremsstrahlung splitting factor (%d).\n",splitting);
	return -19;
      }

      //Set splitting factor for bodies with this material index
      for(unsigned ibody = 0; ibody < geometry.getBodies(); ibody++){
	
	if(geometry.getMat(ibody) != (unsigned)imat) continue;

	if(ibody >= context.NBV){
	  if(verbose > 0){
	    printf("setVarianceReduction: Error: Maximum body index for IF reached (%u)\n",context.NBV);
	  }
	  return -19;
	}
	
	if(context.LFORCE[ibody][PEN_ELECTRON] || context.LFORCE[ibody][PEN_POSITRON]){
	  context.IBRSPL[ibody] = (unsigned)splitting;	  
	}
      }

      //Print configuration
      if(verbose > 1){
	printf(" %5d   %5d\n", imat,splitting);
      }
      
    }
  }
  else if(verbose > 1){
    printf("No material with bremsstrahlung splitting enabled.\n");
  }
  
  // Bodies
  //************

  std::vector<std::string> bremssBodies;
  VRSection.ls("bremss/bodies",bremssBodies);

  if(bremssBodies.size() > 0){
    if(verbose > 1){
      printf("\n\n **** Body bremsstrahlung splitting:\n\n");
      printf(" Body  | Bremss splitting\n");      
    }
    
    for(unsigned ibname = 0; ibname < bremssBodies.size(); ibname++){

      //Get ibody name section
      pen_parserSection bodySection;
      std::string bodySecKey = std::string("bremss/bodies/") + bremssBodies[ibname];
      if(VRSection.readSubsection(bodySecKey,bodySection) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: 'VR/%s' is not a section, skip this body.\n",bodySecKey.c_str());
	}
	continue;
      }

      // Body index
      //***********************

      //Check if specified body exists
      unsigned ibody = geometry.getIBody(bremssBodies[ibname].c_str());
      if(ibody >= geometry.getBodies()){
	if(verbose > 0){
	  printf("setVarianceReduction: Body '%s' doesn't exists in loaded geometry.\n",bremssBodies[ibname].c_str());
	}
	return -20;
      }
      else if(ibody >= context.NBV){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Maximum body index for IF is (%u)\n",context.NBV);
	  printf("                  specified index: %u\n",ibody);
	}
	return -20;	  
      }

      //Check if in this body IF has been enabled
      if(!context.LFORCE[ibody][PEN_ELECTRON] && !context.LFORCE[ibody][PEN_POSITRON]){
	if(verbose > 0){
	  printf("setVarianceReduction: Body '%s' (index %u) has not enabled interaction forcing.\n",bremssBodies[ibname].c_str(),ibody);
	}
	return -21;
      }

      // Splitting factor
      //***********************

      //Get splitting factor
      int splitting;
      err = bodySection.read("splitting",splitting);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'splitting' for bremsstrahlung splitting on body '%s'. Integer expected.\n",bremssBodies[ibname].c_str());
	return -22;
      }

      //Check splitting factor
      if(splitting < 1){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Invalid bremsstrahlung splitting factor (%d).\n",splitting);
	return -23;
      }

      //Set splitting factor for specified body
      context.IBRSPL[ibody] = (unsigned)splitting;	  

      //Print configuration
      if(verbose > 1){
	printf(" %5d   %5d\n", ibody,splitting);
      }
      
    }
  }
  else if(verbose > 1){
    printf("No bodies with bremsstrahlung splitting enabled.\n");
  }

  if(verbose > 1){
    printf("\n\nFinal bremsstrahlung splitting:\n\n");
    printf(" Body  | Bremss splitting\n");      
    for(unsigned ibody = 0; ibody < context.NBV; ibody++){
      
      if(context.IBRSPL[ibody] > 1)
	printf(" %5u   %5u\n", ibody,context.IBRSPL[ibody]);
    }
    printf("\n\n");
  }

  //**************************
  // X-ray splitting
  //**************************

  // Materials
  //************

  std::vector<std::string> xRayMat;
  VRSection.ls("x-ray/materials",xRayMat);

  if(xRayMat.size() > 0){
    if(verbose > 1){
      printf("\n\n **** Material x-ray splitting:\n\n");
      printf("  Mat  | x-ray splitting\n");      
    }
    
    for(unsigned imname = 0; imname < xRayMat.size(); imname++){

      //Get ibody name section
      pen_parserSection matSection;
      std::string matSecKey = std::string("x-ray/materials/") + xRayMat[imname];
      if(VRSection.readSubsection(matSecKey,matSection) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: 'VR/%s' is not a section, skip this material.\n",matSecKey.c_str());
	}
	continue;
      }

      // Material index
      //***********************

      //Read index
      int imat;
      err = matSection.read("mat-index",imat);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Material index not specified for material '%s'. Integer expected\n",xRayMat[imname].c_str());
	}
      }

      //Check if material index is in range
      if(imat < 1 || imat > (int)constants::MAXMAT){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Specified index (%d) for material '%s' out of range.\n",imat,xRayMat[imname].c_str());
	}
	return -24;
      }

      //Check if material is used at current geometry
      if(!usedMat[imat]){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Specified index (%d) for material '%s' is not used at current geometry.\n",imat,xRayMat[imname].c_str());
	}
	return -25;
      }

      //Get splitting factor
      int splitting;
      err = matSection.read("splitting",splitting);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'splitting' for x-ray splitting on material '%s'. Integer expected.\n",xRayMat[imname].c_str());
	return -26;
      }

      //Check splitting factor
      if(splitting < 1){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Invalid x-ray splitting factor (%d).\n",splitting);
	return -27;
      }

      //Set splitting factor for bodies with this material index
      for(unsigned ibody = 0; ibody < geometry.getBodies(); ibody++){
      
	if(geometry.getMat(ibody) != (unsigned)imat) continue;

	if(ibody >= context.NBV){
	  if(verbose > 0){
	    printf("setVarianceReduction: Error: Maximum body index for IF reached (%u)\n",context.NBV);
	  }
	  return -27;
	}
	
	context.IXRSPL[ibody] = (unsigned)splitting;
	context.LXRSPL[ibody] = true;
      }

      //Print configuration
      if(verbose > 1){
	printf(" %5d   %5d\n", imat,splitting);
      }
      
    }
  }
  else if(verbose > 1){
    printf("No material with x-ray splitting enabled.\n");
  }
  
  // Bodies
  //************

  std::vector<std::string> xRayBodies;
  VRSection.ls("x-ray/bodies",xRayBodies);

  if(xRayBodies.size() > 0){
    if(verbose > 1){
      printf("\n\n **** Body x-ray splitting:\n\n");
      printf(" Body  | x-ray splitting\n");      
    }
    
    for(unsigned ibname = 0; ibname < xRayBodies.size(); ibname++){

      //Get ibody name section
      pen_parserSection bodySection;
      std::string bodySecKey = std::string("x-ray/bodies/") + xRayBodies[ibname];
      if(VRSection.readSubsection(bodySecKey,bodySection) != INTDATA_SUCCESS){
	if(verbose > 0){
	  printf("setVarianceReduction: 'VR/%s' is not a section, skip this body.\n",bodySecKey.c_str());
	}
	continue;
      }

      // Body index
      //***********************

      //Check if specified body exists
      unsigned ibody = geometry.getIBody(xRayBodies[ibname].c_str());
      if(ibody >= geometry.getBodies()){
	if(verbose > 0){
	  printf("setVarianceReduction: Body '%s' doesn't exists in loaded geometry.\n",xRayBodies[ibname].c_str());
	}
	return -28;
      }
      else if(ibody >= context.NBV){
	if(verbose > 0){
	  printf("setVarianceReduction: Error: Maximum body index for IF is (%u)\n",context.NBV);
	  printf("                  specified index: %u\n",ibody);
	}
	return -28;
      }

      // Splitting factor
      //***********************

      //Get splitting factor
      int splitting;
      err = bodySection.read("splitting",splitting);
      if(err != INTDATA_SUCCESS){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Unable to read field 'splitting' for x-ray splitting on body '%s'. Integer expected.\n",xRayBodies[ibname].c_str());
	return -29;
      }

      //Check splitting factor
      if(splitting < 1){
	if(verbose > 0)
	  printf("setVarianceReduction: Error: Invalid x-ray splitting factor (%d).\n",splitting);
	return -30;
      }

      //Set splitting factor for specified body
      context.IXRSPL[ibody] = (unsigned)splitting;	  
      context.LXRSPL[ibody] = true;	  

      //Print configuration
      if(verbose > 1){
	printf(" %5d   %5d\n", ibody,splitting);
      }
      
    }
  }
  else if(verbose > 1){
    printf("No bodies with x-ray splitting enabled.\n");
  }

  if(verbose > 1){
    printf("\n\nFinal x-ray splitting:\n\n");
    printf(" Body  | x-ray splitting\n");
    for(unsigned ibody = 0; ibody < context.NBV; ibody++){      
      
      if(context.LXRSPL[ibody])
	printf(" %5u   %5u\n", ibody,context.IBRSPL[ibody]);
    }
    printf("\n\n");
  }
  
  return 0;
}
