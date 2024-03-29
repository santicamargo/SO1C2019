#include "segments.h"


page_t* 
create_page(int timestamp, int key, char* value){
	//TODO: LEVANTAR EXCEPCION SI EL VALUE ES MUY GRANDE????
	page_t* page = (page_t*)malloc(sizeof(page_t));
	page->timestamp = timestamp;
	page->key = key;
	page->value = (char*)malloc(VALUE_SIZE);
	memcpy(page->value, value, VALUE_SIZE);
	// strcpy(page->value, value);
	return page;
}

page_info_t* create_page_info(int dirty_bit){
  	page_info_t* page_info = (page_info_t*)malloc(sizeof(page_info_t));
	page_info->dirty_bit = dirty_bit == 0 ? 0 : 1;
	page_info->next = NULL;
	page_info->prev = NULL;
  	return page_info;
}

segment_t* create_segment(char* table_name){
  	segment_t* segment = (segment_t*)malloc(sizeof(segment_t));
	segment->name = table_name;
  	segment->pages = NULL;
	segment->next = NULL;
	segment->prev = NULL;
	add_segment_to_table(segment);
  	return segment;
}


int is_memory_full(){
	return LRU_TABLE->current_pages < NUMBER_OF_PAGES ? 0 : 1;
}

int find_free_page(){
	log_info(logger, "Searching for  index");
	if(!is_memory_full()){
		for(int i = 0; i < NUMBER_OF_PAGES; i++){ // me fijo que indexes de pagina estan siendo usados
			// printf("Index %d is // ? ", i);
			if(!page_is_on_use(i)){
				// printf("Yes\n");
				return i;
			}
			// printf("No\n");
		}
		
	}
	else{
		log_info(logger, "-- MEMORY FULL, REPLACING PAGE --");
		int index_to_replace = find_unmodified_page();
		if(index_to_replace != -1){
			log_info(logger, "Index to replace: %d", index_to_replace);
			return index_to_replace;
		}
	}
	return -1;
}


// es distinta a is_memory_full(), si encuentra pagina sin modificar, la remueve y devuelve false
int memory_full(){
	int i = find_free_page();
	return i == -1 ? 1 : 0;
}

// busco la primer pagina sin dirtybit y devuelvo el index o -1 si esta todo hasta las bolas
int find_unmodified_page(){
	lru_page_t* to_be_replaced_page;
	log_info(logger, "-- Looking for unmodified page --");
	
	for(int i = 0; i < LRU_TABLE->current_pages; i++){ 
		// printf("Index %d is unmodified?: ", i);		
		to_be_replaced_page = LRU_TABLE->lru_pages+i;
		if(!is_modified(to_be_replaced_page->lru_page)){
			int index = to_be_replaced_page->lru_page->index;
			remove_page(to_be_replaced_page->lru_page);
			// printf("Yes\n");
			return index;
		}
		// printf("No\n");		
	}
	return -1;
}

void free_page(page_t* page){
	free(page->value);
	free(page);
}

// guarda una pagina en memoria sin dirtybit porque es un select de fs
page_info_t* save_page(char* table_name, page_t* page){ 
	page_info_t* page_info = find_page_info(table_name, page->key);
	
	// si ya existe no hago nada, para modificar una pagina hay que usar el insert_page
	if(page_info == NULL){
		page_info = save_page_to_memory(table_name, page, 0);
	}
	return page_info;
}

// Agrega una nueva pagina o modifica una a existente siempre con dirtybit
page_info_t* insert_page(char* table_name, page_t* page){
	page_info_t* page_info = find_page_info(table_name, page->key);
	// si ya existe la pagina, reemplazo el value y toco el dirtybit
	if(page_info != NULL){
		if(page_info->page_ptr->timestamp < page->timestamp){ // si por alguna razon de la vida el timestamp del insert es menor al timestamp que ya tengo en la page, no la modifico
			log_info(logger, "Updating value %s->%s\n", page_info->page_ptr->value, page->value);
			
			memcpy(page_info->page_ptr->value, page->value, VALUE_SIZE);
			page_info->dirty_bit = 1;
		}
	}
	// si no existe, creo una nueva con dirtybit (si no tiene dirtybit no se la mando a fs en el journaling)
	else{
		page_info = save_page_to_memory(table_name, page, 1);
	}
	// free_page(page);
	return page_info;
}

// crea una page_info con o sin dirtybit, se la asigna al segmento, y guarda la pagina en main memory
page_info_t* save_page_to_memory(char* table_name, page_t* page, int dirty_bit){ 
	// si no existe la pagina, busco una pagina libre en memoria y le guardo la page
	int index = find_free_page();
	page_info_t* page_info = create_page_info(dirty_bit);
	if(index == -1){
		journal(); // vacio memoria y vuelvo a buscar un index, deberia ser 0
		index = find_free_page();
	}
	// recien si hay espacio, creo el segmento
	segment_t* segment = find_or_create_segment(table_name); 
	page_info->index = index;
	page_info->page_ptr = MAIN_MEMORY+index;
	memcpy(page_info->page_ptr, page, PAGE_SIZE);
	add_page_to_segment(segment, page_info);
	update_LRU(segment, page_info);
	return page_info;
}

// droppea la pagina sin mandarla al fs
void remove_page(page_info_t* page_info){
	// busco la pagina en la lru table (para saber el segmento al que pertenece)
	lru_page_t* lru_page_info = LRU_TABLE->lru_pages+find_page_in_LRU(page_info);
	
	remove_from_segment(lru_page_info->segment, page_info);
	remove_from_LRU(lru_page_info);
	memset(MAIN_MEMORY+page_info->index, 0, VALUE_SIZE); // seteo a 0 la page en main memory
	// free_lru_page(lru_page_info);
	free(page_info);
}

// libera la pagina y si tiene dirtybit la manda al fs 
void remove_and_save_page(page_info_t* page_info){
	// busco la pagina en la lru table (para saber el segmento al que pertenece)
	if(page_info->dirty_bit != 0){
		lru_page_t* lru_page_info = LRU_TABLE->lru_pages+find_page_in_LRU(page_info);
		log_info(logger, "- Saving \"%s\" to fs -", page_info->page_ptr->value);
		package_insert* insert_info = (package_insert*)malloc(sizeof(package_insert));
		insert_info->table_name = strdup(lru_page_info->segment->name);
		insert_info->instruction = strdup("insert");
		insert_info->key = page_info->page_ptr->key;
		insert_info->value = page_info->page_ptr->value;
		insert_info->timestamp = (unsigned)time(NULL);
		remove_page(page_info);
		
		char* parsed_package_insert = parse_package_insert(insert_info);
		char* response = exec_in_fs(fs_socket, parsed_package_insert);
		log_info(logger, "Response FS: %s", response);
		free(parsed_package_insert);
		free(response);
		// free(lru_page_info->lru_page);
	}
	else{
		remove_page(page_info);
	}
}

// si el segundo argumento es 0, droppeo la pagina sin mandarla al fs
void remove_all_pages_from_segment(segment_t* segment, int save_to_fs_bit){
	log_info(logger, "-- REMOVING PAGES FROM SEGMENT %s--", segment->name);
	if(save_to_fs_bit != 0){
		while(segment->pages != NULL){
			remove_and_save_page(segment->pages);
		}
	}
	else{
		while(segment->pages != NULL){
			remove_page(segment->pages);
		}
	}
}

void free_segment(segment_t* segment){
	free(segment->name);
	free(segment);
}

// remueve el segmento y todas sus paginas, si el save_to_fs_bit es != 0, manda las paginas al fs 
void remove_segment(char* table_name, int save_to_fs_bit){
	segment_t* temp = find_segment(table_name);	
	log_info(logger, "-- REMOVING SEGMENT: %s --", temp->name);
	remove_all_pages_from_segment(temp, save_to_fs_bit);
	if(temp->next != NULL){ // si no es el ultimo..
		temp->next->prev = temp->prev; // le asigno al siguiente de temp, su anterior, o null en caso de que sea el primero
	}
	if(temp->prev != NULL){ // si no es el primero..
		temp->prev->next = temp->next;
	}
	else{ // en caso de que sea el primero..
		SEGMENT_TABLE = temp->next;
	}
	// free(temp);
	free_segment(temp);
	log_info(logger, "-- SEGMENT REMOVED --");	
}

void remove_from_segment(segment_t* segment, page_info_t* temp){
	log_info(logger, "- Removing \"%s\" from table %s -", temp->page_ptr->value, segment->name);
	if(temp->next != NULL){ // si no es el ultimo..
		temp->next->prev = temp->prev; // le asigno al siguiente de temp, su anterior, o null en caso de que sea el primero
	}
	if(temp->prev != NULL){ // si no es el primero..
		temp->prev->next = temp->next;
	}
	else{ // en caso de que sea el primero..
		segment->pages = temp->next;
	}
	// free(temp);
}

int is_modified(page_info_t* page){
	return page->dirty_bit == 0 ? 0 : 1;
}

segment_t* find_segment(char* table_name){

	segment_t* temp = SEGMENT_TABLE;
	while(temp != NULL){
		if(strcmp(temp->name, table_name) == 0){
			return temp;
		}
		temp = temp->next;
	}
	return temp; // si no encontre nada en el loop, devuelvo temp que es NULL
}

page_info_t* find_page_info(char* table_name, int key){
	segment_t* segment = find_segment(table_name);
	if(segment == NULL) return NULL;
	page_info_t* temp = segment->pages;
	while(temp != NULL){
		if(temp->page_ptr->key == key){
			update_LRU(segment, temp);
			return temp;
		}
		temp = temp->next;
	}
	return temp; // si no encontre nada hasta ahora, devuelvo temp que es NULL
}

segment_t* find_or_create_segment(char* table_name){
	segment_t* segment = find_segment(table_name);
	if(segment != NULL){
		return segment;
	}
	segment = create_segment(strdup(table_name));
	return segment;
}

void add_segment_to_table(segment_t* segment){
	if(SEGMENT_TABLE == NULL){
		SEGMENT_TABLE = segment;
		segment->prev = NULL;
	}
	else{
		segment_t* temp = get_last_segment();
		temp->next = segment;
		segment->prev = temp;
	}
	print_segment_table();
}

void add_page_to_segment(segment_t* segment, page_info_t* page_info){
	if(segment->pages == NULL){
		segment->pages = page_info;
		page_info->prev = NULL;
	}
	else{
		page_info_t* temp = get_last_page(segment->pages);
		temp->next = page_info;
		page_info->prev = temp;
	}
}

segment_t* get_last_segment(){
  segment_t* temp = SEGMENT_TABLE;
  while(temp->next != NULL){
    temp = temp->next;
  }
  return temp;
}

page_info_t* get_last_page(page_info_t* page_info){
  page_info_t* temp = page_info;
  while(temp->next != NULL){
    temp = temp->next;
  }
  return temp;
}

void print_page(page_info_t* page_info){
	if(page_info != NULL){
		printf("-%d- \"%s\", key: %d, dirtybit: %d\n", page_info->index, page_info->page_ptr->value, page_info->page_ptr->key, page_info->dirty_bit);
	}
	else {
		printf("page doesnt exist");
	}
}

void print_segment_table(){
	segment_t* temp = SEGMENT_TABLE;
	printf("Segment Table: \n");
	while(temp != NULL){
		printf("%s ", temp->name);
		temp = temp->next;		
	}
	printf("\n");
}

void print_segment_pages(segment_t* segment){
	page_info_t* temp = segment->pages;
	printf("Table %s pages:\n", segment->name);
	while(temp != NULL){
		print_page(temp);
		temp = temp->next;
	}
	printf("\n");
}

LRU_TABLE_t* create_LRU_TABLE(){
	LRU_TABLE_t* lru = (LRU_TABLE_t*)malloc(sizeof(LRU_TABLE_t));
	lru->current_pages = 0;
	lru->lru_pages = (lru_page_t*)malloc(sizeof(lru_page_t) * NUMBER_OF_PAGES);
	lru->used_pages = (int*)malloc(sizeof(int) * NUMBER_OF_PAGES);
	return lru;
}

// busca una pagina dentro del LRU TABLE y devuelve su index, sino -1
int find_page_in_LRU(page_info_t* page_info){
	lru_page_t* lru_page_info;
	for(int i = 0; i < LRU_TABLE->current_pages; i++){
		lru_page_info = LRU_TABLE->lru_pages+i;
		if(lru_page_info->lru_page == page_info){
			return i;
		}
	}
	return -1;
}

lru_page_t* create_lru_page(segment_t* segment, page_info_t* page_info){
	lru_page_t* lru_page_info = (lru_page_t*)malloc(sizeof(lru_page_t));
	lru_page_info->lru_page = page_info;
	lru_page_info->segment = segment;
	log_info(logger, "Creating page... Index %d, Value: %s, Table: %s", lru_page_info->lru_page->index, lru_page_info->lru_page->page_ptr->value, lru_page_info->segment->name);
	return lru_page_info;
}

void update_LRU(segment_t* segment, page_info_t* page_info){
	log_info(logger, "-- Updating LRU --");	
	int index = find_page_in_LRU(page_info);
	int last_index = LRU_TABLE->current_pages-1;
	
	if(index != -1){ // si ya esta en la tabla, la muevo al final
		if(index == last_index){
			log_info(logger, "Page was already the most RU.");
			print_LRU_TABLE();
			return;
		}
		log_info(logger, "Found page on table, updating...");
		lru_page_t temp = *(LRU_TABLE->lru_pages+index); // no uso un puntero porque cuando muevo piso la posicion donde apuntaba y se rompe todo
		memmove(LRU_TABLE->lru_pages+index, LRU_TABLE->lru_pages+index+1, sizeof(lru_page_t) * NUMBER_OF_PAGES-index-1);		
		memcpy(LRU_TABLE->lru_pages+last_index, &temp, sizeof(lru_page_t));

	}
	else{ // si no esta en la tabla la agrego
		lru_page_t* temp = create_lru_page(segment, page_info);
		memcpy(LRU_TABLE->lru_pages+last_index+1, temp, sizeof(lru_page_t));
		LRU_TABLE->current_pages++;
		// free_lru_page(temp);
		free(temp);
	}
	update_used_pages();
	print_LRU_TABLE();
}

void free_lru_page(lru_page_t* lru_page_info){
	free(lru_page_info->lru_page);
	free(lru_page_info);
}

void remove_from_LRU(lru_page_t* lru_page_info){
	log_info(logger, "- Removing \"%s\" from LRU -", lru_page_info->lru_page->page_ptr->value);	
	
	int index = find_page_in_LRU(lru_page_info->lru_page);
	if(index != -1){
		memmove(LRU_TABLE->lru_pages+index, LRU_TABLE->lru_pages+index+1, sizeof(lru_page_t) * NUMBER_OF_PAGES-index-1);		
		// free_lru_page(lru_page_info);
		LRU_TABLE->current_pages--;
	}
	update_used_pages();
}

void print_LRU_TABLE(){
	lru_page_t* lru_page_info;
	printf("LRU Table (%d pages): \n", LRU_TABLE->current_pages);
	if (LRU_TABLE->current_pages != 0){
		for(int i = 0; i < LRU_TABLE->current_pages; i++){
			lru_page_info = LRU_TABLE->lru_pages+i;
			printf("%d ", lru_page_info->lru_page->index);
			// printf("%d ", lru_page_info->lru_page->index, lru_page_info->lru_page->page_ptr->value, lru_page_info->segment->name);
		}
	}
	printf("\n");
}


// lru table tiene un array de los indices de las paginas en uso para accederlos mas rapido, cada vez que se toca la lru hay que actualizarlo. 
int* update_used_pages(){ 
	lru_page_t* lru_page_info = LRU_TABLE->lru_pages;
	for(int i = 0; i < LRU_TABLE->current_pages; i++){
		*(LRU_TABLE->used_pages+i) = (lru_page_info+i)->lru_page->index;
	}
	return LRU_TABLE->used_pages;
}

// devuelvo true or false dependiendo si el index de pagina esta en current_pages
int page_is_on_use(int index){
	for(int i = 0; i < LRU_TABLE->current_pages; i++){
		if(*(LRU_TABLE->used_pages+i) == index){
			return 1;
		}
	}
	return 0;
}

char* exec_in_fs(int memory_fd, char* payload){
    char* responce = malloc(3000);

    
    if ( memory_fd < 0 ){
      log_error(logger, "No se pudo llevar a cabo la accion.");
      return strdup("");
    }

    //ejecutar
    if(send(memory_fd,payload, strlen(payload)+1,MSG_NOSIGNAL)>0){
      	read(memory_fd, responce, 3000);
      	return responce;
    }else{
		// free(responce);
		log_error(logger, "No se logo comuniarse con FS");
      	return strdup("NO SE ENCUENTRA FS\n");
    }  
		// (responce);
    	return strdup("algo sale mal");
}

void journal(){
	// printf("\n-- JOURNALING ATR --\n\n");
	log_info(logger, "Iniciando Journaling");
	// print_everything();
	segment_t* temp = SEGMENT_TABLE;
	while(temp != NULL){
		remove_segment(temp->name, 1);
		temp = SEGMENT_TABLE;
	}
	print_everything();
	log_info(logger, "Journaling terminado");
	// printf("\n-- JOURNALING TERMINADO --\n\n");	
}

void print_everything(){
	segment_t* temp = SEGMENT_TABLE;
	while(temp != NULL){
		print_segment_pages(temp);
		temp = temp->next;
	}
}

void* journal_activation() {
  fflush(stdout);
  while(1) {
    sleep( config_get_int_value(config, "RETARDO_JOURNAL") / 1000);
    journal();
  }
}