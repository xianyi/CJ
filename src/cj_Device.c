#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <cuda_runtime_api.h>

/*
#include "cj_Device.h"
#include "cj_Object.h"
*/
#include <CJ.h>

static int gpu_counter = 0;
static int mic_counter = 0;

void cj_Device_error (const char *func_name, char* msg_text) {
  fprintf(stderr, "CJ_DEVICE_ERROR: %s(): %s\n", func_name, msg_text);
  abort();
  exit(0);
}

char *cj_Device_malloc(size_t len, cj_devType devtype) {
  char *ptr;
  if (devtype == CJ_DEV_CUDA) {
    cudaMalloc((void**)&ptr, len);
  }
  
  return ptr;
}

void cj_Device_free(uintptr_t ptr, cj_devType devtype) {
  if (devtype == CJ_DEV_CUDA) {
    cudaFree((char *) ptr);
  }
}

void cj_Device_report(cj_Device *device) {
}

void cj_Device_bind() {

}

cj_Device *cj_Device_new(cj_devType devtype) {
  int i;

  cj_Device *device = (cj_Device*) malloc(sizeof(cj_Device));
  if (!device) cj_Device_error("Device_new", "memory allocation failed.");

  device->devtype = devtype;

  if (devtype == CJ_DEV_CUDA) {
    cudaError_t error;
	  struct cudaDeviceProp prop;
    error = cudaGetDeviceProperties(&prop, gpu_counter);
	  device->name = prop.name;
	  gpu_counter ++;

	  /* Setup device cache */
	  device->cache.line_size = BLOCK_SIZE*BLOCK_SIZE*sizeof(double);
    for (i = 0; i < CACHE_LINE; i++) {
      device->cache.status[i] = CJ_CACHE_CLEAN;
      device->cache.last_use[i] = 0;
      device->cache.dev_ptr[i] = cj_Device_malloc(device->cache.line_size, CJ_DEV_CUDA);
      //device->cache.hos_ptr[i] = NULL;
    }

    //cj_Device_report(device);
    fprintf(stderr, "  Name         : %s (%d.%d)\n", prop.name, prop.major, prop.minor);
    //fprintf(stderr, "  Device Memory: %d Mbytes\n", (int) ((prop.totalGlobalMem/1024)/1024));
    fprintf(stderr, "  Device Memory: %d Mbytes\n", (unsigned int) (prop.totalGlobalMem/1024)/1024);
  }

  return device;
}