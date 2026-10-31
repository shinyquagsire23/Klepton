#include <stdio.h>
#include <stdlib.h>
int zip_stream_extract(const char*,size_t,const char*,int (*)(const char*,void*),void*);
int main(int argc,char**argv){
 if(argc!=3)return 2;
 FILE*f=fopen(argv[1],"rb");if(!f)return 2;
 if(fseek(f,0,SEEK_END))return 2;long n=ftell(f);if(n<0 || n>512L*1024*1024)return 2;
 rewind(f);char*data=malloc((size_t)n);if(!data)return 2;
 if(fread(data,1,(size_t)n,f)!=(size_t)n)return 2;fclose(f);
 int result=zip_stream_extract(data,(size_t)n,argv[2],NULL,NULL);free(data);return result?1:0;
}
