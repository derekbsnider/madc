// madc embedded termios.h — terminal I/O constants (Linux x86-64)
// Functions (tcgetattr, tcsetattr, tcsendbreak, tcdrain, tcflush,
//            tcflow, cfgetispeed, cfgetospeed, cfsetispeed, cfsetospeed,
//            isatty, ttyname) available via dlsym fallback
// struct termios access deferred

// tcsetattr() action flags
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

// tcflush() queue selectors
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

// tcflow() action values
#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

// c_lflag bits
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000

// c_iflag bits
#define IGNBRK  0000001
#define BRKINT  0000002
#define IGNPAR  0000004
#define PARMRK  0000010
#define INPCK   0000020
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
#define IXOFF   0010000

// c_oflag bits
#define OPOST   0000001
#define ONLCR   0000004
#define OCRNL   0000010
#define ONOCR   0000020
#define ONLRET  0000040

// Baud rates
#define B0       0
#define B50      1
#define B75      2
#define B110     3
#define B134     4
#define B150     5
#define B200     6
#define B300     7
#define B600     8
#define B1200    9
#define B1800    10
#define B2400    11
#define B4800    12
#define B9600    13
#define B19200   14
#define B38400   15
#define B57600   4097
#define B115200  4098
#define B230400  4099
#define B460800  4100
#define B921600  4103

// ioctl TIOCGWINSZ / TIOCSWINSZ (get/set window size)
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
