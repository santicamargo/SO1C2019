#include "engine.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fts.h>
#include <commons/bitarray.h>
#include "filesystem.h"
#include <sys/inotify.h>
 
#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define EVENT_BUF_LEN     ( 1024 * ( EVENT_SIZE + 16 ) )
#define BLOCK_SIZE_DEFAULT 128
#define BLOCKS_AMOUNT_DEFAULT 12
char* MNT_POINT;
t_log* logg;
t_list* tables;
char* tables_path;
DIR* root;
char* bitmap_path;
int block_amount;
int block_size;
int row_amount;
long config_tiempo_dump;
long config_retardo;
t_config* config;

pthread_mutex_t config_lock;
void check_or_create_dir(char* path){
    DIR* dir = opendir(path);
    if (dir != NULL) {
        closedir(dir);
    } else{
       int status = mkdir(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        log_info(logg, "se crea: %s", path);
        if(status!=0){
            log_error(logg, "Fatal error. No se puede escribir en el directorio. root?");
            exit(-1);
        }
 
    }
 
}
 
void check_or_create_file(char* path){
    FILE* file = fopen(path,"r");
    if(file==NULL){
        file = fopen(path,"w");
        log_info(logg, "se crea: %s", path);
    }
 
    fclose(file);
}
 
int recursive_delete(const char *dir){
    int ret = 0;
    FTS *ftsp = NULL;
    FTSENT *curr;
 
    // Cast needed (in C) because fts_open() takes a "char * const *", instead
    // of a "const char * const *", which is only allowed in C++. fts_open()
    // does not modify the argument.
    char *files[] = { (char *) dir, NULL };
 
    // FTS_NOCHDIR  - Avoid changing cwd, which could cause unexpected behavior
    //                in multithreaded programs
    // FTS_PHYSICAL - Don't follow symlinks. Prevents deletion of files outside
    //                of the specified directory
    // FTS_XDEV     - Don't cross filesystem boundaries
    ftsp = fts_open(files, FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, NULL);
    if (!ftsp) {
        fprintf(stderr, "%s: fts_open failed: %s\n", dir, strerror(errno));
        ret = -1;
        goto finish;
    }
 
    while ((curr = fts_read(ftsp))) {
        switch (curr->fts_info) {
        case FTS_NS:
        case FTS_DNR:
        case FTS_ERR:
            fprintf(stderr, "%s: fts_read error: %s\n",
                    curr->fts_accpath, strerror(curr->fts_errno));
            break;
 
        case FTS_DC:
        case FTS_DOT:
        case FTS_NSOK:
            // Not reached unless FTS_LOGICAL, FTS_SEEDOT, or FTS_NOSTAT were
            // passed to fts_open()
            break;
 
        case FTS_D:
            // Do nothing. Need depth-first search, so directories are deleted
            // in FTS_DP
            break;
 
        case FTS_DP:
        case FTS_F:
        case FTS_SL:
        case FTS_SLNONE:
        case FTS_DEFAULT:
            if (remove(curr->fts_accpath) < 0) {
                fprintf(stderr, "%s: Failed to remove: %s\n",
                        curr->fts_path, strerror(errno));
                ret = -1;
            }
            break;
        }
    }
 
    finish:
        if (ftsp) {
            fts_close(ftsp);
        }
 
        return ret;
}
 
void engine_start(t_log* logger){
 
    logg = logger; //CHECK
    config = config_create("config");
    MNT_POINT = config_get_string_value(config, "PUNTO_MONTAJE"); //CHECK
    int TAM_VALUE = config_get_int_value(config, "TAMAÑO_VALUE");
    DIR* mnt_dir = opendir(MNT_POINT); 
    tables = list_create();

    update_engine_config();
    pthread_mutex_init(&config_lock, NULL);
 
    pthread_t tid;
    pthread_create(&tid, NULL, config_worker, NULL);
 
    if(mnt_dir == NULL){
        log_error(logger, "Fatal error. El punto de montaje es invalido.");
        exit(-1);
    }else{
        closedir(mnt_dir);
    }
 
    //Armo los directorios
    char* metadata_dir_path = malloc(strlen(MNT_POINT)+strlen("Metadata/")+1);
    strcpy(metadata_dir_path, MNT_POINT);
    strcat(metadata_dir_path, "Metadata/");
    tables_path = malloc(strlen(MNT_POINT)+strlen("Tables/")+1);
    strcpy( tables_path,MNT_POINT);
    strcat(tables_path, "Tables/");
    char* blocks_path = malloc(strlen(MNT_POINT)+strlen("Bloques/")+1);
    strcpy(blocks_path , MNT_POINT);
    strcat(blocks_path, "Bloques/");
   
 
    check_or_create_dir(metadata_dir_path);
    check_or_create_dir(blocks_path);
    check_or_create_dir(tables_path);
 
 
    //Creo el archivo Metadata/Bitmap.bin
    bitmap_path = malloc(strlen(metadata_dir_path)+strlen("bitmap")+1);
    strcpy(bitmap_path,metadata_dir_path);
    strcat(bitmap_path,"bitmap");
   
    //consigo el directorio metadata
    char* meta_path = malloc(strlen(metadata_dir_path)+strlen("Metadata.bin")+1);
    strcpy(meta_path ,metadata_dir_path);
    strcat(meta_path, "Metadata.bin");
 
    FILE* meta = fopen(meta_path, "r");
    //creo el archivo Metadata/Metadata.bin
    if(meta == NULL){
        meta = fopen(meta_path, "w");
        log_info(logg, "Se crea: %s", meta_path);
        char* text = "BLOCKS=%s\nBLOCK_SIZE=%s\nMAGIC_NUMBER=LISSANDRA\n";
        char* a = string_itoa(BLOCKS_AMOUNT_DEFAULT);
        char* b = string_itoa(BLOCK_SIZE_DEFAULT);
        char* r = malloc( strlen(text) + strlen(a) + strlen(b)+1);
 
        sprintf(r, text, a,b);
 
        fputs(r, meta);
 
        fclose(meta);
 
        free(r);
 
    } else {
        fclose(meta);
    }
 
   
    t_config* meta_config = config_create(meta_path);
    block_amount = config_get_int_value(meta_config, "BLOCKS"); //CHECK
    block_size = config_get_int_value(meta_config, "BLOCK_SIZE"); //CHECK
    row_amount = block_size/(5 + 1 + 2 + TAM_VALUE  );
    FILE* bitmap = fopen(bitmap_path,"r+");
    if(bitmap==NULL){
 
        printf("------------cantidad de bloques---------------\n");
        printf("%d\n", block_amount);
        printf("---------------------------------------\n");
 
        FILE* bitmap = fopen(bitmap_path,"w");
        char* bitearray;
        bitearray = string_repeat( '0', block_amount);
 
        printf("------------El bitmap es---------------\n");
        printf("%s\n", bitearray);
        printf("---------------------------------------\n");
 
        fputs(bitearray, bitmap);
        fclose(bitmap);
 
    }else{
        char* buff = malloc(3000);
        fgets(buff, 3000, bitmap);
        if(strlen(buff)<block_amount){
            char* bits = string_repeat('0', block_amount-strlen(buff));
            fclose(bitmap);
            bitmap = fopen(bitmap_path,"w");

            char* file_buff = malloc(strlen(buff) + strlen(bits) +10);
            strcpy(file_buff, buff);
            strcat(file_buff, bits);

            fputs(file_buff, bitmap);
        }
        if(strlen(buff))
        fclose(bitmap);

        free(buff);
    }
 
    for(int i = 0; i < block_amount;i++){
        char* p = malloc(strlen(blocks_path)+strlen(string_itoa(block_amount))+5);
        strcpy(p, blocks_path);
        strcat(p, string_itoa(i));
        strcat(p, ".bin");
        check_or_create_file(p); //CHECK
        free(p);
    }
 
    load_dir();
    pthread_t tid2;
    pthread_create(&tid2, NULL, dir_wacher, NULL);
    //config_destroy(config); -> si hago destroy pierdo el punto de montaje que uso en engine_dump_table
    config_destroy(meta_config);
    
}
 

void load_dir(){
    DIR* tables_dir = opendir(tables_path); 
    struct dirent *entry;
    while ((entry = readdir(tables_dir)) != NULL) {
        if(entry->d_type == DT_DIR){
            char* old = malloc(strlen(tables_path) + strlen(entry->d_name)+7);
            char* new = malloc(strlen(tables_path) + strlen(entry->d_name)+7);

            strcpy(old, tables_path);
            strcat(old, entry->d_name);

            string_to_upper(entry->d_name);
            strcpy(new, tables_path);
            strcat(new, entry->d_name);

            rename(old, new);

            free(old);
            free(new);
    

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

            
            t_table_metadata* meta = get_table_metadata(entry->d_name);
            if(meta==NULL) continue;
            //printf("pase y ");
            int cond = 0;
            for(int i = 0; i < meta->partition_number;i++){
                t_table_partiton* p = get_table_partition(entry->d_name, i);
                if(p==NULL){
                    cond=1;
                    break;
                }

                free(p);
            }

            if(cond==1) continue;

            if(does_table_exist(entry->d_name)) continue;
            t_table* table;
            table = malloc(sizeof(t_table));

            char* name = malloc(strlen(entry->d_name) +1); //CHECK
            strcpy(name, entry->d_name);
            table->name = name;
            table->compactating = 0;
            table->cond = malloc(sizeof(pthread_cond_t));

            table->compactation_time = meta->compactation_time;
            pthread_mutex_init(&table->lock,NULL);
            pthread_cond_init(table->cond,NULL);
            pthread_t tid;
            table->tid = tid;
            pthread_create(&tid, NULL, compactation_worker, (void*) table);
            //printf("Se detecto talba:%s\n", name);
            list_add(tables, table);
            string_to_upper(name);
            log_info(logg,name);
            
        }
    }
    

    closedir(tables_dir);
 
} 
int does_table_exist(char* table_name){
    char* q = strdup(table_name);
    string_to_upper(q);
 
    bool findTableByName(void* t){
         t_table* tt = (t_table*) t;
        char* cmp = strdup(tt->name);
        string_to_upper(cmp);
        if(!strcmp(q, cmp)){
            free(cmp);
            return true;
        }
        free(cmp);
        return false;
    }

    char* t = list_find(tables,findTableByName);
    //free(q);
    if(t == NULL){
        return 0;
    }
    return 1;
}

void* dir_wacher(void* args){

    char* buf = malloc(EVENT_BUF_LEN);
    while(1){
        usleep(990000);
        load_dir();
    }
   
}
 
int enginet_create_table(char* table_name, int consistency, int particiones, long compactation_time){
   
    //creo la carpeta
    int path_len = strlen(tables_path) + strlen(table_name) +7;
    char* path_dir =  malloc(path_len);
 
    strcpy(path_dir, tables_path);
    strcat(path_dir, table_name);
   
    log_error(logg, tables_path);
 
    check_or_create_dir(path_dir);
   
    //crea la carpeta ahora hay que rellenarla
 
 
    FILE * metadata;
    char* meta_path= malloc( strlen(path_dir) + strlen("/metadata")+1);
    strcpy(meta_path, path_dir);
    strcat(meta_path,"/metadata");
   
    metadata=fopen(meta_path,"w+");
 
    if(metadata==NULL){
        log_error(logg, "Error al crear la tabla. Root?");
        exit(-1);
    }
 
    char* constante = strdup(string_itoa(particiones));
 
    int util=strlen(constante)+strlen("particiones");
    char* partitions=malloc(util +4);
    strcpy(partitions,"\n");
    strcpy(partitions,"PARTICIONES=");
    strcat(partitions,constante);
    free(constante);
 
    log_info(logg,partitions);
    char* constante2= strdup(string_itoa(compactation_time));
    int util2=strlen(constante2)+strlen("\nCOMPACTATION");
    char* compactation= malloc(util2+3);
    strcpy(compactation,"\n");
    strcpy(compactation,"\nCOMPACTATION=");
    strcat(compactation,constante2);
    free(constante2);
 
    log_info(logg,compactation);
    char* constante3= strdup(string_itoa(consistency));
    char* consistencia;
    int larg0=strlen("sc=consistency\n");
    consistencia= malloc(larg0+5);
 
    switch(atoi(constante3)){
    case 0:
        strcpy(consistencia,"CONSISTENCY=0\n");
        break;
    case 1:
        strcpy(consistencia,"CONSISTENCY=1\n");
        break;
    case 2:
        strcpy(consistencia,"CONSISTENCY=2\n");
        break;
    }
    free(constante3);
 
    log_info(logg,consistencia);
    fputs(consistencia,metadata);
    fputs(partitions,metadata);
    fputs(compactation,metadata);
    int c= particiones;
    c--;
 
    char* resp=malloc(1000);
    char* metadata_template = "SIZE=0\nBLOCKS=[]";
 
    while(c>=0){
        char* auxx=NULL;
        auxx=strdup(string_itoa(c));
        strcpy(resp, path_dir);
        strcat(resp,"/");
        strcat(resp,auxx);
        strcat(resp,".part");
        log_info(logg,resp);
        FILE* part = fopen(resp,"w+");
        fputs(metadata_template, part);
        fclose(part);
        free(auxx);
        c--;
    }
   
    free(resp);

    t_table* new_table = malloc( sizeof(t_table));
    new_table->name = table_name;
    new_table->cond = malloc( sizeof(pthread_cond_t));
    pthread_mutex_init(&new_table->lock, NULL);
    pthread_cond_init(new_table->cond, NULL);
    new_table->compactating = 0;
    new_table->compactation_time = compactation_time;
    pthread_t tid;
    new_table->tid = tid;
    pthread_create(&tid, NULL, compactation_worker, (void*) new_table);
    list_add(tables, new_table);
    char* copy = strdup(table_name);
    string_to_upper(copy);
 
    fclose(metadata);
    free(path_dir);
    free(partitions);
    free(consistencia);
    free(compactation);
    free(meta_path);
 
}
 
t_table* get_table(char* q){
    
    bool findTableByName(void* t){
        t_table* tt = (t_table*) t;
        if(!strcmp(q, tt->name)){
            return true;
        }
    }
    t_table* tbl = list_find(tables, findTableByName);
    return tbl;
}
void engine_drop_table(char* table_name){
   char* ruta=malloc(200);
   strcpy(ruta,tables_path);
   strcat(ruta,table_name);
   DIR* t_drop=NULL;
   t_drop=opendir(ruta);
   if(t_drop==NULL){
       free(ruta);
       log_error(logg, "No se encontro la tabla");
       closedir(t_drop);
       return;
   }
    t_table_metadata* metadata = get_table_metadata(table_name);
    int contador = 0;
    char* temp = malloc(100);

    t_table* tt = get_table(table_name);
    pthread_cancel(tt->tid);
    while(contador < metadata->partition_number){
        t_table_partiton* partition = get_table_partition(table_name, contador);
        free_part(partition, contador, table_name);
        contador++;
    }

    recursive_delete(ruta);

    free(temp);
    free(ruta);
    pthread_mutex_destroy(&tt->lock);
    pthread_cond_destroy(tt->cond);
    closedir(t_drop);
    //free(tt);
    return;

    }
/*
    list_remove_by_condition(tables,findTableByName);
    if(tbl!=NULL){
        pthread_mutex_destroy(&tbl->lock);
        pthread_cond_destroy(tbl->cond);
        pthread_cancel(tbl->tid);
    }
    char* path = malloc(strlen(tables_path) + strlen(table_name) + 5);
    strcpy(path, tables_path);
    strcat(path, table_name);
    recursive_delete(path);


 
    free(q);
    free(path);
    return;
    }
*/ 

 
char* get_table_metadata_as_string(char* table_name){
 
    char* table_path = malloc( strlen(table_name) + strlen(tables_path) + strlen("/metadata") +1);
   
    strcpy(table_path, tables_path);
    strcat(table_path, table_name);
    strcat(table_path, "/metadata");
 
    FILE* f = fopen(table_path, "r");
    if(f == NULL) return "";
    fseek(f, 0L, SEEK_END);
    int bytes = ftell(f);
 
    fseek(f, 0l, SEEK_SET);
    char* meta = (char*)calloc(bytes, sizeof(char));   
    fread(meta, sizeof(char), bytes, f);
    printf("metadata de:%s", table_name);
    fclose(f);
    free(table_path);
 
    return meta;
}
 
char* get_all_tables_metadata_as_string(){

    if(list_is_empty(tables)) return strdup("");
    int tables_amount = list_size(tables);

    char* result = strdup("");
 
    for (size_t i = 0; i < tables_amount; i++)
    {   

        result = realloc(result, strlen(result) + strlen(((t_table*)list_get(tables,i))->name)+ 8);
        strcat(result, "NOMBRE=");
        strcat(result, ((t_table*) list_get(tables,i))->name);

        result = realloc(result, strlen(result) + 2);
        strcat(result, "\n");
    
        char* m = get_table_metadata_as_string(((t_table*) list_get(tables,i))->name);
        printf("recivo m:%s\n", m);
        result = realloc(result, strlen(result) + strlen(m) + 1);
        strcat(result, m);
        result = realloc(result, strlen(result) + 4);
        strcat(result, ";\n\n");
 
        free(m);
    }
 
    return result;
}
 
t_table_partiton* get_table_partition(char* table_name, int table_partition_number){
    char* partition_name = strdup(string_itoa(table_partition_number));
    char* partition_path =malloc(strlen(tables_path) + strlen(table_name) + 1 + strlen(partition_name)+ strlen(".part") + 5);
    strcpy(partition_path ,tables_path);
 
    strcat(partition_path,table_name);
    strcat(partition_path,"/");
    strcat(partition_path, partition_name);
    strcat(partition_path, ".part");
 
    free(partition_name);
 
    FILE* test = fopen(partition_path, "r");
    if(test==NULL){
        free(partition_path);
        return NULL;
    } 
    fclose(test);
    t_table_partiton* parition = malloc(sizeof(t_table_partiton));
    t_config* c = config_create(partition_path);
    if(!config_has_property(c,"SIZE")||!config_has_property(c,"BLOCKS")) return NULL;

    parition->blocks_size = config_get_long_value(c, "SIZE");
 
    parition->blocks = config_get_array_value(c, "BLOCKS");
    config_destroy(c);
    free(partition_path);
 
    return parition;
}

void* config_worker(void* args){
    int inotifyFd = inotify_init();
    inotify_add_watch(inotifyFd, "config", IN_CLOSE_WRITE);
    char* buf = malloc(EVENT_BUF_LEN);
    while(1){
        int length = read(inotifyFd, buf, EVENT_BUF_LEN);
 
         if ( length < 0 ) {
            perror( "Error en config" );
        }  
 
        struct inotify_event *event = (struct inotify_event *) buf;
        if(event->mask == IN_CLOSE_WRITE){
        log_info(logg, "Se actualiza la info del config");
        //config_destroy(config);
        config = config_create("config");
        update_engine_config();
 
        }
    }
   
}


void* del_wacher(void* args){
    int inotifyFd = inotify_init();
    inotify_add_watch(inotifyFd, tables_path, IN_DELETE);
    char* buf = malloc(EVENT_BUF_LEN);
    while(1){
        int length = read(inotifyFd, buf, EVENT_BUF_LEN);
 
         if ( length < 0 ) {
            perror( "Error en config" );
        }  
 
        struct inotify_event *event = (struct inotify_event *) buf;
        if(event->mask == IN_DELETE){
        //log_info(logg, "se borro un tablaaaaaaa");
        //config_destroy(config);
            printf("nombre:%s\n", event->name);

            bool table_has_name(void* a ){
                t_table* t = (t_table*) a;
                if(t->name==event->name ) {
                    return true;
                }
                return false;
            }
//            list_remove_by_condition(tables, table_has_name);
        }
    }
   
}

//leo y actualico la informaion del config;
void update_engine_config(){
         
        int dump = config_get_int_value(config, "TIEMPO_DUMP");
        long retardo = config_get_long_value(config, "RETARDO");
        pthread_mutex_lock(&config_lock);
        config_tiempo_dump = dump;
        config_retardo = retardo;
        pthread_mutex_unlock(&config_lock);
}
 
t_table_metadata* get_table_metadata(char* table_name){
    t_table_metadata* meta = malloc(sizeof(t_table_metadata));
    char* meta_path = malloc(strlen(tables_path) + strlen(table_name) + strlen("/metadata") + 1);
    strcpy(meta_path, tables_path);
    strcat(meta_path, table_name);
    strcat(meta_path, "/metadata");
 
    //log_info(logg, meta_path);
    FILE* test = fopen(meta_path, "r");
    if(test == NULL){
        free(meta);
        free(meta_path);
        return NULL;
    }

    fclose(test);
    t_config* c = config_create(meta_path);
    free(meta_path);
    if(!config_has_property(c,"CONSISTENCY")||!config_has_property(c,"PARTICIONES")||!config_has_property(c,"COMPACTATION")) return NULL;
    meta->consistency = config_get_int_value(c, "CONSISTENCY");
    meta->partition_number = config_get_int_value(c, "PARTICIONES");
    meta->compactation_time = config_get_long_value(c, "COMPACTATION");
    config_destroy(c);
    //log_error(logg,"la consistencia %d", meta->consistency);
    //log_error(logg, "numero de particiones %d", meta->partition_number);
    //log_error(logg, "comapctation %dl",meta->compactation_time);
 
 
    return meta;
}
 
void* compactation_worker(void* args){
    t_table* table = (t_table*) args;
    printf("se inicia un thread\n");
    while (1)
    {
        usleep(table->compactation_time/1000);
        t_table_compactation_args_function* y =  engine_preparate_compactation(table->name);
        if(y!=NULL){
            pthread_mutex_lock(&table->lock);
            engine_compactate(y);
            pthread_mutex_unlock(&table->lock);
        }else{
            //printf("nada por aqui\n");

        }
        

    }
    
}

char* get_blocksize_table_rows(char* table_data){
    char* buffer = malloc(block_size);
    char** rows = string_split(table_data, "\n");
    strcpy(buffer, "");
 
    int buffer_size = 0;
    while(*rows){
 
        if((buffer_size+strlen(*rows)+1)>=block_size && buffer_size!=0){
            break;
        }
 
        strcat(buffer, *rows);
        strcat(buffer, "\n");
 
        buffer_size += strlen(*rows) + 1;
 
        rows++;
 
   
    }
    strcpy(table_data, "");
 
 
    while (*rows)
    {
        strcat(table_data,*rows);
        strcat(table_data, "\n");
       
        rows++;
    }
    return buffer;
 
}
 
void engine_dump_table(char* table_name, char* table_dump){ //esta funcion tiene que tomar el dump de cada table y llevarla al fs
 
    if(!does_table_exist(table_name)){ //chequeo que la tabla existe
        log_error(logg,"La tabla no existe");
        exit(-1);
    }
    char* blocks = NULL;
    int dump_size = strlen(table_dump);
 
    while(table_dump[0] != '\0'){
        printf("el table dump antes del dp: %s\n", table_dump);
        char* r = get_blocksize_table_rows(table_dump);
        printf("table dump luego: %s\n", table_dump);
         //agrego los datos a uno o mas bloques -> ver bitmap
        int block = find_free_block(); //elijo un bloque libre
 
        if (block == -1) {
            log_error(logg,"No hay bloques libres");
            exit(-1);
        }
 
        char* block_name = string_itoa(block);
        char* block_path = malloc(strlen(block_name)+strlen(MNT_POINT)+strlen("Bloques/")+strlen(".bin")+1);
       
        block_path[0] = ('\0');
        strcat(block_path ,MNT_POINT);
        strcat(block_path ,"Bloques/");
        strcat(block_path ,block_name);
        strcat(block_path ,".bin");
 
        //escribo el dump en el bloque
        FILE* block_file = fopen(block_path,"r+");
 
        fwrite(r,strlen(r),1,block_file);
 
        fclose(block_file);
        free(block_path);
 
        set_block_as_occupied(block);//marco el bloque como ocupado en el bitmap
       
        if(blocks == NULL){
            blocks = strdup(string_itoa(block));
 
        }else{
            char* blocks_buffer = malloc(strlen(blocks) + strlen(string_itoa(block))+1);
            strcpy(blocks_buffer, blocks );
            strcat(blocks_buffer, ",");
            strcat(blocks_buffer, string_itoa(block));
            free(blocks);
            blocks = blocks_buffer;
        }
 
        dump_size++; //por cada iteracion se agrega un \0
 
    }
 
    //creo los archivos tmp -> ver como nombro los archivos
    //chequeo si existen archivos con el nombre 0.tmp , 1.tmp, 2.tmp, etc... hasta encontrar uno que no exista
    char* tmp_path = malloc(strlen(MNT_POINT)+strlen("Tables/")+strlen(table_name)+strlen("/.tmp")+1);
   
    tmp_path[0] = '\0';
    strcat(tmp_path ,MNT_POINT);
    strcat(tmp_path ,"Tables/");
    strcat(tmp_path ,table_name);
    strcat(tmp_path ,"/");
 
   
    //encuentro un archivo tmp: desde 0 en adelante itero hasta que no existe salgo y devuelvo el num
    char* tmp_file_number = string_itoa(find_tmp_name(tmp_path));
 
    char* tmp_filepath = malloc( strlen(tmp_path) + strlen(tmp_file_number) + strlen(".tmp") + 1);
    strcpy(tmp_filepath, tmp_path);
    strcat(tmp_filepath ,tmp_file_number);
    strcat(tmp_filepath ,".tmp");
 
    //cargo el archivo .tmp
 
    char* text = "SIZE=%s\nBLOCKS=[%s]\n";
    char* a = string_itoa(dump_size); //TODO ver que es size
    char* r = malloc( strlen(text) + strlen(a) + strlen(blocks)+1);
 
    sprintf(r, text, a,blocks);
 
 
    FILE* tmp_file = fopen(tmp_filepath,"w");//creo el archivo .tmp
 
 
   
    fputs(r, tmp_file); //aca da seg fault
 
 
    fclose(tmp_file);
    free(tmp_filepath);
    free(r);
    free(table_dump);
    free(blocks);
    free(tmp_path);
 
 
   
    return;
}
 
int max_row_amount(){
    return row_amount;
}
//encuentro un archivo tmp: desde 0 en adelante itero hasta que no existe salgo y devuelvo el num
int find_tmp_name(char* tmp_path) {
    int found = 0;
    int i = 0;
    while(1) {
        //genero el nombre
        char* tmp_name = string_itoa(i);
        char* tmp_filepath = malloc( strlen(tmp_path) + strlen(tmp_name)+ strlen(".tmp")+1);
        strcpy(tmp_filepath, tmp_path);
        //strcat(tmp_filepath, tmp_path);
        strcat(tmp_filepath ,tmp_name);
        strcat(tmp_filepath ,".tmp");
 
        //printf("\n============================\n");
        //printf("%s\n", tmp_filepath);
        //printf("============================\n");
 
        if(!does_file_exist(tmp_filepath)){
            //printf("encontre un nombre para tmp\n");
            free(tmp_filepath);
            return i;
        }
 
        i++;
    }
}
 
int find_free_block() {
 
    //printf("El bloque %s\n", bitmap_path);
    FILE* bitmap_file = fopen(bitmap_path,"r+");
    char* bitmap = malloc(block_amount+2);
 
    //printf("cantidad de bloques: %d",block_amount);
   
 
    fread(bitmap ,sizeof(char) ,block_amount ,bitmap_file);
    fclose(bitmap_file);
    for(int i = 0; i<block_amount ;i++) {
        if(bitmap[i]=='0'){ //recorro todo el bitmap buscando un 0
            free(bitmap);
            return i; //devuelvo el indice del primer bloque libre que encuentro -> es el numero de bloque libre
        }
    }
 
    //printf("-------------------NO HAY BLOQUES LIBRES-----------------------\n");
    //printf("%s",bitmap);
    //printf("---------------------------------------------------------------\n");
   
 
    free(bitmap);
   // log_error(logg,"No hay bloques libres");
    //exit(-1); //ver bien que hacer cuando no hay bloques libres
    return -1;
}
 
void set_block_as_occupied(int block_number) {
         
    FILE* bitmap_file = fopen(bitmap_path,"r+");
    char *bitmap = malloc(block_amount+2);
    fread(bitmap, sizeof(char),block_amount, bitmap_file);
    fseek(bitmap_file, 0, SEEK_SET);
    bitmap[block_number] = '1';
    fwrite(bitmap, sizeof(char), block_amount, bitmap_file);
    fclose(bitmap_file);
    free(bitmap);
    return;
}
 
void set_block_as_free(int block_number) {
         
    FILE* bitmap_file = fopen(bitmap_path,"r+");
    char *bitmap = malloc(block_amount+2);
    fread(bitmap, sizeof(char),block_amount, bitmap_file);
    fseek(bitmap_file, 0, SEEK_SET);
    bitmap[block_number] = '0';
    fwrite(bitmap, sizeof(char), block_amount, bitmap_file);
    fclose(bitmap_file);
    free(bitmap);
    return;
}
 
int does_file_exist(char* file_path){
   
    FILE* file = fopen(file_path, "r");
   
    if(file == NULL){
        return 0;
    }
 
    fclose(file);
    return 1;
}
 
t_table_partiton* get_table_partition2(char* table_name, int table_partition_number){
    char* partition_name = strdup(string_itoa(table_partition_number));
    char* partition_path =malloc(strlen(tables_path) + strlen(table_name) + 1 + strlen(partition_name)+ strlen(".temp") + 5);
    strcpy(partition_path ,tables_path);
 
    strcat(partition_path,table_name);
    strcat(partition_path,"/");
    strcat(partition_path, partition_name);
    strcat(partition_path, ".tmp");
 
    t_table_partiton* parition = malloc(sizeof(t_table_partiton));
    t_config* c = config_create(partition_path);
    //printf("assasd %s\n", partition_path);
    //log_info(logg,"antes de la linea");
    parition->blocks_size = config_get_long_value(c, "SIZE");
    //log_info(logg,"despues de la linea");
    parition->blocks = config_get_array_value(c, "BLOCKS");
    config_destroy(c);
   
    return parition;
}
 
 t_table_partiton* get_table_partition3(char* table_name, int table_partition_number){
    char* partition_name = string_itoa(table_partition_number);
    char* partition_path =malloc(strlen(tables_path) + strlen(table_name) + 1 + strlen(partition_name)+ strlen(".temp") + 5);
    strcpy(partition_path ,tables_path);
 
    strcat(partition_path,table_name);
    strcat(partition_path,"/");
    strcat(partition_path, partition_name);
    strcat(partition_path, ".tmpc");

    free(partition_name);
 
    t_table_partiton* parition = malloc(sizeof(t_table_partiton));
    t_config* c = config_create(partition_path);
    parition->blocks_size = config_get_long_value(c, "SIZE");
    parition->blocks = config_get_array_value(c, "BLOCKS");
    config_destroy(c);

   free(partition_path);
    return parition;
}

t_table_compactation_args_function* engine_preparate_compactation(char* name_table){
    char* ruta=malloc(strlen(tables_path) + strlen(name_table) + 30);
    strcpy(ruta,tables_path);
    strcat(ruta,name_table);
    DIR* tablaDir=opendir(ruta);
    int cantidad=contadordetemp(tablaDir);
    //printf("cantidad:%d\n", cantidad);
    if(cantidad==0){
        closedir(tablaDir);
        free(ruta);
        return NULL;
    }
    char* temporales[cantidad];
 
    int contador=0;
    struct dirent * file;

    while((file= readdir(tablaDir))!=NULL ){
        int len= strlen(file->d_name);
        if(file->d_name[len-1]=='p'){
           temporales[contador]= strdup(file->d_name);
            contador++;
        }

        // free(file->d_name);
    }
    closedir(tablaDir);
    int contador_rename=0;
    
    
    while(contador>contador_rename){
        char* ruta2=malloc(200);
        strcpy(ruta2,ruta);
        strcat(ruta2,"/");
        strcat(ruta2,temporales[contador_rename]);
        char* new=malloc(strlen(temporales[contador_rename])+200);
        strcpy(new,ruta2);
        strcat(new,"c");
        rename(ruta2,new);
        free(new);
        free(ruta2);
        strcat(temporales[contador_rename],"c");
        contador_rename++;
    }
    
    t_table_compactation_args_function* args = malloc(sizeof(t_table_compactation_args_function));
    args->name = name_table;

    char* file_path=malloc(strlen(ruta) + 50);
    t_list* list = list_create();
    
     for(int i=0;i<cantidad;i++){
        int block_amount;

        t_table_compactation_partition* r = malloc( sizeof(t_table_compactation_partition));
        t_table_partiton*  p = particion_xd_parte1(temporales[i],name_table, &block_amount);
        strcpy(file_path,ruta);
        strcat(file_path,"/");
        strcat(file_path,temporales[i]);
        log_error(logg,file_path);
        
        r->path = strdup(file_path);
        r->partition = p;
        r->block_amount = block_amount;
        list_add(list, r); 
        
    }
    
    free(ruta);
    free(file_path);
    args->table_compact_args_list = list;
    printf("\nel tamaño de la lista es:%d\n", list_size(args->table_compact_args_list));

    return args;
}

void engine_compactate(t_table_compactation_args_function* args){

    for(int i=0;i<list_size(args->table_compact_args_list);i++){
        t_table_compactation_partition* t = list_get(args->table_compact_args_list, i);
        log_error(logg,t->path);
        particiontemporal(t->partition, t->block_amount, args->name);
        remove(t->path);
        string_iterate_lines(t->partition->blocks, free);
        free(t->partition->blocks);
        free(t->partition);
        free(t->path);
        free(t);

    }

    //free(args->name);
    list_destroy(args->table_compact_args_list);
    free(args);
 
    return;
}


int contadordetemp(DIR* directorio){
    struct dirent* file;

    if(directorio==NULL) return 0;

    int contador=0;
    while((file= readdir(directorio))!=NULL ){
        int len= strlen(file->d_name);
        if(file->d_name[len-1]=='p'){
            contador++;
        }
    }
    rewinddir(directorio);
    return contador;
 
}
 
 int contadordetempc(DIR* directorio){
    struct dirent* file;
    int contador=0;
    while((file= readdir(directorio))!=NULL ){
        int len= strlen(file->d_name);
        if(file->d_name[len-1]=='c'){
            contador++;
        }
    }
    rewinddir(directorio);
    return contador;
 
}
 
void new_block(char* new_row,char* tabla,int particion){
 
    char* ruta=malloc(strlen(tabla) + strlen(tables_path) + 50);
    strcpy(ruta,tables_path);
    strcat(ruta,tabla);
    strcat(ruta,"/");
    char* partauux=string_itoa(particion);
    regg registro[2];
    strcat(ruta,partauux);
    strcat(ruta,".part");
    log_info(logg,ruta);
    FILE* part=NULL;
    part=fopen(ruta,"r");
 
    if(part==NULL){
        return;
    }
    int i=0;
    rewind(part);
 
    while(!feof(part)){
        registro[i].line=malloc(100);
        fgets(registro[i].line,100,part);
        //log_info(logg,registro[i].line);
        i++;//cambiar el 100 por max+1
    }
 
    int new_block=find_free_block();
    set_block_as_occupied(new_block);
    char* list = add_block_to_list(registro[1].line,new_block);
    free(registro[1].line);
    registro[1].line = list;
    int adjust=strlen(new_row);
    adjust_size(registro[0].line,adjust);
    rewind(part);
    fclose(part);
    part=fopen(ruta,"w");
    for(int j=0;j<2;j++){
    fputs(registro[j].line,part);
    }
    fclose(part);
    log_info(logg,MNT_POINT);
 
    char* ruta_bloque=malloc(strlen(MNT_POINT) + strlen("Bloques/") +20);
    strcpy(ruta_bloque,MNT_POINT);
    strcat(ruta_bloque,"Bloques/");
 
    char* aux=string_itoa(new_block);
    strcat(ruta_bloque,aux);
    strcat(ruta_bloque,".bin");
    free(aux);
 
 
    FILE* bloque=fopen(ruta_bloque,"w");
    int length_row=strlen(new_row);
    new_row[length_row]='\0';
    fseek(bloque,0,SEEK_END);
   
    if(ftell(bloque)==0){
        fputs(new_row,bloque);  
    }else{
        char salto[1];
        salto[0]='\n';
        fputs(salto,bloque);
        fputs(new_row,bloque);
    }
    fclose(bloque);
    free(ruta);
    free(registro[0].line);
    free(registro[1].line);
  return;
}
 
char* add_block_to_list(char* block_list,int new){
    int i = 1;
    if(block_list[strlen(block_list)]=='\n')i++;
    
    char* new_block=string_itoa(new);
    char* buff = malloc(strlen(block_list) + 5);
    memcpy(buff, block_list, strlen(block_list)-i);
    buff[strlen(block_list)-1] = '\0';

    if(block_list[8]!=']') strcat(buff, ",");
    strcat(buff, new_block);
    strcat(buff, "]");

    return buff;
 
}
 
void engine_adjust(char* tabla,int particion,int adjust){
    char* ruta= malloc(100);
    strcpy(ruta,tables_path);
    strcat(ruta,tabla);
    strcat(ruta,"/");
    char* aux= malloc(10);
    aux=string_itoa(particion);
    strcat(ruta,aux);
    strcat(ruta,".part");
    regg registro[2];
    int i=0;
    FILE* part=fopen(ruta,"r");
    while(!feof(part)){
        registro[i].line=malloc(100);
        fgets(registro[i].line,100,part);
        log_info(logg,registro[i].line);
        i++;//cambiar el 100 por max+1
    }  
    adjust_size(registro[0].line,adjust);
    rewind(part);
    fclose(part);
    part=fopen(ruta,"w");
    for(int j=0;j<2;j++){
        fputs(registro[j].line,part);
    }
    fclose(part);
    free(registro[0].line);
    free(registro[1].line);
    return;
}
 
long get_dump_time(){
    long r;
    pthread_mutex_lock(&config_lock);
    r = config_tiempo_dump;
    pthread_mutex_unlock(&config_lock);
    return r;
}
 
long get_retardo_time(){
    long r;
    pthread_mutex_lock(&config_lock);
    r = config_retardo;
    pthread_mutex_unlock(&config_lock);
    return r;
}

row* select_particiones_temporales(package_select* select_info){
    printf("tms");
    char* ruta=malloc(strlen(tables_path) + strlen(select_info->table_name) + 30);
    strcpy(ruta,tables_path);
    strcat(ruta,select_info->table_name);
    row* row_return=malloc(sizeof(row));//hace malloc
    DIR* tablaDir=opendir(ruta);
    int cantidad=contadordetemp(tablaDir) + contadordetempc(tablaDir);
    printf("cantidad  de ar : %d", cantidad);
    if(cantidad==0){
        closedir(tablaDir);
        free(ruta);
        row_return->timestap=0;
        return row_return;
    }
    char* temporales[cantidad];
    int contador=0;
    int temp_count = 0;
    int tempc_count = 0;

    struct dirent * file;
    while((file= readdir(tablaDir))!=NULL ){
        int len= strlen(file->d_name);
            printf("nombre:%s\n", file->d_name);

        if(file->d_name[len-1]=='p' || file->d_name[len-1]=='c'){
            temporales[contador]=strdup(file->d_name);
            log_info(logg,temporales[contador]);
            printf("nombre dd:%s\n", file->d_name);

            contador++;
        }
    }
    closedir(tablaDir);
    printf("contador:%d\n", contador);
    int contador2=0;
    int temp_part;
    long mayor=0;
    char* r = malloc(100);
    while(contador2<contador){
        int longitud=strlen(temporales[contador2]);
        char* current_part=malloc(150);
        strcpy(current_part,temporales[contador2]);
        temp_part=partition_num(current_part);
        t_table_partiton* partition;

        if(current_part[longitud-1]== 'p'){
            partition=get_table_partition2(select_info->table_name,temp_part);
        }

        if(current_part[longitud-1]== 'c'){
            partition=get_table_partition3(select_info->table_name,temp_part);
        }
        int block_amount = 0;
        void* first_block = partition->blocks;
        while(*partition->blocks){
            block_amount++;
            partition->blocks++;
        }
        partition->blocks = first_block;
 
        if(block_amount==0)return NULL;
 
        pthread_t buscadores[block_amount];
        regg regruta[block_amount];
 
        int i = 0;
        while(i<block_amount){
            regruta[i].line=malloc(100);
            strcpy(regruta[i].line,"MountTest/");
            strcat(regruta[i].line,"Bloques/");
            strcat(regruta[i].line,partition->blocks[i]);
        
            strcat(regruta[i].line,".bin");
        
            log_info(logg,regruta[i].line);
            i++;
        }

        pthread_mutex_t lock;
        pthread_cond_t cond;
        pthread_mutex_init(&lock, NULL);
        pthread_cond_init(&cond, NULL);

        int whilethread=0;
        argumentosthread2* parametros [block_amount];
        int* number_of_threads = malloc(sizeof(int));
        *number_of_threads = block_amount;

        while(whilethread<block_amount){
            argumentosthread2* args = malloc(sizeof(argumentosthread));
            args->bolean=0;
            args->ruta = strdup(regruta[whilethread].line);
            args->key=select_info->key;
            args->cond = &cond;
            args->lock = lock;
            args->number_of_running_threads = number_of_threads;
            parametros[whilethread] = args;
            pthread_create(&buscadores[whilethread],NULL,buscador2,args);
            pthread_detach(buscadores[whilethread]);
            whilethread++;
        }

        free(partition);

        pthread_mutex_lock(&lock);
        pthread_cond_wait(&cond, &lock);
        int whileparametro=0;
        while(whileparametro<block_amount){
            if(parametros[whileparametro]->bolean && mayor<parametros[whileparametro]->timestap_max){
            mayor=parametros[whileparametro]->timestap_max;
            strcpy(r, parametros[whileparametro]->value);
            strcat(r, "\n");
            }
            whileparametro++;
        }


        pthread_mutex_destroy(&lock);
        pthread_cond_destroy(&cond);
        contador2++;
         //retornar timestap y value
        //falta atender los memory leaks, en especial los de los thread.

    }
    row_return->timestap=mayor;
    row_return->value=strdup(r);
    return row_return;
  
 }

 void free_part(t_table_partiton* partition, int numero_particion, char* nombre_table){
    int block_amount = 0;
    void* first_block = partition->blocks;
    while(*partition->blocks){
        block_amount++;
        partition->blocks++;
       }
    partition->blocks = first_block;
    
    char* rutaTemp = malloc(200);
    int tempContador = 0;
    FILE* block;
    while(block_amount > tempContador){
        strcpy(rutaTemp, MNT_POINT);
        strcat(rutaTemp, "Bloques/");
        strcat(rutaTemp, partition->blocks[tempContador]);
        strcat(rutaTemp, ".bin");
        block = fopen(rutaTemp, "w");
        fclose(block);
        int block_num = atoi(partition->blocks[tempContador]);
        set_block_as_free(block_num);
        tempContador++;
        log_info(logg, "Se borro ");
        log_info(logg, rutaTemp);
    }

    free(rutaTemp);
    return;
 }
