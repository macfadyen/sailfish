#define DG_SOLVER
#include <stdio.h>
#include <math.h>
#include "../sailfish.h"

// ============================ COMPAT ========================================
// ============================================================================
#ifdef __ROCM__
#include <hip/hip_runtime.h>
#endif

#if !defined(__NVCC__) && !defined(__ROCM__)
#define __device__
#define __host__
#define EXTERN_C
#else
#define EXTERN_C extern "C"
#endif

#define ADIABATIC_GAMMA (5.0 / 3.0)
#define NCONS 4
#define NUM_GUARD 1
// #define MAX_NUM_FIELDS 60 // this is NCONS * MAX_POLYNOMIALS
#define MAX_NUM_FIELDS 24

// ============================ MATH ==========================================
// ============================================================================
#define real double
#define min2(a, b) ((a) < (b) ? (a) : (b))
#define max2(a, b) ((a) > (b) ? (a) : (b))
#define min3(a, b, c) min2(a, min2(b, c))
#define max3(a, b, c) max2(a, max2(b, c))
#define sign(x) copysign(1.0, x)
#define minabs(a, b, c) min3(fabs(a), fabs(b), fabs(c))

// ============================ HYDRO =========================================
// ============================================================================

static __host__ __device__ int num_polynomials(struct Cell cell)
{
    switch (cell.order)
    {
        case 1: return 1;
        case 2: return 3;
        case 3: return 6;
        case 4: return 10;
        case 5: return 15;
        default: return 0;
    }
}

static __host__ __device__ int num_quadrature_points(struct Cell cell)
{
    return cell.order * cell.order;
}

static __host__ __device__ void conserved_to_primitive(const real *cons, real *prim)
{
    const real rho    = cons[0];
    const real px     = cons[1];
    const real py     = cons[2];
    const real energy = cons[3];

    const real vx = px / rho;
    const real vy = py / rho;
    const real kinetic_energy = 0.5 * rho * (vx * vx + vy * vy);
    const real thermal_energy = energy - kinetic_energy;
    const real pressure = thermal_energy * (ADIABATIC_GAMMA - 1.0);

    prim[0] = rho;
    prim[1] = vx;
    prim[2] = vy;
    prim[3] = pressure;
}

static __device__ __host__ void primitive_to_conserved(const real *prim, real *cons)
{
    const real rho      = prim[0];
    const real vx       = prim[1];
    const real vy       = prim[2];
    const real pressure = prim[3];

    const real px = vx * rho;
    const real py = vy * rho;
    const real kinetic_energy = 0.5 * rho * (vx * vx + vy * vy);
    const real thermal_energy = pressure / (ADIABATIC_GAMMA - 1.0);

    cons[0] = rho;
    cons[1] = px;
    cons[2] = py;
    cons[3] = kinetic_energy + thermal_energy;
}

static __host__ __device__ real primitive_to_velocity_component(const real *prim, int direction)
{
    switch (direction)
    {
        case 0: return prim[1];
        case 1: return prim[2];
        default: return 0.0;
    }
}

static __host__ __device__ void primitive_to_flux(
    const real *prim,
    const real *cons,
    real *flux,
    int direction)
{
    real vn = primitive_to_velocity_component(prim, direction);
    real pressure = prim[3];

    flux[0] = vn * cons[0];
    flux[1] = vn * cons[1] + pressure * (direction == 0);
    flux[2] = vn * cons[2] + pressure * (direction == 1);
    flux[3] = vn * cons[3] + pressure * vn;
}

static __host__ __device__ real primitive_to_sound_speed_squared(const real *prim)
{
    const real rho = prim[0];
    const real pressure = prim[3];
    return ADIABATIC_GAMMA * pressure / rho;
}

static __host__ __device__ void primitive_to_outer_wavespeeds(
    const real *prim,
    real *wavespeeds,
    int direction)
{
    const real cs = sqrt(primitive_to_sound_speed_squared(prim));
    real vn = primitive_to_velocity_component(prim, direction);
    wavespeeds[0] = vn - cs;
    wavespeeds[1] = vn + cs;
}

static __host__ __device__ real primitive_max_wavespeed(const real *prim)
{
    real cs = sqrt(primitive_to_sound_speed_squared(prim));
    real vx = prim[1];
    real vy = prim[2];
    real ax = max2(fabs(vx - cs), fabs(vx + cs));
    real ay = max2(fabs(vy - cs), fabs(vy + cs));
    return max2(ax, ay);
}

static __host__ __device__ void riemann_hlle(const real *pl, const real *pr, real *flux, int direction)
{
    real ul[4];
    real ur[4];
    real fl[4];
    real fr[4];
    real al[2];
    real ar[2];

    primitive_to_conserved(pl, ul);
    primitive_to_conserved(pr, ur);
    primitive_to_flux(pl, ul, fl, direction);
    primitive_to_flux(pr, ur, fr, direction);
    primitive_to_outer_wavespeeds(pl, al, direction);
    primitive_to_outer_wavespeeds(pr, ar, direction);

    const real am = min2(0.0, min2(al[0], ar[0]));
    const real ap = max2(0.0, max2(al[1], ar[1]));

    for (int q = 0; q < NCONS; ++q)
    {
        flux[q] = (fl[q] * ap - fr[q] * am - (ul[q] - ur[q]) * ap * am) / (ap - am);
    }
}

// static __host__ __device__ void riemann_hllc(const real *pl, const real *pr, real *flux, int direction)
// {
//     enum { d, px, py, e }; // Conserved
//     enum { rho, vx, vy, p }; // Primitive

//     real ul[NCONS];
//     real ur[NCONS];
//     real ulstar[NCONS];
//     real urstar[NCONS];
//     real fl[NCONS];
//     real fr[NCONS];
//     real al[2];
//     real ar[2];

//     const real vnl = primitive_to_velocity_component(pl, direction);
//     const real vnr = primitive_to_velocity_component(pr, direction);

//     primitive_to_conserved(pl, ul);
//     primitive_to_conserved(pr, ur);
//     primitive_to_flux(pl, ul, fl, direction);
//     primitive_to_flux(pr, ur, fr, direction);
//     primitive_to_outer_wavespeeds(pl, al, direction);
//     primitive_to_outer_wavespeeds(pr, ar, direction);

//     const real am = min3(0.0, al[0], ar[0]);
//     const real ap = max3(0.0, al[1], ar[1]);

//     REVIEW THE FORMULAS BELOW (maybe typos?):
//     
//     real lc = (
//         + (pr[p] - pr[rho] * vnr * (ap - vnr))
//         - (pl[p] - pl[rho] * vnl * (am - vnl))) / (pl[rho] * (am - vnl) - pr[rho] * (ap - vnr));

//     real ffl = pl[rho] * (am - vnl) / (am - lc);
//     real ffr = pr[rho] * (ap - vnr) / (ap - lc);

//     ulstar[d] = ffl;
//     ulstar[e] = ffl * (ul[e] / pl[rho] + (lc - vnl) * (lc + pl[p] / (pl[rho] * (am - vnl))));
//     ulstar[px] = ffl * ((lc - vnl) * (direction == 0) + pl[vx]);
//     ulstar[py] = ffl * ((lc - vnl) * (direction == 1) + pl[vy]);

//     urstar[d] = ffr;
//     urstar[e] = ffr * (ur[e] / pr[rho] + (lc - vnl) * (lc + pr[p] / (pr[rho] * (ap - vnl))));
//     urstar[px] = ffr * ((lc - vnl) * (direction == 0) + pl[vx]);
//     urstar[py] = ffr * ((lc - vnl) * (direction == 1) + pl[vy]);

//     const real s = 0.0; // stationary face s = x / t

//     if      (s  <= am)          for (int i = 0; i < NCONS; ++i) flux[i] = fl[i];
//     else if (am < s && s <= lc) for (int i = 0; i < NCONS; ++i) flux[i] = fl[i] + am * (ulstar[i] - ul[i]);
//     else if (lc < s && s <= ap) for (int i = 0; i < NCONS; ++i) flux[i] = fr[i] + ap * (urstar[i] - ur[i]);
//     else if (ap < s)            for (int i = 0; i < NCONS; ++i) flux[i] = fr[i];
// }

static __host__ __device__ real minmodTVB(real w1, real w0l, real w0, real w0r, real dl)
{
    real BETA_TVB = 1.0;

    real a = w1 * sqrt(3.0);
    real b = (w0 - w0l) * BETA_TVB;
    real c = (w0r - w0) * BETA_TVB;

    const real M = 10.0; //Cockburn & Shu, JCP 141, 199 (1998) eq. 3.7 suggest M~50.0

    if (fabs(a) <= M * dl * dl)
    {        
        return w1;
    }
    else
    {
        real x1 = fabs(sign(a) + sign(b)) * (sign(a) + sign(c));
        real x2 = minabs(a, b, c);
        real x = (0.25 / sqrt(3.0)) * x1 * x2;

        return x;
    }
}


static __host__ __device__ real minmodB(real a, real b, real c, real dl)
{
    const real M = 1.0; //SAVE Cockburn & Shu, JCP 141, 199 (1998) eq. 3.7 suggest M~50.0

    if (fabs(a) <= M * dl * dl)
    {        
        return a;
    }
    else
    {
        real x1 = fabs(sign(a) + sign(b)) * (sign(a) + sign(c));
        real x2 = minabs(a, b, c);
        real x  = 0.25 * x1 * x2;
    
        return x;
    }
}

// ============================ PATCH =========================================
// ============================================================================
#define FOR_EACH(p, ng) \
    for (int i = p.start[0] + ng; i < p.start[0] + p.count[0] - ng; ++i) \
    for (int j = p.start[1] + ng; j < p.start[1] + p.count[1] - ng; ++j)
#define FOR_EACH_OMP(p, ng) \
_Pragma("omp parallel for") \
    for (int i = p.start[0] + ng; i < p.start[0] + p.count[0] - ng; ++i) \
    for (int j = p.start[1] + ng; j < p.start[1] + p.count[1] - ng; ++j)
#define GET(p, i, j) (p.data + p.jumps[0] * ((i) - p.start[0]) + p.jumps[1] * ((j) - p.start[1]))

struct Patch
{
    int start[2];
    int count[2];
    int jumps[2];
    int num_fields;
    real *data;
};

static struct Patch patch(struct Mesh mesh, int num_fields, int num_guard, real *data)
{
    struct Patch patch;
    patch.start[0] = -num_guard;
    patch.start[1] = -num_guard;
    patch.count[0] = mesh.ni + 2 * num_guard;
    patch.count[1] = mesh.nj + 2 * num_guard;
    patch.jumps[0] = num_fields * patch.count[1];
    patch.jumps[1] = num_fields;
    patch.num_fields = num_fields;
    patch.data = data;
    return patch;
}

// ============================ SCHEME ========================================
// ============================================================================

static __host__ __device__ void advance_rk_zone_dg(
    struct Cell cell,
    struct Mesh mesh,
    struct Patch weights_rd,
    struct Patch weights_wr,
    real dt,
    int i,
    int j)
{
    real dx = mesh.dx;
    //real dy = mesh.dy;

    int n_quad = num_quadrature_points(cell);
    int n_poly = num_polynomials(cell);
    int n_face = cell.order;

    real *wij = GET(weights_rd, i, j);
    real *wli = GET(weights_rd, i - 1, j);
    real *wri = GET(weights_rd, i + 1, j);
    real *wlj = GET(weights_rd, i, j - 1);
    real *wrj = GET(weights_rd, i, j + 1);

    real ulip[NCONS];
    real ulim[NCONS];
    real urip[NCONS];
    real urim[NCONS];
    real uljp[NCONS];
    real uljm[NCONS];
    real urjp[NCONS];
    real urjm[NCONS];

    real plip[NCONS];
    real plim[NCONS];
    real prip[NCONS];
    real prim[NCONS];
    real pljp[NCONS];
    real pljm[NCONS];
    real prjp[NCONS];
    real prjm[NCONS];

    real fli[NCONS];
    real fri[NCONS];
    real flj[NCONS];
    real frj[NCONS];

    real dwij[MAX_NUM_FIELDS];

    for (int q = 0; q < NCONS; ++q)
    {
        for (int l = 0; l < n_poly; ++l)
        {
            dwij[q * n_poly + l] = 0.0;
        }
    }

    // surface term
    for (int qp = 0; qp < n_face; ++qp)
    {
        for (int q = 0; q < NCONS; ++q)
        {
            ulim[q] = 0.0;
            ulip[q] = 0.0;
            urim[q] = 0.0;
            urip[q] = 0.0;
            uljm[q] = 0.0;
            uljp[q] = 0.0;
            urjm[q] = 0.0;
            urjp[q] = 0.0;

            for (int l = 0; l < n_poly; ++l)
            {
                ulim[q] += wli[q * n_poly + l] * cell.face_nodes_ri[qp].phi[l]; // right face of zone i - 1
                ulip[q] += wij[q * n_poly + l] * cell.face_nodes_li[qp].phi[l]; // left face of zone i
                urim[q] += wij[q * n_poly + l] * cell.face_nodes_ri[qp].phi[l]; // right face of zone i
                urip[q] += wri[q * n_poly + l] * cell.face_nodes_li[qp].phi[l]; // left face of zone i + 1
                uljm[q] += wlj[q * n_poly + l] * cell.face_nodes_rj[qp].phi[l]; // top face of zone j - 1
                uljp[q] += wij[q * n_poly + l] * cell.face_nodes_lj[qp].phi[l]; // bottom face of zone j
                urjm[q] += wij[q * n_poly + l] * cell.face_nodes_rj[qp].phi[l]; // top face of zone j
                urjp[q] += wrj[q * n_poly + l] * cell.face_nodes_lj[qp].phi[l]; // bottom face of zone j + 1
            }
        }

        conserved_to_primitive(ulim, plim);
        conserved_to_primitive(ulip, plip);
        conserved_to_primitive(urim, prim);
        conserved_to_primitive(urip, prip);
        conserved_to_primitive(uljm, pljm);
        conserved_to_primitive(uljp, pljp);
        conserved_to_primitive(urjm, prjm);
        conserved_to_primitive(urjp, prjp);

        riemann_hlle(plim, plip, fli, 0);
        riemann_hlle(prim, prip, fri, 0);
        riemann_hlle(pljm, pljp, flj, 1);
        riemann_hlle(prjm, prjp, frj, 1);

        for (int q = 0; q < NCONS; ++q)
        {
            for (int l = 0; l < n_poly; ++l)
            {
                dwij[q * n_poly + l] -= fli[q] * cell.face_nodes_li[qp].phi[l] * cell.face_nodes_li[qp].weight;
                dwij[q * n_poly + l] -= fri[q] * cell.face_nodes_ri[qp].phi[l] * cell.face_nodes_ri[qp].weight;
                dwij[q * n_poly + l] -= flj[q] * cell.face_nodes_lj[qp].phi[l] * cell.face_nodes_lj[qp].weight;
                dwij[q * n_poly + l] -= frj[q] * cell.face_nodes_rj[qp].phi[l] * cell.face_nodes_rj[qp].weight;
            }
        }
    }

    real cons[NCONS];
    real primitive[NCONS];

    real flux_x[NCONS];
    real flux_y[NCONS];

    // volume term
    for (int qp = 0; qp < n_quad; ++qp)
    {
        struct NodeData node = cell.interior_nodes[qp];

        for (int q = 0; q < NCONS; ++q)
        {
            cons[q] = 0.0;

            for (int l = 0; l < n_poly; ++l)
            {
                cons[q] += wij[q * n_poly + l] * node.phi[l];
            }
        }

        conserved_to_primitive(cons, primitive);
        primitive_to_flux(primitive, cons, flux_x, 0);
        primitive_to_flux(primitive, cons, flux_y, 1);

        for (int q = 0; q < NCONS; ++q)
        {
            for (int l = 0; l < n_poly; ++l)
            {
                dwij[q * n_poly + l] += flux_x[q] * node.dphi_dx[l] * node.weight;
                dwij[q * n_poly + l] += flux_y[q] * node.dphi_dy[l] * node.weight;
            }
        }
    }

    real *wout = GET(weights_wr, i, j);

    for (int q = 0; q < NCONS; ++q)
    {
        for (int l = 0; l < n_poly; ++l)
        {
            wout[q * n_poly + l] = wij[q * n_poly + l] + 0.5 * dwij[q * n_poly + l] * dt / dx; // assumes dy = dx
        }
    }
}

static __host__ __device__ void limit_conserved_slopes_zone(
    struct Cell cell,
    struct Mesh mesh,
    struct Patch weights_rd,
    struct Patch weights_wr,
    int i,
    int j)
{
    real dx = mesh.dx;
    real dy = mesh.dy;

    int n_poly = num_polynomials(cell);

    real *wij = GET(weights_rd, i, j);
    real *wli = GET(weights_rd, i - 1, j);
    real *wri = GET(weights_rd, i + 1, j);
    real *wlj = GET(weights_rd, i, j - 1);
    real *wrj = GET(weights_rd, i, j + 1);

    // limit slopes

    real wtilde[MAX_NUM_FIELDS];

    for (int q = 0; q < NCONS; ++q)
    {
        // x slopes
        wtilde[q * n_poly + 2] = minmodTVB(wij[q * n_poly + 2], wli[q * n_poly + 0], wij[q * n_poly + 0], wri[q * n_poly + 0], dx);
        
        // y slopes 
        wtilde[q * n_poly + 1] = minmodTVB(wij[q * n_poly + 1], wlj[q * n_poly + 0], wij[q * n_poly + 0], wrj[q * n_poly + 0], dy);
        
        if ( (wtilde[q * n_poly + 2] != wij[q * n_poly + 2]) ||
             (wtilde[q * n_poly + 1] != wij[q * n_poly + 1]) ) 
        {
            wij[q * n_poly + 2] = wtilde[q * n_poly + 2];
            wij[q * n_poly + 1] = wtilde[q * n_poly + 1];
                
            for (int l = 3; l < n_poly; ++l)
            {
                wij[q * n_poly + l] = 0.0;
            } 
        }
    }

    real *wout = GET(weights_wr, i, j);

    for (int q = 0; q < NCONS; ++q)
    {
        for (int l = 0; l < n_poly; ++l)
        {
            wout[q * n_poly + l] = wij[q * n_poly + l];
        }
    }
}

static __host__ __device__ void limit_characteristic_slopes_zone(
    struct Cell cell,
    struct Mesh mesh,
    struct Patch weights_rd,
    struct Patch weights_wr,
    int i,
    int j)
{
    int n_poly = num_polynomials(cell);

    real dx = mesh.dx;
    real dy = mesh.dy;
    real BETA_TVB = 1.0;
    real SQRT_THREE = sqrt(3.0);
    real *wij = GET(weights_rd, i, j);
    real *wli = GET(weights_rd, i - 1, j);
    real *wri = GET(weights_rd, i + 1, j);
    real *wlj = GET(weights_rd, i, j - 1);
    real *wrj = GET(weights_rd, i, j + 1);

    // limit slopes
    real prim[NCONS];
    real   w0[NCONS];
    real  w0l[NCONS];
    real  w0r[NCONS];
    real  w0b[NCONS];
    real  w0t[NCONS];

    // slopes of conserved variables
    real w1[NCONS]; // y slopes
    real w2[NCONS]; // x slopes

    // slopes of characteristic variables
    real c1[NCONS]; // y slopes
    real c2[NCONS]; // x slopes

    // limited "tilde" slopes
    real w1t[NCONS];
    real w2t[NCONS]; 
    real c1t[NCONS];
    real c2t[NCONS];

    // characteristic version of difference of mean values (l=0) to neighbor zones
    real cl[NCONS]; // left
    real cr[NCONS]; // right   
    real cb[NCONS]; // bottom
    real ct[NCONS]; // top

    // Call routine to detect if this is a troubled cell
    // If so:
    if (1)
    {
        for (int q = 0; q < NCONS; ++q)
        {
            // mean values (l=0) of conserved variables in the cell and nearest neighbor cells
            w0[q]  = wij[q * n_poly + 0];
            w0l[q] = wli[q * n_poly + 0]; // left 
            w0r[q] = wri[q * n_poly + 0]; // right
            w0b[q] = wlj[q * n_poly + 0]; // bottom
            w0t[q] = wrj[q * n_poly + 0]; // top

            // slopes (l=1, l=2) of conserved variables in the cell
            w1[q] =  wij[q * n_poly + 1]; // y slopes
            w2[q] =  wij[q * n_poly + 2]; // x slopes
        }

        conserved_to_primitive(w0, prim);
        real cs2 = primitive_to_sound_speed_squared(prim);
        real cs = sqrt(cs2);
        real g1 = ADIABATIC_GAMMA - 1.0;
        real vx = prim[1];
        real vy = prim[2];
        real k = 0.5 * (vx * vx + vy * vy);
        real h = (cs2 / g1) + k;
        real phi = g1 * k;
        real beta = 1.0 / (2.0 * cs2);

        real lx[4][4]= {
              {beta*(phi+cs*vx),  -beta*(g1*vx+cs),  -beta*g1*vy,       beta*g1},
              {(1.0-2.0*beta*phi), 2.0*beta*g1*vx,   2.0*beta*g1*vy,    -2.0*beta*g1},
              {beta*(phi-cs*vx),  -beta*(g1*vx-cs),  -beta*g1*vy,       beta*g1},
              {vy,                      0.0,            -1.0,            0.0}};

        real ly[4][4] = {
              {beta*(phi+cs*vy),  -beta*g1*vx,  -beta*(g1*vy+cs),       beta*g1},
              {(1.0-2.0*beta*phi), 2.0*beta*g1*vx,   2.0*beta*g1*vy,    -2.0*beta*g1},
              {beta*(phi-cs*vy),  -beta*g1*vx,  -beta*(g1*vy-cs),       beta*g1},
              {-vx,                      1.0,            0.0,            0.0}};

        real rx[4][4] = {
              { 1.0,        1.0,        1.0,        0.0},
              {(vx - cs),   vx,     (vx + cs),      0.0},
              {  vy,        vy,         vy,        -1.0},
              {(h - cs*vx), k,      (h + cs*vx),    -vy}};

        real ry[4][4] = {
              { 1.0,        1.0,        1.0,        0.0},
              {vx,   vx,     vx,      1.0},
              {  vy-cs,        vy,         vy+cs,        0.0},
              {(h - cs*vy), k,      (h + cs*vy),    vx}};

        // convert to characteristic variables
        for (int qi = 0; qi < NCONS; ++qi)
        {
            c2[qi] = 0.0;
            c1[qi] = 0.0;
            cl[qi] = 0.0;
            cr[qi] = 0.0;
            cb[qi] = 0.0;
            ct[qi] = 0.0;

            for (int qj = 0; qj < NCONS; ++qj)
            {
                c2[qi] += lx[qi][qj] *   w2[qj]; // x slopes
                cl[qi] += lx[qi][qj] *  (w0[qj]  - w0l[qj]); // left difference
                cr[qi] += lx[qi][qj] * (w0r[qj]  -  w0[qj]); // right differ
                c1[qi] += ly[qi][qj] *   w1[qj]; // y slopes
                cb[qi] += ly[qi][qj] *  (w0[qj]  - w0b[qj]); // bottom difference
                ct[qi] += ly[qi][qj] * (w0t[qj]  -  w0[qj]); // top difference
            }
        }

        // limit characteristic slopes (for l=1, l=2)
        for (int q = 0; q < NCONS; ++q)
        {
            c1t[q] = minmodB(SQRT_THREE * c1[q], BETA_TVB * cb[q], BETA_TVB * ct[q], dy) / SQRT_THREE;
            c2t[q] = minmodB(SQRT_THREE * c2[q], BETA_TVB * cl[q], BETA_TVB * cr[q], dx) / SQRT_THREE;
        }

        // compute limited conservative slopes (for l=1, l=2)
        for (int qi = 0; qi < NCONS; ++qi)
        {
            w2t[qi] = 0.0;
            w1t[qi] = 0.0;
            for (int qj = 0; qj < NCONS; ++qj)
            {
                w1t[qi] += ry[qi][qj] * c1t[qj]; // y slopes
                w2t[qi] += rx[qi][qj] * c2t[qj]; // x slopes
            }
        }

        for (int q = 0; q < NCONS; ++q)
        {
            if ( (c2t[q] != c2[q]) || (c1t[q] != c1[q]) )
            {              
                wij[q * n_poly + 2] = w2t[q];
                wij[q * n_poly + 1] = w1t[q];
                for (int l = 3; l < n_poly; ++l)
                {
                    wij[q * n_poly + l] = 0.0;
                }                        
            }
        }
    }

    real *wout = GET(weights_wr, i, j);

    for (int q = 0; q < NCONS; ++q)
    {
        for (int l = 0; l < n_poly; ++l)
        {
            wout[q * n_poly + l] = wij[q * n_poly + l];
        }
    }
}

static __host__ __device__ void wavespeed_zone(
    struct Cell cell,
    struct Patch weights,
    struct Patch wavespeed,
    int i,
    int j)
{
    int n_poly = num_polynomials(cell);

    real cons[NCONS];
    real prim[NCONS];

    real *wij = GET(weights, i, j);

    // use zero weights
    for (int q = 0; q < NCONS; ++q)
    {
        cons[q] = wij[q * n_poly + 0];
    }

    conserved_to_primitive(cons, prim);

    real a = primitive_max_wavespeed(prim);

    GET(wavespeed, i, j)[0] = a;
}

// ============================ KERNELS =======================================
// ============================================================================
#if defined(__NVCC__) || defined(__ROCM__)

static void __global__ advance_rk_dg_kernel(
    struct Cell cell,
    struct Mesh mesh,
    struct Patch weights_rd,
    struct Patch weights_wr,
    real dt)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        advance_rk_zone_dg(
            cell,
            mesh,
            weights_rd,
            weights_wr,
            dt,
            i, j
        );
    }
}

static void __global__ limit_conserved_slopes_kernel(
    struct Cell cell,
    struct Mesh mesh,
    struct Patch weights_rd,
    struct Patch weights_wr)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        limit_conserved_slopes_zone(
            cell,
            mesh,
            weights_rd,
            weights_wr,
            i, j
        );
    }
}

static void __global__ limit_characteristic_slopes_kernel(
    struct Cell cell,
    struct Mesh mesh,
    struct Patch weights_rd,
    struct Patch weights_wr)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        limit_characteristic_slopes_zone(
            cell,
            mesh,
            weights_rd,
            weights_wr,
            i, j
        );
    }
}

static void __global__ wavespeed_kernel(
    struct Cell cell,
    struct Mesh mesh,    
    struct Patch weights,
    struct Patch wavespeed)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        wavespeed_zone(cell, weights, wavespeed, i, j);
    }
}

#endif // defined(__NVCC__) || defined(__ROCM__)

// ============================ PUBLIC API ====================================
// ============================================================================

/**
 * Updates an array of DG weights data by advancing it a single Runge-Kutta
 * step.
 * @param mesh                  The mesh [ni,     nj]
 * @param weights_rd_ptr[in]    [-1, -1] [ni + 2, nj + 2] [4]
 * @param weights_wr_ptr[out]   [-1, -1] [ni + 2, nj + 2] [4]
 * @param dt                    The time step
 * @param mode                  The execution mode
 */
EXTERN_C void euler2d_dg_advance_rk(
    struct Cell cell,
    struct Mesh mesh,
    real *weights_rd_ptr,
    real *weights_wr_ptr,
    real dt,
    enum ExecutionMode mode)
{
    int n_poly = num_polynomials(cell);

    struct Patch weights_rd = patch(mesh, n_poly * NCONS, NUM_GUARD, weights_rd_ptr);
    struct Patch weights_wr = patch(mesh, n_poly * NCONS, NUM_GUARD, weights_wr_ptr);

    switch (mode) {
        case CPU: {
            FOR_EACH(weights_rd, NUM_GUARD) {
                advance_rk_zone_dg(
                    cell,
                    mesh,
                    weights_rd,
                    weights_wr,
                    dt,
                    i, j
                );
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            FOR_EACH_OMP(weights_rd, NUM_GUARD) {
                advance_rk_zone_dg(
                    cell,
                    mesh,
                    weights_rd,
                    weights_wr,
                    dt,
                    i, j
                );
            }
            #endif
            break;
        }

        case GPU: {
            #if defined(__NVCC__) || defined(__ROCM__)
            dim3 bs = dim3(16, 16);
            dim3 bd = dim3((mesh.nj + bs.x - 1) / bs.x, (mesh.ni + bs.y - 1) / bs.y);

            advance_rk_dg_kernel<<<bd, bs>>>(
                cell,
                mesh,
                weights_rd,
                weights_wr,
                dt
            );
            #endif
            break;
        }
    }
}

/**
 * Limits slopes of polynomials
 * @param mesh                  The mesh [ni,     nj]
 * @param weights_rd_ptr[in]    [-1, -1] [ni + 2, nj + 2] [4]
 * @param weights_wr_ptr[out]   [-1, -1] [ni + 2, nj + 2] [4]
 * @param mode                  The execution mode
 */
EXTERN_C void euler2d_dg_limit_slopes(
    struct Cell cell,
    struct Mesh mesh,
    real *weights_rd_ptr,
    real *weights_wr_ptr,
    enum ExecutionMode mode)
{
    int n_poly = num_polynomials(cell);

    struct Patch weights_rd = patch(mesh, n_poly * NCONS, NUM_GUARD, weights_rd_ptr);
    struct Patch weights_wr = patch(mesh, n_poly * NCONS, NUM_GUARD, weights_wr_ptr);

    switch (mode) {
        case CPU: {
            FOR_EACH(weights_rd, NUM_GUARD) {
                limit_characteristic_slopes_zone(
                    cell,
                    mesh,
                    weights_rd,
                    weights_wr,
                    i, j
                );
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            FOR_EACH_OMP(weights_rd, NUM_GUARD) {
                limit_characteristic_slopes_zone(
                    cell,
                    mesh,
                    weights_rd,
                    weights_wr,
                    i, j
                );
            }
            #endif
            break;
        }

        case GPU: {
            #if defined(__NVCC__) || defined(__ROCM__)
            dim3 bs = dim3(16, 16);
            dim3 bd = dim3((mesh.nj + bs.x - 1) / bs.x, (mesh.ni + bs.y - 1) / bs.y);

            limit_characteristic_slopes_kernel<<<bd, bs>>>(
                cell,
                mesh,
                weights_rd,
                weights_wr,
            );
            #endif
            break;
        }
    }
}

/**
* Converts an array of weights data to an array of wavespeed data.
* The data array index spaces must follow the descriptions below.
*
* @param cell               The cell [order]
* @param weights_ptr[in]    [-1, -1] [ni + 2, nj + 2] [4]
* @param wavespeed_ptr[out] [ 0,  0] [ni,     nj]     [1]
* @param mode               The execution mode
*/
EXTERN_C void euler2d_dg_wavespeed(
    struct Cell cell,
    struct Mesh mesh,
    real *weights_ptr,
    real *wavespeed_ptr,
    enum ExecutionMode mode)
{
    int n_poly = num_polynomials(cell);

    struct Patch weights   = patch(mesh, NCONS * n_poly, NUM_GUARD, weights_ptr);
    struct Patch wavespeed = patch(mesh, 1, 0, wavespeed_ptr);

    switch (mode) {
        case CPU: {
            FOR_EACH(weights, NUM_GUARD)
            {
                wavespeed_zone(cell, weights, wavespeed, i, j);
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            FOR_EACH_OMP(weights, NUM_GUARD)
            {
                wavespeed_zone(cell, weights, wavespeed, i, j);
            }
            #endif
            break;
        }

        case GPU: {
            #if defined(__NVCC__) || defined(__ROCM__)
            dim3 bs = dim3(16, 16);
            dim3 bd = dim3((mesh.nj + bs.x - 1) / bs.x, (mesh.ni + bs.y - 1) / bs.y);
            wavespeed_kernel<<<bd, bs>>>(cell, mesh, weights, wavespeed);
            #endif
            break;
        }
    }
}

/**
 * Obtain the maximum value in an array of double's, using either a sequential
 * or an OpenMP reduction. Not implemented for GPU execution.
 * 
 * @param data          The data [size]
 * @param size          The number of elements
 * @param mode          The execution mode
 */
EXTERN_C real euler2d_dg_maximum(
    real *data,
    unsigned long size,
    enum ExecutionMode mode)
{
    real a_max = 0.0;

    switch (mode) {
        case CPU: {
            for (unsigned long i = 0; i < size; ++i)
            {
                a_max = max2(a_max, data[i]);
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            #pragma omp parallel for reduction(max:a_max)
            for (unsigned long i = 0; i < size; ++i)
            {
                a_max = max2(a_max, data[i]);
            }
            #endif
            break;
        }

        case GPU: break; // Not implemented, use iso2d_wavespeed
                         // followed by a GPU reduction.
    }
    return a_max;
}