#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 500

#include "utils.h"

#include <stdio.h>
#include <stdlib.h>

#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int keepRunning = 1;

void generate(unsigned char *buf, size_t size, unsigned int seed) {
  srand(seed);
  // interate in ints not chars
  unsigned int *p = (unsigned int*)buf;
  for (size_t i = 0; i < size / sizeof(unsigned int); i++) {
    *p = rand();
    p++;
  }
}



void verifyDevice(char *device, unsigned char *buf, size_t size, const size_t offset) {
  
  unsigned char *readbuf = aligned_alloc(4096, size);
  if (!buf) {
    fprintf(stderr,"*error* can't allocate %zd bytes\n", size);
    exit(1);
  }
  memset(readbuf, 0, size);
  
  int fd = open(device, O_DIRECT | O_RDONLY | O_EXCL, S_IRUSR | S_IWUSR);
  if (fd >= 0) {
    unsigned char *p = readbuf;
    ssize_t ret = 0;
    size_t toreadleft = size;

    lseek64(fd, offset, SEEK_SET);

    while ((ret = read(fd, p, MIN(1024*1024,toreadleft))) != 0) {
      //      fprintf(stderr,"*info read %zd %zd\n", p - readbuf, toreadleft);
      if (ret < 0) {
	fprintf(stderr,"*error* read problem\n");
	perror(device);
	exit(1);
      }
      toreadleft -= ret;
      p += ret;
    }
      
    if (toreadleft != 0) {
      fprintf(stderr,"*error* incomplete read size of %zd\n", size - toreadleft);
      exit(1);
    }
    
    // print out
    unsigned char str1[11], str2[11];
    memcpy(str1, buf, 10);
    memcpy(str2, readbuf, 10);
    str1[10]=0;
    str2[10]=0;

    int pc = memcmp(buf, readbuf, size);
    if (pc != 0) {
      for (size_t i = 0; i < size; i++) {
	if (buf[i] != readbuf[i]) {
	  fprintf(stderr,"*error* failed verification at offset %zd, position %zd in %s\n", offset, i, device);
	  break;
	}
      }
      exit(1);
    }
    fprintf(stderr,"*info* offset %zd, succeeded %zd byte verification of '%s'.\n*info* the first few bytes: RAM '%s', on device '%s'\n", offset, size, device, str1, str2);
    close(fd);
  } else {
    perror(device);
    exit(1);
  }
}

void usage() {
  fprintf(stderr,"Usage: randdd -f /dev/device [-R seed] -G size (GiB) -v (verify) -n gaps (number of locations)\n");
}

int main(int argc, char *argv[]) {

  int opt = 0, help = 0;
  unsigned int seed = 42;
  int verify = 0;
  size_t size = 16*1024*1024;
  char *device = NULL;
  size_t startpos = 0, endpos = 0, gapcount = 1, zap = 0;
  
  while ((opt = getopt(argc, argv, "f:G:wvhR:n:z")) != -1) {
    switch (opt) {
    case 'h':
      help = 1;
      break;
    case 'z':
      zap = 1;
      break;
    case 'f':
      device = optarg;
      break;
    case 'n':
      gapcount = atoi(optarg);
      if (gapcount < 1) gapcount = 1;
      break;
    case 'R':
      seed = atoi(optarg);
      break;
    case 'G':
      size = (size_t)(atof(optarg) * 1024) * 1024 * 1024;
      fprintf(stderr,"*info* size %zd bytes (%.3lf GiB), RAM %zd bytes (%.0lf GiB)\n", size, TOGiB(size), totalRAM(), TOGiB(totalRAM()));
      if (size > totalRAM()) {
	fprintf(stderr,"*error* G ram is more than actual RAM\n");
	exit(1);
      }
      break;
    case 'v':
      verify = 1;
      break;
    case 'w':
      verify = 0;
      break;
    }
  }

  if (help || !device) {
    usage();
    exit(1);
  }

  unsigned char *buf = aligned_alloc(4096, size);
  if (!buf) {
    fprintf(stderr,"*error* can't allocate %zd bytes\n", size);
    exit(1);
  }

  if (zap) {
    memset(buf, 0, size);
  } else {
    generate(buf, size, seed);
  }


  fprintf(stderr,"*info* randdd on '%s', seed %d, size %zd (%.3lf GiB), mode: %s\n", device, seed, size, TOGiB(size), verify?"VERIFY" : "WRITE");

  int fd = open(device, O_DIRECT | O_RDONLY | O_EXCL, S_IRUSR | S_IWUSR);
  if (fd >= 0) {
    endpos = blockDeviceSizeFromFD(fd);
  }
  double gap = (endpos * 1.0 - size - startpos * 1.0) / gapcount;
  close(fd);
  if (gap < size) {
    fprintf(stderr,"*warning* the gap is too small (%.0lf), setting to %zd\n", gap, size);
    gap = size;
  }
  fprintf(stderr,"*info* pos range [%zd, %zd), size %zd, locations %zd, gap %.0lf\n", startpos, endpos, size, gapcount, gap);

  if (!verify) {
    int fd = open(device, O_DIRECT | O_WRONLY | O_TRUNC | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd >= 0) {

      for (double offset = startpos; offset <= endpos - size; offset += gap) {
	
	size_t aloff = alignedNumber((size_t)(offset + 0.5), 4096);
	fprintf(stderr,"*info* write position range: [%zd, %zd) %.1lf%%\n", aloff, aloff + size, aloff*100.0/(endpos - size));
	lseek64(fd, aloff, SEEK_SET);
	
	size_t towrite = size;
	unsigned char *p = buf;
	while (towrite > 0) {
	  ssize_t written  = write(fd, p, MIN(towrite, 1024 * 1024));
	  if (written <= 0) {
	    perror(device);
	    exit(1);
	  }
	  towrite -= written;
	  p += written;
	}
	fdatasync(fd);
      }
      
      close(fd);
      //
      fprintf(stderr,"*info* verifying prior write is on disk\n");
      for (double offset = startpos; offset <= endpos - size; offset += gap) {
	//	fprintf(stderr,"*info* start position: %zd\n", offset);
	size_t aloff = alignedNumber((size_t)(offset + 0.5), 4096);
	verifyDevice(device, buf, size, aloff);
      }
      fprintf(stderr,"*info* write stored OK\n");
  } else {
    perror(device);
    exit(1);
  }
} else {
  for (double offset = startpos; offset <= endpos - size; offset += gap) {
    size_t aloff = alignedNumber((size_t)(offset + 0.5), 4096);
      verifyDevice(device, buf, size, aloff);
    }
    fprintf(stderr,"*info* read verified OK\n");
  }

  free(buf);

}

