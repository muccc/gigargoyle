#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <fftw3.h>
#include <jack/jack.h>

#include "gg_simple_client.h"

#define COLS 24
#define ROWS 4
#define LP_LEN 2
#define BUFSIZE 1024

jack_port_t *input_port;
jack_client_t *client;
gg_frame *frame;

double old_amplitudes[LP_LEN][COLS] = {{0.0}};
double filtered_amplitudes[COLS] = {0.0};

/* Convert bark to physical frequency */
double z_to_f(int z) {
  double map[] = {0, 50, 150, 250, 350, 450, 570, 700, 840, 1000, 1170, 1370, 1600, 1850, 2150, 2500, 2900, 3400, 4000, 4800, 5800, 7000, 8500, 10500, 13500, 20500, 27000};
  return map[z];
}

/* Interpolate linearly between two RGB colors */
void interp_color(int ri1, int gi1, int bi1,
                  int ri2, int gi2, int bi2,
                  int *ro, int *go, int *bo,
                  float lambda) {
  *ro = lambda * ri1 + (1-lambda) * ri2;
  *go = lambda * gi1 + (1-lambda) * gi2;
  *bo = lambda * bi1 + (1-lambda) * bi2;
}

void draw_spectrum_column(gg_frame* f, unsigned int col, unsigned int amplitude) {
  int row;
  for (row = 0; row < 4; ++row) {
    if (row < amplitude) {
      int r, g, b;
      /* "Gradient" */
      switch (row) {
      case 0:
      case 1:
        r = 0;
        g = 255;
        b = 0;
        break;
      case 2:
        r = 127;
        g = 127;
        b = 0;
        break;
      case 3:
        r = 255;
        g = 0;
        b = 0;
        break;
      }
      gg_set_pixel_color(f, col, ROWS-1-row, r, g, b);
    } else {
      gg_set_pixel_color(f, col, ROWS-1-row, 0, 0, 0);
    }
  }

}

/* Input and output arrays for fft */
fftw_complex *in_cplx, *out_cplx;

fftw_plan p;

/* Called by jack when new data ist available */
int process (jack_nframes_t nframes, void *arg) {
  static uint8_t cnt = 0;
  jack_default_audio_sample_t *in, *out;

  int i;
  in = jack_port_get_buffer(input_port, nframes);

  /* Copy samples to fft input vector and window them with a hamming window */
  for (i = 0; i < nframes; ++i){
    in_cplx[i][0] = in[i]*(0.54-0.46*sin(2*M_PI*i/(nframes-1)));
    in_cplx[i][1] = 0.0;
  }

  /* Do the fft */
  fftw_execute(p);

  double acc = 0;
  double acc_i = 0;
  double acc_r = 0;
  //int log_idx = 0;
  double f = 0;
  int z = 0;

  gg_set_frame_color(frame, 0, 0, 0);

  /*  */
  /* Lowpass filter the spectrum */
  /*  */

  /* Move old amplitudes */
  int k, l;
  for (k = 0; k < LP_LEN-1; ++k) {
    for (l = 0; l < COLS; ++l) {
      old_amplitudes[k][l] = old_amplitudes[k+1][l];
    }
  }

  /* Calculate boxcar mean */
  for (l = 0; l < COLS; ++l) {
    for (k = 0; k < LP_LEN; ++k) {    
      filtered_amplitudes[l] += old_amplitudes[k][l];
    }
    filtered_amplitudes[l] /= LP_LEN;
  }

  for (i = 0; i < 512; ++i){
    //double val = out_cplx[i][0]*out_cplx[i][0] + out_cplx[i][1]*out_cplx[i][1];
    double val_i = out_cplx[i][0];
    double val_r = out_cplx[i][1];
    acc_i += val_i;
    acc_r += val_r;

    /* Sampled frequency to physical frequency */
    f = i/512.0*22.05e3;

    /* If we are in a new bark band, we need to start integrating the
     * spectrum */
    if (f > z_to_f(z)) {
      acc = sqrt(acc_i*acc_i + acc_r*acc_r);

      /* Set current amplitude */
      old_amplitudes[LP_LEN-1][z] = acc;

      printf("\e[%dm%1d\e[0m", (int)(acc+30), 0);

      /* Calculate bar height from amplitude */
      int bar_height = 0;
      bar_height = (int)round(4*atan(filtered_amplitudes[z]/40.0)/(M_PI/2.0));
      if (bar_height > 4) bar_height = 4;
      
      /* Do the coloring of current bar from amplitude */
      draw_spectrum_column(frame, z, bar_height);

      ++z;
      acc = 0;
      acc_i = 0;
      acc_r = 0;
    }
  }

  /* Finally we can send our frame */
  if (++cnt % 2 == 0) {
    gg_send_frame((gg_socket *)arg, frame);
  }

  printf("\n");

  return 0;      
}

void jack_shutdown(void *arg) {
  fftw_destroy_plan(p);
  fftw_free(in_cplx); fftw_free(out_cplx);

  exit (1);
}

int main(int argc, char *argv[]) {
  const char **ports;
  const char *client_name = "acabspectrum";
  const char *server_name = NULL;
  jack_options_t options = JackNullOption;
  jack_status_t status;
  gg_socket *gg_socket;

  /* Open connection to gigargoyle */
  frame = gg_init_frame(COLS, ROWS, 3);
  gg_socket = gg_init_socket("localhost", 0xacab);

  /* open a client connection to the JACK server */
  client = jack_client_open (client_name, options, &status, server_name);
  if (client == NULL) {
    fprintf (stderr, "jack_client_open() failed, "
             "status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    exit (1);
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(client);
    fprintf (stderr, "unique name `%s' assigned\n", client_name);
  }

  gg_set_duration(gg_socket, ((double)BUFSIZE)/jack_get_sample_rate(client)*1000.0);

  jack_set_process_callback (client, process, gg_socket);
  jack_on_shutdown (client, jack_shutdown, 0);

  /* create port */
  input_port = jack_port_register (client, "input",
                                   JACK_DEFAULT_AUDIO_TYPE,
                                   JackPortIsInput, 0);

  if (input_port == NULL) {
    fprintf(stderr, "no more JACK ports available\n");
    exit (1);
  }

  /* FFTW */
  in_cplx = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * BUFSIZE);
  out_cplx = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * BUFSIZE);

  p = fftw_plan_dft_1d(BUFSIZE, in_cplx, out_cplx, FFTW_FORWARD, FFTW_ESTIMATE);

  /* Tell the JACK server that we are ready to roll.  Our
   * process() callback will start running now. */

  if (jack_activate (client)) {
    fprintf (stderr, "cannot activate client");
    exit (1);
  }

  /* Connect the ports.  You can't do this before the client is
   * activated, because we can't make connections to clients
   * that aren't running.  Note the confusing (but necessary)
   * orientation of the driver backend ports: playback ports are
   * "input" to the backend, and capture ports are "output" from
   * it.
   */

  ports = jack_get_ports (client, NULL, NULL,
                          JackPortIsPhysical|JackPortIsOutput);
  if (ports == NULL) {
    fprintf(stderr, "no physical capture ports\n");
    exit (1);
  }

  if (jack_connect (client, ports[0], jack_port_name (input_port))) {
    fprintf (stderr, "cannot connect input ports\n");
  }

  free (ports);
	
  /* ports = jack_get_ports (client, NULL, NULL, */
  /*                         JackPortIsPhysical|JackPortIsInput); */
  /* if (ports == NULL) { */
  /*   fprintf(stderr, "no physical playback ports\n"); */
  /*   exit (1); */
  /* } */

  /* if (jack_connect (client, jack_port_name (output_port), ports[0])) { */
  /*   fprintf (stderr, "cannot connect output ports\n"); */
  /* } */

  /* free (ports); */

  /* keep running until stopped by the user */
  sleep (-1);

  jack_client_close(client);
  exit(0);
}
