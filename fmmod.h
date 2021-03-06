/*
 * JMPXRDS, an FM MPX signal generator with RDS support on
 * top of Jack Audio Connection Kit - Main processor
 *
 * Copyright (C) 2015 Nick Kossifidis <mickflemm@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <jack/jack.h>		/* For jack-related types */
#include "filters.h"
#include "resampler.h"
#include "oscilator.h"		/* Also brings in stdint.h and config.h */
#include "rds_encoder.h"
#include "rtp_server.h"


/* We need something big enough to output the MPX
 * signal. 96KHz should be enough for the audio part
 * with SSB enabled but for proper stereo imaging and
 * in order to support RDS output too (57Khz carrier)
 * 192Khz is needed. */
#define FMMOD_OUTPUT_SAMPLERATE_MIN	192000

enum fmmod_errors {
	FMMOD_ERR_INVALID_INPUT = -1,
	FMMOD_ERR_RESAMPLER_ERR = -2,
	FMMOD_ERR_JACKD_ERR = -3,
	FMMOD_ERR_NOMEM = -4,
	FMMOD_ERR_OSC_ERR = -5,
	FMMOD_ERR_RDS_ERR = -6,
	FMMOD_ERR_SHM_ERR = -7,
	FMMOD_ERR_SOCK_ERR = -8,
	FMMOD_ERR_RTP_ERR = -9,
	FMMOD_ERR_ALREADY_RUNNING = -10,
	FMMOD_ERR_LPF = -11,
	FMMOD_ERR_HILBERT = -12,
	FMMOD_ERR_AFLT = -13,
};

/* If JACK's samplerate is not enough (e.g. soundcard
 * can't do 192KHz or CPU can't handle 192KHz processsing)
 * send the data only to the output socket and through RTP */
enum fmmod_output {
	FMMOD_OUT_JACK = 1,
	FMMOD_OUT_NOJACK = 2
};

/* Stereo signal (L-R) encoding:
 * DSB -> Double side band (default)
 * SSB HARTLEY -> Single Side Band Hartley modulator
 * SSB WEAVER -> Single Side Band Weaver modulator
 * For more infos check out fmmod.c */
enum fmmod_stereo_modulation {
	FMMOD_DSB = 0,
	FMMOD_SSB_HARTLEY = 1,
	FMMOD_SSB_WEAVER = 2,
	FMMOD_SSB_LPF = 3,
	FMMOD_MONO = 4
};

/* Control I/O channel */
struct fmmod_control {
	float audio_gain;
	float pilot_gain;
	float rds_gain;
	float stereo_carrier_gain;
	float mpx_gain;
	int stereo_modulation;
	int use_audio_lpf;
	int preemph_tau;
	float peak_mpx_out;
	float peak_audio_in_l;
	float peak_audio_in_r;
	int sample_rate;
	int max_samples;
};

struct fmmod_instance {
	/* State */
	int active;
	/* Audio input buffer */
	float *inbuf_l;
	float *inbuf_r;
	uint32_t inbuf_len;
	uint32_t upsampled_num_samples;
	uint32_t upsampled_buf_len;
	/* Upsampled audio buffers */
	float *uaudio_buf_0;
	float *uaudio_buf_1;
	float *delay_buf;
	/* MPX Output buffer */
	float *mpxbuf;
	int output_type;
	/* For socket output */
	float *sock_outbuf;
	uint32_t sock_outbuf_len;
	int out_sock_fd;
	/* The Oscilator */
	struct osc_state sin_osc;
	/* The audio filter */
	struct audio_filter aflt;
	/* The resampler */
	struct resampler_data rsmpl;
	/* The RDS Encoder */
	struct rds_encoder rds_enc;
	/* The RTP Server */
	struct rtp_server rtpsrv;
	/* Jack-related */
	jack_port_t *inL;
	jack_port_t *inR;
	jack_port_t *outMPX;
	jack_client_t *client;
	jack_nframes_t added_latency;
	/* SSB modulators */
	struct osc_state cos_osc;
	struct lpf_filter_data ssb_lpf;
	struct ssb_filter_data weaver_lpf;
	struct hilbert_transformer_data ht;
	/* Control */
	struct shm_mapping *ctl_map;
	struct fmmod_control *ctl;
};

typedef int (*mpx_generator) (struct fmmod_instance *, float*, float*, int, float*);

int fmmod_initialize(struct fmmod_instance *fmmod);
void fmmod_destroy(struct fmmod_instance *fmmod, int shutdown);
