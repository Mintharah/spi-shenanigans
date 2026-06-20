// shm_spi.h
#pragma once
#include <stdint.h>
#include <time.h>
#include <semaphore.h>

#define SHM_NAME "/spi_data"

#pragma pack(push, 1)
typedef struct {
    uint16_t field1;
    uint16_t field2;
    int16_t  field3;
    uint16_t field4;
    uint8_t  field5;
    uint16_t field6;
    uint32_t field7;
} stm32_data_t;
#pragma pack(pop)

typedef struct {
    sem_t           lock;
    uint32_t        seq;
    struct timespec rx_time;
    stm32_data_t    data;
} spi_shared_t;