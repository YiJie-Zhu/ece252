#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "./lab_png.h"
#include "./crc.c"
#include "./zutil.c"
#include <curl/curl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <time.h>
#include <sys/time.h>


#define PNG_TOP_LENGTH 33
#define PNG_BOT_LENGTH 12
#define IMG_URL "http://ece252-1.uwaterloo.ca:2530/image?img=1&part=20"
#define DUM_URL "https://example.com/"
#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 10240  /* 1024*10 = 10K */
#define BUF_LENGTH 20
#define MAX_BUF_SIZE 1048576
#define NUM_FILES 50
#define STRIP_HEIGHT 6
#define STRIP_WIDTH 400


sem_t *itemSem;
sem_t *spaceSem;
sem_t *bufferMutex;
sem_t *countMutex;
sem_t *chunkMutex;
sem_t *fileMutex;

int *pindex = 0;
int *cindex = 0;

int *totalCount = 0;
int *totalConsumed = 0;

int image_num = 0;
int buffer_size = 0;
int num_producers = 0;
int num_consumers = 0;
int time_sleep = 0;

/* This is a flattened structure, buf points to 
   the memory address immediately after 
   the last member field (i.e. seq) in the structure.
   Here is the memory layout. 
   Note that the memory is a chunk of continuous bytes.
   On a 64-bit machine, the memory layout is as follows:
   +================+
   | buf            | 8 bytes
   +----------------+
   | size           | 8 bytes
   +----------------+
   | max_size       | 8 bytes
   +----------------+
   | seq            | 4 bytes
   +----------------+
   | padding        | 4 bytes
   +----------------+
   | buf[0]         | 1 byte
   +----------------+
   | buf[1]         | 1 byte
   +----------------+
   | ...            | 1 byte
   +----------------+
   | buf[max_size-1]| 1 byte
   +================+
*/
typedef struct recv_buf_flat {
    char *buf;       /* memory to hold a copy of received data */
    size_t size;     /* size of valid data in buf in bytes*/
    size_t max_size; /* max capacity of buf in bytes*/
    int seq;         /* >=0 sequence number extracted from http header */
                     /* <0 indicates an invalid seq number */
} RECV_BUF;

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata);
size_t write_cb_curl(char *p_recv, size_t size, size_t nmemb, void *p_userdata);
int recv_buf_init(RECV_BUF *ptr, size_t max_size);
int recv_buf_cleanup(RECV_BUF *ptr);
int write_file(const char *path, const void *in, size_t len);

U32 swap(U32 value)
{
    value = ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0xFF00FF);
    return (value << 16) | (value >> 16);
}

U32 getIHDRcrc(data_IHDR_p IDHRdata, U32* IHDRtype, U32* width, U32* height){
    FILE *write = fopen("./IHDR.png", "wb+");
    *IHDRtype = swap(*IHDRtype);
    fwrite(IHDRtype, sizeof(U32), 1, write);
    *height = swap(*height);
    *width = swap(*width);
    fwrite(width, sizeof(U32), 1, write);
    fwrite(height, sizeof(U32), 1, write);
    
    fwrite(&IDHRdata->bit_depth, sizeof(U8), 1, write);
    fwrite(&IDHRdata->color_type, sizeof(U8), 1, write);
    fwrite(&IDHRdata->compression, sizeof(U8), 1, write);
    fwrite(&IDHRdata->filter, sizeof(U8), 1, write);
    fwrite(&IDHRdata->interlace, sizeof(U8), 1, write);
    fclose(write);
    FILE *read = fopen("./IHDR.png", "rb");
    U8 IHDRcrcdata[17];
    fread(IHDRcrcdata, sizeof(IHDRcrcdata), 1, read);
    U32 crc_val = crc(IHDRcrcdata, 17);
    fclose(read);
    return crc_val;
}

U32 getIDATcrc(chunk_p IDATchunk, U64 length){
    U8 *IDATall = malloc(sizeof(U8) * length + 4);
    IDATall[0] = IDATchunk->type[0];
    IDATall[1] = IDATchunk->type[1];
    IDATall[2] = IDATchunk->type[2];
    IDATall[3] = IDATchunk->type[3];
    for(int i = 4; i < length + 4; i++){
        IDATall[i] = IDATchunk->p_data[i-4];
    }
    U32 crc_val = crc(IDATall, length + 4);
    free(IDATall);
    return crc_val;
}

void dataToChunk(chunk_p chunk, U8* data, size_t size){
    U32 *length_ptr = malloc(sizeof(U32));
    memcpy(length_ptr, data+33, sizeof(U32));
    chunk->length = ((*length_ptr>>24)&0xff) | ((*length_ptr<<8)&0xff0000) | ((*length_ptr>>8)&0xff00) | ((*length_ptr<<24)&0xff000000);
    chunk->p_data = malloc(chunk->length);
    memcpy(chunk->p_data, data + 33 + 8, chunk->length);
    free(length_ptr);
}

int recv_buf_init(RECV_BUF *ptr, size_t max_size)
{
    void *p = NULL;
    if (ptr == NULL) {
        return 1;
    }

    p = malloc(max_size);
    if (p == NULL) {
	return 2;
    }
    
    ptr->buf = p;
    ptr->size = 0;
    ptr->max_size = max_size;
    ptr->seq = -1;              /* valid seq should be non-negative */
    return 0;
}

size_t header_cb_curl(char *p_recv, size_t size, size_t nmemb, void *userdata)
{
    int realsize = size * nmemb;
    RECV_BUF *p = userdata;
    
    if (realsize > strlen(ECE252_HEADER) &&
	strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

        /* extract img sequence number */
	p->seq = atoi(p_recv + strlen(ECE252_HEADER));

    }
    return realsize;
}

size_t write_cb_curl(char *p_recv, size_t size, size_t nmemb, void *p_userdata)
{
    size_t realsize = size * nmemb;
    RECV_BUF *p = (RECV_BUF *)p_userdata;
 
    if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */ 
        fprintf(stderr, "User buffer is too small, abort...\n");
        abort();
    }

    memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
    p->size += realsize;
    p->buf[p->size] = 0;

    return realsize;
}

int sizeof_shm_recv_buf(size_t nbytes)
{
    return (sizeof(RECV_BUF) + sizeof(char) * nbytes);
}

int sizeof_shm_chunk()
{
    return (sizeof(struct chunk) + sizeof(U8) * 6 * (400*4+1));
}

int shm_recv_buf_init(RECV_BUF *ptr, size_t nbytes)
{
    if ( ptr == NULL ) {
        return 1;
    }
    
    ptr->buf = (char *)ptr + sizeof(RECV_BUF);
    ptr->size = 0;
    ptr->max_size = nbytes;
    ptr->seq = -1;              /* valid seq should be non-negative */
    
    return 0;
}

int shm_chunk_init(chunk_p ptr)
{
    if ( ptr == NULL ) {
        return 1;
    }

    ptr->p_data = (U8*)ptr + sizeof(struct chunk);
    
    return 0;
}

int write_file(const char *path, const void *in, size_t len)
{
    FILE *fp = NULL;

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }

    if (in == NULL) {
        fprintf(stderr, "write_file: input data is null!\n");
        return -1;
    }

    fp = fopen(path, "wb");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }

    if (fwrite(in, 1, len, fp) != len) {
        fprintf(stderr, "write_file: imcomplete write!\n");
        return -3; 
    }
    return fclose(fp);
}

int read_file(const char *path, U8* read, size_t len){
    FILE *fp = NULL;

    if (path == NULL) {
        fprintf(stderr, "write_file: file name is null!\n");
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        perror("fopen");
        return -2;
    }
    fread(read, len, 1, fp);
    return fclose(fp);
}

void producer(RECV_BUF* buffer[], int siteNum){
        CURL *curl_handle;
        CURLcode res;
        /* init a curl session */
        curl_handle = curl_easy_init();

        if (curl_handle == NULL) {
            fprintf(stderr, "curl_easy_init: returned NULL\n");
            return;
        }

        int stay = 1;

        while(stay == 1){
            
            sem_wait(countMutex);
            int tc = *totalCount;
            (*totalCount)++;
            
            sem_post(countMutex);
            if(tc >= 50){
                
                stay = 0;
                break;
            }
            char url[256];
            sprintf(url, "http://ece252-%d.uwaterloo.ca:2530/image?img=%d&part=%d", siteNum, image_num, tc);

            RECV_BUF *p_shm_recv_buf = malloc(sizeof(RECV_BUF) + sizeof(char)*BUF_SIZE);
            recv_buf_init(p_shm_recv_buf, MAX_BUF_SIZE);

            /* specify URL to get */
            curl_easy_setopt(curl_handle, CURLOPT_URL, url);

            /* register write call back function to process received data */
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl); 
            /* user defined data structure passed to the call back function */
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)p_shm_recv_buf);

            /* register header call back function to process received header data */
            curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl); 
            /* user defined data structure passed to the call back function */
            curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void *)p_shm_recv_buf);

            /* some servers requires a user-agent field */
            curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");

        
            /* get it! */
            res = curl_easy_perform(curl_handle);

            if( res != CURLE_OK) {
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            } else {
                sem_wait(spaceSem);
                sem_wait(bufferMutex);
                
                buffer[*pindex]->max_size = p_shm_recv_buf->max_size;
                buffer[*pindex]->size = p_shm_recv_buf->size;
                buffer[*pindex]->seq = p_shm_recv_buf->seq;
                memcpy(buffer[*pindex]->buf, p_shm_recv_buf->buf, p_shm_recv_buf->size);
                *pindex = (*pindex + 1) % buffer_size;
                sem_post(bufferMutex);
                sem_post(itemSem);
            }
            /* cleaning up */
            free(p_shm_recv_buf->buf);
            free(p_shm_recv_buf);
            curl_easy_reset(curl_handle);
        }
        shmdt(buffer);
        curl_easy_cleanup(curl_handle);
}

void consumer(RECV_BUF* buffer[], chunk_p chunks[]){
    int stay = 1;

    while(stay == 1){

        sem_wait(countMutex);
        int tc = *totalConsumed;
        (*totalConsumed)++;
        sem_post(countMutex);
        if(tc >= 50){
            stay = 0;
            break;
        }

        char fname[256];
        int size = 0;
        int seq = 0;

        sem_wait(itemSem);
        sem_wait(bufferMutex);
        seq = buffer[*cindex]->seq;
        size = buffer[*cindex]->size;
        
        sprintf(fname, "temp.png"); 
        write_file(fname, buffer[*cindex]->buf, size);
        *cindex = (*cindex + 1) % buffer_size;
        U8* tempData = malloc(sizeof(U8) * size * 4);
        read_file(fname, tempData, size * 4);
        remove(fname);
        sem_post(bufferMutex);
        sem_post(spaceSem);
        usleep(time_sleep*1000);
        chunk_p tempChunk = malloc(sizeof(struct chunk));
        dataToChunk(tempChunk, tempData, size * 4 - 45);

        U64 decompLength = (U64)(6*(400*4+1));
        chunk_p uncompChunk = malloc(sizeof(struct chunk));
        uncompChunk->p_data = malloc(sizeof(U8) * decompLength);
        mem_inf(uncompChunk->p_data, &decompLength, tempChunk->p_data, (U64)tempChunk->length);
        chunks[seq]->length = (U32)decompLength;
        memcpy(chunks[seq]->p_data, uncompChunk->p_data, decompLength);

        free(tempChunk->p_data);
        free(tempChunk);
        free(uncompChunk->p_data);
        free(uncompChunk);
        free(tempData);
        


    }


}


int main( int argc, char** argv ) 
{
    double times[2];
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[0] = (tv.tv_sec) + tv.tv_usec/1000000.;


    char *p = NULL;

    if(argc - 1 != 5){
        printf("Not enough arguements\n");
        return 0;
    }
    buffer_size = strtol(argv[1], &p, 10);
    num_producers = strtol(argv[2], &p, 10);
    num_consumers = strtol(argv[3], &p, 10);
    time_sleep = strtol(argv[4], &p, 10);
    image_num = strtol(argv[5], &p, 10);

    
    char url[256];
    RECV_BUF* buffer[buffer_size];
    chunk_p UCChunks[NUM_FILES];
    int shm_buf_ids[buffer_size];
    int shm_chunk_ids[50];
    int shm_size = sizeof_shm_recv_buf(BUF_SIZE);
    int shm_chunk_size = sizeof_shm_chunk();

    for(int i = 0; i < buffer_size; i++){
        shm_buf_ids[i] = shmget(IPC_PRIVATE, shm_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
        if ( shm_buf_ids[i] == -1 ) {
            perror("shmget");
            abort();
        }
        buffer[i] = shmat(shm_buf_ids[i], NULL, 0);
        shm_recv_buf_init(buffer[i], BUF_SIZE);
    }
    for(int i = 0; i < 50; i++){
       shm_chunk_ids[i] = shmget(IPC_PRIVATE, shm_chunk_size, IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
        if ( shm_chunk_ids[i] == -1 ) {
            perror("shmget");
            abort();
        }
        UCChunks[i] = shmat(shm_chunk_ids[i], NULL, 0);
        shm_chunk_init(UCChunks[i]);
    }


    if (argc == 1) {
        strcpy(url, IMG_URL); 
    } else {
        strcpy(url, argv[1]);
    }

    int shmid_sem_items = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_sem_spaces = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_sem_buffer = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_sem_count = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_sem_chunk = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_sem_file = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

    itemSem = shmat(shmid_sem_items, NULL, 0);
    spaceSem = shmat(shmid_sem_spaces, NULL, 0);
    bufferMutex = shmat(shmid_sem_buffer, NULL, 0);
    countMutex = shmat(shmid_sem_count, NULL, 0);
    chunkMutex = shmat(shmid_sem_chunk, NULL, 0);
    fileMutex = shmat(shmid_sem_file, NULL, 0);

    if ( itemSem == (void *) -1 || spaceSem == (void *) -1 || bufferMutex == (void *) -1) {
        perror("shmat");
        abort();
    }

    sem_init(itemSem, 1, 0);
    sem_init(spaceSem, 1, buffer_size);
    sem_init(bufferMutex, 1, 1);
    sem_init(countMutex, 1, 1);
    sem_init(chunkMutex, 1, 1);
    sem_init(fileMutex, 1, 1);

    int shmid_pindex = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_cindex = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

    pindex = shmat(shmid_pindex, NULL, 0);
    cindex = shmat(shmid_cindex, NULL, 0);

    int shmid_count = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
    int shmid_consumed = shmget(IPC_PRIVATE, sizeof(int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

    totalCount = shmat(shmid_count, NULL, 0);
    totalConsumed = shmat(shmid_consumed, NULL, 0);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    pid_t f = 0;
    for (int i = 0; i < num_producers; i++){
        f = fork();
        if (f == 0){
            producer(buffer, i%3+1);
            if ( shmdt(itemSem) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(spaceSem) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(bufferMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(countMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(chunkMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(fileMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(pindex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(cindex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(totalCount) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(totalConsumed) != 0 ) {
                perror("shmdt");
                abort();
            }
            for(int i = 0; i < buffer_size; i++){
                if ( shmdt(buffer[i]) != 0 ) {
                    perror("shmdt");
                    abort();
                }
            }
            for(int i = 0; i < 50; i++){
                if ( shmdt(UCChunks[i]) != 0 ) {
                    perror("shmdt");
                    abort();
                }
            }
            exit(0);
        }
    }
     for (int i = 0; i<num_consumers; i++){
        f = fork();
        if (f == 0){
            consumer(buffer, UCChunks);
            if ( shmdt(itemSem) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(spaceSem) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(bufferMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(countMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(chunkMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(fileMutex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(pindex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(cindex) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(totalCount) != 0 ) {
                perror("shmdt");
                abort();
            }
            if ( shmdt(totalConsumed) != 0 ) {
                perror("shmdt");
                abort();
            }
            for(int i = 0; i < buffer_size; i++){
                if ( shmdt(buffer[i]) != 0 ) {
                    perror("shmdt");
                    abort();
                }
            }
            for(int i = 0; i < 50; i++){
                if ( shmdt(UCChunks[i]) != 0 ) {
                    perror("shmdt");
                    abort();
                }
            }
            exit(0);
        }
    }
    while (wait(NULL)>0){}
    int k = 0;
    int totalDecompLength = UCChunks[0]->length*NUM_FILES;
    U8* AllUCData = malloc(sizeof(U8)*totalDecompLength);
    for(int i = 0; i < NUM_FILES; i++){
        for(int j = 0; j < UCChunks[i]->length; j++){
            AllUCData[k] = UCChunks[i]->p_data[j];
            k++;
        }
    }
    FILE *all = fopen("all.png", "wb+");

    U8* fIDATdata = malloc(sizeof(U8)*totalDecompLength);
    U64 IDATcomplength = 0;
    mem_def(fIDATdata, &IDATcomplength, AllUCData, totalDecompLength, Z_BEST_COMPRESSION);
    
    //Getting IDHR data
    U32 tempHeight = NUM_FILES * STRIP_HEIGHT;
    U32 tempWidth = STRIP_WIDTH;
    U32 IHDRType = 0x49484452;
    U32 IHDRLength = 0x0000000d;
    data_IHDR_p mockIDHR = malloc(sizeof(struct data_IHDR));
    mockIDHR->bit_depth = 0x08;
    mockIDHR->color_type = 0x06;
    mockIDHR->compression = 0x00;
    mockIDHR->filter = 0x00;
    mockIDHR->interlace = 0x00;
    U32 IHDRcrc = getIHDRcrc(mockIDHR, &IHDRType, &tempWidth, &tempHeight);

    //Getting IDAT data

    chunk_p fIDATchunk = malloc(sizeof(struct chunk));
    fIDATchunk->length = IDATcomplength;
    fIDATchunk->type[0] = 0x49;
    fIDATchunk->type[1] = 0x44;
    fIDATchunk->type[2] = 0x41;
    fIDATchunk->type[3] = 0x54;
    fIDATchunk->p_data = fIDATdata;

    U32 IDATcrc = getIDATcrc(fIDATchunk, IDATcomplength);

    //Getting IEND Data
    U32 IEND[3];
    IEND[0] = 0x00000000;
    IEND[1] = 0x444e4549;
    IEND[2] = 0x826042ae;

    //Getting header
    U32 header1 = 0x89504e47;
    header1 = swap(header1);
    U32 header2 = 0x0d0a1a0a;
    header2 = swap(header2);

    //Writing header
    fwrite(&header1, sizeof(U32), 1, all);
    fwrite(&header2, sizeof(U32), 1, all);

    //Write IHDR
    IHDRLength = swap(IHDRLength);
    fwrite(&IHDRLength, sizeof(U32), 1, all);
    fwrite(&IHDRType, sizeof(U32), 1, all);
    fwrite(&tempWidth, sizeof(U32), 1, all);
    fwrite(&tempHeight, sizeof(U32), 1, all);
    fwrite(&mockIDHR->bit_depth, sizeof(U8), 1, all);
    fwrite(&mockIDHR->color_type, sizeof(U8), 1, all);
    fwrite(&mockIDHR->compression, sizeof(U8), 1, all);
    fwrite(&mockIDHR->filter, sizeof(U8), 1, all);
    fwrite(&mockIDHR->interlace, sizeof(U8), 1, all);
    IHDRcrc = swap(IHDRcrc);
    fwrite(&IHDRcrc, sizeof(U32), 1, all);

    //Write IDAT
    U32 IDATlength = swap(fIDATchunk->length);
    IDATcrc = swap(IDATcrc);
    fwrite(&IDATlength, sizeof(U32), 1, all);
    fwrite(fIDATchunk->type, sizeof(fIDATchunk->type), 1, all);
    fwrite(fIDATchunk->p_data, IDATcomplength, 1, all);
    fwrite(&IDATcrc, sizeof(U32), 1, all);

    //Write IEND
    fwrite(IEND, sizeof(IEND), 1, all);
    fclose(all);

    if (gettimeofday(&tv, NULL) != 0) {
        perror("gettimeofday");
        abort();
    }
    times[1] = (tv.tv_sec) + tv.tv_usec/1000000.;
    printf("paster2 execution time: %.6lf seconds\n", times[1] - times[0]);

    //clean up
    free(AllUCData);
    free(fIDATdata);
    free(fIDATchunk);
    free(mockIDHR);

    //clean up shm
    if ( shmctl(shmid_sem_items, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_sem_spaces, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_sem_buffer, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_sem_count, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_sem_file, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_pindex, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_cindex, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_count, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    if ( shmctl(shmid_consumed, IPC_RMID, NULL) == -1 ) {
        perror("shmctl");
        abort();
    }
    for(int i = 0; i < buffer_size; i++){
        if ( shmctl(shm_buf_ids[i], IPC_RMID, NULL) == -1 ) {
            perror("shmctl");
            abort();
        }
    }
    for(int i = 0; i < 50; i++){
        if ( shmctl(shm_chunk_ids[i], IPC_RMID, NULL) == -1 ) {
            perror("shmctl");
            abort();
        }
    }

    //destroy sems
    sem_destroy(itemSem);
    sem_destroy(spaceSem);
    sem_destroy(bufferMutex);
    sem_destroy(countMutex);
    sem_destroy(chunkMutex);
    sem_destroy(fileMutex);

    return 0;
}