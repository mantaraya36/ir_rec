#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>


//#include <lo/lo.h>
//#include <portaudio.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <sndfile.h>

//#include <time.h>

#define MAX_IN_CHANS 8
#define RB_SIZE 8192*4
#define MAX_PATH_LEN 256
#define PRECOUNT 4096*64

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
    jack_ringbuffer_t *write_rb;
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
    pthread_mutex_t disk_thread_lock;
    pthread_cond_t  data_ready;

} audio_userdata_t;

int jack_process (jack_nframes_t nframes, void *arg);
void jack_shutdown (void *arg);
int jack_xrun_callback(void *arg);
int init_jack(audio_userdata_t *audio_ud);
void free_jack(audio_userdata_t *audio_ud);

int read_impulse(const char *path, float **buffer, SF_INFO *sf_info_impulse);

void *writer_thread(void *data);

int running;
static void finish(int ignore){ running = 0; fprintf(stderr, "Interrupted.\n");}

int main(int argc, char *argv[])
{
    int ret, c, i;
    audio_userdata_t audio_ud;

    /* Parse command line flags */
    if (argc == 6) {
        audio_ud.num_in_chnls = atoi(argv[1]);
        audio_ud.num_out_chnls = atoi(argv[2]);
        strncpy(audio_ud.impulse_path, argv[3], MAX_PATH_LEN);
        strncpy(audio_ud.out_prefix, argv[4], MAX_PATH_LEN);
        audio_ud.samp_target = atoi(argv[5]);
    } else if (argc == 1) {
        audio_ud.num_in_chnls = 2;
        audio_ud.num_out_chnls = 54;
        strncpy(audio_ud.impulse_path, "sweep.wav", MAX_PATH_LEN);
        strncpy(audio_ud.out_prefix, "run/", MAX_PATH_LEN);
        audio_ud.samp_target = 5; /* seconds */
    } else {
        printf("Usage:\n"
               "%s numinch numoutch impulsepath outprefix totalrectime\n",
               argv[0]);
        return 1;
    }
    printf("Opening %i inputs, %i outputs\n", audio_ud.num_in_chnls, audio_ud.num_out_chnls);

    audio_ud.playback_gain = 0.3;
    audio_ud.precount = 0;

    /* Create ring buffer */
    audio_ud.write_rb = jack_ringbuffer_create(RB_SIZE * audio_ud.num_in_chnls * sizeof(float));
    int res = jack_ringbuffer_mlock(audio_ud.write_rb);
    jack_ringbuffer_reset(audio_ud.write_rb);

    /* initialize data */
    audio_ud.samp_count = 0;
    audio_ud.cur_play_index = 0;
    pthread_mutex_init(&audio_ud.disk_thread_lock, NULL);
    pthread_cond_init(&audio_ud.data_ready, NULL);

    if (read_impulse(audio_ud.impulse_path, &audio_ud.impulse_samples,
                     &audio_ud.sf_info_impulse)) { return ret; }
    if (init_jack(&audio_ud)) { return ret; }
    if (audio_ud.jack_sr != audio_ud.sf_info_impulse.samplerate) {
        free_jack(&audio_ud);
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

    running = 1;
    (void) signal(SIGINT, finish);
    while (audio_ud.running && running) {
        usleep(200000);
    }
    audio_ud.running = 0;
    pthread_join(thread, NULL);

    free_jack(&audio_ud);
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
        printf("Error, soundfile '%s' contains more than 1 channel.\n", path);
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
    jack_set_xrun_callback (client, jack_xrun_callback, NULL);
    jack_on_shutdown (client, jack_shutdown, 0);
    fprintf (stdout, "Engine sample rate: %u\n", jack_get_sample_rate (client));

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
    audio_ud->samp_target *= jack_get_sample_rate (client);
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
    float *buf;

    for (i = 0; i < ud->num_out_chnls; i++) {
        buf = jack_port_get_buffer(ud->output_ports[i], nframes);
        memset(buf, 0, nframes * sizeof(jack_default_audio_sample_t) );
    }
    if (ud->precount < PRECOUNT) {
        ud->precount += nframes;
        return 0;
    }

    /* playback */

    if (ud->cur_play_index >= ud->num_out_chnls) {
        ud->done = 1;
        return 0;
    }
    buf = jack_port_get_buffer(ud->output_ports[ud->cur_play_index], nframes);
    for (i = 0; i < nframes; i++) {
        if (ud->samp_count == ud->samp_target) { /* start new measurement */
            fprintf(stdout, "Finished chan %i\n", ud->cur_play_index);
            ud->samp_count = 0;
            ud->cur_play_index++;
            if (ud->cur_play_index  > ud->num_out_chnls) {
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

    for (i = 0; i < ud->num_in_chnls; i++) {
        ud->in_bufs[i] = jack_port_get_buffer(ud->input_ports[i], nframes);
    }
    /* interleave channels */
    for (samp = 0; samp < nframes; samp++) {
        float frame[MAX_IN_CHANS];
        for (chan = 0; chan < ud->num_in_chnls; chan++) {
            frame[chan] = ud->in_bufs[chan][samp];
        }
        jack_ringbuffer_write(ud->write_rb, (const char *) frame,
                              sizeof(float) * ud->num_in_chnls);
    }
    if (pthread_mutex_trylock (&ud->disk_thread_lock) == 0) {
        pthread_cond_signal (&ud->data_ready);
        pthread_mutex_unlock (&ud->disk_thread_lock);
    }
    return 0;
}

void jack_shutdown (void *arg)
{
    fprintf(stderr, "Error. Jack shutdown!\n");
}

int jack_xrun_callback(void *arg)
{
    fprintf(stderr, "Error. Jack xrun.\n");
}

void *writer_thread(void *data)
{
    audio_userdata_t *ud = (audio_userdata_t *) data;
    float audio[512];
    pthread_setcanceltype (PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_mutex_lock (&ud->disk_thread_lock);
    while (ud->running)  {
        while (jack_ringbuffer_read_space(ud->write_rb) >= sizeof(float) * ud->num_in_chnls) {
            int samps_read = jack_ringbuffer_read(ud->write_rb, (char *) audio,
                                              sizeof(float) * ud->num_in_chnls)/ sizeof(float);
            assert(samps_read == ud->num_in_chnls);
            if (samps_read > 0) {
                ud->rec_counter += samps_read;
                if (ud->rec_counter/ud->num_in_chnls == ud->samp_target) {
                    sf_close(ud->sf);
                    ud->cur_file_index++;
                    char filename[MAX_PATH_LEN];
                    sprintf(filename, "%sout_%02i.wav", ud->out_prefix, ud->cur_file_index);
                    ud->sf = sf_open(filename, SFM_WRITE, &ud->rec_sfinfo);
                    if (!ud->sf) {
                        fprintf(stderr, "Error writing file '%s'\n", filename);
                    }
                    ud->rec_counter = 0;
                }
                sf_write_float(ud->sf, audio, samps_read);
            }
        }
        pthread_cond_wait (&ud->data_ready, &ud->disk_thread_lock);
    }
    pthread_mutex_unlock (&ud->disk_thread_lock);
    fprintf(stdout, "Thread done.\n");
}
