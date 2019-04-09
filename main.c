#include "main.h"

#define WS_PORT         7681
#define WS_INTERVAL		250

#define FFT_SIZE        1024
#define FFT_TIME_SMOOTH 0.999f // 0.0 - 1.0

#define AIRSPY_FREQ     745000000

#define AIRSPY_SAMPLE   10000000

#define AIRSPY_SERIAL	0x644064DC2354AACD // WB

/** LWS Vars **/
int max_poll_elements;
int debug_level = 3;
volatile int force_exit = 0;
struct lws_context *context;

pthread_t fftThread;

/** AirSpy Vars **/
struct airspy_device* device = NULL;
/* Sample type -> 32bit Complex Float */
enum airspy_sample_type sample_type_val = AIRSPY_SAMPLE_FLOAT32_IQ;
/* Sample rate */
uint32_t sample_rate_val = AIRSPY_SAMPLE;
/* DC Bias Tee -> 0 (disabled) */
uint32_t biast_val = 0;
/* Linear Gain */
#define LINEAR
uint32_t linearity_gain_val = 12; // MAX=21
/* Sensitive Gain */
//#define SENSITIVE
uint32_t sensitivity_gain_val = 10; // MAX=21
/* Frequency */
uint32_t freq_hz = AIRSPY_FREQ;

double hanning_window_const[FFT_SIZE];

int airspy_rx(airspy_transfer_t* transfer);

#define FLOAT32_EL_SIZE_BYTE (4)
fftw_complex* fft_in;
fftw_complex*   fft_out;
fftw_plan   fft_plan;

void setup_fft(void)
{
    /* Set up FFTW */
    fft_in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    fft_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * FFT_SIZE);
    fft_plan = fftw_plan_dft_1d(FFT_SIZE, fft_in, fft_out, FFTW_FORWARD, FFTW_PATIENT);
}

static void close_airspy(void)
{
    int result;
    
    /* De-init AirSpy device */
    if(device != NULL)
    {
	    result = airspy_stop_rx(device);
	    if( result != AIRSPY_SUCCESS ) {
		    printf("airspy_stop_rx() failed: %s (%d)\n", airspy_error_name(result), result);
	    }

	    result = airspy_close(device);
	    if( result != AIRSPY_SUCCESS ) 
	    {
		    printf("airspy_close() failed: %s (%d)\n", airspy_error_name(result), result);
	    }
	
	    airspy_exit();
    }
}

static void close_fftw(void)
{
    /* De-init fftw */
    fftw_free(fft_in);
    fftw_free(fft_out);
    fftw_destroy_plan(fft_plan);
}

static uint8_t setup_airspy()
{
    int result;

    result = airspy_init();
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_init() failed: %s (%d)\n", airspy_error_name(result), result);
	    return 0;
    }
    #ifdef AIRSPY_SERIAL
    	result = airspy_open_sn(&device, AIRSPY_SERIAL);
    #else
    	result = airspy_open(&device);
    #endif
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_open() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_sample_type(device, sample_type_val);
    if (result != AIRSPY_SUCCESS) {
	    printf("airspy_set_sample_type() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_samplerate(device, sample_rate_val);
    if (result != AIRSPY_SUCCESS) {
	    printf("airspy_set_samplerate() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_rf_bias(device, biast_val);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_set_rf_bias() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    #ifdef LINEAR
	    result =  airspy_set_linearity_gain(device, linearity_gain_val);
	    if( result != AIRSPY_SUCCESS ) {
		    printf("airspy_set_linearity_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	    }
    #elif defined SENSITIVE
	    result =  airspy_set_sensitivity_gain(device, sensitivity_gain_val);
	    if( result != AIRSPY_SUCCESS ) {
		    printf("airspy_set_sensitivity_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	    }
    #endif

    result = airspy_start_rx(device, airspy_rx, NULL);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_start_rx() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }

    result = airspy_set_freq(device, freq_hz);
    if( result != AIRSPY_SUCCESS ) {
	    printf("airspy_set_freq() failed: %s (%d)\n", airspy_error_name(result), result);
	    airspy_close(device);
	    airspy_exit();
	    return 0;
    }
    
    return 1;
}

typedef struct {
	uint32_t index;
	uint32_t size;
	char data[65536 * FLOAT32_EL_SIZE_BYTE];
	pthread_mutex_t mutex;
	pthread_cond_t 	signal;
} rf_buffer_t;

rf_buffer_t rf_buffer = {
	.index = 0,
	.size = 0,
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.signal = PTHREAD_COND_INITIALIZER,
	.data = { 0xFF }
};

/* Airspy RX Callback, this is called by a new thread within libairspy */
int airspy_rx(airspy_transfer_t* transfer)
{    
	/* transfer->sample_count is normally 65536 */
	/* TODO: I think sample_count is counting 2 for each complex sample (ie. absolute number of samples) */
    if(transfer->samples != NULL && transfer->sample_count>=65536)
    {
        pthread_mutex_lock(&rf_buffer.mutex);
        rf_buffer.index = 0;
        memcpy(
            rf_buffer.data,
            transfer->samples,
            (65536 * FLOAT32_EL_SIZE_BYTE)
        );
        rf_buffer.size = transfer->sample_count / (FFT_SIZE * 2); // Number of potential FFTs
        pthread_cond_signal(&rf_buffer.signal);
        pthread_mutex_unlock(&rf_buffer.mutex);
    }
	return 0;
}

typedef struct {
	float data[FFT_SIZE];
	pthread_mutex_t mutex;
} fft_buffer_t;

fft_buffer_t fft_buffer = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
};

/* FFT Thread */
void *thread_fft(void *dummy)
{
    (void) dummy;
    int             i, offset;
    fftw_complex    pt;
    double           pwr, lpwr;

	double pwr_scale = 1.0 / ((float)FFT_SIZE * (float)FFT_SIZE);

    while(1)
    {
    	/* Lock input buffer */
    	pthread_mutex_lock(&rf_buffer.mutex);

    	if(rf_buffer.index == rf_buffer.size)
    	{
	    	/* Wait for signalled input */
	    	pthread_cond_wait(&rf_buffer.signal, &rf_buffer.mutex);
    	}

    	offset = rf_buffer.index * FFT_SIZE * 2;

    	/* Copy data out of rf buffer into fft_input buffer */
    	for (i = 0; i < FFT_SIZE; i++)
	    {
	        fft_in[i][0] = ((float*)rf_buffer.data)[offset+(2*i)] * hanning_window_const[i];
	        fft_in[i][1] = ((float*)rf_buffer.data)[offset+(2*i)+1] * hanning_window_const[i];
	    }

	    rf_buffer.index++;

	    /* Unlock input buffer */
    	pthread_mutex_unlock(&rf_buffer.mutex);

    	/* Run FFT */
    	fftw_execute(fft_plan);

    	/* Lock output buffer */
    	pthread_mutex_lock(&fft_buffer.mutex);

    	for (i = 0; i < FFT_SIZE; i++)
	    {
	        /* shift, normalize and convert to dBFS */
	        if (i < FFT_SIZE / 2)
	        {
	            pt[0] = fft_out[FFT_SIZE / 2 + i][0] / FFT_SIZE;
	            pt[1] = fft_out[FFT_SIZE / 2 + i][1] / FFT_SIZE;
	        }
	        else
	        {
	            pt[0] = fft_out[i - FFT_SIZE / 2][0] / FFT_SIZE;
	            pt[1] = fft_out[i - FFT_SIZE / 2][1] / FFT_SIZE;
	        }
	        pwr = pwr_scale * (pt[0] * pt[0]) + (pt[1] * pt[1]);
	        lpwr = 10.f * log10(pwr + 1.0e-20);
	        
	        fft_buffer.data[i] = (lpwr * (1.f - FFT_TIME_SMOOTH)) + (fft_buffer.data[i] * FFT_TIME_SMOOTH);
	    }

	    /* Unlock output buffer */
    	pthread_mutex_unlock(&fft_buffer.mutex);
    }

}

#define WEBSOCKET_OUTPUT_LENGTH	4096
typedef struct {
	uint8_t buffer[LWS_PRE+WEBSOCKET_OUTPUT_LENGTH];
	uint32_t length;
	uint32_t sequence_id;
	pthread_mutex_t mutex;
} websocket_output_t;

websocket_output_t websocket_output = {
	.length = 0,
	.sequence_id = 0,
	.mutex = PTHREAD_MUTEX_INITIALIZER
};

#define FLOOR_TARGET	9300
#define FLOOR_TIME_SMOOTH 0.995

uint16_t lowest_smooth = 11118; // value found in testing

void fft_to_string(void)
{
	int32_t i, j;
	uint16_t lowest, offset;
	uint16_t *websocket_output_buffer_ptr;

    /* Lock FFT output buffer for reading */
    pthread_mutex_lock(&fft_buffer.mutex);

    /* Lock websocket output buffer for writing */
    pthread_mutex_lock(&websocket_output.mutex);
    websocket_output_buffer_ptr = (uint16_t *)&websocket_output.buffer[LWS_PRE];

    /* Create and append data points */
    i = 0;
    for(j=(FFT_SIZE*0.1);j<(FFT_SIZE*0.9);j++)
    {
    	websocket_output_buffer_ptr[i] = (uint16_t)((3000*(fft_buffer.data[j] - 33999))) + fft_line_compensation[j] + 5000;
    	i++;
    }

    /* Unlock FFT output buffer */
    pthread_mutex_unlock(&fft_buffer.mutex);

   	/* Calculate noise floor */
   	lowest = 0xFFFF;
   	for(j = 0; j < i; j++)
    {
    	if(websocket_output_buffer_ptr[j] < lowest)
    	{
    		lowest = websocket_output_buffer_ptr[j];
    	}
    }
    lowest_smooth = (lowest * (1.f - FLOOR_TIME_SMOOTH)) + (lowest_smooth * FLOOR_TIME_SMOOTH);

    /* Compensate for noise floor */
    offset = FLOOR_TARGET - lowest_smooth;
    //printf("smooth: %d, offset: %d\n", lowest_smooth,offset);
    for(j = 0; j < i; j++)
    {
    	websocket_output_buffer_ptr[j] += offset;
    }

    websocket_output.length = 2*i;
    websocket_output.sequence_id++;

	pthread_mutex_unlock(&websocket_output.mutex);
}

typedef struct websocket_user_session_t websocket_user_session_t;

struct websocket_user_session_t {
	struct lws *wsi;
	websocket_user_session_t *websocket_user_session_list;
	uint32_t last_sequence_id;
};

typedef struct {
	struct lws_context *context;
	struct lws_vhost *vhost;
	const struct lws_protocols *protocol;
	websocket_user_session_t *websocket_user_session_list;
} websocket_vhost_session_t;

int callback_fft(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	int32_t n;
	websocket_user_session_t *user_session = (websocket_user_session_t *)user;

	websocket_vhost_session_t *vhost_session =
			(websocket_vhost_session_t *)
			lws_protocol_vh_priv_get(lws_get_vhost(wsi),
					lws_get_protocol(wsi));

	switch (reason)
	{
		case LWS_CALLBACK_PROTOCOL_INIT:
			vhost_session = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
					lws_get_protocol(wsi),
					sizeof(websocket_vhost_session_t));
			vhost_session->context = lws_get_context(wsi);
			vhost_session->protocol = lws_get_protocol(wsi);
			vhost_session->vhost = lws_get_vhost(wsi);
			break;

		case LWS_CALLBACK_ESTABLISHED:
			/* add ourselves to the list of live pss held in the vhd */
			lws_ll_fwd_insert(
				user_session,
				websocket_user_session_list,
				vhost_session->websocket_user_session_list
			);
			user_session->wsi = wsi;
			//user_session->last = vhost_session->current;
			break;

		case LWS_CALLBACK_CLOSED:
			/* remove our closing pss from the list of live pss */
			lws_ll_fwd_remove(
				websocket_user_session_t,
				websocket_user_session_list,
				user_session,
				vhost_session->websocket_user_session_list
			);
			break;


		case LWS_CALLBACK_SERVER_WRITEABLE:
			/* Write output data, if data exists */
			pthread_mutex_lock(&websocket_output.mutex);
			if(websocket_output.length != 0 && user_session->last_sequence_id != websocket_output.sequence_id)
			{
				n = lws_write(wsi, (unsigned char*)&websocket_output.buffer[LWS_PRE], websocket_output.length, LWS_WRITE_BINARY);
				if (!n)
				{
					pthread_mutex_unlock(&websocket_output.mutex);
					lwsl_err("ERROR %d writing to socket\n", n);
					return -1;
				}
				user_session->last_sequence_id = websocket_output.sequence_id;
			}
			pthread_mutex_unlock(&websocket_output.mutex);
			
			break;

		case LWS_CALLBACK_RECEIVE:
			if (len < 6)
				break;
			if (strcmp((const char *)in, "closeme\n") == 0)
			{
				lws_close_reason(wsi, LWS_CLOSE_STATUS_GOINGAWAY,
						 (unsigned char *)"seeya", 5);
				return -1;
			}
			break;
		
		default:
			break;
	}

	return 0;
}

enum demo_protocols {
	PROTOCOL_FFT,
	NOP
};

/* list of supported protocols and callbacks */
static struct lws_protocols protocols[] = {
	{
		.name = "fft",
		.callback = callback_fft,
		.per_session_data_size = 128,
		.rx_buffer_size = 4096,
	},
	{
		/* terminator */
		0
	}
};

void sighandler(int sig)
{
	(void) sig;
	force_exit = 1;
	lws_cancel_service(context);
}

int main(int argc, char **argv)
{
	(void) argc;
	(void) argv;
	struct lws_context_creation_info info;
	struct timeval tv;
	unsigned int ms, oldms = 0;
	int n = 0, i;

	signal(SIGINT, sighandler);

	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsts", LOG_PID | LOG_PERROR, LOG_DAEMON);

	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);

	memset(&info, 0, sizeof info);
    info.port = WS_PORT;
	info.iface = NULL;
	info.protocols = protocols;
	info.gid = -1;
	info.uid = -1;
	info.max_http_header_pool = 16;
	info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;
	info.timeout_secs = 5;

    fprintf(stdout, "Initialising Websocket Server (LWS %d) on port %d.. ",LWS_LIBRARY_VERSION_NUMBER,info.port);
    fflush(stdout);
	context = lws_create_context(&info);
	if (context == NULL)
	{
		lwsl_err("LWS init failed\n");
		return -1;
	}
	
	fprintf(stdout, "Initialising AirSpy (%.01fMSPS, %.03fMHz).. ",(float)sample_rate_val/1000000,(float)freq_hz/1000000);
	fflush(stdout);
	if(!setup_airspy())
	{
	    fprintf(stderr, "AirSpy init failed.\n");
		return -1;
	}
	fprintf(stdout, "Done.\n");
	
	fprintf(stdout, "Initialising FFT (%d bin).. ", FFT_SIZE);
	fflush(stdout);
	setup_fft();
	for(i=0; i<FFT_SIZE; i++)
	{
		hanning_window_const[i] = 0.5 * (1.0 - cos(2*M_PI*(((double)i)/FFT_SIZE)));
	}
	fprintf(stdout, "Done.\n");
	
	fprintf(stdout, "Starting FFT Thread.. ");
	if (pthread_create(&fftThread, NULL, thread_fft, NULL))
	{
		fprintf(stderr, "Error creating FFT thread\n");
		return -1;
	}
	pthread_setname_np(fftThread, "airspy_fft_ws: FFT Calculation Thread");
	fprintf(stdout, "Done.\n");

    fprintf(stdout, "Server running.\n");
    fflush(stdout);

	while (n >= 0 && !force_exit)
	{
		gettimeofday(&tv, NULL);

		ms = (tv.tv_sec * 1000) + (tv.tv_usec / 1000);
		if ((ms - oldms) > WS_INTERVAL)
		{
			/* Copy latest FFT data to WS Output Buffer */
			fft_to_string();

			/* Trigger send on all websockets */
			lws_callback_on_writable_all_protocol(context, &protocols[PROTOCOL_FFT]);

			/* Reset timer */
			oldms = ms;
		}
		
        /* Service websockets, else wait 50ms */
		n = lws_service(context, 10);
	}

    /* TODO: Catch SIG for graceful exit */
	lws_context_destroy(context);
	close_airspy();
	close_fftw();
	closelog();

	return 0;
}
