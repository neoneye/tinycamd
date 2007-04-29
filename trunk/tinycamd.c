/*
 * threaded.c -- A simple multi-threaded FastCGI application.
 */

#include <fcgi_config.h>

#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcgiapp.h>
#include <string.h>
#include <sys/wait.h>
#include <stdio.h>

#include "tinycamd.h"

#define MAXFRAME 60

#define THREAD_COUNT 20

static pthread_mutex_t counts_mutex = PTHREAD_MUTEX_INITIALIZER;
static int counts[THREAD_COUNT];
static int sock;

static void do_status_request( FCGX_Request *req, int thread_id)
{
    char *server_name;
    pid_t pid = getpid();
    int i;

    server_name = FCGX_GetParam("SERVER_NAME", req->envp);
    
    FCGX_FPrintF(req->out,
		 "Content-type: text/html\r\n"
		 "\r\n"
		 "<title>FastCGI Hello! (multi-threaded C, fcgiapp library)</title>"
		 "<h1>FastCGI Hello! (multi-threaded C, fcgiapp library)</h1>"
		 "Thread %d, Process %ld<p>"
		 "Request counts for %d threads running on host <i>%s</i><p><code>",
		 thread_id, pid, THREAD_COUNT, server_name ? server_name : "?");
    
    pthread_mutex_lock(&counts_mutex);
    ++counts[thread_id];
    for (i = 0; i < THREAD_COUNT; i++)
	FCGX_FPrintF(req->out, "%5d " , counts[i]);
    pthread_mutex_unlock(&counts_mutex);
}

static void put_image(const struct chunk *c, void *arg)
{
    int i,s=0;
    FCGX_Request *req = (FCGX_Request *)arg;

    for ( i = 0; c[i].data != 0; i++) s += c[i].length;

    FCGX_FPrintF(req->out, 
		 "Content-type: image/jpeg\r\n"
		 "Content-length: %d\r\n"
		 "\r\n", s);
    for ( i = 0; c[i].data != 0; i++) {
	FCGX_PutStr( c[i].data, c[i].length, req->out);
    }
    fprintf(stderr,"image size = %d\n",s);
}

static void stream_image( FCGX_Request *req)
{
    int i;

    FCGX_FPrintF(req->out, 
		 "Cache-Control: no-cache\r\n"
		 "Pragma: no-cache\r\n"
		 "Expires: Thu, 01 Dec 1994 16:00:00 GMT\r\n"
		 "Connection: close\r\n"
		 "Content-Type: multipart/x-mixed-replace; boundary=--myboundary\r\n"
		 "\r\n");
    for(i = 0; i < MAXFRAME; i++) {
	FCGX_FPrintF(req->out, "--myboundary\r\n");
	with_next_frame( &put_image, req);
    }
}

static void do_video_call( FCGX_Request *req, video_action action, int cid, int val)
{
    char buf[8192];

    with_device( action, buf, sizeof(buf), cid, val);
    FCGX_FPrintF(req->out,
		 "Cache-Control: no-cache\r\n"
		 "Pragma: no-cache\r\n"
		 "Expires: Thu, 01 Dec 1994 16:00:00 GMT\r\n"
		 "Content-Type: text/xml\r\n"
		 "Content-Length: %d\r\n"
		 "\r\n%s", strlen(buf), buf);
}

static void *handle_requests(void *a)
{
    int rc, thread_id = (int)a;
    FCGX_Request request;
    char *path_info;
    char *query;

    FCGX_InitRequest(&request, sock, 0);

    for (;;)
    {
        static pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;
	int cid, val;

        /* Some platforms require accept() serialization, some don't.. */
        pthread_mutex_lock(&accept_mutex);
        rc = FCGX_Accept_r(&request);
        pthread_mutex_unlock(&accept_mutex);

        if (rc < 0)
            break;

	path_info = FCGX_GetParam("PATH_INFO", request.envp);
	query = FCGX_GetParam("QUERY_STRING", request.envp);

	if ( verbose) fprintf(stderr,"Request: %s\n", path_info);
	if ( strcmp(path_info,"/status")==0) {
	    do_status_request(&request, thread_id);
	} else if ( strcmp(path_info,"/image.replace")==0) {
	    stream_image(&request);
	} else if ( strcmp(path_info,"/controls")==0) {
	    do_video_call( &request, list_controls,0,0);
	} else if ( strcmp(path_info,"/set")==0 ) {
	    if ( query && sscanf( query, "%d=%d", &cid, &val) == 2) {
		do_video_call( &request, set_control,cid,val);
	    } else {
		FCGX_FPrintF(request.out, "Content-type: text/plain\r\n\r\nBad query string: %s", query);
		// some sort of error
	    }
	} else {
	    with_current_frame( &put_image, &request);
	}

        FCGX_Finish_r(&request);
    }

    return NULL;
}

int main(int argc, char **argv)
{
    int i;
    pthread_t id[THREAD_COUNT];
    pthread_t captureThread;

    do_options(argc, argv);

    open_device();
    init_device();
    start_capturing();

    pthread_create( &captureThread, NULL, main_loop, NULL);

    FCGX_Init();
    sock = FCGX_OpenSocket(":3636",20);

    for (i = 1; i < THREAD_COUNT; i++)
        pthread_create(&id[i], NULL, handle_requests, (void*)i );

    for(;;) sleep(100);

    close_device();

    return 0;
}
