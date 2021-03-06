/*
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

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <complex.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#ifdef USE_THREADS
#include <pthread.h>
#endif

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>

#include "defines.h"
#include "input.h"

#define RADIO_BUFCNT (8)
#define RADIO_BUFFER (512 * 1024)

static int gain_list[128];
static int gain_index, gain_count;

// signal and noise are squared magnitudes
static int snr_callback(void *arg, float snr)
{
    static int best_gain;
    static float best_snr;
    int result = 0;
    //rtlsdr_dev_t *dev = arg;
    SoapySDRKwargs args = {};
    SoapySDRKwargs_set(&args, "driver", "lime");
    SoapySDRDevice *dev = SoapySDRDevice_make(&args);
    SoapySDRKwargs_clear(&args);



    if (gain_count == 0)
        return result;

    // choose the best gain level
    if (snr >= best_snr)
    {
        best_gain = gain_index;
        best_snr = snr;
    }

    log_info("Gain: %.1f dB, CNR: %.1f dB", gain_list[gain_index] / 10.0, 20 * log10f(snr));

    if (gain_index + 1 >= gain_count || snr < best_snr * 0.5)
    {
        log_debug("Best gain: %d", gain_list[best_gain]);
        gain_index = best_gain;
        gain_count = 0;
    }
    else
    {
        gain_index++;
        // continue searching
        result = 1;
    }

    //rtlsdr_set_tuner_gain(dev, gain_list[gain_index]);
    //rtlsdr_reset_buffer(dev);
    SoapySDRDevice_setGain(dev, SOAPY_SDR_RX, 0, gain_list[gain_index]);
    return result;
}

#ifdef USE_THREADS
static void log_lock(void *udata, int lock)
{
    pthread_mutex_t *mutex = udata;
    if (lock)
        pthread_mutex_lock(mutex);
    else
        pthread_mutex_unlock(mutex);
}
#endif

unsigned int parse_freq(char *s)
{
    double d = strtod(s, NULL);
    if (d < 10000) d *= 1e6;
    return (unsigned int) d;
}

static void help(const char *progname)
{
    fprintf(stderr, "Usage: %s [-v] [-q] [-l log-level] [-d device-index] [-g gain] [-p ppm-error] [-r samples-input] [-w samples-output] [-o audio-output -f adts|hdc|wav] [--dump-aas-files directory] frequency program\n", progname);
}

int main(int argc, char *argv[])
{
    static const struct option long_opts[] = {
        { "dump-aas-files", required_argument, NULL, 1 },
        { 0 }
    };
    int err, opt, gain = INT_MIN, ppm_error = 0;
    unsigned int count, i, frequency = 0, program = 0, device_index = 0;
    char *input_name = NULL, *output_name = NULL, *audio_name = NULL, *format_name = NULL, *files_path = NULL;
    FILE *infp = NULL, *outfp = NULL;
    input_t input;
    output_t output;

    while ((opt = getopt_long(argc, argv, "r:w:d:p:o:f:g:ql:v", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
        case 1:
            files_path = optarg;
            break;
        case 'r':
            input_name = optarg;
            break;
        case 'w':
            output_name = optarg;
            break;
        case 'd':
            device_index = atoi(optarg);
            break;
        case 'p':
            ppm_error = atoi(optarg);
            break;
        case 'o':
            audio_name = optarg;
            break;
        case 'f':
            format_name = optarg;
            break;
        case 'g':
            gain = atoi(optarg);
            break;
        case 'q':
            log_set_quiet(1);
            break;
        case 'l':
            log_set_level(atoi(optarg));
            break;
        case 'v':
            printf("nrsc5 revision %s\n", GIT_COMMIT_HASH);
            return 0;
        default:
            help(argv[0]);
            return 0;
        }
    }

#ifdef USE_THREADS
    pthread_mutex_t log_mutex;
    pthread_mutex_init(&log_mutex, NULL);
    log_set_lock(log_lock);
    log_set_udata(&log_mutex);
#endif

    if (input_name == NULL)
    {
        if (optind + 2 != argc)
        {
            help(argv[0]);
            return 0;
        }
        frequency = parse_freq(argv[optind]);
        program = strtoul(argv[optind+1], NULL, 0);

        size_t length;

        //enumerate devices
        SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &length);
        for (size_t i = 0; i < length; i++)
        {
            //printf("Found device #%d: ", (int)i);
            for (size_t j = 0; j < results[i].size; j++)
            {
                log_info("[%d] %s,", results[i].keys[j], results[i].vals[j]);
            }
            count = i;       
            //printf("\n");
        }
        SoapySDRKwargsList_clear(results, length);

    
        if (count == 0)
        {
            log_fatal("No devices found!");
            return 1;
        }

        //for (i = 0; i < count; ++i)
        //    log_info("[%d] %s", i, rtlsdr_get_device_name(i));

        if (device_index >= count)
        {
            log_fatal("Selected device does not exist.");
            return 1;
        }
    }
    else
    {
        if (optind + 1 != argc)
        {
            help(argv[0]);
            return 0;
        }
        program = strtoul(argv[optind], NULL, 0);

        if (strcmp(input_name, "-") == 0)
            infp = stdin;
        else
            infp = fopen(input_name, "rb");

        if (infp == NULL)
        {
            log_fatal("Unable to open input file.");
            return 1;
        }
    }

    if (output_name != NULL)
    {
        outfp = fopen(output_name, "wb");
        if (outfp == NULL)
        {
            log_fatal("Unable to open output file.");
            return 1;
        }
    }

    if (audio_name != NULL)
    {
        if (format_name == NULL)
        {
            log_fatal("Must specify an output format.");
            return 1;
        }
        else if (strcmp(format_name, "wav") == 0)
        {
#ifdef USE_FAAD2
            output_init_wav(&output, audio_name);
#else
            log_fatal("WAV output requires FAAD2.");
            return 1;
#endif
        }
        else if (strcmp(format_name, "adts") == 0)
        {
            output_init_adts(&output, audio_name);
        }
        else if (strcmp(format_name, "hdc") == 0)
        {
            output_init_hdc(&output, audio_name);
        }
        else
        {
            log_fatal("Unknown output format.");
            return 1;
        }
    }
    else
    {
#ifdef USE_FAAD2
        output_init_live(&output);
#else
        log_fatal("Live output requires FAAD2.");
        return 1;
#endif
    }

    output_set_aas_files_path(&output, files_path);

    input_init(&input, &output, frequency, program, outfp);

    if (infp)
    {
        while (!feof(infp))
        {
            uint8_t tmp[RADIO_BUFFER];
            size_t cnt;
            cnt = fread(tmp, 4, sizeof(tmp) / 4, infp);
            if (cnt > 0)
                input_cb(tmp, cnt * 4, &input);
        }
    }
    else
    {
        uint8_t *buf = malloc(128 * SNR_FFT_COUNT);
        //rtlsdr_dev_t *dev;

        //err = rtlsdr_reset_buffer(dev);
        //if (err) FATAL_EXIT("rtlsdr_reset_buffer error: %d", err);
        void *buffs[] = {buf}; //array of buffers
        int flags = 0; //flags set by receive operation
        long long timeNs = 1000000; //timestamp for receive buffer

        //setup a stream (complex floats)
        SoapySDRStream *rxStream;

        //create device instance
        //args can be user defined or from the enumeration result
        SoapySDRKwargs args = {};
        SoapySDRKwargs_set(&args, "driver", "lime");
        SoapySDRDevice *sdr = SoapySDRDevice_make(&args);
        SoapySDRKwargs_clear(&args);

        // use a smaller buffer during auto gain
        int len = 128 * SNR_FFT_COUNT;

        if (sdr == NULL)
        {
            FATAL_EXIT("SoapySDRDevice_make fail: %s", SoapySDRDevice_lastError());
        }

        if (SoapySDRDevice_setSampleRate(sdr, SOAPY_SDR_RX, 0, 1488375) != 0)
        {
            FATAL_EXIT("setSampleRate fail: %s", SoapySDRDevice_lastError());
        }


        //err = rtlsdr_open(&dev, 0);
        //if (err) FATAL_EXIT("rtlsdr_open error: %d", err);
        //err = rtlsdr_set_sample_rate(dev, 1488375);
        //if (err) FATAL_EXIT("rtlsdr_set_sample_rate error: %d", err);
        // ^^ rtlsdr requires setting manual gain mode before setting gain, ignoring for lime:
        //err = rtlsdr_set_tuner_gain_mode(dev, 1);
        //if (err) FATAL_EXIT("rtlsdr_set_tuner_gain_mode error: %d", err);
        // ^^ lime can set ppm error at freq assign, ignoring for now:
        //err = rtlsdr_set_freq_correction(dev, ppm_error);
        //if (err && err != -2) FATAL_EXIT("rtlsdr_set_freq_correction error: %d", err);

        // ^^ may need to look at setFrequencyComponent if this doesn't work
        if (SoapySDRDevice_setFrequency(sdr, SOAPY_SDR_RX, 0, frequency, NULL) != 0)
        {
            FATAL_EXIT("setFrequency fail: %s", SoapySDRDevice_lastError());
        }

        //err = rtlsdr_set_center_freq(dev, frequency);
        //if (err) FATAL_EXIT("rtlsdr_set_center_freq error: %d", err);

        if (gain == INT_MIN)
        {
            SoapySDRRange gains = SoapySDRDevice_getGainRange(sdr, SOAPY_SDR_RX, 0);
            //gain_count = rtlsdr_get_tuner_gains(dev, gain_list);
            gain_count = gains.maximum - gains.minimum;
            // ^ we will need to fill gain list manually from range struct
            for (size_t i = 0; i < gain_count; i++)
            {
                gain_list[i] = gains.minimum + i;
            }
            if (gain_count > 0)
            {
                input_set_snr_callback(&input, snr_callback, sdr);
                //err = rtlsdr_set_tuner_gain(dev, gain_list[0]);
                err = SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, gain_list[0]);
                if (err != 0) FATAL_EXIT("sdr_set_tuner_gain error: %d", err);
            }
        }
        else
        {
            //err = rtlsdr_set_tuner_gain(dev, gain);
            err = SoapySDRDevice_setGain(sdr, SOAPY_SDR_RX, 0, gain);
            if (err != 0) FATAL_EXIT("sdr_set_tuner_gain error b: %d", err);
        }

        

        // special loop for modifying gain (we can't use async transfers)
        while (gain_count)
        {
            

            
            //err = rtlsdr_read_sync(dev, buf, len, &len);
            if (SoapySDRDevice_setupStream(sdr, &rxStream, SOAPY_SDR_RX, SOAPY_SDR_CF32, NULL, 0, NULL) != 0)
            {
                FATAL_EXIT("setupStream fail: %s", SoapySDRDevice_lastError());
            }
            SoapySDRDevice_activateStream(sdr, rxStream, 0, 0, 0); //start streaming

            //create a re-usable buffer for rx samples
            complex float buff[128 * SNR_FFT_COUNT];

            //receive sample
            int ret = SoapySDRDevice_readStream(sdr, rxStream, buffs, 1024, &flags, &timeNs, 100000);
            //printf("ret=%d, flags=%d, timeNs=%lld\n", ret, flags, timeNs);
            //if (err) FATAL_EXIT("rtlsdr_read_sync error: %d", err);

            input_cb(buf, ret * 2, rxStream);
        }
        free(buf);

        //err = rtlsdr_read_async(dev, input_cb, &input, RADIO_BUFCNT, RADIO_BUFFER);
        //if (err) FATAL_EXIT("rtlsdr_read_async error: %d", err);
        int ret = SoapySDRDevice_readStream(sdr, rxStream, buffs, 1024, &flags, &timeNs, 100000);
        input_cb(buf, ret * 2, rxStream);
        //err = rtlsdr_close(dev);
        //if (err) FATAL_EXIT("rtlsdr error: %d", err);
    }

    return 0;
}

