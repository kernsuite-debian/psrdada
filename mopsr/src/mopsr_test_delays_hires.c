/***************************************************************************
 *
 *    Copyright (C) 2013 by Andrew Jameson
 *    Licensed under the Academic Free License version 2.1
 *
 ****************************************************************************/

#include "dada_cuda.h"
#include "mopsr_delays_hires.h"
#include "mopsr_cuda.h"
#include "mopsr_delays_cuda_hires.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <math.h>
#include <cuda_runtime.h>

#define SINGLE
//#define SKZAP

void usage ()
{
  fprintf(stdout, "mopsr_test_delays_hires bays_file modules_file\n"
    " -a nant     number of antennae\n"
    " -c nchan    number of channels\n"
    " -t nsamp    number of samples\n"
    " -h          print this help text\n"
    " -v          verbose output\n"
  );
}

int main(int argc, char** argv)
{
  int arg = 0;
  unsigned nchan = 320;
  unsigned nant = 16;
  unsigned ndim = 2;
  unsigned ntap = 25;
  unsigned nsamp = 16384;
  unsigned s1_memory = 8;
  uint64_t s1_count = 1;

  char verbose = 0;
  char replace_noise = 1;

  int device = 0;

  while ((arg = getopt(argc, argv, "a:c:d:ht:v")) != -1)
  {
    switch (arg)
    {
      case 'a':
        nant = atoi(optarg);
        break;

      case 'c':
        nchan = atoi(optarg);
        break;

      case 'd':
        device = atoi (optarg);
        break;

      case 'h':
        usage ();
        return 0;

      case 't':
        nsamp = atoi (optarg);
        break;

      case 'v':
        verbose ++;
        break;

      default:
        usage ();
        return 0;
    }
  }

  // check and parse the command line arguments
  if (argc-optind != 2)
  {
    fprintf(stderr, "ERROR: 2 argument are required\n");
    usage();
    exit(EXIT_FAILURE);
  }

  unsigned isamp, ichan, iant, imod;;

  unsigned nbay;
  char * bays_file = strdup (argv[optind]);
  mopsr_bay_t * all_bays = read_bays_file (bays_file, &nbay);
  if (!all_bays)
  {
    fprintf (stderr, "ERROR: failed to read bays file [%s]\n", bays_file);
    return EXIT_FAILURE;
  }

  int nmod;
  char * modules_file = strdup (argv[optind+1]);
  mopsr_module_t * modules = read_modules_file (modules_file, &nmod);
  if (!modules)
  {
    fprintf (stderr, "ERROR: failed to read modules file [%s]\n", modules_file);
    return EXIT_FAILURE;
  }

  // preconfigure a source
  mopsr_source_t source;
  sprintf (source.name, "J0437-4715");
  source.raj  = 1.17635;
  source.decj = 0.65013;
  source.raj  = 4.82147205863433;

  mopsr_chan_t * channels = (mopsr_chan_t *) malloc(sizeof(mopsr_chan_t) * nchan);
  double chan_bw = 0.78125 / 8;
  for (ichan=0; ichan<nchan; ichan++)
  {
    channels[ichan].number = ichan;
    channels[ichan].bw     = chan_bw;
    channels[ichan].cfreq  = (800 + (chan_bw/2) + (chan_bw * ichan));
  }

  struct timeval timestamp;

  mopsr_delay_hires_t ** delays = (mopsr_delay_hires_t **) malloc(sizeof(mopsr_delay_hires_t *) * nmod);
  for (imod=0; imod<nmod; imod++)
    delays[imod] = (mopsr_delay_hires_t *) malloc (sizeof(mopsr_delay_hires_t) * nchan);

  // determine the timestamp corresponding to the current byte of data
  timestamp.tv_sec = (long int) time(NULL);
  timestamp.tv_usec = 0;

  timestamp.tv_sec += 60;

  // cuda setup
  // select the gpu device
  int n_devices = dada_cuda_get_device_count();
  fprintf (stderr, "Detected %d CUDA devices\n", n_devices);

  if ((device < 0) && (device >= n_devices))
  {
    fprintf (stderr, "no CUDA devices available [%d]\n",
              n_devices);
    return -1;
  }

  fprintf (stderr, "main: dada_cuda_select_device(%d)\n", device);
  if (dada_cuda_select_device (device) < 0)
  {
    fprintf (stderr, "could not select requested device [%d]\n", device);
    return -1;
  }

  char * device_name = dada_cuda_get_device_name (device);
  if (!device_name)
  {
    fprintf (stderr, "could not get CUDA device name\n");
    return -1;
  }
  fprintf (stderr, "Using device %d : %s\n", device, device_name);
  free(device_name);

  // setup the cuda stream for operations
  cudaStream_t stream;
  fprintf (stderr, "main: cudaStreamCreate()\n");
  cudaError_t error = cudaStreamCreate(&stream);
  fprintf (stderr, "main: stream=%p\n", stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "could not create CUDA stream\n");
    return -1;
  }

  unsigned nbytes_per_samp = nchan * nant * ndim;
  const uint64_t nbytes = nsamp * nbytes_per_samp;

  fprintf (stderr, "nchan=%u nant=%u nsamp=%"PRIu64" nbytes=%"PRIu64"\n", nchan, nant, nsamp, nbytes);

  void * d_sample_delayed;
  void * d_fractional_delayed;

  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_fractional_delayed\n", nbytes);
  error = cudaMalloc(&d_fractional_delayed, nbytes);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMalloc failed for %ld bytes\n", nbytes);
    return -1;
  }
    fprintf (stderr, "main: d_fractional_delayed=%p\n", d_fractional_delayed);

  void * h_in;
  fprintf (stderr, "main: cudaMallocHost(%"PRIu64") for h_in\n", nbytes * 2);
  error = cudaMallocHost (&h_in, nbytes * 2);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocHost failed for %ld bytes\n", nbytes * 2);
    return -1;
  }

  complex float * unpacked = (complex float *) malloc (sizeof(complex float) * nsamp * 2);
  complex float * tapped   = (complex float *) malloc (sizeof(complex float) * nsamp * 2);
  int8_t * delayed8 = (int8_t *) malloc (nbytes * 2);

  void * d_in;
  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_in\n", nbytes);
  error = cudaMalloc (&d_in, nbytes);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMalloc failed for %ld bytes\n", nbytes);
    return -1;
  }

  void * d_fbuf;
  size_t d_fbuf_size = nbytes * sizeof(float);
  fprintf (stderr, "main: allocating %ld bytes of device memory for d_fbuf\n", d_fbuf_size);
  error = cudaMalloc (&d_fbuf, d_fbuf_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: could not allocate %ld bytes of device memory\n", d_fbuf_size);
    return -1;
  }

  fprintf (stderr, "hires_curandState_size()=%ld\n", hires_curandState_size());

  size_t maxthreads = 1024;
  const unsigned nrngs = nchan * nant * maxthreads;
  size_t d_curand_size = (size_t) (nrngs * hires_curandState_size());
  void * d_rstates = 0;
  if (verbose)
   fprintf (stderr, "alloc: allocating %ld bytes of device memory for d_rstates\n", d_curand_size);
  error = cudaMalloc (&d_rstates, d_curand_size);
  if (verbose)
    fprintf (stderr, "alloc: d_rstates=%p\n", d_rstates);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "alloc: could not allocate %ld bytes of device memory\n", d_curand_size);
    return -1;
  }

  unsigned long long seed = (unsigned long long) (0);
  hires_init_rng_sparse (stream, seed, nrngs, 0, 1, d_rstates);

  size_t d_sigmas_size = (size_t) (nchan * nant * sizeof(float));
  void * d_sigmas = 0;
  if (verbose)
    fprintf (stderr, "alloc: allocating %ld bytes of device memory for d_sigmas\n", d_sigmas_size);
  error = cudaMalloc (&d_sigmas, d_sigmas_size);
  if (verbose)
    fprintf (stderr, "alloc: d_sigmas=%p\n", d_sigmas);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "alloc: could not allocate %ld bytes of device memory\n", d_sigmas_size);
    return -1;
  }
  cudaMemsetAsync(d_sigmas, 0, d_sigmas_size, stream);

  size_t d_mask_size = (size_t) (nchan * nant * sizeof(unsigned) * nsamp) / 1024;
  void * d_mask = 0;
  if (verbose)
    fprintf (stderr, "alloc: allocating %ld bytes of device memory for d_mask\n", d_mask_size);
  error = cudaMalloc (&d_mask, d_mask_size);
  if (verbose)
    fprintf (stderr, "alloc: d_mask=%p\n", d_mask);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "alloc: could not allocate %ld bytes of device memory\n", d_mask_size);
    return -1;
  }
  cudaMemsetAsync(d_mask, 0, d_mask_size, stream);

  size_t ant_scales_size = nant * sizeof(float);
  float * h_ant_scales;
  error = cudaMallocHost ((void **) &h_ant_scales, ant_scales_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: could not allocate %ld bytes of host memory\n", ant_scales_size);
    return -1;
  }
  void * d_ant_scales;
  error = cudaMalloc ((void **) &d_ant_scales, ant_scales_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: could not allocate %ld bytes of device memory\n", ant_scales_size);
    return -1;
  }

  float * h_fractional_delays;
  size_t delays_size = sizeof(float) * nchan * nant;
  fprintf (stderr, "main: cudaMallocHost(%"PRIu64") for h_fractional_delays\n", delays_size);
  error = cudaMallocHost ((void **) &h_fractional_delays, delays_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocHost failed for %ld bytes\n", delays_size);
    return -1;
  }

  float * h_fringe_coeffs;
  size_t fringes_size = sizeof(float) * nchan * nant;
  fprintf (stderr, "main: cudaMallocHost(%"PRIu64") for h_fringe_coeffs\n", fringes_size);
  error = cudaMallocHost ((void **) &h_fringe_coeffs, fringes_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocHost failed for %ld bytes\n", fringes_size);
    return -1;
  }
  float * d_fringe_coeffs;
  error = cudaMalloc ((void **) &d_fringe_coeffs, fringes_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMalloc failed for %ld bytes\n", fringes_size);
    return -1;
  }

  void * d_fractional_delays;
  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_fractional_delays\n", delays_size);
  error = cudaMalloc(&d_fractional_delays, delays_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocfailed for %ld bytes\n", delays_size);
    return -1;
  }
  fprintf (stderr, "main: d_fractional_delays=%p\n", d_fractional_delays);

  size_t s2s_size = sizeof(float) * nchan * nant * (nsamp / 1024);
  void * d_s2s;
  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_s2s\n", s2s_size);
  error = cudaMalloc(&d_s2s, s2s_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocfailed for %ld bytes\n", s2s_size);
    return -1;
  }

  size_t s1s_size = s2s_size * s1_memory;
  void * d_s1s;
  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_s1s\n", s1s_size);
  error = cudaMalloc(&d_s1s, s1s_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocfailed for %ld bytes\n", s1s_size);
    return -1;
  }

  size_t thresh_size = nchan * nant * sizeof (float) * 2;
  void * d_thresh;
  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_thresh\n", thresh_size);
  error = cudaMalloc(&d_thresh, thresh_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocfailed for %ld bytes\n", thresh_size);
    return -1;
  }
  cudaMemsetAsync(d_thresh, 0, thresh_size, stream);

  size_t fir_coeffs_size = nchan * nant * ntap * sizeof (float);
  void * d_fir_coeffs;
  fprintf (stderr, "main: cudaMalloc(%"PRIu64") for d_fir_coeff\n", fir_coeffs_size);
  error = cudaMalloc(&d_fir_coeffs, fir_coeffs_size);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocfailed for %ld bytes\n", fir_coeffs_size);
    return -1;
  }

  void * h_out;
  size_t nbytes_out = nbytes + (ndim * nchan * nant * (ntap-1));
  fprintf (stderr, "main: cudaMallocHost(%"PRIu64") for h_out\n", nbytes * 3);
  error = cudaMallocHost (&h_out, nbytes * 3);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "main: cudaMallocHost failed for %ld bytes\n", nbytes * 3);
    return -1;
  }

  // struct for all the goodies
  transpose_delay_hires_t ctx;

  fprintf (stderr, "main: hires_transpose_delay_alloc(%ld, %ld, %ld, %ld)\n", nbytes, nchan, nant, ntap);
  if (hires_transpose_delay_alloc (&ctx, nbytes, nchan, nant, ntap) < 0)
  {
    fprintf (stderr, "hires_transpose_delay_alloc failed\n");
    return EXIT_FAILURE;
  }

  // copy the scales to the GPU
  for (iant=0; iant<nant; iant++)
    h_ant_scales[iant] = 1;
#ifdef USE_CONSTANT_MEMORY
  hires_delay_copy_scales (stream, h_ant_scales, ant_scales_size);
#else
  error = cudaMemcpyAsync (d_ant_scales, h_ant_scales, ant_scales_size, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }
#endif

  //const unsigned fixed_delay = (ntap / 2) + 1;
  const unsigned fixed_delay = 40;
  unsigned i;

  int8_t * ptr;

  // initialize data in h_in to constants for ant number
  ptr = (int8_t *) h_in;

  char apply_instrumental = 1;
  char apply_geometric = 1;
  char is_tracking = 1;
  double tsamp = 10.24;
  float start_md_angle = 0;

  // update the delays
  fprintf (stderr, "main: calculate_delays()\n");
  if (calculate_delays_hires (nbay, all_bays, nmod, modules, nchan, channels, source,
        timestamp, delays, start_md_angle, apply_instrumental,
        apply_geometric, is_tracking, tsamp) < 0)
  {
    fprintf (stderr, "main: calculate_delays() failed\n");
    return -1;
  }

  // manually set the delays
  for (iant=0; iant<nant; iant++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      // override the sample delays for testing...
      delays[iant][ichan].samples = fixed_delay + (iant);

      // set the host fractional delays
      //h_fractional_delays[ichan*nant + iant] = delays[iant][ichan].fractional;
      h_fractional_delays[ichan*nant + iant] = 0.0;

      //h_fringe_coeffs[ichan*nant + iant] = delays[iant][ichan].fringe_coeff;
      h_fringe_coeffs[ichan*nant + iant] = 0;
    }
  }

#ifndef USE_CONSTANT_MEMORY
  error = cudaMemcpyAsync (d_fringe_coeffs, h_fringe_coeffs, fringes_size, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }
#endif

//#define TEST_SAMPLES
#ifdef TEST_SAMPLES

  fprintf (stderr, "WRITING SAMPLE TESTING TO INPUT\n");
  for (isamp=0; isamp<nsamp; isamp++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        ptr[0] = (int8_t) ((((128 - fixed_delay) - iant) + (isamp)) % 128);
        ptr[1] = ptr[0];
        ptr += 2;
      }
    }
  }

  const unsigned slip = 3;

  // then for the second half have a jump of 1 number
  for (isamp=nsamp; isamp<2*nsamp; isamp++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        if (isamp < nsamp + (fixed_delay+iant))
          ptr[0] = (int8_t) ((((128 - fixed_delay) - iant) + isamp) % 128);
      else
          ptr[0] = (int8_t) ((((128 - fixed_delay) - iant) + (isamp - slip)) % 128);
        ptr[1] = ptr[0];
        ptr += 2;
      }
    }
  }

  // copy input from host to device
  fprintf (stderr, "main: cudaMemcpyAsync to=%p from=%p size=%ld\n", d_in, h_in, nbytes);
  error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }

  fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
  d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
  fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

  // copy the delays to the GPU
  fprintf (stderr, "main: copy fractional delays to GPU %d bytes\n", delays_size);
  error = cudaMemcpyAsync (d_fractional_delays, h_fractional_delays, delays_size, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }

  // now do 2 iterations
  for (i=0; i<3; i++)
  {
    size_t in_offset = nbytes;
    size_t ou_offset = i * nbytes;

    fprintf (stderr, "main: cudaMemcpyAsync to=%p from=%p size=%ld\n", d_in, h_in + in_offset, nbytes);
    error = cudaMemcpyAsync (d_in, h_in + in_offset, nbytes, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    cudaStreamSynchronize (stream);

    // manually set the delays for secondary jump
    for (iant=0; iant<nant; iant++)
    {
      for (ichan=0; ichan<nchan; ichan++)
      {
        delays[iant][ichan].samples = fixed_delay + (iant) + slip;
      }
    }

    ptr = (int8_t *) (h_in + in_offset);
    for (isamp=nsamp; isamp<2*nsamp; isamp++)
    {
      for (ichan=0; ichan<nchan; ichan++)
      {
        for (iant=0; iant<nant; iant++)
        {
          ptr[0] = (int8_t) ((((128 - fixed_delay) - iant) + (isamp - slip)) % 128);
          ptr[1] = ptr[0];
          ptr += 2;
        }
      }
    }

    fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
    d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
    fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

#ifdef SKZAP
    fprintf (stderr, "main: hires_delay_fractional_sk_scale(%ld)\n", nbytes_out);
    hires_delay_fractional_sk_scale (stream, d_sample_delayed, d_fractional_delayed, d_fbuf,
                                     d_rstates, d_sigmas, d_mask,
                                     d_fractional_delays, d_fir_coeffs, 
#ifndef USE_CONSTANT_MEMORY
                                     d_fringe_coeffs, d_ant_scales,
#endif
                                     d_s1s, d_s2s, d_thresh, 
                                     h_fringe_coeffs, fringes_size, 
                                     nbytes_out, ctx.nchan, 
                                     ctx.nant, ctx.ntap,
                                     s1_memory, s1_count, replace_noise);
    fprintf (stderr, "main: hires_delay_fractional_sk_scale returned\n");
#else
    fprintf (stderr, "main: hires_delay_fractional(%ld)\n", nbytes_out);
#ifdef USE_CONSTANT_MEMORY
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, h_fringe_coeffs, fringes_size,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#else
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, d_fringe_coeffs, d_ant_scales,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#endif
    fprintf (stderr, "main: hires_delay_fractional returned\n");
#endif

    // copy output from device to host
    fprintf (stderr, "main: copy delayed data back %d bytes nbytes_out=%d\n", nbytes, nbytes_out);
    error = cudaMemcpyAsync (h_out + ou_offset, d_fractional_delayed, nbytes, cudaMemcpyDeviceToHost, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync D2H failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    cudaStreamSynchronize (stream);
  }

  fprintf (stderr, "[B][F][S][T] (re, im) - Testing Sample Integrity\n");
  ptr = (int8_t *) h_out;
  int val;
  uint64_t nerr = 0;

  uint64_t bad_count = 0;
  uint64_t good_count = 0;
  for (i=0; i<3; i++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        for (isamp=0; isamp<nsamp; isamp++)
        {
          val = isamp % 128;
          if (ptr[0] != (int8_t) val)
          {
            if (nerr < 100)
              fprintf (stderr, "[%d][%d][%d][%d] (%d,%d) != %d\n", i, ichan, iant, isamp, ptr[0], ptr[1], val);
            nerr++;
            bad_count++;
          }
          else
            good_count++;
          ptr += 2;
        }
      }
    }
  }

  fprintf (stderr, "bad=%"PRIu64" good=%"PRIu64"\n", bad_count, good_count);

#endif

  for (iant=0; iant<nant; iant++)
    for (ichan=0; ichan<nchan; ichan++)
      delays[iant][ichan].samples = fixed_delay;

//#define TEST_ANT
#ifdef TEST_ANT
  //////////////////////////////////////////////////////////////////////////////
  //
  // Test Antenna integrity
  //
 
  fprintf (stderr, "WRITING ANT TESTING TO INPUT\n");
  ptr = (int8_t *) h_in;
  for (isamp=0; isamp<nsamp; isamp++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        ptr[0] = (int8_t) iant;
        ptr[1] = ptr[0];
        ptr += 2;
      }
    }
  }

  ctx.first_kernel = 1;

  // copy input from host to device
  fprintf (stderr, "main: cudaMemcpyAsync H2D (%ld)\n", nbytes);
  error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }

  cudaStreamSynchronize (stream);

  fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
  d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
  fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

  for (i=0; i<1; i++)
  {
    fprintf (stderr, "main: copy datablock delays to GPU %d bytes\n", nbytes);
    error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
    d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
    fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

    // copy the delays to the GPU
    fprintf (stderr, "main: copy fractional delays to GPU %d bytes\n", delays_size);
    error = cudaMemcpyAsync (d_fractional_delays, h_fractional_delays, delays_size, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }


#ifdef SKZAP
    fprintf (stderr, "main: hires_delay_fractional_sk_scale(%ld)\n", nbytes_out);
    hires_delay_fractional_sk_scale (stream, d_sample_delayed, d_fractional_delayed, d_fbuf,
                                     d_rstates, d_sigmas, d_mask,
                                     d_fractional_delays, d_means, h_fringe_coeffs, 
                                     fringes_size,
                                     nbytes_out, ctx.nchan, ctx.nant, ctx.ntap, replace_noise);
    fprintf (stderr, "main: hires_delay_fractional_sk_scale returned\n");
#else
    fprintf (stderr, "main: hires_delay_fractional(%ld)\n", nbytes_out);
#ifdef USE_CONSTANT_MEMORY
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, h_fringe_coeffs, fringes_size,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#else
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, d_fringe_coeffs, d_ant_scales,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#endif
    fprintf (stderr, "main: hires_delay_fractional returned\n");
#endif

    // copy output from device to host
    fprintf (stderr, "main: copy delayed data back %d bytes\n", nbytes);
    error = cudaMemcpyAsync (h_out, d_fractional_delayed, nbytes, cudaMemcpyDeviceToHost, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync D2H failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    cudaStreamSynchronize (stream);

    fprintf (stderr, "[F][S][T] (re, im) - Testing Antenna Integrity\n");
    ptr = (int8_t *) h_out;
    int val;
    uint64_t nerr = 0;
    uint64_t bad_count = 0;
    uint64_t good_count = 0;
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        for (isamp=0; isamp<nsamp; isamp++)
        {
          val = iant;
          if (ptr[0] != (int8_t) val)
          {
            if (nerr < 10)
              fprintf (stderr, "[%d][%d][%d] (%d,%d) != %d\n", ichan, iant, isamp, ptr[0], ptr[1], val);
            nerr++;
            bad_count++;
          }
          else
            good_count++;
          ptr += 2;
        }
      }
    }
    fprintf (stderr, "bad=%"PRIu64" good=%"PRIu64"\n", bad_count, good_count);
  }
#endif

//#define TEST_CHAN
#ifdef TEST_CHAN
  //////////////////////////////////////////////////////////////////////////////
  //
  // Test Channel integrity
  //
  //
  fprintf (stderr, "WRITING CHAN TESTING TO INPUT\n");
  ptr = (int8_t *) h_in;
  for (isamp=0; isamp<nsamp; isamp++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        ptr[0] = (int8_t) ichan;
        ptr[1] = ptr[0];
        ptr += 2;
      }
    }
  }

  ctx.first_kernel = 1;

  // copy input from host to device
  fprintf (stderr, "main: copy input GPU %d bytes\n", nbytes);
  error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }

  //cudaStreamSynchronize(stream);

  fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
  d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
  fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

  for (i=0; i<1; i++)
  {
    fprintf (stderr, "main: copy input GPU %d bytes\n", nbytes);
    error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
    d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
    fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

    // copy the delays to the GPU
    fprintf (stderr, "main: copy fractional delays to GPU %d bytes\n", delays_size);
    error = cudaMemcpyAsync (d_fractional_delays, h_fractional_delays, delays_size, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

#ifdef SKZAP
    fprintf (stderr, "main: hires_delay_fractional_sk_scale(%ld)\n", nbytes_out);
    hires_delay_fractional_sk_scale (stream, d_sample_delayed, d_fractional_delayed, d_fbuf,
                                     d_rstates, d_sigmas, d_mask,
                                     d_fractional_delays, d_means, h_fringe_coeffs, 
                                     fringes_size,
                                     nbytes_out, ctx.nchan, ctx.nant, ctx.ntap, replace_noise);
#else
    fprintf (stderr, "main: hires_delay_fractional(%ld)\n", nbytes_out);
#ifdef USE_CONSTANT_MEMORY
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, h_fringe_coeffs, fringes_size,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#else
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, d_fringe_coeffs, d_ant_scales,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#endif
#endif

    // copy output from device to host
    fprintf (stderr, "main: copy delayed data back %d bytes\n", nbytes);
    error = cudaMemcpyAsync (h_out, d_fractional_delayed, nbytes, cudaMemcpyDeviceToHost, stream);
    //error = cudaMemcpyAsync (h_out, d_sample_delayed, nbytes_out, cudaMemcpyDeviceToHost, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync D2H failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    cudaStreamSynchronize (stream);

    fprintf (stderr, "[F][S][T] (re, im) - Testing Channel Integrity\n");
    ptr = (int8_t *) h_out;
    int val;
    uint64_t nerr = 0;
    uint64_t bad_count = 0;
    uint64_t good_count = 0;
    for (ichan=0; ichan<nchan; ichan++)
    {
      val = ichan;
      for (iant=0; iant<nant; iant++)
      {
        for (isamp=0; isamp<nsamp; isamp++)
        {
          if (ptr[0] != (int8_t) val)
          {
            if (nerr < 10)
              fprintf (stderr, "[%d][%d][%d] (%d,%d) != %d\n", ichan, iant, isamp, ptr[0], ptr[1], val);
            nerr++;
            bad_count++;
          }
          else
            good_count++;
          ptr += 2;
        }
      }
    }
    fprintf (stderr, "bad=%"PRIu64" good=%"PRIu64"\n", bad_count, good_count);
  }
#endif

//#define TEST_RNG
#ifdef TEST_RNG
  ////////////////////////////////////////////////////////////////
  // Test random number generator
  //
#if 1
  uint8_t * u8_in = (uint8_t *) h_in;
  srand(4);

  fprintf (stderr, "[T][F][S] - Input for RNG integridy\n");
  for (isamp=0; isamp<nsamp; isamp++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        u8_in[0] = (uint8_t) (rand() % 255);
        u8_in[1] = (uint8_t) (rand() % 255);
        u8_in += 2;
      }
    }
  }
#endif
  fprintf (stderr, "fixed_delay=%u\n", fixed_delay);

  // manually set the delays for secondary jump
  for (iant=0; iant<nant; iant++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      delays[iant][ichan].samples = fixed_delay;
    }
  }

  // copy input from host to device
  error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }

  // reset things
  ctx.first_kernel = 1;

  fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
  d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
  fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

  for (i=0; i<1; i++)
  {
    fprintf (stderr, "main: copy datablock delays to GPU %d bytes\n", nbytes);
    error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
    d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
    fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

    // copy the delays to the GPU
    fprintf (stderr, "main: copy fractional delays to GPU %d bytes\n", delays_size);
    error = cudaMemcpyAsync (d_fractional_delays, h_fractional_delays, delays_size, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

#ifdef SKZAP
    fprintf (stderr, "main: hires_delay_fractional_sk_scale(%ld)\n", nbytes_out);
    hires_delay_fractional_sk_scale (stream, d_sample_delayed, d_fractional_delayed, d_fbuf,
                                     d_rstates, d_sigmas, d_mask,
                                     d_fractional_delays, d_fir_coeffs, 
#ifndef USE_CONSTANT_MEMORY
                                     d_fringe_coeffs, d_ant_scales, 
#endif 
                                     d_s1s, d_s2s, d_thresh, 
#ifdef USE_CONSTANT_MEMORY
                                     h_fringe_coeffs, fringes_size, 
#endif
                                     nbytes_out, ctx.nchan, 
                                     ctx.nant, ctx.ntap,
                                     s1_memory, s1_count, replace_noise);
#else
    fprintf (stderr, "main: hires_delay_fractional(%ld)\n", nbytes_out);
#ifdef USE_CONSTANT_MEMORY
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, h_fringe_coeffs, fringes_size,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#else
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, d_fringe_coeffs, d_ant_scales,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#endif
#endif
    fprintf (stderr, "main: hires_delay_fractional returned\n");

    // copy output from device to host
    fprintf (stderr, "main: copy delayed data back %d bytes\n", nbytes);
    error = cudaMemcpyAsync (h_out, d_fractional_delayed, nbytes, cudaMemcpyDeviceToHost, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync D2H failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    cudaStreamSynchronize (stream);

#if 1
    fprintf (stderr, "[F][S][T] out, in - Testing Random Integrity\n");
    uint8_t * u8_out = (uint8_t *) h_out;
    uint8_t * u8_in = (uint8_t *) h_in;
    int val;
    uint8_t nerr = 0;
    unsigned hout_idx = 0;
    unsigned hin_idx = 0;
    uint64_t bad_count = 0;
    uint64_t good_count = 0;
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        for (isamp=0; isamp<nsamp; isamp++)
        {
          uint8_t o1 = u8_out[hout_idx];
          uint8_t o2 = u8_out[hout_idx+1];

          if (isamp + fixed_delay < nsamp)
          {
            hin_idx = (((isamp + fixed_delay) * nchan * nant) + (ichan * nant) + iant) * 2;
            uint8_t i1 = u8_in[hin_idx];
            uint8_t i2 = u8_in[hin_idx+1];

            if (o1 != i1 || o2 != i2)
            {
              if (nerr < 30)
              {
                fprintf (stderr, "[%d][%d][%d] out(%"PRIu8",%"PRIi8") != in(%"PRIu8",%"PRIi8") %u <- %u\n", ichan, iant, isamp, o1, o2, i1, i2, hout_idx, hin_idx);
                nerr++;
              }
              bad_count++;
            }
            else
              good_count++;
          }
          hout_idx += 2;
        }
      }
    }
    fprintf (stderr, "bad=%"PRIu64" good=%"PRIu64"\n", bad_count, good_count);
#endif
  }
#endif

#define TEST_FRACTIONAL
#ifdef TEST_FRACTIONAL
  ////////////////////////////////////////////////////////////////
  // Test random number generator
  //
  uint8_t * u8_in = (uint8_t *) h_in;
  srand(4);

  fprintf (stderr, "[T][F][S] - Input for fractional integridy\n");
  for (isamp=0; isamp<nsamp; isamp++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        u8_in[0] = (uint8_t) (rand() % 255);
        u8_in[1] = (uint8_t) (rand() % 255);
        u8_in += 2;
      }
    }
  }
  float fractional_delay = 0.1;
  fprintf (stderr, "fixed_delay=%u fractional_delay=%f\n", fixed_delay, fractional_delay);

  // manually set the delays for secondary jump
  for (iant=0; iant<nant; iant++)
  {
    for (ichan=0; ichan<nchan; ichan++)
    {
      delays[iant][ichan].samples = fixed_delay;
      h_fractional_delays[ichan*nant + iant] = fractional_delay;
    }
  }

  // copy input from host to device
  error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
  if (error != cudaSuccess)
  {
    fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
    return -1;
  }

  // reset things
  ctx.first_kernel = 1;

  fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
  d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
  fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

  for (i=0; i<1; i++)
  {
    fprintf (stderr, "main: copy datablock delays to GPU %d bytes\n", nbytes);
    error = cudaMemcpyAsync (d_in, h_in, nbytes, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    fprintf (stderr, "main: hires_transpose_delay(%ld)\n", nbytes);
    d_sample_delayed = hires_transpose_delay (stream, &ctx, d_in, nbytes, delays);
    fprintf (stderr, "main: hires_transpose_delay returned %p\n", d_sample_delayed);

    // copy the delays to the GPU
    fprintf (stderr, "main: copy fractional delays to GPU %d bytes\n", delays_size);
    error = cudaMemcpyAsync (d_fractional_delays, h_fractional_delays, delays_size, cudaMemcpyHostToDevice, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync H2D failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

#ifdef SKZAP
    fprintf (stderr, "main: hires_delay_fractional_sk_scale(%ld)\n", nbytes_out);
    hires_delay_fractional_sk_scale (stream, d_sample_delayed, d_fractional_delayed, d_fbuf,
                                     d_rstates, d_sigmas, d_mask,
                                     d_fractional_delays, d_fir_coeffs, 
#ifndef USE_CONSTANT_MEMORY
                                     d_fringe_coeffs, d_ant_scales, 
#endif 
                                     d_s1s, d_s2s, d_thresh, 
#ifdef USE_CONSTANT_MEMORY
                                     h_fringe_coeffs, fringes_size, 
#endif
                                     nbytes_out, ctx.nchan, 
                                     ctx.nant, ctx.ntap,
                                     s1_memory, s1_count, replace_noise);
#else
    fprintf (stderr, "main: hires_delay_fractional(%ld)\n", nbytes_out);
#ifdef USE_CONSTANT_MEMORY
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, h_fringe_coeffs, fringes_size,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#else
    hires_delay_fractional (stream, d_sample_delayed, d_fractional_delayed,
                            d_fractional_delays, d_fir_coeffs, d_fringe_coeffs, d_ant_scales,
                            nbytes_out, ctx.nchan, ctx.nant, ctx.ntap);
#endif
#endif
    fprintf (stderr, "main: hires_delay_fractional returned\n");

    // copy output from device to host
    fprintf (stderr, "main: copy delayed data back %d bytes\n", nbytes);
    error = cudaMemcpyAsync (h_out, d_fractional_delayed, nbytes, cudaMemcpyDeviceToHost, stream);
    if (error != cudaSuccess)
    {
      fprintf (stderr, "cudaMemcpyAsync D2H failed: %s\n", cudaGetErrorString(error));
      return -1;
    }

    cudaStreamSynchronize (stream);

    uint8_t * u8_out = (uint8_t *) h_out;
    uint8_t * u8_in = (uint8_t *) h_in;

    // memory for FIR 
    float * filter = (float *) malloc (ntap * sizeof(float));

    // form delayed CPU for reference
    fprintf (stderr, "Forming CPU FIR delay\n");
    float re, im;
    unsigned half_ntap = ntap / 2;
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        for (isamp=0; isamp<nsamp; isamp++)
        {
          unsigned idx = ((isamp * nchan * nant) + (ichan * nant) + iant) * 2;
          re = ((float) u8_in[idx]) + 0.38;
          im = ((float) u8_in[idx+1]) + 0.38;
          unpacked[isamp] = re + im * I;
        }

        // fractional delay 
        sinc_filter_float (unpacked, tapped, filter, nsamp, ntap, fractional_delay);

        unsigned delay_idx = ((ichan * nant * nsamp) + (iant * nsamp)) * 2;
        for (isamp=0; isamp<nsamp; isamp++)
        {
          delayed8[delay_idx + 0] = (int8_t) rintf (creal(tapped[isamp + fixed_delay]));
          delayed8[delay_idx + 1] = (int8_t) rintf (cimag(tapped[isamp + fixed_delay]));
          delay_idx += 2;
        }
      }
    }

    free(filter);

    fprintf (stderr, "[F][S][T] out, in - Testing FIR Integrity\n");
    uint8_t * u8_del = (uint8_t *) delayed8;
    int val;
    uint8_t nerr = 0;
    unsigned idx = 0;
    uint64_t bad_count = 0;
    uint64_t good_count = 0;
    for (ichan=0; ichan<nchan; ichan++)
    {
      for (iant=0; iant<nant; iant++)
      {
        for (isamp=0; isamp<nsamp; isamp++)
        {
          if (isamp + fixed_delay < nsamp)
          {
            if ((u8_out[idx] != u8_del[idx]) || (u8_out[idx+1] != u8_del[idx+1]))
            {
              if (nerr < 30)
              {
                fprintf (stderr, "[%d][%d][%d] out(%"PRIu8",%"PRIi8") != in(%"PRIu8",%"PRIi8") %u\n", 
                        ichan, iant, isamp, 
                        u8_out[idx], u8_out[idx+1],
                        u8_del[idx], u8_del[idx+1],
                        idx);
                nerr++;
              }
              bad_count++;
            }
            else
              good_count++;
          }
          idx += 2;
        }
      }
    }
    fprintf (stderr, "bad=%"PRIu64" good=%"PRIu64"\n", bad_count, good_count);
  }
#endif

  if (verbose)
    fprintf (stderr, "main: hires_transpose_delay_dealloc()\n");
  if (hires_transpose_delay_dealloc (&ctx) < 0)
  {
    fprintf (stderr, "main: hires_transpose_delay_dealloc failed\n");
    return -1;
  }

  if (verbose)
    fprintf (stderr, "main: cudaFreeHost(h_in)\n");
  if (h_in)
    cudaFreeHost(h_in);
  h_in = 0;

  if (verbose)
    fprintf (stderr, "main: free(delayed8)\n");
  if (delayed8)
    free (delayed8);
  delayed8 = 0;

  if (verbose)
    fprintf (stderr, "main: free(tapped)\n");
  if (tapped)
    free (tapped);
  tapped = 0;

  if (verbose)
    fprintf (stderr, "main: free(unpacked)\n");
  if (unpacked)
    free (unpacked);
  unpacked = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_in)\n");
  if (d_in)
    cudaFree(d_in);
  d_in = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_fbuf)\n");
  if (d_fbuf)
    cudaFree (d_fbuf);
  d_fbuf = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_rstates)\n");
  if (d_rstates)
    cudaFree (d_rstates);
  d_rstates = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_sigmas)\n");
  if (d_sigmas)
    cudaFree (d_sigmas);
  d_sigmas = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_mask)\n");
  if (d_mask)
    cudaFree (d_mask);
  d_mask = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFreeHost(h_ant_scales)\n");
  if (h_ant_scales)
    cudaFreeHost(h_ant_scales);
  h_ant_scales = 0;

#ifndef USE_CONSTANT_MEMORY
  if (verbose)
    fprintf (stderr, "main: cudaFree(d_ant_scales)\n");
  if (d_ant_scales)
    cudaFree (d_ant_scales);
  d_ant_scales = 0;
#endif

  if (verbose)
    fprintf (stderr, "main: cudaFreeHost(h_fringe_coeffs)\n");
  if (h_fringe_coeffs)
    cudaFreeHost(h_fringe_coeffs);
  h_fringe_coeffs = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFreeHost(h_fractional_delays)\n");
  if (h_fractional_delays)
    cudaFreeHost(h_fractional_delays);
  h_fractional_delays = 0;
  if (verbose)
    fprintf (stderr, "main: cudaFree(d_fractional_delayed)=%p\n", d_fractional_delayed);
  if (d_fractional_delayed)
    cudaFree (d_fractional_delayed);
  d_fractional_delayed = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_fractional_delays)\n");
  if (d_fractional_delays)
    cudaFree (d_fractional_delays);
  d_fractional_delays = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_thresh)\n");
  if (d_thresh)
    cudaFree (d_thresh);
  d_thresh = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_fir_coeffs)\n");
  if (d_fir_coeffs)
    cudaFree (d_fir_coeffs);
  d_fir_coeffs = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_s1s)\n");
  if (d_s1s)
    cudaFree (d_s1s);
  d_s1s = 0;

  if (verbose)
    fprintf (stderr, "main: cudaFree(d_s2s)\n");
  if (d_s2s)
    cudaFree (d_s2s);
  d_s2s = 0;

  free (channels);
  free (modules);

  for (imod=0; imod<nmod; imod++)
    free (delays[imod]);
  free (delays);


  return 0;
}
