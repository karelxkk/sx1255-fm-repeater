#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define CLK_FREQ			(32.0e6f)

const char *device="/dev/spidev0.0";
uint8_t mode;
uint8_t bits=8;
uint32_t speed=500000;

void gpio_init(void)
{
	FILE* fp;
	char tmp[256];
	
	//enable
	fp=fopen("/sys/class/gpio/export", "wb");
	if(fp!=NULL)
	{
		sprintf(tmp, "%d", 537); //RST pin is hardcoded at GPIO_25
		fwrite(tmp, strlen(tmp), 1, fp);
		fclose(fp);
	}
	else
	{
		printf("Can not initialize RST pin\nExiting\n");
		exit(1);
	}

	usleep(250000U); //give it 250ms

	sprintf(tmp, "/sys/class/gpio/gpio%d/direction", 537);
	fp=fopen(tmp, "wb");
	if(fp!=NULL)
	{
		fwrite("out", 3, 1, fp);
		fclose(fp);
	}
	else
	{
		printf("Can not initialize RST pin\nExiting\n");
		exit(1);
	}
}

int rst(uint8_t state)
{
	FILE* fp=NULL;
	char tmp[256];

	sprintf(tmp, "/sys/class/gpio/gpio%d/value", 537);
	fp=fopen(tmp, "wb");

	if(fp!=NULL)
	{
		if(state)
		{
			fwrite("1", 1, 1, fp);
		}
		else
		{
			fwrite("0", 1, 1, fp);
		}

		fclose(fp);
		return 0;
	}
	else
	{
		printf("Error - can not set GPIO%d value\n", 537);
		return 1;
	}
}

int spi_init(char *dev)
{
	int fd;
	int ret;

	fd = open(dev, O_RDWR);
	if(fd<0)
	{
		printf("Can't open SPI device\n");
		return fd;
	}

	ret = ioctl(fd, SPI_IOC_WR_MODE, &mode);
	if(ret == -1)
	{
		printf("Can't open SPI device\n");
		return -1;
	}
	ret = ioctl(fd, SPI_IOC_RD_MODE, &mode);
	if(ret == -1)
	{
		printf("Can't open SPI device\n");
		return -1;
	}

	ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
	if(ret == -1)
	{
		printf("Can't set SPI device\n");
		return -1;
	}
	ret = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
	if(ret == -1)
	{
		printf("Can't set SPI device\n");
		return -1;
	}

	ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
	if(ret == -1)
	{
		printf("Can't set SPI device\n");
		return -1;
	}
	ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
	if(ret == -1)
	{
		printf("Can't set SPI device\n");
		return -1;
	}

	close(fd);
	return 0;
}

void sx1255_writereg(uint8_t addr, uint8_t val)
{
	int fd;
	//int ret;

	fd = open(device, O_RDWR);

	uint8_t tx[2] = {addr|(1<<7), val};
	uint8_t rx[2] = {0, 0};
	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = 2,
		.delay_usecs = 0,
		.speed_hz = speed,
		.bits_per_word = bits
	};

	/*ret = */ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
	close(fd);
	usleep(10000U);
}

uint8_t sx1255_readreg(uint8_t addr)
{
	int fd;
	//int ret;

	fd = open(device, O_RDWR);

	uint8_t tx[2] = {addr, 0};
	uint8_t rx[2] = {0, 0};
	struct spi_ioc_transfer tr = {
		.tx_buf = (unsigned long)tx,
		.rx_buf = (unsigned long)rx,
		.len = 2,
		.delay_usecs = 0,
		.speed_hz = speed,
		.bits_per_word = bits
	};

	/*ret = */ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
	close(fd);

	return rx[1];
	usleep(10000U);
}

uint8_t sx1255_set_rx_freq(uint32_t freq)
{
	if(freq<=510e6 && freq>=410e6)
	{
		uint32_t val=lround((float)freq*1048576.0f/CLK_FREQ); //1048576 = 2^20
		sx1255_writereg(0x01, (val>>16)&0xFF); //MSB first
		sx1255_writereg(0x02, (val>>8)&0xFF);
		sx1255_writereg(0x03, val&0xFF); //LSB
		return 0;
	}
	else
		return 1;
}

uint8_t sx1255_set_tx_freq(uint32_t freq)
{
	if(freq<=510e6 && freq>=410e6)
	{
		uint32_t val=lround((float)freq*1048576.0f/CLK_FREQ); //1048576 = 2^20
		sx1255_writereg(0x04, (val>>16)&0xFF); //MSB first
		sx1255_writereg(0x05, (val>>8)&0xFF);
		sx1255_writereg(0x06, val&0xFF); //LSB
		return 0;
	}
	else
		return 1;
}

int main(int argc, char *argv[])
{
	if(spi_init((char*)device)==0)
		printf("SPI config OK\n");
	else
		return -1;

	gpio_init();

	usleep(100000U); //give it 100ms
	rst(1);
	usleep(100000U); //give it 100ms
	rst(0);
	usleep(250000U); //give it 250ms

	//read reg 0x07
	uint8_t val=sx1255_readreg(0x07);
	printf("SX1255 version 0x%02X", val);
	if(val==0x11)
	{
		printf(" - OK\n");
	}
	else
	{
		printf(" - ERROR (expected 0x11)\n");
		return -1;
	}

	//bitrate setting: 8*3^0*2^5=256, I2S left aligned
	sx1255_writereg(0x13, (0<<7)|(0<<6)|(5<<3)|(1<<2));

	//mode B2, div=2^2=4
	sx1255_writereg(0x12, (2<<4)|2);

	//set RX freq
	uint32_t rxf=argc>1?atoi(argv[1]):431500e3;
	printf("Setting RX frequency to %d\n", rxf);
	sx1255_set_rx_freq(rxf);
	//set TX freq
	uint32_t txf=argc>2?atoi(argv[2]):438500e3;
	printf("Setting TX frequency to %d\n", txf);
	sx1255_set_tx_freq(txf);

	//RX
	//RX bandwidth
	sx1255_writereg(0x0D, (0x01<<5)|(0x05<<2)|0x03);

	//RX PLL BW to lowest setting
	sx1255_writereg(0x0E, 0x00);

	//RX gain and LNA input impedance (50ohm)
	sx1255_writereg(0x0C, (0x01<<5)|(0x0F<<1)|0x00); //1..6, 0..F

	//TX
	//TX DAC taps num
	sx1255_writereg(0x0B, 5);
	
	//TX PLL filter
	sx1255_writereg(0x0A, (0<<5)|0);
	
	//TX DAC gain
	sx1255_writereg(0x08, (1<<4)|0xE);

	//enable rx and tx
	sx1255_writereg(0x00, (1<<3)|(1<<2)|(1<<1)|1);

	//read reg 0x11 and 0x13
	val=sx1255_readreg(0x11);

	printf("TX PLL");
	if(val & (1<<0))
		printf(" locked\n");
	else
		printf(" unlocked\n");

	printf("RX PLL");
	if(val & (1<<1))
		printf(" locked\n");
	else
		printf(" unlocked\n");

	val=sx1255_readreg(0x13);

	printf("I2S");
	if((val & (1<<1)) == 0)
		printf(" OK\n");
	else
		printf(" error\n");	

	return 0;
}
