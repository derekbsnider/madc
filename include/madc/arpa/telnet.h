// madc embedded arpa/telnet.h — minimal TELNET protocol constants
// Mirrors the BSD/glibc subset used by typical text-MUD codebases.

#define IAC          255  // interpret as command
#define DONT         254
#define DO           253
#define WONT         252
#define WILL         251
#define SB           250  // sub-option begin
#define GA           249  // go-ahead
#define EL           248  // erase line
#define EC           247  // erase character
#define AYT          246  // are you there
#define AO           245  // abort output
#define IP           244  // interrupt process
#define BREAK        243
#define DM           242  // data mark
#define NOP          241
#define SE           240  // sub-option end
#define EOR          239  // end of record
#define ABORT        238
#define SUSP         237
#define xEOF         236

#define TELOPT_BINARY     0
#define TELOPT_ECHO       1
#define TELOPT_RCP        2
#define TELOPT_SGA        3
#define TELOPT_NAMS       4
#define TELOPT_STATUS     5
#define TELOPT_TM         6
#define TELOPT_RCTE       7
#define TELOPT_NAOL       8
#define TELOPT_NAOP       9
#define TELOPT_NAOCRD    10
#define TELOPT_NAOHTS    11
#define TELOPT_NAOHTD    12
#define TELOPT_NAOFFD    13
#define TELOPT_NAOVTS    14
#define TELOPT_NAOVTD    15
#define TELOPT_NAOLFD    16
#define TELOPT_XASCII    17
#define TELOPT_LOGOUT    18
#define TELOPT_BM        19
#define TELOPT_DET       20
#define TELOPT_TTYPE     24
#define TELOPT_NAWS      31
#define TELOPT_TSPEED    32
#define TELOPT_LFLOW     33
#define TELOPT_LINEMODE  34
#define TELOPT_AUTHENTICATION 37
#define TELOPT_ENCRYPT   38
#define TELOPT_NEW_ENVIRON 39
#define NTELOPTS         40
#define TELOPT_EXOPL     255
