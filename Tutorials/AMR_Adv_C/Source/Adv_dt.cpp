
#include <Adv.H>
#include <Adv_F.H>

Real
Adv::initialTimeStep ()
{
    return estTimeStep(0.0);
}

Real
Adv::estTimeStep (Real)
{
    // This is just a dummy value to start with 
    Real dt_est  = 1.0e+20;

    const Real* dx = geom.CellSize();
    const Real* prob_lo = geom.ProbLo();
    const Real cur_time = state[State_Type].curTime();
    const MultiFab& S_new = get_new_data(State_Type);

#ifdef _OPENMP
#pragma omp parallel reduction(min:dt_est)
#endif
    {
	FArrayBox uedg[BL_SPACEDIM];

	for (MFIter mfi(S_new, true); mfi.isValid(); ++mfi)
	{
	    for (int i = 0; i < BL_SPACEDIM ; i++) {
		const Box& bx = mfi.nodaltilebox(i);
		uedg[i].resize(bx,1);
	    }

	    BL_FORT_PROC_CALL(GET_EDGE_VELOCITY,get_edge_velocity)
		(level, cur_time,
		 D_DECL(BL_TO_FORTRAN(uedg[0]),
			BL_TO_FORTRAN(uedg[1]),
			BL_TO_FORTRAN(uedg[2])),
		 dx, prob_lo);

	    for (int i = 0; i < BL_SPACEDIM; ++i) {
		dt_est = std::min(dt_est, dx[i] / (1.e-10 + uedg[i].norm(0)));
	    }
	}
    }

    ParallelDescriptor::ReduceRealMin(dt_est);
    dt_est *= cfl;

    if (verbose && ParallelDescriptor::IOProcessor())
	std::cout << "Adv::estTimeStep at level " << level << ":  dt_est = " << dt_est << std::endl;
    
    return dt_est;
}

void
Adv::computeNewDt (int                   finest_level,
		   int                   sub_cycle,
		   Array<int>&           n_cycle,
		   const Array<IntVect>& ref_ratio,
		   Array<Real>&          dt_min,
		   Array<Real>&          dt_level,
		   Real                  stop_time,
		   int                   post_regrid_flag)
{
    //
    // We are at the end of a coarse grid timecycle.
    // Compute the timesteps for the next iteration.
    //
    if (level > 0)
        return;

    for (int i = 0; i <= finest_level; i++)
    {
        Adv& adv_level = getLevel(i);
        dt_min[i] = adv_level.estTimeStep(dt_level[i]);
    }

    if (post_regrid_flag == 1) 
    {
	//
	// Limit dt's by pre-regrid dt
	//
	for (int i = 0; i <= finest_level; i++)
	{
	    dt_min[i] = std::min(dt_min[i],dt_level[i]);
	}
    } 
    
    //
    // Find the minimum over all levels
    //
    Real dt_0 = 1.0e+100;
    int n_factor = 1;
    for (int i = 0; i <= finest_level; i++)
    {
        n_factor *= n_cycle[i];
        dt_0 = std::min(dt_0,n_factor*dt_min[i]);
    }

    //
    // Limit dt's by the value of stop_time.
    //
    const Real eps = 0.001*dt_0;
    Real cur_time  = state[State_Type].curTime();
    if (stop_time >= 0.0) {
        if ((cur_time + dt_0) > (stop_time - eps))
            dt_0 = stop_time - cur_time;
    }

    n_factor = 1;
    for (int i = 0; i <= finest_level; i++)
    {
        n_factor *= n_cycle[i];
        dt_level[i] = dt_0/n_factor;
    }
}

void
Adv::computeInitialDt (int                   finest_level,
		       int                   sub_cycle,
		       Array<int>&           n_cycle,
		       const Array<IntVect>& ref_ratio,
		       Array<Real>&          dt_level,
		       Real                  stop_time)
{
    //
    // Grids have been constructed, compute dt for all levels.
    //
    if (level > 0)
        return;

    Real dt_0 = 1.0e+100;
    int n_factor = 1;
    for (int i = 0; i <= finest_level; i++)
    {
        dt_level[i] = getLevel(i).initialTimeStep();
        n_factor   *= n_cycle[i];
        dt_0 = std::min(dt_0,n_factor*dt_level[i]);
    }

    //
    // Limit dt's by the value of stop_time.
    //
    const Real eps = 0.001*dt_0;
    Real cur_time  = state[State_Type].curTime();
    if (stop_time >= 0.0) {
        if ((cur_time + dt_0) > (stop_time - eps))
            dt_0 = stop_time - cur_time;
    }

    n_factor = 1;
    for (int i = 0; i <= finest_level; i++)
    {
        n_factor *= n_cycle[i];
        dt_level[i] = dt_0/n_factor;
    }
}
