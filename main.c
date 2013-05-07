#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//#include <lo/lo.h>
//#include <portaudio.h>
#include <jack/jack.h>
#include <sndfile.h>

#include <time.h>

#include "pa_ringbuffer.h"

#define MAX_IN_CHANS 8
#define RB_SIZE MAX_IN_CHANS * 8192
#define MAX_PATH_LEN 256

typedef struct {
    /* jack members */
    jack_client_t *client;
    jack_port_t **input_ports;
    jack_port_t **output_ports;
    jack_default_audio_sample_t *in_bufs[MAX_IN_CHANS];
    double **out_bufs;

    /* settings */
    int num_out_chnls;
    int num_in_chnls;
    char impulse_path[MAX_PATH_LEN];
    char out_prefix[MAX_PATH_LEN];
    int jack_sr;
    float playback_gain;

    /* internals */
    SF_INFO sf_info_impulse;
    float *impulse_samples;
    PaUtilRingBuffer *write_rb;
    float *interlv_buf;
    int precount;

    int samp_count, samp_target;
    int cur_play_index;
    int done;

    /* for writer thread */
    int running;
    int cur_file_index;
    SF_INFO rec_sfinfo;
    SNDFILE *sf;
    int rec_counter;

} audio_userdata_t;

int jack_process (jack_nframes_t nframes, void *arg);
void jack_shutdown (void *arg);
int init_jack(audio_userdata_t *audio_ud);
void free_jack(audio_userdata_t *audio_ud);

int read_impulse(const char *path, float **buffer, SF_INFO *sf_info_impulse);

void *writer_thread(void *data);

int main(int argc, char *argv[])
{
    int ret, c, i;
    audio_userdata_t audio_ud;
    jack_default_audio_sample_t *rb_buffer_out;

    /* Parse command line flags */
    if (argc == 6) {
        audio_ud.num_in_chnls = atoi(argv[1]);
        audio_ud.num_out_chnls = atoi(argv[2]);
        strncpy(audio_ud.impulse_path, argv[3], MAX_PATH_LEN);
        strncpy(audio_ud.out_prefix, argv[4], MAX_PATH_LEN);
        audio_ud.samp_target = atoi(argv[5]);
    } else if (argc == 1) {
        audio_ud.num_in_chnls = 4;
        audio_ud.num_out_chnls = 4;
        strncpy(audio_ud.impulse_path, "sweep.wav", MAX_PATH_LEN);
        strncpy(audio_ud.out_prefix, "run/", MAX_PATH_LEN);
        audio_ud.samp_target = 4096*32;
    } else {
        printf("Usage:\n"
               "%s numinch numoutch impulsepath outprefix totalrectime\n",
               argv[0]);
        return 1;
    }
    printf("Opening %i inputs, %i outputs\n", audio_ud.num_in_chnls, audio_ud.num_out_chnls);

    audio_ud.playback_gain = 0.1;
    audio_ud.precount = 0;

    /* Create ring buffer */
    audio_ud.write_rb = (PaUtilRingBuffer *) calloc(audio_ud.num_in_chnls, sizeof(PaUtilRingBuffer));
    for (i= 0; i < audio_ud.num_in_chnls; i++) {
        // TODO free this memory
        rb_buffer_out = (jack_default_audio_sample_t *) calloc(RB_SIZE, sizeof(jack_default_audio_sample_t));
        PaUtil_InitializeRingBuffer(&(audio_ud.write_rb[i]),
                                    sizeof(jack_default_audio_sample_t),
                                    RB_SIZE, (void *) rb_buffer_out);
    }

    /* initialize data */
    audio_ud.samp_count = 0;
    audio_ud.cur_play_index = 0;

    if (read_impulse(audio_ud.impulse_path, &audio_ud.impulse_samples,
                     &audio_ud.sf_info_impulse)) { return ret; }
    if (init_jack(&audio_ud)) { return ret; }
    if (audio_ud.jack_sr != audio_ud.sf_info_impulse.samplerate) {
        free_jack(&audio_ud);
        free(rb_buffer_out);
        free(audio_ud.impulse_samples);
        printf("Error. Jack sample rate and impulse file sample rate mismatch.\n");
        return 1;
    }
    pthread_t thread;
    int  iret1;
    audio_ud.running = 1;
    audio_ud.rec_counter = 0;
    audio_ud.cur_file_index = 0;
    audio_ud.rec_sfinfo.channels = audio_ud.num_in_chnls;
    audio_ud.rec_sfinfo.samplerate = audio_ud.jack_sr;
    audio_ud.rec_sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    char filename[MAX_PATH_LEN];
    sprintf(filename, "%sout_%02i.wav", audio_ud.out_prefix, audio_ud.cur_file_index);
    audio_ud.sf = sf_open(filename, SFM_WRITE, &audio_ud.rec_sfinfo);
    if (!audio_ud.sf) {
        printf("Error creating file %s: %s\n", filename, sf_strerror(audio_ud.sf));
    }

    iret1 = pthread_create( &thread, NULL, &writer_thread, (void*) &audio_ud);

    printf("Press ENTER to exit.\n");
    c = getchar();
    audio_ud.running = 0;
    pthread_join(thread, NULL);

//    free(rb_buffer_out);
    free_jack(&audio_ud);
    free(audio_ud.interlv_buf);
    return 0;
}

int read_impulse(const char *path, float **buffer, SF_INFO *sf_info_impulse)
{
    SNDFILE *sf;

    sf_info_impulse->format = 0;
    sf = sf_open(path, SFM_READ, sf_info_impulse);
    if (!sf) {
        printf("Error reading soundfile '%s'.\n", path);
        return 1;
    }
    if (sf_info_impulse->channels != 1) {
        printf("Error, soundfile '%s'' contains more than 1 channel.\n", path);
        return 1;
    }
    *buffer = (float *) calloc(sf_info_impulse->frames, sizeof(float));
    if (sf_read_float(sf, *buffer, sf_info_impulse->frames)
            != sf_info_impulse->frames) {
        printf("Warning, wrong impulse samples read count. Continuing.\n");
        return 1;
    }
}

int init_jack(audio_userdata_t *audio_ud)
{
    jack_client_t *client;
    int i;

    if ((client = jack_client_new ("ir_rec")) == 0) {
        fprintf (stderr, "jack server not running?\n");
        return 1;
    }
    jack_set_process_callback (client, jack_process, audio_ud);

    jack_on_shutdown (client, jack_shutdown, 0);
    printf ("engine sample rate: %ui\n", jack_get_sample_rate (client));

    audio_ud->input_ports = (jack_port_t **) calloc(audio_ud->num_in_chnls, sizeof(jack_port_t *));
    for (i = 0; i < audio_ud->num_in_chnls; i++) {
        char name[16];
        sprintf(name, "input_%i", i);
        audio_ud->input_ports[i] = jack_port_register (client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
    }
    audio_ud->output_ports = (jack_port_t **) calloc(audio_ud->num_out_chnls, sizeof(jack_port_t *));
    for (i = 0; i < audio_ud->num_out_chnls; i++) {
        char name[16];
        sprintf(name, "output_%i", i);
        audio_ud->output_ports[i] = jack_port_register (client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }

    audio_ud->interlv_buf
            = (float *) calloc(audio_ud->num_in_chnls * jack_get_buffer_size(client),
                                            sizeof(float));

    if (jack_activate (client)) {
        fprintf (stderr, "cannot activate client\n");
        return 1;
    }
    audio_ud->client = client;
    audio_ud->jack_sr = jack_get_sample_rate (client);
    return 0;
}

void free_jack(audio_userdata_t *audio_ud)
{
    int i;
    jack_client_t *client = audio_ud->client;
    for (i = 0; i < audio_ud->num_in_chnls; i++) {
         jack_port_unregister (client, audio_ud->input_ports[i]);
    }
    for (i = 0; i < audio_ud->num_out_chnls; i++) {
         jack_port_unregister (client, audio_ud->output_ports[i]);
    }
    if (jack_deactivate (client)) {
        fprintf (stderr, "cannot activate client\n");
    }
    jack_client_close(client);
    free(audio_ud->output_ports);
    free(audio_ud->input_ports);
}

int jack_process (jack_nframes_t nframes, void *arg)
{
    int i;
    int chan, samp;
    audio_userdata_t *ud = (audio_userdata_t *) arg;
    if (ud->precount < 4096*4) {
        ud->precount += nframes;
        return 0;
    }
    /* interleave channels */
    float *ibuf = ud->interlv_buf;
    for (i = 0; i < ud->num_in_chnls; i++) {
        ud->in_bufs[i] = jack_port_get_buffer(ud->input_ports[i], nframes);
    }
    for (samp = 0; samp < nframes; samp++) {
        for (chan = 0; chan < ud->num_in_chnls; chan++) {
            *ibuf++ = ud->in_bufs[chan][samp];
        }
    }

    if (!PaUtil_GetRingBufferWriteAvailable(ud->write_rb)) {
        fprintf(stderr, "Error, ring buffer underrun.\n");
    }
    PaUtil_WriteRingBuffer(ud->write_rb, ud->interlv_buf, nframes*ud->num_in_chnls);
    /* playback */
    for (i = 0; i < ud->num_in_chnls; i++) {
        ud->in_bufs[i] = jack_port_get_buffer(ud->input_ports[i], nframes);
    }
    jack_default_audio_sample_t *buf;
    for (i = 0; i < ud->num_out_chnls; i++) {
        buf = jack_port_get_buffer(ud->output_ports[i], nframes);
        memset(buf, 0, nframes * sizeof(jack_default_audio_sample_t) );
    }
    buf = jack_port_get_buffer(ud->output_ports[0], nframes);
    for (i = 0; i < nframes; i++) {
        if (ud->cur_play_index >= ud->num_out_chnls) {
            ud->done = 1;
            return 0;
        }
        if (ud->samp_count > ud->samp_target) { /* start new measurement */
            fprintf(stdout, "Finished chan %i\n", ud->cur_play_index);
            ud->samp_count = 0;
            ud->cur_play_index++;
            if (ud->cur_play_index < ud->num_out_chnls) {
                buf = jack_port_get_buffer(ud->output_ports[ud->cur_play_index], nframes);
            }
        }
        if (ud->samp_count < ud->sf_info_impulse.frames) {
            buf[i] = ud->impulse_samples[ud->samp_count] * ud->playback_gain;
        } else {
            buf[i] = 0.0;
        }
        ud->samp_count++;
    }
    return 0;
}

void jack_shutdown (void *arg)
{
    printf("Error. Jack shutdown!\n");
}


void *writer_thread(void *data)
{
    audio_userdata_t *ud = (audio_userdata_t *) data;
    float audio[128];
    while (ud->running)  {
        int samps_read = PaUtil_ReadRingBuffer(ud->write_rb, audio, 128);
        sf_write_float(ud->sf, audio, samps_read);
        ud->rec_counter += samps_read;
        if (samps_read > 0) {
            if (ud->rec_counter/ud->num_in_chnls > ud->samp_target) {
                sf_close(ud->sf);
                ud->cur_file_index++;
                char filename[MAX_PATH_LEN];
                sprintf(filename, "%sout_%02i.wav", ud->out_prefix, ud->cur_file_index);
                ud->sf = sf_open(filename, SFM_WRITE, &ud->rec_sfinfo);
                ud->rec_counter = 0;
            }
        }
        nanosleep((struct timespec[]){{0, 10}}, NULL);
    }
    printf("Thread done.");
}
