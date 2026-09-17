#include <cuda_runtime.h>
#include <cstdio>
#include <chrono>
#define CUDA(x) do {cudaError_t e=(x);if(e!=cudaSuccess){fprintf(stderr,"%s: %s\n",#x,cudaGetErrorString(e));return 2;}} while(0)
int main() {
    int count=0;CUDA(cudaGetDeviceCount(&count));
    if(count!=1){fprintf(stderr,"Expected one CUDA GPU, found %d\n",count);return 2;}
    char bus[32];CUDA(cudaDeviceGetPCIBusId(bus,sizeof(bus),0));
    const size_t n=256ull<<20;unsigned char *h=nullptr,*d=nullptr;
    CUDA(cudaMallocHost(&h,n));CUDA(cudaMalloc(&d,n));
    for(size_t i=0;i<n;i++)h[i]=(unsigned char)((i*37u)^(i>>16));
    printf("GPU=%s pinned buffer=256MiB, sequential H2D/D2H; no MMIO or reset\n",bus);fflush(stdout);
    for(int direction=0;direction<2;direction++) {
        auto start=std::chrono::steady_clock::now();unsigned iterations=0;double seconds;
        do {
            if(!direction){CUDA(cudaMemcpy(d,h,n,cudaMemcpyHostToDevice));}
            else {CUDA(cudaMemcpy(h,d,n,cudaMemcpyDeviceToHost));}
            iterations++;
            seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        }while(seconds<12.0);
        printf("%s %.3f GB/s wall=%.3fs copies=%u\n",direction?"D2H":"H2D",n*double(iterations)/seconds/1e9,seconds,iterations);fflush(stdout);
        if(!direction)for(size_t i=0;i<n;i++)h[i]=0;
    }
    for(size_t i=0;i<n;i++)if(h[i]!=(unsigned char)((i*37u)^(i>>16))){fprintf(stderr,"Mismatch offset=%llu\n",(unsigned long long)i);return 3;}
    CUDA(cudaFree(d));CUDA(cudaFreeHost(h));puts("PASS: full 256MiB round-trip pattern verified");return 0;
}
