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

struct idletimer
{
	int lastdl;
	time_t idlestart;
};

void read_auth_keys(const char *filename, int bufsize,
		char *ckey, char *csecret, char *atok, char *atoksecret);

void config_curlopts(CURL *curl, const char *url, FILE *outfile, void *prog_data);

void reconnect_wait(char bool_httperror);

int progress_callback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow);

int main(int argc, const char *argv[])
{
	FILE *out;
	if(argc == 3)
	{
		out = fopen(argv[2], "w");
	}
	else if(argc == 2)
	{
		out = stdout;
	}
	else
	{
		printf("usage: %s keyfile [outfile]\n", argv[0]);
		return 0;
	}

	// These may be found on your twitter dev page, under "Applications"
	// You will need to create a new app if you haven't already
	// The four keys should be on separate lines in this order:
	int bufsize = 64;
	char *ckey = (char *)malloc(bufsize * sizeof(char));
	char *csecret = (char *)malloc(bufsize * sizeof(char));
	char *atok = (char *)malloc(bufsize * sizeof(char));
	char *atoksecret = (char *)malloc(bufsize * sizeof(char));
	read_auth_keys(argv[1], bufsize, ckey, csecret, atok, atoksecret);

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
	
	const char *url = "https://stream.twitter.com/1/statuses/sample.json";

	// Sign the URL with OAuth
	char *signedurl = oauth_sign_url2(url, NULL, OA_HMAC, "GET", ckey, csecret, atok, atoksecret);

	curl_global_init(CURL_GLOBAL_ALL);
	CURL *curl = curl_easy_init();

	struct idletimer timeout;
	timeout.lastdl = 0;
	timeout.idlestart = 0;

	config_curlopts(curl, signedurl, out, (void *)&timeout);

	int curlstatus, httpstatus;
	char reconnect = 1;
	while(reconnect && (curlstatus = curl_easy_perform(curl)))
	{
		switch(curlstatus)
		{
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
					case 503:
						// Will attempt reconnect
						fprintf(stderr, "Received HTTP error %d, attempting reconnect...\n", httpstatus);
						reconnect_wait(1);
						config_curlopts(curl, signedurl, out, (void *)&timeout);
						break;
					default:
						fprintf(stderr, "Unexpected HTTP error %d. Aborting...\n", httpstatus);
						reconnect = 0;
						break;
				}
				break;
			case CURLE_ABORTED_BY_CALLBACK:
				fprintf(stderr, "Timeout, attempting reconnect...\n");
				reconnect_wait(1);
				config_curlopts(curl, signedurl, out, (void *)&timeout);
				break;
			default:
				// Probably a socket error, do a full cleanup and reconnect
				fprintf(stderr, "Unexpected error, attempting reconnect...\n");
				reconnect_wait(0);
				curl_easy_cleanup(curl);
				curl = curl_easy_init();
				config_curlopts(curl, signedurl, out, (void *)&timeout);
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
 * outfile: file stream for retrieved data
 * prog_data: data to send to progress callback function
 */
void config_curlopts(CURL *curl, const char *url, FILE *outfile, void *prog_data)
{
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "tweetslurp/0.1");

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
 * bool_httperror: whether this was an http error or
 * 		not (i.e. it was a socket error)
 */
void reconnect_wait(char bool_httperror)
{
	static int http_sleep_s = 5;
	static long sock_sleep_ms = 250;

	if(bool_httperror)
	{
		struct timespec t;
		t.tv_sec = http_sleep_s;
		t.tv_nsec = 0;
		nanosleep(&t, NULL);

		// As per the streaming endpoint guidelines, double the
		// delay until 320 seconds is reached
		http_sleep_s *= 2;
		if(http_sleep_s > 320)
		{
			http_sleep_s = 320;
		}
	}
	else
	{
		struct timespec t;
		t.tv_sec = 0;
		t.tv_nsec = sock_sleep_ms * MS_TO_NS;
		nanosleep(&t, NULL);

		// As per the streaming endpoint guidelines, add 250ms
		// for each successive attempt until 16 seconds is reached
		sock_sleep_ms += 250;
		if(sock_sleep_ms > 16000)
		{
			sock_sleep_ms = 16000;
		}
	}
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
			timeout->lastdl = 0;
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

