#include "capture.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>


static void log_stderr(camera_log_t type, const char* msg, void* pointer) {
  (void) pointer;
  switch (type) {
  case CAMERA_ERROR:
    fprintf(stderr, "ERROR [%s] %d: %s\n", msg, errno, strerror(errno));
    return;
  case CAMERA_FAIL:
    fprintf(stderr, "FAIL [%s]\n", msg);
    return;
  case CAMERA_INFO:
    fprintf(stderr, "INFO [%s]\n", msg);
    return;
  }
}

static bool error(camera_t* camera, const char * msg)
{
  camera->context.log(CAMERA_ERROR, msg, camera->context.pointer);
  return false;
}
static bool failure(camera_t* camera, const char * msg)
{
  camera->context.log(CAMERA_FAIL, msg, camera->context.pointer);
  return false;
}

static int xioctl(int fd, unsigned long int request, void* arg)
{
  for (int i = 0; i < 100; i++) {
    int r = ioctl(fd, request, arg);
    if (r != -1 || errno != EINTR) return r;
  }
  return -1;
}



camera_t* camera_open(const char * device)
{
  int fd = open(device, O_RDWR | O_NONBLOCK, 0);
  if (fd == -1) return NULL;

  camera_t* camera = malloc(sizeof (camera_t));
  camera->fd = fd;
  camera->initialized = false;
  camera->width = 0;
  camera->height = 0;
  camera->buffer_count = 0;
  camera->buffers = NULL;
  camera->head.length = 0;
  camera->head.start = NULL;
  camera->context.pointer = NULL;
  camera->context.log = &log_stderr;
  return camera;
}

static void free_buffers(camera_t* camera, size_t count)
{
  for (size_t i = 0; i < count; i++) {
    munmap(camera->buffers[i].start, camera->buffers[i].length);
  }
  free(camera->buffers);
  camera->buffers = NULL;
  camera->buffer_count = 0;
}

static bool camera_init(camera_t* camera) {
  struct v4l2_capability cap;
  if (xioctl(camera->fd, VIDIOC_QUERYCAP, &cap) == -1)
    return error(camera, "VIDIOC_QUERYCAP");
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    return failure(camera, "no capture");
  if (!(cap.capabilities & V4L2_CAP_STREAMING))
    return failure(camera, "no streaming");

  struct v4l2_cropcap cropcap;
  memset(&cropcap, 0, sizeof cropcap);
  cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(camera->fd, VIDIOC_CROPCAP, &cropcap) == 0) {
    struct v4l2_crop crop;
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;
    if (xioctl(camera->fd, VIDIOC_S_CROP, &crop) == -1) {
      // cropping not supported
    }
  }
  camera->initialized = true;
  return true;
}

static bool camera_buffer_prepare(camera_t* camera)
{
  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof req);
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(camera->fd, VIDIOC_REQBUFS, &req) == -1)
    return error(camera, "VIDIOC_REQBUFS");
  camera->buffer_count = req.count;
  camera->buffers = calloc(req.count, sizeof (camera_buffer_t));

  size_t buf_max = 0;
  for (size_t i = 0; i < camera->buffer_count; i++) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (xioctl(camera->fd, VIDIOC_QUERYBUF, &buf) == -1) {
      free_buffers(camera, i);
      return error(camera, "VIDIOC_QUERYBUF");
    }
    if (buf.length > buf_max) buf_max = buf.length;
    camera->buffers[i].length = buf.length;
    camera->buffers[i].start =
      mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
           camera->fd, buf.m.offset);
    if (camera->buffers[i].start == MAP_FAILED) {
      free_buffers(camera, i);
      return error(camera, "mmap");
    }
  }
  camera->head.start = calloc(buf_max, sizeof (uint8_t));
  return true;
}

static void camera_buffer_finish(camera_t* camera)
{
  free_buffers(camera, camera->buffer_count);
  free(camera->head.start);
  camera->head.length = 0;
  camera->head.start = NULL;
}

static bool camera_load_settings(camera_t* camera)
{
  struct v4l2_format format;
  memset(&format, 0, sizeof format);
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(camera->fd, VIDIOC_G_FMT, &format) == -1)
    return error(camera, "VIDIOC_G_FMT");
  camera->width = format.fmt.pix.width;
  camera->height = format.fmt.pix.height;
  camera->format = format.fmt.pix.pixelformat;
  return true;
}

static bool camera_load(camera_t* camera)
{
  if (!camera->initialized) {
    if (!camera_init(camera)) return false;
  }
  if (camera->buffer_count == 0) {
    if (!camera_load_settings(camera)) return false;
    if (!camera_buffer_prepare(camera)) return false;
  }
  return true;
}

bool camera_stop(camera_t* camera)
{
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(camera->fd, VIDIOC_STREAMOFF, &type) == -1)
    return error(camera, "VIDIOC_STREAMOFF");
  camera_buffer_finish(camera);

  struct v4l2_requestbuffers req;
  memset(&req, 0, sizeof req);
  req.count = 0;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if (xioctl(camera->fd, VIDIOC_REQBUFS, &req) == -1)
    return error(camera, "VIDIOC_REQBUFS 0");
  return true;
}

bool camera_start(camera_t* camera)
{
  if (!camera_load(camera)) return false;

  for (size_t i = 0; i < camera->buffer_count; i++) {
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (xioctl(camera->fd, VIDIOC_QBUF, &buf) == -1)
      return error(camera, "VIDIOC_QBUF");
  }

  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(camera->fd, VIDIOC_STREAMON, &type) == -1)
    return error(camera, "VIDIOC_STREAMON");
  return true;
}


bool camera_close(camera_t* camera)
{
  if (camera->buffer_count > 0) {
    camera_stop(camera);
  }
  for (int i = 0; i < 10; i++) {
    if (close(camera->fd) != -1) break;
  }
  free(camera);
  return true;
}


//[[capturing]
bool camera_capture(camera_t* camera)
{
  struct v4l2_buffer buf;
  memset(&buf, 0, sizeof buf);
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  if (xioctl(camera->fd, VIDIOC_DQBUF, &buf) == -1) return false;
  if (camera->format != camera_format_id("MJPG") ||
      !verify_jpeg_dht(camera->buffers[buf.index].start, buf.bytesused, &camera->head.start, &camera->head.length)) {
    memcpy(camera->head.start, camera->buffers[buf.index].start, buf.bytesused);
    camera->head.length = buf.bytesused;
  }
  if (xioctl(camera->fd, VIDIOC_QBUF, &buf) == -1) return false;
  return true;
}


static inline int minmax(int min, int v, int max)
{
  return (v < min) ? min : (max < v) ? max : v;
}
static inline uint8_t yuv2r(int y, int u, int v)
{
  (void) u; return minmax(0, (y + 359 * v) >> 8, 255);
}
static inline uint8_t yuv2g(int y, int u, int v)
{
  return minmax(0, (y + 88 * v - 183 * u) >> 8, 255);
}
static inline uint8_t yuv2b(int y, int u, int v)
{
  (void) v; return minmax(0, (y + 454 * u) >> 8, 255);
}
uint8_t* yuyv2rgb(const uint8_t* yuyv, uint32_t width, uint32_t height)
{
  uint8_t* rgb = calloc(width * height * 3, sizeof (uint8_t));
  for (size_t i = 0; i < height; i++) {
    for (size_t j = 0; j < width; j += 2) {
      size_t index = i * width + j;
      size_t index2 = index * 2, index3 = index * 3;
      int y0 = yuyv[index2 + 0] << 8;
      int u = yuyv[index2 + 1] - 128;
      int y1 = yuyv[index2 + 2] << 8;
      int v = yuyv[index2 + 3] - 128;
      rgb[index3 + 0] = yuv2r(y0, u, v);
      rgb[index3 + 1] = yuv2g(y0, u, v);
      rgb[index3 + 2] = yuv2b(y0, u, v);
      rgb[index3 + 3] = yuv2r(y1, u, v);
      rgb[index3 + 4] = yuv2g(y1, u, v);
      rgb[index3 + 5] = yuv2b(y1, u, v);
    }
  }
  return rgb;
}

int verify_jpeg_dht(uint8_t *src,  uint32_t lsrc, uint8_t **dst, uint32_t *ldst)
{
  /* This function is based on a patch provided by Scott J. Bertin. */

  static unsigned char dht[] =
    {
      0xff, 0xc4, 0x01, 0xa2, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
      0x09, 0x0a, 0x0b, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02,
      0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d,
      0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31,
      0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32,
      0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
      0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
      0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a,
      0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
      0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57,
      0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
      0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
      0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
      0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
      0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
      0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
      0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
      0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
      0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
      0xf9, 0xfa, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
      0x0b, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04,
      0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01,
      0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
      0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14,
      0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
      0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25,
      0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a,
      0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46,
      0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
      0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a,
      0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83,
      0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
      0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
      0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
      0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
      0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8,
      0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
      0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa
  };
  uint8_t *p, *i = NULL;

  /* By default we simply return the source image. */
  uint8_t *odst = *dst;
  *dst = src;
  *ldst = lsrc;

  /* Scan for an existing DHT segment or the first SOS segment. */
  for(p = src + 2; ((uint8_t) (p - src)) < ((uint32_t) (lsrc - 3)) && i == NULL; )
  {
   if(*(p++) != 0xFF) continue;

   if(*p == 0xD9) break;           /* JPEG_EOI */
   if(*p == 0xC4) return(0);       /* JPEG_DHT */
   if(*p == 0xDA && !i) i = p - 1; /* JPEG_SOS */

   /* Move to next segment. */
   p += (p[1] << 8) + p[2];
  }

  /* If no SOS was found, insert the DHT directly after the SOI. */
  if(i == NULL) i = src + 2;

  *ldst = lsrc + sizeof(dht);
  *dst  = odst;

  /* Copy the JPEG data, inserting the DHT segment. */
  memcpy((p  = *dst), src, i - src);
  memcpy((p += i - src), dht, sizeof(dht));
  memcpy((p += sizeof(dht)), i, lsrc - (i - src));

  return(1);
}


//[formats and config]
uint32_t camera_format_id(const char* name)
{
  //assert(strlen(name) == 4);
  return (uint32_t) name[0] | ((uint32_t) name[1] << 8) |
    ((uint32_t) name[2] << 16) | ((uint32_t) name[3] << 24);
}
void camera_format_name(uint32_t format_id, char* name)
{
  name[0] = format_id & 0xff;
  name[1] = (format_id >> 8) & 0xff;
  name[2] = (format_id >> 16) & 0xff;
  name[3] = (format_id >> 24) & 0xff;
  name[4] = '\0';
}

static bool camera_format_set(camera_t* camera, const camera_format_t* format)
{
  if (format->width > 0 && format->height > 0) {
    uint32_t pixformat = format->format ? format->format : V4L2_PIX_FMT_YUYV;
    struct v4l2_format vformat;
    memset(&vformat, 0, sizeof vformat);
    vformat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vformat.fmt.pix.width = format->width;
    vformat.fmt.pix.height = format->height;
    vformat.fmt.pix.pixelformat = pixformat;
    vformat.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(camera->fd, VIDIOC_S_FMT, &vformat) == -1)
      return error(camera, "VIDIOC_S_FMT");
  }
  if (format->interval.numerator != 0 && format->interval.denominator != 0) {
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = format->interval.numerator;
    parm.parm.capture.timeperframe.denominator = format->interval.denominator;
    if (xioctl(camera->fd, VIDIOC_S_PARM, &parm) == -1)
      return error(camera, "VIDIOC_S_PARM");
  }
  return true;
}

static bool camera_format_get(camera_t* camera, camera_format_t* format)
{
  struct v4l2_format vformat;
  memset(&vformat, 0, sizeof vformat);
  vformat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(camera->fd, VIDIOC_G_FMT, &vformat) == -1)
    return error(camera, "VIDIOC_G_FMT");

  format->format = vformat.fmt.pix.pixelformat;
  format->width = vformat.fmt.pix.width;
  format->height = vformat.fmt.pix.height;

  struct v4l2_streamparm parm;
  memset(&parm, 0, sizeof parm);
  parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(camera->fd, VIDIOC_G_PARM, &parm) == -1)
    return error(camera, "VIDIOC_G_PARM");
  format->interval.numerator = parm.parm.capture.timeperframe.numerator;
  format->interval.denominator = parm.parm.capture.timeperframe.denominator;
  return true;
}

bool camera_config_get(camera_t* camera, camera_format_t* format)
{
  return camera_format_get(camera, format);
}
bool camera_config_set(camera_t* camera, const camera_format_t* format)
{
  if (camera->buffer_count > 0) {
    if (!camera_stop(camera)) return false;
  }
  if (!camera->initialized) {
    if (!camera_init(camera)) return false;
  }
  if (!camera_format_set(camera, format)) return false;
  if (!camera_load_settings(camera)) return false;
  return camera_buffer_prepare(camera);
}

camera_formats_t*  camera_formats_new(const camera_t* camera)
{
  camera_formats_t* ret = malloc(sizeof (camera_formats_t));
  ret->length = 0;
  ret->head = NULL;
  for (uint32_t i = 0; ; i++) {
    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.index = i;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_ENUM_FMT, &fmt) == -1) break;
    //printf("[%s]\n", fmt.description);
    for (uint32_t j = 0; ; j++) {
      struct v4l2_frmsizeenum frmsize;
      memset(&frmsize, 0, sizeof frmsize);
      frmsize.index = j;
      frmsize.pixel_format = fmt.pixelformat;
      if (ioctl(camera->fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == -1) break;
      if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        //printf("- w: %d, h: %d\n",
        //       frmsize.discrete.width, frmsize.discrete.height);
        for (uint32_t k = 0; ; k++) {
          struct v4l2_frmivalenum frmival;
          memset(&frmival, 0, sizeof frmival);
          frmival.index = k;
          frmival.pixel_format = fmt.pixelformat;
          frmival.width = frmsize.discrete.width;
          frmival.height = frmsize.discrete.height;
          if (ioctl(camera->fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == -1)
            break;
          if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            //printf("  - fps: %d/%d\n",
            //       frmival.discrete.denominator,frmival.discrete.numerator);
            ret->head =
              realloc(ret->head, (ret->length + 1) * sizeof (camera_format_t));
            ret->head[ret->length].format = fmt.pixelformat;
            ret->head[ret->length].width = frmsize.discrete.width;
            ret->head[ret->length].height = frmsize.discrete.height;
            ret->head[ret->length].interval.numerator =
              frmival.discrete.numerator;
            ret->head[ret->length].interval.denominator =
              frmival.discrete.denominator;
            ret->length++;
          } else {
            //printf("  - fps: %d/%d-%d/%d\n",
            //       frmival.stepwise.min.denominator,
            //       frmival.stepwise.min.numerator,
            //       frmival.stepwise.max.denominator,
            //       frmival.stepwise.max.numerator);
            // TBD: when stepwize
          }
        }
      } else {
        //printf("- w: %d-%d, h: %d-%d\n",
        //       frmsize.stepwise.min_width, frmsize.stepwise.max_width,
        //      frmsize.stepwise.min_height, frmsize.stepwise.max_height);
        // TBD: when stepwize
      }
    }

  }
  return ret;
}
void camera_formats_delete(camera_formats_t* formats)
{
  free(formats->head);
  free(formats);
}


//[controls]
static void
camera_menu_copy(camera_menu_t* menu, struct v4l2_querymenu* qmenu)
{
  memcpy(menu->name, qmenu->name, sizeof qmenu->name);
}
#ifndef CAMERA_OLD_VIDEODEV2_H
static void
camera_integer_menu_copy(camera_menu_t* menu, struct v4l2_querymenu* qmenu)
{
  menu->value = qmenu->value;
}
#endif
static void
camera_controls_menus(const camera_t* camera, camera_control_t* control)
{
  void (*copy)(camera_menu_t*, struct v4l2_querymenu*) = &camera_menu_copy;
  switch (control->type) {
  case CAMERA_CTRL_MENU:
    break;
#ifndef CAMERA_OLD_VIDEODEV2_H
  case CAMERA_CTRL_INTEGER_MENU:
    copy = &camera_integer_menu_copy;
    break;
#endif
  default:
    control->menus.length = 0;
    control->menus.head = NULL;
    return;
  }
  control->menus.length = control->max + 1;
  control->menus.head = calloc(control->menus.length, sizeof (camera_menu_t));
  for (uint32_t mindex = 0; mindex < control->menus.length; mindex++) {
    struct v4l2_querymenu qmenu;
    memset(&qmenu, 0, sizeof qmenu);
    qmenu.id = control->id;
    qmenu.index = mindex;
    if (ioctl(camera->fd, VIDIOC_QUERYMENU, &qmenu) == 0) {
      copy(&control->menus.head[mindex], &qmenu);
    }
  }
}
static camera_control_t*
camera_controls_query(const camera_t* camera, camera_control_t* control_list)
{
  camera_control_t* control_list_last = control_list;

  for (uint32_t cid = V4L2_CID_USER_BASE; cid < V4L2_CID_LASTP1; cid++) {
    struct v4l2_queryctrl qctrl;
    memset(&qctrl, 0, sizeof qctrl);
    qctrl.id = cid;
    if (ioctl(camera->fd, VIDIOC_QUERYCTRL, &qctrl) == -1) continue;
    camera_control_t* control = control_list_last++;
    control->id = qctrl.id;
    memcpy(control->name, qctrl.name, sizeof qctrl.name);
    control->flags.disabled = (qctrl.flags & V4L2_CTRL_FLAG_DISABLED) != 0;
    control->flags.grabbed = (qctrl.flags & V4L2_CTRL_FLAG_GRABBED) != 0;
    control->flags.read_only = (qctrl.flags & V4L2_CTRL_FLAG_READ_ONLY) != 0;
    control->flags.update = (qctrl.flags & V4L2_CTRL_FLAG_UPDATE) != 0;
    control->flags.inactive = (qctrl.flags & V4L2_CTRL_FLAG_INACTIVE) != 0;
    control->flags.slider = (qctrl.flags & V4L2_CTRL_FLAG_SLIDER) != 0;
    control->flags.write_only = (qctrl.flags & V4L2_CTRL_FLAG_WRITE_ONLY) != 0;
    control->flags.volatile_value =
      (qctrl.flags & V4L2_CTRL_FLAG_VOLATILE) != 0;
    control->type = qctrl.type;
    control->max = qctrl.maximum;
    control->min = qctrl.minimum;
    control->step = qctrl.step;
    control->default_value = qctrl.default_value;
    camera_controls_menus(camera, control);
  }
  return control_list_last;
}
camera_controls_t* camera_controls_new(const camera_t* camera)
{
  camera_control_t control_list[V4L2_CID_LASTP1 - V4L2_CID_USER_BASE];
  camera_control_t* control_list_last =
    camera_controls_query(camera, control_list);
  camera_controls_t* controls = malloc(sizeof (camera_controls_t));
  controls->length = control_list_last - control_list;
  controls->head = calloc(controls->length, sizeof (camera_control_t));
  for (size_t i = 0; i < controls->length; i++) {
    controls->head[i] = control_list[i];
  }
  return controls;
}

void camera_controls_delete(camera_controls_t* controls)
{
  for (size_t i = 0; i < controls->length; i++) {
    free(controls->head[i].menus.head);
  }
  free(controls);
}

bool camera_control_get(camera_t* camera, uint32_t id, int32_t* value)
{
  struct v4l2_control ctrl;
  ctrl.id = id;
  ctrl.value = 0;
  if (ioctl(camera->fd, VIDIOC_G_CTRL, &ctrl) == -1)
    return error(camera, "VIDIOC_G_CTRL");
  *value = ctrl.value;
  return true;
}

bool camera_control_set(camera_t* camera, uint32_t id, int32_t value)
{
  struct v4l2_control ctrl;
  ctrl.id = id;
  ctrl.value = value;
  if (ioctl(camera->fd, VIDIOC_S_CTRL, &ctrl) == -1)
    return error(camera, "VIDIOC_S_CTRL");
  return true;
}
