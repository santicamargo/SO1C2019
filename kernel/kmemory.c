#include "kmemory.h"
#include "scheduler.h"
#include <commons/collections/list.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <commons/config.h>
#include <commons/collections/dictionary.h>
#include <commons/string.h>
#include <stdlib.h>
extern scheduler_config* config_not;
extern pthread_mutex_t config_lock;
long MEMORY_FINDER_SLEEP;
pthread_mutex_t sleep_lock;
 t_log* logger;
 t_log* logger_debug;
t_dictionary* tbl_list;   //Lista de tablas (metadata)

pthread_mutex_t mem_list_lock = PTHREAD_MUTEX_INITIALIZER;  //mutex para la lista de memorias
t_list* mem_list;   //Lista de memorias

pthread_mutex_t sc_lock = PTHREAD_MUTEX_INITIALIZER;
t_kmemory* strong_memory; //La memoria SC por default es la principal

pthread_mutex_t hc_lock = PTHREAD_MUTEX_INITIALIZER;
t_list* hc_list;

pthread_mutex_t any_lock = PTHREAD_MUTEX_INITIALIZER;
t_list* any_list;
int last_position = 0;

typedef struct{
    char* name;
    t_consistency consistency;
}t_table;


void start_kmemory_module(t_log* logg,t_log* logg_debug, char* main_memory_ip, int main_memoy_port){
    logger = logg;
    logger_debug = logg_debug;
    mem_list = list_create();
    tbl_list = dictionary_create();
    
    hc_list = list_create();
    any_list = list_create();

    //t_table* table_debug = malloc(sizeof(t_table));

    //DEBUG 
    int* sad = malloc(sizeof(int));
    *sad = 0;
    log_debug(logg_debug, "el valor del puntero es: %d \n", *sad);
    dictionary_put(tbl_list, "A",(void*) sad );
    int* rs = dictionary_get(tbl_list, "A");
    log_debug(logg_debug, "lo que queda luego de ingresarlo %d \n", *rs);
    log_debug(logg_debug, "mismo puntero %d \n", *sad);
    t_kmemory* mp = malloc(sizeof(t_kmemory));
    
    mp->id = 0;
    mp->fd = connect_to_memory(main_memory_ip, main_memoy_port);
    pthread_mutex_init(&mp->lock,NULL);

    if(mp->fd > -1){
        list_add(mem_list, mp);
        strong_memory = mp;
   
        free(ksyscall( strdup("MEMORY")));
        
    }else{
        printf("\r");
        log_error(logger, "kmemory: No Existe memoria principal");
        pthread_mutex_destroy(&mp->lock);
        free(mp);
        //printf("\rkernel>");
        strong_memory = NULL;
    }

    pthread_t tid_metadata_service;
    pthread_create(&tid_metadata_service, NULL,metadata_service, NULL);
    pthread_mutex_init(&sleep_lock, NULL);
    MEMORY_FINDER_SLEEP = 5000;

    pthread_t tid_memory_finder_service;
    pthread_create(&tid_memory_finder_service, NULL,memory_finder_service, NULL);

    log_info(logger_debug, "kmemory: El modulo kmemory fue inicializado exitosamente");


}

int get_loked_memory(t_consistency consistency, char* table_name){
    if(consistency == S_CONSISTENCY){
        log_debug(logger_debug, "kmemory: se pide memoria de SC");
        return get_sc_memory();
    }else if( consistency == H_CONSISTENCY){
        log_debug(logger_debug, "kmemory: se pide memoria de HC");
        return get_hc_memory(table_name);
    }else if (consistency == ANY_CONSISTENCY){
        log_debug(logger_debug, "kmemory: se pide memoria de EVENTUAL");
        return get_any_memory();
    }else if( consistency == ALL_CONSISTENCY){
        return get_memory();
    }else if(consistency == ERR_CONSISTENCY){
        log_error(logger, "No se reconoce la tabla.");
        return -1;
    }else{
        log_error(logger, "Error fatal. El sitema no reconoce la consitencia de la tabla solicitada");
        exit(-1);

    }

    
}

int get_loked_main_memory(){
    pthread_mutex_lock(&mem_list_lock);

    if(list_size(mem_list)<1){
        //si no hay memrias en la lista no hay memoria principal 
        pthread_mutex_unlock(&mem_list_lock);
        return -1;
    }
    t_kmemory* main = list_get(mem_list, 0);
    pthread_mutex_unlock(&mem_list_lock);
    if(main == NULL) return -1;
    pthread_mutex_lock(&main->lock);
    return main->fd;
}

//consegue cualquier memoria
int get_memory(){
    pthread_mutex_lock(&mem_list_lock);
    int list_s = list_size(mem_list);
    pthread_mutex_unlock(&mem_list_lock);

    if(list_s == 0){
        log_error(logger, "No existen memorias añadidas al kernel. Reiniciar proceso.");
        return -1;
    }
    t_kmemory* memory;

    int i = last_position++;
    if(i > list_s-1) i = 0;

    while(i != last_position){
        pthread_mutex_lock(&mem_list_lock);
        t_kmemory* m = list_get(mem_list, i);
        pthread_mutex_unlock(&mem_list_lock);
        int status = pthread_mutex_trylock(&m->lock);
    
        if(status == 0){
            memory = m;
            last_position = i;
            break;
        }else{
            i++;

        }

        pthread_mutex_lock(&mem_list_lock);
        list_s = list_size(mem_list);
        pthread_mutex_unlock(&mem_list_lock);
        if(i>list_s){
            i = 0;
        }
    }
    last_position++;
    if(i>list_s){
            i = 0;
        }

    if(memory == NULL){
        pthread_mutex_lock(&mem_list_lock);
        memory = list_get(mem_list, last_position);
        pthread_mutex_unlock(&mem_list_lock);

        pthread_mutex_lock(&memory->lock);
        return memory->fd;
    }
    return memory->fd;
}

void kmemory_set_active_tables(t_dictionary* dic){
    tbl_list = dic;
}

int get_sc_memory(){

    if(strong_memory == NULL){
        log_error(logger, "kmemory: No hay memoria en el citerio SC");
        return -1;
    }
    log_debug(logger_debug, "kmemory: bloqueo memoria");
    pthread_mutex_lock(&strong_memory->lock);
    return strong_memory->fd;
}

int get_hc_memory(char* table_name){
     pthread_mutex_lock(&hc_lock);
    int list_s = list_size(hc_list);
    pthread_mutex_unlock(&hc_lock);

    if(list_s == 0){
        log_error(logger, "No existen memorias en el criterio.");
        return -1;
    }

    int memory_position = hash(table_name) % (list_s);
 
    pthread_mutex_lock(&hc_lock);
    t_kmemory* memory = list_get(hc_list, memory_position);
    pthread_mutex_unlock(&hc_lock);
    pthread_mutex_lock(&memory->lock);

    return memory->fd;
}

int get_any_memory(){
    
    
    pthread_mutex_lock(&any_lock);
    int list_s = list_size(any_list);
    pthread_mutex_unlock(&any_lock);

    if(list_s == 0){
        log_error(logger, "No existen memorias en el criterio.");
        return -1;
    }
    t_kmemory* memory;

    int i = last_position++;
    if(i > list_s-1) i = 0;


    while(i != last_position){
        pthread_mutex_lock(&any_lock);
        t_kmemory* m = list_get(any_list, i);
        pthread_mutex_unlock(&any_lock);
        int status = pthread_mutex_trylock(&m->lock);

        if(status == 0){
            memory = m;
            last_position = i;
            break;
        }else{
            i++;

        }

        pthread_mutex_lock(&any_lock);
        list_s = list_size(any_list);
        pthread_mutex_unlock(&any_lock);
        if(i>list_s){
            i = 0;
        }
    }
    last_position++;
    if(i>list_s){
            i = 0;
        }

    if(memory == NULL){
        pthread_mutex_lock(&any_lock);
        memory = list_get(any_list, last_position);
        pthread_mutex_unlock(&any_lock);

        pthread_mutex_lock(&memory->lock);
        return memory->fd;
    }
    return memory->fd;

}

void add_memory_to_sc(int id){
    bool find_memory_by_id(void* m){
        t_kmemory* mem = (t_kmemory*) m;
        pthread_mutex_lock(&mem->lock);
        int memId = mem->id;
        pthread_mutex_unlock(&mem->lock);
        if(memId == id){
            return true;

        }
        return false;
    }
    t_kmemory* finded = list_find(mem_list, find_memory_by_id);

    if(finded == NULL){
        log_error(logger, "La memoria no existe. Revise las memorias e intentelo mas tarde");
        return;
    }

    pthread_mutex_lock(&sc_lock);
    strong_memory = finded;
    pthread_mutex_unlock(&sc_lock);

} 



void add_memory_to_hc(int id){

    bool find_memory_by_id(void* m){
        t_kmemory* mem = m;
        pthread_mutex_lock(&mem->lock);
        int memId = mem->id;
        ///printf("%d", memId);
        pthread_mutex_unlock(&mem->lock);
        if(memId == id){
            return true;
        }
        return false;
    }

    pthread_mutex_lock(&mem_list_lock);
    t_kmemory* finded = list_find(mem_list, find_memory_by_id);
    pthread_mutex_unlock(&mem_list_lock);

    if(finded == NULL){
        log_error(logger, "La memoria no existe. Revise las memorias e intentelo mas tarde.");
        return;
    }
    pthread_mutex_lock(&hc_lock);
    t_kmemory* hcs = list_find(hc_list, find_memory_by_id);
    pthread_mutex_unlock(&hc_lock);

    if(hcs != NULL){
        log_error(logger, "La memoria ya se encuentra en el criterio.");
        return;
    }

    void* journal_hc(void* mem){
        t_kmemory* t = (t_kmemory*) mem;
        char* r = malloc(30);
        pthread_mutex_lock(&t->lock);
        write(t->fd,"JOURNAL",8 );
        read(t->fd,r, 30);
        pthread_mutex_unlock(&t->lock);
        free(r);
        return t;
    }
    pthread_mutex_lock(&hc_lock);
    list_add(hc_list, finded);

    list_map(hc_list,journal_hc);
    pthread_mutex_unlock(&hc_lock);


}

void add_memory_to_any(int id){

    bool find_memory_by_id(void* m){
        t_kmemory* mem = m;
        pthread_mutex_lock(&mem->lock);
        int memId = mem->id;
        //printf("%d", memId);
        pthread_mutex_unlock(&mem->lock);
        if(memId == id){
            return true;
        }
        return false;
    }
    t_kmemory* finded = list_find(mem_list, find_memory_by_id);

    if(finded == NULL){
        log_error(logger, "La memoria no existe. Revise las memorias e intentelo mas tarde.");
        return;
    }

    t_kmemory* anys = list_find(any_list, find_memory_by_id);

    if(anys != NULL){
        log_error(logger, "La memoria ya se encuentra en el criterio.");
        return;
    }

    pthread_mutex_lock(&any_lock);
    list_add(any_list, finded);
    pthread_mutex_unlock(&any_lock);


}

int unlock_memory(int memoryfd){
    if(memoryfd < 0) return -1; //si no exite no ago nada
    bool has_memory_fd(void* memory){
    t_kmemory* mem = memory;
       if(mem->fd == memoryfd){
           return true;
       }
       return false;
    }

    pthread_mutex_lock(&mem_list_lock);
    t_kmemory* mem = list_find(mem_list, has_memory_fd);
    
    pthread_mutex_unlock(&mem_list_lock);
    if(mem!=NULL){
        int i = mem->id;
        pthread_mutex_unlock(&mem->lock);
        return i;
    }

}

void check_for_new_memory(char* ip, int port, int id){
    
    for(int i = 0; i < list_size(mem_list); i++){
        pthread_mutex_lock(&mem_list_lock);
        t_kmemory* m = list_get(mem_list, i);
        pthread_mutex_unlock(&mem_list_lock);
        

        if(id == m->id){
            return; 
        }
    }

    int fd = connect_to_memory(ip, port);
    if(fd > 0){
        char* id_request = malloc(3000);
        strcpy(id_request, "");
        write(fd,"MEMORY", 7);
        read(fd, id_request, 3000);
        char* id_buffer = malloc(27);
        strcpy(id_buffer,"");
        for(int i = 0; i < strlen(id_request);i++){
            if(id_request[i]=='|') break;
            id_buffer[i] = id_request[i];
            id_buffer[i+1] = '\0';
        }
        int mem_id = atoi(id_buffer);

        free(id_buffer);
        free(id_request);
        
        
        t_kmemory* memory = malloc(sizeof(t_kmemory));
        memory->id = mem_id;
        memory->fd = fd;

        pthread_mutex_init(&memory->lock, NULL);
        pthread_mutex_lock(&mem_list_lock);
        list_add(mem_list, memory);
        pthread_mutex_unlock(&mem_list_lock);
        printf("\r");
        log_info(logger, "kmemory: se añadio una nueva memoria. id:%d", mem_id);
        printf("kernel>");
        fflush(stdout);
    }
    
    

}

void remove_form_any(int id){}
void remove_from_sc(int id){}
void remove_from_hc(int id){

}
void remove_memory_from_list(int fd, t_list* list, pthread_mutex_t* lock){

     if(fd < 0) return; //si no exite no ago nada

    bool has_memory_fd(void* memory){
    t_kmemory* mem = memory;
       if(mem->fd == fd){
           return true;
       }
       return false;
    }

    

    pthread_mutex_lock(lock);
    int i = 0; 
    for(int i= 0;i<list_size(mem_list);i++){
        t_kmemory* mem = list_get(list, i);
        if(mem->fd == fd){
           list_remove(list, i); 
           pthread_mutex_destroy( &mem->lock);
           free(mem);
        } 
    }
    
    pthread_mutex_unlock(lock);
    //log_error(logger, "kmemory: se desconecto una memoria.");
}

void disconect_from_memory(int memoryfd){
    if(strong_memory!=NULL){
        if(strong_memory->fd==memoryfd){ 
                strong_memory=NULL;
            }
    }
    remove_memory_from_list(memoryfd, mem_list, &mem_list_lock);
    remove_memory_from_list(memoryfd, hc_list, &hc_lock);
    
    

    log_info(logger, "kmemory: se desconecto una memoria.");
}

int connect_to_memory(char* ip, int port){

    int memoryfd = socket(AF_INET, SOCK_STREAM, 0); 
    struct sockaddr_in sock_client;
    sock_client.sin_family = AF_INET; 
    sock_client.sin_addr.s_addr = inet_addr(ip); 
    sock_client.sin_port = htons( port );
    //fcntl(memoryfd, F_SETFL, O_NONBLOCK);


    
    int conection_result = connect(memoryfd, (struct sockaddr*)&sock_client, sizeof(sock_client));
// printf("\033[<500>C");
    if(conection_result<0){
        close(memoryfd);

        printf("\r");
        log_error(logger, "kmemory: No se logro establecer coneccion con una memoria");
        printf("kernel>");
        fflush(stdout);
      return -1;
    }
    return memoryfd;
}

void kmemory_add_table(char* name, t_consistency* cons){
    dictionary_put(tbl_list, name, cons);
}

t_consistency get_table_consistency(char* table_name){

    if(!dictionary_has_key(tbl_list, table_name)){
       exec_err_abort(); //cuano la 
        return ERR_CONSISTENCY;
    }
    int r =*((int *) dictionary_get(tbl_list, table_name));
    
    switch (r)
    {
    case 0: return S_CONSISTENCY; break;
    case 1: return H_CONSISTENCY; break;
    case 2: return ANY_CONSISTENCY; break;
    default: return ERR_CONSISTENCY; break;
    }
    
    
    return ERR_CONSISTENCY;
}

void update_memory_finder_service_time(long time){
    pthread_mutex_lock(&sleep_lock);
    MEMORY_FINDER_SLEEP = time;
    pthread_mutex_unlock(&sleep_lock);
}

void *metadata_service(void* args){
    while(true){
        pthread_mutex_lock(&config_lock);
        long sleep_interval = config_not->metadata_refresh * 1000;
        pthread_mutex_unlock(&config_lock);
        usleep( sleep_interval);
        //log_debug(logger, "metadataService: Se actualiza la metadata de las tablas");
        char* r = ksyscall(strdup("DESCRIBE"));
        log_debug(logger, r);
        free(r);
        
        
    }
    
}

void *memory_finder_service(void* args){
    while(true){
        pthread_mutex_lock(&sleep_lock);
        long sleep = MEMORY_FINDER_SLEEP;
        pthread_mutex_unlock(&sleep_lock);
        usleep( sleep);
        log_debug(logger_debug, "kmemoryService: Se actualiza las memorias");
        char* r = ksyscall(strdup("MEMORY"));
        free(r);
    }
}

void kmemoy_add_table(char* tbl_name, t_consistency c){
    t_consistency* cons = malloc(sizeof(t_consistency));
    *cons = c;
    dictionary_put(tbl_list, tbl_name, cons);
}


void kmemory_drop_table(char* tbl_name){
    if(dictionary_has_key(tbl_list, tbl_name)){
        t_consistency* cons = dictionary_remove(tbl_list, tbl_name);
        free(cons);
    }
}

int hash(char* string){
    int hashr = 0;
    int i = 0;
    while(string[i]){
        char c = string[i];
        hashr += c*(i+1) >> 3;
        i++;
    }
    return hashr;
}

void set_main_memory_id(int id){
    log_info(logger_debug, "el id de la memoria principal es: %d", id);
    pthread_mutex_lock(&mem_list_lock);
    ((t_kmemory*) list_get(mem_list, 0))->id = id;
    pthread_mutex_unlock(&mem_list_lock);
}