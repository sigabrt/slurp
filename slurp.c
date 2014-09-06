//
//  slurp.c
//
//  Public domain, do what you want with it,
//  though I'd certainly be interested to hear
//  what you're using it for. :)
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <oauth.h>
#include <curl/curl.h>

#define MS_TO_NS (1000000)

#define DATA_TIMEOUT (90)
#define MAX_KEYWORDS (16)

static const char *SAMPLE_URL =
	"https://stream.twitter.com/1.1/statuses/sample.json";

static const char *FILTER_URL =
	"https://stream.twitter.com/1.1/statuses/filter.json";

typedef enum
{
	ERROR_TYPE_HTTP,
	ERROR_TYPE_RATE_LIMITED,
	ERROR_TYPE_SOCKET,
} error_type;

struct idletimer
{
	int lastdl;
	time_t idlestart;
};

void read_auth_keys(const char *filename, int bufsize,
		char *ckey, char *csecret, char *atok, char *atoksecret);

void config_curlopts(CURL *curl, const char *url, char *postargs, FILE *outfile, void *prog_data);

void reconnect_wait(error_type error);

int progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow);

int main(int argc, char *argv[])
{
	// Output file stream
	FILE *out = stdout;

	// File containing the OAuth keys
	const char *keyfile = NULL;

	// OAuth keys
	char *ckey, *csecret, *atok, *atoksecret;

	// Streaming endpoint (no params, just base URL)
	const char *endpoint = SAMPLE_URL;

	// Full unsigned URL with parameters
	char *url = NULL;

	// Signed URL
	char *signedurl = NULL;

	// POST parameters
	char *postargs = NULL;

	// Search keywords for the filter endpoint
	const char *keywords[MAX_KEYWORDS];

	// Process command line arguments
	// ==============================
	unsigned  int keyword_count = 0;
	int opt;
	while((opt = getopt(argc, argv, "sfk:")) != -1)
	{
		switch(opt)
		{
			case 'k':
				endpoint = FILTER_URL;
				if(keyword_count >= MAX_KEYWORDS)
				{
					break;
				}
				else if(strlen(optarg) > 60)
				{
					fprintf(stderr, "Keyword too long (omitting): %s\n", optarg);
					break;
				}
				keywords[keyword_count] = optarg;
				keyword_count++;
				break;
			default:
				break;
		}
	}
	if(optind+1 < argc)
	{
		keyfile = argv[optind];
		out = fopen(argv[optind+1], "w");
	}
	else if(optind < argc)
	{
		keyfile = argv[optind];
	}
	else
	{
		fprintf(stderr, "usage: %s [-k 'search phrase'] keyfile [outfile]\n", argv[0]);
		return 1;
	}

	// Generate full URL
	// =================
	if(endpoint == FILTER_URL)
	{
		url = (char *)malloc(strlen(endpoint) + (keyword_count * 61) + strlen("?track=") + 1);
		strcpy(url, FILTER_URL);
		strcat(url, "?track=");
		int i;
		for(i = 0; i < keyword_count; i++)
		{
			strcat(url, keywords[i]);
			if(i+1 < keyword_count)
			{
				strcat(url, ",");
			}
		}
	}
	else if(endpoint == SAMPLE_URL)
	{
		url = (char *)malloc(strlen(endpoint)+1);
		strcpy(url, SAMPLE_URL);
	}

	// Read the OAuth keys
	// ===================
	// These may be found on your twitter dev page, under "Applications"
	// You will need to create a new app if you haven't already
	// The four keys should be on separate lines in this order:
	int bufsize = 64;
	ckey = (char *)malloc(bufsize * sizeof(char));
	csecret = (char *)malloc(bufsize * sizeof(char));
	atok = (char *)malloc(bufsize * sizeof(char));
	atoksecret = (char *)malloc(bufsize * sizeof(char));
	read_auth_keys(keyfile, bufsize, ckey, csecret, atok, atoksecret);
	if(ckey == NULL || csecret == NULL ||
			atok == NULL || atoksecret == NULL)
	{
		fprintf(stderr, "Couldn't read key file. Aborting...\n");
		free(ckey);
		free(csecret);
		free(atok);
		free(atoksecret);
		return 1;
	}
	
	// Sign the URL with OAuth
	// =======================
	// Note that all parameters will be stripped from the URL
	// and added to postargs automatically
	signedurl = oauth_sign_url2(url, &postargs, OA_HMAC, "POST", ckey, csecret, atok, atoksecret);

	// Initialize the idle timer
	// =========================
	struct idletimer timeout;
	timeout.lastdl = 1;
	timeout.idlestart = 0;

	// Initialize libcURL
	// ==================
	curl_global_init(CURL_GLOBAL_ALL);
	CURL *curl = curl_easy_init();
	config_curlopts(curl, signedurl, postargs, out, (void *)&timeout);

	int curlstatus, httpstatus;
	char reconnect = 1;
	while(reconnect)
	{
		curlstatus = curl_easy_perform(curl);
		switch(curlstatus)
		{
			case 0: // Twitter closed the connection
				fprintf(stderr, "Connection terminated. Attempting reconnect...\n");
				reconnect_wait(ERROR_TYPE_SOCKET);
				curl_easy_cleanup(curl);
				curl = curl_easy_init();

				// The signed URL contains a timestamp, so it needs to be
				// regenerated each time we reconnect or else we'll get a 401
				signedurl = oauth_sign_url2(url, &postargs, OA_HMAC, "POST", ckey, csecret, atok, atoksecret);
				config_curlopts(curl, signedurl, postargs, out, (void *)&timeout);
				break;
			case CURLE_HTTP_RETURNED_ERROR:
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpstatus);
				switch(httpstatus)
				{
					case 401:
					case 403:
					case 404:
					case 406:
					case 413:
					case 416:
						// No reconnects with these errors
						fprintf(stderr, "Request failed with HTTP error %d. Aborting...\n", httpstatus);
						reconnect = 0;
						break;
					case 420:
						fprintf(stderr, "Received rate limiting response, attempting reconnect...\n");
						reconnect_wait(ERROR_TYPE_RATE_LIMITED);

						signedurl = oauth_sign_url2(url, &postargs, OA_HMAC, "POST", ckey, csecret, atok, atoksecret);
						config_curlopts(curl, signedurl, postargs, out, (void *)&timeout);
						break;
					case 503:
						fprintf(stderr, "Received HTTP error %d, attempting reconnect...\n", httpstatus);
						reconnect_wait(ERROR_TYPE_HTTP);

						signedurl = oauth_sign_url2(url, &postargs, OA_HMAC, "POST", ckey, csecret, atok, atoksecret);
						config_curlopts(curl, signedurl, postargs, out, (void *)&timeout);
						break;
					default:
						fprintf(stderr, "Unexpected HTTP error %d. Aborting...\n", httpstatus);
						reconnect = 0;
						break;
				}
				break;
			case CURLE_ABORTED_BY_CALLBACK:
				fprintf(stderr, "Timeout, attempting reconnect...\n");
				reconnect_wait(ERROR_TYPE_SOCKET);

				signedurl = oauth_sign_url2(url, &postargs, OA_HMAC, "POST", ckey, csecret, atok, atoksecret);
				config_curlopts(curl, signedurl, postargs, out, (void *)&timeout);
				break;
			default:
				// Probably a socket error, attempt reconnnect
				fprintf(stderr, "Unexpected error, attempting reconnect...\n");
				reconnect_wait(ERROR_TYPE_SOCKET);

				curl_easy_cleanup(curl);
				curl = curl_easy_init();
				signedurl = oauth_sign_url2(url, &postargs, OA_HMAC, "POST", ckey, csecret, atok, atoksecret);
				config_curlopts(curl, signedurl, postargs, out, (void *)&timeout);
				break;
		}
	}

	printf("Cleaning up...\n");
	curl_easy_cleanup(curl);
	curl_global_cleanup();

	free(ckey);
	free(csecret);
	free(atok);
	free(atoksecret);
	free(signedurl);
	free(url);

	fclose(out);

	return 0;
}

/* read_auth_keys
 * filename: The name of the text file containing
 * 		the keys
 * bufsize: The maximum number of characters to read per line
 * ckey: Consumer key, must be allocated already
 * csecret: Consumer secret, must be allocated already
 * atok: App token, must be allocated already
 * atoksecret: App token secret, must be allocated already
 */
void read_auth_keys(const char *filename, int bufsize,
		char *ckey, char *csecret, char *atok, char *atoksecret)
{
	FILE *file = fopen(filename, "r");

	if(fgets(ckey, bufsize, file) == NULL)
	{
		return;
	}
	ckey[strlen(ckey)-1] = '\0'; // Remove the newline

	if(fgets(csecret, bufsize, file) == NULL)
	{
		return;
	}
	csecret[strlen(csecret)-1] = '\0';

	if(fgets(atok, bufsize, file) == NULL)
	{
		return;
	}
	atok[strlen(atok)-1] = '\0';

	if(fgets(atoksecret, bufsize, file) == NULL)
	{
		return;
	}
	atoksecret[strlen(atoksecret)-1] = '\0';

	fclose(file);
}

/* config_curlopts
 * curl: cURL easy handle
 * url: URL of streaming endpoint
 * postargs: POST parameters
 * outfile: file stream for retrieved data
 * prog_data: data to send to progress callback function
 */
void config_curlopts(CURL *curl, const char *url, char *postargs, FILE *outfile, void *prog_data)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "tweetslurp/0.1");

	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postargs);

	// libcurl will now fail on an HTTP error (>=400)
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);

	// If no data callback is specified, libcurl will
	// write data to stdout or the file in writedata
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)outfile);

	// noprogress must be set to 0 for a user-defined
	// progress method to be called
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

	curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
	curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void *)prog_data);
}

/* reconnect_wait
 * error: type of error we're recovering from
 */
void reconnect_wait(error_type error)
{
	static int http_sleep_s = 5;
	static int rate_limit_sleep_s = 60;
	static long sock_sleep_ms = 250;

	struct timespec t;
	switch(error)
	{
		case ERROR_TYPE_HTTP:
			t.tv_sec = http_sleep_s;
			t.tv_nsec = 0;

			// As per the streaming endpoint guidelines, double the
			// delay until 320 seconds is reached
			http_sleep_s *= 2;
			if(http_sleep_s > 320)
			{
				http_sleep_s = 320;
			}
			break;
		case ERROR_TYPE_RATE_LIMITED:
			t.tv_sec = rate_limit_sleep_s;
			t.tv_nsec = 0;

			// As per the streaming endpoint guidelines, double the
			// delay
			rate_limit_sleep_s *= 2;
			break;
		case ERROR_TYPE_SOCKET:
			t.tv_sec = sock_sleep_ms / 1000;
			t.tv_nsec = (sock_sleep_ms % 1000) * MS_TO_NS;

			// As per the streaming endpoint guidelines, add 250ms
			// for each successive attempt until 16 seconds is reached
			sock_sleep_ms += 250;
			if(sock_sleep_ms > 16000)
			{
				sock_sleep_ms = 16000;
			}
			break;
		default:
			t.tv_sec = 0;
			t.tv_nsec = 0;
			break;
	}
	nanosleep(&t, NULL);
}

/* progress_callback
 * see libcURL docs for method sig details
 */
int progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	struct idletimer *timeout;
	timeout = (struct idletimer *)clientp;

	if(dlnow == 0) // No data was transferred this time...
	{
		// ...but some was last time:
		if(timeout->lastdl != 0)
		{
			// so start the timer
			timeout->idlestart = time(NULL);
		}
		// ...and 1) the timer has been started, and
		// 2) we've hit the timeout:
		else if(timeout->idlestart != 0 &&
				(time(NULL) - timeout->idlestart) > DATA_TIMEOUT)
		{
			// so we reset the timer and return a non-zero
			// value to abort the transfer
			timeout->lastdl = 1;
			timeout->idlestart = 0;
			return 1;
		}
	}
	else // We transferred some data...
	{
		// ...but we didn't last time:
		if(timeout->lastdl == 0)
		{
			// so reset the timer
			timeout->idlestart = 0;
		}
	}

	timeout->lastdl = dlnow;
	return 0;
}

