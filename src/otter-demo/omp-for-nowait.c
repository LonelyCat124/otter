#include <omp.h>

#define THREADS   2
#define LOOPS     1

int main(void)
{
    int j=0, k=0;
    #pragma omp parallel num_threads(2)
    {
        #pragma omp for nowait
        for (j=0; j<LOOPS; j++)
        {
            if (omp_get_thread_num() == 0) while(1);
        }

        #pragma omp for nowait
        for (k=0; k<LOOPS; k++)
        {
            // if (omp_get_thread_num() == 0) while(1);
        }
    }
}