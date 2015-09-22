//
// Created by Dusan Klinec on 20.09.15.
//

#ifndef OPENSIPS_1_11_2_TLS_MESSAGETHREADWRAPPER_H
#define OPENSIPS_1_11_2_TLS_MESSAGETHREADWRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif
/* give up on ut.h, don't need it anyway. Also give up on inline stuff */
#define ut_h
#ifdef __cplusplus
#define inline
#endif

#include "../../../str.h"
#include "../../../dprint.h"
#include "../../../mem/mem.h"
#include "common.h"

struct cell;
struct tmcb_params;
struct sip_msg;

#ifdef __cplusplus
#undef inline
#endif

// Initializes new thread manager. Memory allocated by this call is deallocated by destroy call.
// Should be called in global init before forking. Allocates memory on SHM.
thread_mgr* thread_mgr_init();

// Destroys allocated thread manager.
int thread_mgr_destroy(thread_mgr *mgr);

// Initializes process local sender. Should be called in child_init after forking.
int thread_mgr_init_sender(thread_mgr *mgr);

// Destroys process local sender.
int thread_mgr_destroy_sender(thread_mgr *mgr);

// Dumping messages. User was registered when this gets called.
int thread_mgr_dump(thread_mgr *mgr, struct sip_msg* msg, char* owner, str uname, str host);

// Periodical cleaning, called by timer thread.
int thread_mgr_clean(thread_mgr *mgr);

// Updates API hooks.
int thread_mgr_update_api(thread_mgr *mgr, struct db_func * msilo_dbf, struct tm_binds * tmb);

// TM callback
void thread_mgr_tm_callback(struct cell *t, int type, struct tmcb_params *ps);

// Allocates new tsx callback data
thread_tsx_callback * thread_mgr_alloc_tsx_data();

#ifdef __cplusplus
}
#endif



#endif //OPENSIPS_1_11_2_TLS_MESSAGETHREADWRAPPER_H
