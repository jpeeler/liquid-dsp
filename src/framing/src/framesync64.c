/*
 * Copyright (c) 2007 - 2014 Joseph Gaeddert
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// framesync64.c
//
// basic frame synchronizer
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <assert.h>

#include "liquid.internal.h"

#define DEBUG_FRAMESYNC64           1
#define DEBUG_FRAMESYNC64_PRINT     0
#define DEBUG_FILENAME              "framesync64_internal_debug.m"
#define DEBUG_BUFFER_LEN            (1600)

// push samples through detection stage
void framesync64_execute_seekpn(framesync64   _q,
                                float complex _x);

// update symbol synchronizer internal state (filtered error, index, etc.)
//  _q      :   frame synchronizer
//  _x      :   input sample
//  _y      :   output symbol
int framesync64_step(framesync64     _q,
                     float complex   _x,
                     float complex * _y);

// push samples through synchronizer, saving received p/n symbols
void framesync64_execute_rxpreamble(framesync64   _q,
                                    float complex _x);

// receive payload symbols
void framesync64_execute_rxpayload(framesync64   _q,
                                   float complex _x);

// framesync64 object structure
struct framesync64_s {
    // callback
    framesync_callback  callback;   // user-defined callback function
    void *              userdata;   // user-defined data structure
    framesyncstats_s    framestats; // frame statistic object
    
    // synchronizer objects
    qdetector_cccf detector;  // pre-demod detector
    float tau_hat;                  // fractional timing offset estimate
    float dphi_hat;                 // carrier frequency offset estimate
    float phi_hat;                  // carrier phase offset estimate
    float gamma_hat;                // channel gain estimate
    nco_crcf mixer;            // coarse carrier frequency recovery

    // timing recovery objects, states
    firpfb_crcf mf;                 // matched filter decimator
    unsigned int npfb;              // number of filters in symsync
    unsigned int    mf_counter;       // matched filter output timer
    unsigned int    pfb_index;      // filterbank index
    float complex symsync_out;      // symbol synchronizer output

    //eqlms_cccf equalizer;         // equalizer (trained on input p/n sequence)

    // preamble
    float complex preamble_pn[64];  // known 64-symbol p/n sequence
    float complex preamble_rx[64];  // received p/n symbols
    
    // payload decoder
    float complex payload_rx[630];  // received payload symbols with pilots
    float complex payload_sym[600]; // received payload symbols
    unsigned char payload_dec[72];  // decoded payload bytes
    qpacketmodem  dec;              // packet demodulator/decoder
    qpilotsync    pilotsync;        // pilot extraction, carrier recovery
    int           payload_valid;    // did payload pass crc?
    
    // status variables
    enum {
        FRAMESYNC64_STATE_DETECTFRAME=0,        // detect frame (seek p/n sequence)
        FRAMESYNC64_STATE_RXPREAMBLE,           // receive p/n sequence
        FRAMESYNC64_STATE_RXPAYLOAD,            // receive payload data
    } state;
    unsigned int preamble_counter;  // counter: num of p/n syms received
    unsigned int payload_counter;   // counter: num of payload syms received

#if DEBUG_FRAMESYNC64
    int debug_enabled;              // debugging enabled?
    int debug_objects_created;      // debugging objects created?
    windowcf debug_x;               // debug: raw input samples
#endif
};

// create framesync64 object
//  _callback       :   callback function invoked when frame is received
//  _userdata       :   user-defined data object passed to callback
framesync64 framesync64_create(framesync_callback _callback,
                               void *             _userdata)
{
    framesync64 q = (framesync64) malloc(sizeof(struct framesync64_s));
    q->callback = _callback;
    q->userdata = _userdata;

    unsigned int i;

    // generate p/n sequence
    msequence ms = msequence_create(6, 0x0043, 1);
    for (i=0; i<64; i++)
        q->preamble_pn[i] = (msequence_advance(ms)) ? 1.0f : -1.0f;
    msequence_destroy(ms);

    // create frame detector
    unsigned int k    = 2;    // samples/symbol
    unsigned int m    = 3;    // filter delay (symbols)
    float        beta = 0.5f; // excess bandwidth factor
    q->detector = qdetector_cccf_create(q->preamble_pn, 64, LIQUID_FIRFILT_ARKAISER, k, m, beta);

    // create symbol timing recovery filters
    q->npfb = 32;   // number of filters in the bank
    q->mf   = firpfb_crcf_create_rnyquist(LIQUID_FIRFILT_ARKAISER, q->npfb,k,m,beta);

    // create down-coverters for carrier phase tracking
    q->mixer = nco_crcf_create(LIQUID_NCO);
    
    // create payload demodulator/decoder object
    int check      = LIQUID_CRC_24;
    int fec0       = LIQUID_FEC_NONE;
    int fec1       = LIQUID_FEC_GOLAY2412;
    int mod_scheme = LIQUID_MODEM_QPSK;
    q->dec         = qpacketmodem_create();
    qpacketmodem_configure(q->dec, 72, check, fec0, fec1, mod_scheme);
    //qpacketmodem_print(q->dec);
    assert( qpacketmodem_get_frame_len(q->dec)==600 );

    // create pilot synchronizer
    q->pilotsync   = qpilotsync_create(600, 21);
    assert( qpilotsync_get_frame_len(q->pilotsync)==630 );

#if DEBUG_FRAMESYNC64
    // set debugging flags, objects to NULL
    q->debug_enabled         = 0;
    q->debug_objects_created = 0;
    q->debug_x               = NULL;
#endif

    // reset state
    framesync64_reset(q);

    return q;
}

// destroy frame synchronizer object, freeing all internal memory
void framesync64_destroy(framesync64 _q)
{
#if DEBUG_FRAMESYNC64
    // clean up debug objects (if created)
    if (_q->debug_objects_created) {
        windowcf_destroy(_q->debug_x);
    }
#endif

    // destroy synchronization objects
    qdetector_cccf_destroy(_q->detector);  // frame detector
    firpfb_crcf_destroy(_q->mf);                // matched filter
    nco_crcf_destroy(_q->mixer);           // coarse NCO

    qpacketmodem_destroy(_q->dec);              // payload demodulator
    qpilotsync_destroy(_q->pilotsync);          // pilot synchronizer

    // free main object memory
    free(_q);
}

// print frame synchronizer object internals
void framesync64_print(framesync64 _q)
{
    printf("framesync64:\n");
}

// reset frame synchronizer object
void framesync64_reset(framesync64 _q)
{
    // reset binary pre-demod synchronizer
    qdetector_cccf_reset(_q->detector);

    // reset carrier recovery objects
    nco_crcf_reset(_q->mixer);

    // reset symbol timing recovery state
    firpfb_crcf_reset(_q->mf);
        
    // reset state
    _q->state           = FRAMESYNC64_STATE_DETECTFRAME;
    _q->preamble_counter= 0;
    _q->payload_counter = 0;
    
    // reset frame statistics
    _q->framestats.evm = 0.0f;
}

// execute frame synchronizer
//  _q     :   frame synchronizer object
//  _x      :   input sample array [size: _n x 1]
//  _n      :   number of input samples
void framesync64_execute(framesync64     _q,
                         float complex * _x,
                         unsigned int    _n)
{
    unsigned int i;
    for (i=0; i<_n; i++) {
#if DEBUG_FRAMESYNC64
        if (_q->debug_enabled)
            windowcf_push(_q->debug_x, _x[i]);
#endif
        switch (_q->state) {
        case FRAMESYNC64_STATE_DETECTFRAME:
            // detect frame (look for p/n sequence)
            framesync64_execute_seekpn(_q, _x[i]);
            break;
        case FRAMESYNC64_STATE_RXPREAMBLE:
            // receive p/n sequence symbols
            framesync64_execute_rxpreamble(_q, _x[i]);
            break;
        case FRAMESYNC64_STATE_RXPAYLOAD:
            // receive payload symbols
            framesync64_execute_rxpayload(_q, _x[i]);
            break;
        default:
            fprintf(stderr,"error: framesync64_exeucte(), unknown/unsupported state\n");
            exit(1);
        }
    }
}

// 
// internal methods
//

// execute synchronizer, seeking p/n sequence
//  _q     :   frame synchronizer object
//  _x      :   input sample
//  _sym    :   demodulated symbol
void framesync64_execute_seekpn(framesync64   _q,
                                float complex _x)
{
    // push through pre-demod synchronizer
    float complex * v = qdetector_cccf_execute(_q->detector, _x);

    // check if frame has been detected
    if (v != NULL) {
        // get estimates
        _q->tau_hat   = qdetector_cccf_get_tau  (_q->detector);
        _q->gamma_hat = qdetector_cccf_get_gamma(_q->detector);
        _q->dphi_hat  = qdetector_cccf_get_dphi (_q->detector);
        _q->phi_hat   = qdetector_cccf_get_phi  (_q->detector);

        printf("***** frame detected! tau-hat:%8.4f, dphi-hat:%8.4f, gamma:%8.2f dB\n",
                _q->tau_hat, _q->dphi_hat, 20*log10f(_q->gamma_hat));

        // set estimates
        firpfb_crcf_set_scale(_q->mf, 0.5f / _q->gamma_hat);
        _q->pfb_index = 0;  // TODO: set value appropriately if tau_hat is negative
        nco_crcf_set_frequency(_q->mixer, _q->dphi_hat);
        nco_crcf_set_phase    (_q->mixer, _q->phi_hat );

        // update state
        _q->state = FRAMESYNC64_STATE_RXPREAMBLE;

        // run buffered samples through synchronizer
        unsigned int buf_len = qdetector_cccf_get_buf_len(_q->detector);
        framesync64_execute(_q, v, buf_len);
    }
}

// step...
//  _q      :   frame synchronizer
//  _x      :   input sample
//  _y      :   output symbol
int framesync64_step(framesync64     _q,
                     float complex   _x,
                     float complex * _y)
{
    // mix sample down
    float complex v;
    nco_crcf_mix_down(_q->mixer, _x, &v);
    nco_crcf_step    (_q->mixer);
    
    // push sample into filterbank
    firpfb_crcf_push   (_q->mf, v);
    firpfb_crcf_execute(_q->mf, _q->pfb_index, &v);

    // TODO: push sample through equalizer

    // increment counter to determine if sample is available
    _q->mf_counter++;
    int sample_available = (_q->mf_counter == 1) ? 1 : 0;
    
    // set output sample if available
    if (sample_available)
        *_y = v;

    // reset counter modulo samples/symbol
    _q->mf_counter %= 2; // k=2 samples/symbol

    // return flag
    return sample_available;
}

// execute synchronizer, receiving p/n sequence
//  _q     :   frame synchronizer object
//  _x      :   input sample
//  _sym    :   demodulated symbol
void framesync64_execute_rxpreamble(framesync64   _q,
                                    float complex _x)
{
    //
    float complex mf_out = 0.0f;
    int sample_available = framesync64_step(_q, _x, &mf_out);

    // compute output if timeout
    if (sample_available) {

        // TODO: absorb delays in filter

        // save output in p/n symbols buffer
        unsigned int m = 3;
        if (_q->preamble_counter >= 2*m)
            _q->preamble_rx[ _q->preamble_counter-2*m ] = mf_out;

        // update p/n counter
        _q->preamble_counter++;

        // update state
        if (_q->preamble_counter == 64 + 2*m)
            _q->state = FRAMESYNC64_STATE_RXPAYLOAD;
    }
}

// execute synchronizer, receiving payload
//  _q      :   frame synchronizer object
//  _x      :   input sample
//  _sym    :   demodulated symbol
void framesync64_execute_rxpayload(framesync64   _q,
                                   float complex _x)
{
    //
    float complex mf_out = 0.0f;
    int sample_available = framesync64_step(_q, _x, &mf_out);

    // compute output if timeout
    if (sample_available) {
        // save payload symbols (modem input/output)
        _q->payload_rx[_q->payload_counter] = mf_out;

        // increment counter
        _q->payload_counter++;

        if (_q->payload_counter == 630) {
            // recover data symbols from pilots
            qpilotsync_execute(_q->pilotsync, _q->payload_rx, _q->payload_sym);

            // decode payload
            _q->payload_valid = qpacketmodem_decode(_q->dec,
                                                    _q->payload_sym,
                                                    _q->payload_dec);

            // invoke callback
            if (_q->callback != NULL) {
                // set framestats internals
                _q->framestats.evm           = 0.0f; //20*log10f(sqrtf(_q->framestats.evm / 600));
                _q->framestats.rssi          = 20*log10f(_q->gamma_hat);
                _q->framestats.cfo           = nco_crcf_get_frequency(_q->mixer);
                _q->framestats.framesyms     = _q->payload_sym;
                _q->framestats.num_framesyms = 600;
                _q->framestats.mod_scheme    = LIQUID_MODEM_QPSK;
                _q->framestats.mod_bps       = 2;
                _q->framestats.check         = LIQUID_CRC_24;
                _q->framestats.fec0          = LIQUID_FEC_NONE;
                _q->framestats.fec1          = LIQUID_FEC_GOLAY2412;

                // invoke callback method
                _q->callback(&_q->payload_dec[0],   // header is first 8 bytes
                             _q->payload_valid,
                             &_q->payload_dec[8],   // payload is last 64 bytes
                             64,
                             _q->payload_valid,
                             _q->framestats,
                             _q->userdata);
            }

            // reset frame synchronizer
            framesync64_reset(_q);
            return;
        }
    }
}

// enable debugging
void framesync64_debug_enable(framesync64 _q)
{
    // create debugging objects if necessary
#if DEBUG_FRAMESYNC64
    if (_q->debug_objects_created)
        return;

    // create debugging objects
    _q->debug_x = windowcf_create(DEBUG_BUFFER_LEN);

    // set debugging flags
    _q->debug_enabled = 1;
    _q->debug_objects_created = 1;
#else
    fprintf(stderr,"framesync64_debug_enable(): compile-time debugging disabled\n");
#endif
}

// disable debugging
void framesync64_debug_disable(framesync64 _q)
{
    // disable debugging
#if DEBUG_FRAMESYNC64
    _q->debug_enabled = 0;
#else
    fprintf(stderr,"framesync64_debug_enable(): compile-time debugging disabled\n");
#endif
}


// print debugging information
void framesync64_debug_print(framesync64  _q,
                             const char * _filename)
{
#if DEBUG_FRAMESYNC64
    if (!_q->debug_objects_created) {
        fprintf(stderr,"error: framesync64_debug_print(), debugging objects don't exist; enable debugging first\n");
        return;
    }
    unsigned int i;
    float complex * rc;
    FILE* fid = fopen(_filename,"w");
    fprintf(fid,"%% %s: auto-generated file", _filename);
    fprintf(fid,"\n\n");
    fprintf(fid,"clear all;\n");
    fprintf(fid,"close all;\n\n");
    fprintf(fid,"n = %u;\n", DEBUG_BUFFER_LEN);

    // write x
    fprintf(fid,"x = zeros(1,n);\n");
    windowcf_read(_q->debug_x, &rc);
    for (i=0; i<DEBUG_BUFFER_LEN; i++)
        fprintf(fid,"x(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(1:length(x),real(x), 1:length(x),imag(x));\n");
    fprintf(fid,"ylabel('received signal, x');\n");

    // write p/n sequence
    fprintf(fid,"preamble_pn = zeros(1,64);\n");
    rc = _q->preamble_pn;
    for (i=0; i<64; i++)
        fprintf(fid,"preamble_pn(%4u) = %12.4e + 1i*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));

    // write p/n symbols
    fprintf(fid,"preamble_rx = zeros(1,64);\n");
    rc = _q->preamble_rx;
    for (i=0; i<64; i++)
        fprintf(fid,"preamble_rx(%4u) = %12.4e + 1i*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));

    // write payload symbols
    unsigned int payload_sym_len = 600;
    fprintf(fid,"payload_syms = zeros(1,%u);\n", payload_sym_len);
    rc = _q->payload_sym;
    for (i=0; i<payload_sym_len; i++)
        fprintf(fid,"payload_syms(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));

    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(real(payload_syms),imag(payload_syms),'o');\n");
    fprintf(fid,"xlabel('in-phase');\n");
    fprintf(fid,"ylabel('quadrature phase');\n");
    fprintf(fid,"grid on;\n");
    fprintf(fid,"axis([-1 1 -1 1]*1.5);\n");
    fprintf(fid,"axis square;\n");

#if 0
    // NCO, timing, etc.
    fprintf(fid,"symsync_index = zeros(1,664);\n");
    fprintf(fid,"nco_phase     = zeros(1,664);\n");
    for (i=0; i<664; i++) {
        fprintf(fid,"symsync_index(%4u) = %12.4e;\n", i+1, _q->debug_symsync_index[i]);
        fprintf(fid,"nco_phase(%4u)     = %12.4e;\n", i+1, _q->debug_nco_phase[i]);
    }
    fprintf(fid,"figure;\n");
    fprintf(fid,"subplot(2,1,1);\n");
    fprintf(fid,"  plot(nco_phase);\n");
    fprintf(fid,"  ylabel('nco phase');\n");
    fprintf(fid,"  grid on;\n");
    fprintf(fid,"subplot(2,1,2);\n");
    fprintf(fid,"  plot(symsync_index);\n");
    fprintf(fid,"  ylabel('symsync index');\n");
    fprintf(fid,"  grid on;\n");
#endif

    fprintf(fid,"\n\n");
    fclose(fid);

    printf("framesync64/debug: results written to %s\n", _filename);
#else
    fprintf(stderr,"framesync64_debug_print(): compile-time debugging disabled\n");
#endif
}

