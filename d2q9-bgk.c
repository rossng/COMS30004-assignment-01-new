/*
** Code to implement a d2q9-bgk lattice boltzmann scheme.
** 'd2' inidates a 2-dimensional grid, and
** 'q9' indicates 9 velocities per grid cell.
** 'bgk' refers to the Bhatnagar-Gross-Krook collision step.
**
** The 'speeds' in each cell are numbered as follows:
**
** 6 2 5
**  \|/
** 3-0-1
**  /|\
** 7 4 8
**
** A 2D grid:
**
**           cols
**       --- --- ---
**      | D | E | F |
** rows  --- --- ---
**      | A | B | C |
**       --- --- ---
**
** 'unwrapped' in row major order to give a 1D array:
**
**  --- --- --- --- --- ---
** | A | B | C | D | E | F |
**  --- --- --- --- --- ---
**
** Grid indicies are:
**
**          ny
**          ^       cols(jj)
**          |  ----- ----- -----
**          | | ... | ... | etc |
**          |  ----- ----- -----
** rows(ii) | | 1,0 | 1,1 | 1,2 |
**          |  ----- ----- -----
**          | | 0,0 | 0,1 | 0,2 |
**          |  ----- ----- -----
**          ----------------------> nx
**
** Note the names of the input parameter and obstacle files
** are passed on the command line, e.g.:
**
**   d2q9-bgk.exe input.params obstacles.dat
**
** Be sure to adjust the grid dimensions in the parameter file
** if you choose a different obstacle file.
*/

#include<stdio.h>
#include<stdlib.h>
#include<math.h>
#include<time.h>
#include<sys/time.h>
#include<sys/resource.h>
#include <xmmintrin.h>
#include <omp.h>

#define NSPEEDS         9
#define FINALSTATEFILE  "final_state.dat"
#define AVVELSFILE      "av_vels.dat"

/* struct to hold the parameter values */
typedef struct
{
  int    nx;            /* no. of cells in x-direction */
  int    ny;            /* no. of cells in y-direction */
  int    maxIters;      /* no. of iterations */
  int    reynolds_dim;  /* dimension for Reynolds number */
  float density;       /* density per link */
  float accel;         /* density redistribution */
  float omega;         /* relaxation parameter */
} t_param;

/* struct to hold the 'speed' temporary values and calculated derivatives */
typedef struct
{
    float local_density;
    float u_x;
    float u_y;
} t_speed_temp;

/*
** function prototypes
*/

/* load params, allocate memory, load obstacles & initialise fluid particle densities */
int initialise(const char* paramfile, const char* obstaclefile,
               t_param* params, t_speed_temp** tmp_cells_ptr,
               int** obstacles_ptr, float** av_vels_ptr, float** contiguous_speeds, float** contiguous_speeds_tmp);

/*
** The main calculation methods.
** timestep calls, in order, the functions:
** accelerate_flow(), propagate(), rebound() & rebound_and_collision()
*/
void timestep(const t_param params, t_speed_temp* tmp_cells, int* obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp);
void accelerate_flow(const t_param params, int* obstacles, float* contiguous_speeds);
void propagate(const t_param params, t_speed_temp* tmp_cells, float* contiguous_speeds, float* contiguous_speeds_tmp);
void rebound_and_collision(const t_param params, t_speed_temp *tmp_cells, int *obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp);
int write_values(const t_param params, int* obstacles, float* av_vels, float* contiguous_speeds);

/* finalise, including freeing up allocated memory */
int finalise(const t_param* params, t_speed_temp** tmp_cells_ptr,
             int** obstacles_ptr, float** av_vels_ptr, float** contiguous_speeds, float** contiguous_speeds_tmp);

/* Sum all the densities in the grid.
** The total should remain constant from one timestep to the next. */
float total_density(const t_param params, float* contiguous_speeds);

/* compute average velocity */
float av_velocity(const t_param params, int* obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp);

/* calculate Reynolds number */
float calc_reynolds(const t_param params, int* obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp);

/* utility functions */
void die(const char* message, const int line, const char* file);
void usage(const char* exe);

inline float fast_sqrt(float fIn) {
  if (fIn == 0) { return 0.0f; }
  float fOut;
  _mm_store_ss(&fOut, _mm_mul_ss(_mm_load_ss(&fIn), _mm_rsqrt_ss(_mm_load_ss( &fIn ))));
  return fOut;
}

int tot_cells = 0;

/* accelerate_flow() constants: */
/* weighting factors */
float accelerate_flow_w1, accelerate_flow_w2;
/* 2nd row of the grid */
int accelerate_flow_ii;

/*
** main program:
** initialise, timestep loop, finalise
*/
int main(int argc, char* argv[])
{
  char*    paramfile = NULL;    /* name of the input parameter file */
  char*    obstaclefile = NULL; /* name of a the input obstacle file */
  t_param  params;              /* struct to hold parameter values */
  t_speed_temp* tmp_cells = NULL;    /* scratch space */
  float* contiguous_speeds = NULL; /* grid containing fluid densities */
  float* contiguous_speeds_tmp = NULL;
  int*     obstacles = NULL;    /* grid indicating which cells are blocked */
  float* av_vels   = NULL;     /* a record of the av. velocity computed for each timestep */
  struct timeval timstr;        /* structure to hold elapsed time */
  struct rusage ru;             /* structure to hold CPU time--system and user */
  double tic, toc;              /* floating point numbers to calculate elapsed wallclock time */
  double usrtim;                /* floating point number to record elapsed user CPU time */
  double systim;                /* floating point number to record elapsed system CPU time */

  /* parse the command line */
  if (argc != 3)
  {
    usage(argv[0]);
  }
  else
  {
    paramfile = argv[1];
    obstaclefile = argv[2];
  }

  /* initialise our data structures and load values from file */
  initialise(paramfile, obstaclefile, &params, &tmp_cells, &obstacles, &av_vels, &contiguous_speeds, &contiguous_speeds_tmp);

  /* iterate for maxIters timesteps */
  gettimeofday(&timstr, NULL);
  tic = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  for (int ii = 0; ii < params.nx * params.ny; ii++) {
    if (!obstacles[ii]) {
      tot_cells++;
    }
  }

  /* set up accelerate_flow() constants */
  accelerate_flow_w1 = params.density * params.accel / 9.0f;
  accelerate_flow_w2 = params.density * params.accel / 36.0f;
  accelerate_flow_ii = params.ny - 2;

  for (int tt = 0; tt < params.maxIters; tt++)
  {
    timestep(params, tmp_cells, obstacles, contiguous_speeds, contiguous_speeds_tmp);
    av_vels[tt] = av_velocity(params, obstacles, contiguous_speeds, contiguous_speeds_tmp);
#ifdef DEBUG
    printf("==timestep: %d==\n", tt);
    printf("av velocity: %.12E\n", av_vels[tt]);
    printf("tot density: %.12E\n", total_density(params, cells));
#endif
  }

  gettimeofday(&timstr, NULL);
  toc = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
  getrusage(RUSAGE_SELF, &ru);
  timstr = ru.ru_utime;
  usrtim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);
  timstr = ru.ru_stime;
  systim = timstr.tv_sec + (timstr.tv_usec / 1000000.0);

  /* write final values and free memory */
  printf("==done==\n");
  printf("Reynolds number:\t\t%.12E\n", calc_reynolds(params, obstacles, contiguous_speeds, contiguous_speeds_tmp));
  printf("Elapsed time:\t\t\t%.6lf (s)\n", toc - tic);
  printf("Elapsed user CPU time:\t\t%.6lf (s)\n", usrtim);
  printf("Elapsed system CPU time:\t%.6lf (s)\n", systim);
  //printf("Num, max num of threads:\t%d\t%d\n", omp_get_num_threads(), omp_get_max_threads());
  write_values(params, obstacles, av_vels, contiguous_speeds);
  finalise(&params, &tmp_cells, &obstacles, &av_vels, &contiguous_speeds, &contiguous_speeds_tmp);

  return EXIT_SUCCESS;
}

void timestep(const t_param params, t_speed_temp* tmp_cells, int* obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp)
{
  accelerate_flow(params, obstacles, contiguous_speeds);
  propagate(params, tmp_cells, contiguous_speeds, contiguous_speeds_tmp);
  rebound_and_collision(params, tmp_cells, obstacles, contiguous_speeds, contiguous_speeds_tmp);
}

void accelerate_flow(const t_param params, int* obstacles, float* contiguous_speeds)
{

#pragma omp parallel for
  for (int jj = 0; jj < params.nx; jj++)
  {
    /* if the cell is not occupied and
    ** we don't send a negative density */
    if (!obstacles[accelerate_flow_ii * params.nx + jj]
        && (contiguous_speeds[3 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] - accelerate_flow_w1) > 0.0
        && (contiguous_speeds[6 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] - accelerate_flow_w2) > 0.0
        && (contiguous_speeds[7 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] - accelerate_flow_w2) > 0.0)
    {
      /* increase 'east-side' densities */
      contiguous_speeds[1 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] += accelerate_flow_w1;
      contiguous_speeds[5 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] += accelerate_flow_w2;
      contiguous_speeds[8 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] += accelerate_flow_w2;
      /* decrease 'west-side' densities */
      contiguous_speeds[3 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] -= accelerate_flow_w1;
      contiguous_speeds[6 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] -= accelerate_flow_w2;
      contiguous_speeds[7 * params.nx * params.ny + accelerate_flow_ii * params.nx + jj] -= accelerate_flow_w2;
    }
  }
}

void propagate(const t_param params, t_speed_temp* tmp_cells, float* contiguous_speeds, float* contiguous_speeds_tmp)
{
  /* loop over _all_ cells */
#pragma omp parallel for
  for (int ii = 0; ii < params.ny; ii++)
  {
    for (int jj = 0; jj < params.nx; jj++)
    {
      /* determine indices of axis-direction neighbours
      ** respecting periodic boundary conditions (wrap around) */
      int y_n = (ii + 1) % params.ny;
      int x_e = (jj + 1) % params.nx;
      int y_s = (ii == 0) ? (ii + params.ny - 1) : (ii - 1);
      int x_w = (jj == 0) ? (jj + params.nx - 1) : (jj - 1);
      /* propagate densities to neighbouring cells, following
      ** appropriate directions of travel and writing into
      ** scratch space grid */
      contiguous_speeds_tmp[0 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[0 * params.nx * params.ny + ii * params.nx + jj];
      contiguous_speeds_tmp[1 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[1 * params.nx * params.ny + ii * params.nx + x_w];
      contiguous_speeds_tmp[2 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[2 * params.nx * params.ny + y_s * params.nx + jj];
      contiguous_speeds_tmp[3 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[3 * params.nx * params.ny + ii * params.nx + x_e];
      contiguous_speeds_tmp[4 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[4 * params.nx * params.ny + y_n * params.nx + jj];
      contiguous_speeds_tmp[5 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[5 * params.nx * params.ny + y_s * params.nx + x_w];
      contiguous_speeds_tmp[6 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[6 * params.nx * params.ny + y_s * params.nx + x_e];
      contiguous_speeds_tmp[7 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[7 * params.nx * params.ny + y_n * params.nx + x_e];
      contiguous_speeds_tmp[8 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds[8 * params.nx * params.ny + y_n * params.nx + x_w];

      /* compute local density total */
      tmp_cells[ii * params.nx + jj].local_density = contiguous_speeds_tmp[0 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[1 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[2 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[3 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[4 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[5 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[6 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[7 * params.nx * params.ny + ii * params.nx + jj]
                                                 + contiguous_speeds_tmp[8 * params.nx * params.ny + ii * params.nx + jj];

      /* compute x velocity component */
      tmp_cells[ii * params.nx + jj].u_x = (contiguous_speeds_tmp[1 * params.nx * params.ny + ii * params.nx + jj]
                                            + contiguous_speeds_tmp[5 * params.nx * params.ny + ii * params.nx + jj]
                                            + contiguous_speeds_tmp[8 * params.nx * params.ny + ii * params.nx + jj]
                                            - (contiguous_speeds_tmp[3 * params.nx * params.ny + ii * params.nx + jj]
                                               + contiguous_speeds_tmp[6 * params.nx * params.ny + ii * params.nx + jj]
                                               + contiguous_speeds_tmp[7 * params.nx * params.ny + ii * params.nx + jj]))
                                           / tmp_cells[ii * params.nx + jj].local_density;

      /* compute y velocity component */
      tmp_cells[ii * params.nx + jj].u_y = (contiguous_speeds_tmp[2 * params.nx * params.ny + ii * params.nx + jj]
                                            + contiguous_speeds_tmp[5 * params.nx * params.ny + ii * params.nx + jj]
                                            + contiguous_speeds_tmp[6 * params.nx * params.ny + ii * params.nx + jj]
                                            - (contiguous_speeds_tmp[4 * params.nx * params.ny + ii * params.nx + jj]
                                               + contiguous_speeds_tmp[7 * params.nx * params.ny + ii * params.nx + jj]
                                               + contiguous_speeds_tmp[8 * params.nx * params.ny + ii * params.nx + jj]))
                                           / tmp_cells[ii * params.nx + jj].local_density;

    }
  }
}

void rebound_and_collision(const t_param params, t_speed_temp *tmp_cells, int *obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp)
{
  static const float w0 = 4.0f / 9.0f;  /* weighting factor */
  static const float w1 = 1.0f / 9.0f;  /* weighting factor */
  static const float w2 = 1.0f / 36.0f; /* weighting factor */

  /* loop over the cells in the grid
  ** NB the collision step is called after
  ** the propagate step and so values of interest
  ** are in the scratch-space grid */
#pragma omp parallel for
  for (int ii = 0; ii < params.ny; ii++)
  {
    for (int jj = 0; jj < params.nx; jj++)
    {
      /* don't consider occupied cells */
      if (!obstacles[ii * params.nx + jj])
      {
        /* equilibrium densities */
        float d_equ[NSPEEDS];
        /* zero velocity density: weight w0 */
        d_equ[0] = w0 * tmp_cells[ii * params.nx + jj].local_density * (1.0f - (tmp_cells[ii * params.nx + jj].u_x * tmp_cells[ii * params.nx + jj].u_x + tmp_cells[ii * params.nx + jj].u_y * tmp_cells[ii * params.nx + jj].u_y) * 1.5f);
        /* axis speeds: weight w1 */
        d_equ[1] = w1 * tmp_cells[ii * params.nx + jj].local_density * (tmp_cells[ii * params.nx + jj].u_x * (3.0f * tmp_cells[ii * params.nx + jj].u_x + 3.0f) - 1.5f * tmp_cells[ii * params.nx + jj].u_y * tmp_cells[ii * params.nx + jj].u_y + 1.0f);
        d_equ[2] = w1 * tmp_cells[ii * params.nx + jj].local_density * (-1.5f * tmp_cells[ii * params.nx + jj].u_x * tmp_cells[ii * params.nx + jj].u_x + tmp_cells[ii * params.nx + jj].u_y * (3.0f * tmp_cells[ii * params.nx + jj].u_y + 3.0f) + 1.0f);
        d_equ[3] = w1 * tmp_cells[ii * params.nx + jj].local_density * (tmp_cells[ii * params.nx + jj].u_x * (3.0f * tmp_cells[ii * params.nx + jj].u_x - 3.0f) - 1.5f * tmp_cells[ii * params.nx + jj].u_y * tmp_cells[ii * params.nx + jj].u_y + 1.0f);
        d_equ[4] = w1 * tmp_cells[ii * params.nx + jj].local_density * (-1.5f * tmp_cells[ii * params.nx + jj].u_x * tmp_cells[ii * params.nx + jj].u_x + tmp_cells[ii * params.nx + jj].u_y * (3.0f * tmp_cells[ii * params.nx + jj].u_y - 3.0f) + 1.0f);
        /* diagonal speeds: weight w2 */
        d_equ[5] = w2 * tmp_cells[ii * params.nx + jj].local_density * (tmp_cells[ii * params.nx + jj].u_x * (3.0f * tmp_cells[ii * params.nx + jj].u_x + 9.0f * tmp_cells[ii * params.nx + jj].u_y + 3.0f) + tmp_cells[ii * params.nx + jj].u_y * (3.0f * tmp_cells[ii * params.nx + jj].u_y + 3.0f) + 1.0f);
        d_equ[6] = w2 * tmp_cells[ii * params.nx + jj].local_density * (tmp_cells[ii * params.nx + jj].u_y * (-9.0f * tmp_cells[ii * params.nx + jj].u_x + 3.0f * tmp_cells[ii * params.nx + jj].u_y + 3.0f) + tmp_cells[ii * params.nx + jj].u_x * (3.0f * tmp_cells[ii * params.nx + jj].u_x - 3.0f) + 1.0f);
        d_equ[7] = w2 * tmp_cells[ii * params.nx + jj].local_density * (tmp_cells[ii * params.nx + jj].u_x * (3.0f * tmp_cells[ii * params.nx + jj].u_x + 9.0f * tmp_cells[ii * params.nx + jj].u_y - 3.0f) + tmp_cells[ii * params.nx + jj].u_y * (3.0f * tmp_cells[ii * params.nx + jj].u_y - 3.0f) + 1.0f);
        d_equ[8] = w2 * tmp_cells[ii * params.nx + jj].local_density * (tmp_cells[ii * params.nx + jj].u_y * (-9.0f * tmp_cells[ii * params.nx + jj].u_x + 3.0f * tmp_cells[ii * params.nx + jj].u_y - 3.0f) + tmp_cells[ii * params.nx + jj].u_x * (3.0f * tmp_cells[ii * params.nx + jj].u_x + 3.0f) + 1.0f);

        /* relaxation step */
        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          contiguous_speeds[kk * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[kk * params.nx * params.ny + ii * params.nx + jj]
                  + params.omega
                  * (d_equ[kk] - contiguous_speeds_tmp[kk * params.nx * params.ny + ii * params.nx + jj]);
        }
      } else {
        /* called after propagate, so taking values from scratch space
        ** mirroring, and writing into main grid */
        contiguous_speeds[1 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[3 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[2 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[4 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[3 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[1 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[4 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[2 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[5 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[7 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[6 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[8 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[7 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[5 * params.nx * params.ny + ii * params.nx + jj];
        contiguous_speeds[8 * params.nx * params.ny + ii * params.nx + jj] = contiguous_speeds_tmp[6 * params.nx * params.ny + ii * params.nx + jj];
      }
    }
  }
}

float av_velocity(const t_param params, int* obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp)
{
  float tot_u;          /* accumulated magnitudes of velocity for each cell */

  /* initialise */
  tot_u = 0.0;
#pragma omp parallel for reduction(+:tot_u)
  /* loop over all non-blocked cells */
  for (int ii = 0; ii < params.ny; ii++)
  {
    for (int jj = 0; jj < params.nx; jj++)
    {
      /* ignore occupied cells */
      if (!obstacles[ii * params.nx + jj])
      {
        /* local density total */
        float local_density = 0.0f;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += contiguous_speeds[kk * params.nx * params.ny + ii * params.nx + jj];
        }

        /* compute x velocity component */
        float u_x = (contiguous_speeds[1 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[5 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[8 * params.nx * params.ny + ii * params.nx + jj]
               - (contiguous_speeds[3 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[6 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[7 * params.nx * params.ny + ii * params.nx + jj]))
              / local_density;
        /* compute y velocity component */
        float u_y = (contiguous_speeds[2 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[5 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[6 * params.nx * params.ny + ii * params.nx + jj]
               - (contiguous_speeds[4 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[7 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[8 * params.nx * params.ny + ii * params.nx + jj]))
              / local_density;
        /* accumulate the norm of x- and y- velocity components */
        tot_u += fast_sqrt((float) ((u_x * u_x) + (u_y * u_y)));
      }
    }
  }

  return tot_u / (float)tot_cells;
}

int initialise(const char* paramfile, const char* obstaclefile,
               t_param* params, t_speed_temp** tmp_cells_ptr,
               int** obstacles_ptr, float** av_vels_ptr, float** contiguous_speeds, float** contiguous_speeds_tmp)
{
  char   message[1024];  /* message buffer */
  FILE*   fp;            /* file pointer */
  int    xx, yy;         /* generic array indices */
  int    blocked;        /* indicates whether a cell is blocked by an obstacle */
  int    retval;         /* to hold return value for checking */

  /* open the parameter file */
  fp = fopen(paramfile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input parameter file: %s", paramfile);
    die(message, __LINE__, __FILE__);
  }

  /* read in the parameter values */
  retval = fscanf(fp, "%d\n", &(params->nx));

  if (retval != 1) die("could not read param file: nx", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->ny));

  if (retval != 1) die("could not read param file: ny", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->maxIters));

  if (retval != 1) die("could not read param file: maxIters", __LINE__, __FILE__);

  retval = fscanf(fp, "%d\n", &(params->reynolds_dim));

  if (retval != 1) die("could not read param file: reynolds_dim", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->density));

  if (retval != 1) die("could not read param file: density", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->accel));

  if (retval != 1) die("could not read param file: accel", __LINE__, __FILE__);

  retval = fscanf(fp, "%f\n", &(params->omega));

  if (retval != 1) die("could not read param file: omega", __LINE__, __FILE__);

  /* and close up the file */
  fclose(fp);

  /*
  ** Allocate memory.
  **
  ** Remember C is pass-by-value, so we need to
  ** pass pointers into the initialise function.
  **
  ** NB we are allocating a 1D array, so that the
  ** memory will be contiguous.  We still want to
  ** index this memory as if it were a (row major
  ** ordered) 2D array, however.  We will perform
  ** some arithmetic using the row and column
  ** coordinates, inside the square brackets, when
  ** we want to access elements of this array.
  **
  ** Note also that we are using a structure to
  ** hold an array of 'speeds'.  We will allocate
  ** a 1D array of these structs.
  */

  /* 'helper' grid, used as scratch space */
  *tmp_cells_ptr = (t_speed_temp*)malloc(sizeof(t_speed_temp) * (params->ny * params->nx));

  if (*tmp_cells_ptr == NULL) die("cannot allocate memory for tmp_cells", __LINE__, __FILE__);

  /* the map of obstacles */
  *obstacles_ptr = malloc(sizeof(int) * (params->ny * params->nx));

  if (*obstacles_ptr == NULL) die("cannot allocate column memory for obstacles", __LINE__, __FILE__);

  /* the contiguous speeds arrays */
  *contiguous_speeds = malloc(sizeof(float) * (params->ny * params->nx * NSPEEDS));

  if (*contiguous_speeds == NULL) die("cannot allocate memory for contiguous_speeds", __LINE__, __FILE__);

  /* the contiguous speeds arrays */
  *contiguous_speeds_tmp = malloc(sizeof(float) * (params->ny * params->nx * NSPEEDS));

  if (*contiguous_speeds_tmp == NULL) die("cannot allocate memory for contiguous_speeds_tmp", __LINE__, __FILE__);

  /* initialise densities */
  float w0 = params->density * 4.0f / 9.0f;
  float w1 = params->density      / 9.0f;
  float w2 = params->density      / 36.0f;

  for (int ii = 0; ii < params->ny; ii++)
  {
    for (int jj = 0; jj < params->nx; jj++)
    {
      /* centre */
      (*contiguous_speeds)[0 * params->nx * params->ny + ii * params->nx + jj] = w0;
      /* axis directions */
      (*contiguous_speeds)[1 * params->nx * params->ny + ii * params->nx + jj] = w1;
      (*contiguous_speeds)[2 * params->nx * params->ny + ii * params->nx + jj] = w1;
      (*contiguous_speeds)[3 * params->nx * params->ny + ii * params->nx + jj] = w1;
      (*contiguous_speeds)[4 * params->nx * params->ny + ii * params->nx + jj] = w1;
      /* diagonals */
      (*contiguous_speeds)[5 * params->nx * params->ny + ii * params->nx + jj] = w2;
      (*contiguous_speeds)[6 * params->nx * params->ny + ii * params->nx + jj] = w2;
      (*contiguous_speeds)[7 * params->nx * params->ny + ii * params->nx + jj] = w2;
      (*contiguous_speeds)[8 * params->nx * params->ny + ii * params->nx + jj] = w2;
    }
  }

  /* first set all cells in obstacle array to zero */
  for (int ii = 0; ii < params->ny; ii++)
  {
    for (int jj = 0; jj < params->nx; jj++)
    {
      (*obstacles_ptr)[ii * params->nx + jj] = 0;
    }
  }

  /* open the obstacle data file */
  fp = fopen(obstaclefile, "r");

  if (fp == NULL)
  {
    sprintf(message, "could not open input obstacles file: %s", obstaclefile);
    die(message, __LINE__, __FILE__);
  }

  /* read-in the blocked cells list */
  while ((retval = fscanf(fp, "%d %d %d\n", &xx, &yy, &blocked)) != EOF)
  {
    /* some checks */
    if (retval != 3) die("expected 3 values per line in obstacle file", __LINE__, __FILE__);

    if (xx < 0 || xx > params->nx - 1) die("obstacle x-coord out of range", __LINE__, __FILE__);

    if (yy < 0 || yy > params->ny - 1) die("obstacle y-coord out of range", __LINE__, __FILE__);

    if (blocked != 1) die("obstacle blocked value should be 1", __LINE__, __FILE__);

    /* assign to array */
    (*obstacles_ptr)[yy * params->nx + xx] = blocked;
  }

  /* and close the file */
  fclose(fp);

  /*
  ** allocate space to hold a record of the avarage velocities computed
  ** at each timestep
  */
  *av_vels_ptr = (float*)malloc(sizeof(float) * params->maxIters);

  return EXIT_SUCCESS;
}

int finalise(const t_param* params, t_speed_temp** tmp_cells_ptr,
             int** obstacles_ptr, float** av_vels_ptr, float** contiguous_speeds, float** contiguous_speeds_tmp)
{
  /*
  ** free up allocated memory
  */

  free(*tmp_cells_ptr);
  *tmp_cells_ptr = NULL;

  free(*obstacles_ptr);
  *obstacles_ptr = NULL;

  free(*av_vels_ptr);
  *av_vels_ptr = NULL;

  free(*contiguous_speeds);
  *contiguous_speeds = NULL;

  free(*contiguous_speeds_tmp);
  *contiguous_speeds_tmp = NULL;

  return EXIT_SUCCESS;
}


float calc_reynolds(const t_param params, int* obstacles, float* contiguous_speeds, float* contiguous_speeds_tmp)
{
  const float viscosity = 1.0f / 6.0f * (2.0f / params.omega - 1.0f);

  return av_velocity(params, obstacles, contiguous_speeds, contiguous_speeds_tmp) * params.reynolds_dim / viscosity;
}

float total_density(const t_param params, float* contiguous_speeds)
{
  float total = 0.0f;  /* accumulator */

  for (int ii = 0; ii < params.ny; ii++)
  {
    for (int jj = 0; jj < params.nx; jj++)
    {
      for (int kk = 0; kk < NSPEEDS; kk++)
      {
        total += contiguous_speeds[kk * params.nx * params.ny + ii * params.nx + jj];
      }
    }
  }

  return total;
}

int write_values(const t_param params, int* obstacles, float* av_vels, float* contiguous_speeds)
{
  FILE* fp;                     /* file pointer */
  const float c_sq = 1.0f / 3.0f; /* sq. of speed of sound */
  float local_density;         /* per grid cell sum of densities */
  float pressure;              /* fluid pressure in grid cell */
  float u_x;                   /* x-component of velocity in grid cell */
  float u_y;                   /* y-component of velocity in grid cell */
  float u;                     /* norm--root of summed squares--of u_x and u_y */

  fp = fopen(FINALSTATEFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int ii = 0; ii < params.ny; ii++)
  {
    for (int jj = 0; jj < params.nx; jj++)
    {
      /* an occupied cell */
      if (obstacles[ii * params.nx + jj])
      {
        u_x = u_y = u = 0.0;
        pressure = params.density * c_sq;
      }
      /* no obstacle */
      else
      {
        local_density = 0.0;

        for (int kk = 0; kk < NSPEEDS; kk++)
        {
          local_density += contiguous_speeds[kk * params.nx * params.ny + ii * params.nx + jj];
        }

        /* compute x velocity component */
        u_x = (contiguous_speeds[1 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[5 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[8 * params.nx * params.ny + ii * params.nx + jj]
               - (contiguous_speeds[3 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[6 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[7 * params.nx * params.ny + ii * params.nx + jj]))
              / local_density;
        /* compute y velocity component */
        u_y = (contiguous_speeds[2 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[5 * params.nx * params.ny + ii * params.nx + jj]
               + contiguous_speeds[6 * params.nx * params.ny + ii * params.nx + jj]
               - (contiguous_speeds[4 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[7 * params.nx * params.ny + ii * params.nx + jj]
                  + contiguous_speeds[8 * params.nx * params.ny + ii * params.nx + jj]))
              / local_density;
        /* compute norm of velocity */
        u = fast_sqrt((u_x * u_x) + (u_y * u_y));
        /* compute pressure */
        pressure = local_density * c_sq;
      }

      /* write to file */
      fprintf(fp, "%d %d %.12E %.12E %.12E %.12E %d\n", jj, ii, u_x, u_y, u, pressure, obstacles[ii * params.nx + jj]);
    }
  }

  fclose(fp);

  fp = fopen(AVVELSFILE, "w");

  if (fp == NULL)
  {
    die("could not open file output file", __LINE__, __FILE__);
  }

  for (int ii = 0; ii < params.maxIters; ii++)
  {
    fprintf(fp, "%d:\t%.12E\n", ii, av_vels[ii]);
  }

  fclose(fp);

  return EXIT_SUCCESS;
}

void die(const char* message, const int line, const char* file)
{
  fprintf(stderr, "Error at line %d of file %s:\n", line, file);
  fprintf(stderr, "%s\n", message);
  fflush(stderr);
  exit(EXIT_FAILURE);
}

void usage(const char* exe)
{
  fprintf(stderr, "Usage: %s <paramfile> <obstaclefile>\n", exe);
  exit(EXIT_FAILURE);
}

