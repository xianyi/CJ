void cj_Gemm_nn_task_function (void*);
void cj_Gemm_nn (cj_Object*, cj_Object*, cj_Object*);

extern void sgemm_( char *, char *, int *, int *, int *, float *, float *, int *, float *, int *, float *, float *, int * );
extern void dgemm_( char *, char *, int *, int *, int *, double *, double *, int *, double *, int *, double *, double *, int * );
