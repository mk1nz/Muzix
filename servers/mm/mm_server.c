#include "server.h"
#include "mm_service.h"
#include "mm_copy.h"
#include "mproc_model.h"
#include <string.h>

static muzix_mm_service_t g_mm;
static muzix_mm_service_t *g_mm_ptr = 0;
static muzix_server_message_t g_reply;

static int mm_server_init(void) {
    muzix_mm_service_init(&g_mm, 2);
    g_mm_ptr = &g_mm;
    return 0;
}

static void mm_server_cleanup(void) {
    if (g_mm_ptr) {
        muzix_mm_service_destroy(g_mm_ptr);
        g_mm_ptr = 0;
    }
}

static int handle_alloc_page(const muzix_server_message_t *msg) {
    (void)msg;
    if (!g_mm_ptr) return -1;
    uint8_t page;
    int result = muzix_mm_alloc_page(g_mm_ptr, &page);
    g_reply.m1_i1 = page;
    g_reply.m1_i2 = result;
    return 0;
}

static int handle_free_page(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    muzix_mm_free_page(g_mm_ptr, msg->m1_i1);
    g_reply.m1_i1 = 0;
    return 0;
}

static int handle_newmap(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    int result = muzix_mm_newmap(g_mm_ptr, msg->m1_i1, msg->m1_p1);
    g_reply.m1_i1 = result;
    return 0;
}

static int handle_load_map(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    int result = muzix_mm_load_map(g_mm_ptr, msg->m1_i1, (const process_map_t *)msg->m1_p1);
    g_reply.m1_i1 = result;
    return 0;
}

static int handle_copy_user_to_kernel(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    int result = muzix_mm_copy_user_to_kernel(g_mm_ptr, msg->m1_i1, msg->m1_p1, (void *)msg->m1_p2, msg->m1_i2);
    g_reply.m1_i1 = result;
    return 0;
}

static int handle_copy_kernel_to_user(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    int result = muzix_mm_copy_kernel_to_user(g_mm_ptr, msg->m1_i1, msg->m1_p1, (const void *)msg->m1_p2, msg->m1_i2);
    g_reply.m1_i1 = result;
    return 0;
}

static int handle_copy_map_to_kernel(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    int result = muzix_mm_copy_map_to_kernel(g_mm_ptr, (const process_map_t *)msg->m1_p1, msg->m1_i2, (void *)msg->m1_p2, msg->m1_i3);
    g_reply.m1_i1 = result;
    return 0;
}

static int handle_copy_kernel_to_map(const muzix_server_message_t *msg) {
    if (!g_mm_ptr) return -1;
    int result = muzix_mm_copy_kernel_to_map(g_mm_ptr, (const process_map_t *)msg->m1_p1, msg->m1_i2, (const void *)msg->m1_p2, msg->m1_i3);
    g_reply.m1_i1 = result;
    return 0;
}

static int mm_server_handle(const void *msg_v, void *reply) {
    const muzix_server_message_t *msg = (const muzix_server_message_t *)msg_v;
    muzix_server_message_t *r = (muzix_server_message_t *)reply;

    /* g_reply is file-static and outlives the request, so it must be cleared
     * per request: a handler that writes only m1_i1 would otherwise answer
     * with the previous request's other union fields, and `*r = g_reply` below
     * would stamp those over the reply the caller is handed.  The request's
     * own identity goes into it as well, the same two fields pm_server.c's
     * handler copies into its reply buffer, so the success path answers with
     * the request's source and type and a zeroed payload, exactly as the
     * failure path already does. */
    memset(&g_reply, 0, sizeof(g_reply));
    g_reply.m_source = msg->m_source;
    g_reply.m_type = msg->m_type;

    memset(r, 0, sizeof(*r));
    r->m_source = msg->m_source;
    r->m_type = msg->m_type;
    
    int result = -1;
    switch (msg->m_type) {
        case 100: result = handle_alloc_page(msg); break;
        case 101: result = handle_free_page(msg); break;
        case 102: result = handle_newmap(msg); break;
        case 103: result = handle_load_map(msg); break;
        case 104: result = handle_copy_user_to_kernel(msg); break;
        case 105: result = handle_copy_kernel_to_user(msg); break;
        case 106: result = handle_copy_map_to_kernel(msg); break;
        case 107: result = handle_copy_kernel_to_map(msg); break;
        default: result = -1; break;
    }
    
    if (result != -1) {
        *r = g_reply;
    }
    return result;
}

static muzix_server_ops_t g_mm_ops = {
    .init = mm_server_init,
    .handle_message = mm_server_handle,
    .cleanup = mm_server_cleanup,
};

int main(void) {
    muzix_server_t server;
    muzix_server_init(&server, MUZIX_SERVER_TYPE_MM, 2, &g_mm_ops);
    return muzix_server_run(&server);
}