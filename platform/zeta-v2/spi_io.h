#ifndef MUZIX_ZETA_SPI_IO_H
#define MUZIX_ZETA_SPI_IO_H

#include "ztypes.h"

void muzix_zeta_spi_init(void);
void muzix_zeta_spi_deinit(void);
uint8_t muzix_zeta_spi_tx(uint8_t byte);
void muzix_zeta_spi_cs_low(void);
void muzix_zeta_spi_cs_high(void);

#endif
