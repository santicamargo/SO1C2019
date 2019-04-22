#ifndef PHARSER_H_   
#define PHARSER_H_
#include "server.h"

void action_select(package_select* select_info);

void action_insert(package_insert* insert_info);

void action_create(package_create* create_info);

void action_describe(package_describe* describe_info);

void action_drop(package_drop* drop_info);

void action_journal(package_journal* journal_info);

void action_add(package_add* add_info);

void action_run(package_run* run_info);

void action_metrics(package_metrics* metrics_info);





#endif
