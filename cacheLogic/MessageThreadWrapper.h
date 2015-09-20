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
#include "../../str.h"
#include "../../dprint.h"
#include "../../mem/mem.h"
#ifdef __cplusplus
#undef inline
#endif

// C-style wrapper for C++ thread manager.
typedef struct {
    void *mgr;
    void *sender;
} thread_mgr;

// Initializes new thread manager. Memory allocated by this call is deallocated by destroy call.
// Should be called in global init before forking. Allocates memory on SHM.
thread_mgr* thread_mgr_init();

// Destroys allocated thread manager.
int thread_mgr_destroy(thread_mgr *mgr);

// Initializes process local sender. Should be called in child_init after forking.
int thread_mgr_init_sender(thread_mgr *mgr);

// Destroys process local sender.
int thread_mgr_destroy_sender(thread_mgr *mgr);


// TODO: all desired operations, bindings to m_dump.
// send_message(str * receiver);
// send_message(str * receiver, str * sender);
//




#ifdef __cplusplus
}
#endif



#endif //OPENSIPS_1_11_2_TLS_MESSAGETHREADWRAPPER_H