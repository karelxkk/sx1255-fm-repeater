// ok1lbc hotfix sx1255-spi.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <gpiod.h>
#include <getopt.h>

// Build with gcc sx1255-spi-fixed.c -o sx1255-spi-fixed -lgpiod -lm
static int spi_fd = -1;

// --- Configuration ---
#define CLK_FREQ (32.0e6f)
#define RST_PIN_OFFSET 22 // GPIO pin 22 on gpiochip0 for reset

const char *spi_device = "/dev/spidev0.0";
const char *gpio_chip_path = "/dev/gpiochip0";

// --- Global libgpiod handles ---
struct gpiod_chip *gpio_chip;
struct gpiod_line *rst_line;

// --- Unchanged SPI and SX1255 Functions (Omitted for brevity) ---
uint8_t bits = 8;
uint32_t speed = 500000;
uint8_t mode;

// --- SX1255 ---
typedef enum
{
    RATE_125K,
    RATE_250K,
    RATE_500K
} rate_t;

// settings
uint32_t rxf;
uint32_t txf;
float mix_gain;
int8_t dac_gain;
uint8_t lna_gain;
uint8_t pga_gain;
uint16_t pll_bw;
rate_t rate;
uint8_t addr;
uint8_t val;

int spi_init(char *dev)
{
int fd;
int ret;

fd = open(dev, O_RDWR);
if (fd < 0) {
    perror("Can't open SPI device");
    return fd;
}

/* Configure per-FD settings */
ret = ioctl(fd, SPI_IOC_WR_MODE, &mode);
if (ret == -1) { perror("Can't set SPI mode"); close(fd); return -1; }

ret = ioctl(fd, SPI_IOC_RD_MODE, &mode);
if (ret == -1) { perror("Can't read back SPI mode"); close(fd); return -1; }

ret = ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
if (ret == -1) { perror("Can't set SPI write bits per word"); close(fd); return -1; }

ret = ioctl(fd, SPI_IOC_RD_BITS_PER_WORD, &bits);
if (ret == -1) { perror("Can't read back SPI bits per word"); close(fd); return -1; }

ret = ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
if (ret == -1) { perror("Can't set SPI write max speed"); close(fd); return -1; }

ret = ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed);
if (ret == -1) { perror("Can't read back SPI max speed"); close(fd); return -1; }

/* Keep descriptor open for all transfers */
spi_fd = fd;
return 0;
}

void sx1255_writereg(uint8_t addr, uint8_t val)
{
    if (spi_fd < 0) {
        if (spi_init((char *)spi_device) != 0) return;
    }
    uint8_t tx[2] = { (uint8_t)(addr | 0x80), val };
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = 0,
        .len = 2,
        .speed_hz = speed,
        .bits_per_word = bits,
        .cs_change = 0,
        .delay_usecs = 0
    };
    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) {
        perror("sx1255_writereg: transfer");
    }
    usleep(1000);
}


uint8_t sx1255_readreg(uint8_t addr)
{
    if (spi_fd < 0) {
        if (spi_init((char *)spi_device) != 0) return 0;
    }
    uint8_t tx[2] = { (uint8_t)(addr & ~0x80), 0 };
    uint8_t rx[2] = { 0, 0 };
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = 2,
        .speed_hz = speed,
        .bits_per_word = bits,
        .cs_change = 0,
        .delay_usecs = 0
    };
    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 1) {
        perror("sx1255_readreg: transfer");
        return 0;
    }
    usleep(1000);
    return rx[1];
}


void sx1255_set_rx_freq(uint32_t freq)
{
    uint32_t val = lround((float)freq * 1048576.0f / CLK_FREQ);
    sx1255_writereg(0x01, (val >> 16) & 0xFF);
    sx1255_writereg(0x02, (val >> 8) & 0xFF);
    sx1255_writereg(0x03, val & 0xFF);
}

void sx1255_set_tx_freq(uint32_t freq)
{
    uint32_t val = lround((float)freq * 1048576.0f / CLK_FREQ);
    sx1255_writereg(0x04, (val >> 16) & 0xFF);
    sx1255_writereg(0x05, (val >> 8) & 0xFF);
    sx1255_writereg(0x06, val & 0xFF);
}

int8_t sx1255_setrate(rate_t r)
{
    uint8_t n, div;

    switch (r)
    {
    case RATE_125K:
        n = 5;
        div = 2;
        break;

    case RATE_250K:
        n = 4;
        div = 1;
        break;

    case RATE_500K:
        n = 3;
        div = 0;
        break;

    default: // 125k
        n = 5;
        div = 2;
        break;
    }

    // interpolation/decimation factor = mant * 3^m * 2^n
    sx1255_writereg(0x13, (0 << 7) | (0 << 6) | (n << 3) | (1 << 2));

    // mode B2, XTAL/CLK_OUT division factor=2^div
    sx1255_writereg(0x12, (2 << 4) | div);

    // return i2s status flag
    return (sx1255_readreg(0x13) & (1 << 1)) >> 1;
}

int gpio_init(void)
{
    // Open the GPIO chip
    gpio_chip = gpiod_chip_open(gpio_chip_path);
    if (!gpio_chip)
    {
        fprintf(stderr, "Error opening GPIO chip '%s': %s\n", gpio_chip_path, strerror(errno));
        return -1;
    }

    rst_line = gpiod_chip_get_line(gpio_chip, RST_PIN_OFFSET);
    if (!rst_line)
    {
        fprintf(stderr, "Error getting GPIO line for pin %d: %s\n", RST_PIN_OFFSET, strerror(errno));
        gpiod_chip_close(gpio_chip);
        gpio_chip = NULL;
        return -1;
    }

    if (gpiod_line_request_output(rst_line, "sx1255-reset", 0) < 0)
    {
        fprintf(stderr, "Error requesting GPIO line for pin %d: %s\n", RST_PIN_OFFSET, strerror(errno));
        gpiod_chip_close(gpio_chip);
        rst_line = NULL;
        gpio_chip = NULL;
        return -1;
    }
    return 0;
}

int rst(uint8_t state)
{
    if (gpiod_line_set_value(rst_line, state ? 1 : 0) < 0)
    {
        fprintf(stderr, "Error setting GPIO line value for pin %d to state %d: %s\n",
                RST_PIN_OFFSET, state, strerror(errno));
        return 1;
    }
    return 0;
}

void gpio_cleanup(void)
{
    if (rst_line)
    {
        gpiod_line_release(rst_line);
        rst_line = NULL;
    }
    if (gpio_chip)
    {
        gpiod_chip_close(gpio_chip);
        gpio_chip = NULL;
    }
}

void print_help(const char *program_name)
{
    printf("SX1255 config tool\n\n");
    printf("Usage: %s [OPTIONS]\n\n", program_name);
    printf("Optional options:\n");
    printf("  -E, --reset               Reset device\n");
    printf("  -s, --rate=RATE           Set sample rate in kHz (125, 250, 500)\n");
    printf("  -r, --rxf=FREQ            Receive frequency (Hz)\n");
    printf("  -t, --txf=FREQ            Transmit frequency (Hz)\n");
    printf("  -l, --lna_gain=GAIN       RX LNA gain (0..48 dB)\n");
    printf("  -p, --pga_gain=GAIN       RX PGA gain (0..30 dB)\n");
    printf("  -d, --dac_gain=GAIN       TX DAC gain (-9, -6, -3, 0 dB)\n");
    printf("  -m, --mix_gain=GAIN       TX mixer gain (-37.5..-7.5 dB)\n");
    printf("  -a  --rx_pll_bw=VAL       RX PLL bandwidth in kHz (75, 150, 225, 300)\n");
    printf("  -b  --tx_pll_bw=VAL       TX PLL bandwidth in kHz (75, 150, 225, 300)\n");
    printf("  -T, --tx_ena=VAL          TX path enable (0/1)\n");
    printf("  -R, --rx_ena=VAL          RX path enable (0/1)\n");
    printf("  -P, --pll_flags           Get PLL lock flags\n");
    printf("  -G, --get_reg=ADDR        Get register value (dec or hex address)\n");
    printf("  -S, --set_reg=ADDR,VAL    Set register value (hex address and value)\n");
    printf("  -h, --help                Display this help message and exit\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s -s 500 -r 433475000 -t 438000000\n", program_name);
}

// --- Main Program Logic ---
int main(int argc, char *argv[])
{
    mode = SPI_MODE_0;

    if (spi_init((char *)spi_device) == 0)
    {
        // printf("SPI config OK\n");
    }
    else
    {
        return -1;
    }

    if (gpio_init() == 0)
    {
        ;
    }
    else
    {
        fprintf(stderr, "Can not initialize RST pin\nExiting\n");
        return -1;
    }

    uint8_t val = sx1255_readreg(0x07);
    printf("Detected SX1255 chip version V%d%c", (val >> 4) & 0xF, 'A' + (val & 0xF) - 1);
    if (val == 0x11)
    {
        printf(" - OK\n");
    }
    else
    {
        printf(" - WARNING (expected V1A)\n");
    }

    // Define the long options
    static struct option long_options[] =
    {
        {"reset", no_argument, 0, 'E'},
        {"rate", required_argument, 0, 's'},
        {"rxf", required_argument, 0, 'r'},
        {"txf", required_argument, 0, 't'},
        {"lna_gain", required_argument, 0, 'l'},
        {"pga_gain", required_argument, 0, 'p'},
        {"dac_gain", required_argument, 0, 'd'},
        {"mix_gain", required_argument, 0, 'm'},
        {"rx_pll_bw", required_argument, 0, 'a'},
        {"tx_pll_bw", required_argument, 0, 'b'},
        {"tx_ena", required_argument, 0, 'T'},
        {"rx_ena", required_argument, 0, 'R'},
        {"pll_flags", no_argument, 0, 'P'},
        {"get_reg", required_argument, 0, 'G'},
        {"set_reg", required_argument, 0, 'S'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    // autogenerate the arg list
    char arglist[64] = {0};
    for (uint8_t i = 0; i < sizeof(long_options) / sizeof(struct option) - 1; i++)
    {
        arglist[strlen(arglist)] = long_options[i].val;
        if (long_options[i].has_arg != no_argument)
            arglist[strlen(arglist)] = ':';
    }

    int opt;
    int option_index = 0;

    // Parse command line arguments
    while ((opt = getopt_long(argc, argv, arglist, long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        // reset
        case 'E':
            printf("Resetting device... ");
            usleep(100000U);
            if(rst(1)!=0)
                return -1;
            usleep(100000U);
            if(rst(0)!=0)
                return -1;
            usleep(250000U);
            printf("completed\n");
            break;

        // sample rate
        case 's':
            if (strlen(optarg) > 0)
            {
                uint16_t srate = atoi(optarg);
                if (srate == 125)
                {
                    printf("Setting sample rate to %d kSa/s\n", srate);
                    rate = RATE_125K;
                }
                else if (srate == 250)
                {
                    printf("Setting sample rate to %d kSa/s\n", srate);
                    rate = RATE_250K;
                }
                else if (srate == 500)
                {
                    printf("Setting sample rate to %d kSa/s\n", srate);
                    rate = RATE_500K;
                }
                else
                {
                    printf("Unsupported rate setting of %d kSa/s. Using 125 kSa/s\n", srate);
                }
            }
            else
            {
                printf("Invalid sample rate. Using 125 kHz.\n");
                rate = RATE_125K;
            }

            int8_t retval = sx1255_setrate(rate);
            printf("I2S setup %s\n", retval == 0 ? "OK" : "error");
            if (retval != 0)
            {
                printf("Exiting\n");
                gpio_cleanup();
                return -1;
            }
            break;

        // rx freq
        case 'r':
            if (strlen(optarg) > 0)
            {
                rxf = atoi(optarg);
                printf("Setting RX frequency to %u Hz\n", rxf);
            }
            else
            {
                printf("Invalid RX frequency. Using 435000000 Hz.\n");
                rxf = 435000000;
            }
            sx1255_set_rx_freq(rxf);
            break;

        // tx freq
        case 't':
            if (strlen(optarg) > 0)
            {
                txf = atoi(optarg);
                printf("Setting TX frequency to %u Hz\n", txf);
            }
            else
            {
                printf("Invalid TX frequency. Using 435000000 Hz.\n");
                txf = 435000000;
            }
            sx1255_set_tx_freq(txf);
            break;
    
        // lna gain
        case 'l':
            if (strlen(optarg) > 0)
            {
                lna_gain = atoi(optarg);
                if (lna_gain <= 48)
                {
                    uint8_t real_gain;

                    if (lna_gain > 45)
                    {
                        lna_gain = 1;
                        real_gain = 48;
                    }
                    else if (lna_gain > 39)
                    {
                        lna_gain = 2;
                        real_gain = 42;
                    }
                    else if (lna_gain > 30)
                    {
                        lna_gain = 3;
                        real_gain = 36;
                    }
                    else if (lna_gain > 18)
                    {
                        lna_gain = 4;
                        real_gain = 24;
                    }
                    else if (lna_gain > 6)
                    {
                        lna_gain = 5;
                        real_gain = 12;
                    }
                    else
                    {
                        lna_gain = 6;
                        real_gain = 0;
                    }
                    printf("Setting LNA gain to %d dB\n", real_gain);
                }
                else
                {
                    printf("Invalid LNA gain. Using 48 dB.\n");
                    lna_gain = 1;
                }
            }
            else
            {
                printf("Missing LNA gain. Using 48 dB.\n");
                lna_gain = 1;
            }
            val = sx1255_readreg(0x0C);
            val &= (uint8_t)~(0xE0);
            val |= lna_gain << 5;
            sx1255_writereg(0x0C, val);
            break;

        // pga gain
        case 'p':
            if (strlen(optarg) > 0)
            {
                pga_gain = atoi(optarg);
                if (pga_gain <= 30)
                {
                    pga_gain = pga_gain / 2;
                    printf("Setting PGA gain to %d dB\n", pga_gain * 2);
                }
                else
                {
                    printf("PGA gain out of range. Using 30 dB.\n");
                    pga_gain = 15;
                }
            }
            else
            {
                printf("Missing PGA gain. Using 30 dB.\n");
                pga_gain = 15;
            }
            val = sx1255_readreg(0x0C);
            val &= (uint8_t)~(0x1E);
            val |= pga_gain << 1;
            sx1255_writereg(0x0C, val);
            break;

        // dac gain
        case 'd':
            if (strlen(optarg) > 0)
            {
                dac_gain = atoi(optarg);
                if (dac_gain == 0)
                {
                    printf("Setting DAC gain to %d dB\n", dac_gain);
                    dac_gain = 3;
                }
                else if (dac_gain == -3)
                {
                    printf("Setting DAC gain to %d dB\n", dac_gain);
                    dac_gain = 2;
                }
                else if (dac_gain == -6)
                {
                    printf("Setting DAC gain to %d dB\n", dac_gain);
                    dac_gain = 1;
                }
                else if (dac_gain == -9)
                {
                    printf("Setting DAC gain to %d dB\n", dac_gain);
                    dac_gain = 0;
                }
                else
                {
                    printf("Unsupported DAC gain setting of %d dB. Using -3 dB.\n", dac_gain);
                    dac_gain = 2;
                }
            }
            else
            {
                printf("Missing DAC gain. Using -3 dB.\n");
                dac_gain = 2;
            }
            val = sx1255_readreg(0x08);
            val &= (uint8_t)0x0F;
            val |= (uint8_t)dac_gain << 4;
            sx1255_writereg(0x08, val);
            break;

        // mixer gain
        case 'm':
            if (strlen(optarg) > 0)
            {
                mix_gain = atof(optarg);
                if (mix_gain >= -37.5 && mix_gain <= -7.5)
                {
                    printf("Setting mixer gain to %.1f dB\n", mix_gain);
                    mix_gain += 37.5f;
                    mix_gain = roundf(mix_gain / 2.0f);
                }
                else
                {
                    printf("Invalid mixer gain. Using -9.5 dB.\n");
                    mix_gain = 0x0E;
                }
            }
            else
            {
                printf("Missing mixer gain. Using -9.5 dB.\n");
                mix_gain = 0x0E;
            }
            val = sx1255_readreg(0x08);
            val &= (uint8_t)0xF0;
            val |= (uint8_t)mix_gain;
            sx1255_writereg(0x08, val);
            break;

        // rx pll bw
        case 'a':
            if (strlen(optarg) > 0)
            {
                pll_bw = atof(optarg);
                if (pll_bw >= 75 && pll_bw <= 300)
                {
                    pll_bw -= 75;
                    pll_bw = pll_bw / 75;
                    printf("Setting RX PLL bandwidth to %d kHz\n", (pll_bw+1)*75);
                }
                else
                {
                    printf("Invalid RX PLL bandwidth. Using 75 kHz.\n");
                    pll_bw = 0;
                }
            }
            else
            {
                printf("Missing RX PLL bandwidth. Using 75 kHz.\n");
                pll_bw = 0;
            }
            val = sx1255_readreg(0x0E);
            val &= (uint8_t)~(0x06);
            val |= (uint8_t)pll_bw<<1;
            sx1255_writereg(0x0E, val);
            break;

        // tx pll bw
        case 'b':
            if (strlen(optarg) > 0)
            {
                pll_bw = atof(optarg);
                if (pll_bw >= 75 && pll_bw <= 300)
                {
                    pll_bw -= 75;
                    pll_bw = pll_bw / 75;
                    printf("Setting TX PLL bandwidth to %d kHz\n", (pll_bw+1)*75);
                }
                else
                {
                    printf("Invalid TX PLL bandwidth. Using 75 kHz.\n");
                    pll_bw = 0;
                }
            }
            else
            {
                printf("Missing TX PLL bandwidth. Using 75 kHz.\n");
                pll_bw = 0;
            }
            val = sx1255_readreg(0x0A);
            val &= (uint8_t)~(0x60);
            val |= (uint8_t)pll_bw<<5;
            sx1255_writereg(0x0A, val);
            break;

        // enable/disable TX front end
        case 'T':
            if (strlen(optarg) > 0)
            {
                val = atoi(optarg);
                if (val == 0)
                {
                    val = sx1255_readreg(0x00);
                    val &= (uint8_t)~((1 << 2) | (1 << 3));
                    sx1255_writereg(0x00, val);
                    printf("Disabling TX path.\n");
                }
                else
                {
                    val = sx1255_readreg(0x00);
                    val |= (uint8_t)(1 << 2) | (1 << 3);
                    sx1255_writereg(0x00, val);
                    printf("Enabling TX path.\n");
                }
            }
            else
            {
                printf("Missing TX enable parameter.\n");
            }
            break;

        // enable/disable RX front end
        case 'R':
            if (strlen(optarg) > 0)
            {
                val = atoi(optarg);
                if (val == 0)
                {
                    val = sx1255_readreg(0x00);
                    val &= (uint8_t)~(1 << 1);
                    sx1255_writereg(0x00, val);
                    printf("Disabling RX path.\n");
                }
                else
                {
                    val = sx1255_readreg(0x00);
                    val |= (uint8_t)(1 << 1);
                    sx1255_writereg(0x00, val);
                    printf("Enabling RX path.\n");
                }
            }
            else
            {
                printf("Missing RX enable parameter.\n");
            }
            break;

        // get PLL lock flags
        case 'P':
            val = sx1255_readreg(0x11);
            printf("TX PLL %s\n", (val & (1 << 0)) ? "locked" : "unlocked");
            printf("RX PLL %s\n", (val & (1 << 1)) ? "locked" : "unlocked");
            break;

        // get register value
        case 'G':
            if (strlen(optarg) > 0)
            {
                if (strstr(optarg, "0x") != NULL)
                    addr = strtol(optarg, NULL, 16);
                else
                    addr = atoi(optarg);

                if (addr <= 0x13)
                    printf("Register 0x%02X value: 0x%02X\n", addr, sx1255_readreg(addr));
                else
                    printf("Register readout error: address out of range.\n");
            }
            else
            {
                printf("Register readout error: missing register address.\n");
            }
            break;

        // set register value (=hex,hex)
        case 'S':
            if (strlen(optarg) > 0)
            {
                addr = strtol(optarg, NULL, 16);
                val = strtol(strstr(optarg, ",") + 1, NULL, 16);

                if (addr <= 0x13)
                {
                    printf("Seting register 0x%02X to 0x%02X\n", addr, val);
                    sx1255_writereg(addr, val);
                }
                else
                    printf("Register write error: address out of range.\n");
            }
            else
            {
                printf("Register write error: missing params.\n");
            }
            break;

        // help
        case 'h':
            print_help(argv[0]);
            gpio_cleanup();
            return 0;
            break;
        }
    }

    // we leave some registers for later
    // TODO: add those to args above as well
    sx1255_writereg(0x0D, (0x01 << 5) | (0x05 << 2) | 0x03);
    sx1255_writereg(0x0B, 5);

    gpio_cleanup();
    return 0;
}

void spi_close(void){ if (spi_fd>=0){ close(spi_fd); spi_fd=-1; } }
