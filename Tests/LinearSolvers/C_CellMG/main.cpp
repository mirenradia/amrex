//
// $Id: main.cpp,v 1.38 2011-07-19 17:11:07 marc Exp $
//

#include <fstream>
#include <iomanip>

#include <Utility.H>
#include <ParmParse.H>
#include <LO_BCTYPES.H>
#include <BndryData.H>
#include <MultiGrid.H>
#include <CGSolver.H>
#include <Laplacian.H>
#include <ABecLaplacian.H>
#include <ParallelDescriptor.H>
#include <VisMF.H>
#include <COEF_F.H>

static
Real
mfnorm_0_valid (const MultiFab& mf)
{
    Real r = 0;
    for ( MFIter cmfi(mf); cmfi.isValid(); ++cmfi )
    {
        Real s = mf[cmfi].norm(cmfi.validbox(), 0, 0, mf[cmfi].nComp());
        r = (r > s) ? r : s;
    }
    ParallelDescriptor::ReduceRealMax(r);
    return r;
}

static
Real
mfnorm_2_valid (const MultiFab& mf)
{
    Real r = 0;
    for ( MFIter cmfi(mf); cmfi.isValid(); ++cmfi )
    {
        Real s = mf[cmfi].norm(cmfi.validbox(), 2, 0, mf[cmfi].nComp());
        r += s*s;
    }
    ParallelDescriptor::ReduceRealSum(r);
    return ::sqrt(r);
}

static
BoxList
readBoxList(const std::string file, Box& domain)
{
    BoxList retval;

    std::ifstream boxspec;

    boxspec.open(file.c_str(), std::ios::in);

    if( !boxspec )
    {
        std::string msg = "readBoxList: unable to open ";
        msg += file;
        BoxLib::Error(msg.c_str());
    }
    boxspec >> domain;
    
    int numbox = 0;
    boxspec >> numbox;

    for ( int i=0; i<numbox; i++ )
    {
        Box tmpbox;
        boxspec >> tmpbox;
        if( !domain.contains(tmpbox) )
	{
            std::cerr << "readBoxList: bogus box " << tmpbox << '\n';
            exit(1);
        }
        retval.push_back(tmpbox);
    }

    return retval;
}

static
void
writePlotFile (const std::string& dir,
               const MultiFab&    mf,
               const Geometry&    geom)
{
    BL_ASSERT(mf.nComp() == 2);
    //
    // Only let 64 CPUs be writing at any one time.
    //
    VisMF::SetNOutFiles(64);
    //
    // Only the I/O processor makes the directory if it doesn't already exist.
    //
    if (ParallelDescriptor::IOProcessor())
        if (!BoxLib::UtilCreateDirectory(dir, 0755))
            BoxLib::CreateDirectoryFailed(dir);
    //
    // Force other processors to wait till directory is built.
    //
    ParallelDescriptor::Barrier();

    std::string HeaderFileName = dir + "/Header";

    VisMF::IO_Buffer io_buffer(VisMF::IO_Buffer_Size);

    std::ofstream HeaderFile;

    HeaderFile.rdbuf()->pubsetbuf(io_buffer.dataPtr(), io_buffer.size());

    if (ParallelDescriptor::IOProcessor())
    {
        //
        // Only the IOProcessor() writes to the header file.
        //
        HeaderFile.open(HeaderFileName.c_str(), std::ios::out|std::ios::trunc|std::ios::binary);
        if (!HeaderFile.good())
            BoxLib::FileOpenFailed(HeaderFileName);
        HeaderFile << "NavierStokes-V1.1\n";
        HeaderFile << 2 << '\n';
        HeaderFile << "soln\nrhs\n";
        HeaderFile << BL_SPACEDIM << '\n';
        HeaderFile << 0 << '\n';
        HeaderFile << 0 << '\n';
        for (int i = 0; i < BL_SPACEDIM; i++)
            HeaderFile << geom.ProbLo(i) << ' ';
        HeaderFile << '\n';
        for (int i = 0; i < BL_SPACEDIM; i++)
            HeaderFile << geom.ProbHi(i) << ' ';
        HeaderFile << '\n';
        HeaderFile << '\n';
        HeaderFile << geom.Domain() << ' ';
        HeaderFile << '\n';
        HeaderFile << 0 << ' ';
        HeaderFile << '\n';
        for (int k = 0; k < BL_SPACEDIM; k++)
            HeaderFile << geom.CellSize()[k] << ' ';
        HeaderFile << '\n';
        HeaderFile << geom.Coord() << '\n';
        HeaderFile << "0\n";
    }
    // Build the directory to hold the MultiFab at this level.
    // The name is relative to the directory containing the Header file.
    //
    static const std::string BaseName = "/Cell";
    char buf[64];
    sprintf(buf, "Level_%d", 0);
    std::string Level = buf;
    //
    // Now for the full pathname of that directory.
    //
    std::string FullPath = dir;
    if (!FullPath.empty() && FullPath[FullPath.length()-1] != '/')
        FullPath += '/';
    FullPath += Level;
    //
    // Only the I/O processor makes the directory if it doesn't already exist.
    //
    if (ParallelDescriptor::IOProcessor())
        if (!BoxLib::UtilCreateDirectory(FullPath, 0755))
            BoxLib::CreateDirectoryFailed(FullPath);
    //
    // Force other processors to wait till directory is built.
    //
    ParallelDescriptor::Barrier();

    if (ParallelDescriptor::IOProcessor())
    {
        HeaderFile << 0 << ' ' << mf.boxArray().size() << ' ' << 0 << '\n';
        HeaderFile << 0 << '\n';

        for (int i = 0; i < mf.boxArray().size(); ++i)
        {
            RealBox loc = RealBox(mf.boxArray()[i],geom.CellSize(),geom.ProbLo());
            for (int n = 0; n < BL_SPACEDIM; n++)
                HeaderFile << loc.lo(n) << ' ' << loc.hi(n) << '\n';
        }

        std::string PathNameInHeader = Level;
        PathNameInHeader += BaseName;
        HeaderFile << PathNameInHeader << '\n';
    }
    //
    // Use the Full pathname when naming the MultiFab.
    //
    std::string TheFullPath = FullPath;
    TheFullPath += BaseName;
    VisMF::Write(mf,TheFullPath);
}

int
main (int argc, char* argv[])
{
  BoxLib::Initialize(argc,argv);

  std::cout << std::setprecision(15);

  ParmParse pp;
  //
  // Obtain prob domain and box-list, set H per phys domain [0:1]Xn
  //
  Box container;
  int ba_coarsen = 1        ; pp.query("ba_coarsen", ba_coarsen);

#if (BL_SPACEDIM == 2)
  std::string boxfile("grids/gr.2_small_a") ; pp.query("boxes", boxfile);
#elif (BL_SPACEDIM == 3)
  std::string boxfile("grids/gr.3_small_a") ; pp.query("boxes", boxfile);
#endif

  BoxArray bs(readBoxList(boxfile,container));

  if ( ba_coarsen > 1 )
      bs.coarsen(ba_coarsen);

  Geometry geom( container );
  Real dx[BL_SPACEDIM];
  for ( int n=0; n<BL_SPACEDIM; n++ )
      dx[n] = ( geom.ProbHi(n) - geom.ProbLo(n) )/container.length(n);
  //
  // Allocate/initialize solution and right-hand-side, reset
  // rhs=1 at each box center.
  //
  int Ncomp=1;
  int Nghost=1;
  MultiFab soln(bs, Ncomp, Nghost, Fab_allocate); soln.setVal(0.0);
  MultiFab  rhs(bs, Ncomp, Nghost, Fab_allocate);  rhs.setVal(0.0);
  for ( MFIter rhsmfi(rhs); rhsmfi.isValid(); ++rhsmfi )
  {
      IntVect ivmid = (rhs[rhsmfi].smallEnd() + rhs[rhsmfi].bigEnd())/2;
      rhs[rhsmfi].operator()(ivmid,0) = 1;
      ivmid += IntVect::TheUnitVector();
      rhs[rhsmfi].operator()(ivmid,0) = -1;
//    std::cout << rhs[rhsmfi] << std::endl;
  }
  //
  // Initialize boundary data, set boundary condition flags and locations:
  // (phys boundaries set to dirichlet on cell walls).
  //
  BndryData bd(bs, 1, geom);
  int comp = 0;
  for ( int n=0; n<BL_SPACEDIM; ++n )
    {
        for ( MFIter mfi(rhs); mfi.isValid(); ++mfi )
	{
            int i = mfi.index();  //   ^^^ using rhs to get mfi.index() yes, this is a hack
            bd.setBoundLoc(Orientation(n, Orientation::low) ,i,0.0 );
            bd.setBoundLoc(Orientation(n, Orientation::high),i,0.0 );
            bd.setBoundCond(Orientation(n, Orientation::low) ,i,comp,LO_DIRICHLET);
            bd.setBoundCond(Orientation(n, Orientation::high),i,comp,LO_DIRICHLET);
            bd.setValue(Orientation(n, Orientation::low) ,i, 0.0);
            bd.setValue(Orientation(n, Orientation::high),i, 0.0);
	}
    }
  //
  // Choose operator (Laplacian or ABecLaplacian), get tolerance, numiter.
  //
  bool ABec=false           ; pp.query("ABec",ABec);
  Real tolerance = 1.0e-12  ; pp.query("tol", tolerance);
  Real tolerance_abs = -1.0 ; pp.query("tol_abs", tolerance_abs);
  int numiter = 41          ; pp.query("numiter", numiter);
  int maxiter = 40          ; pp.query("maxiter", maxiter);
  bool mg = true            ; pp.query("mg", mg);
  bool cg = false           ; pp.query("cg", cg);
  bool bicg = false         ; pp.query("bicg", bicg);
  bool use_mg_pre=false     ; pp.query("mg_pre",use_mg_pre);
  bool new_bc=false         ; pp.query("new_bc",new_bc);
  bool dump_norm=true       ; pp.query("dump_norm", dump_norm);
  bool dump_Lp=false        ; pp.query("dump_Lp",dump_Lp);
  bool dump_MF=false        ; pp.query("dump_MF", dump_MF);
  bool dump_VisMF=false     ; pp.query("dump_VisMF", dump_VisMF);
  bool dump_ascii=false     ; pp.query("dump_ascii", dump_ascii);
  bool dump_rhs_ascii=false ; pp.query("dump_rhs_ascii", dump_rhs_ascii);

  bool use_variable_coef=false; pp.query("use_variable_coef", use_variable_coef);

  int res;

  if ( !ABec )
  {
      //
      // Build Laplacian operator, solver, then solve.
      //
      Laplacian lp(bd, dx[0]);
      {
          double d = lp.norm();
          if ( ParallelDescriptor::IOProcessor() )
	  {
              std::cout << "Norm = " << d << std::endl;
	  }
      }
      if ( mg )
      {
          const Real run_strt = ParallelDescriptor::second();

	  MultiGrid mg(lp);
	  mg.solve(soln, rhs, tolerance, tolerance_abs);
	  if ( new_bc )
          {
	      for ( MFIter mfi(rhs); mfi.isValid(); ++mfi )
              {
		  int i = mfi.index(); //   ^^^ using rhs to get mfi.index() yes, this is a hack
		  for (int n=0; n<BL_SPACEDIM; ++n)
                  {
		      bd.setValue(Orientation(n, Orientation::low) ,i,2.0);
		      bd.setValue(Orientation(n, Orientation::high),i,2.0);
                  }
              }
	      lp.bndryData(bd);
	      mg.solve(soln, rhs, tolerance, tolerance_abs);
          }

          const int IOProc   = ParallelDescriptor::IOProcessorNumber();
          Real      run_stop = ParallelDescriptor::second() - run_strt;

          ParallelDescriptor::ReduceRealMax(run_stop,IOProc);

         if (ParallelDescriptor::IOProcessor())
              std::cout << "Run time = " << run_stop << std::endl;
      }
      if ( cg )
      {
	  CGSolver cg(lp,use_mg_pre); cg.setCGSolver(CGSolver::CG);
	  cg.setMaxIter(maxiter);
	  res = cg.solve(soln, rhs, tolerance, tolerance_abs);
	  std::cout << "CG Result = " << res << std::endl;
	  if ( new_bc )
          {
	      for ( MFIter mfi(rhs); mfi.isValid(); ++mfi )
              {
		  int i = mfi.index();  //   ^^^ using rhs to get mfi.index() yes, this is a hack
		  for ( int n=0; n<BL_SPACEDIM; ++n )
                  {
		      bd.setValue(Orientation(n, Orientation::low) ,i,4.0);
		      bd.setValue(Orientation(n, Orientation::high),i,4.0);
                  }
              }
	      lp.bndryData(bd);
	      res  = cg.solve(soln, rhs, tolerance, tolerance_abs);
	      std::cout << "CG (new_bc) Result = " << res << std::endl;
          }
      }
      if ( bicg )
      {
	  CGSolver cg(lp,use_mg_pre); cg.setCGSolver(CGSolver::BiCGStab);
	  cg.setMaxIter(maxiter);
	  res = cg.solve(soln, rhs, tolerance, tolerance_abs);
	  std::cout << "BiCGStab Result = " << res << std::endl;
	  if ( new_bc )
          {
	      for ( MFIter mfi(rhs); mfi.isValid(); ++mfi )
              {
		  int i = mfi.index();  //   ^^^ using rhs to get mfi.index() yes, this is a hack
		  for ( int n=0; n<BL_SPACEDIM; ++n )
                  {
		      bd.setValue(Orientation(n, Orientation::low) ,i,4.0);
		      bd.setValue(Orientation(n, Orientation::high),i,4.0);
                  }
              }
	      lp.bndryData(bd);
	      res = cg.solve(soln, rhs, tolerance, tolerance_abs);
	      std::cout << "BiCGStab (new_bc) Result = " << res << std::endl;
          }
      }

      if ( dump_Lp )
          std::cout << lp << std::endl;
	
        
  }
  else
  {
      //
      // Allocate space for ABecLapacian coeffs, fill with values.
      //
      Real alpha = 1.0; pp.query("alpha",alpha);
      Real beta =  1.0; pp.query("beta",beta);
      Real a=0.0; pp.query("a",  a);
      Tuple<Real, BL_SPACEDIM> b;
      b[0]=1.0; pp.query("b0", b[0]);
      b[1]=1.0; pp.query("b1", b[1]);
#if (BL_SPACEDIM > 2)
      b[2]=1.0; pp.query("b2", b[2]);
#endif
        
      MultiFab  acoefs;
      acoefs.define(bs, Ncomp, Nghost, Fab_allocate);
      acoefs.setVal(a);
        
      MultiFab bcoefs[BL_SPACEDIM];

      if (use_variable_coef) {
        MultiFab cc_coef(bs,1,1);
        for ( MFIter mfi(cc_coef); mfi.isValid(); ++mfi )
        {
          const int* clo = cc_coef[mfi].loVect();
          const int* chi = cc_coef[mfi].hiVect();
          const Box& bx = mfi.validbox();
  
          FORT_SET_CC_COEF(cc_coef[mfi].dataPtr(),ARLIM(clo),ARLIM(chi),bx.loVect(),bx.hiVect(),dx,geom.ProbLo(),geom.ProbHi());
        }

        VisMF::Write(cc_coef,"COEF");

        for ( int n=0; n<BL_SPACEDIM; ++n )
        {
  	  BoxArray bsC(bs);
	  bcoefs[n].define(bsC.surroundingNodes(n), Ncomp, Nghost, Fab_allocate);
          for ( MFIter mfi(bcoefs[n]); mfi.isValid(); ++mfi )
          {
            Box bx(bs[mfi.index()]);
            const int* clo = cc_coef[mfi].loVect();
            const int* chi = cc_coef[mfi].hiVect();
            const int* edgelo = bcoefs[n][mfi].loVect();
            const int* edgehi = bcoefs[n][mfi].hiVect();

            FORT_COEF_TO_EDGES(&n,bcoefs[n][mfi].dataPtr(),ARLIM(edgelo),ARLIM(edgehi),
                               cc_coef[mfi].dataPtr(),ARLIM(clo),ARLIM(chi),
                               bx.loVect(),bx.hiVect());
          }
        }
      } else {
        for ( int n=0; n<BL_SPACEDIM; ++n )
        {
  	  BoxArray bsC(bs);
	  bcoefs[n].define(bsC.surroundingNodes(n), Ncomp, Nghost, Fab_allocate);
  	  bcoefs[n].setVal(b[n]);
        }
      }
      //
      // Build operator, set coeffs, build solver, solve
      //
      {
	  ABecLaplacian lp(bd, dx);
	  lp.setScalars(alpha, beta);
	  lp.setCoefficients(acoefs, bcoefs);
          {
              double d = lp.norm();
              if ( ParallelDescriptor::IOProcessor() )
	      {
                  std::cout << "Norm = " << d << std::endl;
	      }
          }

	  if ( mg )
          {
	      const Real run_strt = ParallelDescriptor::second();

	      MultiGrid mg(lp);
	      mg.solve(soln, rhs, tolerance, tolerance_abs);
	      if ( new_bc )
              {
		  for ( int i=0; i < bs.size(); ++i )
                  {
		      for ( int n=0; n<BL_SPACEDIM; ++n )
                      {
			  bd.setValue(Orientation(n, Orientation::low) ,i,2.0);
			  bd.setValue(Orientation(n, Orientation::high),i,2.0);
                      } 
                  }
		  lp.bndryData(bd);
		  mg.solve(soln, rhs, tolerance, tolerance_abs);
              }

	      const int IOProc   = ParallelDescriptor::IOProcessorNumber();
	      Real      run_stop = ParallelDescriptor::second() - run_strt;

	      ParallelDescriptor::ReduceRealMax(run_stop,IOProc);

	      if (ParallelDescriptor::IOProcessor())
                  std::cout << "Run time = " << run_stop << std::endl;
          }
	  if ( cg )
          {
	      CGSolver cg(lp,use_mg_pre);
	      cg.setMaxIter(maxiter);
	      cg.solve(soln, rhs, tolerance, tolerance_abs);
	      if ( new_bc )
              {
		  for ( int i=0; i < bs.size(); ++i )
                  {
		      for ( int n=0; n<BL_SPACEDIM; ++n )
                      {
			  bd.setValue(Orientation(n, Orientation::low) ,i,4.0);
			  bd.setValue(Orientation(n, Orientation::high),i,4.0);
                      }
                  }
		  lp.bndryData(bd);
		  cg.solve(soln, rhs, tolerance, tolerance_abs);
              }
          }
	  if ( bicg )
          {
	      CGSolver cg(lp,use_mg_pre); cg.setCGSolver(CGSolver::BiCGStab);
	      cg.setMaxIter(maxiter);
	      cg.solve(soln, rhs, tolerance, tolerance_abs);
	      if ( new_bc )
              {
		  for ( int i=0; i < bs.size(); ++i )
                  {
		      for ( int n=0; n<BL_SPACEDIM; ++n )
                      {
			  bd.setValue(Orientation(n, Orientation::low) ,i,4.0);
			  bd.setValue(Orientation(n, Orientation::high),i,4.0);
                      }
                  }
		  lp.bndryData(bd);
		  cg.solve(soln, rhs, tolerance, tolerance_abs);
              }
          }

	  if ( dump_Lp )
              std::cout << lp << std::endl;
      }
  } // -->> solve D^2(soln)=rhs   or   (alpha*a - beta*D.(b.G))soln=rhs

  //
  // Write solution, and rhs.
  //
  if ( dump_norm )
  {
      double d1 = mfnorm_2_valid(soln);
      double d2 = mfnorm_0_valid(soln);
      if ( ParallelDescriptor::IOProcessor() )
      {
	  std::cout << "solution norm = " << d1 << "/" << d2 << std::endl;
      }

      if (false)
      {
          double mean = 0;
          for (MFIter mfi(soln); mfi.isValid(); ++mfi)
              mean += soln[mfi].sum(0);

          ParallelDescriptor::ReduceRealSum(mean);

          mean /= soln.boxArray().numPts();

          for (MFIter mfi(soln); mfi.isValid(); ++mfi)
              soln[mfi].plus(-mean);

          double d1 = mfnorm_2_valid(soln);
          double d2 = mfnorm_0_valid(soln);
          if ( ParallelDescriptor::IOProcessor() )
          {
              std::cout << "solution norm (w/mean subtracted off) = " << d1 << "/" << d2 << std::endl;
          }
      }
  }
  if ( dump_MF || dump_VisMF )
  {
      MultiFab temp(bs, 2, 0);
      temp.setVal(0.0);
      temp.copy(soln, 0, 0, 1);
      temp.copy(rhs,  0, 1, 1);
      if ( dump_MF )
      {
	  writePlotFile("soln_pf", temp, geom);
      }
      if ( dump_VisMF )
      {
	  VisMF::Write(temp, "soln_vismf", VisMF::OneFilePerCPU);
      }
  }
  
  if ( dump_ascii )
  {
      for ( MFIter mfi(soln); mfi.isValid(); ++mfi )
      {
	  std::cout << soln[mfi] << std::endl;
      }
  }

  if ( dump_rhs_ascii )
  {
      for ( MFIter mfi(soln); mfi.isValid(); ++mfi )
      {
	  std::cout << rhs[mfi] << std::endl;
      }
  }

  BoxLib::Finalize();

}

