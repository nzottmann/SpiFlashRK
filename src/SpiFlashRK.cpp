#include "Particle.h"

#include "SpiFlashRK.h"


SpiFlash::SpiFlash(SPIClass &spi, int cs) : spi(spi), cs(cs) {

}

SpiFlash::~SpiFlash() {

}

void SpiFlash::begin() {
	spi.begin(cs);
	digitalWrite(cs, HIGH);

	if (!sharedBus) {
		setSpiSettings();
	}

	// Send release from powerdown 0xab
	wakeFromSleep();
}

bool SpiFlash::isValid() {
	uint8_t foundManufacturerId = (jedecIdRead() >> 16) & 0xff;

	return manufacturerId == foundManufacturerId;
}


void SpiFlash::beginTransaction() {
	if (sharedBus) {
		setSpiSettings();
		// Changing the SPI settings seems to leave the bus unstable for a period of time.
		if (sharedBusDelay != 0) {
			delayMicroseconds(sharedBusDelay);
		}
	}
	pinResetFast(cs);

	// There is some code to do this in the STM32F2xx HAL, but I don't think it's necessary to put
	// a really tiny delay before doing the SPI transfer
	// asm("mov r2, r2");
}

void SpiFlash::endTransaction() {
	pinSetFast(cs);
}

void SpiFlash::setSpiSettings() {
	spi.setBitOrder(spiBitOrder); // Default: MSBFIRST
	spi.setClockSpeed(spiClockSpeedMHz, MHZ); // Default: 30
	spi.setDataMode(SPI_MODE3); // Default: SPI_MODE3
}


uint32_t SpiFlash::jedecIdRead() {

	uint8_t txBuf[4], rxBuf[4];
	txBuf[0] = 0x9f;

	beginTransaction();
	spi.transfer(txBuf, rxBuf, sizeof(txBuf), NULL);
	endTransaction();

	return (rxBuf[1] << 16) | (rxBuf[2] << 8) | (rxBuf[3]);
}

uint8_t SpiFlash::readStatus() {
	uint8_t txBuf[2], rxBuf[2];
	txBuf[0] = 0x05; // RDSR
	txBuf[1] = 0;

	beginTransaction();
	spi.transfer(txBuf, rxBuf, sizeof(txBuf), NULL);
	endTransaction();

	return rxBuf[1];
}

uint8_t SpiFlash::readConfiguration() {
	uint8_t txBuf[2], rxBuf[2];
	txBuf[0] = 0x15; // RDCR
	txBuf[1] = 0;

	beginTransaction();
	spi.transfer(txBuf, rxBuf, sizeof(txBuf), NULL);
	endTransaction();

	return rxBuf[1];
}

bool SpiFlash::isWriteInProgress() {
	return (readStatus() & STATUS_WIP) != 0;
}

void SpiFlash::waitForWriteComplete(unsigned long timeout) {
	unsigned long startTime = millis();

	if (timeout == 0) {
		timeout = waitWriteCompletionTimeoutMs;
	}

	// Wait for up to 500 ms. Most operations should take much less than that.
	while(isWriteInProgress() && millis() - startTime < timeout) {
		// For long timeouts, yield the CPU
		if (timeout > 500) {
			delay(1);
		}
	}

	// Log.trace("isWriteInProgress=%d time=%u", isWriteInProgress(), millis() - startTime);
}


void SpiFlash::writeStatus(uint8_t status) {
	waitForWriteComplete();

	uint8_t txBuf[2];
	txBuf[0] = 0x01; // WRSR
	txBuf[1] = status;

	beginTransaction();
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();
}

void SpiFlash::readData(size_t addr, void *buf, size_t bufLen) {
	uint8_t *curBuf = (uint8_t *)buf;

	while(bufLen > 0) {
		size_t pageOffset = addr % pageSize;
		size_t pageStart = addr - pageOffset;

		size_t count = (pageStart + pageSize) - addr;
		if (count > bufLen) {
			count = bufLen;
		}

		uint8_t txBuf[5];

		setInstWithAddr(addressMode4B ? 0x13 : 0x03, addr, txBuf); // READ 4B

		beginTransaction();
		spi.transfer(txBuf, NULL, addressMode4B ? sizeof(txBuf) : sizeof(txBuf)-1, NULL);
		spi.transfer(NULL, curBuf, bufLen, NULL);
		endTransaction();

		addr += count;
		curBuf += count;
		bufLen -= count;
	}
}


void SpiFlash::setInstWithAddr(uint8_t inst, size_t addr, uint8_t *buf) {
	buf[0] = inst;
	if(addressMode4B) {
		buf[1] = (uint8_t) (addr >> 24);
		buf[2] = (uint8_t) (addr >> 16);
		buf[3] = (uint8_t) (addr >> 8);
		buf[4] = (uint8_t) addr;
	}
	else {
		buf[1] = (uint8_t) (addr >> 16);
		buf[2] = (uint8_t) (addr >> 8);
		buf[3] = (uint8_t) addr;
	}
}


void SpiFlash::writeData(size_t addr, const void *buf, size_t bufLen) {
	uint8_t *curBuf = (uint8_t *)buf;

	waitForWriteComplete();

	while(bufLen > 0) {
		size_t pageOffset = addr % pageSize;
		size_t pageStart = addr - pageOffset;

		size_t count = (pageStart + pageSize) - addr;
		if (count > bufLen) {
			count = bufLen;
		}

		// Log.info("writeData addr=%lx pageOffset=%lu pageStart=%lu count=%lu pageSize=%lu", addr, pageOffset, pageStart, count, pageSize);

		uint8_t txBuf[5];

		setInstWithAddr(addressMode4B ? 0x12 : 0x02, addr, txBuf); // PAGE_PROG 4B

		writeEnable();

		beginTransaction();
		spi.transfer(txBuf, NULL, addressMode4B ? sizeof(txBuf) : sizeof(txBuf)-1, NULL);
		spi.transfer(curBuf, NULL, count, NULL);
		endTransaction();

		waitForWriteComplete(pageProgramTimeoutMs);

		addr += count;
		curBuf += count;
		bufLen -= count;
	}

}


void SpiFlash::sectorErase(size_t addr) {
	waitForWriteComplete();

	uint8_t txBuf[5];

	// Log.trace("sectorEraseCmd=%02x", sectorEraseCmd);

	//
	// ISSI 25LQ080 uses 0x20 or 0xD7
	// Winbond uses 0x20 only, so use that
	setInstWithAddr(addressMode4B ? 0x21 : 0x20, addr, txBuf); // SECTOR_ER 4B


	writeEnable();

	beginTransaction();
	spi.transfer(txBuf, NULL, addressMode4B ? sizeof(txBuf) : sizeof(txBuf)-1, NULL);
	endTransaction();

	waitForWriteComplete(sectorEraseTimeoutMs);
}

void SpiFlash::blockErase(size_t addr) {
	waitForWriteComplete();

	uint8_t txBuf[5];

	setInstWithAddr(addressMode4B ? 0xDC : 0xD8, addr, txBuf); // BLOCK_ER 4B

	writeEnable();

	beginTransaction();
	spi.transfer(txBuf, NULL, addressMode4B ? sizeof(txBuf) : sizeof(txBuf)-1, NULL);
	endTransaction();

	waitForWriteComplete(chipEraseTimeoutMs);

}

void SpiFlash::chipErase() {
	waitForWriteComplete();

	uint8_t txBuf[1];

	txBuf[0] = 0xC7; // CHIP_ER

	writeEnable();

	beginTransaction();
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();

	waitForWriteComplete(chipEraseTimeoutMs);
}

void SpiFlash::resetDevice() {
	waitForWriteComplete();

	uint8_t txBuf[1];

	txBuf[0] = 0x66; // Enable reset

	beginTransaction();
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();

	delayMicroseconds(1);

	txBuf[0] = 0x99; // Reset

	beginTransaction();
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();

	delayMicroseconds(1);
}

void SpiFlash::wakeFromSleep() {
	// Send release from powerdown 0xab
	uint8_t txBuf[1];
	txBuf[0] = 0xab;

	beginTransaction();
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();

	// Need to wait tres (3 microseconds) before issuing the next command
	delayMicroseconds(3);
}

// Note: not all chips support this. Macronix does.
void SpiFlash::deepPowerDown() {

	uint8_t txBuf[1];
	txBuf[0] = 0xb9;

	beginTransaction();
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();

	// Need to wait tdp (10 microseconds) before issuing the next command, but since we're probably doing
	// this before sleep, it's not necessary
}


void SpiFlash::writeEnable() {
	uint8_t txBuf[1];

	beginTransaction();
	txBuf[0] = 0x06; // WREN
	spi.transfer(txBuf, NULL, sizeof(txBuf), NULL);
	endTransaction();

	// ISSI devices require a 3us delay here, but Winbond devices do not
	if (writeEnableDelayUs > 0) {
		delayMicroseconds(writeEnableDelayUs);
	}
}

#if PLATFORM_ID==8

#include "spi_flash.h"

SpiFlashP1::SpiFlashP1() {

}
SpiFlashP1::~SpiFlashP1() {

}

void SpiFlashP1::begin() {
	sFLASH_Init();
}

bool SpiFlashP1::isValid() {
	// TODO: Check the value from jedecIdRead
	return true;
}

uint32_t SpiFlashP1::jedecIdRead() {
	return sFLASH_ReadID();
}

void SpiFlashP1::readData(size_t addr, void *buf, size_t bufLen) {
	sFLASH_ReadBuffer((uint8_t *)buf, addr,  bufLen);
}

void SpiFlashP1::writeData(size_t addr, const void *buf, size_t bufLen) {
	sFLASH_WriteBuffer((const uint8_t *)buf, addr, bufLen);
}

void SpiFlashP1::sectorErase(size_t addr) {
	sFLASH_EraseSector(addr);
}

void SpiFlashP1::chipErase() {
	sFLASH_EraseBulk();
}


#endif

