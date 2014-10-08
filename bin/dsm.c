//---------------------------------------------------------------------------
//  __________________    _________  _____            _____  .__         ._.
//  \______   \______ \  /   _____/ /     \          /  _  \ |__| ____   | |
//   |    |  _/|    |  \ \_____  \ /  \ /  \        /  /_\  \|  _/ __ \  | |
//   |    |   \|    `   \/        /    Y    \      /    |    |  \  ___/   \|
//   |______  /_______  /_______  \____|__  / /\   \____|__  |__|\___ |   __
//          \/        \/        \/        \/  )/           \/        \/   \/
//
// This file is part of libdsm. Copyright © 2014 VideoLabs SAS
//
// Author: Julien 'Lta' BALLET <contact@lta.io>
//
// This program is free software. It comes without any warranty, to the extent
// permitted by applicable law. You can redistribute it and/or modify it under
// the terms of the Do What The Fuck You Want To Public License, Version 2, as
// published by Sam Hocevar. See the COPYING file for more details.
//----------------------------------------------------------------------------

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#ifdef HAVE_TARGETCONDITIONALS_H
  #include "TargetConditionals.h"
  // Header does not exist in the iOS SDK
  #ifndef TARGET_OS_IPHONE
    #include <netinet/udp.h>
  #endif
#else
  #include <netinet/udp.h>
#endif
#include <arpa/inet.h>

#include <getopt.h>

#include "bdsm.h"

#define NBT_UDP_PORT        138
#define NBT_TCP_PORT        139
#define USAGE

/* *INDENT-OFF* */
char usage_str[] = {
  "usage: %s [options] host login password share file\n"
  "  -h, --help         Show this help screen.\n"
  "  -v, --version      Print the version and quit.\n"
};
/* *INDENT-ON* */

static void print_usage(const char *pname, int err)
{
  fprintf(stderr, usage_str, pname);
  exit(err);
}

static int parse_options(int argc, char * argv[])
{
  struct option long_options[] = {
    {"help", no_argument, 0, 'h'},
    {"version", no_argument, 0, 'v'},
    {0, 0, 0, 0},
  };

  int c, opt_index = 0;

  char const *pname = ((pname = strrchr(argv[0], '/')) != NULL) ? pname + 1 : argv[0];

  while (0 < (c = getopt_long(argc, argv, "hv", long_options, &opt_index)) ) {
    switch (c) {

    case 'h':
      print_usage(pname, 0);

    case 'v':
      fprintf(stderr, "v%s\n", VERSION);
      exit(0);

    default:
      fprintf(stderr, "unknown option, %c, in getopt_long.\n", c);
      exit(-1);
    }
  }

  return optind;
}

int main(int ac, char **av)
{
  const char          *pname, *host, *login, *password, *fname, *share;
  struct sockaddr_in  addr;
  netbios_ns          *ns;
  smb_session         *session;
  int                 argoffset;
  char                **share_list;
  smb_file            *files, *fiter;


  pname     = ((pname = strrchr(av[0], '/')) != NULL) ? pname + 1 : av[0];
  argoffset = parse_options(ac, av);

  if (argoffset >= ac || ac - argoffset != 5) {
    print_usage(pname, -1);
  }

  host      = av[argoffset++];
  login     = av[argoffset++];
  password  = av[argoffset++];
  share     = av[argoffset++];
  fname     = av[argoffset++];

  ns = netbios_ns_new();
  if (!netbios_ns_resolve(ns, host, NETBIOS_FILESERVER, &addr.sin_addr.s_addr))
    exit(-1);

  printf("%s's IP address is : %s\n", host, inet_ntoa(addr.sin_addr));

  //netbios_ns_discover(ctx->ns);
  //exit(0);

  // netbios_session *session;
  // session = netbios_session_new(addr.sin_addr.s_addr);
  // if (netbios_session_connect(session, "Cerbere"))
  //   printf("A NetBIOS session with %s has been established\n", host);
  // else
  // {
  //   printf("Unable to establish a NetBIOS session with %s\n", host);
  //   exit(21);
  // }

  // netbios_session_destroy(session);

  session = smb_session_new();

  //inet_aton("192.168.110.138", &addr.sin_addr);

  if (smb_session_connect(session, host, addr.sin_addr.s_addr, SMB_TRANSPORT_TCP))
  {
    printf("Successfully connected to %s\n", host);
    fprintf(stderr, "Challenge key is 0x%"PRIx64"\n", session->srv.challenge);
  }
  else
  {
    printf("Unable to connect to %s\n", host);
    exit(42);
  }

  smb_session_set_creds(session, host, login, password);
  if (smb_session_login(session))
  {
    if (session->guest)
      printf("Login FAILED but we were logged in as GUEST \n");
    else
      printf("Successfully logged in as %s\\%s\n", host, login);
  }
  // else if (smb_session_login(session, "WORKGROUP", login, password))
  // {
  //   if (session->guest)
  //     printf("Login FAILED but we were logged in as GUEST \n");
  //   else
  //     printf("Successfully logged in as %s\\%s\n", host, login);
  // }
  else
  {
    printf("Authentication FAILURE.\n");
    exit(42);
  }

  if (!smb_share_get_list(session, &share_list))
  {
    fprintf(stderr, "Unable to list share for %s\n", host);
    exit(42);
  }

  fprintf(stderr, "Share list : \n");
  for (size_t j = 0; share_list[j] != NULL; j++)
    fprintf(stderr, "- %s\n", share_list[j]);
  smb_share_list_destroy(share_list);


  smb_tid test = smb_tree_connect(session, share);
  if (test)
    fprintf(stderr, "Connected to %s share\n", share);
  else
  {
    fprintf(stderr, "Unable to connect to %s share\n", share);
    exit(42);
  }

  // smb_fd fd = smb_fopen(session, test, "\\BDSM\\test.txt", SMB_MOD_RO);
  // if (fd)
  //   fprintf(stderr, "Successfully opened file: fd = 0x%.8x\n", fd);
  // else
  // {
  //   fprintf(stderr, "Unable to open file\n");
  //   exit(42);
  // }

  //char              data[1024];


  // smb_fread(session, fd, data, 1024);
  // fprintf(stderr, "Read from file:\n%s\n", data);
  // smb_fclose(session, fd);

  fprintf(stderr, "Let's find files at share's root :\n");
  files = fiter = smb_find(session, test, "\\*");
  if (fiter != NULL)
    while(fiter)
    {
      fprintf(stdout, "Found a file %s \n", fiter->name);
      fiter = fiter->next;
    }
  else
    fprintf(stderr, "Unable to list files\n");
  smb_stat_list_destroy(files);

  fprintf(stderr, "Query file info for path: %s\n", fname);
  files = smb_fstat(session, test, fname);

  if (files != NULL)
  {
    printf("File '%s' is %"PRIu64" bytes long\n", fname, files->size);
    smb_stat_destroy(files);
  }


  smb_session_destroy(session);
  netbios_ns_destroy(ns);

  return (0);
}
