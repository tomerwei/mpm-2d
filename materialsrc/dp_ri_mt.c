/**
    \file dp_ri_mt.c
    \author Sachith Dunatunga
    \date 04.12.13

    mpm_2d -- An implementation of the Material Point Method in 2D.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "interpolate.h"
#include "particle.h"
#include "node.h"
#include "process.h"
#include "material.h"
#include "exitcodes.h"

#define jp(x) job->particles[i].x

#undef EMOD
#undef NUMOD

#undef G
#undef K

#define szz jp(state[1])
#define gammap jp(state[9])
#define gammadotp jp(state[10])

#define MAT_VERSION_STRING "1.0 " __DATE__ " " __TIME__

void calculate_stress(job_t *job);
void calculate_stress_threaded(threadtask_t *task);

/*
    The Young's Modulus (E) and Poisson ratio (nu), set in the material init
    procedure. These are Ccpies of the properties given in the
    configuration file. Shear modulus (G) and bulk modulus (K) are derived from
    these.
*/
static double E;
static double nu;
static double G;
static double K;
static double mu_s;

/*----------------------------------------------------------------------------*/
void material_init(job_t *job)
{
    int i, j;

    for (i = 0; i < job->num_particles; i++) {
        for (j = 0; j < DEPVAR; j++) {
            job->particles[i].state[j] = 0;
        }
    }

    for (i = 0; i < job->num_particles; i++) {
        szz = 0;
        gammap = 0;
        gammadotp = 0;
    }

    if (job->material.num_fp64_props < 3) {
        fprintf(stderr,
            "%s:%s: Need at least 3 properties defined (E, nu, mu_s).\n",
            __FILE__, __func__);
        exit(EXIT_ERROR_MATERIAL_FILE);
    } else {
        E = job->material.fp64_props[0];
        nu = job->material.fp64_props[1];
        mu_s = job->material.fp64_props[2];
        G = E / (2.0 * (1.0 + nu));
        K = E / (3.0 * (1.0 - 2*nu));
        printf("%s:%s: properties (E = %g, nu = %g, G = %g, K = %g, mu_s = %g).\n",
            __FILE__, __func__, E, nu, G , K, mu_s);
    }

    printf("%s:%s: (material version %s) done initializing material.\n",
        __FILE__,  __func__, MAT_VERSION_STRING);
    return;
}
/*----------------------------------------------------------------------------*/

/* Local granular fluidity model. */
void calculate_stress(job_t *job)
{
    threadtask_t t;
    t.job = job;
    t.offset = 0;
    t.blocksize = job->num_particles;
    calculate_stress_threaded(&t);
    return;
}

/*----------------------------------------------------------------------------*/
void calculate_stress_threaded(threadtask_t *task)
{
    job_t *job = task->job;

    /* Since this is local, we can split the particles among the threads. */
    size_t p_start = task->offset;
    size_t p_stop = task->offset + task->blocksize;

    /* value at end of timestep */
    double tau_tau;
    double scale_factor;

    /* increment using jaumann rate */
    double dsjxx, dsjxy, dsjyy;

    /* trial values */
    double sxx_tr, sxy_tr, syy_tr, szz_tr;
    double t0xx_tr, t0xy_tr, t0yy_tr, t0zz_tr;
    double p_tr, tau_tr;

    double nup_tau;

    double const c = 0;

    int density_flag;
    
    size_t i;

    double trD;
    const double lambda = K - 2.0 * G / 3.0;

    double S0;

/*    fprintf(stderr, "processing particle ids [%zu %zu].\n", p_start, p_stop);*/

    for (i = p_start; i < p_stop; i++) {
        if (job->active[i] == 0) {
            continue;
        }

        /* Calculate tau and p trial values. */
        trD = job->particles[i].exx_t + job->particles[i].eyy_t;
        dsjxx = lambda * trD + 2.0 * G * job->particles[i].exx_t;
        dsjxy = 2.0 * G * job->particles[i].exy_t;
        dsjyy = lambda * trD + 2.0 * G * job->particles[i].eyy_t;
        dsjxx += 2 * job->particles[i].wxy_t * job->particles[i].sxy;
        dsjxy -= job->particles[i].wxy_t * (job->particles[i].sxx - job->particles[i].syy);
        dsjyy -= 2 * job->particles[i].wxy_t * job->particles[i].sxy;

        sxx_tr = job->particles[i].sxx + job->dt * dsjxx;
        sxy_tr = job->particles[i].sxy + job->dt * dsjxy;
        syy_tr = job->particles[i].syy + job->dt * dsjyy;
        szz_tr = szz + job->dt * lambda * trD;

        p_tr = -(sxx_tr + syy_tr + szz_tr) / 3.0;
        t0xx_tr = sxx_tr + p_tr;
        t0xy_tr = sxy_tr;
        t0yy_tr = syy_tr + p_tr;
        t0zz_tr = szz_tr + p_tr; 
        tau_tr = sqrt(0.5*(t0xx_tr*t0xx_tr + 2*t0xy_tr*t0xy_tr + t0yy_tr*t0yy_tr + t0zz_tr*t0zz_tr));

        if ((job->particles[i].m / job->particles[i].v) < 1485.0f) {
            density_flag = 1;
/*            printf("%4d: density %lf\n", i, (job->particles[i].m / job->particles[i].v));*/
        } else {
            density_flag = 0;
        }

        if (density_flag || p_tr <= c) {
            nup_tau = (tau_tr) / (G * job->dt);

            job->particles[i].sxx = 0;
            job->particles[i].sxy = 0;
            job->particles[i].syy = 0;
            szz = 0;
        } else if (p_tr > c) {
            S0 = mu_s * p_tr;
            if (tau_tr <= S0) {
                tau_tau = tau_tr;
                scale_factor = 1.0;
            } else {
                tau_tau = S0;
                scale_factor = (tau_tau / tau_tr);
            }

            nup_tau = ((tau_tr - tau_tau) / G) / job->dt;

            job->particles[i].sxx = scale_factor * t0xx_tr - p_tr;
            job->particles[i].sxy = scale_factor * t0xy_tr;
            job->particles[i].syy = scale_factor * t0yy_tr - p_tr;
            szz = scale_factor * t0zz_tr - p_tr;
        } else {
/*            fprintf(stderr, "u %zu %3.3g %3.3g %d ", i, f, p_tr, density_flag);*/
            fprintf(stderr, "u"); 
            nup_tau = 0;
        }

        /* use strain rate to calculate stress increment */
        gammap += nup_tau * job->dt;
        gammadotp = nup_tau;
    }

    return;
}
/*----------------------------------------------------------------------------*/

