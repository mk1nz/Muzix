#ifndef MUZIX_SERVER_H
#define MUZIX_SERVER_H

#include <stdint.h>

#define MUZIX_SERVER_TYPE_FS    1
#define MUZIX_SERVER_TYPE_MM    2
#define MUZIX_SERVER_TYPE_PM    3
#define MUZIX_SERVER_TYPE_TTY   4

#define MUZIX_SERVER_STACK_SIZE 1024

typedef struct {
    int (*init)(void);
    int (*handle_message)(const void *msg, void *reply);
    void (*cleanup)(void);
    void *private_data;
} muzix_server_ops_t;

typedef struct {
    uint8_t server_type;
    uint8_t server_id;
    uint8_t ipc_slot;
    uint8_t active;
    uint16_t stack_base;
    uint16_t stack_ptr;
    muzix_server_ops_t *ops;
    void *server_data;
} muzix_server_t;

int muzix_server_init(muzix_server_t *server, uint8_t type, uint8_t id, muzix_server_ops_t *ops);
int muzix_server_run(muzix_server_t *server);
int muzix_server_send(muzix_server_t *server, int dest_type, int dest_id, const void *msg, void *reply);
void muzix_server_exit(muzix_server_t *server);

#define MUZIX_SERVER_MSG_MAX_SIZE 64

typedef struct {
    int16_t m_source;
    int16_t m_type;
    union {
        struct {
            int16_t m1_i1, m1_i2, m1_i3;
            uint16_t m1_p1, m1_p2, m1_p3;
        } m_m1;
        struct {
            int16_t m2_i1, m2_i2, m2_i3;
            int32_t m2_l1, m2_l2;
            uint16_t m2_p1;
        } m_m2;
        struct {
            int16_t m3_i1, m3_i2;
            uint16_t m3_p1;
            char m3_ca1[14];
        } m_m3;
        struct {
            int32_t m4_l1, m4_l2, m4_l3, m4_l4;
        } m_m4;
        struct {
            int8_t m5_c1, m5_c2;
            int16_t m5_i1, m5_i2;
            int32_t m5_l1, m5_l2, m5_l3;
        } m_m5;
        struct {
            int16_t m6_i1, m6_i2, m6_i3;
            int32_t m6_l1;
            void (*m6_f1)(void);
        } m_m6;
    } m_u;
} muzix_server_message_t;

#define MESS_SIZE (sizeof(muzix_server_message_t))

#define m1_i1  m_u.m_m1.m1_i1
#define m1_i2  m_u.m_m1.m1_i2
#define m1_i3  m_u.m_m1.m1_i3
#define m1_p1  m_u.m_m1.m1_p1
#define m1_p2  m_u.m_m1.m1_p2
#define m1_p3  m_u.m_m1.m1_p3

#define m2_i1  m_u.m_m2.m2_i1
#define m2_i2  m_u.m_m2.m2_i2
#define m2_i3  m_u.m_m2.m2_i3
#define m2_l1  m_u.m_m2.m2_l1
#define m2_l2  m_u.m_m2.m2_l2
#define m2_p1  m_u.m_m2.m2_p1

#define m3_i1  m_u.m_m3.m3_i1
#define m3_i2  m_u.m_m3.m3_i2
#define m3_p1  m_u.m_m3.m3_p1
#define m3_ca1 m_u.m_m3.m3_ca1

#define m4_l1  m_u.m_m4.m4_l1
#define m4_l2  m_u.m_m4.m4_l2
#define m4_l3  m_u.m_m4.m4_l3
#define m4_l4  m_u.m_m4.m4_l4

#define m5_c1  m_u.m_m5.m5_c1
#define m5_c2  m_u.m_m5.m5_c2
#define m5_i1  m_u.m_m5.m5_i1
#define m5_i2  m_u.m_m5.m5_i2
#define m5_l1  m_u.m_m5.m5_l1
#define m5_l2  m_u.m_m5.m5_l2
#define m5_l3  m_u.m_m5.m5_l3

#define m6_i1  m_u.m_m6.m6_i1
#define m6_i2  m_u.m_m6.m6_i2
#define m6_i3  m_u.m_m6.m6_i3
#define m6_l1  m_u.m_m6.m6_l1
#define m6_f1  m_u.m_m6.m6_f1

#endif