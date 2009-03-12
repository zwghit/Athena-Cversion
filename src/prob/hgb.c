#include "copyright.h"
/*==============================================================================
 * FILE: hgb.c
 *
 * PURPOSE:  Problem generator for 3D shearing sheet.  Based on the initial
 *   conditions described in "Local Three-dimensional Magnetohydrodynamic
 *   Simulations of Accretion Disks" by Hawley, Gammie & Balbus, or HGB.
 *
 * Several different field configurations and perturbations are possible:
 *
 *  ifield = 0 - uses field set by choice of ipert flag
 *  ifield = 1 - Bz=B0sin(kx*x1) field with zero-net-flux [default] (kx input)
 *  ifield = 2 - uniform Bz
 *  ifield = 3 - B=(0,B0cos(kx*x1),B0sin(kx*x1))= zero-net flux w helicity
 *  ifield = 4 - B=(0,B0/sqrt(2),B0/sqrt(2))= net toroidal+vertical field
 *
 *  ipert = 1 - random perturbations to P and V [default, used by HGB]
 *  ipert = 2 - uniform Vx=amp (epicyclic wave test)
 *  ipert = 3 - J&G vortical shwave (hydro test)
 *  ipert = 4 - nonlinear density wave test of Fromang & Paploizou
 *  ipert = 5 - 2nd MHD shwave test of JGG (2008) -- their figure 9
 *  ipert = 6 - 3rd MHD shwave test of JGG (2008) -- their figure 11
 *
 * To run simulations of stratified disks (including vertical gravity),
 * un-comment the macro VERTICAL_GRAVITY below.
 *
 * Code must be configured using --enable-shearing-box
 *
 * REFERENCE: Hawley, J. F. & Balbus, S. A., ApJ 400, 595-609 (1992).
 *            Johnson, Guan, & Gammie, ApJSupp, (2008)
 *============================================================================*/

#include <float.h>
#include <math.h>

#include <stdlib.h>
#include <string.h>
#include "defs.h"
#include "athena.h"
#include "globals.h"
#include "prototypes.h"

/* #define VERTICAL_GRAVITY */

/*==============================================================================
 * PRIVATE FUNCTION PROTOTYPES:
 * ran2()          - random number generator from NR
 * ShearingBoxPot() - tidal potential in 3D shearing box
 * expr_dV2()       - computes delta(Vy)
 * hst_*            - new history variables
 *============================================================================*/

static double ran2(long int *idum);
static Real ShearingBoxPot(const Real x1, const Real x2, const Real x3);
static Real expr_dV2(const Grid *pG, const int i, const int j, const int k);
static Real hst_rho_Vx_dVy(const Grid *pG,const int i,const int j,const int k);
static Real hst_rho_dVy2(const Grid *pG, const int i, const int j, const int k);
#ifdef ADIABATIC
static Real hst_E_total(const Grid *pG, const int i, const int j, const int k);
#endif
#ifdef MHD
static Real hst_Bx(const Grid *pG, const int i, const int j, const int k);
static Real hst_By(const Grid *pG, const int i, const int j, const int k);
static Real hst_Bz(const Grid *pG, const int i, const int j, const int k);
static Real hst_BxBy(const Grid *pG, const int i, const int j, const int k);
static Real hst_dEw2(const Grid *pG, const int i, const int j, const int k);
static Real hst_dBy(const Grid *pG, const int i, const int j, const int k);
#endif /* MHD */

/*=========================== PUBLIC FUNCTIONS =================================
 * Contains the usual, plus:
 * ShearingSheetBC() - shearing sheet BCs in 3D, called by set_bval().
 * EvolveEy()      - sets Ey in integrator to keep <Bz>=const. 
 *============================================================================*/
/*----------------------------------------------------------------------------*/
/* problem:  */

void problem(Grid *pGrid, Domain *pDomain)
{
  FILE *fp;
  Real xFP[160],dFP[160],vxFP[160],vyFP[160];
  int is = pGrid->is, ie = pGrid->ie;
  int js = pGrid->js, je = pGrid->je;
  int ks = pGrid->ks, ke = pGrid->ke;
  int ixs,jxs,kxs,i,j,k,ipert,ifield;
  long int iseed = -1; /* Initialize on the first call to ran2 */
  Real x1,x2,x3,xmin,xmax,Lx,Ly,Lz;
  Real den = 1.0, pres = 1.0e-6, rd, rp, rvx, rvy, rvz, rbx, rby, rbz;
  Real beta,B0,kx,ky,kz,amp;
  int nwx,nwy,nwz;  /* input number of waves per Lx,Ly,Lz [default=1] */
  double rval;

  if (pGrid->Nx2 == 1){
    ath_error("[problem]: HGB only works on a 2D or 3D grid\n");
  }

/* Read problem parameters.  Note Omega set to 10^{-3} by default */
  Omega = par_getd_def("problem","omega",1.0e-3);
  amp = par_getd("problem","amp");
  beta = par_getd("problem","beta");
  ifield = par_geti_def("problem","ifield", 1);
  ipert = par_geti_def("problem","ipert", 1);

/* Compute field strength based on beta.  */
#ifdef ISOTHERMAL
  pres = Iso_csound2;
#endif
  B0 = sqrt((double)(2.0*pres/beta));

/* Ensure a different initial random seed for each process in an MPI calc. */
  ixs = pGrid->is + pGrid->idisp;
  jxs = pGrid->js + pGrid->jdisp;
  kxs = pGrid->ks + pGrid->kdisp;
  iseed = -1 - (ixs + pDomain->Nx1*(jxs + pDomain->Nx2*kxs));

/* Initialize boxsize */
  xmin = par_getd("grid","x1min");
  xmax = par_getd("grid","x1max");
  Lx = xmax - xmin;

  xmin = par_getd("grid","x2min");
  xmax = par_getd("grid","x2max");
  Ly = xmax - xmin;

  xmin = par_getd("grid","x3min");
  xmax = par_getd("grid","x3max");
  Lz = xmax - xmin;

/* initialize wavenumbers, given input number of waves per L */
  nwx = par_geti_def("problem","nwx",1);
  nwy = par_geti_def("problem","nwy",1);
  nwz = par_geti_def("problem","nwz",1);
  kx = (2.0*PI/Lx)*((double)nwx);  /* nxw should be -ve for leading wave */
  ky = (2.0*PI/Ly)*((double)nwy);
  kz = (2.0*PI/Lz)*((double)nwz);

/* For PF density wave test, read data from file */

  if (ipert == 4) {
    if (pGrid->Nx1 == 160) {
      if((fp = fopen("Data-160-FPwave.dat","r")) == NULL)
         ath_error("Error opening Data-160-FPwave.dat\n");
      for (i=0; i<160; i++) {
        fscanf(fp,"%lf %lf %lf %lf",&xFP[i],&dFP[i],&vxFP[i],&vyFP[i]);
      }
    }

    if (pGrid->Nx1 == 40) {
      if((fp = fopen("Data-40-FPwave.dat","r")) == NULL)
         ath_error("Error opening Data-40-FPwave.dat\n");
      for (i=0; i<40; i++) {
        fscanf(fp,"%lf %lf %lf %lf",&xFP[i],&dFP[i],&vxFP[i],&vyFP[i]);
      }
    }

    xmin = par_getd("grid","x1min");
    if (xmin != -4.7965) ath_error("[hgb]: iprob=4 requires xmin=-4.7965\n");
    xmax = par_getd("grid","x1max");
    if (xmax != 4.7965) ath_error("[hgb]: iprob=4 requires xmax=4.7965\n");
  }

/* Rescale amp to sound speed for ipert 2,3 */
#ifdef ADIABATIC
  if (ipert == 2 || ipert == 3) amp *= sqrt(Gamma*pres/den);
#else
  if (ipert == 2 || ipert == 3) amp *= Iso_csound;
#endif

  for (k=ks; k<=ke; k++) {
  for (j=js; j<=je; j++) {
    for (i=is; i<=ie; i++) {
      cc_pos(pGrid,i,j,k,&x1,&x2,&x3);

/* Initialize perturbations
 *  ipert = 1 - random perturbations to P and V [default, used by HGB]
 *  ipert = 2 - uniform Vx=amp (epicyclic wave test)
 *  ipert = 3 - vortical shwave (hydro test)
 *  ipert = 4 - Fromang & Paploizou nonlinear density wave (hydro test)
 *  ipert = 5 & 6 - JGG MHD shwave tests
 */
      if (ipert == 1) {
        rval = amp*(ran2(&iseed) - 0.5);
#ifdef ADIABATIC
        rp = pres*(1.0 + 2.0*rval);
        rd = den;
#else
        rd = den*(1.0 + 2.0*rval);
#endif
/* To conform to HGB, the perturbations to V/Cs are (1/5)amp/sqrt(Gamma)  */
        rval = amp*(ran2(&iseed) - 0.5);
        rvx = 0.4*rval*sqrt(pres/den);

        rval = amp*(ran2(&iseed) - 0.5);
        rvy = 0.4*rval*sqrt(pres/den);

        rval = amp*(ran2(&iseed) - 0.5);
        rvz = 0.4*rval*sqrt(pres/den);
      }
      if (ipert == 2) {
        rp = pres;
        rd = den;
        rvx = amp;
        rvy = 0.0;
        rvz = 0.0;
      }
      if (ipert == 3) {
        rp = pres;
        rd = den;
        rvx = amp*sin((double)(kx*x1 + ky*x2));
        rvy = -amp*(kx/ky)*sin((double)(kx*x1 + ky*x2));
        rvz = 0.0;
      }
      if (ipert == 4) {
        rd = dFP[i+pGrid->idisp];
        rvx = vxFP[i+pGrid->idisp];
        rvy = vyFP[i+pGrid->idisp] + 1.5*Omega*x1;  /* subtract mean flow */
        rvz = 0.0;
      }
/* Note the initial conditions in JGG for this test are incorrect.  B. Johnson
 * [private communication] is not certain what are the correct values used to
 * make the plot in the paper.  Thus, this test does not work */
      if (ipert == 5) {
        ifield = 0;
        rd = den + 8.9525e-10*cos((double)(kx*x1 + ky*x2 + kz*x3 - PI/4.));
        rvx = 8.16589e-8*cos((double)(kx*x1 + ky*x2 + kz*x3 + PI/4.));
        rvy = 8.70641e-8*cos((double)(kx*x1 + ky*x2 + kz*x3 + PI/4.));
        rvz = 0.762537e-8*cos((double)(kx*x1 + ky*x2 + kz*x3 + PI/4.));
        rbx = -1.08076e-7;
        rbx *= cos((double)(kx*(x1-0.5*pGrid->dx1) + ky*x2 + kz*x3 - PI/4.));
        rby = 1.04172e-7;
        rby *= cos((double)(kx*x1 + ky*(x2-0.5*pGrid->dx2) + kz*x3 - PI/4.));
        rbz = -0.320324e-7;
        rbz *= cos((double)(kx*x1 + ky*x2 + kz*(x3-0.5*pGrid->dx3) - PI/4.));;
        rbz += (sqrt(15.0)/16.0)*(Omega/kz);
      } 
      if (ipert == 6) {
        ifield = 0;
        rd = den + 5.48082e-6*cos((double)(kx*x1 + ky*x2 + kz*x3));
        rvx = -4.5856e-6*cos((double)(kx*x1 + ky*x2 + kz*x3));
        rvy = 2.29279e-6*cos((double)(kx*x1 + ky*x2 + kz*x3));
        rvz = 2.29279e-6*cos((double)(kx*x1 + ky*x2 + kz*x3));
        rbx = 5.48082e-7;
        rbx *= cos((double)(kx*(x1-0.5*pGrid->dx1) + ky*x2 + kz*x3));
        rbx += (0.1);
        rby = 1.0962e-6;
        rby *= cos((double)(kx*x1 + ky*(x2-0.5*pGrid->dx2) + kz*x3));
        rby += (0.2);
        rbz = 0.0;
      }

/* Initialize d, M, and P.  For 3D shearing box M1=Vx, M2=Vy, M3=Vz
 * With FARGO do not initialize the background shear */ 

      pGrid->U[k][j][i].d  = rd;
      pGrid->U[k][j][i].M1 = rd*rvx;
      pGrid->U[k][j][i].M2 = rd*rvy;
#ifndef FARGO
      pGrid->U[k][j][i].M2 -= rd*(1.5*Omega*x1);
#endif
      pGrid->U[k][j][i].M3 = rd*rvz;
#ifdef ADIABATIC
      pGrid->U[k][j][i].E = rp/Gamma_1
        + 0.5*(SQR(pGrid->U[k][j][i].M1) + SQR(pGrid->U[k][j][i].M2) 
             + SQR(pGrid->U[k][j][i].M3))/rd;
#endif

/* Initialize magnetic field.  For 3D shearing box B1=Bx, B2=By, B3=Bz
 *  ifield = 0 - 
 *  ifield = 1 - Bz=B0 sin(x1) field with zero-net-flux [default]
 *  ifield = 2 - uniform Bz
 *  ifield = 3 - B=(0,B0cos(kx*x1),B0sin(kx*x1))= zero-net flux w helicity
 *  ifield = 4 - B=(0,B0/sqrt(2),B0/sqrt(2))= net toroidal+vertical field
 */
#ifdef MHD
      if (ifield == 0) {
        pGrid->B1i[k][j][i] = rbx;
        pGrid->B2i[k][j][i] = rby;
        pGrid->B3i[k][j][i] = rbz;
        if (i==ie) pGrid->B1i[k][j][ie+1] =  pGrid->B1i[k][j][is];
        if (j==je) pGrid->B2i[k][je+1][i] =  pGrid->B2i[k][js][i];
        if (k==ke) pGrid->B3i[ke+1][j][i] =  pGrid->B3i[ks][j][i];
      }
      if (ifield == 1) {
        pGrid->B1i[k][j][i] = 0.0;
        pGrid->B2i[k][j][i] = 0.0;
        pGrid->B3i[k][j][i] = B0*(sin((double)kx*x1));
        if (i==ie) pGrid->B1i[k][j][ie+1] = 0.0;
        if (j==je) pGrid->B2i[k][je+1][i] = 0.0;
        if (k==ke) pGrid->B3i[ke+1][j][i] = B0*(sin((double)kx*x1));
      }
      if (ifield == 2) {
        pGrid->B1i[k][j][i] = 0.0;
        pGrid->B2i[k][j][i] = 0.0;
        pGrid->B3i[k][j][i] = B0;
        if (i==ie) pGrid->B1i[k][j][ie+1] = 0.0;
        if (j==je) pGrid->B2i[k][je+1][i] = 0.0;
        if (k==ke) pGrid->B3i[ke+1][j][i] = B0;
      }
      if (ifield == 3) {
        pGrid->B1i[k][j][i] = 0.0;
        pGrid->B2i[k][j][i] = B0*(cos((double)kx*x1));
        pGrid->B3i[k][j][i] = B0*(sin((double)kx*x1));
        if (i==ie) pGrid->B1i[k][j][ie+1] = 0.0;
        if (j==je) pGrid->B2i[k][je+1][i] = B0*(cos((double)kx*x1));
        if (k==ke) pGrid->B3i[ke+1][j][i] = B0*(sin((double)kx*x1));
      }
      if (ifield == 4) {
        pGrid->B1i[k][j][i] = 0.0;
        pGrid->B2i[k][j][i] = B0/sqrt(2);
        pGrid->B3i[k][j][i] = B0/sqrt(2);
        if (i==ie) pGrid->B1i[k][j][ie+1] = 0.0;
        if (j==je) pGrid->B2i[k][je+1][i] = B0/sqrt(2);
        if (k==ke) pGrid->B3i[ke+1][j][i] = B0/sqrt(2);
      }
#endif /* MHD */
    }
  }}
#ifdef MHD
  for (k=ks; k<=ke; k++) {
    for (j=js; j<=je; j++) {
      for (i=is; i<=ie; i++) {
        pGrid->U[k][j][i].B1c = 0.5*(pGrid->B1i[k][j][i]+pGrid->B1i[k][j][i+1]);
        pGrid->U[k][j][i].B2c = 0.5*(pGrid->B2i[k][j][i]+pGrid->B2i[k][j+1][i]);
        pGrid->U[k][j][i].B3c = 0.5*(pGrid->B3i[k][j][i]+pGrid->B3i[k+1][j][i]);
#ifdef ADIABATIC
      pGrid->U[k][j][i].E += 0.5*(SQR(pGrid->U[k][j][i].B1c)
         + SQR(pGrid->U[k][j][i].B2c) + SQR(pGrid->U[k][j][i].B3c));
#endif
      }
    }
  }
#endif /* MHD */

/* enroll gravitational potential function */

  StaticGravPot = ShearingBoxPot;

/* enroll new history variables */

  dump_history_enroll(hst_rho_Vx_dVy, "<rho Vx dVy>");
  dump_history_enroll(hst_rho_dVy2, "<rho dVy^2>");
#ifdef ADIABATIC
  dump_history_enroll(hst_E_total, "<E + rho Phi>");
#endif
#ifdef MHD
  dump_history_enroll(hst_Bx, "<Bx>");
  dump_history_enroll(hst_By, "<By>");
  dump_history_enroll(hst_Bz, "<Bz>");
  dump_history_enroll(hst_BxBy, "<-Bx By>");
  if (ipert == 5) dump_history_enroll(hst_dEw2, "<dEw2>");
  if (ipert == 6) dump_history_enroll(hst_dBy, "<dBy>");
#endif /* MHD */

/* With viscosity and/or resistivity, read eta_Ohm and nu_V */
#ifdef OHMIC
  eta_Ohm = par_getd("problem","eta");
#endif
#ifdef NAVIER_STOKES
  nu_V = par_getd("problem","nu");
#endif

  return;
}

/*==============================================================================
 * PUBLIC PROBLEM USER FUNCTIONS:
 * problem_write_restart() - writes problem-specific user data to restart files
 * problem_read_restart()  - reads problem-specific user data from restart files
 * get_usr_expr()          - sets pointer to expression for special output data
 * get_usr_out_fun()       - returns a user defined output function pointer
 * get_usr_par_prop()      - returns a user defined particle selection function
 * Userwork_in_loop        - problem specific work IN     main loop
 * Userwork_after_loop     - problem specific work AFTER  main loop
 *----------------------------------------------------------------------------*/

void problem_write_restart(Grid *pG, Domain *pD, FILE *fp)
{
  return;
}

/*
 * 'problem_read_restart' must enroll gravity on restarts
 */

void problem_read_restart(Grid *pG, Domain *pD, FILE *fp)
{
/* Read Omega, and with viscosity and/or resistivity, read eta_Ohm and nu_V */

  Omega = par_getd_def("problem","omega",1.0e-3);
#ifdef OHMIC
  eta_Ohm = par_getd("problem","eta");
#endif
#ifdef NAVIER_STOKES
  nu_V = par_getd("problem","nu");
#endif

/* enroll gravitational potential function */

  StaticGravPot = ShearingBoxPot;

/* enroll new history variables */

  dump_history_enroll(hst_rho_Vx_dVy, "<rho Vx dVy>");
  dump_history_enroll(hst_rho_dVy2, "<rho dVy^2>");
#ifdef ADIABATIC
  dump_history_enroll(hst_E_total, "<E + rho Phi>");
#endif
#ifdef MHD
  dump_history_enroll(hst_Bx, "<Bx>");
  dump_history_enroll(hst_By, "<By>");
  dump_history_enroll(hst_Bz, "<Bz>");
  dump_history_enroll(hst_BxBy, "<-Bx By>");
#endif /* MHD */

  return;
}

/* Get_user_expression computes dVy */
Gasfun_t get_usr_expr(const char *expr)
{
  if(strcmp(expr,"dVy")==0) return expr_dV2;
  return NULL;
}

VGFunout_t get_usr_out_fun(const char *name){
  return NULL;
}

#ifdef PARTICLES
PropFun_t get_usr_par_prop(const char *name)
{
  return NULL;
}

GVDFun_t get_usr_gasvshift(const char *name)
{
  return NULL;
}
#endif

void Userwork_in_loop(Grid *pGrid, Domain *pDomain)
{
}

void Userwork_after_loop(Grid *pGrid, Domain *pDomain)
{
}

/*------------------------------------------------------------------------------
 * ran2: extracted from the Numerical Recipes in C (version 2) code.  Modified
 *   to use doubles instead of floats. -- T. A. Gardiner -- Aug. 12, 2003
 */

#define IM1 2147483563
#define IM2 2147483399
#define AM (1.0/IM1)
#define IMM1 (IM1-1)
#define IA1 40014
#define IA2 40692
#define IQ1 53668
#define IQ2 52774
#define IR1 12211
#define IR2 3791
#define NTAB 32
#define NDIV (1+IMM1/NTAB)
#define RNMX (1.0-DBL_EPSILON)

/* Long period (> 2 x 10^{18}) random number generator of L'Ecuyer
 * with Bays-Durham shuffle and added safeguards.  Returns a uniform
 * random deviate between 0.0 and 1.0 (exclusive of the endpoint
 * values).  Call with idum = a negative integer to initialize;
 * thereafter, do not alter idum between successive deviates in a
 * sequence.  RNMX should appriximate the largest floating point value
 * that is less than 1.
 */

double ran2(long int *idum)
{
  int j;
  long int k;
  static long int idum2=123456789;
  static long int iy=0;
  static long int iv[NTAB];
  double temp;

  if (*idum <= 0) { /* Initialize */
    if (-(*idum) < 1) *idum=1; /* Be sure to prevent idum = 0 */
    else *idum = -(*idum);
    idum2=(*idum);
    for (j=NTAB+7;j>=0;j--) { /* Load the shuffle table (after 8 warm-ups) */
      k=(*idum)/IQ1;
      *idum=IA1*(*idum-k*IQ1)-k*IR1;
      if (*idum < 0) *idum += IM1;
      if (j < NTAB) iv[j] = *idum;
    }
    iy=iv[0];
  }
  k=(*idum)/IQ1;                 /* Start here when not initializing */
  *idum=IA1*(*idum-k*IQ1)-k*IR1; /* Compute idum=(IA1*idum) % IM1 without */
  if (*idum < 0) *idum += IM1;   /* overflows by Schrage's method */
  k=idum2/IQ2;
  idum2=IA2*(idum2-k*IQ2)-k*IR2; /* Compute idum2=(IA2*idum) % IM2 likewise */
  if (idum2 < 0) idum2 += IM2;
  j=(int)(iy/NDIV);              /* Will be in the range 0...NTAB-1 */
  iy=iv[j]-idum2;                /* Here idum is shuffled, idum and idum2 */
  iv[j] = *idum;                 /* are combined to generate output */
  if (iy < 1) iy += IMM1;
  if ((temp=AM*iy) > RNMX) return RNMX; /* No endpoint values */
  else return temp;
}

#undef IM1
#undef IM2
#undef AM
#undef IMM1
#undef IA1
#undef IA2
#undef IQ1
#undef IQ2
#undef IR1
#undef IR2
#undef NTAB
#undef NDIV
#undef RNMX

/*------------------------------------------------------------------------------
 * ShearingBoxPot: includes vertical gravity if macro VERTICAL_GRAVITY is
 *   defined above.
 */

static Real ShearingBoxPot(const Real x1, const Real x2, const Real x3)
{
  Real phi=0.0;
#ifndef FARGO
  phi -= 1.5*Omega*Omega*x1*x1;
#endif
#ifdef VERTICAL_GRAVITY
  phi += 0.5*Omega*Omega*x3*x3;
#endif /* VERTICAL_GRAVITY */
  return phi;
}

/*------------------------------------------------------------------------------
 * expr_dV2: computes delta(Vy) 
 */

static Real expr_dV2(const Grid *pG, const int i, const int j, const int k)
{
  Real x1,x2,x3;
  cc_pos(pG,i,j,k,&x1,&x2,&x3);
#ifdef FARGO
  return (pG->U[k][j][i].M2/pG->U[k][j][i].d);
#else
  return (pG->U[k][j][i].M2/pG->U[k][j][i].d + 1.5*Omega*x1);
#endif
}

/*------------------------------------------------------------------------------
 * Hydro history variables:
 * hst_rho_Vx_dVy: Reynolds stress, added as history variable.
 * hst_rho_dVy2: KE in y-velocity fluctuations
 * hst_E_total: total energy (including tidal potential).
 */

static Real hst_rho_Vx_dVy(const Grid *pG,const int i,const int j, const int k)
{
  Real x1,x2,x3;
  cc_pos(pG,i,j,k,&x1,&x2,&x3);
#ifdef FARGO
  return pG->U[k][j][i].M1*(pG->U[k][j][i].M2/pG->U[k][j][i].d);
#else
  return pG->U[k][j][i].M1*(pG->U[k][j][i].M2/pG->U[k][j][i].d + 1.5*Omega*x1);
#endif
}

static Real hst_rho_dVy2(const Grid *pG, const int i, const int j, const int k)
{
  Real x1,x2,x3,dVy;
  cc_pos(pG,i,j,k,&x1,&x2,&x3);
#ifdef FARGO
  dVy = (pG->U[k][j][i].M2/pG->U[k][j][i].d);
#else
  dVy = (pG->U[k][j][i].M2/pG->U[k][j][i].d + 1.5*Omega*x1);
#endif
  return pG->U[k][j][i].d*dVy*dVy;
}

#ifdef ADIABATIC
static Real hst_E_total(const Grid *pG, const int i, const int j, const int k)
{
  Real x1,x2,x3,phi;
  cc_pos(pG,i,j,k,&x1,&x2,&x3);
  phi = ShearingBoxPot(x1, x2, x3);

  return pG->U[k][j][i].E + pG->U[k][j][i].d*phi;
}
#endif /* ADIABATIC */

/*------------------------------------------------------------------------------
 * MHD history variables
 * hst_Bx, etc.: Net flux, and Maxwell stress, added as history variables
 */

#ifdef MHD
static Real hst_Bx(const Grid *pG, const int i, const int j, const int k)
{
  return pG->U[k][j][i].B1c;
}

static Real hst_By(const Grid *pG, const int i, const int j, const int k)
{
  return pG->U[k][j][i].B2c;
}

static Real hst_Bz(const Grid *pG, const int i, const int j, const int k)
{
  return pG->U[k][j][i].B3c;
}

static Real hst_BxBy(const Grid *pG, const int i, const int j, const int k)
{
  return -pG->U[k][j][i].B1c*pG->U[k][j][i].B2c;
}

static Real hst_dEw2(const Grid *pG, const int i, const int j, const int k)
{
  Real x1,x2,x3,dVx,dVy,dVz,dBz;
  cc_pos(pG,i,j,k,&x1,&x2,&x3);
  dBz = pG->U[k][j][i].B3c-(sqrt(15.0/16.0))/(2.0*PI)/sqrt(4.*PI);
  dVx = pG->U[k][j][i].M1/pG->U[k][j][i].d;
  dVy = pG->U[k][j][i].M2/pG->U[k][j][i].d + 1.5*Omega*x1;
  dVz = pG->U[k][j][i].M3/pG->U[k][j][i].d;
  
/*  return (dVx*dVx + dVy*dVy + dVz*dVz + pG->U[k][j][i].B1c*pG->U[k][j][i].B1c
    + pG->U[k][j][i].B2c*pG->U[k][j][i].B2c + dBz*dBz); */
  return (pG->U[k][j][i].B1c*pG->U[k][j][i].B1c
    + pG->U[k][j][i].B2c*pG->U[k][j][i].B2c + dBz*dBz); 
}

static Real hst_dBy(const Grid *pG, const int i, const int j, const int k)
{
  double fkx, fky, fkz; /* Fourier kx, ky */
  double dBy;
  Real xmin,xmax,Lx,Ly,Lz,x1,x2,x3;

  xmin = par_getd("grid","x1min");
  xmax = par_getd("grid","x1max");
  Lx = xmax - xmin;

  xmin = par_getd("grid","x2min");
  xmax = par_getd("grid","x2max");
  Ly = xmax - xmin;

  xmin = par_getd("grid","x3min");
  xmax = par_getd("grid","x3max");
  Lz = xmax - xmin;

  fky = 2.0*PI/Ly;
  fkx = -4.0*PI/Lx + 1.5*Omega*fky*pG->time;
  fkz = 2.0*PI/Lz;

/* compute real part of Fourier mode, for comparison to JGG fig 11 */
  cc_pos(pG,i,j,k,&x1,&x2,&x3);
  dBy = 2.0*(pG->U[k][j][i].B2c - (0.2-0.15*Omega*pG->time));
  dBy *= cos(fkx*x1 + fky*x2 + fkz*x3);

  return dBy;
}

#endif /* MHD */

