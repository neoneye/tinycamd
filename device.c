#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>

#include <linux/videodev2.h>
#include "tinycamd.h"

int videodev = -1;

struct buffer {
        void *                  start;
        size_t                  length;
};
static struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;

pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;

#define CLEAR(x) memset (&(x), 0, sizeof (x))

static void errno_exit(const char *s)
{
    fatal_f("%s error %d, %s\n", s, errno, strerror (errno));
}

static int xioctl(int fd, int request, void *arg)
{
    int r;
    
    do r = ioctl (fd, request, arg);
    while (-1 == r && EINTR == errno);
    
    return r;
}

static int read_frame(void)
{
    unsigned int i, len;
    
    switch (io_method) {
      case IO_METHOD_READ:
	if (-1 == (len = read (videodev, buffers[0].start, buffers[0].length))) {
	    switch (errno) {
	      case EAGAIN:
		return 0;
		
	      case EIO:
		/* Could ignore EIO, see spec. */
		/* fall through */
	      default:
		errno_exit ("read");
	    }
	}
	new_frame (buffers[0].start, len, 0);
	break;
      case IO_METHOD_MMAP:
	  {
	      struct v4l2_buffer buf = {
		  .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		  .memory = V4L2_MEMORY_MMAP,
	      };
	      if (-1 == xioctl (videodev, VIDIOC_DQBUF, &buf)) {
		  switch (errno) {
		    case EAGAIN:
		      return 0;
		    case EIO:
		      /* Could ignore EIO, see spec. */
		      /* fall through */
		    default:
		      errno_exit ("VIDIOC_DQBUF");
		  }
	      }
	      
	      assert (buf.index < n_buffers);
	      new_frame (buffers[buf.index].start, buf.bytesused, &buf);
	      if ( buf.type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		  if (-1 == xioctl (videodev, VIDIOC_QBUF, &buf)) errno_exit ("VIDIOC_QBUF");
	      }
	  }
	  break;
	  
      case IO_METHOD_USERPTR:
	  {
	      struct v4l2_buffer buf = {
		  .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		  .memory = V4L2_MEMORY_USERPTR,
	      };
	      
	      if (-1 == xioctl (videodev, VIDIOC_DQBUF, &buf)) {
		  switch (errno) {
		    case EAGAIN:
		      return 0;
		    case EIO:
		      /* Could ignore EIO, see spec. */
		      /* fall through */
		    default:
		      errno_exit ("VIDIOC_DQBUF");
		  }
	      }
	      
	      for (i = 0; i < n_buffers; ++i)
		  if (buf.m.userptr == (unsigned long) buffers[i].start
		      && buf.length == buffers[i].length)
		      break;
	      
	      assert (i < n_buffers);
	      new_frame ((void *) buf.m.userptr, buf.bytesused, &buf);
	      if ( buf.type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		  if (-1 == xioctl (videodev, VIDIOC_QBUF, &buf)) errno_exit ("VIDIOC_QBUF");
	      }
	  }
	  break;
    }
    return 1;
}

void *main_loop (void *args)
{
    for (;;) {
	fd_set fds;
	int r;

	FD_ZERO (&fds);
	FD_SET (videodev, &fds);

	r = select (videodev + 1, &fds, NULL, NULL, 0);

	if (-1 == r) {
	    if (EINTR == errno)	continue;
	    errno_exit ("select");
	}
	
	pthread_mutex_lock(&video_mutex);
	read_frame();
	pthread_mutex_unlock(&video_mutex);
    }
    return NULL;
}

void start_capturing (void)
{
    unsigned int i;
    enum v4l2_buf_type type;
    
    pthread_mutex_lock(&video_mutex);
    switch (io_method) {
      case IO_METHOD_READ:
	/* Nothing to do. */
	break;
	
      case IO_METHOD_MMAP:
	for (i = 0; i < n_buffers; ++i) {
	    struct v4l2_buffer buf = {
		.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory      = V4L2_MEMORY_MMAP,
		.index       = i,
	    };
	    
	    if (-1 == xioctl (videodev, VIDIOC_QBUF, &buf)) errno_exit ("VIDIOC_QBUF");
	}
	
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl (videodev, VIDIOC_STREAMON, &type)) errno_exit ("VIDIOC_STREAMON");
	
	break;
	
      case IO_METHOD_USERPTR:
	for (i = 0; i < n_buffers; ++i) {
	    struct v4l2_buffer buf = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_USERPTR,
		.index = i,
		.m.userptr = (unsigned long) buffers[i].start,
		.length = buffers[i].length,
	    };
	    
	    if (-1 == xioctl (videodev, VIDIOC_QBUF, &buf)) errno_exit ("VIDIOC_QBUF");
	}
	
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl (videodev, VIDIOC_STREAMON, &type)) errno_exit ("VIDIOC_STREAMON");
	
	break;
    }
    pthread_mutex_unlock(&video_mutex);
}

void stop_capturing (void)
{
    enum v4l2_buf_type type;
    
    pthread_mutex_lock(&video_mutex);
    switch (io_method) {
      case IO_METHOD_READ:
	/* Nothing to do. */
	break;
      case IO_METHOD_MMAP:
      case IO_METHOD_USERPTR:
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl (videodev, VIDIOC_STREAMOFF, &type)) errno_exit ("VIDIOC_STREAMOFF");
	break;
    }
    pthread_mutex_unlock(&video_mutex);
}


void init_read (unsigned int buffer_size)
{
    buffers = calloc (1, sizeof (*buffers));
    
    if (!buffers) fatal_f("Out of memory\n");
    
    buffers[0].length = buffer_size;
    buffers[0].start = malloc (buffer_size);
    
    if (!buffers[0].start) fatal_f("Out of memory\n");
}

void init_mmap (void)
{
    struct v4l2_requestbuffers req = { 
	.count = 4,
	.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	.memory = V4L2_MEMORY_MMAP,
    };

    if (-1 == xioctl (videodev, VIDIOC_REQBUFS, &req)) {
	if (EINVAL == errno) {
	  fatal_f( "%s does not support memory mapping\n",videodev_name);
	} else {
	    errno_exit ("VIDIOC_REQBUFS");
	}
    }
    
    if (req.count < 2) {
      fatal_f("Insufficient buffer memory on %s\n",videodev_name);
    }
    
    buffers = calloc (req.count, sizeof (*buffers));
    
    if (!buffers) {
      fatal_f("Out of memory\n");
    }
    
    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
	struct v4l2_buffer buf = {
	    .type        = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	    .memory      = V4L2_MEMORY_MMAP,
	    .index       = n_buffers,
	};

	if (-1 == xioctl (videodev, VIDIOC_QUERYBUF, &buf)) errno_exit ("VIDIOC_QUERYBUF");
	
	buffers[n_buffers].length = buf.length;
	buffers[n_buffers].start =
	    mmap (NULL /* start anywhere */,
		  buf.length,
		  PROT_READ | PROT_WRITE /* required */,
		  MAP_SHARED /* recommended */,
		  videodev, buf.m.offset);
	
	if (MAP_FAILED == buffers[n_buffers].start) errno_exit ("mmap");
    }
}

void init_userp	(unsigned int buffer_size)
{
    struct v4l2_requestbuffers req = {0};
    
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    
    if (-1 == xioctl (videodev, VIDIOC_REQBUFS, &req)) {
	if (EINVAL == errno) {
	  fatal_f("%s does not support user pointer i/o\n",videodev_name);
	} else {
	    errno_exit ("VIDIOC_REQBUFS");
	}
    }
    
    buffers = calloc (4, sizeof (*buffers));
    
    if (!buffers) {
      fatal_f("Out of memory\n");
    }
    
    for (n_buffers = 0; n_buffers < 4; ++n_buffers) {
	buffers[n_buffers].length = buffer_size;
	buffers[n_buffers].start = malloc (buffer_size);
	
	if (!buffers[n_buffers].start) {
	  fatal_f( "Out of memory\n");
	}
    }
}


void init_device (void)
{
    unsigned int min;

    unsigned int pixelformat;

    switch(camera_method) {
    case CAMERA_METHOD_MJPEG:
      pixelformat = V4L2_PIX_FMT_MJPEG;
      break;
    case CAMERA_METHOD_JPEG:
      pixelformat = V4L2_PIX_FMT_JPEG;
      break;
    case CAMERA_METHOD_YUYV:
      pixelformat = V4L2_PIX_FMT_YUYV;
      break;
    default:
      fatal_f("Unsupported camera method.\n");
    }


    pthread_mutex_lock(&video_mutex);
    /*
    ** Is it a video device?
    */
    {
	struct v4l2_capability cap;

	if (-1 == xioctl (videodev, VIDIOC_QUERYCAP, &cap)) {
	    if (EINVAL == errno) {
	      fatal_f("%s is no V4L2 device\n", videodev_name);
	    } else {
		errno_exit ("VIDIOC_QUERYCAP");
	    }
	}

	/*
	** Can it capture?
	*/
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
	  fatal_f("%s is no video capture device\n", videodev_name);
	}


	/*
	** Like we want it to?
	*/
	switch (io_method) {
	  case IO_METHOD_READ:
	    if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
	      fatal_f( "%s does not support read i/o\n", videodev_name);
	    }
	    break;
	  case IO_METHOD_MMAP:
	  case IO_METHOD_USERPTR:
	    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
	      fatal_f("%s does not support streaming i/o\n", videodev_name);
	    }
	    break;
	}
    }

        /* Select video input, video standard and tune here. */

    add_logitech_controls(videodev);

    /*
    ** Clear the crop
    */
    {
	struct v4l2_cropcap cropcap = {
	    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, 
	};

	if (0 == xioctl (videodev, VIDIOC_CROPCAP, &cropcap)) {
	    struct v4l2_crop crop = {
		.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.c = cropcap.defrect, /* reset to default */
	    };
	    
	    if (-1 == xioctl (videodev, VIDIOC_S_CROP, &crop)) {
		switch (errno) {
		  case EINVAL:
		    /* Cropping not supported. */
		    break;
		  default:
		    /* Errors ignored. */
		    break;
		}
	    }
	} else {	
	    /* Errors ignored. */
	}
    }


    /*
    ** The the format
    */
    {
	struct v4l2_format fmt = {
	    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	    .fmt.pix.width = video_width,
	    .fmt.pix.height = video_height,
	    // .fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV,
	    .fmt.pix.pixelformat = pixelformat,
	    .fmt.pix.field = V4L2_FIELD_INTERLACED,
	};
	struct v4l2_jpegcompression comp = {
	    .quality = quality,
	};
	struct v4l2_streamparm strm = {
	    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
	};

	if ( verbose) {
	  fprintf(stderr,"formating %dx%d pf=%c%c%c%c\n", fmt.fmt.pix.width, fmt.fmt.pix.height,
		    fmt.fmt.pix.pixelformat & 0xff,
		    (fmt.fmt.pix.pixelformat >> 8) & 0xff,
		    (fmt.fmt.pix.pixelformat >> 16) & 0xff,
		    (fmt.fmt.pix.pixelformat >> 24) & 0xff);
	}
	if (-1 == xioctl (videodev, VIDIOC_S_FMT, &fmt)) errno_exit ("VIDIOC_S_FMT");
	if (-1 == xioctl (videodev, VIDIOC_G_FMT, &fmt)) errno_exit("VIDIOC_G_FMT");
	if ( verbose) {
	    fprintf(stderr,"got format %dx%d pf=%c%c%c%c\n", fmt.fmt.pix.width, fmt.fmt.pix.height, 
		    fmt.fmt.pix.pixelformat & 0xff,
		    (fmt.fmt.pix.pixelformat >> 8) & 0xff,
		    (fmt.fmt.pix.pixelformat >> 16) & 0xff,
		    (fmt.fmt.pix.pixelformat >> 24) & 0xff);
	}
	if ( fmt.fmt.pix.pixelformat != pixelformat) {
	  fatal_f("Unable to set requested pixelformat.\n");
	}

	if (-1 == xioctl( videodev, VIDIOC_G_JPEGCOMP, &comp)) {
	    if ( errno != EINVAL) errno_exit("VIDIOC_G_JPEGCOMP");
	    log_f("driver does not support VIDIOC_G_JPEGCOMP\n");
	    comp.quality = quality;
	} else {
	    comp.quality = quality;
	    if (-1 == xioctl( videodev, VIDIOC_S_JPEGCOMP, &comp)) errno_exit("VIDIOC_S_JPEGCOMP");
	    if (-1 == xioctl( videodev, VIDIOC_G_JPEGCOMP, &comp)) errno_exit("VIDIOC_G_JPEGCOMP");
	    log_f("jpegcomp quality came out at %d\n", comp.quality);
	}

	if (-1 == xioctl( videodev, VIDIOC_G_PARM, &strm)) errno_exit("VIDIOC_G_PARM");
	strm.parm.capture.timeperframe.numerator = 1;
	if ( strm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
	    log_f("fps=%d\n", fps);
	    strm.parm.capture.timeperframe.denominator = fps;
	    if (-1 == xioctl( videodev, VIDIOC_S_PARM, &strm)) {
		log_f("failed to set fps: %s\n", strerror(errno));
	    } else {
		log_f("fps came out %d/%d\n", 
		      strm.parm.capture.timeperframe.numerator,
		      strm.parm.capture.timeperframe.denominator);
	    }
	}
	/* Note VIDIOC_S_FMT may change width and height. */
	
	/* Buggy driver paranoia. */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
	    fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
	    fmt.fmt.pix.sizeimage = min;
	
	switch (io_method) {
	  case IO_METHOD_READ:
	    init_read (fmt.fmt.pix.sizeimage);
	    break;
	  case IO_METHOD_MMAP:
	    init_mmap ();
	    break;
	  case IO_METHOD_USERPTR:
	    init_userp (fmt.fmt.pix.sizeimage);
	    break;
	}
    }
    pthread_mutex_unlock(&video_mutex);
}

void close_device (void)
{
    pthread_mutex_lock(&video_mutex);
    if (-1 == close (videodev)) errno_exit("close");
    videodev = -1;
    pthread_mutex_unlock(&video_mutex);
}

void probe_device(void)
{
    do_probe(videodev);
}

void open_device ( void)
{
    struct stat st; 
    
    if (-1 == stat (videodev_name, &st)) {
      fatal_f( "Cannot identify '%s': %d, %s\n",
	      videodev_name, errno, strerror (errno));
    }
    
    if (!S_ISCHR (st.st_mode)) {
      fatal_f( "%s is no device\n", videodev_name);
    }
    
    pthread_mutex_lock(&video_mutex);
    videodev = open (videodev_name, O_RDWR /* required */, 0);
    pthread_mutex_unlock(&video_mutex);
    
    if (-1 == videodev) {
      fatal_f( "Cannot open '%s': %d, %s\n",
		 videodev_name, errno, strerror (errno));
    }
}


int with_device( video_action func, char *buf, int size, int cid, int val)
{
    int r;

    pthread_mutex_lock(&video_mutex);
    r = (*func)(videodev, buf, size, cid, val);
    pthread_mutex_unlock(&video_mutex);
    return r;
}
