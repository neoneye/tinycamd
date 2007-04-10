/*
** we need this for the read/write pthread locks
*/
#define _XOPEN_SOURCE 600

#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/time.h>
#include <linux/videodev2.h>

#include "tinycamd.h"

struct frame {
    pthread_rwlock_t lock; // following 4 fields guarded by lock
    void *data;
    unsigned int length;
    unsigned int hufftabInsert;
    struct v4l2_buffer buffer;

    pthread_cond_t cond;
    pthread_mutex_t mutex;
    int serial;            // this is guarded by mutex, not lock.

};

static struct frame currentFrame = {
    .lock = PTHREAD_RWLOCK_INITIALIZER,

    // this cond and associated mutex is used to wait for the next frame
    .cond = PTHREAD_COND_INITIALIZER,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

/*
** MPJEG files are typically, though not always, missing their DHT. If they are
** missing then this is almost certainly what they need. I'd feel a lot better
** if there was a standard written down for this.
*/
static const unsigned char fixed_dht[] = {
  0xff,0xc4,0x01,0xa2,0x00,0x00,0x01,0x05,0x01,0x01,0x01,0x01,
  0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,
  0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x01,0x00,0x03,
  0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x00,0x00,0x00,
  0x00,0x00,0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,
  0x0a,0x0b,0x10,0x00,0x02,0x01,0x03,0x03,0x02,0x04,0x03,0x05,
  0x05,0x04,0x04,0x00,0x00,0x01,0x7d,0x01,0x02,0x03,0x00,0x04,
  0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,
  0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,
  0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,
  0x18,0x19,0x1a,0x25,0x26,0x27,0x28,0x29,0x2a,0x34,0x35,0x36,
  0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,
  0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,
  0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
  0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,0x95,
  0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,
  0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,
  0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,
  0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
  0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,
  0xfa,0x11,0x00,0x02,0x01,0x02,0x04,0x04,0x03,0x04,0x07,0x05,
  0x04,0x04,0x00,0x01,0x02,0x77,0x00,0x01,0x02,0x03,0x11,0x04,
  0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,
  0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,
  0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,
  0xf1,0x17,0x18,0x19,0x1a,0x26,0x27,0x28,0x29,0x2a,0x35,0x36,
  0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,
  0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,
  0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
  0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8a,0x92,0x93,0x94,
  0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
  0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,
  0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,
  0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
  0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa
};

static unsigned int find_hufftab_location(const unsigned char *p, unsigned int len)
{
    unsigned int i;

    for ( i = 0; i < len-1; i++) {
	// Has its own DHT table
	if ( p[i] == 0xff && p[i+1]== 0xc4) return 0;

	// Needs a DHT here
	if ( p[i] == 0xff && p[i+1] == 0xda) return i;
    }
    return 0;  /* reall oughtn't get here */
}


/*
** Buf is the new buffer on the way in, but is set to the old buffer on the way out.
** If there is no old buffer then the .type field will be zero.
*/
void new_frame( void *data, unsigned int length, struct v4l2_buffer *buf)
{
    struct v4l2_buffer obuf;
    int rc;

    if ( pthread_rwlock_wrlock( &currentFrame.lock)) {
	fprintf(stderr,"Failed to acquire current frame write lock: %s\n", strerror(errno));
	exit(1);
    }
    if ( verbose) fprintf(stderr,"write locked frame\n");

    obuf = currentFrame.buffer;
    currentFrame.data = data;
    currentFrame.length = length;
    currentFrame.hufftabInsert = find_hufftab_location( currentFrame.data, currentFrame.length);
    if ( buf) {
	currentFrame.buffer = *buf;
    } else currentFrame.buffer.type = 0;
    
    *buf = obuf;
    if ( verbose) fprintf(stderr,"new_frame %08x %d\n", (unsigned int)data, length);

    if ( pthread_rwlock_unlock( &currentFrame.lock)) {
	fprintf(stderr,"Failed to release current frame write lock: %s\n", strerror(errno));
	exit(1);
    }
    if ( verbose) fprintf(stderr,"write unlocked frame\n");

    // Notify folk that the frame has changed
    rc = pthread_mutex_lock(&currentFrame.mutex);
    currentFrame.serial++;
    rc = pthread_cond_broadcast(&currentFrame.cond);
    rc = pthread_mutex_unlock(&currentFrame.mutex);

    return;
}


void with_current_frame( frame_sender func, void *arg)
{
    struct chunk c[4];

    if ( pthread_rwlock_rdlock( &currentFrame.lock)) {
	fprintf(stderr,"Failed to acquire current frame read lock: %s\n", strerror(errno));
	exit(1);
    }
    if ( verbose) fprintf(stderr,"read locked frame\n");

    if ( currentFrame.hufftabInsert == 0) {
	c[0].data = currentFrame.data;
	c[0].length = currentFrame.length;
	c[1].data = 0;
    } else {
	c[0].data = currentFrame.data;
	c[0].length = currentFrame.hufftabInsert;
	c[1].data = fixed_dht;
	c[1].length = sizeof(fixed_dht);
	c[2].data = currentFrame.data + currentFrame.hufftabInsert;
	c[2].length = currentFrame.length - currentFrame.hufftabInsert;
	c[3].data = 0;
    }
    (*func)(c,arg);

    if ( pthread_rwlock_unlock( &currentFrame.lock)) {
	fprintf(stderr,"Failed to release current frame read lock: %s\n", strerror(errno));
	exit(1);
    }
    if ( verbose) fprintf(stderr,"read unlocked frame\n");
}

void with_next_frame( frame_sender func, void *arg)
{
    int s;

    if ( verbose) fprintf(stderr,"with_next_frame\n");
    pthread_mutex_lock( &currentFrame.mutex);
    s = currentFrame.serial;
    while( currentFrame.serial == s) {
	pthread_cond_wait( &currentFrame.cond, &currentFrame.mutex);
    }
    pthread_mutex_unlock( &currentFrame.mutex);
    with_current_frame( func, arg);
}

