// madc embedded pwd.h — password database
// Functions (getpwuid, getpwnam, getpwent, setpwent, endpwent, getlogin)
// available via dlsym fallback

#ifndef _PWD_H
#define _PWD_H 1

typedef unsigned int __uid_t;
typedef unsigned int __gid_t;

struct passwd
{
  char *pw_name;
  char *pw_passwd;
  __uid_t pw_uid;
  __gid_t pw_gid;
  char *pw_gecos;
  char *pw_dir;
  char *pw_shell;
};

#endif
