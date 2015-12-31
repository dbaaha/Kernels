/*
Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without 
modification, are permitted provided that the following conditions 
are met:

* Redistributions of source code must retain the above copyright 
      notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above 
      copyright notice, this list of conditions and the following 
      disclaimer in the documentation and/or other materials provided 
      with the distribution.
* Neither the name of Intel Corporation nor the names of its 
      contributors may be used to endorse or promote products 
      derived from this software without specific prior written 
      permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE 
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, 
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN 
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
POSSIBILITY OF SUCH DAMAGE.
*/

/*******************************************************************

NAME:    PIC

PURPOSE: This program tests the efficiency with which a cloud of
         charged particles can be moved through a spatially fixed
         collection of charges located at the vertices of a square
         equi-spaced grid. It is a proxy for a component of a
         particle-in-cell methjod
  
USAGE:   <progname> <#simulation steps> <grid size> <#particles> \
                    <horizontal velocity> <vertical velocity>    \
                    <init mode> <init parameters>                \
                    [<population change mode> <population change parameters>]

  
         The output consists of diagnostics to make sure the 
         algorithm worked, and of timing statistics.

FUNCTIONS CALLED:

         Other than standard C functions, the following functions are used in 
         this program:
         wtime()
         bad_path()
         LCG_next()

HISTORY: - Written by Evangelos Georganas, August 2015.
         - RvdW: Refactored to make the code PRK conforming, December 2015
  
**********************************************************************************/

#include <par-res-kern_general.h>
#include <lcg.h>

#include <math.h>
#include <stdint.h>
#include <inttypes.h>

#define QG(i,j) Qgrid[(j)*(g)+i]
#define mass_inv 1.0
#define Q 1.0
#define epsilon 0.000001
#define dt 1.0

#define SUCCESS 1
#define FAILURE 0

#define REL_X 0.5
#define REL_Y 0.5

#define GEOMETRIC  0
#define SINUSOIDAL 1
#define LINEAR     2
#define PATCH      3
#define UNDEFINED  4

typedef struct {
  int64_t xleft;
  int64_t xright;
  int64_t ybottom;
  int64_t ytop;
} bbox_t;

/* Particle data structure */
typedef struct particle_t {
   double   x;
   double   y;
   double   v_x;
   double   v_y;
   double   q;
   /* The following variables are used only for verification/debug purposes */
   double   x0;
   double   y0;
   int64_t  k; //  determines how many cells particles move per time step in the x direction 
   int64_t  m; //  determines how many cells particles move per time step in the y direction 
   int64_t  initTimestamp;
} particle_t;

/* Initializes the grid of charges
  We follow a column major format for the grid. Note that this may affect cache performance, depending on access pattern of particles. */

/* The grid is indexed in this way:
 
   y  ^                                        
      |
      |
      |
      |
      |
      |
   (0,0)--------------> x                           */

double *initializeGrid(int64_t g)
{
   double   *Qgrid;
   int64_t  y, x;
   
   Qgrid = (double*) malloc(g*g*sizeof(double));
   if (Qgrid == NULL) {
      printf("ERROR: Could not allocate space for grid\n");
      exit(EXIT_FAILURE);
   }
   
   /* initialization with dipoles */
   for (x=0; x<g; x++) {
      for (y=0; y<g; y++) {
         QG(y,x) = (x%2 == 0) ? Q : -Q;
      }
   }
   return Qgrid;
}

/* Initializes the particles following the geometric distribution as described in the spec */
particle_t *initializeParticlesGeometric(int64_t n, int64_t g, double rho)
{
   particle_t  *particles;
   int64_t     y, x, p, pi, n_part_column;
   double      A;
   
   particles = (particle_t*) malloc(n * sizeof(particle_t));
   if (particles == NULL) {
      printf("ERROR: Could not allocate space for particles\n");
      exit(EXIT_FAILURE);
   }
   
   /* Add appropriate number of particles to each cell to form distribution decribed in spec. 
      Each cell in the i-th column of cells contains p(i) = A * rho^i particles */
   A = n * ((1-rho) / (1-pow(rho, g-1)));
   for (pi=0,x=0; x<g-1; x++) {
      n_part_column = (int64_t) floor(A * pow(rho, x));
      
      for (p=0; p<n_part_column; p++) {
         /* arc4random_uniform() would avoid bias caused by modulo operation, but is not 
            reproducible. Use Knuth's reproducible mixed LCG instead */
         particles[pi].x = x + REL_X;
         particles[pi].y = LCG_next(g-1) + REL_Y;
         pi++;
      }
   }
   
   /* Add remaining particles in first column of cells */
   for (; pi<n; pi++) {
      particles[pi].x = REL_X;
      particles[pi].y = LCG_next(g-1) + REL_Y;
   }
   
   return particles;
}


/* Initialize with a particle distribution where the number of particles per cell-column follows a sinusoidal distribution */
particle_t *initializeParticlesSinusoidal(int64_t n, int64_t g)
{
   particle_t  *particles;
   double      total_weight = 0.0 , step = 2*M_PI / (g-2), current_weight;
   int64_t     x, y, pi = 0, i, p, n_part_column;

   particles = (particle_t*) malloc(n * sizeof(particle_t));
   if (particles == NULL) {
      printf("ERROR: Could not allocate space for particles\n");
      exit(EXIT_FAILURE);
   }
   
   /* First, find sum of all corresponding weights to normalize number of particles later */
   for (i=0; i<=g-2; i++) {
      total_weight += (1 + cos(step * ((double) i)));
   }
   
   /* Iterate over the columns of cells and assign number of particles proportional to the corresponding sinusodial weight */
   for (x=0; x<=g-2; x++) {
      current_weight = (1 + cos(step * ((double) x)));
      n_part_column = (int64_t) floor(n * current_weight / total_weight);
      for (p=0; p < n_part_column; p++) {
         particles[pi].x = x + REL_X;
         particles[pi].y = LCG_next(g-1) + REL_Y;
         pi++;
      }
   }

   /* distribute remaining particles across entire grid */   
   for (; pi<n; pi++) {
      particles[pi].x = LCG_next(g-1) + REL_X;
      particles[pi].y = LCG_next(g-1) + REL_Y;
   }
   
   return particles;
}

/* Initialize particles with "linearly-decreasing" distribution */
/* The linear function is f(x) = -alpha * x + beta , x in [0,1]*/
particle_t *initializeParticlesLinear(int64_t n, int64_t g, int alpha, int beta)
{
   particle_t  *particles;
   double      total_weight = 0.0 , step = 1.0 / (g-2), current_weight;
   int64_t     pi = 0, i, p, n_part_column, x, y;
   
   particles = (particle_t*) malloc(n * sizeof(particle_t));
   if (particles == NULL) {
      printf("ERROR: Could not allocate space for particles\n");
      exit(EXIT_FAILURE);
   }
   
   /* First, find the sum of all the corresponding weights in order to normalize the number of particles later */
   for (i=0; i<=g-2; i++) {
      total_weight += (beta - alpha * step * ((double) i));
   }
   
   /* Iterate over the columns of cells and assign number of particles proportional to the corresponding linear weight */
   for (x=0; x<=g-2; x++) {
      current_weight = (beta - alpha * step * ((double) x));
      n_part_column = (int64_t) floor(n * current_weight / total_weight);
      for (p=0; p < n_part_column; p++) {
         particles[pi].x = x + REL_X;
         particles[pi].y = LCG_next(g-1) + REL_Y;
         pi++;
         
      }
   }
   
   for (; pi<n; pi++) {
      particles[pi].x = LCG_next(g-1) + REL_X;
      particles[pi].y = LCG_next(g-1) + REL_Y;
   }
   
   return particles;
}

/* Initialize uniformly particles within a "patch" */

particle_t *initializeParticlesPatch(int64_t n, int64_t g, bbox_t patch)
{
   particle_t  *particles;
   int64_t     pi = 0, i, p, n_part_column, x, y, total_cells, particles_per_cell;
   
   particles = (particle_t*) malloc(n * sizeof(particle_t));
   if (particles == NULL) {
      printf("ERROR: Could not allocate space for particles\n");
      exit(EXIT_FAILURE);
   }
   
   total_cells  = (patch.xright - patch.xleft)*(patch.ytop - patch.ybottom);
   particles_per_cell = (int64_t) floor((1.0*n)/total_cells);
   
   for (x = patch.xleft; x < patch.xright; x++) {
      for (y = patch.ybottom; y < patch.ytop; y++) {
         for (i=0; i<particles_per_cell; i++) {
            particles[pi].x = x + REL_X;
            particles[pi].y = y + REL_Y;
            pi++;
         }
      }
   }
   
   /* Distribute the remaining particles evenly */
   for (x = patch.xleft; x < patch.xright; x++) {
      for (y = patch.ybottom; (y < patch.ytop)&& (pi < n); y++) {
         particles[pi].x = x + REL_X;
         particles[pi].y = y + REL_Y;
         pi++;
      }
   }
   
   return particles;
}

/* Injects particles in a specified area of the simulation domain */
particle_t *inject_particles(int64_t injection_timestep, bbox_t patch, int particles_per_cell, int64_t *n, particle_t *particles)
{
   int64_t  total_cells  = (patch.xright - patch.xleft)*(patch.ytop - patch.ybottom);
   int64_t  total_injected_particles = total_cells * particles_per_cell;
   int64_t  new_size = (*n) + total_injected_particles;
   int64_t  x, y, pos = (*n), i;
   
   /* Allocate new array for existing and injected particles */
   particle_t *new_particles_array = (particle_t*) malloc(new_size * sizeof(particle_t));
   memcpy(new_particles_array, particles, (*n) * sizeof(particle_t));
   free(particles);
   
   /* Add particles in the injection area */
   for (y=patch.ybottom; y<patch.ytop; y++) {
      for (x=patch.xleft; x<patch.xright; x++) {
         for (i=0; i<particles_per_cell; i++) {
            new_particles_array[pos].x = x + REL_X;
            new_particles_array[pos].y = y + REL_Y;
            pos++;
         }
      }
   }
   
   (*n) = new_size;
   return new_particles_array;
}

/* Completes particle distribution */
void finish_distribution(int64_t timestep, int k, int m, int64_t n, particle_t *particles) {

  double x_coord, y_coord, rel_x, rel_y, cos_theta, cos_phi, r1_sq, r2_sq, charge;
  int64_t x, pi;

  for (pi=0; pi<n; pi++) {
    x_coord = particles[pi].x;
    y_coord = particles[pi].y;
    rel_x = fmod(x_coord,1.0);
    rel_y = fmod(y_coord,1.0);
    x = (int64_t) x_coord;
    r1_sq = rel_y * rel_y + rel_x * rel_x;
    r2_sq = rel_y * rel_y + (1.0-rel_x) * (1.0-rel_x);
    cos_theta = rel_x/sqrt(r1_sq);
    cos_phi = (1.0-rel_x)/sqrt(r2_sq);
    charge = 1.0 / ((dt*dt) * Q * (cos_theta/r1_sq + cos_phi/r2_sq));
         
    particles[pi].v_x = 0.0;
    particles[pi].v_y = ((double) m) / dt;
    particles[pi].q = (x%2 == 0) ? (2*k+1) * charge : -1.0 * (2*k+1) * charge ;
    particles[pi].x0 = x_coord;
    particles[pi].y0 = y_coord;
    particles[pi].k = k;
    particles[pi].m = m;
    particles[pi].initTimestamp = timestep;;
  }
}

/* Verifies the final position of a particle */
int verifyParticle(particle_t p, int64_t current_timestep, double *Qgrid, int64_t g)
{
   int64_t  total_steps = current_timestep - p.initTimestamp;
   double   x_T, y_T, x_periodic, y_periodic;
   double   L = (g-1);
   
   /* Coordinates of the cell containing the particle initially */
   int64_t y = (int64_t) floor(p.y0);
   int64_t x = (int64_t) floor(p.x0);
   
   /* According to initial location and charge determine the direction of displacements */
   x_T = ( (p.q * QG(y,x)) > 0) ? p.x0 + total_steps * (2*p.k+1) : p.x0 - total_steps * (2*p.k+1)  ;
   y_T = p.y0 + p.m * total_steps;
   
   x_periodic = fmod(x_T+total_steps *(2*p.k+1)*L, L);
   y_periodic = fmod(y_T+total_steps *fabs(p.m)*L, L);
   
   if ( fabs(p.x - x_periodic) > epsilon || fabs(p.y - y_periodic) > epsilon) {
      return FAILURE;
   }
   
   return SUCCESS;
}

/* Removes particles from a specified area of the simulation domain */
void remove_particles(int64_t removal_timestep, bbox_t patch, int64_t *n, particle_t *particles, int *partial_correctness, double *Qgrid, int64_t g)
{
   int64_t  pos = 0, i;
   /* The boundaries of the simulation domain where we have to remove the particles */
   double   left_boundary = patch.xleft;
   double   right_boundary = patch.xright;
   double   top_boundary = patch.ytop;
   double   bottom_boundary = patch.ybottom;
   particle_t  *new_particles_array;
   
   for (i = 0; i < (*n); i++) {
      if ( (particles[i].x > left_boundary) && (particles[i].x < right_boundary) && 
           (particles[i].y > bottom_boundary) && (particles[i].y < top_boundary)) {
         /* We should remove and verify this particle */
         (*partial_correctness) *= verifyParticle(particles[i], removal_timestep, Qgrid, g);
      } else {
         /* We should keep the particle */
         particles[pos] = particles[i];
         pos++;
      }
   }
   (*n) = pos;
   return;
}


/* Computes the Coulomb force among two charges q1 and q2 */
int computeCoulomb(double x_dist, double y_dist, double q1, double q2, double *fx, double *fy)
{
   double   r2 = x_dist * x_dist + y_dist * y_dist;
   double   r = sqrt(r2);
   double   f_coulomb = q1 * q2 / r2;
   
   (*fx) = f_coulomb * x_dist / r; // f_coulomb * cos_theta
   (*fy) = f_coulomb * y_dist / r; // f_coulomb * sin_theta
   
   return 0;
}

/* Computes the total Coulomb force on a particle exerted from the charges of the corresponding cell */
int computeTotalForce(particle_t p, int64_t g, double *Qgrid, double *fx, double *fy)
{
   int64_t  y, x, k;
   double   tmp_fx, tmp_fy, rel_y, rel_x;
   double   tmp_res_x = 0.0;
   double   tmp_res_y = 0.0;
   
   /* Coordinates of the cell containing the particle */
   y = (int64_t) floor(p.y);
   x = (int64_t) floor(p.x);
   rel_x = p.x -  x;
   rel_y = p.y -  y;
   
   /* Coulomb force from top-left charge */
   computeCoulomb(rel_x, rel_y, p.q, QG(y,x), &tmp_fx, &tmp_fy);
   tmp_res_x += tmp_fx;
   tmp_res_y += tmp_fy;
   
   /* Coulomb force from bottom-left charge */
   computeCoulomb(rel_x, 1.0-rel_y, p.q, QG(y+1,x), &tmp_fx, &tmp_fy);
   tmp_res_x += tmp_fx;
   tmp_res_y -= tmp_fy;
   
   /* Coulomb force from top-right charge */
   computeCoulomb(1.0-rel_x, rel_y, p.q, QG(y,x+1), &tmp_fx, &tmp_fy);
   tmp_res_x -= tmp_fx;
   tmp_res_y += tmp_fy;
   
   /* Coulomb force from bottom-right charge */
   computeCoulomb(1.0-rel_x, 1.0-rel_y, p.q, QG(y+1,x+1), &tmp_fx, &tmp_fy);
   tmp_res_x -= tmp_fx;
   tmp_res_y -= tmp_fy;
   
   (*fx) = tmp_res_x;
   (*fy) = tmp_res_y;
   
   return 0;
}

/* Moves a particle given the total acceleration */
int moveParticle(particle_t *particle, double ax, double ay, double L)
{
   
   /* Update particle positions, taking into account periodic boundaries */
   particle->x = fmod(particle->x + particle->v_x*dt + 0.5*ax*dt*dt + L, L);
   particle->y = fmod(particle->y + particle->v_y*dt + 0.5*ay*dt*dt + L, L);
   
   /* Update velocities */
   particle->v_x += ax * dt;
   particle->v_y += ay * dt;
   
   return 0;
}


int bad_patch(bbox_t *patch, bbox_t *patch_contain) {
  if (patch->xleft>=patch->xright || patch->ybottom>=patch->ytop) return(1);
  if (patch_contain) {
    if (patch->xleft  <patch_contain->xleft   || patch->xright>patch_contain->xright) return(2);
    if (patch->ybottom<patch_contain->ybottom || patch->ytop  >patch_contain->ytop)   return(3);
  }
  return(0);
}

int main(int argc, char ** argv) {

  int         args_used = 1; // keeps track of # consumed arguments
  int64_t     g;     // dimension of grid in points
  int64_t     T;     // total number of simulation steps
  int64_t     n;     // total number of particles in the simulation
  int64_t     n_old; // number of particled before removal
  char        *init_mode; // Initialization mode for particles
  double      rho;   // rho parameter for the initial geometric particle distribution
  int64_t     k, m;  // determine initial horizontal and vertical velocity of particles-- 
                     //  (2*k)+1 cells per time step 
  int64_t     particle_mode;
  double      alpha, beta;
  bbox_t      grid_patch, init_patch, injection_patch, removal_patch;
  int         removal_mode = 0, injection_mode = 0, injection_timestep, removal_timestep, particles_per_cell;
  int         partial_correctness = 1;
  int64_t     L;     // dimension of grid in cells
  double      *Qgrid;// the grid is represented as an array of charges
  particle_t  *particles; // the particles array
  int64_t     t, i, j;
  double      fx, fy, ax, ay, simulation_time;
  int         correct_simulation = 1;
  int         error;
  int64_t     particle_steps, particles_added;
  double      avg_time;

  printf("Parallel Research Kernels Version %s\n", PRKVERSION);
  printf("Serial Particle-in-Cell execution on 2D grid\n");

  /*******************************************************************************
  ** process and test input parameters    
  ********************************************************************************/

  if (argc<6) {
    printf("Usage: %s <#simulation steps> <grid size> <#particles> <k (particle charge semi-increment)> ", argv[0]);
    printf("<m (vertical particle velocity)>\n");
    printf("          <init mode> <init parameters> [<population change mode> <population change parameters>]\n");
    printf("   init mode \"GEOMETRIC\"  parameters: <attenuation factor>\n");
    printf("             \"SINUSOIDAL\" parameters: none\n");
    printf("             \"LINEAR\"     parameters: <slope> <constant>\n");
    printf("             \"PATCH\"      parameters: <xleft> <xright>  <ybottom> <ytop>\n");
    printf("   population change mode \"INJECTION\" parameters:  <# particles> <time step> <xleft> <xright>  <ybottom> <ytop>\n");
    printf("                          \"REMOVAL\"   parameters:  <time step> <xleft> <xright>  <ybottom> <ytop>\n");
    exit(SUCCESS);
  }
   
  T = atol(*++argv);  args_used++;   
  if (T<1) {
    printf("ERROR: Number of time steps must be positive: %ld\n", g);
    exit(FAILURE);
  }
  L = atol(*++argv);  args_used++;   
  if (L<1 || L%2) {
    printf("ERROR: Number of grid cells must be positive and even: %ld\n", L);
    exit(FAILURE);
  }
  g = L+1;
  grid_patch = (bbox_t){0, g, 0, g};
  n = atol(*++argv);  args_used++;   
  if (n<1) {
    printf("ERROR: Number of particles must be positive: %ld\n", n);
    exit(FAILURE);
  }

  particle_steps = n*T;  
  particle_mode  = UNDEFINED;
  partial_correctness = 1;
  k = atoi(*++argv);   args_used++; 
  if (k<0) {
    printf("ERROR: Particle semi-charge must be non-negative: %d\n", k);
    exit(FAILURE);
  }
  m = atoi(*++argv);   args_used++; 
  init_mode = *++argv; args_used++;  
   
   /* Initialize particles with geometric distribution */
   if (strcmp(init_mode, "GEOMETRIC") == 0) {
      if (argc<args_used+1) {
         printf("ERROR: Not enough arguments\n"); exit(FAILURE);
      }
      particle_mode = GEOMETRIC;
      rho = atof(*++argv);   args_used++;
   }
   
   /* Initialize with a sinusoidal particle distribution (single period) */
   if (strcmp(init_mode, "SINUSOIDAL") == 0) {
      particle_mode = SINUSOIDAL;
   }
   
   /* Initialize particles with linear distribution */
   /* The linear function is f(x) = -alpha * x + beta , x in [0,1]*/
   if (strcmp(init_mode, "LINEAR") == 0) {
      if (argc<args_used+2) {
         printf("ERROR: Not enough arguments\n");
         exit(EXIT_FAILURE);
      }
      particle_mode = LINEAR;
      alpha = atof(*++argv); args_used++; 
      beta  = atof(*++argv); args_used++;
   }
   
   /* Initialize uniformly particles within a "patch" */
   if (strcmp(init_mode, "PATCH") == 0) {
      if (argc<args_used+4) {
         printf("ERROR: Not enough arguments\n");
         exit(FAILURE);
      }
      particle_mode = PATCH;
      init_patch.xleft   = atoi(*++argv); args_used++;
      init_patch.xright  = atoi(*++argv); args_used++;
      init_patch.ybottom = atoi(*++argv); args_used++;
      init_patch.ytop    = atoi(*++argv); args_used++;
      if (bad_patch(&init_patch, 0)) {
        printf("ERROR: inconsistent initial patch\n");
        exit(FAILURE);
      }
   }

   printf("Grid size                      = %lld\n", L);
   printf("Initial number of particles    = %lld\n", n);
   printf("Number of time steps           = %lld\n", T);
   printf("Initialization mode            = %s\n", init_mode);
   switch(particle_mode) {
   case GEOMETRIC: printf("  Attenuation factor           = %lf\n", rho);    break;
   case SINUSOIDAL:                                                          break;
   case LINEAR:    printf("  Negative slope               = %lf\n", alpha);
                   printf("  Offset                       = %lf\n", beta);   break;
   case PATCH:     printf("  Bounding box                 = %d, %d, %d, %d\n",
                          init_patch.xleft, init_patch.xright, 
                          init_patch.ybottom, init_patch.ytop);              break;
   default:        printf("ERROR: Unsupported particle initializating mode\n");
                   exit(FAILURE);
   }
   printf("Particle charge semi-increment = %d\n", k);
   printf("Vertical velocity              = %d\n", m);
   
   /* Check if user requested injection/removal of particles */
   if (argc > args_used) {
      char *ir_mode=*++argv; args_used++;
      if (strcmp(ir_mode, "INJECTION") == 0 ) {
      if (argc<args_used+6) {
         printf("ERROR: Not enough arguments\n");
         exit(FAILURE);
      }
         injection_mode = 1;
         /* Particles per cell to inject */
         particles_per_cell = atoi(*++argv);
         if (particles_per_cell < 0) {
           printf("Injected particles per cell need to be non-negative: %ld\n", 
		  particles_per_cell);
           exit(FAILURE);
         }
         injection_timestep = atoi(*++argv);
         if (injection_timestep < 0) {
           printf("Injection time step needs to be non-negative: %ld\n", injection_timestep);
           exit(FAILURE);
         }           
         /* Coordinates that define the simulation area where injection will take place */
         injection_patch.xleft   = atoi(*++argv);
         injection_patch.xright  = atoi(*++argv);
         injection_patch.ybottom = atoi(*++argv);
         injection_patch.ytop    = atoi(*++argv);
         if (error=bad_patch(&injection_patch, &grid_patch)) {
           printf("ERROR: inconsistent injection patch: %d\n",error);
           exit(FAILURE);
         }
         printf("Population change mode         = INJECTION\n");
         printf("  Bounding box                 = %d, %d, %d, %d\n",     
                injection_patch.xleft, injection_patch.xright, 
                injection_patch.ybottom, injection_patch.ytop);   
         printf("  Injection time step          = %d\n", injection_timestep);
         printf("  Particles per cell           = %d\n",  particles_per_cell);
         particles_added = 
           (injection_patch.xright-injection_patch.xleft)*
           (injection_patch.ytop-injection_patch.ybottom)*
           particles_per_cell;
         printf("  Total particles added        = %d\n",  particles_added);
         particle_steps += particles_added*(T+1-injection_timestep);
      }
      
      if (strcmp(ir_mode, "REMOVAL") == 0 ) {
         removal_mode = 1;
         removal_timestep = atoi(*++argv);
         /* Coordinates that define the simulation area where the particles will be removed */
         removal_patch.xleft   = atoi(*++argv);
         removal_patch.xright  = atoi(*++argv);
         removal_patch.ybottom = atoi(*++argv);
         removal_patch.ytop    = atoi(*++argv);
         if (bad_patch(&removal_patch, &grid_patch)) {
           printf("ERROR: inconsistent removal patch\n");
           exit(FAILURE);
         }
         printf("Population change mode         = REMOVAL\n");
         printf("  Bounding box                 = %d, %d, %d, %d\n",     
                removal_patch.xleft, removal_patch.xright, 
                removal_patch.ybottom, removal_patch.ytop);   
         printf("  removal time step            = %d\n", removal_timestep);
      }
   }

   /* Initialize grid of charges and particles */
   Qgrid = initializeGrid(g);
   
   switch(particle_mode) {
   case GEOMETRIC:  particles = initializeParticlesGeometric(n, g, rho);      break;
   case SINUSOIDAL: particles = initializeParticlesSinusoidal(n, g);          break;
   case LINEAR:     particles = initializeParticlesLinear(n, g, alpha, beta); break;
   case PATCH:      particles = initializeParticlesPatch(n, g, init_patch);   break;
   default:         printf("ERROR: No supported particle distribution\n");  exit(FAILURE);
   }   

   finish_distribution(0, k, m, n, particles);

   /* Run the simulation */
   for (t=0; t<=T; t++) {
    
     /* start the timer after one warm-up time step */
      if (t==1) simulation_time = wtime();  
      /* Check if we have to inject particles at this timestep  */
      if (injection_mode && (t == injection_timestep)) {
         n_old=n;
         particles = inject_particles(t, injection_patch, particles_per_cell, &n, particles);
         finish_distribution(t, 0, 0, n-n_old, particles+n_old);
      }
      
      /* Check if we have to remove particles now. Validate removed particles */
      if (removal_mode && (t == removal_timestep)) {
         n_old = n;
         remove_particles(t, removal_patch, &n, particles, &partial_correctness, Qgrid, g);
         particle_steps -= (n_old-n)*(T+1-removal_timestep);
      }
      
      /* Calculate forces on particles and update positions */
      for (i=0; i<n; i++) {
         fx = 0.0;
         fy = 0.0;
         computeTotalForce(particles[i], g, Qgrid, &fx, &fy);
         ax = fx * mass_inv;
         ay = fy * mass_inv;
         moveParticle(&particles[i], ax, ay, L);
      }
   }
   
   simulation_time = wtime() - simulation_time;
   
   /* In case of particles removal initalize the correctness flag with the correctness */
   correct_simulation = partial_correctness;
   
   /* Run the verification test */
   for (i=0; i<n; i++) {
      correct_simulation *= verifyParticle(particles[i], T+1, Qgrid, g);
   }
   
   if (correct_simulation) {
     printf("Solution validates\n");
#ifdef VERBOSE
      printf("Final number of particles = %lld\n", n);
#endif
      printf("Simulation time is %lf seconds\n", simulation_time);
      avg_time = particle_steps/simulation_time;
      printf("Rate (Mparticles_moved/s): %lf\n", 1.0e-6*avg_time);
   } else {
      printf("Solution does not validate\n");
   }
   
   return 0;
}
