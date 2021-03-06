/*
 * cj_Lapack.c
 * Implement a subset of LAPACK (Linear Algebra Package) based on the FLAME implementation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef CJ_HAVE_CUDA
#include <cuda_runtime_api.h>
#include <cublas_v2.h>
#endif

#include <cj.h>

#define dA(i,j)  (dA + (j)*ldda + (i))

void cj_Lapack_error (const char *func_name, char* msg_text) {
  fprintf(stderr, "CJ_LAPACK_ERROR: %s(): %s\n", func_name, msg_text);
  abort();
  exit(0);
}

int get_dpotrf_nb (int n) {
	if (n <= 3328) return 128;
	else if (n<=4256) return 128;
	else return 256;
}

#ifdef CJ_HAVE_CUDA
void hybrid_dpotrf (cublasHandle_t *handle, int n, double *dA, int ldda, int *info) {
  int	   j, jb, nb;
  double f_one  = 1.0, f_mone = -1.0;
  double *work;

  cublasStatus_t status;

  *info = 0;

  cudaStream_t stream[2];
  cudaStreamCreate(&stream[0]);
  cudaStreamCreate(&stream[1]);

  nb = get_dpotrf_nb(n);
  cudaMallocHost((void**)&work, nb*nb*sizeof(double));

  if ((nb <= 1) || (nb >= n)) {
    cublasGetMatrix(n, n, sizeof(double), dA, ldda, work, n);
    dpotrf("L", &n, work, &n, info);
    cublasSetMatrix(n, n, sizeof(double), work, n, dA, ldda);
  } 
  else {
    for (j = 0; j < n; j += nb) {
      jb = min(nb, (n - j));

      status = cublasDsyrk(*handle, CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_N, 
          jb, j, &f_mone, dA(j,0), ldda, &f_one, dA(j,j), ldda);
      cudaMemcpy2DAsync(work, jb*sizeof(double), dA(j,j), ldda*sizeof(double),
          sizeof(double)*jb, jb, cudaMemcpyDeviceToHost, stream[1]);

      if ((j + jb) < n) {
			  status = cublasDgemm(*handle, CUBLAS_OP_N, CUBLAS_OP_T, 
            (n - j - jb), jb, j, &f_mone, dA(j+jb,0), ldda, 
						dA(j,0), ldda, &f_one, dA(j+jb,j), ldda);
      }
      cudaStreamSynchronize(stream[1]);
      dpotrf("L", &jb, work, &jb, info);
      if (*info != 0) {
        *info = *info + j;
        break;
      }
      cudaMemcpy2DAsync(dA(j,j), ldda*sizeof(double), work, jb*sizeof(double),
          sizeof(double)*jb, jb, cudaMemcpyHostToDevice, stream[0]);

      if ( (j + jb) < n) {
        status = cublasDtrsm(*handle, CUBLAS_SIDE_RIGHT, CUBLAS_FILL_MODE_LOWER, CUBLAS_OP_T, CUBLAS_DIAG_NON_UNIT, 
            (n - j - jb), jb, &f_one, dA(j,j), ldda, dA(j+jb,j), ldda); 
      }
    }
  }
  cudaStreamDestroy(stream[0]);
  cudaStreamDestroy(stream[1]);

  cudaFreeHost(work);
};
#endif

void cj_Chol_l_task_function (void *task_ptr) {
  cj_Task *task = (cj_Task *) task_ptr;
  cj_Worker *worker = task->worker;
  cj_devType devtype = worker->devtype;
  int device_id = worker->device_id;
  int dest = device_id + 1;

  cj_Object *A;
  cj_Matrix *a;
  A = task->arg->dqueue->head;
  a = A->matrix;

  if (device_id != -1 && devtype == CJ_DEV_CUDA) {
#ifdef CJ_HAVE_CUDA
    cudaSetDevice(device_id);
    cj_Device *device = worker->cj_ptr->device[device_id];
    cj_Cache *cache = &(device->cache);
    cublasHandle_t *handle = &(device->handle);
    cj_Distribution *dist_a = a->base->dist[a->offm/BLOCK_SIZE][a->offn/BLOCK_SIZE];
    if (dist_a->avail[dest] != TRUE) {
      cj_Blas_error("cj_Chol_l_task_function", "No propriate distribution.");
    }

    if (a->eletype == CJ_SINGLE) { 
      float f_one = 1.0, f_mone = -1.0;
      float *a_buff = (float *) cache->dev_ptr[dist_a->line[dest]];
    }
    else {
      double f_one = 1.0, f_mone = -1.0;
      int info;
      double *a_buff = (double *) cache->dev_ptr[dist_a->line[dest]];
      cudaSetDevice(device->id);
      hybrid_dpotrf (&device->handle, a->m, a_buff, BLOCK_SIZE, &info);
    }
#endif
  }
  else {
    if (a->eletype == CJ_SINGLE) {
      float f_one = 1.0, f_mone = -1.0;
      int info = 0;
      float *a_buff = (float *) (a->base->buff) + (a->base->m)*(a->offn) + a->offm;
      spotrf_("L", &(a->m), a_buff, &(a->base->m), &info);
    }
    else {
      double f_one = 1.0, f_mone = -1.0;
      int info = 0;
      double *a_buff = (double *) (a->base->buff) + (a->base->m)*(a->offn) + a->offm;
      dpotrf_("L", &(a->m), a_buff, &(a->base->m), &info);
    }
  }

  fprintf(stderr, YELLOW "  Worker_execute %d (%d, %s), A(%d, %d): \n" NONE, 
      task->worker->id, task->id, task->name,
      a->offm/BLOCK_SIZE, a->offn/BLOCK_SIZE);
}

void cj_Chol_l_task(cj_Object *A) {
  /* Gemm will read A, B, C and write C. */
  cj_Object *A_copy, *task;
  cj_Matrix *a;
  
  /* Generate copies of A, B and C. */
  A_copy = cj_Object_new(CJ_MATRIX);
  cj_Matrix_duplicate(A, A_copy);

  a = A_copy->matrix; 
  /*
     fprintf(stderr, "          a->base : %d\n", (int)a->base);
     fprintf(stderr, "          b->base : %d\n", (int)b->base);
     fprintf(stderr, "          c->base : %d\n", (int)c->base);
     fprintf(stderr, "          a->offm : %d, a->offn : %d\n", a->offm, a->offn);
     fprintf(stderr, "          b->offm : %d, b->offn : %d\n", b->offm, b->offn);
     fprintf(stderr, "          c->offm : %d, c->offn : %d\n", c->offm, c->offn);
  */   

  task = cj_Object_new(CJ_TASK);
  cj_Task_set(task->task, CJ_TASK_POTRF, &cj_Chol_l_task_function);

  /* Pushing arguments. */
  cj_Object *arg_A = cj_Object_append(CJ_MATRIX, a);
  arg_A->rwtype = CJ_RW;
  cj_Dqueue_push_tail(task->task->arg, arg_A);

  /* Setup task name. */
  snprintf(task->task->name,  64, "Chol_l%d", task->task->id);
  snprintf(task->task->label, 64, "A%d%d=L*L'", 
      a->offm/BLOCK_SIZE, a->offn/BLOCK_SIZE);

  cj_Task_dependency_analysis(task);
}

//void cj_Chol_l_unb_var3(cj_Object *A) {
//  cj_Object *ATL,   *ATR,      *A00,  *a01,     *A02,
//            *ABL,   *ABR,      *a10t, *alpha11, *a12t,
//                               *A20,  *a21,     *A22;
//
//  fprintf(stderr, "Chol_l_unb_var3 (A(%d, %d)): \n", 
//	  A->matrix->m, A->matrix->n);
//  fprintf(stderr, "{\n");
//
//  ATL = cj_Object_new(CJ_MATRIX); ATR = cj_Object_new(CJ_MATRIX); 
//  A00 = cj_Object_new(CJ_MATRIX); a01 = cj_Object_new(CJ_MATRIX); A02 = cj_Object_new(CJ_MATRIX);
//
//  ABL = cj_Object_new(CJ_MATRIX); ABR = cj_Object_new(CJ_MATRIX);
//  a10t = cj_Object_new(CJ_MATRIX); alpha11 = cj_Object_new(CJ_MATRIX); a12t = cj_Object_new(CJ_MATRIX);
//  A20 = cj_Object_new(CJ_MATRIX); a21 = cj_Object_new(CJ_MATRIX); A22 = cj_Object_new(CJ_MATRIX);
//
//
//  cj_Matrix_part_2x2( A,    ATL, ATR,
//                      ABL,  ABR,     0, 0, CJ_TL );
//
//  while ( ATL->matrix->m  < A->matrix->m ){
//
//    cj_Matrix_repart_2x2_to_3x3( ATL, /**/ ATR,       A00,  /**/ a01,     A02,
//                        /* ************* */   /* ************************** */
//                                                     a10t, /**/ alpha11, a12t,
//                                 ABL, /**/ ABR,       A20,  /**/ a21,     A22,
//                                 1, 1, CJ_BR );
//
//    /*------------------------------------------------------------*/
//
//    // alpha11 = sqrt( alpha11 )
//    cj_Sqrt( alpha11 );
//
//    //if ( r_val != FLA_SUCCESS )
//    //  return ( FLA_Obj_length( A00 ) );
//
//    // a21 = a21 / alpha11
//    //FLA_Inv_scal_external( alpha11, a21 );
//	//cj_Inv_Scal( alpha11, a21);
//    cj_Trsm_rlt(alpha11, a21);
//
//    // A22 = A22 - a21 * a21'
//    //FLA_Her_external( FLA_LOWER_TRIANGULAR, FLA_MINUS_ONE, a21, A22 );
//	cj_Syrk_ln(a21, A22);
//
//    /*------------------------------------------------------------*/
//
//    cj_Matrix_cont_with_3x3_to_2x2( ATL, /**/ ATR,       A00,  a01,     /**/ A02,
//                                                  a10t, alpha11, /**/ a12t,
//                            /* ************** */  /* ************************ */
//                              ABL, /**/ ABR,       A20,  a21,     /**/ A22,
//                              CJ_TL );
//  }
//
//
//}


void cj_Chol_l_blk_var3(cj_Object *A )
{
  cj_Object *ATL,   *ATR,      *A00, *A01, *A02, 
            *ABL,   *ABR,      *A10, *A11, *A12,
                               *A20, *A21, *A22;

  fprintf(stderr, "Chol_l_blk_var3 (A(%d, %d)): \n", 
	  A->matrix->m, A->matrix->n);
  fprintf(stderr, "{\n");

  ATL = cj_Object_new(CJ_MATRIX); ATR = cj_Object_new(CJ_MATRIX); 
  A00 = cj_Object_new(CJ_MATRIX); A01 = cj_Object_new(CJ_MATRIX); A02 = cj_Object_new(CJ_MATRIX);

  ABL = cj_Object_new(CJ_MATRIX); ABR = cj_Object_new(CJ_MATRIX);
  A10 = cj_Object_new(CJ_MATRIX); A11 = cj_Object_new(CJ_MATRIX); A12 = cj_Object_new(CJ_MATRIX);
  A20 = cj_Object_new(CJ_MATRIX); A21 = cj_Object_new(CJ_MATRIX); A22 = cj_Object_new(CJ_MATRIX);

  int b;

  cj_Matrix_part_2x2( A,    ATL, ATR,
                            ABL, ABR,     0, 0, CJ_TL );

  while ( ATL->matrix->m  < A->matrix->m ){

	b = min(ABR->matrix->m, BLOCK_SIZE);

    cj_Matrix_repart_2x2_to_3x3( ATL, /**/ ATR,       A00, /**/ A01, A02,
                              /* ************* */   /* ******************** */
                                                      A10, /**/ A11, A12,
                                 ABL, /**/ ABR,       A20, /**/ A21, A22,
                                 b, b, CJ_BR );
    /*------------------------------------------------------------*/

    // A11 = chol( A11 )
    //FLA_Chol_internal( FLA_LOWER_TRIANGULAR, A11,
    //                           FLA_Cntl_sub_chol( cntl ) );
	
	//cj_Chol_l_unb_var3(A11);

    cj_Chol_l_task(A11);

    //if ( r_val != FLA_SUCCESS )
    //  return ( FLA_Obj_length( A00 ) + r_val );
	//fprintf some error message if fails?

    // A21 = A21 * inv( tril( A11 )' )
    cj_Trsm_rlt(A11, A21);

    // A22 = A22 - A21 * A21'
	cj_Syrk_ln(A21, A22);

    /*------------------------------------------------------------*/

    cj_Matrix_cont_with_3x3_to_2x2( ATL, /**/ ATR,       A00, A01, /**/ A02,
                                                     A10, A11, /**/ A12,
                            /* ************** */  /* ****************** */
                              ABL, /**/ ABR,       A20, A21, /**/ A22,
                              CJ_TL );
  }

}


/* A -> LL^T, A is SPD matrix. A: n*n. NEED to do: Symmmetry */
void cj_Chol_l (cj_Object *A) {
  cj_Matrix *a;
  if (!A) 
    cj_Lapack_error("chol", "matrice hasn't been initialized yet.");
  if (!A->matrix)
    cj_Lapack_error("chol", "Object types are not matrix type.");
  a = A->matrix;
  if ((a->m != a->n)) 
    cj_Lapack_error("chol", "matrice is not a square matrix.");

  cj_Queue_end();
  cj_Chol_l_blk_var3(A);
  cj_Queue_begin();
}



