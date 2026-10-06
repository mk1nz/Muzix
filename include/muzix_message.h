#ifndef MUZIX_MESSAGE_H
#define MUZIX_MESSAGE_H

#include <stdint.h>

typedef struct {
    uint8_t m_type;
    uint8_t source;
    uint8_t proc1;
    uint8_t proc2;
    uint8_t pid;
    uint8_t signal;
    uint16_t mem_ptr;
    uint16_t stack_ptr;
    uint8_t src_seg;
    uint8_t dst_seg;
    uint8_t src_proc;
    uint8_t dst_proc;
    uint16_t src_vir;
    uint16_t dst_vir;
    uint16_t byte_count;
} muzix_system_message_t;

#endif
