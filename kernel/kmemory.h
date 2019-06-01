#include <pthread.h>
#include <commons/log.h>
#include <stdlib.h>
typedef enum{
    S_CONSISTENCY,
    H_CONSISTENCY,
    ANY_CONSISTENCY
}t_consistency;

typedef struct{
    int id;
    int fd;
    t_consistency consistency;
    pthread_mutex_t lock;
}t_kmemory;

t_consistency getTableConsistency(char* table_name);
int get_loked_memory(t_consistency consistency);
void unlock_memory(int memoryfd);
int getStrongConsistencyMemory();
void start_kmemory_module(t_log* logg, char* main_memory_ip, int main_memoy_port);
int connect_to_memory(char* ip, int port);
