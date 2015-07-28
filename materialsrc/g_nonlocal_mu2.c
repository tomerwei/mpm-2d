/**
    \file g_nonlocal_mu2.c
    \author Sachith Dunatunga
    \date 10.09.2014

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

/* we need the nodal DOFs to use the node number array */
#include "element.h"
#include "exitcodes.h"

#include <assert.h>

#include "cs_cg.h"

#define jp(x) job->particles[i].x

#undef EMOD
#undef NUMOD

#define dense jp(state[0])
#define gf jp(state[3])
#define eta jp(state[4])
#define gf_local jp(state[5])
#define SZZ_STATE 6
#define szz jp(state[SZZ_STATE])
#define xisq jp(state[7])
#define gammap jp(state[9])
#define gammadotp jp(state[10])

#define MAT_VERSION_STRING "1.0 " __DATE__ " " __TIME__

typedef struct {
    double _sxx;
    double _sxy;
    double _syy;
    double _szz;
} plane_stress_t;

typedef struct p_trial_s {
    double tau_tr;
    double p_tr;

    double t0xx_tr;
    double t0xy_tr;
    double t0yy_tr;
    double t0zz_tr;

    double tau_tau;
    double p_tau;

    double s; // scaling factor
} trial_t;

void calculate_local_values(job_t *job, trial_t *trial_values);

size_t global_node_numbering(job_t *job, size_t physical_nn);

void create_ngf_stiffness_1(cs *triplets, job_t *job, long int *node_map);
void create_ngf_stiffness_2(cs *triplets, job_t *job, long int *node_map, double *ng_loc, double *g1);
void create_gloc_load_from_g(job_t *job, long int *node_map, double *ng_loc, double *ng);

void create_nodal_volumes(job_t *job, long int *node_map, double *volumes_nodal, size_t slda);
void create_nodal_stress_components(job_t *job, long int *node_map, double *sxx_nodal, double *sxy_nodal, double *syy_nodal, double *szz_nodal, double *volumes_nodal, size_t slda);

void calculate_stress(job_t *job);
void solve_diffusion_part(job_t *job, trial_t *trial_values);
void calculate_stress_threaded(threadtask_t *task);

static double E, nu, G, K, lambda;
static double mu_s, mu_2, I_0, rho_s, rho_c, d, A;

double calculate_g_local(double tau, double p);
double calculate_g_local_from_g(double tau_tr, double p_tr, double g, double delta_t);
void map_g_to_particles(job_t *job, const long int *node_map, const double *g, size_t slda);
void cs_solve(cs *triplets, double *load, double *guess, size_t lda);

size_t count_valid_dofs(job_t *job, long int *node_map);

size_t global_node_numbering(job_t *job, size_t physical_nn)
{
    return (job->node_number_override[NODAL_DOF * physical_nn + 0] - 0) / NODAL_DOF;
}

double sum(double *data, size_t n);
double sum(double *data, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) {
        s += data[i];
    }
    return s;
}

double calculate_g_local(double tau, double p)
{
    double g_local = 0;
    const double S0 = mu_s * p;
    if (tau > S0 && p > 0) {
        const double S2 = mu_2 * p;
        const double zeta = I_0 / (d * sqrt(rho_s));

        if (tau >= S2) {
            fprintf(stderr, "\n%g > %g: %g\n", tau, S2, tau / p);
        }
        assert(tau < S2);

        if (tau < S2) {
            g_local = p * sqrt(p) * zeta * (1.0 - S0 / tau) / (S2 - tau);
        } else {
            g_local = 1;
        }

    }
    return g_local;
}

double calculate_xisq(double tau, double p)
{
    const double cap = 15 * d;
    double xisqc = 0;
    if (p > 0) {
        const double S0 = mu_s * p;
        const double S2 = mu_2 * p;

        if (tau > S2) {
            fprintf(stderr, "\n%g > %g: %g\n", tau, S2, tau / p);
            tau = S2;
        }
        assert(tau <= S2);

        if (tau != S0) {
            xisqc  = (fabs(S2 - tau) * A * A * d * d) / (fabs(tau - S0) * (mu_2 - mu_s));
        } else {
            xisqc = cap;
        }

        if (xisqc > cap) {
            xisqc = cap;
        }
    }
    // return xisqc;
    return 1;
}

double calculate_g_local_from_g(double tau_tr, double p_tr, double g, double delta_t)
{
    double g_local = 0;
    const double s = g * G * delta_t + p_tr;
    const double tau_s = mu_s * s;
    const double tau_2 = mu_2 * s;
    if (g >= 0 && p_tr > 0) {
        if (tau_tr > tau_s && tau_tr < tau_2) {
            const double zeta = I_0 / (d * sqrt(rho_s));
            g_local = (sqrt(p_tr) / tau_tr) * zeta * s * (tau_tr - tau_s) / (tau_2 - tau_tr);
        }
        assert(tau_tr < tau_2);
    }
    return g_local;
}

double calculate_g_local_from_g1dg2(double tau_tr, double p_tr, double g1, double dg2, double delta_t);
double calculate_g_local_from_g1dg2(double tau_tr, double p_tr, double g1, double dg2, double delta_t)
{
    double g_local = 0;
    const double g = g1 + dg2;
    const double s = g * G * delta_t + p_tr;
    const double tau_s = mu_s * s;
    const double tau_2 = mu_2 * s;
    const double g_min = ((tau_tr / mu_2) - p_tr) / (G * delta_t);
    if (g >= 0 && p_tr > 0) {
        if (tau_tr > tau_s && tau_tr < tau_2) {
            const double zeta = I_0 / (d * sqrt(rho_s));
            g_local = (sqrt(p_tr) / tau_tr) * zeta * s * (tau_tr - tau_s) / (tau_2 - tau_tr);
        }
        assert(g1 >= g_min);
        if (g < g_min) {
            const double delta_g = 0.5 * (g1 - g_min);
            const double s_h = (g_min + delta_g) * G * delta_t + p_tr;
            const double zeta = I_0 / (d * sqrt(rho_s));
            g_local = (sqrt(p_tr) / tau_tr) * zeta * s_h * (tau_tr - tau_s) / (tau_2 - tau_tr);
        }
    }
    return g_local;
}

double calculate_deriv_g_local_from_g(double tau_tr, double p_tr, double g, double delta_t);
double calculate_deriv_g_local_from_g(double tau_tr, double p_tr, double g, double delta_t)
{
    double dg_localdg = 0;
    if (g >= 0 && tau_tr > 0 && p_tr > 0) {
        const double p_tilde = (p_tr + g * G * delta_t);
        const double mu_g = tau_tr / p_tilde;
        if (mu_g < mu_2 && mu_g > mu_s) {
            const double zeta = I_0 / (d * sqrt(rho_s));
            dg_localdg = zeta * sqrt(p_tr) * ((mu_s * (mu_2 - 2*mu_g) + mu_g * mu_g) / ((mu_2 - mu_g) * (mu_2 - mu_g))) * (G * delta_t) / tau_tr;
        } else if (mu_g < mu_s) {
            dg_localdg = (mu_g / mu_s) * (delta_t * G * I_0 * mu_s * sqrt(p_tr)) / (d * sqrt(rho_s) * (mu_s - 1) * tau_tr);
        } else { 
            dg_localdg = 0;
        }
    }
    return dg_localdg;
}

void trial_step(const particle_t *p, const double dt, trial_t *trial)
{

    const double trD = p->exx_t + p->eyy_t;
    const double dsjxx = lambda * trD + 2.0 * G * p->exx_t + 2 * p->wxy_t * p->sxy;
    const double dsjxy = 2.0 * G * p->exy_t - p->wxy_t * (p->sxx - p->syy);
    const double dsjyy = lambda * trD + 2.0 * G * p->eyy_t - 2 * p->wxy_t * p->sxy;
    const double dsjzz = lambda * trD;

    const double sxx_tr = p->sxx + dt * dsjxx;
    const double sxy_tr = p->sxy + dt * dsjxy;
    const double syy_tr = p->syy + dt * dsjyy;
    const double szz_tr = p->state[SZZ_STATE] + dt * dsjzz;

    const double p_tr = -(sxx_tr + syy_tr + szz_tr) / 3.0;;
    const double t0xx_tr = sxx_tr + p_tr;
    const double t0xy_tr = sxy_tr;
    const double t0yy_tr = syy_tr + p_tr;
    const double t0zz_tr = szz_tr + p_tr;
    const double tau_tr = sqrt(0.5*(t0xx_tr*t0xx_tr + 2*t0xy_tr*t0xy_tr + t0yy_tr*t0yy_tr + t0zz_tr*t0zz_tr));

    trial->tau_tr = tau_tr;
    trial->p_tr = p_tr;
    trial->t0xx_tr = t0xx_tr;
    trial->t0xy_tr = t0xy_tr;
    trial->t0yy_tr = t0yy_tr;
    trial->t0zz_tr = t0zz_tr;

    return;
}

// assume an active, valid particle is passed in to p
// retuns dot(bar(gamma))^p
//double local_step(particle_t *p, double dt, double * restrict tau_tau, double * restrict p_tau, int * restrict flag);
double local_step(const particle_t *p, double dt, trial_t *trial, int *flag);

// flag is set to 0 if the material is open, 1 if it is dense.
double local_step(const particle_t *p, double dt, trial_t *trial, int *flag)
{
    trial_step(p, dt, trial);

    const double rho = (p->m / p->v);
    const double tau_tr = trial->tau_tr;
    const double p_tr = trial->p_tr;

    double nup_tau;
    double tau_tau;
    trial->s = 0;
    if (rho < rho_c || p_tr <= 0) {
        *flag = 0;
        nup_tau = (tau_tr) / (G * dt);
        tau_tau = 0;
    } else if (p_tr > 0) {
        *flag = 1;
        const double S0 = mu_s * p_tr;
        double scale_factor = 1.0;
        tau_tau = tau_tr;
        if (tau_tr > S0) {
            const double S2 = mu_2 * p_tr;
            const double alpha = G * I_0 * dt * sqrt(p_tr / rho_s) / d;
            const double B = S2 + tau_tr + alpha;
            const double H = S2 * tau_tr + S0 * alpha;
            tau_tau = 2.0 * H / (B + sqrt(B * B - 4 * H));
            scale_factor = (tau_tau / tau_tr);
        }

        assert(scale_factor <= 1.0);
        assert(scale_factor > 0);
        nup_tau = tau_tr * (1.0 - scale_factor) / G / dt;
        trial->s = scale_factor;
    } else {
        fprintf(stderr, "u");
        *flag = 0;
        tau_tau = 0;
        nup_tau = 0;
    }
    trial->tau_tau = tau_tau;
    if (p_tr > 0) {
        trial->p_tau = p_tr;
    } else {
        trial->p_tau = 0;
    }
    return nup_tau;
}

void cs_print_to_file(const cs *A)
{
    int m, n, *Ap, *Ai;
    double *Ax ;
    if (!A) { return ; }
    m = A->m ; n = A->n ; Ap = A->p ; Ai = A->i ; Ax = A->x ;
    
    double *A_dense = calloc(sizeof(double), m*n);
    FILE *fp = fopen("matrix.cs", "w");

    for (int j = 0; j < n; j++) {
        for (int p = Ap [j]; p < Ap [j+1]; p++) {
            A_dense[Ai[p]*n + j] = Ax[p];
        }
    }

    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            fprintf(fp, "%lg,", A_dense[i*n + j]);
        }
        fprintf(fp, "\n");
    }

    if (fp != NULL) {
        fclose(fp);
    }
    free(A_dense);

    return;
}

void create_ngf_stiffness_1(cs *triplets, job_t *job, long int *node_map)
{
    /* create 1st stiffness matrix. */
    for (size_t i = 0; i < job->num_particles; i++) {
        // collapsed other conditionals into the dense flag
        if (dense == 0) {
            continue;
        }
        double s[4];
        double grad_s[4][2];

        s[0] = job->h1[i];
        s[1] = job->h2[i];
        s[2] = job->h3[i];
        s[3] = job->h4[i];

        grad_s[0][0] = job->b11[i];
        grad_s[1][0] = job->b12[i];
        grad_s[2][0] = job->b13[i];
        grad_s[3][0] = job->b14[i];

        grad_s[0][1] = job->b21[i];
        grad_s[1][1] = job->b22[i];
        grad_s[2][1] = job->b23[i];
        grad_s[3][1] = job->b24[i];

        const size_t p = job->in_element[i];
        int *nn = job->elements[p].nodes;

        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            for (int ej = 0; ej < NODES_PER_ELEMENT; ej++) {
                const size_t gi = global_node_numbering(job, nn[ei]);
                const size_t gj = global_node_numbering(job, nn[ej]);

                const size_t sgi = node_map[gi];
                const size_t sgj = node_map[gj];

                assert(isfinite(xisq));
                assert(isfinite(job->particles[i].v));

                assert(job->particles[i].v >= 0);
                assert(xisq >= 0);

                const double k_component = job->particles[i].v * (s[ei] * s[ej] +
                        xisq * (grad_s[ei][0]*grad_s[ej][0] + grad_s[ei][1]*grad_s[ej][1]));

                assert(isfinite(k_component));
                
                cs_entry(triplets, sgi, sgj, k_component);
            }
        }
    }

    return;
}

void create_ngf_stiffness_2(cs *triplets, job_t *job, long int *node_map, double *ng_loc, double *g1)
{
    double *ng = g1;

    /* create 1st stiffness matrix. */
    for (size_t i = 0; i < job->num_particles; i++) {
        // collapsed other conditionals into the dense flag
        if (dense == 0) {
            continue;
        }
        trial_t tr;
        trial_step(&(job->particles[i]), job->dt, &tr);

        double s[4];
        double grad_s[4][2];

        s[0] = job->h1[i];
        s[1] = job->h2[i];
        s[2] = job->h3[i];
        s[3] = job->h4[i];

        grad_s[0][0] = job->b11[i];
        grad_s[1][0] = job->b12[i];
        grad_s[2][0] = job->b13[i];
        grad_s[3][0] = job->b14[i];

        grad_s[0][1] = job->b21[i];
        grad_s[1][1] = job->b22[i];
        grad_s[2][1] = job->b23[i];
        grad_s[3][1] = job->b24[i];

        const size_t p = job->in_element[i];
        int *nn = job->elements[p].nodes;

        double reconstructed_g = 0;
        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            reconstructed_g += ng[sgi] * s[ei];
        }

        const double gloc_from_g = calculate_g_local_from_g(tr.tau_tr, tr.p_tr, reconstructed_g, job->dt);
        
        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            const double ng_loc_component = (job->particles[i].v) * gloc_from_g * s[ei];
            if (ng_loc_component < 0) {
                printf("v: %lg\ng_loc^1: %g\n", job->particles[i].v, ng_loc_component);
                printf("reconstructed_g: %g\n", reconstructed_g);
                printf("g_loc_from_g: %g\n", gloc_from_g);
            }
            assert(ng_loc_component >= 0);

            ng_loc[sgi] += ng_loc_component;
        }

        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            for (int ej = 0; ej < NODES_PER_ELEMENT; ej++) {
                const size_t gi = global_node_numbering(job, nn[ei]);
                const size_t gj = global_node_numbering(job, nn[ej]);

                const size_t sgi = node_map[gi];
                const size_t sgj = node_map[gj];

                assert(isfinite(xisq));
                assert(isfinite(job->particles[i].v));
                assert(job->particles[i].v >= 0);
                assert(xisq >= 0);

                const double dglocdg = calculate_deriv_g_local_from_g(tr.tau_tr, tr.p_tr, reconstructed_g, job->dt);
                const double k_component = job->particles[i].v * ((1 - dglocdg) * s[ei] * s[ej] +
                        xisq * (grad_s[ei][0]*grad_s[ej][0] + grad_s[ei][1]*grad_s[ej][1]));

                assert(isfinite(k_component));
                
                cs_entry(triplets, sgi, sgj, k_component);
            }
        }
    }

    return;
}

void create_gloc_load_from_g(job_t *job, long int *node_map, double *ng_loc, double *ng)
{
    for (size_t i = 0; i < job->num_particles; i++) {
        // collapsed other conditionals into the dense flag
        if (dense == 0) {
            continue;
        }
        trial_t tr;
        trial_step(&(job->particles[i]), job->dt, &tr);

        const double s[4] = { job->h1[i], job->h2[i], job->h3[i], job->h4[i] };
        const size_t p = job->in_element[i];
        int *nn = job->elements[p].nodes;

        double reconstructed_g = 0;
        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            reconstructed_g += ng[sgi] * s[ei];
        }

        const double gloc_from_g = calculate_g_local_from_g(tr.tau_tr, tr.p_tr, reconstructed_g, job->dt);
        
        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            const double ng_loc_component = (job->particles[i].v) * gloc_from_g * s[ei];
            if (ng_loc_component < 0) {
                printf("v: %lg\ng_loc^1: %g\n", job->particles[i].v, ng_loc_component);
                printf("reconstructed_g: %g\n", reconstructed_g);
                printf("g_loc_from_g: %g\n", gloc_from_g);
            }
            assert(ng_loc_component >= 0);

            ng_loc[sgi] += ng_loc_component;
        }
    }

    return;
}

void create_gloc_load_from_g1dg2(job_t *job, long int *node_map, double *ng_loc, double *ng1, double *ndg2)
{
    for (size_t i = 0; i < job->num_particles; i++) {
        // collapsed other conditionals into the dense flag
        if (dense == 0) {
            continue;
        }
        trial_t tr;
        trial_step(&(job->particles[i]), job->dt, &tr);

        const double s[4] = { job->h1[i], job->h2[i], job->h3[i], job->h4[i] };
        const size_t p = job->in_element[i];
        int *nn = job->elements[p].nodes;

        double reconstructed_g1 = 0;
        double reconstructed_dg2 = 0;
        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            reconstructed_g1 += ng1[sgi] * s[ei];
            reconstructed_dg2 += ndg2[sgi] * s[ei];
        }

        const double gloc_from_g = calculate_g_local_from_g1dg2(tr.tau_tr, tr.p_tr, reconstructed_g1, reconstructed_dg2, job->dt);
        
        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            const double ng_loc_component = (job->particles[i].v) * gloc_from_g * s[ei];
            if (ng_loc_component < 0) {
                printf("v: %lg\ng_loc^1: %g\n", job->particles[i].v, ng_loc_component);
                printf("reconstructed_g1: %g\n", reconstructed_g1);
                printf("reconstructed_dg2: %g\n", reconstructed_dg2);
                printf("g_loc_from_g: %g\n", gloc_from_g);
            }
            assert(ng_loc_component >= 0);

            ng_loc[sgi] += ng_loc_component;
        }
    }

    return;
}

void create_nodal_volumes(job_t *job, long int *node_map, double *volumes_nodal, size_t slda)
{
    // clear existing values
    for (size_t i = 0; i < slda; i++) {
        volumes_nodal[i] = 0;
    }

    for (size_t i = 0; i < job->num_particles; i++) {
        if (job->active[i] == 0) {
            continue;
        }

        const double s[4] = { job->h1[i], job->h2[i], job->h3[i], job->h4[i] };

        const size_t p = job->in_element[i];
        int *nn = job->elements[p].nodes;

        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];
            const double v_contrib = job->particles[i].v * s[ei];
            volumes_nodal[sgi] += v_contrib;
        }
    }

    return;
}

void create_nodal_stress_components(job_t *job, long int *node_map, double *sxx_nodal, double *sxy_nodal, double *syy_nodal, double *szz_nodal, double *volumes_nodal, size_t slda)
{
    for (size_t i = 0; i < slda; i++) {
        sxx_nodal[i] = 0;
        sxy_nodal[i] = 0;
        syy_nodal[i] = 0;
        szz_nodal[i] = 0;
        volumes_nodal[i] = 0;
    }

    for (size_t i = 0; i < job->num_particles; i++) {
        if (job->active[i] == 0) {
            continue;
        }
        trial_t tr;
        trial_step(&(job->particles[i]), job->dt, &tr);

        const double s[4] = { job->h1[i], job->h2[i], job->h3[i], job->h4[i] };

        const size_t p = job->in_element[i];
        int *nn = job->elements[p].nodes;

        for (int ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];
            const double v_contrib = job->particles[i].v * s[ei];
            const double sxx_tr = tr.t0xx_tr - tr.p_tr;
            const double sxy_tr = tr.t0xy_tr;
            const double syy_tr = tr.t0yy_tr - tr.p_tr;
            const double szz_tr = tr.t0zz_tr - tr.p_tr;
            sxx_nodal[sgi] += v_contrib * sxx_tr;
            sxy_nodal[sgi] += v_contrib * sxy_tr;
            syy_nodal[sgi] += v_contrib * syy_tr;
            szz_nodal[sgi] += v_contrib * szz_tr;
            volumes_nodal[sgi] += v_contrib;
        }
    }

    const double volume_tolerance = 0;
    for (size_t i = 0; i < slda; i++) {
        if (volumes_nodal[i] > volume_tolerance) {
            sxx_nodal[i] /= volumes_nodal[i];
            sxy_nodal[i] /= volumes_nodal[i];
            syy_nodal[i] /= volumes_nodal[i];
            szz_nodal[i] /= volumes_nodal[i];
        }
    }

    return;
}

/*----------------------------------------------------------------------------*/
void material_init(job_t *job)
{
    for (size_t i = 0; i < job->num_particles; i++) {
        for (size_t j = 0; j < DEPVAR; j++) {
            job->particles[i].state[j] = 0;
        }
    }

    for (size_t i = 0; i < job->num_particles; i++) {
        if ((job->particles[i].m / job->particles[i].v) > rho_c) {
            dense = 1;
        } else {
            dense = 0;
        }
        eta = 0;
        gammap = 0;
        gammadotp = 0;
        gf = 0;
        gf_local = 0;
        xisq = 0;
        szz = 0.5 * (job->particles[i].sxx + job->particles[i].syy); // ugly hack
    }

    if (job->material.num_fp64_props < 9) {
        fprintf(stderr,
            "%s:%s: Need at least 9 properties defined (%s).\n",
            "E, nu, mu_s, mu_2, I_0, rho_s, rho_c, d, A",
            __FILE__, __func__);
        exit(EXIT_ERROR_MATERIAL_FILE);
    } else {
        E = job->material.fp64_props[0];
        nu = job->material.fp64_props[1];
        mu_s = job->material.fp64_props[2];
        mu_2 = job->material.fp64_props[3];
        I_0 = job->material.fp64_props[4];
        rho_s = job->material.fp64_props[5];
        rho_c = job->material.fp64_props[6];
        d = job->material.fp64_props[7];
        A = job->material.fp64_props[8];
        G = E / (2.0 * (1.0 + nu));
        K = E / (3.0 * (1.0 - 2*nu));
        lambda = K - 2.0 * G / 3.0;
        printf("%s:%s: properties (E = %g, nu = %g, G = %g, K = %g, mu_s = %g,"
            " mu_2 = %g, I_0 = %g, rho_s = %g, rho_c = %g, d = %g, A = %g).\n",
            __FILE__, __func__, E, nu, G, K, mu_s, mu_2, I_0, rho_s, rho_c, d, A);
    }

    printf("%s:%s: (material version %s) done initializing material.\n",
        __FILE__,  __func__, MAT_VERSION_STRING);
    return;
}
/*----------------------------------------------------------------------------*/

/* nonlocal granular fluidity model. */
void calculate_stress_threaded(threadtask_t *task)
{
    job_t *job = task->job;

    /* Only one of the threads can compute the nonlocal solution. */
    if (task->id == 0) {
        calculate_stress(job);
    }

    return;
}

/*----------------------------------------------------------------------------*/
void calculate_stress(job_t *job)
{
    trial_t *trial_values = calloc(job->num_particles, sizeof(trial_t));

    // populates the trial structure with the local values
    // density flag in the actual particle is set appropriately.
    calculate_local_values(job, trial_values);

    /* g_local is calculated when we create the stiffness matrix. */
    /* build FEM diffusion array/load vector and solve for g_nonlocal */
    solve_diffusion_part(job, trial_values);

    for (size_t i = 0; i < job->num_particles; i++) {
        if (job->active[i] == 0) {
            continue;
        }

        const double p_tr = trial_values[i].p_tr;
        const double t0xx_tr = trial_values[i].t0xx_tr;
        const double t0xy_tr = trial_values[i].t0xy_tr;
        const double t0yy_tr = trial_values[i].t0yy_tr;
        const double t0zz_tr = trial_values[i].t0zz_tr;
        const double tau_tr = trial_values[i].tau_tr;

        double nup_tau = 0;
        if (dense) {
            // const double scale_factor = p_tr / (G * job->dt * gf + p_tr);
            const double scale_factor = trial_values[i].tau_tau / trial_values[i].tau_tr;
            const double tau_tau = tau_tr * scale_factor;

            nup_tau = ((tau_tr - tau_tau) / G) / job->dt;

            job->particles[i].sxx = scale_factor * t0xx_tr - p_tr;
            job->particles[i].sxy = scale_factor * t0xy_tr;
            job->particles[i].syy = scale_factor * t0yy_tr - p_tr;
            job->particles[i].state[SZZ_STATE] = scale_factor * t0zz_tr - p_tr;
        } else {
            // local step should already take care of this.
            nup_tau = (tau_tr / G) / job->dt;
            job->particles[i].sxx = 0;
            job->particles[i].sxy = 0;
            job->particles[i].syy = 0;
            job->particles[i].state[SZZ_STATE] = 0;
        }

        /* use strain rate to calculate stress increment */
        gammap += nup_tau * job->dt;
        gammadotp = nup_tau;
    }

    free(trial_values);

    return;
}
/*----------------------------------------------------------------------------*/

void initialize_g_loc_from_local(job_t *job, long int *node_map, trial_t *trial_values, double *nodal_g_loc, double *nodal_volume);
void initialize_g_loc_from_local(job_t *job, long int *node_map, trial_t *trial_values, double *nodal_g_loc, double *nodal_volume)
{
    // initial g_loc from local step
    for (size_t i = 0; i < job->num_particles; i++) {
        if (job->active[i] == 0) {
            dense = 0;
            continue; 
        }
        const int p = job->in_element[i];
        if (p == -1) {
            dense = 0;
            continue;
        }
        const int *nn = job->elements[p].nodes;
        if (dense) {
            xisq = calculate_xisq(trial_values[i].tau_tau, trial_values[i].p_tau);
            const double g_loc = calculate_g_local(trial_values[i].tau_tau, trial_values[i].p_tau);
            const double s[4] = { job->h1[i], job->h2[i], job->h3[i], job->h4[i] };

            assert(isfinite(gf_local));
            assert(isfinite(xisq));
            
            for (size_t ei = 0; ei < NODES_PER_ELEMENT; ei++) {
                const size_t gi = global_node_numbering(job, nn[ei]);
                const size_t sgi = node_map[gi];

                const double v_contrib = job->particles[i].v * s[ei];
                const double ng_loc_component = g_loc * v_contrib;
                if (ng_loc_component < 0) {
                    printf("v: %g g_loc^1: %g\n", job->particles[i].v, ng_loc_component);
                }
                assert(ng_loc_component >= 0);

                nodal_g_loc[sgi] += ng_loc_component;
                nodal_volume[sgi] += v_contrib;
            }
        } else {
            // set stresses to 0
            job->particles[i].sxx = 0;
            job->particles[i].sxy = 0;
            job->particles[i].syy = 0;
            job->particles[i].state[SZZ_STATE] = 0;
        }
    }
}

/*----------------------------------------------------------------------------*/
void solve_diffusion_part(job_t *job, trial_t *trial_values)
{
    double *ng_loc = calloc(job->num_nodes, sizeof(double));
    double *ng = calloc(job->num_nodes, sizeof(double));
    double *nv = calloc(job->num_nodes, sizeof(double));

    double *current_tau_values = calloc(job->num_particles, sizeof(double));
    double *minimum_tau_values = calloc(job->num_particles, sizeof(double));
    double *maximum_tau_values = calloc(job->num_particles, sizeof(double));

    for (size_t i = 0; i < job->num_particles; i++) {
        minimum_tau_values[i] = 0;
        maximum_tau_values[i] = trial_values[i].p_tr * mu_2;
        current_tau_values[i] = trial_values[i].tau_tau;
        // printf("TAU[%zu]: [%g, %g, %g]\n", i, minimum_tau_values[i], maximum_tau_values[i], current_tau_values[i]);
    }

    /* initialize node level fluidity arrays */
    for (size_t i = 0; i < job->num_nodes; i++) {
        ng[i] = 0;
        ng_loc[i] = 0;
        // clear volumes
        nv[i] = 0;
    }

    /* get number of dofs and initialize mapping arrays */
    long int *node_map = malloc(job->num_nodes * sizeof(long int));
    const size_t slda = count_valid_dofs(job, node_map);

    if (slda == 0) {
        goto diffusion_dealloc;
    }

    double *f = calloc(sizeof(double), slda);

    /*  calculate number of nonzero elements (before summing duplicates). */
    size_t nnz = 0;
    for (size_t i = 0; i < job->num_particles; i++) {
        if (dense) {
            nnz += (NODES_PER_ELEMENT * NODES_PER_ELEMENT) * 1;
        }
    }
    nnz += slda;

    // use local algorithm to calculate initial guesses
    initialize_g_loc_from_local(job, node_map, trial_values, ng_loc, nv);

    const double sum_g_local = sum(ng_loc, slda);
    printf("sum(g_local) = %g\n", sum_g_local);
    if (sum_g_local < 1e-12) {
        for (size_t i = 0; i < job->num_particles; i++) {
            gf = 0;
        }
        printf("%zu: Skipping matrix solve.\n", job->stepcount);
    } else {
        double rel_error = 1;
        const double max_rel_error = 1e-5;
        int inner_iterations = 0;
        const int max_inner_iterations = 8;

        do {
            /* slda contains degrees of freedom of new matrix */
            cs *triplets = cs_spalloc(slda, slda, nnz, 1, 1);
            cs *triplets_hat = cs_spalloc(slda, slda, nnz, 1, 1); 

            double *gloc1 = calloc(slda, sizeof(double));

            for (size_t i = 0; i < slda; i++) {
                f[i] = ng_loc[i]; // copy load for solution
                gloc1[i] = ng_loc[i];
            }

            printf("II=%d\n", inner_iterations);
            for (size_t i = 0; i < slda; i++) {
                assert(f[i] >= 0);
            }

            create_ngf_stiffness_1(triplets, job, node_map);
            cs_solve(triplets, f, ng_loc, slda);

            const double neg_g_tol = -1e-10;
            for (size_t i = 0; i < slda; i++) {
                ng[i] = f[i]; // solution is g1, save it as ng
                assert(f[i] >= neg_g_tol);
                if (f[i] < 0 && f[i] > neg_g_tol) {
                    ng[i] = 0;
                }
            }

            for (size_t i = 0; i < slda; i++) {
                if (ng[i] < 0) {
                    fprintf(stderr, "g1[%zu] = %g\n", i, ng[i]);
                    for (size_t j = 0; j < slda; j++) {
                        fprintf(stderr, "f[%zu] = %g\n", j, ng_loc[j]);
                    }
                    cs *A = cs_compress(triplets);
                    cs_dupl(A);
                    cs_print_to_file(A);
                    FILE *ff = fopen("load.cs", "w");
                    for (size_t j = 0; j < slda; j++) {
                        fprintf(ff, "%lg\n", ng_loc[j]);
                    }
                    fclose(ff);
                    cs_spfree(A);
                }
                assert(ng[i] >= 0);
            }

            // gf state variable of each particle is projection of g field
            map_g_to_particles(job, node_map, ng, slda);

            double rtr = 0;
            FILE *fout = fopen("delta_tau.csv", "w");
            FILE *fout2 = fopen("gammadotp.csv", "w");
            for (size_t i = 0; i < job->num_particles; i++) {
                if (dense == 0) {
                    continue;
                }

                const double reconstructed_g = gf;
                const double tau_elastic = current_tau_values[i];
                const double gammadotbarp = (trial_values[i].tau_tr - tau_elastic) / (G * job->dt);
                const double tau_plastic = trial_values[i].p_tau * (trial_values[i].tau_tr - tau_elastic) / (G * job->dt * reconstructed_g);

                const double max_step_fraction = 0.05;
                const double delta_tau = tau_plastic - tau_elastic;

                fprintf(fout, "%g %g %g %g %g\n", job->particles[i].x, job->particles[i].y, delta_tau, delta_tau / tau_elastic, gammadotbarp);

                rtr += delta_tau * delta_tau / (tau_elastic * tau_elastic);
                if (delta_tau > 0) {
                    // plastic stress larger than elastic; decrease D^p (increase current tau)
                    const double max_delta_tau_neg = current_tau_values[i];
                } else {
                    // elastic stress larger than plastic; increase D^p (decrease current tau)
                }
                // printf("[%zu]: %g\n", i, tau_plastic);
            }
            fclose(fout);
            fclose(fout2);

            const double rel_error = sqrt(rtr / job->num_particles);
            const double rgloc2 = 1;

            inner_iterations++;
            printf("%d: %d %g %zu %g\n", job->stepcount, inner_iterations, rtr, slda, rel_error);

            cs_spfree(triplets);
            cs_spfree(triplets_hat);

            free(gloc1);

        } while ((inner_iterations < max_inner_iterations) && (rel_error > max_rel_error));

    }

    map_g_to_particles(job, node_map, ng, slda);
    free(f);

diffusion_dealloc:
    free(node_map);
    free(ng);
    free(ng_loc);
    free(nv);
    free(current_tau_values);
    free(minimum_tau_values);
    free(maximum_tau_values);

    return;
}
/*----------------------------------------------------------------------------*/

void map_g_to_particles(job_t *job, const long int *node_map, const double *g, size_t slda)
{
    /* map nodal g_nonlocal back to particles */
    for (size_t i = 0; i < job->num_particles; i++) {
        if (job->active[i] == 0) {
            continue;
        }

        if (dense == 0) {
            continue;
        }

        const int p = job->in_element[i];
        if (p == -1) {
            continue;
        }

        const double s[4] = { job->h1[i], job->h2[i], job->h3[i], job->h4[i] };

        gf = 0;
        const int *nn = job->elements[p].nodes;

        for (size_t ei = 0; ei < NODES_PER_ELEMENT; ei++) {
            const size_t gi = global_node_numbering(job, nn[ei]);
            const size_t sgi = node_map[gi];

            gf += g[sgi] * s[ei];
            // gf += g[sgi] * 0.25;
        }

        if (gf < 0) {
            fprintf(stderr, "%zu: %lg\n", i, gf);
            // cs_print_to_file(smat);
            FILE *ff = fopen("g_nodes.cs", "w");
            for (size_t j = 0; j < slda; j++) {
                fprintf(ff, "%lg\n", g[j]);
            }
            fclose(ff);
            gf = 0;
        }
        assert(gf >= 0);
    }

    return;
}

void cs_solve(cs *triplets, double *load, double *guess, size_t lda)
{
    /* create compressed sparse matrix */
    cs *A = cs_compress(triplets);
    cs_dupl(A);

#define DIRECT_SOLVE
#ifndef DIRECT_SOLVE
//    fprintf(stderr, "%d by %d\n", smat_hat->m, smat_hat->n); // print out matrix for debugging
    if (!cs_bicgstab(A, load, guess, 1e-13)) {
        fprintf(stderr, "cg error!\n");
        //cs_print(smat_hat, 0); // print out matrix for debugging
        cs_print_to_file(A);
        FILE *ff = fopen("load.cs", "w");
        for (size_t j = 0; j < lda; j++) {
            fprintf(ff, "%lg\n", load[j]);
        }
        fclose(ff);
        exit(EXIT_ERROR_CS_SOL);
    }
#else
//    fprintf(stderr, "%d by %d\n", smat_hat->m, smat_hat->n); // print out matrix for debugging
    if (!cs_lusol(1, A, load, 1e-16)) {
        fprintf(stderr, "lusol error!\n");
        if (cs_qrsol(1, A, load)) {
            fprintf(stderr, "qrsol error!\n");
            //cs_print(smat_hat, 0); // print out matrix for debugging
            cs_print_to_file(A);
            exit(EXIT_ERROR_CS_SOL);
        }
    }
#endif
#undef DIRECT_SOLVE

    cs_spfree(A);
    return;
}

size_t count_valid_dofs(job_t *job, long int *node_map)
{
    int *num_dense_particles_near_node = calloc(job->num_nodes, sizeof(int));

    for (size_t i = 0; i < job->num_particles; i++) {
        if (job->active[i] == 0) {
            continue;
        }

        if (dense == 0) {
            continue;
        }

        const int p = job->in_element[i];
        if (p == -1) {
            continue;
        }

        const int *nn = job->elements[p].nodes;

        for (size_t j = 0; j < NODES_PER_ELEMENT; j++) {
            num_dense_particles_near_node[nn[j]]++;
        }
    }

    size_t slda = 0;
    for (size_t i = 0; i < job->num_nodes; i++) {
        node_map[i] = -1;
    }

    for (size_t i = 0; i < job->num_nodes; i++) {
        const size_t i_new = global_node_numbering(job, i);

        if (node_map[i_new] != -1) {
            continue;
        }

        if (num_dense_particles_near_node[i] > 0) {
            node_map[i_new] = slda;
            slda++;
        }

    }

    free(num_dense_particles_near_node);

    return slda;
}

void calculate_local_values(job_t *job, trial_t *trial_values)
{
    for (size_t i = 0; i < job->num_particles; i++) {
        // trial_step(&(job->particles[i]), job->dt, &(trial_values[i]));
        int flag = 0;
        local_step(&(job->particles[i]), job->dt, &(trial_values[i]), &flag);
        dense = flag;
    }

    return;
}
